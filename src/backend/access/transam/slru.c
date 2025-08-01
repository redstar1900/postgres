/*-------------------------------------------------------------------------
 *
 * slru.c
 *		Simple LRU buffering for wrap-around-able permanent metadata
 *
 * This module is used to maintain various pieces of transaction status
 * indexed by TransactionId (such as commit status, parent transaction ID,
 * commit timestamp), as well as storage for multixacts, serializable
 * isolation locks and NOTIFY traffic.  Extensions can define their own
 * SLRUs, too.
 *
 * Under ordinary circumstances we expect that write traffic will occur
 * mostly to the latest page (and to the just-prior page, soon after a
 * page transition).  Read traffic will probably touch a larger span of
 * pages, but a relatively small number of buffers should be sufficient.
 *
 * We use a simple least-recently-used scheme to manage a pool of shared
 * page buffers, split in banks by the lowest bits of the page number, and
 * the management algorithm only processes the bank to which the desired
 * page belongs, so a linear search is sufficient; there's no need for a
 * hashtable or anything fancy.  The algorithm is straight LRU except that
 * we will never swap out the latest page (since we know it's going to be
 * hit again eventually).
 *
 * We use per-bank control LWLocks to protect the shared data structures,
 * plus per-buffer LWLocks that synchronize I/O for each buffer.  The
 * bank's control lock must be held to examine or modify any of the bank's
 * shared state.  A process that is reading in or writing out a page
 * buffer does not hold the control lock, only the per-buffer lock for the
 * buffer it is working on.  One exception is latest_page_number, which is
 * read and written using atomic ops.
 *
 * "Holding the bank control lock" means exclusive lock in all cases
 * except for SimpleLruReadPage_ReadOnly(); see comments for
 * SlruRecentlyUsed() for the implications of that.
 *
 * When initiating I/O on a buffer, we acquire the per-buffer lock exclusively
 * before releasing the control lock.  The per-buffer lock is released after
 * completing the I/O, re-acquiring the control lock, and updating the shared
 * state.  (Deadlock is not possible here, because we never try to initiate
 * I/O when someone else is already doing I/O on the same buffer.)
 * To wait for I/O to complete, release the control lock, acquire the
 * per-buffer lock in shared mode, immediately release the per-buffer lock,
 * reacquire the control lock, and then recheck state (since arbitrary things
 * could have happened while we didn't have the lock).
 *
 * As with the regular buffer manager, it is possible for another process
 * to re-dirty a page that is currently being written out.  This is handled
 * by re-setting the page's page_dirty flag.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/transam/slru.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/slru.h"
#include "access/transam.h"
#include "access/xlog.h"
#include "access/xlogutils.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/fd.h"
#include "storage/shmem.h"
#include "utils/guc.h"

/*
 * Converts segment number to the filename of the segment.
 *
 * "path" should point to a buffer at least MAXPGPATH characters long.
 *
 * If ctl->long_segment_names is true, segno can be in the range [0, 2^60-1].
 * The resulting file name is made of 15 characters, e.g. dir/123456789ABCDEF.
 *
 * If ctl->long_segment_names is false, segno can be in the range [0, 2^24-1].
 * The resulting file name is made of 4 to 6 characters, as of:
 *
 *  dir/1234   for [0, 2^16-1]
 *  dir/12345  for [2^16, 2^20-1]
 *  dir/123456 for [2^20, 2^24-1]
 */
static inline int
SlruFileName(SlruCtl ctl, char *path, int64 segno)
{
	if (ctl->long_segment_names)
	{
		/*
		 * We could use 16 characters here but the disadvantage would be that
		 * the SLRU segments will be hard to distinguish from WAL segments.
		 *
		 * For this reason we use 15 characters. It is enough but also means
		 * that in the future we can't decrease SLRU_PAGES_PER_SEGMENT easily.
		 */
		Assert(segno >= 0 && segno <= INT64CONST(0xFFFFFFFFFFFFFFF));
		return snprintf(path, MAXPGPATH, "%s/%015" PRIX64, ctl->Dir, segno);
	}
	else
	{
		/*
		 * Despite the fact that %04X format string is used up to 24 bit
		 * integers are allowed. See SlruCorrectSegmentFilenameLength()
		 */
		Assert(segno >= 0 && segno <= INT64CONST(0xFFFFFF));
		return snprintf(path, MAXPGPATH, "%s/%04X", (ctl)->Dir,
						(unsigned int) segno);
	}
}

/*
 * During SimpleLruWriteAll(), we will usually not need to write more than one
 * or two physical files, but we may need to write several pages per file.  We
 * can consolidate the I/O requests by leaving files open until control returns
 * to SimpleLruWriteAll().  This data structure remembers which files are open.
 */
#define MAX_WRITEALL_BUFFERS	16

typedef struct SlruWriteAllData
{
	int			num_files;		/* # files actually open */
	int			fd[MAX_WRITEALL_BUFFERS];	/* their FD's */
	int64		segno[MAX_WRITEALL_BUFFERS];	/* their log seg#s */
} SlruWriteAllData;

typedef struct SlruWriteAllData *SlruWriteAll;


/*
 * Bank size for the slot array.  Pages are assigned a bank according to their
 * page number, with each bank being this size.  We want a power of 2 so that
 * we can determine the bank number for a page with just bit shifting; we also
 * want to keep the bank size small so that LRU victim search is fast.  16
 * buffers per bank seems a good number.
 */
#define SLRU_BANK_BITSHIFT		4
#define SLRU_BANK_SIZE			(1 << SLRU_BANK_BITSHIFT)

/*
 * Macro to get the bank number to which the slot belongs.
 */
#define SlotGetBankNumber(slotno)	((slotno) >> SLRU_BANK_BITSHIFT)


/*
 * Populate a file tag describing a segment file.  We only use the segment
 * number, since we can derive everything else we need by having separate
 * sync handler functions for clog, multixact etc.
 */
#define INIT_SLRUFILETAG(a,xx_handler,xx_segno) \
( \
	memset(&(a), 0, sizeof(FileTag)), \
	(a).handler = (xx_handler), \
	(a).segno = (xx_segno) \
)

/* Saved info for SlruReportIOError */
typedef enum
{
	SLRU_OPEN_FAILED,
	SLRU_SEEK_FAILED,
	SLRU_READ_FAILED,
	SLRU_WRITE_FAILED,
	SLRU_FSYNC_FAILED,
	SLRU_CLOSE_FAILED,
} SlruErrorCause;

static SlruErrorCause slru_errcause;
static int	slru_errno;


static void SimpleLruZeroLSNs(SlruCtl ctl, int slotno);
static void SimpleLruWaitIO(SlruCtl ctl, int slotno);
static void SlruInternalWritePage(SlruCtl ctl, int slotno, SlruWriteAll fdata);
static bool SlruPhysicalReadPage(SlruCtl ctl, int64 pageno, int slotno);
static bool SlruPhysicalWritePage(SlruCtl ctl, int64 pageno, int slotno,
								  SlruWriteAll fdata);
static void SlruReportIOError(SlruCtl ctl, int64 pageno, TransactionId xid);
static int	SlruSelectLRUPage(SlruCtl ctl, int64 pageno);

static bool SlruScanDirCbDeleteCutoff(SlruCtl ctl, char *filename,
									  int64 segpage, void *data);
static void SlruInternalDeleteSegment(SlruCtl ctl, int64 segno);
static inline void SlruRecentlyUsed(SlruShared shared, int slotno);


/*
 * Initialization of shared memory
 */

Size
SimpleLruShmemSize(int nslots, int nlsns)
{
	int			nbanks = nslots / SLRU_BANK_SIZE;
	Size		sz;

	Assert(nslots <= SLRU_MAX_ALLOWED_BUFFERS);
	Assert(nslots % SLRU_BANK_SIZE == 0);

	/* we assume nslots isn't so large as to risk overflow */
	sz = MAXALIGN(sizeof(SlruSharedData));
	sz += MAXALIGN(nslots * sizeof(char *));	/* page_buffer[] */
	sz += MAXALIGN(nslots * sizeof(SlruPageStatus));	/* page_status[] */
	sz += MAXALIGN(nslots * sizeof(bool));	/* page_dirty[] */
	sz += MAXALIGN(nslots * sizeof(int64)); /* page_number[] */
	sz += MAXALIGN(nslots * sizeof(int));	/* page_lru_count[] */
	sz += MAXALIGN(nslots * sizeof(LWLockPadded));	/* buffer_locks[] */
	sz += MAXALIGN(nbanks * sizeof(LWLockPadded));	/* bank_locks[] */
	sz += MAXALIGN(nbanks * sizeof(int));	/* bank_cur_lru_count[] */

	if (nlsns > 0)
		sz += MAXALIGN(nslots * nlsns * sizeof(XLogRecPtr));	/* group_lsn[] */

	return BUFFERALIGN(sz) + BLCKSZ * nslots;
}

/*
 * Determine a number of SLRU buffers to use.
 *
 * We simply divide shared_buffers by the divisor given and cap
 * that at the maximum given; but always at least SLRU_BANK_SIZE.
 * Round down to the nearest multiple of SLRU_BANK_SIZE.
 */
int
SimpleLruAutotuneBuffers(int divisor, int max)
{
	return Min(max - (max % SLRU_BANK_SIZE),
			   Max(SLRU_BANK_SIZE,
				   NBuffers / divisor - (NBuffers / divisor) % SLRU_BANK_SIZE));
}

/*
 * Initialize, or attach to, a simple LRU cache in shared memory.
 *
 * ctl: address of local (unshared) control structure.
 * name: name of SLRU.  (This is user-visible, pick with care!)
 * nslots: number of page slots to use.
 * nlsns: number of LSN groups per page (set to zero if not relevant).
 * subdir: PGDATA-relative subdirectory that will contain the files.
 * buffer_tranche_id: tranche ID to use for the SLRU's per-buffer LWLocks.
 * bank_tranche_id: tranche ID to use for the bank LWLocks.
 * sync_handler: which set of functions to use to handle sync requests
 */
void
SimpleLruInit(SlruCtl ctl, const char *name, int nslots, int nlsns,
			  const char *subdir, int buffer_tranche_id, int bank_tranche_id,
			  SyncRequestHandler sync_handler, bool long_segment_names)
{
	SlruShared	shared;
	bool		found;
	int			nbanks = nslots / SLRU_BANK_SIZE;

	Assert(nslots <= SLRU_MAX_ALLOWED_BUFFERS);

	shared = (SlruShared) ShmemInitStruct(name,
										  SimpleLruShmemSize(nslots, nlsns),
										  &found);

	if (!IsUnderPostmaster)
	{
		/* Initialize locks and shared memory area */
		char	   *ptr;
		Size		offset;

		Assert(!found);

		memset(shared, 0, sizeof(SlruSharedData));

		shared->num_slots = nslots;
		shared->lsn_groups_per_page = nlsns;

		pg_atomic_init_u64(&shared->latest_page_number, 0);

		shared->slru_stats_idx = pgstat_get_slru_index(name);

		ptr = (char *) shared;
		offset = MAXALIGN(sizeof(SlruSharedData));
		shared->page_buffer = (char **) (ptr + offset);
		offset += MAXALIGN(nslots * sizeof(char *));
		shared->page_status = (SlruPageStatus *) (ptr + offset);
		offset += MAXALIGN(nslots * sizeof(SlruPageStatus));
		shared->page_dirty = (bool *) (ptr + offset);
		offset += MAXALIGN(nslots * sizeof(bool));
		shared->page_number = (int64 *) (ptr + offset);
		offset += MAXALIGN(nslots * sizeof(int64));
		shared->page_lru_count = (int *) (ptr + offset);
		offset += MAXALIGN(nslots * sizeof(int));

		/* Initialize LWLocks */
		shared->buffer_locks = (LWLockPadded *) (ptr + offset);
		offset += MAXALIGN(nslots * sizeof(LWLockPadded));
		shared->bank_locks = (LWLockPadded *) (ptr + offset);
		offset += MAXALIGN(nbanks * sizeof(LWLockPadded));
		shared->bank_cur_lru_count = (int *) (ptr + offset);
		offset += MAXALIGN(nbanks * sizeof(int));

		if (nlsns > 0)
		{
			shared->group_lsn = (XLogRecPtr *) (ptr + offset);
			offset += MAXALIGN(nslots * nlsns * sizeof(XLogRecPtr));
		}

		ptr += BUFFERALIGN(offset);
		for (int slotno = 0; slotno < nslots; slotno++)
		{
			LWLockInitialize(&shared->buffer_locks[slotno].lock,
							 buffer_tranche_id);

			shared->page_buffer[slotno] = ptr;
			shared->page_status[slotno] = SLRU_PAGE_EMPTY;
			shared->page_dirty[slotno] = false;
			shared->page_lru_count[slotno] = 0;
			ptr += BLCKSZ;
		}

		/* Initialize the slot banks. */
		for (int bankno = 0; bankno < nbanks; bankno++)
		{
			LWLockInitialize(&shared->bank_locks[bankno].lock, bank_tranche_id);
			shared->bank_cur_lru_count[bankno] = 0;
		}

		/* Should fit to estimated shmem size */
		Assert(ptr - (char *) shared <= SimpleLruShmemSize(nslots, nlsns));
	}
	else
	{
		Assert(found);
		Assert(shared->num_slots == nslots);
	}

	/*
	 * Initialize the unshared control struct, including directory path. We
	 * assume caller set PagePrecedes.
	 */
	ctl->shared = shared;
	ctl->sync_handler = sync_handler;
	ctl->long_segment_names = long_segment_names;
	ctl->nbanks = nbanks;
	strlcpy(ctl->Dir, subdir, sizeof(ctl->Dir));
}

/*
 * Helper function for GUC check_hook to check whether slru buffers are in
 * multiples of SLRU_BANK_SIZE.
 */
bool
check_slru_buffers(const char *name, int *newval)
{
	/* Valid values are multiples of SLRU_BANK_SIZE */
	if (*newval % SLRU_BANK_SIZE == 0)
		return true;

	GUC_check_errdetail("\"%s\" must be a multiple of %d.", name,
						SLRU_BANK_SIZE);
	return false;
}

/*
 * Initialize (or reinitialize) a page to zeroes.
 *
 * The page is not actually written, just set up in shared memory.
 * The slot number of the new page is returned.
 *
 * Bank lock must be held at entry, and will be held at exit.
 */
int
SimpleLruZeroPage(SlruCtl ctl, int64 pageno)
{
	SlruShared	shared = ctl->shared;
	int			slotno;

	Assert(LWLockHeldByMeInMode(SimpleLruGetBankLock(ctl, pageno), LW_EXCLUSIVE));

	/* Find a suitable buffer slot for the page */
	slotno = SlruSelectLRUPage(ctl, pageno);
	Assert(shared->page_status[slotno] == SLRU_PAGE_EMPTY ||
		   (shared->page_status[slotno] == SLRU_PAGE_VALID &&
			!shared->page_dirty[slotno]) ||
		   shared->page_number[slotno] == pageno);

	/* Mark the slot as containing this page */
	shared->page_number[slotno] = pageno;
	shared->page_status[slotno] = SLRU_PAGE_VALID;
	shared->page_dirty[slotno] = true;
	SlruRecentlyUsed(shared, slotno);

	/* Set the buffer to zeroes */
	MemSet(shared->page_buffer[slotno], 0, BLCKSZ);

	/* Set the LSNs for this new page to zero */
	SimpleLruZeroLSNs(ctl, slotno);

	/*
	 * Assume this page is now the latest active page.
	 *
	 * Note that because both this routine and SlruSelectLRUPage run with
	 * ControlLock held, it is not possible for this to be zeroing a page that
	 * SlruSelectLRUPage is going to evict simultaneously.  Therefore, there's
	 * no memory barrier here.
	 */
	pg_atomic_write_u64(&shared->latest_page_number, pageno);

	/* update the stats counter of zeroed pages */
	pgstat_count_slru_page_zeroed(shared->slru_stats_idx);

	return slotno;
}

/*
 * Zero all the LSNs we store for this slru page.
 *
 * This should be called each time we create a new page, and each time we read
 * in a page from disk into an existing buffer.  (Such an old page cannot
 * have any interesting LSNs, since we'd have flushed them before writing
 * the page in the first place.)
 *
 * This assumes that InvalidXLogRecPtr is bitwise-all-0.
 */
static void
SimpleLruZeroLSNs(SlruCtl ctl, int slotno)
{
	SlruShared	shared = ctl->shared;

	if (shared->lsn_groups_per_page > 0)
		MemSet(&shared->group_lsn[slotno * shared->lsn_groups_per_page], 0,
			   shared->lsn_groups_per_page * sizeof(XLogRecPtr));
}

/*
 * This is a convenience wrapper for the common case of zeroing a page and
 * immediately flushing it to disk.
 *
 * Control lock is acquired and released here.
 */
void
SimpleLruZeroAndWritePage(SlruCtl ctl, int64 pageno)
{
	int			slotno;
	LWLock	   *lock;

	lock = SimpleLruGetBankLock(ctl, pageno);
	LWLockAcquire(lock, LW_EXCLUSIVE);

	/* Create and zero the page */
	slotno = SimpleLruZeroPage(ctl, pageno);

	/* Make sure it's written out */
	SimpleLruWritePage(ctl, slotno);
	Assert(!ctl->shared->page_dirty[slotno]);

	LWLockRelease(lock);
}

/*
 * Wait for any active I/O on a page slot to finish.  (This does not
 * guarantee that new I/O hasn't been started before we return, though.
 * In fact the slot might not even contain the same page anymore.)
 *
 * Bank lock must be held at entry, and will be held at exit.
 */
static void
SimpleLruWaitIO(SlruCtl ctl, int slotno)
{
	SlruShared	shared = ctl->shared;
	int			bankno = SlotGetBankNumber(slotno);

	Assert(shared->page_status[slotno] != SLRU_PAGE_EMPTY);

	/* See notes at top of file */
	LWLockRelease(&shared->bank_locks[bankno].lock);
	LWLockAcquire(&shared->buffer_locks[slotno].lock, LW_SHARED);
	LWLockRelease(&shared->buffer_locks[slotno].lock);
	LWLockAcquire(&shared->bank_locks[bankno].lock, LW_EXCLUSIVE);

	/*
	 * If the slot is still in an io-in-progress state, then either someone
	 * already started a new I/O on the slot, or a previous I/O failed and
	 * neglected to reset the page state.  That shouldn't happen, really, but
	 * it seems worth a few extra cycles to check and recover from it. We can
	 * cheaply test for failure by seeing if the buffer lock is still held (we
	 * assume that transaction abort would release the lock).
	 */
	if (shared->page_status[slotno] == SLRU_PAGE_READ_IN_PROGRESS ||
		shared->page_status[slotno] == SLRU_PAGE_WRITE_IN_PROGRESS)
	{
		if (LWLockConditionalAcquire(&shared->buffer_locks[slotno].lock, LW_SHARED))
		{
			/* indeed, the I/O must have failed */
			if (shared->page_status[slotno] == SLRU_PAGE_READ_IN_PROGRESS)
				shared->page_status[slotno] = SLRU_PAGE_EMPTY;
			else				/* write_in_progress */
			{
				shared->page_status[slotno] = SLRU_PAGE_VALID;
				shared->page_dirty[slotno] = true;
			}
			LWLockRelease(&shared->buffer_locks[slotno].lock);
		}
	}
}

/*
 * Find a page in a shared buffer, reading it in if necessary.
 * The page number must correspond to an already-initialized page.
 *
 * If write_ok is true then it is OK to return a page that is in
 * WRITE_IN_PROGRESS state; it is the caller's responsibility to be sure
 * that modification of the page is safe.  If write_ok is false then we
 * will not return the page until it is not undergoing active I/O.
 *
 * The passed-in xid is used only for error reporting, and may be
 * InvalidTransactionId if no specific xid is associated with the action.
 *
 * Return value is the shared-buffer slot number now holding the page.
 * The buffer's LRU access info is updated.
 *
 * The correct bank lock must be held at entry, and will be held at exit.
 */
int
SimpleLruReadPage(SlruCtl ctl, int64 pageno, bool write_ok,
				  TransactionId xid)
{
	SlruShared	shared = ctl->shared;
	LWLock	   *banklock = SimpleLruGetBankLock(ctl, pageno);

	Assert(LWLockHeldByMeInMode(banklock, LW_EXCLUSIVE));

	/* Outer loop handles restart if we must wait for someone else's I/O */
	for (;;)
	{
		int			slotno;
		bool		ok;

		/* See if page already is in memory; if not, pick victim slot */
		slotno = SlruSelectLRUPage(ctl, pageno);

		/* Did we find the page in memory? */
		if (shared->page_status[slotno] != SLRU_PAGE_EMPTY &&
			shared->page_number[slotno] == pageno)
		{
			/*
			 * If page is still being read in, we must wait for I/O.  Likewise
			 * if the page is being written and the caller said that's not OK.
			 */
			if (shared->page_status[slotno] == SLRU_PAGE_READ_IN_PROGRESS ||
				(shared->page_status[slotno] == SLRU_PAGE_WRITE_IN_PROGRESS &&
				 !write_ok))
			{
				SimpleLruWaitIO(ctl, slotno);
				/* Now we must recheck state from the top */
				continue;
			}
			/* Otherwise, it's ready to use */
			SlruRecentlyUsed(shared, slotno);

			/* update the stats counter of pages found in the SLRU */
			pgstat_count_slru_page_hit(shared->slru_stats_idx);

			return slotno;
		}

		/* We found no match; assert we selected a freeable slot */
		Assert(shared->page_status[slotno] == SLRU_PAGE_EMPTY ||
			   (shared->page_status[slotno] == SLRU_PAGE_VALID &&
				!shared->page_dirty[slotno]));

		/* Mark the slot read-busy */
		shared->page_number[slotno] = pageno;
		shared->page_status[slotno] = SLRU_PAGE_READ_IN_PROGRESS;
		shared->page_dirty[slotno] = false;

		/* Acquire per-buffer lock (cannot deadlock, see notes at top) */
		LWLockAcquire(&shared->buffer_locks[slotno].lock, LW_EXCLUSIVE);

		/* Release bank lock while doing I/O */
		LWLockRelease(banklock);

		/* Do the read */
		ok = SlruPhysicalReadPage(ctl, pageno, slotno);

		/* Set the LSNs for this newly read-in page to zero */
		SimpleLruZeroLSNs(ctl, slotno);

		/* Re-acquire bank control lock and update page state */
		LWLockAcquire(banklock, LW_EXCLUSIVE);

		Assert(shared->page_number[slotno] == pageno &&
			   shared->page_status[slotno] == SLRU_PAGE_READ_IN_PROGRESS &&
			   !shared->page_dirty[slotno]);

		shared->page_status[slotno] = ok ? SLRU_PAGE_VALID : SLRU_PAGE_EMPTY;

		LWLockRelease(&shared->buffer_locks[slotno].lock);

		/* Now it's okay to ereport if we failed */
		if (!ok)
			SlruReportIOError(ctl, pageno, xid);

		SlruRecentlyUsed(shared, slotno);

		/* update the stats counter of pages not found in SLRU */
		pgstat_count_slru_page_read(shared->slru_stats_idx);

		return slotno;
	}
}

/*
 * Find a page in a shared buffer, reading it in if necessary.
 * The page number must correspond to an already-initialized page.
 * The caller must intend only read-only access to the page.
 *
 * The passed-in xid is used only for error reporting, and may be
 * InvalidTransactionId if no specific xid is associated with the action.
 *
 * Return value is the shared-buffer slot number now holding the page.
 * The buffer's LRU access info is updated.
 *
 * Bank control lock must NOT be held at entry, but will be held at exit.
 * It is unspecified whether the lock will be shared or exclusive.
 */
int
SimpleLruReadPage_ReadOnly(SlruCtl ctl, int64 pageno, TransactionId xid)
{
	SlruShared	shared = ctl->shared;
	LWLock	   *banklock = SimpleLruGetBankLock(ctl, pageno);
	int			bankno = pageno % ctl->nbanks;
	int			bankstart = bankno * SLRU_BANK_SIZE;
	int			bankend = bankstart + SLRU_BANK_SIZE;

	/* Try to find the page while holding only shared lock */
	LWLockAcquire(banklock, LW_SHARED);

	/* See if page is already in a buffer */
	for (int slotno = bankstart; slotno < bankend; slotno++)
	{
		if (shared->page_status[slotno] != SLRU_PAGE_EMPTY &&
			shared->page_number[slotno] == pageno &&
			shared->page_status[slotno] != SLRU_PAGE_READ_IN_PROGRESS)
		{
			/* See comments for SlruRecentlyUsed macro */
			SlruRecentlyUsed(shared, slotno);

			/* update the stats counter of pages found in the SLRU */
			pgstat_count_slru_page_hit(shared->slru_stats_idx);

			return slotno;
		}
	}

	/* No luck, so switch to normal exclusive lock and do regular read */
	LWLockRelease(banklock);
	LWLockAcquire(banklock, LW_EXCLUSIVE);

	return SimpleLruReadPage(ctl, pageno, true, xid);
}

/*
 * Write a page from a shared buffer, if necessary.
 * Does nothing if the specified slot is not dirty.
 *
 * NOTE: only one write attempt is made here.  Hence, it is possible that
 * the page is still dirty at exit (if someone else re-dirtied it during
 * the write).  However, we *do* attempt a fresh write even if the page
 * is already being written; this is for checkpoints.
 *
 * Bank lock must be held at entry, and will be held at exit.
 */
static void
SlruInternalWritePage(SlruCtl ctl, int slotno, SlruWriteAll fdata)
{
	SlruShared	shared = ctl->shared;
	int64		pageno = shared->page_number[slotno];
	int			bankno = SlotGetBankNumber(slotno);
	bool		ok;

	Assert(shared->page_status[slotno] != SLRU_PAGE_EMPTY);
	Assert(LWLockHeldByMeInMode(SimpleLruGetBankLock(ctl, pageno), LW_EXCLUSIVE));

	/* If a write is in progress, wait for it to finish */
	while (shared->page_status[slotno] == SLRU_PAGE_WRITE_IN_PROGRESS &&
		   shared->page_number[slotno] == pageno)
	{
		SimpleLruWaitIO(ctl, slotno);
	}

	/*
	 * Do nothing if page is not dirty, or if buffer no longer contains the
	 * same page we were called for.
	 */
	if (!shared->page_dirty[slotno] ||
		shared->page_status[slotno] != SLRU_PAGE_VALID ||
		shared->page_number[slotno] != pageno)
		return;

	/*
	 * Mark the slot write-busy, and clear the dirtybit.  After this point, a
	 * transaction status update on this page will mark it dirty again.
	 */
	shared->page_status[slotno] = SLRU_PAGE_WRITE_IN_PROGRESS;
	shared->page_dirty[slotno] = false;

	/* Acquire per-buffer lock (cannot deadlock, see notes at top) */
	LWLockAcquire(&shared->buffer_locks[slotno].lock, LW_EXCLUSIVE);

	/* Release bank lock while doing I/O */
	LWLockRelease(&shared->bank_locks[bankno].lock);

	/* Do the write */
	ok = SlruPhysicalWritePage(ctl, pageno, slotno, fdata);

	/* If we failed, and we're in a flush, better close the files */
	if (!ok && fdata)
	{
		for (int i = 0; i < fdata->num_files; i++)
			CloseTransientFile(fdata->fd[i]);
	}

	/* Re-acquire bank lock and update page state */
	LWLockAcquire(&shared->bank_locks[bankno].lock, LW_EXCLUSIVE);

	Assert(shared->page_number[slotno] == pageno &&
		   shared->page_status[slotno] == SLRU_PAGE_WRITE_IN_PROGRESS);

	/* If we failed to write, mark the page dirty again */
	if (!ok)
		shared->page_dirty[slotno] = true;

	shared->page_status[slotno] = SLRU_PAGE_VALID;

	LWLockRelease(&shared->buffer_locks[slotno].lock);

	/* Now it's okay to ereport if we failed */
	if (!ok)
		SlruReportIOError(ctl, pageno, InvalidTransactionId);

	/* If part of a checkpoint, count this as a SLRU buffer written. */
	if (fdata)
	{
		CheckpointStats.ckpt_slru_written++;
		PendingCheckpointerStats.slru_written++;
	}
}

/*
 * Wrapper of SlruInternalWritePage, for external callers.
 * fdata is always passed a NULL here.
 */
void
SimpleLruWritePage(SlruCtl ctl, int slotno)
{
	Assert(ctl->shared->page_status[slotno] != SLRU_PAGE_EMPTY);

	SlruInternalWritePage(ctl, slotno, NULL);
}

/*
 * Return whether the given page exists on disk.
 *
 * A false return means that either the file does not exist, or that it's not
 * large enough to contain the given page.
 */
bool
SimpleLruDoesPhysicalPageExist(SlruCtl ctl, int64 pageno)
{
	int64		segno = pageno / SLRU_PAGES_PER_SEGMENT;
	int			rpageno = pageno % SLRU_PAGES_PER_SEGMENT;
	int			offset = rpageno * BLCKSZ;
	char		path[MAXPGPATH];
	int			fd;
	bool		result;
	off_t		endpos;

	/* update the stats counter of checked pages */
	pgstat_count_slru_page_exists(ctl->shared->slru_stats_idx);

	SlruFileName(ctl, path, segno);

	fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
	if (fd < 0)
	{
		/* expected: file doesn't exist */
		if (errno == ENOENT)
			return false;

		/* report error normally */
		slru_errcause = SLRU_OPEN_FAILED;
		slru_errno = errno;
		SlruReportIOError(ctl, pageno, 0);
	}

	if ((endpos = lseek(fd, 0, SEEK_END)) < 0)
	{
		slru_errcause = SLRU_SEEK_FAILED;
		slru_errno = errno;
		SlruReportIOError(ctl, pageno, 0);
	}

	result = endpos >= (off_t) (offset + BLCKSZ);

	if (CloseTransientFile(fd) != 0)
	{
		slru_errcause = SLRU_CLOSE_FAILED;
		slru_errno = errno;
		return false;
	}

	return result;
}

/*
 * Physical read of a (previously existing) page into a buffer slot
 *
 * On failure, we cannot just ereport(ERROR) since caller has put state in
 * shared memory that must be undone.  So, we return false and save enough
 * info in static variables to let SlruReportIOError make the report.
 *
 * For now, assume it's not worth keeping a file pointer open across
 * read/write operations.  We could cache one virtual file pointer ...
 */
static bool
SlruPhysicalReadPage(SlruCtl ctl, int64 pageno, int slotno)
{
	SlruShared	shared = ctl->shared;
	int64		segno = pageno / SLRU_PAGES_PER_SEGMENT;
	int			rpageno = pageno % SLRU_PAGES_PER_SEGMENT;
	off_t		offset = rpageno * BLCKSZ;
	char		path[MAXPGPATH];
	int			fd;

	SlruFileName(ctl, path, segno);

	/*
	 * In a crash-and-restart situation, it's possible for us to receive
	 * commands to set the commit status of transactions whose bits are in
	 * already-truncated segments of the commit log (see notes in
	 * SlruPhysicalWritePage).  Hence, if we are InRecovery, allow the case
	 * where the file doesn't exist, and return zeroes instead.
	 */
	fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
	if (fd < 0)
	{
		if (errno != ENOENT || !InRecovery)
		{
			slru_errcause = SLRU_OPEN_FAILED;
			slru_errno = errno;
			return false;
		}

		ereport(LOG,
				(errmsg("file \"%s\" doesn't exist, reading as zeroes",
						path)));
		MemSet(shared->page_buffer[slotno], 0, BLCKSZ);
		return true;
	}

	errno = 0;
	pgstat_report_wait_start(WAIT_EVENT_SLRU_READ);
	if (pg_pread(fd, shared->page_buffer[slotno], BLCKSZ, offset) != BLCKSZ)
	{
		pgstat_report_wait_end();
		slru_errcause = SLRU_READ_FAILED;
		slru_errno = errno;
		CloseTransientFile(fd);
		return false;
	}
	pgstat_report_wait_end();

	if (CloseTransientFile(fd) != 0)
	{
		slru_errcause = SLRU_CLOSE_FAILED;
		slru_errno = errno;
		return false;
	}

	return true;
}

/*
 * Physical write of a page from a buffer slot
 *
 * On failure, we cannot just ereport(ERROR) since caller has put state in
 * shared memory that must be undone.  So, we return false and save enough
 * info in static variables to let SlruReportIOError make the report.
 *
 * For now, assume it's not worth keeping a file pointer open across
 * independent read/write operations.  We do batch operations during
 * SimpleLruWriteAll, though.
 *
 * fdata is NULL for a standalone write, pointer to open-file info during
 * SimpleLruWriteAll.
 */
static bool
SlruPhysicalWritePage(SlruCtl ctl, int64 pageno, int slotno, SlruWriteAll fdata)
{
	SlruShared	shared = ctl->shared;
	int64		segno = pageno / SLRU_PAGES_PER_SEGMENT;
	int			rpageno = pageno % SLRU_PAGES_PER_SEGMENT;
	off_t		offset = rpageno * BLCKSZ;
	char		path[MAXPGPATH];
	int			fd = -1;

	/* update the stats counter of written pages */
	pgstat_count_slru_page_written(shared->slru_stats_idx);

	/*
	 * Honor the write-WAL-before-data rule, if appropriate, so that we do not
	 * write out data before associated WAL records.  This is the same action
	 * performed during FlushBuffer() in the main buffer manager.
	 */
	if (shared->group_lsn != NULL)
	{
		/*
		 * We must determine the largest async-commit LSN for the page. This
		 * is a bit tedious, but since this entire function is a slow path
		 * anyway, it seems better to do this here than to maintain a per-page
		 * LSN variable (which'd need an extra comparison in the
		 * transaction-commit path).
		 */
		XLogRecPtr	max_lsn;
		int			lsnindex;

		lsnindex = slotno * shared->lsn_groups_per_page;
		max_lsn = shared->group_lsn[lsnindex++];
		for (int lsnoff = 1; lsnoff < shared->lsn_groups_per_page; lsnoff++)
		{
			XLogRecPtr	this_lsn = shared->group_lsn[lsnindex++];

			if (max_lsn < this_lsn)
				max_lsn = this_lsn;
		}

		if (!XLogRecPtrIsInvalid(max_lsn))
		{
			/*
			 * As noted above, elog(ERROR) is not acceptable here, so if
			 * XLogFlush were to fail, we must PANIC.  This isn't much of a
			 * restriction because XLogFlush is just about all critical
			 * section anyway, but let's make sure.
			 */
			START_CRIT_SECTION();
			XLogFlush(max_lsn);
			END_CRIT_SECTION();
		}
	}

	/*
	 * During a SimpleLruWriteAll, we may already have the desired file open.
	 */
	if (fdata)
	{
		for (int i = 0; i < fdata->num_files; i++)
		{
			if (fdata->segno[i] == segno)
			{
				fd = fdata->fd[i];
				break;
			}
		}
	}

	if (fd < 0)
	{
		/*
		 * If the file doesn't already exist, we should create it.  It is
		 * possible for this to need to happen when writing a page that's not
		 * first in its segment; we assume the OS can cope with that. (Note:
		 * it might seem that it'd be okay to create files only when
		 * SimpleLruZeroPage is called for the first page of a segment.
		 * However, if after a crash and restart the REDO logic elects to
		 * replay the log from a checkpoint before the latest one, then it's
		 * possible that we will get commands to set transaction status of
		 * transactions that have already been truncated from the commit log.
		 * Easiest way to deal with that is to accept references to
		 * nonexistent files here and in SlruPhysicalReadPage.)
		 *
		 * Note: it is possible for more than one backend to be executing this
		 * code simultaneously for different pages of the same file. Hence,
		 * don't use O_EXCL or O_TRUNC or anything like that.
		 */
		SlruFileName(ctl, path, segno);
		fd = OpenTransientFile(path, O_RDWR | O_CREAT | PG_BINARY);
		if (fd < 0)
		{
			slru_errcause = SLRU_OPEN_FAILED;
			slru_errno = errno;
			return false;
		}

		if (fdata)
		{
			if (fdata->num_files < MAX_WRITEALL_BUFFERS)
			{
				fdata->fd[fdata->num_files] = fd;
				fdata->segno[fdata->num_files] = segno;
				fdata->num_files++;
			}
			else
			{
				/*
				 * In the unlikely event that we exceed MAX_WRITEALL_BUFFERS,
				 * fall back to treating it as a standalone write.
				 */
				fdata = NULL;
			}
		}
	}

	errno = 0;
	pgstat_report_wait_start(WAIT_EVENT_SLRU_WRITE);
	if (pg_pwrite(fd, shared->page_buffer[slotno], BLCKSZ, offset) != BLCKSZ)
	{
		pgstat_report_wait_end();
		/* if write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		slru_errcause = SLRU_WRITE_FAILED;
		slru_errno = errno;
		if (!fdata)
			CloseTransientFile(fd);
		return false;
	}
	pgstat_report_wait_end();

	/* Queue up a sync request for the checkpointer. */
	if (ctl->sync_handler != SYNC_HANDLER_NONE)
	{
		FileTag		tag;

		INIT_SLRUFILETAG(tag, ctl->sync_handler, segno);
		if (!RegisterSyncRequest(&tag, SYNC_REQUEST, false))
		{
			/* No space to enqueue sync request.  Do it synchronously. */
			pgstat_report_wait_start(WAIT_EVENT_SLRU_SYNC);
			if (pg_fsync(fd) != 0)
			{
				pgstat_report_wait_end();
				slru_errcause = SLRU_FSYNC_FAILED;
				slru_errno = errno;
				CloseTransientFile(fd);
				return false;
			}
			pgstat_report_wait_end();
		}
	}

	/* Close file, unless part of flush request. */
	if (!fdata)
	{
		if (CloseTransientFile(fd) != 0)
		{
			slru_errcause = SLRU_CLOSE_FAILED;
			slru_errno = errno;
			return false;
		}
	}

	return true;
}

/*
 * Issue the error message after failure of SlruPhysicalReadPage or
 * SlruPhysicalWritePage.  Call this after cleaning up shared-memory state.
 */
static void
SlruReportIOError(SlruCtl ctl, int64 pageno, TransactionId xid)
{
	int64		segno = pageno / SLRU_PAGES_PER_SEGMENT;
	int			rpageno = pageno % SLRU_PAGES_PER_SEGMENT;
	int			offset = rpageno * BLCKSZ;
	char		path[MAXPGPATH];

	SlruFileName(ctl, path, segno);
	errno = slru_errno;
	switch (slru_errcause)
	{
		case SLRU_OPEN_FAILED:
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not access status of transaction %" PRIu64,
							xid),
					 errdetail("Could not open file \"%s\": %m.", path)));
			break;
		case SLRU_SEEK_FAILED:
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not access status of transaction %" PRIu64,
							xid),
					 errdetail("Could not seek in file \"%s\" to offset %d: %m.",
							   path, offset)));
			break;
		case SLRU_READ_FAILED:
			if (errno)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not access status of transaction %" PRIu64,
								xid),
						 errdetail("Could not read from file \"%s\" at offset %d: %m.",
								   path, offset)));
			else
				ereport(ERROR,
						(errmsg("could not access status of transaction %" PRIu64,
								xid),
						 errdetail("Could not read from file \"%s\" at offset %d: read too few bytes.",
								   path, offset)));
			break;
		case SLRU_WRITE_FAILED:
			if (errno)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not access status of transaction %" PRIu64,
								xid),
						 errdetail("Could not write to file \"%s\" at offset %d: %m.",
								   path, offset)));
			else
				ereport(ERROR,
						(errmsg("could not access status of transaction %" PRIu64,
								xid),
						 errdetail("Could not write to file \"%s\" at offset %d: wrote too few bytes.",
								   path, offset)));
			break;
		case SLRU_FSYNC_FAILED:
			ereport(data_sync_elevel(ERROR),
					(errcode_for_file_access(),
					 errmsg("could not access status of transaction %" PRIu64,
							xid),
					 errdetail("Could not fsync file \"%s\": %m.",
							   path)));
			break;
		case SLRU_CLOSE_FAILED:
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not access status of transaction %" PRIu64,
							xid),
					 errdetail("Could not close file \"%s\": %m.",
							   path)));
			break;
		default:
			/* can't get here, we trust */
			elog(ERROR, "unrecognized SimpleLru error cause: %d",
				 (int) slru_errcause);
			break;
	}
}

/*
 * Mark a buffer slot "most recently used".
 */
static inline void
SlruRecentlyUsed(SlruShared shared, int slotno)
{
	int			bankno = SlotGetBankNumber(slotno);
	int			new_lru_count = shared->bank_cur_lru_count[bankno];

	Assert(shared->page_status[slotno] != SLRU_PAGE_EMPTY);

	/*
	 * The reason for the if-test is that there are often many consecutive
	 * accesses to the same page (particularly the latest page).  By
	 * suppressing useless increments of bank_cur_lru_count, we reduce the
	 * probability that old pages' counts will "wrap around" and make them
	 * appear recently used.
	 *
	 * We allow this code to be executed concurrently by multiple processes
	 * within SimpleLruReadPage_ReadOnly().  As long as int reads and writes
	 * are atomic, this should not cause any completely-bogus values to enter
	 * the computation.  However, it is possible for either bank_cur_lru_count
	 * or individual page_lru_count entries to be "reset" to lower values than
	 * they should have, in case a process is delayed while it executes this
	 * function.  With care in SlruSelectLRUPage(), this does little harm, and
	 * in any case the absolute worst possible consequence is a nonoptimal
	 * choice of page to evict.  The gain from allowing concurrent reads of
	 * SLRU pages seems worth it.
	 */
	if (new_lru_count != shared->page_lru_count[slotno])
	{
		shared->bank_cur_lru_count[bankno] = ++new_lru_count;
		shared->page_lru_count[slotno] = new_lru_count;
	}
}

/*
 * Select the slot to re-use when we need a free slot for the given page.
 *
 * The target page number is passed not only because we need to know the
 * correct bank to use, but also because we need to consider the possibility
 * that some other process reads in the target page while we are doing I/O to
 * free a slot.  Hence, check or recheck to see if any slot already holds the
 * target page, and return that slot if so.  Thus, the returned slot is
 * *either* a slot already holding the pageno (could be any state except
 * EMPTY), *or* a freeable slot (state EMPTY or CLEAN).
 *
 * The correct bank lock must be held at entry, and will be held at exit.
 */
static int
SlruSelectLRUPage(SlruCtl ctl, int64 pageno)
{
	SlruShared	shared = ctl->shared;

	/* Outer loop handles restart after I/O */
	for (;;)
	{
		int			cur_count;
		int			bestvalidslot = 0;	/* keep compiler quiet */
		int			best_valid_delta = -1;
		int64		best_valid_page_number = 0; /* keep compiler quiet */
		int			bestinvalidslot = 0;	/* keep compiler quiet */
		int			best_invalid_delta = -1;
		int64		best_invalid_page_number = 0;	/* keep compiler quiet */
		int			bankno = pageno % ctl->nbanks;
		int			bankstart = bankno * SLRU_BANK_SIZE;
		int			bankend = bankstart + SLRU_BANK_SIZE;

		Assert(LWLockHeldByMe(SimpleLruGetBankLock(ctl, pageno)));

		/* See if page already has a buffer assigned */
		for (int slotno = bankstart; slotno < bankend; slotno++)
		{
			if (shared->page_status[slotno] != SLRU_PAGE_EMPTY &&
				shared->page_number[slotno] == pageno)
				return slotno;
		}

		/*
		 * If we find any EMPTY slot, just select that one. Else choose a
		 * victim page to replace.  We normally take the least recently used
		 * valid page, but we will never take the slot containing
		 * latest_page_number, even if it appears least recently used.  We
		 * will select a slot that is already I/O busy only if there is no
		 * other choice: a read-busy slot will not be least recently used once
		 * the read finishes, and waiting for an I/O on a write-busy slot is
		 * inferior to just picking some other slot.  Testing shows the slot
		 * we pick instead will often be clean, allowing us to begin a read at
		 * once.
		 *
		 * Normally the page_lru_count values will all be different and so
		 * there will be a well-defined LRU page.  But since we allow
		 * concurrent execution of SlruRecentlyUsed() within
		 * SimpleLruReadPage_ReadOnly(), it is possible that multiple pages
		 * acquire the same lru_count values.  In that case we break ties by
		 * choosing the furthest-back page.
		 *
		 * Notice that this next line forcibly advances cur_lru_count to a
		 * value that is certainly beyond any value that will be in the
		 * page_lru_count array after the loop finishes.  This ensures that
		 * the next execution of SlruRecentlyUsed will mark the page newly
		 * used, even if it's for a page that has the current counter value.
		 * That gets us back on the path to having good data when there are
		 * multiple pages with the same lru_count.
		 */
		cur_count = (shared->bank_cur_lru_count[bankno])++;
		for (int slotno = bankstart; slotno < bankend; slotno++)
		{
			int			this_delta;
			int64		this_page_number;

			if (shared->page_status[slotno] == SLRU_PAGE_EMPTY)
				return slotno;

			this_delta = cur_count - shared->page_lru_count[slotno];
			if (this_delta < 0)
			{
				/*
				 * Clean up in case shared updates have caused cur_count
				 * increments to get "lost".  We back off the page counts,
				 * rather than trying to increase cur_count, to avoid any
				 * question of infinite loops or failure in the presence of
				 * wrapped-around counts.
				 */
				shared->page_lru_count[slotno] = cur_count;
				this_delta = 0;
			}

			/*
			 * If this page is the one most recently zeroed, don't consider it
			 * an eviction candidate. See comments in SimpleLruZeroPage for an
			 * explanation about the lack of a memory barrier here.
			 */
			this_page_number = shared->page_number[slotno];
			if (this_page_number ==
				pg_atomic_read_u64(&shared->latest_page_number))
				continue;

			if (shared->page_status[slotno] == SLRU_PAGE_VALID)
			{
				if (this_delta > best_valid_delta ||
					(this_delta == best_valid_delta &&
					 ctl->PagePrecedes(this_page_number,
									   best_valid_page_number)))
				{
					bestvalidslot = slotno;
					best_valid_delta = this_delta;
					best_valid_page_number = this_page_number;
				}
			}
			else
			{
				if (this_delta > best_invalid_delta ||
					(this_delta == best_invalid_delta &&
					 ctl->PagePrecedes(this_page_number,
									   best_invalid_page_number)))
				{
					bestinvalidslot = slotno;
					best_invalid_delta = this_delta;
					best_invalid_page_number = this_page_number;
				}
			}
		}

		/*
		 * If all pages (except possibly the latest one) are I/O busy, we'll
		 * have to wait for an I/O to complete and then retry.  In that
		 * unhappy case, we choose to wait for the I/O on the least recently
		 * used slot, on the assumption that it was likely initiated first of
		 * all the I/Os in progress and may therefore finish first.
		 */
		if (best_valid_delta < 0)
		{
			SimpleLruWaitIO(ctl, bestinvalidslot);
			continue;
		}

		/*
		 * If the selected page is clean, we're set.
		 */
		if (!shared->page_dirty[bestvalidslot])
			return bestvalidslot;

		/*
		 * Write the page.
		 */
		SlruInternalWritePage(ctl, bestvalidslot, NULL);

		/*
		 * Now loop back and try again.  This is the easiest way of dealing
		 * with corner cases such as the victim page being re-dirtied while we
		 * wrote it.
		 */
	}
}

/*
 * Write dirty pages to disk during checkpoint or database shutdown.  Flushing
 * is deferred until the next call to ProcessSyncRequests(), though we do fsync
 * the containing directory here to make sure that newly created directory
 * entries are on disk.
 */
void
SimpleLruWriteAll(SlruCtl ctl, bool allow_redirtied)
{
	SlruShared	shared = ctl->shared;
	SlruWriteAllData fdata;
	int64		pageno = 0;
	int			prevbank = SlotGetBankNumber(0);
	bool		ok;

	/* update the stats counter of flushes */
	pgstat_count_slru_flush(shared->slru_stats_idx);

	/*
	 * Find and write dirty pages
	 */
	fdata.num_files = 0;

	LWLockAcquire(&shared->bank_locks[prevbank].lock, LW_EXCLUSIVE);

	for (int slotno = 0; slotno < shared->num_slots; slotno++)
	{
		int			curbank = SlotGetBankNumber(slotno);

		/*
		 * If the current bank lock is not same as the previous bank lock then
		 * release the previous lock and acquire the new lock.
		 */
		if (curbank != prevbank)
		{
			LWLockRelease(&shared->bank_locks[prevbank].lock);
			LWLockAcquire(&shared->bank_locks[curbank].lock, LW_EXCLUSIVE);
			prevbank = curbank;
		}

		/* Do nothing if slot is unused */
		if (shared->page_status[slotno] == SLRU_PAGE_EMPTY)
			continue;

		SlruInternalWritePage(ctl, slotno, &fdata);

		/*
		 * In some places (e.g. checkpoints), we cannot assert that the slot
		 * is clean now, since another process might have re-dirtied it
		 * already.  That's okay.
		 */
		Assert(allow_redirtied ||
			   shared->page_status[slotno] == SLRU_PAGE_EMPTY ||
			   (shared->page_status[slotno] == SLRU_PAGE_VALID &&
				!shared->page_dirty[slotno]));
	}

	LWLockRelease(&shared->bank_locks[prevbank].lock);

	/*
	 * Now close any files that were open
	 */
	ok = true;
	for (int i = 0; i < fdata.num_files; i++)
	{
		if (CloseTransientFile(fdata.fd[i]) != 0)
		{
			slru_errcause = SLRU_CLOSE_FAILED;
			slru_errno = errno;
			pageno = fdata.segno[i] * SLRU_PAGES_PER_SEGMENT;
			ok = false;
		}
	}
	if (!ok)
		SlruReportIOError(ctl, pageno, InvalidTransactionId);

	/* Ensure that directory entries for new files are on disk. */
	if (ctl->sync_handler != SYNC_HANDLER_NONE)
		fsync_fname(ctl->Dir, true);
}

/*
 * Remove all segments before the one holding the passed page number
 *
 * All SLRUs prevent concurrent calls to this function, either with an LWLock
 * or by calling it only as part of a checkpoint.  Mutual exclusion must begin
 * before computing cutoffPage.  Mutual exclusion must end after any limit
 * update that would permit other backends to write fresh data into the
 * segment immediately preceding the one containing cutoffPage.  Otherwise,
 * when the SLRU is quite full, SimpleLruTruncate() might delete that segment
 * after it has accrued freshly-written data.
 */
void
SimpleLruTruncate(SlruCtl ctl, int64 cutoffPage)
{
	SlruShared	shared = ctl->shared;
	int			prevbank;

	/* update the stats counter of truncates */
	pgstat_count_slru_truncate(shared->slru_stats_idx);

	/*
	 * Scan shared memory and remove any pages preceding the cutoff page, to
	 * ensure we won't rewrite them later.  (Since this is normally called in
	 * or just after a checkpoint, any dirty pages should have been flushed
	 * already ... we're just being extra careful here.)
	 */
restart:

	/*
	 * An important safety check: the current endpoint page must not be
	 * eligible for removal.  This check is just a backstop against wraparound
	 * bugs elsewhere in SLRU handling, so we don't care if we read a slightly
	 * outdated value; therefore we don't add a memory barrier.
	 */
	if (ctl->PagePrecedes(pg_atomic_read_u64(&shared->latest_page_number),
						  cutoffPage))
	{
		ereport(LOG,
				(errmsg("could not truncate directory \"%s\": apparent wraparound",
						ctl->Dir)));
		return;
	}

	prevbank = SlotGetBankNumber(0);
	LWLockAcquire(&shared->bank_locks[prevbank].lock, LW_EXCLUSIVE);
	for (int slotno = 0; slotno < shared->num_slots; slotno++)
	{
		int			curbank = SlotGetBankNumber(slotno);

		/*
		 * If the current bank lock is not same as the previous bank lock then
		 * release the previous lock and acquire the new lock.
		 */
		if (curbank != prevbank)
		{
			LWLockRelease(&shared->bank_locks[prevbank].lock);
			LWLockAcquire(&shared->bank_locks[curbank].lock, LW_EXCLUSIVE);
			prevbank = curbank;
		}

		if (shared->page_status[slotno] == SLRU_PAGE_EMPTY)
			continue;
		if (!ctl->PagePrecedes(shared->page_number[slotno], cutoffPage))
			continue;

		/*
		 * If page is clean, just change state to EMPTY (expected case).
		 */
		if (shared->page_status[slotno] == SLRU_PAGE_VALID &&
			!shared->page_dirty[slotno])
		{
			shared->page_status[slotno] = SLRU_PAGE_EMPTY;
			continue;
		}

		/*
		 * Hmm, we have (or may have) I/O operations acting on the page, so
		 * we've got to wait for them to finish and then start again. This is
		 * the same logic as in SlruSelectLRUPage.  (XXX if page is dirty,
		 * wouldn't it be OK to just discard it without writing it?
		 * SlruMayDeleteSegment() uses a stricter qualification, so we might
		 * not delete this page in the end; even if we don't delete it, we
		 * won't have cause to read its data again.  For now, keep the logic
		 * the same as it was.)
		 */
		if (shared->page_status[slotno] == SLRU_PAGE_VALID)
			SlruInternalWritePage(ctl, slotno, NULL);
		else
			SimpleLruWaitIO(ctl, slotno);

		LWLockRelease(&shared->bank_locks[prevbank].lock);
		goto restart;
	}

	LWLockRelease(&shared->bank_locks[prevbank].lock);

	/* Now we can remove the old segment(s) */
	(void) SlruScanDirectory(ctl, SlruScanDirCbDeleteCutoff, &cutoffPage);
}

/*
 * Delete an individual SLRU segment.
 *
 * NB: This does not touch the SLRU buffers themselves, callers have to ensure
 * they either can't yet contain anything, or have already been cleaned out.
 */
static void
SlruInternalDeleteSegment(SlruCtl ctl, int64 segno)
{
	char		path[MAXPGPATH];

	/* Forget any fsync requests queued for this segment. */
	if (ctl->sync_handler != SYNC_HANDLER_NONE)
	{
		FileTag		tag;

		INIT_SLRUFILETAG(tag, ctl->sync_handler, segno);
		RegisterSyncRequest(&tag, SYNC_FORGET_REQUEST, true);
	}

	/* Unlink the file. */
	SlruFileName(ctl, path, segno);
	ereport(DEBUG2, (errmsg_internal("removing file \"%s\"", path)));
	unlink(path);
}

/*
 * Delete an individual SLRU segment, identified by the segment number.
 */
void
SlruDeleteSegment(SlruCtl ctl, int64 segno)
{
	SlruShared	shared = ctl->shared;
	int			prevbank = SlotGetBankNumber(0);
	bool		did_write;

	/* Clean out any possibly existing references to the segment. */
	LWLockAcquire(&shared->bank_locks[prevbank].lock, LW_EXCLUSIVE);
restart:
	did_write = false;
	for (int slotno = 0; slotno < shared->num_slots; slotno++)
	{
		int64		pagesegno;
		int			curbank = SlotGetBankNumber(slotno);

		/*
		 * If the current bank lock is not same as the previous bank lock then
		 * release the previous lock and acquire the new lock.
		 */
		if (curbank != prevbank)
		{
			LWLockRelease(&shared->bank_locks[prevbank].lock);
			LWLockAcquire(&shared->bank_locks[curbank].lock, LW_EXCLUSIVE);
			prevbank = curbank;
		}

		if (shared->page_status[slotno] == SLRU_PAGE_EMPTY)
			continue;

		pagesegno = shared->page_number[slotno] / SLRU_PAGES_PER_SEGMENT;
		/* not the segment we're looking for */
		if (pagesegno != segno)
			continue;

		/* If page is clean, just change state to EMPTY (expected case). */
		if (shared->page_status[slotno] == SLRU_PAGE_VALID &&
			!shared->page_dirty[slotno])
		{
			shared->page_status[slotno] = SLRU_PAGE_EMPTY;
			continue;
		}

		/* Same logic as SimpleLruTruncate() */
		if (shared->page_status[slotno] == SLRU_PAGE_VALID)
			SlruInternalWritePage(ctl, slotno, NULL);
		else
			SimpleLruWaitIO(ctl, slotno);

		did_write = true;
	}

	/*
	 * Be extra careful and re-check. The IO functions release the control
	 * lock, so new pages could have been read in.
	 */
	if (did_write)
		goto restart;

	SlruInternalDeleteSegment(ctl, segno);

	LWLockRelease(&shared->bank_locks[prevbank].lock);
}

/*
 * Determine whether a segment is okay to delete.
 *
 * segpage is the first page of the segment, and cutoffPage is the oldest (in
 * PagePrecedes order) page in the SLRU containing still-useful data.  Since
 * every core PagePrecedes callback implements "wrap around", check the
 * segment's first and last pages:
 *
 * first<cutoff  && last<cutoff:  yes
 * first<cutoff  && last>=cutoff: no; cutoff falls inside this segment
 * first>=cutoff && last<cutoff:  no; wrap point falls inside this segment
 * first>=cutoff && last>=cutoff: no; every page of this segment is too young
 */
static bool
SlruMayDeleteSegment(SlruCtl ctl, int64 segpage, int64 cutoffPage)
{
	int64		seg_last_page = segpage + SLRU_PAGES_PER_SEGMENT - 1;

	Assert(segpage % SLRU_PAGES_PER_SEGMENT == 0);

	return (ctl->PagePrecedes(segpage, cutoffPage) &&
			ctl->PagePrecedes(seg_last_page, cutoffPage));
}

#ifdef USE_ASSERT_CHECKING
static void
SlruPagePrecedesTestOffset(SlruCtl ctl, int per_page, uint32 offset)
{
	TransactionId lhs,
				rhs;
	int64		newestPage,
				oldestPage;
	TransactionId newestXact,
				oldestXact;

	/*
	 * Compare an XID pair having undefined order (see RFC 1982), a pair at
	 * "opposite ends" of the XID space.  TransactionIdPrecedes() treats each
	 * as preceding the other.  If RHS is oldestXact, LHS is the first XID we
	 * must not assign.
	 */
	lhs = per_page + offset;	/* skip first page to avoid non-normal XIDs */
	rhs = lhs + (1ULL << 63);
	Assert(TransactionIdPrecedes(lhs, rhs));
	Assert(TransactionIdPrecedes(rhs, lhs));
	Assert(!TransactionIdPrecedes(lhs - 1, rhs));
	Assert(TransactionIdPrecedes(rhs, lhs - 1));
	Assert(TransactionIdPrecedes(lhs + 1, rhs));
	Assert(!TransactionIdPrecedes(rhs, lhs + 1));
	Assert(!TransactionIdFollowsOrEquals(lhs, rhs));
	Assert(!TransactionIdFollowsOrEquals(rhs, lhs));
	Assert(!ctl->PagePrecedes(lhs / per_page, lhs / per_page));
	Assert(!ctl->PagePrecedes(lhs / per_page, rhs / per_page));
	Assert(!ctl->PagePrecedes(rhs / per_page, lhs / per_page));
	Assert(!ctl->PagePrecedes((lhs - per_page) / per_page, rhs / per_page));
	Assert(ctl->PagePrecedes(rhs / per_page, (lhs - 3 * per_page) / per_page));
	Assert(ctl->PagePrecedes(rhs / per_page, (lhs - 2 * per_page) / per_page));
	Assert(ctl->PagePrecedes(rhs / per_page, (lhs - 1 * per_page) / per_page)
		   || (1ULL << 63) % per_page != 0);	/* See CommitTsPagePrecedes() */
	Assert(ctl->PagePrecedes((lhs + 1 * per_page) / per_page, rhs / per_page)
		   || (1ULL << 63) % per_page != 0);
	Assert(ctl->PagePrecedes((lhs + 2 * per_page) / per_page, rhs / per_page));
	Assert(ctl->PagePrecedes((lhs + 3 * per_page) / per_page, rhs / per_page));
	Assert(!ctl->PagePrecedes(rhs / per_page, (lhs + per_page) / per_page));


	/*
	 * GetNewTransactionId() has assigned the last XID it can safely use, and
	 * that XID is in the *LAST* page of the second segment.  We must not
	 * delete that segment.
	 */
	newestPage = 2 * SLRU_PAGES_PER_SEGMENT - 1;
	newestXact = newestPage * per_page + offset;
	Assert(newestXact / per_page == newestPage);
	oldestXact = newestXact + 1;
	oldestXact -= 1ULL << 63;
	oldestPage = oldestXact / per_page;
	Assert(!SlruMayDeleteSegment(ctl,
								 (newestPage -
								  newestPage % SLRU_PAGES_PER_SEGMENT),
								 oldestPage));

	/*
	 * GetNewTransactionId() has assigned the last XID it can safely use, and
	 * that XID is in the *FIRST* page of the second segment.  We must not
	 * delete that segment.
	 */
	newestPage = SLRU_PAGES_PER_SEGMENT;
	newestXact = newestPage * per_page + offset;
	Assert(newestXact / per_page == newestPage);
	oldestXact = newestXact + 1;
	oldestXact -= 1ULL << 63;
	oldestPage = oldestXact / per_page;
	Assert(!SlruMayDeleteSegment(ctl,
								 (newestPage -
								  newestPage % SLRU_PAGES_PER_SEGMENT),
								 oldestPage));
}

/*
 * Unit-test a PagePrecedes function.
 *
 * This assumes every uint32 >= FirstNormalTransactionId is a valid key.  It
 * assumes each value occupies a contiguous, fixed-size region of SLRU bytes.
 * (MultiXactMemberCtl separates flags from XIDs.  NotifyCtl has
 * variable-length entries, no keys, and no random access.  These unit tests
 * do not apply to them.)
 */
void
SlruPagePrecedesUnitTests(SlruCtl ctl, int per_page)
{
	/* Test first, middle and last entries of a page. */
	SlruPagePrecedesTestOffset(ctl, per_page, 0);
	SlruPagePrecedesTestOffset(ctl, per_page, per_page / 2);
	SlruPagePrecedesTestOffset(ctl, per_page, per_page - 1);
}
#endif

/*
 * SlruScanDirectory callback
 *		This callback reports true if there's any segment wholly prior to the
 *		one containing the page passed as "data".
 */
bool
SlruScanDirCbReportPresence(SlruCtl ctl, char *filename, int64 segpage,
							void *data)
{
	int64		cutoffPage = *(int64 *) data;

	if (SlruMayDeleteSegment(ctl, segpage, cutoffPage))
		return true;			/* found one; don't iterate any more */

	return false;				/* keep going */
}

/*
 * SlruScanDirectory callback.
 *		This callback deletes segments prior to the one passed in as "data".
 */
static bool
SlruScanDirCbDeleteCutoff(SlruCtl ctl, char *filename, int64 segpage,
						  void *data)
{
	int64		cutoffPage = *(int64 *) data;

	if (SlruMayDeleteSegment(ctl, segpage, cutoffPage))
		SlruInternalDeleteSegment(ctl, segpage / SLRU_PAGES_PER_SEGMENT);

	return false;				/* keep going */
}

/*
 * SlruScanDirectory callback.
 *		This callback deletes all segments.
 */
bool
SlruScanDirCbDeleteAll(SlruCtl ctl, char *filename, int64 segpage, void *data)
{
	SlruInternalDeleteSegment(ctl, segpage / SLRU_PAGES_PER_SEGMENT);

	return false;				/* keep going */
}

/*
 * An internal function used by SlruScanDirectory().
 *
 * Returns true if a file with a name of a given length may be a correct
 * SLRU segment.
 */
static inline bool
SlruCorrectSegmentFilenameLength(SlruCtl ctl, size_t len)
{
	if (ctl->long_segment_names)
		return (len == 15);		/* see SlruFileName() */
	else

		/*
		 * Commit 638cf09e76d allowed 5-character lengths. Later commit
		 * 73c986adde5 allowed 6-character length.
		 *
		 * Note: There is an ongoing plan to migrate all SLRUs to 64-bit page
		 * numbers, and the corresponding 15-character file names, which may
		 * eventually deprecate the support for 4, 5, and 6-character names.
		 */
		return (len == 4 || len == 5 || len == 6);
}

/*
 * Scan the SimpleLru directory and apply a callback to each file found in it.
 *
 * If the callback returns true, the scan is stopped.  The last return value
 * from the callback is returned.
 *
 * The callback receives the following arguments: 1. the SlruCtl struct for the
 * slru being truncated; 2. the filename being considered; 3. the page number
 * for the first page of that file; 4. a pointer to the opaque data given to us
 * by the caller.
 *
 * Note that the ordering in which the directory is scanned is not guaranteed.
 *
 * Note that no locking is applied.
 */
bool
SlruScanDirectory(SlruCtl ctl, SlruScanCallback callback, void *data)
{
	bool		retval = false;
	DIR		   *cldir;
	struct dirent *clde;
	int64		segno;
	int64		segpage;

	cldir = AllocateDir(ctl->Dir);
	while ((clde = ReadDir(cldir, ctl->Dir)) != NULL)
	{
		size_t		len;

		len = strlen(clde->d_name);

		if (SlruCorrectSegmentFilenameLength(ctl, len) &&
			strspn(clde->d_name, "0123456789ABCDEF") == len)
		{
			segno = strtoi64(clde->d_name, NULL, 16);
			segpage = segno * SLRU_PAGES_PER_SEGMENT;

			elog(DEBUG2, "SlruScanDirectory invoking callback on %s/%s",
				 ctl->Dir, clde->d_name);
			retval = callback(ctl, clde->d_name, segpage, data);
			if (retval)
				break;
		}
	}
	FreeDir(cldir);

	return retval;
}

/*
 * Individual SLRUs (clog, ...) have to provide a sync.c handler function so
 * that they can provide the correct "SlruCtl" (otherwise we don't know how to
 * build the path), but they just forward to this common implementation that
 * performs the fsync.
 */
int
SlruSyncFileTag(SlruCtl ctl, const FileTag *ftag, char *path)
{
	int			fd;
	int			save_errno;
	int			result;

	SlruFileName(ctl, path, ftag->segno);

	fd = OpenTransientFile(path, O_RDWR | PG_BINARY);
	if (fd < 0)
		return -1;

	pgstat_report_wait_start(WAIT_EVENT_SLRU_FLUSH_SYNC);
	result = pg_fsync(fd);
	pgstat_report_wait_end();
	save_errno = errno;

	CloseTransientFile(fd);

	errno = save_errno;
	return result;
}

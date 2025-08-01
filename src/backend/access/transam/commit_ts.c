/*-------------------------------------------------------------------------
 *
 * commit_ts.c
 *		PostgreSQL commit timestamp manager
 *
 * This module is a pg_xact-like system that stores the commit timestamp
 * for each transaction.
 *
 * XLOG interactions: this module generates an XLOG record whenever a new
 * CommitTs page is initialized to zeroes.  Other writes of CommitTS come
 * from recording of transaction commit in xact.c, which generates its own
 * XLOG records for these events and will re-perform the status update on
 * redo; so we need make no additional XLOG entry here.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/transam/commit_ts.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/commit_ts.h"
#include "access/htup_details.h"
#include "access/slru.h"
#include "access/transam.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/shmem.h"
#include "utils/fmgrprotos.h"
#include "utils/guc_hooks.h"
#include "utils/timestamp.h"

/*
 * Defines for CommitTs page sizes.  A page is the same BLCKSZ as is used
 * everywhere else in Postgres.
 *
 * Note: because TransactionIds are 32 bits and wrap around at 0xFFFFFFFF,
 * CommitTs page numbering also wraps around at
 * 0xFFFFFFFF/COMMIT_TS_XACTS_PER_PAGE, and CommitTs segment numbering at
 * 0xFFFFFFFF/COMMIT_TS_XACTS_PER_PAGE/SLRU_PAGES_PER_SEGMENT.  We need take no
 * explicit notice of that fact in this module, except when comparing segment
 * and page numbers in TruncateCommitTs (see CommitTsPagePrecedes).
 */

/*
 * We need 8+2 bytes per xact.  Note that enlarging this struct might mean
 * the largest possible file name is more than 5 chars long; see
 * SlruScanDirectory.
 */
typedef struct CommitTimestampEntry
{
	TimestampTz time;
	RepOriginId nodeid;
} CommitTimestampEntry;

#define SizeOfCommitTimestampEntry (offsetof(CommitTimestampEntry, nodeid) + \
									sizeof(RepOriginId))

#define COMMIT_TS_XACTS_PER_PAGE \
	(BLCKSZ / SizeOfCommitTimestampEntry)


/*
 * Although we return an int64 the actual value can't currently exceed
 * 0xFFFFFFFF/COMMIT_TS_XACTS_PER_PAGE.
 */
static inline int64
TransactionIdToCTsPage(TransactionId xid)
{
	return xid / (int64) COMMIT_TS_XACTS_PER_PAGE;
}

#define TransactionIdToCTsEntry(xid)	\
	((xid) % (TransactionId) COMMIT_TS_XACTS_PER_PAGE)

/*
 * Link to shared-memory data structures for CommitTs control
 */
static SlruCtlData CommitTsCtlData;

#define CommitTsCtl (&CommitTsCtlData)

/*
 * We keep a cache of the last value set in shared memory.
 *
 * This is also good place to keep the activation status.  We keep this
 * separate from the GUC so that the standby can activate the module if the
 * primary has it active independently of the value of the GUC.
 *
 * This is protected by CommitTsLock.  In some places, we use commitTsActive
 * without acquiring the lock; where this happens, a comment explains the
 * rationale for it.
 */
typedef struct CommitTimestampShared
{
	TransactionId xidLastCommit;
	CommitTimestampEntry dataLastCommit;
	bool		commitTsActive;
} CommitTimestampShared;

static CommitTimestampShared *commitTsShared;


/* GUC variable */
bool		track_commit_timestamp;

static void SetXidCommitTsInPage(TransactionId xid, int nsubxids,
								 TransactionId *subxids, TimestampTz ts,
								 RepOriginId nodeid, int64 pageno);
static void TransactionIdSetCommitTs(TransactionId xid, TimestampTz ts,
									 RepOriginId nodeid, int slotno);
static void error_commit_ts_disabled(void);
static bool CommitTsPagePrecedes(int64 page1, int64 page2);
static void ActivateCommitTs(void);
static void DeactivateCommitTs(void);
static void WriteTruncateXlogRec(int64 pageno, TransactionId oldestXid);

/*
 * TransactionTreeSetCommitTsData
 *
 * Record the final commit timestamp of transaction entries in the commit log
 * for a transaction and its subtransaction tree, as efficiently as possible.
 *
 * xid is the top level transaction id.
 *
 * subxids is an array of xids of length nsubxids, representing subtransactions
 * in the tree of xid. In various cases nsubxids may be zero.
 * The reason why tracking just the parent xid commit timestamp is not enough
 * is that the subtrans SLRU does not stay valid across crashes (it's not
 * permanent) so we need to keep the information about them here. If the
 * subtrans implementation changes in the future, we might want to revisit the
 * decision of storing timestamp info for each subxid.
 */
void
TransactionTreeSetCommitTsData(TransactionId xid, int nsubxids,
							   TransactionId *subxids, TimestampTz timestamp,
							   RepOriginId nodeid)
{
	int			i;
	TransactionId headxid;
	TransactionId newestXact;

	/*
	 * No-op if the module is not active.
	 *
	 * An unlocked read here is fine, because in a standby (the only place
	 * where the flag can change in flight) this routine is only called by the
	 * recovery process, which is also the only process which can change the
	 * flag.
	 */
	if (!commitTsShared->commitTsActive)
		return;

	/*
	 * Figure out the latest Xid in this batch: either the last subxid if
	 * there's any, otherwise the parent xid.
	 */
	if (nsubxids > 0)
		newestXact = subxids[nsubxids - 1];
	else
		newestXact = xid;

	/*
	 * We split the xids to set the timestamp to in groups belonging to the
	 * same SLRU page; the first element in each such set is its head.  The
	 * first group has the main XID as the head; subsequent sets use the first
	 * subxid not on the previous page as head.  This way, we only have to
	 * lock/modify each SLRU page once.
	 */
	headxid = xid;
	i = 0;
	for (;;)
	{
		int64		pageno = TransactionIdToCTsPage(headxid);
		int			j;

		for (j = i; j < nsubxids; j++)
		{
			if (TransactionIdToCTsPage(subxids[j]) != pageno)
				break;
		}
		/* subxids[i..j] are on the same page as the head */

		SetXidCommitTsInPage(headxid, j - i, subxids + i, timestamp, nodeid,
							 pageno);

		/* if we wrote out all subxids, we're done. */
		if (j >= nsubxids)
			break;

		/*
		 * Set the new head and skip over it, as well as over the subxids we
		 * just wrote.
		 */
		headxid = subxids[j];
		i = j + 1;
	}

	/* update the cached value in shared memory */
	LWLockAcquire(CommitTsLock, LW_EXCLUSIVE);
	commitTsShared->xidLastCommit = xid;
	commitTsShared->dataLastCommit.time = timestamp;
	commitTsShared->dataLastCommit.nodeid = nodeid;

	/* and move forwards our endpoint, if needed */
	if (TransactionIdPrecedes(TransamVariables->newestCommitTsXid, newestXact))
		TransamVariables->newestCommitTsXid = newestXact;
	LWLockRelease(CommitTsLock);
}

/*
 * Record the commit timestamp of transaction entries in the commit log for all
 * entries on a single page.  Atomic only on this page.
 */
static void
SetXidCommitTsInPage(TransactionId xid, int nsubxids,
					 TransactionId *subxids, TimestampTz ts,
					 RepOriginId nodeid, int64 pageno)
{
	LWLock	   *lock = SimpleLruGetBankLock(CommitTsCtl, pageno);
	int			slotno;
	int			i;

	LWLockAcquire(lock, LW_EXCLUSIVE);

	slotno = SimpleLruReadPage(CommitTsCtl, pageno, true, xid);

	TransactionIdSetCommitTs(xid, ts, nodeid, slotno);
	for (i = 0; i < nsubxids; i++)
		TransactionIdSetCommitTs(subxids[i], ts, nodeid, slotno);

	CommitTsCtl->shared->page_dirty[slotno] = true;

	LWLockRelease(lock);
}

/*
 * Sets the commit timestamp of a single transaction.
 *
 * Caller must hold the correct SLRU bank lock, will be held at exit
 */
static void
TransactionIdSetCommitTs(TransactionId xid, TimestampTz ts,
						 RepOriginId nodeid, int slotno)
{
	int			entryno = TransactionIdToCTsEntry(xid);
	CommitTimestampEntry entry;

	Assert(TransactionIdIsNormal(xid));

	entry.time = ts;
	entry.nodeid = nodeid;

	memcpy(CommitTsCtl->shared->page_buffer[slotno] +
		   SizeOfCommitTimestampEntry * entryno,
		   &entry, SizeOfCommitTimestampEntry);
}

/*
 * Interrogate the commit timestamp of a transaction.
 *
 * The return value indicates whether a commit timestamp record was found for
 * the given xid.  The timestamp value is returned in *ts (which may not be
 * null), and the origin node for the Xid is returned in *nodeid, if it's not
 * null.
 */
bool
TransactionIdGetCommitTsData(TransactionId xid, TimestampTz *ts,
							 RepOriginId *nodeid)
{
	int64		pageno = TransactionIdToCTsPage(xid);
	int			entryno = TransactionIdToCTsEntry(xid);
	int			slotno;
	CommitTimestampEntry entry;
	TransactionId oldestCommitTsXid;
	TransactionId newestCommitTsXid;

	if (!TransactionIdIsValid(xid))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot retrieve commit timestamp for transaction %" PRIu64,
						xid)));
	else if (!TransactionIdIsNormal(xid))
	{
		/* frozen and bootstrap xids are always committed far in the past */
		*ts = 0;
		if (nodeid)
			*nodeid = 0;
		return false;
	}

	LWLockAcquire(CommitTsLock, LW_SHARED);

	/* Error if module not enabled */
	if (!commitTsShared->commitTsActive)
		error_commit_ts_disabled();

	/*
	 * If we're asked for the cached value, return that.  Otherwise, fall
	 * through to read from SLRU.
	 */
	if (commitTsShared->xidLastCommit == xid)
	{
		*ts = commitTsShared->dataLastCommit.time;
		if (nodeid)
			*nodeid = commitTsShared->dataLastCommit.nodeid;

		LWLockRelease(CommitTsLock);
		return *ts != 0;
	}

	oldestCommitTsXid = TransamVariables->oldestCommitTsXid;
	newestCommitTsXid = TransamVariables->newestCommitTsXid;
	/* neither is invalid, or both are */
	Assert(TransactionIdIsValid(oldestCommitTsXid) == TransactionIdIsValid(newestCommitTsXid));
	LWLockRelease(CommitTsLock);

	/*
	 * Return empty if the requested value is outside our valid range.
	 */
	if (!TransactionIdIsValid(oldestCommitTsXid) ||
		TransactionIdPrecedes(xid, oldestCommitTsXid) ||
		TransactionIdPrecedes(newestCommitTsXid, xid))
	{
		*ts = 0;
		if (nodeid)
			*nodeid = InvalidRepOriginId;
		return false;
	}

	/* lock is acquired by SimpleLruReadPage_ReadOnly */
	slotno = SimpleLruReadPage_ReadOnly(CommitTsCtl, pageno, xid);
	memcpy(&entry,
		   CommitTsCtl->shared->page_buffer[slotno] +
		   SizeOfCommitTimestampEntry * entryno,
		   SizeOfCommitTimestampEntry);

	*ts = entry.time;
	if (nodeid)
		*nodeid = entry.nodeid;

	LWLockRelease(SimpleLruGetBankLock(CommitTsCtl, pageno));
	return *ts != 0;
}

/*
 * Return the Xid of the latest committed transaction.  (As far as this module
 * is concerned, anyway; it's up to the caller to ensure the value is useful
 * for its purposes.)
 *
 * ts and nodeid are filled with the corresponding data; they can be passed
 * as NULL if not wanted.
 */
TransactionId
GetLatestCommitTsData(TimestampTz *ts, RepOriginId *nodeid)
{
	TransactionId xid;

	LWLockAcquire(CommitTsLock, LW_SHARED);

	/* Error if module not enabled */
	if (!commitTsShared->commitTsActive)
		error_commit_ts_disabled();

	xid = commitTsShared->xidLastCommit;
	if (ts)
		*ts = commitTsShared->dataLastCommit.time;
	if (nodeid)
		*nodeid = commitTsShared->dataLastCommit.nodeid;
	LWLockRelease(CommitTsLock);

	return xid;
}

static void
error_commit_ts_disabled(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
			 errmsg("could not get commit timestamp data"),
			 RecoveryInProgress() ?
			 errhint("Make sure the configuration parameter \"%s\" is set on the primary server.",
					 "track_commit_timestamp") :
			 errhint("Make sure the configuration parameter \"%s\" is set.",
					 "track_commit_timestamp")));
}

/*
 * SQL-callable wrapper to obtain commit time of a transaction
 */
Datum
pg_xact_commit_timestamp(PG_FUNCTION_ARGS)
{
	TransactionId xid = PG_GETARG_TRANSACTIONID(0);
	TimestampTz ts;
	bool		found;

	found = TransactionIdGetCommitTsData(xid, &ts, NULL);

	if (!found)
		PG_RETURN_NULL();

	PG_RETURN_TIMESTAMPTZ(ts);
}


/*
 * pg_last_committed_xact
 *
 * SQL-callable wrapper to obtain some information about the latest
 * committed transaction: transaction ID, timestamp and replication
 * origin.
 */
Datum
pg_last_committed_xact(PG_FUNCTION_ARGS)
{
	TransactionId xid;
	RepOriginId nodeid;
	TimestampTz ts;
	Datum		values[3];
	bool		nulls[3];
	TupleDesc	tupdesc;
	HeapTuple	htup;

	/* and construct a tuple with our data */
	xid = GetLatestCommitTsData(&ts, &nodeid);

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	if (!TransactionIdIsNormal(xid))
	{
		memset(nulls, true, sizeof(nulls));
	}
	else
	{
		values[0] = TransactionIdGetDatum(xid);
		nulls[0] = false;

		values[1] = TimestampTzGetDatum(ts);
		nulls[1] = false;

		values[2] = ObjectIdGetDatum((Oid) nodeid);
		nulls[2] = false;
	}

	htup = heap_form_tuple(tupdesc, values, nulls);

	PG_RETURN_DATUM(HeapTupleGetDatum(htup));
}

/*
 * pg_xact_commit_timestamp_origin
 *
 * SQL-callable wrapper to obtain commit timestamp and replication origin
 * of a given transaction.
 */
Datum
pg_xact_commit_timestamp_origin(PG_FUNCTION_ARGS)
{
	TransactionId xid = PG_GETARG_TRANSACTIONID(0);
	RepOriginId nodeid;
	TimestampTz ts;
	Datum		values[2];
	bool		nulls[2];
	TupleDesc	tupdesc;
	HeapTuple	htup;
	bool		found;

	found = TransactionIdGetCommitTsData(xid, &ts, &nodeid);

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	if (!found)
	{
		memset(nulls, true, sizeof(nulls));
	}
	else
	{
		values[0] = TimestampTzGetDatum(ts);
		nulls[0] = false;

		values[1] = ObjectIdGetDatum((Oid) nodeid);
		nulls[1] = false;
	}

	htup = heap_form_tuple(tupdesc, values, nulls);

	PG_RETURN_DATUM(HeapTupleGetDatum(htup));
}

/*
 * Number of shared CommitTS buffers.
 *
 * If asked to autotune, use 2MB for every 1GB of shared buffers, up to 8MB.
 * Otherwise just cap the configured amount to be between 16 and the maximum
 * allowed.
 */
static int
CommitTsShmemBuffers(void)
{
	/* auto-tune based on shared buffers */
	if (commit_timestamp_buffers == 0)
		return SimpleLruAutotuneBuffers(512, 1024);

	return Min(Max(16, commit_timestamp_buffers), SLRU_MAX_ALLOWED_BUFFERS);
}

/*
 * Shared memory sizing for CommitTs
 */
Size
CommitTsShmemSize(void)
{
	return SimpleLruShmemSize(CommitTsShmemBuffers(), 0) +
		sizeof(CommitTimestampShared);
}

/*
 * Initialize CommitTs at system startup (postmaster start or standalone
 * backend)
 */
void
CommitTsShmemInit(void)
{
	bool		found;

	/* If auto-tuning is requested, now is the time to do it */
	if (commit_timestamp_buffers == 0)
	{
		char		buf[32];

		snprintf(buf, sizeof(buf), "%d", CommitTsShmemBuffers());
		SetConfigOption("commit_timestamp_buffers", buf, PGC_POSTMASTER,
						PGC_S_DYNAMIC_DEFAULT);

		/*
		 * We prefer to report this value's source as PGC_S_DYNAMIC_DEFAULT.
		 * However, if the DBA explicitly set commit_timestamp_buffers = 0 in
		 * the config file, then PGC_S_DYNAMIC_DEFAULT will fail to override
		 * that and we must force the matter with PGC_S_OVERRIDE.
		 */
		if (commit_timestamp_buffers == 0)	/* failed to apply it? */
			SetConfigOption("commit_timestamp_buffers", buf, PGC_POSTMASTER,
							PGC_S_OVERRIDE);
	}
	Assert(commit_timestamp_buffers != 0);

	CommitTsCtl->PagePrecedes = CommitTsPagePrecedes;
	SimpleLruInit(CommitTsCtl, "commit_timestamp", CommitTsShmemBuffers(), 0,
				  "pg_commit_ts", LWTRANCHE_COMMITTS_BUFFER,
				  LWTRANCHE_COMMITTS_SLRU,
				  SYNC_HANDLER_COMMIT_TS,
				  true);
	SlruPagePrecedesUnitTests(CommitTsCtl, COMMIT_TS_XACTS_PER_PAGE);

	commitTsShared = ShmemInitStruct("CommitTs shared",
									 sizeof(CommitTimestampShared),
									 &found);

	if (!IsUnderPostmaster)
	{
		Assert(!found);

		commitTsShared->xidLastCommit = InvalidTransactionId;
		TIMESTAMP_NOBEGIN(commitTsShared->dataLastCommit.time);
		commitTsShared->dataLastCommit.nodeid = InvalidRepOriginId;
		commitTsShared->commitTsActive = false;
	}
	else
		Assert(found);
}

/*
 * GUC check_hook for commit_timestamp_buffers
 */
bool
check_commit_ts_buffers(int *newval, void **extra, GucSource source)
{
	return check_slru_buffers("commit_timestamp_buffers", newval);
}

/*
 * This function must be called ONCE on system install.
 *
 * (The CommitTs directory is assumed to have been created by initdb, and
 * CommitTsShmemInit must have been called already.)
 */
void
BootStrapCommitTs(void)
{
	/*
	 * Nothing to do here at present, unlike most other SLRU modules; segments
	 * are created when the server is started with this module enabled. See
	 * ActivateCommitTs.
	 */
}

/*
 * This must be called ONCE during postmaster or standalone-backend startup,
 * after StartupXLOG has initialized TransamVariables->nextXid.
 */
void
StartupCommitTs(void)
{
	ActivateCommitTs();
}

/*
 * This must be called ONCE during postmaster or standalone-backend startup,
 * after recovery has finished.
 */
void
CompleteCommitTsInitialization(void)
{
	/*
	 * If the feature is not enabled, turn it off for good.  This also removes
	 * any leftover data.
	 *
	 * Conversely, we activate the module if the feature is enabled.  This is
	 * necessary for primary and standby as the activation depends on the
	 * control file contents at the beginning of recovery or when a
	 * XLOG_PARAMETER_CHANGE is replayed.
	 */
	if (!track_commit_timestamp)
		DeactivateCommitTs();
	else
		ActivateCommitTs();
}

/*
 * Activate or deactivate CommitTs' upon reception of a XLOG_PARAMETER_CHANGE
 * XLog record during recovery.
 */
void
CommitTsParameterChange(bool newvalue, bool oldvalue)
{
	/*
	 * If the commit_ts module is disabled in this server and we get word from
	 * the primary server that it is enabled there, activate it so that we can
	 * replay future WAL records involving it; also mark it as active on
	 * pg_control.  If the old value was already set, we already did this, so
	 * don't do anything.
	 *
	 * If the module is disabled in the primary, disable it here too, unless
	 * the module is enabled locally.
	 *
	 * Note this only runs in the recovery process, so an unlocked read is
	 * fine.
	 */
	if (newvalue)
	{
		if (!commitTsShared->commitTsActive)
			ActivateCommitTs();
	}
	else if (commitTsShared->commitTsActive)
		DeactivateCommitTs();
}

/*
 * Activate this module whenever necessary.
 *		This must happen during postmaster or standalone-backend startup,
 *		or during WAL replay anytime the track_commit_timestamp setting is
 *		changed in the primary.
 *
 * The reason why this SLRU needs separate activation/deactivation functions is
 * that it can be enabled/disabled during start and the activation/deactivation
 * on the primary is propagated to the standby via replay. Other SLRUs don't
 * have this property and they can be just initialized during normal startup.
 *
 * This is in charge of creating the currently active segment, if it's not
 * already there.  The reason for this is that the server might have been
 * running with this module disabled for a while and thus might have skipped
 * the normal creation point.
 */
static void
ActivateCommitTs(void)
{
	TransactionId xid;
	int64		pageno;

	/*
	 * During bootstrap, we should not register commit timestamps so skip the
	 * activation in this case.
	 */
	if (IsBootstrapProcessingMode())
		return;

	/* If we've done this already, there's nothing to do */
	LWLockAcquire(CommitTsLock, LW_EXCLUSIVE);
	if (commitTsShared->commitTsActive)
	{
		LWLockRelease(CommitTsLock);
		return;
	}
	LWLockRelease(CommitTsLock);

	xid = XidFromFullTransactionId(TransamVariables->nextXid);
	pageno = TransactionIdToCTsPage(xid);

	/*
	 * Re-Initialize our idea of the latest page number.
	 */
	pg_atomic_write_u64(&CommitTsCtl->shared->latest_page_number, pageno);

	/*
	 * If CommitTs is enabled, but it wasn't in the previous server run, we
	 * need to set the oldest and newest values to the next Xid; that way, we
	 * will not try to read data that might not have been set.
	 *
	 * XXX does this have a problem if a server is started with commitTs
	 * enabled, then started with commitTs disabled, then restarted with it
	 * enabled again?  It doesn't look like it does, because there should be a
	 * checkpoint that sets the value to InvalidTransactionId at end of
	 * recovery; and so any chance of injecting new transactions without
	 * CommitTs values would occur after the oldestCommitTsXid has been set to
	 * Invalid temporarily.
	 */
	LWLockAcquire(CommitTsLock, LW_EXCLUSIVE);
	if (TransamVariables->oldestCommitTsXid == InvalidTransactionId)
	{
		TransamVariables->oldestCommitTsXid =
			TransamVariables->newestCommitTsXid = ReadNextTransactionId();
	}
	LWLockRelease(CommitTsLock);

	/* Create the current segment file, if necessary */
	if (!SimpleLruDoesPhysicalPageExist(CommitTsCtl, pageno))
		SimpleLruZeroAndWritePage(CommitTsCtl, pageno);

	/* Change the activation status in shared memory. */
	LWLockAcquire(CommitTsLock, LW_EXCLUSIVE);
	commitTsShared->commitTsActive = true;
	LWLockRelease(CommitTsLock);
}

/*
 * Deactivate this module.
 *
 * This must be called when the track_commit_timestamp parameter is turned off.
 * This happens during postmaster or standalone-backend startup, or during WAL
 * replay.
 *
 * Resets CommitTs into invalid state to make sure we don't hand back
 * possibly-invalid data; also removes segments of old data.
 */
static void
DeactivateCommitTs(void)
{
	/*
	 * Cleanup the status in the shared memory.
	 *
	 * We reset everything in the commitTsShared record to prevent user from
	 * getting confusing data about last committed transaction on the standby
	 * when the module was activated repeatedly on the primary.
	 */
	LWLockAcquire(CommitTsLock, LW_EXCLUSIVE);

	commitTsShared->commitTsActive = false;
	commitTsShared->xidLastCommit = InvalidTransactionId;
	TIMESTAMP_NOBEGIN(commitTsShared->dataLastCommit.time);
	commitTsShared->dataLastCommit.nodeid = InvalidRepOriginId;

	TransamVariables->oldestCommitTsXid = InvalidTransactionId;
	TransamVariables->newestCommitTsXid = InvalidTransactionId;

	/*
	 * Remove *all* files.  This is necessary so that there are no leftover
	 * files; in the case where this feature is later enabled after running
	 * with it disabled for some time there may be a gap in the file sequence.
	 * (We can probably tolerate out-of-sequence files, as they are going to
	 * be overwritten anyway when we wrap around, but it seems better to be
	 * tidy.)
	 *
	 * Note that we do this with CommitTsLock acquired in exclusive mode. This
	 * is very heavy-handed, but since this routine can only be called in the
	 * replica and should happen very rarely, we don't worry too much about
	 * it.  Note also that no process should be consulting this SLRU if we
	 * have just deactivated it.
	 */
	(void) SlruScanDirectory(CommitTsCtl, SlruScanDirCbDeleteAll, NULL);

	LWLockRelease(CommitTsLock);
}

/*
 * Perform a checkpoint --- either during shutdown, or on-the-fly
 */
void
CheckPointCommitTs(void)
{
	/*
	 * Write dirty CommitTs pages to disk.  This may result in sync requests
	 * queued for later handling by ProcessSyncRequests(), as part of the
	 * checkpoint.
	 */
	SimpleLruWriteAll(CommitTsCtl, true);
}

/*
 * Make sure that CommitTs has room for a newly-allocated XID.
 *
 * NB: this is called while holding XidGenLock.  We want it to be very fast
 * most of the time; even when it's not so fast, no actual I/O need happen
 * unless we're forced to write out a dirty CommitTs or xlog page to make room
 * in shared memory.
 *
 * NB: the current implementation relies on track_commit_timestamp being
 * PGC_POSTMASTER.
 */
void
ExtendCommitTs(TransactionId newestXact)
{
	int64		pageno;
	LWLock	   *lock;

	/*
	 * Nothing to do if module not enabled.  Note we do an unlocked read of
	 * the flag here, which is okay because this routine is only called from
	 * GetNewTransactionId, which is never called in a standby.
	 */
	Assert(!InRecovery);
	if (!commitTsShared->commitTsActive)
		return;

	/*
	 * No work except at first XID of a page.  But beware: just after
	 * wraparound, the first XID of page zero is FirstNormalTransactionId.
	 */
	if (TransactionIdToCTsEntry(newestXact) != 0 &&
		!TransactionIdEquals(newestXact, FirstNormalTransactionId))
		return;

	pageno = TransactionIdToCTsPage(newestXact);

	lock = SimpleLruGetBankLock(CommitTsCtl, pageno);

	LWLockAcquire(lock, LW_EXCLUSIVE);

	/* Zero the page ... */
	SimpleLruZeroPage(CommitTsCtl, pageno);

	/* and make a WAL entry about that, unless we're in REDO */
	if (!InRecovery)
		XLogSimpleInsertInt64(RM_COMMIT_TS_ID, COMMIT_TS_ZEROPAGE, pageno);

	LWLockRelease(lock);
}

/*
 * Remove all CommitTs segments before the one holding the passed
 * transaction ID.
 *
 * Note that we don't need to flush XLOG here.
 */
void
TruncateCommitTs(TransactionId oldestXact)
{
	int64		cutoffPage;

	/*
	 * The cutoff point is the start of the segment containing oldestXact. We
	 * pass the *page* containing oldestXact to SimpleLruTruncate.
	 */
	cutoffPage = TransactionIdToCTsPage(oldestXact);

	/* Check to see if there's any files that could be removed */
	if (!SlruScanDirectory(CommitTsCtl, SlruScanDirCbReportPresence,
						   &cutoffPage))
		return;					/* nothing to remove */

	/* Write XLOG record */
	WriteTruncateXlogRec(cutoffPage, oldestXact);

	/* Now we can remove the old CommitTs segment(s) */
	SimpleLruTruncate(CommitTsCtl, cutoffPage);
}

/*
 * Set the limit values between which commit TS can be consulted.
 */
void
SetCommitTsLimit(TransactionId oldestXact, TransactionId newestXact)
{
	/*
	 * Be careful not to overwrite values that are either further into the
	 * "future" or signal a disabled committs.
	 */
	LWLockAcquire(CommitTsLock, LW_EXCLUSIVE);
	if (TransamVariables->oldestCommitTsXid != InvalidTransactionId)
	{
		if (TransactionIdPrecedes(TransamVariables->oldestCommitTsXid, oldestXact))
			TransamVariables->oldestCommitTsXid = oldestXact;
		if (TransactionIdPrecedes(newestXact, TransamVariables->newestCommitTsXid))
			TransamVariables->newestCommitTsXid = newestXact;
	}
	else
	{
		Assert(TransamVariables->newestCommitTsXid == InvalidTransactionId);
		TransamVariables->oldestCommitTsXid = oldestXact;
		TransamVariables->newestCommitTsXid = newestXact;
	}
	LWLockRelease(CommitTsLock);
}

/*
 * Move forwards the oldest commitTS value that can be consulted
 */
void
AdvanceOldestCommitTsXid(TransactionId oldestXact)
{
	LWLockAcquire(CommitTsLock, LW_EXCLUSIVE);
	if (TransamVariables->oldestCommitTsXid != InvalidTransactionId &&
		TransactionIdPrecedes(TransamVariables->oldestCommitTsXid, oldestXact))
		TransamVariables->oldestCommitTsXid = oldestXact;
	LWLockRelease(CommitTsLock);
}


/*
 * Decide whether a commitTS page number is "older" for truncation purposes.
 * Analogous to CLOGPagePrecedes().
 */
static bool
CommitTsPagePrecedes(int64 page1, int64 page2)
{
	TransactionId xid1;
	TransactionId xid2;

	xid1 = ((TransactionId) page1) * COMMIT_TS_XACTS_PER_PAGE;
	xid1 += FirstNormalTransactionId + 1;
	xid2 = ((TransactionId) page2) * COMMIT_TS_XACTS_PER_PAGE;
	xid2 += FirstNormalTransactionId + 1;

	return (TransactionIdPrecedes(xid1, xid2) &&
			TransactionIdPrecedes(xid1, xid2 + COMMIT_TS_XACTS_PER_PAGE - 1));
}


/*
 * Write a TRUNCATE xlog record
 */
static void
WriteTruncateXlogRec(int64 pageno, TransactionId oldestXid)
{
	xl_commit_ts_truncate xlrec;

	xlrec.pageno = pageno;
	xlrec.oldestXid = oldestXid;

	XLogBeginInsert();
	XLogRegisterData(&xlrec, SizeOfCommitTsTruncate);
	(void) XLogInsert(RM_COMMIT_TS_ID, COMMIT_TS_TRUNCATE);
}

/*
 * CommitTS resource manager's routines
 */
void
commit_ts_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	/* Backup blocks are not used in commit_ts records */
	Assert(!XLogRecHasAnyBlockRefs(record));

	if (info == COMMIT_TS_ZEROPAGE)
	{
		int64		pageno;

		memcpy(&pageno, XLogRecGetData(record), sizeof(pageno));
		SimpleLruZeroAndWritePage(CommitTsCtl, pageno);
	}
	else if (info == COMMIT_TS_TRUNCATE)
	{
		xl_commit_ts_truncate *trunc = (xl_commit_ts_truncate *) XLogRecGetData(record);

		AdvanceOldestCommitTsXid(trunc->oldestXid);

		/*
		 * During XLOG replay, latest_page_number isn't set up yet; insert a
		 * suitable value to bypass the sanity test in SimpleLruTruncate.
		 */
		pg_atomic_write_u64(&CommitTsCtl->shared->latest_page_number,
							trunc->pageno);

		SimpleLruTruncate(CommitTsCtl, trunc->pageno);
	}
	else
		elog(PANIC, "commit_ts_redo: unknown op code %u", info);
}

/*
 * Entrypoint for sync.c to sync commit_ts files.
 */
int
committssyncfiletag(const FileTag *ftag, char *path)
{
	return SlruSyncFileTag(CommitTsCtl, ftag, path);
}

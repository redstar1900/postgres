/*-------------------------------------------------------------------------
 *
 * bufpage.h
 *	  Standard POSTGRES buffer page definitions.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/bufpage.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BUFPAGE_H
#define BUFPAGE_H

#include "access/multixact.h"
#include "access/transam.h"
#include "access/xlogdefs.h"
#include "storage/block.h"
#include "storage/item.h"
#include "storage/off.h"
#include "utils/elog.h"

/* GUC variable */
extern PGDLLIMPORT bool ignore_checksum_failure;

/*
 * A postgres disk page is an abstraction layered on top of a postgres
 * disk block (which is simply a unit of i/o, see block.h).
 *
 * specifically, while a disk block can be unformatted, a postgres
 * disk page is always a slotted page of the form:
 *
 * +----------------+---------------------------------+
 * | PageHeaderData | linp1 linp2 linp3 ...           |
 * +-----------+----+---------------------------------+
 * | ... linpN |									  |
 * +-----------+--------------------------------------+
 * |		   ^ pd_lower							  |
 * |												  |
 * |			 v pd_upper							  |
 * +-------------+------------------------------------+
 * |			 | tupleN ...                         |
 * +-------------+------------------+-----------------+
 * |	   ... tuple3 tuple2 tuple1 | "special space" |
 * +--------------------------------+-----------------+
 *									^ pd_special
 *
 * a page is full when nothing can be added between pd_lower and
 * pd_upper.
 *
 * all blocks written out by an access method must be disk pages.
 *
 * EXCEPTIONS:
 *
 * obviously, a page is not formatted before it is initialized by
 * a call to PageInit.
 *
 * NOTES:
 *
 * linp1..N form an ItemId (line pointer) array.  ItemPointers point
 * to a physical block number and a logical offset (line pointer
 * number) within that block/page.  Note that OffsetNumbers
 * conventionally start at 1, not 0.
 *
 * tuple1..N are added "backwards" on the page.  Since an ItemPointer
 * offset is used to access an ItemId entry rather than an actual
 * byte-offset position, tuples can be physically shuffled on a page
 * whenever the need arises.  This indirection also keeps crash recovery
 * relatively simple, because the low-level details of page space
 * management can be controlled by standard buffer page code during
 * logging, and during recovery.
 *
 * AM-generic per-page information is kept in PageHeaderData.
 *
 * AM-specific per-page data (if any) is kept in the area marked "special
 * space"; each AM has an "opaque" structure defined somewhere that is
 * stored as the page trailer.  An access method should always
 * initialize its pages with PageInit and then set its own opaque
 * fields.
 */

typedef char PageData;
typedef PageData *Page;


/*
 * location (byte offset) within a page.
 *
 * note that this is actually limited to 2^15 because we have limited
 * ItemIdData.lp_off and ItemIdData.lp_len to 15 bits (see itemid.h).
 */
typedef uint16 LocationIndex;


/*
 * For historical reasons, the 64-bit LSN value is stored as two 32-bit
 * values.
 */
typedef struct
{
	uint32		xlogid;			/* high bits */
	uint32		xrecoff;		/* low bits */
} PageXLogRecPtr;

static inline XLogRecPtr
PageXLogRecPtrGet(PageXLogRecPtr val)
{
	return (uint64) val.xlogid << 32 | val.xrecoff;
}

#define PageXLogRecPtrSet(ptr, lsn) \
	((ptr).xlogid = (uint32) ((lsn) >> 32), (ptr).xrecoff = (uint32) (lsn))

/*
 * disk page organization
 *
 * space management information generic to any page
 *
 *		pd_lsn		- identifies xlog record for last change to this page.
 *		pd_checksum - page checksum, if set.
 *		pd_flags	- flag bits.
 *		pd_lower	- offset to start of free space.
 *		pd_upper	- offset to end of free space.
 *		pd_special	- offset to start of special space.
 *		pd_pagesize_version - size in bytes and page layout version number.
 *		pd_prune_xid - oldest XID among potentially prunable tuples on page.
 *
 * The LSN is used by the buffer manager to enforce the basic rule of WAL:
 * "thou shalt write xlog before data".  A dirty buffer cannot be dumped
 * to disk until xlog has been flushed at least as far as the page's LSN.
 *
 * pd_checksum stores the page checksum, if it has been set for this page;
 * zero is a valid value for a checksum. If a checksum is not in use then
 * we leave the field unset. This will typically mean the field is zero
 * though non-zero values may also be present if databases have been
 * pg_upgraded from releases prior to 9.3, when the same byte offset was
 * used to store the current timelineid when the page was last updated.
 * Note that there is no indication on a page as to whether the checksum
 * is valid or not, a deliberate design choice which avoids the problem
 * of relying on the page contents to decide whether to verify it. Hence
 * there are no flag bits relating to checksums.
 *
 * pd_prune_xid is a hint field that helps determine whether pruning will be
 * useful.  It is currently unused in index pages.
 *
 * The page version number and page size are packed together into a single
 * uint16 field.  This is for historical reasons: before PostgreSQL 7.3,
 * there was no concept of a page version number, and doing it this way
 * lets us pretend that pre-7.3 databases have page version number zero.
 * We constrain page sizes to be multiples of 256, leaving the low eight
 * bits available for a version number.
 *
 * Minimum possible page size is perhaps 64B to fit page header, opaque space
 * and a minimal tuple; of course, in reality you want it much bigger, so
 * the constraint on pagesize mod 256 is not an important restriction.
 * On the high end, we can only support pages up to 32KB because lp_off/lp_len
 * are 15 bits.
 */

typedef struct PageHeaderData
{
	/* XXX LSN is member of *any* block, not only page-organized ones */
	PageXLogRecPtr pd_lsn;		/* LSN: next byte after last byte of xlog
								 * record for last change to this page */
	uint16		pd_checksum;	/* checksum */
	uint16		pd_flags;		/* flag bits, see below */
	LocationIndex pd_lower;		/* offset to start of free space */
	LocationIndex pd_upper;		/* offset to end of free space */
	LocationIndex pd_special;	/* offset to start of special space */
	uint16		pd_pagesize_version;
	ShortTransactionId pd_prune_xid;	/* oldest prunable XID, or zero if
										 * none */
	ItemIdData	pd_linp[FLEXIBLE_ARRAY_MEMBER]; /* line pointer array */
} PageHeaderData;

typedef PageHeaderData *PageHeader;


/*
 * HeapPageSpecialData -- data that stored at the end of each heap page.
 *
 *		pd_xid_base - base value for transaction IDs on page
 *		pd_multi_base - base value for multixact IDs on page
 *
 * pd_xid_base and pd_multi_base are base values for calculation of transaction
 * identifiers from t_xmin and t_xmax in each heap tuple header on the page.
 */
typedef struct HeapPageSpecialData
{
	TransactionId pd_xid_base;		/* base value for transaction IDs on page */
	TransactionId pd_multi_base;	/* base value for multixact IDs on page */
}			HeapPageSpecialData;

typedef HeapPageSpecialData *HeapPageSpecial;
#define HeapPageSpecialOffset (BLCKSZ - MAXALIGN(sizeof(HeapPageSpecialData)))

typedef struct ToastPageSpecialData
{
	TransactionId pd_xid_base;		/* base value for transaction IDs on page */
}			ToastPageSpecialData;

typedef ToastPageSpecialData *ToastPageSpecial;
#define ToastPageSpecialOffset (BLCKSZ - MAXALIGN(sizeof(ToastPageSpecialData)))

extern PGDLLIMPORT HeapPageSpecial heapDoubleXmaxSpecial;
extern PGDLLIMPORT ToastPageSpecial toastDoubleXmaxSpecial;

/*
 * pd_flags contains the following flag bits.  Undefined bits are initialized
 * to zero and may be used in the future.
 *
 * PD_HAS_FREE_LINES is set if there are any LP_UNUSED line pointers before
 * pd_lower.  This should be considered a hint rather than the truth, since
 * changes to it are not WAL-logged.
 *
 * PD_PAGE_FULL is set if an UPDATE doesn't find enough free space in the
 * page for its new tuple version; this suggests that a prune is needed.
 * Again, this is just a hint.
 */
#define PD_HAS_FREE_LINES	0x0001	/* are there any unused line pointers? */
#define PD_PAGE_FULL		0x0002	/* not enough free space for new tuple? */
#define PD_ALL_VISIBLE		0x0004	/* all tuples on page are visible to
									 * everyone */

#define PD_VALID_FLAG_BITS	0x0007	/* OR of all valid pd_flags bits */

/*
 * Page layout version number 0 is for pre-7.3 Postgres releases.
 * Releases 7.3 and 7.4 use 1, denoting a new HeapTupleHeader layout.
 * Release 8.0 uses 2; it changed the HeapTupleHeader layout again.
 * Release 8.1 uses 3; it redefined HeapTupleHeader infomask bits.
 * Release 8.3 uses 4; it changed the HeapTupleHeader layout again, and
 *		added the pd_flags field (by stealing some bits from pd_tli),
 *		as well as adding the pd_prune_xid field (which enlarges the header).
 *
 * As of Release 9.3, the checksum version must also be considered when
 * handling pages.
 */
#define PG_PAGE_LAYOUT_VERSION		5
#define PG_DATA_CHECKSUM_VERSION	1

/* ----------------------------------------------------------------
 *						page support functions
 * ----------------------------------------------------------------
 */

/*
 * line pointer(s) do not count as part of header
 */
#define SizeOfPageHeaderData (offsetof(PageHeaderData, pd_linp))

/*
 * PageIsEmpty
 *		returns true iff no itemid has been allocated on the page
 */
static inline bool
PageIsEmpty(const PageData *page)
{
	return ((const PageHeaderData *) page)->pd_lower <= SizeOfPageHeaderData;
}

/*
 * PageIsNew
 *		returns true iff page has not been initialized (by PageInit)
 */
static inline bool
PageIsNew(const PageData *page)
{
	return ((const PageHeaderData *) page)->pd_upper == 0;
}

/*
 * PageGetItemId
 *		Returns an item identifier of a page.
 */
static inline ItemId
PageGetItemId(Page page, OffsetNumber offsetNumber)
{
	return &((PageHeader) page)->pd_linp[offsetNumber - 1];
}

/*
 * PageGetContents
 *		To be used in cases where the page does not contain line pointers.
 *
 * Note: prior to 8.3 this was not guaranteed to yield a MAXALIGN'd result.
 * Now it is.  Beware of old code that might think the offset to the contents
 * is just SizeOfPageHeaderData rather than MAXALIGN(SizeOfPageHeaderData).
 */
static inline char *
PageGetContents(Page page)
{
	return (char *) page + MAXALIGN(SizeOfPageHeaderData);
}

/* ----------------
 *		functions to access page size info
 * ----------------
 */

/*
 * PageGetPageSize
 *		Returns the page size of a page.
 *
 * this can only be called on a formatted page (unlike
 * BufferGetPageSize, which can be called on an unformatted page).
 * however, it can be called on a page that is not stored in a buffer.
 */
static inline Size
PageGetPageSize(const PageData *page)
{
	return (Size) (((const PageHeaderData *) page)->pd_pagesize_version & (uint16) 0xFF00);
}

/*
 * PageGetPageLayoutVersion
 *		Returns the page layout version of a page.
 */
static inline uint8
PageGetPageLayoutVersion(const PageData *page)
{
	return (((const PageHeaderData *) page)->pd_pagesize_version & 0x00FF);
}

/*
 * PageSetPageSizeAndVersion
 *		Sets the page size and page layout version number of a page.
 *
 * We could support setting these two values separately, but there's
 * no real need for it at the moment.
 */
static inline void
PageSetPageSizeAndVersion(Page page, Size size, uint8 version)
{
	Assert((size & 0xFF00) == size);
	Assert((version & 0x00FF) == version);

	((PageHeader) page)->pd_pagesize_version = size | version;
}

/* ----------------
 *		page special data functions
 * ----------------
 */
/*
 * PageGetSpecialSize
 *		Returns size of special space on a page.
 */
static inline uint16
PageGetSpecialSize(const PageData *page)
{
	return (PageGetPageSize(page) - ((const PageHeaderData *) page)->pd_special);
}

/*
 * Using assertions, validate that the page special pointer is OK.
 *
 * This is intended to catch use of the pointer before page initialization.
 */
static inline void
PageValidateSpecialPointer(const PageData *page)
{
	Assert(page);
	Assert(((const PageHeaderData *) page)->pd_special <= BLCKSZ);
	Assert(((const PageHeaderData *) page)->pd_special >= SizeOfPageHeaderData);
}

/*
 * PageGetSpecialPointer
 *		Returns pointer to special space on a page.
 */
#define PageGetSpecialPointer(page) \
( \
	PageValidateSpecialPointer(page), \
	((page) + ((PageHeader) (page))->pd_special) \
)

/*
 * PageGetItem
 *		Retrieves an item on the given page.
 *
 * Note:
 *		This does not change the status of any of the resources passed.
 *		The semantics may change in the future.
 */
static inline Item
PageGetItem(const PageData *page, const ItemIdData *itemId)
{
	Assert(page);
	Assert(ItemIdHasStorage(itemId));

	return (Item) (((const char *) page) + ItemIdGetOffset(itemId));
}

/*
 * PageGetMaxOffsetNumber
 *		Returns the maximum offset number used by the given page.
 *		Since offset numbers are 1-based, this is also the number
 *		of items on the page.
 *
 *		NOTE: if the page is not initialized (pd_lower == 0), we must
 *		return zero to ensure sane behavior.
 */
static inline OffsetNumber
PageGetMaxOffsetNumber(const PageData *page)
{
	const PageHeaderData *pageheader = (const PageHeaderData *) page;

	if (pageheader->pd_lower <= SizeOfPageHeaderData)
		return 0;
	else
		return (pageheader->pd_lower - SizeOfPageHeaderData) / sizeof(ItemIdData);
}

/*
 * Additional functions for access to page headers.
 */
static inline XLogRecPtr
PageGetLSN(const PageData *page)
{
	return PageXLogRecPtrGet(((const PageHeaderData *) page)->pd_lsn);
}
static inline void
PageSetLSN(Page page, XLogRecPtr lsn)
{
	PageXLogRecPtrSet(((PageHeader) page)->pd_lsn, lsn);
}

static inline bool
PageHasFreeLinePointers(const PageData *page)
{
	return ((const PageHeaderData *) page)->pd_flags & PD_HAS_FREE_LINES;
}
static inline void
PageSetHasFreeLinePointers(Page page)
{
	((PageHeader) page)->pd_flags |= PD_HAS_FREE_LINES;
}
static inline void
PageClearHasFreeLinePointers(Page page)
{
	((PageHeader) page)->pd_flags &= ~PD_HAS_FREE_LINES;
}

static inline bool
PageIsFull(const PageData *page)
{
	return ((const PageHeaderData *) page)->pd_flags & PD_PAGE_FULL;
}
static inline void
PageSetFull(Page page)
{
	((PageHeader) page)->pd_flags |= PD_PAGE_FULL;
}
static inline void
PageClearFull(Page page)
{
	((PageHeader) page)->pd_flags &= ~PD_PAGE_FULL;
}

static inline bool
PageIsAllVisible(const PageData *page)
{
	return ((const PageHeaderData *) page)->pd_flags & PD_ALL_VISIBLE;
}
static inline void
PageSetAllVisible(Page page)
{
	((PageHeader) page)->pd_flags |= PD_ALL_VISIBLE;
}
static inline void
PageClearAllVisible(Page page)
{
	((PageHeader) page)->pd_flags &= ~PD_ALL_VISIBLE;
}

/*
 * Check if page is in "double xmax" format.
 */
static inline bool
HeapPageIsDoubleXmax(const PageData *page)
{
	return ((const PageHeaderData *) (page))->pd_special == BLCKSZ;
}

/*
 * Get page HeapPageSpecial for general use.
 */
static inline HeapPageSpecial
HeapPageGetSpecial(Page page)
{
	PageHeader pageheader = (PageHeader) page;

	if (HeapPageIsDoubleXmax(page))
		return heapDoubleXmaxSpecial;

	Assert(pageheader->pd_special == HeapPageSpecialOffset);

	return (HeapPageSpecial) ((char *) page + pageheader->pd_special);
}

/*
 * Get page ToastPageSpecial for general use.
 */
static inline ToastPageSpecial
ToastPageGetSpecial(Page page)
{
	PageHeader pageheader = (PageHeader) page;

	if (HeapPageIsDoubleXmax(page))
		return toastDoubleXmaxSpecial;

	Assert(pageheader->pd_special == ToastPageSpecialOffset);

	return (ToastPageSpecial) ((char *) page + pageheader->pd_special);
}

/*
 * Get pd_xid_base from page based on size of the special area.
 */
static inline TransactionId
PageGetSpecialXidBase(Page page)
{
	int		pd_special = ((PageHeader) page)->pd_special;

	if (pd_special == HeapPageSpecialOffset)
		return HeapPageGetSpecial(page)->pd_xid_base;

	if (pd_special == ToastPageSpecialOffset)
		return ToastPageGetSpecial(page)->pd_xid_base;

	elog(PANIC, "invalid page pd_special %d", pd_special);

	return InvalidTransactionId;	/* keep compiler quiet */
}

/*
 * Convert short xid from/to full xid.  Assertion should fail if we full xid
 * doesn't fit to xid base.
 */
static inline TransactionId
ShortTransactionIdToNormal(TransactionId base, ShortTransactionId xid)
{
	if (!TransactionIdIsNormal(xid))
		return (TransactionId) xid;

#ifndef FRONTEND
	/* xid + base should not overflow TransactionId */
	Assert(xid + base >= base);
#endif

	return (TransactionId) (xid + base);
}

static inline ShortTransactionId
NormalTransactionIdToShort(TransactionId base, TransactionId xid, bool multi)
{
	if (!TransactionIdIsNormal(xid))
		return (ShortTransactionId) xid;

#ifdef FRONTEND
	Assert(xid >= base + (!multi ? FirstNormalTransactionId :
								   FirstMultiXactId));
	Assert(xid <= base + MaxShortTransactionId);
#else
	if (xid < base + (!multi ? FirstNormalTransactionId : FirstMultiXactId) ||
		xid > base + MaxShortTransactionId)
		ereport(PANIC,
				(errcode(ERRCODE_INTERNAL_ERROR),
						errbacktrace(),
						errmsg("xid %" PRIu64 " does not fit into valid range for base %" PRIu64,
							   xid, base)));
#endif

	return (ShortTransactionId) (xid - base);
}

/*
 * Set pd_prune_xid.
 */
static inline void
HeapPageSetPruneXid(Page page, TransactionId xid)
{
	TransactionId	base;
	PageHeader		pagehdr = (PageHeader) page;

	if (HeapPageIsDoubleXmax(page))
		return;

	if (!TransactionIdIsNormal(xid))
	{
		pagehdr->pd_prune_xid = xid;
		return;
	}

	base = PageGetSpecialXidBase(page);
	pagehdr->pd_prune_xid = NormalTransactionIdToShort(base, xid, false);

	Assert(pagehdr->pd_prune_xid <= MaxShortTransactionId);
}

/*
 * Get pd_prune_xid from locked page.
 */
static inline TransactionId
HeapPageGetPruneXid(Page page)
{
	TransactionId	base;

	if (HeapPageIsDoubleXmax(page))
		return ((PageHeader) (page))->pd_prune_xid;

	base = PageGetSpecialXidBase(page);

	return ShortTransactionIdToNormal(base,
									  ((PageHeader) (page))->pd_prune_xid);
}

static inline void
PageSetPrunable(Page page, TransactionId xid)
{
	TransactionId	prune_xid;

	Assert(TransactionIdIsNormal(xid));

	if (HeapPageIsDoubleXmax(page))
		return;

	prune_xid = HeapPageGetPruneXid(page);
	if ((!TransactionIdIsValid(prune_xid) ||
		TransactionIdPrecedes(xid, prune_xid)))
	{
		HeapPageSetPruneXid(page, xid);
	}
}

/*
 * Get pd_prune_xid from non-locked page.  May return invalid value, but doesn't
 * causes assert failures.
 *
 * Can be used for non-consistent reads of non-locked pages.
 */
static inline TransactionId
HeapPageGetPruneXidNoAssert(Page page, bool is_toast)
{
	TransactionId	base;
	PageHeader		pageheader = (PageHeader) page;
	LocationIndex	pd_special = pageheader->pd_special;
	char		   *special = (char *) page + pd_special;

	if (HeapPageIsDoubleXmax(page))
		return pageheader->pd_prune_xid;

	if (pd_special != HeapPageSpecialOffset &&
		pd_special != ToastPageSpecialOffset)
	{
		/* The page doesn't seem to be consistent. */
		return InvalidTransactionId;
	}

	base = is_toast ? ((ToastPageSpecial) special)->pd_xid_base :
					  ((HeapPageSpecial) special)->pd_xid_base;

	return ShortTransactionIdToNormal(base, pageheader->pd_prune_xid);
}

/* ----------------------------------------------------------------
 *		extern declarations
 * ----------------------------------------------------------------
 */

/* flags for PageAddItemExtended() */
#define PAI_OVERWRITE			(1 << 0)
#define PAI_IS_HEAP				(1 << 1)

/* flags for PageIsVerified() */
#define PIV_LOG_WARNING			(1 << 0)
#define PIV_LOG_LOG				(1 << 1)
#define PIV_IGNORE_CHECKSUM_FAILURE (1 << 2)

#define PageAddItem(page, item, size, offsetNumber, overwrite, is_heap) \
	PageAddItemExtended(page, item, size, offsetNumber, \
						((overwrite) ? PAI_OVERWRITE : 0) | \
						((is_heap) ? PAI_IS_HEAP : 0))

/*
 * Check that BLCKSZ is a multiple of sizeof(size_t).  In PageIsVerified(), it
 * is much faster to check if a page is full of zeroes using the native word
 * size.  Note that this assertion is kept within a header to make sure that
 * StaticAssertDecl() works across various combinations of platforms and
 * compilers.
 */
StaticAssertDecl(BLCKSZ == ((BLCKSZ / sizeof(size_t)) * sizeof(size_t)),
				 "BLCKSZ has to be a multiple of sizeof(size_t)");

/*
 * Tuple defrag support for PageRepairFragmentation and PageIndexMultiDelete
 */
typedef struct ItemIdCompactData
{
	uint16		offsetindex;	/* linp array index */
	int16		itemoff;		/* page offset of item data */
	uint16		alignedlen;		/* MAXALIGN(item data len) */
} ItemIdCompactData;

typedef ItemIdCompactData *ItemIdCompact;

extern int	itemoffcompare(const void *item1, const void *item2);

extern void PageInit(Page page, Size pageSize, Size specialSize);
extern bool PageIsVerified(PageData *page, BlockNumber blkno, int flags,
						   bool *checksum_failure_p);
extern OffsetNumber PageAddItemExtended(Page page, Item item, Size size,
										OffsetNumber offsetNumber, int flags);
extern Page PageGetTempPage(const PageData *page);
extern Page PageGetTempPageCopy(const PageData *page);
extern Page PageGetTempPageCopySpecial(const PageData *page);
extern void PageRestoreTempPage(Page tempPage, Page oldPage);
extern void PageRepairFragmentation(Page page, bool is_toast);
extern void PageTruncateLinePointerArray(Page page);
extern Size PageGetFreeSpace(const PageData *page);
extern Size PageGetFreeSpaceForMultipleTuples(const PageData *page, int ntups);
extern Size PageGetExactFreeSpace(const PageData *page);
extern Size PageGetHeapFreeSpace(const PageData *page);
extern void PageIndexTupleDelete(Page page, OffsetNumber offnum);
extern void PageIndexMultiDelete(Page page, OffsetNumber *itemnos, int nitems);
extern void PageIndexTupleDeleteNoCompact(Page page, OffsetNumber offnum);
extern bool PageIndexTupleOverwrite(Page page, OffsetNumber offnum,
									Item newtup, Size newsize);
extern char *PageSetChecksumCopy(Page page, BlockNumber blkno);
extern void PageSetChecksumInplace(Page page, BlockNumber blkno);

#endif							/* BUFPAGE_H */

/*-------------------------------------------------------------------------
 *
 * htup_details.h
 *	  POSTGRES heap tuple header definitions.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/htup_details.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HTUP_DETAILS_H
#define HTUP_DETAILS_H

#include "access/htup.h"
#include "access/transam.h"
#include "access/tupdesc.h"
#include "access/tupmacs.h"
#include "storage/bufpage.h"
#include "storage/bufmgr.h"
#include "varatt.h"

/*
 * MaxTupleAttributeNumber limits the number of (user) columns in a tuple.
 * The key limit on this value is that the size of the fixed overhead for
 * a tuple, plus the size of the null-values bitmap (at 1 bit per column),
 * plus MAXALIGN alignment, must fit into t_hoff which is uint8.  On most
 * machines the upper limit without making t_hoff wider would be a little
 * over 1700.  We use round numbers here and for MaxHeapAttributeNumber
 * so that alterations in HeapTupleHeaderData layout won't change the
 * supported max number of columns.
 */
#define MaxTupleAttributeNumber 1664	/* 8 * 208 */

/*
 * MaxHeapAttributeNumber limits the number of (user) columns in a table.
 * This should be somewhat less than MaxTupleAttributeNumber.  It must be
 * at least one less, else we will fail to do UPDATEs on a maximal-width
 * table (because UPDATE has to form working tuples that include CTID).
 * In practice we want some additional daylight so that we can gracefully
 * support operations that add hidden "resjunk" columns, for example
 * SELECT * FROM wide_table ORDER BY foo, bar, baz.
 * In any case, depending on column data types you will likely be running
 * into the disk-block-based limit on overall tuple size if you have more
 * than a thousand or so columns.  TOAST won't help.
 */
#define MaxHeapAttributeNumber	1600	/* 8 * 200 */

/*
 * Heap tuple header.  To avoid wasting space, the fields should be
 * laid out in such a way as to avoid structure padding.
 *
 * Datums of composite types (row types) share the same general structure
 * as on-disk tuples, so that the same routines can be used to build and
 * examine them.  However the requirements are slightly different: a Datum
 * does not need any transaction visibility information, and it does need
 * a length word and some embedded type information.  We can achieve this
 * by overlaying the xmin/cmin/xmax/cmax/xvac fields of a heap tuple
 * with the fields needed in the Datum case.  Typically, all tuples built
 * in-memory will be initialized with the Datum fields; but when a tuple is
 * about to be inserted in a table, the transaction fields will be filled,
 * overwriting the datum fields.
 *
 * The overall structure of a heap tuple looks like:
 *			fixed fields (HeapTupleHeaderData struct)
 *			nulls bitmap (if HEAP_HASNULL is set in t_infomask)
 *			alignment padding (as needed to make user data MAXALIGN'd)
 *			object ID (if HEAP_HASOID_OLD is set in t_infomask, not created
 *          anymore)
 *			user data fields
 *
 * We store five "virtual" fields Xmin, Cmin, Xmax, Cmax, and Xvac in three
 * physical fields.  Xmin and Xmax are always really stored, but Cmin, Cmax
 * and Xvac share a field.  This works because we know that Cmin and Cmax
 * are only interesting for the lifetime of the inserting and deleting
 * transaction respectively.  If a tuple is inserted and deleted in the same
 * transaction, we store a "combo" command id that can be mapped to the real
 * cmin and cmax, but only by use of local state within the originating
 * backend.  See combocid.c for more details.  Meanwhile, Xvac is only set by
 * old-style VACUUM FULL, which does not have any command sub-structure and so
 * does not need either Cmin or Cmax.  (This requires that old-style VACUUM
 * FULL never try to move a tuple whose Cmin or Cmax is still interesting,
 * ie, an insert-in-progress or delete-in-progress tuple.)
 *
 * A word about t_ctid: whenever a new tuple is stored on disk, its t_ctid
 * is initialized with its own TID (location).  If the tuple is ever updated,
 * its t_ctid is changed to point to the replacement version of the tuple.  Or
 * if the tuple is moved from one partition to another, due to an update of
 * the partition key, t_ctid is set to a special value to indicate that
 * (see ItemPointerSetMovedPartitions).  Thus, a tuple is the latest version
 * of its row iff XMAX is invalid or
 * t_ctid points to itself (in which case, if XMAX is valid, the tuple is
 * either locked or deleted).  One can follow the chain of t_ctid links
 * to find the newest version of the row, unless it was moved to a different
 * partition.  Beware however that VACUUM might
 * erase the pointed-to (newer) tuple before erasing the pointing (older)
 * tuple.  Hence, when following a t_ctid link, it is necessary to check
 * to see if the referenced slot is empty or contains an unrelated tuple.
 * Check that the referenced tuple has XMIN equal to the referencing tuple's
 * XMAX to verify that it is actually the descendant version and not an
 * unrelated tuple stored into a slot recently freed by VACUUM.  If either
 * check fails, one may assume that there is no live descendant version.
 *
 * t_ctid is sometimes used to store a speculative insertion token, instead
 * of a real TID.  A speculative token is set on a tuple that's being
 * inserted, until the inserter is sure that it wants to go ahead with the
 * insertion.  Hence a token should only be seen on a tuple with an XMAX
 * that's still in-progress, or invalid/aborted.  The token is replaced with
 * the tuple's real TID when the insertion is confirmed.  One should never
 * see a speculative insertion token while following a chain of t_ctid links,
 * because they are not used on updates, only insertions.
 *
 * Following the fixed header fields, the nulls bitmap is stored (beginning
 * at t_bits).  The bitmap is *not* stored if t_infomask shows that there
 * are no nulls in the tuple.  If an OID field is present (as indicated by
 * t_infomask), then it is stored just before the user data, which begins at
 * the offset shown by t_hoff.  Note that t_hoff must be a multiple of
 * MAXALIGN.
 */

typedef struct HeapTupleFields
{
	ShortTransactionId t_xmin;	/* inserting xact ID */
	ShortTransactionId t_xmax;	/* deleting or locking xact ID */

	union
	{
		CommandId	t_cid;		/* inserting or deleting command ID, or both */
		ShortTransactionId t_xvac;	/* old-style VACUUM FULL xact ID */
	}			t_field3;
} HeapTupleFields;

typedef struct DatumTupleFields
{
	int32		datum_len_;		/* varlena header (do not touch directly!) */

	int32		datum_typmod;	/* -1, or identifier of a record type */

	Oid			datum_typeid;	/* composite type OID, or RECORDOID */

	/*
	 * datum_typeid cannot be a domain over composite, only plain composite,
	 * even if the datum is meant as a value of a domain-over-composite type.
	 * This is in line with the general principle that CoerceToDomain does not
	 * change the physical representation of the base type value.
	 *
	 * Note: field ordering is chosen with thought that Oid might someday
	 * widen to 64 bits.
	 */
} DatumTupleFields;

struct HeapTupleHeaderData
{
	union
	{
		HeapTupleFields t_heap;
		DatumTupleFields t_datum;
	}			t_choice;

	ItemPointerData t_ctid;		/* current TID of this or newer tuple (or a
								 * speculative insertion token) */

	/* Fields below here must match MinimalTupleData! */

#define FIELDNO_HEAPTUPLEHEADERDATA_INFOMASK2 2
	uint16		t_infomask2;	/* number of attributes + various flags */

#define FIELDNO_HEAPTUPLEHEADERDATA_INFOMASK 3
	uint16		t_infomask;		/* various flag bits, see below */

#define FIELDNO_HEAPTUPLEHEADERDATA_HOFF 4
	uint8		t_hoff;			/* sizeof header incl. bitmap, padding */

	/* ^ - 23 bytes - ^ */

#define FIELDNO_HEAPTUPLEHEADERDATA_BITS 5
	bits8		t_bits[FLEXIBLE_ARRAY_MEMBER];	/* bitmap of NULLs */

	/* MORE DATA FOLLOWS AT END OF STRUCT */
};

/* typedef appears in htup.h */

#define SizeofHeapTupleHeader offsetof(HeapTupleHeaderData, t_bits)

/*
 * information stored in t_infomask:
 */
#define HEAP_HASNULL			0x0001	/* has null attribute(s) */
#define HEAP_HASVARWIDTH		0x0002	/* has variable-width attribute(s) */
#define HEAP_HASEXTERNAL		0x0004	/* has external stored attribute(s) */
#define HEAP_HASOID_OLD			0x0008	/* has an object-id field */
#define HEAP_XMAX_KEYSHR_LOCK	0x0010	/* xmax is a key-shared locker */
#define HEAP_COMBOCID			0x0020	/* t_cid is a combo CID */
#define HEAP_XMAX_EXCL_LOCK		0x0040	/* xmax is exclusive locker */
#define HEAP_XMAX_LOCK_ONLY		0x0080	/* xmax, if valid, is only a locker */

 /* xmax is a shared locker */
#define HEAP_XMAX_SHR_LOCK	(HEAP_XMAX_EXCL_LOCK | HEAP_XMAX_KEYSHR_LOCK)

#define HEAP_LOCK_MASK	(HEAP_XMAX_SHR_LOCK | HEAP_XMAX_EXCL_LOCK | \
						 HEAP_XMAX_KEYSHR_LOCK)
#define HEAP_XMIN_COMMITTED		0x0100	/* t_xmin committed */
#define HEAP_XMIN_INVALID		0x0200	/* t_xmin invalid/aborted */
#define HEAP_XMIN_FROZEN		(HEAP_XMIN_COMMITTED|HEAP_XMIN_INVALID)
#define HEAP_XMAX_COMMITTED		0x0400	/* t_xmax committed */
#define HEAP_XMAX_INVALID		0x0800	/* t_xmax invalid/aborted */
#define HEAP_XMAX_IS_MULTI		0x1000	/* t_xmax is a MultiXactId */
#define HEAP_UPDATED			0x2000	/* this is UPDATEd version of row */
#define HEAP_MOVED_OFF			0x4000	/* moved to another place by pre-9.0
										 * VACUUM FULL; kept for binary
										 * upgrade support */
#define HEAP_MOVED_IN			0x8000	/* moved from another place by pre-9.0
										 * VACUUM FULL; kept for binary
										 * upgrade support */
#define HEAP_MOVED (HEAP_MOVED_OFF | HEAP_MOVED_IN)

#define HEAP_XACT_MASK			0xFFF0	/* visibility-related bits */

/*
 * A tuple is only locked (i.e. not updated by its Xmax) if the
 * HEAP_XMAX_LOCK_ONLY bit is set; or, for pg_upgrade's sake, if the Xmax is
 * not a multi and the EXCL_LOCK bit is set.
 *
 * See also HeapTupleIsOnlyLocked, which also checks for a possible
 * aborted updater transaction.
 */
static inline bool
HEAP_XMAX_IS_LOCKED_ONLY(uint16 infomask)
{
	return (infomask & HEAP_XMAX_LOCK_ONLY) ||
		(infomask & (HEAP_XMAX_IS_MULTI | HEAP_LOCK_MASK)) == HEAP_XMAX_EXCL_LOCK;
}

/*
 * A tuple that has HEAP_XMAX_IS_MULTI and HEAP_XMAX_LOCK_ONLY but neither of
 * HEAP_XMAX_EXCL_LOCK and HEAP_XMAX_KEYSHR_LOCK must come from a tuple that was
 * share-locked in 9.2 or earlier and then pg_upgrade'd.
 *
 * In 9.2 and prior, HEAP_XMAX_IS_MULTI was only set when there were multiple
 * FOR SHARE lockers of that tuple.  That set HEAP_XMAX_LOCK_ONLY (with a
 * different name back then) but neither of HEAP_XMAX_EXCL_LOCK and
 * HEAP_XMAX_KEYSHR_LOCK.  That combination is no longer possible in 9.3 and
 * up, so if we see that combination we know for certain that the tuple was
 * locked in an earlier release; since all such lockers are gone (they cannot
 * survive through pg_upgrade), such tuples can safely be considered not
 * locked.
 *
 * We must not resolve such multixacts locally, because the result would be
 * bogus, regardless of where they stand with respect to the current valid
 * multixact range.
 */
static inline bool
HEAP_LOCKED_UPGRADED(uint16 infomask)
{
	return
		(infomask & HEAP_XMAX_IS_MULTI) != 0 &&
		(infomask & HEAP_XMAX_LOCK_ONLY) != 0 &&
		(infomask & (HEAP_XMAX_EXCL_LOCK | HEAP_XMAX_KEYSHR_LOCK)) == 0;
}

/*
 * Use these to test whether a particular lock is applied to a tuple
 */
static inline bool
HEAP_XMAX_IS_SHR_LOCKED(int16 infomask)
{
	return (infomask & HEAP_LOCK_MASK) == HEAP_XMAX_SHR_LOCK;
}

static inline bool
HEAP_XMAX_IS_EXCL_LOCKED(int16 infomask)
{
	return (infomask & HEAP_LOCK_MASK) == HEAP_XMAX_EXCL_LOCK;
}

static inline bool
HEAP_XMAX_IS_KEYSHR_LOCKED(int16 infomask)
{
	return (infomask & HEAP_LOCK_MASK) == HEAP_XMAX_KEYSHR_LOCK;
}

/* turn these all off when Xmax is to change */
#define HEAP_XMAX_BITS (HEAP_XMAX_COMMITTED | HEAP_XMAX_INVALID | \
						HEAP_XMAX_IS_MULTI | HEAP_LOCK_MASK | HEAP_XMAX_LOCK_ONLY)

/*
 * information stored in t_infomask2:
 */
#define HEAP_NATTS_MASK			0x07FF	/* 11 bits for number of attributes */
/* bits 0x1800 are available */
#define HEAP_KEYS_UPDATED		0x2000	/* tuple was updated and key cols
										 * modified, or tuple deleted */
#define HEAP_HOT_UPDATED		0x4000	/* tuple was HOT-updated */
#define HEAP_ONLY_TUPLE			0x8000	/* this is heap-only tuple */

#define HEAP2_XACT_MASK			0xE000	/* visibility-related bits */

/*
 * HEAP_TUPLE_HAS_MATCH is a temporary flag used during hash joins.  It is
 * only used in tuples that are in the hash table, and those don't need
 * any visibility information, so we can overlay it on a visibility flag
 * instead of using up a dedicated bit.
 */
#define HEAP_TUPLE_HAS_MATCH	HEAP_ONLY_TUPLE /* tuple has a join match */

/*
 * HeapTupleHeader accessor functions
 */

static bool HeapTupleHeaderXminFrozen(const HeapTupleHeaderData *tup);

/*
 * HeapTupleGetRawXmin returns the "raw" xmin field, which is the xid
 * originally used to insert the tuple.  However, the tuple might actually
 * be frozen (via HeapTupleHeaderStoreXminFrozen) in which case the tuple's xmin
 * is visible to every snapshot.  Prior to PostgreSQL 9.4, we actually changed
 * the xmin to FrozenTransactionId, and that value may still be encountered
 * on disk.
 */
static inline TransactionId
HeapTupleGetRawXmin(const HeapTupleData *tup)
{
	return tup->t_xmin;
}

static inline TransactionId
HeapTupleGetXmin(const HeapTupleData *tup)
{
	return HeapTupleHeaderXminFrozen((tup)->t_data) ?
		FrozenTransactionId : HeapTupleGetRawXmin(tup);
}

static inline void
HeapTupleSetXmin(HeapTupleData *tup, TransactionId xid)
{
	tup->t_xmin = xid;
}

/*
 * Functions for accessing "double xmax".  On pg_upgraded instances, it might
 * happend that we can't fit new special area to the page.  But we still
 * might neep to write xmax of tuples for updates and deletes.  The trick is
 * that we actually don't need xmin field.  After pg_upgrade (wich implies
 * restart) no insertions went to this page yet (otherwise special area could
 * fit).  So, if tuple is visible (othewise it would be deleted), then it's
 * visible for everybody.  Thus, t_xmin isn't needed.  Therefore, we can use
 * both t_xmin and t_xmax to store 64-bit xmax.
 *
 * See heap_convert.c for details.
 */
static inline TransactionId
HeapTupleHeaderGetDoubleXmax(const HeapTupleHeaderData *tup)
{
	TransactionId xmax;

	xmax = tup->t_choice.t_heap.t_xmin;
	xmax <<= 32;
	xmax += tup->t_choice.t_heap.t_xmax;

	return xmax;
}

static inline void
HeapTupleHeaderSetDoubleXmax(HeapTupleHeaderData *tup, TransactionId xid)
{
	tup->t_choice.t_heap.t_xmax = xid & 0xFFFFFFFF;
	tup->t_choice.t_heap.t_xmin = (xid >> 32) & 0xFFFFFFFF;
}

static inline void
HeapTupleHeaderStoreXmin(Page page, HeapTupleData *tup)
{
	TransactionId	base,
					xmin;

	Assert(!HeapPageIsDoubleXmax(page));

	base = PageGetSpecialXidBase(page);
	xmin = NormalTransactionIdToShort(base, tup->t_xmin, false);
	tup->t_data->t_choice.t_heap.t_xmin = xmin;
}

static inline void
HeapTupleAndHeaderSetXmin(Page page, HeapTupleData *tup, TransactionId xid)
{
	HeapTupleSetXmin(tup, xid);
	HeapTupleHeaderStoreXmin(page, tup);
}

static inline bool
HeapTupleHeaderXminCommitted(const HeapTupleHeaderData *tup)
{
	return (tup->t_infomask & HEAP_XMIN_COMMITTED) != 0;
}

static inline bool
HeapTupleHeaderXminInvalid(const HeapTupleHeaderData *tup)
{
	return (tup->t_infomask & (HEAP_XMIN_COMMITTED | HEAP_XMIN_INVALID)) ==
		HEAP_XMIN_INVALID;
}

static inline bool
HeapTupleHeaderXminFrozen(const HeapTupleHeaderData *tup)
{
	return (tup->t_infomask & HEAP_XMIN_FROZEN) == HEAP_XMIN_FROZEN;
}

static inline void
HeapTupleHeaderStoreXminFrozen(HeapTupleHeaderData *tup)
{
	Assert(!HeapTupleHeaderXminInvalid(tup));
	tup->t_infomask |= HEAP_XMIN_FROZEN;
}

static inline TransactionId
HeapTupleHeaderGetRawXmax(Page page, const HeapTupleHeaderData *tup)
{
	TransactionId base;

	if (HeapPageIsDoubleXmax(page))
		return HeapTupleHeaderGetDoubleXmax(tup);

	base = (tup->t_infomask & HEAP_XMAX_IS_MULTI) ?
				HeapPageGetSpecial(page)->pd_multi_base :
				HeapPageGetSpecial(page)->pd_xid_base;
	return ShortTransactionIdToNormal(base,
									  tup->t_choice.t_heap.t_xmax);
}


static inline void
HeapTupleHeaderSetXmax(HeapTupleHeaderData *tup, TransactionId xid)
{
	tup->t_choice.t_heap.t_xmax = xid;
}

static inline TransactionId
HeapTupleGetRawXmax(const HeapTupleData *tup)
{
	return tup->t_xmax;
}

static inline void
HeapTupleSetXmax(HeapTupleData *tup, TransactionId xid)
{
	tup->t_xmax = xid;
}

#ifndef FRONTEND
/*
 * HeapTupleHeaderGetRawXmax gets you the raw Xmax field.  To find out the Xid
 * that updated a tuple, you might need to resolve the MultiXactId if certain
 * bits are set.  HeapTupleHeaderGetUpdateXid checks those bits and takes care
 * to resolve the MultiXactId if necessary.  This might involve multixact I/O,
 * so it should only be used if absolutely necessary.
 */

static inline TransactionId
HeapTupleGetUpdateXidAny(const HeapTupleData *htup)
{
	const HeapTupleHeaderData *tup = htup->t_data;

	if (!(tup->t_infomask & HEAP_XMAX_INVALID) &&
		(tup->t_infomask & HEAP_XMAX_IS_MULTI) &&
		!(tup->t_infomask & HEAP_XMAX_LOCK_ONLY))
		return HeapTupleGetUpdateXid(htup);
	else
		return HeapTupleGetRawXmax(htup);
}
#endif							/* FRONTEND */

/*
 * Set xid as xmax for HeapTupleHeader.
 */
static inline void
HeapTupleHeaderStoreXmax(Page page, const HeapTupleData *tup)
{
	TransactionId	base,
					xmax;
	bool			multi;

	if (HeapPageIsDoubleXmax(page))
	{
		HeapTupleHeaderSetDoubleXmax(tup->t_data, tup->t_xmax);
		return;
	}

	multi = (tup->t_data->t_infomask & HEAP_XMAX_IS_MULTI) == HEAP_XMAX_IS_MULTI;
	base = multi ? HeapPageGetSpecial(page)->pd_multi_base :
				   PageGetSpecialXidBase(page);
	xmax = NormalTransactionIdToShort(base, tup->t_xmax, multi);
	tup->t_data->t_choice.t_heap.t_xmax = xmax;
}

/*
 * Set xid as xmax for HeadTuple and HeapTupleHeader.
 */
static inline void
HeapTupleAndHeaderSetXmax(Page page, HeapTupleData *tup, TransactionId xid)
{
	HeapTupleSetXmax(tup, xid);
	HeapTupleHeaderStoreXmax(page, tup);
}

/*
 * HeapTupleHeaderGetRawCommandId will give you what's in the header whether
 * it is useful or not.  Most code should use HeapTupleGetCmin or
 * HeapTupleGetCmax instead, but note that those Assert that you can
 * get a legitimate result, ie you are in the originating transaction!
 */
static inline CommandId
HeapTupleHeaderGetRawCommandId(const HeapTupleHeaderData *tup)
{
	return tup->t_choice.t_heap.t_field3.t_cid;
}

/* SetCmin is reasonably simple since we never need a combo CID */
static inline void
HeapTupleHeaderSetCmin(HeapTupleHeaderData *tup, CommandId cid)
{
	Assert(!(tup->t_infomask & HEAP_MOVED));
	tup->t_choice.t_heap.t_field3.t_cid = cid;
	tup->t_infomask &= ~HEAP_COMBOCID;
}

/* SetCmax must be used after HeapTupleHeaderAdjustCmax; see combocid.c */
static inline void
HeapTupleHeaderSetCmax(HeapTupleHeaderData *tup, CommandId cid, bool iscombo)
{
	Assert(!((tup)->t_infomask & HEAP_MOVED));
	tup->t_choice.t_heap.t_field3.t_cid = cid;
	if (iscombo)
		tup->t_infomask |= HEAP_COMBOCID;
	else
		tup->t_infomask &= ~HEAP_COMBOCID;
}

static inline TransactionId
HeapTupleHeaderGetXvac(const HeapTupleHeaderData *tup)
{
	if (tup->t_infomask & HEAP_MOVED)
		return tup->t_choice.t_heap.t_field3.t_xvac;
	else
		return InvalidTransactionId;
}

static inline void
HeapTupleHeaderSetXvac(HeapTupleHeaderData *tup, TransactionId xid)
{
	Assert(tup->t_infomask & HEAP_MOVED);
	tup->t_choice.t_heap.t_field3.t_xvac = xid;
}

StaticAssertDecl(MaxOffsetNumber < SpecTokenOffsetNumber,
				 "invalid speculative token constant");

static inline bool
HeapTupleHeaderIsSpeculative(const HeapTupleHeaderData *tup)
{
	return ItemPointerGetOffsetNumberNoCheck(&tup->t_ctid) == SpecTokenOffsetNumber;
}

static inline BlockNumber
HeapTupleHeaderGetSpeculativeToken(const HeapTupleHeaderData *tup)
{
	Assert(HeapTupleHeaderIsSpeculative(tup));
	return ItemPointerGetBlockNumber(&tup->t_ctid);
}

static inline void
HeapTupleHeaderSetSpeculativeToken(HeapTupleHeaderData *tup, BlockNumber token)
{
	ItemPointerSet(&tup->t_ctid, token, SpecTokenOffsetNumber);
}

static inline bool
HeapTupleHeaderIndicatesMovedPartitions(const HeapTupleHeaderData *tup)
{
	return ItemPointerIndicatesMovedPartitions(&tup->t_ctid);
}

static inline void
HeapTupleHeaderSetMovedPartitions(HeapTupleHeaderData *tup)
{
	ItemPointerSetMovedPartitions(&tup->t_ctid);
}

static inline uint32
HeapTupleHeaderGetDatumLength(const HeapTupleHeaderData *tup)
{
	return VARSIZE(tup);
}

static inline void
HeapTupleHeaderSetDatumLength(HeapTupleHeaderData *tup, uint32 len)
{
	SET_VARSIZE(tup, len);
}

static inline Oid
HeapTupleHeaderGetTypeId(const HeapTupleHeaderData *tup)
{
	return tup->t_choice.t_datum.datum_typeid;
}

static inline void
HeapTupleHeaderSetTypeId(HeapTupleHeaderData *tup, Oid datum_typeid)
{
	tup->t_choice.t_datum.datum_typeid = datum_typeid;
}

static inline int32
HeapTupleHeaderGetTypMod(const HeapTupleHeaderData *tup)
{
	return tup->t_choice.t_datum.datum_typmod;
}

static inline void
HeapTupleHeaderSetTypMod(HeapTupleHeaderData *tup, int32 typmod)
{
	tup->t_choice.t_datum.datum_typmod = typmod;
}

/*
 * Note that we stop considering a tuple HOT-updated as soon as it is known
 * aborted or the would-be updating transaction is known aborted.  For best
 * efficiency, check tuple visibility before using this function, so that the
 * INVALID bits will be as up to date as possible.
 */
static inline bool
HeapTupleHeaderIsHotUpdated(const HeapTupleHeaderData *tup)
{
	return
		(tup->t_infomask2 & HEAP_HOT_UPDATED) != 0 &&
		(tup->t_infomask & HEAP_XMAX_INVALID) == 0 &&
		!HeapTupleHeaderXminInvalid(tup);
}

static inline void
HeapTupleHeaderSetHotUpdated(HeapTupleHeaderData *tup)
{
	tup->t_infomask2 |= HEAP_HOT_UPDATED;
}

static inline void
HeapTupleHeaderClearHotUpdated(HeapTupleHeaderData *tup)
{
	tup->t_infomask2 &= ~HEAP_HOT_UPDATED;
}

static inline bool
HeapTupleHeaderIsHeapOnly(const HeapTupleHeaderData *tup) \
{
	return (tup->t_infomask2 & HEAP_ONLY_TUPLE) != 0;
}

static inline void
HeapTupleHeaderSetHeapOnly(HeapTupleHeaderData *tup)
{
	tup->t_infomask2 |= HEAP_ONLY_TUPLE;
}

static inline void
HeapTupleHeaderClearHeapOnly(HeapTupleHeaderData *tup)
{
	tup->t_infomask2 &= ~HEAP_ONLY_TUPLE;
}

/*
 * These are used with both HeapTuple and MinimalTuple, so they must be
 * macros.
 */

#define HeapTupleHeaderGetNatts(tup) \
	((tup)->t_infomask2 & HEAP_NATTS_MASK)

#define HeapTupleHeaderSetNatts(tup, natts) \
( \
	(tup)->t_infomask2 = ((tup)->t_infomask2 & ~HEAP_NATTS_MASK) | (natts) \
)

#define HeapTupleHeaderHasExternal(tup) \
		(((tup)->t_infomask & HEAP_HASEXTERNAL) != 0)


/*
 * BITMAPLEN(NATTS) -
 *		Computes size of null bitmap given number of data columns.
 */
static inline int
BITMAPLEN(int NATTS)
{
	return (NATTS + 7) / 8;
}

/*
 * MaxHeapTupleSize is the maximum allowed size of a heap tuple, including
 * header and MAXALIGN alignment padding.  Basically it's BLCKSZ minus the
 * other stuff that has to be on a disk page.  Since heap pages use no
 * "special space", there's no deduction for that.
 *
 * NOTE: we allow for the ItemId that must point to the tuple, ensuring that
 * an otherwise-empty page can indeed hold a tuple of this size.  Because
 * ItemIds and tuples have different alignment requirements, don't assume that
 * you can, say, fit 2 tuples of size MaxHeapTupleSize/2 on the same page.
 *
 * On shift to 64-bit XIDs MaxHeapTupleSize decreased by sizeof(HeapPageSpecialData).
 * Extant tuples with length over new MaxHeapTupleSize are inherited on DoubleXmax
 * pages. They could be read, but can not be updated unless their length decreases
 * to fit MaxHeapTupleSize. Vacuum full will also copy these double xmax pages
 * without change.
 */

#define MaxHeapTupleSize  (BLCKSZ - MAXALIGN(SizeOfPageHeaderData + sizeof(ItemIdData)) - MAXALIGN(sizeof(HeapPageSpecialData)))
#define MaxHeapTupleSize_32  (BLCKSZ - MAXALIGN(SizeOfPageHeaderData + sizeof(ItemIdData)))
#define MinHeapTupleSize  MAXALIGN(SizeofHeapTupleHeader)

/*
 * MaxHeapTuplesPerPage is an upper bound on the number of tuples that can
 * fit on one heap page.  (Note that indexes could have more, because they
 * use a smaller tuple header.)  We arrive at the divisor because each tuple
 * must be maxaligned, and it must have an associated line pointer.
 *
 * Note: with HOT, there could theoretically be more line pointers (not actual
 * tuples) than this on a heap page.  However we constrain the number of line
 * pointers to this anyway, to avoid excessive line-pointer bloat and not
 * require increases in the size of work arrays.
 */
#define MaxHeapTuplesPerPage	\
	((int) ((BLCKSZ - SizeOfPageHeaderData) / \
			(MAXALIGN(SizeofHeapTupleHeader) + sizeof(ItemIdData))))

/*
 * MaxAttrSize is a somewhat arbitrary upper limit on the declared size of
 * data fields of char(n) and similar types.  It need not have anything
 * directly to do with the *actual* upper limit of varlena values, which
 * is currently 1Gb (see TOAST structures in postgres.h).  I've set it
 * at 10Mb which seems like a reasonable number --- tgl 8/6/00.
 */
#define MaxAttrSize		(10 * 1024 * 1024)


/*
 * MinimalTuple is an alternative representation that is used for transient
 * tuples inside the executor, in places where transaction status information
 * is not required, the tuple rowtype is known, and shaving off a few bytes
 * is worthwhile because we need to store many tuples.  The representation
 * is chosen so that tuple access routines can work with either full or
 * minimal tuples via a HeapTupleData pointer structure.  The access routines
 * see no difference, except that they must not access the transaction status
 * or t_ctid fields because those aren't there.
 *
 * For the most part, MinimalTuples should be accessed via TupleTableSlot
 * routines.  These routines will prevent access to the "system columns"
 * and thereby prevent accidental use of the nonexistent fields.
 *
 * MinimalTupleData contains a length word, some padding, and fields matching
 * HeapTupleHeaderData beginning with t_infomask2. The padding is chosen so
 * that offsetof(t_infomask2) is the same modulo MAXIMUM_ALIGNOF in both
 * structs.   This makes data alignment rules equivalent in both cases.
 *
 * When a minimal tuple is accessed via a HeapTupleData pointer, t_data is
 * set to point MINIMAL_TUPLE_OFFSET bytes before the actual start of the
 * minimal tuple --- that is, where a full tuple matching the minimal tuple's
 * data would start.  This trick is what makes the structs seem equivalent.
 *
 * Note that t_hoff is computed the same as in a full tuple, hence it includes
 * the MINIMAL_TUPLE_OFFSET distance.  t_len does not include that, however.
 *
 * MINIMAL_TUPLE_DATA_OFFSET is the offset to the first useful (non-pad) data
 * other than the length word.  tuplesort.c and tuplestore.c use this to avoid
 * writing the padding to disk.
 */
#define MINIMAL_TUPLE_OFFSET \
	((offsetof(HeapTupleHeaderData, t_infomask2) - sizeof(uint32)) / MAXIMUM_ALIGNOF * MAXIMUM_ALIGNOF)
#define MINIMAL_TUPLE_PADDING \
	((offsetof(HeapTupleHeaderData, t_infomask2) - sizeof(uint32)) % MAXIMUM_ALIGNOF)
#define MINIMAL_TUPLE_DATA_OFFSET \
	offsetof(MinimalTupleData, t_infomask2)

struct MinimalTupleData
{
	uint32		t_len;			/* actual length of minimal tuple */

	char		mt_padding[MINIMAL_TUPLE_PADDING];

	/* Fields below here must match HeapTupleHeaderData! */

	uint16		t_infomask2;	/* number of attributes + various flags */

	uint16		t_infomask;		/* various flag bits, see below */

	uint8		t_hoff;			/* sizeof header incl. bitmap, padding */

	/* ^ - 23 bytes - ^ */

	bits8		t_bits[FLEXIBLE_ARRAY_MEMBER];	/* bitmap of NULLs */

	/* MORE DATA FOLLOWS AT END OF STRUCT */
};

/* typedef appears in htup.h */

#define SizeofMinimalTupleHeader offsetof(MinimalTupleData, t_bits)

/*
 * MinimalTuple accessor functions
 */

static inline bool
HeapTupleHeaderHasMatch(const MinimalTupleData *tup)
{
	return (tup->t_infomask2 & HEAP_TUPLE_HAS_MATCH) != 0;
}

static inline void
HeapTupleHeaderSetMatch(MinimalTupleData *tup)
{
	tup->t_infomask2 |= HEAP_TUPLE_HAS_MATCH;
}

static inline void
HeapTupleHeaderClearMatch(MinimalTupleData *tup)
{
	tup->t_infomask2 &= ~HEAP_TUPLE_HAS_MATCH;
}


/*
 * GETSTRUCT - given a HeapTuple pointer, return address of the user data
 */
static inline void *
GETSTRUCT(const HeapTupleData *tuple)
{
	return ((char *) (tuple->t_data) + tuple->t_data->t_hoff);
}

/*
 * Accessor functions to be used with HeapTuple pointers.
 */

static inline bool
HeapTupleHasNulls(const HeapTupleData *tuple)
{
	return (tuple->t_data->t_infomask & HEAP_HASNULL) != 0;
}

static inline bool
HeapTupleNoNulls(const HeapTupleData *tuple)
{
	return !HeapTupleHasNulls(tuple);
}

static inline bool
HeapTupleHasVarWidth(const HeapTupleData *tuple)
{
	return (tuple->t_data->t_infomask & HEAP_HASVARWIDTH) != 0;
}

static inline bool
HeapTupleAllFixed(const HeapTupleData *tuple)
{
	return !HeapTupleHasVarWidth(tuple);
}

static inline bool
HeapTupleHasExternal(const HeapTupleData *tuple)
{
	return (tuple->t_data->t_infomask & HEAP_HASEXTERNAL) != 0;
}

static inline bool
HeapTupleIsHotUpdated(const HeapTupleData *tuple)
{
	return HeapTupleHeaderIsHotUpdated(tuple->t_data);
}

static inline void
HeapTupleSetHotUpdated(const HeapTupleData *tuple)
{
	HeapTupleHeaderSetHotUpdated(tuple->t_data);
}

static inline void
HeapTupleClearHotUpdated(const HeapTupleData *tuple)
{
	HeapTupleHeaderClearHotUpdated(tuple->t_data);
}

static inline bool
HeapTupleIsHeapOnly(const HeapTupleData *tuple)
{
	return HeapTupleHeaderIsHeapOnly(tuple->t_data);
}

static inline void
HeapTupleSetHeapOnly(const HeapTupleData *tuple)
{
	HeapTupleHeaderSetHeapOnly(tuple->t_data);
}

static inline void
HeapTupleClearHeapOnly(const HeapTupleData *tuple)
{
	HeapTupleHeaderClearHeapOnly(tuple->t_data);
}

/*
 * Copy base values for xid and multixacts from one heap tuple to heap tuple.
 * Should be called on tuple copy or making desc tuple on the base on src tuple
 * saving visibility information.
 */
static inline void
HeapTupleCopyXids(HeapTupleData *dest, const HeapTupleData *src)
{
	dest->t_xmin = src->t_xmin;
	dest->t_xmax = src->t_xmax;
}

/*
 * Set base values for tuple xids/multixacts to zero.  Used when visibility
 * infromation is negligible or will be set later.
 */
static inline void
HeapTupleSetZeroXids(HeapTupleData *tup)
{
	tup->t_xmin = 0;
	tup->t_xmax = 0;
}

/*
 * Copy HeapTupleHeader xmin/xmax in raw way ???
 */
static inline void
HeapTupleCopyHeaderXids(HeapTupleData *tup)
{
	tup->t_xmin = tup->t_data->t_choice.t_heap.t_xmin;
	tup->t_xmax = tup->t_data->t_choice.t_heap.t_xmax;
}

static inline void
HeapTupleCopyXminFromPage(HeapTupleData *tup, Page page)
{
	TransactionId			base;
	ShortTransactionId		xmin;	/* short xmin from tuple header */

	if (HeapTupleHeaderXminFrozen(tup->t_data))
	{
		tup->t_xmin = FrozenTransactionId;
		return;
	}

	xmin = tup->t_data->t_choice.t_heap.t_xmin;

	if (!TransactionIdIsNormal(xmin))
		base = 0;
	else
		base = PageGetSpecialXidBase(page);

	tup->t_xmin = ShortTransactionIdToNormal(base, xmin);
}

static inline void
HeapTupleCopyXmaxFromPage(HeapTupleData *tup, Page page)
{
	TransactionId			base;
	ShortTransactionId		xmax;	/* short xmax from tuple header */

	xmax = tup->t_data->t_choice.t_heap.t_xmax;

	if (!TransactionIdIsNormal(xmax))
		base = 0;
	else if (tup->t_data->t_infomask & HEAP_XMAX_IS_MULTI)
		base = HeapPageGetSpecial(page)->pd_multi_base;
	else
		base = PageGetSpecialXidBase(page);

	tup->t_xmax = ShortTransactionIdToNormal(base, xmax);
}

/*
 * Copy base values for xid and multixacts from page to heap tuple.  Should be
 * called each time tuple is read from page.  Otherwise, it would be impossible
 * to correctly read tuple xmin and xmax.
 */
static inline void
HeapTupleCopyXidsFromPage(Buffer buffer, HeapTupleData *tup, Page page)
{
	Assert(IsBufferLocked(buffer));

	if (HeapPageIsDoubleXmax(page))
	{
		/*
		 * On double xmax pages, xmax is extracted from tuple header.
		 */
		tup->t_xmin = FrozenTransactionId;
		tup->t_xmax = HeapTupleHeaderGetDoubleXmax(tup->t_data);
		return;
	}

	HeapTupleCopyXminFromPage(tup, page);
	HeapTupleCopyXmaxFromPage(tup, page);
}

/* prototypes for functions in common/heaptuple.c */
extern Size heap_compute_data_size(TupleDesc tupleDesc,
								   const Datum *values, const bool *isnull);
extern void heap_fill_tuple(TupleDesc tupleDesc,
							const Datum *values, const bool *isnull,
							char *data, Size data_size,
							uint16 *infomask, bits8 *bit);
extern bool heap_attisnull(HeapTuple tup, int attnum, TupleDesc tupleDesc);
extern Datum nocachegetattr(HeapTuple tup, int attnum,
							TupleDesc tupleDesc);
extern Datum heap_getsysattr(HeapTuple tup, int attnum, TupleDesc tupleDesc,
							 bool *isnull);
extern Datum getmissingattr(TupleDesc tupleDesc,
							int attnum, bool *isnull);
extern HeapTuple heap_copytuple(HeapTuple tuple);
extern void heap_copytuple_with_tuple(HeapTuple src, HeapTuple dest);
extern Datum heap_copy_tuple_as_datum(HeapTuple tuple, TupleDesc tupleDesc);
extern HeapTuple heap_form_tuple(TupleDesc tupleDescriptor,
								 const Datum *values, const bool *isnull);
extern HeapTuple heap_modify_tuple(HeapTuple tuple,
								   TupleDesc tupleDesc,
								   const Datum *replValues,
								   const bool *replIsnull,
								   const bool *doReplace);
extern HeapTuple heap_modify_tuple_by_cols(HeapTuple tuple,
										   TupleDesc tupleDesc,
										   int nCols,
										   const int *replCols,
										   const Datum *replValues,
										   const bool *replIsnull);
extern void heap_deform_tuple(HeapTuple tuple, TupleDesc tupleDesc,
							  Datum *values, bool *isnull);
extern void heap_freetuple(HeapTuple htup);
extern MinimalTuple heap_form_minimal_tuple(TupleDesc tupleDescriptor,
											const Datum *values, const bool *isnull,
											Size extra);
extern void heap_free_minimal_tuple(MinimalTuple mtup);
extern MinimalTuple heap_copy_minimal_tuple(MinimalTuple mtup, Size extra);
extern HeapTuple heap_tuple_from_minimal_tuple(MinimalTuple mtup);
extern MinimalTuple minimal_tuple_from_heap_tuple(HeapTuple htup, Size extra);
extern size_t varsize_any(void *p);
extern HeapTuple heap_expand_tuple(HeapTuple sourceTuple, TupleDesc tupleDesc);
extern MinimalTuple minimal_expand_tuple(HeapTuple sourceTuple, TupleDesc tupleDesc);

#ifndef FRONTEND
/*
 *	fastgetattr
 *		Fetch a user attribute's value as a Datum (might be either a
 *		value, or a pointer into the data area of the tuple).
 *
 *		This must not be used when a system attribute might be requested.
 *		Furthermore, the passed attnum MUST be valid.  Use heap_getattr()
 *		instead, if in doubt.
 *
 *		This gets called many times, so we macro the cacheable and NULL
 *		lookups, and call nocachegetattr() for the rest.
 */
static inline Datum
fastgetattr(HeapTuple tup, int attnum, TupleDesc tupleDesc, bool *isnull)
{
	Assert(attnum > 0);

	*isnull = false;
	if (HeapTupleNoNulls(tup))
	{
		CompactAttribute *att;

		att = TupleDescCompactAttr(tupleDesc, attnum - 1);
		if (att->attcacheoff >= 0)
			return fetchatt(att, (char *) tup->t_data + tup->t_data->t_hoff +
							att->attcacheoff);
		else
			return nocachegetattr(tup, attnum, tupleDesc);
	}
	else
	{
		if (att_isnull(attnum - 1, tup->t_data->t_bits))
		{
			*isnull = true;
			return (Datum) NULL;
		}
		else
			return nocachegetattr(tup, attnum, tupleDesc);
	}
}

/*
 *	heap_getattr
 *		Extract an attribute of a heap tuple and return it as a Datum.
 *		This works for either system or user attributes.  The given attnum
 *		is properly range-checked.
 *
 *		If the field in question has a NULL value, we return a zero Datum
 *		and set *isnull == true.  Otherwise, we set *isnull == false.
 *
 *		<tup> is the pointer to the heap tuple.  <attnum> is the attribute
 *		number of the column (field) caller wants.  <tupleDesc> is a
 *		pointer to the structure describing the row and all its fields.
 *
 */
static inline Datum
heap_getattr(HeapTuple tup, int attnum, TupleDesc tupleDesc, bool *isnull)
{
	if (attnum > 0)
	{
		if (attnum > (int) HeapTupleHeaderGetNatts(tup->t_data))
			return getmissingattr(tupleDesc, attnum, isnull);
		else
			return fastgetattr(tup, attnum, tupleDesc, isnull);
	}
	else
		return heap_getsysattr(tup, attnum, tupleDesc, isnull);
}
#endif							/* FRONTEND */

#endif							/* HTUP_DETAILS_H */

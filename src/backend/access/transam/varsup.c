/*-------------------------------------------------------------------------
 *
 * varsup.c
 *	  postgres OID & XID variables support routines
 *
 * Copyright (c) 2000-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/access/transam/varsup.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/clog.h"
#include "access/commit_ts.h"
#include "access/subtrans.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlogutils.h"
#include "commands/dbcommands.h"
#include "miscadmin.h"
#include "postmaster/autovacuum.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "utils/syscache.h"


/* Number of OIDs to prefetch (preallocate) per XLOG write */
#define VAR_OID_PREFETCH		8192

/* pointer to variables struct in shared memory */
TransamVariablesData *TransamVariables = NULL;


/*
 * Initialization of shared memory for TransamVariables.
 */
Size
VarsupShmemSize(void)
{
	return sizeof(TransamVariablesData);
}

void
VarsupShmemInit(void)
{
	bool		found;

	/* Initialize our shared state struct */
	TransamVariables = ShmemInitStruct("TransamVariables",
									   sizeof(TransamVariablesData),
									   &found);
	if (!IsUnderPostmaster)
	{
		Assert(!found);
		memset(TransamVariables, 0, sizeof(TransamVariablesData));
	}
	else
		Assert(found);
}

/*
 * Allocate the next FullTransactionId for a new transaction or
 * subtransaction.
 *
 * The new XID is also stored into MyProc->xid/ProcGlobal->xids[] before
 * returning.
 *
 * Note: when this is called, we are actually already inside a valid
 * transaction, since XIDs are now not allocated until the transaction
 * does something.  So it is safe to do a database lookup if we want to
 * issue a warning about XID wrap.
 */
FullTransactionId
GetNewTransactionId(bool isSubXact)
{
	FullTransactionId full_xid;
	TransactionId xid;

	/*
	 * Workers synchronize transaction state at the beginning of each parallel
	 * operation, so we can't account for new XIDs after that point.
	 */
	if (IsInParallelMode())
		elog(ERROR, "cannot assign TransactionIds during a parallel operation");

	/*
	 * During bootstrap initialization, we return the special bootstrap
	 * transaction id.
	 */
	if (IsBootstrapProcessingMode())
	{
		Assert(!isSubXact);
		pg_atomic_write_u64(&MyProc->xid, BootstrapTransactionId);
		pg_atomic_write_u64(&ProcGlobal->xids[MyProc->pgxactoff], BootstrapTransactionId);
		return FullTransactionIdFromXid(BootstrapTransactionId);
	}

	/* safety check, we should never get this far in a HS standby */
	if (RecoveryInProgress())
		elog(ERROR, "cannot assign TransactionIds during recovery");

	LWLockAcquire(XidGenLock, LW_EXCLUSIVE);

	full_xid = TransamVariables->nextXid;
	xid = XidFromFullTransactionId(full_xid);

	if (TransactionIdFollowsOrEquals(xid, TransamVariables->xidVacLimit))
	{
		/*
		 * For safety's sake, we release XidGenLock while sending signals,
		 * warnings, etc.  This is not so much because we care about
		 * preserving concurrency in this situation, as to avoid any
		 * possibility of deadlock while doing get_database_name(). First,
		 * copy all the shared values we'll need in this path.
		 */
		LWLockRelease(XidGenLock);

		/*
		 * To avoid swamping the postmaster with signals, we issue the autovac
		 * request only once per 64K transaction starts.  This still gives
		 * plenty of chances before we get into real trouble.
		 */
		if (IsUnderPostmaster && (xid % 65536) == 0)
			SendPostmasterSignal(PMSIGNAL_START_AUTOVAC_LAUNCHER);

		/* Re-acquire lock and start over */
		LWLockAcquire(XidGenLock, LW_EXCLUSIVE);
		full_xid = TransamVariables->nextXid;
		xid = XidFromFullTransactionId(full_xid);
	}

	/*
	 * If we are allocating the first XID of a new page of the commit log,
	 * zero out that commit-log page before returning. We must do this while
	 * holding XidGenLock, else another xact could acquire and commit a later
	 * XID before we zero the page.  Fortunately, a page of the commit log
	 * holds 32K or more transactions, so we don't have to do this very often.
	 *
	 * Extend pg_subtrans and pg_commit_ts too.
	 */
	ExtendCLOG(xid);
	ExtendCommitTs(xid);
	ExtendSUBTRANS(xid);

	/*
	 * Now advance the nextXid counter.  This must not happen until after we
	 * have successfully completed ExtendCLOG() --- if that routine fails, we
	 * want the next incoming transaction to try it again.  We cannot assign
	 * more XIDs until there is CLOG space for them.
	 */
	FullTransactionIdAdvance(&TransamVariables->nextXid);

	/*
	 * We must store the new XID into the shared ProcArray before releasing
	 * XidGenLock.  This ensures that every active XID older than
	 * latestCompletedXid is present in the ProcArray, which is essential for
	 * correct OldestXmin tracking; see src/backend/access/transam/README.
	 *
	 * Note that readers of ProcGlobal->xids/PGPROC->xid should be careful to
	 * fetch the value for each proc only once, rather than assume they can
	 * read a value multiple times and get the same answer each time.  Note we
	 * are assuming that TransactionId and int fetch/store are atomic.
	 *
	 * The same comments apply to the subxact xid count and overflow fields.
	 *
	 * Use of a write barrier prevents dangerous code rearrangement in this
	 * function; other backends could otherwise e.g. be examining my subxids
	 * info concurrently, and we don't want them to see an invalid
	 * intermediate state, such as an incremented nxids before the array entry
	 * is filled.
	 *
	 * Other processes that read nxids should do so before reading xids
	 * elements with a pg_read_barrier() in between, so that they can be sure
	 * not to read an uninitialized array element; see
	 * src/backend/storage/lmgr/README.barrier.
	 *
	 * If there's no room to fit a subtransaction XID into PGPROC, set the
	 * cache-overflowed flag instead.  This forces readers to look in
	 * pg_subtrans to map subtransaction XIDs up to top-level XIDs. There is a
	 * race-condition window, in that the new XID will not appear as running
	 * until its parent link has been placed into pg_subtrans. However, that
	 * will happen before anyone could possibly have a reason to inquire about
	 * the status of the XID, so it seems OK.  (Snapshots taken during this
	 * window *will* include the parent XID, so they will deliver the correct
	 * answer later on when someone does have a reason to inquire.)
	 */
	if (!isSubXact)
	{
		Assert(ProcGlobal->subxidStates[MyProc->pgxactoff].count == 0);
		Assert(!ProcGlobal->subxidStates[MyProc->pgxactoff].overflowed);
		Assert(MyProc->subxidStatus.count == 0);
		Assert(!MyProc->subxidStatus.overflowed);

		/* LWLockRelease acts as barrier */
		pg_atomic_write_u64(&MyProc->xid, xid);
		pg_atomic_write_u64(&ProcGlobal->xids[MyProc->pgxactoff], xid);
	}
	else
	{
		XidCacheStatus *substat = &ProcGlobal->subxidStates[MyProc->pgxactoff];
		int			nxids = MyProc->subxidStatus.count;

		Assert(substat->count == MyProc->subxidStatus.count);
		Assert(substat->overflowed == MyProc->subxidStatus.overflowed);

		if (nxids < PGPROC_MAX_CACHED_SUBXIDS)
		{
			MyProc->subxids.xids[nxids] = xid;
			pg_write_barrier();
			MyProc->subxidStatus.count = substat->count = nxids + 1;
		}
		else
			MyProc->subxidStatus.overflowed = substat->overflowed = true;
	}

	LWLockRelease(XidGenLock);

	return full_xid;
}

/*
 * Read nextXid but don't allocate it.
 */
FullTransactionId
ReadNextFullTransactionId(void)
{
	FullTransactionId fullXid;

	LWLockAcquire(XidGenLock, LW_SHARED);
	fullXid = TransamVariables->nextXid;
	LWLockRelease(XidGenLock);

	return fullXid;
}

/*
 * Advance nextXid to the value after a given xid.
 * This must only be called during recovery or from two-phase start-up code.
 */
void
AdvanceNextFullTransactionIdPastXid(TransactionId xid)
{
	FullTransactionId newNextFullXid;
	TransactionId next_xid;

	/*
	 * It is safe to read nextXid without a lock, because this is only called
	 * from the startup process or single-process mode, meaning that no other
	 * process can modify it.
	 */
	Assert(AmStartupProcess() || !IsUnderPostmaster);

	/* Fast return if this isn't an xid high enough to move the needle. */
	next_xid = XidFromFullTransactionId(TransamVariables->nextXid);
	if (!TransactionIdFollowsOrEquals(xid, next_xid))
		return;

	/* Compute the FullTransactionId that comes after the given xid. */
	TransactionIdAdvance(xid);
	newNextFullXid = FullTransactionIdFromXid(xid);

	/*
	 * We still need to take a lock to modify the value when there are
	 * concurrent readers.
	 */
	LWLockAcquire(XidGenLock, LW_EXCLUSIVE);
	TransamVariables->nextXid = newNextFullXid;
	LWLockRelease(XidGenLock);
}

/*
 * Advance the cluster-wide value for the oldest valid clog entry.
 *
 * We must acquire XactTruncationLock to advance the oldestClogXid. It's not
 * necessary to hold the lock during the actual clog truncation, only when we
 * advance the limit, as code looking up arbitrary xids is required to hold
 * XactTruncationLock from when it tests oldestClogXid through to when it
 * completes the clog lookup.
 */
void
AdvanceOldestClogXid(TransactionId oldest_datfrozenxid)
{
	LWLockAcquire(XactTruncationLock, LW_EXCLUSIVE);
	if (TransactionIdPrecedes(TransamVariables->oldestClogXid,
							  oldest_datfrozenxid))
	{
		TransamVariables->oldestClogXid = oldest_datfrozenxid;
	}
	LWLockRelease(XactTruncationLock);
}

/*
 * Determine the last safe XID to allocate using the currently oldest
 * datfrozenxid (ie, the oldest XID that might exist in any database
 * of our cluster), and the OID of the (or a) database with that value.
 */
void
SetTransactionIdLimit(TransactionId oldest_datfrozenxid, Oid oldest_datoid)
{
	TransactionId xidVacLimit;
	TransactionId curXid;

	Assert(TransactionIdIsNormal(oldest_datfrozenxid));

	/*
	 * We'll start trying to force autovacuums when oldest_datfrozenxid gets
	 * to be more than autovacuum_freeze_max_age transactions old.
	 *
	 * Note: autovacuum_freeze_max_age is a PGC_POSTMASTER parameter so that
	 * we don't have to worry about dealing with on-the-fly changes in its
	 * value.  It doesn't look practical to update shared state from a GUC
	 * assign hook (too many processes would try to execute the hook,
	 * resulting in race conditions as well as crashes of those not connected
	 * to shared memory).  Perhaps this can be improved someday.  See also
	 * SetMultiXactIdLimit.
	 */
	xidVacLimit = oldest_datfrozenxid + autovacuum_freeze_max_age;
	if (xidVacLimit < FirstNormalTransactionId)
		xidVacLimit += FirstNormalTransactionId;

	/* Grab lock for just long enough to set the new limit values */
	LWLockAcquire(XidGenLock, LW_EXCLUSIVE);
	TransamVariables->oldestXid = oldest_datfrozenxid;
	TransamVariables->xidVacLimit = xidVacLimit;
	TransamVariables->oldestXidDB = oldest_datoid;
	curXid = XidFromFullTransactionId(TransamVariables->nextXid);
	LWLockRelease(XidGenLock);

	/*
	 * If past the autovacuum force point, immediately signal an autovac
	 * request.  The reason for this is that autovac only processes one
	 * database per invocation.  Once it's finished cleaning up the oldest
	 * database, it'll call here, and we'll signal the postmaster to start
	 * another iteration immediately if there are still any old databases.
	 */
	if (TransactionIdFollowsOrEquals(curXid, xidVacLimit) &&
		IsUnderPostmaster && !InRecovery)
		SendPostmasterSignal(PMSIGNAL_START_AUTOVAC_LAUNCHER);
}


/*
 * ForceTransactionIdLimitUpdate -- does the XID wrap-limit data need updating?
 *
 * We primarily check whether oldestXidDB is valid.  The cases we have in
 * mind are that that database was dropped, or the field was reset to zero
 * by pg_resetwal.  In either case we should force recalculation of the
 * wrap limit.  Also do it if oldestXid is old enough to be forcing
 * autovacuums or other actions; this ensures we update our state as soon
 * as possible once extra overhead is being incurred.
 */
bool
ForceTransactionIdLimitUpdate(void)
{
	TransactionId nextXid;
	TransactionId xidVacLimit;
	TransactionId oldestXid;
	Oid			oldestXidDB;

	/* Locking is probably not really necessary, but let's be careful */
	LWLockAcquire(XidGenLock, LW_SHARED);
	nextXid = XidFromFullTransactionId(TransamVariables->nextXid);
	xidVacLimit = TransamVariables->xidVacLimit;
	oldestXid = TransamVariables->oldestXid;
	oldestXidDB = TransamVariables->oldestXidDB;
	LWLockRelease(XidGenLock);

	if (!TransactionIdIsNormal(oldestXid))
		return true;			/* shouldn't happen, but just in case */
	if (!TransactionIdIsValid(xidVacLimit))
		return true;			/* this shouldn't happen anymore either */
	if (TransactionIdFollowsOrEquals(nextXid, xidVacLimit))
		return true;			/* past xidVacLimit, don't delay updating */
	if (!SearchSysCacheExists1(DATABASEOID, ObjectIdGetDatum(oldestXidDB)))
		return true;			/* could happen, per comments above */
	return false;
}


/*
 * GetNewObjectId -- allocate a new OID
 *
 * OIDs are generated by a cluster-wide counter.  Since they are only 32 bits
 * wide, counter wraparound will occur eventually, and therefore it is unwise
 * to assume they are unique unless precautions are taken to make them so.
 * Hence, this routine should generally not be used directly.  The only direct
 * callers should be GetNewOidWithIndex() and GetNewRelFileNumber() in
 * catalog/catalog.c.
 */
Oid
GetNewObjectId(void)
{
	Oid			result;

	/* safety check, we should never get this far in a HS standby */
	if (RecoveryInProgress())
		elog(ERROR, "cannot assign OIDs during recovery");

	LWLockAcquire(OidGenLock, LW_EXCLUSIVE);

	/*
	 * Check for wraparound of the OID counter.  We *must* not return 0
	 * (InvalidOid), and in normal operation we mustn't return anything below
	 * FirstNormalObjectId since that range is reserved for initdb (see
	 * IsCatalogRelationOid()).  Note we are relying on unsigned comparison.
	 *
	 * During initdb, we start the OID generator at FirstGenbkiObjectId, so we
	 * only wrap if before that point when in bootstrap or standalone mode.
	 * The first time through this routine after normal postmaster start, the
	 * counter will be forced up to FirstNormalObjectId.  This mechanism
	 * leaves the OIDs between FirstGenbkiObjectId and FirstNormalObjectId
	 * available for automatic assignment during initdb, while ensuring they
	 * will never conflict with user-assigned OIDs.
	 */
	if (TransamVariables->nextOid < ((Oid) FirstNormalObjectId))
	{
		if (IsPostmasterEnvironment)
		{
			/* wraparound, or first post-initdb assignment, in normal mode */
			TransamVariables->nextOid = FirstNormalObjectId;
			TransamVariables->oidCount = 0;
		}
		else
		{
			/* we may be bootstrapping, so don't enforce the full range */
			if (TransamVariables->nextOid < ((Oid) FirstGenbkiObjectId))
			{
				/* wraparound in standalone mode (unlikely but possible) */
				TransamVariables->nextOid = FirstNormalObjectId;
				TransamVariables->oidCount = 0;
			}
		}
	}

	/* If we run out of logged for use oids then we must log more */
	if (TransamVariables->oidCount == 0)
	{
		XLogPutNextOid(TransamVariables->nextOid + VAR_OID_PREFETCH);
		TransamVariables->oidCount = VAR_OID_PREFETCH;
	}

	result = TransamVariables->nextOid;

	(TransamVariables->nextOid)++;
	(TransamVariables->oidCount)--;

	LWLockRelease(OidGenLock);

	return result;
}

/*
 * SetNextObjectId
 *
 * This may only be called during initdb; it advances the OID counter
 * to the specified value.
 */
static void
SetNextObjectId(Oid nextOid)
{
	/* Safety check, this is only allowable during initdb */
	if (IsPostmasterEnvironment)
		elog(ERROR, "cannot advance OID counter anymore");

	/* Taking the lock is, therefore, just pro forma; but do it anyway */
	LWLockAcquire(OidGenLock, LW_EXCLUSIVE);

	if (TransamVariables->nextOid > nextOid)
		elog(ERROR, "too late to advance OID counter to %u, it is now %u",
			 nextOid, TransamVariables->nextOid);

	TransamVariables->nextOid = nextOid;
	TransamVariables->oidCount = 0;

	LWLockRelease(OidGenLock);
}

/*
 * StopGeneratingPinnedObjectIds
 *
 * This is called once during initdb to force the OID counter up to
 * FirstUnpinnedObjectId.  This supports letting initdb's post-bootstrap
 * processing create some pinned objects early on.  Once it's done doing
 * so, it calls this (via pg_stop_making_pinned_objects()) so that the
 * remaining objects it makes will be considered un-pinned.
 */
void
StopGeneratingPinnedObjectIds(void)
{
	SetNextObjectId(FirstUnpinnedObjectId);
}


#ifdef USE_ASSERT_CHECKING

/*
 * Assert that xid is between [oldestXid, nextXid], which is the range we
 * expect XIDs coming from tables etc to be in.
 *
 * As TransamVariables->oldestXid could change just after this call without
 * further precautions, and as a wrapped-around xid could again fall within
 * the valid range, this assertion can only detect if something is definitely
 * wrong, but not establish correctness.
 *
 * This intentionally does not expose a return value, to avoid code being
 * introduced that depends on the return value.
 */
void
AssertTransactionIdInAllowableRange(TransactionId xid)
{
	TransactionId oldest_xid;
	TransactionId next_xid;

	Assert(TransactionIdIsValid(xid));

	/* we may see bootstrap / frozen */
	if (!TransactionIdIsNormal(xid))
		return;

	/*
	 * We can't acquire XidGenLock, as this may be called with XidGenLock
	 * already held (or with other locks that don't allow XidGenLock to be
	 * nested). That's ok for our purposes though, since we already rely on
	 * 32bit reads to be atomic. While nextXid is 64 bit, we only look at the
	 * lower 32bit, so a skewed read doesn't hurt.
	 *
	 * There's no increased danger of falling outside [oldest, next] by
	 * accessing them without a lock. xid needs to have been created with
	 * GetNewTransactionId() in the originating session, and the locks there
	 * pair with the memory barrier below.  We do however accept xid to be <=
	 * to next_xid, instead of just <, as xid could be from the procarray,
	 * before we see the updated nextXid value.
	 */
	pg_memory_barrier();
	oldest_xid = TransamVariables->oldestXid;
	next_xid = XidFromFullTransactionId(TransamVariables->nextXid);

	Assert(TransactionIdFollowsOrEquals(xid, oldest_xid) ||
		   TransactionIdPrecedesOrEquals(xid, next_xid));
}
#endif

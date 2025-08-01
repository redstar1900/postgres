/*-------------------------------------------------------------------------
 * logical.c
 *	   PostgreSQL logical decoding coordination
 *
 * Copyright (c) 2012-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/logical.c
 *
 * NOTES
 *	  This file coordinates interaction between the various modules that
 *	  together provide logical decoding, primarily by providing so
 *	  called LogicalDecodingContexts. The goal is to encapsulate most of the
 *	  internal complexity for consumers of logical decoding, so they can
 *	  create and consume a changestream with a low amount of code. Builtin
 *	  consumers are the walsender and SQL SRF interface, but it's possible to
 *	  add further ones without changing core code, e.g. to consume changes in
 *	  a bgworker.
 *
 *	  The idea is that a consumer provides three callbacks, one to read WAL,
 *	  one to prepare a data write, and a final one for actually writing since
 *	  their implementation depends on the type of consumer.  Check
 *	  logicalfuncs.c for an example implementation of a fairly simple consumer
 *	  and an implementation of a WAL reading callback that's suitable for
 *	  simple consumers.
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xact.h"
#include "access/xlog_internal.h"
#include "access/xlogutils.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "replication/decode.h"
#include "replication/logical.h"
#include "replication/reorderbuffer.h"
#include "replication/slotsync.h"
#include "replication/snapbuild.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/injection_point.h"
#include "utils/inval.h"
#include "utils/memutils.h"

/* data for errcontext callback */
typedef struct LogicalErrorCallbackState
{
	LogicalDecodingContext *ctx;
	const char *callback_name;
	XLogRecPtr	report_location;
} LogicalErrorCallbackState;

/* wrappers around output plugin callbacks */
static void output_plugin_error_callback(void *arg);
static void startup_cb_wrapper(LogicalDecodingContext *ctx, OutputPluginOptions *opt,
							   bool is_init);
static void shutdown_cb_wrapper(LogicalDecodingContext *ctx);
static void begin_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn);
static void commit_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
							  XLogRecPtr commit_lsn);
static void begin_prepare_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn);
static void prepare_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
							   XLogRecPtr prepare_lsn);
static void commit_prepared_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
									   XLogRecPtr commit_lsn);
static void rollback_prepared_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
										 XLogRecPtr prepare_end_lsn, TimestampTz prepare_time);
static void change_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
							  Relation relation, ReorderBufferChange *change);
static void truncate_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
								int nrelations, Relation relations[], ReorderBufferChange *change);
static void message_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
							   XLogRecPtr message_lsn, bool transactional,
							   const char *prefix, Size message_size, const char *message);

/* streaming callbacks */
static void stream_start_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
									XLogRecPtr first_lsn);
static void stream_stop_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
								   XLogRecPtr last_lsn);
static void stream_abort_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
									XLogRecPtr abort_lsn);
static void stream_prepare_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
									  XLogRecPtr prepare_lsn);
static void stream_commit_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
									 XLogRecPtr commit_lsn);
static void stream_change_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
									 Relation relation, ReorderBufferChange *change);
static void stream_message_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
									  XLogRecPtr message_lsn, bool transactional,
									  const char *prefix, Size message_size, const char *message);
static void stream_truncate_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
									   int nrelations, Relation relations[], ReorderBufferChange *change);

/* callback to update txn's progress */
static void update_progress_txn_cb_wrapper(ReorderBuffer *cache,
										   ReorderBufferTXN *txn,
										   XLogRecPtr lsn);

static void LoadOutputPlugin(OutputPluginCallbacks *callbacks, const char *plugin);

/*
 * Make sure the current settings & environment are capable of doing logical
 * decoding.
 */
void
CheckLogicalDecodingRequirements(void)
{
	CheckSlotRequirements();

	/*
	 * NB: Adding a new requirement likely means that RestoreSlotFromDisk()
	 * needs the same check.
	 */

	if (wal_level < WAL_LEVEL_LOGICAL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical decoding requires \"wal_level\" >= \"logical\"")));

	if (MyDatabaseId == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical decoding requires a database connection")));

	if (RecoveryInProgress())
	{
		/*
		 * This check may have race conditions, but whenever
		 * XLOG_PARAMETER_CHANGE indicates that wal_level has changed, we
		 * verify that there are no existing logical replication slots. And to
		 * avoid races around creating a new slot,
		 * CheckLogicalDecodingRequirements() is called once before creating
		 * the slot, and once when logical decoding is initially starting up.
		 */
		if (GetActiveWalLevelOnStandby() < WAL_LEVEL_LOGICAL)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("logical decoding on standby requires \"wal_level\" >= \"logical\" on the primary")));
	}
}

/*
 * Helper function for CreateInitDecodingContext() and
 * CreateDecodingContext() performing common tasks.
 */
static LogicalDecodingContext *
StartupDecodingContext(List *output_plugin_options,
					   XLogRecPtr start_lsn,
					   TransactionId xmin_horizon,
					   bool need_full_snapshot,
					   bool fast_forward,
					   bool in_create,
					   XLogReaderRoutine *xl_routine,
					   LogicalOutputPluginWriterPrepareWrite prepare_write,
					   LogicalOutputPluginWriterWrite do_write,
					   LogicalOutputPluginWriterUpdateProgress update_progress)
{
	ReplicationSlot *slot;
	MemoryContext context,
				old_context;
	LogicalDecodingContext *ctx;

	/* shorter lines... */
	slot = MyReplicationSlot;

	context = AllocSetContextCreate(CurrentMemoryContext,
									"Logical decoding context",
									ALLOCSET_DEFAULT_SIZES);
	old_context = MemoryContextSwitchTo(context);
	ctx = palloc0(sizeof(LogicalDecodingContext));

	ctx->context = context;

	/*
	 * (re-)load output plugins, so we detect a bad (removed) output plugin
	 * now.
	 */
	if (!fast_forward)
		LoadOutputPlugin(&ctx->callbacks, NameStr(slot->data.plugin));

	/*
	 * Now that the slot's xmin has been set, we can announce ourselves as a
	 * logical decoding backend which doesn't need to be checked individually
	 * when computing the xmin horizon because the xmin is enforced via
	 * replication slots.
	 *
	 * We can only do so if we're outside of a transaction (i.e. the case when
	 * streaming changes via walsender), otherwise an already setup
	 * snapshot/xid would end up being ignored. That's not a particularly
	 * bothersome restriction since the SQL interface can't be used for
	 * streaming anyway.
	 */
	if (!IsTransactionOrTransactionBlock())
	{
		LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
		MyProc->statusFlags |= PROC_IN_LOGICAL_DECODING;
		ProcGlobal->statusFlags[MyProc->pgxactoff] = MyProc->statusFlags;
		LWLockRelease(ProcArrayLock);
	}

	ctx->slot = slot;

	ctx->reader = XLogReaderAllocate(wal_segment_size, NULL, xl_routine, ctx);
	if (!ctx->reader)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed while allocating a WAL reading processor.")));

	ctx->reorder = ReorderBufferAllocate();
	ctx->snapshot_builder =
		AllocateSnapshotBuilder(ctx->reorder, xmin_horizon, start_lsn,
								need_full_snapshot, in_create, slot->data.two_phase_at);

	ctx->reorder->private_data = ctx;

	/* wrap output plugin callbacks, so we can add error context information */
	ctx->reorder->begin = begin_cb_wrapper;
	ctx->reorder->apply_change = change_cb_wrapper;
	ctx->reorder->apply_truncate = truncate_cb_wrapper;
	ctx->reorder->commit = commit_cb_wrapper;
	ctx->reorder->message = message_cb_wrapper;

	/*
	 * To support streaming, we require start/stop/abort/commit/change
	 * callbacks. The message and truncate callbacks are optional, similar to
	 * regular output plugins. We however enable streaming when at least one
	 * of the methods is enabled so that we can easily identify missing
	 * methods.
	 *
	 * We decide it here, but only check it later in the wrappers.
	 */
	ctx->streaming = (ctx->callbacks.stream_start_cb != NULL) ||
		(ctx->callbacks.stream_stop_cb != NULL) ||
		(ctx->callbacks.stream_abort_cb != NULL) ||
		(ctx->callbacks.stream_commit_cb != NULL) ||
		(ctx->callbacks.stream_change_cb != NULL) ||
		(ctx->callbacks.stream_message_cb != NULL) ||
		(ctx->callbacks.stream_truncate_cb != NULL);

	/*
	 * streaming callbacks
	 *
	 * stream_message and stream_truncate callbacks are optional, so we do not
	 * fail with ERROR when missing, but the wrappers simply do nothing. We
	 * must set the ReorderBuffer callbacks to something, otherwise the calls
	 * from there will crash (we don't want to move the checks there).
	 */
	ctx->reorder->stream_start = stream_start_cb_wrapper;
	ctx->reorder->stream_stop = stream_stop_cb_wrapper;
	ctx->reorder->stream_abort = stream_abort_cb_wrapper;
	ctx->reorder->stream_prepare = stream_prepare_cb_wrapper;
	ctx->reorder->stream_commit = stream_commit_cb_wrapper;
	ctx->reorder->stream_change = stream_change_cb_wrapper;
	ctx->reorder->stream_message = stream_message_cb_wrapper;
	ctx->reorder->stream_truncate = stream_truncate_cb_wrapper;


	/*
	 * To support two-phase logical decoding, we require
	 * begin_prepare/prepare/commit-prepare/abort-prepare callbacks. The
	 * filter_prepare callback is optional. We however enable two-phase
	 * logical decoding when at least one of the methods is enabled so that we
	 * can easily identify missing methods.
	 *
	 * We decide it here, but only check it later in the wrappers.
	 */
	ctx->twophase = (ctx->callbacks.begin_prepare_cb != NULL) ||
		(ctx->callbacks.prepare_cb != NULL) ||
		(ctx->callbacks.commit_prepared_cb != NULL) ||
		(ctx->callbacks.rollback_prepared_cb != NULL) ||
		(ctx->callbacks.stream_prepare_cb != NULL) ||
		(ctx->callbacks.filter_prepare_cb != NULL);

	/*
	 * Callback to support decoding at prepare time.
	 */
	ctx->reorder->begin_prepare = begin_prepare_cb_wrapper;
	ctx->reorder->prepare = prepare_cb_wrapper;
	ctx->reorder->commit_prepared = commit_prepared_cb_wrapper;
	ctx->reorder->rollback_prepared = rollback_prepared_cb_wrapper;

	/*
	 * Callback to support updating progress during sending data of a
	 * transaction (and its subtransactions) to the output plugin.
	 */
	ctx->reorder->update_progress_txn = update_progress_txn_cb_wrapper;

	ctx->out = makeStringInfo();
	ctx->prepare_write = prepare_write;
	ctx->write = do_write;
	ctx->update_progress = update_progress;

	ctx->output_plugin_options = output_plugin_options;

	ctx->fast_forward = fast_forward;

	MemoryContextSwitchTo(old_context);

	return ctx;
}

/*
 * Create a new decoding context, for a new logical slot.
 *
 * plugin -- contains the name of the output plugin
 * output_plugin_options -- contains options passed to the output plugin
 * need_full_snapshot -- if true, must obtain a snapshot able to read all
 *		tables; if false, one that can read only catalogs is acceptable.
 * restart_lsn -- if given as invalid, it's this routine's responsibility to
 *		mark WAL as reserved by setting a convenient restart_lsn for the slot.
 *		Otherwise, we set for decoding to start from the given LSN without
 *		marking WAL reserved beforehand.  In that scenario, it's up to the
 *		caller to guarantee that WAL remains available.
 * xl_routine -- XLogReaderRoutine for underlying XLogReader
 * prepare_write, do_write, update_progress --
 *		callbacks that perform the use-case dependent, actual, work.
 *
 * Needs to be called while in a memory context that's at least as long lived
 * as the decoding context because further memory contexts will be created
 * inside it.
 *
 * Returns an initialized decoding context after calling the output plugin's
 * startup function.
 */
LogicalDecodingContext *
CreateInitDecodingContext(const char *plugin,
						  List *output_plugin_options,
						  bool need_full_snapshot,
						  XLogRecPtr restart_lsn,
						  XLogReaderRoutine *xl_routine,
						  LogicalOutputPluginWriterPrepareWrite prepare_write,
						  LogicalOutputPluginWriterWrite do_write,
						  LogicalOutputPluginWriterUpdateProgress update_progress)
{
	TransactionId xmin_horizon = InvalidTransactionId;
	ReplicationSlot *slot;
	NameData	plugin_name;
	LogicalDecodingContext *ctx;
	MemoryContext old_context;

	/*
	 * On a standby, this check is also required while creating the slot.
	 * Check the comments in the function.
	 */
	CheckLogicalDecodingRequirements();

	/* shorter lines... */
	slot = MyReplicationSlot;

	/* first some sanity checks that are unlikely to be violated */
	if (slot == NULL)
		elog(ERROR, "cannot perform logical decoding without an acquired slot");

	if (plugin == NULL)
		elog(ERROR, "cannot initialize logical decoding without a specified plugin");

	/* Make sure the passed slot is suitable. These are user facing errors. */
	if (SlotIsPhysical(slot))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot use physical replication slot for logical decoding")));

	if (slot->data.database != MyDatabaseId)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("replication slot \"%s\" was not created in this database",
						NameStr(slot->data.name))));

	if (IsTransactionState() &&
		GetTopTransactionIdIfAny() != InvalidTransactionId)
		ereport(ERROR,
				(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
				 errmsg("cannot create logical replication slot in transaction that has performed writes")));

	/*
	 * Register output plugin name with slot.  We need the mutex to avoid
	 * concurrent reading of a partially copied string.  But we don't want any
	 * complicated code while holding a spinlock, so do namestrcpy() outside.
	 */
	namestrcpy(&plugin_name, plugin);
	SpinLockAcquire(&slot->mutex);
	slot->data.plugin = plugin_name;
	SpinLockRelease(&slot->mutex);

	if (XLogRecPtrIsInvalid(restart_lsn))
		ReplicationSlotReserveWal();
	else
	{
		SpinLockAcquire(&slot->mutex);
		slot->data.restart_lsn = restart_lsn;
		SpinLockRelease(&slot->mutex);
	}

	/* ----
	 * This is a bit tricky: We need to determine a safe xmin horizon to start
	 * decoding from, to avoid starting from a running xacts record referring
	 * to xids whose rows have been vacuumed or pruned
	 * already. GetOldestSafeDecodingTransactionId() returns such a value, but
	 * without further interlock its return value might immediately be out of
	 * date.
	 *
	 * So we have to acquire the ProcArrayLock to prevent computation of new
	 * xmin horizons by other backends, get the safe decoding xid, and inform
	 * the slot machinery about the new limit. Once that's done the
	 * ProcArrayLock can be released as the slot machinery now is
	 * protecting against vacuum.
	 *
	 * Note that, temporarily, the data, not just the catalog, xmin has to be
	 * reserved if a data snapshot is to be exported.  Otherwise the initial
	 * data snapshot created here is not guaranteed to be valid. After that
	 * the data xmin doesn't need to be managed anymore and the global xmin
	 * should be recomputed. As we are fine with losing the pegged data xmin
	 * after crash - no chance a snapshot would get exported anymore - we can
	 * get away with just setting the slot's
	 * effective_xmin. ReplicationSlotRelease will reset it again.
	 *
	 * ----
	 */
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

	xmin_horizon = GetOldestSafeDecodingTransactionId(!need_full_snapshot);

	SpinLockAcquire(&slot->mutex);
	slot->effective_catalog_xmin = xmin_horizon;
	slot->data.catalog_xmin = xmin_horizon;
	if (need_full_snapshot)
		slot->effective_xmin = xmin_horizon;
	SpinLockRelease(&slot->mutex);

	ReplicationSlotsComputeRequiredXmin(true);

	LWLockRelease(ProcArrayLock);

	ReplicationSlotMarkDirty();
	ReplicationSlotSave();

	ctx = StartupDecodingContext(NIL, restart_lsn, xmin_horizon,
								 need_full_snapshot, false, true,
								 xl_routine, prepare_write, do_write,
								 update_progress);

	/* call output plugin initialization callback */
	old_context = MemoryContextSwitchTo(ctx->context);
	if (ctx->callbacks.startup_cb != NULL)
		startup_cb_wrapper(ctx, &ctx->options, true);
	MemoryContextSwitchTo(old_context);

	/*
	 * We allow decoding of prepared transactions when the two_phase is
	 * enabled at the time of slot creation, or when the two_phase option is
	 * given at the streaming start, provided the plugin supports all the
	 * callbacks for two-phase.
	 */
	ctx->twophase &= slot->data.two_phase;

	ctx->reorder->output_rewrites = ctx->options.receive_rewrites;

	return ctx;
}

/*
 * Create a new decoding context, for a logical slot that has previously been
 * used already.
 *
 * start_lsn
 *		The LSN at which to start decoding.  If InvalidXLogRecPtr, restart
 *		from the slot's confirmed_flush; otherwise, start from the specified
 *		location (but move it forwards to confirmed_flush if it's older than
 *		that, see below).
 *
 * output_plugin_options
 *		options passed to the output plugin.
 *
 * fast_forward
 *		bypass the generation of logical changes.
 *
 * xl_routine
 *		XLogReaderRoutine used by underlying xlogreader
 *
 * prepare_write, do_write, update_progress
 *		callbacks that have to be filled to perform the use-case dependent,
 *		actual work.
 *
 * Needs to be called while in a memory context that's at least as long lived
 * as the decoding context because further memory contexts will be created
 * inside it.
 *
 * Returns an initialized decoding context after calling the output plugin's
 * startup function.
 */
LogicalDecodingContext *
CreateDecodingContext(XLogRecPtr start_lsn,
					  List *output_plugin_options,
					  bool fast_forward,
					  XLogReaderRoutine *xl_routine,
					  LogicalOutputPluginWriterPrepareWrite prepare_write,
					  LogicalOutputPluginWriterWrite do_write,
					  LogicalOutputPluginWriterUpdateProgress update_progress)
{
	LogicalDecodingContext *ctx;
	ReplicationSlot *slot;
	MemoryContext old_context;

	/* shorter lines... */
	slot = MyReplicationSlot;

	/* first some sanity checks that are unlikely to be violated */
	if (slot == NULL)
		elog(ERROR, "cannot perform logical decoding without an acquired slot");

	/* make sure the passed slot is suitable, these are user facing errors */
	if (SlotIsPhysical(slot))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot use physical replication slot for logical decoding")));

	/*
	 * We need to access the system tables during decoding to build the
	 * logical changes unless we are in fast_forward mode where no changes are
	 * generated.
	 */
	if (slot->data.database != MyDatabaseId && !fast_forward)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("replication slot \"%s\" was not created in this database",
						NameStr(slot->data.name))));

	/*
	 * The slots being synced from the primary can't be used for decoding as
	 * they are used after failover. However, we do allow advancing the LSNs
	 * during the synchronization of slots. See update_local_synced_slot.
	 */
	if (RecoveryInProgress() && slot->data.synced && !IsSyncingReplicationSlots())
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("cannot use replication slot \"%s\" for logical decoding",
					   NameStr(slot->data.name)),
				errdetail("This replication slot is being synchronized from the primary server."),
				errhint("Specify another replication slot."));

	/* slot must be valid to allow decoding */
	Assert(slot->data.invalidated == RS_INVAL_NONE);
	Assert(slot->data.restart_lsn != InvalidXLogRecPtr);

	if (start_lsn == InvalidXLogRecPtr)
	{
		/* continue from last position */
		start_lsn = slot->data.confirmed_flush;
	}
	else if (start_lsn < slot->data.confirmed_flush)
	{
		/*
		 * It might seem like we should error out in this case, but it's
		 * pretty common for a client to acknowledge a LSN it doesn't have to
		 * do anything for, and thus didn't store persistently, because the
		 * xlog records didn't result in anything relevant for logical
		 * decoding. Clients have to be able to do that to support synchronous
		 * replication.
		 *
		 * Starting at a different LSN than requested might not catch certain
		 * kinds of client errors; so the client may wish to check that
		 * confirmed_flush_lsn matches its expectations.
		 */
		elog(LOG, "%X/%08X has been already streamed, forwarding to %X/%08X",
			 LSN_FORMAT_ARGS(start_lsn),
			 LSN_FORMAT_ARGS(slot->data.confirmed_flush));

		start_lsn = slot->data.confirmed_flush;
	}

	ctx = StartupDecodingContext(output_plugin_options,
								 start_lsn, InvalidTransactionId, false,
								 fast_forward, false, xl_routine, prepare_write,
								 do_write, update_progress);

	/* call output plugin initialization callback */
	old_context = MemoryContextSwitchTo(ctx->context);
	if (ctx->callbacks.startup_cb != NULL)
		startup_cb_wrapper(ctx, &ctx->options, false);
	MemoryContextSwitchTo(old_context);

	/*
	 * We allow decoding of prepared transactions when the two_phase is
	 * enabled at the time of slot creation, or when the two_phase option is
	 * given at the streaming start, provided the plugin supports all the
	 * callbacks for two-phase.
	 */
	ctx->twophase &= (slot->data.two_phase || ctx->twophase_opt_given);

	/* Mark slot to allow two_phase decoding if not already marked */
	if (ctx->twophase && !slot->data.two_phase)
	{
		SpinLockAcquire(&slot->mutex);
		slot->data.two_phase = true;
		slot->data.two_phase_at = start_lsn;
		SpinLockRelease(&slot->mutex);
		ReplicationSlotMarkDirty();
		ReplicationSlotSave();
		SnapBuildSetTwoPhaseAt(ctx->snapshot_builder, start_lsn);
	}

	ctx->reorder->output_rewrites = ctx->options.receive_rewrites;

	ereport(LOG,
			(errmsg("starting logical decoding for slot \"%s\"",
					NameStr(slot->data.name)),
			 errdetail("Streaming transactions committing after %X/%08X, reading WAL from %X/%08X.",
					   LSN_FORMAT_ARGS(slot->data.confirmed_flush),
					   LSN_FORMAT_ARGS(slot->data.restart_lsn))));

	return ctx;
}

/*
 * Returns true if a consistent initial decoding snapshot has been built.
 */
bool
DecodingContextReady(LogicalDecodingContext *ctx)
{
	return SnapBuildCurrentState(ctx->snapshot_builder) == SNAPBUILD_CONSISTENT;
}

/*
 * Read from the decoding slot, until it is ready to start extracting changes.
 */
void
DecodingContextFindStartpoint(LogicalDecodingContext *ctx)
{
	ReplicationSlot *slot = ctx->slot;

	/* Initialize from where to start reading WAL. */
	XLogBeginRead(ctx->reader, slot->data.restart_lsn);

	elog(DEBUG1, "searching for logical decoding starting point, starting at %X/%08X",
		 LSN_FORMAT_ARGS(slot->data.restart_lsn));

	/* Wait for a consistent starting point */
	for (;;)
	{
		XLogRecord *record;
		char	   *err = NULL;

		/* the read_page callback waits for new WAL */
		record = XLogReadRecord(ctx->reader, &err);
		if (err)
			elog(ERROR, "could not find logical decoding starting point: %s", err);
		if (!record)
			elog(ERROR, "could not find logical decoding starting point");

		LogicalDecodingProcessRecord(ctx, ctx->reader);

		/* only continue till we found a consistent spot */
		if (DecodingContextReady(ctx))
			break;

		CHECK_FOR_INTERRUPTS();
	}

	SpinLockAcquire(&slot->mutex);
	slot->data.confirmed_flush = ctx->reader->EndRecPtr;
	if (slot->data.two_phase)
		slot->data.two_phase_at = ctx->reader->EndRecPtr;
	SpinLockRelease(&slot->mutex);
}

/*
 * Free a previously allocated decoding context, invoking the shutdown
 * callback if necessary.
 */
void
FreeDecodingContext(LogicalDecodingContext *ctx)
{
	if (ctx->callbacks.shutdown_cb != NULL)
		shutdown_cb_wrapper(ctx);

	ReorderBufferFree(ctx->reorder);
	FreeSnapshotBuilder(ctx->snapshot_builder);
	XLogReaderFree(ctx->reader);
	MemoryContextDelete(ctx->context);
}

/*
 * Prepare a write using the context's output routine.
 */
void
OutputPluginPrepareWrite(struct LogicalDecodingContext *ctx, bool last_write)
{
	if (!ctx->accept_writes)
		elog(ERROR, "writes are only accepted in commit, begin and change callbacks");

	ctx->prepare_write(ctx, ctx->write_location, ctx->write_xid, last_write);
	ctx->prepared_write = true;
}

/*
 * Perform a write using the context's output routine.
 */
void
OutputPluginWrite(struct LogicalDecodingContext *ctx, bool last_write)
{
	if (!ctx->prepared_write)
		elog(ERROR, "OutputPluginPrepareWrite needs to be called before OutputPluginWrite");

	ctx->write(ctx, ctx->write_location, ctx->write_xid, last_write);
	ctx->prepared_write = false;
}

/*
 * Update progress tracking (if supported).
 */
void
OutputPluginUpdateProgress(struct LogicalDecodingContext *ctx,
						   bool skipped_xact)
{
	if (!ctx->update_progress)
		return;

	ctx->update_progress(ctx, ctx->write_location, ctx->write_xid,
						 skipped_xact);
}

/*
 * Load the output plugin, lookup its output plugin init function, and check
 * that it provides the required callbacks.
 */
static void
LoadOutputPlugin(OutputPluginCallbacks *callbacks, const char *plugin)
{
	LogicalOutputPluginInit plugin_init;

	plugin_init = (LogicalOutputPluginInit)
		load_external_function(plugin, "_PG_output_plugin_init", false, NULL);

	if (plugin_init == NULL)
		elog(ERROR, "output plugins have to declare the _PG_output_plugin_init symbol");

	/* ask the output plugin to fill the callback struct */
	plugin_init(callbacks);

	if (callbacks->begin_cb == NULL)
		elog(ERROR, "output plugins have to register a begin callback");
	if (callbacks->change_cb == NULL)
		elog(ERROR, "output plugins have to register a change callback");
	if (callbacks->commit_cb == NULL)
		elog(ERROR, "output plugins have to register a commit callback");
}

static void
output_plugin_error_callback(void *arg)
{
	LogicalErrorCallbackState *state = (LogicalErrorCallbackState *) arg;

	/* not all callbacks have an associated LSN  */
	if (state->report_location != InvalidXLogRecPtr)
		errcontext("slot \"%s\", output plugin \"%s\", in the %s callback, associated LSN %X/%08X",
				   NameStr(state->ctx->slot->data.name),
				   NameStr(state->ctx->slot->data.plugin),
				   state->callback_name,
				   LSN_FORMAT_ARGS(state->report_location));
	else
		errcontext("slot \"%s\", output plugin \"%s\", in the %s callback",
				   NameStr(state->ctx->slot->data.name),
				   NameStr(state->ctx->slot->data.plugin),
				   state->callback_name);
}

static void
startup_cb_wrapper(LogicalDecodingContext *ctx, OutputPluginOptions *opt, bool is_init)
{
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* Push callback + info on the error context stack */
	state.ctx = ctx;
	state.callback_name = "startup";
	state.report_location = InvalidXLogRecPtr;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state */
	ctx->accept_writes = false;
	ctx->end_xact = false;

	/* do the actual work: call callback */
	ctx->callbacks.startup_cb(ctx, opt, is_init);

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;
}

static void
shutdown_cb_wrapper(LogicalDecodingContext *ctx)
{
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* Push callback + info on the error context stack */
	state.ctx = ctx;
	state.callback_name = "shutdown";
	state.report_location = InvalidXLogRecPtr;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state */
	ctx->accept_writes = false;
	ctx->end_xact = false;

	/* do the actual work: call callback */
	ctx->callbacks.shutdown_cb(ctx);

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;
}


/*
 * Callbacks for ReorderBuffer which add in some more information and then call
 * output_plugin.h plugins.
 */
static void
begin_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* Push callback + info on the error context stack */
	state.ctx = ctx;
	state.callback_name = "begin";
	state.report_location = txn->first_lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;
	ctx->write_location = txn->first_lsn;
	ctx->end_xact = false;

	/* do the actual work: call callback */
	ctx->callbacks.begin_cb(ctx, txn);

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;
}

static void
commit_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
				  XLogRecPtr commit_lsn)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* Push callback + info on the error context stack */
	state.ctx = ctx;
	state.callback_name = "commit";
	state.report_location = txn->final_lsn; /* beginning of commit record */
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;
	ctx->write_location = txn->end_lsn; /* points to the end of the record */
	ctx->end_xact = true;

	/* do the actual work: call callback */
	ctx->callbacks.commit_cb(ctx, txn, commit_lsn);

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;
}

/*
 * The functionality of begin_prepare is quite similar to begin with the
 * exception that this will have gid (global transaction id) information which
 * can be used by plugin. Now, we thought about extending the existing begin
 * but that would break the replication protocol and additionally this looks
 * cleaner.
 */
static void
begin_prepare_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* We're only supposed to call this when two-phase commits are supported */
	Assert(ctx->twophase);

	/* Push callback + info on the error context stack */
	state.ctx = ctx;
	state.callback_name = "begin_prepare";
	state.report_location = txn->first_lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;
	ctx->write_location = txn->first_lsn;
	ctx->end_xact = false;

	/*
	 * If the plugin supports two-phase commits then begin prepare callback is
	 * mandatory
	 */
	if (ctx->callbacks.begin_prepare_cb == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical replication at prepare time requires a %s callback",
						"begin_prepare_cb")));

	/* do the actual work: call callback */
	ctx->callbacks.begin_prepare_cb(ctx, txn);

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;
}

static void
prepare_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
				   XLogRecPtr prepare_lsn)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* We're only supposed to call this when two-phase commits are supported */
	Assert(ctx->twophase);

	/* Push callback + info on the error context stack */
	state.ctx = ctx;
	state.callback_name = "prepare";
	state.report_location = txn->final_lsn; /* beginning of prepare record */
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;
	ctx->write_location = txn->end_lsn; /* points to the end of the record */
	ctx->end_xact = true;

	/*
	 * If the plugin supports two-phase commits then prepare callback is
	 * mandatory
	 */
	if (ctx->callbacks.prepare_cb == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical replication at prepare time requires a %s callback",
						"prepare_cb")));

	/* do the actual work: call callback */
	ctx->callbacks.prepare_cb(ctx, txn, prepare_lsn);

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;
}

static void
commit_prepared_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
						   XLogRecPtr commit_lsn)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* We're only supposed to call this when two-phase commits are supported */
	Assert(ctx->twophase);

	/* Push callback + info on the error context stack */
	state.ctx = ctx;
	state.callback_name = "commit_prepared";
	state.report_location = txn->final_lsn; /* beginning of commit record */
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;
	ctx->write_location = txn->end_lsn; /* points to the end of the record */
	ctx->end_xact = true;

	/*
	 * If the plugin support two-phase commits then commit prepared callback
	 * is mandatory
	 */
	if (ctx->callbacks.commit_prepared_cb == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical replication at prepare time requires a %s callback",
						"commit_prepared_cb")));

	/* do the actual work: call callback */
	ctx->callbacks.commit_prepared_cb(ctx, txn, commit_lsn);

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;
}

static void
rollback_prepared_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
							 XLogRecPtr prepare_end_lsn,
							 TimestampTz prepare_time)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* We're only supposed to call this when two-phase commits are supported */
	Assert(ctx->twophase);

	/* Push callback + info on the error context stack */
	state.ctx = ctx;
	state.callback_name = "rollback_prepared";
	state.report_location = txn->final_lsn; /* beginning of commit record */
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;
	ctx->write_location = txn->end_lsn; /* points to the end of the record */
	ctx->end_xact = true;

	/*
	 * If the plugin support two-phase commits then rollback prepared callback
	 * is mandatory
	 */
	if (ctx->callbacks.rollback_prepared_cb == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical replication at prepare time requires a %s callback",
						"rollback_prepared_cb")));

	/* do the actual work: call callback */
	ctx->callbacks.rollback_prepared_cb(ctx, txn, prepare_end_lsn,
										prepare_time);

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;
}

static void
change_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
				  Relation relation, ReorderBufferChange *change)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* Push callback + info on the error context stack */
	state.ctx = ctx;
	state.callback_name = "change";
	state.report_location = change->lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;

	/*
	 * Report this change's lsn so replies from clients can give an up-to-date
	 * answer. This won't ever be enough (and shouldn't be!) to confirm
	 * receipt of this transaction, but it might allow another transaction's
	 * commit to be confirmed with one message.
	 */
	ctx->write_location = change->lsn;

	ctx->end_xact = false;

	ctx->callbacks.change_cb(ctx, txn, relation, change);

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;
}

static void
truncate_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
					int nrelations, Relation relations[], ReorderBufferChange *change)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	if (!ctx->callbacks.truncate_cb)
		return;

	/* Push callback + info on the error context stack */
	state.ctx = ctx;
	state.callback_name = "truncate";
	state.report_location = change->lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;

	/*
	 * Report this change's lsn so replies from clients can give an up-to-date
	 * answer. This won't ever be enough (and shouldn't be!) to confirm
	 * receipt of this transaction, but it might allow another transaction's
	 * commit to be confirmed with one message.
	 */
	ctx->write_location = change->lsn;

	ctx->end_xact = false;

	ctx->callbacks.truncate_cb(ctx, txn, nrelations, relations, change);

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;
}

bool
filter_prepare_cb_wrapper(LogicalDecodingContext *ctx, TransactionId xid,
						  const char *gid)
{
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;
	bool		ret;

	Assert(!ctx->fast_forward);

	/* Push callback + info on the error context stack */
	state.ctx = ctx;
	state.callback_name = "filter_prepare";
	state.report_location = InvalidXLogRecPtr;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state */
	ctx->accept_writes = false;
	ctx->end_xact = false;

	/* do the actual work: call callback */
	ret = ctx->callbacks.filter_prepare_cb(ctx, xid, gid);

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;

	return ret;
}

bool
filter_by_origin_cb_wrapper(LogicalDecodingContext *ctx, RepOriginId origin_id)
{
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;
	bool		ret;

	Assert(!ctx->fast_forward);

	/* Push callback + info on the error context stack */
	state.ctx = ctx;
	state.callback_name = "filter_by_origin";
	state.report_location = InvalidXLogRecPtr;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state */
	ctx->accept_writes = false;
	ctx->end_xact = false;

	/* do the actual work: call callback */
	ret = ctx->callbacks.filter_by_origin_cb(ctx, origin_id);

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;

	return ret;
}

static void
message_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
				   XLogRecPtr message_lsn, bool transactional,
				   const char *prefix, Size message_size, const char *message)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	if (ctx->callbacks.message_cb == NULL)
		return;

	/* Push callback + info on the error context stack */
	state.ctx = ctx;
	state.callback_name = "message";
	state.report_location = message_lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state */
	ctx->accept_writes = true;
	ctx->write_xid = txn != NULL ? txn->xid : InvalidTransactionId;
	ctx->write_location = message_lsn;
	ctx->end_xact = false;

	/* do the actual work: call callback */
	ctx->callbacks.message_cb(ctx, txn, message_lsn, transactional, prefix,
							  message_size, message);

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;
}

static void
stream_start_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
						XLogRecPtr first_lsn)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* We're only supposed to call this when streaming is supported. */
	Assert(ctx->streaming);

	/* Push callback + info on the error context stack */
	state.ctx = ctx;
	state.callback_name = "stream_start";
	state.report_location = first_lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;

	/*
	 * Report this message's lsn so replies from clients can give an
	 * up-to-date answer. This won't ever be enough (and shouldn't be!) to
	 * confirm receipt of this transaction, but it might allow another
	 * transaction's commit to be confirmed with one message.
	 */
	ctx->write_location = first_lsn;

	ctx->end_xact = false;

	/* in streaming mode, stream_start_cb is required */
	if (ctx->callbacks.stream_start_cb == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical streaming requires a %s callback",
						"stream_start_cb")));

	ctx->callbacks.stream_start_cb(ctx, txn);

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;
}

static void
stream_stop_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
					   XLogRecPtr last_lsn)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* We're only supposed to call this when streaming is supported. */
	Assert(ctx->streaming);

	/* Push callback + info on the error context stack */
	state.ctx = ctx;
	state.callback_name = "stream_stop";
	state.report_location = last_lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;

	/*
	 * Report this message's lsn so replies from clients can give an
	 * up-to-date answer. This won't ever be enough (and shouldn't be!) to
	 * confirm receipt of this transaction, but it might allow another
	 * transaction's commit to be confirmed with one message.
	 */
	ctx->write_location = last_lsn;

	ctx->end_xact = false;

	/* in streaming mode, stream_stop_cb is required */
	if (ctx->callbacks.stream_stop_cb == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical streaming requires a %s callback",
						"stream_stop_cb")));

	ctx->callbacks.stream_stop_cb(ctx, txn);

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;
}

static void
stream_abort_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
						XLogRecPtr abort_lsn)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* We're only supposed to call this when streaming is supported. */
	Assert(ctx->streaming);

	/* Push callback + info on the error context stack */
	state.ctx = ctx;
	state.callback_name = "stream_abort";
	state.report_location = abort_lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;
	ctx->write_location = abort_lsn;
	ctx->end_xact = true;

	/* in streaming mode, stream_abort_cb is required */
	if (ctx->callbacks.stream_abort_cb == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical streaming requires a %s callback",
						"stream_abort_cb")));

	ctx->callbacks.stream_abort_cb(ctx, txn, abort_lsn);

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;
}

static void
stream_prepare_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
						  XLogRecPtr prepare_lsn)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/*
	 * We're only supposed to call this when streaming and two-phase commits
	 * are supported.
	 */
	Assert(ctx->streaming);
	Assert(ctx->twophase);

	/* Push callback + info on the error context stack */
	state.ctx = ctx;
	state.callback_name = "stream_prepare";
	state.report_location = txn->final_lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;
	ctx->write_location = txn->end_lsn;
	ctx->end_xact = true;

	/* in streaming mode with two-phase commits, stream_prepare_cb is required */
	if (ctx->callbacks.stream_prepare_cb == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical streaming at prepare time requires a %s callback",
						"stream_prepare_cb")));

	ctx->callbacks.stream_prepare_cb(ctx, txn, prepare_lsn);

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;
}

static void
stream_commit_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
						 XLogRecPtr commit_lsn)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* We're only supposed to call this when streaming is supported. */
	Assert(ctx->streaming);

	/* Push callback + info on the error context stack */
	state.ctx = ctx;
	state.callback_name = "stream_commit";
	state.report_location = txn->final_lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;
	ctx->write_location = txn->end_lsn;
	ctx->end_xact = true;

	/* in streaming mode, stream_commit_cb is required */
	if (ctx->callbacks.stream_commit_cb == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical streaming requires a %s callback",
						"stream_commit_cb")));

	ctx->callbacks.stream_commit_cb(ctx, txn, commit_lsn);

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;
}

static void
stream_change_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
						 Relation relation, ReorderBufferChange *change)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* We're only supposed to call this when streaming is supported. */
	Assert(ctx->streaming);

	/* Push callback + info on the error context stack */
	state.ctx = ctx;
	state.callback_name = "stream_change";
	state.report_location = change->lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;

	/*
	 * Report this change's lsn so replies from clients can give an up-to-date
	 * answer. This won't ever be enough (and shouldn't be!) to confirm
	 * receipt of this transaction, but it might allow another transaction's
	 * commit to be confirmed with one message.
	 */
	ctx->write_location = change->lsn;

	ctx->end_xact = false;

	/* in streaming mode, stream_change_cb is required */
	if (ctx->callbacks.stream_change_cb == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical streaming requires a %s callback",
						"stream_change_cb")));

	ctx->callbacks.stream_change_cb(ctx, txn, relation, change);

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;
}

static void
stream_message_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
						  XLogRecPtr message_lsn, bool transactional,
						  const char *prefix, Size message_size, const char *message)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* We're only supposed to call this when streaming is supported. */
	Assert(ctx->streaming);

	/* this callback is optional */
	if (ctx->callbacks.stream_message_cb == NULL)
		return;

	/* Push callback + info on the error context stack */
	state.ctx = ctx;
	state.callback_name = "stream_message";
	state.report_location = message_lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state */
	ctx->accept_writes = true;
	ctx->write_xid = txn != NULL ? txn->xid : InvalidTransactionId;
	ctx->write_location = message_lsn;
	ctx->end_xact = false;

	/* do the actual work: call callback */
	ctx->callbacks.stream_message_cb(ctx, txn, message_lsn, transactional, prefix,
									 message_size, message);

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;
}

static void
stream_truncate_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
						   int nrelations, Relation relations[],
						   ReorderBufferChange *change)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* We're only supposed to call this when streaming is supported. */
	Assert(ctx->streaming);

	/* this callback is optional */
	if (!ctx->callbacks.stream_truncate_cb)
		return;

	/* Push callback + info on the error context stack */
	state.ctx = ctx;
	state.callback_name = "stream_truncate";
	state.report_location = change->lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;

	/*
	 * Report this change's lsn so replies from clients can give an up-to-date
	 * answer. This won't ever be enough (and shouldn't be!) to confirm
	 * receipt of this transaction, but it might allow another transaction's
	 * commit to be confirmed with one message.
	 */
	ctx->write_location = change->lsn;

	ctx->end_xact = false;

	ctx->callbacks.stream_truncate_cb(ctx, txn, nrelations, relations, change);

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;
}

static void
update_progress_txn_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
							   XLogRecPtr lsn)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* Push callback + info on the error context stack */
	state.ctx = ctx;
	state.callback_name = "update_progress_txn";
	state.report_location = lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state */
	ctx->accept_writes = false;
	ctx->write_xid = txn->xid;

	/*
	 * Report this change's lsn so replies from clients can give an up-to-date
	 * answer. This won't ever be enough (and shouldn't be!) to confirm
	 * receipt of this transaction, but it might allow another transaction's
	 * commit to be confirmed with one message.
	 */
	ctx->write_location = lsn;

	ctx->end_xact = false;

	OutputPluginUpdateProgress(ctx, false);

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;
}

/*
 * Set the required catalog xmin horizon for historic snapshots in the current
 * replication slot.
 *
 * Note that in the most cases, we won't be able to immediately use the xmin
 * to increase the xmin horizon: we need to wait till the client has confirmed
 * receiving current_lsn with LogicalConfirmReceivedLocation().
 */
void
LogicalIncreaseXminForSlot(XLogRecPtr current_lsn, TransactionId xmin)
{
	bool		updated_xmin = false;
	ReplicationSlot *slot;
	bool		got_new_xmin = false;

	slot = MyReplicationSlot;

	Assert(slot != NULL);

	SpinLockAcquire(&slot->mutex);

	/*
	 * don't overwrite if we already have a newer xmin. This can happen if we
	 * restart decoding in a slot.
	 */
	if (TransactionIdPrecedesOrEquals(xmin, slot->data.catalog_xmin))
	{
	}

	/*
	 * If the client has already confirmed up to this lsn, we directly can
	 * mark this as accepted. This can happen if we restart decoding in a
	 * slot.
	 */
	else if (current_lsn <= slot->data.confirmed_flush)
	{
		slot->candidate_catalog_xmin = xmin;
		slot->candidate_xmin_lsn = current_lsn;

		/* our candidate can directly be used */
		updated_xmin = true;
	}

	/*
	 * Only increase if the previous values have been applied, otherwise we
	 * might never end up updating if the receiver acks too slowly.
	 */
	else if (slot->candidate_xmin_lsn == InvalidXLogRecPtr)
	{
		slot->candidate_catalog_xmin = xmin;
		slot->candidate_xmin_lsn = current_lsn;

		/*
		 * Log new xmin at an appropriate log level after releasing the
		 * spinlock.
		 */
		got_new_xmin = true;
	}
	SpinLockRelease(&slot->mutex);

	if (got_new_xmin)
		elog(DEBUG1, "got new catalog xmin %" PRIu64 " at %X/%08X",
			 xmin, LSN_FORMAT_ARGS(current_lsn));

	/* candidate already valid with the current flush position, apply */
	if (updated_xmin)
		LogicalConfirmReceivedLocation(slot->data.confirmed_flush);
}

/*
 * Mark the minimal LSN (restart_lsn) we need to read to replay all
 * transactions that have not yet committed at current_lsn.
 *
 * Just like LogicalIncreaseXminForSlot this only takes effect when the
 * client has confirmed to have received current_lsn.
 */
void
LogicalIncreaseRestartDecodingForSlot(XLogRecPtr current_lsn, XLogRecPtr restart_lsn)
{
	bool		updated_lsn = false;
	ReplicationSlot *slot;

	slot = MyReplicationSlot;

	Assert(slot != NULL);
	Assert(restart_lsn != InvalidXLogRecPtr);
	Assert(current_lsn != InvalidXLogRecPtr);

	SpinLockAcquire(&slot->mutex);

	/* don't overwrite if have a newer restart lsn */
	if (restart_lsn <= slot->data.restart_lsn)
	{
		SpinLockRelease(&slot->mutex);
	}

	/*
	 * We might have already flushed far enough to directly accept this lsn,
	 * in this case there is no need to check for existing candidate LSNs
	 */
	else if (current_lsn <= slot->data.confirmed_flush)
	{
		slot->candidate_restart_valid = current_lsn;
		slot->candidate_restart_lsn = restart_lsn;
		SpinLockRelease(&slot->mutex);

		/* our candidate can directly be used */
		updated_lsn = true;
	}

	/*
	 * Only increase if the previous values have been applied, otherwise we
	 * might never end up updating if the receiver acks too slowly. A missed
	 * value here will just cause some extra effort after reconnecting.
	 */
	else if (slot->candidate_restart_valid == InvalidXLogRecPtr)
	{
		slot->candidate_restart_valid = current_lsn;
		slot->candidate_restart_lsn = restart_lsn;
		SpinLockRelease(&slot->mutex);

		elog(DEBUG1, "got new restart lsn %X/%08X at %X/%08X",
			 LSN_FORMAT_ARGS(restart_lsn),
			 LSN_FORMAT_ARGS(current_lsn));
	}
	else
	{
		XLogRecPtr	candidate_restart_lsn;
		XLogRecPtr	candidate_restart_valid;
		XLogRecPtr	confirmed_flush;

		candidate_restart_lsn = slot->candidate_restart_lsn;
		candidate_restart_valid = slot->candidate_restart_valid;
		confirmed_flush = slot->data.confirmed_flush;
		SpinLockRelease(&slot->mutex);

		elog(DEBUG1, "failed to increase restart lsn: proposed %X/%08X, after %X/%08X, current candidate %X/%08X, current after %X/%08X, flushed up to %X/%08X",
			 LSN_FORMAT_ARGS(restart_lsn),
			 LSN_FORMAT_ARGS(current_lsn),
			 LSN_FORMAT_ARGS(candidate_restart_lsn),
			 LSN_FORMAT_ARGS(candidate_restart_valid),
			 LSN_FORMAT_ARGS(confirmed_flush));
	}

	/* candidates are already valid with the current flush position, apply */
	if (updated_lsn)
		LogicalConfirmReceivedLocation(slot->data.confirmed_flush);
}

/*
 * Handle a consumer's confirmation having received all changes up to lsn.
 */
void
LogicalConfirmReceivedLocation(XLogRecPtr lsn)
{
	Assert(lsn != InvalidXLogRecPtr);

	/* Do an unlocked check for candidate_lsn first. */
	if (MyReplicationSlot->candidate_xmin_lsn != InvalidXLogRecPtr ||
		MyReplicationSlot->candidate_restart_valid != InvalidXLogRecPtr)
	{
		bool		updated_xmin = false;
		bool		updated_restart = false;
		XLogRecPtr	restart_lsn pg_attribute_unused();

		SpinLockAcquire(&MyReplicationSlot->mutex);

		/* remember the old restart lsn */
		restart_lsn = MyReplicationSlot->data.restart_lsn;

		/*
		 * Prevent moving the confirmed_flush backwards, as this could lead to
		 * data duplication issues caused by replicating already replicated
		 * changes.
		 *
		 * This can happen when a client acknowledges an LSN it doesn't have
		 * to do anything for, and thus didn't store persistently. After a
		 * restart, the client can send the prior LSN that it stored
		 * persistently as an acknowledgement, but we need to ignore such an
		 * LSN. See similar case handling in CreateDecodingContext.
		 */
		if (lsn > MyReplicationSlot->data.confirmed_flush)
			MyReplicationSlot->data.confirmed_flush = lsn;

		/* if we're past the location required for bumping xmin, do so */
		if (MyReplicationSlot->candidate_xmin_lsn != InvalidXLogRecPtr &&
			MyReplicationSlot->candidate_xmin_lsn <= lsn)
		{
			/*
			 * We have to write the changed xmin to disk *before* we change
			 * the in-memory value, otherwise after a crash we wouldn't know
			 * that some catalog tuples might have been removed already.
			 *
			 * Ensure that by first writing to ->xmin and only update
			 * ->effective_xmin once the new state is synced to disk. After a
			 * crash ->effective_xmin is set to ->xmin.
			 */
			if (TransactionIdIsValid(MyReplicationSlot->candidate_catalog_xmin) &&
				MyReplicationSlot->data.catalog_xmin != MyReplicationSlot->candidate_catalog_xmin)
			{
				MyReplicationSlot->data.catalog_xmin = MyReplicationSlot->candidate_catalog_xmin;
				MyReplicationSlot->candidate_catalog_xmin = InvalidTransactionId;
				MyReplicationSlot->candidate_xmin_lsn = InvalidXLogRecPtr;
				updated_xmin = true;
			}
		}

		if (MyReplicationSlot->candidate_restart_valid != InvalidXLogRecPtr &&
			MyReplicationSlot->candidate_restart_valid <= lsn)
		{
			Assert(MyReplicationSlot->candidate_restart_lsn != InvalidXLogRecPtr);

			MyReplicationSlot->data.restart_lsn = MyReplicationSlot->candidate_restart_lsn;
			MyReplicationSlot->candidate_restart_lsn = InvalidXLogRecPtr;
			MyReplicationSlot->candidate_restart_valid = InvalidXLogRecPtr;
			updated_restart = true;
		}

		SpinLockRelease(&MyReplicationSlot->mutex);

		/* first write new xmin to disk, so we know what's up after a crash */
		if (updated_xmin || updated_restart)
		{
#ifdef USE_INJECTION_POINTS
			XLogSegNo	seg1,
						seg2;

			XLByteToSeg(restart_lsn, seg1, wal_segment_size);
			XLByteToSeg(MyReplicationSlot->data.restart_lsn, seg2, wal_segment_size);

			/* trigger injection point, but only if segment changes */
			if (seg1 != seg2)
				INJECTION_POINT("logical-replication-slot-advance-segment", NULL);
#endif

			ReplicationSlotMarkDirty();
			ReplicationSlotSave();
			elog(DEBUG1, "updated xmin: %u restart: %u", updated_xmin, updated_restart);
		}

		/*
		 * Now the new xmin is safely on disk, we can let the global value
		 * advance. We do not take ProcArrayLock or similar since we only
		 * advance xmin here and there's not much harm done by a concurrent
		 * computation missing that.
		 */
		if (updated_xmin)
		{
			SpinLockAcquire(&MyReplicationSlot->mutex);
			MyReplicationSlot->effective_catalog_xmin = MyReplicationSlot->data.catalog_xmin;
			SpinLockRelease(&MyReplicationSlot->mutex);

			ReplicationSlotsComputeRequiredXmin(false);
			ReplicationSlotsComputeRequiredLSN();
		}
	}
	else
	{
		SpinLockAcquire(&MyReplicationSlot->mutex);

		/*
		 * Prevent moving the confirmed_flush backwards. See comments above
		 * for the details.
		 */
		if (lsn > MyReplicationSlot->data.confirmed_flush)
			MyReplicationSlot->data.confirmed_flush = lsn;

		SpinLockRelease(&MyReplicationSlot->mutex);
	}
}

/*
 * Clear logical streaming state during (sub)transaction abort.
 */
void
ResetLogicalStreamingState(void)
{
	CheckXidAlive = InvalidTransactionId;
	bsysscan = false;
}

/*
 * Report stats for a slot.
 */
void
UpdateDecodingStats(LogicalDecodingContext *ctx)
{
	ReorderBuffer *rb = ctx->reorder;
	PgStat_StatReplSlotEntry repSlotStat;

	/* Nothing to do if we don't have any replication stats to be sent. */
	if (rb->spillBytes <= 0 && rb->streamBytes <= 0 && rb->totalBytes <= 0)
		return;

	elog(DEBUG2, "UpdateDecodingStats: updating stats %p %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64,
		 rb,
		 rb->spillTxns,
		 rb->spillCount,
		 rb->spillBytes,
		 rb->streamTxns,
		 rb->streamCount,
		 rb->streamBytes,
		 rb->totalTxns,
		 rb->totalBytes);

	repSlotStat.spill_txns = rb->spillTxns;
	repSlotStat.spill_count = rb->spillCount;
	repSlotStat.spill_bytes = rb->spillBytes;
	repSlotStat.stream_txns = rb->streamTxns;
	repSlotStat.stream_count = rb->streamCount;
	repSlotStat.stream_bytes = rb->streamBytes;
	repSlotStat.total_txns = rb->totalTxns;
	repSlotStat.total_bytes = rb->totalBytes;

	pgstat_report_replslot(ctx->slot, &repSlotStat);

	rb->spillTxns = 0;
	rb->spillCount = 0;
	rb->spillBytes = 0;
	rb->streamTxns = 0;
	rb->streamCount = 0;
	rb->streamBytes = 0;
	rb->totalTxns = 0;
	rb->totalBytes = 0;
}

/*
 * Read up to the end of WAL starting from the decoding slot's restart_lsn.
 * Return true if any meaningful/decodable WAL records are encountered,
 * otherwise false.
 */
bool
LogicalReplicationSlotHasPendingWal(XLogRecPtr end_of_wal)
{
	bool		has_pending_wal = false;

	Assert(MyReplicationSlot);

	PG_TRY();
	{
		LogicalDecodingContext *ctx;

		/*
		 * Create our decoding context in fast_forward mode, passing start_lsn
		 * as InvalidXLogRecPtr, so that we start processing from the slot's
		 * confirmed_flush.
		 */
		ctx = CreateDecodingContext(InvalidXLogRecPtr,
									NIL,
									true,	/* fast_forward */
									XL_ROUTINE(.page_read = read_local_xlog_page,
											   .segment_open = wal_segment_open,
											   .segment_close = wal_segment_close),
									NULL, NULL, NULL);

		/*
		 * Start reading at the slot's restart_lsn, which we know points to a
		 * valid record.
		 */
		XLogBeginRead(ctx->reader, MyReplicationSlot->data.restart_lsn);

		/* Invalidate non-timetravel entries */
		InvalidateSystemCaches();

		/* Loop until the end of WAL or some changes are processed */
		while (!has_pending_wal && ctx->reader->EndRecPtr < end_of_wal)
		{
			XLogRecord *record;
			char	   *errm = NULL;

			record = XLogReadRecord(ctx->reader, &errm);

			if (errm)
				elog(ERROR, "could not find record for logical decoding: %s", errm);

			if (record != NULL)
				LogicalDecodingProcessRecord(ctx, ctx->reader);

			has_pending_wal = ctx->processing_required;

			CHECK_FOR_INTERRUPTS();
		}

		/* Clean up */
		FreeDecodingContext(ctx);
		InvalidateSystemCaches();
	}
	PG_CATCH();
	{
		/* clear all timetravel entries */
		InvalidateSystemCaches();

		PG_RE_THROW();
	}
	PG_END_TRY();

	return has_pending_wal;
}

/*
 * Helper function for advancing our logical replication slot forward.
 *
 * The slot's restart_lsn is used as start point for reading records, while
 * confirmed_flush is used as base point for the decoding context.
 *
 * We cannot just do LogicalConfirmReceivedLocation to update confirmed_flush,
 * because we need to digest WAL to advance restart_lsn allowing to recycle
 * WAL and removal of old catalog tuples.  As decoding is done in fast_forward
 * mode, no changes are generated anyway.
 *
 * *found_consistent_snapshot will be true if the initial decoding snapshot has
 * been built; Otherwise, it will be false.
 */
XLogRecPtr
LogicalSlotAdvanceAndCheckSnapState(XLogRecPtr moveto,
									bool *found_consistent_snapshot)
{
	LogicalDecodingContext *ctx;
	ResourceOwner old_resowner = CurrentResourceOwner;
	XLogRecPtr	retlsn;

	Assert(moveto != InvalidXLogRecPtr);

	if (found_consistent_snapshot)
		*found_consistent_snapshot = false;

	PG_TRY();
	{
		/*
		 * Create our decoding context in fast_forward mode, passing start_lsn
		 * as InvalidXLogRecPtr, so that we start processing from my slot's
		 * confirmed_flush.
		 */
		ctx = CreateDecodingContext(InvalidXLogRecPtr,
									NIL,
									true,	/* fast_forward */
									XL_ROUTINE(.page_read = read_local_xlog_page,
											   .segment_open = wal_segment_open,
											   .segment_close = wal_segment_close),
									NULL, NULL, NULL);

		/*
		 * Wait for specified streaming replication standby servers (if any)
		 * to confirm receipt of WAL up to moveto lsn.
		 */
		WaitForStandbyConfirmation(moveto);

		/*
		 * Start reading at the slot's restart_lsn, which we know to point to
		 * a valid record.
		 */
		XLogBeginRead(ctx->reader, MyReplicationSlot->data.restart_lsn);

		/* invalidate non-timetravel entries */
		InvalidateSystemCaches();

		/* Decode records until we reach the requested target */
		while (ctx->reader->EndRecPtr < moveto)
		{
			char	   *errm = NULL;
			XLogRecord *record;

			/*
			 * Read records.  No changes are generated in fast_forward mode,
			 * but snapbuilder/slot statuses are updated properly.
			 */
			record = XLogReadRecord(ctx->reader, &errm);
			if (errm)
				elog(ERROR, "could not find record while advancing replication slot: %s",
					 errm);

			/*
			 * Process the record.  Storage-level changes are ignored in
			 * fast_forward mode, but other modules (such as snapbuilder)
			 * might still have critical updates to do.
			 */
			if (record)
				LogicalDecodingProcessRecord(ctx, ctx->reader);

			CHECK_FOR_INTERRUPTS();
		}

		if (found_consistent_snapshot && DecodingContextReady(ctx))
			*found_consistent_snapshot = true;

		/*
		 * Logical decoding could have clobbered CurrentResourceOwner during
		 * transaction management, so restore the executor's value.  (This is
		 * a kluge, but it's not worth cleaning up right now.)
		 */
		CurrentResourceOwner = old_resowner;

		if (ctx->reader->EndRecPtr != InvalidXLogRecPtr)
		{
			LogicalConfirmReceivedLocation(moveto);

			/*
			 * If only the confirmed_flush LSN has changed the slot won't get
			 * marked as dirty by the above. Callers on the walsender
			 * interface are expected to keep track of their own progress and
			 * don't need it written out. But SQL-interface users cannot
			 * specify their own start positions and it's harder for them to
			 * keep track of their progress, so we should make more of an
			 * effort to save it for them.
			 *
			 * Dirty the slot so it is written out at the next checkpoint. The
			 * LSN position advanced to may still be lost on a crash but this
			 * makes the data consistent after a clean shutdown.
			 */
			ReplicationSlotMarkDirty();
		}

		retlsn = MyReplicationSlot->data.confirmed_flush;

		/* free context, call shutdown callback */
		FreeDecodingContext(ctx);

		InvalidateSystemCaches();
	}
	PG_CATCH();
	{
		/* clear all timetravel entries */
		InvalidateSystemCaches();

		PG_RE_THROW();
	}
	PG_END_TRY();

	return retlsn;
}

/*-------------------------------------------------------------------------
 *
 * vacuum.c
 *	  The postgres vacuum cleaner.
 *
 * This file includes (a) control and dispatch code for VACUUM and ANALYZE
 * commands, (b) code to compute various vacuum thresholds, and (c) index
 * vacuum code.
 *
 * VACUUM for heap AM is implemented in vacuumlazy.c, parallel vacuum in
 * vacuumparallel.c, ANALYZE in analyze.c, and VACUUM FULL is a variant of
 * CLUSTER, handled in cluster.c.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/vacuum.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/clog.h"
#include "access/commit_ts.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_database.h"
#include "catalog/pg_inherits.h"
#include "commands/cluster.h"
#include "commands/defrem.h"
#include "commands/progress.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "pgstat.h"
#include "postmaster/autovacuum.h"
#include "postmaster/bgworker_internals.h"
#include "postmaster/interrupt.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/acl.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/guc_hooks.h"
#include "utils/injection_point.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/*
 * Minimum interval for cost-based vacuum delay reports from a parallel worker.
 * This aims to avoid sending too many messages and waking up the leader too
 * frequently.
 */
#define PARALLEL_VACUUM_DELAY_REPORT_INTERVAL_NS	(NS_PER_S)

/*
 * GUC parameters
 */
int64		vacuum_freeze_min_age;
int64		vacuum_freeze_table_age;
int64		vacuum_multixact_freeze_min_age;
int64		vacuum_multixact_freeze_table_age;
int64		vacuum_failsafe_age;
int64		vacuum_multixact_failsafe_age;
double		vacuum_max_eager_freeze_failure_rate;
bool		track_cost_delay_timing;
bool		vacuum_truncate;

/*
 * Variables for cost-based vacuum delay. The defaults differ between
 * autovacuum and vacuum. They should be set with the appropriate GUC value in
 * vacuum code. They are initialized here to the defaults for client backends
 * executing VACUUM or ANALYZE.
 */
double		vacuum_cost_delay = 0;
int			vacuum_cost_limit = 200;

/* Variable for reporting cost-based vacuum delay from parallel workers. */
int64		parallel_vacuum_worker_delay_ns = 0;

/*
 * VacuumFailsafeActive is a defined as a global so that we can determine
 * whether or not to re-enable cost-based vacuum delay when vacuuming a table.
 * If failsafe mode has been engaged, we will not re-enable cost-based delay
 * for the table until after vacuuming has completed, regardless of other
 * settings.
 *
 * Only VACUUM code should inspect this variable and only table access methods
 * should set it to true. In Table AM-agnostic VACUUM code, this variable is
 * inspected to determine whether or not to allow cost-based delays. Table AMs
 * are free to set it if they desire this behavior, but it is false by default
 * and reset to false in between vacuuming each relation.
 */
bool		VacuumFailsafeActive = false;

/*
 * Variables for cost-based parallel vacuum.  See comments atop
 * compute_parallel_delay to understand how it works.
 */
pg_atomic_uint32 *VacuumSharedCostBalance = NULL;
pg_atomic_uint32 *VacuumActiveNWorkers = NULL;
int			VacuumCostBalanceLocal = 0;

/* non-export function prototypes */
static List *expand_vacuum_rel(VacuumRelation *vrel,
							   MemoryContext vac_context, int options);
static List *get_all_vacuum_rels(MemoryContext vac_context, int options);
static void vac_truncate_clog(TransactionId frozenXID,
							  MultiXactId minMulti,
							  TransactionId lastSaneFrozenXid,
							  MultiXactId lastSaneMinMulti);
static bool vacuum_rel(Oid relid, RangeVar *relation, VacuumParams params,
					   BufferAccessStrategy bstrategy);
static double compute_parallel_delay(void);
static VacOptValue get_vacoptval_from_boolean(DefElem *def);
static bool vac_tid_reaped(ItemPointer itemptr, void *state);

/*
 * GUC check function to ensure GUC value specified is within the allowable
 * range.
 */
bool
check_vacuum_buffer_usage_limit(int *newval, void **extra,
								GucSource source)
{
	/* Value upper and lower hard limits are inclusive */
	if (*newval == 0 || (*newval >= MIN_BAS_VAC_RING_SIZE_KB &&
						 *newval <= MAX_BAS_VAC_RING_SIZE_KB))
		return true;

	/* Value does not fall within any allowable range */
	GUC_check_errdetail("\"%s\" must be 0 or between %d kB and %d kB.",
						"vacuum_buffer_usage_limit",
						MIN_BAS_VAC_RING_SIZE_KB, MAX_BAS_VAC_RING_SIZE_KB);

	return false;
}

/*
 * Primary entry point for manual VACUUM and ANALYZE commands
 *
 * This is mainly a preparation wrapper for the real operations that will
 * happen in vacuum().
 */
void
ExecVacuum(ParseState *pstate, VacuumStmt *vacstmt, bool isTopLevel)
{
	VacuumParams params;
	BufferAccessStrategy bstrategy = NULL;
	bool		verbose = false;
	bool		skip_locked = false;
	bool		analyze = false;
	bool		freeze = false;
	bool		full = false;
	bool		disable_page_skipping = false;
	bool		process_main = true;
	bool		process_toast = true;
	int			ring_size;
	bool		skip_database_stats = false;
	bool		only_database_stats = false;
	MemoryContext vac_context;
	ListCell   *lc;

	/* index_cleanup and truncate values unspecified for now */
	params.index_cleanup = VACOPTVALUE_UNSPECIFIED;
	params.truncate = VACOPTVALUE_UNSPECIFIED;

	/* By default parallel vacuum is enabled */
	params.nworkers = 0;

	/* Will be set later if we recurse to a TOAST table. */
	params.toast_parent = InvalidOid;

	/*
	 * Set this to an invalid value so it is clear whether or not a
	 * BUFFER_USAGE_LIMIT was specified when making the access strategy.
	 */
	ring_size = -1;

	/* Parse options list */
	foreach(lc, vacstmt->options)
	{
		DefElem    *opt = (DefElem *) lfirst(lc);

		/* Parse common options for VACUUM and ANALYZE */
		if (strcmp(opt->defname, "verbose") == 0)
			verbose = defGetBoolean(opt);
		else if (strcmp(opt->defname, "skip_locked") == 0)
			skip_locked = defGetBoolean(opt);
		else if (strcmp(opt->defname, "buffer_usage_limit") == 0)
		{
			const char *hintmsg;
			int			result;
			char	   *vac_buffer_size;

			vac_buffer_size = defGetString(opt);

			/*
			 * Check that the specified value is valid and the size falls
			 * within the hard upper and lower limits if it is not 0.
			 */
			if (!parse_int(vac_buffer_size, &result, GUC_UNIT_KB, &hintmsg) ||
				(result != 0 &&
				 (result < MIN_BAS_VAC_RING_SIZE_KB || result > MAX_BAS_VAC_RING_SIZE_KB)))
			{
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("BUFFER_USAGE_LIMIT option must be 0 or between %d kB and %d kB",
								MIN_BAS_VAC_RING_SIZE_KB, MAX_BAS_VAC_RING_SIZE_KB),
						 hintmsg ? errhint("%s", _(hintmsg)) : 0));
			}

			ring_size = result;
		}
		else if (!vacstmt->is_vacuumcmd)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized ANALYZE option \"%s\"", opt->defname),
					 parser_errposition(pstate, opt->location)));

		/* Parse options available on VACUUM */
		else if (strcmp(opt->defname, "analyze") == 0)
			analyze = defGetBoolean(opt);
		else if (strcmp(opt->defname, "freeze") == 0)
			freeze = defGetBoolean(opt);
		else if (strcmp(opt->defname, "full") == 0)
			full = defGetBoolean(opt);
		else if (strcmp(opt->defname, "disable_page_skipping") == 0)
			disable_page_skipping = defGetBoolean(opt);
		else if (strcmp(opt->defname, "index_cleanup") == 0)
		{
			/* Interpret no string as the default, which is 'auto' */
			if (!opt->arg)
				params.index_cleanup = VACOPTVALUE_AUTO;
			else
			{
				char	   *sval = defGetString(opt);

				/* Try matching on 'auto' string, or fall back on boolean */
				if (pg_strcasecmp(sval, "auto") == 0)
					params.index_cleanup = VACOPTVALUE_AUTO;
				else
					params.index_cleanup = get_vacoptval_from_boolean(opt);
			}
		}
		else if (strcmp(opt->defname, "process_main") == 0)
			process_main = defGetBoolean(opt);
		else if (strcmp(opt->defname, "process_toast") == 0)
			process_toast = defGetBoolean(opt);
		else if (strcmp(opt->defname, "truncate") == 0)
			params.truncate = get_vacoptval_from_boolean(opt);
		else if (strcmp(opt->defname, "parallel") == 0)
		{
			if (opt->arg == NULL)
			{
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("parallel option requires a value between 0 and %d",
								MAX_PARALLEL_WORKER_LIMIT),
						 parser_errposition(pstate, opt->location)));
			}
			else
			{
				int			nworkers;

				nworkers = defGetInt32(opt);
				if (nworkers < 0 || nworkers > MAX_PARALLEL_WORKER_LIMIT)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("parallel workers for vacuum must be between 0 and %d",
									MAX_PARALLEL_WORKER_LIMIT),
							 parser_errposition(pstate, opt->location)));

				/*
				 * Disable parallel vacuum, if user has specified parallel
				 * degree as zero.
				 */
				if (nworkers == 0)
					params.nworkers = -1;
				else
					params.nworkers = nworkers;
			}
		}
		else if (strcmp(opt->defname, "skip_database_stats") == 0)
			skip_database_stats = defGetBoolean(opt);
		else if (strcmp(opt->defname, "only_database_stats") == 0)
			only_database_stats = defGetBoolean(opt);
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized VACUUM option \"%s\"", opt->defname),
					 parser_errposition(pstate, opt->location)));
	}

	/* Set vacuum options */
	params.options =
		(vacstmt->is_vacuumcmd ? VACOPT_VACUUM : VACOPT_ANALYZE) |
		(verbose ? VACOPT_VERBOSE : 0) |
		(skip_locked ? VACOPT_SKIP_LOCKED : 0) |
		(analyze ? VACOPT_ANALYZE : 0) |
		(freeze ? VACOPT_FREEZE : 0) |
		(full ? VACOPT_FULL : 0) |
		(disable_page_skipping ? VACOPT_DISABLE_PAGE_SKIPPING : 0) |
		(process_main ? VACOPT_PROCESS_MAIN : 0) |
		(process_toast ? VACOPT_PROCESS_TOAST : 0) |
		(skip_database_stats ? VACOPT_SKIP_DATABASE_STATS : 0) |
		(only_database_stats ? VACOPT_ONLY_DATABASE_STATS : 0);

	/* sanity checks on options */
	Assert(params.options & (VACOPT_VACUUM | VACOPT_ANALYZE));
	Assert((params.options & VACOPT_VACUUM) ||
		   !(params.options & (VACOPT_FULL | VACOPT_FREEZE)));

	if ((params.options & VACOPT_FULL) && params.nworkers > 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("VACUUM FULL cannot be performed in parallel")));

	/*
	 * BUFFER_USAGE_LIMIT does nothing for VACUUM (FULL) so just raise an
	 * ERROR for that case.  VACUUM (FULL, ANALYZE) does make use of it, so
	 * we'll permit that.
	 */
	if (ring_size != -1 && (params.options & VACOPT_FULL) &&
		!(params.options & VACOPT_ANALYZE))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("BUFFER_USAGE_LIMIT cannot be specified for VACUUM FULL")));

	/*
	 * Make sure VACOPT_ANALYZE is specified if any column lists are present.
	 */
	if (!(params.options & VACOPT_ANALYZE))
	{
		foreach(lc, vacstmt->rels)
		{
			VacuumRelation *vrel = lfirst_node(VacuumRelation, lc);

			if (vrel->va_cols != NIL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("ANALYZE option must be specified when a column list is provided")));
		}
	}


	/*
	 * Sanity check DISABLE_PAGE_SKIPPING option.
	 */
	if ((params.options & VACOPT_FULL) != 0 &&
		(params.options & VACOPT_DISABLE_PAGE_SKIPPING) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("VACUUM option DISABLE_PAGE_SKIPPING cannot be used with FULL")));

	/* sanity check for PROCESS_TOAST */
	if ((params.options & VACOPT_FULL) != 0 &&
		(params.options & VACOPT_PROCESS_TOAST) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("PROCESS_TOAST required with VACUUM FULL")));

	/* sanity check for ONLY_DATABASE_STATS */
	if (params.options & VACOPT_ONLY_DATABASE_STATS)
	{
		Assert(params.options & VACOPT_VACUUM);
		if (vacstmt->rels != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("ONLY_DATABASE_STATS cannot be specified with a list of tables")));
		/* don't require people to turn off PROCESS_TOAST/MAIN explicitly */
		if (params.options & ~(VACOPT_VACUUM |
							   VACOPT_VERBOSE |
							   VACOPT_PROCESS_MAIN |
							   VACOPT_PROCESS_TOAST |
							   VACOPT_ONLY_DATABASE_STATS))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("ONLY_DATABASE_STATS cannot be specified with other VACUUM options")));
	}

	/*
	 * All freeze ages are zero if the FREEZE option is given; otherwise pass
	 * them as -1 which means to use the default values.
	 */
	if (params.options & VACOPT_FREEZE)
	{
		params.freeze_min_age = 0;
		params.freeze_table_age = 0;
		params.multixact_freeze_min_age = 0;
		params.multixact_freeze_table_age = 0;
	}
	else
	{
		params.freeze_min_age = -1;
		params.freeze_table_age = -1;
		params.multixact_freeze_min_age = -1;
		params.multixact_freeze_table_age = -1;
	}

	/* user-invoked vacuum is never "for wraparound" */
	params.is_wraparound = false;

	/* user-invoked vacuum uses VACOPT_VERBOSE instead of log_min_duration */
	params.log_min_duration = -1;

	/*
	 * Later, in vacuum_rel(), we check if a reloption override was specified.
	 */
	params.max_eager_freeze_failure_rate = vacuum_max_eager_freeze_failure_rate;

	/*
	 * Create special memory context for cross-transaction storage.
	 *
	 * Since it is a child of PortalContext, it will go away eventually even
	 * if we suffer an error; there's no need for special abort cleanup logic.
	 */
	vac_context = AllocSetContextCreate(PortalContext,
										"Vacuum",
										ALLOCSET_DEFAULT_SIZES);

	/*
	 * Make a buffer strategy object in the cross-transaction memory context.
	 * We needn't bother making this for VACUUM (FULL) or VACUUM
	 * (ONLY_DATABASE_STATS) as they'll not make use of it.  VACUUM (FULL,
	 * ANALYZE) is possible, so we'd better ensure that we make a strategy
	 * when we see ANALYZE.
	 */
	if ((params.options & (VACOPT_ONLY_DATABASE_STATS |
						   VACOPT_FULL)) == 0 ||
		(params.options & VACOPT_ANALYZE) != 0)
	{

		MemoryContext old_context = MemoryContextSwitchTo(vac_context);

		Assert(ring_size >= -1);

		/*
		 * If BUFFER_USAGE_LIMIT was specified by the VACUUM or ANALYZE
		 * command, it overrides the value of VacuumBufferUsageLimit.  Either
		 * value may be 0, in which case GetAccessStrategyWithSize() will
		 * return NULL, effectively allowing full use of shared buffers.
		 */
		if (ring_size == -1)
			ring_size = VacuumBufferUsageLimit;

		bstrategy = GetAccessStrategyWithSize(BAS_VACUUM, ring_size);

		MemoryContextSwitchTo(old_context);
	}

	/* Now go through the common routine */
	vacuum(vacstmt->rels, params, bstrategy, vac_context, isTopLevel);

	/* Finally, clean up the vacuum memory context */
	MemoryContextDelete(vac_context);
}

/*
 * Internal entry point for autovacuum and the VACUUM / ANALYZE commands.
 *
 * relations, if not NIL, is a list of VacuumRelation to process; otherwise,
 * we process all relevant tables in the database.  For each VacuumRelation,
 * if a valid OID is supplied, the table with that OID is what to process;
 * otherwise, the VacuumRelation's RangeVar indicates what to process.
 *
 * params contains a set of parameters that can be used to customize the
 * behavior.
 *
 * bstrategy may be passed in as NULL when the caller does not want to
 * restrict the number of shared_buffers that VACUUM / ANALYZE can use,
 * otherwise, the caller must build a BufferAccessStrategy with the number of
 * shared_buffers that VACUUM / ANALYZE should try to limit themselves to
 * using.
 *
 * isTopLevel should be passed down from ProcessUtility.
 *
 * It is the caller's responsibility that all parameters are allocated in a
 * memory context that will not disappear at transaction commit.
 */
void
vacuum(List *relations, const VacuumParams params, BufferAccessStrategy bstrategy,
	   MemoryContext vac_context, bool isTopLevel)
{
	static bool in_vacuum = false;

	const char *stmttype;
	volatile bool in_outer_xact,
				use_own_xacts;

	stmttype = (params.options & VACOPT_VACUUM) ? "VACUUM" : "ANALYZE";

	/*
	 * We cannot run VACUUM inside a user transaction block; if we were inside
	 * a transaction, then our commit- and start-transaction-command calls
	 * would not have the intended effect!	There are numerous other subtle
	 * dependencies on this, too.
	 *
	 * ANALYZE (without VACUUM) can run either way.
	 */
	if (params.options & VACOPT_VACUUM)
	{
		PreventInTransactionBlock(isTopLevel, stmttype);
		in_outer_xact = false;
	}
	else
		in_outer_xact = IsInTransactionBlock(isTopLevel);

	/*
	 * Check for and disallow recursive calls.  This could happen when VACUUM
	 * FULL or ANALYZE calls a hostile index expression that itself calls
	 * ANALYZE.
	 */
	if (in_vacuum)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("%s cannot be executed from VACUUM or ANALYZE",
						stmttype)));

	/*
	 * Build list of relation(s) to process, putting any new data in
	 * vac_context for safekeeping.
	 */
	if (params.options & VACOPT_ONLY_DATABASE_STATS)
	{
		/* We don't process any tables in this case */
		Assert(relations == NIL);
	}
	else if (relations != NIL)
	{
		List	   *newrels = NIL;
		ListCell   *lc;

		foreach(lc, relations)
		{
			VacuumRelation *vrel = lfirst_node(VacuumRelation, lc);
			List	   *sublist;
			MemoryContext old_context;

			sublist = expand_vacuum_rel(vrel, vac_context, params.options);
			old_context = MemoryContextSwitchTo(vac_context);
			newrels = list_concat(newrels, sublist);
			MemoryContextSwitchTo(old_context);
		}
		relations = newrels;
	}
	else
		relations = get_all_vacuum_rels(vac_context, params.options);

	/*
	 * Decide whether we need to start/commit our own transactions.
	 *
	 * For VACUUM (with or without ANALYZE): always do so, so that we can
	 * release locks as soon as possible.  (We could possibly use the outer
	 * transaction for a one-table VACUUM, but handling TOAST tables would be
	 * problematic.)
	 *
	 * For ANALYZE (no VACUUM): if inside a transaction block, we cannot
	 * start/commit our own transactions.  Also, there's no need to do so if
	 * only processing one relation.  For multiple relations when not within a
	 * transaction block, and also in an autovacuum worker, use own
	 * transactions so we can release locks sooner.
	 */
	if (params.options & VACOPT_VACUUM)
		use_own_xacts = true;
	else
	{
		Assert(params.options & VACOPT_ANALYZE);
		if (AmAutoVacuumWorkerProcess())
			use_own_xacts = true;
		else if (in_outer_xact)
			use_own_xacts = false;
		else if (list_length(relations) > 1)
			use_own_xacts = true;
		else
			use_own_xacts = false;
	}

	/*
	 * vacuum_rel expects to be entered with no transaction active; it will
	 * start and commit its own transaction.  But we are called by an SQL
	 * command, and so we are executing inside a transaction already. We
	 * commit the transaction started in PostgresMain() here, and start
	 * another one before exiting to match the commit waiting for us back in
	 * PostgresMain().
	 */
	if (use_own_xacts)
	{
		Assert(!in_outer_xact);

		/* ActiveSnapshot is not set by autovacuum */
		if (ActiveSnapshotSet())
			PopActiveSnapshot();

		/* matches the StartTransaction in PostgresMain() */
		CommitTransactionCommand();
	}

	/* Turn vacuum cost accounting on or off, and set/clear in_vacuum */
	PG_TRY();
	{
		ListCell   *cur;

		in_vacuum = true;
		VacuumFailsafeActive = false;
		VacuumUpdateCosts();
		VacuumCostBalance = 0;
		VacuumCostBalanceLocal = 0;
		VacuumSharedCostBalance = NULL;
		VacuumActiveNWorkers = NULL;

		/*
		 * Loop to process each selected relation.
		 */
		foreach(cur, relations)
		{
			VacuumRelation *vrel = lfirst_node(VacuumRelation, cur);

			if (params.options & VACOPT_VACUUM)
			{
				if (!vacuum_rel(vrel->oid, vrel->relation, params, bstrategy))
					continue;
			}

			if (params.options & VACOPT_ANALYZE)
			{
				/*
				 * If using separate xacts, start one for analyze. Otherwise,
				 * we can use the outer transaction.
				 */
				if (use_own_xacts)
				{
					StartTransactionCommand();
					/* functions in indexes may want a snapshot set */
					PushActiveSnapshot(GetTransactionSnapshot());
				}

				analyze_rel(vrel->oid, vrel->relation, params,
							vrel->va_cols, in_outer_xact, bstrategy);

				if (use_own_xacts)
				{
					PopActiveSnapshot();
					/* standard_ProcessUtility() does CCI if !use_own_xacts */
					CommandCounterIncrement();
					CommitTransactionCommand();
				}
				else
				{
					/*
					 * If we're not using separate xacts, better separate the
					 * ANALYZE actions with CCIs.  This avoids trouble if user
					 * says "ANALYZE t, t".
					 */
					CommandCounterIncrement();
				}
			}

			/*
			 * Ensure VacuumFailsafeActive has been reset before vacuuming the
			 * next relation.
			 */
			VacuumFailsafeActive = false;
		}
	}
	PG_FINALLY();
	{
		in_vacuum = false;
		VacuumCostActive = false;
		VacuumFailsafeActive = false;
		VacuumCostBalance = 0;
	}
	PG_END_TRY();

	/*
	 * Finish up processing.
	 */
	if (use_own_xacts)
	{
		/* here, we are not in a transaction */

		/*
		 * This matches the CommitTransaction waiting for us in
		 * PostgresMain().
		 */
		StartTransactionCommand();
	}

	if ((params.options & VACOPT_VACUUM) &&
		!(params.options & VACOPT_SKIP_DATABASE_STATS))
	{
		/*
		 * Update pg_database.datfrozenxid, and truncate pg_xact if possible.
		 */
		vac_update_datfrozenxid();
	}

}

/*
 * Check if the current user has privileges to vacuum or analyze the relation.
 * If not, issue a WARNING log message and return false to let the caller
 * decide what to do with this relation.  This routine is used to decide if a
 * relation can be processed for VACUUM or ANALYZE.
 */
bool
vacuum_is_permitted_for_relation(Oid relid, Form_pg_class reltuple,
								 bits32 options)
{
	char	   *relname;

	Assert((options & (VACOPT_VACUUM | VACOPT_ANALYZE)) != 0);

	/*----------
	 * A role has privileges to vacuum or analyze the relation if any of the
	 * following are true:
	 *   - the role owns the current database and the relation is not shared
	 *   - the role has the MAINTAIN privilege on the relation
	 *----------
	 */
	if ((object_ownercheck(DatabaseRelationId, MyDatabaseId, GetUserId()) &&
		 !reltuple->relisshared) ||
		pg_class_aclcheck(relid, GetUserId(), ACL_MAINTAIN) == ACLCHECK_OK)
		return true;

	relname = NameStr(reltuple->relname);

	if ((options & VACOPT_VACUUM) != 0)
	{
		ereport(WARNING,
				(errmsg("permission denied to vacuum \"%s\", skipping it",
						relname)));

		/*
		 * For VACUUM ANALYZE, both logs could show up, but just generate
		 * information for VACUUM as that would be the first one to be
		 * processed.
		 */
		return false;
	}

	if ((options & VACOPT_ANALYZE) != 0)
		ereport(WARNING,
				(errmsg("permission denied to analyze \"%s\", skipping it",
						relname)));

	return false;
}


/*
 * vacuum_open_relation
 *
 * This routine is used for attempting to open and lock a relation which
 * is going to be vacuumed or analyzed.  If the relation cannot be opened
 * or locked, a log is emitted if possible.
 */
Relation
vacuum_open_relation(Oid relid, RangeVar *relation, bits32 options,
					 bool verbose, LOCKMODE lmode)
{
	Relation	rel;
	bool		rel_lock = true;
	int			elevel;

	Assert((options & (VACOPT_VACUUM | VACOPT_ANALYZE)) != 0);

	/*
	 * Open the relation and get the appropriate lock on it.
	 *
	 * There's a race condition here: the relation may have gone away since
	 * the last time we saw it.  If so, we don't need to vacuum or analyze it.
	 *
	 * If we've been asked not to wait for the relation lock, acquire it first
	 * in non-blocking mode, before calling try_relation_open().
	 */
	if (!(options & VACOPT_SKIP_LOCKED))
		rel = try_relation_open(relid, lmode);
	else if (ConditionalLockRelationOid(relid, lmode))
		rel = try_relation_open(relid, NoLock);
	else
	{
		rel = NULL;
		rel_lock = false;
	}

	/* if relation is opened, leave */
	if (rel)
		return rel;

	/*
	 * Relation could not be opened, hence generate if possible a log
	 * informing on the situation.
	 *
	 * If the RangeVar is not defined, we do not have enough information to
	 * provide a meaningful log statement.  Chances are that the caller has
	 * intentionally not provided this information so that this logging is
	 * skipped, anyway.
	 */
	if (relation == NULL)
		return NULL;

	/*
	 * Determine the log level.
	 *
	 * For manual VACUUM or ANALYZE, we emit a WARNING to match the log
	 * statements in the permission checks; otherwise, only log if the caller
	 * so requested.
	 */
	if (!AmAutoVacuumWorkerProcess())
		elevel = WARNING;
	else if (verbose)
		elevel = LOG;
	else
		return NULL;

	if ((options & VACOPT_VACUUM) != 0)
	{
		if (!rel_lock)
			ereport(elevel,
					(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
					 errmsg("skipping vacuum of \"%s\" --- lock not available",
							relation->relname)));
		else
			ereport(elevel,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("skipping vacuum of \"%s\" --- relation no longer exists",
							relation->relname)));

		/*
		 * For VACUUM ANALYZE, both logs could show up, but just generate
		 * information for VACUUM as that would be the first one to be
		 * processed.
		 */
		return NULL;
	}

	if ((options & VACOPT_ANALYZE) != 0)
	{
		if (!rel_lock)
			ereport(elevel,
					(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
					 errmsg("skipping analyze of \"%s\" --- lock not available",
							relation->relname)));
		else
			ereport(elevel,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("skipping analyze of \"%s\" --- relation no longer exists",
							relation->relname)));
	}

	return NULL;
}


/*
 * Given a VacuumRelation, fill in the table OID if it wasn't specified,
 * and optionally add VacuumRelations for partitions or inheritance children.
 *
 * If a VacuumRelation does not have an OID supplied and is a partitioned
 * table, an extra entry will be added to the output for each partition.
 * Presently, only autovacuum supplies OIDs when calling vacuum(), and
 * it does not want us to expand partitioned tables.
 *
 * We take care not to modify the input data structure, but instead build
 * new VacuumRelation(s) to return.  (But note that they will reference
 * unmodified parts of the input, eg column lists.)  New data structures
 * are made in vac_context.
 */
static List *
expand_vacuum_rel(VacuumRelation *vrel, MemoryContext vac_context,
				  int options)
{
	List	   *vacrels = NIL;
	MemoryContext oldcontext;

	/* If caller supplied OID, there's nothing we need do here. */
	if (OidIsValid(vrel->oid))
	{
		oldcontext = MemoryContextSwitchTo(vac_context);
		vacrels = lappend(vacrels, vrel);
		MemoryContextSwitchTo(oldcontext);
	}
	else
	{
		/*
		 * Process a specific relation, and possibly partitions or child
		 * tables thereof.
		 */
		Oid			relid;
		HeapTuple	tuple;
		Form_pg_class classForm;
		bool		include_children;
		bool		is_partitioned_table;
		int			rvr_opts;

		/*
		 * Since autovacuum workers supply OIDs when calling vacuum(), no
		 * autovacuum worker should reach this code.
		 */
		Assert(!AmAutoVacuumWorkerProcess());

		/*
		 * We transiently take AccessShareLock to protect the syscache lookup
		 * below, as well as find_all_inheritors's expectation that the caller
		 * holds some lock on the starting relation.
		 */
		rvr_opts = (options & VACOPT_SKIP_LOCKED) ? RVR_SKIP_LOCKED : 0;
		relid = RangeVarGetRelidExtended(vrel->relation,
										 AccessShareLock,
										 rvr_opts,
										 NULL, NULL);

		/*
		 * If the lock is unavailable, emit the same log statement that
		 * vacuum_rel() and analyze_rel() would.
		 */
		if (!OidIsValid(relid))
		{
			if (options & VACOPT_VACUUM)
				ereport(WARNING,
						(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
						 errmsg("skipping vacuum of \"%s\" --- lock not available",
								vrel->relation->relname)));
			else
				ereport(WARNING,
						(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
						 errmsg("skipping analyze of \"%s\" --- lock not available",
								vrel->relation->relname)));
			return vacrels;
		}

		/*
		 * To check whether the relation is a partitioned table and its
		 * ownership, fetch its syscache entry.
		 */
		tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for relation %u", relid);
		classForm = (Form_pg_class) GETSTRUCT(tuple);

		/*
		 * Make a returnable VacuumRelation for this rel if the user has the
		 * required privileges.
		 */
		if (vacuum_is_permitted_for_relation(relid, classForm, options))
		{
			oldcontext = MemoryContextSwitchTo(vac_context);
			vacrels = lappend(vacrels, makeVacuumRelation(vrel->relation,
														  relid,
														  vrel->va_cols));
			MemoryContextSwitchTo(oldcontext);
		}

		/*
		 * Vacuuming a partitioned table with ONLY will not do anything since
		 * the partitioned table itself is empty.  Issue a warning if the user
		 * requests this.
		 */
		include_children = vrel->relation->inh;
		is_partitioned_table = (classForm->relkind == RELKIND_PARTITIONED_TABLE);
		if ((options & VACOPT_VACUUM) && is_partitioned_table && !include_children)
			ereport(WARNING,
					(errmsg("VACUUM ONLY of partitioned table \"%s\" has no effect",
							vrel->relation->relname)));

		ReleaseSysCache(tuple);

		/*
		 * Unless the user has specified ONLY, make relation list entries for
		 * its partitions or inheritance child tables.  Note that the list
		 * returned by find_all_inheritors() includes the passed-in OID, so we
		 * have to skip that.  There's no point in taking locks on the
		 * individual partitions or child tables yet, and doing so would just
		 * add unnecessary deadlock risk.  For this last reason, we do not yet
		 * check the ownership of the partitions/tables, which get added to
		 * the list to process.  Ownership will be checked later on anyway.
		 */
		if (include_children)
		{
			List	   *part_oids = find_all_inheritors(relid, NoLock, NULL);
			ListCell   *part_lc;

			foreach(part_lc, part_oids)
			{
				Oid			part_oid = lfirst_oid(part_lc);

				if (part_oid == relid)
					continue;	/* ignore original table */

				/*
				 * We omit a RangeVar since it wouldn't be appropriate to
				 * complain about failure to open one of these relations
				 * later.
				 */
				oldcontext = MemoryContextSwitchTo(vac_context);
				vacrels = lappend(vacrels, makeVacuumRelation(NULL,
															  part_oid,
															  vrel->va_cols));
				MemoryContextSwitchTo(oldcontext);
			}
		}

		/*
		 * Release lock again.  This means that by the time we actually try to
		 * process the table, it might be gone or renamed.  In the former case
		 * we'll silently ignore it; in the latter case we'll process it
		 * anyway, but we must beware that the RangeVar doesn't necessarily
		 * identify it anymore.  This isn't ideal, perhaps, but there's little
		 * practical alternative, since we're typically going to commit this
		 * transaction and begin a new one between now and then.  Moreover,
		 * holding locks on multiple relations would create significant risk
		 * of deadlock.
		 */
		UnlockRelationOid(relid, AccessShareLock);
	}

	return vacrels;
}

/*
 * Construct a list of VacuumRelations for all vacuumable rels in
 * the current database.  The list is built in vac_context.
 */
static List *
get_all_vacuum_rels(MemoryContext vac_context, int options)
{
	List	   *vacrels = NIL;
	Relation	pgclass;
	TableScanDesc scan;
	HeapTuple	tuple;

	pgclass = table_open(RelationRelationId, AccessShareLock);

	scan = table_beginscan_catalog(pgclass, 0, NULL);

	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_class classForm = (Form_pg_class) GETSTRUCT(tuple);
		MemoryContext oldcontext;
		Oid			relid = classForm->oid;

		/*
		 * We include partitioned tables here; depending on which operation is
		 * to be performed, caller will decide whether to process or ignore
		 * them.
		 */
		if (classForm->relkind != RELKIND_RELATION &&
			classForm->relkind != RELKIND_MATVIEW &&
			classForm->relkind != RELKIND_PARTITIONED_TABLE)
			continue;

		/* check permissions of relation */
		if (!vacuum_is_permitted_for_relation(relid, classForm, options))
			continue;

		/*
		 * Build VacuumRelation(s) specifying the table OIDs to be processed.
		 * We omit a RangeVar since it wouldn't be appropriate to complain
		 * about failure to open one of these relations later.
		 */
		oldcontext = MemoryContextSwitchTo(vac_context);
		vacrels = lappend(vacrels, makeVacuumRelation(NULL,
													  relid,
													  NIL));
		MemoryContextSwitchTo(oldcontext);
	}

	table_endscan(scan);
	table_close(pgclass, AccessShareLock);

	return vacrels;
}

/*
 * vacuum_get_cutoffs() -- compute OldestXmin and freeze cutoff points
 *
 * The target relation and VACUUM parameters are our inputs.
 *
 * Output parameters are the cutoffs that VACUUM caller should use.
 *
 * Return value indicates if vacuumlazy.c caller should make its VACUUM
 * operation aggressive.  An aggressive VACUUM must advance relfrozenxid up to
 * FreezeLimit (at a minimum), and relminmxid up to MultiXactCutoff (at a
 * minimum).
 */
bool
vacuum_get_cutoffs(Relation rel, const VacuumParams params,
				   struct VacuumCutoffs *cutoffs)
{
	int64		freeze_min_age,
				multixact_freeze_min_age,
				freeze_table_age,
				multixact_freeze_table_age,
				effective_multixact_freeze_max_age;
	TransactionId nextXID,
				safeOldestXmin,
				aggressiveXIDCutoff;
	MultiXactId nextMXID,
				safeOldestMxact,
				aggressiveMXIDCutoff;

	/* Use mutable copies of freeze age parameters */
	freeze_min_age = params.freeze_min_age;
	multixact_freeze_min_age = params.multixact_freeze_min_age;
	freeze_table_age = params.freeze_table_age;
	multixact_freeze_table_age = params.multixact_freeze_table_age;

	/* Set pg_class fields in cutoffs */
	cutoffs->relfrozenxid = rel->rd_rel->relfrozenxid;
	cutoffs->relminmxid = rel->rd_rel->relminmxid;

	/*
	 * Acquire OldestXmin.
	 *
	 * We can always ignore processes running lazy vacuum.  This is because we
	 * use these values only for deciding which tuples we must keep in the
	 * tables.  Since lazy vacuum doesn't write its XID anywhere (usually no
	 * XID assigned), it's safe to ignore it.  In theory it could be
	 * problematic to ignore lazy vacuums in a full vacuum, but keep in mind
	 * that only one vacuum process can be working on a particular table at
	 * any time, and that each vacuum is always an independent transaction.
	 */
	cutoffs->OldestXmin = GetOldestNonRemovableTransactionId(rel);

	Assert(TransactionIdIsNormal(cutoffs->OldestXmin));

	/* Acquire OldestMxact */
	cutoffs->OldestMxact = GetOldestMultiXactId();
	Assert(MultiXactIdIsValid(cutoffs->OldestMxact));

	/* Acquire next XID/next MXID values used to apply age-based settings */
	nextXID = ReadNextTransactionId();
	nextMXID = ReadNextMultiXactId();

	/*
	 * Also compute the multixact age for which freezing is urgent.  This is
	 * normally autovacuum_multixact_freeze_max_age, but may be less if we are
	 * short of multixact member space.
	 */
	effective_multixact_freeze_max_age = autovacuum_multixact_freeze_max_age;

	/*
	 * Almost ready to set freeze output parameters; check if OldestXmin or
	 * OldestMxact are held back to an unsafe degree before we start on that
	 */
	safeOldestXmin = nextXID - autovacuum_freeze_max_age;
	if (!TransactionIdIsNormal(safeOldestXmin))
		safeOldestXmin = FirstNormalTransactionId;
	safeOldestMxact = nextMXID - effective_multixact_freeze_max_age;
	if (safeOldestMxact < FirstMultiXactId)
		safeOldestMxact = FirstMultiXactId;
	if (TransactionIdPrecedes(cutoffs->OldestXmin, safeOldestXmin))
		ereport(WARNING,
				(errmsg("cutoff for removing and freezing tuples is far in the past"),
				 errhint("Close open transactions soon to avoid wraparound problems.\n"
						 "You might also need to commit or roll back old prepared transactions, or drop stale replication slots.")));
	if (MultiXactIdPrecedes(cutoffs->OldestMxact, safeOldestMxact))
		ereport(WARNING,
				(errmsg("cutoff for freezing multixacts is far in the past"),
				 errhint("Close open transactions soon to avoid wraparound problems.\n"
						 "You might also need to commit or roll back old prepared transactions, or drop stale replication slots.")));

	/*
	 * Determine the minimum freeze age to use: as specified by the caller, or
	 * vacuum_freeze_min_age, but in any case not more than half
	 * autovacuum_freeze_max_age, so that autovacuums to prevent XID
	 * wraparound won't occur too frequently.
	 */
	if (freeze_min_age < 0)
		freeze_min_age = vacuum_freeze_min_age;
	freeze_min_age = Min(freeze_min_age, autovacuum_freeze_max_age / 2);
	Assert(freeze_min_age >= 0);

	/* Compute FreezeLimit, being careful to generate a normal XID */
	cutoffs->FreezeLimit = nextXID - freeze_min_age;
	if (!TransactionIdIsNormal(cutoffs->FreezeLimit))
		cutoffs->FreezeLimit = FirstNormalTransactionId;
	/* FreezeLimit must always be <= OldestXmin */
	if (TransactionIdPrecedes(cutoffs->OldestXmin, cutoffs->FreezeLimit))
		cutoffs->FreezeLimit = cutoffs->OldestXmin;

	/*
	 * Determine the minimum multixact freeze age to use: as specified by
	 * caller, or vacuum_multixact_freeze_min_age, but in any case not more
	 * than half effective_multixact_freeze_max_age, so that autovacuums to
	 * prevent MultiXact wraparound won't occur too frequently.
	 */
	if (multixact_freeze_min_age < 0)
		multixact_freeze_min_age = vacuum_multixact_freeze_min_age;
	multixact_freeze_min_age = Min(multixact_freeze_min_age,
								   effective_multixact_freeze_max_age / 2);
	Assert(multixact_freeze_min_age >= 0);

	/* Compute MultiXactCutoff, being careful to generate a valid value */
	cutoffs->MultiXactCutoff = nextMXID - multixact_freeze_min_age;
	if (cutoffs->MultiXactCutoff < FirstMultiXactId)
		cutoffs->MultiXactCutoff = FirstMultiXactId;
	/* MultiXactCutoff must always be <= OldestMxact */
	if (MultiXactIdPrecedes(cutoffs->OldestMxact, cutoffs->MultiXactCutoff))
		cutoffs->MultiXactCutoff = cutoffs->OldestMxact;

	/*
	 * Finally, figure out if caller needs to do an aggressive VACUUM or not.
	 *
	 * Determine the table freeze age to use: as specified by the caller, or
	 * the value of the vacuum_freeze_table_age GUC, but in any case not more
	 * than autovacuum_freeze_max_age * 0.95, so that if you have e.g nightly
	 * VACUUM schedule, the nightly VACUUM gets a chance to freeze XIDs before
	 * anti-wraparound autovacuum is launched.
	 */
	if (freeze_table_age < 0)
		freeze_table_age = vacuum_freeze_table_age;
	freeze_table_age = Min(freeze_table_age, autovacuum_freeze_max_age * 0.95);
	Assert(freeze_table_age >= 0);
	aggressiveXIDCutoff = nextXID - freeze_table_age;
	if (!TransactionIdIsNormal(aggressiveXIDCutoff))
		aggressiveXIDCutoff = FirstNormalTransactionId;
	if (TransactionIdPrecedesOrEquals(cutoffs->relfrozenxid,
									  aggressiveXIDCutoff))
		return true;

	/*
	 * Similar to the above, determine the table freeze age to use for
	 * multixacts: as specified by the caller, or the value of the
	 * vacuum_multixact_freeze_table_age GUC, but in any case not more than
	 * effective_multixact_freeze_max_age * 0.95, so that if you have e.g.
	 * nightly VACUUM schedule, the nightly VACUUM gets a chance to freeze
	 * multixacts before anti-wraparound autovacuum is launched.
	 */
	if (multixact_freeze_table_age < 0)
		multixact_freeze_table_age = vacuum_multixact_freeze_table_age;
	multixact_freeze_table_age =
		Min(multixact_freeze_table_age,
			effective_multixact_freeze_max_age * 0.95);
	Assert(multixact_freeze_table_age >= 0);
	aggressiveMXIDCutoff = nextMXID - multixact_freeze_table_age;
	if (aggressiveMXIDCutoff < FirstMultiXactId)
		aggressiveMXIDCutoff = FirstMultiXactId;
	if (MultiXactIdPrecedesOrEquals(cutoffs->relminmxid,
									aggressiveMXIDCutoff))
		return true;

	/* Non-aggressive VACUUM */
	return false;
}

/*
 * vacuum_xid_failsafe_check() -- Used by VACUUM's wraparound failsafe
 * mechanism to determine if its table's relfrozenxid and relminmxid are now
 * dangerously far in the past.
 *
 * When we return true, VACUUM caller triggers the failsafe.
 */
bool
vacuum_xid_failsafe_check(const struct VacuumCutoffs *cutoffs)
{
	TransactionId relfrozenxid = cutoffs->relfrozenxid;
	MultiXactId relminmxid = cutoffs->relminmxid;
	TransactionId xid_skip_limit;
	MultiXactId multi_skip_limit;
	int			skip_index_vacuum;

	Assert(TransactionIdIsNormal(relfrozenxid));
	Assert(MultiXactIdIsValid(relminmxid));

	/*
	 * Determine the index skipping age to use. In any case no less than
	 * autovacuum_freeze_max_age * 1.05.
	 */
	skip_index_vacuum = Max(vacuum_failsafe_age, autovacuum_freeze_max_age * 1.05);

	xid_skip_limit = ReadNextTransactionId() - skip_index_vacuum;
	if (!TransactionIdIsNormal(xid_skip_limit))
		xid_skip_limit = FirstNormalTransactionId;

	if (TransactionIdPrecedes(relfrozenxid, xid_skip_limit))
	{
		/* The table's relfrozenxid is too old */
		return true;
	}

	/*
	 * Similar to above, determine the index skipping age to use for
	 * multixact. In any case no less than autovacuum_multixact_freeze_max_age *
	 * 1.05.
	 */
	skip_index_vacuum = Max(vacuum_multixact_failsafe_age,
							autovacuum_multixact_freeze_max_age * 1.05);

	multi_skip_limit = ReadNextMultiXactId() - skip_index_vacuum;
	if (multi_skip_limit < FirstMultiXactId)
		multi_skip_limit = FirstMultiXactId;

	if (MultiXactIdPrecedes(relminmxid, multi_skip_limit))
	{
		/* The table's relminmxid is too old */
		return true;
	}

	return false;
}

/*
 * vac_estimate_reltuples() -- estimate the new value for pg_class.reltuples
 *
 *		If we scanned the whole relation then we should just use the count of
 *		live tuples seen; but if we did not, we should not blindly extrapolate
 *		from that number, since VACUUM may have scanned a quite nonrandom
 *		subset of the table.  When we have only partial information, we take
 *		the old value of pg_class.reltuples/pg_class.relpages as a measurement
 *		of the tuple density in the unscanned pages.
 *
 *		Note: scanned_tuples should count only *live* tuples, since
 *		pg_class.reltuples is defined that way.
 */
double
vac_estimate_reltuples(Relation relation,
					   BlockNumber total_pages,
					   BlockNumber scanned_pages,
					   double scanned_tuples)
{
	BlockNumber old_rel_pages = relation->rd_rel->relpages;
	double		old_rel_tuples = relation->rd_rel->reltuples;
	double		old_density;
	double		unscanned_pages;
	double		total_tuples;

	/* If we did scan the whole table, just use the count as-is */
	if (scanned_pages >= total_pages)
		return scanned_tuples;

	/*
	 * When successive VACUUM commands scan the same few pages again and
	 * again, without anything from the table really changing, there is a risk
	 * that our beliefs about tuple density will gradually become distorted.
	 * This might be caused by vacuumlazy.c implementation details, such as
	 * its tendency to always scan the last heap page.  Handle that here.
	 *
	 * If the relation is _exactly_ the same size according to the existing
	 * pg_class entry, and only a few of its pages (less than 2%) were
	 * scanned, keep the existing value of reltuples.  Also keep the existing
	 * value when only a subset of rel's pages <= a single page were scanned.
	 *
	 * (Note: we might be returning -1 here.)
	 */
	if (old_rel_pages == total_pages &&
		scanned_pages < (double) total_pages * 0.02)
		return old_rel_tuples;
	if (scanned_pages <= 1)
		return old_rel_tuples;

	/*
	 * If old density is unknown, we can't do much except scale up
	 * scanned_tuples to match total_pages.
	 */
	if (old_rel_tuples < 0 || old_rel_pages == 0)
		return floor((scanned_tuples / scanned_pages) * total_pages + 0.5);

	/*
	 * Okay, we've covered the corner cases.  The normal calculation is to
	 * convert the old measurement to a density (tuples per page), then
	 * estimate the number of tuples in the unscanned pages using that figure,
	 * and finally add on the number of tuples in the scanned pages.
	 */
	old_density = old_rel_tuples / old_rel_pages;
	unscanned_pages = (double) total_pages - (double) scanned_pages;
	total_tuples = old_density * unscanned_pages + scanned_tuples;
	return floor(total_tuples + 0.5);
}


/*
 *	vac_update_relstats() -- update statistics for one relation
 *
 *		Update the whole-relation statistics that are kept in its pg_class
 *		row.  There are additional stats that will be updated if we are
 *		doing ANALYZE, but we always update these stats.  This routine works
 *		for both index and heap relation entries in pg_class.
 *
 *		We violate transaction semantics here by overwriting the rel's
 *		existing pg_class tuple with the new values.  This is reasonably
 *		safe as long as we're sure that the new values are correct whether or
 *		not this transaction commits.  The reason for doing this is that if
 *		we updated these tuples in the usual way, vacuuming pg_class itself
 *		wouldn't work very well --- by the time we got done with a vacuum
 *		cycle, most of the tuples in pg_class would've been obsoleted.  Of
 *		course, this only works for fixed-size not-null columns, but these are.
 *
 *		Another reason for doing it this way is that when we are in a lazy
 *		VACUUM and have PROC_IN_VACUUM set, we mustn't do any regular updates.
 *		Somebody vacuuming pg_class might think they could delete a tuple
 *		marked with xmin = our xid.
 *
 *		In addition to fundamentally nontransactional statistics such as
 *		relpages and relallvisible, we try to maintain certain lazily-updated
 *		DDL flags such as relhasindex, by clearing them if no longer correct.
 *		It's safe to do this in VACUUM, which can't run in parallel with
 *		CREATE INDEX/RULE/TRIGGER and can't be part of a transaction block.
 *		However, it's *not* safe to do it in an ANALYZE that's within an
 *		outer transaction, because for example the current transaction might
 *		have dropped the last index; then we'd think relhasindex should be
 *		cleared, but if the transaction later rolls back this would be wrong.
 *		So we refrain from updating the DDL flags if we're inside an outer
 *		transaction.  This is OK since postponing the flag maintenance is
 *		always allowable.
 *
 *		Note: num_tuples should count only *live* tuples, since
 *		pg_class.reltuples is defined that way.
 *
 *		This routine is shared by VACUUM and ANALYZE.
 */
void
vac_update_relstats(Relation relation,
					BlockNumber num_pages, double num_tuples,
					BlockNumber num_all_visible_pages,
					BlockNumber num_all_frozen_pages,
					bool hasindex, TransactionId frozenxid,
					MultiXactId minmulti,
					bool *frozenxid_updated, bool *minmulti_updated,
					bool in_outer_xact)
{
	Oid			relid = RelationGetRelid(relation);
	Relation	rd;
	ScanKeyData key[1];
	HeapTuple	ctup;
	void	   *inplace_state;
	Form_pg_class pgcform;
	bool		dirty,
				futurexid,
				futuremxid;
	TransactionId oldfrozenxid;
	MultiXactId oldminmulti;

	rd = table_open(RelationRelationId, RowExclusiveLock);

	/* Fetch a copy of the tuple to scribble on */
	ScanKeyInit(&key[0],
				Anum_pg_class_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	systable_inplace_update_begin(rd, ClassOidIndexId, true,
								  NULL, 1, key, &ctup, &inplace_state);
	if (!HeapTupleIsValid(ctup))
		elog(ERROR, "pg_class entry for relid %u vanished during vacuuming",
			 relid);
	pgcform = (Form_pg_class) GETSTRUCT(ctup);

	/* Apply statistical updates, if any, to copied tuple */

	dirty = false;
	if (pgcform->relpages != (int32) num_pages)
	{
		pgcform->relpages = (int32) num_pages;
		dirty = true;
	}
	if (pgcform->reltuples != (float4) num_tuples)
	{
		pgcform->reltuples = (float4) num_tuples;
		dirty = true;
	}
	if (pgcform->relallvisible != (int32) num_all_visible_pages)
	{
		pgcform->relallvisible = (int32) num_all_visible_pages;
		dirty = true;
	}
	if (pgcform->relallfrozen != (int32) num_all_frozen_pages)
	{
		pgcform->relallfrozen = (int32) num_all_frozen_pages;
		dirty = true;
	}

	/* Apply DDL updates, but not inside an outer transaction (see above) */

	if (!in_outer_xact)
	{
		/*
		 * If we didn't find any indexes, reset relhasindex.
		 */
		if (pgcform->relhasindex && !hasindex)
		{
			pgcform->relhasindex = false;
			dirty = true;
		}

		/* We also clear relhasrules and relhastriggers if needed */
		if (pgcform->relhasrules && relation->rd_rules == NULL)
		{
			pgcform->relhasrules = false;
			dirty = true;
		}
		if (pgcform->relhastriggers && relation->trigdesc == NULL)
		{
			pgcform->relhastriggers = false;
			dirty = true;
		}
	}

	/*
	 * Update relfrozenxid, unless caller passed InvalidTransactionId
	 * indicating it has no new data.
	 *
	 * Ordinarily, we don't let relfrozenxid go backwards.  However, if the
	 * stored relfrozenxid is "in the future" then it seems best to assume
	 * it's corrupt, and overwrite with the oldest remaining XID in the table.
	 * This should match vac_update_datfrozenxid() concerning what we consider
	 * to be "in the future".
	 */
	oldfrozenxid = pgcform->relfrozenxid;
	futurexid = false;
	if (frozenxid_updated)
		*frozenxid_updated = false;

	Assert(TransactionIdPrecedesOrEquals(frozenxid, ReadNextTransactionId()));

	if (TransactionIdIsNormal(frozenxid) && oldfrozenxid != frozenxid)
	{
		bool		update = false;

		if (TransactionIdPrecedes(oldfrozenxid, frozenxid))
			update = true;
		else if (TransactionIdPrecedes(ReadNextTransactionId(), oldfrozenxid))
			futurexid = update = true;

		if (update)
		{
			pgcform->relfrozenxid = frozenxid;
			dirty = true;
			if (frozenxid_updated)
				*frozenxid_updated = true;
		}
	}

	/* Similarly for relminmxid */
	oldminmulti = pgcform->relminmxid;
	futuremxid = false;
	if (minmulti_updated)
		*minmulti_updated = false;

	Assert(MultiXactIdPrecedesOrEquals(minmulti, ReadNextMultiXactId()));

	if (MultiXactIdIsValid(minmulti) && oldminmulti != minmulti)
	{
		bool		update = false;

		if (MultiXactIdPrecedes(oldminmulti, minmulti))
			update = true;
		else if (MultiXactIdPrecedes(ReadNextMultiXactId(), oldminmulti))
			futuremxid = update = true;

		if (update)
		{
			pgcform->relminmxid = minmulti;
			dirty = true;
			if (minmulti_updated)
				*minmulti_updated = true;
		}
	}

	/* If anything changed, write out the tuple. */
	if (dirty)
		systable_inplace_update_finish(inplace_state, ctup);
	else
		systable_inplace_update_cancel(inplace_state);

	table_close(rd, RowExclusiveLock);

	if (futurexid)
		ereport(WARNING,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("overwrote invalid relfrozenxid value %" PRIu64 " with new value %" PRIu64 " for table \"%s\"",
								 oldfrozenxid, frozenxid,
								 RelationGetRelationName(relation))));
	if (futuremxid)
		ereport(WARNING,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("overwrote invalid relminmxid value %" PRIu64 " with new value %" PRIu64 " for table \"%s\"",
								 oldminmulti, minmulti,
								 RelationGetRelationName(relation))));
}


/*
 *	vac_update_datfrozenxid() -- update pg_database.datfrozenxid for our DB
 *
 *		Update pg_database's datfrozenxid entry for our database to be the
 *		minimum of the pg_class.relfrozenxid values.
 *
 *		Similarly, update our datminmxid to be the minimum of the
 *		pg_class.relminmxid values.
 *
 *		If we are able to advance either pg_database value, also try to
 *		truncate pg_xact and pg_multixact.
 *
 *		We violate transaction semantics here by overwriting the database's
 *		existing pg_database tuple with the new values.  This is reasonably
 *		safe since the new values are correct whether or not this transaction
 *		commits.  As with vac_update_relstats, this avoids leaving dead tuples
 *		behind after a VACUUM.
 */
void
vac_update_datfrozenxid(void)
{
	HeapTuple	tuple;
	Form_pg_database dbform;
	Relation	relation;
	SysScanDesc scan;
	HeapTuple	classTup;
	TransactionId newFrozenXid;
	MultiXactId newMinMulti;
	TransactionId lastSaneFrozenXid;
	MultiXactId lastSaneMinMulti;
	bool		bogus = false;
	bool		dirty = false;
	ScanKeyData key[1];
	void	   *inplace_state;

	/*
	 * Restrict this task to one backend per database.  This avoids race
	 * conditions that would move datfrozenxid or datminmxid backward.  It
	 * avoids calling vac_truncate_clog() with a datfrozenxid preceding a
	 * datfrozenxid passed to an earlier vac_truncate_clog() call.
	 */
	LockDatabaseFrozenIds(ExclusiveLock);

	/*
	 * Initialize the "min" calculation with
	 * GetOldestNonRemovableTransactionId(), which is a reasonable
	 * approximation to the minimum relfrozenxid for not-yet-committed
	 * pg_class entries for new tables; see AddNewRelationTuple().  So we
	 * cannot produce a wrong minimum by starting with this.
	 */
	newFrozenXid = GetOldestNonRemovableTransactionId(NULL);

	/*
	 * Similarly, initialize the MultiXact "min" with the value that would be
	 * used on pg_class for new tables.  See AddNewRelationTuple().
	 */
	newMinMulti = GetOldestMultiXactId();

	/*
	 * Identify the latest relfrozenxid and relminmxid values that we could
	 * validly see during the scan.  These are conservative values, but it's
	 * not really worth trying to be more exact.
	 */
	lastSaneFrozenXid = ReadNextTransactionId();
	lastSaneMinMulti = ReadNextMultiXactId();

	/*
	 * We must seqscan pg_class to find the minimum Xid, because there is no
	 * index that can help us here.
	 *
	 * See vac_truncate_clog() for the race condition to prevent.
	 */
	relation = table_open(RelationRelationId, AccessShareLock);

	scan = systable_beginscan(relation, InvalidOid, false,
							  NULL, 0, NULL);

	while ((classTup = systable_getnext(scan)) != NULL)
	{
		volatile FormData_pg_class *classForm = (Form_pg_class) GETSTRUCT(classTup);
		TransactionId relfrozenxid = classForm->relfrozenxid;
		TransactionId relminmxid = classForm->relminmxid;

		/*
		 * Only consider relations able to hold unfrozen XIDs (anything else
		 * should have InvalidTransactionId in relfrozenxid anyway).
		 */
		if (classForm->relkind != RELKIND_RELATION &&
			classForm->relkind != RELKIND_MATVIEW &&
			classForm->relkind != RELKIND_TOASTVALUE)
		{
			Assert(!TransactionIdIsValid(relfrozenxid));
			Assert(!MultiXactIdIsValid(relminmxid));
			continue;
		}

		/*
		 * Some table AMs might not need per-relation xid / multixid horizons.
		 * It therefore seems reasonable to allow relfrozenxid and relminmxid
		 * to not be set (i.e. set to their respective Invalid*Id)
		 * independently. Thus validate and compute horizon for each only if
		 * set.
		 *
		 * If things are working properly, no relation should have a
		 * relfrozenxid or relminmxid that is "in the future".  However, such
		 * cases have been known to arise due to bugs in pg_upgrade.  If we
		 * see any entries that are "in the future", chicken out and don't do
		 * anything.  This ensures we won't truncate clog & multixact SLRUs
		 * before those relations have been scanned and cleaned up.
		 */

		if (TransactionIdIsValid(relfrozenxid))
		{
			Assert(TransactionIdIsNormal(relfrozenxid));

			/* check for values in the future */
			if (TransactionIdPrecedes(lastSaneFrozenXid, relfrozenxid))
			{
				bogus = true;
				break;
			}

			/* determine new horizon */
			if (TransactionIdPrecedes(relfrozenxid, newFrozenXid))
				newFrozenXid = relfrozenxid;
		}

		if (MultiXactIdIsValid(relminmxid))
		{
			/* check for values in the future */
			if (MultiXactIdPrecedes(lastSaneMinMulti, relminmxid))
			{
				bogus = true;
				break;
			}

			/* determine new horizon */
			if (MultiXactIdPrecedes(relminmxid, newMinMulti))
				newMinMulti = relminmxid;
		}
	}

	/* we're done with pg_class */
	systable_endscan(scan);
	table_close(relation, AccessShareLock);

	/* chicken out if bogus data found */
	if (bogus)
		return;

	Assert(TransactionIdIsNormal(newFrozenXid));
	Assert(MultiXactIdIsValid(newMinMulti));

	/* Now fetch the pg_database tuple we need to update. */
	relation = table_open(DatabaseRelationId, RowExclusiveLock);

	/*
	 * Fetch a copy of the tuple to scribble on.  We could check the syscache
	 * tuple first.  If that concluded !dirty, we'd avoid waiting on
	 * concurrent heap_update() and would avoid exclusive-locking the buffer.
	 * For now, don't optimize that.
	 */
	ScanKeyInit(&key[0],
				Anum_pg_database_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(MyDatabaseId));

	systable_inplace_update_begin(relation, DatabaseOidIndexId, true,
								  NULL, 1, key, &tuple, &inplace_state);

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "could not find tuple for database %u", MyDatabaseId);

	dbform = (Form_pg_database) GETSTRUCT(tuple);

	/*
	 * As in vac_update_relstats(), we ordinarily don't want to let
	 * datfrozenxid go backward; but if it's "in the future" then it must be
	 * corrupt and it seems best to overwrite it.
	 */
	if (dbform->datfrozenxid != newFrozenXid &&
		(TransactionIdPrecedes(dbform->datfrozenxid, newFrozenXid) ||
		 TransactionIdPrecedes(lastSaneFrozenXid, dbform->datfrozenxid)))
	{
		dbform->datfrozenxid = newFrozenXid;
		dirty = true;
	}
	else
		newFrozenXid = dbform->datfrozenxid;

	/* Ditto for datminmxid */
	if (dbform->datminmxid != newMinMulti &&
		(MultiXactIdPrecedes(dbform->datminmxid, newMinMulti) ||
		 MultiXactIdPrecedes(lastSaneMinMulti, dbform->datminmxid)))
	{
		dbform->datminmxid = newMinMulti;
		dirty = true;
	}
	else
		newMinMulti = dbform->datminmxid;

	if (dirty)
		systable_inplace_update_finish(inplace_state, tuple);
	else
		systable_inplace_update_cancel(inplace_state);

	heap_freetuple(tuple);
	table_close(relation, RowExclusiveLock);

	/*
	 * If we were able to advance datfrozenxid or datminmxid, see if we can
	 * truncate pg_xact and/or pg_multixact.  Also do it if the shared
	 * XID-wrap-limit info is stale, since this action will update that too.
	 */
	if (dirty || ForceTransactionIdLimitUpdate())
		vac_truncate_clog(newFrozenXid, newMinMulti,
						  lastSaneFrozenXid, lastSaneMinMulti);
}


/*
 *	vac_truncate_clog() -- attempt to truncate the commit log
 *
 *		Scan pg_database to determine the system-wide oldest datfrozenxid,
 *		and use it to truncate the transaction commit log (pg_xact).
 *		Also update the XID wrap limit info maintained by varsup.c.
 *		Likewise for datminmxid.
 *
 *		The passed frozenXID and minMulti are the updated values for my own
 *		pg_database entry. They're used to initialize the "min" calculations.
 *		The caller also passes the "last sane" XID and MXID, since it has
 *		those at hand already.
 *
 *		This routine is only invoked when we've managed to change our
 *		DB's datfrozenxid/datminmxid values, or we found that the shared
 *		XID-wrap-limit info is stale.
 */
static void
vac_truncate_clog(TransactionId frozenXID,
				  MultiXactId minMulti,
				  TransactionId lastSaneFrozenXid,
				  MultiXactId lastSaneMinMulti)
{
	TransactionId nextXID = ReadNextTransactionId();
	Relation	relation;
	TableScanDesc scan;
	HeapTuple	tuple;
	Oid			oldestxid_datoid;
	Oid			minmulti_datoid;
	bool		bogus = false;
	bool		frozenAlreadyWrapped = false;

	/* Restrict task to one backend per cluster; see SimpleLruTruncate(). */
	LWLockAcquire(WrapLimitsVacuumLock, LW_EXCLUSIVE);

	/* init oldest datoids to sync with my frozenXID/minMulti values */
	oldestxid_datoid = MyDatabaseId;
	minmulti_datoid = MyDatabaseId;

	/*
	 * Scan pg_database to compute the minimum datfrozenxid/datminmxid
	 *
	 * Since vac_update_datfrozenxid updates datfrozenxid/datminmxid in-place,
	 * the values could change while we look at them.  Fetch each one just
	 * once to ensure sane behavior of the comparison logic.  (Here, as in
	 * many other places, we assume that fetching or updating an XID in shared
	 * storage is atomic.)
	 *
	 * Note: we need not worry about a race condition with new entries being
	 * inserted by CREATE DATABASE.  Any such entry will have a copy of some
	 * existing DB's datfrozenxid, and that source DB cannot be ours because
	 * of the interlock against copying a DB containing an active backend.
	 * Hence the new entry will not reduce the minimum.  Also, if two VACUUMs
	 * concurrently modify the datfrozenxid's of different databases, the
	 * worst possible outcome is that pg_xact is not truncated as aggressively
	 * as it could be.
	 */
	relation = table_open(DatabaseRelationId, AccessShareLock);

	scan = table_beginscan_catalog(relation, 0, NULL);

	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		volatile FormData_pg_database *dbform = (Form_pg_database) GETSTRUCT(tuple);
		TransactionId datfrozenxid = dbform->datfrozenxid;
		TransactionId datminmxid = dbform->datminmxid;

		Assert(TransactionIdIsNormal(datfrozenxid));
		Assert(MultiXactIdIsValid(datminmxid));

		/*
		 * If database is in the process of getting dropped, or has been
		 * interrupted while doing so, no connections to it are possible
		 * anymore. Therefore we don't need to take it into account here.
		 * Which is good, because it can't be processed by autovacuum either.
		 */
		if (database_is_invalid_form((Form_pg_database) dbform))
		{
			elog(DEBUG2,
				 "skipping invalid database \"%s\" while computing relfrozenxid",
				 NameStr(dbform->datname));
			continue;
		}

		/*
		 * If things are working properly, no database should have a
		 * datfrozenxid or datminmxid that is "in the future".  However, such
		 * cases have been known to arise due to bugs in pg_upgrade.  If we
		 * see any entries that are "in the future", chicken out and don't do
		 * anything.  This ensures we won't truncate clog before those
		 * databases have been scanned and cleaned up.  (We will issue the
		 * "already wrapped" warning if appropriate, though.)
		 */
		if (TransactionIdPrecedes(lastSaneFrozenXid, datfrozenxid) ||
			MultiXactIdPrecedes(lastSaneMinMulti, datminmxid))
			bogus = true;

		if (TransactionIdPrecedes(nextXID, datfrozenxid))
			frozenAlreadyWrapped = true;
		else if (TransactionIdPrecedes(datfrozenxid, frozenXID))
		{
			frozenXID = datfrozenxid;
			oldestxid_datoid = dbform->oid;
		}

		if (MultiXactIdPrecedes(datminmxid, minMulti))
		{
			minMulti = datminmxid;
			minmulti_datoid = dbform->oid;
		}
	}

	table_endscan(scan);

	table_close(relation, AccessShareLock);

	/*
	 * Do not truncate CLOG if we seem to have suffered wraparound already;
	 * the computed minimum XID might be bogus.  This case should now be
	 * impossible due to the defenses in GetNewTransactionId, but we keep the
	 * test anyway.
	 */
	if (frozenAlreadyWrapped)
	{
		ereport(WARNING,
				(errmsg("some databases have not been vacuumed in over 2 billion transactions"),
				 errdetail("You might have already suffered transaction-wraparound data loss.")));
		LWLockRelease(WrapLimitsVacuumLock);
		return;
	}

	/* chicken out if data is bogus in any other way */
	if (bogus)
	{
		LWLockRelease(WrapLimitsVacuumLock);
		return;
	}

	/*
	 * Advance the oldest value for commit timestamps before truncating, so
	 * that if a user requests a timestamp for a transaction we're truncating
	 * away right after this point, they get NULL instead of an ugly "file not
	 * found" error from slru.c.  This doesn't matter for xact/multixact
	 * because they are not subject to arbitrary lookups from users.
	 */
	AdvanceOldestCommitTsXid(frozenXID);

	/*
	 * Truncate CLOG, multixact and CommitTs to the oldest computed value.
	 */
	TruncateCLOG(frozenXID, oldestxid_datoid);
	TruncateCommitTs(frozenXID);
	TruncateMultiXact(minMulti, minmulti_datoid);

	/*
	 * Update the wrap limit for GetNewTransactionId and creation of new
	 * MultiXactIds.  Note: these functions will also signal the postmaster
	 * for an(other) autovac cycle if needed.   XXX should we avoid possibly
	 * signaling twice?
	 */
	SetTransactionIdLimit(frozenXID, oldestxid_datoid);
	SetMultiXactIdLimit(minMulti, minmulti_datoid, false);

	LWLockRelease(WrapLimitsVacuumLock);
}


/*
 *	vacuum_rel() -- vacuum one heap relation
 *
 *		relid identifies the relation to vacuum.  If relation is supplied,
 *		use the name therein for reporting any failure to open/lock the rel;
 *		do not use it once we've successfully opened the rel, since it might
 *		be stale.
 *
 *		Returns true if it's okay to proceed with a requested ANALYZE
 *		operation on this table.
 *
 *		Doing one heap at a time incurs extra overhead, since we need to
 *		check that the heap exists again just before we vacuum it.  The
 *		reason that we do this is so that vacuuming can be spread across
 *		many small transactions.  Otherwise, two-phase locking would require
 *		us to lock the entire database during one pass of the vacuum cleaner.
 *
 *		At entry and exit, we are not inside a transaction.
 */
static bool
vacuum_rel(Oid relid, RangeVar *relation, VacuumParams params,
		   BufferAccessStrategy bstrategy)
{
	LOCKMODE	lmode;
	Relation	rel;
	LockRelId	lockrelid;
	Oid			priv_relid;
	Oid			toast_relid;
	Oid			save_userid;
	int			save_sec_context;
	int			save_nestlevel;
	VacuumParams toast_vacuum_params;

	/*
	 * This function scribbles on the parameters, so make a copy early to
	 * avoid affecting the TOAST table (if we do end up recursing to it).
	 */
	memcpy(&toast_vacuum_params, &params, sizeof(VacuumParams));

	/* Begin a transaction for vacuuming this relation */
	StartTransactionCommand();

	if (!(params.options & VACOPT_FULL))
	{
		/*
		 * In lazy vacuum, we can set the PROC_IN_VACUUM flag, which lets
		 * other concurrent VACUUMs know that they can ignore this one while
		 * determining their OldestXmin.  (The reason we don't set it during a
		 * full VACUUM is exactly that we may have to run user-defined
		 * functions for functional indexes, and we want to make sure that if
		 * they use the snapshot set above, any tuples it requires can't get
		 * removed from other tables.  An index function that depends on the
		 * contents of other tables is arguably broken, but we won't break it
		 * here by violating transaction semantics.)
		 *
		 * We also set the VACUUM_FOR_WRAPAROUND flag, which is passed down by
		 * autovacuum; it's used to avoid canceling a vacuum that was invoked
		 * in an emergency.
		 *
		 * Note: these flags remain set until CommitTransaction or
		 * AbortTransaction.  We don't want to clear them until we reset
		 * MyProc->xid/xmin, otherwise GetOldestNonRemovableTransactionId()
		 * might appear to go backwards, which is probably Not Good.  (We also
		 * set PROC_IN_VACUUM *before* taking our own snapshot, so that our
		 * xmin doesn't become visible ahead of setting the flag.)
		 */
		LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
		MyProc->statusFlags |= PROC_IN_VACUUM;
		if (params.is_wraparound)
			MyProc->statusFlags |= PROC_VACUUM_FOR_WRAPAROUND;
		ProcGlobal->statusFlags[MyProc->pgxactoff] = MyProc->statusFlags;
		LWLockRelease(ProcArrayLock);
	}

	/*
	 * Need to acquire a snapshot to prevent pg_subtrans from being truncated,
	 * cutoff xids in local memory wrapping around, and to have updated xmin
	 * horizons.
	 */
	PushActiveSnapshot(GetTransactionSnapshot());

	/*
	 * Check for user-requested abort.  Note we want this to be inside a
	 * transaction, so xact.c doesn't issue useless WARNING.
	 */
	CHECK_FOR_INTERRUPTS();

	/*
	 * Determine the type of lock we want --- hard exclusive lock for a FULL
	 * vacuum, but just ShareUpdateExclusiveLock for concurrent vacuum. Either
	 * way, we can be sure that no other backend is vacuuming the same table.
	 */
	lmode = (params.options & VACOPT_FULL) ?
		AccessExclusiveLock : ShareUpdateExclusiveLock;

	/* open the relation and get the appropriate lock on it */
	rel = vacuum_open_relation(relid, relation, params.options,
							   params.log_min_duration >= 0, lmode);

	/* leave if relation could not be opened or locked */
	if (!rel)
	{
		PopActiveSnapshot();
		CommitTransactionCommand();
		return false;
	}

	/*
	 * When recursing to a TOAST table, check privileges on the parent.  NB:
	 * This is only safe to do because we hold a session lock on the main
	 * relation that prevents concurrent deletion.
	 */
	if (OidIsValid(params.toast_parent))
		priv_relid = params.toast_parent;
	else
		priv_relid = RelationGetRelid(rel);

	/*
	 * Check if relation needs to be skipped based on privileges.  This check
	 * happens also when building the relation list to vacuum for a manual
	 * operation, and needs to be done additionally here as VACUUM could
	 * happen across multiple transactions where privileges could have changed
	 * in-between.  Make sure to only generate logs for VACUUM in this case.
	 */
	if (!vacuum_is_permitted_for_relation(priv_relid,
										  rel->rd_rel,
										  params.options & ~VACOPT_ANALYZE))
	{
		relation_close(rel, lmode);
		PopActiveSnapshot();
		CommitTransactionCommand();
		return false;
	}

	/*
	 * Check that it's of a vacuumable relkind.
	 */
	if (rel->rd_rel->relkind != RELKIND_RELATION &&
		rel->rd_rel->relkind != RELKIND_MATVIEW &&
		rel->rd_rel->relkind != RELKIND_TOASTVALUE &&
		rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
	{
		ereport(WARNING,
				(errmsg("skipping \"%s\" --- cannot vacuum non-tables or special system tables",
						RelationGetRelationName(rel))));
		relation_close(rel, lmode);
		PopActiveSnapshot();
		CommitTransactionCommand();
		return false;
	}

	/*
	 * Silently ignore tables that are temp tables of other backends ---
	 * trying to vacuum these will lead to great unhappiness, since their
	 * contents are probably not up-to-date on disk.  (We don't throw a
	 * warning here; it would just lead to chatter during a database-wide
	 * VACUUM.)
	 */
	if (RELATION_IS_OTHER_TEMP(rel))
	{
		relation_close(rel, lmode);
		PopActiveSnapshot();
		CommitTransactionCommand();
		return false;
	}

	/*
	 * Silently ignore partitioned tables as there is no work to be done.  The
	 * useful work is on their child partitions, which have been queued up for
	 * us separately.
	 */
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		relation_close(rel, lmode);
		PopActiveSnapshot();
		CommitTransactionCommand();
		/* It's OK to proceed with ANALYZE on this table */
		return true;
	}

	/*
	 * Get a session-level lock too. This will protect our access to the
	 * relation across multiple transactions, so that we can vacuum the
	 * relation's TOAST table (if any) secure in the knowledge that no one is
	 * deleting the parent relation.
	 *
	 * NOTE: this cannot block, even if someone else is waiting for access,
	 * because the lock manager knows that both lock requests are from the
	 * same process.
	 */
	lockrelid = rel->rd_lockInfo.lockRelId;
	LockRelationIdForSession(&lockrelid, lmode);

	/*
	 * Set index_cleanup option based on index_cleanup reloption if it wasn't
	 * specified in VACUUM command, or when running in an autovacuum worker
	 */
	if (params.index_cleanup == VACOPTVALUE_UNSPECIFIED)
	{
		StdRdOptIndexCleanup vacuum_index_cleanup;

		if (rel->rd_options == NULL)
			vacuum_index_cleanup = STDRD_OPTION_VACUUM_INDEX_CLEANUP_AUTO;
		else
			vacuum_index_cleanup =
				((StdRdOptions *) rel->rd_options)->vacuum_index_cleanup;

		if (vacuum_index_cleanup == STDRD_OPTION_VACUUM_INDEX_CLEANUP_AUTO)
			params.index_cleanup = VACOPTVALUE_AUTO;
		else if (vacuum_index_cleanup == STDRD_OPTION_VACUUM_INDEX_CLEANUP_ON)
			params.index_cleanup = VACOPTVALUE_ENABLED;
		else
		{
			Assert(vacuum_index_cleanup ==
				   STDRD_OPTION_VACUUM_INDEX_CLEANUP_OFF);
			params.index_cleanup = VACOPTVALUE_DISABLED;
		}
	}

#ifdef USE_INJECTION_POINTS
	if (params.index_cleanup == VACOPTVALUE_AUTO)
		INJECTION_POINT("vacuum-index-cleanup-auto", NULL);
	else if (params.index_cleanup == VACOPTVALUE_DISABLED)
		INJECTION_POINT("vacuum-index-cleanup-disabled", NULL);
	else if (params.index_cleanup == VACOPTVALUE_ENABLED)
		INJECTION_POINT("vacuum-index-cleanup-enabled", NULL);
#endif

	/*
	 * Check if the vacuum_max_eager_freeze_failure_rate table storage
	 * parameter was specified. This overrides the GUC value.
	 */
	if (rel->rd_options != NULL &&
		((StdRdOptions *) rel->rd_options)->vacuum_max_eager_freeze_failure_rate >= 0)
		params.max_eager_freeze_failure_rate =
			((StdRdOptions *) rel->rd_options)->vacuum_max_eager_freeze_failure_rate;

	/*
	 * Set truncate option based on truncate reloption or GUC if it wasn't
	 * specified in VACUUM command, or when running in an autovacuum worker
	 */
	if (params.truncate == VACOPTVALUE_UNSPECIFIED)
	{
		StdRdOptions *opts = (StdRdOptions *) rel->rd_options;

		if (opts && opts->vacuum_truncate_set)
		{
			if (opts->vacuum_truncate)
				params.truncate = VACOPTVALUE_ENABLED;
			else
				params.truncate = VACOPTVALUE_DISABLED;
		}
		else if (vacuum_truncate)
			params.truncate = VACOPTVALUE_ENABLED;
		else
			params.truncate = VACOPTVALUE_DISABLED;
	}

#ifdef USE_INJECTION_POINTS
	if (params.truncate == VACOPTVALUE_AUTO)
		INJECTION_POINT("vacuum-truncate-auto", NULL);
	else if (params.truncate == VACOPTVALUE_DISABLED)
		INJECTION_POINT("vacuum-truncate-disabled", NULL);
	else if (params.truncate == VACOPTVALUE_ENABLED)
		INJECTION_POINT("vacuum-truncate-enabled", NULL);
#endif

	/*
	 * Remember the relation's TOAST relation for later, if the caller asked
	 * us to process it.  In VACUUM FULL, though, the toast table is
	 * automatically rebuilt by cluster_rel so we shouldn't recurse to it,
	 * unless PROCESS_MAIN is disabled.
	 */
	if ((params.options & VACOPT_PROCESS_TOAST) != 0 &&
		((params.options & VACOPT_FULL) == 0 ||
		 (params.options & VACOPT_PROCESS_MAIN) == 0))
		toast_relid = rel->rd_rel->reltoastrelid;
	else
		toast_relid = InvalidOid;

	/*
	 * Switch to the table owner's userid, so that any index functions are run
	 * as that user.  Also lock down security-restricted operations and
	 * arrange to make GUC variable changes local to this command. (This is
	 * unnecessary, but harmless, for lazy VACUUM.)
	 */
	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(rel->rd_rel->relowner,
						   save_sec_context | SECURITY_RESTRICTED_OPERATION);
	save_nestlevel = NewGUCNestLevel();
	RestrictSearchPath();

	/*
	 * If PROCESS_MAIN is set (the default), it's time to vacuum the main
	 * relation.  Otherwise, we can skip this part.  If processing the TOAST
	 * table is required (e.g., PROCESS_TOAST is set), we force PROCESS_MAIN
	 * to be set when we recurse to the TOAST table.
	 */
	if (params.options & VACOPT_PROCESS_MAIN)
	{
		/*
		 * Do the actual work --- either FULL or "lazy" vacuum
		 */
		if (params.options & VACOPT_FULL)
		{
			ClusterParams cluster_params = {0};

			if ((params.options & VACOPT_VERBOSE) != 0)
				cluster_params.options |= CLUOPT_VERBOSE;

			/* VACUUM FULL is now a variant of CLUSTER; see cluster.c */
			cluster_rel(rel, InvalidOid, &cluster_params);
			/* cluster_rel closes the relation, but keeps lock */

			rel = NULL;
		}
		else
			table_relation_vacuum(rel, params, bstrategy);
	}

	/* Roll back any GUC changes executed by index functions */
	AtEOXact_GUC(false, save_nestlevel);

	/* Restore userid and security context */
	SetUserIdAndSecContext(save_userid, save_sec_context);

	/* all done with this class, but hold lock until commit */
	if (rel)
		relation_close(rel, NoLock);

	/*
	 * Complete the transaction and free all temporary memory used.
	 */
	PopActiveSnapshot();
	CommitTransactionCommand();

	/*
	 * If the relation has a secondary toast rel, vacuum that too while we
	 * still hold the session lock on the main table.  Note however that
	 * "analyze" will not get done on the toast table.  This is good, because
	 * the toaster always uses hardcoded index access and statistics are
	 * totally unimportant for toast relations.
	 */
	if (toast_relid != InvalidOid)
	{
		/*
		 * Force VACOPT_PROCESS_MAIN so vacuum_rel() processes it.  Likewise,
		 * set toast_parent so that the privilege checks are done on the main
		 * relation.  NB: This is only safe to do because we hold a session
		 * lock on the main relation that prevents concurrent deletion.
		 */
		toast_vacuum_params.options |= VACOPT_PROCESS_MAIN;
		toast_vacuum_params.toast_parent = relid;

		vacuum_rel(toast_relid, NULL, toast_vacuum_params, bstrategy);
	}

	/*
	 * Now release the session-level lock on the main table.
	 */
	UnlockRelationIdForSession(&lockrelid, lmode);

	/* Report that we really did it. */
	return true;
}


/*
 * Open all the vacuumable indexes of the given relation, obtaining the
 * specified kind of lock on each.  Return an array of Relation pointers for
 * the indexes into *Irel, and the number of indexes into *nindexes.
 *
 * We consider an index vacuumable if it is marked insertable (indisready).
 * If it isn't, probably a CREATE INDEX CONCURRENTLY command failed early in
 * execution, and what we have is too corrupt to be processable.  We will
 * vacuum even if the index isn't indisvalid; this is important because in a
 * unique index, uniqueness checks will be performed anyway and had better not
 * hit dangling index pointers.
 */
void
vac_open_indexes(Relation relation, LOCKMODE lockmode,
				 int *nindexes, Relation **Irel)
{
	List	   *indexoidlist;
	ListCell   *indexoidscan;
	int			i;

	Assert(lockmode != NoLock);

	indexoidlist = RelationGetIndexList(relation);

	/* allocate enough memory for all indexes */
	i = list_length(indexoidlist);

	if (i > 0)
		*Irel = (Relation *) palloc(i * sizeof(Relation));
	else
		*Irel = NULL;

	/* collect just the ready indexes */
	i = 0;
	foreach(indexoidscan, indexoidlist)
	{
		Oid			indexoid = lfirst_oid(indexoidscan);
		Relation	indrel;

		indrel = index_open(indexoid, lockmode);
		if (indrel->rd_index->indisready)
			(*Irel)[i++] = indrel;
		else
			index_close(indrel, lockmode);
	}

	*nindexes = i;

	list_free(indexoidlist);
}

/*
 * Release the resources acquired by vac_open_indexes.  Optionally release
 * the locks (say NoLock to keep 'em).
 */
void
vac_close_indexes(int nindexes, Relation *Irel, LOCKMODE lockmode)
{
	if (Irel == NULL)
		return;

	while (nindexes--)
	{
		Relation	ind = Irel[nindexes];

		index_close(ind, lockmode);
	}
	pfree(Irel);
}

/*
 * vacuum_delay_point --- check for interrupts and cost-based delay.
 *
 * This should be called in each major loop of VACUUM processing,
 * typically once per page processed.
 */
void
vacuum_delay_point(bool is_analyze)
{
	double		msec = 0;

	/* Always check for interrupts */
	CHECK_FOR_INTERRUPTS();

	if (InterruptPending ||
		(!VacuumCostActive && !ConfigReloadPending))
		return;

	/*
	 * Autovacuum workers should reload the configuration file if requested.
	 * This allows changes to [autovacuum_]vacuum_cost_limit and
	 * [autovacuum_]vacuum_cost_delay to take effect while a table is being
	 * vacuumed or analyzed.
	 */
	if (ConfigReloadPending && AmAutoVacuumWorkerProcess())
	{
		ConfigReloadPending = false;
		ProcessConfigFile(PGC_SIGHUP);
		VacuumUpdateCosts();
	}

	/*
	 * If we disabled cost-based delays after reloading the config file,
	 * return.
	 */
	if (!VacuumCostActive)
		return;

	/*
	 * For parallel vacuum, the delay is computed based on the shared cost
	 * balance.  See compute_parallel_delay.
	 */
	if (VacuumSharedCostBalance != NULL)
		msec = compute_parallel_delay();
	else if (VacuumCostBalance >= vacuum_cost_limit)
		msec = vacuum_cost_delay * VacuumCostBalance / vacuum_cost_limit;

	/* Nap if appropriate */
	if (msec > 0)
	{
		instr_time	delay_start;

		if (msec > vacuum_cost_delay * 4)
			msec = vacuum_cost_delay * 4;

		if (track_cost_delay_timing)
			INSTR_TIME_SET_CURRENT(delay_start);

		pgstat_report_wait_start(WAIT_EVENT_VACUUM_DELAY);
		pg_usleep(msec * 1000);
		pgstat_report_wait_end();

		if (track_cost_delay_timing)
		{
			instr_time	delay_end;
			instr_time	delay;

			INSTR_TIME_SET_CURRENT(delay_end);
			INSTR_TIME_SET_ZERO(delay);
			INSTR_TIME_ACCUM_DIFF(delay, delay_end, delay_start);

			/*
			 * For parallel workers, we only report the delay time every once
			 * in a while to avoid overloading the leader with messages and
			 * interrupts.
			 */
			if (IsParallelWorker())
			{
				static instr_time last_report_time;
				instr_time	time_since_last_report;

				Assert(!is_analyze);

				/* Accumulate the delay time */
				parallel_vacuum_worker_delay_ns += INSTR_TIME_GET_NANOSEC(delay);

				/* Calculate interval since last report */
				INSTR_TIME_SET_ZERO(time_since_last_report);
				INSTR_TIME_ACCUM_DIFF(time_since_last_report, delay_end, last_report_time);

				/* If we haven't reported in a while, do so now */
				if (INSTR_TIME_GET_NANOSEC(time_since_last_report) >=
					PARALLEL_VACUUM_DELAY_REPORT_INTERVAL_NS)
				{
					pgstat_progress_parallel_incr_param(PROGRESS_VACUUM_DELAY_TIME,
														parallel_vacuum_worker_delay_ns);

					/* Reset variables */
					last_report_time = delay_end;
					parallel_vacuum_worker_delay_ns = 0;
				}
			}
			else if (is_analyze)
				pgstat_progress_incr_param(PROGRESS_ANALYZE_DELAY_TIME,
										   INSTR_TIME_GET_NANOSEC(delay));
			else
				pgstat_progress_incr_param(PROGRESS_VACUUM_DELAY_TIME,
										   INSTR_TIME_GET_NANOSEC(delay));
		}

		/*
		 * We don't want to ignore postmaster death during very long vacuums
		 * with vacuum_cost_delay configured.  We can't use the usual
		 * WaitLatch() approach here because we want microsecond-based sleep
		 * durations above.
		 */
		if (IsUnderPostmaster && !PostmasterIsAlive())
			exit(1);

		VacuumCostBalance = 0;

		/*
		 * Balance and update limit values for autovacuum workers. We must do
		 * this periodically, as the number of workers across which we are
		 * balancing the limit may have changed.
		 *
		 * TODO: There may be better criteria for determining when to do this
		 * besides "check after napping".
		 */
		AutoVacuumUpdateCostLimit();

		/* Might have gotten an interrupt while sleeping */
		CHECK_FOR_INTERRUPTS();
	}
}

/*
 * Computes the vacuum delay for parallel workers.
 *
 * The basic idea of a cost-based delay for parallel vacuum is to allow each
 * worker to sleep in proportion to the share of work it's done.  We achieve this
 * by allowing all parallel vacuum workers including the leader process to
 * have a shared view of cost related parameters (mainly VacuumCostBalance).
 * We allow each worker to update it as and when it has incurred any cost and
 * then based on that decide whether it needs to sleep.  We compute the time
 * to sleep for a worker based on the cost it has incurred
 * (VacuumCostBalanceLocal) and then reduce the VacuumSharedCostBalance by
 * that amount.  This avoids putting to sleep those workers which have done less
 * I/O than other workers and therefore ensure that workers
 * which are doing more I/O got throttled more.
 *
 * We allow a worker to sleep only if it has performed I/O above a certain
 * threshold, which is calculated based on the number of active workers
 * (VacuumActiveNWorkers), and the overall cost balance is more than
 * VacuumCostLimit set by the system.  Testing reveals that we achieve
 * the required throttling if we force a worker that has done more than 50%
 * of its share of work to sleep.
 */
static double
compute_parallel_delay(void)
{
	double		msec = 0;
	uint32		shared_balance;
	int			nworkers;

	/* Parallel vacuum must be active */
	Assert(VacuumSharedCostBalance);

	nworkers = pg_atomic_read_u32(VacuumActiveNWorkers);

	/* At least count itself */
	Assert(nworkers >= 1);

	/* Update the shared cost balance value atomically */
	shared_balance = pg_atomic_add_fetch_u32(VacuumSharedCostBalance, VacuumCostBalance);

	/* Compute the total local balance for the current worker */
	VacuumCostBalanceLocal += VacuumCostBalance;

	if ((shared_balance >= vacuum_cost_limit) &&
		(VacuumCostBalanceLocal > 0.5 * ((double) vacuum_cost_limit / nworkers)))
	{
		/* Compute sleep time based on the local cost balance */
		msec = vacuum_cost_delay * VacuumCostBalanceLocal / vacuum_cost_limit;
		pg_atomic_sub_fetch_u32(VacuumSharedCostBalance, VacuumCostBalanceLocal);
		VacuumCostBalanceLocal = 0;
	}

	/*
	 * Reset the local balance as we accumulated it into the shared value.
	 */
	VacuumCostBalance = 0;

	return msec;
}

/*
 * A wrapper function of defGetBoolean().
 *
 * This function returns VACOPTVALUE_ENABLED and VACOPTVALUE_DISABLED instead
 * of true and false.
 */
static VacOptValue
get_vacoptval_from_boolean(DefElem *def)
{
	return defGetBoolean(def) ? VACOPTVALUE_ENABLED : VACOPTVALUE_DISABLED;
}

/*
 *	vac_bulkdel_one_index() -- bulk-deletion for index relation.
 *
 * Returns bulk delete stats derived from input stats
 */
IndexBulkDeleteResult *
vac_bulkdel_one_index(IndexVacuumInfo *ivinfo, IndexBulkDeleteResult *istat,
					  TidStore *dead_items, VacDeadItemsInfo *dead_items_info)
{
	/* Do bulk deletion */
	istat = index_bulk_delete(ivinfo, istat, vac_tid_reaped,
							  dead_items);

	ereport(ivinfo->message_level,
			(errmsg("scanned index \"%s\" to remove %" PRId64 " row versions",
					RelationGetRelationName(ivinfo->index),
					dead_items_info->num_items)));

	return istat;
}

/*
 *	vac_cleanup_one_index() -- do post-vacuum cleanup for index relation.
 *
 * Returns bulk delete stats derived from input stats
 */
IndexBulkDeleteResult *
vac_cleanup_one_index(IndexVacuumInfo *ivinfo, IndexBulkDeleteResult *istat)
{
	istat = index_vacuum_cleanup(ivinfo, istat);

	if (istat)
		ereport(ivinfo->message_level,
				(errmsg("index \"%s\" now contains %.0f row versions in %u pages",
						RelationGetRelationName(ivinfo->index),
						istat->num_index_tuples,
						istat->num_pages),
				 errdetail("%.0f index row versions were removed.\n"
						   "%u index pages were newly deleted.\n"
						   "%u index pages are currently deleted, of which %u are currently reusable.",
						   istat->tuples_removed,
						   istat->pages_newly_deleted,
						   istat->pages_deleted, istat->pages_free)));

	return istat;
}

/*
 *	vac_tid_reaped() -- is a particular tid deletable?
 *
 *		This has the right signature to be an IndexBulkDeleteCallback.
 */
static bool
vac_tid_reaped(ItemPointer itemptr, void *state)
{
	TidStore   *dead_items = (TidStore *) state;

	return TidStoreIsMember(dead_items, itemptr);
}

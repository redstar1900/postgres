/*------------------------------------------------------------------------
 *
 * regress.c
 *	 Code for various C-language functions defined as part of the
 *	 regression tests.
 *
 * This code is released under the terms of the PostgreSQL License.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/test/regress/regress.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <math.h>
#include <signal.h>

#include "access/detoast.h"
#include "access/htup_details.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "commands/sequence.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/supportnodes.h"
#include "optimizer/optimizer.h"
#include "optimizer/plancat.h"
#include "parser/parse_coerce.h"
#include "port/atomics.h"
#include "postmaster/postmaster.h"	/* for MAX_BACKENDS */
#include "storage/checksum.h"
#include "storage/spin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/geo_decls.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/typcache.h"

#define EXPECT_TRUE(expr)	\
	do { \
		if (!(expr)) \
			elog(ERROR, \
				 "%s was unexpectedly false in file \"%s\" line %u", \
				 #expr, __FILE__, __LINE__); \
	} while (0)

#define EXPECT_EQ_U32(result_expr, expected_expr)	\
	do { \
		uint32		actual_result = (result_expr); \
		uint32		expected_result = (expected_expr); \
		if (actual_result != expected_result) \
			elog(ERROR, \
				 "%s yielded %u, expected %s in file \"%s\" line %u", \
				 #result_expr, actual_result, #expected_expr, __FILE__, __LINE__); \
	} while (0)

#define EXPECT_EQ_U64(result_expr, expected_expr)	\
	do { \
		uint64		actual_result = (result_expr); \
		uint64		expected_result = (expected_expr); \
		if (actual_result != expected_result) \
			elog(ERROR, \
				 "%s yielded " UINT64_FORMAT ", expected %s in file \"%s\" line %u", \
				 #result_expr, actual_result, #expected_expr, __FILE__, __LINE__); \
	} while (0)

#define LDELIM			'('
#define RDELIM			')'
#define DELIM			','

static void regress_lseg_construct(LSEG *lseg, Point *pt1, Point *pt2);

PG_MODULE_MAGIC_EXT(
					.name = "regress",
					.version = PG_VERSION
);


/* return the point where two paths intersect, or NULL if no intersection. */
PG_FUNCTION_INFO_V1(interpt_pp);

Datum
interpt_pp(PG_FUNCTION_ARGS)
{
	PATH	   *p1 = PG_GETARG_PATH_P(0);
	PATH	   *p2 = PG_GETARG_PATH_P(1);
	int			i,
				j;
	LSEG		seg1,
				seg2;
	bool		found;			/* We've found the intersection */

	found = false;				/* Haven't found it yet */

	for (i = 0; i < p1->npts - 1 && !found; i++)
	{
		regress_lseg_construct(&seg1, &p1->p[i], &p1->p[i + 1]);
		for (j = 0; j < p2->npts - 1 && !found; j++)
		{
			regress_lseg_construct(&seg2, &p2->p[j], &p2->p[j + 1]);
			if (DatumGetBool(DirectFunctionCall2(lseg_intersect,
												 LsegPGetDatum(&seg1),
												 LsegPGetDatum(&seg2))))
				found = true;
		}
	}

	if (!found)
		PG_RETURN_NULL();

	/*
	 * Note: DirectFunctionCall2 will kick out an error if lseg_interpt()
	 * returns NULL, but that should be impossible since we know the two
	 * segments intersect.
	 */
	PG_RETURN_DATUM(DirectFunctionCall2(lseg_interpt,
										LsegPGetDatum(&seg1),
										LsegPGetDatum(&seg2)));
}


/* like lseg_construct, but assume space already allocated */
static void
regress_lseg_construct(LSEG *lseg, Point *pt1, Point *pt2)
{
	lseg->p[0].x = pt1->x;
	lseg->p[0].y = pt1->y;
	lseg->p[1].x = pt2->x;
	lseg->p[1].y = pt2->y;
}

PG_FUNCTION_INFO_V1(overpaid);

Datum
overpaid(PG_FUNCTION_ARGS)
{
	HeapTupleHeader tuple = PG_GETARG_HEAPTUPLEHEADER(0);
	bool		isnull;
	int32		salary;

	salary = DatumGetInt32(GetAttributeByName(tuple, "salary", &isnull));
	if (isnull)
		PG_RETURN_NULL();
	PG_RETURN_BOOL(salary > 699);
}

/* New type "widget"
 * This used to be "circle", but I added circle to builtins,
 *	so needed to make sure the names do not collide. - tgl 97/04/21
 */

typedef struct
{
	Point		center;
	double		radius;
} WIDGET;

PG_FUNCTION_INFO_V1(widget_in);
PG_FUNCTION_INFO_V1(widget_out);

#define NARGS	3

Datum
widget_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	char	   *p,
			   *coord[NARGS];
	int			i;
	WIDGET	   *result;

	for (i = 0, p = str; *p && i < NARGS && *p != RDELIM; p++)
	{
		if (*p == DELIM || (*p == LDELIM && i == 0))
			coord[i++] = p + 1;
	}

	/*
	 * Note: DON'T convert this error to "soft" style (errsave/ereturn).  We
	 * want this data type to stay permanently in the hard-error world so that
	 * it can be used for testing that such cases still work reasonably.
	 */
	if (i < NARGS)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s: \"%s\"",
						"widget", str)));

	result = (WIDGET *) palloc(sizeof(WIDGET));
	result->center.x = atof(coord[0]);
	result->center.y = atof(coord[1]);
	result->radius = atof(coord[2]);

	PG_RETURN_POINTER(result);
}

Datum
widget_out(PG_FUNCTION_ARGS)
{
	WIDGET	   *widget = (WIDGET *) PG_GETARG_POINTER(0);
	char	   *str = psprintf("(%g,%g,%g)",
							   widget->center.x, widget->center.y, widget->radius);

	PG_RETURN_CSTRING(str);
}

PG_FUNCTION_INFO_V1(pt_in_widget);

Datum
pt_in_widget(PG_FUNCTION_ARGS)
{
	Point	   *point = PG_GETARG_POINT_P(0);
	WIDGET	   *widget = (WIDGET *) PG_GETARG_POINTER(1);
	float8		distance;

	distance = DatumGetFloat8(DirectFunctionCall2(point_distance,
												  PointPGetDatum(point),
												  PointPGetDatum(&widget->center)));

	PG_RETURN_BOOL(distance < widget->radius);
}

PG_FUNCTION_INFO_V1(reverse_name);

Datum
reverse_name(PG_FUNCTION_ARGS)
{
	char	   *string = PG_GETARG_CSTRING(0);
	int			i;
	int			len;
	char	   *new_string;

	new_string = palloc0(NAMEDATALEN);
	for (i = 0; i < NAMEDATALEN && string[i]; ++i)
		;
	if (i == NAMEDATALEN || !string[i])
		--i;
	len = i;
	for (; i >= 0; --i)
		new_string[len - i] = string[i];
	PG_RETURN_CSTRING(new_string);
}

PG_FUNCTION_INFO_V1(trigger_return_old);

Datum
trigger_return_old(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	HeapTuple	tuple;

	if (!CALLED_AS_TRIGGER(fcinfo))
		elog(ERROR, "trigger_return_old: not fired by trigger manager");

	tuple = trigdata->tg_trigtuple;

	return PointerGetDatum(tuple);
}


/*
 * Type int44 has no real-world use, but the regression tests use it
 * (under the alias "city_budget").  It's a four-element vector of int4's.
 */

/*
 *		int44in			- converts "num, num, ..." to internal form
 *
 *		Note: Fills any missing positions with zeroes.
 */
PG_FUNCTION_INFO_V1(int44in);

Datum
int44in(PG_FUNCTION_ARGS)
{
	char	   *input_string = PG_GETARG_CSTRING(0);
	int32	   *result = (int32 *) palloc(4 * sizeof(int32));
	int			i;

	i = sscanf(input_string,
			   "%d, %d, %d, %d",
			   &result[0],
			   &result[1],
			   &result[2],
			   &result[3]);
	while (i < 4)
		result[i++] = 0;

	PG_RETURN_POINTER(result);
}

/*
 *		int44out		- converts internal form to "num, num, ..."
 */
PG_FUNCTION_INFO_V1(int44out);

Datum
int44out(PG_FUNCTION_ARGS)
{
	int32	   *an_array = (int32 *) PG_GETARG_POINTER(0);
	char	   *result = (char *) palloc(16 * 4);

	snprintf(result, 16 * 4, "%d,%d,%d,%d",
			 an_array[0],
			 an_array[1],
			 an_array[2],
			 an_array[3]);

	PG_RETURN_CSTRING(result);
}

PG_FUNCTION_INFO_V1(test_canonicalize_path);
Datum
test_canonicalize_path(PG_FUNCTION_ARGS)
{
	char	   *path = text_to_cstring(PG_GETARG_TEXT_PP(0));

	canonicalize_path(path);
	PG_RETURN_TEXT_P(cstring_to_text(path));
}

PG_FUNCTION_INFO_V1(make_tuple_indirect);
Datum
make_tuple_indirect(PG_FUNCTION_ARGS)
{
	HeapTupleHeader rec = PG_GETARG_HEAPTUPLEHEADER(0);
	HeapTupleData tuple;
	int			ncolumns;
	Datum	   *values;
	bool	   *nulls;

	Oid			tupType;
	int32		tupTypmod;
	TupleDesc	tupdesc;

	HeapTuple	newtup;

	int			i;

	MemoryContext old_context;

	/* Extract type info from the tuple itself */
	tupType = HeapTupleHeaderGetTypeId(rec);
	tupTypmod = HeapTupleHeaderGetTypMod(rec);
	tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);
	ncolumns = tupdesc->natts;

	/* Build a temporary HeapTuple control structure */
	tuple.t_len = HeapTupleHeaderGetDatumLength(rec);
	ItemPointerSetInvalid(&(tuple.t_self));
	tuple.t_tableOid = InvalidOid;
	HeapTupleSetZeroXids(&tuple);
	tuple.t_data = rec;

	values = (Datum *) palloc(ncolumns * sizeof(Datum));
	nulls = (bool *) palloc(ncolumns * sizeof(bool));

	heap_deform_tuple(&tuple, tupdesc, values, nulls);

	old_context = MemoryContextSwitchTo(TopTransactionContext);

	for (i = 0; i < ncolumns; i++)
	{
		struct varlena *attr;
		struct varlena *new_attr;
		struct varatt_indirect redirect_pointer;

		/* only work on existing, not-null varlenas */
		if (TupleDescAttr(tupdesc, i)->attisdropped ||
			nulls[i] ||
			TupleDescAttr(tupdesc, i)->attlen != -1 ||
			TupleDescAttr(tupdesc, i)->attstorage == TYPSTORAGE_PLAIN)
			continue;

		attr = (struct varlena *) DatumGetPointer(values[i]);

		/* don't recursively indirect */
		if (VARATT_IS_EXTERNAL_INDIRECT(attr))
			continue;

		/* copy datum, so it still lives later */
		if (VARATT_IS_EXTERNAL_ONDISK(attr))
			attr = detoast_external_attr(attr);
		else
		{
			struct varlena *oldattr = attr;

			attr = palloc0(VARSIZE_ANY(oldattr));
			memcpy(attr, oldattr, VARSIZE_ANY(oldattr));
		}

		/* build indirection Datum */
		new_attr = (struct varlena *) palloc0(INDIRECT_POINTER_SIZE);
		redirect_pointer.pointer = attr;
		SET_VARTAG_EXTERNAL(new_attr, VARTAG_INDIRECT);
		memcpy(VARDATA_EXTERNAL(new_attr), &redirect_pointer,
			   sizeof(redirect_pointer));

		values[i] = PointerGetDatum(new_attr);
	}

	newtup = heap_form_tuple(tupdesc, values, nulls);
	pfree(values);
	pfree(nulls);
	ReleaseTupleDesc(tupdesc);

	MemoryContextSwitchTo(old_context);

	/*
	 * We intentionally don't use PG_RETURN_HEAPTUPLEHEADER here, because that
	 * would cause the indirect toast pointers to be flattened out of the
	 * tuple immediately, rendering subsequent testing irrelevant.  So just
	 * return the HeapTupleHeader pointer as-is.  This violates the general
	 * rule that composite Datums shouldn't contain toast pointers, but so
	 * long as the regression test scripts don't insert the result of this
	 * function into a container type (record, array, etc) it should be OK.
	 */
	PG_RETURN_POINTER(newtup->t_data);
}

PG_FUNCTION_INFO_V1(get_environ);

Datum
get_environ(PG_FUNCTION_ARGS)
{
#if !defined(WIN32) || defined(_MSC_VER)
	extern char **environ;
#endif
	int			nvals = 0;
	ArrayType  *result;
	Datum	   *env;

	for (char **s = environ; *s; s++)
		nvals++;

	env = palloc(nvals * sizeof(Datum));

	for (int i = 0; i < nvals; i++)
		env[i] = CStringGetTextDatum(environ[i]);

	result = construct_array_builtin(env, nvals, TEXTOID);

	PG_RETURN_POINTER(result);
}

PG_FUNCTION_INFO_V1(regress_setenv);

Datum
regress_setenv(PG_FUNCTION_ARGS)
{
	char	   *envvar = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *envval = text_to_cstring(PG_GETARG_TEXT_PP(1));

	if (!superuser())
		elog(ERROR, "must be superuser to change environment variables");

	if (setenv(envvar, envval, 1) != 0)
		elog(ERROR, "could not set environment variable: %m");

	PG_RETURN_VOID();
}

/* Sleep until no process has a given PID. */
PG_FUNCTION_INFO_V1(wait_pid);

Datum
wait_pid(PG_FUNCTION_ARGS)
{
	int			pid = PG_GETARG_INT32(0);

	if (!superuser())
		elog(ERROR, "must be superuser to check PID liveness");

	while (kill(pid, 0) == 0)
	{
		CHECK_FOR_INTERRUPTS();
		pg_usleep(50000);
	}

	if (errno != ESRCH)
		elog(ERROR, "could not check PID %d liveness: %m", pid);

	PG_RETURN_VOID();
}

static void
test_atomic_flag(void)
{
	pg_atomic_flag flag;

	pg_atomic_init_flag(&flag);
	EXPECT_TRUE(pg_atomic_unlocked_test_flag(&flag));
	EXPECT_TRUE(pg_atomic_test_set_flag(&flag));
	EXPECT_TRUE(!pg_atomic_unlocked_test_flag(&flag));
	EXPECT_TRUE(!pg_atomic_test_set_flag(&flag));
	pg_atomic_clear_flag(&flag);
	EXPECT_TRUE(pg_atomic_unlocked_test_flag(&flag));
	EXPECT_TRUE(pg_atomic_test_set_flag(&flag));
	pg_atomic_clear_flag(&flag);
}

static void
test_atomic_uint32(void)
{
	pg_atomic_uint32 var;
	uint32		expected;
	int			i;

	pg_atomic_init_u32(&var, 0);
	EXPECT_EQ_U32(pg_atomic_read_u32(&var), 0);
	pg_atomic_write_u32(&var, 3);
	EXPECT_EQ_U32(pg_atomic_read_u32(&var), 3);
	EXPECT_EQ_U32(pg_atomic_fetch_add_u32(&var, pg_atomic_read_u32(&var) - 2),
				  3);
	EXPECT_EQ_U32(pg_atomic_fetch_sub_u32(&var, 1), 4);
	EXPECT_EQ_U32(pg_atomic_sub_fetch_u32(&var, 3), 0);
	EXPECT_EQ_U32(pg_atomic_add_fetch_u32(&var, 10), 10);
	EXPECT_EQ_U32(pg_atomic_exchange_u32(&var, 5), 10);
	EXPECT_EQ_U32(pg_atomic_exchange_u32(&var, 0), 5);

	/* test around numerical limits */
	EXPECT_EQ_U32(pg_atomic_fetch_add_u32(&var, INT_MAX), 0);
	EXPECT_EQ_U32(pg_atomic_fetch_add_u32(&var, INT_MAX), INT_MAX);
	pg_atomic_fetch_add_u32(&var, 2);	/* wrap to 0 */
	EXPECT_EQ_U32(pg_atomic_fetch_add_u32(&var, PG_INT16_MAX), 0);
	EXPECT_EQ_U32(pg_atomic_fetch_add_u32(&var, PG_INT16_MAX + 1),
				  PG_INT16_MAX);
	EXPECT_EQ_U32(pg_atomic_fetch_add_u32(&var, PG_INT16_MIN),
				  2 * PG_INT16_MAX + 1);
	EXPECT_EQ_U32(pg_atomic_fetch_add_u32(&var, PG_INT16_MIN - 1),
				  PG_INT16_MAX);
	pg_atomic_fetch_add_u32(&var, 1);	/* top up to UINT_MAX */
	EXPECT_EQ_U32(pg_atomic_read_u32(&var), UINT_MAX);
	EXPECT_EQ_U32(pg_atomic_fetch_sub_u32(&var, INT_MAX), UINT_MAX);
	EXPECT_EQ_U32(pg_atomic_read_u32(&var), (uint32) INT_MAX + 1);
	EXPECT_EQ_U32(pg_atomic_sub_fetch_u32(&var, INT_MAX), 1);
	pg_atomic_sub_fetch_u32(&var, 1);
	expected = PG_INT16_MAX;
	EXPECT_TRUE(!pg_atomic_compare_exchange_u32(&var, &expected, 1));
	expected = PG_INT16_MAX + 1;
	EXPECT_TRUE(!pg_atomic_compare_exchange_u32(&var, &expected, 1));
	expected = PG_INT16_MIN;
	EXPECT_TRUE(!pg_atomic_compare_exchange_u32(&var, &expected, 1));
	expected = PG_INT16_MIN - 1;
	EXPECT_TRUE(!pg_atomic_compare_exchange_u32(&var, &expected, 1));

	/* fail exchange because of old expected */
	expected = 10;
	EXPECT_TRUE(!pg_atomic_compare_exchange_u32(&var, &expected, 1));

	/* CAS is allowed to fail due to interrupts, try a couple of times */
	for (i = 0; i < 1000; i++)
	{
		expected = 0;
		if (!pg_atomic_compare_exchange_u32(&var, &expected, 1))
			break;
	}
	if (i == 1000)
		elog(ERROR, "atomic_compare_exchange_u32() never succeeded");
	EXPECT_EQ_U32(pg_atomic_read_u32(&var), 1);
	pg_atomic_write_u32(&var, 0);

	/* try setting flagbits */
	EXPECT_TRUE(!(pg_atomic_fetch_or_u32(&var, 1) & 1));
	EXPECT_TRUE(pg_atomic_fetch_or_u32(&var, 2) & 1);
	EXPECT_EQ_U32(pg_atomic_read_u32(&var), 3);
	/* try clearing flagbits */
	EXPECT_EQ_U32(pg_atomic_fetch_and_u32(&var, ~2) & 3, 3);
	EXPECT_EQ_U32(pg_atomic_fetch_and_u32(&var, ~1), 1);
	/* no bits set anymore */
	EXPECT_EQ_U32(pg_atomic_fetch_and_u32(&var, ~0), 0);
}

static void
test_atomic_uint64(void)
{
	pg_atomic_uint64 var;
	uint64		expected;
	int			i;

	pg_atomic_init_u64(&var, 0);
	EXPECT_EQ_U64(pg_atomic_read_u64(&var), 0);
	pg_atomic_write_u64(&var, 3);
	EXPECT_EQ_U64(pg_atomic_read_u64(&var), 3);
	EXPECT_EQ_U64(pg_atomic_fetch_add_u64(&var, pg_atomic_read_u64(&var) - 2),
				  3);
	EXPECT_EQ_U64(pg_atomic_fetch_sub_u64(&var, 1), 4);
	EXPECT_EQ_U64(pg_atomic_sub_fetch_u64(&var, 3), 0);
	EXPECT_EQ_U64(pg_atomic_add_fetch_u64(&var, 10), 10);
	EXPECT_EQ_U64(pg_atomic_exchange_u64(&var, 5), 10);
	EXPECT_EQ_U64(pg_atomic_exchange_u64(&var, 0), 5);

	/* fail exchange because of old expected */
	expected = 10;
	EXPECT_TRUE(!pg_atomic_compare_exchange_u64(&var, &expected, 1));

	/* CAS is allowed to fail due to interrupts, try a couple of times */
	for (i = 0; i < 100; i++)
	{
		expected = 0;
		if (!pg_atomic_compare_exchange_u64(&var, &expected, 1))
			break;
	}
	if (i == 100)
		elog(ERROR, "atomic_compare_exchange_u64() never succeeded");
	EXPECT_EQ_U64(pg_atomic_read_u64(&var), 1);

	pg_atomic_write_u64(&var, 0);

	/* try setting flagbits */
	EXPECT_TRUE(!(pg_atomic_fetch_or_u64(&var, 1) & 1));
	EXPECT_TRUE(pg_atomic_fetch_or_u64(&var, 2) & 1);
	EXPECT_EQ_U64(pg_atomic_read_u64(&var), 3);
	/* try clearing flagbits */
	EXPECT_EQ_U64((pg_atomic_fetch_and_u64(&var, ~2) & 3), 3);
	EXPECT_EQ_U64(pg_atomic_fetch_and_u64(&var, ~1), 1);
	/* no bits set anymore */
	EXPECT_EQ_U64(pg_atomic_fetch_and_u64(&var, ~0), 0);
}

/*
 * Perform, fairly minimal, testing of the spinlock implementation.
 *
 * It's likely worth expanding these to actually test concurrency etc, but
 * having some regularly run tests is better than none.
 */
static void
test_spinlock(void)
{
	/*
	 * Basic tests for spinlocks, as well as the underlying operations.
	 *
	 * We embed the spinlock in a struct with other members to test that the
	 * spinlock operations don't perform too wide writes.
	 */
	{
		struct test_lock_struct
		{
			char		data_before[4];
			slock_t		lock;
			char		data_after[4];
		}			struct_w_lock;

		memcpy(struct_w_lock.data_before, "abcd", 4);
		memcpy(struct_w_lock.data_after, "ef12", 4);

		/* test basic operations via the SpinLock* API */
		SpinLockInit(&struct_w_lock.lock);
		SpinLockAcquire(&struct_w_lock.lock);
		SpinLockRelease(&struct_w_lock.lock);

		/* test basic operations via underlying S_* API */
		S_INIT_LOCK(&struct_w_lock.lock);
		S_LOCK(&struct_w_lock.lock);
		S_UNLOCK(&struct_w_lock.lock);

		/* and that "contended" acquisition works */
		s_lock(&struct_w_lock.lock, "testfile", 17, "testfunc");
		S_UNLOCK(&struct_w_lock.lock);

		/*
		 * Check, using TAS directly, that a single spin cycle doesn't block
		 * when acquiring an already acquired lock.
		 */
#ifdef TAS
		S_LOCK(&struct_w_lock.lock);

		if (!TAS(&struct_w_lock.lock))
			elog(ERROR, "acquired already held spinlock");

#ifdef TAS_SPIN
		if (!TAS_SPIN(&struct_w_lock.lock))
			elog(ERROR, "acquired already held spinlock");
#endif							/* defined(TAS_SPIN) */

		S_UNLOCK(&struct_w_lock.lock);
#endif							/* defined(TAS) */

		/*
		 * Verify that after all of this the non-lock contents are still
		 * correct.
		 */
		if (memcmp(struct_w_lock.data_before, "abcd", 4) != 0)
			elog(ERROR, "padding before spinlock modified");
		if (memcmp(struct_w_lock.data_after, "ef12", 4) != 0)
			elog(ERROR, "padding after spinlock modified");
	}
}

PG_FUNCTION_INFO_V1(test_atomic_ops);
Datum
test_atomic_ops(PG_FUNCTION_ARGS)
{
	test_atomic_flag();

	test_atomic_uint32();

	test_atomic_uint64();

	/*
	 * Arguably this shouldn't be tested as part of this function, but it's
	 * closely enough related that that seems ok for now.
	 */
	test_spinlock();

	PG_RETURN_BOOL(true);
}

PG_FUNCTION_INFO_V1(test_fdw_handler);
Datum
test_fdw_handler(PG_FUNCTION_ARGS)
{
	elog(ERROR, "test_fdw_handler is not implemented");
	PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(is_catalog_text_unique_index_oid);
Datum
is_catalog_text_unique_index_oid(PG_FUNCTION_ARGS)
{
	return IsCatalogTextUniqueIndexOid(PG_GETARG_OID(0));
}

PG_FUNCTION_INFO_V1(test_support_func);
Datum
test_support_func(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);
	Node	   *ret = NULL;

	if (IsA(rawreq, SupportRequestSelectivity))
	{
		/*
		 * Assume that the target is int4eq; that's safe as long as we don't
		 * attach this to any other boolean-returning function.
		 */
		SupportRequestSelectivity *req = (SupportRequestSelectivity *) rawreq;
		Selectivity s1;

		if (req->is_join)
			s1 = join_selectivity(req->root, Int4EqualOperator,
								  req->args,
								  req->inputcollid,
								  req->jointype,
								  req->sjinfo);
		else
			s1 = restriction_selectivity(req->root, Int4EqualOperator,
										 req->args,
										 req->inputcollid,
										 req->varRelid);

		req->selectivity = s1;
		ret = (Node *) req;
	}

	if (IsA(rawreq, SupportRequestCost))
	{
		/* Provide some generic estimate */
		SupportRequestCost *req = (SupportRequestCost *) rawreq;

		req->startup = 0;
		req->per_tuple = 2 * cpu_operator_cost;
		ret = (Node *) req;
	}

	if (IsA(rawreq, SupportRequestRows))
	{
		/*
		 * Assume that the target is generate_series_int4; that's safe as long
		 * as we don't attach this to any other set-returning function.
		 */
		SupportRequestRows *req = (SupportRequestRows *) rawreq;

		if (req->node && IsA(req->node, FuncExpr))	/* be paranoid */
		{
			List	   *args = ((FuncExpr *) req->node)->args;
			Node	   *arg1 = linitial(args);
			Node	   *arg2 = lsecond(args);

			if (IsA(arg1, Const) &&
				!((Const *) arg1)->constisnull &&
				IsA(arg2, Const) &&
				!((Const *) arg2)->constisnull)
			{
				int32		val1 = DatumGetInt32(((Const *) arg1)->constvalue);
				int32		val2 = DatumGetInt32(((Const *) arg2)->constvalue);

				req->rows = val2 - val1 + 1;
				ret = (Node *) req;
			}
		}
	}

	PG_RETURN_POINTER(ret);
}

PG_FUNCTION_INFO_V1(test_opclass_options_func);
Datum
test_opclass_options_func(PG_FUNCTION_ARGS)
{
	PG_RETURN_NULL();
}

/* one-time tests for encoding infrastructure */
PG_FUNCTION_INFO_V1(test_enc_setup);
Datum
test_enc_setup(PG_FUNCTION_ARGS)
{
	/* Test pg_encoding_set_invalid() */
	for (int i = 0; i < _PG_LAST_ENCODING_; i++)
	{
		char		buf[2],
					bigbuf[16];
		int			len,
					mblen,
					valid;

		if (pg_encoding_max_length(i) == 1)
			continue;
		pg_encoding_set_invalid(i, buf);
		len = strnlen(buf, 2);
		if (len != 2)
			elog(WARNING,
				 "official invalid string for encoding \"%s\" has length %d",
				 pg_enc2name_tbl[i].name, len);
		mblen = pg_encoding_mblen(i, buf);
		if (mblen != 2)
			elog(WARNING,
				 "official invalid string for encoding \"%s\" has mblen %d",
				 pg_enc2name_tbl[i].name, mblen);
		valid = pg_encoding_verifymbstr(i, buf, len);
		if (valid != 0)
			elog(WARNING,
				 "official invalid string for encoding \"%s\" has valid prefix of length %d",
				 pg_enc2name_tbl[i].name, valid);
		valid = pg_encoding_verifymbstr(i, buf, 1);
		if (valid != 0)
			elog(WARNING,
				 "first byte of official invalid string for encoding \"%s\" has valid prefix of length %d",
				 pg_enc2name_tbl[i].name, valid);
		memset(bigbuf, ' ', sizeof(bigbuf));
		bigbuf[0] = buf[0];
		bigbuf[1] = buf[1];
		valid = pg_encoding_verifymbstr(i, bigbuf, sizeof(bigbuf));
		if (valid != 0)
			elog(WARNING,
				 "trailing data changed official invalid string for encoding \"%s\" to have valid prefix of length %d",
				 pg_enc2name_tbl[i].name, valid);
	}

	PG_RETURN_VOID();
}

/*
 * Call an encoding conversion or verification function.
 *
 * Arguments:
 *	string	  bytea -- string to convert
 *	src_enc	  name  -- source encoding
 *	dest_enc  name  -- destination encoding
 *	noError	  bool  -- if set, don't ereport() on invalid or untranslatable
 *					   input
 *
 * Result is a tuple with two attributes:
 *  int4	-- number of input bytes successfully converted
 *  bytea	-- converted string
 */
PG_FUNCTION_INFO_V1(test_enc_conversion);
Datum
test_enc_conversion(PG_FUNCTION_ARGS)
{
	bytea	   *string = PG_GETARG_BYTEA_PP(0);
	char	   *src_encoding_name = NameStr(*PG_GETARG_NAME(1));
	int			src_encoding = pg_char_to_encoding(src_encoding_name);
	char	   *dest_encoding_name = NameStr(*PG_GETARG_NAME(2));
	int			dest_encoding = pg_char_to_encoding(dest_encoding_name);
	bool		noError = PG_GETARG_BOOL(3);
	TupleDesc	tupdesc;
	char	   *src;
	char	   *dst;
	bytea	   *retval;
	Size		srclen;
	Size		dstsize;
	Oid			proc;
	int			convertedbytes;
	int			dstlen;
	Datum		values[2];
	bool		nulls[2] = {0};
	HeapTuple	tuple;

	if (src_encoding < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid source encoding name \"%s\"",
						src_encoding_name)));
	if (dest_encoding < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid destination encoding name \"%s\"",
						dest_encoding_name)));

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	tupdesc = BlessTupleDesc(tupdesc);

	srclen = VARSIZE_ANY_EXHDR(string);
	src = VARDATA_ANY(string);

	if (src_encoding == dest_encoding)
	{
		/* just check that the source string is valid */
		int			oklen;

		oklen = pg_encoding_verifymbstr(src_encoding, src, srclen);

		if (oklen == srclen)
		{
			convertedbytes = oklen;
			retval = string;
		}
		else if (!noError)
		{
			report_invalid_encoding(src_encoding, src + oklen, srclen - oklen);
		}
		else
		{
			/*
			 * build bytea data type structure.
			 */
			Assert(oklen < srclen);
			convertedbytes = oklen;
			retval = (bytea *) palloc(oklen + VARHDRSZ);
			SET_VARSIZE(retval, oklen + VARHDRSZ);
			memcpy(VARDATA(retval), src, oklen);
		}
	}
	else
	{
		proc = FindDefaultConversionProc(src_encoding, dest_encoding);
		if (!OidIsValid(proc))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("default conversion function for encoding \"%s\" to \"%s\" does not exist",
							pg_encoding_to_char(src_encoding),
							pg_encoding_to_char(dest_encoding))));

		if (srclen >= (MaxAllocSize / (Size) MAX_CONVERSION_GROWTH))
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("out of memory"),
					 errdetail("String of %d bytes is too long for encoding conversion.",
							   (int) srclen)));

		dstsize = (Size) srclen * MAX_CONVERSION_GROWTH + 1;
		dst = MemoryContextAlloc(CurrentMemoryContext, dstsize);

		/* perform conversion */
		convertedbytes = pg_do_encoding_conversion_buf(proc,
													   src_encoding,
													   dest_encoding,
													   (unsigned char *) src, srclen,
													   (unsigned char *) dst, dstsize,
													   noError);
		dstlen = strlen(dst);

		/*
		 * build bytea data type structure.
		 */
		retval = (bytea *) palloc(dstlen + VARHDRSZ);
		SET_VARSIZE(retval, dstlen + VARHDRSZ);
		memcpy(VARDATA(retval), dst, dstlen);

		pfree(dst);
	}

	values[0] = Int32GetDatum(convertedbytes);
	values[1] = PointerGetDatum(retval);
	tuple = heap_form_tuple(tupdesc, values, nulls);

	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/* Provide SQL access to IsBinaryCoercible() */
PG_FUNCTION_INFO_V1(binary_coercible);
Datum
binary_coercible(PG_FUNCTION_ARGS)
{
	Oid			srctype = PG_GETARG_OID(0);
	Oid			targettype = PG_GETARG_OID(1);

	PG_RETURN_BOOL(IsBinaryCoercible(srctype, targettype));
}

/*
 * Sanity checks for functions in relpath.h
 */
PG_FUNCTION_INFO_V1(test_relpath);
Datum
test_relpath(PG_FUNCTION_ARGS)
{
	RelPathStr	rpath;

	/*
	 * Verify that PROCNUMBER_CHARS and MAX_BACKENDS stay in sync.
	 * Unfortunately I don't know how to express that in a way suitable for a
	 * static assert.
	 */
	if ((int) ceil(log10(MAX_BACKENDS)) != PROCNUMBER_CHARS)
		elog(WARNING, "mismatch between MAX_BACKENDS and PROCNUMBER_CHARS");

	/* verify that the max-length relpath is generated ok */
	rpath = GetRelationPath(OID_MAX, OID_MAX, OID_MAX, MAX_BACKENDS - 1,
							INIT_FORKNUM);

	if (strlen(rpath.str) != REL_PATH_STR_MAXLEN)
		elog(WARNING, "maximum length relpath is if length %zu instead of %zu",
			 strlen(rpath.str), REL_PATH_STR_MAXLEN);

	PG_RETURN_VOID();
}

#include "access/hio.h"
#include "access/relation.h"
#include "storage/bufmgr.h"
#include "utils/rel.h"

static void
CheckNewPage(char *msg, Page page)
{
	uint16			size;

	if (PageGetPageLayoutVersion(page) != PG_PAGE_LAYOUT_VERSION)
		elog(ERROR, "%s: page version is %d, expected %d ",
			 msg, PageGetPageLayoutVersion(page), PG_PAGE_LAYOUT_VERSION);

	size = PageGetSpecialSize(page);
	if (size == MAXALIGN(sizeof(HeapPageSpecialData)))
		elog(INFO, "%s: page is converted to xid64 format", msg);
	else if (HeapPageIsDoubleXmax(page))
		elog(INFO, "%s: page is converted into double xmax format", msg);
	else
		elog(ERROR, "%s: converted page has pageSpecial size %u, expected %" PRIu64,
			 msg, size, MAXALIGN(sizeof(HeapPageSpecialData)));
}

/*
 * Get page from relation.
 * Make this page look like in 32-bit xid format.
 * Convert it to 64-bit xid format.
 * Run basic checks.
 */
PG_FUNCTION_INFO_V1(xid64_test_1);
Datum
xid64_test_1(PG_FUNCTION_ARGS)
{
	Oid					relid;
	Relation			rel;
	Buffer				buf;
	Page				page;
	PageHeader			hdr;

	relid = PG_GETARG_OID(0);
	rel = relation_open(relid, AccessExclusiveLock);
	buf = ReadBuffer(rel, 0);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

	page = BufferGetPage(buf);
	hdr = (PageHeader) page;

	if (PageGetSpecialSize(page) != MAXALIGN(sizeof(HeapPageSpecialData)))
		elog(ERROR, "page expected in new format");

	if (PageGetPageLayoutVersion(page) != PG_PAGE_LAYOUT_VERSION)
		elog(ERROR, "unknown page version (%u)",
			 PageGetPageLayoutVersion(page));

	hdr->pd_special = BLCKSZ;
	PageSetPageSizeAndVersion(page, BLCKSZ, PG_PAGE_LAYOUT_VERSION - 1);
	hdr->pd_checksum = pg_checksum_page((char *) page, 0);

	convert_page(rel, page, buf, 0);
	CheckNewPage("test 1", page);

	UnlockReleaseBuffer(buf);
	relation_close(rel, AccessExclusiveLock);

	PG_RETURN_VOID();
}

typedef struct TupleCheckValues
{
	TransactionId		xmin;
	TransactionId		xmax;
} TupleCheckValues;

typedef struct RelCheckValues
{
	TupleCheckValues   *tcv;
	Size				ntuples;
} RelCheckValues;

static RelCheckValues
FillRelCheckValues(Relation rel, Buffer buffer, Page page)
{
	RelCheckValues		set;
	Size				n;

#define DEFAULT_SET_SIZE 64
	n = DEFAULT_SET_SIZE;
	set.ntuples = 0;
	set.tcv = palloc(sizeof(set.tcv[0]) * n);

	{
		OffsetNumber		maxoff,
							offnum;
		HeapTupleHeader		tuphdr;
		ItemId				itemid;
		HeapTupleData		tuple;
		TransactionId		xmin,
							xmax;

		maxoff = PageGetMaxOffsetNumber(page);

		for (offnum = FirstOffsetNumber;
			 offnum <= maxoff;
			 offnum = OffsetNumberNext(offnum))
		{
			itemid = PageGetItemId(page, offnum);
			tuphdr = (HeapTupleHeader) PageGetItem(page, itemid);
			tuple.t_data = tuphdr;
			tuple.t_len = ItemIdGetLength(itemid);
			tuple.t_tableOid = RelationGetRelid(rel);

			if (HeapPageGetSpecial(page) == heapDoubleXmaxSpecial)
			{
				xmin = tuphdr->t_choice.t_heap.t_xmin;
				xmax = tuphdr->t_choice.t_heap.t_xmax;
			}
			else
			{
				HeapTupleCopyXidsFromPage(buffer, &tuple, page);

				xmin = HeapTupleGetRawXmin(&tuple);
				xmax = HeapTupleGetRawXmax(&tuple);
			}

			if (set.ntuples == n)
			{
				n *= 2;
				set.tcv = repalloc(set.tcv, sizeof(set.tcv[0]) * n);
			}

			set.tcv[set.ntuples].xmin = xmin;
			set.tcv[set.ntuples].xmax = xmax;
			set.ntuples++;
		}
	}

	return set;
}

/*
 * Test xmin/xmax invariant when converting page from 32bit xid to 64xid.
 *
 * Scenario:
 * - enforce all relation pages to 32bit xid format, discarding pd_xid_base and
 *   pd_multi_base
 * - store all xmin/xmax in array
 * - convert all the pages from relation into 64xid format
 * - store all new xmin/xmax in array
 * - compare old and new xmin/xmax
 *
 * NOTE: inital xid value does not affect test as pd_xid_base/pd_multi_base
 * discarded.
 */
PG_FUNCTION_INFO_V1(xid64_test_2);
Datum
xid64_test_2(PG_FUNCTION_ARGS)
{
	Oid					relid;
	Relation			rel;
	RelCheckValues		before,
						after;
	BlockNumber			pageno,
						npages;
	Size				i;

	relid = PG_GETARG_OID(0);
	rel = relation_open(relid, AccessExclusiveLock);
	npages = RelationGetNumberOfBlocks(rel);

	for (pageno = 0; pageno != npages; ++pageno)
	{
		Buffer			buf;
		Page			page;
		PageHeader		hdr;

		/* get page */
		buf = ReadBuffer(rel, pageno);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);
		hdr = (PageHeader) page;

		/* make page look like 32-bit xid page */
		hdr->pd_special = BLCKSZ;
		PageSetPageSizeAndVersion(page, BLCKSZ, PG_PAGE_LAYOUT_VERSION - 1);
		hdr->pd_checksum = pg_checksum_page((char *) page, pageno);

		before = FillRelCheckValues(rel, buf, page);
		convert_page(rel, page, buf, pageno);
		after = FillRelCheckValues(rel, buf, page);

		/* check */
		if (before.ntuples != after.ntuples)
			elog(ERROR, "numer of tuples must be equal");

		for (i = 0; i != before.ntuples; ++i)
		{
			if (before.tcv[i].xmin != after.tcv[i].xmin && after.tcv[i].xmin)
				elog(ERROR, "old and new xmin does not match (%" PRIu64 " != %" PRIu64 ")",
					 before.tcv[i].xmin, after.tcv[i].xmin);

			if (before.tcv[i].xmax != after.tcv[i].xmax)
				elog(ERROR, "old and new xmax does not match (%" PRIu64 " != %" PRIu64 ")",
					 before.tcv[i].xmax, after.tcv[i].xmax);
		}

		Assert(npages != 0);
		pfree(before.tcv);
		pfree(after.tcv);

		UnlockReleaseBuffer(buf);
	}

	relation_close(rel, AccessExclusiveLock);

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(xid64_test_double_xmax);
Datum
xid64_test_double_xmax(PG_FUNCTION_ARGS)
{
	Oid					relid;
	Relation			rel;
	BlockNumber			pageno,
						npages;
	bool				found;

	relid = PG_GETARG_OID(0);
	rel = relation_open(relid, AccessExclusiveLock);
	npages = RelationGetNumberOfBlocks(rel);
	found = false;

	for (pageno = 0; pageno != npages; ++pageno)
	{
		Buffer			buf;
		Page			page;
		PageHeader		hdr;
		ItemId			itemid;
		OffsetNumber 	offnum;
		HeapTupleHeader	tuphdr;

		buf = ReadBuffer(rel, pageno);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);
		hdr = (PageHeader) page;

		if (pageno == 0)
		{
			itemid = PageGetItemId(page, FirstOffsetNumber);
			itemid->lp_len += 16; /* Move to overlap special */
		}

		for (offnum = FirstOffsetNumber;
			 offnum <= PageGetMaxOffsetNumber(page);
			 offnum = OffsetNumberNext(offnum))
		{
			itemid = PageGetItemId(page, offnum);
			tuphdr = (HeapTupleHeader) PageGetItem(page, itemid);
			tuphdr->t_infomask |= HEAP_XMIN_COMMITTED;
		}

		hdr->pd_special = BLCKSZ;
		PageSetPageSizeAndVersion(page, BLCKSZ, PG_PAGE_LAYOUT_VERSION - 1);
		hdr->pd_checksum = pg_checksum_page((char *) page, pageno);

		convert_page(rel, page, buf, pageno);

		if (HeapPageIsDoubleXmax(page))
		{
			found = true;
			elog(INFO, "test double xmax: page %u is converted into double xmax format",
				 pageno);
		}

		UnlockReleaseBuffer(buf);
	}

	if (!found)
		elog(ERROR, "test double xmax: failed, no double xmax");

	Assert(npages != 0);
	elog(INFO, "test double xmax: end");

	relation_close(rel, AccessExclusiveLock);

	PG_RETURN_VOID();
}

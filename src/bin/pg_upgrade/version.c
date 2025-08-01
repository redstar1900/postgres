/*
 *	version.c
 *
 *	Postgres-version-specific routines
 *
 *	Copyright (c) 2010-2025, PostgreSQL Global Development Group
 *	src/bin/pg_upgrade/version.c
 */

#include "postgres_fe.h"

#include "access/transam.h"
#include "fe_utils/string_utils.h"
#include "pg_upgrade.h"

/*
 * version_hook functions for check_for_data_types_usage in order to determine
 * whether a data type check should be executed for the cluster in question or
 * not.
 */
bool
jsonb_9_4_check_applicable(ClusterInfo *cluster)
{
	/* JSONB changed its storage format during 9.4 beta */
	if (GET_MAJOR_VERSION(cluster->major_version) == 904 &&
		cluster->controldata.cat_ver < JSONB_FORMAT_CHANGE_CAT_VER)
		return true;

	return false;
}

/*
 * And another one for the xid type.
 */
bool
xid_check_applicable(ClusterInfo *cluster)
{
	/* xid type changed its storage format */
	if (cluster->controldata.cat_ver < XID_FORMATCHANGE_CAT_VER)
		return true;

	return false;
}

/*
 * old_9_6_invalidate_hash_indexes()
 *	9.6 -> 10
 *	Hash index binary format has changed from 9.6->10.0
 */
void
old_9_6_invalidate_hash_indexes(ClusterInfo *cluster, bool check_mode)
{
	int			dbnum;
	FILE	   *script = NULL;
	bool		found = false;
	char	   *output_path = "reindex_hash.sql";

	prep_status("Checking for hash indexes");

	for (dbnum = 0; dbnum < cluster->dbarr.ndbs; dbnum++)
	{
		PGresult   *res;
		bool		db_used = false;
		int			ntups;
		int			rowno;
		int			i_nspname,
					i_relname;
		DbInfo	   *active_db = &cluster->dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(cluster, active_db->db_name);

		/* find hash indexes */
		res = executeQueryOrDie(conn,
								"SELECT n.nspname, c.relname "
								"FROM	pg_catalog.pg_class c, "
								"		pg_catalog.pg_index i, "
								"		pg_catalog.pg_am a, "
								"		pg_catalog.pg_namespace n "
								"WHERE	i.indexrelid = c.oid AND "
								"		c.relam = a.oid AND "
								"		c.relnamespace = n.oid AND "
								"		a.amname = 'hash'"
			);

		ntups = PQntuples(res);
		i_nspname = PQfnumber(res, "nspname");
		i_relname = PQfnumber(res, "relname");
		for (rowno = 0; rowno < ntups; rowno++)
		{
			found = true;
			if (!check_mode)
			{
				if (script == NULL && (script = fopen_priv(output_path, "w")) == NULL)
					pg_fatal("could not open file \"%s\": %m", output_path);
				if (!db_used)
				{
					PQExpBufferData connectbuf;

					initPQExpBuffer(&connectbuf);
					appendPsqlMetaConnect(&connectbuf, active_db->db_name);
					fputs(connectbuf.data, script);
					termPQExpBuffer(&connectbuf);
					db_used = true;
				}
				fprintf(script, "REINDEX INDEX %s.%s;\n",
						quote_identifier(PQgetvalue(res, rowno, i_nspname)),
						quote_identifier(PQgetvalue(res, rowno, i_relname)));
			}
		}

		PQclear(res);

		if (!check_mode && db_used)
		{
			/* mark hash indexes as invalid */
			PQclear(executeQueryOrDie(conn,
									  "UPDATE pg_catalog.pg_index i "
									  "SET	indisvalid = false "
									  "FROM	pg_catalog.pg_class c, "
									  "		pg_catalog.pg_am a, "
									  "		pg_catalog.pg_namespace n "
									  "WHERE	i.indexrelid = c.oid AND "
									  "		c.relam = a.oid AND "
									  "		c.relnamespace = n.oid AND "
									  "		a.amname = 'hash'"));
		}

		PQfinish(conn);
	}

	if (script)
		fclose(script);

	if (found)
	{
		report_status(PG_WARNING, "warning");
		if (check_mode)
			pg_log(PG_WARNING, "\n"
				   "Your installation contains hash indexes.  These indexes have different\n"
				   "internal formats between your old and new clusters, so they must be\n"
				   "reindexed with the REINDEX command.  After upgrading, you will be given\n"
				   "REINDEX instructions.");
		else
			pg_log(PG_WARNING, "\n"
				   "Your installation contains hash indexes.  These indexes have different\n"
				   "internal formats between your old and new clusters, so they must be\n"
				   "reindexed with the REINDEX command.  The file\n"
				   "    %s\n"
				   "when executed by psql by the database superuser will recreate all invalid\n"
				   "indexes; until then, none of these indexes will be used.",
				   output_path);
	}
	else
		check_ok();
}

/*
 * Callback function for processing results of query for
 * report_extension_updates()'s UpgradeTask.  If the query returned any rows,
 * write the details to the report file.
 */
static void
process_extension_updates(DbInfo *dbinfo, PGresult *res, void *arg)
{
	int			ntups = PQntuples(res);
	int			i_name = PQfnumber(res, "name");
	UpgradeTaskReport *report = (UpgradeTaskReport *) arg;
	PQExpBufferData connectbuf;

	AssertVariableIsOfType(&process_extension_updates, UpgradeTaskProcessCB);

	if (ntups == 0)
		return;

	if (report->file == NULL &&
		(report->file = fopen_priv(report->path, "w")) == NULL)
		pg_fatal("could not open file \"%s\": %m", report->path);

	initPQExpBuffer(&connectbuf);
	appendPsqlMetaConnect(&connectbuf, dbinfo->db_name);
	fputs(connectbuf.data, report->file);
	termPQExpBuffer(&connectbuf);

	for (int rowno = 0; rowno < ntups; rowno++)
		fprintf(report->file, "ALTER EXTENSION %s UPDATE;\n",
				quote_identifier(PQgetvalue(res, rowno, i_name)));
}

/*
 * report_extension_updates()
 *	Report extensions that should be updated.
 */
void
report_extension_updates(ClusterInfo *cluster)
{
	UpgradeTaskReport report;
	UpgradeTask *task = upgrade_task_create();
	const char *query = "SELECT name "
		"FROM pg_available_extensions "
		"WHERE installed_version != default_version";

	prep_status("Checking for extension updates");

	report.file = NULL;
	strcpy(report.path, "update_extensions.sql");

	upgrade_task_add_step(task, query, process_extension_updates,
						  true, &report);

	upgrade_task_run(task, cluster);
	upgrade_task_free(task);

	if (report.file)
	{
		fclose(report.file);
		report_status(PG_REPORT, "notice");
		pg_log(PG_REPORT, "\n"
			   "Your installation contains extensions that should be updated\n"
			   "with the ALTER EXTENSION command.  The file\n"
			   "    %s\n"
			   "when executed by psql by the database superuser will update\n"
			   "these extensions.",
			   report.path);
	}
	else
		check_ok();
}

/*
 * invalidate_indexes()
 *	Invalidates all indexes satisfying given predicate.
 */
static void
invalidate_indexes(ClusterInfo *cluster, bool check_mode,
				   const char *name, const char *pred)
{
	int			dbnum;
	FILE	   *script = NULL;
	bool		found = false;
	char		output_path[MAXPGPATH];

	snprintf(output_path, sizeof(output_path), "reindex_%s.sql", name);

	prep_status("Checking for %s indexes", name);

	for (dbnum = 0; dbnum < cluster->dbarr.ndbs; dbnum++)
	{
		PGresult   *res;
		bool		db_used = false;
		int			ntups;
		int			rowno;
		int			i_nspname,
					i_relname;
		DbInfo	   *active_db = &cluster->dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(cluster, active_db->db_name);


		/*
		 * Find indexes satisfying predicate.
		 *
		 * System indexes (with oids < FirstNormalObjectId) are excluded from
		 * the search as they are recreated in the new cluster during initdb.
		 */
		res = executeQueryOrDie(
								conn,
								"SELECT n.nspname, c.relname, i.indexrelid "
								"FROM	pg_catalog.pg_class c, "
								"		pg_catalog.pg_index i, "
								"		pg_catalog.pg_am a, "
								"		pg_catalog.pg_namespace n "
								"WHERE	i.indexrelid = c.oid AND "
								"		c.relam = a.oid AND "
								"		c.relnamespace = n.oid AND "
								"		i.indexrelid >= '%u'::pg_catalog.oid AND "
								"		%s "
								"ORDER BY i.indexrelid ASC",
								FirstNormalObjectId,
								pred);

		ntups = PQntuples(res);
		i_nspname = PQfnumber(res, "nspname");
		i_relname = PQfnumber(res, "relname");
		for (rowno = 0; rowno < ntups; rowno++)
		{
			found = true;
			if (!check_mode)
			{
				if (script == NULL && (script = fopen_priv(output_path, "w")) == NULL)
					pg_fatal("could not open file \"%s\": %m", output_path);
				if (!db_used)
				{
					PQExpBufferData connectbuf;

					initPQExpBuffer(&connectbuf);
					appendPsqlMetaConnect(&connectbuf, active_db->db_name);
					fputs(connectbuf.data, script);
					termPQExpBuffer(&connectbuf);
					db_used = true;
				}
				fprintf(script, "REINDEX INDEX %s.%s;\n",
						quote_identifier(PQgetvalue(res, rowno, i_nspname)),
						quote_identifier(PQgetvalue(res, rowno, i_relname)));
			}
		}

		PQclear(res);

		if (!check_mode && db_used)
		{
			/*
			 * Mark indexes satisfying predicate as invalid.
			 *
			 * System indexes (with oids < FirstNormalObjectId) are excluded
			 * from the search (see above).
			 */
			PQclear(executeQueryOrDie(
									  conn,
									  "UPDATE pg_catalog.pg_index i "
									  "SET	indisvalid = false "
									  "FROM	pg_catalog.pg_class c, "
									  "		pg_catalog.pg_am a, "
									  "		pg_catalog.pg_namespace n "
									  "WHERE	i.indexrelid = c.oid AND "
									  "		c.relam = a.oid AND "
									  "		c.relnamespace = n.oid AND "
									  "		i.indexrelid >= '%u'::pg_catalog.oid AND "
									  "		%s",
									  FirstNormalObjectId,
									  pred));
		}

		PQfinish(conn);
	}

	if (script)
		fclose(script);

	if (found)
	{
		report_status(PG_WARNING, "warning");
		if (check_mode)
			pg_log(PG_WARNING, "\n"
				   "Your installation contains %s indexes.  These indexes have different\n"
				   "internal formats between your old and new clusters, so they must be\n"
				   "reindexed with the REINDEX command.  After upgrading, you will be given\n"
				   "REINDEX instructions.",
				   name);
		else
			pg_log(PG_WARNING, "\n"
				   "Your installation contains %s indexes.  These indexes have different\n"
				   "internal formats between your old and new clusters, so they must be\n"
				   "reindexed with the REINDEX command.  The file\n"
				   "    %s\n"
				   "when executed by psql by the database superuser will recreate all invalid\n"
				   "indexes; until then, none of these indexes will be used.",
				   name,
				   output_path);
	}
	else
		check_ok();
}

/*
 * invalidate_spgist_indexes()
 *	32bit -> 64bit
 *	SP-GIST contains xids.
 */
void
invalidate_spgist_indexes(ClusterInfo *cluster, bool check_mode)
{
	invalidate_indexes(cluster, check_mode, "spgist", "a.amname = 'spgist'");
}

/*
 * invalidate_gin_indexes()
 *	32bit -> 64bit
 *	Gin indexes contains xids in deleted pages.
 */
void
invalidate_gin_indexes(ClusterInfo *cluster, bool check_mode)
{
	invalidate_indexes(cluster, check_mode, "gin", "a.amname = 'gin'");
}

/*
 * invalidate_external_indexes()
 *	Generate script to REINDEX non standard external indexes (like RUM etc)
 */
void
invalidate_external_indexes(ClusterInfo *cluster, bool check_mode)
{
	invalidate_indexes(cluster, check_mode, "external",
					   "NOT a.amname IN ('btree', 'hash', 'gist', 'gin', 'spgist', 'brin')");
}

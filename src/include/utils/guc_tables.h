/*-------------------------------------------------------------------------
 *
 * guc_tables.h
 *		Declarations of tables used by GUC.
 *
 * See src/backend/utils/misc/README for design notes.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 *
 *	  src/include/utils/guc_tables.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef GUC_TABLES_H
#define GUC_TABLES_H 1

#include "lib/ilist.h"
#include "utils/guc.h"

/*
 * GUC supports these types of variables:
 */
enum config_type
{
	PGC_BOOL,
	PGC_INT,
	PGC_INT64,
	PGC_REAL,
	PGC_STRING,
	PGC_ENUM,
};

union config_var_val
{
	bool		boolval;
	int			intval;
	int64		int64val;
	double		realval;
	char	   *stringval;
	int			enumval;
};

/*
 * The actual value of a GUC variable can include a malloc'd opaque struct
 * "extra", which is created by its check_hook and used by its assign_hook.
 */
typedef struct config_var_value
{
	union config_var_val val;
	void	   *extra;
} config_var_value;

/*
 * Groupings to help organize all the run-time options for display.
 * Be sure this agrees with the way the options are categorized in config.sgml!
 */
enum config_group
{
	UNGROUPED,					/* use for options not shown in pg_settings */
	FILE_LOCATIONS,
	CONN_AUTH_SETTINGS,
	CONN_AUTH_TCP,
	CONN_AUTH_AUTH,
	CONN_AUTH_SSL,
	RESOURCES_MEM,
	RESOURCES_DISK,
	RESOURCES_KERNEL,
	RESOURCES_BGWRITER,
	RESOURCES_IO,
	RESOURCES_WORKER_PROCESSES,
	WAL_SETTINGS,
	WAL_CHECKPOINTS,
	WAL_ARCHIVING,
	WAL_RECOVERY,
	WAL_ARCHIVE_RECOVERY,
	WAL_RECOVERY_TARGET,
	WAL_SUMMARIZATION,
	REPLICATION_SENDING,
	REPLICATION_PRIMARY,
	REPLICATION_STANDBY,
	REPLICATION_SUBSCRIBERS,
	QUERY_TUNING_METHOD,
	QUERY_TUNING_COST,
	QUERY_TUNING_GEQO,
	QUERY_TUNING_OTHER,
	LOGGING_WHERE,
	LOGGING_WHEN,
	LOGGING_WHAT,
	PROCESS_TITLE,
	STATS_MONITORING,
	STATS_CUMULATIVE,
	VACUUM_AUTOVACUUM,
	VACUUM_COST_DELAY,
	VACUUM_DEFAULT,
	VACUUM_FREEZING,
	CLIENT_CONN_STATEMENT,
	CLIENT_CONN_LOCALE,
	CLIENT_CONN_PRELOAD,
	CLIENT_CONN_OTHER,
	LOCK_MANAGEMENT,
	COMPAT_OPTIONS_PREVIOUS,
	COMPAT_OPTIONS_OTHER,
	ERROR_HANDLING_OPTIONS,
	PRESET_OPTIONS,
	CUSTOM_OPTIONS,
	DEVELOPER_OPTIONS,
};

/*
 * Stack entry for saving the state a variable had prior to an uncommitted
 * transactional change
 */
typedef enum
{
	/* This is almost GucAction, but we need a fourth state for SET+LOCAL */
	GUC_SAVE,					/* entry caused by function SET option */
	GUC_SET,					/* entry caused by plain SET command */
	GUC_LOCAL,					/* entry caused by SET LOCAL command */
	GUC_SET_LOCAL,				/* entry caused by SET then SET LOCAL */
} GucStackState;

typedef struct guc_stack
{
	struct guc_stack *prev;		/* previous stack item, if any */
	int			nest_level;		/* nesting depth at which we made entry */
	GucStackState state;		/* see enum above */
	GucSource	source;			/* source of the prior value */
	/* masked value's source must be PGC_S_SESSION, so no need to store it */
	GucContext	scontext;		/* context that set the prior value */
	GucContext	masked_scontext;	/* context that set the masked value */
	Oid			srole;			/* role that set the prior value */
	Oid			masked_srole;	/* role that set the masked value */
	config_var_value prior;		/* previous value of variable */
	config_var_value masked;	/* SET value in a GUC_SET_LOCAL entry */
} GucStack;

/*
 * Generic fields applicable to all types of variables
 *
 * The short description should be less than 80 chars in length. Some
 * applications may use the long description as well, and will append
 * it to the short description. (separated by a newline or '. ')
 *
 * If the GUC accepts a special value like -1 to disable the feature, use a
 * system default, etc., it should be mentioned in the long description with
 * the following style:
 *
 *  - Special values should be listed at the end of the long description.
 *  - Descriptions should use numerals (e.g., "0") instead of words (e.g.,
 *    "zero").
 *  - Special value mentions should be concise and direct (e.g., "0 disables
 *    the timeout.", "An empty string means use the operating system
 *    setting.").
 *  - Multiple special values should be listed in ascending order.
 *
 * As an exception, special values should _not_ be mentioned if the description
 * would be too complex or if the meaning is sufficiently obvious.
 *
 * srole is the role that set the current value, or BOOTSTRAP_SUPERUSERID
 * if the value came from an internal source or the config file.  Similarly
 * for reset_srole (which is usually BOOTSTRAP_SUPERUSERID, but not always).
 *
 * Variables that are currently of active interest for maintenance
 * operations are linked into various lists using the xxx_link fields.
 * The link fields are unused/garbage in variables not currently having
 * the specified properties.
 *
 * Note that sourcefile/sourceline are kept here, and not pushed into stacked
 * values, although in principle they belong with some stacked value if the
 * active value is session- or transaction-local.  This is to avoid bloating
 * stack entries.  We know they are only relevant when source == PGC_S_FILE.
 */
struct config_generic
{
	/* constant fields, must be set correctly in initial value: */
	const char *name;			/* name of variable - MUST BE FIRST */
	GucContext	context;		/* context required to set the variable */
	enum config_group group;	/* to help organize variables by function */
	const char *short_desc;		/* short desc. of this variable's purpose */
	const char *long_desc;		/* long desc. of this variable's purpose */
	int			flags;			/* flag bits, see guc.h */
	/* variable fields, initialized at runtime: */
	enum config_type vartype;	/* type of variable (set only at startup) */
	int			status;			/* status bits, see below */
	GucSource	source;			/* source of the current actual value */
	GucSource	reset_source;	/* source of the reset_value */
	GucContext	scontext;		/* context that set the current value */
	GucContext	reset_scontext; /* context that set the reset value */
	Oid			srole;			/* role that set the current value */
	Oid			reset_srole;	/* role that set the reset value */
	GucStack   *stack;			/* stacked prior values */
	void	   *extra;			/* "extra" pointer for current actual value */
	dlist_node	nondef_link;	/* list link for variables that have source
								 * different from PGC_S_DEFAULT */
	slist_node	stack_link;		/* list link for variables that have non-NULL
								 * stack */
	slist_node	report_link;	/* list link for variables that have the
								 * GUC_NEEDS_REPORT bit set in status */
	char	   *last_reported;	/* if variable is GUC_REPORT, value last sent
								 * to client (NULL if not yet sent) */
	char	   *sourcefile;		/* file current setting is from (NULL if not
								 * set in config file) */
	int			sourceline;		/* line in source file */
};

/* bit values in status field */
#define GUC_IS_IN_FILE		0x0001	/* found it in config file */
/*
 * Caution: the GUC_IS_IN_FILE bit is transient state for ProcessConfigFile.
 * Do not assume that its value represents useful information elsewhere.
 */
#define GUC_PENDING_RESTART 0x0002	/* changed value cannot be applied yet */
#define GUC_NEEDS_REPORT	0x0004	/* new value must be reported to client */


/* GUC records for specific variable types */

struct config_bool
{
	struct config_generic gen;
	/* constant fields, must be set correctly in initial value: */
	bool	   *variable;
	bool		boot_val;
	GucBoolCheckHook check_hook;
	GucBoolAssignHook assign_hook;
	GucShowHook show_hook;
	/* variable fields, initialized at runtime: */
	bool		reset_val;
	void	   *reset_extra;
};

struct config_int
{
	struct config_generic gen;
	/* constant fields, must be set correctly in initial value: */
	int		   *variable;
	int			boot_val;
	int			min;
	int			max;
	GucIntCheckHook check_hook;
	GucIntAssignHook assign_hook;
	GucShowHook show_hook;
	/* variable fields, initialized at runtime: */
	int			reset_val;
	void	   *reset_extra;
};

struct config_int64
{
	struct config_generic gen;
	/* constant fields, must be set correctly in initial value: */
	int64	   *variable;
	int64		boot_val;
	int64		min;
	int64		max;
	GucInt64CheckHook check_hook;
	GucInt64AssignHook assign_hook;
	GucShowHook show_hook;
	/* variable fields, initialized at runtime: */
	int64		reset_val;
	void	   *reset_extra;
};

struct config_real
{
	struct config_generic gen;
	/* constant fields, must be set correctly in initial value: */
	double	   *variable;
	double		boot_val;
	double		min;
	double		max;
	GucRealCheckHook check_hook;
	GucRealAssignHook assign_hook;
	GucShowHook show_hook;
	/* variable fields, initialized at runtime: */
	double		reset_val;
	void	   *reset_extra;
};

/*
 * A note about string GUCs: the boot_val is allowed to be NULL, which leads
 * to the reset_val and the actual variable value (*variable) also being NULL.
 * However, there is no way to set a NULL value subsequently using
 * set_config_option or any other GUC API.  Also, GUC APIs such as SHOW will
 * display a NULL value as an empty string.  Callers that choose to use a NULL
 * boot_val should overwrite the setting later in startup, or else be careful
 * that NULL doesn't have semantics that are visibly different from an empty
 * string.
 */
struct config_string
{
	struct config_generic gen;
	/* constant fields, must be set correctly in initial value: */
	char	  **variable;
	const char *boot_val;
	GucStringCheckHook check_hook;
	GucStringAssignHook assign_hook;
	GucShowHook show_hook;
	/* variable fields, initialized at runtime: */
	char	   *reset_val;
	void	   *reset_extra;
};

struct config_enum
{
	struct config_generic gen;
	/* constant fields, must be set correctly in initial value: */
	int		   *variable;
	int			boot_val;
	const struct config_enum_entry *options;
	GucEnumCheckHook check_hook;
	GucEnumAssignHook assign_hook;
	GucShowHook show_hook;
	/* variable fields, initialized at runtime: */
	int			reset_val;
	void	   *reset_extra;
};

/* constant tables corresponding to enums above and in guc.h */
extern PGDLLIMPORT const char *const config_group_names[];
extern PGDLLIMPORT const char *const config_type_names[];
extern PGDLLIMPORT const char *const GucContext_Names[];
extern PGDLLIMPORT const char *const GucSource_Names[];

/* data arrays defining all the built-in GUC variables */
extern PGDLLIMPORT struct config_bool ConfigureNamesBool[];
extern PGDLLIMPORT struct config_int ConfigureNamesInt[];
extern PGDLLIMPORT struct config_int64 ConfigureNamesInt64[];
extern PGDLLIMPORT struct config_real ConfigureNamesReal[];
extern PGDLLIMPORT struct config_string ConfigureNamesString[];
extern PGDLLIMPORT struct config_enum ConfigureNamesEnum[];

/* lookup GUC variables, returning config_generic pointers */
extern struct config_generic *find_option(const char *name,
										  bool create_placeholders,
										  bool skip_errors,
										  int elevel);
extern struct config_generic **get_explain_guc_options(int *num);

/* get string value of variable */
extern char *ShowGUCOption(struct config_generic *record, bool use_units);

/* get whether or not the GUC variable is visible to current user */
extern bool ConfigOptionIsVisible(struct config_generic *conf);

/* get the current set of variables */
extern struct config_generic **get_guc_variables(int *num_vars);

extern void build_guc_variables(void);

/* search in enum options */
extern const char *config_enum_lookup_by_value(struct config_enum *record, int val);
extern bool config_enum_lookup_by_name(struct config_enum *record,
									   const char *value, int *retval);
extern char *config_enum_get_options(struct config_enum *record,
									 const char *prefix,
									 const char *suffix,
									 const char *separator);

#endif							/* GUC_TABLES_H */

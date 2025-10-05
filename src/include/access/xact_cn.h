/*-------------------------------------------------------------------------*
 *
 * xact.h
 *    postgres 事务系统定义
 *    postgres transaction system definitions
 *
 *
 * 部分版权 (c) 1996-2024，PostgreSQL 全球开发组
 * 部分版权 (c) 1994，加利福尼亚大学董事会
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/xact.h
 *
 *-------------------------------------------------------------------------*/
#ifndef XACT_H
#define XACT_H

#include "access/transam.h"
#include "access/xlogreader.h"
#include "datatype/timestamp.h"
#include "lib/stringinfo.h"
#include "nodes/pg_list.h"
#include "storage/relfilelocator.h"
#include "storage/sinval.h"

/*
 * 全局事务 ID 的最大大小（包括 '\0'）。
 * Maximum size of Global Transaction ID (including '\0').
 *
 * 注意，GIDSIZE 的最大值必须适合在 TwoPhaseFileHeader 中指定的 uint16 gidlen。
 * Note that the max value of GIDSIZE must fit in the uint16 gidlen,
 * specified in TwoPhaseFileHeader.
 */
#define GIDSIZE 200

/*
 * 事务隔离级别
 * Xact isolation levels
 */
#define XACT_READ_UNCOMMITTED	0    /* 读未提交 */
#define XACT_READ_COMMITTED		1    /* 读已提交 */
#define XACT_REPEATABLE_READ	2    /* 可重复读 */
#define XACT_SERIALIZABLE		3    /* 可序列化 */

extern PGDLLIMPORT int DefaultXactIsoLevel;    /* 默认事务隔离级别 */
extern PGDLLIMPORT int XactIsoLevel;            /* 当前事务隔离级别 */

/*
 * 我们在内部实现三个隔离级别。
 * 两个更强的级别为每个数据库事务使用一个快照；
 * 其他级别为每个语句使用一个快照。
 * 可序列化级别除了快照外还使用谓词锁。
 * 应使用这些宏来检查选择了哪个隔离级别。
 * We implement three isolation levels internally.
 * The two stronger ones use one snapshot per database transaction;
 * the others use one snapshot per statement.
 * Serializable uses predicate locks in addition to snapshots.
 * These macros should be used to check which isolation level is selected.
 */
#define IsolationUsesXactSnapshot() (XactIsoLevel >= XACT_REPEATABLE_READ)    /* 隔离级别是否使用事务快照 */
#define IsolationIsSerializable() (XactIsoLevel == XACT_SERIALIZABLE)            /* 隔离级别是否为可序列化 */

/* 事务只读状态 */
/* Xact read-only state */
extern PGDLLIMPORT bool DefaultXactReadOnly;    /* 默认事务只读状态 */
extern PGDLLIMPORT bool XactReadOnly;            /* 当前事务只读状态 */

/* 用于在此事务中记录语句的标志 */
/* flag for logging statements in this transaction */
extern PGDLLIMPORT bool xact_is_sampled;

/*
 * 事务可延迟 -- 目前仅对只读 SERIALIZABLE 事务有意义
 * Xact is deferrable -- only meaningful (currently) for read only
 * SERIALIZABLE transactions
 */
extern PGDLLIMPORT bool DefaultXactDeferrable;    /* 默认事务可延迟状态 */
extern PGDLLIMPORT bool XactDeferrable;            /* 当前事务可延迟状态 */

typedef enum
{
    SYNCHRONOUS_COMMIT_OFF,            /* 异步提交 */
    SYNCHRONOUS_COMMIT_LOCAL_FLUSH,    /* 仅等待本地刷新 */
    SYNCHRONOUS_COMMIT_REMOTE_WRITE,    /* 等待本地刷新和远程写入 */
    SYNCHRONOUS_COMMIT_REMOTE_FLUSH,    /* 等待本地和远程刷新 */
    SYNCHRONOUS_COMMIT_REMOTE_APPLY,    /* 等待本地和远程刷新以及远程应用 */
} SyncCommitLevel;    /* 同步提交级别 */

/* 定义 synchronous_commit 的默认设置 */
/* Define the default setting for synchronous_commit */
#define SYNCHRONOUS_COMMIT_ON	SYNCHRONOUS_COMMIT_REMOTE_FLUSH

/* 同步提交级别 */
/* Synchronous commit level */
extern PGDLLIMPORT int synchronous_commit;

/* 用于事务的逻辑流复制 */
/* used during logical streaming of a transaction */
extern PGDLLIMPORT TransactionId CheckXidAlive;
extern PGDLLIMPORT bool bsysscan;

/*
 * 用于记录顶级事务上发生的事件的各种标志位。
 * 这些标志仅在 MyXactFlags 中持久化，旨在让我们记住在事务后期要做的某些事情。
 * 这是全局可访问的，因此可以从代码中任何需要记录标志的地方设置。
 * Miscellaneous flag bits to record events which occur on the top level
 * transaction. These flags are only persisted in MyXactFlags and are intended
 * so we remember to do certain things later in the transaction. This is
 * globally accessible, so can be set from anywhere in the code which requires
 * recording flags.
 */
extern PGDLLIMPORT int MyXactFlags;

/*
 * XACT_FLAGS_ACCESSEDTEMPNAMESPACE - 当访问临时对象时设置。
 * 在这种情况下，我们不允许 PREPARE TRANSACTION。
 * XACT_FLAGS_ACCESSEDTEMPNAMESPACE - set when a temporary object is accessed.
 * We don't allow PREPARE TRANSACTION in that case.
 */
#define XACT_FLAGS_ACCESSEDTEMPNAMESPACE		(1U << 0)

/*
 * XACT_FLAGS_ACQUIREDACCESSEXCLUSIVELOCK - 记录顶级事务是否记录了任何访问排他锁。
 * XACT_FLAGS_ACQUIREDACCESSEXCLUSIVELOCK - records whether the top level xact
 * logged any Access Exclusive Locks.
 */
#define XACT_FLAGS_ACQUIREDACCESSEXCLUSIVELOCK	(1U << 1)

/*
 * XACT_FLAGS_NEEDIMMEDIATECOMMIT - 记录顶级语句是否是需要立即提交的语句，如 CREATE DATABASE。
 * XACT_FLAGS_NEEDIMMEDIATECOMMIT - records whether the top level statement
 * is one that requires immediate commit, such as CREATE DATABASE.
 */
#define XACT_FLAGS_NEEDIMMEDIATECOMMIT		(1U << 2)

/*
 * XACT_FLAGS_PIPELINING - 当我们完成扩展查询协议的 Execute 消息时设置。
 * 这对于检测是否通过流水线创建了隐式事务块很有用。
 * XACT_FLAGS_PIPELINING - set when we complete an extended-query-protocol
 * Execute message.  This is useful for detecting that an implicit transaction
 * block has been created via pipelining.
 */
#define XACT_FLAGS_PIPELINING				(1U << 3)

/*
 * 动态加载模块的事务开始和结束回调
 * start- and end-of-transaction callbacks for dynamically loaded modules
 */
typedef enum
{
    XACT_EVENT_COMMIT,                /* 事务提交 */
    XACT_EVENT_PARALLEL_COMMIT,       /* 并行事务提交 */
    XACT_EVENT_ABORT,                 /* 事务中止 */
    XACT_EVENT_PARALLEL_ABORT,        /* 并行事务中止 */
    XACT_EVENT_PREPARE,               /* 事务准备 */
    XACT_EVENT_PRE_COMMIT,            /* 事务预提交 */
    XACT_EVENT_PARALLEL_PRE_COMMIT,   /* 并行事务预提交 */
    XACT_EVENT_PRE_PREPARE,           /* 事务预准备 */
} XactEvent;    /* 事务事件 */

typedef void (*XactCallback) (XactEvent event, void *arg);    /* 事务回调函数类型 */

typedef enum
{
    SUBXACT_EVENT_START_SUB,          /* 子事务开始 */
    SUBXACT_EVENT_COMMIT_SUB,         /* 子事务提交 */
    SUBXACT_EVENT_ABORT_SUB,          /* 子事务中止 */
    SUBXACT_EVENT_PRE_COMMIT_SUB,     /* 子事务预提交 */
} SubXactEvent;    /* 子事务事件 */

typedef void (*SubXactCallback) (SubXactEvent event, SubTransactionId mySubid, 
                                 SubTransactionId parentSubid, void *arg);    /* 子事务回调函数类型 */

/* Save/RestoreTransactionCharacteristics 的数据结构 */
/* Data structure for Save/RestoreTransactionCharacteristics */
typedef struct SavedTransactionCharacteristics
{
    int         save_XactIsoLevel;    /* 保存的事务隔离级别 */
    bool        save_XactReadOnly;    /* 保存的事务只读状态 */
    bool        save_XactDeferrable;  /* 保存的事务可延迟状态 */
} SavedTransactionCharacteristics;


/* ----------------
 *    与事务相关的 XLOG 条目
 *    transaction-related XLOG entries
 * ----------------
 */

/*
 * XLOG 允许在日志记录 xl_info 字段的高 4 位中存储一些信息。
 * 我们使用 3 位用于操作码，1 位用于可选标志变量。
 * XLOG allows to store some information in high 4 bits of log record xl_info
 * field. We use 3 for the opcode, and one about an optional flag variable.
 */
#define XLOG_XACT_COMMIT		0x00    /* 事务提交记录 */
#define XLOG_XACT_PREPARE		0x10    /* 事务准备记录 */
#define XLOG_XACT_ABORT			0x20    /* 事务中止记录 */
#define XLOG_XACT_COMMIT_PREPARED	0x30    /* 已准备事务提交记录 */
#define XLOG_XACT_ABORT_PREPARED	0x40    /* 已准备事务中止记录 */
#define XLOG_XACT_ASSIGNMENT		0x50    /* 事务分配记录 */
#define XLOG_XACT_INVALIDATIONS	0x60    /* 事务失效记录 */
/* free opcode 0x70 */

/* 用于从 xl_info 中过滤操作码的掩码 */
/* mask for filtering opcodes out of xl_info */
#define XLOG_XACT_OPMASK		0x70

/* 此记录是否有 'xinfo' 字段 */
/* does this record have a 'xinfo' field or not */
#define XLOG_XACT_HAS_INFO		0x80

/*
 * 以下标志存储在 xinfo 中，确定提交/中止记录中包含哪些信息。
 * The following flags, stored in xinfo, determine which information is
 * contained in commit/abort records.
 */
#define XACT_XINFO_HAS_DBINFO			(1U << 0)    /* 有数据库信息 */
#define XACT_XINFO_HAS_SUBXACTS			(1U << 1)    /* 有子事务 */
#define XACT_XINFO_HAS_RELFILELOCATORS	(1U << 2)    /* 有关系文件定位器 */
#define XACT_XINFO_HAS_INVALS			(1U << 3)    /* 有失效信息 */
#define XACT_XINFO_HAS_TWOPHASE			(1U << 4)    /* 有两阶段提交 */
#define XACT_XINFO_HAS_ORIGIN			(1U << 5)    /* 有起源信息 */
#define XACT_XINFO_HAS_AE_LOCKS			(1U << 6)    /* 有访问排他锁 */
#define XACT_XINFO_HAS_GID				(1U << 7)    /* 有全局事务 ID */
#define XACT_XINFO_HAS_DROPPED_STATS	(1U << 8)    /* 有已删除的统计信息 */

/*
 * 也存储在 xinfo 中，这些标志指示在恢复期间模拟事务效果时需要执行的各种附加操作。
 * 它们被命名为 XactCompletion... 以区别于在原始事务完成结束时运行的 EOXact... 例程。
 * Also stored in xinfo, these indicating a variety of additional actions that
 * need to occur when emulating transaction effects during recovery.
 *
 * They are named XactCompletion... to differentiate them from
 * EOXact... routines which run at the end of the original transaction
 * completion.
 */
#define XACT_COMPLETION_APPLY_FEEDBACK			(1U << 29)    /* 应用反馈 */
#define XACT_COMPLETION_UPDATE_RELCACHE_FILE	(1U << 30)    /* 更新关系缓存文件 */
#define XACT_COMPLETION_FORCE_SYNC_COMMIT		(1U << 31)    /* 强制同步提交 */

/* 上述标志的访问宏 */
/* Access macros for above flags */
#define XactCompletionApplyFeedback(xinfo) \
    ((xinfo & XACT_COMPLETION_APPLY_FEEDBACK) != 0)
#define XactCompletionRelcacheInitFileInval(xinfo) \
    ((xinfo & XACT_COMPLETION_UPDATE_RELCACHE_FILE) != 0)
#define XactCompletionForceSyncCommit(xinfo) \
    ((xinfo & XACT_COMPLETION_FORCE_SYNC_COMMIT) != 0)

typedef struct xl_xact_assignment
{
    TransactionId xtop;            /* 分配的 XID 的顶级 XID */
    int         nsubxacts;        /* 子事务 XID 的数量 */
    TransactionId xsub[FLEXIBLE_ARRAY_MEMBER];    /* 分配的子 XID */
} xl_xact_assignment;

#define MinSizeOfXactAssignment offsetof(xl_xact_assignment, xsub)

/*
 * 提交和中止记录可以包含很多信息。但很大一部分记录不需要所有可能的信息片段。
 * 所以我们只包含需要的内容。
 *
 * 最小的提交/中止记录仅由 xl_xact_commit/abort 结构体组成。
 * 额外信息的存在由 'xl_xact_xinfo->xinfo' 中设置的位指示。
 * xinfo 字段本身的存在由 xl_info 字段中设置的 XLOG_XACT_HAS_INFO 位表示。
 *
 * 注意：所有单独的数据块的大小应为 sizeof(int) 的倍数，并且只需要 int32 对齐。
 * 如果它们需要更大的对齐，则需要在读取时复制它们。
 * Commit and abort records can contain a lot of information. But a large
 * portion of the records won't need all possible pieces of information. So we
 * only include what's needed.
 *
 * A minimal commit/abort record only consists of a xl_xact_commit/abort
 * struct. The presence of additional information is indicated by bits set in
 * 'xl_xact_xinfo->xinfo'. The presence of the xinfo field itself is signaled
 * by a set XLOG_XACT_HAS_INFO bit in the xl_info field.
 *
 * NB: All the individual data chunks should be sized to multiples of
 * sizeof(int) and only require int32 alignment. If they require bigger
 * alignment, they need to be copied upon reading.
 */

/* 提交/中止的子记录 */
/* sub-records for commit/abort */

typedef struct xl_xact_xinfo
{
    /*
     * 即使我们现在在 xinfo 中只需要两个字节的空间，我们也使用四个字节，
     * 这样后续记录就不必关心对齐问题。
     * 提交记录可能很大，因此复制大部分内容并不吸引人。
     * Even though we right now only require two bytes of space in xinfo we
     * use four so following records don't have to care about alignment.
     * Commit records can be large, so copying large portions isn't
     * attractive.
     */
    uint32        xinfo;
} xl_xact_xinfo;

typedef struct xl_xact_dbinfo
{
    Oid            dbId;        /* 我的数据库 ID */
    Oid            tsId;        /* 我的数据库表空间 */
} xl_xact_dbinfo;

typedef struct xl_xact_subxacts
{
    int            nsubxacts;    /* 子事务 XID 的数量 */
    TransactionId subxacts[FLEXIBLE_ARRAY_MEMBER];
} xl_xact_subxacts;
#define MinSizeOfXactSubxacts offsetof(xl_xact_subxacts, subxacts)

typedef struct xl_xact_relfilelocators
{
    int            nrels;        /* 关系数量 */
    RelFileLocator xlocators[FLEXIBLE_ARRAY_MEMBER];
} xl_xact_relfilelocators;
#define MinSizeOfXactRelfileLocators offsetof(xl_xact_relfilelocators, xlocators)

/*
 * 事务性删除的统计条目。
 * 在 pgstat.h 中声明这个而不是在这里，因为 pgstat.h 不能从前端代码中包含，
 * 但 WAL 格式需要可由前端程序读取。
 * A transactionally dropped statistics entry.
 *
 * Declared here rather than pgstat.h because pgstat.h can't be included from
 * frontend code, but the WAL format needs to be readable by frontend
 * programs.
 */
typedef struct xl_xact_stats_item
{
    int            kind;
    Oid            dboid;
    Oid            objoid;
} xl_xact_stats_item;

typedef struct xl_xact_stats_items
{
    int            nitems;
    xl_xact_stats_item items[FLEXIBLE_ARRAY_MEMBER];
} xl_xact_stats_items;
#define MinSizeOfXactStatsItems offsetof(xl_xact_stats_items, items)

typedef struct xl_xact_invals
{
    int            nmsgs;        /* 共享失效消息的数量 */
    SharedInvalidationMessage msgs[FLEXIBLE_ARRAY_MEMBER];
} xl_xact_invals;
#define MinSizeOfXactInvals offsetof(xl_xact_invals, msgs)

typedef struct xl_xact_twophase
{
    TransactionId xid;
} xl_xact_twophase;

typedef struct xl_xact_origin
{
    XLogRecPtr    origin_lsn;
    TimestampTz origin_timestamp;
} xl_xact_origin;

typedef struct xl_xact_commit
{
    TimestampTz xact_time;        /* 提交时间 */

    /* 如果 XLOG_XACT_HAS_INFO，则后面跟着 xl_xact_xinfo */
    /* xl_xact_xinfo follows if XLOG_XACT_HAS_INFO */
    /* 如果 XINFO_HAS_DBINFO，则后面跟着 xl_xact_dbinfo */
    /* xl_xact_dbinfo follows if XINFO_HAS_DBINFO */
    /* 如果 XINFO_HAS_SUBXACT，则后面跟着 xl_xact_subxacts */
    /* xl_xact_subxacts follows if XINFO_HAS_SUBXACT */
    /* 如果 XINFO_HAS_RELFILELOCATORS，则后面跟着 xl_xact_relfilelocators */
    /* xl_xact_relfilelocators follows if XINFO_HAS_RELFILELOCATORS */
    /* 如果 XINFO_HAS_DROPPED_STATS，则后面跟着 xl_xact_stats_items */
    /* xl_xact_stats_items follows if XINFO_HAS_DROPPED_STATS */
    /* 如果 XINFO_HAS_INVALS，则后面跟着 xl_xact_invals */
    /* xl_xact_invals follows if XINFO_HAS_INVALS */
    /* 如果 XINFO_HAS_TWOPHASE，则后面跟着 xl_xact_twophase */
    /* xl_xact_twophase follows if XINFO_HAS_TWOPHASE */
    /* 如果 XINFO_HAS_GID，则后面跟着 twophase_gid（作为以 null 结尾的字符串） */
    /* twophase_gid follows if XINFO_HAS_GID. As a null-terminated string. */
    /* 如果 XINFO_HAS_ORIGIN，则后面跟着 xl_xact_origin（未对齐存储！） */
    /* xl_xact_origin follows if XINFO_HAS_ORIGIN, stored unaligned! */
} xl_xact_commit;
#define MinSizeOfXactCommit (offsetof(xl_xact_commit, xact_time) + sizeof(TimestampTz))

typedef struct xl_xact_abort
{
    TimestampTz xact_time;        /* 中止时间 */

    /* 如果 XLOG_XACT_HAS_INFO，则后面跟着 xl_xact_xinfo */
    /* xl_xact_xinfo follows if XLOG_XACT_HAS_INFO */
    /* 如果 XINFO_HAS_DBINFO，则后面跟着 xl_xact_dbinfo */
    /* xl_xact_dbinfo follows if XINFO_HAS_DBINFO */
    /* 如果 XINFO_HAS_SUBXACT，则后面跟着 xl_xact_subxacts */
    /* xl_xact_subxacts follows if XINFO_HAS_SUBXACT */
    /* 如果 XINFO_HAS_RELFILELOCATORS，则后面跟着 xl_xact_relfilelocators */
    /* xl_xact_relfilelocators follows if XINFO_HAS_RELFILELOCATORS */
    /* 如果 XINFO_HAS_DROPPED_STATS，则后面跟着 xl_xact_stats_items */
    /* xl_xact_stats_items follows if XINFO_HAS_DROPPED_STATS */
    /* 不需要失效消息。 */
    /* No invalidation messages needed. */
    /* 如果 XINFO_HAS_TWOPHASE，则后面跟着 xl_xact_twophase */
    /* xl_xact_twophase follows if XINFO_HAS_TWOPHASE */
    /* 如果 XINFO_HAS_GID，则后面跟着 twophase_gid（作为以 null 结尾的字符串） */
    /* twophase_gid follows if XINFO_HAS_GID. As a null-terminated string. */
    /* 如果 XINFO_HAS_ORIGIN，则后面跟着 xl_xact_origin（未对齐存储！） */
    /* xl_xact_origin follows if XINFO_HAS_ORIGIN, stored unaligned! */
} xl_xact_abort;
#define MinSizeOfXactAbort sizeof(xl_xact_abort)

typedef struct xl_xact_prepare
{
    uint32        magic;            /* 格式标识符 */
    uint32        total_len;        /* 实际文件长度 */
    TransactionId xid;            /* 原始事务 XID */
    Oid            database;        /* 它所在的数据库 OID */
    TimestampTz prepared_at;        /* 准备时间 */
    Oid            owner;            /* 运行事务的用户 */
    int32        nsubxacts;        /* 后续子事务 XID 的数量 */
    int32        ncommitrels;        /* 提交时删除的关系数 */
    int32        nabortrels;        /* 中止时删除的关系数 */
    int32        ncommitstats;        /* 提交时删除的统计信息数 */
    int32        nabortstats;        /* 中止时删除的统计信息数 */
    int32        ninvalmsgs;        /* 缓存失效消息的数量 */
    bool        initfileinval;        /* 关系缓存初始化文件是否需要失效？ */
    uint16        gidlen;            /* GID 的长度 - GID 跟随头部 */
    XLogRecPtr    origin_lsn;        /* 此记录在源节点的 lsn */
    TimestampTz origin_timestamp;    /* 源节点的准备时间 */
} xl_xact_prepare;

/*
 * 上述形式的提交/中止记录解析起来有点冗长，
 * 因此 ParseCommit/AbortRecord() 生成了一个解构版本，以便于使用。
 * Commit/Abort records in the above form are a bit verbose to parse, so
 * there's a deconstructed versions generated by ParseCommit/AbortRecord() for
 * easier consumption.
 */
typedef struct xl_xact_parsed_commit
{
    TimestampTz xact_time;
    uint32        xinfo;

    Oid            dbId;            /* MyDatabaseId */
    Oid            tsId;            /* MyDatabaseTableSpace */

    int            nsubxacts;
    TransactionId *subxacts;

    int            nrels;
    RelFileLocator *xlocators;

    int            nstats;
    xl_xact_stats_item *stats;

    int            nmsgs;
    SharedInvalidationMessage *msgs;

    TransactionId twophase_xid;    /* 仅用于 2PC */
    char        twophase_gid[GIDSIZE];    /* 仅用于 2PC */
    int            nabortrels;        /* 仅用于 2PC */
    RelFileLocator *abortlocators;    /* 仅用于 2PC */
    int            nabortstats;        /* 仅用于 2PC */
    xl_xact_stats_item *abortstats;    /* 仅用于 2PC */

    XLogRecPtr    origin_lsn;
    TimestampTz origin_timestamp;
} xl_xact_parsed_commit;

typedef xl_xact_parsed_commit xl_xact_parsed_prepare;

typedef struct xl_xact_parsed_abort
{
    TimestampTz xact_time;
    uint32        xinfo;

    Oid            dbId;            /* MyDatabaseId */
    Oid            tsId;            /* MyDatabaseTableSpace */

    int            nsubxacts;
    TransactionId *subxacts;

    int            nrels;
    RelFileLocator *xlocators;

    int            nstats;
    xl_xact_stats_item *stats;

    TransactionId twophase_xid;    /* 仅用于 2PC */
    char        twophase_gid[GIDSIZE];    /* 仅用于 2PC */

    XLogRecPtr    origin_lsn;
    TimestampTz origin_timestamp;
} xl_xact_parsed_abort;


/* ----------------
 *    外部定义
 *    extern definitions
 * ----------------
 */
extern bool IsTransactionState(void);                                    /* 是否处于事务状态 */
extern bool IsAbortedTransactionBlockState(void);                          /* 是否处于已中止的事务块状态 */
extern TransactionId GetTopTransactionId(void);                            /* 获取顶级事务 ID */
extern TransactionId GetTopTransactionIdIfAny(void);                       /* 获取顶级事务 ID（如果有） */
extern TransactionId GetCurrentTransactionId(void);                        /* 获取当前事务 ID */
extern TransactionId GetCurrentTransactionIdIfAny(void);                   /* 获取当前事务 ID（如果有） */
extern TransactionId GetStableLatestTransactionId(void);                   /* 获取稳定的最新事务 ID */
extern SubTransactionId GetCurrentSubTransactionId(void);                  /* 获取当前子事务 ID */
extern FullTransactionId GetTopFullTransactionId(void);                    /* 获取顶级完整事务 ID */
extern FullTransactionId GetTopFullTransactionIdIfAny(void);               /* 获取顶级完整事务 ID（如果有） */
extern FullTransactionId GetCurrentFullTransactionId(void);                /* 获取当前完整事务 ID */
extern FullTransactionId GetCurrentFullTransactionIdIfAny(void);           /* 获取当前完整事务 ID（如果有） */
extern void MarkCurrentTransactionIdLoggedIfAny(void);                     /* 如果存在则标记当前事务 ID 已记录 */
extern bool SubTransactionIsActive(SubTransactionId subxid);               /* 子事务是否活跃 */
extern CommandId GetCurrentCommandId(bool used);                           /* 获取当前命令 ID */
extern void SetParallelStartTimestamps(TimestampTz xact_ts, TimestampTz stmt_ts); /* 设置并行启动时间戳 */
extern TimestampTz GetCurrentTransactionStartTimestamp(void);              /* 获取当前事务开始时间戳 */
extern TimestampTz GetCurrentStatementStartTimestamp(void);                /* 获取当前语句开始时间戳 */
extern TimestampTz GetCurrentTransactionStopTimestamp(void);               /* 获取当前事务停止时间戳 */
extern void SetCurrentStatementStartTimestamp(void);                       /* 设置当前语句开始时间戳 */
extern int GetCurrentTransactionNestLevel(void);                           /* 获取当前事务嵌套级别 */
extern bool TransactionIdIsCurrentTransactionId(TransactionId xid);         /* 事务 ID 是否为当前事务 ID */
extern void CommandCounterIncrement(void);                                 /* 命令计数器递增 */
extern void ForceSyncCommit(void);                                         /* 强制同步提交 */
extern void StartTransactionCommand(void);                                 /* 开始事务命令 */
extern void SaveTransactionCharacteristics(SavedTransactionCharacteristics *s); /* 保存事务特性 */
extern void RestoreTransactionCharacteristics(const SavedTransactionCharacteristics *s); /* 恢复事务特性 */
extern void CommitTransactionCommand(void);                                /* 提交事务命令 */
extern void AbortCurrentTransaction(void);                                 /* 中止当前事务 */
extern void BeginTransactionBlock(void);                                   /* 开始事务块 */
extern bool EndTransactionBlock(bool chain);                               /* 结束事务块 */
extern bool PrepareTransactionBlock(const char *gid);                      /* 准备事务块 */
extern void UserAbortTransactionBlock(bool chain);                         /* 用户中止事务块 */
extern void BeginImplicitTransactionBlock(void);                           /* 开始隐式事务块 */
extern void EndImplicitTransactionBlock(void);                             /* 结束隐式事务块 */
extern void ReleaseSavepoint(const char *name);                            /* 释放保存点 */
extern void DefineSavepoint(const char *name);                             /* 定义保存点 */
extern void RollbackToSavepoint(const char *name);                         /* 回滚到保存点 */
extern void BeginInternalSubTransaction(const char *name);                 /* 开始内部子事务 */
extern void ReleaseCurrentSubTransaction(void);                            /* 释放当前子事务 */
extern void RollbackAndReleaseCurrentSubTransaction(void);                 /* 回滚并释放当前子事务 */
extern bool IsSubTransaction(void);                                        /* 是否为子事务 */
extern Size EstimateTransactionStateSpace(void);                           /* 估计事务状态空间 */
extern void SerializeTransactionState(Size maxsize, char *start_address);  /* 序列化事务状态 */
extern void StartParallelWorkerTransaction(char *tstatespace);             /* 开始并行工作器事务 */
extern void EndParallelWorkerTransaction(void);                            /* 结束并行工作器事务 */
extern bool IsTransactionBlock(void);                                      /* 是否为事务块 */
extern bool IsTransactionOrTransactionBlock(void);                         /* 是否为事务或事务块 */
extern char TransactionBlockStatusCode(void);                              /* 事务块状态码 */
extern void AbortOutOfAnyTransaction(void);                                /* 中止任何事务外的操作 */
extern void PreventInTransactionBlock(bool isTopLevel, const char *stmtType); /* 防止在事务块中执行 */
extern void RequireTransactionBlock(bool isTopLevel, const char *stmtType); /* 要求在事务块中执行 */
extern void WarnNoTransactionBlock(bool isTopLevel, const char *stmtType); /* 警告不在事务块中 */
extern bool IsInTransactionBlock(bool isTopLevel);                         /* 是否在事务块中 */
extern void RegisterXactCallback(XactCallback callback, void *arg);         /* 注册事务回调 */
extern void UnregisterXactCallback(XactCallback callback, void *arg);       /* 注销事务回调 */
extern void RegisterSubXactCallback(SubXactCallback callback, void *arg);   /* 注册子事务回调 */
extern void UnregisterSubXactCallback(SubXactCallback callback, void *arg); /* 注销子事务回调 */

extern bool IsSubxactTopXidLogPending(void);                               /* 子事务顶级 XID 日志是否待处理 */
extern void MarkSubxactTopXidLogged(void);                                 /* 标记子事务顶级 XID 已记录 */

extern int xactGetCommittedChildren(TransactionId **ptr);                  /* 获取事务已提交的子事务 */

extern XLogRecPtr XactLogCommitRecord(TimestampTz commit_time,
                                      int nsubxacts, TransactionId *subxacts,
                                      int nrels, RelFileLocator *rels,
                                      int ndroppedstats,
                                      xl_xact_stats_item *droppedstats,
                                      int nmsgs, SharedInvalidationMessage *msgs,
                                      bool relcacheInval,
                                      int xactflags,
                                      TransactionId twophase_xid,
                                      const char *twophase_gid);

extern XLogRecPtr XactLogAbortRecord(TimestampTz abort_time,
                                     int nsubxacts, TransactionId *subxacts,
                                     int nrels, RelFileLocator *rels,
                                     int ndroppedstats,
                                     xl_xact_stats_item *droppedstats,
                                     int xactflags, TransactionId twophase_xid,
                                     const char *twophase_gid);
extern void xact_redo(XLogReaderState *record);

/* xactdesc.c */
extern void xact_desc(StringInfo buf, XLogReaderState *record);
extern const char *xact_identify(uint8 info);

/* 也在 xactdesc.c 中，因此它们可以在前端/后端代码之间共享 */
/* also in xactdesc.c, so they can be shared between front/backend code */
extern void ParseCommitRecord(uint8 info, xl_xact_commit *xlrec, xl_xact_parsed_commit *parsed);
extern void ParseAbortRecord(uint8 info, xl_xact_abort *xlrec, xl_xact_parsed_abort *parsed);
extern void ParsePrepareRecord(uint8 info, xl_xact_prepare *xlrec, xl_xact_parsed_prepare *parsed);

extern void EnterParallelMode(void);                                       /* 进入并行模式 */
extern void ExitParallelMode(void);                                        /* 退出并行模式 */
extern bool IsInParallelMode(void);                                        /* 是否处于并行模式 */

#endif				/* XACT_H */
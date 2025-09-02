/*-------------------------------------------------------------------------
 *
 * relscan.h
 *	  POSTGRES relation scan descriptor definitions.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/relscan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RELSCAN_H
#define RELSCAN_H

#include "access/htup_details.h"
#include "access/itup.h"
#include "port/atomics.h"
#include "storage/buf.h"
#include "storage/spin.h"
#include "utils/relcache.h"


struct ParallelTableScanDescData;

/*
 * Generic descriptor for table scans. This is the base-class for table scans,
 * which needs to be embedded in the scans of individual AMs.
 * 通用表扫描描述符，是所有表扫描的基类，需要嵌入到具体访问方法的扫描结构体中。
 */
typedef struct TableScanDescData
{
    /* scan parameters */
    Relation	rs_rd;			/* heap relation descriptor 堆关系描述符 */
    struct SnapshotData *rs_snapshot;	/* snapshot to see 扫描快照 */
    int			rs_nkeys;		/* number of scan keys 扫描键数量 */
    struct ScanKeyData *rs_key; /* array of scan key descriptors 扫描键数组 */

    /* Range of ItemPointers for table_scan_getnextslot_tidrange() to scan. */
    ItemPointerData rs_mintid;  // 扫描的最小ItemPointer
    ItemPointerData rs_maxtid;  // 扫描的最大ItemPointer

    /*
     * Information about type and behaviour of the scan, a bitmask of members
     * of the ScanOptions enum (see tableam.h).
     * 扫描类型和行为信息，ScanOptions枚举的位掩码（见tableam.h）。
     */
    uint32		rs_flags;

    struct ParallelTableScanDescData *rs_parallel;	/* parallel scan
                                                     * information 并行扫描信息 */
} TableScanDescData;
typedef struct TableScanDescData *TableScanDesc;

/*
 * Shared state for parallel table scan.
 *
 * Each backend participating in a parallel table scan has its own
 * TableScanDesc in backend-private memory, and those objects all contain a
 * pointer to this structure.  The information here must be sufficient to
 * properly initialize each new TableScanDesc as workers join the scan, and it
 * must act as a information what to scan for those workers.
 * 并行表扫描的共享状态。每个后端有自己的TableScanDesc，指向此结构体，用于初始化和指导并行扫描。
 */
typedef struct ParallelTableScanDescData
{
    Oid			phs_relid;		/* OID of relation to scan 要扫描的关系OID */
    bool		phs_syncscan;	/* report location to syncscan logic? 是否同步扫描 */
    bool		phs_snapshot_any;	/* SnapshotAny, not phs_snapshot_data? 是否为任意快照 */
    Size		phs_snapshot_off;	/* data for snapshot 快照数据偏移 */
} ParallelTableScanDescData;
typedef struct ParallelTableScanDescData *ParallelTableScanDesc;

/*
 * Shared state for parallel table scans, for block oriented storage.
 * 块存储并行表扫描的共享状态。
 */
typedef struct ParallelBlockTableScanDescData
{
    ParallelTableScanDescData base;

    BlockNumber phs_nblocks;	/* # blocks in relation at start of scan 扫描开始时的块数 */
    slock_t		phs_mutex;		/* mutual exclusion for setting startblock 设置起始块的互斥锁 */
    BlockNumber phs_startblock; /* starting block number 起始块号 */
    pg_atomic_uint64 phs_nallocated;	/* number of blocks allocated to
                                         * workers so far. 已分配给工作进程的块数 */
}			ParallelBlockTableScanDescData;
typedef struct ParallelBlockTableScanDescData *ParallelBlockTableScanDesc;

/*
 * Per backend state for parallel table scan, for block-oriented storage.
 * 块存储并行表扫描的每个后端状态。
 */
typedef struct ParallelBlockTableScanWorkerData
{
    uint64		phsw_nallocated;	/* Current # of blocks into the scan 当前已扫描块数 */
    uint32		phsw_chunk_remaining;	/* # blocks left in this chunk 当前块剩余数量 */
    uint32		phsw_chunk_size;	/* The number of blocks to allocate in
                                     * each I/O chunk for the scan 每次分配的块数 */
} ParallelBlockTableScanWorkerData;
typedef struct ParallelBlockTableScanWorkerData *ParallelBlockTableScanWorker;

/*
 * Base class for fetches from a table via an index. This is the base-class
 * for such scans, which needs to be embedded in the respective struct for
 * individual AMs.
 * 通过索引从表中获取数据的基类，需要嵌入到具体访问方法的结构体中。
 */
typedef struct IndexFetchTableData
{
    Relation	rel; // 关联的表
} IndexFetchTableData;

/*
 * We use the same IndexScanDescData structure for both amgettuple-based
 * and amgetbitmap-based index scans.  Some fields are only relevant in
 * amgettuple-based scans.
 * amgettuple和amgetbitmap两种索引扫描都使用IndexScanDescData结构体，部分字段仅用于amgettuple扫描。
 */
typedef struct IndexScanDescData
{
    /* scan parameters */
    Relation	heapRelation;	/* heap relation descriptor, or NULL 堆关系描述符或NULL */
    Relation	indexRelation;	/* index relation descriptor 索引关系描述符 */
    struct SnapshotData *xs_snapshot;	/* snapshot to see 扫描快照 */
    int			numberOfKeys;	/* number of index qualifier conditions 索引条件数量 */
    int			numberOfOrderBys;	/* number of ordering operators 排序操作符数量 */
    struct ScanKeyData *keyData;	/* array of index qualifier descriptors 索引条件数组 */
    struct ScanKeyData *orderByData;	/* array of ordering op descriptors 排序操作符数组 */
    bool		xs_want_itup;	/* caller requests index tuples 是否需要索引元组 */
    bool		xs_temp_snap;	/* unregister snapshot at scan end? 扫描结束时是否注销快照 */

    /* signaling to index AM about killing index tuples */
    bool		kill_prior_tuple;	/* last-returned tuple is dead 上一个返回元组已删除 */
    bool		ignore_killed_tuples;	/* do not return killed entries 忽略已删除条目 */
    bool		xactStartedInRecovery;	/* prevents killing/seeing killed
                                         * tuples 是否在恢复中启动事务 */

    /* index access method's private state */
    void	   *opaque;			/* access-method-specific info 访问方法私有信息 */

    /*
     * In an index-only scan, a successful amgettuple call must fill either
     * xs_itup (and xs_itupdesc) or xs_hitup (and xs_hitupdesc) to provide the
     * data returned by the scan.  It can fill both, in which case the heap
     * format will be used.
     * 索引仅扫描时，amgettuple成功后必须填充xs_itup（及xs_itupdesc）或xs_hitup（及xs_hitupdesc），用于返回数据。
     * 如果都填充，则使用堆格式。
     */
    IndexTuple	xs_itup;		/* index tuple returned by AM 索引访问方法返回的元组 */
    struct TupleDescData *xs_itupdesc;	/* rowtype descriptor of xs_itup xs_itup的行类型描述符 */
    HeapTuple	xs_hitup;		/* index data returned by AM, as HeapTuple 索引访问方法返回的堆元组 */
    struct TupleDescData *xs_hitupdesc; /* rowtype descriptor of xs_hitup xs_hitup的行类型描述符 */

    ItemPointerData xs_heaptid; /* result 结果ItemPointer */
    bool		xs_heap_continue;	/* T if must keep walking, potential
                                     * further results 是否继续扫描后续结果 */
    IndexFetchTableData *xs_heapfetch; // 堆数据获取结构体指针

    bool		xs_recheck;		/* T means scan keys must be rechecked 是否需要重新检查扫描键 */

    /*
     * When fetching with an ordering operator, the values of the ORDER BY
     * expressions of the last returned tuple, according to the index.  If
     * xs_recheckorderby is true, these need to be rechecked just like the
     * scan keys, and the values returned here are a lower-bound on the actual
     * values.
     * 使用排序操作符获取数据时，保存最后返回元组的ORDER BY表达式值。
     * 如果xs_recheckorderby为真，则需要重新检查，这里的值是实际值的下界。
     */
    Datum	   *xs_orderbyvals;   // ORDER BY表达式的值
    bool	   *xs_orderbynulls;  // ORDER BY表达式是否为NULL
    bool		xs_recheckorderby; // 是否需要重新检查ORDER BY

    /* parallel index scan information, in shared memory */
    struct ParallelIndexScanDescData *parallel_scan; // 并行索引扫描信息（共享内存）
}			IndexScanDescData;

/* Generic structure for parallel scans
 * 并行扫描的通用结构体
 */
typedef struct ParallelIndexScanDescData
{
    Oid			ps_relid;      // 关系OID
    Oid			ps_indexid;    // 索引OID
    Size		ps_offset;		/* Offset in bytes of am specific structure 访问方法特定结构体的字节偏移 */
    char		ps_snapshot_data[FLEXIBLE_ARRAY_MEMBER]; // 快照数据
}			ParallelIndexScanDescData;

struct TupleTableSlot;

/* Struct for storage-or-index scans of system tables
 * 系统表存储或索引扫描结构体
 */
typedef struct SysScanDescData
{
    Relation	heap_rel;		/* catalog being scanned 被扫描的系统表 */
    Relation	irel;			/* NULL if doing heap scan 堆扫描时为NULL */
    struct TableScanDescData *scan; /* only valid in storage-scan case 仅存储扫描时有效 */
    struct IndexScanDescData *iscan;	/* only valid in index-scan case 仅索引扫描时有效 */
    struct SnapshotData *snapshot;	/* snapshot to unregister at end of scan 扫描结束时注销的快照 */
    struct TupleTableSlot *slot;    // 元组槽
}			SysScanDescData;

#endif							/* RELSCAN_H */

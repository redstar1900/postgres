/*-------------------------------------------------------------------------
 *
 * smgr.h
 *	  storage manager switch public interface declarations.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/smgr.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SMGR_H
#define SMGR_H

#include "lib/ilist.h"
#include "storage/block.h"
#include "storage/relfilelocator.h"

/*
 * smgr.c maintains a table of SMgrRelation objects, which are essentially
 * cached file handles.  An SMgrRelation is created (if not already present)
 * by smgropen(), and destroyed by smgrdestroy().  Note that neither of these
 * operations imply I/O, they just create or destroy a hashtable entry.  (But
 * smgrdestroy() may release associated resources, such as OS-level file
 * descriptors.)
 *
 * smgr.c 维护一个 SMgrRelation 对象表，本质上是缓存的文件句柄。
 * SMgrRelation 通过 smgropen() 创建（如果尚未存在），通过 smgrdestroy() 销毁。
 * 注意，这两个操作都不涉及 I/O，只是创建或销毁哈希表项。（但 smgrdestroy() 可能会释放相关资源，如操作系统级文件描述符。）
 *
 * An SMgrRelation may be "pinned", to prevent it from being destroyed while
 * it's in use.  We use this to prevent pointers relcache to smgr from being
 * invalidated.  SMgrRelations that are not pinned are deleted at end of
 * transaction.
 *
 * SMgrRelation 可以被“固定”，以防止在使用期间被销毁。我们用它来防止 relcache 到 smgr 的指针失效。
 * 未固定的 SMgrRelation 会在事务结束时删除。
 */
typedef struct SMgrRelationData
{
    /* rlocator is the hashtable lookup key, so it must be first! */
    RelFileLocatorBackend smgr_rlocator;	/* relation physical identifier */
    // smgr_rlocator 是哈希表查找键，必须放在第一个！关系物理标识符

    /*
     * The following fields are reset to InvalidBlockNumber upon a cache flush
     * event, and hold the last known size for each fork.  This information is
     * currently only reliable during recovery, since there is no cache
     * invalidation for fork extension.
     */
    BlockNumber smgr_targblock; /* current insertion target block */
    // 当前插入目标块号
    BlockNumber smgr_cached_nblocks[MAX_FORKNUM + 1];	/* last known size */
    // 每个 fork 的最后已知大小

    /* additional public fields may someday exist here */
    // 以后可能会有更多公共字段

    /*
     * Fields below here are intended to be private to smgr.c and its
     * submodules.  Do not touch them from elsewhere.
     */
    int			smgr_which;		/* storage manager selector */
    // 存储管理器选择器，仅供 smgr.c 及其子模块使用

    /*
     * for md.c; per-fork arrays of the number of open segments
     * (md_num_open_segs) and the segments themselves (md_seg_fds).
     */
    int			md_num_open_segs[MAX_FORKNUM + 1];
    // 每个 fork 已打开的段数量
    struct _MdfdVec *md_seg_fds[MAX_FORKNUM + 1];
    // 每个 fork 的段本身

    /*
     * Pinning support.  If unpinned (ie. pincount == 0), 'node' is a list
     * link in list of all unpinned SMgrRelations.
     */
    int			pincount;
    // 固定计数，未固定时为 0
    dlist_node	node;
    // 未固定 SMgrRelation 的链表节点
} SMgrRelationData;

typedef SMgrRelationData *SMgrRelation;
// SMgrRelation 类型定义

#define SmgrIsTemp(smgr) \
    RelFileLocatorBackendIsTemp((smgr)->smgr_rlocator)
// 判断 SMgrRelation 是否为临时关系

// 存储管理器相关函数声明
extern void smgrinit(void); // 初始化存储管理器
extern SMgrRelation smgropen(RelFileLocator rlocator, ProcNumber backend); // 打开关系
extern bool smgrexists(SMgrRelation reln, ForkNumber forknum); // 判断关系文件是否存在
extern void smgrpin(SMgrRelation reln); // 固定关系
extern void smgrunpin(SMgrRelation reln); // 解除固定
extern void smgrclose(SMgrRelation reln); // 关闭关系
extern void smgrdestroyall(void); // 销毁所有关系
extern void smgrrelease(SMgrRelation reln); // 释放关系
extern void smgrreleaseall(void); // 释放所有关系
extern void smgrreleaserellocator(RelFileLocatorBackend rlocator); // 通过定位器释放关系
extern void smgrcreate(SMgrRelation reln, ForkNumber forknum, bool isRedo); // 创建关系文件
extern void smgrdosyncall(SMgrRelation *rels, int nrels); // 同步所有关系
extern void smgrdounlinkall(SMgrRelation *rels, int nrels, bool isRedo); // 删除所有关系文件
extern void smgrextend(SMgrRelation reln, ForkNumber forknum,
                       BlockNumber blocknum, const void *buffer, bool skipFsync); // 扩展关系文件
extern void smgrzeroextend(SMgrRelation reln, ForkNumber forknum,
                           BlockNumber blocknum, int nblocks, bool skipFsync); // 零填充扩展
extern bool smgrprefetch(SMgrRelation reln, ForkNumber forknum,
                         BlockNumber blocknum, int nblocks); // 预取块
extern void smgrreadv(SMgrRelation reln, ForkNumber forknum,
                      BlockNumber blocknum,
                      void **buffers, BlockNumber nblocks); // 读取多个块
extern void smgrwritev(SMgrRelation reln, ForkNumber forknum,
                       BlockNumber blocknum,
                       const void **buffers, BlockNumber nblocks,
                       bool skipFsync); // 写入多个块
extern void smgrwriteback(SMgrRelation reln, ForkNumber forknum,
                          BlockNumber blocknum, BlockNumber nblocks); // 写回块
extern BlockNumber smgrnblocks(SMgrRelation reln, ForkNumber forknum); // 获取块数
extern BlockNumber smgrnblocks_cached(SMgrRelation reln, ForkNumber forknum); // 获取缓存的块数
extern void smgrtruncate(SMgrRelation reln, ForkNumber *forknum, int nforks,
                         BlockNumber *nblocks); // 截断关系文件
extern void smgrtruncate2(SMgrRelation reln, ForkNumber *forknum, int nforks,
                          BlockNumber *old_nblocks,
                          BlockNumber *nblocks); // 截断关系文件（带旧块数）
extern void smgrimmedsync(SMgrRelation reln, ForkNumber forknum); // 立即同步
extern void smgrregistersync(SMgrRelation reln, ForkNumber forknum); // 注册同步
extern void AtEOXact_SMgr(void); // 事务结束时处理
extern bool ProcessBarrierSmgrRelease(void); // 处理屏障释放

// 内联函数：读取单个块
static inline void
smgrread(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
         void *buffer)
{
    smgrreadv(reln, forknum, blocknum, &buffer, 1);
}

// 内联函数：写入单个块
static inline void
smgrwrite(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
          const void *buffer, bool skipFsync)
{
    smgrwritev(reln, forknum, blocknum, &buffer, 1, skipFsync);
}

#endif							/* SMGR_H */

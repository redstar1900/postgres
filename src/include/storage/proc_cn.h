/*-------------------------------------------------------------------------  
 *  
 * proc.h  
 *      per-process shared memory data structures  
 *      每个进程的共享内存数据结构  
 *  
 *  
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group  
 * Portions Copyright (c) 1994, Regents of the University of California  
 *  
 * src/include/storage/proc.h  
 *  
 *-------------------------------------------------------------------------  
 */  
#ifndef _PROC_H_  
#define _PROC_H_  

#include "access/clog.h"  
#include "access/xlogdefs.h"  
#include "lib/ilist.h"  
#include "storage/latch.h"  
#include "storage/lock.h"  
#include "storage/pg_sema.h"  
#include "storage/proclist_types.h"  
#include "storage/procnumber.h"  

/*  
 * Each backend advertises up to PGPROC_MAX_CACHED_SUBXIDS TransactionIds  
 * for non-aborted subtransactions of its current top transaction.  These  
 * have to be treated as running XIDs by other backends.  
 * 每个后端会为当前顶级事务的非中止子事务最多缓存PGPROC_MAX_CACHED_SUBXIDS个事务ID。  
 * 这些ID需要被其他后端视为运行中的事务ID。  
 *  
 * We also keep track of whether the cache overflowed (ie, the transaction has  
 * generated at least one subtransaction that didn't fit in the cache).  
 * If none of the caches have overflowed, we can assume that an XID that's not  
 * listed anywhere in the PGPROC array is not a running transaction.  Else we  
 * have to look at pg_subtrans.  
 * 我们还会跟踪缓存是否溢出（即事务是否生成了至少一个无法放入缓存的子事务）。  
 * 如果没有缓存溢出，我们可以假设PGPROC数组中任何地方都未列出的XID不是运行中的事务。  
 * 否则我们必须查看pg_subtrans。  
 *  
 * See src/test/isolation/specs/subxid-overflow.spec if you change this.  
 * 如果更改此值，请参阅src/test/isolation/specs/subxid-overflow.spec。  
 */  
#define PGPROC_MAX_CACHED_SUBXIDS 64    /* XXX guessed-at value */  
#define PGPROC_MAX_CACHED_SUBXIDS 64    /* XXX 估计值 */  

typedef struct XidCacheStatus  
{  
    /* number of cached subxids, never more than PGPROC_MAX_CACHED_SUBXIDS */  
    uint8       count;  
    /* 缓存的子事务ID数量，永远不超过PGPROC_MAX_CACHED_SUBXIDS */  
    
    /* has PGPROC->subxids overflowed */  
    bool        overflowed;  
    /* PGPROC->subxids是否溢出 */  
} XidCacheStatus;  

struct XidCache  
{  
    TransactionId xids[PGPROC_MAX_CACHED_SUBXIDS];  
    /* 子事务ID的缓存数组 */  
};  

/*  
 * Flags for PGPROC->statusFlags and PROC_HDR->statusFlags[]  
 * PGPROC->statusFlags和PROC_HDR->statusFlags[]的标志位  
 */  
#define         PROC_IS_AUTOVACUUM     0x01    /* is it an autovac worker? */  
#define         PROC_IS_AUTOVACUUM     0x01    /* 是否为自动清理工作进程 */  
#define         PROC_IN_VACUUM         0x02    /* currently running lazy vacuum */  
#define         PROC_IN_VACUUM         0x02    /* 当前正在运行惰性清理 */  
#define         PROC_IN_SAFE_IC        0x04    /* currently running CREATE INDEX  
                                                 * CONCURRENTLY or REINDEX  
                                                 * CONCURRENTLY on non-expressional,  
                                                 * non-partial index */  
#define         PROC_IN_SAFE_IC        0x04    /* 当前正在运行CREATE INDEX CONCURRENTLY或  
                                                 * REINDEX CONCURRENTLY操作（针对非表达式、  
                                                 * 非部分索引） */  
#define         PROC_VACUUM_FOR_WRAPAROUND       0x08    /* set by autovac only */  
#define         PROC_VACUUM_FOR_WRAPAROUND       0x08    /* 仅由自动清理设置 */  
#define         PROC_IN_LOGICAL_DECODING        0x10    /* currently doing logical  
                                                         * decoding outside xact */  
#define         PROC_IN_LOGICAL_DECODING        0x10    /* 当前在事务外进行逻辑解码 */  
#define         PROC_AFFECTS_ALL_HORIZONS       0x20    /* this proc's xmin must be  
                                                         * included in vacuum horizons  
                                                         * in all databases */  
#define         PROC_AFFECTS_ALL_HORIZONS       0x20    /* 该进程的xmin必须包含在所有数据库的清理边界中 */  

/* flags reset at EOXact */  
#define         PROC_VACUUM_STATE_MASK  
    (PROC_IN_VACUUM | PROC_IN_SAFE_IC | PROC_VACUUM_FOR_WRAPAROUND)  
/* 在事务结束时重置的标志 */  
#define         PROC_VACUUM_STATE_MASK  
    (PROC_IN_VACUUM | PROC_IN_SAFE_IC | PROC_VACUUM_FOR_WRAPAROUND)  

/*  
 * Xmin-related flags. Make sure any flags that affect how the process' Xmin  
 * value is interpreted by VACUUM are included here.  
 * 与Xmin相关的标志。确保任何影响VACUUM如何解释进程Xmin值的标志都包含在此处。  
 */  
#define         PROC_XMIN_FLAGS (PROC_IN_VACUUM | PROC_IN_SAFE_IC)  
#define         PROC_XMIN_FLAGS (PROC_IN_VACUUM | PROC_IN_SAFE_IC)  

/*  
 * We allow a small number of "weak" relation locks (AccessShareLock,  
 * RowShareLock, RowExclusiveLock) to be recorded in the PGPROC structure  
 * rather than the main lock table.  This eases contention on the lock  
 * manager LWLocks.  See storage/lmgr/README for additional details.  
 * 我们允许将少量"弱"关系锁（AccessShareLock、RowShareLock、RowExclusiveLock）记录在PGPROC结构中，  
 * 而不是主锁表中。这可以减轻锁管理器LWLocks上的争用。有关更多详情，请参见storage/lmgr/README。  
 */  
#define         FP_LOCK_SLOTS_PER_BACKEND 16  
#define         FP_LOCK_SLOTS_PER_BACKEND 16  

/*  
 * Flags for PGPROC.delayChkptFlags  
 * PGPROC.delayChkptFlags的标志位  
 *  
 * These flags can be used to delay the start or completion of a checkpoint  
 * for short periods. A flag is in effect if the corresponding bit is set in  
 * the PGPROC of any backend.  
 * 这些标志可用于在短时间内延迟检查点的开始或完成。如果任何后端的PGPROC中设置了相应的位，则标志生效。  
 *  
 * For our purposes here, a checkpoint has three phases: (1) determine the  
 * location to which the redo pointer will be moved, (2) write all the  
 * data durably to disk, and (3) WAL-log the checkpoint.  
 * 对于此处的目的，检查点有三个阶段：(1)确定重做指针将移动到的位置，(2)将所有数据持久化写入磁盘，  
 * 以及(3)将检查点写入WAL日志。  
 *  
 * Setting DELAY_CHKPT_START prevents the system from moving from phase 1  
 * to phase 2. This is useful when we are performing a WAL-logged modification  
 * of data that will be flushed to disk in phase 2. By setting this flag  
 * before writing WAL and clearing it after we've both written WAL and  
 * performed the corresponding modification, we ensure that if the WAL record  
 * is inserted prior to the new redo point, the corresponding data changes will  
 * also be flushed to disk before the checkpoint can complete. (In the  
 * extremely common case where the data being modified is in shared buffers  
 * and we acquire an exclusive content lock on the relevant buffers before  
 * writing WAL, this mechanism is not needed, because phase 2 will block  
 * until we release the content lock and then flush the modified data to  
 * disk.)  
 * 设置DELAY_CHKPT_START可以防止系统从阶段1移动到阶段2。这在我们执行WAL日志记录的数据修改时很有用，  
 * 因为这些数据将在阶段2刷新到磁盘。通过在写入WAL之前设置此标志，并在我们既写入了WAL又执行了相应的  
 * 修改之后清除它，我们确保如果WAL记录在新的重做点之前插入，相应的数据更改也将在检查点完成之前  
 * 刷新到磁盘。（在极常见的情况下，即被修改的数据在共享缓冲区中，并且我们在写入WAL之前获取了对相关  
 * 缓冲区的独占内容锁，则不需要此机制，因为阶段2将阻塞直到我们释放内容锁并将修改的数据刷新到磁盘。）  
 *  
 * Setting DELAY_CHKPT_COMPLETE prevents the system from moving from phase 2  
 * to phase 3. This is useful if we are performing a WAL-logged operation that  
 * might invalidate buffers, such as relation truncation. In this case, we need  
 * to ensure that any buffers which were invalidated and thus not flushed by  
 * the checkpoint are actually destroyed on disk. Replay can cope with a file  
 * or block that doesn't exist, but not with a block that has the wrong  
 * contents.  
 * 设置DELAY_CHKPT_COMPLETE可以防止系统从阶段2移动到阶段3。这在我们执行可能会使缓冲区无效的WAL日志记录  
 * 操作（例如关系截断）时很有用。在这种情况下，我们需要确保任何被无效化并且因此未被检查点刷新的缓冲区  
 * 实际上在磁盘上被销毁。重放可以处理不存在的文件或块，但不能处理内容错误的块。  
 */  
#define DELAY_CHKPT_START        (1<<0)  
#define DELAY_CHKPT_START        (1<<0)    /* 延迟检查点开始 */  
#define DELAY_CHKPT_COMPLETE     (1<<1)  
#define DELAY_CHKPT_COMPLETE     (1<<1)    /* 延迟检查点完成 */  

typedef enum  
{  
    PROC_WAIT_STATUS_OK,  
    PROC_WAIT_STATUS_WAITING,  
    PROC_WAIT_STATUS_ERROR,  
} ProcWaitStatus;  

typedef enum  
{  
    PROC_WAIT_STATUS_OK,        /* 等待状态正常 */  
    PROC_WAIT_STATUS_WAITING,   /* 正在等待 */  
    PROC_WAIT_STATUS_ERROR,     /* 等待出错 */  
} ProcWaitStatus;  

/*  
 * Each backend has a PGPROC struct in shared memory.  There is also a list of  
 * currently-unused PGPROC structs that will be reallocated to new backends.  
 * 每个后端在共享内存中都有一个PGPROC结构体。还有一个当前未使用的PGPROC结构体列表，将重新分配给新的后端。  
 *  
 * links: list link for any list the PGPROC is in.  When waiting for a lock,  
 * the PGPROC is linked into that lock's waitProcs queue.  A recycled PGPROC  
 * is linked into ProcGlobal's freeProcs list.  
 * links: PGPROC所在任何列表的链接。等待锁时，PGPROC被链接到该锁的waitProcs队列中。  
 * 被回收的PGPROC被链接到ProcGlobal的freeProcs列表中。  
 *  
 * Note: twophase.c also sets up a dummy PGPROC struct for each currently  
 * prepared transaction.  These PGPROCs appear in the ProcArray data structure  
 * so that the prepared transactions appear to be still running and are  
 * correctly shown as holding locks.  A prepared transaction PGPROC can be  
 * distinguished from a real one at need by the fact that it has pid == 0.  
 * The semaphore and lock-activity fields in a prepared-xact PGPROC are unused,  
 * but its myProcLocks[] lists are valid.  
 * 注意：twophase.c还为每个当前准备好的事务设置了一个虚拟PGPROC结构体。这些PGPROC出现在ProcArray数据结构中，  
 * 以便准备好的事务看起来仍在运行，并正确显示为持有锁。必要时，可以通过pid == 0的事实将准备事务的PGPROC  
 * 与真实的PGPROC区分开来。准备事务的PGPROC中的信号量和锁活动字段未使用，但其myProcLocks[]列表是有效的。  
 *  
 * We allow many fields of this struct to be accessed without locks, such as  
 * delayChkptFlags and isBackgroundWorker. However, keep in mind that writing  
 * mirrored ones (see below) requires holding ProcArrayLock or XidGenLock in  
 * at least shared mode, so that pgxactoff does not change concurrently.  
 * 我们允许在不使用锁的情况下访问此结构体的许多字段，例如delayChkptFlags和isBackgroundWorker。  
 * 但是请记住，写入镜像字段（见下文）需要至少以共享模式持有ProcArrayLock或XidGenLock，  
 * 以确保pgxactoff不会并发更改。  
 *  
 * Mirrored fields:  
 * 镜像字段：  
 *  
 * Some fields in PGPROC (see "mirrored in ..." comment) are mirrored into an  
 * element of more densely packed ProcGlobal arrays. These arrays are indexed  
 * by PGPROC->pgxactoff. Both copies need to be maintained coherently.  
 * PGPROC中的某些字段（参见"mirrored in ..."注释）被镜像到更密集的ProcGlobal数组的元素中。  
 * 这些数组由PGPROC->pgxactoff索引。两个副本都需要保持一致。  
 *  
 * NB: The pgxactoff indexed value can *never* be accessed without holding  
 * locks.  
 * 注意：在不持有锁的情况下，*永远*不能访问pgxactoff索引值。  
 *  
 * See PROC_HDR for details.  
 * 有关详细信息，请参见PROC_HDR。  
 */  
struct PGPROC  
{  
    /* proc->links MUST BE FIRST IN STRUCT (see ProcSleep,ProcWakeup,etc) */  
    dlist_node  links;           /* list link if process is in a list */  
    /* proc->links必须位于结构体的第一个位置（参见ProcSleep、ProcWakeup等） */  
    dlist_node  links;           /* 如果进程在列表中，则为列表链接 */  
    
    dlist_head *procgloballist; /* procglobal list that owns this PGPROC */  
    dlist_head *procgloballist; /* 拥有此PGPROC的procglobal列表 */  

    PGSemaphore sem;             /* ONE semaphore to sleep on */  
    PGSemaphore sem;             /* 用于睡眠的信号量 */  
    ProcWaitStatus waitStatus;  
    ProcWaitStatus waitStatus;   /* 等待状态 */  

    Latch       procLatch;       /* generic latch for process */  
    Latch       procLatch;       /* 进程的通用闩锁 */  


    TransactionId xid;           /* id of top-level transaction currently being  
                                 * executed by this proc, if running and XID  
                                 * is assigned; else InvalidTransactionId.  
                                 * mirrored in ProcGlobal->xids[pgxactoff] */  
    TransactionId xid;           /* 当前由该进程执行的顶级事务ID（如果正在运行且已分配XID）；  
                                 * 否则为InvalidTransactionId。  
                                 * 镜像到ProcGlobal->xids[pgxactoff] */  

    TransactionId xmin;          /* minimal running XID as it was when we were  
                                 * starting our xact, excluding LAZY VACUUM:  
                                 * vacuum must not remove tuples deleted by  
                                 * xid >= xmin ! */  
    TransactionId xmin;          /* 我们开始事务时的最小运行XID，不包括LAZY VACUUM：  
                                 * vacuum不得移除由xid >= xmin删除的元组！ */  

    int         pid;             /* Backend's process ID; 0 if prepared xact */  
    int         pid;             /* 后端的进程ID；准备事务时为0 */  

    int         pgxactoff;       /* offset into various ProcGlobal->arrays with  
                                 * data mirrored from this PGPROC */  
    int         pgxactoff;       /* 从该PGPROC镜像数据到各种ProcGlobal->数组的偏移量 */  

    /*  
     * Currently running top-level transaction's virtual xid. Together these  
     * form a VirtualTransactionId, but we don't use that struct because this  
     * is not atomically assignable as whole, and we want to enforce code to  
     * consider both parts separately.  See comments at VirtualTransactionId.  
     * 当前运行的顶级事务的虚拟xid。它们一起形成VirtualTransactionId，但我们不使用该结构体，  
     * 因为它不能作为整体原子分配，并且我们希望强制代码分别考虑这两个部分。  
     * 请参见VirtualTransactionId的注释。  
     */  
    struct  
    {  
        ProcNumber procNumber; /* For regular backends, equal to  
                                 * GetNumberFromPGProc(proc).  For prepared  
                                 * xacts, ID of the original backend that  
                                 * processed the transaction. For unused  
                                 * PGPROC entries, INVALID_PROC_NUMBER. */  
        ProcNumber procNumber; /* 对于常规后端，等于GetNumberFromPGProc(proc)。  
                                 * 对于准备事务，是处理事务的原始后端的ID。  
                                 * 对于未使用的PGPROC条目，为INVALID_PROC_NUMBER。 */  
        
        LocalTransactionId lxid;    /* local id of top-level transaction  
                                     * currently * being executed by this  
                                     * proc, if running; else  
                                     * InvalidLocalTransactionId */  
        LocalTransactionId lxid;    /* 当前由该进程执行的顶级事务的本地ID（如果正在运行）；  
                                     * 否则为InvalidLocalTransactionId */  
    }           vxid;  

    /* These fields are zero while a backend is still starting up: */  
    Oid         databaseId;      /* OID of database this backend is using */  
    /* 后端启动时这些字段为零： */  
    Oid         databaseId;      /* 此后端使用的数据库的OID */  
    
    Oid         roleId;          /* OID of role using this backend */  
    Oid         roleId;          /* 使用此后端的角色的OID */  

    Oid         tempNamespaceId; /* OID of temp schema this backend is  
                                 * using */  
    Oid         tempNamespaceId; /* 此后端使用的临时模式的OID */  

    bool        isBackgroundWorker; /* true if not a regular backend. */  
    bool        isBackgroundWorker; /* 如果不是常规后端则为true。 */  

    /*  
     * While in hot standby mode, shows that a conflict signal has been sent  
     * for the current transaction. Set/cleared while holding ProcArrayLock,  
     * though not required. Accessed without lock, if needed.  
     * 在热备模式下，表示已向当前事务发送冲突信号。在持有ProcArrayLock时设置/清除，  
     * 尽管不是必需的。如果需要，可以在不锁定的情况下访问。  
     */  
    bool        recoveryConflictPending;  
    bool        recoveryConflictPending;  /* 恢复冲突待处理标志 */  

    /* Info about LWLock the process is currently waiting for, if any. */  
    /* 如果进程当前正在等待LWLock，则有关该锁的信息。 */  
    uint8       lwWaiting;       /* see LWLockWaitState */  
    uint8       lwWaiting;       /* 参见LWLockWaitState */  
    uint8       lwWaitMode;      /* lwlock mode being waited for */  
    uint8       lwWaitMode;      /* 正在等待的lwlock模式 */  
    proclist_node lwWaitLink;    /* position in LW lock wait list */  
    proclist_node lwWaitLink;    /* 在LW锁等待列表中的位置 */  

    /* Support for condition variables. */  
    /* 条件变量支持 */  
    proclist_node cvWaitLink;    /* position in CV wait list */  
    proclist_node cvWaitLink;    /* 在CV等待列表中的位置 */  

    /* Info about lock the process is currently waiting for, if any. */  
    /* 如果进程当前正在等待锁，则有关该锁的信息。 */  
    /* waitLock and waitProcLock are NULL if not currently waiting. */  
    /* 如果当前未等待，则waitLock和waitProcLock为NULL。 */  
    LOCK       *waitLock;        /* Lock object we're sleeping on ... */  
    LOCK       *waitLock;        /* 我们正在等待的锁对象... */  
    PROCLOCK   *waitProcLock;    /* Per-holder info for awaited lock */  
    PROCLOCK   *waitProcLock;    /* 等待锁的每个持有者的信息 */  
    LOCKMODE    waitLockMode;    /* type of lock we're waiting for */  
    LOCKMODE    waitLockMode;    /* 我们正在等待的锁类型 */  
    LOCKMASK    heldLocks;       /* bitmask for lock types already held on this  
                                 * lock object by this backend */  
    LOCKMASK    heldLocks;       /* 此后端已在此锁对象上持有的锁类型的位掩码 */  
    pg_atomic_uint64 waitStart; /* time at which wait for lock acquisition  
                                 * started */  
    pg_atomic_uint64 waitStart; /* 开始等待锁获取的时间 */  

    int         delayChkptFlags; /* for DELAY_CHKPT_* flags */  
    int         delayChkptFlags; /* 用于DELAY_CHKPT_*标志 */  

    uint8       statusFlags;     /* this backend's status flags, see PROC_*  
                                 * above. mirrored in  
                                 * ProcGlobal->statusFlags[pgxactoff] */  
    uint8       statusFlags;     /* 此后端的状态标志，参见上面的PROC_*。  
                                 * 镜像到ProcGlobal->statusFlags[pgxactoff] */  

    /*  
     * Info to allow us to wait for synchronous replication, if needed.  
     * waitLSN is InvalidXLogRecPtr if not waiting; set only by user backend.  
     * syncRepState must not be touched except by owning process or WALSender.  
     * syncRepLinks used only while holding SyncRepLock.  
     * 允许我们在需要时等待同步复制的信息。如果不等待，则waitLSN为InvalidXLogRecPtr；  
     * 仅由用户后端设置。除了拥有进程或WALSender之外，不得触碰syncRepState。  
     * 仅在持有SyncRepLock时使用syncRepLinks。  
     */  
    XLogRecPtr  waitLSN;         /* waiting for this LSN or higher */  
    XLogRecPtr  waitLSN;         /* 等待此LSN或更高 */  
    int         syncRepState;    /* wait state for sync rep */  
    int         syncRepState;    /* 同步复制的等待状态 */  
    dlist_node  syncRepLinks;    /* list link if process is in syncrep queue */  
    dlist_node  syncRepLinks;    /* 如果进程在同步复制队列中，则为列表链接 */  

    /*  
     * All PROCLOCK objects for locks held or awaited by this backend are  
     * linked into one of these lists, according to the partition number of  
     * their lock.  
     * 此后端持有的或等待的所有PROCLOCK对象根据其锁的分区号链接到这些列表之一中。  
     */  
    dlist_head  myProcLocks[NUM_LOCK_PARTITIONS];  
    dlist_head  myProcLocks[NUM_LOCK_PARTITIONS]; /* 本进程持有的或等待的锁列表 */  

    XidCacheStatus subxidStatus; /* mirrored with  
                                 * ProcGlobal->subxidStates[i] */  
    XidCacheStatus subxidStatus; /* 与ProcGlobal->subxidStates[i]镜像 */  
    struct XidCache subxids;     /* cache for subtransaction XIDs */  
    struct XidCache subxids;     /* 子事务XID的缓存 */  

    /* Support for group XID clearing. */  
    /* 组XID清除支持 */  
    /* true, if member of ProcArray group waiting for XID clear */  
    bool        procArrayGroupMember;  
    /* 如果是等待XID清除的ProcArray组成员则为true */  
    bool        procArrayGroupMember;  
    /* next ProcArray group member waiting for XID clear */  
    pg_atomic_uint32 procArrayGroupNext;  
    /* 下一个等待XID清除的ProcArray组成员 */  
    pg_atomic_uint32 procArrayGroupNext;  

    /*  
     * latest transaction id among the transaction's main XID and  
     * subtransactions  
     * 事务的主XID和子事务中的最新事务ID  
     */  
    TransactionId procArrayGroupMemberXid;  
    TransactionId procArrayGroupMemberXid;  

    uint32      wait_event_info; /* proc's wait information */  
    uint32      wait_event_info; /* 进程的等待信息 */  

    /* Support for group transaction status update. */  
    /* 组事务状态更新支持 */  
    bool        clogGroupMember; /* true, if member of clog group */  
    bool        clogGroupMember; /* 如果是clog组成员则为true */  
    pg_atomic_uint32 clogGroupNext; /* next clog group member */  
    pg_atomic_uint32 clogGroupNext; /* 下一个clog组成员 */  
    TransactionId clogGroupMemberXid; /* transaction id of clog group member */  
    TransactionId clogGroupMemberXid; /* clog组成员的事务ID */  
    XidStatus   clogGroupMemberXidStatus; /* transaction status of clog  
                                         * group member */  
    XidStatus   clogGroupMemberXidStatus; /* clog组成员的事务状态 */  
    int64       clogGroupMemberPage; /* clog page corresponding to  
                                     * transaction id of clog group member */  
    int64       clogGroupMemberPage; /* 对应于clog组成员事务ID的clog页面 */  
    XLogRecPtr  clogGroupMemberLsn; /* WAL location of commit record for clog  
                                     * group member */  
    XLogRecPtr  clogGroupMemberLsn; /* clog组成员提交记录的WAL位置 */  

    /* Lock manager data, recording fast-path locks taken by this backend. */  
    /* 锁管理器数据，记录此后端获取的快速路径锁 */  
    LWLock      fpInfoLock;      /* protects per-backend fast-path state */  
    LWLock      fpInfoLock;      /* 保护每个后端的快速路径状态 */  
    uint64      fpLockBits;      /* lock modes held for each fast-path slot */  
    uint64      fpLockBits;      /* 每个快速路径槽持有的锁模式 */  
    Oid         fpRelId[FP_LOCK_SLOTS_PER_BACKEND]; /* slots for rel oids */  
    Oid         fpRelId[FP_LOCK_SLOTS_PER_BACKEND]; /* 关系OID的槽位 */  
    bool        fpVXIDLock;      /* are we holding a fast-path VXID lock? */  
    bool        fpVXIDLock;      /* 我们是否持有快速路径VXID锁？ */  
    LocalTransactionId fpLocalTransactionId; /* lxid for fast-path VXID  
                                             * lock */  
    LocalTransactionId fpLocalTransactionId; /* 快速路径VXID锁的lxid */  

    /*  
     * Support for lock groups.  Use LockHashPartitionLockByProc on the group  
     * leader to get the LWLock protecting these fields.  
     * 锁组支持。对组领导者使用LockHashPartitionLockByProc获取保护这些字段的LWLock。  
     */  
    PGPROC     *lockGroupLeader; /* lock group leader, if I'm a member */  
    PGPROC     *lockGroupLeader; /* 锁组领导者，如果我是成员 */  
    dlist_head  lockGroupMembers; /* list of members, if I'm a leader */  
    dlist_head  lockGroupMembers; /* 成员列表，如果我是领导者 */  
    dlist_node  lockGroupLink;   /* my member link, if I'm a member */  
    dlist_node  lockGroupLink;   /* 我的成员链接，如果我是成员 */  
};  

/* NOTE: "typedef struct PGPROC PGPROC" appears in storage/lock.h. */  
/* 注意："typedef struct PGPROC PGPROC"出现在storage/lock.h中。 */  


extern PGDLLIMPORT PGPROC *MyProc;  

extern PGDLLIMPORT PGPROC *MyProc;  

/*  
 * There is one ProcGlobal struct for the whole database cluster.  
 * 整个数据库集群只有一个ProcGlobal结构体。  
 *  
 * Adding/Removing an entry into the procarray requires holding *both*  
 * ProcArrayLock and XidGenLock in exclusive mode (in that order). Both are  
 * needed because the dense arrays (see below) are accessed from  
 * GetNewTransactionId() and GetSnapshotData(), and we don't want to add  
 * further contention by both using the same lock. Adding/Removing a procarray  
 * entry is much less frequent.  
 * 向procarray添加/删除条目需要以独占模式持有*两个*锁：ProcArrayLock和XidGenLock（按此顺序）。  
 * 两者都需要，因为密集数组（见下文）可以从GetNewTransactionId()和GetSnapshotData()访问，  
 * 我们不希望通过都使用同一个锁来增加争用。添加/删除procarray条目的频率要低得多。  
 *  
 * Some fields in PGPROC are mirrored into more densely packed arrays (e.g.  
 * xids), with one entry for each backend. These arrays only contain entries  
 * for PGPROCs that have been added to the shared array with ProcArrayAdd()  
 * (in contrast to PGPROC array which has unused PGPROCs interspersed).  
 * PGPROC中的某些字段被镜像到更密集的数组中（例如xids），每个后端有一个条目。  
 * 这些数组只包含已通过ProcArrayAdd()添加到共享数组的PGPROC的条目（与散布着未使用PGPROC的PGPROC数组相反）。  
 *  
 * The dense arrays are indexed by PGPROC->pgxactoff. Any concurrent  
 * ProcArrayAdd() / ProcArrayRemove() can lead to pgxactoff of a procarray  
 * member to change.  Therefore it is only safe to use PGPROC->pgxactoff to  
 * access the dense array while holding either ProcArrayLock or XidGenLock.  
 * 密集数组由PGPROC->pgxactoff索引。任何并发的ProcArrayAdd()/ProcArrayRemove()都可能导致procarray  
 * 成员的pgxactoff更改。因此，只有在持有ProcArrayLock或XidGenLock时，使用PGPROC->pgxactoff访问  
 * 密集数组才是安全的。  
 *  
 * As long as a PGPROC is in the procarray, the mirrored values need to be  
 * maintained in both places in a coherent manner.  
 * 只要PGPROC在procarray中，镜像值就需要在两个地方保持一致。  
 *  
 * The denser separate arrays are beneficial for three main reasons: First, to  
 * allow for as tight loops accessing the data as possible. Second, to prevent  
 * updates of frequently changing data (e.g. xmin) from invalidating  
 * cachelines also containing less frequently changing data (e.g. xid,  
 * statusFlags). Third to condense frequently accessed data into as few  
 * cachelines as possible.  
 * 更密集的单独数组有三个主要好处：首先，允许尽可能紧凑地循环访问数据。其次，防止频繁更改的数据  
 * （例如xmin）的更新使同时包含不太频繁更改的数据（例如xid、statusFlags）的缓存行失效。  
 * 第三，将频繁访问的数据压缩到尽可能少的缓存行中。  
 *  
 * There are two main reasons to have the data mirrored between these dense  
 * arrays and PGPROC. First, as explained above, a PGPROC's array entries can  
 * only be accessed with either ProcArrayLock or XidGenLock held, whereas the  
 * PGPROC entries do not require that (obviously there may still be locking  
 * requirements around the individual field, separate from the concerns  
 * here). That is particularly important for a backend to efficiently checks  
 * it own values, which it often can safely do without locking.  Second, the  
 * PGPROC fields allow to avoid unnecessary accesses and modification to the  
 * dense arrays. A backend's own PGPROC is more likely to be in a local cache,  
 * whereas the cachelines for the dense array will be modified by other  
 * backends (often removing it from the cache for other cores/sockets). At  
 * commit/abort time a check of the PGPROC value can avoid accessing/dirtying  
 * the corresponding array value.  
 * 在这些密集数组和PGPROC之间镜像数据有两个主要原因。首先，如上所述，PGPROC的数组条目只能在持有  
 * ProcArrayLock或XidGenLock的情况下访问，而PGPROC条目不需要这样（显然，围绕各个字段可能仍然有  
 * 锁定要求，与这里的关注点分开）。这对于后端有效地检查自己的值尤为重要，它通常可以在不锁定的情况下  
 * 安全地进行。其次，PGPROC字段允许避免对密集数组的不必要访问和修改。后端自己的PGPROC更可能在本地  
 * 缓存中，而密集数组的缓存行将被其他后端修改（通常会从其他核心/套接字的缓存中移除）。在提交/中止时，  
 * 检查PGPROC值可以避免访问/弄脏相应的数组值。  
 *  
 * Basically it makes sense to access the PGPROC variable when checking a  
 * single backend's data, especially when already looking at the PGPROC for  
 * other reasons already.  It makes sense to look at the "dense" arrays if we  
 * need to look at many / most entries, because we then benefit from the  
 * reduced indirection and better cross-process cache-ability.  
 * 基本上，在检查单个后端的数据时访问PGPROC变量是有意义的，特别是当已经因为其他原因查看PGPROC时。  
 * 如果我们需要查看许多/大多数条目，查看"密集"数组是有意义的，因为这样我们可以受益于减少的间接  
 * 访问和更好的跨进程缓存能力。  
 *  
 * When entering a PGPROC for 2PC transactions with ProcArrayAdd(), the data  
 * in the dense arrays is initialized from the PGPROC while it already holds  
 * ProcArrayLock.  
 * 当使用ProcArrayAdd()为2PC事务输入PGPROC时，密集数组中的数据在已经持有ProcArrayLock的同时  
 * 从PGPROC初始化。  
 */  
typedef struct PROC_HDR  
{  
    /* Array of PGPROC structures (not including dummies for prepared txns) */  
    PGPROC     *allProcs;  
    /* PGPROC结构体数组（不包括准备事务的虚拟结构） */  
    PGPROC     *allProcs;  

    /* Array mirroring PGPROC.xid for each PGPROC currently in the procarray */  
    TransactionId *xids;  
    /* 为procarray中当前每个PGPROC镜像PGPROC.xid的数组 */  
    TransactionId *xids;  

    /*  
     * Array mirroring PGPROC.subxidStatus for each PGPROC currently in the  
     * procarray.  
     * 为procarray中当前每个PGPROC镜像PGPROC.subxidStatus的数组。  
     */  
    XidCacheStatus *subxidStates;  
    XidCacheStatus *subxidStates;  

    /*  
     * Array mirroring PGPROC.statusFlags for each PGPROC currently in the  
     * procarray.  
     * 为procarray中当前每个PGPROC镜像PGPROC.statusFlags的数组。  
     */  
    uint8      *statusFlags;  
    uint8      *statusFlags;  

    /* Length of allProcs array */  
    uint32      allProcCount;  
    /* allProcs数组的长度 */  
    uint32      allProcCount;  
    /* Head of list of free PGPROC structures */  
    dlist_head  freeProcs;  
    /* 空闲PGPROC结构体列表的头部 */  
    dlist_head  freeProcs;  
    /* Head of list of autovacuum & special worker free PGPROC structures */  
    dlist_head  autovacFreeProcs;  
    /* 自动清理和特殊工作进程空闲PGPROC结构体列表的头部 */  
    dlist_head  autovacFreeProcs;  
    /* Head of list of bgworker free PGPROC structures */  
    dlist_head  bgworkerFreeProcs;  
    /* 后台工作进程空闲PGPROC结构体列表的头部 */  
    dlist_head  bgworkerFreeProcs;  
    /* Head of list of walsender free PGPROC structures */  
    dlist_head  walsenderFreeProcs;  
    /* WAL发送进程空闲PGPROC结构体列表的头部 */  
    dlist_head  walsenderFreeProcs;  
    /* First pgproc waiting for group XID clear */  
    pg_atomic_uint32 procArrayGroupFirst;  
    /* 等待组XID清除的第一个pgproc */  
    pg_atomic_uint32 procArrayGroupFirst;  
    /* First pgproc waiting for group transaction status update */  
    pg_atomic_uint32 clogGroupFirst;  
    /* 等待组事务状态更新的第一个pgproc */  
    pg_atomic_uint32 clogGroupFirst;  
    /* WALWriter process's latch */  
    Latch      *walwriterLatch;  
    /* WALWriter进程的闩锁 */  
    Latch      *walwriterLatch;  
    /* Checkpointer process's latch */  
    Latch      *checkpointerLatch;  
    /* Checkpointer进程的闩锁 */  
    Latch      *checkpointerLatch;  
    /* Current shared estimate of appropriate spins_per_delay value */  
    int         spins_per_delay;  
    /* 当前共享的适当spins_per_delay值的估计 */  
    int         spins_per_delay;  
    /* Buffer id of the buffer that Startup process waits for pin on, or -1 */  
    int         startupBufferPinWaitBufId;  
    /* Startup进程等待pin的缓冲区的buffer id，或-1 */  
    int         startupBufferPinWaitBufId;  
} PROC_HDR;  

extern PGDLLIMPORT PROC_HDR *ProcGlobal;  

extern PGDLLIMPORT PROC_HDR *ProcGlobal;  

extern PGDLLIMPORT PGPROC *PreparedXactProcs;  

extern PGDLLIMPORT PGPROC *PreparedXactProcs;  

/*  
 * Accessors for getting PGPROC given a ProcNumber and vice versa.  
 * 给定ProcNumber获取PGPROC的访问器，反之亦然。  
 */  
#define GetPGProcByNumber(n) (&ProcGlobal->allProcs[(n)])  
#define GetPGProcByNumber(n) (&ProcGlobal->allProcs[(n)])  
#define GetNumberFromPGProc(proc) ((proc) - &ProcGlobal->allProcs[0])  
#define GetNumberFromPGProc(proc) ((proc) - &ProcGlobal->allProcs[0])  

/*  
 * We set aside some extra PGPROC structures for "special worker" processes,  
 * which are full-fledged backends (they can run transactions)  
 * but are unique animals that there's never more than one of.  
 * Currently there are two such processes: the autovacuum launcher  
 * and the slotsync worker.  
 * 我们为"特殊工作进程"预留了一些额外的PGPROC结构，这些进程是完整的后端（它们可以运行事务），  
 * 但都是独一无二的，永远不会有多个。目前有两个这样的进程：自动清理启动器和slotsync工作进程。  
 */  
#define NUM_SPECIAL_WORKER_PROCS    2  
#define NUM_SPECIAL_WORKER_PROCS    2  

/*  
 * We set aside some extra PGPROC structures for auxiliary processes,  
 * ie things that aren't full-fledged backends (they cannot run transactions  
 * or take heavyweight locks) but need shmem access.  
 *  
 * Background writer, checkpointer, WAL writer, WAL summarizer, and archiver  
 * run during normal operation.  Startup process and WAL receiver also consume  
 * 2 slots, but WAL writer is launched only after startup has exited, so we  
 * only need 6 slots.  
 * 我们为辅助进程预留了一些额外的PGPROC结构，即那些不是完整后端的进程（它们不能运行事务或获取重量级锁），  
 * 但需要共享内存访问。  
 *  
 * 在正常操作期间，后台写入器、检查点进程、WAL写入器、WAL汇总器和归档器运行。Startup进程和WAL接收器也消耗  
 * 2个槽位，但WAL写入器只有在Startup退出后才启动，因此我们只需要6个槽位。  
 */  
#define NUM_AUXILIARY_PROCS         6  
#define NUM_AUXILIARY_PROCS         6  

/* configurable options */  
extern PGDLLIMPORT int DeadlockTimeout;  
extern PGDLLIMPORT int StatementTimeout;  
extern PGDLLIMPORT int LockTimeout;  
extern PGDLLIMPORT int IdleInTransactionSessionTimeout;  
extern PGDLLIMPORT int TransactionTimeout;  
extern PGDLLIMPORT int IdleSessionTimeout;  
extern PGDLLIMPORT bool log_lock_waits;  

/* 可配置选项 */  
extern PGDLLIMPORT int DeadlockTimeout;        /* 死锁超时时间 */  
extern PGDLLIMPORT int StatementTimeout;       /* 语句超时时间 */  
extern PGDLLIMPORT int LockTimeout;            /* 锁超时时间 */  
extern PGDLLIMPORT int IdleInTransactionSessionTimeout; /* 事务中闲置会话超时时间 */  
extern PGDLLIMPORT int TransactionTimeout;     /* 事务超时时间 */  
extern PGDLLIMPORT int IdleSessionTimeout;     /* 闲置会话超时时间 */  
extern PGDLLIMPORT bool log_lock_waits;        /* 是否记录锁等待 */  


/*  
 * Function Prototypes  
 * 函数原型  
 */  
extern int    ProcGlobalSemas(void);  
extern Size   ProcGlobalShmemSize(void);  
extern void   InitProcGlobal(void);  
extern void   InitProcess(void);  
extern void   InitProcessPhase2(void);  
extern void   InitAuxiliaryProcess(void);  

extern int    ProcGlobalSemas(void);            /* 获取ProcGlobal所需的信号量数量 */  
extern Size   ProcGlobalShmemSize(void);        /* 计算ProcGlobal所需的共享内存大小 */  
extern void   InitProcGlobal(void);             /* 初始化ProcGlobal结构体 */  
extern void   InitProcess(void);                /* 初始化当前进程的PGPROC */  
extern void   InitProcessPhase2(void);          /* 初始化进程的第二阶段 */  
extern void   InitAuxiliaryProcess(void);       /* 初始化辅助进程 */  

extern void   SetStartupBufferPinWaitBufId(int bufid);  
extern int    GetStartupBufferPinWaitBufId(void);  

extern void   SetStartupBufferPinWaitBufId(int bufid);  /* 设置Startup进程等待pin的缓冲区ID */  
extern int    GetStartupBufferPinWaitBufId(void);      /* 获取Startup进程等待pin的缓冲区ID */  

extern bool   HaveNFreeProcs(int n, int *nfree);  
extern void   ProcReleaseLocks(bool isCommit);  

extern bool   HaveNFreeProcs(int n, int *nfree);  /* 检查是否有n个空闲的PGPROC */  
extern void   ProcReleaseLocks(bool isCommit);     /* 释放进程持有的所有锁 */  

extern ProcWaitStatus ProcSleep(LOCALLOCK *locallock,  
                                LockMethod lockMethodTable,  
                                bool dontWait);  
extern void   ProcWakeup(PGPROC *proc, ProcWaitStatus waitStatus);  
extern void   ProcLockWakeup(LockMethod lockMethodTable, LOCK *lock);  
extern void   CheckDeadLockAlert(void);  
extern bool   IsWaitingForLock(void);  
extern void   LockErrorCleanup(void);  

extern ProcWaitStatus ProcSleep(LOCALLOCK *locallock,  
                                LockMethod lockMethodTable,  
                                bool dontWait);  /* 等待锁获取 */  
extern void   ProcWakeup(PGPROC *proc, ProcWaitStatus waitStatus);  /* 唤醒等待的进程 */  
extern void   ProcLockWakeup(LockMethod lockMethodTable, LOCK *lock); /* 唤醒等待特定锁的所有进程 */  
extern void   CheckDeadLockAlert(void);       /* 检查死锁并发出警报 */  
extern bool   IsWaitingForLock(void);         /* 检查当前进程是否正在等待锁 */  
extern void   LockErrorCleanup(void);         /* 锁错误清理 */  

extern void   ProcWaitForSignal(uint32 wait_event_info);  
extern void   ProcSendSignal(ProcNumber procNumber);  

extern void   ProcWaitForSignal(uint32 wait_event_info); /* 等待信号 */  
extern void   ProcSendSignal(ProcNumber procNumber);    /* 向指定进程发送信号 */  

extern PGPROC *AuxiliaryPidGetProc(int pid);  

extern PGPROC *AuxiliaryPidGetProc(int pid);   /* 通过PID获取辅助进程的PGPROC */  

extern void   BecomeLockGroupLeader(void);  
extern bool   BecomeLockGroupMember(PGPROC *leader, int pid);  

extern void   BecomeLockGroupLeader(void);       /* 成为锁组领导者 */  
extern bool   BecomeLockGroupMember(PGPROC *leader, int pid); /* 成为锁组成员 */  

#endif                          /* _PROC_H_ */  
#endif                          /* _PROC_H_ */
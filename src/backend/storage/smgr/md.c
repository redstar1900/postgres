/*-------------------------------------------------------------------------
 *
 * md.c
 *	  This code manages relations that reside on magnetic disk.
 *
 * Or at least, that was what the Berkeley folk had in mind when they named
 * this file.  In reality, what this code provides is an interface from
 * the smgr API to Unix-like filesystem APIs, so it will work with any type
 * of device for which the operating system provides filesystem support.
 * It doesn't matter whether the bits are on spinning rust or some other
 * storage technology.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/smgr/md.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>

#include "access/xlog.h"
#include "access/xlogutils.h"
#include "commands/tablespace.h"
#include "miscadmin.h"
#include "pg_trace.h"
#include "pgstat.h"
#include "postmaster/bgwriter.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/md.h"
#include "storage/relfilelocator.h"
#include "storage/smgr.h"
#include "storage/sync.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

/*
 * The magnetic disk storage manager keeps track of open file
 * descriptors in its own descriptor pool.  This is done to make it
 * easier to support relations that are larger than the operating
 * system's file size limit (often 2GBytes).  In order to do that,
 * we break relations up into "segment" files that are each shorter than
 * the OS file size limit.  The segment size is set by the RELSEG_SIZE
 * configuration constant in pg_config.h.
 *
 * On disk, a relation must consist of consecutively numbered segment
 * files in the pattern
 *	-- Zero or more full segments of exactly RELSEG_SIZE blocks each
 *	-- Exactly one partial segment of size 0 <= size < RELSEG_SIZE blocks
 *	-- Optionally, any number of inactive segments of size 0 blocks.
 * The full and partial segments are collectively the "active" segments.
 * Inactive segments are those that once contained data but are currently
 * not needed because of an mdtruncate() operation.  The reason for leaving
 * them present at size zero, rather than unlinking them, is that other
 * backends and/or the checkpointer might be holding open file references to
 * such segments.  If the relation expands again after mdtruncate(), such
 * that a deactivated segment becomes active again, it is important that
 * such file references still be valid --- else data might get written
 * out to an unlinked old copy of a segment file that will eventually
 * disappear.
 *
 * File descriptors are stored in the per-fork md_seg_fds arrays inside
 * SMgrRelation. The length of these arrays is stored in md_num_open_segs.
 * Note that a fork's md_num_open_segs having a specific value does not
 * necessarily mean the relation doesn't have additional segments; we may
 * just not have opened the next segment yet.  (We could not have "all
 * segments are in the array" as an invariant anyway, since another backend
 * could extend the relation while we aren't looking.)  We do not have
 * entries for inactive segments, however; as soon as we find a partial
 * segment, we assume that any subsequent segments are inactive.
 *
 * The entire MdfdVec array is palloc'd in the MdCxt memory context.
 */
/*
 * 磁盘存储管理器负责跟踪未打开的文件
 * 描述子在其独立描述子池中。 这样做是为了制造
 * 支持规模大于运营的关系更为容易
 * 系统文件大小限制（通常为2GByte）。 为了实现这一点，
 * 我们将关系拆分为“段”文件，每个文件都比
 * 操作系统文件大小限制。 分段大小由RELSEG_SIZE决定
 * 配置常数以pg_config.h为单位。
 *
 * 在圆盘上，关系必须由连续编号的段组成
 * 模式中的文件
 * —— 零个或多个恰好为RELSEG_SIZE块的完整段
 * —— 恰好有一个部分段大小为0 <= 块数< RELSEG_SIZE
 * ——可选地，任意数量大小为0的非活跃段。
 * 完整和部分片段合称为“活跃”片段。
 * 非活跃段是指曾经包含数据但目前已不存在的段
 * 由于 mdtruncate（） 操作，不需要。 离开的原因
 * 它们存在于零大小时，不是解除关联，而是
 * 后端和/或检查点可能持有打开的文件引用
 * 这样的片段。 如果关系在mdtruncate（）之后再次展开，则
 * 当一个停用的片段重新激活时，这一点很重要
 * 此类文件引用仍然有效---否则数据可能会被写入
 * 导出一个未关联的旧段文件副本，该文件最终会
 * 消失。
 *
 * 文件描述符存储在每个分支的md_seg_fds数组中
 * SMgrRelation。这些数组的长度存储在md_num_open_segs中。
 * 注意，分支md_num_open_segs具有特定值并不代表
 * 必然意味着该关系没有额外的段;我们可能会
 * 只是还没打开下一个片段。 （我们不可能拥有“全部”
 * 段在数组中“作为不变量，因为另一个后端
 * 可以在我们不注意时延长关系。） 我们没有
 * 非活跃片段的条目;只要我们找到部分
 * 段，我们假设后续段均为不活跃。
 *
 * 整个 MdfdVec 数组在 MdCxt 内存上下文中被调色。
 */


typedef struct _MdfdVec
{
	File		mdfd_vfd;		/* fd number in fd.c's pool */
	BlockNumber mdfd_segno;		/* segment number, from 0 */
} MdfdVec;

static MemoryContext MdCxt;		/* context for all MdfdVec objects */


/* Populate a file tag describing an md.c segment file. */
#define INIT_MD_FILETAG(a,xx_rlocator,xx_forknum,xx_segno) \
( \
	memset(&(a), 0, sizeof(FileTag)), \
	(a).handler = SYNC_HANDLER_MD, \
	(a).rlocator = (xx_rlocator), \
	(a).forknum = (xx_forknum), \
	(a).segno = (xx_segno) \
)


/*** behavior for mdopen & _mdfd_getseg ***/
/* ereport if segment not present */
#define EXTENSION_FAIL				(1 << 0)
/* return NULL if segment not present */
#define EXTENSION_RETURN_NULL		(1 << 1)
/* create new segments as needed */
#define EXTENSION_CREATE			(1 << 2)
/* create new segments if needed during recovery */
#define EXTENSION_CREATE_RECOVERY	(1 << 3)
/*
 * Allow opening segments which are preceded by segments smaller than
 * RELSEG_SIZE, e.g. inactive segments (see above). Note that this breaks
 * mdnblocks() and related functionality henceforth - which currently is ok,
 * because this is only required in the checkpointer which never uses
 * mdnblocks().
 */
/*
 * 允许开启段落前置于
 * 有 RELSEG_SIZE，例如非活跃段（见上文）。注意这点有问题
 * mdnblocks（） 及相关功能，目前是可以接受的，
 * 因为这只在检查点中要求，而检查点从未使用
 * mdnblocks（）。
 */
#define EXTENSION_DONT_CHECK_SIZE	(1 << 4)
/* don't try to open a segment, if not already open */
#define EXTENSION_DONT_OPEN			(1 << 5)


/* local routines */
static void mdunlinkfork(RelFileLocatorBackend rlocator, ForkNumber forknum,
						 bool isRedo);
static MdfdVec *mdopenfork(SMgrRelation reln, ForkNumber forknum, int behavior);
static void register_dirty_segment(SMgrRelation reln, ForkNumber forknum,
								   MdfdVec *seg);
static void register_unlink_segment(RelFileLocatorBackend rlocator, ForkNumber forknum,
									BlockNumber segno);
static void register_forget_request(RelFileLocatorBackend rlocator, ForkNumber forknum,
									BlockNumber segno);
static void _fdvec_resize(SMgrRelation reln,
						  ForkNumber forknum,
						  int nseg);
static char *_mdfd_segpath(SMgrRelation reln, ForkNumber forknum,
						   BlockNumber segno);
static MdfdVec *_mdfd_openseg(SMgrRelation reln, ForkNumber forknum,
							  BlockNumber segno, int oflags);
static MdfdVec *_mdfd_getseg(SMgrRelation reln, ForkNumber forknum,
							 BlockNumber blkno, bool skipFsync, int behavior);
static BlockNumber _mdnblocks(SMgrRelation reln, ForkNumber forknum,
							  MdfdVec *seg);

static inline int
_mdfd_open_flags(void)
{
	int			flags = O_RDWR | PG_BINARY;

	if (io_direct_flags & IO_DIRECT_DATA)
		flags |= PG_O_DIRECT;

	return flags;
}

/*
 * mdinit() -- Initialize private state for magnetic disk storage manager.
 */
/*
 * mdinit（） —— 初始化磁盘存储管理器的私有状态。
 */
void
mdinit(void)
{
	MdCxt = AllocSetContextCreate(TopMemoryContext,
								  "MdSmgr",
								  ALLOCSET_DEFAULT_SIZES);
}

/*
 * mdexists() -- Does the physical file exist?
 *
 * Note: this will return true for lingering files, with pending deletions
 */
/*
 * mdexists（） -- 物理文件存在吗？
 *
 * 注：对于未处理删除的文件，此方法将恢复为真
 */
bool
mdexists(SMgrRelation reln, ForkNumber forknum)
{
	/*
	 * Close it first, to ensure that we notice if the fork has been unlinked
	 * since we opened it.  As an optimization, we can skip that in recovery,
	 * which already closes relations when dropping them.
	 */
/*
 * 先关闭它，以确保我们能注意到叉是否被解绑
 * 自从我们打开它以来。 作为优化，我们可以在恢复中跳过这个过程，
 * 这在断开关系时已经关闭了。
 */
	if (!InRecovery)
		mdclose(reln, forknum);

	return (mdopenfork(reln, forknum, EXTENSION_RETURN_NULL) != NULL);
}

/*
 * mdcreate() -- Create a new relation on magnetic disk.
 *
 * If isRedo is true, it's okay for the relation to exist already.
 */
/*
 * mdcreate（） —— 在磁盘上创建一个新的关系。
 *
 * 如果isRedo为真，关系已经存在是可以的。
 */
void
mdcreate(SMgrRelation reln, ForkNumber forknum, bool isRedo)
{
	MdfdVec    *mdfd;
	char	   *path;
	File		fd;

	if (isRedo && reln->md_num_open_segs[forknum] > 0)
		return;					/* created and opened already... */

	Assert(reln->md_num_open_segs[forknum] == 0);

	/*
	 * We may be using the target table space for the first time in this
	 * database, so create a per-database subdirectory if needed.
	 *
	 * XXX this is a fairly ugly violation of module layering, but this seems
	 * to be the best place to put the check.  Maybe TablespaceCreateDbspace
	 * should be here and not in commands/tablespace.c?  But that would imply
	 * importing a lot of stuff that smgr.c oughtn't know, either.
	 */
/*
 * 我们可能首次使用目标表空间
 * 数据库，因此如果需要，可以创建一个每个数据库的子目录。
 *
 * XXX 这确实是模块分层的一个相当严重的违规，但这似乎
 * 成为结账的最佳地点。 也许是TablespaceCreateDbspace
 * 应该在这里，而不是在 commands/tablespace.c 里？ 但那意味着
 * 导入了很多SMGR.C也不该知道的东西。
 */
	TablespaceCreateDbspace(reln->smgr_rlocator.locator.spcOid,
							reln->smgr_rlocator.locator.dbOid,
							isRedo);

	path = relpath(reln->smgr_rlocator, forknum);

	fd = PathNameOpenFile(path, _mdfd_open_flags() | O_CREAT | O_EXCL);

	if (fd < 0)
	{
		int			save_errno = errno;

		if (isRedo)
			fd = PathNameOpenFile(path, _mdfd_open_flags());
		if (fd < 0)
		{
			/* be sure to report the error reported by create, not open */
			errno = save_errno;
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not create file \"%s\": %m", path)));
		}
	}

	pfree(path);

	_fdvec_resize(reln, forknum, 1);
	mdfd = &reln->md_seg_fds[forknum][0];
	mdfd->mdfd_vfd = fd;
	mdfd->mdfd_segno = 0;

	if (!SmgrIsTemp(reln))
		register_dirty_segment(reln, forknum, mdfd);
}

/*
 * mdunlink() -- Unlink a relation.
 *
 * Note that we're passed a RelFileLocatorBackend --- by the time this is called,
 * there won't be an SMgrRelation hashtable entry anymore.
 *
 * forknum can be a fork number to delete a specific fork, or InvalidForkNumber
 * to delete all forks.
 *
 * For regular relations, we don't unlink the first segment file of the rel,
 * but just truncate it to zero length, and record a request to unlink it after
 * the next checkpoint.  Additional segments can be unlinked immediately,
 * however.  Leaving the empty file in place prevents that relfilenumber
 * from being reused.  The scenario this protects us from is:
 * 1. We delete a relation (and commit, and actually remove its file).
 * 2. We create a new relation, which by chance gets the same relfilenumber as
 *	  the just-deleted one (OIDs must've wrapped around for that to happen).
 * 3. We crash before another checkpoint occurs.
 * During replay, we would delete the file and then recreate it, which is fine
 * if the contents of the file were repopulated by subsequent WAL entries.
 * But if we didn't WAL-log insertions, but instead relied on fsyncing the
 * file after populating it (as we do at wal_level=minimal), the contents of
 * the file would be lost forever.  By leaving the empty file until after the
 * next checkpoint, we prevent reassignment of the relfilenumber until it's
 * safe, because relfilenumber assignment skips over any existing file.
 *
 * Additional segments, if any, are truncated and then unlinked.  The reason
 * for truncating is that other backends may still hold open FDs for these at
 * the smgr level, so that the kernel can't remove the file yet.  We want to
 * reclaim the disk space right away despite that.
 *
 * We do not need to go through this dance for temp relations, though, because
 * we never make WAL entries for temp rels, and so a temp rel poses no threat
 * to the health of a regular rel that has taken over its relfilenumber.
 * The fact that temp rels and regular rels have different file naming
 * patterns provides additional safety.  Other backends shouldn't have open
 * FDs for them, either.
 *
 * We also don't do it while performing a binary upgrade.  There is no reuse
 * hazard in that case, since after a crash or even a simple ERROR, the
 * upgrade fails and the whole cluster must be recreated from scratch.
 * Furthermore, it is important to remove the files from disk immediately,
 * because we may be about to reuse the same relfilenumber.
 *
 * All the above applies only to the relation's main fork; other forks can
 * just be removed immediately, since they are not needed to prevent the
 * relfilenumber from being recycled.  Also, we do not carefully
 * track whether other forks have been created or not, but just attempt to
 * unlink them unconditionally; so we should never complain about ENOENT.
 *
 * If isRedo is true, it's unsurprising for the relation to be already gone.
 * Also, we should remove the file immediately instead of queuing a request
 * for later, since during redo there's no possibility of creating a
 * conflicting relation.
 *
 * Note: we currently just never warn about ENOENT at all.  We could warn in
 * the main-fork, non-isRedo case, but it doesn't seem worth the trouble.
 *
 * Note: any failure should be reported as WARNING not ERROR, because
 * we are usually not in a transaction anymore when this is called.
 */
/*
 * mdunlink（） —— 取消关联链接。
 *
 * 注意，当调用时，我们已经收到了一个RelFileLocatorBackend ---，
 * 将不再有SMgrRelation的哈希表条目。
 *
 * forknum 可以是用于删除特定分支的分支编号，或 InvalidForkNumber
 * 删除所有分支。
 *
 * 对于正则关系，我们不解绑关系的第一段文件，
 * 但只需截断为零长度，然后录制取消链接的请求
 * 下一个检查点。 额外的段可以立即解除关联，
 * 然而。 保持空文件的位置会阻止该 relfilenumber
 * 避免被重复使用。 这保护我们免受的情景是：
 * 1.我们删除一个关系（提交，实际上移除其文件）。
 * 2.我们创建一个新关系，偶然地获得了相同的relfilenumber，与
 * 刚删除的那个（必须是OID包围才会发生这种情况）。
 * 3.我们在下一个检查点出现前坠毁了。
 * 回放时，我们会删除文件再重新创建，这没问题
 * 如果文件内容被后续的WAL条目重新填充。
 * 但如果我们不进行 WAL 日志插入，而是依赖 fsync
 * 文件在填充后（如我们在 wal_level=最小值时所做的那样），包含
 * 文件将永远丢失。 通过将空文件留在
 * 下一个检查点，我们阻止重新分配文件号，直到它
 * 安全，因为relfilenumber赋值会跳过任何已有的文件。
 *
 * 如果有额外的段，会被截断然后取消链接。 原因
 * 截断是指其他后端可能仍可为这些在
 * SMGR 级别，这样内核还无法移除该文件。 我们想
 * 尽管如此，还是要立刻收回磁盘空间。
 *
 * 不过我们不需要为了临时关系而经历这场舞蹈，因为
 * 我们从不为临时关系做WAL条目，因此临时关系没有威胁
 * 对已接管其 relfile number的常规 rel 的健康状态。
 * 临时 rels 和普通 rels 文件命名不同
 * 图案提供了额外的安全保障。 其他后端不应该是开放的
 * 也给他们定期存款。
 *
 * 我们也不会在进行二进制升级时这样做。 没有重复使用
 * 在这种情况下，因为在发生事故甚至简单的错误后，
 * 升级失败，整个集群必须从头重建。
 * 此外，必须立即从磁盘中删除文件，
 * 因为我们可能要重复使用同一个文件号。
 *
 * 上述所有内容仅适用于关系的主叉;其他分支则可以
 * 立即移除，因为他们不需要来防止
 * 回收编号。 此外，我们也不会小心
 * 跟踪是否创建了其他分支，但只需尝试
 * 无条件解除关联;所以我们永远不应该抱怨ENOENT。
 *
 * 如果isRedo为真，关系已经消失也就不足为奇了。
 * 另外，我们应该立即删除文件，而不是排队请求
 * 以后用，因为重做时无法创建
 * 矛盾关系。
 *
 * 注：我们目前根本不对ENOENT发出警告。 我们可以警告
 * 主分支，非isRedo案例，但似乎不值得麻烦。
 *
 * 注意：任何失败都应报告为“警告”而非“错误”，因为
 * 通常我们不再处于交易状态时，这个请求会被叫出。
 */
void
mdunlink(RelFileLocatorBackend rlocator, ForkNumber forknum, bool isRedo)
{
	/* Now do the per-fork work */
	if (forknum == InvalidForkNumber)
	{
		for (forknum = 0; forknum <= MAX_FORKNUM; forknum++)
			mdunlinkfork(rlocator, forknum, isRedo);
	}
	else
		mdunlinkfork(rlocator, forknum, isRedo);
}

/*
 * Truncate a file to release disk space.
 */
static int
do_truncate(const char *path)
{
	int			save_errno;
	int			ret;

	ret = pg_truncate(path, 0);

	/* Log a warning here to avoid repetition in callers. */
	if (ret < 0 && errno != ENOENT)
	{
		save_errno = errno;
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not truncate file \"%s\": %m", path)));
		errno = save_errno;
	}

	return ret;
}

static void
mdunlinkfork(RelFileLocatorBackend rlocator, ForkNumber forknum, bool isRedo)
{
	char	   *path;
	int			ret;
	int			save_errno;

	path = relpath(rlocator, forknum);

	/*
	 * Truncate and then unlink the first segment, or just register a request
	 * to unlink it later, as described in the comments for mdunlink().
	 */
	if (isRedo || IsBinaryUpgrade || forknum != MAIN_FORKNUM ||
		RelFileLocatorBackendIsTemp(rlocator))
	{
		if (!RelFileLocatorBackendIsTemp(rlocator))
		{
			/* Prevent other backends' fds from holding on to the disk space */
			ret = do_truncate(path);

			/* Forget any pending sync requests for the first segment */
			save_errno = errno;
			register_forget_request(rlocator, forknum, 0 /* first seg */ );
			errno = save_errno;
		}
		else
			ret = 0;

		/* Next unlink the file, unless it was already found to be missing */
		if (ret >= 0 || errno != ENOENT)
		{
			ret = unlink(path);
			if (ret < 0 && errno != ENOENT)
			{
				save_errno = errno;
				ereport(WARNING,
						(errcode_for_file_access(),
						 errmsg("could not remove file \"%s\": %m", path)));
				errno = save_errno;
			}
		}
	}
	else
	{
		/* Prevent other backends' fds from holding on to the disk space */
		ret = do_truncate(path);

		/* Register request to unlink first segment later */
		save_errno = errno;
		register_unlink_segment(rlocator, forknum, 0 /* first seg */ );
		errno = save_errno;
	}

	/*
	 * Delete any additional segments.
	 *
	 * Note that because we loop until getting ENOENT, we will correctly
	 * remove all inactive segments as well as active ones.  Ideally we'd
	 * continue the loop until getting exactly that errno, but that risks an
	 * infinite loop if the problem is directory-wide (for instance, if we
	 * suddenly can't read the data directory itself).  We compromise by
	 * continuing after a non-ENOENT truncate error, but stopping after any
	 * unlink error.  If there is indeed a directory-wide problem, additional
	 * unlink attempts wouldn't work anyway.
	 */
	if (ret >= 0 || errno != ENOENT)
	{
		char	   *segpath = (char *) palloc(strlen(path) + 12);
		BlockNumber segno;

		for (segno = 1;; segno++)
		{
			sprintf(segpath, "%s.%u", path, segno);

			if (!RelFileLocatorBackendIsTemp(rlocator))
			{
				/*
				 * Prevent other backends' fds from holding on to the disk
				 * space.  We're done if we see ENOENT, though.
				 */
				if (do_truncate(segpath) < 0 && errno == ENOENT)
					break;

				/*
				 * Forget any pending sync requests for this segment before we
				 * try to unlink.
				 */
				register_forget_request(rlocator, forknum, segno);
			}

			if (unlink(segpath) < 0)
			{
				/* ENOENT is expected after the last segment... */
				if (errno != ENOENT)
					ereport(WARNING,
							(errcode_for_file_access(),
							 errmsg("could not remove file \"%s\": %m", segpath)));
				break;
			}
		}
		pfree(segpath);
	}

	pfree(path);
}

/*
 * mdextend() -- Add a block to the specified relation.
 *
 * The semantics are nearly the same as mdwrite(): write at the
 * specified position.  However, this is to be used for the case of
 * extending a relation (i.e., blocknum is at or beyond the current
 * EOF).  Note that we assume writing a block beyond current EOF
 * causes intervening file space to become filled with zeroes.
 */
/*
 * mdextend（） -- 向指定的关系添加一个块。
 *
 * 语义几乎与 mdwrite（）： write at the
 * 指定位置。 然而，这适用于
 * 扩展关系（即块数位于或超过当前电流）
 * EOF）。 注意，我们假设写入一个超出当前EOF的块
 * 导致中间的文件空间被填满了零。
 */
void
mdextend(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		 const void *buffer, bool skipFsync)
{
	off_t		seekpos;
	int			nbytes;
	MdfdVec    *v;

	/* If this build supports direct I/O, the buffer must be I/O aligned. */
	if (PG_O_DIRECT != 0 && PG_IO_ALIGN_SIZE <= BLCKSZ)
		Assert((uintptr_t) buffer == TYPEALIGN(PG_IO_ALIGN_SIZE, buffer));

	/* This assert is too expensive to have on normally ... */
#ifdef CHECK_WRITE_VS_EXTEND
	Assert(blocknum >= mdnblocks(reln, forknum));
#endif

	/*
	 * If a relation manages to grow to 2^32-1 blocks, refuse to extend it any
	 * more --- we mustn't create a block whose number actually is
	 * InvalidBlockNumber.  (Note that this failure should be unreachable
	 * because of upstream checks in bufmgr.c.)
	 */
/*
 * 如果关系扩展到2^32-1块，拒绝扩展任何
 * 更重要---我们不能创建编号实际上为
 * 无效块编号。 （注意，这个失败应该是不可达的
 * 因为在bufmgr.c的上游检查。）
 */
	if (blocknum == InvalidBlockNumber)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("cannot extend file \"%s\" beyond %u blocks",
						relpath(reln->smgr_rlocator, forknum),
						InvalidBlockNumber)));

	v = _mdfd_getseg(reln, forknum, blocknum, skipFsync, EXTENSION_CREATE);

	seekpos = (off_t) BLCKSZ * (blocknum % ((BlockNumber) RELSEG_SIZE));

	Assert(seekpos < (off_t) BLCKSZ * RELSEG_SIZE);

	if ((nbytes = FileWrite(v->mdfd_vfd, buffer, BLCKSZ, seekpos, WAIT_EVENT_DATA_FILE_EXTEND)) != BLCKSZ)
	{
		if (nbytes < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not extend file \"%s\": %m",
							FilePathName(v->mdfd_vfd)),
					 errhint("Check free disk space.")));
		/* short write: complain appropriately */
		ereport(ERROR,
				(errcode(ERRCODE_DISK_FULL),
				 errmsg("could not extend file \"%s\": wrote only %d of %d bytes at block %u",
						FilePathName(v->mdfd_vfd),
						nbytes, BLCKSZ, blocknum),
				 errhint("Check free disk space.")));
	}

	if (!skipFsync && !SmgrIsTemp(reln))
		register_dirty_segment(reln, forknum, v);

	Assert(_mdnblocks(reln, forknum, v) <= ((BlockNumber) RELSEG_SIZE));
}

/*
 * mdzeroextend() -- Add new zeroed out blocks to the specified relation.
 *
 * Similar to mdextend(), except the relation can be extended by multiple
 * blocks at once and the added blocks will be filled with zeroes.
 */
/*
 * mdzeroextend（） -- 向指定的关系添加新的归零区块。
 *
 * 类似于 mdextend（），但该关系可以被扩展为多倍
 * 块同时填入，新增的块将被填入零。
 */
void
mdzeroextend(SMgrRelation reln, ForkNumber forknum,
			 BlockNumber blocknum, int nblocks, bool skipFsync)
{
	MdfdVec    *v;
	BlockNumber curblocknum = blocknum;
	int			remblocks = nblocks;

	Assert(nblocks > 0);

	/* This assert is too expensive to have on normally ... */
#ifdef CHECK_WRITE_VS_EXTEND
	Assert(blocknum >= mdnblocks(reln, forknum));
#endif

	/*
	 * If a relation manages to grow to 2^32-1 blocks, refuse to extend it any
	 * more --- we mustn't create a block whose number actually is
	 * InvalidBlockNumber or larger.
	 */
	if ((uint64) blocknum + nblocks >= (uint64) InvalidBlockNumber)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("cannot extend file \"%s\" beyond %u blocks",
						relpath(reln->smgr_rlocator, forknum),
						InvalidBlockNumber)));

	while (remblocks > 0)
	{
		BlockNumber segstartblock = curblocknum % ((BlockNumber) RELSEG_SIZE);
		off_t		seekpos = (off_t) BLCKSZ * segstartblock;
		int			numblocks;

		if (segstartblock + remblocks > RELSEG_SIZE)
			numblocks = RELSEG_SIZE - segstartblock;
		else
			numblocks = remblocks;

		v = _mdfd_getseg(reln, forknum, curblocknum, skipFsync, EXTENSION_CREATE);

		Assert(segstartblock < RELSEG_SIZE);
		Assert(segstartblock + numblocks <= RELSEG_SIZE);

		/*
		 * If available and useful, use posix_fallocate() (via
		 * FileFallocate()) to extend the relation. That's often more
		 * efficient than using write(), as it commonly won't cause the kernel
		 * to allocate page cache space for the extended pages.
		 *
		 * However, we don't use FileFallocate() for small extensions, as it
		 * defeats delayed allocation on some filesystems. Not clear where
		 * that decision should be made though? For now just use a cutoff of
		 * 8, anything between 4 and 8 worked OK in some local testing.
		 */
/*
 * 如有且有用，请使用 posix_fallocate（）（通过）
 * FileFallocate（）） 以扩展关系。这通常更多
 * 比使用 write（） 更高效，因为通常不会导致内核
 * 用于为扩展页面分配页面缓存空间。
 *
 * 不过，我们不使用 FileFallocate（） 来处理小型扩展，因为
 * 在某些文件系统上击败延迟分配。具体位置不明
 * 这个决定应该做吗？目前只需使用以下截断
 * 8,4到8之间的任何在局部测试中都正常。
 */
		if (numblocks > 8)
		{
			int			ret;

			ret = FileFallocate(v->mdfd_vfd,
								seekpos, (off_t) BLCKSZ * numblocks,
								WAIT_EVENT_DATA_FILE_EXTEND);
			if (ret != 0)
			{
				ereport(ERROR,
						errcode_for_file_access(),
						errmsg("could not extend file \"%s\" with FileFallocate(): %m",
							   FilePathName(v->mdfd_vfd)),
						errhint("Check free disk space."));
			}
		}
		else
		{
			int			ret;

			/*
			 * Even if we don't want to use fallocate, we can still extend a
			 * bit more efficiently than writing each 8kB block individually.
			 * pg_pwrite_zeros() (via FileZero()) uses pg_pwritev_with_retry()
			 * to avoid multiple writes or needing a zeroed buffer for the
			 * whole length of the extension.
			 */
/*
 * 即使我们不想用 Fallocate，我们仍然可以扩展
 * 比单独写入每个8kB块更高效。
 * pg_pwrite_zeros（）（通过FileZero（））使用pg_pwritev_with_retry（）
 * 以避免多次写入或需要归零缓冲区
 * 整个延伸线长度。
 */
			ret = FileZero(v->mdfd_vfd,
						   seekpos, (off_t) BLCKSZ * numblocks,
						   WAIT_EVENT_DATA_FILE_EXTEND);
			if (ret < 0)
				ereport(ERROR,
						errcode_for_file_access(),
						errmsg("could not extend file \"%s\": %m",
							   FilePathName(v->mdfd_vfd)),
						errhint("Check free disk space."));
		}

		if (!skipFsync && !SmgrIsTemp(reln))
			register_dirty_segment(reln, forknum, v);

		Assert(_mdnblocks(reln, forknum, v) <= ((BlockNumber) RELSEG_SIZE));

		remblocks -= numblocks;
		curblocknum += numblocks;
	}
}

/*
 * mdopenfork() -- Open one fork of the specified relation.
 *
 * Note we only open the first segment, when there are multiple segments.
 *
 * If first segment is not present, either ereport or return NULL according
 * to "behavior".  We treat EXTENSION_CREATE the same as EXTENSION_FAIL;
 * EXTENSION_CREATE means it's OK to extend an existing relation, not to
 * invent one out of whole cloth.
 */
/*
 * mdopenfork（） —— 开启指定关系的一个分支。
 *
 * 注意，当有多个段落时，我们只打开第一个片段。
 *
 * 如果第一个段不存在，则根据电子报告或返回NULL
 * 对“行为”。 我们把EXTENSION_CREATE和EXTENSION_FAIL一样对待;
 * EXTENSION_CREATE 表示可以扩展现有关系，而不是
 * 凭空发明一个。
 */
static MdfdVec *
mdopenfork(SMgrRelation reln, ForkNumber forknum, int behavior)
{
	MdfdVec    *mdfd;
	char	   *path;
	File		fd;

	/* No work if already open */
	if (reln->md_num_open_segs[forknum] > 0)
		return &reln->md_seg_fds[forknum][0];

	path = relpath(reln->smgr_rlocator, forknum);

	fd = PathNameOpenFile(path, _mdfd_open_flags());

	if (fd < 0)
	{
		if ((behavior & EXTENSION_RETURN_NULL) &&
			FILE_POSSIBLY_DELETED(errno))
		{
			pfree(path);
			return NULL;
		}
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));
	}

	pfree(path);

	_fdvec_resize(reln, forknum, 1);
	mdfd = &reln->md_seg_fds[forknum][0];
	mdfd->mdfd_vfd = fd;
	mdfd->mdfd_segno = 0;

	Assert(_mdnblocks(reln, forknum, mdfd) <= ((BlockNumber) RELSEG_SIZE));

	return mdfd;
}

/*
 * mdopen() -- Initialize newly-opened relation.
 */
void
mdopen(SMgrRelation reln)
{
	/* mark it not open */
	for (int forknum = 0; forknum <= MAX_FORKNUM; forknum++)
		reln->md_num_open_segs[forknum] = 0;
}

/*
 * mdclose() -- Close the specified relation, if it isn't closed already.
 */
void
mdclose(SMgrRelation reln, ForkNumber forknum)
{
	int			nopensegs = reln->md_num_open_segs[forknum];

	/* No work if already closed */
	if (nopensegs == 0)
		return;

	/* close segments starting from the end */
	while (nopensegs > 0)
	{
		MdfdVec    *v = &reln->md_seg_fds[forknum][nopensegs - 1];

		FileClose(v->mdfd_vfd);
		_fdvec_resize(reln, forknum, nopensegs - 1);
		nopensegs--;
	}
}

/*
 * mdprefetch() -- Initiate asynchronous read of the specified block of a relation
 */
/*
 * mdprefetch（） —— 发起对关系中指定块的异步读取
 */
bool
mdprefetch(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum)
{
#ifdef USE_PREFETCH
	off_t		seekpos;
	MdfdVec    *v;

	Assert((io_direct_flags & IO_DIRECT_DATA) == 0);

	v = _mdfd_getseg(reln, forknum, blocknum, false,
					 InRecovery ? EXTENSION_RETURN_NULL : EXTENSION_FAIL);
	if (v == NULL)
		return false;

	seekpos = (off_t) BLCKSZ * (blocknum % ((BlockNumber) RELSEG_SIZE));

	Assert(seekpos < (off_t) BLCKSZ * RELSEG_SIZE);

	(void) FilePrefetch(v->mdfd_vfd, seekpos, BLCKSZ, WAIT_EVENT_DATA_FILE_PREFETCH);
#endif							/* USE_PREFETCH */

	return true;
}

/*
 * mdread() -- Read the specified block from a relation.
 */
void
mdread(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
	   void *buffer)
{
	off_t		seekpos;
	int			nbytes;
	MdfdVec    *v;

	/* If this build supports direct I/O, the buffer must be I/O aligned. */
	if (PG_O_DIRECT != 0 && PG_IO_ALIGN_SIZE <= BLCKSZ)
		Assert((uintptr_t) buffer == TYPEALIGN(PG_IO_ALIGN_SIZE, buffer));

	TRACE_POSTGRESQL_SMGR_MD_READ_START(forknum, blocknum,
										reln->smgr_rlocator.locator.spcOid,
										reln->smgr_rlocator.locator.dbOid,
										reln->smgr_rlocator.locator.relNumber,
										reln->smgr_rlocator.backend);

	v = _mdfd_getseg(reln, forknum, blocknum, false,
					 EXTENSION_FAIL | EXTENSION_CREATE_RECOVERY);

	seekpos = (off_t) BLCKSZ * (blocknum % ((BlockNumber) RELSEG_SIZE));

	Assert(seekpos < (off_t) BLCKSZ * RELSEG_SIZE);

	nbytes = FileRead(v->mdfd_vfd, buffer, BLCKSZ, seekpos, WAIT_EVENT_DATA_FILE_READ);

	TRACE_POSTGRESQL_SMGR_MD_READ_DONE(forknum, blocknum,
									   reln->smgr_rlocator.locator.spcOid,
									   reln->smgr_rlocator.locator.dbOid,
									   reln->smgr_rlocator.locator.relNumber,
									   reln->smgr_rlocator.backend,
									   nbytes,
									   BLCKSZ);

	if (nbytes != BLCKSZ)
	{
		if (nbytes < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read block %u in file \"%s\": %m",
							blocknum, FilePathName(v->mdfd_vfd))));

		/*
		 * Short read: we are at or past EOF, or we read a partial block at
		 * EOF.  Normally this is an error; upper levels should never try to
		 * read a nonexistent block.  However, if zero_damaged_pages is ON or
		 * we are InRecovery, we should instead return zeroes without
		 * complaining.  This allows, for example, the case of trying to
		 * update a block that was later truncated away.
		 */
/*
 * 简短读数：我们已到达或已过EOF，或读到部分区块
 * EOF。 通常这是错误;上层绝不应该尝试
 * 读取一个不存在的块。 然而，如果zero_damaged_pages 开启 或
 * 我们是InRecovery，我们应该返回零，但没有
 * 抱怨。 这允许例如尝试
 * 更新后来被截断的块。
 */
		if (zero_damaged_pages || InRecovery)
			MemSet(buffer, 0, BLCKSZ);
		else
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("could not read block %u in file \"%s\": read only %d of %d bytes",
							blocknum, FilePathName(v->mdfd_vfd),
							nbytes, BLCKSZ)));
	}
}

/*
 * mdwrite() -- Write the supplied block at the appropriate location.
 *
 * This is to be used only for updating already-existing blocks of a
 * relation (ie, those before the current EOF).  To extend a relation,
 * use mdextend().
 */
/*
 * mdwrite() -- Write the supplied block at the appropriate location.
 *
 * This is to be used only for updating already-existing blocks of a
 * relation (ie, those before the current EOF).  To extend a relation,
 * use mdextend().
 */
void
mdwrite(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		const void *buffer, bool skipFsync)
{
	off_t		seekpos;
	int			nbytes;
	MdfdVec    *v;

	/* If this build supports direct I/O, the buffer must be I/O aligned. */
	if (PG_O_DIRECT != 0 && PG_IO_ALIGN_SIZE <= BLCKSZ)
		Assert((uintptr_t) buffer == TYPEALIGN(PG_IO_ALIGN_SIZE, buffer));

	/* This assert is too expensive to have on normally ... */
#ifdef CHECK_WRITE_VS_EXTEND
	Assert(blocknum < mdnblocks(reln, forknum));
#endif

	TRACE_POSTGRESQL_SMGR_MD_WRITE_START(forknum, blocknum,
										 reln->smgr_rlocator.locator.spcOid,
										 reln->smgr_rlocator.locator.dbOid,
										 reln->smgr_rlocator.locator.relNumber,
										 reln->smgr_rlocator.backend);

	v = _mdfd_getseg(reln, forknum, blocknum, skipFsync,
					 EXTENSION_FAIL | EXTENSION_CREATE_RECOVERY);

	seekpos = (off_t) BLCKSZ * (blocknum % ((BlockNumber) RELSEG_SIZE));

	Assert(seekpos < (off_t) BLCKSZ * RELSEG_SIZE);

	nbytes = FileWrite(v->mdfd_vfd, buffer, BLCKSZ, seekpos, WAIT_EVENT_DATA_FILE_WRITE);

	TRACE_POSTGRESQL_SMGR_MD_WRITE_DONE(forknum, blocknum,
										reln->smgr_rlocator.locator.spcOid,
										reln->smgr_rlocator.locator.dbOid,
										reln->smgr_rlocator.locator.relNumber,
										reln->smgr_rlocator.backend,
										nbytes,
										BLCKSZ);

	if (nbytes != BLCKSZ)
	{
		if (nbytes < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write block %u in file \"%s\": %m",
							blocknum, FilePathName(v->mdfd_vfd))));
		/* short write: complain appropriately */
		ereport(ERROR,
				(errcode(ERRCODE_DISK_FULL),
				 errmsg("could not write block %u in file \"%s\": wrote only %d of %d bytes",
						blocknum,
						FilePathName(v->mdfd_vfd),
						nbytes, BLCKSZ),
				 errhint("Check free disk space.")));
	}

	if (!skipFsync && !SmgrIsTemp(reln))
		register_dirty_segment(reln, forknum, v);
}

/*
 * mdwriteback() -- Tell the kernel to write pages back to storage.
 *
 * This accepts a range of blocks because flushing several pages at once is
 * considerably more efficient than doing so individually.
 */
/*
 * mdwriteback（） -- 告诉内核将页面写回存储。
 *
 * 此方法接受多个块，因为一次冲洗多个页面是
 * 比单独操作效率高得多。
 */
void
mdwriteback(SMgrRelation reln, ForkNumber forknum,
			BlockNumber blocknum, BlockNumber nblocks)
{
	Assert((io_direct_flags & IO_DIRECT_DATA) == 0);

	/*
	 * Issue flush requests in as few requests as possible; have to split at
	 * segment boundaries though, since those are actually separate files.
	 */
/*
 * 尽量少发冲洗请求;必须分开
 * 段边界，因为那些实际上是独立文件。
 */
	while (nblocks > 0)
	{
		BlockNumber nflush = nblocks;
		off_t		seekpos;
		MdfdVec    *v;
		int			segnum_start,
					segnum_end;

		v = _mdfd_getseg(reln, forknum, blocknum, true /* not used */ ,
						 EXTENSION_DONT_OPEN);

		/*
		 * We might be flushing buffers of already removed relations, that's
		 * ok, just ignore that case.  If the segment file wasn't open already
		 * (ie from a recent mdwrite()), then we don't want to re-open it, to
		 * avoid a race with PROCSIGNAL_BARRIER_SMGRRELEASE that might leave
		 * us with a descriptor to a file that is about to be unlinked.
		 */
/*
 * 我们可能正在清除已经断绝关系的缓冲，那是
 * 好的，别理那个箱子。 如果段文件还没打开
 * （即来自最近的 mdwrite（）），那么我们不想重新打开它，去
 * 避免与可能离开的PROCSIGNAL_BARRIER_SMGRRELEASE比赛
 * 我们，并用描述符指向即将被解除绑定的文件。
 */
		if (!v)
			return;

		/* compute offset inside the current segment */
		segnum_start = blocknum / RELSEG_SIZE;

		/* compute number of desired writes within the current segment */
		segnum_end = (blocknum + nblocks - 1) / RELSEG_SIZE;
		if (segnum_start != segnum_end)
			nflush = RELSEG_SIZE - (blocknum % ((BlockNumber) RELSEG_SIZE));

		Assert(nflush >= 1);
		Assert(nflush <= nblocks);

		seekpos = (off_t) BLCKSZ * (blocknum % ((BlockNumber) RELSEG_SIZE));

		FileWriteback(v->mdfd_vfd, seekpos, (off_t) BLCKSZ * nflush, WAIT_EVENT_DATA_FILE_FLUSH);

		nblocks -= nflush;
		blocknum += nflush;
	}
}

/*
 * mdnblocks() -- Get the number of blocks stored in a relation.
 *
 * Important side effect: all active segments of the relation are opened
 * and added to the md_seg_fds array.  If this routine has not been
 * called, then only segments up to the last one actually touched
 * are present in the array.
 */
/*
 * mdnblocks（） -- 获取存储在关系中的块数。
 *
 * 重要副作用：关系中所有活跃片段均被打开
 * 并加入md_seg_fds阵列。 如果这个习惯还没被
 * 被叫喊，然后只有最后一个实际触碰的片段
 * 出现在阵列中。
 */
BlockNumber
mdnblocks(SMgrRelation reln, ForkNumber forknum)
{
	MdfdVec    *v;
	BlockNumber nblocks;
	BlockNumber segno;

	mdopenfork(reln, forknum, EXTENSION_FAIL);

	/* mdopen has opened the first segment */
	Assert(reln->md_num_open_segs[forknum] > 0);

	/*
	 * Start from the last open segments, to avoid redundant seeks.  We have
	 * previously verified that these segments are exactly RELSEG_SIZE long,
	 * and it's useless to recheck that each time.
	 *
	 * NOTE: this assumption could only be wrong if another backend has
	 * truncated the relation.  We rely on higher code levels to handle that
	 * scenario by closing and re-opening the md fd, which is handled via
	 * relcache flush.  (Since the checkpointer doesn't participate in
	 * relcache flush, it could have segment entries for inactive segments;
	 * that's OK because the checkpointer never needs to compute relation
	 * size.)
	 */
/*
 * 从最后开放的片段开始，以避免重复寻址。 我们有
 * 先前已确认这些段长度正好RELSEG_SIZE，
 * 而且每次都没必要再检查一次。
 *
 * 注意：只有当另一个后端
 * 截断了关系。 我们依赖更高级别的代码来处理这些
 * 场景通过关闭和重新打开MD FD，处理方式为
 * Relcache 同花。 （因为检查点不参与
 * relcache flush，它可以包含非活跃段的段项;
 * 这没关系，因为检查点从不需要计算关系
 * 尺寸。）
 */
	segno = reln->md_num_open_segs[forknum] - 1;
	v = &reln->md_seg_fds[forknum][segno];

	for (;;)
	{
		nblocks = _mdnblocks(reln, forknum, v);
		if (nblocks > ((BlockNumber) RELSEG_SIZE))
			elog(FATAL, "segment too big");
		if (nblocks < ((BlockNumber) RELSEG_SIZE))
			return (segno * ((BlockNumber) RELSEG_SIZE)) + nblocks;

		/*
		 * If segment is exactly RELSEG_SIZE, advance to next one.
		 */
/*
 * 如果该片段正好RELSEG_SIZE，则进入下一个。
 */
		segno++;

		/*
		 * We used to pass O_CREAT here, but that has the disadvantage that it
		 * might create a segment which has vanished through some operating
		 * system misadventure.  In such a case, creating the segment here
		 * undermines _mdfd_getseg's attempts to notice and report an error
		 * upon access to a missing segment.
		 */
/*
	 * 我们以前会O_CREAT过这里，但那有个缺点
	 * 可能创建一个因某些操作而消失的段
	 * 系统失误。 在这种情况下，创建
	 * 破坏_mdfd_getseg试图发现和报告错误的努力
	 * 在访问缺失片段时。
	 */
		v = _mdfd_openseg(reln, forknum, segno, 0);
		if (v == NULL)
			return segno * ((BlockNumber) RELSEG_SIZE);
	}
}

/*
 * mdtruncate() -- Truncate relation to specified number of blocks.
 *
 * Guaranteed not to allocate memory, so it can be used in a critical section.
 * Caller must have called smgrnblocks() to obtain curnblk while holding a
 * sufficient lock to prevent a change in relation size, and not used any smgr
 * functions for this relation or handled interrupts in between.  This makes
 * sure we have opened all active segments, so that truncate loop will get
 * them all!
 */
/*
 * mdtruncate（） —— 截断与指定块数的关系。
 *
 * 保证不分配内存，因此可在关键区段使用。
 * 来电者必须在持有 a 时呼叫 smgrnblocks（） 才能获得 curnblk
 * 足够的锁以防止相对大小变化，且未使用任何SMGR
 * 该关系的函数或中间处理的中断。 这让
 * 当然我们已经打开了所有活跃段，所以截断环路会得到
 * 他们全部！
 */
void
mdtruncate(SMgrRelation reln, ForkNumber forknum,
		   BlockNumber curnblk, BlockNumber nblocks)
{
	BlockNumber priorblocks;
	int			curopensegs;

	if (nblocks > curnblk)
	{
		/* Bogus request ... but no complaint if InRecovery */
		if (InRecovery)
			return;
		ereport(ERROR,
				(errmsg("could not truncate file \"%s\" to %u blocks: it's only %u blocks now",
						relpath(reln->smgr_rlocator, forknum),
						nblocks, curnblk)));
	}
	if (nblocks == curnblk)
		return;					/* no work */

	/*
	 * Truncate segments, starting at the last one. Starting at the end makes
	 * managing the memory for the fd array easier, should there be errors.
	 */
	curopensegs = reln->md_num_open_segs[forknum];
	while (curopensegs > 0)
	{
		MdfdVec    *v;

		priorblocks = (curopensegs - 1) * RELSEG_SIZE;

		v = &reln->md_seg_fds[forknum][curopensegs - 1];

		if (priorblocks > nblocks)
		{
			/*
			 * This segment is no longer active. We truncate the file, but do
			 * not delete it, for reasons explained in the header comments.
			 */
			if (FileTruncate(v->mdfd_vfd, 0, WAIT_EVENT_DATA_FILE_TRUNCATE) < 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not truncate file \"%s\": %m",
								FilePathName(v->mdfd_vfd))));

			if (!SmgrIsTemp(reln))
				register_dirty_segment(reln, forknum, v);

			/* we never drop the 1st segment */
			Assert(v != &reln->md_seg_fds[forknum][0]);

			FileClose(v->mdfd_vfd);
			_fdvec_resize(reln, forknum, curopensegs - 1);
		}
		else if (priorblocks + ((BlockNumber) RELSEG_SIZE) > nblocks)
		{
			/*
			 * This is the last segment we want to keep. Truncate the file to
			 * the right length. NOTE: if nblocks is exactly a multiple K of
			 * RELSEG_SIZE, we will truncate the K+1st segment to 0 length but
			 * keep it. This adheres to the invariant given in the header
			 * comments.
			 */
			BlockNumber lastsegblocks = nblocks - priorblocks;

			if (FileTruncate(v->mdfd_vfd, (off_t) lastsegblocks * BLCKSZ, WAIT_EVENT_DATA_FILE_TRUNCATE) < 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not truncate file \"%s\" to %u blocks: %m",
								FilePathName(v->mdfd_vfd),
								nblocks)));
			if (!SmgrIsTemp(reln))
				register_dirty_segment(reln, forknum, v);
		}
		else
		{
			/*
			 * We still need this segment, so nothing to do for this and any
			 * earlier segment.
			 */
			break;
		}
		curopensegs--;
	}
}

/*
 * mdimmedsync() -- Immediately sync a relation to stable storage.
 *
 * Note that only writes already issued are synced; this routine knows
 * nothing of dirty buffers that may exist inside the buffer manager.  We
 * sync active and inactive segments; smgrDoPendingSyncs() relies on this.
 * Consider a relation skipping WAL.  Suppose a checkpoint syncs blocks of
 * some segment, then mdtruncate() renders that segment inactive.  If we
 * crash before the next checkpoint syncs the newly-inactive segment, that
 * segment may survive recovery, reintroducing unwanted data into the table.
 */
/*
 * mdimmedsync（） —— 立即同步与稳定存储的关系。
 *
 * 注意，只有已发布的写入才会同步;这个例行公事知道
 * 不涉及缓冲区管理器中可能存在的脏缓冲区。 我们
 * 同步活跃和非活跃段;smgrDoPendingSyncs（） 依赖于此。
 * 考虑一个跳过 WAL 的关系。 假设一个检查点同步了 的块
 * 某个段，然后 mdtruncate（） 使该段变得非活跃。 如果我们
 * 在下一个检查点同步新停止的段之前崩溃，即
 * 段可能通过恢复存活，重新引入不需要的数据到表中。
 */
void
mdimmedsync(SMgrRelation reln, ForkNumber forknum)
{
	int			segno;
	int			min_inactive_seg;

	/*
	 * NOTE: mdnblocks makes sure we have opened all active segments, so that
	 * fsync loop will get them all!
	 */
/*
 * 注：mdnblocks 确保我们已打开所有活跃段，因此
 * fsync 循环能抓到他们所有人！
 */
	mdnblocks(reln, forknum);

	min_inactive_seg = segno = reln->md_num_open_segs[forknum];

	/*
	 * Temporarily open inactive segments, then close them after sync.  There
	 * may be some inactive segments left opened after fsync() error, but that
	 * is harmless.  We don't bother to clean them up and take a risk of
	 * further trouble.  The next mdclose() will soon close them.
	 */
/*
 * 暂时打开非活跃段，同步后关闭。 在那里
 * 可能是 fsync（） 错误后未激活的部分被打开，但
 * 是无害的。 我们不去清理它们，也不会冒风险
 * 进一步麻烦。 下一个 mdclose（） 很快就会关闭它们。
 */
	while (_mdfd_openseg(reln, forknum, segno, 0) != NULL)
		segno++;

	while (segno > 0)
	{
		MdfdVec    *v = &reln->md_seg_fds[forknum][segno - 1];

		/*
		 * fsyncs done through mdimmedsync() should be tracked in a separate
		 * IOContext than those done through mdsyncfiletag() to differentiate
		 * between unavoidable client backend fsyncs (e.g. those done during
		 * index build) and those which ideally would have been done by the
		 * checkpointer. Since other IO operations bypassing the buffer
		 * manager could also be tracked in such an IOContext, wait until
		 * these are also tracked to track immediate fsyncs.
		 */
/*
 * 通过mdimmedSync（）完成的fsync应单独跟踪
 * IOContext 比通过 mdsyncfiletag（） 进行的 IOContext 进行区分
 * 在不可避免的客户端后端同步之间（例如在期间完成的）
 *索引构建）以及理想情况下由
 * 检查点。由于其他IO操作绕过缓冲区
 * 管理器也可以在这样的IOContext中被追踪，等一下
 * 这些也被追踪以跟踪即时同步。
 */
		if (FileSync(v->mdfd_vfd, WAIT_EVENT_DATA_FILE_IMMEDIATE_SYNC) < 0)
			ereport(data_sync_elevel(ERROR),
					(errcode_for_file_access(),
					 errmsg("could not fsync file \"%s\": %m",
							FilePathName(v->mdfd_vfd))));

		/* Close inactive segments immediately */
		if (segno > min_inactive_seg)
		{
			FileClose(v->mdfd_vfd);
			_fdvec_resize(reln, forknum, segno - 1);
		}

		segno--;
	}
}

/*
 * register_dirty_segment() -- Mark a relation segment as needing fsync
 *
 * If there is a local pending-ops table, just make an entry in it for
 * ProcessSyncRequests to process later.  Otherwise, try to pass off the
 * fsync request to the checkpointer process.  If that fails, just do the
 * fsync locally before returning (we hope this will not happen often
 * enough to be a performance problem).
 */
/*
 * register_dirty_segment（） -- 标记关系段为需要fsync。
 *
 * 如果存在本地待处理操作表，只需在其中输入
 * ProcessSyncRequests 待处理。 否则，试着把
 * fsync 请求到检查点进程。 如果不行，就按
 * 本地fsync后返回（我们希望这种情况不会经常发生
 * 足以造成性能问题）。
 */
static void
register_dirty_segment(SMgrRelation reln, ForkNumber forknum, MdfdVec *seg)
{
	FileTag		tag;

	INIT_MD_FILETAG(tag, reln->smgr_rlocator.locator, forknum, seg->mdfd_segno);

	/* Temp relations should never be fsync'd */
	Assert(!SmgrIsTemp(reln));

	if (!RegisterSyncRequest(&tag, SYNC_REQUEST, false /* retryOnError */ ))
	{
		instr_time	io_start;

		ereport(DEBUG1,
				(errmsg_internal("could not forward fsync request because request queue is full")));

		io_start = pgstat_prepare_io_time();

		if (FileSync(seg->mdfd_vfd, WAIT_EVENT_DATA_FILE_SYNC) < 0)
			ereport(data_sync_elevel(ERROR),
					(errcode_for_file_access(),
					 errmsg("could not fsync file \"%s\": %m",
							FilePathName(seg->mdfd_vfd))));

		/*
		 * We have no way of knowing if the current IOContext is
		 * IOCONTEXT_NORMAL or IOCONTEXT_[BULKREAD, BULKWRITE, VACUUM] at this
		 * point, so count the fsync as being in the IOCONTEXT_NORMAL
		 * IOContext. This is probably okay, because the number of backend
		 * fsyncs doesn't say anything about the efficacy of the
		 * BufferAccessStrategy. And counting both fsyncs done in
		 * IOCONTEXT_NORMAL and IOCONTEXT_[BULKREAD, BULKWRITE, VACUUM] under
		 * IOCONTEXT_NORMAL is likely clearer when investigating the number of
		 * backend fsyncs.
		 */
/*
 * 我们无法确定当前的IOContext是否是
 * IOCONTEXT_NORMAL或IOCONTEXT_[批量阅读、批量写作、吸尘器]
 * 点，所以把fsync算作IOCONTEXT_NORMAL
 * IOContext。这大概没问题，因为后端数量
 * fsyncs并没有说明
 * 缓冲访问策略。而且把两个fsync都算进去了
 * IOCONTEXT_NORMAL和IOCONTEXT_[Bulkread， Bulkwrite， vacuum] 在下面
 * IOCONTEXT_NORMAL在研究 的数量时可能更为清晰
 * 后端同步。
 */
		pgstat_count_io_op_time(IOOBJECT_RELATION, IOCONTEXT_NORMAL,
								IOOP_FSYNC, io_start, 1);
	}
}

/*
 * register_unlink_segment() -- Schedule a file to be deleted after next checkpoint
 */
static void
register_unlink_segment(RelFileLocatorBackend rlocator, ForkNumber forknum,
						BlockNumber segno)
{
	FileTag		tag;

	INIT_MD_FILETAG(tag, rlocator.locator, forknum, segno);

	/* Should never be used with temp relations */
	Assert(!RelFileLocatorBackendIsTemp(rlocator));

	RegisterSyncRequest(&tag, SYNC_UNLINK_REQUEST, true /* retryOnError */ );
}

/*
 * register_forget_request() -- forget any fsyncs for a relation fork's segment
 */
static void
register_forget_request(RelFileLocatorBackend rlocator, ForkNumber forknum,
						BlockNumber segno)
{
	FileTag		tag;

	INIT_MD_FILETAG(tag, rlocator.locator, forknum, segno);

	RegisterSyncRequest(&tag, SYNC_FORGET_REQUEST, true /* retryOnError */ );
}

/*
 * ForgetDatabaseSyncRequests -- forget any fsyncs and unlinks for a DB
 */
void
ForgetDatabaseSyncRequests(Oid dbid)
{
	FileTag		tag;
	RelFileLocator rlocator;

	rlocator.dbOid = dbid;
	rlocator.spcOid = 0;
	rlocator.relNumber = 0;

	INIT_MD_FILETAG(tag, rlocator, InvalidForkNumber, InvalidBlockNumber);

	RegisterSyncRequest(&tag, SYNC_FILTER_REQUEST, true /* retryOnError */ );
}

/*
 * DropRelationFiles -- drop files of all given relations
 */
void
DropRelationFiles(RelFileLocator *delrels, int ndelrels, bool isRedo)
{
	SMgrRelation *srels;
	int			i;

	srels = palloc(sizeof(SMgrRelation) * ndelrels);
	for (i = 0; i < ndelrels; i++)
	{
		SMgrRelation srel = smgropen(delrels[i], InvalidBackendId);

		if (isRedo)
		{
			ForkNumber	fork;

			for (fork = 0; fork <= MAX_FORKNUM; fork++)
				XLogDropRelation(delrels[i], fork);
		}
		srels[i] = srel;
	}

	smgrdounlinkall(srels, ndelrels, isRedo);

	for (i = 0; i < ndelrels; i++)
		smgrclose(srels[i]);
	pfree(srels);
}


/*
 * _fdvec_resize() -- Resize the fork's open segments array
 */
static void
_fdvec_resize(SMgrRelation reln,
			  ForkNumber forknum,
			  int nseg)
{
	if (nseg == 0)
	{
		if (reln->md_num_open_segs[forknum] > 0)
		{
			pfree(reln->md_seg_fds[forknum]);
			reln->md_seg_fds[forknum] = NULL;
		}
	}
	else if (reln->md_num_open_segs[forknum] == 0)
	{
		reln->md_seg_fds[forknum] =
			MemoryContextAlloc(MdCxt, sizeof(MdfdVec) * nseg);
	}
	else if (nseg > reln->md_num_open_segs[forknum])
	{
		/*
		 * It doesn't seem worthwhile complicating the code to amortize
		 * repalloc() calls.  Those are far faster than PathNameOpenFile() or
		 * FileClose(), and the memory context internally will sometimes avoid
		 * doing an actual reallocation.
		 */
		reln->md_seg_fds[forknum] =
			repalloc(reln->md_seg_fds[forknum],
					 sizeof(MdfdVec) * nseg);
	}
	else
	{
		/*
		 * We don't reallocate a smaller array, because we want mdtruncate()
		 * to be able to promise that it won't allocate memory, so that it is
		 * allowed in a critical section.  This means that a bit of space in
		 * the array is now wasted, until the next time we add a segment and
		 * reallocate.
		 */
	}

	reln->md_num_open_segs[forknum] = nseg;
}

/*
 * Return the filename for the specified segment of the relation. The
 * returned string is palloc'd.
 */
static char *
_mdfd_segpath(SMgrRelation reln, ForkNumber forknum, BlockNumber segno)
{
	char	   *path,
			   *fullpath;

	path = relpath(reln->smgr_rlocator, forknum);

	if (segno > 0)
	{
		fullpath = psprintf("%s.%u", path, segno);
		pfree(path);
	}
	else
		fullpath = path;

	return fullpath;
}

/*
 * Open the specified segment of the relation,
 * and make a MdfdVec object for it.  Returns NULL on failure.
 */
static MdfdVec *
_mdfd_openseg(SMgrRelation reln, ForkNumber forknum, BlockNumber segno,
			  int oflags)
{
	MdfdVec    *v;
	File		fd;
	char	   *fullpath;

	fullpath = _mdfd_segpath(reln, forknum, segno);

	/* open the file */
	fd = PathNameOpenFile(fullpath, _mdfd_open_flags() | oflags);

	pfree(fullpath);

	if (fd < 0)
		return NULL;

	/*
	 * Segments are always opened in order from lowest to highest, so we must
	 * be adding a new one at the end.
	 */
	Assert(segno == reln->md_num_open_segs[forknum]);

	_fdvec_resize(reln, forknum, segno + 1);

	/* fill the entry */
	v = &reln->md_seg_fds[forknum][segno];
	v->mdfd_vfd = fd;
	v->mdfd_segno = segno;

	Assert(_mdnblocks(reln, forknum, v) <= ((BlockNumber) RELSEG_SIZE));

	/* all done */
	return v;
}

/*
 * _mdfd_getseg() -- Find the segment of the relation holding the
 *					 specified block.
 *
 * If the segment doesn't exist, we ereport, return NULL, or create the
 * segment, according to "behavior".  Note: skipFsync is only used in the
 * EXTENSION_CREATE case.
 */
static MdfdVec *
_mdfd_getseg(SMgrRelation reln, ForkNumber forknum, BlockNumber blkno,
			 bool skipFsync, int behavior)
{
	MdfdVec    *v;
	BlockNumber targetseg;
	BlockNumber nextsegno;

	/* some way to handle non-existent segments needs to be specified */
	Assert(behavior &
		   (EXTENSION_FAIL | EXTENSION_CREATE | EXTENSION_RETURN_NULL |
			EXTENSION_DONT_OPEN));

	targetseg = blkno / ((BlockNumber) RELSEG_SIZE);

	/* if an existing and opened segment, we're done */
	if (targetseg < reln->md_num_open_segs[forknum])
	{
		v = &reln->md_seg_fds[forknum][targetseg];
		return v;
	}

	/* The caller only wants the segment if we already had it open. */
	if (behavior & EXTENSION_DONT_OPEN)
		return NULL;

	/*
	 * The target segment is not yet open. Iterate over all the segments
	 * between the last opened and the target segment. This way missing
	 * segments either raise an error, or get created (according to
	 * 'behavior'). Start with either the last opened, or the first segment if
	 * none was opened before.
	 */
	if (reln->md_num_open_segs[forknum] > 0)
		v = &reln->md_seg_fds[forknum][reln->md_num_open_segs[forknum] - 1];
	else
	{
		v = mdopenfork(reln, forknum, behavior);
		if (!v)
			return NULL;		/* if behavior & EXTENSION_RETURN_NULL */
	}

	for (nextsegno = reln->md_num_open_segs[forknum];
		 nextsegno <= targetseg; nextsegno++)
	{
		BlockNumber nblocks = _mdnblocks(reln, forknum, v);
		int			flags = 0;

		Assert(nextsegno == v->mdfd_segno + 1);

		if (nblocks > ((BlockNumber) RELSEG_SIZE))
			elog(FATAL, "segment too big");

		if ((behavior & EXTENSION_CREATE) ||
			(InRecovery && (behavior & EXTENSION_CREATE_RECOVERY)))
		{
			/*
			 * Normally we will create new segments only if authorized by the
			 * caller (i.e., we are doing mdextend()).  But when doing WAL
			 * recovery, create segments anyway; this allows cases such as
			 * replaying WAL data that has a write into a high-numbered
			 * segment of a relation that was later deleted. We want to go
			 * ahead and create the segments so we can finish out the replay.
			 *
			 * We have to maintain the invariant that segments before the last
			 * active segment are of size RELSEG_SIZE; therefore, if
			 * extending, pad them out with zeroes if needed.  (This only
			 * matters if in recovery, or if the caller is extending the
			 * relation discontiguously, but that can happen in hash indexes.)
			 */
/*
 * 通常只有在获得
 * 呼叫者（即我们正在使用 mdextend（））。 但做WAL
 * 恢复，无论如何都要创建分段;这允许以下情况
 * 重放带有写入高编号 的 WAL 数据
 * 一段后来被删除的关系片段。我们想去
 * 前进并创建片段，这样我们才能完成回放。
 *
 * 我们必须保持在上一个之前分段的不变量
 * 活动段大小为RELSEG_SIZE;因此，如果
 * 延长，必要时用零填充。 （仅此而已
 * 是否处于康复状态，或呼叫者是否在延长
 * 关系是不连续的，但这在哈希索引中也可能发生。）
 */
			if (nblocks < ((BlockNumber) RELSEG_SIZE))
			{
				char	   *zerobuf = palloc_aligned(BLCKSZ, PG_IO_ALIGN_SIZE,
													 MCXT_ALLOC_ZERO);

				mdextend(reln, forknum,
						 nextsegno * ((BlockNumber) RELSEG_SIZE) - 1,
						 zerobuf, skipFsync);
				pfree(zerobuf);
			}
			flags = O_CREAT;
		}
		else if (!(behavior & EXTENSION_DONT_CHECK_SIZE) &&
				 nblocks < ((BlockNumber) RELSEG_SIZE))
		{
			/*
			 * When not extending (or explicitly including truncated
			 * segments), only open the next segment if the current one is
			 * exactly RELSEG_SIZE.  If not (this branch), either return NULL
			 * or fail.
			 */
	/*
 * 当不扩展（或明确包含截断）
 * 段），只有当当前段为
 * 完全RELSEG_SIZE。 如果不是（该分支），则返回 NULL
 * 或失败。
 */
			if (behavior & EXTENSION_RETURN_NULL)
			{
				/*
				 * Some callers discern between reasons for _mdfd_getseg()
				 * returning NULL based on errno. As there's no failing
				 * syscall involved in this case, explicitly set errno to
				 * ENOENT, as that seems the closest interpretation.
				 */
/*
 * 有些来电者会分辨_mdfd_getseg原因
 * 根据错误返回NULL。因为没有失败
 * 此案涉及系统调用，明确将 errno 设置为
 * ENOENT，因为这似乎是最接近的解释。
 */
				errno = ENOENT;
				return NULL;
			}

			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\" (target block %u): previous segment is only %u blocks",
							_mdfd_segpath(reln, forknum, nextsegno),
							blkno, nblocks)));
		}

		v = _mdfd_openseg(reln, forknum, nextsegno, flags);

		if (v == NULL)
		{
			if ((behavior & EXTENSION_RETURN_NULL) &&
				FILE_POSSIBLY_DELETED(errno))
				return NULL;
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\" (target block %u): %m",
							_mdfd_segpath(reln, forknum, nextsegno),
							blkno)));
		}
	}

	return v;
}

/*
 * Get number of blocks present in a single disk file
 */
static BlockNumber
_mdnblocks(SMgrRelation reln, ForkNumber forknum, MdfdVec *seg)
{
	off_t		len;

	len = FileSize(seg->mdfd_vfd);
	if (len < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not seek to end of file \"%s\": %m",
						FilePathName(seg->mdfd_vfd))));
	/* note that this calculation will ignore any partial block at EOF */
	return (BlockNumber) (len / BLCKSZ);
}

/*
 * Sync a file to disk, given a file tag.  Write the path into an output
 * buffer so the caller can use it in error messages.
 *
 * Return 0 on success, -1 on failure, with errno set.
 */
/*
 * 将文件同步到磁盘，给定文件标签。 将路径写入输出
 * 缓冲区，方便呼叫者在错误消息中使用。
 *
 * 成功时返回0，失败时返回-1，errno设置。
 */
int
mdsyncfiletag(const FileTag *ftag, char *path)
{
	SMgrRelation reln = smgropen(ftag->rlocator, InvalidBackendId);
	File		file;
	instr_time	io_start;
	bool		need_to_close;
	int			result,
				save_errno;

	/* See if we already have the file open, or need to open it. */
	if (ftag->segno < reln->md_num_open_segs[ftag->forknum])
	{
		file = reln->md_seg_fds[ftag->forknum][ftag->segno].mdfd_vfd;
		strlcpy(path, FilePathName(file), MAXPGPATH);
		need_to_close = false;
	}
	else
	{
		char	   *p;

		p = _mdfd_segpath(reln, ftag->forknum, ftag->segno);
		strlcpy(path, p, MAXPGPATH);
		pfree(p);

		file = PathNameOpenFile(path, _mdfd_open_flags());
		if (file < 0)
			return -1;
		need_to_close = true;
	}

	io_start = pgstat_prepare_io_time();

	/* Sync the file. */
	result = FileSync(file, WAIT_EVENT_DATA_FILE_SYNC);
	save_errno = errno;

	if (need_to_close)
		FileClose(file);

	pgstat_count_io_op_time(IOOBJECT_RELATION, IOCONTEXT_NORMAL,
							IOOP_FSYNC, io_start, 1);

	errno = save_errno;
	return result;
}

/*
 * Unlink a file, given a file tag.  Write the path into an output
 * buffer so the caller can use it in error messages.
 *
 * Return 0 on success, -1 on failure, with errno set.
 */
/*
 * 给定文件标签后，解除链接。 将路径写入输出
 * 缓冲区，方便呼叫者在错误消息中使用。
 *
 * 成功时返回0，失败时返回-1，errno设置。
 */
int
mdunlinkfiletag(const FileTag *ftag, char *path)
{
	char	   *p;

	/* Compute the path. */
	p = relpathperm(ftag->rlocator, MAIN_FORKNUM);
	strlcpy(path, p, MAXPGPATH);
	pfree(p);

	/* Try to unlink the file. */
	return unlink(path);
}

/*
 * Check if a given candidate request matches a given tag, when processing
 * a SYNC_FILTER_REQUEST request.  This will be called for all pending
 * requests to find out whether to forget them.
 */
/*
 * 处理时检查候选请求是否匹配给定标签
 * 一个SYNC_FILTER_REQUEST请求。 所有待处理的都会被调用
 * 请求是否忘记他们。
 */
bool
mdfiletagmatches(const FileTag *ftag, const FileTag *candidate)
{
	/*
	 * For now we only use filter requests as a way to drop all scheduled
	 * callbacks relating to a given database, when dropping the database.
	 * We'll return true for all candidates that have the same database OID as
	 * the ftag from the SYNC_FILTER_REQUEST request, so they're forgotten.
	 */
/*
 * 目前我们只用过滤请求来删除所有排程
 * 与给定数据库相关的回调，当丢弃数据库时。
 * 对于所有拥有相同数据库OID的候选人，我们会返回true。
 * SYNC_FILTER_REQUEST请求中的FTAG，所以他们被遗忘了。
 */
	return ftag->rlocator.dbOid == candidate->rlocator.dbOid;
}

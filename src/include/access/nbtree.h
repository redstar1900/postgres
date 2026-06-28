/*-------------------------------------------------------------------------
 *
 * nbtree.h
 *	  header file for postgres btree access method implementation.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/nbtree.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NBTREE_H
#define NBTREE_H

#include "access/amapi.h"
#include "access/itup.h"
#include "access/sdir.h"
#include "access/tableam.h"
#include "access/xlogreader.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_index.h"
#include "lib/stringinfo.h"
#include "storage/bufmgr.h"
#include "storage/shm_toc.h"

/* There's room for a 16-bit vacuum cycle ID in BTPageOpaqueData */
typedef uint16 BTCycleId;

/*
 *	BTPageOpaqueData -- At the end of every page, we store a pointer
 *	to both siblings in the tree.  This is used to do forward/backward
 *	index scans.  The next-page link is also critical for recovery when
 *	a search has navigated to the wrong page due to concurrent page splits
 *	or deletions; see src/backend/access/nbtree/README for more info.
 *
 *	In addition, we store the page's btree level (counting upwards from
 *	zero at a leaf page) as well as some flag bits indicating the page type
 *	and status.  If the page is deleted, a BTDeletedPageData struct is stored
 *	in the page's tuple area, while a standard BTPageOpaqueData struct is
 *	stored in the page special area.
 *
 *	We also store a "vacuum cycle ID".  When a page is split while VACUUM is
 *	processing the index, a nonzero value associated with the VACUUM run is
 *	stored into both halves of the split page.  (If VACUUM is not running,
 *	both pages receive zero cycleids.)	This allows VACUUM to detect whether
 *	a page was split since it started, with a small probability of false match
 *	if the page was last split some exact multiple of MAX_BT_CYCLE_ID VACUUMs
 *	ago.  Also, during a split, the BTP_SPLIT_END flag is cleared in the left
 *	(original) page, and set in the right page, but only if the next page
 *	to its right has a different cycleid.
 *
 *	NOTE: the BTP_LEAF flag bit is redundant since level==0 could be tested
 *	instead.
 *
 *	NOTE: the btpo_level field used to be a union type in order to allow
 *	deleted pages to store a 32-bit safexid in the same field.  We now store
 *	64-bit/full safexid values using BTDeletedPageData instead.
 */
/*
 * BTPageOpaqueData ——每页末尾我们都会存储一个指针
 * 献给树上的两位兄弟姐妹。 这用于前进/后退操作
 * 索引扫描。 下一页链接对于恢复也至关重要
 * 由于并发页面分割，搜索导航到错误页面
 * 或删除;更多信息请参见src/backend/access/nbtree/README。
 *
 * 此外，我们存储页面的 btree 层级（从中数到现在
 * 叶页为零），以及一些指示页面类型的标志位
 * 以及状态。 如果页面被删除，会存储一个 BTDeletedPageData 结构体
 * 在页面的元组区域，而标准的 BTPageOpaqueData 结构体则是
 * 存储在页面特别区域。
 *
 * 我们还存储了“真空循环ID”。 当页面被拆分而VACUUM则是
 * 处理索引，VACUUM 运行相关的非零值为
 * 存储在分页的两半中。 （如果真空未运行，
 * 两页均无 CycleID。）	这使得VACUM能够检测
 * 自开始以来，已有一页被分割，且有小概率出现错误匹配
 * 如果页面最后一次被拆分为MAX_BT_CYCLE_ID真空的正好倍数
 * 很久以前。 此外，在分叉时，左侧的BTP_SPLIT_END旗会被清除
 *（原始）页面，并且设置在正确的页面，但仅限于下一页
 * 右侧的周期id不同。
 *
 * 注意：BTP_LEAF标志位是冗余的，因为可以测试电平==0
 * 而不是。
 *
 * 注意：btpo_level字段曾为并集类型，以便允许
 * 删除页面以在同一字段存储32位安全X。 我们现在存放
 * 使用 BTDeletedPageData 的 64 位/完整 safexid 值。
 */
typedef struct BTPageOpaqueData
{
	BlockNumber btpo_prev;		/* left sibling, or P_NONE if leftmost */
	BlockNumber btpo_next;		/* right sibling, or P_NONE if rightmost */
	uint32		btpo_level;		/* tree level --- zero for leaf pages */
	uint16		btpo_flags;		/* flag bits, see below */
	BTCycleId	btpo_cycleid;	/* vacuum cycle ID of latest split */
} BTPageOpaqueData;

typedef BTPageOpaqueData *BTPageOpaque;

#define BTPageGetOpaque(page) ((BTPageOpaque) PageGetSpecialPointer(page))

/* Bits defined in btpo_flags */
#define BTP_LEAF		(1 << 0)	/* leaf page, i.e. not internal page */
#define BTP_ROOT		(1 << 1)	/* root page (has no parent) */
#define BTP_DELETED		(1 << 2)	/* page has been deleted from tree */
#define BTP_META		(1 << 3)	/* meta-page */
#define BTP_HALF_DEAD	(1 << 4)	/* empty, but still in tree */
#define BTP_SPLIT_END	(1 << 5)	/* rightmost page of split group */
#define BTP_HAS_GARBAGE (1 << 6)	/* page has LP_DEAD tuples (deprecated) */
#define BTP_INCOMPLETE_SPLIT (1 << 7)	/* right sibling's downlink is missing */
#define BTP_HAS_FULLXID	(1 << 8)	/* contains BTDeletedPageData */

/*
 * The max allowed value of a cycle ID is a bit less than 64K.  This is
 * for convenience of pg_filedump and similar utilities: we want to use
 * the last 2 bytes of special space as an index type indicator, and
 * restricting cycle ID lets btree use that space for vacuum cycle IDs
 * while still allowing index type to be identified.
 */
#define MAX_BT_CYCLE_ID		0xFF7F


/*
 * The Meta page is always the first page in the btree index.
 * Its primary purpose is to point to the location of the btree root page.
 * We also point to the "fast" root, which is the current effective root;
 * see README for discussion.
 */

typedef struct BTMetaPageData
{
	uint32		btm_magic;		/* should contain BTREE_MAGIC */
	uint32		btm_version;	/* nbtree version (always <= BTREE_VERSION) */
	BlockNumber btm_root;		/* current root location */
	uint32		btm_level;		/* tree level of the root page */
	BlockNumber btm_fastroot;	/* current "fast" root location */
	uint32		btm_fastlevel;	/* tree level of the "fast" root page */
	/* remaining fields only valid when btm_version >= BTREE_NOVAC_VERSION */

	/* number of deleted, non-recyclable pages during last cleanup */
	uint32		btm_last_cleanup_num_delpages;
	/* number of heap tuples during last cleanup (deprecated) */
	float8		btm_last_cleanup_num_heap_tuples;

	bool		btm_allequalimage;	/* are all columns "equalimage"? */
} BTMetaPageData;

#define BTPageGetMeta(p) \
	((BTMetaPageData *) PageGetContents(p))

/*
 * The current Btree version is 4.  That's what you'll get when you create
 * a new index.
 *
 * Btree version 3 was used in PostgreSQL v11.  It is mostly the same as
 * version 4, but heap TIDs were not part of the keyspace.  Index tuples
 * with duplicate keys could be stored in any order.  We continue to
 * support reading and writing Btree versions 2 and 3, so that they don't
 * need to be immediately re-indexed at pg_upgrade.  In order to get the
 * new heapkeyspace semantics, however, a REINDEX is needed.
 *
 * Deduplication is safe to use when the btm_allequalimage field is set to
 * true.  It's safe to read the btm_allequalimage field on version 3, but
 * only version 4 indexes make use of deduplication.  Even version 4
 * indexes created on PostgreSQL v12 will need a REINDEX to make use of
 * deduplication, though, since there is no other way to set
 * btm_allequalimage to true (pg_upgrade hasn't been taught to set the
 * metapage field).
 *
 * Btree version 2 is mostly the same as version 3.  There are two new
 * fields in the metapage that were introduced in version 3.  A version 2
 * metapage will be automatically upgraded to version 3 on the first
 * insert to it.  INCLUDE indexes cannot use version 2.
 */
/*
 * 当前的Btree版本是4。 这就是你创作时会得到的
 * 一个新的索引。
 *
 * Btree 3 版本曾用于 PostgreSQL v11。 它大致上与
 * 版本4，但堆TID不属于密钥空间。 索引元组
 * 带有重复密钥的存储顺序可以任意。 我们继续
 * 支持读取和写入 Btree 版本 2 和 3，使其不
 * 需要立即重新索引于pg_upgrade。 为了获得
 * 新的堆密钥空间语义，但需要重新索引。
 *
 * 当btm_allequalimage字段设置为 时，重复去重是安全的
 * 确实如此。 3版的btm_allequalimage栏是安全的，但
 * 只有版本4的索引使用去重功能。 甚至是第四版
 * 在PostgreSQL v12上创建的索引需要重新索引才能使用
 * 但需要去重，因为没有其他方法可以设置
 * btm_allequalimage 真（pg_upgrade 没有被教过如何设置
 * 元页面字段）。
 *
 * Btree版本2与版本3基本相同。 有两个新角色
 * 元页面中的字段，这些字段是在版本3中引入的。 A 版本 2
 * Metapage 将在第一版自动升级到 3 版
 * 插入其中。 INCLUDE 索引不能使用 2 版。
 */
#define BTREE_METAPAGE	0		/* first page is meta */
#define BTREE_MAGIC		0x053162	/* magic number in metapage */
#define BTREE_VERSION	4		/* current version number */
#define BTREE_MIN_VERSION	2	/* minimum supported version */
#define BTREE_NOVAC_VERSION	3	/* version with all meta fields set */

/*
 * Maximum size of a btree index entry, including its tuple header.
 *
 * We actually need to be able to fit three items on every page,
 * so restrict any one item to 1/3 the per-page available space.
 *
 * There are rare cases where _bt_truncate() will need to enlarge
 * a heap index tuple to make space for a tiebreaker heap TID
 * attribute, which we account for here.
 */
/*
 * btree 索引条目的最大大小，包括其元组头部。
 *
 * 我们实际上需要在每一页放三个物品，
 * 因此，将任何一项限制在每页可用空间的三分之一。
 *
 * 在极少数情况下，_bt_truncate（）需要放大
 * 堆索引元组用于腾出空间给平局决胜堆 TID
 * 属性，我们在这里考虑了这一点。
 */
#define BTMaxItemSize(page) \
	(MAXALIGN_DOWN((PageGetPageSize(page) - \
					MAXALIGN(SizeOfPageHeaderData + 3*sizeof(ItemIdData)) - \
					MAXALIGN(sizeof(BTPageOpaqueData))) / 3) - \
					MAXALIGN(sizeof(ItemPointerData)))
#define BTMaxItemSizeNoHeapTid(page) \
	MAXALIGN_DOWN((PageGetPageSize(page) - \
				   MAXALIGN(SizeOfPageHeaderData + 3*sizeof(ItemIdData)) - \
				   MAXALIGN(sizeof(BTPageOpaqueData))) / 3)

/*
 * MaxTIDsPerBTreePage is an upper bound on the number of heap TIDs tuples
 * that may be stored on a btree leaf page.  It is used to size the
 * per-page temporary buffers.
 *
 * Note: we don't bother considering per-tuple overheads here to keep
 * things simple (value is based on how many elements a single array of
 * heap TIDs must have to fill the space between the page header and
 * special area).  The value is slightly higher (i.e. more conservative)
 * than necessary as a result, which is considered acceptable.
 */
/*
 * MaxTIDsPerBTreePage 是堆 TIDs 元组数量的上界
 * 可能存储在树叶页上。 它用于为
 * 每页临时缓冲区。
 *
 * 注：我们这里不考虑每元组的开销
 * 简单事物（值基于单个数组的元素数量
 * 堆 TID 必须填满页面头和
 * 特殊区域）。 其数值略高（即更保守）
 * 比必要的结果更重要，这被认为是可以接受的。
 */
#define MaxTIDsPerBTreePage \
	(int) ((BLCKSZ - SizeOfPageHeaderData - sizeof(BTPageOpaqueData)) / \
		   sizeof(ItemPointerData))

/*
 * The leaf-page fillfactor defaults to 90% but is user-adjustable.
 * For pages above the leaf level, we use a fixed 70% fillfactor.
 * The fillfactor is applied during index build and when splitting
 * a rightmost page; when splitting non-rightmost pages we try to
 * divide the data equally.  When splitting a page that's entirely
 * filled with a single value (duplicates), the effective leaf-page
 * fillfactor is 96%, regardless of whether the page is a rightmost
 * page.
 */
/*
 * 叶页填充率默认为90%，但用户可调节。
 * 对于叶子层以上的页面，我们使用固定的70%填充率。
 * 填充因子在索引构建和拆分时应用
 * 最右侧一页;在拆分非最右侧的页面时，我们会尝试
 * 将数据平均分配。 当拆分一页时，完全是
 * 填充单个值（重复），即有效叶页
 * 填充率为96%，无论页面是否最右边
 * 页面。
 */
#define BTREE_MIN_FILLFACTOR		10
#define BTREE_DEFAULT_FILLFACTOR	90
#define BTREE_NONLEAF_FILLFACTOR	70
#define BTREE_SINGLEVAL_FILLFACTOR	96

/*
 *	In general, the btree code tries to localize its knowledge about
 *	page layout to a couple of routines.  However, we need a special
 *	value to indicate "no page number" in those places where we expect
 *	page numbers.  We can use zero for this because we never need to
 *	make a pointer to the metadata page.
 */
/*
 * 一般来说，btree代码试图对
 * 页面布局，包含几个例行程序。 不过，我们需要一个特别的
 * 值表示在我们预期的那些地方“无页码”
 * 页码。 我们可以用零来实现这个，因为我们从来不需要
 * 指向元数据页面。
 */
#define P_NONE			0

/*
 * Macros to test whether a page is leftmost or rightmost on its tree level,
 * as well as other state info kept in the opaque data.
 */
/*
 * 宏用于测试页面在树级上是最左还是最右，
 * 以及其他保存在不透明数据中的州级信息。
 */
#define P_LEFTMOST(opaque)		((opaque)->btpo_prev == P_NONE)
#define P_RIGHTMOST(opaque)		((opaque)->btpo_next == P_NONE)
#define P_ISLEAF(opaque)		(((opaque)->btpo_flags & BTP_LEAF) != 0)
#define P_ISROOT(opaque)		(((opaque)->btpo_flags & BTP_ROOT) != 0)
#define P_ISDELETED(opaque)		(((opaque)->btpo_flags & BTP_DELETED) != 0)
#define P_ISMETA(opaque)		(((opaque)->btpo_flags & BTP_META) != 0)
#define P_ISHALFDEAD(opaque)	(((opaque)->btpo_flags & BTP_HALF_DEAD) != 0)
#define P_IGNORE(opaque)		(((opaque)->btpo_flags & (BTP_DELETED|BTP_HALF_DEAD)) != 0)
#define P_HAS_GARBAGE(opaque)	(((opaque)->btpo_flags & BTP_HAS_GARBAGE) != 0)
#define P_INCOMPLETE_SPLIT(opaque)	(((opaque)->btpo_flags & BTP_INCOMPLETE_SPLIT) != 0)
#define P_HAS_FULLXID(opaque)	(((opaque)->btpo_flags & BTP_HAS_FULLXID) != 0)

/*
 * BTDeletedPageData is the page contents of a deleted page
 */
/*
 * BTDeletedPageData 是已删除页面的页面内容
 */
typedef struct BTDeletedPageData
{
	FullTransactionId safexid;	/* See BTPageIsRecyclable() */
} BTDeletedPageData;

static inline void
BTPageSetDeleted(Page page, FullTransactionId safexid)
{
	BTPageOpaque opaque;
	PageHeader	header;
	BTDeletedPageData *contents;

	opaque = BTPageGetOpaque(page);
	header = ((PageHeader) page);

	opaque->btpo_flags &= ~BTP_HALF_DEAD;
	opaque->btpo_flags |= BTP_DELETED | BTP_HAS_FULLXID;
	header->pd_lower = MAXALIGN(SizeOfPageHeaderData) +
		sizeof(BTDeletedPageData);
	header->pd_upper = header->pd_special;

	/* Set safexid in deleted page */
	contents = ((BTDeletedPageData *) PageGetContents(page));
	contents->safexid = safexid;
}

static inline FullTransactionId
BTPageGetDeleteXid(Page page)
{
	BTPageOpaque opaque;
	BTDeletedPageData *contents;

	/* We only expect to be called with a deleted page */
	Assert(!PageIsNew(page));
	opaque = BTPageGetOpaque(page);
	Assert(P_ISDELETED(opaque));

	/* pg_upgrade'd deleted page -- must be safe to delete now */
	if (!P_HAS_FULLXID(opaque))
		return FirstNormalFullTransactionId;

	/* Get safexid from deleted page */
	contents = ((BTDeletedPageData *) PageGetContents(page));
	return contents->safexid;
}

/*
 * Is an existing page recyclable?
 *
 * This exists to centralize the policy on which deleted pages are now safe to
 * re-use.  However, _bt_pendingfsm_finalize() duplicates some of the same
 * logic because it doesn't work directly with pages -- keep the two in sync.
 *
 * Note: PageIsNew() pages are always safe to recycle, but we can't deal with
 * them here (caller is responsible for that case themselves).  Caller might
 * well need special handling for new pages anyway.
 */
/*
 * 现有页面可以回收吗？
 *
 * 此页面旨在集中制定已删除页面的安全政策
 * 再利用。 然而，_bt_pendingfsm_finalize（）会重复部分相同的内容
 * 逻辑，因为它不能直接和页面一起工作——保持两者同步。
 *
 * 注：PageIsNew（） 页面始终安全可回收，但我们无法处理
 * 他们在这里（来电者对该案负责）。 来电者可能
 * 反正新页面也需要特别处理。
 */
static inline bool
BTPageIsRecyclable(Page page, Relation heaprel)
{
	BTPageOpaque opaque;

	Assert(!PageIsNew(page));
	Assert(heaprel != NULL);

	/* Recycling okay iff page is deleted and safexid is old enough */
	opaque = BTPageGetOpaque(page);
	if (P_ISDELETED(opaque))
	{
		FullTransactionId safexid = BTPageGetDeleteXid(page);

		/*
		 * The page was deleted, but when? If it was just deleted, a scan
		 * might have seen the downlink to it, and will read the page later.
		 * As long as that can happen, we must keep the deleted page around as
		 * a tombstone.
		 *
		 * For that check if the deletion XID could still be visible to
		 * anyone. If not, then no scan that's still in progress could have
		 * seen its downlink, and we can recycle it.
		 */
/*
 * 页面被删除了，但是什么时候？如果只是被删除，就扫描一下
 * 可能看过它的下游链接，稍后会阅读页面。
 * 只要能做到这一点，我们就必须保留被删除的页面，因为
 * 墓碑。
 *
 * 检查删除XID是否仍可见
 * 任何人。如果没有，那正在进行的扫描就可能有问题
 * 看到了它的下行链路，我们可以回收它。
 */
		return GlobalVisCheckRemovableFullXid(heaprel, safexid);
	}

	return false;
}

/*
 * BTVacState and BTPendingFSM are private nbtree.c state used during VACUUM.
 * They are exported for use by page deletion related code in nbtpage.c.
 */
/*
 * BTVacState 和 BTPendingFSM 是 VACUUM 期间使用的私有 nbtree.c 状态。
 * 它们被导出用于nbtpage.c中页面删除相关代码。
 */
typedef struct BTPendingFSM
{
	BlockNumber target;			/* Page deleted by current VACUUM */
	FullTransactionId safexid;	/* Page's BTDeletedPageData.safexid */
} BTPendingFSM;

typedef struct BTVacState
{
	IndexVacuumInfo *info;
	IndexBulkDeleteResult *stats;
	IndexBulkDeleteCallback callback;
	void	   *callback_state;
	BTCycleId	cycleid;
	MemoryContext pagedelcontext;

	/*
	 * _bt_pendingfsm_finalize() state
	 */
	int			bufsize;		/* pendingpages space (in # elements) */
	int			maxbufsize;		/* max bufsize that respects work_mem */
	BTPendingFSM *pendingpages; /* One entry per newly deleted page */
	int			npendingpages;	/* current # valid pendingpages */
} BTVacState;

/*
 *	Lehman and Yao's algorithm requires a ``high key'' on every non-rightmost
 *	page.  The high key is not a tuple that is used to visit the heap.  It is
 *	a pivot tuple (see "Notes on B-Tree tuple format" below for definition).
 *	The high key on a page is required to be greater than or equal to any
 *	other key that appears on the page.  If we find ourselves trying to
 *	insert a key that is strictly > high key, we know we need to move right
 *	(this should only happen if the page was split since we examined the
 *	parent page).
 *
 *	Our insertion algorithm guarantees that we can use the initial least key
 *	on our right sibling as the high key.  Once a page is created, its high
 *	key changes only if the page is split.
 *
 *	On a non-rightmost page, the high key lives in item 1 and data items
 *	start in item 2.  Rightmost pages have no high key, so we store data
 *	items beginning in item 1.
 */
/*
 * Lehman 和 Yao 的算法要求对每个非最右边的 Alpha 进行“高键”
 * 页面。 高键不是用来访问堆的元组。 确实如此
 * 一个枢轴元组（定义见下文“B树元组格式说明”）。
 * 页面上的高键必须大于或等于任意
 * 页面上出现的其他键。 如果我们发现自己在努力
 * 插入一个严格>高调的调性，我们知道需要向右移动
 *（这只应该发生在页面被拆分时，因为我们检查了
 * 父页）。
 *
 * 我们的插入算法保证可以使用初始最小密钥
 * 在我们右边的兄弟姐妹，作为高调。 一旦页面创建，它就很高
 * 只有页面被拆分时键位才会变。
 *
 * 在非最右侧页面，高键位于第1项和数据项
 * 从第二项开始。 最右侧的页面没有高键，所以我们存储数据
 * 项目开始于第1项。
 */
#define P_HIKEY				((OffsetNumber) 1)
#define P_FIRSTKEY			((OffsetNumber) 2)
#define P_FIRSTDATAKEY(opaque)	(P_RIGHTMOST(opaque) ? P_HIKEY : P_FIRSTKEY)

/*
 * Notes on B-Tree tuple format, and key and non-key attributes:
 *
 * INCLUDE B-Tree indexes have non-key attributes.  These are extra
 * attributes that may be returned by index-only scans, but do not influence
 * the order of items in the index (formally, non-key attributes are not
 * considered to be part of the key space).  Non-key attributes are only
 * present in leaf index tuples whose item pointers actually point to heap
 * tuples (non-pivot tuples).  _bt_check_natts() enforces the rules
 * described here.
 *
 * Non-pivot tuple format (plain/non-posting variant):
 *
 *  t_tid | t_info | key values | INCLUDE columns, if any
 *
 * t_tid points to the heap TID, which is a tiebreaker key column as of
 * BTREE_VERSION 4.
 *
 * Non-pivot tuples complement pivot tuples, which only have key columns.
 * The sole purpose of pivot tuples is to represent how the key space is
 * separated.  In general, any B-Tree index that has more than one level
 * (i.e. any index that does not just consist of a metapage and a single
 * leaf root page) must have some number of pivot tuples, since pivot
 * tuples are used for traversing the tree.  Suffix truncation can omit
 * trailing key columns when a new pivot is formed, which makes minus
 * infinity their logical value.  Since BTREE_VERSION 4 indexes treat heap
 * TID as a trailing key column that ensures that all index tuples are
 * physically unique, it is necessary to represent heap TID as a trailing
 * key column in pivot tuples, though very often this can be truncated
 * away, just like any other key column. (Actually, the heap TID is
 * omitted rather than truncated, since its representation is different to
 * the non-pivot representation.)
 *
 * Pivot tuple format:
 *
 *  t_tid | t_info | key values | [heap TID]
 *
 * We store the number of columns present inside pivot tuples by abusing
 * their t_tid offset field, since pivot tuples never need to store a real
 * offset (pivot tuples generally store a downlink in t_tid, though).  The
 * offset field only stores the number of columns/attributes when the
 * INDEX_ALT_TID_MASK bit is set, which doesn't count the trailing heap
 * TID column sometimes stored in pivot tuples -- that's represented by
 * the presence of BT_PIVOT_HEAP_TID_ATTR.  The INDEX_ALT_TID_MASK bit in
 * t_info is always set on BTREE_VERSION 4 pivot tuples, since
 * BTreeTupleIsPivot() must work reliably on heapkeyspace versions.
 *
 * In version 2 or version 3 (!heapkeyspace) indexes, INDEX_ALT_TID_MASK
 * might not be set in pivot tuples.  BTreeTupleIsPivot() won't work
 * reliably as a result.  The number of columns stored is implicitly the
 * same as the number of columns in the index, just like any non-pivot
 * tuple. (The number of columns stored should not vary, since suffix
 * truncation of key columns is unsafe within any !heapkeyspace index.)
 *
 * The 12 least significant bits from t_tid's offset number are used to
 * represent the number of key columns within a pivot tuple.  This leaves 4
 * status bits (BT_STATUS_OFFSET_MASK bits), which are shared by all tuples
 * that have the INDEX_ALT_TID_MASK bit set (set in t_info) to store basic
 * tuple metadata.  BTreeTupleIsPivot() and BTreeTupleIsPosting() use the
 * BT_STATUS_OFFSET_MASK bits.
 *
 * Sometimes non-pivot tuples also use a representation that repurposes
 * t_tid to store metadata rather than a TID.  PostgreSQL v13 introduced a
 * new non-pivot tuple format to support deduplication: posting list
 * tuples.  Deduplication merges together multiple equal non-pivot tuples
 * into a logically equivalent, space efficient representation.  A posting
 * list is an array of ItemPointerData elements.  Non-pivot tuples are
 * merged together to form posting list tuples lazily, at the point where
 * we'd otherwise have to split a leaf page.
 *
 * Posting tuple format (alternative non-pivot tuple representation):
 *
 *  t_tid | t_info | key values | posting list (TID array)
 *
 * Posting list tuples are recognized as such by having the
 * INDEX_ALT_TID_MASK status bit set in t_info and the BT_IS_POSTING status
 * bit set in t_tid's offset number.  These flags redefine the content of
 * the posting tuple's t_tid to store the location of the posting list
 * (instead of a block number), as well as the total number of heap TIDs
 * present in the tuple (instead of a real offset number).
 *
 * The 12 least significant bits from t_tid's offset number are used to
 * represent the number of heap TIDs present in the tuple, leaving 4 status
 * bits (the BT_STATUS_OFFSET_MASK bits).  Like any non-pivot tuple, the
 * number of columns stored is always implicitly the total number in the
 * index (in practice there can never be non-key columns stored, since
 * deduplication is not supported with INCLUDE indexes).
 */
/*
 * 关于B树元组格式及键和非键属性的说明：
 *
 * INCLUDE B树索引具有非键属性。 这些是额外的
 * 可通过仅索引扫描返回但不影响的属性
 * 索引中项的顺序（形式上，非键属性不为
 * 被视为密钥空间的一部分）。 非关键属性仅为
 * 存在于叶索引元组中，其项指针实际上指向堆
 * 元组（非枢轴元组）。 _bt_check_natts（） 执行规则
 * 此处描述。
 *
 * 非枢轴元组格式（纯/非后置变体）：
 *
 * t_tid |t_info |关键值 |如果有的话，包含列
 *
 * t_tid 指向堆 TID，即
 * BTREE_VERSION 4。
 *
 * 非枢轴元组补集枢轴元组，而枢轴元组仅有键列。
 * 枢轴元组的唯一目的是表示密钥空间的状态
 * 分开了。 一般来说，任何具有多个层级的B树索引
 *（即任何不仅由元页面和单一索引组成的索引
 * 叶根页）必须有若干枢轴元组，因为枢轴
 * 元组用于遍历树。 后缀截断可以省略
 * 当形成新的枢轴时，尾随关键列，使得负值
 * 它们的逻辑价值无限大。 由于BTREE_VERSION 4个索引处理堆
 * TID 作为后置键列，确保所有索引元组
 * 物理上唯一，需要将堆TID表示为后段
 * 枢轴元组中的键列，尽管通常可以截断
 * 离开，就像其他关键栏一样。（实际上，堆TID是
 * 省略而非截断，因其表示方式不同于
 * 非枢轴表示。）
 *
 * 枢轴元组格式：
 *
 * t_tid |t_info |关键值 |[回复]
 *
 * 我们通过滥用 来存储枢轴元组内存在的列数
 * 它们的t_tid偏移场，因为枢轴元组从不需要存储实数
 * 偏移（不过枢轴元组通常会在t_tid中存储下行链路）。 该
 * 偏移字段仅存储当
 * INDEX_ALT_TID_MASK位已设置，不包括后方堆
 * TID 列有时以枢轴元组形式存储——表示为
 * BT_PIVOT_HEAP_TID_ATTR的存在。 INDEX_ALT_TID_MASK咬了一口
 * t_info总是设在BTREE_VERSION 4个枢轴元组上，因为
 * BTreeTupleIsPivot（） 必须在堆域版本上可靠工作。
 *
 * 在版本 2 或 版本 3（！heapkeyspace）索引中，INDEX_ALT_TID_MASK
 * 可能不存在枢轴元组中。 BTreeTupleIsPivot（） 无法使用
 * 因此，可靠地存在。 存储的列数隐含为
 * 与索引中的列数相同，就像任何非枢轴节点一样
 * 元组。（存储的列数不应变化，因为后缀
 * 在任何 ！heapkeyspace 索引中，截断键列都是不安全的。）
 *
 * t_tid 偏移数中的 12 个最低有效位用于
 * 表示枢轴元组内的关键列数。 这样还剩4个
 * 状态位（3 BT_STATUS_OFFSET_MASK位），所有元组共享
 * INDEX_ALT_TID_MASK位设置（在t_info中设置）用于存储基本
 * 元组元数据。 BTreeTupleIsPivot（） 和 BTreeTupleIsPosting（） 使用以下
 * BT_STATUS_OFFSET_MASK块。
 *
 * 有时非枢轴元组也会使用重新利用的表示
 * t_tid用于存储元数据，而非TID。 PostgreSQL v13 引入了
 * 支持去重的新非枢轴元组格式：发布列表
 * 元组。 去重合并是多个相等的非枢轴元组
 * 变成逻辑等价、空间高效的表示。 一个派驻
 * 列表是 ItemPointerData 元素的数组。 非枢轴元组为
 * 在
 * 否则我们得分开一页。
 *
 * 发布元组格式（替代非核心元组表示）：
 *
 * t_tid |t_info |关键值 |发布列表（TID数组）
 *
 * 发布列表元组通过具有
 * INDEX_ALT_TID_MASK状态位在t_info中设置，BT_IS_POSTING状态
 * 位设在t_tid的偏移数中。 这些标志重新定义了 的内容
 * 发布元组的 t_tid 用于存储发布列表的位置
 *（代替块编号），以及堆的总 TID 数量
 * 出现在元组中（而非实际偏移数）。
 *
 * t_tid 偏移数中的 12 个最低有效位用于
 * 表示元组中存在的堆 TID 数量，保持 4 状态
 * 比特（BT_STATUS_OFFSET_MASK比特）。 像任何非枢轴元组一样，
 * 存储的列数总是隐含的总数
 * 索引（实际上永远不会存储非键列，因为
 * INCLUDE 索引不支持重复去重）。
 */
#define INDEX_ALT_TID_MASK			INDEX_AM_RESERVED_BIT

/* Item pointer offset bit masks */
#define BT_OFFSET_MASK				0x0FFF
#define BT_STATUS_OFFSET_MASK		0xF000
/* BT_STATUS_OFFSET_MASK status bits */
#define BT_PIVOT_HEAP_TID_ATTR		0x1000
#define BT_IS_POSTING				0x2000

/*
 * Mask allocated for number of keys in index tuple must be able to fit
 * maximum possible number of index attributes
 */
StaticAssertDecl(BT_OFFSET_MASK >= INDEX_MAX_KEYS,
				 "BT_OFFSET_MASK can't fit INDEX_MAX_KEYS");

/*
 * Note: BTreeTupleIsPivot() can have false negatives (but not false
 * positives) when used with !heapkeyspace indexes
 */
static inline bool
BTreeTupleIsPivot(IndexTuple itup)
{
	if ((itup->t_info & INDEX_ALT_TID_MASK) == 0)
		return false;
	/* absence of BT_IS_POSTING in offset number indicates pivot tuple */
	if ((ItemPointerGetOffsetNumberNoCheck(&itup->t_tid) & BT_IS_POSTING) != 0)
		return false;

	return true;
}

static inline bool
BTreeTupleIsPosting(IndexTuple itup)
{
	if ((itup->t_info & INDEX_ALT_TID_MASK) == 0)
		return false;
	/* presence of BT_IS_POSTING in offset number indicates posting tuple */
	if ((ItemPointerGetOffsetNumberNoCheck(&itup->t_tid) & BT_IS_POSTING) == 0)
		return false;

	return true;
}

static inline void
BTreeTupleSetPosting(IndexTuple itup, uint16 nhtids, int postingoffset)
{
	Assert(nhtids > 1);
	Assert((nhtids & BT_STATUS_OFFSET_MASK) == 0);
	Assert((size_t) postingoffset == MAXALIGN(postingoffset));
	Assert(postingoffset < INDEX_SIZE_MASK);
	Assert(!BTreeTupleIsPivot(itup));

	itup->t_info |= INDEX_ALT_TID_MASK;
	ItemPointerSetOffsetNumber(&itup->t_tid, (nhtids | BT_IS_POSTING));
	ItemPointerSetBlockNumber(&itup->t_tid, postingoffset);
}

static inline uint16
BTreeTupleGetNPosting(IndexTuple posting)
{
	OffsetNumber existing;

	Assert(BTreeTupleIsPosting(posting));

	existing = ItemPointerGetOffsetNumberNoCheck(&posting->t_tid);
	return (existing & BT_OFFSET_MASK);
}

static inline uint32
BTreeTupleGetPostingOffset(IndexTuple posting)
{
	Assert(BTreeTupleIsPosting(posting));

	return ItemPointerGetBlockNumberNoCheck(&posting->t_tid);
}

static inline ItemPointer
BTreeTupleGetPosting(IndexTuple posting)
{
	return (ItemPointer) ((char *) posting +
						  BTreeTupleGetPostingOffset(posting));
}

static inline ItemPointer
BTreeTupleGetPostingN(IndexTuple posting, int n)
{
	return BTreeTupleGetPosting(posting) + n;
}

/*
 * Get/set downlink block number in pivot tuple.
 *
 * Note: Cannot assert that tuple is a pivot tuple.  If we did so then
 * !heapkeyspace indexes would exhibit false positive assertion failures.
 */
static inline BlockNumber
BTreeTupleGetDownLink(IndexTuple pivot)
{
	return ItemPointerGetBlockNumberNoCheck(&pivot->t_tid);
}

static inline void
BTreeTupleSetDownLink(IndexTuple pivot, BlockNumber blkno)
{
	ItemPointerSetBlockNumber(&pivot->t_tid, blkno);
}

/*
 * Get number of attributes within tuple.
 *
 * Note that this does not include an implicit tiebreaker heap TID
 * attribute, if any.  Note also that the number of key attributes must be
 * explicitly represented in all heapkeyspace pivot tuples.
 *
 * Note: This is defined as a macro rather than an inline function to
 * avoid including rel.h.
 */
/*
 * 获取元组内的属性数量。
 *
 * 注意，这不包括隐含的平局决胜堆TID
 * 属性，如果有的话。 还要注意，关键属性的数量必须为
 * 在所有堆空间枢轴元组中显式表示。
 *
 * 注：这被定义为宏，而非 的内联函数
 * 避免包含 Rel.H.
 */
#define BTreeTupleGetNAtts(itup, rel)	\
	( \
		(BTreeTupleIsPivot(itup)) ? \
		( \
			ItemPointerGetOffsetNumberNoCheck(&(itup)->t_tid) & BT_OFFSET_MASK \
		) \
		: \
		IndexRelationGetNumberOfAttributes(rel) \
	)

/*
 * Set number of key attributes in tuple.
 *
 * The heap TID tiebreaker attribute bit may also be set here, indicating that
 * a heap TID value will be stored at the end of the tuple (i.e. using the
 * special pivot tuple representation).
 */
static inline void
BTreeTupleSetNAtts(IndexTuple itup, uint16 nkeyatts, bool heaptid)
{
	Assert(nkeyatts <= INDEX_MAX_KEYS);
	Assert((nkeyatts & BT_STATUS_OFFSET_MASK) == 0);
	Assert(!heaptid || nkeyatts > 0);
	Assert(!BTreeTupleIsPivot(itup) || nkeyatts == 0);

	itup->t_info |= INDEX_ALT_TID_MASK;

	if (heaptid)
		nkeyatts |= BT_PIVOT_HEAP_TID_ATTR;

	/* BT_IS_POSTING bit is deliberately unset here */
	ItemPointerSetOffsetNumber(&itup->t_tid, nkeyatts);
	Assert(BTreeTupleIsPivot(itup));
}

/*
 * Get/set leaf page's "top parent" link from its high key.  Used during page
 * deletion.
 *
 * Note: Cannot assert that tuple is a pivot tuple.  If we did so then
 * !heapkeyspace indexes would exhibit false positive assertion failures.
 */
static inline BlockNumber
BTreeTupleGetTopParent(IndexTuple leafhikey)
{
	return ItemPointerGetBlockNumberNoCheck(&leafhikey->t_tid);
}

static inline void
BTreeTupleSetTopParent(IndexTuple leafhikey, BlockNumber blkno)
{
	ItemPointerSetBlockNumber(&leafhikey->t_tid, blkno);
	BTreeTupleSetNAtts(leafhikey, 0, false);
}

/*
 * Get tiebreaker heap TID attribute, if any.
 *
 * This returns the first/lowest heap TID in the case of a posting list tuple.
 */
static inline ItemPointer
BTreeTupleGetHeapTID(IndexTuple itup)
{
	if (BTreeTupleIsPivot(itup))
	{
		/* Pivot tuple heap TID representation? */
		if ((ItemPointerGetOffsetNumberNoCheck(&itup->t_tid) &
			 BT_PIVOT_HEAP_TID_ATTR) != 0)
			return (ItemPointer) ((char *) itup + IndexTupleSize(itup) -
								  sizeof(ItemPointerData));

		/* Heap TID attribute was truncated */
		return NULL;
	}
	else if (BTreeTupleIsPosting(itup))
		return BTreeTupleGetPosting(itup);

	return &itup->t_tid;
}

/*
 * Get maximum heap TID attribute, which could be the only TID in the case of
 * a non-pivot tuple that does not have a posting list tuple.
 *
 * Works with non-pivot tuples only.
 */
static inline ItemPointer
BTreeTupleGetMaxHeapTID(IndexTuple itup)
{
	Assert(!BTreeTupleIsPivot(itup));

	if (BTreeTupleIsPosting(itup))
	{
		uint16		nposting = BTreeTupleGetNPosting(itup);

		return BTreeTupleGetPostingN(itup, nposting - 1);
	}

	return &itup->t_tid;
}

/*
 *	Operator strategy numbers for B-tree have been moved to access/stratnum.h,
 *	because many places need to use them in ScanKeyInit() calls.
 *
 *	The strategy numbers are chosen so that we can commute them by
 *	subtraction, thus:
 */
#define BTCommuteStrategyNumber(strat)	(BTMaxStrategyNumber + 1 - (strat))

/*
 *	When a new operator class is declared, we require that the user
 *	supply us with an amproc procedure (BTORDER_PROC) for determining
 *	whether, for two keys a and b, a < b, a = b, or a > b.  This routine
 *	must return < 0, 0, > 0, respectively, in these three cases.
 *
 *	To facilitate accelerated sorting, an operator class may choose to
 *	offer a second procedure (BTSORTSUPPORT_PROC).  For full details, see
 *	src/include/utils/sortsupport.h.
 *
 *	To support window frames defined by "RANGE offset PRECEDING/FOLLOWING",
 *	an operator class may choose to offer a third amproc procedure
 *	(BTINRANGE_PROC), independently of whether it offers sortsupport.
 *	For full details, see doc/src/sgml/btree.sgml.
 *
 *	To facilitate B-Tree deduplication, an operator class may choose to
 *	offer a forth amproc procedure (BTEQUALIMAGE_PROC).  For full details,
 *	see doc/src/sgml/btree.sgml.
 */

#define BTORDER_PROC		1
#define BTSORTSUPPORT_PROC	2
#define BTINRANGE_PROC		3
#define BTEQUALIMAGE_PROC	4
#define BTOPTIONS_PROC		5
#define BTNProcs			5

/*
 *	We need to be able to tell the difference between read and write
 *	requests for pages, in order to do locking correctly.
 */

#define BT_READ			BUFFER_LOCK_SHARE
#define BT_WRITE		BUFFER_LOCK_EXCLUSIVE

/*
 * BTStackData -- As we descend a tree, we push the location of pivot
 * tuples whose downlink we are about to follow onto a private stack.  If
 * we split a leaf, we use this stack to walk back up the tree and insert
 * data into its parent page at the correct location.  We also have to
 * recursively insert into the grandparent page if and when the parent page
 * splits.  Our private stack can become stale due to concurrent page
 * splits and page deletions, but it should never give us an irredeemably
 * bad picture.
 */
typedef struct BTStackData
{
	BlockNumber bts_blkno;
	OffsetNumber bts_offset;
	struct BTStackData *bts_parent;
} BTStackData;

typedef BTStackData *BTStack;

/*
 * BTScanInsertData is the btree-private state needed to find an initial
 * position for an indexscan, or to insert new tuples -- an "insertion
 * scankey" (not to be confused with a search scankey).  It's used to descend
 * a B-Tree using _bt_search.
 *
 * heapkeyspace indicates if we expect all keys in the index to be physically
 * unique because heap TID is used as a tiebreaker attribute, and if index may
 * have truncated key attributes in pivot tuples.  This is actually a property
 * of the index relation itself (not an indexscan).  heapkeyspace indexes are
 * indexes whose version is >= version 4.  It's convenient to keep this close
 * by, rather than accessing the metapage repeatedly.
 *
 * allequalimage is set to indicate that deduplication is safe for the index.
 * This is also a property of the index relation rather than an indexscan.
 *
 * anynullkeys indicates if any of the keys had NULL value when scankey was
 * built from index tuple (note that already-truncated tuple key attributes
 * set NULL as a placeholder key value, which also affects value of
 * anynullkeys).  This is a convenience for unique index non-pivot tuple
 * insertion, which usually temporarily unsets scantid, but shouldn't iff
 * anynullkeys is true.  Value generally matches non-pivot tuple's HasNulls
 * bit, but may not when inserting into an INCLUDE index (tuple header value
 * is affected by the NULL-ness of both key and non-key attributes).
 *
 * When nextkey is false (the usual case), _bt_search and _bt_binsrch will
 * locate the first item >= scankey.  When nextkey is true, they will locate
 * the first item > scan key.
 *
 * pivotsearch is set to true by callers that want to re-find a leaf page
 * using a scankey built from a leaf page's high key.  Most callers set this
 * to false.
 *
 * scantid is the heap TID that is used as a final tiebreaker attribute.  It
 * is set to NULL when index scan doesn't need to find a position for a
 * specific physical tuple.  Must be set when inserting new tuples into
 * heapkeyspace indexes, since every tuple in the tree unambiguously belongs
 * in one exact position (it's never set with !heapkeyspace indexes, though).
 * Despite the representational difference, nbtree search code considers
 * scantid to be just another insertion scankey attribute.
 *
 * scankeys is an array of scan key entries for attributes that are compared
 * before scantid (user-visible attributes).  keysz is the size of the array.
 * During insertion, there must be a scan key for every attribute, but when
 * starting a regular index scan some can be omitted.  The array is used as a
 * flexible array member, though it's sized in a way that makes it possible to
 * use stack allocations.  See nbtree/README for full details.
 */
typedef struct BTScanInsertData
{
	bool		heapkeyspace;
	bool		allequalimage;
	bool		anynullkeys;
	bool		nextkey;
	bool		pivotsearch;
	ItemPointer scantid;		/* tiebreaker for scankeys */
	int			keysz;			/* Size of scankeys array */
	ScanKeyData scankeys[INDEX_MAX_KEYS];	/* Must appear last */
} BTScanInsertData;

typedef BTScanInsertData *BTScanInsert;

/*
 * BTInsertStateData is a working area used during insertion.
 *
 * This is filled in after descending the tree to the first leaf page the new
 * tuple might belong on.  Tracks the current position while performing
 * uniqueness check, before we have determined which exact page to insert
 * to.
 *
 * (This should be private to nbtinsert.c, but it's also used by
 * _bt_binsrch_insert)
 */
typedef struct BTInsertStateData
{
	IndexTuple	itup;			/* Item we're inserting */
	Size		itemsz;			/* Size of itup -- should be MAXALIGN()'d */
	BTScanInsert itup_key;		/* Insertion scankey */

	/* Buffer containing leaf page we're likely to insert itup on */
	Buffer		buf;

	/*
	 * Cache of bounds within the current buffer.  Only used for insertions
	 * where _bt_check_unique is called.  See _bt_binsrch_insert and
	 * _bt_findinsertloc for details.
	 */
	bool		bounds_valid;
	OffsetNumber low;
	OffsetNumber stricthigh;

	/*
	 * if _bt_binsrch_insert found the location inside existing posting list,
	 * save the position inside the list.  -1 sentinel value indicates overlap
	 * with an existing posting list tuple that has its LP_DEAD bit set.
	 */
	int			postingoff;
} BTInsertStateData;

typedef BTInsertStateData *BTInsertState;

/*
 * State used to representing an individual pending tuple during
 * deduplication.
 */
typedef struct BTDedupInterval
{
	OffsetNumber baseoff;
	uint16		nitems;
} BTDedupInterval;

/*
 * BTDedupStateData is a working area used during deduplication.
 *
 * The status info fields track the state of a whole-page deduplication pass.
 * State about the current pending posting list is also tracked.
 *
 * A pending posting list is comprised of a contiguous group of equal items
 * from the page, starting from page offset number 'baseoff'.  This is the
 * offset number of the "base" tuple for new posting list.  'nitems' is the
 * current total number of existing items from the page that will be merged to
 * make a new posting list tuple, including the base tuple item.  (Existing
 * items may themselves be posting list tuples, or regular non-pivot tuples.)
 *
 * The total size of the existing tuples to be freed when pending posting list
 * is processed gets tracked by 'phystupsize'.  This information allows
 * deduplication to calculate the space saving for each new posting list
 * tuple, and for the entire pass over the page as a whole.
 */
typedef struct BTDedupStateData
{
	/* Deduplication status info for entire pass over page */
	bool		deduplicate;	/* Still deduplicating page? */
	int			nmaxitems;		/* Number of max-sized tuples so far */
	Size		maxpostingsize; /* Limit on size of final tuple */

	/* Metadata about base tuple of current pending posting list */
	IndexTuple	base;			/* Use to form new posting list */
	OffsetNumber baseoff;		/* page offset of base */
	Size		basetupsize;	/* base size without original posting list */

	/* Other metadata about pending posting list */
	ItemPointer htids;			/* Heap TIDs in pending posting list */
	int			nhtids;			/* Number of heap TIDs in htids array */
	int			nitems;			/* Number of existing tuples/line pointers */
	Size		phystupsize;	/* Includes line pointer overhead */

	/*
	 * Array of tuples to go on new version of the page.  Contains one entry
	 * for each group of consecutive items.  Note that existing tuples that
	 * will not become posting list tuples do not appear in the array (they
	 * are implicitly unchanged by deduplication pass).
	 */
	int			nintervals;		/* current number of intervals in array */
	BTDedupInterval intervals[MaxIndexTuplesPerPage];
} BTDedupStateData;

typedef BTDedupStateData *BTDedupState;

/*
 * BTVacuumPostingData is state that represents how to VACUUM (or delete) a
 * posting list tuple when some (though not all) of its TIDs are to be
 * deleted.
 *
 * Convention is that itup field is the original posting list tuple on input,
 * and palloc()'d final tuple used to overwrite existing tuple on output.
 */
typedef struct BTVacuumPostingData
{
	/* Tuple that will be/was updated */
	IndexTuple	itup;
	OffsetNumber updatedoffset;

	/* State needed to describe final itup in WAL */
	uint16		ndeletedtids;
	uint16		deletetids[FLEXIBLE_ARRAY_MEMBER];
} BTVacuumPostingData;

typedef BTVacuumPostingData *BTVacuumPosting;

/*
 * BTScanOpaqueData is the btree-private state needed for an indexscan.
 * This consists of preprocessed scan keys (see _bt_preprocess_keys() for
 * details of the preprocessing), information about the current location
 * of the scan, and information about the marked location, if any.  (We use
 * BTScanPosData to represent the data needed for each of current and marked
 * locations.)	In addition we can remember some known-killed index entries
 * that must be marked before we can move off the current page.
 *
 * Index scans work a page at a time: we pin and read-lock the page, identify
 * all the matching items on the page and save them in BTScanPosData, then
 * release the read-lock while returning the items to the caller for
 * processing.  This approach minimizes lock/unlock traffic.  Note that we
 * keep the pin on the index page until the caller is done with all the items
 * (this is needed for VACUUM synchronization, see nbtree/README).  When we
 * are ready to step to the next page, if the caller has told us any of the
 * items were killed, we re-lock the page to mark them killed, then unlock.
 * Finally we drop the pin and step to the next page in the appropriate
 * direction.
 *
 * If we are doing an index-only scan, we save the entire IndexTuple for each
 * matched item, otherwise only its heap TID and offset.  The IndexTuples go
 * into a separate workspace array; each BTScanPosItem stores its tuple's
 * offset within that array.  Posting list tuples store a "base" tuple once,
 * allowing the same key to be returned for each TID in the posting list
 * tuple.
 */

typedef struct BTScanPosItem	/* what we remember about each match */
{
	ItemPointerData heapTid;	/* TID of referenced heap item */
	OffsetNumber indexOffset;	/* index item's location within page */
	LocationIndex tupleOffset;	/* IndexTuple's offset in workspace, if any */
} BTScanPosItem;

typedef struct BTScanPosData
{
	Buffer		buf;			/* if valid, the buffer is pinned */

	XLogRecPtr	lsn;			/* pos in the WAL stream when page was read */
	BlockNumber currPage;		/* page referenced by items array */
	BlockNumber nextPage;		/* page's right link when we scanned it */

	/*
	 * moreLeft and moreRight track whether we think there may be matching
	 * index entries to the left and right of the current page, respectively.
	 * We can clear the appropriate one of these flags when _bt_checkkeys()
	 * returns continuescan = false.
	 */
	bool		moreLeft;
	bool		moreRight;

	/*
	 * If we are doing an index-only scan, nextTupleOffset is the first free
	 * location in the associated tuple storage workspace.
	 */
	int			nextTupleOffset;

	/*
	 * The items array is always ordered in index order (ie, increasing
	 * indexoffset).  When scanning backwards it is convenient to fill the
	 * array back-to-front, so we start at the last slot and fill downwards.
	 * Hence we need both a first-valid-entry and a last-valid-entry counter.
	 * itemIndex is a cursor showing which entry was last returned to caller.
	 */
	int			firstItem;		/* first valid index in items[] */
	int			lastItem;		/* last valid index in items[] */
	int			itemIndex;		/* current index in items[] */

	BTScanPosItem items[MaxTIDsPerBTreePage];	/* MUST BE LAST */
} BTScanPosData;

typedef BTScanPosData *BTScanPos;

#define BTScanPosIsPinned(scanpos) \
( \
	AssertMacro(BlockNumberIsValid((scanpos).currPage) || \
				!BufferIsValid((scanpos).buf)), \
	BufferIsValid((scanpos).buf) \
)
#define BTScanPosUnpin(scanpos) \
	do { \
		ReleaseBuffer((scanpos).buf); \
		(scanpos).buf = InvalidBuffer; \
	} while (0)
#define BTScanPosUnpinIfPinned(scanpos) \
	do { \
		if (BTScanPosIsPinned(scanpos)) \
			BTScanPosUnpin(scanpos); \
	} while (0)

#define BTScanPosIsValid(scanpos) \
( \
	AssertMacro(BlockNumberIsValid((scanpos).currPage) || \
				!BufferIsValid((scanpos).buf)), \
	BlockNumberIsValid((scanpos).currPage) \
)
#define BTScanPosInvalidate(scanpos) \
	do { \
		(scanpos).currPage = InvalidBlockNumber; \
		(scanpos).nextPage = InvalidBlockNumber; \
		(scanpos).buf = InvalidBuffer; \
		(scanpos).lsn = InvalidXLogRecPtr; \
		(scanpos).nextTupleOffset = 0; \
	} while (0)

/* We need one of these for each equality-type SK_SEARCHARRAY scan key */
typedef struct BTArrayKeyInfo
{
	int			scan_key;		/* index of associated key in arrayKeyData */
	int			cur_elem;		/* index of current element in elem_values */
	int			mark_elem;		/* index of marked element in elem_values */
	int			num_elems;		/* number of elems in current array value */
	Datum	   *elem_values;	/* array of num_elems Datums */
} BTArrayKeyInfo;

typedef struct BTScanOpaqueData
{
	/* all fields (except arraysStarted) are set by _bt_preprocess_keys(): */
	bool		qual_ok;		/* false if qual can never be satisfied */
	bool		arraysStarted;	/* Started array keys, but have yet to "reach
								 * past the end" of all arrays? */
	int			numberOfKeys;	/* number of preprocessed scan keys */
	ScanKey		keyData;		/* array of preprocessed scan keys */

	/* workspace for SK_SEARCHARRAY support */
	ScanKey		arrayKeyData;	/* modified copy of scan->keyData */
	int			numArrayKeys;	/* number of equality-type array keys (-1 if
								 * there are any unsatisfiable array keys) */
	int			arrayKeyCount;	/* count indicating number of array scan keys
								 * processed */
	BTArrayKeyInfo *arrayKeys;	/* info about each equality-type array key */
	MemoryContext arrayContext; /* scan-lifespan context for array data */

	/* info about killed items if any (killedItems is NULL if never used) */
	int		   *killedItems;	/* currPos.items indexes of killed items */
	int			numKilled;		/* number of currently stored items */

	/*
	 * If we are doing an index-only scan, these are the tuple storage
	 * workspaces for the currPos and markPos respectively.  Each is of size
	 * BLCKSZ, so it can hold as much as a full page's worth of tuples.
	 */
	char	   *currTuples;		/* tuple storage for currPos */
	char	   *markTuples;		/* tuple storage for markPos */

	/*
	 * If the marked position is on the same page as current position, we
	 * don't use markPos, but just keep the marked itemIndex in markItemIndex
	 * (all the rest of currPos is valid for the mark position). Hence, to
	 * determine if there is a mark, first look at markItemIndex, then at
	 * markPos.
	 */
	int			markItemIndex;	/* itemIndex, or -1 if not valid */

	/* keep these last in struct for efficiency */
	BTScanPosData currPos;		/* current position data */
	BTScanPosData markPos;		/* marked position, if any */
} BTScanOpaqueData;

typedef BTScanOpaqueData *BTScanOpaque;

/*
 * We use some private sk_flags bits in preprocessed scan keys.  We're allowed
 * to use bits 16-31 (see skey.h).  The uppermost bits are copied from the
 * index's indoption[] array entry for the index attribute.
 */
#define SK_BT_REQFWD	0x00010000	/* required to continue forward scan */
#define SK_BT_REQBKWD	0x00020000	/* required to continue backward scan */
#define SK_BT_INDOPTION_SHIFT  24	/* must clear the above bits */
#define SK_BT_DESC			(INDOPTION_DESC << SK_BT_INDOPTION_SHIFT)
#define SK_BT_NULLS_FIRST	(INDOPTION_NULLS_FIRST << SK_BT_INDOPTION_SHIFT)

typedef struct BTOptions
{
	int32		varlena_header_;	/* varlena header (do not touch directly!) */
	int			fillfactor;		/* page fill factor in percent (0..100) */
	float8		vacuum_cleanup_index_scale_factor;	/* deprecated */
	bool		deduplicate_items;	/* Try to deduplicate items? */
} BTOptions;

#define BTGetFillFactor(relation) \
	(AssertMacro(relation->rd_rel->relkind == RELKIND_INDEX && \
				 relation->rd_rel->relam == BTREE_AM_OID), \
	 (relation)->rd_options ? \
	 ((BTOptions *) (relation)->rd_options)->fillfactor : \
	 BTREE_DEFAULT_FILLFACTOR)
#define BTGetTargetPageFreeSpace(relation) \
	(BLCKSZ * (100 - BTGetFillFactor(relation)) / 100)
#define BTGetDeduplicateItems(relation) \
	(AssertMacro(relation->rd_rel->relkind == RELKIND_INDEX && \
				 relation->rd_rel->relam == BTREE_AM_OID), \
	((relation)->rd_options ? \
	 ((BTOptions *) (relation)->rd_options)->deduplicate_items : true))

/*
 * Constant definition for progress reporting.  Phase numbers must match
 * btbuildphasename.
 */
/* PROGRESS_CREATEIDX_SUBPHASE_INITIALIZE is 1 (see progress.h) */
#define PROGRESS_BTREE_PHASE_INDEXBUILD_TABLESCAN		2
#define PROGRESS_BTREE_PHASE_PERFORMSORT_1				3
#define PROGRESS_BTREE_PHASE_PERFORMSORT_2				4
#define PROGRESS_BTREE_PHASE_LEAF_LOAD					5

/*
 * external entry points for btree, in nbtree.c
 */
extern void btbuildempty(Relation index);
extern bool btinsert(Relation rel, Datum *values, bool *isnull,
					 ItemPointer ht_ctid, Relation heapRel,
					 IndexUniqueCheck checkUnique,
					 bool indexUnchanged,
					 struct IndexInfo *indexInfo);
extern IndexScanDesc btbeginscan(Relation rel, int nkeys, int norderbys);
extern Size btestimateparallelscan(void);
extern void btinitparallelscan(void *target);
extern bool btgettuple(IndexScanDesc scan, ScanDirection dir);
extern int64 btgetbitmap(IndexScanDesc scan, TIDBitmap *tbm);
extern void btrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
					 ScanKey orderbys, int norderbys);
extern void btparallelrescan(IndexScanDesc scan);
extern void btendscan(IndexScanDesc scan);
extern void btmarkpos(IndexScanDesc scan);
extern void btrestrpos(IndexScanDesc scan);
extern IndexBulkDeleteResult *btbulkdelete(IndexVacuumInfo *info,
										   IndexBulkDeleteResult *stats,
										   IndexBulkDeleteCallback callback,
										   void *callback_state);
extern IndexBulkDeleteResult *btvacuumcleanup(IndexVacuumInfo *info,
											  IndexBulkDeleteResult *stats);
extern bool btcanreturn(Relation index, int attno);

/*
 * prototypes for internal functions in nbtree.c
 */
extern bool _bt_parallel_seize(IndexScanDesc scan, BlockNumber *pageno);
extern void _bt_parallel_release(IndexScanDesc scan, BlockNumber scan_page);
extern void _bt_parallel_done(IndexScanDesc scan);
extern void _bt_parallel_advance_array_keys(IndexScanDesc scan);

/*
 * prototypes for functions in nbtdedup.c
 */
extern void _bt_dedup_pass(Relation rel, Buffer buf, IndexTuple newitem,
						   Size newitemsz, bool bottomupdedup);
extern bool _bt_bottomupdel_pass(Relation rel, Buffer buf, Relation heapRel,
								 Size newitemsz);
extern void _bt_dedup_start_pending(BTDedupState state, IndexTuple base,
									OffsetNumber baseoff);
extern bool _bt_dedup_save_htid(BTDedupState state, IndexTuple itup);
extern Size _bt_dedup_finish_pending(Page newpage, BTDedupState state);
extern IndexTuple _bt_form_posting(IndexTuple base, ItemPointer htids,
								   int nhtids);
extern void _bt_update_posting(BTVacuumPosting vacposting);
extern IndexTuple _bt_swap_posting(IndexTuple newitem, IndexTuple oposting,
								   int postingoff);

/*
 * prototypes for functions in nbtinsert.c
 */
extern bool _bt_doinsert(Relation rel, IndexTuple itup,
						 IndexUniqueCheck checkUnique, bool indexUnchanged,
						 Relation heapRel);
extern void _bt_finish_split(Relation rel, Relation heaprel, Buffer lbuf,
							 BTStack stack);
extern Buffer _bt_getstackbuf(Relation rel, Relation heaprel, BTStack stack,
							  BlockNumber child);

/*
 * prototypes for functions in nbtsplitloc.c
 */
extern OffsetNumber _bt_findsplitloc(Relation rel, Page origpage,
									 OffsetNumber newitemoff, Size newitemsz, IndexTuple newitem,
									 bool *newitemonleft);

/*
 * prototypes for functions in nbtpage.c
 */
extern void _bt_initmetapage(Page page, BlockNumber rootbknum, uint32 level,
							 bool allequalimage);
extern bool _bt_vacuum_needs_cleanup(Relation rel);
extern void _bt_set_cleanup_info(Relation rel, BlockNumber num_delpages);
extern void _bt_upgrademetapage(Page page);
extern Buffer _bt_getroot(Relation rel, Relation heaprel, int access);
extern Buffer _bt_gettrueroot(Relation rel);
extern int	_bt_getrootheight(Relation rel);
extern void _bt_metaversion(Relation rel, bool *heapkeyspace,
							bool *allequalimage);
extern void _bt_checkpage(Relation rel, Buffer buf);
extern Buffer _bt_getbuf(Relation rel, BlockNumber blkno, int access);
extern Buffer _bt_allocbuf(Relation rel, Relation heaprel);
extern Buffer _bt_relandgetbuf(Relation rel, Buffer obuf,
							   BlockNumber blkno, int access);
extern void _bt_relbuf(Relation rel, Buffer buf);
extern void _bt_lockbuf(Relation rel, Buffer buf, int access);
extern void _bt_unlockbuf(Relation rel, Buffer buf);
extern bool _bt_conditionallockbuf(Relation rel, Buffer buf);
extern void _bt_upgradelockbufcleanup(Relation rel, Buffer buf);
extern void _bt_pageinit(Page page, Size size);
extern void _bt_delitems_vacuum(Relation rel, Buffer buf,
								OffsetNumber *deletable, int ndeletable,
								BTVacuumPosting *updatable, int nupdatable);
extern void _bt_delitems_delete_check(Relation rel, Buffer buf,
									  Relation heapRel,
									  TM_IndexDeleteOp *delstate);
extern void _bt_pagedel(Relation rel, Buffer leafbuf, BTVacState *vstate);
extern void _bt_pendingfsm_init(Relation rel, BTVacState *vstate,
								bool cleanuponly);
extern void _bt_pendingfsm_finalize(Relation rel, BTVacState *vstate);

/*
 * prototypes for functions in nbtsearch.c
 */
extern BTStack _bt_search(Relation rel, Relation heaprel, BTScanInsert key,
						  Buffer *bufP, int access, Snapshot snapshot);
extern Buffer _bt_moveright(Relation rel, Relation heaprel, BTScanInsert key,
							Buffer buf, bool forupdate, BTStack stack,
							int access, Snapshot snapshot);
extern OffsetNumber _bt_binsrch_insert(Relation rel, BTInsertState insertstate);
extern int32 _bt_compare(Relation rel, BTScanInsert key, Page page, OffsetNumber offnum);
extern bool _bt_first(IndexScanDesc scan, ScanDirection dir);
extern bool _bt_next(IndexScanDesc scan, ScanDirection dir);
extern Buffer _bt_get_endpoint(Relation rel, uint32 level, bool rightmost,
							   Snapshot snapshot);

/*
 * prototypes for functions in nbtutils.c
 */
extern BTScanInsert _bt_mkscankey(Relation rel, IndexTuple itup);
extern void _bt_freestack(BTStack stack);
extern void _bt_preprocess_array_keys(IndexScanDesc scan);
extern void _bt_start_array_keys(IndexScanDesc scan, ScanDirection dir);
extern bool _bt_advance_array_keys(IndexScanDesc scan, ScanDirection dir);
extern void _bt_mark_array_keys(IndexScanDesc scan);
extern void _bt_restore_array_keys(IndexScanDesc scan);
extern void _bt_preprocess_keys(IndexScanDesc scan);
extern bool _bt_checkkeys(IndexScanDesc scan, IndexTuple tuple,
						  int tupnatts, ScanDirection dir, bool *continuescan);
extern void _bt_killitems(IndexScanDesc scan);
extern BTCycleId _bt_vacuum_cycleid(Relation rel);
extern BTCycleId _bt_start_vacuum(Relation rel);
extern void _bt_end_vacuum(Relation rel);
extern void _bt_end_vacuum_callback(int code, Datum arg);
extern Size BTreeShmemSize(void);
extern void BTreeShmemInit(void);
extern bytea *btoptions(Datum reloptions, bool validate);
extern bool btproperty(Oid index_oid, int attno,
					   IndexAMProperty prop, const char *propname,
					   bool *res, bool *isnull);
extern char *btbuildphasename(int64 phasenum);
extern IndexTuple _bt_truncate(Relation rel, IndexTuple lastleft,
							   IndexTuple firstright, BTScanInsert itup_key);
extern int	_bt_keep_natts_fast(Relation rel, IndexTuple lastleft,
								IndexTuple firstright);
extern bool _bt_check_natts(Relation rel, bool heapkeyspace, Page page,
							OffsetNumber offnum);
extern void _bt_check_third_page(Relation rel, Relation heap,
								 bool needheaptidspace, Page page, IndexTuple newtup);
extern bool _bt_allequalimage(Relation rel, bool debugmessage);

/*
 * prototypes for functions in nbtvalidate.c
 */
extern bool btvalidate(Oid opclassoid);
extern void btadjustmembers(Oid opfamilyoid,
							Oid opclassoid,
							List *operators,
							List *functions);

/*
 * prototypes for functions in nbtsort.c
 */
extern IndexBuildResult *btbuild(Relation heap, Relation index,
								 struct IndexInfo *indexInfo);
extern void _bt_parallel_build_main(dsm_segment *seg, shm_toc *toc);

#endif							/* NBTREE_H */

/*-------------------------------------------------------------------------
 *
 * tuptable.h
 *	  tuple table support stuff / 元组表支持相关
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/tuptable.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TUPTABLE_H
#define TUPTABLE_H

#include "access/htup.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/tupdesc.h"
#include "storage/buf.h"

/*----------
 * 执行器将元组存储在 "tuple table" 中，即由独立的 TupleTableSlot 组成的 List。
 *
 * 存在多种不同类型的元组表槽，每种槽可存储不同类型的元组。可在不修改核心代码
 * 的情况下添加新的槽类型。槽的类型由传给槽创建例程的 TupleTableSlotOps* 决定。
 * 内置槽类型有：
 *
 * 1. 磁盘缓冲区页中的物理元组 (TTSOpsBufferHeapTuple)
 * 2. 在 palloc 分配内存中构造的物理元组 (TTSOpsHeapTuple)
 * 3. 在 palloc 分配内存中构造的 "minimal" 物理元组 (TTSOpsMinimalTuple)
 * 4. 由 Datum/isnull 数组组成的 "virtual" 元组 (TTSOpsVirtual)
 *
 *
 * 前两种情况类似，都处理 "materialized" 元组，但资源管理不同。对于磁盘页中的
 * 元组，在 TupleTableSlot 仍引用该元组期间需保持对缓冲区的 pin；对于 palloc 分配
 * 的元组，通常希望在 TupleTableSlot 的引用被释放时对元组执行 pfree。
 *
 * "minimal" 元组的处理方式与 palloc 分配的普通元组类似。目前 minimal 元组从不
 * 存储在缓冲区中，因此没有与情况 1 对应的并行机制。注意 minimal 元组没有
 * "system columns"。
 *
 * "virtual" 元组是一种优化，用于在计划节点嵌套中最小化物理数据拷贝。在物化之前，
 * 槽中按引用传递的 Datum 指向的存储并不直接关联于 TupleTableSlot；通常它们指向
 * 较低层计划节点输出 TupleTableSlot 中存储的元组的一部分，或指向在计划节点
 * 逐元组 econtext 中构造的函数结果。生成该元组的计划节点负责确保这些资源在
 * virtual 元组需要有效或被物化期间不被释放。同样注意 virtual 元组没有任何
 * "system columns"。
 *
 * TupleTableSlot 的 Datum/isnull 数组具有双重用途。对于 virtual 槽，它们是权威
 * 数据。对于其他内置槽，数组包含从元组中提取的数据。（在此状态下，任何按引用
 * 传递的 Datum 都指向物理元组内部。）提取的信息是 "lazy" 构建的，即仅在需要时
 * 才构建，以避免反复从物理元组提取数据。
 *
 * TupleTableSlot 也可处于 "empty" 状态，由 tts_flags 中的 TTS_FLAG_EMPTY 标志
 * 表示，不持有有效数据。这是尚未分配元组描述符的新建槽的唯一有效状态。在此
 * 状态下，tts_flags 中不应设置 TTS_FLAG_SHOULDFREE，且 tts_nvalid 应设为 0。
 *
 * TupleTableSlot 代码仅引用 tupleDescriptor，不拷贝它。ExecSetSlotDescriptor()
 * 的调用方须提供一个与槽同生命周期的描述符。（通常槽和描述符都在 per-query 内存
 * 中，并在查询结束时通过内存上下文释放而释放；因此不值得提供额外机制。但若提供
 * 了带引用计数的 tupdesc，槽会增加其引用计数。）
 *
 * 当 tts_flags 中设置 TTS_FLAG_SHOULDFREE 时，物理元组由槽 "拥有"，应在槽对
 * 元组的引用被释放时释放该元组。
 *
 * tts_values/tts_isnull 在创建槽时（提供描述符时）或向槽分配描述符时分配；
 * 其长度等于描述符的 natts。
 *
 * TTS_FLAG_SLOW 标志是 slot_deform_heap_tuple 的保存状态，其他代码不应修改。
 *
 * The executor stores tuples in a "tuple table" which is a List of
 * independent TupleTableSlots.
 *
 * There's various different types of tuple table slots, each being able to
 * store different types of tuples. Additional types of slots can be added
 * without modifying core code. The type of a slot is determined by the
 * TupleTableSlotOps* passed to the slot creation routine. The builtin types
 * of slots are
 *
 * 1. physical tuple in a disk buffer page (TTSOpsBufferHeapTuple)
 * 2. physical tuple constructed in palloc'ed memory (TTSOpsHeapTuple)
 * 3. "minimal" physical tuple constructed in palloc'ed memory
 *    (TTSOpsMinimalTuple)
 * 4. "virtual" tuple consisting of Datum/isnull arrays (TTSOpsVirtual)
 *
 *
 * The first two cases are similar in that they both deal with "materialized"
 * tuples, but resource management is different.  For a tuple in a disk page
 * we need to hold a pin on the buffer until the TupleTableSlot's reference
 * to the tuple is dropped; while for a palloc'd tuple we usually want the
 * tuple pfree'd when the TupleTableSlot's reference is dropped.
 *
 * A "minimal" tuple is handled similarly to a palloc'd regular tuple.
 * At present, minimal tuples never are stored in buffers, so there is no
 * parallel to case 1.  Note that a minimal tuple has no "system columns".
 *
 * A "virtual" tuple is an optimization used to minimize physical data copying
 * in a nest of plan nodes.  Until materialized pass-by-reference Datums in
 * the slot point to storage that is not directly associated with the
 * TupleTableSlot; generally they will point to part of a tuple stored in a
 * lower plan node's output TupleTableSlot, or to a function result
 * constructed in a plan node's per-tuple econtext.  It is the responsibility
 * of the generating plan node to be sure these resources are not released for
 * as long as the virtual tuple needs to be valid or is materialized.  Note
 * also that a virtual tuple does not have any "system columns".
 *
 * The Datum/isnull arrays of a TupleTableSlot serve double duty.  For virtual
 * slots they are the authoritative data.  For the other builtin slots,
 * the arrays contain data extracted from the tuple.  (In this state, any
 * pass-by-reference Datums point into the physical tuple.)  The extracted
 * information is built "lazily", ie, only as needed.  This serves to avoid
 * repeated extraction of data from the physical tuple.
 *
 * A TupleTableSlot can also be "empty", indicated by flag TTS_FLAG_EMPTY set
 * in tts_flags, holding no valid data.  This is the only valid state for a
 * freshly-created slot that has not yet had a tuple descriptor assigned to
 * it.  In this state, TTS_FLAG_SHOULDFREE should not be set in tts_flags and
 * tts_nvalid should be set to zero.
 *
 * The tupleDescriptor is simply referenced, not copied, by the TupleTableSlot
 * code.  The caller of ExecSetSlotDescriptor() is responsible for providing
 * a descriptor that will live as long as the slot does.  (Typically, both
 * slots and descriptors are in per-query memory and are freed by memory
 * context deallocation at query end; so it's not worth providing any extra
 * mechanism to do more.  However, the slot will increment the tupdesc
 * reference count if a reference-counted tupdesc is supplied.)
 *
 * When TTS_FLAG_SHOULDFREE is set in tts_flags, the physical tuple is "owned"
 * by the slot and should be freed when the slot's reference to the tuple is
 * dropped.
 *
 * tts_values/tts_isnull are allocated either when the slot is created (when
 * the descriptor is provided), or when a descriptor is assigned to the slot;
 * they are of length equal to the descriptor's natts.
 *
 * The TTS_FLAG_SLOW flag is saved state for
 * slot_deform_heap_tuple, and should not be touched by any other code.
 *----------
 */

/* true = slot is empty / 为真表示槽为空 */
#define			TTS_FLAG_EMPTY			(1 << 1)
#define TTS_EMPTY(slot)	(((slot)->tts_flags & TTS_FLAG_EMPTY) != 0)

/* should pfree tuple "owned" by the slot? / 是否应对槽"拥有"的元组执行 pfree？ */
#define			TTS_FLAG_SHOULDFREE		(1 << 2)
#define TTS_SHOULDFREE(slot) (((slot)->tts_flags & TTS_FLAG_SHOULDFREE) != 0)

/* saved state for slot_deform_heap_tuple / slot_deform_heap_tuple 的保存状态 */
#define			TTS_FLAG_SLOW		(1 << 3)
#define TTS_SLOW(slot) (((slot)->tts_flags & TTS_FLAG_SLOW) != 0)

/* fixed tuple descriptor / 固定的元组描述符 */
#define			TTS_FLAG_FIXED		(1 << 4)
#define TTS_FIXED(slot) (((slot)->tts_flags & TTS_FLAG_FIXED) != 0)

struct TupleTableSlotOps;
typedef struct TupleTableSlotOps TupleTableSlotOps;

/* base tuple table slot type / 基础元组表槽类型 */
typedef struct TupleTableSlot
{
	NodeTag		type;
#define FIELDNO_TUPLETABLESLOT_FLAGS 1
	uint16		tts_flags;		/* Boolean states / 布尔状态位 */
#define FIELDNO_TUPLETABLESLOT_NVALID 2
	AttrNumber	tts_nvalid;		/* # of valid values in tts_values / tts_values 中有效值的数量 */
	const TupleTableSlotOps *const tts_ops; /* implementation of slot / 槽的实现 */
#define FIELDNO_TUPLETABLESLOT_TUPLEDESCRIPTOR 4
	TupleDesc	tts_tupleDescriptor;	/* slot's tuple descriptor / 槽的元组描述符 */
#define FIELDNO_TUPLETABLESLOT_VALUES 5
	Datum	   *tts_values;		/* current per-attribute values / 当前各属性的值 */
#define FIELDNO_TUPLETABLESLOT_ISNULL 6
	bool	   *tts_isnull;		/* current per-attribute isnull flags / 当前各属性的 isnull 标志 */
	MemoryContext tts_mcxt;		/* slot itself is in this context / 槽本身所在的内存上下文 */
	ItemPointerData tts_tid;	/* stored tuple's tid / 所存元组的 tid */
	Oid			tts_tableOid;	/* table oid of tuple / 元组所在表的 oid */
} TupleTableSlot;

/* routines for a TupleTableSlot implementation / TupleTableSlot 实现的例程 */
struct TupleTableSlotOps
{
	/* Minimum size of the slot / 槽的最小大小 */
	size_t		base_slot_size;

	/* Initialization. / 初始化。 */
	void		(*init) (TupleTableSlot *slot);

	/* Destruction. / 销毁。 */
	void		(*release) (TupleTableSlot *slot);

	/*
	 * 清除槽的内容。仅清除内容，不清除元组描述符。实现此回调时通常应释放
	 * 槽中所含元组占用的内存。
	 *
	 * Clear the contents of the slot. Only the contents are expected to be
	 * cleared and not the tuple descriptor. Typically an implementation of
	 * this callback should free the memory allocated for the tuple contained
	 * in the slot.
	 */
	void		(*clear) (TupleTableSlot *slot);

	/*
	 * 用槽中所含元组的值填充 tts_values 和 tts_isnull 数组的前 natts 项。
	 * 调用时 natts 可能大于元组中可用属性数，此时应将 tts_nvalid 设为实际
	 * 返回的列数。
	 *
	 * Fill up first natts entries of tts_values and tts_isnull arrays with
	 * values from the tuple contained in the slot. The function may be called
	 * with natts more than the number of attributes available in the tuple,
	 * in which case it should set tts_nvalid to the number of returned
	 * columns.
	 */
	void		(*getsomeattrs) (TupleTableSlot *slot, int natts);

	/*
	 * 将给定系统属性的值作为 datum 返回；若非 NULL 则将 isnull 设为 false。
	 * 若槽类型不支持系统属性则抛出错误。
	 *
	 * Returns value of the given system attribute as a datum and sets isnull
	 * to false, if it's not NULL. Throws an error if the slot type does not
	 * support system attributes.
	 */
	Datum		(*getsysattr) (TupleTableSlot *slot, int attnum, bool *isnull);

	/*
	 * 使槽的内容仅依赖槽本身，而不依赖底层资源（如其他内存上下文、缓冲区等）。
	 *
	 * Make the contents of the slot solely depend on the slot, and not on
	 * underlying resources (like another memory context, buffers, etc).
	 */
	void		(*materialize) (TupleTableSlot *slot);

	/*
	 * 将源槽的内容拷贝到目标槽自身的上下文中。通过目标槽的回调调用。
	 *
	 * Copy the contents of the source slot into the destination slot's own
	 * context. Invoked using callback of the destination slot.
	 */
	void		(*copyslot) (TupleTableSlot *dstslot, TupleTableSlot *srcslot);

	/*
	 * 返回由槽"拥有"的 heap tuple。释放 heap tuple 占用的内存是槽的责任。
	 * 若槽不能"拥有" heap tuple，则不应实现此回调，应将其设为 NULL。
	 *
	 * Return a heap tuple "owned" by the slot. It is slot's responsibility to
	 * free the memory consumed by the heap tuple. If the slot can not "own" a
	 * heap tuple, it should not implement this callback and should set it as
	 * NULL.
	 */
	HeapTuple	(*get_heap_tuple) (TupleTableSlot *slot);

	/*
	 * 返回由槽"拥有"的 minimal tuple。释放 minimal tuple 占用的内存是槽的责任。
	 * 若槽不能"拥有" minimal tuple，则不应实现此回调，应将其设为 NULL。
	 *
	 * Return a minimal tuple "owned" by the slot. It is slot's responsibility
	 * to free the memory consumed by the minimal tuple. If the slot can not
	 * "own" a minimal tuple, it should not implement this callback and should
	 * set it as NULL.
	 */
	MinimalTuple (*get_minimal_tuple) (TupleTableSlot *slot);

	/*
	 * 返回表示槽内容的 heap tuple 副本。副本须在现行内存上下文中 palloc 分配。
	 * 槽本身应不受影响。副本中不应包含有意义的 "system columns"。副本不由槽
	 * "拥有"，即调用方负责释放其占用的内存。
	 *
	 * Return a copy of heap tuple representing the contents of the slot. The
	 * copy needs to be palloc'd in the current memory context. The slot
	 * itself is expected to remain unaffected. It is *not* expected to have
	 * meaningful "system columns" in the copy. The copy is not be "owned" by
	 * the slot i.e. the caller has to take responsibility to free memory
	 * consumed by the slot.
	 */
	HeapTuple	(*copy_heap_tuple) (TupleTableSlot *slot);

	/*
	 * 返回表示槽内容的 minimal tuple 副本。副本须在现行内存上下文中 palloc 分配。
	 * 槽本身应不受影响。副本中不应包含有意义的 "system columns"。副本不由槽
	 * "拥有"，即调用方负责释放其占用的内存。
	 *
	 * Return a copy of minimal tuple representing the contents of the slot.
	 * The copy needs to be palloc'd in the current memory context. The slot
	 * itself is expected to remain unaffected. It is *not* expected to have
	 * meaningful "system columns" in the copy. The copy is not be "owned" by
	 * the slot i.e. the caller has to take responsibility to free memory
	 * consumed by the slot.
	 */
	MinimalTuple (*copy_minimal_tuple) (TupleTableSlot *slot);
};

/*
 * 各类 TupleTableSlotOps 的预定义实例。也用于标识给定槽的类型。
 *
 * Predefined TupleTableSlotOps for various types of TupleTableSlotOps. The
 * same are used to identify the type of a given slot.
 */
extern PGDLLIMPORT const TupleTableSlotOps TTSOpsVirtual;
extern PGDLLIMPORT const TupleTableSlotOps TTSOpsHeapTuple;
extern PGDLLIMPORT const TupleTableSlotOps TTSOpsMinimalTuple;
extern PGDLLIMPORT const TupleTableSlotOps TTSOpsBufferHeapTuple;

#define TTS_IS_VIRTUAL(slot) ((slot)->tts_ops == &TTSOpsVirtual)
#define TTS_IS_HEAPTUPLE(slot) ((slot)->tts_ops == &TTSOpsHeapTuple)
#define TTS_IS_MINIMALTUPLE(slot) ((slot)->tts_ops == &TTSOpsMinimalTuple)
#define TTS_IS_BUFFERTUPLE(slot) ((slot)->tts_ops == &TTSOpsBufferHeapTuple)


/*
 * Tuple table slot implementations. / 元组表槽实现。
 */

typedef struct VirtualTupleTableSlot
{
	pg_node_attr(abstract)

	TupleTableSlot base;

	char	   *data;			/* data for materialized slots / 物化槽的数据 */
} VirtualTupleTableSlot;

typedef struct HeapTupleTableSlot
{
	pg_node_attr(abstract)

	TupleTableSlot base;

#define FIELDNO_HEAPTUPLETABLESLOT_TUPLE 1
	HeapTuple	tuple;			/* physical tuple / 物理元组 */
#define FIELDNO_HEAPTUPLETABLESLOT_OFF 2
	uint32		off;			/* saved state for slot_deform_heap_tuple / slot_deform_heap_tuple 的保存状态 */
	HeapTupleData tupdata;		/* optional workspace for storing tuple / 存储元组的可选工作区 */
} HeapTupleTableSlot;

/* heap tuple residing in a buffer / 位于缓冲区中的 heap tuple */
typedef struct BufferHeapTupleTableSlot
{
	pg_node_attr(abstract)

	HeapTupleTableSlot base;

	/*
	 * 若 buffer 不是 InvalidBuffer，则槽持有对该缓冲区页的 pin；在槽释放对该
	 * 缓冲区的引用时应 drop pin。（此类情况下不应在 tts_flags 中设置
	 * TTS_FLAG_SHOULDFREE，因为 base.tuple 应指向缓冲区内。）
	 *
	 * If buffer is not InvalidBuffer, then the slot is holding a pin on the
	 * indicated buffer page; drop the pin when we release the slot's
	 * reference to that buffer.  (TTS_FLAG_SHOULDFREE should not be set in
	 * such a case, since presumably base.tuple is pointing into the buffer.)
	 */
	Buffer		buffer;			/* tuple's buffer, or InvalidBuffer / 元组的缓冲区，或 InvalidBuffer */
} BufferHeapTupleTableSlot;

typedef struct MinimalTupleTableSlot
{
	pg_node_attr(abstract)

	TupleTableSlot base;

	/*
	 * 在 minimal 槽中，tuple 指向 minhdr，且该结构体的字段已正确设置以便访问
	 * minimal tuple；特别是 minhdr.t_data 指向 mintuple 之前 MINIMAL_TUPLE_OFFSET
	 * 字节处。这使列提取可将此情况与普通物理元组同等处理。
	 *
	 * In a minimal slot tuple points at minhdr and the fields of that struct
	 * are set correctly for access to the minimal tuple; in particular,
	 * minhdr.t_data points MINIMAL_TUPLE_OFFSET bytes before mintuple.  This
	 * allows column extraction to treat the case identically to regular
	 * physical tuples.
	 */
#define FIELDNO_MINIMALTUPLETABLESLOT_TUPLE 1
	HeapTuple	tuple;			/* tuple wrapper / 元组包装器 */
	MinimalTuple mintuple;		/* minimal tuple, or NULL if none / minimal tuple，无则为 NULL */
	HeapTupleData minhdr;		/* workspace for minimal-tuple-only case / 仅 minimal tuple 时的工作区 */
#define FIELDNO_MINIMALTUPLETABLESLOT_OFF 4
	uint32		off;			/* saved state for slot_deform_heap_tuple / slot_deform_heap_tuple 的保存状态 */
} MinimalTupleTableSlot;

/*
 * TupIsNull -- is a TupleTableSlot empty? / TupleTableSlot 是否为空？
 */
#define TupIsNull(slot) \
	((slot) == NULL || TTS_EMPTY(slot))

/* in executor/execTuples.c / 定义于 executor/execTuples.c */
extern TupleTableSlot *MakeTupleTableSlot(TupleDesc tupleDesc,
										  const TupleTableSlotOps *tts_ops);
extern TupleTableSlot *ExecAllocTableSlot(List **tupleTable, TupleDesc desc,
										  const TupleTableSlotOps *tts_ops);
extern void ExecResetTupleTable(List *tupleTable, bool shouldFree);
extern TupleTableSlot *MakeSingleTupleTableSlot(TupleDesc tupdesc,
												const TupleTableSlotOps *tts_ops);
extern void ExecDropSingleTupleTableSlot(TupleTableSlot *slot);
extern void ExecSetSlotDescriptor(TupleTableSlot *slot, TupleDesc tupdesc);
extern TupleTableSlot *ExecStoreHeapTuple(HeapTuple tuple,
										  TupleTableSlot *slot,
										  bool shouldFree);
extern void ExecForceStoreHeapTuple(HeapTuple tuple,
									TupleTableSlot *slot,
									bool shouldFree);
extern TupleTableSlot *ExecStoreBufferHeapTuple(HeapTuple tuple,
												TupleTableSlot *slot,
												Buffer buffer);
extern TupleTableSlot *ExecStorePinnedBufferHeapTuple(HeapTuple tuple,
													  TupleTableSlot *slot,
													  Buffer buffer);
extern TupleTableSlot *ExecStoreMinimalTuple(MinimalTuple mtup,
											 TupleTableSlot *slot,
											 bool shouldFree);
extern void ExecForceStoreMinimalTuple(MinimalTuple mtup, TupleTableSlot *slot,
									   bool shouldFree);
extern TupleTableSlot *ExecStoreVirtualTuple(TupleTableSlot *slot);
extern TupleTableSlot *ExecStoreAllNullTuple(TupleTableSlot *slot);
extern void ExecStoreHeapTupleDatum(Datum data, TupleTableSlot *slot);
extern HeapTuple ExecFetchSlotHeapTuple(TupleTableSlot *slot, bool materialize, bool *shouldFree);
extern MinimalTuple ExecFetchSlotMinimalTuple(TupleTableSlot *slot,
											  bool *shouldFree);
extern Datum ExecFetchSlotHeapTupleDatum(TupleTableSlot *slot);
extern void slot_getmissingattrs(TupleTableSlot *slot, int startAttNum,
								 int lastAttNum);
extern void slot_getsomeattrs_int(TupleTableSlot *slot, int attnum);


#ifndef FRONTEND

/*
 * 此函数强制槽的 Datum/isnull 数组中至少到 attnum 项的条目有效。
 *
 * This function forces the entries of the slot's Datum/isnull arrays to be
 * valid at least up through the attnum'th entry.
 */
static inline void
slot_getsomeattrs(TupleTableSlot *slot, int attnum)
{
	if (slot->tts_nvalid < attnum)
		slot_getsomeattrs_int(slot, attnum);
}

/*
 * slot_getallattrs
 *		此函数强制槽的 Datum/isnull 数组中所有条目有效。调用方可直接从这些数组
 *		提取数据，而不必使用 slot_getattr。
 *
 *		This function forces all the entries of the slot's Datum/isnull
 *		arrays to be valid.  The caller may then extract data directly
 *		from those arrays instead of using slot_getattr.
 */
static inline void
slot_getallattrs(TupleTableSlot *slot)
{
	slot_getsomeattrs(slot, slot->tts_tupleDescriptor->natts);
}


/*
 * slot_attisnull
 *
 * 检测槽的某属性是否为 null，而不实际取回该属性。
 *
 * Detect whether an attribute of the slot is null, without actually fetching
 * it.
 */
static inline bool
slot_attisnull(TupleTableSlot *slot, int attnum)
{
	Assert(attnum > 0);

	if (attnum > slot->tts_nvalid)
		slot_getsomeattrs(slot, attnum);

	return slot->tts_isnull[attnum - 1];
}

/*
 * slot_getattr - fetch one attribute of the slot's contents.
 *              - 取回槽内容的一个属性。
 */
static inline Datum
slot_getattr(TupleTableSlot *slot, int attnum,
			 bool *isnull)
{
	Assert(attnum > 0);

	if (attnum > slot->tts_nvalid)
		slot_getsomeattrs(slot, attnum);

	*isnull = slot->tts_isnull[attnum - 1];

	return slot->tts_values[attnum - 1];
}

/*
 * slot_getsysattr - fetch a system attribute of the slot's current tuple.
 *                 - 取回槽当前元组的一个系统属性。
 *
 *  若槽类型不含系统属性，此函数将抛出错误。因此调用前，调用方应确认槽类型
 *  支持系统属性。
 *
 *  If the slot type does not contain system attributes, this will throw an
 *  error.  Hence before calling this function, callers should make sure that
 *  the slot type is the one that supports system attributes.
 */
static inline Datum
slot_getsysattr(TupleTableSlot *slot, int attnum, bool *isnull)
{
	Assert(attnum < 0);			/* caller error / 调用方错误 */

	if (attnum == TableOidAttributeNumber)
	{
		*isnull = false;
		return ObjectIdGetDatum(slot->tts_tableOid);
	}
	else if (attnum == SelfItemPointerAttributeNumber)
	{
		*isnull = false;
		return PointerGetDatum(&slot->tts_tid);
	}

	/* Fetch the system attribute from the underlying tuple. / 从底层元组取系统属性。 */
	return slot->tts_ops->getsysattr(slot, attnum, isnull);
}

/*
 * ExecClearTuple - clear the slot's contents / 清除槽的内容
 */
static inline TupleTableSlot *
ExecClearTuple(TupleTableSlot *slot)
{
	slot->tts_ops->clear(slot);

	return slot;
}

/* ExecMaterializeSlot - force a slot into the "materialized" state.
 *                       - 强制槽进入 "materialized" 状态。
 *
 * 这使槽的元组成为本地副本，不依赖任何外部存储（即不指向 Buffer，或不在其他
 * 内存上下文中分配）。
 *
 * 典型用途是为将要写入磁盘的计算元组做准备。原始数据可能是 virtual 也可能
 * 不是，但无论如何 heap_insert 都需要一份可写入的私有副本。
 *
 * This causes the slot's tuple to be a local copy not dependent on any
 * external storage (i.e. pointing into a Buffer, or having allocations in
 * another memory context).
 *
 * A typical use for this operation is to prepare a computed tuple for being
 * stored on disk.  The original data may or may not be virtual, but in any
 * case we need a private copy for heap_insert to scribble on.
 */
static inline void
ExecMaterializeSlot(TupleTableSlot *slot)
{
	slot->tts_ops->materialize(slot);
}

/*
 * ExecCopySlotHeapTuple - return HeapTuple allocated in caller's context
 *                         - 返回在调用方上下文中分配的 HeapTuple
 */
static inline HeapTuple
ExecCopySlotHeapTuple(TupleTableSlot *slot)
{
	Assert(!TTS_EMPTY(slot));

	return slot->tts_ops->copy_heap_tuple(slot);
}

/*
 * ExecCopySlotMinimalTuple - return MinimalTuple allocated in caller's context
 *                            - 返回在调用方上下文中分配的 MinimalTuple
 */
static inline MinimalTuple
ExecCopySlotMinimalTuple(TupleTableSlot *slot)
{
	return slot->tts_ops->copy_minimal_tuple(slot);
}

/*
 * ExecCopySlot - copy one slot's contents into another.
 *              - 将一个槽的内容拷贝到另一个槽。
 *
 * 若要在目标槽中访问源槽的系统属性，目标槽与源槽的类型须一致。
 *
 * If a source's system attributes are supposed to be accessed in the target
 * slot, the target slot and source slot types need to match.
 */
static inline TupleTableSlot *
ExecCopySlot(TupleTableSlot *dstslot, TupleTableSlot *srcslot)
{
	Assert(!TTS_EMPTY(srcslot));
	Assert(srcslot != dstslot);

	dstslot->tts_ops->copyslot(dstslot, srcslot);

	return dstslot;
}

#endif							/* FRONTEND */

#endif							/* TUPTABLE_H */

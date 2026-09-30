/*-------------------------------------------------------------------------
 *
 * heapam.h
 *	  POSTGRES heap access method definitions.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/heapam.h
 *
 *-------------------------------------------------------------------------
 *
 * 中文翻译：
 * heapam.h POSTGRES 堆访问方法定义。 src/inc
 * lude/access/heapam.h
 */
#ifndef HEAPAM_H
#define HEAPAM_H

#include "access/heapam_xlog.h"
#include "access/relation.h"	/* for backward compatibility */

/* 中文翻译：为了向后兼容 */
#include "access/relscan.h"
#include "access/sdir.h"
#include "access/skey.h"
#include "access/table.h"		/* for backward compatibility */

/* 中文翻译：为了向后兼容 */
#include "access/tableam.h"
#include "nodes/lockoptions.h"
#include "nodes/primnodes.h"
#include "storage/bufpage.h"
#include "storage/dsm.h"
#include "storage/lockdefs.h"
#include "storage/read_stream.h"
#include "storage/shm_toc.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"


/* "options" flag bits for heap_insert */

/* 中文翻译：heap_insert 的“选项”标志位 */
#define HEAP_INSERT_SKIP_FSM	TABLE_INSERT_SKIP_FSM
#define HEAP_INSERT_FROZEN		TABLE_INSERT_FROZEN
#define HEAP_INSERT_NO_LOGICAL	TABLE_INSERT_NO_LOGICAL
#define HEAP_INSERT_SPECULATIVE 0x0010

/* "options" flag bits for heap_page_prune_and_freeze */

/* 中文翻译：heap_page_prune_and_freeze 的“选项”标志位 */
#define HEAP_PAGE_PRUNE_MARK_UNUSED_NOW		(1 << 0)
#define HEAP_PAGE_PRUNE_FREEZE				(1 << 1)

typedef struct BulkInsertStateData *BulkInsertState;
struct TupleTableSlot;
struct VacuumCutoffs;

#define MaxLockTupleMode	LockTupleExclusive

/*
 * Descriptor for heap table scans.
 *
 * 中文翻译：
 * 堆表扫描的描述符。
 */
typedef struct HeapScanDescData
{
	TableScanDescData rs_base;	/* AM independent part of the descriptor */

	/* 中文翻译：AM 描述符的独立部分 */

	/* state set up at initscan time */

	/* 中文翻译：在 initscan 时设置的状态 */
	BlockNumber rs_nblocks;		/* total number of blocks in rel */

	/* 中文翻译：rel 中的块总数 */
	BlockNumber rs_startblock;	/* block # to start at */

	/* 中文翻译：块#开始于 */
	BlockNumber rs_numblocks;	/* max number of blocks to scan */

	/* 中文翻译：要扫描的最大块数 */
	/* rs_numblocks is usually InvalidBlockNumber, meaning "scan whole rel" */

	/* 中文翻译：rs_numblocks通常是InvalidBlockNumber，意思是“扫描整个rel” */

	/* scan current state */

	/* 中文翻译：扫描当前状态 */
	bool		rs_inited;		/* false = scan not init'd yet */

	/* 中文翻译：false = 扫描尚未初始化 */
	OffsetNumber rs_coffset;	/* current offset # in non-page-at-a-time mode */

	/* 中文翻译：非一次分页模式下的当前偏移量# */
	BlockNumber rs_cblock;		/* current block # in scan, if any */

	/* 中文翻译：扫描中的当前块#（如果有） */
	Buffer		rs_cbuf;		/* current buffer in scan, if any */

	/* 中文翻译：扫描中的当前缓冲区（如果有） */
	/* NB: if rs_cbuf is not InvalidBuffer, we hold a pin on that buffer */

	/* 中文翻译：注意：如果 rs_cbuf 不是 InvalidBuffer，我们会在该缓冲区上保留一个 pin */

	BufferAccessStrategy rs_strategy;	/* access strategy for reads */

	/* 中文翻译：读取的访问策略 */

	HeapTupleData rs_ctup;		/* current tuple in scan, if any */

	/* 中文翻译：扫描中的当前元组（如果有） */

	/* For scans that stream reads */

	/* 中文翻译：对于流读取的扫描 */
	ReadStream *rs_read_stream;

	/*
	 * For sequential scans and TID range scans to stream reads. The read
	 * stream is allocated at the beginning of the scan and reset on rescan or
	 * when the scan direction changes. The scan direction is saved each time
	 * a new page is requested. If the scan direction changes from one page to
	 * the next, the read stream releases all previously pinned buffers and
	 * resets the prefetch block.
	 *
	 * 中文翻译：
	 * 用于顺序扫描和 TID 范围扫描以流式读取。读取流在扫描开始时分配，
	 * 并在重新扫描或扫描方向改变时重置。每次请求新页面时都会保存扫描方向。
	 * 如果扫描方向从一页更改为下一页，则读取流将释放所有先前固定的缓冲区并
	 * 重置预取块。
	 */
	ScanDirection rs_dir;
	BlockNumber rs_prefetch_block;

	/*
	 * For parallel scans to store page allocation data.  NULL when not
	 * performing a parallel scan.
	 *
	 * 中文翻译：
	 * 用于并行扫描来存储页面分配数据。不执行并行扫描时为 NULL。
	 */
	ParallelBlockTableScanWorkerData *rs_parallelworkerdata;

	/* these fields only used in page-at-a-time mode and for bitmap scans */

	/* 中文翻译：这些字段仅用于一次一页模式和位图扫描 */
	uint32		rs_cindex;		/* current tuple's index in vistuples */

	/* 中文翻译：当前元组在 vistuples 中的索引 */
	uint32		rs_ntuples;		/* number of visible tuples on page */

	/* 中文翻译：页面上可见元组的数量 */
	OffsetNumber rs_vistuples[MaxHeapTuplesPerPage];	/* their offsets */

	/* 中文翻译：他们的偏移量 */
} HeapScanDescData;
typedef struct HeapScanDescData *HeapScanDesc;

typedef struct BitmapHeapScanDescData
{
	HeapScanDescData rs_heap_base;

	/* Holds no data */

	/* 中文翻译：不保存任何数据 */
}			BitmapHeapScanDescData;
typedef struct BitmapHeapScanDescData *BitmapHeapScanDesc;

/*
 * Descriptor for fetches from heap via an index.
 *
 * 中文翻译：
 * 通过索引从堆中获取的描述符。
 */
typedef struct IndexFetchHeapData
{
	IndexFetchTableData xs_base;	/* AM independent part of the descriptor */

	/* 中文翻译：AM 描述符的独立部分 */

	Buffer		xs_cbuf;		/* current heap buffer in scan, if any */

	/* 中文翻译：扫描中的当前堆缓冲区（如果有） */
	/* NB: if xs_cbuf is not InvalidBuffer, we hold a pin on that buffer */

	/* 中文翻译：注意：如果 xs_cbuf 不是 InvalidBuffer，我们会在该缓冲区上保留一个 pin */
} IndexFetchHeapData;

/* Result codes for HeapTupleSatisfiesVacuum */

/* 中文翻译：HeapTupleSatisfiesVacuum 的结果代码 */
typedef enum
{
	HEAPTUPLE_DEAD,				/* tuple is dead and deletable */

	/* 中文翻译：元组已死且可删除 */
	HEAPTUPLE_LIVE,				/* tuple is live (committed, no deleter) */

	/* 中文翻译：元组处于活动状态（已提交，无删除器） */
	HEAPTUPLE_RECENTLY_DEAD,	/* tuple is dead, but not deletable yet */

	/* 中文翻译：元组已死，但尚不可删除 */
	HEAPTUPLE_INSERT_IN_PROGRESS,	/* inserting xact is still in progress */

	/* 中文翻译：插入 xact 仍在进行中 */
	HEAPTUPLE_DELETE_IN_PROGRESS,	/* deleting xact is still in progress */

	/* 中文翻译：删除 xact 仍在进行中 */
} HTSV_Result;

/*
 * heap_prepare_freeze_tuple may request that heap_freeze_execute_prepared
 * check any tuple's to-be-frozen xmin and/or xmax status using pg_xact
 *
 * 中文翻译：
 * heap_prepare_freeze_tuple 可以请求 hea
 * p_freeze_execute_prepared 使用 pg_xa
 * ct 检查任何元组的待冻结 xmin 和/或 xmax 状态
 */
#define		HEAP_FREEZE_CHECK_XMIN_COMMITTED	0x01
#define		HEAP_FREEZE_CHECK_XMAX_ABORTED		0x02

/* heap_prepare_freeze_tuple state describing how to freeze a tuple */

/* 中文翻译：heap_prepare_freeze_tuple 状态描述如何冻结元组 */
typedef struct HeapTupleFreeze
{
	/* Fields describing how to process tuple */

	/* 中文翻译：描述如何处理元组的字段 */
	TransactionId xmax;
	uint16		t_infomask2;
	uint16		t_infomask;
	uint8		frzflags;

	/* xmin/xmax check flags */

	/* 中文翻译：xmin/xmax 检查标志 */
	uint8		checkflags;
	/* Page offset number for tuple */

	/* 中文翻译：元组的页偏移量 */
	OffsetNumber offset;
} HeapTupleFreeze;

/*
 * State used by VACUUM to track the details of freezing all eligible tuples
 * on a given heap page.
 *
 * VACUUM prepares freeze plans for each page via heap_prepare_freeze_tuple
 * calls (every tuple with storage gets its own call).  This page-level freeze
 * state is updated across each call, which ultimately determines whether or
 * not freezing the page is required.
 *
 * Aside from the basic question of whether or not freezing will go ahead, the
 * state also tracks the oldest extant XID/MXID in the table as a whole, for
 * the purposes of advancing relfrozenxid/relminmxid values in pg_class later
 * on.  Each heap_prepare_freeze_tuple call pushes NewRelfrozenXid and/or
 * NewRelminMxid back as required to avoid unsafe final pg_class values.  Any
 * and all unfrozen XIDs or MXIDs that remain after VACUUM finishes _must_
 * have values >= the final relfrozenxid/relminmxid values in pg_class.  This
 * includes XIDs that remain as MultiXact members from any tuple's xmax.
 *
 * When 'freeze_required' flag isn't set after all tuples are examined, the
 * final choice on freezing is made by vacuumlazy.c.  It can decide to trigger
 * freezing based on whatever criteria it deems appropriate.  However, it is
 * recommended that vacuumlazy.c avoid early freezing when freezing does not
 * enable setting the target page all-frozen in the visibility map afterwards.
 *
 * 中文翻译：
 * VACUUM 使用状态来跟踪给定堆页上冻结所有符合条件的元组的详细信
 * 息。 VACUUM 通过 heap_prepare_freeze_t
 * uple 调用为每个页面准备冻结计划（每个具有存储的元组都有自己的调
 * 用）。此页面级冻结状态会在每次调用时更新，最终确定是否需要冻结页面。
 * 除了冻结是否继续进行的基本问题之外，状态还整体跟踪表中最古老的现有
 * XID/MXID，以便稍后在 pg_class 中推进 relfro
 * zenxid/relminmxid 值。每个 heap_prepar
 * e_freeze_tuple 调用都会根据需要将 NewRelfro
 * zenXid 和/或 NewRelminMxid 推回，以避免不安全
 * 的最终 pg_class 值。 VACUUM 完成后剩余的任何和所有
 * 未冻结的 XID 或 MXID _必须_ 具有值 >= pg_cla
 * ss 中的最终 relfrozenxid/relminmxid 值。
 * 这包括作为任何元组的 xmax 中的 MultiXact 成员保留的
 *  XID。当检查完所有元组后未设置“freeze_required”
 * 标志时，冻结的最终选择由vacuumlazy.c 做出。它可以根据它
 * 认为合适的任何标准决定触发冻结。但是，建议在冻结之后无法在可见性图中
 * 将目标页面设置为全部冻结时，vacuumlazy.c 避免早期冻结。
 */
typedef struct HeapPageFreeze
{
	/* Is heap_prepare_freeze_tuple caller required to freeze page? */

	/* 中文翻译：heap_prepare_freeze_tuple 调用者是否需要冻结页面？ */
	bool		freeze_required;

	/*
	 * "Freeze" NewRelfrozenXid/NewRelminMxid trackers.
	 *
	 * Trackers used when heap_freeze_execute_prepared freezes, or when there
	 * are zero freeze plans for a page.  It is always valid for vacuumlazy.c
	 * to freeze any page, by definition.  This even includes pages that have
	 * no tuples with storage to consider in the first place.  That way the
	 * 'totally_frozen' results from heap_prepare_freeze_tuple can always be
	 * used in the same way, even when no freeze plans need to be executed to
	 * "freeze the page".  Only the "freeze" path needs to consider the need
	 * to set pages all-frozen in the visibility map under this scheme.
	 *
	 * When we freeze a page, we generally freeze all XIDs < OldestXmin, only
	 * leaving behind XIDs that are ineligible for freezing, if any.  And so
	 * you might wonder why these trackers are necessary at all; why should
	 * _any_ page that VACUUM freezes _ever_ be left with XIDs/MXIDs that
	 * ratchet back the top-level NewRelfrozenXid/NewRelminMxid trackers?
	 *
	 * It is useful to use a definition of "freeze the page" that does not
	 * overspecify how MultiXacts are affected.  heap_prepare_freeze_tuple
	 * generally prefers to remove Multis eagerly, but lazy processing is used
	 * in cases where laziness allows VACUUM to avoid allocating a new Multi.
	 * The "freeze the page" trackers enable this flexibility.
	 *
	 * 中文翻译：
	 * “冻结”NewRelfrozenXid/NewRelminMxid
	 * 跟踪器。当 heap_freeze_execute_prepared
	 *  冻结或页面的冻结计划为零时使用的跟踪器。根据定义，vacuumla
	 * zy.c 冻结任何页面始终有效。这甚至包括没有首先要考虑存储的元组的
	 * 页面。这样，即使不需要执行冻结计划来“冻结页面”，也始终可以以相同的
	 * 方式使用 heap_prepare_freeze_tuple 的“t
	 * otally_frozen”结果。该方案下只有“冻结”路径需要考虑将
	 * 可见图中的页面设置为全冻结的需要。当我们冻结页面时，我们通常会冻结所
	 * 有 < OldestXmin 的 XID，只留下不符合冻结条件的 X
	 * ID（如果有）。所以你可能想知道为什么这些追踪器是必要的？为什么 V
	 * ACUUM 冻结的_任何_页面都应该留下 XID/MXID 来回溯顶
	 * 级 NewRelfrozenXid/NewRelminMxid 跟踪
	 * 器？使用“冻结页面”的定义非常有用，该定义不会过度指定 MultiX
	 * acts 的影响方式。 heap_prepare_freeze_tu
	 * ple 通常更喜欢立即删除 Multi，但在惰性允许 VACUUM
	 * 避免分配新 Multi 的情况下使用惰性处理。 “冻结页面”跟踪器实
	 * 现了这种灵活性。
	 */
	TransactionId FreezePageRelfrozenXid;
	MultiXactId FreezePageRelminMxid;

	/*
	 * "No freeze" NewRelfrozenXid/NewRelminMxid trackers.
	 *
	 * These trackers are maintained in the same way as the trackers used when
	 * VACUUM scans a page that isn't cleanup locked.  Both code paths are
	 * based on the same general idea (do less work for this page during the
	 * ongoing VACUUM, at the cost of having to accept older final values).
	 *
	 * 中文翻译：
	 * “不冻结”NewRelfrozenXid/NewRelminMxid
	 *  跟踪器。这些跟踪器的维护方式与 VACUUM 扫描未清除锁定的页面
	 * 时使用的跟踪器相同。两个代码路径都基于相同的总体思路（在正在进行的
	 * VACUUM 期间为此页面执行更少的工作，但代价是必须接受旧的最终值
	 * ）。
	 */
	TransactionId NoFreezePageRelfrozenXid;
	MultiXactId NoFreezePageRelminMxid;

} HeapPageFreeze;

/*
 * Per-page state returned by heap_page_prune_and_freeze()
 *
 * 中文翻译：
 * heap_page_prune_and_freeze() 返回的每页
 * 状态
 */
typedef struct PruneFreezeResult
{
	int			ndeleted;		/* Number of tuples deleted from the page */

	/* 中文翻译：从页面删除的元组数量 */
	int			nnewlpdead;		/* Number of newly LP_DEAD items */

	/* 中文翻译：新 LP_DEAD 项目的数量 */
	int			nfrozen;		/* Number of tuples we froze */

	/* 中文翻译：我们冻结的元组数量 */

	/* Number of live and recently dead tuples on the page, after pruning */

	/* 中文翻译：修剪后页面上存活和最近死亡的元组数量 */
	int			live_tuples;
	int			recently_dead_tuples;

	/*
	 * all_visible and all_frozen indicate if the all-visible and all-frozen
	 * bits in the visibility map can be set for this page, after pruning.
	 *
	 * vm_conflict_horizon is the newest xmin of live tuples on the page.  The
	 * caller can use it as the conflict horizon when setting the VM bits.  It
	 * is only valid if we froze some tuples (nfrozen > 0), and all_frozen is
	 * true.
	 *
	 * These are only set if the HEAP_PRUNE_FREEZE option is set.
	 *
	 * 中文翻译：
	 * all_visible 和 all_frozen 指示在修剪之后是否
	 * 可以为此页面设置可见性映射中的全部可见和全部冻结位。 vm_conf
	 * lict_horizo​​n 是页面上最新的活动元组 xmin。调用
	 * 者可以在设置 VM 位时将其用作冲突范围。仅当我们冻结一些元组（nf
	 * rozen > 0）并且 all_frozen 为 true 时，它
	 * ​​才有效。仅当设置了 HEAP_PRUNE_FREEZE 选项时才
	 * 设置这些。
	 */
	bool		all_visible;
	bool		all_frozen;
	TransactionId vm_conflict_horizon;

	/*
	 * Whether or not the page makes rel truncation unsafe.  This is set to
	 * 'true', even if the page contains LP_DEAD items.  VACUUM will remove
	 * them before attempting to truncate.
	 *
	 * 中文翻译：
	 * 该页面是否使 rel 截断变得不安全。即使页面包含 LP_DEAD
	 * 项目，此值也设置为“true”。 VACUUM 将在尝试截断之前删除
	 * 它们。
	 */
	bool		hastup;

	/*
	 * LP_DEAD items on the page after pruning.  Includes existing LP_DEAD
	 * items.
	 *
	 * 中文翻译：
	 * 修剪后页面上的 LP_DEAD 项。包括现有的 LP_DEAD 项目
	 * 。
	 */
	int			lpdead_items;
	OffsetNumber deadoffsets[MaxHeapTuplesPerPage];
} PruneFreezeResult;

/* 'reason' codes for heap_page_prune_and_freeze() */

/* 中文翻译：heap_page_prune_and_freeze() 的“原因”代码 */
typedef enum
{
	PRUNE_ON_ACCESS,			/* on-access pruning */

	/* 中文翻译：访问时修剪 */
	PRUNE_VACUUM_SCAN,			/* VACUUM 1st heap pass */

	/* 中文翻译：VACUUM 第一堆传递 */
	PRUNE_VACUUM_CLEANUP,		/* VACUUM 2nd heap pass */

	/* 中文翻译：VACUUM 第二次堆传递 */
} PruneReason;

/* ----------------
 *		function prototypes for heap access method
 *
 * heap_create, heap_create_with_catalog, and heap_drop_with_catalog
 * are declared in catalog/heap.h
 * ----------------
 *
 * 中文翻译：
 * 堆访问方法 heap_create、heap_create_with
 * _catalog 和 heap_drop_with_catalog
 * 的函数原型在catalog/heap.h 中声明
 */


/*
 * HeapScanIsValid
 *		True iff the heap scan is valid.
 *
 * 中文翻译：
 * HeapScanIsValid 当且仅当堆扫描有效时为 True。
 */
#define HeapScanIsValid(scan) PointerIsValid(scan)

/*
 * Function heap_beginscan initializes the requested access-layer operation by preparing input state, validating required context, and returning the initialized handle or result.
 *
 * 函数 heap_beginscan通过准备输入状态、校验所需上下文并返回已初始化的句柄或结果，初始化请求的访问层操作。
 */
extern TableScanDesc heap_beginscan(Relation relation, Snapshot snapshot,
									int nkeys, ScanKey key,
									ParallelTableScanDesc parallel_scan,
									uint32 flags);
/*
 * Function heap_setscanlimits updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 heap_setscanlimits通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
extern void heap_setscanlimits(TableScanDesc sscan, BlockNumber startBlk,
							   BlockNumber numBlks);
/*
 * Function heap_prepare_pagescan updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 heap_prepare_pagescan通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
extern void heap_prepare_pagescan(TableScanDesc sscan);
/*
 * Function heap_rescan carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 heap_rescan通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern void heap_rescan(TableScanDesc sscan, ScanKey key, bool set_params,
						bool allow_strat, bool allow_sync, bool allow_pagemode);
/*
 * Function heap_endscan completes the requested access-layer operation by releasing or finalizing the supplied state after the required cleanup steps.
 *
 * 函数 heap_endscan在完成必要的清理步骤后，通过释放或终结传入状态来结束请求的访问层操作。
 */
extern void heap_endscan(TableScanDesc sscan);
/*
 * Function heap_getnext retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 heap_getnext通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
extern HeapTuple heap_getnext(TableScanDesc sscan, ScanDirection direction);
/*
 * Function heap_getnextslot retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 heap_getnextslot通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
extern bool heap_getnextslot(TableScanDesc sscan,
							 ScanDirection direction, struct TupleTableSlot *slot);
/*
 * Function heap_set_tidrange updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 heap_set_tidrange通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
extern void heap_set_tidrange(TableScanDesc sscan, ItemPointer mintid,
							  ItemPointer maxtid);
/*
 * Function heap_getnextslot_tidrange retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 heap_getnextslot_tidrange通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
extern bool heap_getnextslot_tidrange(TableScanDesc sscan,
									  ScanDirection direction,
									  TupleTableSlot *slot);
/*
 * Function heap_fetch retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 heap_fetch通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
extern bool heap_fetch(Relation relation, Snapshot snapshot,
					   HeapTuple tuple, Buffer *userbuf, bool keep_buf);
/*
 * Function heap_hot_search_buffer retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 heap_hot_search_buffer通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
extern bool heap_hot_search_buffer(ItemPointer tid, Relation relation,
								   Buffer buffer, Snapshot snapshot, HeapTuple heapTuple,
								   bool *all_dead, bool first_call);

/*
 * Function heap_get_latest_tid retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 heap_get_latest_tid通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
extern void heap_get_latest_tid(TableScanDesc sscan, ItemPointer tid);

/*
 * Function GetBulkInsertState retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 GetBulkInsertState通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
extern BulkInsertState GetBulkInsertState(void);
/*
 * Function FreeBulkInsertState completes the requested access-layer operation by releasing or finalizing the supplied state after the required cleanup steps.
 *
 * 函数 FreeBulkInsertState在完成必要的清理步骤后，通过释放或终结传入状态来结束请求的访问层操作。
 */
extern void FreeBulkInsertState(BulkInsertState);
/*
 * Function ReleaseBulkInsertStatePin completes the requested access-layer operation by releasing or finalizing the supplied state after the required cleanup steps.
 *
 * 函数 ReleaseBulkInsertStatePin在完成必要的清理步骤后，通过释放或终结传入状态来结束请求的访问层操作。
 */
extern void ReleaseBulkInsertStatePin(BulkInsertState bistate);

/*
 * Function heap_insert constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 heap_insert通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern void heap_insert(Relation relation, HeapTuple tup, CommandId cid,
						int options, BulkInsertState bistate);
/*
 * Function heap_multi_insert constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 heap_multi_insert通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern void heap_multi_insert(Relation relation, struct TupleTableSlot **slots,
							  int ntuples, CommandId cid, int options,
							  BulkInsertState bistate);
/*
 * Function heap_delete constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 heap_delete通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern TM_Result heap_delete(Relation relation, ItemPointer tid,
							 CommandId cid, Snapshot crosscheck, bool wait,
							 struct TM_FailureData *tmfd, bool changingPart);
/*
 * Function heap_finish_speculative completes the requested access-layer operation by releasing or finalizing the supplied state after the required cleanup steps.
 *
 * 函数 heap_finish_speculative在完成必要的清理步骤后，通过释放或终结传入状态来结束请求的访问层操作。
 */
extern void heap_finish_speculative(Relation relation, ItemPointer tid);
/*
 * Function heap_abort_speculative completes the requested access-layer operation by releasing or finalizing the supplied state after the required cleanup steps.
 *
 * 函数 heap_abort_speculative在完成必要的清理步骤后，通过释放或终结传入状态来结束请求的访问层操作。
 */
extern void heap_abort_speculative(Relation relation, ItemPointer tid);
/*
 * Function heap_update constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 heap_update通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern TM_Result heap_update(Relation relation, ItemPointer otid,
							 HeapTuple newtup,
							 CommandId cid, Snapshot crosscheck, bool wait,
							 struct TM_FailureData *tmfd, LockTupleMode *lockmode,
							 TU_UpdateIndexes *update_indexes);
/*
 * Function heap_lock_tuple updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 heap_lock_tuple通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
extern TM_Result heap_lock_tuple(Relation relation, HeapTuple tuple,
								 CommandId cid, LockTupleMode mode, LockWaitPolicy wait_policy,
								 bool follow_updates,
								 Buffer *buffer, struct TM_FailureData *tmfd);

/*
 * Function heap_inplace_lock updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 heap_inplace_lock通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
extern bool heap_inplace_lock(Relation relation,
							  HeapTuple oldtup_ptr, Buffer buffer,
							  void (*release_callback) (void *), void *arg);
/*
 * Function heap_inplace_update_and_unlock updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 heap_inplace_update_and_unlock通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
extern void heap_inplace_update_and_unlock(Relation relation,
										   HeapTuple oldtup, HeapTuple tuple,
										   Buffer buffer);
/*
 * Function heap_inplace_unlock updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 heap_inplace_unlock通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
extern void heap_inplace_unlock(Relation relation,
								HeapTuple oldtup, Buffer buffer);
/*
 * Function heap_prepare_freeze_tuple completes the requested access-layer operation by releasing or finalizing the supplied state after the required cleanup steps.
 *
 * 函数 heap_prepare_freeze_tuple在完成必要的清理步骤后，通过释放或终结传入状态来结束请求的访问层操作。
 */
extern bool heap_prepare_freeze_tuple(HeapTupleHeader tuple,
									  const struct VacuumCutoffs *cutoffs,
									  HeapPageFreeze *pagefrz,
									  HeapTupleFreeze *frz, bool *totally_frozen);

/*
 * Function heap_pre_freeze_checks completes the requested access-layer operation by releasing or finalizing the supplied state after the required cleanup steps.
 *
 * 函数 heap_pre_freeze_checks在完成必要的清理步骤后，通过释放或终结传入状态来结束请求的访问层操作。
 */
extern void heap_pre_freeze_checks(Buffer buffer,
								   HeapTupleFreeze *tuples, int ntuples);
/*
 * Function heap_freeze_prepared_tuples completes the requested access-layer operation by releasing or finalizing the supplied state after the required cleanup steps.
 *
 * 函数 heap_freeze_prepared_tuples在完成必要的清理步骤后，通过释放或终结传入状态来结束请求的访问层操作。
 */
extern void heap_freeze_prepared_tuples(Buffer buffer,
										HeapTupleFreeze *tuples, int ntuples);
/*
 * Function heap_freeze_tuple completes the requested access-layer operation by releasing or finalizing the supplied state after the required cleanup steps.
 *
 * 函数 heap_freeze_tuple在完成必要的清理步骤后，通过释放或终结传入状态来结束请求的访问层操作。
 */
extern bool heap_freeze_tuple(HeapTupleHeader tuple,
							  TransactionId relfrozenxid, TransactionId relminmxid,
							  TransactionId FreezeLimit, TransactionId MultiXactCutoff);
/*
 * Function heap_tuple_should_freeze completes the requested access-layer operation by releasing or finalizing the supplied state after the required cleanup steps.
 *
 * 函数 heap_tuple_should_freeze在完成必要的清理步骤后，通过释放或终结传入状态来结束请求的访问层操作。
 */
extern bool heap_tuple_should_freeze(HeapTupleHeader tuple,
									 const struct VacuumCutoffs *cutoffs,
									 TransactionId *NoFreezePageRelfrozenXid,
									 MultiXactId *NoFreezePageRelminMxid);
/*
 * Function heap_tuple_needs_eventual_freeze completes the requested access-layer operation by releasing or finalizing the supplied state after the required cleanup steps.
 *
 * 函数 heap_tuple_needs_eventual_freeze在完成必要的清理步骤后，通过释放或终结传入状态来结束请求的访问层操作。
 */
extern bool heap_tuple_needs_eventual_freeze(HeapTupleHeader tuple);

/*
 * Function simple_heap_insert constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 simple_heap_insert通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern void simple_heap_insert(Relation relation, HeapTuple tup);
/*
 * Function simple_heap_delete constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 simple_heap_delete通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern void simple_heap_delete(Relation relation, ItemPointer tid);
/*
 * Function simple_heap_update constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 simple_heap_update通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern void simple_heap_update(Relation relation, ItemPointer otid,
							   HeapTuple tup, TU_UpdateIndexes *update_indexes);

/*
 * Function heap_index_delete_tuples constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 heap_index_delete_tuples通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern TransactionId heap_index_delete_tuples(Relation rel,
											  TM_IndexDeleteOp *delstate);

/* in heap/pruneheap.c */

/* 中文翻译：在堆/pruneheap.c中 */
struct GlobalVisState;
/*
 * Function heap_page_prune_opt carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 heap_page_prune_opt通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern void heap_page_prune_opt(Relation relation, Buffer buffer);
/*
 * Function heap_page_prune_and_freeze completes the requested access-layer operation by releasing or finalizing the supplied state after the required cleanup steps.
 *
 * 函数 heap_page_prune_and_freeze在完成必要的清理步骤后，通过释放或终结传入状态来结束请求的访问层操作。
 */
extern void heap_page_prune_and_freeze(Relation relation, Buffer buffer,
									   struct GlobalVisState *vistest,
									   int options,
									   struct VacuumCutoffs *cutoffs,
									   PruneFreezeResult *presult,
									   PruneReason reason,
									   OffsetNumber *off_loc,
									   TransactionId *new_relfrozen_xid,
									   MultiXactId *new_relmin_mxid);
/*
 * Function heap_page_prune_execute carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 heap_page_prune_execute通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern void heap_page_prune_execute(Buffer buffer, bool lp_truncate_only,
									OffsetNumber *redirected, int nredirected,
									OffsetNumber *nowdead, int ndead,
									OffsetNumber *nowunused, int nunused);
/*
 * Function heap_get_root_tuples retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 heap_get_root_tuples通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
extern void heap_get_root_tuples(Page page, OffsetNumber *root_offsets);
/*
 * Function log_heap_prune_and_freeze completes the requested access-layer operation by releasing or finalizing the supplied state after the required cleanup steps.
 *
 * 函数 log_heap_prune_and_freeze在完成必要的清理步骤后，通过释放或终结传入状态来结束请求的访问层操作。
 */
extern void log_heap_prune_and_freeze(Relation relation, Buffer buffer,
									  TransactionId conflict_xid,
									  bool cleanup_lock,
									  PruneReason reason,
									  HeapTupleFreeze *frozen, int nfrozen,
									  OffsetNumber *redirected, int nredirected,
									  OffsetNumber *dead, int ndead,
									  OffsetNumber *unused, int nunused);

/* in heap/vacuumlazy.c */

/* 中文翻译：在堆/vacuumlazy.c 中 */
struct VacuumParams;
/*
 * Function heap_vacuum_rel carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 heap_vacuum_rel通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern void heap_vacuum_rel(Relation rel,
							struct VacuumParams *params, BufferAccessStrategy bstrategy);

/* in heap/heapam_visibility.c */

/* 中文翻译：在堆/heapam_visibility.c中 */
/*
 * Function HeapTupleSatisfiesVisibility evaluates the requested access-layer condition by examining the supplied state and returning whether the condition holds.
 *
 * 函数 HeapTupleSatisfiesVisibility通过检查传入状态来评估请求的访问层条件，并返回该条件是否成立。
 */
extern bool HeapTupleSatisfiesVisibility(HeapTuple htup, Snapshot snapshot,
										 Buffer buffer);
/*
 * Function HeapTupleSatisfiesUpdate constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 HeapTupleSatisfiesUpdate通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern TM_Result HeapTupleSatisfiesUpdate(HeapTuple htup, CommandId curcid,
										  Buffer buffer);
/*
 * Function HeapTupleSatisfiesVacuum evaluates the requested access-layer condition by examining the supplied state and returning whether the condition holds.
 *
 * 函数 HeapTupleSatisfiesVacuum通过检查传入状态来评估请求的访问层条件，并返回该条件是否成立。
 */
extern HTSV_Result HeapTupleSatisfiesVacuum(HeapTuple htup, TransactionId OldestXmin,
											Buffer buffer);
/*
 * Function HeapTupleSatisfiesVacuumHorizon evaluates the requested access-layer condition by examining the supplied state and returning whether the condition holds.
 *
 * 函数 HeapTupleSatisfiesVacuumHorizon通过检查传入状态来评估请求的访问层条件，并返回该条件是否成立。
 */
extern HTSV_Result HeapTupleSatisfiesVacuumHorizon(HeapTuple htup, Buffer buffer,
												   TransactionId *dead_after);
/*
 * Function HeapTupleSetHintBits updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 HeapTupleSetHintBits通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
extern void HeapTupleSetHintBits(HeapTupleHeader tuple, Buffer buffer,
								 uint16 infomask, TransactionId xid);
/*
 * Function HeapTupleHeaderIsOnlyLocked updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 HeapTupleHeaderIsOnlyLocked通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
extern bool HeapTupleHeaderIsOnlyLocked(HeapTupleHeader tuple);
/*
 * Function HeapTupleIsSurelyDead evaluates the requested access-layer condition by examining the supplied state and returning whether the condition holds.
 *
 * 函数 HeapTupleIsSurelyDead通过检查传入状态来评估请求的访问层条件，并返回该条件是否成立。
 */
extern bool HeapTupleIsSurelyDead(HeapTuple htup,
								  struct GlobalVisState *vistest);

/*
 * To avoid leaking too much knowledge about reorderbuffer implementation
 * details this is implemented in reorderbuffer.c not heapam_visibility.c
 *
 * 中文翻译：
 * 为了避免泄漏太多有关 reorderbuffer 实现细节的知识，这
 * 是在 reorderbuffer.c 而不是 heapam_visi
 * bility.c 中实现的
 */
struct HTAB;
/*
 * Function ResolveCminCmaxDuringDecoding carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 ResolveCminCmaxDuringDecoding通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern bool ResolveCminCmaxDuringDecoding(struct HTAB *tuplecid_data,
										  Snapshot snapshot,
										  HeapTuple htup,
										  Buffer buffer,
										  CommandId *cmin, CommandId *cmax);
/*
 * Function HeapCheckForSerializableConflictOut carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 HeapCheckForSerializableConflictOut通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern void HeapCheckForSerializableConflictOut(bool visible, Relation relation, HeapTuple tuple,
												Buffer buffer, Snapshot snapshot);

/*
 * heap_execute_freeze_tuple
 *		Execute the prepared freezing of a tuple with caller's freeze plan.
 *
 * Caller is responsible for ensuring that no other backend can access the
 * storage underlying this tuple, either by holding an exclusive lock on the
 * buffer containing it (which is what lazy VACUUM does), or by having it be
 * in private storage (which is what CLUSTER and friends do).
 *
 * 中文翻译：
 * heap_execute_freeze_tuple 使用调用者的冻结
 * 计划执行准备好的元组冻结。调用者负责确保没有其他后端可以访问该元组底
 * 层的存储，方法是在包含它的缓冲区上持有独占锁（这是惰性 VACUUM
 *  所做的），或者将其放在私有存储中（这是 CLUSTER 和朋友所做
 * 的）。
 */
/*
 * Function heap_execute_freeze_tuple completes the requested access-layer operation by releasing or finalizing the supplied state after the required cleanup steps.
 *
 * 函数 heap_execute_freeze_tuple在完成必要的清理步骤后，通过释放或终结传入状态来结束请求的访问层操作。
 */
static inline void
heap_execute_freeze_tuple(HeapTupleHeader tuple, HeapTupleFreeze *frz)
{
	HeapTupleHeaderSetXmax(tuple, frz->xmax);

	if (frz->frzflags & XLH_FREEZE_XVAC)
		HeapTupleHeaderSetXvac(tuple, FrozenTransactionId);

	if (frz->frzflags & XLH_INVALID_XVAC)
		HeapTupleHeaderSetXvac(tuple, InvalidTransactionId);

	tuple->t_infomask = frz->t_infomask;
	tuple->t_infomask2 = frz->t_infomask2;
}

#endif							/* HEAPAM_H */

/* 中文翻译：HEAPAM_H */

/*-------------------------------------------------------------------------
 *
 * verify_nbtree.c
 *		Verifies the integrity of nbtree indexes based on invariants.
 *
 * For B-Tree indexes, verification includes checking that each page in the
 * target index has items in logical order as reported by an insertion scankey
 * (the insertion scankey sort-wise NULL semantics are needed for
 * verification).
 *
 * When index-to-heap verification is requested, a Bloom filter is used to
 * fingerprint all tuples in the target index, as the index is traversed to
 * verify its structure.  A heap scan later uses Bloom filter probes to verify
 * that every visible heap tuple has a matching index tuple.
 *
 *
 * Copyright (c) 2017-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/amcheck/verify_nbtree.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heaptoast.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xact.h"
#include "verify_common.h"
#include "catalog/index.h"
#include "catalog/pg_am.h"
#include "catalog/pg_opfamily_d.h"
#include "common/pg_prng.h"
#include "lib/bloomfilter.h"
#include "miscadmin.h"
#include "storage/smgr.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"


PG_MODULE_MAGIC_EXT(
					.name = "amcheck",
					.version = PG_VERSION
);

/*
 * A B-Tree cannot possibly have this many levels, since there must be one
 * block per level, which is bound by the range of BlockNumber:
 *
 * 一棵 B 树不可能有这么多层，因为每一层必须有一个块，它受到 BlockNumber 范围的限制：
 */
#define InvalidBtreeLevel	((uint32) InvalidBlockNumber)
#define BTreeTupleGetNKeyAtts(itup, rel)   \
	Min(IndexRelationGetNumberOfKeyAttributes(rel), BTreeTupleGetNAtts(itup, rel))

/*
 * State associated with verifying a B-Tree index
 *
 * 与验证 B 树索引相关的状态
 *
 * target is the point of reference for a verification operation.
 *
 * 目标是验证操作的参考点。
 *
 * Other B-Tree pages may be allocated, but those are always auxiliary (e.g.,
 * they are current target's child pages).  Conceptually, problems are only
 * ever found in the current target page (or for a particular heap tuple during
 * heapallindexed verification).  Each page found by verification's left/right,
 * top/bottom scan becomes the target exactly once.
 *
 * 可以分配其他 B 树页面，但这些页面始终是辅助的（例如，它们是当前目标的子页面）。  从概念上讲，问题仅在当前目标页面中发现（或在 heapallindexed 验证期间针对特定堆元组）。  通过验证左/右、上/下扫描找到的每个页面都成为目标一次。
 */
typedef struct BtreeCheckState
{
	/*
	 * Unchanging state, established at start of verification:
	 *
	 * 不变的状态，在验证开始时建立：
	 */

	/* B-Tree Index Relation and associated heap relation
	 *
	 * B-Tree索引关系和关联堆关系
	 */
	Relation	rel;
	Relation	heaprel;
	/* rel is heapkeyspace index?
	 *
	 * rel 是堆键空间索引吗？
	 */
	bool		heapkeyspace;
	/* ShareLock held on heap/index, rather than AccessShareLock?
	 *
	 * ShareLock 保存在堆/索引上，而不是 AccessShareLock 上？
	 */
	bool		readonly;
	/* Also verifying heap has no unindexed tuples?
	 *
	 * 还验证堆没有未索引的元组？
	 */
	bool		heapallindexed;
	/* Also making sure non-pivot tuples can be found by new search?
	 *
	 * 还要确保新搜索可以找到非枢轴元组？
	 */
	bool		rootdescend;
	/* Also check uniqueness constraint if index is unique
	 *
	 * 如果索引是唯一的，还检查唯一性约束
	 */
	bool		checkunique;
	/* Per-page context
	 *
	 * 每页上下文
	 */
	MemoryContext targetcontext;
	/* Buffer access strategy
	 *
	 * 缓冲区访问策略
	 */
	BufferAccessStrategy checkstrategy;

	/*
	 * Info for uniqueness checking. Fill this field and the one below once
	 * per index check.
	 *
	 * 用于唯一性检查的信息。每次索引检查时填写此字段和下面的字段一次。
	 */
	IndexInfo  *indexinfo;
	/* Table scan snapshot for heapallindexed and checkunique
	 *
	 * heapallindexed 和 checkunique 的表扫描快照
	 */
	Snapshot	snapshot;

	/*
	 * Mutable state, for verification of particular page:
	 *
	 * 可变状态，用于验证特定页面：
	 */

	/* Current target page
	 *
	 * 当前目标页面
	 */
	Page		target;
	/* Target block number
	 *
	 * 目标块号
	 */
	BlockNumber targetblock;
	/* Target page's LSN
	 *
	 * 目标页面的LSN
	 */
	XLogRecPtr	targetlsn;

	/*
	 * Low key: high key of left sibling of target page.  Used only for child
	 * verification.  So, 'lowkey' is kept only when 'readonly' is set.
	 *
	 * 低调：目标页面左同级的高调。  仅用于子验证。  因此，仅当设置“readonly”时才保留“lowkey”。
	 */
	IndexTuple	lowkey;

	/*
	 * The rightlink and incomplete split flag of block one level down to the
	 * target page, which was visited last time via downlink from target page.
	 * We use it to check for missing downlinks.
	 *
	 * 下一层到目标页面的右链接和不完整分割标志，上次是通过目标页面下行访问的。我们用它来检查丢失的下行链路。
	 */
	BlockNumber prevrightlink;
	bool		previncompletesplit;

	/*
	 * Mutable state, for optional heapallindexed verification:
	 *
	 * 可变状态，用于可选的 heapallindexed 验证：
	 */

	/* Bloom filter fingerprints B-Tree index
	 *
	 * 布隆过滤器指纹 B 树索引
	 */
	bloom_filter *filter;
	/* Debug counter
	 *
	 * 调试计数器
	 */
	int64		heaptuplespresent;
} BtreeCheckState;

/*
 * Starting point for verifying an entire B-Tree index level
 *
 * 验证整个 B 树索引级别的起点
 */
typedef struct BtreeLevel
{
	/* Level number (0 is leaf page level).
	 *
	 * 级别编号（0 是叶页级别）。
	 */
	uint32		level;

	/* Left most block on level.  Scan of level begins here.
	 *
	 * 水平面上最左边的方块。  水平扫描从这里开始。
	 */
	BlockNumber leftmost;

	/* Is this level reported as "true" root level by meta page?
	 *
	 * 此级别是否被元页面报告为“真实”根级别？
	 */
	bool		istruerootlevel;
} BtreeLevel;

/*
 * Information about the last visible entry with current B-tree key.  Used
 * for validation of the unique constraint.
 *
 * 有关当前 B 树键的最后一个可见条目的信息。  用于验证唯一约束。
 */
typedef struct BtreeLastVisibleEntry
{
	BlockNumber blkno;			/* Index block */
	OffsetNumber offset;		/* Offset on index block */
	int			postingIndex;	/* Number in the posting list (-1 for
								 * non-deduplicated tuples) */
	ItemPointer tid;			/* Heap tid */
} BtreeLastVisibleEntry;

/*
 * arguments for the bt_index_check_callback callback
 *
 * bt_index_check_callback 回调的参数
 */
typedef struct BTCallbackState
{
	bool		parentcheck;
	bool		heapallindexed;
	bool		rootdescend;
	bool		checkunique;
} BTCallbackState;

PG_FUNCTION_INFO_V1(bt_index_check);
PG_FUNCTION_INFO_V1(bt_index_parent_check);

static void bt_index_check_callback(Relation indrel, Relation heaprel,
									void *state, bool readonly);
static void bt_check_every_level(Relation rel, Relation heaprel,
								 bool heapkeyspace, bool readonly, bool heapallindexed,
								 bool rootdescend, bool checkunique);
static BtreeLevel bt_check_level_from_leftmost(BtreeCheckState *state,
											   BtreeLevel level);
static bool bt_leftmost_ignoring_half_dead(BtreeCheckState *state,
										   BlockNumber start,
										   BTPageOpaque start_opaque);
static void bt_recheck_sibling_links(BtreeCheckState *state,
									 BlockNumber btpo_prev_from_target,
									 BlockNumber leftcurrent);
static bool heap_entry_is_visible(BtreeCheckState *state, ItemPointer tid);
static void bt_report_duplicate(BtreeCheckState *state,
								BtreeLastVisibleEntry *lVis,
								ItemPointer nexttid,
								BlockNumber nblock, OffsetNumber noffset,
								int nposting);
static void bt_entry_unique_check(BtreeCheckState *state, IndexTuple itup,
								  BlockNumber targetblock, OffsetNumber offset,
								  BtreeLastVisibleEntry *lVis);
static void bt_target_page_check(BtreeCheckState *state);
static BTScanInsert bt_right_page_check_scankey(BtreeCheckState *state,
												OffsetNumber *rightfirstoffset);
static void bt_child_check(BtreeCheckState *state, BTScanInsert targetkey,
						   OffsetNumber downlinkoffnum);
static void bt_child_highkey_check(BtreeCheckState *state,
								   OffsetNumber target_downlinkoffnum,
								   Page loaded_child,
								   uint32 target_level);
static void bt_downlink_missing_check(BtreeCheckState *state, bool rightsplit,
									  BlockNumber blkno, Page page);
static void bt_tuple_present_callback(Relation index, ItemPointer tid,
									  Datum *values, bool *isnull,
									  bool tupleIsAlive, void *checkstate);
static IndexTuple bt_normalize_tuple(BtreeCheckState *state,
									 IndexTuple itup);
static inline IndexTuple bt_posting_plain_tuple(IndexTuple itup, int n);
static bool bt_rootdescend(BtreeCheckState *state, IndexTuple itup);
static inline bool offset_is_negative_infinity(BTPageOpaque opaque,
											   OffsetNumber offset);
static inline bool invariant_l_offset(BtreeCheckState *state, BTScanInsert key,
									  OffsetNumber upperbound);
static inline bool invariant_leq_offset(BtreeCheckState *state,
										BTScanInsert key,
										OffsetNumber upperbound);
static inline bool invariant_g_offset(BtreeCheckState *state, BTScanInsert key,
									  OffsetNumber lowerbound);
static inline bool invariant_l_nontarget_offset(BtreeCheckState *state,
												BTScanInsert key,
												BlockNumber nontargetblock,
												Page nontarget,
												OffsetNumber upperbound);
static Page palloc_btree_page(BtreeCheckState *state, BlockNumber blocknum);
static inline BTScanInsert bt_mkscankey_pivotsearch(Relation rel,
													IndexTuple itup);
static ItemId PageGetItemIdCareful(BtreeCheckState *state, BlockNumber block,
								   Page page, OffsetNumber offset);
static inline ItemPointer BTreeTupleGetHeapTIDCareful(BtreeCheckState *state,
													  IndexTuple itup, bool nonpivot);
static inline ItemPointer BTreeTupleGetPointsToTID(IndexTuple itup);

/*
 * bt_index_check(index regclass, heapallindexed boolean, checkunique boolean)
 *
 * bt_index_check（索引regclass，heapallindexed布尔值，checkunique布尔值）
 *
 * Verify integrity of B-Tree index.
 *
 * 验证 B 树索引的完整性。
 *
 * Acquires AccessShareLock on heap & index relations.  Does not consider
 * invariants that exist between parent/child pages.  Optionally verifies
 * that heap does not contain any unindexed or incorrectly indexed tuples.
 *
 * 获取堆和索引关系上的 AccessShareLock。  不考虑父/子页面之间存在的不变量。  （可选）验证堆不包含任何未索引或索引不正确的元组。
 */
Datum
bt_index_check(PG_FUNCTION_ARGS)
{
	Oid			indrelid = PG_GETARG_OID(0);
	BTCallbackState args;

	args.heapallindexed = false;
	args.rootdescend = false;
	args.parentcheck = false;
	args.checkunique = false;

	if (PG_NARGS() >= 2)
		args.heapallindexed = PG_GETARG_BOOL(1);
	if (PG_NARGS() >= 3)
		args.checkunique = PG_GETARG_BOOL(2);

	amcheck_lock_relation_and_check(indrelid, BTREE_AM_OID,
									bt_index_check_callback,
									AccessShareLock, &args);

	PG_RETURN_VOID();
}

/*
 * bt_index_parent_check(index regclass, heapallindexed boolean, rootdescend boolean, checkunique boolean)
 *
 * bt_index_parent_check（索引regclass，heapallindexed布尔值，rootdescend布尔值，checkunique布尔值）
 *
 * Verify integrity of B-Tree index.
 *
 * 验证 B 树索引的完整性。
 *
 * Acquires ShareLock on heap & index relations.  Verifies that downlinks in
 * parent pages are valid lower bounds on child pages.  Optionally verifies
 * that heap does not contain any unindexed or incorrectly indexed tuples.
 *
 * 获取堆和索引关系上的 ShareLock。  验证父页面中的下行链接是否是子页面上的有效下限。  （可选）验证堆不包含任何未索引或索引不正确的元组。
 */
Datum
bt_index_parent_check(PG_FUNCTION_ARGS)
{
	Oid			indrelid = PG_GETARG_OID(0);
	BTCallbackState args;

	args.heapallindexed = false;
	args.rootdescend = false;
	args.parentcheck = true;
	args.checkunique = false;

	if (PG_NARGS() >= 2)
		args.heapallindexed = PG_GETARG_BOOL(1);
	if (PG_NARGS() >= 3)
		args.rootdescend = PG_GETARG_BOOL(2);
	if (PG_NARGS() >= 4)
		args.checkunique = PG_GETARG_BOOL(3);

	amcheck_lock_relation_and_check(indrelid, BTREE_AM_OID,
									bt_index_check_callback,
									ShareLock, &args);

	PG_RETURN_VOID();
}

/*
 * Helper for bt_index_[parent_]check, coordinating the bulk of the work.
 *
 * bt_index_[parent_]check 的助手，协调大部分工作。
 */
static void
bt_index_check_callback(Relation indrel, Relation heaprel, void *state, bool readonly)
{
	BTCallbackState *args = (BTCallbackState *) state;
	bool		heapkeyspace,
				allequalimage;

	if (!smgrexists(RelationGetSmgr(indrel), MAIN_FORKNUM))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" lacks a main relation fork",
						RelationGetRelationName(indrel))));

	/* Extract metadata from metapage, and sanitize it in passing
	 *
	 * 从元页面中提取元数据，并顺便对其进行清理
	 */
	_bt_metaversion(indrel, &heapkeyspace, &allequalimage);
	if (allequalimage && !heapkeyspace)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" metapage has equalimage field set on unsupported nbtree version",
						RelationGetRelationName(indrel))));
	if (allequalimage && !_bt_allequalimage(indrel, false))
	{
		bool		has_interval_ops = false;

		for (int i = 0; i < IndexRelationGetNumberOfKeyAttributes(indrel); i++)
			if (indrel->rd_opfamily[i] == INTERVAL_BTREE_FAM_OID)
			{
				has_interval_ops = true;
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("index \"%s\" metapage incorrectly indicates that deduplication is safe",
								RelationGetRelationName(indrel)),
						 has_interval_ops
						 ? errhint("This is known of \"interval\" indexes last built on a version predating 2023-11.")
						 : 0));
			}
	}

	/* Check index, possibly against table it is an index on
	 *
	 * 检查索引，可能针对它是索引的表
	 */
	bt_check_every_level(indrel, heaprel, heapkeyspace, readonly,
						 args->heapallindexed, args->rootdescend, args->checkunique);
}

/*
 * Main entry point for B-Tree SQL-callable functions. Walks the B-Tree in
 * logical order, verifying invariants as it goes.  Optionally, verification
 * checks if the heap relation contains any tuples that are not represented in
 * the index but should be.
 *
 * B 树 SQL 可调用函数的主入口点。按逻辑顺序遍历 B 树，同时验证不变量。  或者，验证检查堆关系是否包含索引中未表示但应表示的任何元组。
 *
 * It is the caller's responsibility to acquire appropriate heavyweight lock on
 * the index relation, and advise us if extra checks are safe when a ShareLock
 * is held.  (A lock of the same type must also have been acquired on the heap
 * relation.)
 *
 * 调用者有责任在索引关系上获取适当的重量级锁，并告诉我们在持有 ShareLock 时额外的检查是否安全。  （堆关系上还必须获取相同类型的锁。）
 *
 * A ShareLock is generally assumed to prevent any kind of physical
 * modification to the index structure, including modifications that VACUUM may
 * make.  This does not include setting of the LP_DEAD bit by concurrent index
 * scans, although that is just metadata that is not able to directly affect
 * any check performed here.  Any concurrent process that might act on the
 * LP_DEAD bit being set (recycle space) requires a heavyweight lock that
 * cannot be held while we hold a ShareLock.  (Besides, even if that could
 * happen, the ad-hoc recycling when a page might otherwise split is performed
 * per-page, and requires an exclusive buffer lock, which wouldn't cause us
 * trouble.  _bt_delitems_vacuum() may only delete leaf items, and so the extra
 * parent/child check cannot be affected.)
 *
 * 通常假设 ShareLock 可以防止对索引结构进行任何类型的物理修改，包括 VACUUM 可能进行的修改。  这不包括通过并发索引扫描设置 LP_DEAD 位，尽管这只是元数据，无法直接影响此处执行的任何检查。  任何可能对正在设置的 LP_DEAD 位（回收空间）起作用的并发进程都需要一个重量级锁，而当我们持有 ShareLock 时，该锁无法被持有。  （此外，即使可能发生这种情况，页面可能会拆分时的临时回收也是按页执行的，并且需要独占缓冲区锁，这不会给我们带来麻烦。_bt_delitems_vacuum() 可能只会删除叶项，因此额外的父/子检查不会受到影响。）
 */
static void
bt_check_every_level(Relation rel, Relation heaprel, bool heapkeyspace,
					 bool readonly, bool heapallindexed, bool rootdescend,
					 bool checkunique)
{
	BtreeCheckState *state;
	Page		metapage;
	BTMetaPageData *metad;
	uint32		previouslevel;
	BtreeLevel	current;

	if (!readonly)
		elog(DEBUG1, "verifying consistency of tree structure for index \"%s\"",
			 RelationGetRelationName(rel));
	else
		elog(DEBUG1, "verifying consistency of tree structure for index \"%s\" with cross-level checks",
			 RelationGetRelationName(rel));

	/*
	 * This assertion matches the one in index_getnext_tid().  See page
	 * recycling/"visible to everyone" notes in nbtree README.
	 *
	 * 该断言与index_getnext_tid() 中的断言相匹配。  请参阅 nbtree README 中的页面回收/“所有人可见”注释。
	 */
	Assert(TransactionIdIsValid(RecentXmin));

	/*
	 * Initialize state for entire verification operation
	 *
	 * 整个验证操作的初始化状态
	 */
	state = palloc0(sizeof(BtreeCheckState));
	state->rel = rel;
	state->heaprel = heaprel;
	state->heapkeyspace = heapkeyspace;
	state->readonly = readonly;
	state->heapallindexed = heapallindexed;
	state->rootdescend = rootdescend;
	state->checkunique = checkunique;
	state->snapshot = InvalidSnapshot;

	if (state->heapallindexed)
	{
		int64		total_pages;
		int64		total_elems;
		uint64		seed;

		/*
		 * Size Bloom filter based on estimated number of tuples in index,
		 * while conservatively assuming that each block must contain at least
		 * MaxTIDsPerBTreePage / 3 "plain" tuples -- see
		 * bt_posting_plain_tuple() for definition, and details of how posting
		 * list tuples are handled.
		 *
		 * 根据索引中估计的元组数量调整布隆过滤器的大小，同时保守地假设每个块必须至少包含 MaxTIDsPerBTreePage / 3 个“普通”元组 - 请参阅 bt_posting_plain_tuple() 了解定义以及如何处理发布列表元组的详细信息。
		 */
		total_pages = RelationGetNumberOfBlocks(rel);
		total_elems = Max(total_pages * (MaxTIDsPerBTreePage / 3),
						  (int64) state->rel->rd_rel->reltuples);
		/* Generate a random seed to avoid repetition
		 *
		 * 生成随机种子以避免重复
		 */
		seed = pg_prng_uint64(&pg_global_prng_state);
		/* Create Bloom filter to fingerprint index
		 *
		 * 创建布隆过滤器到指纹索引
		 */
		state->filter = bloom_create(total_elems, maintenance_work_mem, seed);
		state->heaptuplespresent = 0;

		/*
		 * Register our own snapshot for heapallindexed, rather than asking
		 * table_index_build_scan() to do this for us later.  This needs to
		 * happen before index fingerprinting begins, so we can later be
		 * certain that index fingerprinting should have reached all tuples
		 * returned by table_index_build_scan().
		 *
		 * 为 heapallindexed 注册我们自己的快照，而不是要求 table_index_build_scan() 稍后为我们执行此操作。  这需要在索引指纹识别开始之前发生，因此我们稍后可以确定索引指纹识别应该已经到达 table_index_build_scan() 返回的所有元组。
		 */
		state->snapshot = RegisterSnapshot(GetTransactionSnapshot());

		/*
		 * GetTransactionSnapshot() always acquires a new MVCC snapshot in
		 * READ COMMITTED mode.  A new snapshot is guaranteed to have all the
		 * entries it requires in the index.
		 *
		 * GetTransactionSnapshot() 始终在 READ COMMITTED 模式下获取新的 MVCC 快照。  新快照保证在索引中包含它所需的所有条目。
		 *
		 * We must defend against the possibility that an old xact snapshot
		 * was returned at higher isolation levels when that snapshot is not
		 * safe for index scans of the target index.  This is possible when
		 * the snapshot sees tuples that are before the index's indcheckxmin
		 * horizon.  Throwing an error here should be very rare.  It doesn't
		 * seem worth using a secondary snapshot to avoid this.
		 *
		 * 当旧的 xact 快照对于目标索引的索引扫描不安全时，我们必须防止以较高隔离级别返回旧的 xact 快照的可能性。  当快照看到索引的 indcheckxmin 范围之前的元组时，这是可能的。  这里抛出错误的情况应该很少见。  似乎不值得使用辅助快照来避免这种情况。
		 */
		if (IsolationUsesXactSnapshot() && rel->rd_index->indcheckxmin &&
			!TransactionIdPrecedes(HeapTupleHeaderGetXmin(rel->rd_indextuple->t_data),
								   state->snapshot->xmin))
			ereport(ERROR,
					errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					errmsg("index \"%s\" cannot be verified using transaction snapshot",
						   RelationGetRelationName(rel)));
	}

	/*
	 * We need a snapshot to check the uniqueness of the index.  For better
	 * performance, take it once per index check.  If one was already taken
	 * above, use that.
	 *
	 * 我们需要一个快照来检查索引的唯一性。  为了获得更好的性能，请在每次索引检查时使用一次。  如果上面已经获取了一个，请使用它。
	 */
	if (state->checkunique)
	{
		state->indexinfo = BuildIndexInfo(state->rel);

		if (state->indexinfo->ii_Unique && state->snapshot == InvalidSnapshot)
			state->snapshot = RegisterSnapshot(GetTransactionSnapshot());
	}

	Assert(!state->rootdescend || state->readonly);
	if (state->rootdescend && !state->heapkeyspace)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot verify that tuples from index \"%s\" can each be found by an independent index search",
						RelationGetRelationName(rel)),
				 errhint("Only B-Tree version 4 indexes support rootdescend verification.")));

	/* Create context for page
	 *
	 * 为页面创建上下文
	 */
	state->targetcontext = AllocSetContextCreate(CurrentMemoryContext,
												 "amcheck context",
												 ALLOCSET_DEFAULT_SIZES);
	state->checkstrategy = GetAccessStrategy(BAS_BULKREAD);

	/* Get true root block from meta-page
	 *
	 * 从元页面获取真正的根块
	 */
	metapage = palloc_btree_page(state, BTREE_METAPAGE);
	metad = BTPageGetMeta(metapage);

	/*
	 * Certain deletion patterns can result in "skinny" B-Tree indexes, where
	 * the fast root and true root differ.
	 *
	 * 某些删除模式可能会导致“瘦”B 树索引，其中快速根和真实根不同。
	 *
	 * Start from the true root, not the fast root, unlike conventional index
	 * scans.  This approach is more thorough, and removes the risk of
	 * following a stale fast root from the meta page.
	 *
	 * 与传统索引扫描不同，从真正的根开始，而不是从快速根开始。  这种方法更加彻底，并且消除了从元页面跟踪过时的快速根的风险。
	 */
	if (metad->btm_fastroot != metad->btm_root)
		ereport(DEBUG1,
				(errcode(ERRCODE_NO_DATA),
				 errmsg_internal("harmless fast root mismatch in index \"%s\"",
								 RelationGetRelationName(rel)),
				 errdetail_internal("Fast root block %u (level %u) differs from true root block %u (level %u).",
									metad->btm_fastroot, metad->btm_fastlevel,
									metad->btm_root, metad->btm_level)));

	/*
	 * Starting at the root, verify every level.  Move left to right, top to
	 * bottom.  Note that there may be no pages other than the meta page (meta
	 * page can indicate that root is P_NONE when the index is totally empty).
	 *
	 * 从根源开始，验证每个级别。  从左到右、从上到下移动。  请注意，除了元页面之外，可能没有其他页面（当索引完全为空时，元页面可以指示根为 P_NONE）。
	 */
	previouslevel = InvalidBtreeLevel;
	current.level = metad->btm_level;
	current.leftmost = metad->btm_root;
	current.istruerootlevel = true;
	while (current.leftmost != P_NONE)
	{
		/*
		 * Verify this level, and get left most page for next level down, if
		 * not at leaf level
		 *
		 * 验证此级别，如果不在叶级别，则获取下一级的最左边页面
		 */
		current = bt_check_level_from_leftmost(state, current);

		if (current.leftmost == InvalidBlockNumber)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("index \"%s\" has no valid pages on level below %u or first level",
							RelationGetRelationName(rel), previouslevel)));

		previouslevel = current.level;
	}

	/*
	 * * Check whether heap contains unindexed/malformed tuples *
	 *
	 * * 检查堆中是否包含未索引/格式错误的元组 *
	 */
	if (state->heapallindexed)
	{
		IndexInfo  *indexinfo = BuildIndexInfo(state->rel);
		TableScanDesc scan;

		/*
		 * Create our own scan for table_index_build_scan(), rather than
		 * getting it to do so for us.  This is required so that we can
		 * actually use the MVCC snapshot registered earlier.
		 *
		 * 为 table_index_build_scan() 创建我们自己的扫描，而不是让它为我们执行此操作。  这是必需的，以便我们可以实际使用之前注册的 MVCC 快照。
		 *
		 * Note that table_index_build_scan() calls heap_endscan() for us.
		 *
		 * 请注意，table_index_build_scan() 为我们调用了heap_endscan()。
		 */
		scan = table_beginscan_strat(state->heaprel,	/* relation */
									 state->snapshot,	/* snapshot */
									 0, /* number of keys */
									 NULL,	/* scan key */
									 true,	/* buffer access strategy OK */
									 true); /* syncscan OK? */

		/*
		 * Scan will behave as the first scan of a CREATE INDEX CONCURRENTLY
		 * behaves.
		 *
		 * 扫描的行为将与 CREATE INDEX CONCURRENTLY 的第一次扫描的行为相同。
		 *
		 * It's okay that we don't actually use the same lock strength for the
		 * heap relation as any other ii_Concurrent caller would.  We have no
		 * reason to care about a concurrent VACUUM operation, since there
		 * isn't going to be a second scan of the heap that needs to be sure
		 * that there was no concurrent recycling of TIDs.
		 *
		 * 我们实际上并不像任何其他 ii_Concurrent 调用者那样对堆关系使用相同的锁定强度，这没关系。  我们没有理由关心并发 VACUUM 操作，因为不会对堆进行第二次扫描来确保没有并发回收 TID。
		 */
		indexinfo->ii_Concurrent = true;

		/*
		 * Don't wait for uncommitted tuple xact commit/abort when index is a
		 * unique index on a catalog (or an index used by an exclusion
		 * constraint).  This could otherwise happen in the readonly case.
		 *
		 * 当索引是目录上的唯一索引（或排除约束使用的索引）时，不要等待未提交的元组 xact 提交/中止。  否则，在只读情况下可能会发生这种情况。
		 */
		indexinfo->ii_Unique = false;
		indexinfo->ii_ExclusionOps = NULL;
		indexinfo->ii_ExclusionProcs = NULL;
		indexinfo->ii_ExclusionStrats = NULL;

		elog(DEBUG1, "verifying that tuples from index \"%s\" are present in \"%s\"",
			 RelationGetRelationName(state->rel),
			 RelationGetRelationName(state->heaprel));

		table_index_build_scan(state->heaprel, state->rel, indexinfo, true, false,
							   bt_tuple_present_callback, state, scan);

		ereport(DEBUG1,
				(errmsg_internal("finished verifying presence of " INT64_FORMAT " tuples from table \"%s\" with bitset %.2f%% set",
								 state->heaptuplespresent, RelationGetRelationName(heaprel),
								 100.0 * bloom_prop_bits_set(state->filter))));

		bloom_free(state->filter);
	}

	/* Be tidy:
	 *
	 * 保持整洁：
	 */
	if (state->snapshot != InvalidSnapshot)
		UnregisterSnapshot(state->snapshot);
	MemoryContextDelete(state->targetcontext);
}

/*
 * Given a left-most block at some level, move right, verifying each page
 * individually (with more verification across pages for "readonly"
 * callers).  Caller should pass the true root page as the leftmost initially,
 * working their way down by passing what is returned for the last call here
 * until level 0 (leaf page level) was reached.
 *
 * 给定某个级别的最左边的块，向右移动，单独验证每个页面（为“只读”调用者提供跨页面的更多验证）。  调用者应该首先将真正的根页面作为最左边传递，然后通过在此处传递最后一次调用返回的内容来向下工作，直到达到级别 0（叶页面级别）。
 *
 * Returns state for next call, if any.  This includes left-most block number
 * one level lower that should be passed on next level/call, which is set to
 * P_NONE on last call here (when leaf level is verified).  Level numbers
 * follow the nbtree convention: higher levels have higher numbers, because new
 * levels are added only due to a root page split.  Note that prior to the
 * first root page split, the root is also a leaf page, so there is always a
 * level 0 (leaf level), and it's always the last level processed.
 *
 * 返回下一次调用的状态（如果有）。  这包括最左边的块号低一级，应在下一级/调用中传递，在此处最后一次调用时设置为 P_NONE（当验证叶级别时）。  级别编号遵循 nbtree 约定：级别越高，编号越高，因为新级别仅由于根页面拆分而添加。  请注意，在第一次根页面拆分之前，根也是叶页面，因此始终存在级别 0（叶级别），并且它始终是处理的最后一个级别。
 *
 * Note on memory management:  State's per-page context is reset here, between
 * each call to bt_target_page_check().
 *
 * 关于内存管理的注意事项：在每次调用 bt_target_page_check() 之间，状态的每页上下文都会在这里重置。
 */
static BtreeLevel
bt_check_level_from_leftmost(BtreeCheckState *state, BtreeLevel level)
{
	/* State to establish early, concerning entire level
	 *
	 * 国家早日建立，关乎整个层面
	 */
	BTPageOpaque opaque;
	MemoryContext oldcontext;
	BtreeLevel	nextleveldown;

	/* Variables for iterating across level using right links
	 *
	 * 使用正确链接跨级别迭代的变量
	 */
	BlockNumber leftcurrent = P_NONE;
	BlockNumber current = level.leftmost;

	/* Initialize return state
	 *
	 * 初始化返回状态
	 */
	nextleveldown.leftmost = InvalidBlockNumber;
	nextleveldown.level = InvalidBtreeLevel;
	nextleveldown.istruerootlevel = false;

	/* Use page-level context for duration of this call
	 *
	 * 在此调用期间使用页面级上下文
	 */
	oldcontext = MemoryContextSwitchTo(state->targetcontext);

	elog(DEBUG1, "verifying level %u%s", level.level,
		 level.istruerootlevel ?
		 " (true root level)" : level.level == 0 ? " (leaf level)" : "");

	state->prevrightlink = InvalidBlockNumber;
	state->previncompletesplit = false;

	do
	{
		/* Don't rely on CHECK_FOR_INTERRUPTS() calls at lower level
		 *
		 * 不要依赖较低级别的 CHECK_FOR_INTERRUPTS() 调用
		 */
		CHECK_FOR_INTERRUPTS();

		/* Initialize state for this iteration
		 *
		 * 初始化本次迭代的状态
		 */
		state->targetblock = current;
		state->target = palloc_btree_page(state, state->targetblock);
		state->targetlsn = PageGetLSN(state->target);

		opaque = BTPageGetOpaque(state->target);

		if (P_IGNORE(opaque))
		{
			/*
			 * Since there cannot be a concurrent VACUUM operation in readonly
			 * mode, and since a page has no links within other pages
			 * (siblings and parent) once it is marked fully deleted, it
			 * should be impossible to land on a fully deleted page in
			 * readonly mode. See bt_child_check() for further details.
			 *
			 * 由于在只读模式下不能并发 VACUUM 操作，并且一旦页面被标记为完全删除，则页面在其他页面（同级页面和父页面）内就没有链接，因此不可能在只读模式下登陆完全删除的页面。有关更多详细信息，请参阅 bt_child_check()。
			 *
			 * The bt_child_check() P_ISDELETED() check is repeated here so
			 * that pages that are only reachable through sibling links get
			 * checked.
			 *
			 * 此处重复 bt_child_check() P_ISDELETED() 检查，以便检查只能通过同级链接访问的页面。
			 */
			if (state->readonly && P_ISDELETED(opaque))
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("downlink or sibling link points to deleted block in index \"%s\"",
								RelationGetRelationName(state->rel)),
						 errdetail_internal("Block=%u left block=%u left link from block=%u.",
											current, leftcurrent, opaque->btpo_prev)));

			if (P_RIGHTMOST(opaque))
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("block %u fell off the end of index \"%s\"",
								current, RelationGetRelationName(state->rel))));
			else
				ereport(DEBUG1,
						(errcode(ERRCODE_NO_DATA),
						 errmsg_internal("block %u of index \"%s\" concurrently deleted",
										 current, RelationGetRelationName(state->rel))));
			goto nextpage;
		}
		else if (nextleveldown.leftmost == InvalidBlockNumber)
		{
			/*
			 * A concurrent page split could make the caller supplied leftmost
			 * block no longer contain the leftmost page, or no longer be the
			 * true root, but where that isn't possible due to heavyweight
			 * locking, check that the first valid page meets caller's
			 * expectations.
			 *
			 * 并发页面分割可能会使调用者提供的最左边的块不再包含最左边的页面，或者不再是真正的根，但如果由于重量级锁定而无法做到这一点，请检查第一个有效页面是否满足调用者的期望。
			 */
			if (state->readonly)
			{
				if (!bt_leftmost_ignoring_half_dead(state, current, opaque))
					ereport(ERROR,
							(errcode(ERRCODE_INDEX_CORRUPTED),
							 errmsg("block %u is not leftmost in index \"%s\"",
									current, RelationGetRelationName(state->rel))));

				if (level.istruerootlevel && (!P_ISROOT(opaque) && !P_INCOMPLETE_SPLIT(opaque)))
					ereport(ERROR,
							(errcode(ERRCODE_INDEX_CORRUPTED),
							 errmsg("block %u is not true root in index \"%s\"",
									current, RelationGetRelationName(state->rel))));
			}

			/*
			 * Before beginning any non-trivial examination of level, prepare
			 * state for next bt_check_level_from_leftmost() invocation for
			 * the next level for the next level down (if any).
			 *
			 * 在开始任何重要的级别检查之前，为下一个级别（如果有）的下一个级别的下一个 bt_check_level_from_leftmost() 调用准备状态。
			 *
			 * There should be at least one non-ignorable page per level,
			 * unless this is the leaf level, which is assumed by caller to be
			 * final level.
			 *
			 * 每个级别至少应该有一个不可忽略的页面，除非这是叶级别（调用者假定它是最终级别）。
			 */
			if (!P_ISLEAF(opaque))
			{
				IndexTuple	itup;
				ItemId		itemid;

				/* Internal page -- downlink gets leftmost on next level
				 *
				 * 内部页面——下行链接位于下一级的最左边
				 */
				itemid = PageGetItemIdCareful(state, state->targetblock,
											  state->target,
											  P_FIRSTDATAKEY(opaque));
				itup = (IndexTuple) PageGetItem(state->target, itemid);
				nextleveldown.leftmost = BTreeTupleGetDownLink(itup);
				nextleveldown.level = opaque->btpo_level - 1;
			}
			else
			{
				/*
				 * Leaf page -- final level caller must process.
				 *
				 * 叶页——调用者必须处理的最后一级。
				 *
				 * Note that this could also be the root page, if there has
				 * been no root page split yet.
				 *
				 * 请注意，如果还没有根页面拆分，则这也可能是根页面。
				 */
				nextleveldown.leftmost = P_NONE;
				nextleveldown.level = InvalidBtreeLevel;
			}

			/*
			 * Finished setting up state for this call/level.  Control will
			 * never end up back here in any future loop iteration for this
			 * level.
			 *
			 * 已完成此调用/级别的状态设置。  在该级别的任何未来循环迭代中，控制永远不会回到这里。
			 */
		}

		/*
		 * Sibling links should be in mutual agreement.  There arises
		 * leftcurrent == P_NONE && btpo_prev != P_NONE when the left sibling
		 * of the parent's low-key downlink is half-dead.  (A half-dead page
		 * has no downlink from its parent.)  Under heavyweight locking, the
		 * last bt_leftmost_ignoring_half_dead() validated this btpo_prev.
		 * Without heavyweight locking, validation of the P_NONE case remains
		 * unimplemented.
		 *
		 * 兄弟姐妹的联系应该是相互同意的。  当父级低调下行链路的左兄弟处于半死状态时，就会出现 leftcurrent == P_NONE && btpo_prev != P_NONE。  （半死页没有来自其父级的下行链路。）在重量级锁定下，最后一个 bt_leftmost_ignoring_half_dead() 验证了此 btpo_prev。如果没有重量级锁定，P_NONE 情况的验证仍然无法实现。
		 */
		if (opaque->btpo_prev != leftcurrent && leftcurrent != P_NONE)
			bt_recheck_sibling_links(state, opaque->btpo_prev, leftcurrent);

		/* Check level
		 *
		 * 检查液位
		 */
		if (level.level != opaque->btpo_level)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("leftmost down link for level points to block in index \"%s\" whose level is not one level down",
							RelationGetRelationName(state->rel)),
					 errdetail_internal("Block pointed to=%u expected level=%u level in pointed to block=%u.",
										current, level.level, opaque->btpo_level)));

		/* Verify invariants for page
		 *
		 * 验证页面的不变量
		 */
		bt_target_page_check(state);

nextpage:

		/* Try to detect circular links
		 *
		 * 尝试检测循环链接
		 */
		if (current == leftcurrent || current == opaque->btpo_prev)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("circular link chain found in block %u of index \"%s\"",
							current, RelationGetRelationName(state->rel))));

		leftcurrent = current;
		current = opaque->btpo_next;

		if (state->lowkey)
		{
			Assert(state->readonly);
			pfree(state->lowkey);
			state->lowkey = NULL;
		}

		/*
		 * Copy current target high key as the low key of right sibling.
		 * Allocate memory in upper level context, so it would be cleared
		 * after reset of target context.
		 *
		 * 将当前目标高调复制为右兄弟的低音调。在上层上下文中分配内存，因此目标上下文重置后会被清除。
		 *
		 * We only need the low key in corner cases of checking child high
		 * keys. We use high key only when incomplete split on the child level
		 * falls to the boundary of pages on the target level.  See
		 * bt_child_highkey_check() for details.  So, typically we won't end
		 * up doing anything with low key, but it's simpler for general case
		 * high key verification to always have it available.
		 *
		 * 我们只在检查子高键的特殊情况下需要低键。仅当子级别上的不完全分割落在目标级别上的页面边界时，我们才使用高调。  有关详细信息，请参阅 bt_child_highkey_check()。  因此，通常我们最终不会以低调的方式做任何事情，但对于一般情况的高密钥验证来说，始终保持可用会更简单。
		 *
		 * The correctness of managing low key in the case of concurrent
		 * splits wasn't investigated yet.  Thankfully we only need low key
		 * for readonly verification and concurrent splits won't happen.
		 *
		 * 尚未研究在并发拆分情况下低调管理的正确性。  值得庆幸的是，我们只需要低调进行只读验证，并且不会发生并发拆分。
		 */
		if (state->readonly && !P_RIGHTMOST(opaque))
		{
			IndexTuple	itup;
			ItemId		itemid;

			itemid = PageGetItemIdCareful(state, state->targetblock,
										  state->target, P_HIKEY);
			itup = (IndexTuple) PageGetItem(state->target, itemid);

			state->lowkey = MemoryContextAlloc(oldcontext, IndexTupleSize(itup));
			memcpy(state->lowkey, itup, IndexTupleSize(itup));
		}

		/* Free page and associated memory for this iteration
		 *
		 * 本次迭代的可用页面和关联内存
		 */
		MemoryContextReset(state->targetcontext);
	}
	while (current != P_NONE);

	if (state->lowkey)
	{
		Assert(state->readonly);
		pfree(state->lowkey);
		state->lowkey = NULL;
	}

	/* Don't change context for caller
	 *
	 * 不要更改调用者的上下文
	 */
	MemoryContextSwitchTo(oldcontext);

	return nextleveldown;
}

/* Check visibility of the table entry referenced by nbtree index
 *
 * 检查 nbtree 索引引用的表条目的可见性
 */
static bool
heap_entry_is_visible(BtreeCheckState *state, ItemPointer tid)
{
	bool		tid_visible;

	TupleTableSlot *slot = table_slot_create(state->heaprel, NULL);

	tid_visible = table_tuple_fetch_row_version(state->heaprel,
												tid, state->snapshot, slot);
	if (slot != NULL)
		ExecDropSingleTupleTableSlot(slot);

	return tid_visible;
}

/*
 * Prepare an error message for unique constrain violation in
 * a btree index and report ERROR.
 *
 * 为 btree 索引中的唯一约束违规准备一条错误消息并报告错误。
 */
static void
bt_report_duplicate(BtreeCheckState *state,
					BtreeLastVisibleEntry *lVis,
					ItemPointer nexttid, BlockNumber nblock, OffsetNumber noffset,
					int nposting)
{
	char	   *htid,
			   *nhtid,
			   *itid,
			   *nitid = "",
			   *pposting = "",
			   *pnposting = "";

	htid = psprintf("tid=(%u,%u)",
					ItemPointerGetBlockNumberNoCheck(lVis->tid),
					ItemPointerGetOffsetNumberNoCheck(lVis->tid));
	nhtid = psprintf("tid=(%u,%u)",
					 ItemPointerGetBlockNumberNoCheck(nexttid),
					 ItemPointerGetOffsetNumberNoCheck(nexttid));
	itid = psprintf("tid=(%u,%u)", lVis->blkno, lVis->offset);

	if (nblock != lVis->blkno || noffset != lVis->offset)
		nitid = psprintf(" tid=(%u,%u)", nblock, noffset);

	if (lVis->postingIndex >= 0)
		pposting = psprintf(" posting %u", lVis->postingIndex);

	if (nposting >= 0)
		pnposting = psprintf(" posting %u", nposting);

	ereport(ERROR,
			(errcode(ERRCODE_INDEX_CORRUPTED),
			 errmsg("index uniqueness is violated for index \"%s\"",
					RelationGetRelationName(state->rel)),
			 errdetail("Index %s%s and%s%s (point to heap %s and %s) page lsn=%X/%X.",
					   itid, pposting, nitid, pnposting, htid, nhtid,
					   LSN_FORMAT_ARGS(state->targetlsn))));
}

/* Check if current nbtree leaf entry complies with UNIQUE constraint
 *
 * 检查当前 nbtree 叶子条目是否符合 UNIQUE 约束
 */
static void
bt_entry_unique_check(BtreeCheckState *state, IndexTuple itup,
					  BlockNumber targetblock, OffsetNumber offset,
					  BtreeLastVisibleEntry *lVis)
{
	ItemPointer tid;
	bool		has_visible_entry = false;

	Assert(targetblock != P_NONE);

	/*
	 * Current tuple has posting list. Report duplicate if TID of any posting
	 * list entry is visible and lVis->tid is valid.
	 *
	 * 当前元组有发布列表。如果任何发布列表条目的 TID 可见并且 lVis->tid 有效，则报告重复。
	 */
	if (BTreeTupleIsPosting(itup))
	{
		for (int i = 0; i < BTreeTupleGetNPosting(itup); i++)
		{
			tid = BTreeTupleGetPostingN(itup, i);
			if (heap_entry_is_visible(state, tid))
			{
				has_visible_entry = true;
				if (ItemPointerIsValid(lVis->tid))
				{
					bt_report_duplicate(state,
										lVis,
										tid, targetblock,
										offset, i);
				}

				/*
				 * Prevent double reporting unique constraint violation
				 * between the posting list entries of the first tuple on the
				 * page after cross-page check.
				 *
				 * 防止跨页检查后页面上第一个元组的发布列表条目之间发生重复报告唯一约束冲突。
				 */
				if (lVis->blkno != targetblock && ItemPointerIsValid(lVis->tid))
					return;

				lVis->blkno = targetblock;
				lVis->offset = offset;
				lVis->postingIndex = i;
				lVis->tid = tid;
			}
		}
	}

	/*
	 * Current tuple has no posting list. If TID is visible save info about it
	 * for the next comparisons in the loop in bt_target_page_check(). Report
	 * duplicate if lVis->tid is already valid.
	 *
	 * 当前元组没有发布列表。如果 TID 可见，则保存有关它的信息，以便在 bt_target_page_check() 循环中进行下一次比较。如果 lVis->tid 已经有效，则报告重复。
	 */
	else
	{
		tid = BTreeTupleGetHeapTID(itup);
		if (heap_entry_is_visible(state, tid))
		{
			has_visible_entry = true;
			if (ItemPointerIsValid(lVis->tid))
			{
				bt_report_duplicate(state,
									lVis,
									tid, targetblock,
									offset, -1);
			}

			lVis->blkno = targetblock;
			lVis->offset = offset;
			lVis->tid = tid;
			lVis->postingIndex = -1;
		}
	}

	if (!has_visible_entry &&
		lVis->blkno != InvalidBlockNumber &&
		lVis->blkno != targetblock)
	{
		char	   *posting = "";

		if (lVis->postingIndex >= 0)
			posting = psprintf(" posting %u", lVis->postingIndex);
		ereport(DEBUG1,
				(errcode(ERRCODE_NO_DATA),
				 errmsg("index uniqueness can not be checked for index tid=(%u,%u) in index \"%s\"",
						targetblock, offset,
						RelationGetRelationName(state->rel)),
				 errdetail("It doesn't have visible heap tids and key is equal to the tid=(%u,%u)%s (points to heap tid=(%u,%u)).",
						   lVis->blkno, lVis->offset, posting,
						   ItemPointerGetBlockNumberNoCheck(lVis->tid),
						   ItemPointerGetOffsetNumberNoCheck(lVis->tid)),
				 errhint("VACUUM the table and repeat the check.")));
	}
}

/*
 * Like P_LEFTMOST(start_opaque), but accept an arbitrarily-long chain of
 * half-dead, sibling-linked pages to the left.  If a half-dead page appears
 * under state->readonly, the database exited recovery between the first-stage
 * and second-stage WAL records of a deletion.
 *
 * 与 P_LEFTMOST(start_opaque) 类似，但接受左侧任意长的半死、同级链接页面链。  如果state->readonly下出现半死页，则数据库在删除的第一阶段和第二阶段WAL记录之间退出恢复。
 */
static bool
bt_leftmost_ignoring_half_dead(BtreeCheckState *state,
							   BlockNumber start,
							   BTPageOpaque start_opaque)
{
	BlockNumber reached = start_opaque->btpo_prev,
				reached_from = start;
	bool		all_half_dead = true;

	/*
	 * To handle the !readonly case, we'd need to accept BTP_DELETED pages and
	 * potentially observe nbtree/README "Page deletion and backwards scans".
	 *
	 * 为了处理 !readonly 情况，我们需要接受 BTP_DELETED 页面并可能观察 nbtree/README“页面删除和向后扫描”。
	 */
	Assert(state->readonly);

	while (reached != P_NONE && all_half_dead)
	{
		Page		page = palloc_btree_page(state, reached);
		BTPageOpaque reached_opaque = BTPageGetOpaque(page);

		CHECK_FOR_INTERRUPTS();

		/*
		 * Try to detect btpo_prev circular links.  _bt_unlink_halfdead_page()
		 * writes that side-links will continue to point to the siblings.
		 * Check btpo_next for that property.
		 *
		 * 尝试检测 btpo_prev 循环链接。  _bt_unlink_halfdead_page() 写入侧链接将继续指向同级。检查 btpo_next 的该属性。
		 */
		all_half_dead = P_ISHALFDEAD(reached_opaque) &&
			reached != start &&
			reached != reached_from &&
			reached_opaque->btpo_next == reached_from;
		if (all_half_dead)
		{
			XLogRecPtr	pagelsn = PageGetLSN(page);

			/* pagelsn should point to an XLOG_BTREE_MARK_PAGE_HALFDEAD
			 *
			 * pagelsn 应该指向 XLOG_BTREE_MARK_PAGE_HALFDEAD
			 */
			ereport(DEBUG1,
					(errcode(ERRCODE_NO_DATA),
					 errmsg_internal("harmless interrupted page deletion detected in index \"%s\"",
									 RelationGetRelationName(state->rel)),
					 errdetail_internal("Block=%u right block=%u page lsn=%X/%X.",
										reached, reached_from,
										LSN_FORMAT_ARGS(pagelsn))));

			reached_from = reached;
			reached = reached_opaque->btpo_prev;
		}

		pfree(page);
	}

	return all_half_dead;
}

/*
 * Raise an error when target page's left link does not point back to the
 * previous target page, called leftcurrent here.  The leftcurrent page's
 * right link was followed to get to the current target page, and we expect
 * mutual agreement among leftcurrent and the current target page.  Make sure
 * that this condition has definitely been violated in the !readonly case,
 * where concurrent page splits are something that we need to deal with.
 *
 * 当目标页面的左侧链接未指向上一个目标页面（此处称为 leftcurrent）时，会引发错误。  通过 leftcurrent 页面的右链接到达当前目标页面，我们期望 leftcurrent 和当前目标页面之间能够达成一致。  确保在 !readonly 情况下肯定违反了这个条件，在这种情况下，我们需要处理并发页面拆分。
 *
 * Cross-page inconsistencies involving pages that don't agree about being
 * siblings are known to be a particularly good indicator of corruption
 * involving partial writes/lost updates.  The bt_right_page_check_scankey
 * check also provides a way of detecting cross-page inconsistencies for
 * !readonly callers, but it can only detect sibling pages that have an
 * out-of-order keyspace, which can't catch many of the problems that we
 * expect to catch here.
 *
 * 众所周知，涉及不同意成为同级页面的跨页面不一致是涉及部分写入/丢失更新的损坏的特别好的指标。  bt_right_page_check_scankey 检查还为 !readonly 调用者提供了一种检测跨页面不一致的方法，但它只能检测具有无序键空间的同级页面，这无法捕获我们期望在这里捕获的许多问题。
 *
 * The classic example of the kind of inconsistency that we can only catch
 * with this check (when in !readonly mode) involves three sibling pages that
 * were affected by a faulty page split at some point in the past.  The
 * effects of the split are reflected in the original page and its new right
 * sibling page, with a lack of any accompanying changes for the _original_
 * right sibling page.  The original right sibling page's left link fails to
 * point to the new right sibling page (its left link still points to the
 * original page), even though the first phase of a page split is supposed to
 * work as a single atomic action.  This subtle inconsistency will probably
 * only break backwards scans in practice.
 *
 * 我们只能通过此检查（当处于 !readonly 模式时）捕获这种不一致的经典示例涉及三个同级页面，这些页面在过去的某个时刻受到错误页面拆分的影响。  拆分的效果反映在原始页面及其新的右同级页面中，而_原始_右同级页面没有任何伴随的更改。  原始右同级页面的左链接无法指向新的右同级页面（其左链接仍然指向原始页面），即使页面拆分的第一阶段应该作为单个原子操作工作。  这种微妙的不一致可能只会在实践中破坏向后扫描。
 *
 * Note that this is the only place where amcheck will "couple" buffer locks
 * (and only for !readonly callers).  In general we prefer to avoid more
 * thorough cross-page checks in !readonly mode, but it seems worth the
 * complexity here.  Also, the performance overhead of performing lock
 * coupling here is negligible in practice.  Control only reaches here with a
 * non-corrupt index when there is a concurrent page split at the instant
 * caller crossed over to target page from leftcurrent page.
 *
 * 请注意，这是 amcheck 将“耦合”缓冲区锁的唯一位置（并且仅适用于 !readonly 调用者）。  一般来说，我们更喜欢避免在 !readonly 模式下进行更彻底的跨页检查，但这里的复杂性似乎值得。  而且，这里执行锁耦合的性能开销在实践中可以忽略不计。  仅当即时调用者从左当前页面交叉到目标页面时存在并发页面拆分时，控制才会以未损坏的索引到达此处。
 */
static void
bt_recheck_sibling_links(BtreeCheckState *state,
						 BlockNumber btpo_prev_from_target,
						 BlockNumber leftcurrent)
{
	/* passing metapage to BTPageGetOpaque() would give irrelevant findings
	 *
	 * 将元页面传递给 BTPageGetOpaque() 会给出不相关的结果
	 */
	Assert(leftcurrent != P_NONE);

	if (!state->readonly)
	{
		Buffer		lbuf;
		Buffer		newtargetbuf;
		Page		page;
		BTPageOpaque opaque;
		BlockNumber newtargetblock;

		/* Couple locks in the usual order for nbtree:  Left to right
		 *
		 * 按照 nbtree 通常的顺序对锁：从左到右
		 */
		lbuf = ReadBufferExtended(state->rel, MAIN_FORKNUM, leftcurrent,
								  RBM_NORMAL, state->checkstrategy);
		LockBuffer(lbuf, BT_READ);
		_bt_checkpage(state->rel, lbuf);
		page = BufferGetPage(lbuf);
		opaque = BTPageGetOpaque(page);
		if (P_ISDELETED(opaque))
		{
			/*
			 * Cannot reason about concurrently deleted page -- the left link
			 * in the page to the right is expected to point to some other
			 * page to the left (not leftcurrent page).
			 *
			 * 无法推断同时删除的页面 - 右侧页面中的左侧链接预计会指向左侧的某个其他页面（不是左侧当前页面）。
			 *
			 * Note that we deliberately don't give up with a half-dead page.
			 *
			 * 请注意，我们故意不放弃半死的页面。
			 */
			UnlockReleaseBuffer(lbuf);
			return;
		}

		newtargetblock = opaque->btpo_next;
		/* Avoid self-deadlock when newtargetblock == leftcurrent
		 *
		 * 当 newtargetblock == leftcurrent 时避免自死锁
		 */
		if (newtargetblock != leftcurrent)
		{
			newtargetbuf = ReadBufferExtended(state->rel, MAIN_FORKNUM,
											  newtargetblock, RBM_NORMAL,
											  state->checkstrategy);
			LockBuffer(newtargetbuf, BT_READ);
			_bt_checkpage(state->rel, newtargetbuf);
			page = BufferGetPage(newtargetbuf);
			opaque = BTPageGetOpaque(page);
			/* btpo_prev_from_target may have changed; update it
			 *
			 * btpo_prev_from_target 可能已更改；更新它
			 */
			btpo_prev_from_target = opaque->btpo_prev;
		}
		else
		{
			/*
			 * leftcurrent right sibling points back to leftcurrent block.
			 * Index is corrupt.  Easiest way to handle this is to pretend
			 * that we actually read from a distinct page that has an invalid
			 * block number in its btpo_prev.
			 *
			 * leftcurrent 右兄弟指向 leftcurrent 块。索引已损坏。  处理这个问题的最简单方法是假装我们实际上是从一个不同的页面读取数据，该页面的 btpo_prev 中的块号无效。
			 */
			newtargetbuf = InvalidBuffer;
			btpo_prev_from_target = InvalidBlockNumber;
		}

		/*
		 * No need to check P_ISDELETED here, since new target block cannot be
		 * marked deleted as long as we hold a lock on lbuf
		 *
		 * 这里不需要检查 P_ISDELETED，因为只要我们锁定 lbuf，新的目标块就不能被标记为已删除
		 */
		if (BufferIsValid(newtargetbuf))
			UnlockReleaseBuffer(newtargetbuf);
		UnlockReleaseBuffer(lbuf);

		if (btpo_prev_from_target == leftcurrent)
		{
			/* Report split in left sibling, not target (or new target)
			 *
			 * 报告左兄弟的拆分，而不是目标（或新目标）
			 */
			ereport(DEBUG1,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg_internal("harmless concurrent page split detected in index \"%s\"",
									 RelationGetRelationName(state->rel)),
					 errdetail_internal("Block=%u new right sibling=%u original right sibling=%u.",
										leftcurrent, newtargetblock,
										state->targetblock)));
			return;
		}

		/*
		 * Index is corrupt.  Make sure that we report correct target page.
		 *
		 * 索引已损坏。  确保我们报告正确的目标页面。
		 *
		 * This could have changed in cases where there was a concurrent page
		 * split, as well as index corruption (at least in theory).  Note that
		 * btpo_prev_from_target was already updated above.
		 *
		 * 如果存在并发页面拆分以及索引损坏（至少在理论上），这种情况可能会发生变化。  请注意，btpo_prev_from_target 已在上面更新。
		 */
		state->targetblock = newtargetblock;
	}

	ereport(ERROR,
			(errcode(ERRCODE_INDEX_CORRUPTED),
			 errmsg("left link/right link pair in index \"%s\" not in agreement",
					RelationGetRelationName(state->rel)),
			 errdetail_internal("Block=%u left block=%u left link from block=%u.",
								state->targetblock, leftcurrent,
								btpo_prev_from_target)));
}

/*
 * Function performs the following checks on target page, or pages ancillary to
 * target page:
 *
 * 函数对目标页面或目标页面的辅助页面执行以下检查：
 *
 * - That every "real" data item is less than or equal to the high key, which
 *	 is an upper bound on the items on the page.  Data items should be
 *	 strictly less than the high key when the page is an internal page.
 *
 * - 每个“真实”数据项都小于或等于高键，这是页面上的项目的上限。  当页面是内部页面时，数据项应严格小于高键。
 *
 * - That within the page, every data item is strictly less than the item
 *	 immediately to its right, if any (i.e., that the items are in order
 *	 within the page, so that the binary searches performed by index scans are
 *	 sane).
 *
 * - 在页面内，每个数据项都严格小于紧邻其右侧的项（如果有）（即，这些项在页面内按顺序排列，以便索引扫描执行的二进制搜索是合理的）。
 *
 * - That the last data item stored on the page is strictly less than the
 *	 first data item on the page to the right (when such a first item is
 *	 available).
 *
 * - 页面上存储的最后一个数据项严格小于右侧页面上的第一个数据项（当第一个数据项可用时）。
 *
 * - Various checks on the structure of tuples themselves.  For example, check
 *	 that non-pivot tuples have no truncated attributes.
 *
 * - 对元组本身结构的各种检查。  例如，检查非主元元组是否没有被截断的属性。
 *
 * - For index with unique constraint make sure that only one of table entries
 *   for equal keys is visible.
 *
 * - 对于具有唯一约束的索引，请确保只有相等键的表条目之一可见。
 *
 * Furthermore, when state passed shows ShareLock held, function also checks:
 *
 * 此外，当传递的状态显示 ShareLock 已持有时，函数还会检查：
 *
 * - That all child pages respect strict lower bound from parent's pivot
 *	 tuple.
 *
 * - 所有子页面均遵守父级数据透视元组的严格下限。
 *
 * - That downlink to block was encountered in parent where that's expected.
 *
 * - 在父级中遇到了预期的下行链路阻止。
 *
 * - That high keys of child pages matches corresponding pivot keys in parent.
 *
 * - 子页面的高键与父页面中相应的枢轴键相匹配。
 *
 * This is also where heapallindexed callers use their Bloom filter to
 * fingerprint IndexTuples for later table_index_build_scan() verification.
 *
 * 这也是 heapallindexed 调用者使用其 Bloom 过滤器对 IndexTuples 进行指纹识别以供后续 table_index_build_scan() 验证的地方。
 *
 * Note:  Memory allocated in this routine is expected to be released by caller
 * resetting state->targetcontext.
 *
 * 注意：在此例程中分配的内存预计将通过调用者重置 state->targetcontext 来释放。
 */
static void
bt_target_page_check(BtreeCheckState *state)
{
	OffsetNumber offset;
	OffsetNumber max;
	BTPageOpaque topaque;

	/* Last visible entry info for checking indexes with unique constraint
	 *
	 * 用于检查具有唯一约束的索引的最后可见条目信息
	 */
	BtreeLastVisibleEntry lVis = {InvalidBlockNumber, InvalidOffsetNumber, -1, NULL};

	topaque = BTPageGetOpaque(state->target);
	max = PageGetMaxOffsetNumber(state->target);

	elog(DEBUG2, "verifying %u items on %s block %u", max,
		 P_ISLEAF(topaque) ? "leaf" : "internal", state->targetblock);

	/*
	 * Check the number of attributes in high key. Note, rightmost page
	 * doesn't contain a high key, so nothing to check
	 *
	 * 检查高调属性的数量。请注意，最右边的页面不包含高调，因此无需检查
	 */
	if (!P_RIGHTMOST(topaque))
	{
		ItemId		itemid;
		IndexTuple	itup;

		/* Verify line pointer before checking tuple
		 *
		 * 在检查元组之前验证行指针
		 */
		itemid = PageGetItemIdCareful(state, state->targetblock,
									  state->target, P_HIKEY);
		if (!_bt_check_natts(state->rel, state->heapkeyspace, state->target,
							 P_HIKEY))
		{
			itup = (IndexTuple) PageGetItem(state->target, itemid);
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("wrong number of high key index tuple attributes in index \"%s\"",
							RelationGetRelationName(state->rel)),
					 errdetail_internal("Index block=%u natts=%u block type=%s page lsn=%X/%X.",
										state->targetblock,
										BTreeTupleGetNAtts(itup, state->rel),
										P_ISLEAF(topaque) ? "heap" : "index",
										LSN_FORMAT_ARGS(state->targetlsn))));
		}
	}

	/*
	 * Loop over page items, starting from first non-highkey item, not high
	 * key (if any).  Most tests are not performed for the "negative infinity"
	 * real item (if any).
	 *
	 * 从第一个非高调项目开始循环页面项目，而不是高调项目（如果有）。  大多数测试不是针对“负无穷”真实项（如果有）执行的。
	 */
	for (offset = P_FIRSTDATAKEY(topaque);
		 offset <= max;
		 offset = OffsetNumberNext(offset))
	{
		ItemId		itemid;
		IndexTuple	itup;
		size_t		tupsize;
		BTScanInsert skey;
		bool		lowersizelimit;
		ItemPointer scantid;

		/*
		 * True if we already called bt_entry_unique_check() for the current
		 * item.  This helps to avoid visiting the heap for keys, which are
		 * anyway presented only once and can't comprise a unique violation.
		 *
		 * 如果我们已经为当前项目调用了 bt_entry_unique_check()，则为 true。  这有助于避免访问堆中的键，无论如何，这些键仅出现一次并且不能构成唯一的违规。
		 */
		bool		unique_checked = false;

		CHECK_FOR_INTERRUPTS();

		itemid = PageGetItemIdCareful(state, state->targetblock,
									  state->target, offset);
		itup = (IndexTuple) PageGetItem(state->target, itemid);
		tupsize = IndexTupleSize(itup);

		/*
		 * lp_len should match the IndexTuple reported length exactly, since
		 * lp_len is completely redundant in indexes, and both sources of
		 * tuple length are MAXALIGN()'d.  nbtree does not use lp_len all that
		 * frequently, and is surprisingly tolerant of corrupt lp_len fields.
		 *
		 * lp_len 应该与 IndexTuple 报告的长度完全匹配，因为 lp_len 在索引中是完全冗余的，并且元组长度的两个来源都是 MAXALIGN()'d。  nbtree 不那么频繁地使用 lp_len，并且令人惊讶地容忍损坏的 lp_len 字段。
		 */
		if (tupsize != ItemIdGetLength(itemid))
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("index tuple size does not equal lp_len in index \"%s\"",
							RelationGetRelationName(state->rel)),
					 errdetail_internal("Index tid=(%u,%u) tuple size=%zu lp_len=%u page lsn=%X/%X.",
										state->targetblock, offset,
										tupsize, ItemIdGetLength(itemid),
										LSN_FORMAT_ARGS(state->targetlsn)),
					 errhint("This could be a torn page problem.")));

		/* Check the number of index tuple attributes
		 *
		 * 检查索引元组属性的数量
		 */
		if (!_bt_check_natts(state->rel, state->heapkeyspace, state->target,
							 offset))
		{
			ItemPointer tid;
			char	   *itid,
					   *htid;

			itid = psprintf("(%u,%u)", state->targetblock, offset);
			tid = BTreeTupleGetPointsToTID(itup);
			htid = psprintf("(%u,%u)",
							ItemPointerGetBlockNumberNoCheck(tid),
							ItemPointerGetOffsetNumberNoCheck(tid));

			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("wrong number of index tuple attributes in index \"%s\"",
							RelationGetRelationName(state->rel)),
					 errdetail_internal("Index tid=%s natts=%u points to %s tid=%s page lsn=%X/%X.",
										itid,
										BTreeTupleGetNAtts(itup, state->rel),
										P_ISLEAF(topaque) ? "heap" : "index",
										htid,
										LSN_FORMAT_ARGS(state->targetlsn))));
		}

		/*
		 * Don't try to generate scankey using "negative infinity" item on
		 * internal pages. They are always truncated to zero attributes.
		 *
		 * 不要尝试在内部页面上使用“负无穷大”项生成扫描键。它们总是被截断为零属性。
		 */
		if (offset_is_negative_infinity(topaque, offset))
		{
			/*
			 * We don't call bt_child_check() for "negative infinity" items.
			 * But if we're performing downlink connectivity check, we do it
			 * for every item including "negative infinity" one.
			 *
			 * 我们不会为“负无穷”项调用 bt_child_check()。但是，如果我们要执行下行链路连接检查，我们会对每一项进行检查，包括“负无穷大”项。
			 */
			if (!P_ISLEAF(topaque) && state->readonly)
			{
				bt_child_highkey_check(state,
									   offset,
									   NULL,
									   topaque->btpo_level);
			}
			continue;
		}

		/*
		 * Readonly callers may optionally verify that non-pivot tuples can
		 * each be found by an independent search that starts from the root.
		 * Note that we deliberately don't do individual searches for each
		 * TID, since the posting list itself is validated by other checks.
		 *
		 * 只读调用者可以选择验证非枢轴元组是否可以通过从根开始的独立搜索找到。请注意，我们故意不对每个 TID 进行单独搜索，因为发布列表本身是通过其他检查进行验证的。
		 */
		if (state->rootdescend && P_ISLEAF(topaque) &&
			!bt_rootdescend(state, itup))
		{
			ItemPointer tid = BTreeTupleGetPointsToTID(itup);
			char	   *itid,
					   *htid;

			itid = psprintf("(%u,%u)", state->targetblock, offset);
			htid = psprintf("(%u,%u)", ItemPointerGetBlockNumber(tid),
							ItemPointerGetOffsetNumber(tid));

			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("could not find tuple using search from root page in index \"%s\"",
							RelationGetRelationName(state->rel)),
					 errdetail_internal("Index tid=%s points to heap tid=%s page lsn=%X/%X.",
										itid, htid,
										LSN_FORMAT_ARGS(state->targetlsn))));
		}

		/*
		 * If tuple is a posting list tuple, make sure posting list TIDs are
		 * in order
		 *
		 * 如果元组是发布列表元组，请确保发布列表 TID 有序
		 */
		if (BTreeTupleIsPosting(itup))
		{
			ItemPointerData last;
			ItemPointer current;

			ItemPointerCopy(BTreeTupleGetHeapTID(itup), &last);

			for (int i = 1; i < BTreeTupleGetNPosting(itup); i++)
			{

				current = BTreeTupleGetPostingN(itup, i);

				if (ItemPointerCompare(current, &last) <= 0)
				{
					char	   *itid = psprintf("(%u,%u)", state->targetblock, offset);

					ereport(ERROR,
							(errcode(ERRCODE_INDEX_CORRUPTED),
							 errmsg_internal("posting list contains misplaced TID in index \"%s\"",
											 RelationGetRelationName(state->rel)),
							 errdetail_internal("Index tid=%s posting list offset=%d page lsn=%X/%X.",
												itid, i,
												LSN_FORMAT_ARGS(state->targetlsn))));
				}

				ItemPointerCopy(current, &last);
			}
		}

		/* Build insertion scankey for current page offset
		 *
		 * 为当前页面偏移量构建插入扫描键
		 */
		skey = bt_mkscankey_pivotsearch(state->rel, itup);

		/*
		 * Make sure tuple size does not exceed the relevant BTREE_VERSION
		 * specific limit.
		 *
		 * 确保元组大小不超过相关 BTREE_VERSION 特定限制。
		 *
		 * BTREE_VERSION 4 (which introduced heapkeyspace rules) requisitioned
		 * a small amount of space from BTMaxItemSize() in order to ensure
		 * that suffix truncation always has enough space to add an explicit
		 * heap TID back to a tuple -- we pessimistically assume that every
		 * newly inserted tuple will eventually need to have a heap TID
		 * appended during a future leaf page split, when the tuple becomes
		 * the basis of the new high key (pivot tuple) for the leaf page.
		 *
		 * BTREE_VERSION 4（引入了堆键空间规则）从 BTMaxItemSize() 中申请了少量空间，以确保后缀截断始终有足够的空间将显式堆 TID 添加回元组 - 我们悲观地假设每个新插入的元组最终需要在未来的叶页拆分期间附加堆 TID，此时该元组成为新的高键（枢轴）的基础元组）用于叶页。
		 *
		 * Since the reclaimed space is reserved for that purpose, we must not
		 * enforce the slightly lower limit when the extra space has been used
		 * as intended.  In other words, there is only a cross-version
		 * difference in the limit on tuple size within leaf pages.
		 *
		 * 由于回收的空间是为此目的而保留的，因此当额外空间已按预期使用时，我们不得强制执行稍低的限制。  换句话说，叶页内元组大小的限制仅存在跨版本差异。
		 *
		 * Still, we're particular about the details within BTREE_VERSION 4
		 * internal pages.  Pivot tuples may only use the extra space for its
		 * designated purpose.  Enforce the lower limit for pivot tuples when
		 * an explicit heap TID isn't actually present. (In all other cases
		 * suffix truncation is guaranteed to generate a pivot tuple that's no
		 * larger than the firstright tuple provided to it by its caller.)
		 *
		 * 尽管如此，我们还是特别关注 BTREE_VERSION 4 内部页面的细节。  枢轴元组只能将额外空间用于其指定目的。  当显式堆 TID 实际上不存在时，强制执行枢轴元组的下限。 （在所有其他情况下，后缀截断保证生成一个不大于调用者提供给它的第一个右元组的主元组。）
		 */
		lowersizelimit = skey->heapkeyspace &&
			(P_ISLEAF(topaque) || BTreeTupleGetHeapTID(itup) == NULL);
		if (tupsize > (lowersizelimit ? BTMaxItemSize : BTMaxItemSizeNoHeapTid))
		{
			ItemPointer tid = BTreeTupleGetPointsToTID(itup);
			char	   *itid,
					   *htid;

			itid = psprintf("(%u,%u)", state->targetblock, offset);
			htid = psprintf("(%u,%u)",
							ItemPointerGetBlockNumberNoCheck(tid),
							ItemPointerGetOffsetNumberNoCheck(tid));

			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("index row size %zu exceeds maximum for index \"%s\"",
							tupsize, RelationGetRelationName(state->rel)),
					 errdetail_internal("Index tid=%s points to %s tid=%s page lsn=%X/%X.",
										itid,
										P_ISLEAF(topaque) ? "heap" : "index",
										htid,
										LSN_FORMAT_ARGS(state->targetlsn))));
		}

		/* Fingerprint leaf page tuples (those that point to the heap)
		 *
		 * 指纹叶页元组（指向堆的元组）
		 */
		if (state->heapallindexed && P_ISLEAF(topaque) && !ItemIdIsDead(itemid))
		{
			IndexTuple	norm;

			if (BTreeTupleIsPosting(itup))
			{
				/* Fingerprint all elements as distinct "plain" tuples
				 *
				 * 将所有元素指纹识别为不同的“普通”元组
				 */
				for (int i = 0; i < BTreeTupleGetNPosting(itup); i++)
				{
					IndexTuple	logtuple;

					logtuple = bt_posting_plain_tuple(itup, i);
					norm = bt_normalize_tuple(state, logtuple);
					bloom_add_element(state->filter, (unsigned char *) norm,
									  IndexTupleSize(norm));
					/* Be tidy
					 *
					 * 保持整洁
					 */
					if (norm != logtuple)
						pfree(norm);
					pfree(logtuple);
				}
			}
			else
			{
				norm = bt_normalize_tuple(state, itup);
				bloom_add_element(state->filter, (unsigned char *) norm,
								  IndexTupleSize(norm));
				/* Be tidy
				 *
				 * 保持整洁
				 */
				if (norm != itup)
					pfree(norm);
			}
		}

		/*
		 * * High key check *
		 *
		 * *高调检查*
		 *
		 * If there is a high key (if this is not the rightmost page on its
		 * entire level), check that high key actually is upper bound on all
		 * page items.  If this is a posting list tuple, we'll need to set
		 * scantid to be highest TID in posting list.
		 *
		 * 如果存在高键（如果这不是整个级别的最右侧页面），请检查高键是否实际上是所有页面项目的上限。  如果这是一个发布列表元组，我们需要将 scantid 设置为发布列表中的最高 TID。
		 *
		 * We prefer to check all items against high key rather than checking
		 * just the last and trusting that the operator class obeys the
		 * transitive law (which implies that all previous items also
		 * respected the high key invariant if they pass the item order
		 * check).
		 *
		 * 我们更喜欢根据高键检查所有项目，而不是仅检查最后一个项目并相信运算符类遵守传递律（这意味着所有先前的项目如果通过了项目顺序检查，也遵循高键不变量）。
		 *
		 * Ideally, we'd compare every item in the index against every other
		 * item in the index, and not trust opclass obedience of the
		 * transitive law to bridge the gap between children and their
		 * grandparents (as well as great-grandparents, and so on).  We don't
		 * go to those lengths because that would be prohibitively expensive,
		 * and probably not markedly more effective in practice.
		 *
		 * 理想情况下，我们会将索引中的每个项目与索引中的每个其他项目进行比较，并且不相信opclass遵守传递律来弥合儿童与其祖父母（以及曾祖父母等）之间的差距。  我们不会这样做，因为这会非常昂贵，而且在实践中可能不会明显更有​​效。
		 *
		 * On the leaf level, we check that the key is <= the highkey.
		 * However, on non-leaf levels we check that the key is < the highkey,
		 * because the high key is "just another separator" rather than a copy
		 * of some existing key item; we expect it to be unique among all keys
		 * on the same level.  (Suffix truncation will sometimes produce a
		 * leaf highkey that is an untruncated copy of the lastleft item, but
		 * never any other item, which necessitates weakening the leaf level
		 * check to <=.)
		 *
		 * 在叶级别，我们检查键是否 <= 高键。然而，在非叶级别上，我们检查键是否<高键，因为高键“只是另一个分隔符”而不是某些现有键项的副本；我们希望它在同一级别的所有键中是唯一的。  （后缀截断有时会产生一个叶高键，它是最后一个项目的未截断副本，但不会产生任何其他项目，这需要将叶级别检查弱化为 <=。）
		 *
		 * Full explanation for why a highkey is never truly a copy of another
		 * item from the same level on internal levels:
		 *
		 * 完整解释为什么 highkey 永远不会真正是内部级别上同一级别的另一个项目的副本：
		 *
		 * While the new left page's high key is copied from the first offset
		 * on the right page during an internal page split, that's not the
		 * full story.  In effect, internal pages are split in the middle of
		 * the firstright tuple, not between the would-be lastleft and
		 * firstright tuples: the firstright key ends up on the left side as
		 * left's new highkey, and the firstright downlink ends up on the
		 * right side as right's new "negative infinity" item.  The negative
		 * infinity tuple is truncated to zero attributes, so we're only left
		 * with the downlink.  In other words, the copying is just an
		 * implementation detail of splitting in the middle of a (pivot)
		 * tuple. (See also: "Notes About Data Representation" in the nbtree
		 * README.)
		 *
		 * 虽然新左页的高调是在内部页面拆分期间从右页上的第一个偏移复制的，但这并不是完整的故事。  实际上，内部页面在firstright元组的中间分割，而不是在可能的lastleft和firstright元组之间分割：firstright键最终在左侧作为左的新高键，而firstright下行链路最终在右侧作为右新的“负无穷大”项。  负无穷元组被截断为零属性，因此我们只剩下下行链路。  换句话说，复制只是（枢轴）元组中间分裂的实现细节。 （另请参阅：nbtree 自述文件中的“关于数据表示的注释”。）
		 */
		scantid = skey->scantid;
		if (state->heapkeyspace && BTreeTupleIsPosting(itup))
			skey->scantid = BTreeTupleGetMaxHeapTID(itup);

		if (!P_RIGHTMOST(topaque) &&
			!(P_ISLEAF(topaque) ? invariant_leq_offset(state, skey, P_HIKEY) :
			  invariant_l_offset(state, skey, P_HIKEY)))
		{
			ItemPointer tid = BTreeTupleGetPointsToTID(itup);
			char	   *itid,
					   *htid;

			itid = psprintf("(%u,%u)", state->targetblock, offset);
			htid = psprintf("(%u,%u)",
							ItemPointerGetBlockNumberNoCheck(tid),
							ItemPointerGetOffsetNumberNoCheck(tid));

			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("high key invariant violated for index \"%s\"",
							RelationGetRelationName(state->rel)),
					 errdetail_internal("Index tid=%s points to %s tid=%s page lsn=%X/%X.",
										itid,
										P_ISLEAF(topaque) ? "heap" : "index",
										htid,
										LSN_FORMAT_ARGS(state->targetlsn))));
		}
		/* Reset, in case scantid was set to (itup) posting tuple's max TID
		 *
		 * 重置，以防 scantid 设置为 (itup) 发布元组的最大 TID
		 */
		skey->scantid = scantid;

		/*
		 * * Item order check *
		 *
		 * * 商品订单检查 *
		 *
		 * Check that items are stored on page in logical order, by checking
		 * current item is strictly less than next item (if any).
		 *
		 * 通过检查当前项目严格小于下一个项目（如果有），检查项目是否按逻辑顺序存储在页面上。
		 */
		if (OffsetNumberNext(offset) <= max &&
			!invariant_l_offset(state, skey, OffsetNumberNext(offset)))
		{
			ItemPointer tid;
			char	   *itid,
					   *htid,
					   *nitid,
					   *nhtid;

			itid = psprintf("(%u,%u)", state->targetblock, offset);
			tid = BTreeTupleGetPointsToTID(itup);
			htid = psprintf("(%u,%u)",
							ItemPointerGetBlockNumberNoCheck(tid),
							ItemPointerGetOffsetNumberNoCheck(tid));
			nitid = psprintf("(%u,%u)", state->targetblock,
							 OffsetNumberNext(offset));

			/* Reuse itup to get pointed-to heap location of second item
			 *
			 * 重用 itup 来获取第二项的指向堆位置
			 */
			itemid = PageGetItemIdCareful(state, state->targetblock,
										  state->target,
										  OffsetNumberNext(offset));
			itup = (IndexTuple) PageGetItem(state->target, itemid);
			tid = BTreeTupleGetPointsToTID(itup);
			nhtid = psprintf("(%u,%u)",
							 ItemPointerGetBlockNumberNoCheck(tid),
							 ItemPointerGetOffsetNumberNoCheck(tid));

			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("item order invariant violated for index \"%s\"",
							RelationGetRelationName(state->rel)),
					 errdetail_internal("Lower index tid=%s (points to %s tid=%s) "
										"higher index tid=%s (points to %s tid=%s) "
										"page lsn=%X/%X.",
										itid,
										P_ISLEAF(topaque) ? "heap" : "index",
										htid,
										nitid,
										P_ISLEAF(topaque) ? "heap" : "index",
										nhtid,
										LSN_FORMAT_ARGS(state->targetlsn))));
		}

		/*
		 * If the index is unique verify entries uniqueness by checking the
		 * heap tuples visibility.  Immediately check posting tuples and
		 * tuples with repeated keys.  Postpone check for keys, which have the
		 * first appearance.
		 *
		 * 如果索引是唯一的，则通过检查堆元组可见性来验证条目的唯一性。  立即检查发布元组和具有重复键的元组。  推迟检查首次出现的密钥。
		 */
		if (state->checkunique && state->indexinfo->ii_Unique &&
			P_ISLEAF(topaque) && !skey->anynullkeys &&
			(BTreeTupleIsPosting(itup) || ItemPointerIsValid(lVis.tid)))
		{
			bt_entry_unique_check(state, itup, state->targetblock, offset,
								  &lVis);
			unique_checked = true;
		}

		if (state->checkunique && state->indexinfo->ii_Unique &&
			P_ISLEAF(topaque) && OffsetNumberNext(offset) <= max)
		{
			/* Save current scankey tid
			 *
			 * 保存当前扫描键 tid
			 */
			scantid = skey->scantid;

			/*
			 * Invalidate scankey tid to make _bt_compare compare only keys in
			 * the item to report equality even if heap TIDs are different
			 *
			 * 使 scankey tid 无效以使 _bt_compare 仅比较项目中的键以报告相等性，即使堆 TID 不同
			 */
			skey->scantid = NULL;

			/*
			 * If next key tuple is different, invalidate last visible entry
			 * data (whole index tuple or last posting in index tuple). Key
			 * containing null value does not violate unique constraint and
			 * treated as different to any other key.
			 *
			 * 如果下一个键元组不同，则使最后一个可见条目数据（整个索引元组或索引元组中的最后发布）无效。包含空值的键不违反唯一约束，并且被视为与任何其他键不同。
			 *
			 * If the next key is the same as the previous one, do the
			 * bt_entry_unique_check() call if it was postponed.
			 *
			 * 如果下一个键与前一个键相同，则执行 bt_entry_unique_check() 调用（如果已推迟）。
			 */
			if (_bt_compare(state->rel, skey, state->target,
							OffsetNumberNext(offset)) != 0 || skey->anynullkeys)
			{
				lVis.blkno = InvalidBlockNumber;
				lVis.offset = InvalidOffsetNumber;
				lVis.postingIndex = -1;
				lVis.tid = NULL;
			}
			else if (!unique_checked)
			{
				bt_entry_unique_check(state, itup, state->targetblock, offset,
									  &lVis);
			}
			skey->scantid = scantid;	/* Restore saved scan key state */
		}

		/*
		 * * Last item check *
		 *
		 * *最后一项检查*
		 *
		 * Check last item against next/right page's first data item's when
		 * last item on page is reached.  This additional check will detect
		 * transposed pages iff the supposed right sibling page happens to
		 * belong before target in the key space.  (Otherwise, a subsequent
		 * heap verification will probably detect the problem.)
		 *
		 * 当到达页面上的最后一项时，对照下一页/右页的第一个数据项检查最后一项。  如果假设的右兄弟页面恰好属于键空间中的目标之前，则此附加检查将检测转置页面。  （否则，后续的堆验证可能会检测到该问题。）
		 *
		 * This check is similar to the item order check that will have
		 * already been performed for every other "real" item on target page
		 * when last item is checked.  The difference is that the next item
		 * (the item that is compared to target's last item) needs to come
		 * from the next/sibling page.  There may not be such an item
		 * available from sibling for various reasons, though (e.g., target is
		 * the rightmost page on level).
		 *
		 * 此检查类似于项目顺序检查，当检查最后一个项目时，已经对目标页面上的每个其他“真实”项目执行了该检查。  不同之处在于下一个项目（与目标的最后一个项目进行比较的项目）需要来自下一个/同级页面。  不过，由于各种原因，同级可能无法提供这样的项目（例如，目标是级别上最右边的页面）。
		 */
		if (offset == max)
		{
			BTScanInsert rightkey;

			/* first offset on a right index page (log only)
			 *
			 * 右侧索引页上的第一个偏移量（仅日志）
			 */
			OffsetNumber rightfirstoffset = InvalidOffsetNumber;

			/* Get item in next/right page
			 *
			 * 获取下一页/右页的项目
			 */
			rightkey = bt_right_page_check_scankey(state, &rightfirstoffset);

			if (rightkey &&
				!invariant_g_offset(state, rightkey, max))
			{
				/*
				 * As explained at length in bt_right_page_check_scankey(),
				 * there is a known !readonly race that could account for
				 * apparent violation of invariant, which we must check for
				 * before actually proceeding with raising error.  Our canary
				 * condition is that target page was deleted.
				 *
				 * 正如 bt_right_page_check_scankey() 中详细解释的那样，存在一个已知的 !readonly 竞争，它可以解释明显违反不变量的情况，我们必须在实际继续引发错误之前进行检查。  我们的金丝雀条件是目标页面已被删除。
				 */
				if (!state->readonly)
				{
					/* Get fresh copy of target page
					 *
					 * 获取目标页面的新副本
					 */
					state->target = palloc_btree_page(state, state->targetblock);
					/* Note that we deliberately do not update target LSN
					 *
					 * 请注意，我们故意不更新目标 LSN
					 */
					topaque = BTPageGetOpaque(state->target);

					/*
					 * All !readonly checks now performed; just return
					 *
					 * 现在执行所有 !readonly 检查；就回来
					 */
					if (P_IGNORE(topaque))
						return;
				}

				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("cross page item order invariant violated for index \"%s\"",
								RelationGetRelationName(state->rel)),
						 errdetail_internal("Last item on page tid=(%u,%u) page lsn=%X/%X.",
											state->targetblock, offset,
											LSN_FORMAT_ARGS(state->targetlsn))));
			}

			/*
			 * If index has unique constraint make sure that no more than one
			 * found equal items is visible.
			 *
			 * 如果索引具有唯一约束，请确保不超过一个找到的相同项可见。
			 */
			if (state->checkunique && state->indexinfo->ii_Unique &&
				rightkey && P_ISLEAF(topaque) && !P_RIGHTMOST(topaque))
			{
				BlockNumber rightblock_number = topaque->btpo_next;

				elog(DEBUG2, "check cross page unique condition");

				/*
				 * Make _bt_compare compare only index keys without heap TIDs.
				 * rightkey->scantid is modified destructively but it is ok
				 * for it is not used later.
				 *
				 * 使 _bt_compare 仅比较没有堆 TID 的索引键。 rightkey->scantid被破坏性修改了，不过以后不用了就ok了。
				 */
				rightkey->scantid = NULL;

				/* The first key on the next page is the same
				 *
				 * 下一页的第一个键是相同的
				 */
				if (_bt_compare(state->rel, rightkey, state->target, max) == 0 &&
					!rightkey->anynullkeys)
				{
					Page		rightpage;

					/*
					 * Do the bt_entry_unique_check() call if it was
					 * postponed.
					 *
					 * 如果被推迟，请调用 bt_entry_unique_check() 。
					 */
					if (!unique_checked)
						bt_entry_unique_check(state, itup, state->targetblock,
											  offset, &lVis);

					elog(DEBUG2, "cross page equal keys");
					rightpage = palloc_btree_page(state,
												  rightblock_number);
					topaque = BTPageGetOpaque(rightpage);

					if (P_IGNORE(topaque))
					{
						pfree(rightpage);
						break;
					}

					if (unlikely(!P_ISLEAF(topaque)))
						ereport(ERROR,
								(errcode(ERRCODE_INDEX_CORRUPTED),
								 errmsg("right block of leaf block is non-leaf for index \"%s\"",
										RelationGetRelationName(state->rel)),
								 errdetail_internal("Block=%u page lsn=%X/%X.",
													state->targetblock,
													LSN_FORMAT_ARGS(state->targetlsn))));

					itemid = PageGetItemIdCareful(state, rightblock_number,
												  rightpage,
												  rightfirstoffset);
					itup = (IndexTuple) PageGetItem(rightpage, itemid);

					bt_entry_unique_check(state, itup, rightblock_number, rightfirstoffset, &lVis);

					pfree(rightpage);
				}
			}
		}

		/*
		 * * Downlink check *
		 *
		 * * 下行检查 *
		 *
		 * Additional check of child items iff this is an internal page and
		 * caller holds a ShareLock.  This happens for every downlink (item)
		 * in target excluding the negative-infinity downlink (again, this is
		 * because it has no useful value to compare).
		 *
		 * 对子项进行额外检查，前提是这是一个内部页面并且调用者持有 ShareLock。  这种情况发生在目标中的每个下行链路（项目）中，不包括负无穷大下行链路（同样，这是因为它没有可比较的有用值）。
		 */
		if (!P_ISLEAF(topaque) && state->readonly)
			bt_child_check(state, skey, offset);
	}

	/*
	 * Special case bt_child_highkey_check() call
	 *
	 * 特殊情况 bt_child_highkey_check() 调用
	 *
	 * We don't pass a real downlink, but we've to finish the level
	 * processing. If condition is satisfied, we've already processed all the
	 * downlinks from the target level.  But there still might be pages to the
	 * right of the child page pointer to by our rightmost downlink.  And they
	 * might have missing downlinks.  This final call checks for them.
	 *
	 * 我们不传递真正的下行链路，但我们必须完成电平处理。如果条件满足，我们已经处理了目标级别的所有下行链路。  但在我们最右边的下行链接指向的子页面指针的右侧仍然可能有页面。  他们可能缺少下行链路。  最后一次调用会检查它们。
	 */
	if (!P_ISLEAF(topaque) && P_RIGHTMOST(topaque) && state->readonly)
	{
		bt_child_highkey_check(state, InvalidOffsetNumber,
							   NULL, topaque->btpo_level);
	}
}

/*
 * Return a scankey for an item on page to right of current target (or the
 * first non-ignorable page), sufficient to check ordering invariant on last
 * item in current target page.  Returned scankey relies on local memory
 * allocated for the child page, which caller cannot pfree().  Caller's memory
 * context should be reset between calls here.
 *
 * 返回当前目标右侧页面（或第一个不可忽略页面）上项目的扫描键，足以检查当前目标页面中最后一个项目的排序不变性。  返回的 scankey 依赖于为子页面分配的本地内存，调用者无法 pfree()。  调用者的内存上下文应该在调用之间重置。
 *
 * This is the first data item, and so all adjacent items are checked against
 * their immediate sibling item (which may be on a sibling page, or even a
 * "cousin" page at parent boundaries where target's rightlink points to page
 * with different parent page).  If no such valid item is available, return
 * NULL instead.
 *
 * 这是第一个数据项，因此所有相邻项都会根据其直接同级项进行检查（可能位于同级页面上，甚至可能位于父边界的“表兄弟”页面上，其中目标的右链接指向具有不同父页面的页面）。  如果没有这样的有效项目可用，则返回 NULL。
 *
 * Note that !readonly callers must reverify that target page has not
 * been concurrently deleted.
 *
 * 请注意，!readonly 调用者必须重新验证目标页面是否未被同时删除。
 *
 * Save rightfirstoffset for detailed error message.
 *
 * 保存 rightfirstoffset 以获取详细的错误消息。
 */
static BTScanInsert
bt_right_page_check_scankey(BtreeCheckState *state, OffsetNumber *rightfirstoffset)
{
	BTPageOpaque opaque;
	ItemId		rightitem;
	IndexTuple	firstitup;
	BlockNumber targetnext;
	Page		rightpage;
	OffsetNumber nline;

	/* Determine target's next block number
	 *
	 * 确定目标的下一个块号
	 */
	opaque = BTPageGetOpaque(state->target);

	/* If target is already rightmost, no right sibling; nothing to do here
	 *
	 * 如果目标已经是最右边的，则没有右兄弟；这里没什么可做的
	 */
	if (P_RIGHTMOST(opaque))
		return NULL;

	/*
	 * General notes on concurrent page splits and page deletion:
	 *
	 * 关于并发页面拆分和页面删除的一般注意事项：
	 *
	 * Routines like _bt_search() don't require *any* page split interlock
	 * when descending the tree, including something very light like a buffer
	 * pin. That's why it's okay that we don't either.  This avoidance of any
	 * need to "couple" buffer locks is the raison d' etre of the Lehman & Yao
	 * algorithm, in fact.
	 *
	 * 像 _bt_search() 这样的例程在树下降时不需要*任何*页面分割互锁，包括像缓冲引脚这样非常轻的东西。这就是为什么我们也不这样做也没关系。  事实上，避免任何“耦合”缓冲区锁的需要是 Lehman & Yao 算法存在的理由。
	 *
	 * That leaves deletion.  A deleted page won't actually be recycled by
	 * VACUUM early enough for us to fail to at least follow its right link
	 * (or left link, or downlink) and find its sibling, because recycling
	 * does not occur until no possible index scan could land on the page.
	 * Index scans can follow links with nothing more than their snapshot as
	 * an interlock and be sure of at least that much.  (See page
	 * recycling/"visible to everyone" notes in nbtree README.)
	 *
	 * 这就只剩下删除了。  已删除的页面实际上不会被 VACUUM 回收得足够早，以至于我们至少无法跟踪其右链接（或左链接或下行链接）并找到其同级，因为直到没有可能的索引扫描可以登陆该页面时才会发生回收。索引扫描可以跟踪链接，只需要它们的快照作为互锁，并且至少可以保证这么多。  （请参阅 nbtree 自述文件中的页面回收/“对所有人可见”注释。）
	 *
	 * Furthermore, it's okay if we follow a rightlink and find a half-dead or
	 * dead (ignorable) page one or more times.  There will either be a
	 * further right link to follow that leads to a live page before too long
	 * (before passing by parent's rightmost child), or we will find the end
	 * of the entire level instead (possible when parent page is itself the
	 * rightmost on its level).
	 *
	 * 此外，如果我们点击正确的链接并多次找到半死或死（可忽略）的页面，这也没关系。  要么有一个更右边的链接可以在不久之后（在经过父级最右边的子级之前）引导到实时页面，要么我们会找到整个级别的结尾（当父级页面本身是其级别的最右边时可能）。
	 */
	targetnext = opaque->btpo_next;
	for (;;)
	{
		CHECK_FOR_INTERRUPTS();

		rightpage = palloc_btree_page(state, targetnext);
		opaque = BTPageGetOpaque(rightpage);

		if (!P_IGNORE(opaque) || P_RIGHTMOST(opaque))
			break;

		/*
		 * We landed on a deleted or half-dead sibling page.  Step right until
		 * we locate a live sibling page.
		 *
		 * 我们到达了一个已删除或半死不活的同级页面。  向右移动，直到找到一个活动的同级页面。
		 */
		ereport(DEBUG2,
				(errcode(ERRCODE_NO_DATA),
				 errmsg_internal("level %u sibling page in block %u of index \"%s\" was found deleted or half dead",
								 opaque->btpo_level, targetnext, RelationGetRelationName(state->rel)),
				 errdetail_internal("Deleted page found when building scankey from right sibling.")));

		targetnext = opaque->btpo_next;

		/* Be slightly more pro-active in freeing this memory, just in case
		 *
		 * 稍微主动地释放这些内存，以防万一
		 */
		pfree(rightpage);
	}

	/*
	 * No ShareLock held case -- why it's safe to proceed.
	 *
	 * 没有 ShareLock 持有案例——为什么可以安全地继续。
	 *
	 * Problem:
	 *
	 * We must avoid false positive reports of corruption when caller treats
	 * item returned here as an upper bound on target's last item.  In
	 * general, false positives are disallowed.  Avoiding them here when
	 * caller is !readonly is subtle.
	 *
	 * 当调用者将此处返回的项目视为目标最后一个项目的上限时，我们必须避免误报损坏。  一般来说，误报是不允许的。  当调用者为 !readonly 时，避免使用它们是很微妙的。
	 *
	 * A concurrent page deletion by VACUUM of the target page can result in
	 * the insertion of items on to this right sibling page that would
	 * previously have been inserted on our target page.  There might have
	 * been insertions that followed the target's downlink after it was made
	 * to point to right sibling instead of target by page deletion's first
	 * phase. The inserters insert items that would belong on target page.
	 * This race is very tight, but it's possible.  This is our only problem.
	 *
	 * 通过 VACUUM 对目标页面进行并发页面删除可能会导致将先前已插入到目标页面上的项目插入到该右同级页面上。  在页面删除的第一阶段使目标指向右兄弟而不是目标后，可能会在目标的下行链路之后进行插入。插入器插入属于目标页面的项目。这场比赛非常激烈，但还是有可能的。  这是我们唯一的问题。
	 *
	 * Non-problems:
	 *
	 * We are not hindered by a concurrent page split of the target; we'll
	 * never land on the second half of the page anyway.  A concurrent split
	 * of the right page will also not matter, because the first data item
	 * remains the same within the left half, which we'll reliably land on. If
	 * we had to skip over ignorable/deleted pages, it cannot matter because
	 * their key space has already been atomically merged with the first
	 * non-ignorable page we eventually find (doesn't matter whether the page
	 * we eventually find is a true sibling or a cousin of target, which we go
	 * into below).
	 *
	 * 我们不会受到目标的并发页面拆分的阻碍；无论如何，我们永远不会到达页面的后半部分。  右页的并发拆分也无关紧要，因为第一个数据项在左半部分中保持不变，我们将可靠地落在左半部分上。如果我们必须跳过可忽略/已删除的页面，那也没关系，因为它们的键空间已经与我们最终找到的第一个不可忽略的页面自动合并（无论我们最终找到的页面是真正的同级页面还是目标的表兄弟，我们将在下面介绍）。
	 *
	 * Solution:
	 *
	 * Caller knows that it should reverify that target is not ignorable
	 * (half-dead or deleted) when cross-page sibling item comparison appears
	 * to indicate corruption (invariant fails).  This detects the single race
	 * condition that exists for caller.  This is correct because the
	 * continued existence of target block as non-ignorable (not half-dead or
	 * deleted) implies that target page was not merged into from the right by
	 * deletion; the key space at or after target never moved left.  Target's
	 * parent either has the same downlink to target as before, or a <
	 * downlink due to deletion at the left of target.  Target either has the
	 * same highkey as before, or a highkey < before when there is a page
	 * split. (The rightmost concurrently-split-from-target-page page will
	 * still have the same highkey as target was originally found to have,
	 * which for our purposes is equivalent to target's highkey itself never
	 * changing, since we reliably skip over
	 * concurrently-split-from-target-page pages.)
	 *
	 * 调用者知道，当跨页同级项比较似乎表明损坏（不变失败）时，它应该重新验证目标不可忽略（半死或删除）。  这会检测调用者存在的单一竞争条件。  这是正确的，因为目标块作为不可忽略（不是半死或删除）的持续存在意味着目标页面没有通过删除从右侧合并到；目标处或之后的关键空间从未向左移动。  目标的父级要么具有与以前相同的到目标的下行链路，要么由于目标左侧的删除而具有 < 下行链路。  目标要么具有与之前相同的 highkey，要么在存在页面拆分时具有之前的 highkey <。 （最右边的并发拆分目标页面页面仍将具有与目标最初发现的相同的 highkey，对于我们的目的来说，这相当于目标的 highkey 本身永远不会改变，因为我们可靠地跳过并发拆分目标页面页面。）
	 *
	 * In simpler terms, we allow that the key space of the target may expand
	 * left (the key space can move left on the left side of target only), but
	 * the target key space cannot expand right and get ahead of us without
	 * our detecting it.  The key space of the target cannot shrink, unless it
	 * shrinks to zero due to the deletion of the original page, our canary
	 * condition.  (To be very precise, we're a bit stricter than that because
	 * it might just have been that the target page split and only the
	 * original target page was deleted.  We can be more strict, just not more
	 * lax.)
	 *
	 * 简单来说，我们允许目标的键空间可以向左扩展（键空间只能在目标的左侧向左移动），但是目标键空间不能向右扩展并在我们没有检测到的情况下超出我们的范围。  目标的键空间无法收缩，除非由于删除原始页面（我们的金丝雀条件）而收缩到零。  （更准确地说，我们比这更严格一点，因为可能只是目标页面分裂，只删除了原始目标页面。我们可以更严格，但不能更宽松。）
	 *
	 * Top level tree walk caller moves on to next page (makes it the new
	 * target) following recovery from this race.  (cf.  The rationale for
	 * child/downlink verification needing a ShareLock within
	 * bt_child_check(), where page deletion is also the main source of
	 * trouble.)
	 *
	 * 从本次比赛恢复后，顶级树行走调用者将移至下一页（使其成为新目标）。  （参见 bt_child_check() 中需要 ShareLock 的子/下行验证的基本原理，其中页面删除也是麻烦的主要来源。）
	 *
	 * Note that it doesn't matter if right sibling page here is actually a
	 * cousin page, because in order for the key space to be readjusted in a
	 * way that causes us issues in next level up (guiding problematic
	 * concurrent insertions to the cousin from the grandparent rather than to
	 * the sibling from the parent), there'd have to be page deletion of
	 * target's parent page (affecting target's parent's downlink in target's
	 * grandparent page).  Internal page deletion only occurs when there are
	 * no child pages (they were all fully deleted), and caller is checking
	 * that the target's parent has at least one non-deleted (so
	 * non-ignorable) child: the target page.  (Note that the first phase of
	 * deletion atomically marks the page to be deleted half-dead/ignorable at
	 * the same time downlink in its parent is removed, so caller will
	 * definitely not fail to detect that this happened.)
	 *
	 * 请注意，这里的右兄弟页面是否实际上是表兄弟页面并不重要，因为为了以某种方式重新调整键空间，导致我们在下一个级别出现问题（引导有问题的并发插入从祖父母到表兄弟，而不是从父辈到兄弟姐妹），必须删除目标父页面的页面（影响目标父页面在目标祖父母页面中的下行链路）。  仅当没有子页面（它们全部被完全删除）并且调用者正在检查目标的父页面是否至少有一个未删除（因此不可忽略）的子页面：目标页面时，才会发生内部页面删除。  （请注意，删除的第一阶段原子地将要删除的页面标记为半死/可忽略，同时删除其父级中的下行链路，因此调用者肯定不会无法检测到发生了这种情况。）
	 *
	 * This trick is inspired by the method backward scans use for dealing
	 * with concurrent page splits; concurrent page deletion is a problem that
	 * similarly receives special consideration sometimes (it's possible that
	 * the backwards scan will re-read its "original" block after failing to
	 * find a right-link to it, having already moved in the opposite direction
	 * (right/"forwards") a few times to try to locate one).  Just like us,
	 * that happens only to determine if there was a concurrent page deletion
	 * of a reference page, and just like us if there was a page deletion of
	 * that reference page it means we can move on from caring about the
	 * reference page.  See the nbtree README for a full description of how
	 * that works.
	 *
	 * 这个技巧的灵感来自于用于处理并发页面分割的向后扫描方法；并发页面删除是一个类似地有时会受到特殊考虑的问题（向后扫描可能会在未能找到指向它的右侧链接后重新读取其“原始”块，并且已经向相反方向（右/“向前”）移动了几次以尝试找到一个）。  就像我们一样，这只是为了确定是否存在参考页面的并发页面删除，就像我们一样，如果该参考页面存在页面删除，则意味着我们可以不再关心参考页面。  有关其工作原理的完整说明，请参阅 nbtree 自述文件。
	 */
	nline = PageGetMaxOffsetNumber(rightpage);

	/*
	 * Get first data item, if any
	 *
	 * 获取第一个数据项（如果有）
	 */
	if (P_ISLEAF(opaque) && nline >= P_FIRSTDATAKEY(opaque))
	{
		/* Return first data item (if any)
		 *
		 * 返回第一个数据项（如果有）
		 */
		rightitem = PageGetItemIdCareful(state, targetnext, rightpage,
										 P_FIRSTDATAKEY(opaque));
		*rightfirstoffset = P_FIRSTDATAKEY(opaque);
	}
	else if (!P_ISLEAF(opaque) &&
			 nline >= OffsetNumberNext(P_FIRSTDATAKEY(opaque)))
	{
		/*
		 * Return first item after the internal page's "negative infinity"
		 * item
		 *
		 * 返回内部页面的“负无穷”项之后的第一项
		 */
		rightitem = PageGetItemIdCareful(state, targetnext, rightpage,
										 OffsetNumberNext(P_FIRSTDATAKEY(opaque)));
	}
	else
	{
		/*
		 * No first item.  Page is probably empty leaf page, but it's also
		 * possible that it's an internal page with only a negative infinity
		 * item.
		 *
		 * 没有第一项。  页面可能是空的叶页面，但也可能是只有负无穷项的内部页面。
		 */
		ereport(DEBUG2,
				(errcode(ERRCODE_NO_DATA),
				 errmsg_internal("%s block %u of index \"%s\" has no first data item",
								 P_ISLEAF(opaque) ? "leaf" : "internal", targetnext,
								 RelationGetRelationName(state->rel))));
		return NULL;
	}

	/*
	 * Return first real item scankey.  Note that this relies on right page
	 * memory remaining allocated.
	 *
	 * 返回第一个真实项目扫描键。  请注意，这依赖于剩余分配的右页内存。
	 */
	firstitup = (IndexTuple) PageGetItem(rightpage, rightitem);
	return bt_mkscankey_pivotsearch(state->rel, firstitup);
}

/*
 * Check if two tuples are binary identical except the block number.  So,
 * this function is capable to compare pivot keys on different levels.
 *
 * 检查两个元组除了块号之外是否二进制相同。  因此，该函数能够比较不同级别的主键。
 */
static bool
bt_pivot_tuple_identical(bool heapkeyspace, IndexTuple itup1, IndexTuple itup2)
{
	if (IndexTupleSize(itup1) != IndexTupleSize(itup2))
		return false;

	if (heapkeyspace)
	{
		/*
		 * Offset number will contain important information in heapkeyspace
		 * indexes: the number of attributes left in the pivot tuple following
		 * suffix truncation.  Don't skip over it (compare it too).
		 *
		 * 偏移量将包含堆键空间索引中的重要信息：后缀截断后枢轴元组中剩余的属性数量。  不要跳过它（也可以比较）。
		 */
		if (memcmp(&itup1->t_tid.ip_posid, &itup2->t_tid.ip_posid,
				   IndexTupleSize(itup1) -
				   offsetof(ItemPointerData, ip_posid)) != 0)
			return false;
	}
	else
	{
		/*
		 * Cannot rely on offset number field having consistent value across
		 * levels on pg_upgrade'd !heapkeyspace indexes.  Compare contents of
		 * tuple starting from just after item pointer (i.e. after block
		 * number and offset number).
		 *
		 * 不能依赖在 pg_upgrade'd !heapkeyspace 索引上跨级别具有一致值的偏移量字段。  从项目指针之后（即块号和偏移量之后）开始比较元组的内容。
		 */
		if (memcmp(&itup1->t_info, &itup2->t_info,
				   IndexTupleSize(itup1) -
				   offsetof(IndexTupleData, t_info)) != 0)
			return false;
	}

	return true;
}

/*---
 * Check high keys on the child level.  Traverse rightlinks from previous
 * downlink to the current one.  Check that there are no intermediate pages
 * with missing downlinks.
 *
 * 检查儿童级别的高调。  从上一个下行链路遍历右链路到当前下行链路。  检查是否存在缺少下行链路的中间页面。
 *
 * If 'loaded_child' is given, it's assumed to be the page pointed to by the
 * downlink referenced by 'downlinkoffnum' of the target page.
 *
 * 如果给出“loaded_child”，则假定它是目标页面的“downlinkoffnum”引用的下行链路所指向的页面。
 *
 * Basically this function is called for each target downlink and checks two
 * invariants:
 *
 * 基本上，每个目标下行链路都会调用此函数并检查两个不变量：
 *
 * 1) You can reach the next child from previous one via rightlinks;
 * 2) Each child high key have matching pivot key on target level.
 *
 * 1) 您可以通过右链接到达上一个孩子的下一个孩子； 2) 每个子高键在目标级别上都有匹配的主键。
 *
 * Consider the sample tree picture.
 *
 * 考虑示例树图片。
 *
 *               1
 *           /       \
 *        2     <->     3
 *      /   \        /     \
 *    4  <>  5  <> 6 <> 7 <> 8
 *
 * This function will be called for blocks 4, 5, 6 and 8.  Consider what is
 * happening for each function call.
 *
 * 该函数将被块 4、5、6 和 8 调用。考虑每个函数调用发生了什么。
 *
 * - The function call for block 4 initializes data structure and matches high
 *   key of block 4 to downlink's pivot key of block 2.
 * - The high key of block 5 is matched to the high key of block 2.
 * - The block 6 has an incomplete split flag set, so its high key isn't
 *   matched to anything.
 * - The function call for block 8 checks that block 8 can be found while
 *   following rightlinks from block 6.  The high key of block 7 will be
 *   matched to downlink's pivot key in block 3.
 *
 * - 块 4 的函数调用初始化数据结构，并将块 4 的高键与块 2 的下行链路枢轴键进行匹配。 - 块 5 的高键与块 2 的高键匹配。 - 块 6 具有不完整的分割标志设置，因此其高键不与任何内容匹配。 - 块 8 的函数调用检查在跟随块 6 的右链接时是否可以找到块 8。块 7 的高调将与块 3 中下行链路的枢轴键匹配。
 *
 * There is also final call of this function, which checks that there is no
 * missing downlinks for children to the right of the child referenced by
 * rightmost downlink in target level.
 *
 * 还有此函数的最终调用，它检查目标级别中最右侧下行链路引用的子级右侧的子级是否没有丢失下行链路。
 */
static void
bt_child_highkey_check(BtreeCheckState *state,
					   OffsetNumber target_downlinkoffnum,
					   Page loaded_child,
					   uint32 target_level)
{
	BlockNumber blkno = state->prevrightlink;
	Page		page;
	BTPageOpaque opaque;
	bool		rightsplit = state->previncompletesplit;
	bool		first = true;
	ItemId		itemid;
	IndexTuple	itup;
	BlockNumber downlink;

	if (OffsetNumberIsValid(target_downlinkoffnum))
	{
		itemid = PageGetItemIdCareful(state, state->targetblock,
									  state->target, target_downlinkoffnum);
		itup = (IndexTuple) PageGetItem(state->target, itemid);
		downlink = BTreeTupleGetDownLink(itup);
	}
	else
	{
		downlink = P_NONE;
	}

	/*
	 * If no previous rightlink is memorized for current level just below
	 * target page's level, we are about to start from the leftmost page. We
	 * can't follow rightlinks from previous page, because there is no
	 * previous page.  But we still can match high key.
	 *
	 * 如果当前级别没有记住目标页面级别之下的先前右链接，则我们将从最左边的页面开始。我们无法跟踪上一页的正确链接，因为没有上一页。  但我们还是可以配高调的。
	 *
	 * So we initialize variables for the loop above like there is previous
	 * page referencing current child.  Also we imply previous page to not
	 * have incomplete split flag, that would make us require downlink for
	 * current child.  That's correct, because leftmost page on the level
	 * should always have parent downlink.
	 *
	 * 因此，我们为上面的循环初始化变量，就像上一页引用当前子级一样。  此外，我们还暗示前一页没有不完整的分割标志，这将使我们需要当前子项的下行链路。  这是正确的，因为该级别的最左边页面应该始终具有父下行链路。
	 */
	if (!BlockNumberIsValid(blkno))
	{
		blkno = downlink;
		rightsplit = false;
	}

	/* Move to the right on the child level
	 *
	 * 在子级别向右移动
	 */
	while (true)
	{
		/*
		 * Did we traverse the whole tree level and this is check for pages to
		 * the right of rightmost downlink?
		 *
		 * 我们是否遍历了整个树级别，这是检查最右侧下行链路右侧的页面？
		 */
		if (blkno == P_NONE && downlink == P_NONE)
		{
			state->prevrightlink = InvalidBlockNumber;
			state->previncompletesplit = false;
			return;
		}

		/* Did we traverse the whole tree level and don't find next downlink?
		 *
		 * 我们是否遍历了整个树层但没有找到下一个下行链路？
		 */
		if (blkno == P_NONE)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("can't traverse from downlink %u to downlink %u of index \"%s\"",
							state->prevrightlink, downlink,
							RelationGetRelationName(state->rel))));

		/* Load page contents
		 *
		 * 加载页面内容
		 */
		if (blkno == downlink && loaded_child)
			page = loaded_child;
		else
			page = palloc_btree_page(state, blkno);

		opaque = BTPageGetOpaque(page);

		/* The first page we visit at the level should be leftmost
		 *
		 * 我们在该级别访问的第一页应该是最左边的
		 */
		if (first && !BlockNumberIsValid(state->prevrightlink) &&
			!bt_leftmost_ignoring_half_dead(state, blkno, opaque))
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("the first child of leftmost target page is not leftmost of its level in index \"%s\"",
							RelationGetRelationName(state->rel)),
					 errdetail_internal("Target block=%u child block=%u target page lsn=%X/%X.",
										state->targetblock, blkno,
										LSN_FORMAT_ARGS(state->targetlsn))));

		/* Do level sanity check
		 *
		 * 进行级别健全性检查
		 */
		if ((!P_ISDELETED(opaque) || P_HAS_FULLXID(opaque)) &&
			opaque->btpo_level != target_level - 1)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("block found while following rightlinks from child of index \"%s\" has invalid level",
							RelationGetRelationName(state->rel)),
					 errdetail_internal("Block pointed to=%u expected level=%u level in pointed to block=%u.",
										blkno, target_level - 1, opaque->btpo_level)));

		/* Try to detect circular links
		 *
		 * 尝试检测循环链接
		 */
		if ((!first && blkno == state->prevrightlink) || blkno == opaque->btpo_prev)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("circular link chain found in block %u of index \"%s\"",
							blkno, RelationGetRelationName(state->rel))));

		if (blkno != downlink && !P_IGNORE(opaque))
		{
			/* blkno probably has missing parent downlink
			 *
			 * blkno 可能缺少父下行链路
			 */
			bt_downlink_missing_check(state, rightsplit, blkno, page);
		}

		rightsplit = P_INCOMPLETE_SPLIT(opaque);

		/*
		 * If we visit page with high key, check that it is equal to the
		 * target key next to corresponding downlink.
		 *
		 * 如果我们访问密钥高的页面，请检查它是否等于相应下行链路旁边的目标密钥。
		 */
		if (!rightsplit && !P_RIGHTMOST(opaque) && !P_ISHALFDEAD(opaque))
		{
			BTPageOpaque topaque;
			IndexTuple	highkey;
			OffsetNumber pivotkey_offset;

			/* Get high key
			 *
			 * 获得高调
			 */
			itemid = PageGetItemIdCareful(state, blkno, page, P_HIKEY);
			highkey = (IndexTuple) PageGetItem(page, itemid);

			/*
			 * There might be two situations when we examine high key.  If
			 * current child page is referenced by given target downlink, we
			 * should look to the next offset number for matching key from
			 * target page.
			 *
			 * 我们考察高调的时候可能有两种情况。  如果当前子页面被给定的目标下行链路引用，我们应该查找下一个偏移量以匹配目标页面的键。
			 *
			 * Alternatively, we're following rightlinks somewhere in the
			 * middle between page referenced by previous target's downlink
			 * and the page referenced by current target's downlink.  If
			 * current child page hasn't incomplete split flag set, then its
			 * high key should match to the target's key of current offset
			 * number. This happens when a previous call here (to
			 * bt_child_highkey_check()) found an incomplete split, and we
			 * reach a right sibling page without a downlink -- the right
			 * sibling page's high key still needs to be matched to a
			 * separator key on the parent/target level.
			 *
			 * 或者，我们跟踪前一个目标的下行链接引用的页面和当前目标的下行链接引用的页面之间的中间位置的右链接。  如果当前子页面没有设置不完整的分割标志，则其高键应该与当前偏移量的目标键匹配。当之前的调用（对 bt_child_highkey_check()）发现不完整的分割，并且我们到达没有下行链接的右同级页面时，就会发生这种情况 - 右同级页面的 high key 仍然需要与父/目标级别上的分隔符键匹配。
			 *
			 * Don't apply OffsetNumberNext() to target_downlinkoffnum when we
			 * already had to step right on the child level. Our traversal of
			 * the child level must try to move in perfect lockstep behind (to
			 * the left of) the target/parent level traversal.
			 *
			 * 当我们已经必须在子级别上向右迈出时，不要将 OffsetNumberNext() 应用于 target_downlinkoffnum。我们对子级别的遍历必须尝试以完美的同步方式移动到目标/父级别遍历的后面（左侧）。
			 */
			if (blkno == downlink)
				pivotkey_offset = OffsetNumberNext(target_downlinkoffnum);
			else
				pivotkey_offset = target_downlinkoffnum;

			topaque = BTPageGetOpaque(state->target);

			if (!offset_is_negative_infinity(topaque, pivotkey_offset))
			{
				/*
				 * If we're looking for the next pivot tuple in target page,
				 * but there is no more pivot tuples, then we should match to
				 * high key instead.
				 *
				 * 如果我们正在目标页面中寻找下一个主元组，但没有更多的主元组，那么我们应该匹配高调。
				 */
				if (pivotkey_offset > PageGetMaxOffsetNumber(state->target))
				{
					if (P_RIGHTMOST(topaque))
						ereport(ERROR,
								(errcode(ERRCODE_INDEX_CORRUPTED),
								 errmsg("child high key is greater than rightmost pivot key on target level in index \"%s\"",
										RelationGetRelationName(state->rel)),
								 errdetail_internal("Target block=%u child block=%u target page lsn=%X/%X.",
													state->targetblock, blkno,
													LSN_FORMAT_ARGS(state->targetlsn))));
					pivotkey_offset = P_HIKEY;
				}
				itemid = PageGetItemIdCareful(state, state->targetblock,
											  state->target, pivotkey_offset);
				itup = (IndexTuple) PageGetItem(state->target, itemid);
			}
			else
			{
				/*
				 * We cannot try to match child's high key to a negative
				 * infinity key in target, since there is nothing to compare.
				 * However, it's still possible to match child's high key
				 * outside of target page.  The reason why we're are is that
				 * bt_child_highkey_check() was previously called for the
				 * cousin page of 'loaded_child', which is incomplete split.
				 * So, now we traverse to the right of that cousin page and
				 * current child level page under consideration still belongs
				 * to the subtree of target's left sibling.  Thus, we need to
				 * match child's high key to its left uncle page high key.
				 * Thankfully we saved it, it's called a "low key" of target
				 * page.
				 *
				 * 我们不能尝试将子级的高调与目标中的负无穷大调相匹配，因为没有什么可比较的。但是，仍然可以在目标页面之外匹配子级的高调。  我们这样做的原因是 bt_child_highkey_check() 之前是为“loaded_child”的表兄弟页面调用的，这是不完整的拆分。因此，现在我们遍历该表兄弟页面的右侧，并且当前考虑的子级页面仍然属于目标左兄弟的子树。  因此，我们需要将孩子的高键与其左叔叔页面高键相匹配。值得庆幸的是我们保存了它，它被称为“低调”的目标页面。
				 */
				if (!state->lowkey)
					ereport(ERROR,
							(errcode(ERRCODE_INDEX_CORRUPTED),
							 errmsg("can't find left sibling high key in index \"%s\"",
									RelationGetRelationName(state->rel)),
							 errdetail_internal("Target block=%u child block=%u target page lsn=%X/%X.",
												state->targetblock, blkno,
												LSN_FORMAT_ARGS(state->targetlsn))));
				itup = state->lowkey;
			}

			if (!bt_pivot_tuple_identical(state->heapkeyspace, highkey, itup))
			{
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("mismatch between parent key and child high key in index \"%s\"",
								RelationGetRelationName(state->rel)),
						 errdetail_internal("Target block=%u child block=%u target page lsn=%X/%X.",
											state->targetblock, blkno,
											LSN_FORMAT_ARGS(state->targetlsn))));
			}
		}

		/* Exit if we already found next downlink
		 *
		 * 如果我们已经找到下一个下行链路，则退出
		 */
		if (blkno == downlink)
		{
			state->prevrightlink = opaque->btpo_next;
			state->previncompletesplit = rightsplit;
			return;
		}

		/* Traverse to the next page using rightlink
		 *
		 * 使用 rightlink 遍历到下一页
		 */
		blkno = opaque->btpo_next;

		/* Free page contents if it's allocated by us
		 *
		 * 免费页面内容（如果由我们分配）
		 */
		if (page != loaded_child)
			pfree(page);
		first = false;
	}
}

/*
 * Checks one of target's downlink against its child page.
 *
 * 根据其子页面检查目标之一的下行链路。
 *
 * Conceptually, the target page continues to be what is checked here.  The
 * target block is still blamed in the event of finding an invariant violation.
 * The downlink insertion into the target is probably where any problem raised
 * here arises, and there is no such thing as a parent link, so doing the
 * verification this way around is much more practical.
 *
 * 从概念上讲，目标页面仍然是此处检查的页面。  如果发现不变违规，目标块仍然会受到指责。下行链路插入目标可能是这里提出的任何问题出现的地方，并且不存在诸如父链路之类的东西，因此以这种方式进行验证更加实用。
 *
 * This function visits child page and it's sequentially called for each
 * downlink of target page.  Assuming this we also check downlink connectivity
 * here in order to save child page visits.
 *
 * 该函数访问子页面，并为目标页面的每个下行链接顺序调用。  假设这一点，我们还在这里检查下行链路连接，以保存子页面访问。
 */
static void
bt_child_check(BtreeCheckState *state, BTScanInsert targetkey,
			   OffsetNumber downlinkoffnum)
{
	ItemId		itemid;
	IndexTuple	itup;
	BlockNumber childblock;
	OffsetNumber offset;
	OffsetNumber maxoffset;
	Page		child;
	BTPageOpaque copaque;
	BTPageOpaque topaque;

	itemid = PageGetItemIdCareful(state, state->targetblock,
								  state->target, downlinkoffnum);
	itup = (IndexTuple) PageGetItem(state->target, itemid);
	childblock = BTreeTupleGetDownLink(itup);

	/*
	 * Caller must have ShareLock on target relation, because of
	 * considerations around page deletion by VACUUM.
	 *
	 * 出于对 VACUUM 删除页面的考虑，调用者必须在目标关系上拥有 ShareLock。
	 *
	 * NB: In general, page deletion deletes the right sibling's downlink, not
	 * the downlink of the page being deleted; the deleted page's downlink is
	 * reused for its sibling.  The key space is thereby consolidated between
	 * the deleted page and its right sibling.  (We cannot delete a parent
	 * page's rightmost child unless it is the last child page, and we intend
	 * to also delete the parent itself.)
	 *
	 * 注意：一般情况下，页面删除是删除右兄弟的下行链路，而不是被删除页面的下行链路；已删除页面的下行链路将重新用于其同级页面。  因此，键空间在已删除页面与其右兄弟之间进行了合并。  （我们无法删除父页面最右边的子页面，除非它是最后一个子页面，并且我们还打算删除父页面本身。）
	 *
	 * If this verification happened without a ShareLock, the following race
	 * condition could cause false positives:
	 *
	 * 如果在没有 ShareLock 的情况下进行此验证，则以下竞争条件可能会导致误报：
	 *
	 * In general, concurrent page deletion might occur, including deletion of
	 * the left sibling of the child page that is examined here.  If such a
	 * page deletion were to occur, closely followed by an insertion into the
	 * newly expanded key space of the child, a window for the false positive
	 * opens up: the stale parent/target downlink originally followed to get
	 * to the child legitimately ceases to be a lower bound on all items in
	 * the page, since the key space was concurrently expanded "left".
	 * (Insertion followed the "new" downlink for the child, not our now-stale
	 * downlink, which was concurrently physically removed in target/parent as
	 * part of deletion's first phase.)
	 *
	 * 通常，可能会发生并发页面删除，包括删除此处检查的子页面的左兄弟。  如果发生这样的页面删除，紧接着插入到子项新扩展的键空间中，则会打开一个误报窗口：最初到达子项的过时父/目标下行链路不再是页面中所有项目的下界，因为键空间同时“向左”扩展。 （插入是在子级的“新”下行链路之后进行的，而不是我们现在过时的下行链路，作为删除第一阶段的一部分，该下行链路同时在目标/父级中物理删除。）
	 *
	 * While we use various techniques elsewhere to perform cross-page
	 * verification for !readonly callers, a similar trick seems difficult
	 * here.  The tricks used by bt_recheck_sibling_links and by
	 * bt_right_page_check_scankey both involve verification of a same-level,
	 * cross-sibling invariant.  Cross-level invariants are far more squishy,
	 * though.  The nbtree REDO routines do not actually couple buffer locks
	 * across levels during page splits, so making any cross-level check work
	 * reliably in !readonly mode may be impossible.
	 *
	 * 虽然我们在其他地方使用各种技术来为 !readonly 调用者执行跨页面验证，但类似的技巧在这里似乎很困难。  bt_recheck_sibling_links 和 bt_right_page_check_scankey 使用的技巧都涉及同一级别、跨兄弟不变量的验证。  不过，跨级不变量要脆弱得多。  nbtree REDO 例程实际上并不在页面分割期间跨级别耦合缓冲区锁，因此在 !readonly 模式下使任何跨级别检查可靠地工作可能是不可能的。
	 */
	Assert(state->readonly);

	/*
	 * Verify child page has the downlink key from target page (its parent) as
	 * a lower bound; downlink must be strictly less than all keys on the
	 * page.
	 *
	 * 验证子页面将目标页面（其父页面）的下行密钥作为下限；下行链路必须严格小于页面上的所有键。
	 *
	 * Check all items, rather than checking just the first and trusting that
	 * the operator class obeys the transitive law.
	 *
	 * 检查所有项目，而不是仅检查第一个项目并相信运算符类遵守传递律。
	 */
	topaque = BTPageGetOpaque(state->target);
	child = palloc_btree_page(state, childblock);
	copaque = BTPageGetOpaque(child);
	maxoffset = PageGetMaxOffsetNumber(child);

	/*
	 * Since we've already loaded the child block, combine this check with
	 * check for downlink connectivity.
	 *
	 * 由于我们已经加载了子块，因此将此检查与下行链路连接检查结合起来。
	 */
	bt_child_highkey_check(state, downlinkoffnum,
						   child, topaque->btpo_level);

	/*
	 * Since there cannot be a concurrent VACUUM operation in readonly mode,
	 * and since a page has no links within other pages (siblings and parent)
	 * once it is marked fully deleted, it should be impossible to land on a
	 * fully deleted page.
	 *
	 * 由于只读模式下不能并发 VACUUM 操作，并且一旦页面被标记为完全删除，则页面在其他页面（同级页面和父页面）内就没有链接，因此不可能登陆完全删除的页面。
	 *
	 * It does not quite make sense to enforce that the page cannot even be
	 * half-dead, despite the fact the downlink is modified at the same stage
	 * that the child leaf page is marked half-dead.  That's incorrect because
	 * there may occasionally be multiple downlinks from a chain of pages
	 * undergoing deletion, where multiple successive calls are made to
	 * _bt_unlink_halfdead_page() by VACUUM before it can finally safely mark
	 * the leaf page as fully dead.  While _bt_mark_page_halfdead() usually
	 * removes the downlink to the leaf page that is marked half-dead, that's
	 * not guaranteed, so it's possible we'll land on a half-dead page with a
	 * downlink due to an interrupted multi-level page deletion.
	 *
	 * 尽管下行链路是在子叶页面被标记为半死的同一阶段修改的，但强制页面不能半死是没有多大意义的。  这是不正确的，因为有时可能存在来自正在删除的页面链的多个下行链接，其中 VACUUM 对 _bt_unlink_halfdead_page() 进行多次连续调用，然后才最终安全地将叶页面标记为完全死亡。  虽然 _bt_mark_page_halfdead() 通常会删除标记为半死的叶页的下行链路，但这并不能保证，因此由于多级页面删除中断，我们可能会到达带有下行链路的半死页。
	 *
	 * We go ahead with our checks if the child page is half-dead.  It's safe
	 * to do so because we do not test the child's high key, so it does not
	 * matter that the original high key will have been replaced by a dummy
	 * truncated high key within _bt_mark_page_halfdead().  All other page
	 * items are left intact on a half-dead page, so there is still something
	 * to test.
	 *
	 * 我们继续检查子页面是否处于半死状态。  这样做是安全的，因为我们不测试子级的高键，因此原始高键将被 _bt_mark_page_halfdead() 中的虚拟截断高键替换并不重要。  所有其他页面项目都完好无损地保留在半死页面上，因此仍然有一些东西需要测试。
	 */
	if (P_ISDELETED(copaque))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("downlink to deleted page found in index \"%s\"",
						RelationGetRelationName(state->rel)),
				 errdetail_internal("Parent block=%u child block=%u parent page lsn=%X/%X.",
									state->targetblock, childblock,
									LSN_FORMAT_ARGS(state->targetlsn))));

	for (offset = P_FIRSTDATAKEY(copaque);
		 offset <= maxoffset;
		 offset = OffsetNumberNext(offset))
	{
		/*
		 * Skip comparison of target page key against "negative infinity"
		 * item, if any.  Checking it would indicate that it's not a strict
		 * lower bound, but that's only because of the hard-coding for
		 * negative infinity items within _bt_compare().
		 *
		 * 跳过目标页面键与“负无穷大”项（如果有）的比较。  检查它会表明它不是严格的下界，但这只是因为 _bt_compare() 中对负无穷项进行了硬编码。
		 *
		 * If nbtree didn't truncate negative infinity tuples during internal
		 * page splits then we'd expect child's negative infinity key to be
		 * equal to the scankey/downlink from target/parent (it would be a
		 * "low key" in this hypothetical scenario, and so it would still need
		 * to be treated as a special case here).
		 *
		 * 如果 nbtree 在内部页面分割期间没有截断负无穷大元组，那么我们期望子进程的负无穷大键等于来自目标/父进程的扫描键/下行链路（在这个假设场景中它将是“低调”，因此这里仍然需要将其视为特殊情况）。
		 *
		 * Negative infinity items can be thought of as a strict lower bound
		 * that works transitively, with the last non-negative-infinity pivot
		 * followed during a descent from the root as its "true" strict lower
		 * bound.  Only a small number of negative infinity items are truly
		 * negative infinity; those that are the first items of leftmost
		 * internal pages.  In more general terms, a negative infinity item is
		 * only negative infinity with respect to the subtree that the page is
		 * at the root of.
		 *
		 * 负无穷项可以被认为是传递性工作的严格下界，从根下降期间遵循的最后一个非负无穷主元作为其“真正的”严格下界。  只有少数负无穷项才是真正的负无穷；最左边内部页面的第一项。  更一般地说，负无穷项只是相对于页面所在的子树而言的负无穷大。
		 *
		 * See also: bt_rootdescend(), which can even detect transitive
		 * inconsistencies on cousin leaf pages.
		 *
		 * 另请参阅：bt_rootdescend()，它甚至可以检测表兄弟叶子页面上的传递不一致。
		 */
		if (offset_is_negative_infinity(copaque, offset))
			continue;

		if (!invariant_l_nontarget_offset(state, targetkey, childblock, child,
										  offset))
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("down-link lower bound invariant violated for index \"%s\"",
							RelationGetRelationName(state->rel)),
					 errdetail_internal("Parent block=%u child index tid=(%u,%u) parent page lsn=%X/%X.",
										state->targetblock, childblock, offset,
										LSN_FORMAT_ARGS(state->targetlsn))));
	}

	pfree(child);
}

/*
 * Checks if page is missing a downlink that it should have.
 *
 * 检查页面是否缺少应有的下行链接。
 *
 * A page that lacks a downlink/parent may indicate corruption.  However, we
 * must account for the fact that a missing downlink can occasionally be
 * encountered in a non-corrupt index.  This can be due to an interrupted page
 * split, or an interrupted multi-level page deletion (i.e. there was a hard
 * crash or an error during a page split, or while VACUUM was deleting a
 * multi-level chain of pages).
 *
 * 缺少下行链路/父页面的页面可能表明已损坏。  然而，我们必须考虑到这样一个事实：在未损坏的索引中偶尔会遇到丢失的下行链路。  这可能是由于页面拆分中断或多级页面删除中断（即页面拆分期间或 VACUUM 删除多级页面链时发生硬崩溃或错误）。
 *
 * Note that this can only be called in readonly mode, so there is no need to
 * be concerned about concurrent page splits or page deletions.
 *
 * 请注意，这只能在只读模式下调用，因此无需担心并发页面拆分或页面删除。
 */
static void
bt_downlink_missing_check(BtreeCheckState *state, bool rightsplit,
						  BlockNumber blkno, Page page)
{
	BTPageOpaque opaque = BTPageGetOpaque(page);
	ItemId		itemid;
	IndexTuple	itup;
	Page		child;
	BTPageOpaque copaque;
	uint32		level;
	BlockNumber childblk;
	XLogRecPtr	pagelsn;

	Assert(state->readonly);
	Assert(!P_IGNORE(opaque));

	/* No next level up with downlinks to fingerprint from the true root
	 *
	 * 没有下一个级别的下行链路可以从真正的根进行指纹识别
	 */
	if (P_ISROOT(opaque))
		return;

	pagelsn = PageGetLSN(page);

	/*
	 * Incomplete (interrupted) page splits can account for the lack of a
	 * downlink.  Some inserting transaction should eventually complete the
	 * page split in passing, when it notices that the left sibling page is
	 * P_INCOMPLETE_SPLIT().
	 *
	 * 不完整（中断）的页面分割可能会导致缺乏下行链路。  当某些插入事务注意到左同级页面是 P_INCOMPLETE_SPLIT() 时，它最终应该完成页面分割。
	 *
	 * In general, VACUUM is not prepared for there to be no downlink to a
	 * page that it deletes.  This is the main reason why the lack of a
	 * downlink can be reported as corruption here.  It's not obvious that an
	 * invalid missing downlink can result in wrong answers to queries,
	 * though, since index scans that land on the child may end up
	 * consistently moving right. The handling of concurrent page splits (and
	 * page deletions) within _bt_moveright() cannot distinguish
	 * inconsistencies that last for a moment from inconsistencies that are
	 * permanent and irrecoverable.
	 *
	 * 一般来说，VACUUM 不会准备好没有下行链接到它删除的页面。  这就是为什么缺乏下行链路可以被报告为腐败的主要原因。  不过，无效的缺失下行链路是否会导致错误的查询答案并不明显，因为落在子进程上的索引扫描可能最终会始终向右移动。 _bt_moveright() 中并发页面拆分（和页面删除）的处理无法区分持续一段时间的不一致和永久且不可恢复的不一致。
	 *
	 * VACUUM isn't even prepared to delete pages that have no downlink due to
	 * an incomplete page split, but it can detect and reason about that case
	 * by design, so it shouldn't be taken to indicate corruption.  See
	 * _bt_pagedel() for full details.
	 *
	 * VACUUM 甚至不准备删除由于页面分割不完整而没有下行链路的页面，但它可以通过设计检测和推理这种情况，因此不应将其视为指示损坏。  有关完整详细信息，请参阅 _bt_pagedel()。
	 */
	if (rightsplit)
	{
		ereport(DEBUG1,
				(errcode(ERRCODE_NO_DATA),
				 errmsg_internal("harmless interrupted page split detected in index \"%s\"",
								 RelationGetRelationName(state->rel)),
				 errdetail_internal("Block=%u level=%u left sibling=%u page lsn=%X/%X.",
									blkno, opaque->btpo_level,
									opaque->btpo_prev,
									LSN_FORMAT_ARGS(pagelsn))));
		return;
	}

	/*
	 * Page under check is probably the "top parent" of a multi-level page
	 * deletion.  We'll need to descend the subtree to make sure that
	 * descendant pages are consistent with that, though.
	 *
	 * 受检查的页面可能是多级页面删除的“顶级父级”。  不过，我们需要下降子树以确保后代页面与之一致。
	 *
	 * If the page (which must be non-ignorable) is a leaf page, then clearly
	 * it can't be the top parent.  The lack of a downlink is probably a
	 * symptom of a broad problem that could just as easily cause
	 * inconsistencies anywhere else.
	 *
	 * 如果该页面（必须是不可忽略的）是叶页面，那么显然它不能是顶级父级。  缺乏下行链路可能是一个广泛问题的症状，很容易在其他地方导致不一致。
	 */
	if (P_ISLEAF(opaque))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("leaf index block lacks downlink in index \"%s\"",
						RelationGetRelationName(state->rel)),
				 errdetail_internal("Block=%u page lsn=%X/%X.",
									blkno,
									LSN_FORMAT_ARGS(pagelsn))));

	/* Descend from the given page, which is an internal page
	 *
	 * 从给定页面下降，该页面是内部页面
	 */
	elog(DEBUG1, "checking for interrupted multi-level deletion due to missing downlink in index \"%s\"",
		 RelationGetRelationName(state->rel));

	level = opaque->btpo_level;
	itemid = PageGetItemIdCareful(state, blkno, page, P_FIRSTDATAKEY(opaque));
	itup = (IndexTuple) PageGetItem(page, itemid);
	childblk = BTreeTupleGetDownLink(itup);
	for (;;)
	{
		CHECK_FOR_INTERRUPTS();

		child = palloc_btree_page(state, childblk);
		copaque = BTPageGetOpaque(child);

		if (P_ISLEAF(copaque))
			break;

		/* Do an extra sanity check in passing on internal pages
		 *
		 * 在传递内部页面时进行额外的健全性检查
		 */
		if (copaque->btpo_level != level - 1)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg_internal("downlink points to block in index \"%s\" whose level is not one level down",
									 RelationGetRelationName(state->rel)),
					 errdetail_internal("Top parent/under check block=%u block pointed to=%u expected level=%u level in pointed to block=%u.",
										blkno, childblk,
										level - 1, copaque->btpo_level)));

		level = copaque->btpo_level;
		itemid = PageGetItemIdCareful(state, childblk, child,
									  P_FIRSTDATAKEY(copaque));
		itup = (IndexTuple) PageGetItem(child, itemid);
		childblk = BTreeTupleGetDownLink(itup);
		/* Be slightly more pro-active in freeing this memory, just in case
		 *
		 * 稍微主动地释放这些内存，以防万一
		 */
		pfree(child);
	}

	/*
	 * Since there cannot be a concurrent VACUUM operation in readonly mode,
	 * and since a page has no links within other pages (siblings and parent)
	 * once it is marked fully deleted, it should be impossible to land on a
	 * fully deleted page.  See bt_child_check() for further details.
	 *
	 * 由于只读模式下不能并发 VACUUM 操作，并且一旦页面被标记为完全删除，则页面在其他页面（同级页面和父页面）内就没有链接，因此不可能登陆完全删除的页面。  有关更多详细信息，请参阅 bt_child_check()。
	 *
	 * The bt_child_check() P_ISDELETED() check is repeated here because
	 * bt_child_check() does not visit pages reachable through negative
	 * infinity items.  Besides, bt_child_check() is unwilling to descend
	 * multiple levels.  (The similar bt_child_check() P_ISDELETED() check
	 * within bt_check_level_from_leftmost() won't reach the page either,
	 * since the leaf's live siblings should have their sibling links updated
	 * to bypass the deletion target page when it is marked fully dead.)
	 *
	 * 此处重复 bt_child_check() P_ISDELETED() 检查，因为 bt_child_check() 不会访问通过负无穷项可到达的页面。  此外，bt_child_check() 不愿意下降多个级别。  （bt_check_level_from_leftmost() 中类似的 bt_child_check() P_ISDELETED() 检查也不会到达该页面，因为叶子的活动同级页面应该更新其同级链接，以在标记为完全死亡时绕过删除目标页面。）
	 *
	 * If this error is raised, it might be due to a previous multi-level page
	 * deletion that failed to realize that it wasn't yet safe to mark the
	 * leaf page as fully dead.  A "dangling downlink" will still remain when
	 * this happens.  The fact that the dangling downlink's page (the leaf's
	 * parent/ancestor page) lacked a downlink is incidental.
	 *
	 * 如果引发此错误，可能是由于之前的多级页面删除未能意识到将叶页面标记为完全死亡还不安全。  当这种情况发生时，“悬空下行链路”仍然存在。  悬空下行链路的页面（叶子的父/祖先页面）缺少下行链路的事实是偶然的。
	 */
	if (P_ISDELETED(copaque))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg_internal("downlink to deleted leaf page found in index \"%s\"",
								 RelationGetRelationName(state->rel)),
				 errdetail_internal("Top parent/target block=%u leaf block=%u top parent/under check lsn=%X/%X.",
									blkno, childblk,
									LSN_FORMAT_ARGS(pagelsn))));

	/*
	 * Iff leaf page is half-dead, its high key top parent link should point
	 * to what VACUUM considered to be the top parent page at the instant it
	 * was interrupted.  Provided the high key link actually points to the
	 * page under check, the missing downlink we detected is consistent with
	 * there having been an interrupted multi-level page deletion.  This means
	 * that the subtree with the page under check at its root (a page deletion
	 * chain) is in a consistent state, enabling VACUUM to resume deleting the
	 * entire chain the next time it encounters the half-dead leaf page.
	 *
	 * 如果叶子页面处于半死状态，则其高关键顶部父链接应指向 VACUUM 在中断时认为是顶部父页面的内容。  如果高关键链接实际上指向受检查的页面，则我们检测到的丢失的下行链接与中断的多级页面删除一致。  这意味着根部受检查页面的子树（页面删除链）处于一致状态，使得 VACUUM 能够在下次遇到半死叶页面时恢复删除整个链。
	 */
	if (P_ISHALFDEAD(copaque) && !P_RIGHTMOST(copaque))
	{
		itemid = PageGetItemIdCareful(state, childblk, child, P_HIKEY);
		itup = (IndexTuple) PageGetItem(child, itemid);
		if (BTreeTupleGetTopParent(itup) == blkno)
			return;
	}

	ereport(ERROR,
			(errcode(ERRCODE_INDEX_CORRUPTED),
			 errmsg("internal index block lacks downlink in index \"%s\"",
					RelationGetRelationName(state->rel)),
			 errdetail_internal("Block=%u level=%u page lsn=%X/%X.",
								blkno, opaque->btpo_level,
								LSN_FORMAT_ARGS(pagelsn))));
}

/*
 * Per-tuple callback from table_index_build_scan, used to determine if index has
 * all the entries that definitely should have been observed in leaf pages of
 * the target index (that is, all IndexTuples that were fingerprinted by our
 * Bloom filter).  All heapallindexed checks occur here.
 *
 * 来自 table_index_build_scan 的每个元组回调，用于确定索引是否具有绝对应该在目标索引的叶页中观察到的所有条目（即由我们的布隆过滤器指纹识别的所有 IndexTuples）。  所有 heapallindexed 检查都发生在这里。
 *
 * The redundancy between an index and the table it indexes provides a good
 * opportunity to detect corruption, especially corruption within the table.
 * The high level principle behind the verification performed here is that any
 * IndexTuple that should be in an index following a fresh CREATE INDEX (based
 * on the same index definition) should also have been in the original,
 * existing index, which should have used exactly the same representation
 *
 * 索引与其索引的表之间的冗余提供了检测损坏的好机会，尤其是表内的损坏。这里执行的验证背后的高级原则是，任何应该位于新 CREATE INDEX（基于相同索引定义）之后的索引中的 IndexTuple 也应该位于原始的现有索引中，该索引应该使用完全相同的表示形式
 *
 * Since the overall structure of the index has already been verified, the most
 * likely explanation for error here is a corrupt heap page (could be logical
 * or physical corruption).  Index corruption may still be detected here,
 * though.  Only readonly callers will have verified that left links and right
 * links are in agreement, and so it's possible that a leaf page transposition
 * within index is actually the source of corruption detected here (for
 * !readonly callers).  The checks performed only for readonly callers might
 * more accurately frame the problem as a cross-page invariant issue (this
 * could even be due to recovery not replaying all WAL records).  The !readonly
 * ERROR message raised here includes a HINT about retrying with readonly
 * verification, just in case it's a cross-page invariant issue, though that
 * isn't particularly likely.
 *
 * 由于索引的整体结构已经被验证，因此这里错误最可能的解释是堆页损坏（可能是逻辑或物理损坏）。  不过，此处仍可能检测到索引损坏。  只有只读调用者才会验证左链接和右链接是否一致，因此索引内的叶页转置实际上可能是此处检测到的损坏来源（对于！只读调用者）。  仅对只读调用者执行的检查可能会更准确地将问题描述为跨页不变问题（这甚至可能是由于恢复未重播所有 WAL 记录）。  此处提出的 !readonly ERROR 消息包含有关重试只读验证的提示，以防万一出现跨页不变问题，尽管这种情况不太可能发生。
 *
 * table_index_build_scan() expects to be able to find the root tuple when a
 * heap-only tuple (the live tuple at the end of some HOT chain) needs to be
 * indexed, in order to replace the actual tuple's TID with the root tuple's
 * TID (which is what we're actually passed back here).  The index build heap
 * scan code will raise an error when a tuple that claims to be the root of the
 * heap-only tuple's HOT chain cannot be located.  This catches cases where the
 * original root item offset/root tuple for a HOT chain indicates (for whatever
 * reason) that the entire HOT chain is dead, despite the fact that the latest
 * heap-only tuple should be indexed.  When this happens, sequential scans may
 * always give correct answers, and all indexes may be considered structurally
 * consistent (i.e. the nbtree structural checks would not detect corruption).
 * It may be the case that only index scans give wrong answers, and yet heap or
 * SLRU corruption is the real culprit.  (While it's true that LP_DEAD bit
 * setting will probably also leave the index in a corrupt state before too
 * long, the problem is nonetheless that there is heap corruption.)
 *
 * table_index_build_scan() 期望在需要对纯堆元组（某个热链末尾的活动元组）建立索引时能够找到根元组，以便用根元组的 TID（这就是我们实际传回这里的内容）替换实际元组的 TID。  当无法找到声称是仅堆元组的 HOT 链的根的元组时，索引构建堆扫描代码将引发错误。  这捕获了 HOT 链的原始根项偏移/根元组指示（无论出于何种原因）整个 HOT 链已死亡的情况，尽管应该对最新的仅堆元组建立索引。  当发生这种情况时，顺序扫描可能总是给出正确的答案，并且所有索引可能被认为在结构上是一致的（即 nbtree 结构检查不会检测到损坏）。可能只有索引扫描才会给出错误的答案，而堆或 SLRU 损坏才是真正的罪魁祸首。  （虽然 LP_DEAD 位设置确实可能也会在不久之后使索引处于损坏状态，但问题仍然是存在堆损坏。）
 *
 * Heap-only tuple handling within table_index_build_scan() works in a way that
 * helps us to detect index tuples that contain the wrong values (values that
 * don't match the latest tuple in the HOT chain).  This can happen when there
 * is no superseding index tuple due to a faulty assessment of HOT safety,
 * perhaps during the original CREATE INDEX.  Because the latest tuple's
 * contents are used with the root TID, an error will be raised when a tuple
 * with the same TID but non-matching attribute values is passed back to us.
 * Faulty assessment of HOT-safety was behind at least two distinct CREATE
 * INDEX CONCURRENTLY bugs that made it into stable releases, one of which was
 * undetected for many years.  In short, the same principle that allows a
 * REINDEX to repair corruption when there was an (undetected) broken HOT chain
 * also allows us to detect the corruption in many cases.
 *
 * table_index_build_scan() 中的仅堆元组处理方式可以帮助我们检测包含错误值（与 HOT 链中最新元组不匹配的值）的索引元组。  当由于对 HOT 安全性的错误评估而没有替代索引元组时（可能是在原始 CREATE INDEX 期间），可能会发生这种情况。  由于最新元组的内容与根 TID 一起使用，因此当将具有相同 TID 但属性值不匹配的元组传回给我们时，将会引发错误。对热安全性的错误评估导致了至少两个不同的 CREATE INDEX CONCURRENTLY 错误，这些错误使其成为稳定版本，其中之一多年来一直未被发现。  简而言之，当存在（未检测到的）损坏的 HOT 链时，允许 REINDEX 修复损坏的相同原理也允许我们在许多情况下检测到损坏。
 */
static void
bt_tuple_present_callback(Relation index, ItemPointer tid, Datum *values,
						  bool *isnull, bool tupleIsAlive, void *checkstate)
{
	BtreeCheckState *state = (BtreeCheckState *) checkstate;
	IndexTuple	itup,
				norm;

	Assert(state->heapallindexed);

	/* Generate a normalized index tuple for fingerprinting
	 *
	 * 生成用于指纹识别的标准化索引元组
	 */
	itup = index_form_tuple(RelationGetDescr(index), values, isnull);
	itup->t_tid = *tid;
	norm = bt_normalize_tuple(state, itup);

	/* Probe Bloom filter -- tuple should be present
	 *
	 * 探测布隆过滤器——元组应该存在
	 */
	if (bloom_lacks_element(state->filter, (unsigned char *) norm,
							IndexTupleSize(norm)))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("heap tuple (%u,%u) from table \"%s\" lacks matching index tuple within index \"%s\"",
						ItemPointerGetBlockNumber(&(itup->t_tid)),
						ItemPointerGetOffsetNumber(&(itup->t_tid)),
						RelationGetRelationName(state->heaprel),
						RelationGetRelationName(state->rel)),
				 !state->readonly
				 ? errhint("Retrying verification using the function bt_index_parent_check() might provide a more specific error.")
				 : 0));

	state->heaptuplespresent++;
	pfree(itup);
	/* Cannot leak memory here
	 *
	 * 这里不能泄漏内存
	 */
	if (norm != itup)
		pfree(norm);
}

/*
 * Normalize an index tuple for fingerprinting.
 *
 * 标准化索引元组以进行指纹识别。
 *
 * In general, index tuple formation is assumed to be deterministic by
 * heapallindexed verification, and IndexTuples are assumed immutable.  While
 * the LP_DEAD bit is mutable in leaf pages, that's ItemId metadata, which is
 * not fingerprinted.  Normalization is required to compensate for corner
 * cases where the determinism assumption doesn't quite work.
 *
 * 一般来说，索引元组的形成通过堆索引验证被假定为确定性的，并且 IndexTuples 被假定为不可变的。  虽然 LP_DEAD 位在叶页中是可变的，但这是 ItemId 元数据，未进行指纹识别。  需要标准化来补偿确定性假设不太有效的极端情况。
 *
 * There is currently one such case: index_form_tuple() does not try to hide
 * the source TOAST state of input datums.  The executor applies TOAST
 * compression for heap tuples based on different criteria to the compression
 * applied within btinsert()'s call to index_form_tuple(): it sometimes
 * compresses more aggressively, resulting in compressed heap tuple datums but
 * uncompressed corresponding index tuple datums.  A subsequent heapallindexed
 * verification will get a logically equivalent though bitwise unequal tuple
 * from index_form_tuple().  False positive heapallindexed corruption reports
 * could occur without normalizing away the inconsistency.
 *
 * 目前存在一种这样的情况：index_form_tuple() 不会尝试隐藏输入数据的源 TOAST 状态。  执行器根据与 btinsert() 调用 index_form_tuple() 中应用的压缩不同的标准对堆元组应用 TOAST 压缩：它有时会更积极地压缩，从而导致压缩堆元组数据，但未压缩相应的索引元组数据。  随后的 heapallindexed 验证将从 index_form_tuple() 获得逻辑上等效但按位不相等的元组。  如果不规范化不一致性，可能会出现误报 heapallindexed 腐败报告。
 *
 * Returned tuple is often caller's own original tuple.  Otherwise, it is a
 * new representation of caller's original index tuple, palloc()'d in caller's
 * memory context.
 *
 * 返回的元组通常是调用者自己的原始元组。  否则，它是调用者的原始索引元组的新表示，在调用者的内存上下文中进行 palloc() 处理。
 *
 * Note: This routine is not concerned with distinctions about the
 * representation of tuples beyond those that might break heapallindexed
 * verification.  In particular, it won't try to normalize opclass-equal
 * datums with potentially distinct representations (e.g., btree/numeric_ops
 * index datums will not get their display scale normalized-away here).
 * Caller does normalization for non-pivot tuples that have a posting list,
 * since dummy CREATE INDEX callback code generates new tuples with the same
 * normalized representation.
 *
 * 注意：除了可能破坏堆索引验证的元组表示之外，此例程不关心元组表示的区别。  特别是，它不会尝试使用可能不同的表示来标准化 opclass 相等的数据（例如，btree/numeric_ops 索引数据不会在此处标准化其显示比例）。调用者对具有发布列表的非主元元组进行规范化，因为虚拟 CREATE INDEX 回调代码生成具有相同规范化表示的新元组。
 */
static IndexTuple
bt_normalize_tuple(BtreeCheckState *state, IndexTuple itup)
{
	TupleDesc	tupleDescriptor = RelationGetDescr(state->rel);
	Datum		normalized[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	bool		need_free[INDEX_MAX_KEYS];
	bool		formnewtup = false;
	IndexTuple	reformed;
	int			i;

	/* Caller should only pass "logical" non-pivot tuples here
	 *
	 * 调用者应该只在此处传递“逻辑”非枢轴元组
	 */
	Assert(!BTreeTupleIsPosting(itup) && !BTreeTupleIsPivot(itup));

	/* Easy case: It's immediately clear that tuple has no varlena datums
	 *
	 * 简单情况：很明显元组没有 varlena 数据
	 */
	if (!IndexTupleHasVarwidths(itup))
		return itup;

	for (i = 0; i < tupleDescriptor->natts; i++)
	{
		Form_pg_attribute att;

		att = TupleDescAttr(tupleDescriptor, i);

		/* Assume untoasted/already normalized datum initially
		 *
		 * 最初假设未烘烤/已经标准化的数据
		 */
		need_free[i] = false;
		normalized[i] = index_getattr(itup, att->attnum,
									  tupleDescriptor,
									  &isnull[i]);
		if (att->attbyval || att->attlen != -1 || isnull[i])
			continue;

		/*
		 * Callers always pass a tuple that could safely be inserted into the
		 * index without further processing, so an external varlena header
		 * should never be encountered here
		 *
		 * 调用者总是传递一个可以安全插入到索引中而无需进一步处理的元组，因此这里永远不会遇到外部 varlena 标头
		 */
		if (VARATT_IS_EXTERNAL(DatumGetPointer(normalized[i])))
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("external varlena datum in tuple that references heap row (%u,%u) in index \"%s\"",
							ItemPointerGetBlockNumber(&(itup->t_tid)),
							ItemPointerGetOffsetNumber(&(itup->t_tid)),
							RelationGetRelationName(state->rel))));
		else if (!VARATT_IS_COMPRESSED(DatumGetPointer(normalized[i])) &&
				 VARSIZE(DatumGetPointer(normalized[i])) > TOAST_INDEX_TARGET &&
				 (att->attstorage == TYPSTORAGE_EXTENDED ||
				  att->attstorage == TYPSTORAGE_MAIN))
		{
			/*
			 * This value will be compressed by index_form_tuple() with the
			 * current storage settings.  We may be here because this tuple
			 * was formed with different storage settings.  So, force forming.
			 *
			 * 该值将由index_form_tuple() 使用当前存储设置进行压缩。  我们可能在这里，因为这个元组是用不同的存储设置形成的。  所以，力形成。
			 */
			formnewtup = true;
		}
		else if (VARATT_IS_COMPRESSED(DatumGetPointer(normalized[i])))
		{
			formnewtup = true;
			normalized[i] = PointerGetDatum(PG_DETOAST_DATUM(normalized[i]));
			need_free[i] = true;
		}

		/*
		 * Short tuples may have 1B or 4B header. Convert 4B header of short
		 * tuples to 1B
		 *
		 * 短元组可能有 1B 或 4B 标头。将短元组的 4B 标头转换为 1B
		 */
		else if (VARATT_CAN_MAKE_SHORT(DatumGetPointer(normalized[i])))
		{
			/* convert to short varlena
			 *
			 * 转换为短varlena
			 */
			Size		len = VARATT_CONVERTED_SHORT_SIZE(DatumGetPointer(normalized[i]));
			char	   *data = palloc(len);

			SET_VARSIZE_SHORT(data, len);
			memcpy(data + 1, VARDATA(DatumGetPointer(normalized[i])), len - 1);

			formnewtup = true;
			normalized[i] = PointerGetDatum(data);
			need_free[i] = true;
		}
	}

	/*
	 * Easier case: Tuple has varlena datums, none of which are compressed or
	 * short with 4B header
	 *
	 * 更简单的情况：元组具有 varlena 数据，其中没有一个被压缩或带有 4B 标头
	 */
	if (!formnewtup)
		return itup;

	/*
	 * Hard case: Tuple had compressed varlena datums that necessitate
	 * creating normalized version of the tuple from uncompressed input datums
	 * (normalized input datums).  This is rather naive, but shouldn't be
	 * necessary too often.
	 *
	 * 困难情况：元组具有压缩的 varlena 数据，需要从未压缩的输入数据（标准化输入数据）创建元组的标准化版本。  这是相当天真的，但不应该太频繁。
	 *
	 * In the heap, tuples may contain short varlena datums with both 1B
	 * header and 4B headers.  But the corresponding index tuple should always
	 * have such varlena's with 1B headers.  So, if there is a short varlena
	 * with 4B header, we need to convert it for fingerprinting.
	 *
	 * 在堆中，元组可能包含带有 1B 标头和 4B 标头的短 varlena 数据。  但相应的索引元组应该始终具有带有 1B 标头的 varlena。  因此，如果有一个带有 4B 标头的短 varlena，我们需要将其转换为指纹识别。
	 *
	 * Note that we rely on deterministic index_form_tuple() TOAST compression
	 * of normalized input.
	 *
	 * 请注意，我们依赖于规范化输入的确定性 index_form_tuple() TOAST 压缩。
	 */
	reformed = index_form_tuple(tupleDescriptor, normalized, isnull);
	reformed->t_tid = itup->t_tid;

	/* Cannot leak memory here
	 *
	 * 这里不能泄漏内存
	 */
	for (i = 0; i < tupleDescriptor->natts; i++)
		if (need_free[i])
			pfree(DatumGetPointer(normalized[i]));

	return reformed;
}

/*
 * Produce palloc()'d "plain" tuple for nth posting list entry/TID.
 *
 * 为第 n 个发布列表条目/TID 生成 palloc() 的“普通”元组。
 *
 * In general, deduplication is not supposed to change the logical contents of
 * an index.  Multiple index tuples are merged together into one equivalent
 * posting list index tuple when convenient.
 *
 * 一般来说，重复数据删除不应改变索引的逻辑内容。  方便时，多个索引元组会合并到一个等效的发布列表索引元组中。
 *
 * heapallindexed verification must normalize-away this variation in
 * representation by converting posting list tuples into two or more "plain"
 * tuples.  Each tuple must be fingerprinted separately -- there must be one
 * tuple for each corresponding Bloom filter probe during the heap scan.
 *
 * heapallindexed 验证必须通过将发布列表元组转换为两个或多个“普通”元组来规范化这种表示形式的变化。  每个元组必须单独进行指纹识别——在堆扫描期间，每个相应的布隆过滤器探针必须有一个元组。
 *
 * Note: Caller still needs to call bt_normalize_tuple() with returned tuple.
 *
 * 注意：调用者仍然需要使用返回的元组调用 bt_normalize_tuple()。
 */
static inline IndexTuple
bt_posting_plain_tuple(IndexTuple itup, int n)
{
	Assert(BTreeTupleIsPosting(itup));

	/* Returns non-posting-list tuple
	 *
	 * 返回非发布列表元组
	 */
	return _bt_form_posting(itup, BTreeTupleGetPostingN(itup, n), 1);
}

/*
 * Search for itup in index, starting from fast root page.  itup must be a
 * non-pivot tuple.  This is only supported with heapkeyspace indexes, since
 * we rely on having fully unique keys to find a match with only a single
 * visit to a leaf page, barring an interrupted page split, where we may have
 * to move right.  (A concurrent page split is impossible because caller must
 * be readonly caller.)
 *
 * 在索引中搜索 itup，从快速根页面开始。  itup 必须是非主元组。  这仅支持堆键空间索引，因为我们依靠完全唯一的键来查找仅一次访问叶页面的匹配项，除非页面分割中断，否则我们可能必须向右移动。  （并发页面分割是不可能的，因为调用者必须是只读调用者。）
 *
 * This routine can detect very subtle transitive consistency issues across
 * more than one level of the tree.  Leaf pages all have a high key (even the
 * rightmost page has a conceptual positive infinity high key), but not a low
 * key.  Their downlink in parent is a lower bound, which along with the high
 * key is almost enough to detect every possible inconsistency.  A downlink
 * separator key value won't always be available from parent, though, because
 * the first items of internal pages are negative infinity items, truncated
 * down to zero attributes during internal page splits.  While it's true that
 * bt_child_check() and the high key check can detect most imaginable key
 * space problems, there are remaining problems it won't detect with non-pivot
 * tuples in cousin leaf pages.  Starting a search from the root for every
 * existing leaf tuple detects small inconsistencies in upper levels of the
 * tree that cannot be detected any other way.  (Besides all this, this is
 * probably also useful as a direct test of the code used by index scans
 * themselves.)
 *
 * 该例程可以检测树的多个级别上非常微妙的传递一致性问题。  叶子页面都具有高调（甚至最右边的页面也具有概念上的正无穷大高调），但不是低调。  它们在父级中的下行链路是一个下限，它与高密钥一起几乎足以检测所有可能的不一致。  不过，下行链路分隔符键值并不总是可以从父级获得，因为内部页面的第一个项目是负无穷项目，在内部页面拆分期间被截断为零属性。  虽然 bt_child_check() 和高键检查确实可以检测到大多数可以想象的键空间问题，但仍然存在一些它无法检测到表兄弟叶页中的非枢轴元组的问题。  从根开始搜索每个现有叶元组会检测树的上层中的小不一致，而这些不一致是任何其他方式都无法检测到的。  （除此之外，这对于索引扫描本身使用的代码的直接测试也可能很有用。）
 */
static bool
bt_rootdescend(BtreeCheckState *state, IndexTuple itup)
{
	BTScanInsert key;
	BTStack		stack;
	Buffer		lbuf;
	bool		exists;

	key = _bt_mkscankey(state->rel, itup);
	Assert(key->heapkeyspace && key->scantid != NULL);

	/*
	 * Search from root.
	 *
	 * 从根开始搜索。
	 *
	 * Ideally, we would arrange to only move right within _bt_search() when
	 * an interrupted page split is detected (i.e. when the incomplete split
	 * bit is found to be set), but for now we accept the possibility that
	 * that could conceal an inconsistency.
	 *
	 * 理想情况下，我们会安排仅在检测到中断的页面拆分时（即，当发现设置了不完整的拆分位时）在 _bt_search() 内向右移动，但现在我们接受这可能隐藏不一致的可能性。
	 */
	Assert(state->readonly && state->rootdescend);
	exists = false;
	stack = _bt_search(state->rel, NULL, key, &lbuf, BT_READ);

	if (BufferIsValid(lbuf))
	{
		BTInsertStateData insertstate;
		OffsetNumber offnum;
		Page		page;

		insertstate.itup = itup;
		insertstate.itemsz = MAXALIGN(IndexTupleSize(itup));
		insertstate.itup_key = key;
		insertstate.postingoff = 0;
		insertstate.bounds_valid = false;
		insertstate.buf = lbuf;

		/* Get matching tuple on leaf page
		 *
		 * 在叶子页面上获取匹配的元组
		 */
		offnum = _bt_binsrch_insert(state->rel, &insertstate);
		/* Compare first >= matching item on leaf page, if any
		 *
		 * 比较叶页上第一个 >= 匹配的项目（如果有）
		 */
		page = BufferGetPage(lbuf);
		/* Should match on first heap TID when tuple has a posting list
		 *
		 * 当元组具有发布列表时，应在第一个堆 TID 上匹配
		 */
		if (offnum <= PageGetMaxOffsetNumber(page) &&
			insertstate.postingoff <= 0 &&
			_bt_compare(state->rel, key, page, offnum) == 0)
			exists = true;
		_bt_relbuf(state->rel, lbuf);
	}

	_bt_freestack(stack);
	pfree(key);

	return exists;
}

/*
 * Is particular offset within page (whose special state is passed by caller)
 * the page negative-infinity item?
 *
 * 页面内的特定偏移量（其特殊状态由调用者传递）是页面负无穷项吗？
 *
 * As noted in comments above _bt_compare(), there is special handling of the
 * first data item as a "negative infinity" item.  The hard-coding within
 * _bt_compare() makes comparing this item for the purposes of verification
 * pointless at best, since the IndexTuple only contains a valid TID (a
 * reference TID to child page).
 *
 * 正如 _bt_compare() 上面的注释中所指出的，第一个数据项作为“负无穷大”项进行了特殊处理。  _bt_compare() 中的硬编码使得出于验证目的而比较此项至多毫无意义，因为 IndexTuple 仅包含有效的 TID（子页面的引用 TID）。
 */
static inline bool
offset_is_negative_infinity(BTPageOpaque opaque, OffsetNumber offset)
{
	/*
	 * For internal pages only, the first item after high key, if any, is
	 * negative infinity item.  Internal pages always have a negative infinity
	 * item, whereas leaf pages never have one.  This implies that negative
	 * infinity item is either first or second line item, or there is none
	 * within page.
	 *
	 * 仅对于内部页面，高调之后的第一项（如果有）是负无穷项。  内部页面总是有一个负无穷项，而叶子页从来没有一个。  这意味着负无穷项目要么是第一个或第二个行项目，要么页面内没有。
	 *
	 * Negative infinity items are a special case among pivot tuples.  They
	 * always have zero attributes, while all other pivot tuples always have
	 * nkeyatts attributes.
	 *
	 * 负无穷项是主元组中的一个特例。  它们始终具有零属性，而所有其他主元组始终具有 nkeyatts 属性。
	 *
	 * Right-most pages don't have a high key, but could be said to
	 * conceptually have a "positive infinity" high key.  Thus, there is a
	 * symmetry between down link items in parent pages, and high keys in
	 * children.  Together, they represent the part of the key space that
	 * belongs to each page in the index.  For example, all children of the
	 * root page will have negative infinity as a lower bound from root
	 * negative infinity downlink, and positive infinity as an upper bound
	 * (implicitly, from "imaginary" positive infinity high key in root).
	 *
	 * 最右边的页面没有高调，但可以说在概念上具有“正无穷大”高调。  因此，父页面中的下行链接项目与子页面中的高键之间存在对称性。  它们一起表示属于索引中每个页面的键空间部分。  例如，根页面的所有子页面都将以负无穷作为根负无穷下行链路的下界，以正无穷作为上限（隐含地，来自根中的“虚数”正无穷高键）。
	 */
	return !P_ISLEAF(opaque) && offset == P_FIRSTDATAKEY(opaque);
}

/*
 * Does the invariant hold that the key is strictly less than a given upper
 * bound offset item?
 *
 * 不变量是否认为键严格小于给定的上限偏移项？
 *
 * Verifies line pointer on behalf of caller.
 *
 * 代表调用者验证行指针。
 *
 * If this function returns false, convention is that caller throws error due
 * to corruption.
 *
 * 如果此函数返回 false，则约定调用者会因损坏而引发错误。
 */
static inline bool
invariant_l_offset(BtreeCheckState *state, BTScanInsert key,
				   OffsetNumber upperbound)
{
	ItemId		itemid;
	int32		cmp;

	Assert(!key->nextkey && key->backward);

	/* Verify line pointer before checking tuple
	 *
	 * 在检查元组之前验证行指针
	 */
	itemid = PageGetItemIdCareful(state, state->targetblock, state->target,
								  upperbound);
	/* pg_upgrade'd indexes may legally have equal sibling tuples
	 *
	 * pg_upgrade'd 索引可以合法地具有相等的兄弟元组
	 */
	if (!key->heapkeyspace)
		return invariant_leq_offset(state, key, upperbound);

	cmp = _bt_compare(state->rel, key, state->target, upperbound);

	/*
	 * _bt_compare() is capable of determining that a scankey with a
	 * filled-out attribute is greater than pivot tuples where the comparison
	 * is resolved at a truncated attribute (value of attribute in pivot is
	 * minus infinity).  However, it is not capable of determining that a
	 * scankey is _less than_ a tuple on the basis of a comparison resolved at
	 * _scankey_ minus infinity attribute.  Complete an extra step to simulate
	 * having minus infinity values for omitted scankey attribute(s).
	 *
	 * _bt_compare() 能够确定具有填充属性的扫描键大于数据透视元组，其中比较在截断的属性处进行解析（数据透视中的属性值是负无穷大）。  但是，它无法根据在_scankey_减去无穷大属性处解析的比较来确定扫描键_小于_元组。  完成一个额外的步骤来模拟省略的扫描键属性具有负无穷值。
	 */
	if (cmp == 0)
	{
		BTPageOpaque topaque;
		IndexTuple	ritup;
		int			uppnkeyatts;
		ItemPointer rheaptid;
		bool		nonpivot;

		ritup = (IndexTuple) PageGetItem(state->target, itemid);
		topaque = BTPageGetOpaque(state->target);
		nonpivot = P_ISLEAF(topaque) && upperbound >= P_FIRSTDATAKEY(topaque);

		/* Get number of keys + heap TID for item to the right
		 *
		 * 获取右侧项目的键数 + 堆 TID
		 */
		uppnkeyatts = BTreeTupleGetNKeyAtts(ritup, state->rel);
		rheaptid = BTreeTupleGetHeapTIDCareful(state, ritup, nonpivot);

		/* Heap TID is tiebreaker key attribute
		 *
		 * 堆 TID 是决定胜负的关键属性
		 */
		if (key->keysz == uppnkeyatts)
			return key->scantid == NULL && rheaptid != NULL;

		return key->keysz < uppnkeyatts;
	}

	return cmp < 0;
}

/*
 * Does the invariant hold that the key is less than or equal to a given upper
 * bound offset item?
 *
 * 不变量是否认为键小于或等于给定的上限偏移项？
 *
 * Caller should have verified that upperbound's line pointer is consistent
 * using PageGetItemIdCareful() call.
 *
 * 调用者应该使用 PageGetItemIdCareful() 调用验证 upperbound 的行指针是否一致。
 *
 * If this function returns false, convention is that caller throws error due
 * to corruption.
 *
 * 如果此函数返回 false，则约定调用者会因损坏而引发错误。
 */
static inline bool
invariant_leq_offset(BtreeCheckState *state, BTScanInsert key,
					 OffsetNumber upperbound)
{
	int32		cmp;

	Assert(!key->nextkey && key->backward);

	cmp = _bt_compare(state->rel, key, state->target, upperbound);

	return cmp <= 0;
}

/*
 * Does the invariant hold that the key is strictly greater than a given lower
 * bound offset item?
 *
 * 不变量是否认为键严格大于给定的下限偏移项？
 *
 * Caller should have verified that lowerbound's line pointer is consistent
 * using PageGetItemIdCareful() call.
 *
 * 调用者应该使用 PageGetItemIdCareful() 调用验证 lowerbound 的行指针是否一致。
 *
 * If this function returns false, convention is that caller throws error due
 * to corruption.
 *
 * 如果此函数返回 false，则约定调用者会因损坏而引发错误。
 */
static inline bool
invariant_g_offset(BtreeCheckState *state, BTScanInsert key,
				   OffsetNumber lowerbound)
{
	int32		cmp;

	Assert(!key->nextkey && key->backward);

	cmp = _bt_compare(state->rel, key, state->target, lowerbound);

	/* pg_upgrade'd indexes may legally have equal sibling tuples
	 *
	 * pg_upgrade'd 索引可以合法地具有相等的兄弟元组
	 */
	if (!key->heapkeyspace)
		return cmp >= 0;

	/*
	 * No need to consider the possibility that scankey has attributes that we
	 * need to force to be interpreted as negative infinity.  _bt_compare() is
	 * able to determine that scankey is greater than negative infinity.  The
	 * distinction between "==" and "<" isn't interesting here, since
	 * corruption is indicated either way.
	 *
	 * 无需考虑 scankey 具有我们需要强制解释为负无穷的属性的可能性。  _bt_compare() 能够确定 scankey 大于负无穷大。  “==”和“<”之间的区别在这里并不有趣，因为无论哪种方式都表明损坏。
	 */
	return cmp > 0;
}

/*
 * Does the invariant hold that the key is strictly less than a given upper
 * bound offset item, with the offset relating to a caller-supplied page that
 * is not the current target page?
 *
 * 不变量是否认为键严格小于给定的上限偏移量项，且偏移量与调用者提供的页面（不是当前目标页面）相关？
 *
 * Caller's non-target page is a child page of the target, checked as part of
 * checking a property of the target page (i.e. the key comes from the
 * target).  Verifies line pointer on behalf of caller.
 *
 * 调用者的非目标页面是目标的子页面，作为检查目标页面的属性的一部分进行检查（即密钥来自目标）。  代表调用者验证行指针。
 *
 * If this function returns false, convention is that caller throws error due
 * to corruption.
 *
 * 如果此函数返回 false，则约定调用者会因损坏而引发错误。
 */
static inline bool
invariant_l_nontarget_offset(BtreeCheckState *state, BTScanInsert key,
							 BlockNumber nontargetblock, Page nontarget,
							 OffsetNumber upperbound)
{
	ItemId		itemid;
	int32		cmp;

	Assert(!key->nextkey && key->backward);

	/* Verify line pointer before checking tuple
	 *
	 * 在检查元组之前验证行指针
	 */
	itemid = PageGetItemIdCareful(state, nontargetblock, nontarget,
								  upperbound);
	cmp = _bt_compare(state->rel, key, nontarget, upperbound);

	/* pg_upgrade'd indexes may legally have equal sibling tuples
	 *
	 * pg_upgrade'd 索引可以合法地具有相等的兄弟元组
	 */
	if (!key->heapkeyspace)
		return cmp <= 0;

	/* See invariant_l_offset() for an explanation of this extra step
	 *
	 * 有关此额外步骤的说明，请参阅 invariant_l_offset()
	 */
	if (cmp == 0)
	{
		IndexTuple	child;
		int			uppnkeyatts;
		ItemPointer childheaptid;
		BTPageOpaque copaque;
		bool		nonpivot;

		child = (IndexTuple) PageGetItem(nontarget, itemid);
		copaque = BTPageGetOpaque(nontarget);
		nonpivot = P_ISLEAF(copaque) && upperbound >= P_FIRSTDATAKEY(copaque);

		/* Get number of keys + heap TID for child/non-target item
		 *
		 * 获取子/非目标项的键数 + 堆 TID
		 */
		uppnkeyatts = BTreeTupleGetNKeyAtts(child, state->rel);
		childheaptid = BTreeTupleGetHeapTIDCareful(state, child, nonpivot);

		/* Heap TID is tiebreaker key attribute
		 *
		 * 堆 TID 是决定胜负的关键属性
		 */
		if (key->keysz == uppnkeyatts)
			return key->scantid == NULL && childheaptid != NULL;

		return key->keysz < uppnkeyatts;
	}

	return cmp < 0;
}

/*
 * Given a block number of a B-Tree page, return page in palloc()'d memory.
 * While at it, perform some basic checks of the page.
 *
 * 给定 B 树页面的块号，返回 palloc() 内存中的页面。在此期间，对页面执行一些基本检查。
 *
 * There is never an attempt to get a consistent view of multiple pages using
 * multiple concurrent buffer locks; in general, we only acquire a single pin
 * and buffer lock at a time, which is often all that the nbtree code requires.
 * (Actually, bt_recheck_sibling_links couples buffer locks, which is the only
 * exception to this general rule.)
 *
 * 永远不会尝试使用多个并发缓冲区锁来获得多个页面的一致视图；一般来说，我们一次只获取一个 pin 和缓冲区锁，这通常是 nbtree 代码所需要的。 （实际上，bt_recheck_sibling_links 耦合了缓冲区锁，这是此一般规则的唯一例外。）
 *
 * Operating on a copy of the page is useful because it prevents control
 * getting stuck in an uninterruptible state when an underlying operator class
 * misbehaves.
 *
 * 对页面副本进行操作非常有用，因为它可以防止当基础运算符类行为不当时控件陷入不可中断状态。
 */
static Page
palloc_btree_page(BtreeCheckState *state, BlockNumber blocknum)
{
	Buffer		buffer;
	Page		page;
	BTPageOpaque opaque;
	OffsetNumber maxoffset;

	page = palloc(BLCKSZ);

	/*
	 * We copy the page into local storage to avoid holding pin on the buffer
	 * longer than we must.
	 *
	 * 我们将页面复制到本地存储中，以避免在缓冲区上保持 pin 的时间超过我们必须的时间。
	 */
	buffer = ReadBufferExtended(state->rel, MAIN_FORKNUM, blocknum, RBM_NORMAL,
								state->checkstrategy);
	LockBuffer(buffer, BT_READ);

	/*
	 * Perform the same basic sanity checking that nbtree itself performs for
	 * every page:
	 *
	 * 对每个页面执行与 nbtree 本身执行的相同的基本健全性检查：
	 */
	_bt_checkpage(state->rel, buffer);

	/* Only use copy of page in palloc()'d memory
	 *
	 * 仅使用 palloc() 内存中页面的副本
	 */
	memcpy(page, BufferGetPage(buffer), BLCKSZ);
	UnlockReleaseBuffer(buffer);

	opaque = BTPageGetOpaque(page);

	if (P_ISMETA(opaque) && blocknum != BTREE_METAPAGE)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("invalid meta page found at block %u in index \"%s\"",
						blocknum, RelationGetRelationName(state->rel))));

	/* Check page from block that ought to be meta page
	 *
	 * 检查应该是元页面的块中的页面
	 */
	if (blocknum == BTREE_METAPAGE)
	{
		BTMetaPageData *metad = BTPageGetMeta(page);

		if (!P_ISMETA(opaque) ||
			metad->btm_magic != BTREE_MAGIC)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("index \"%s\" meta page is corrupt",
							RelationGetRelationName(state->rel))));

		if (metad->btm_version < BTREE_MIN_VERSION ||
			metad->btm_version > BTREE_VERSION)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("version mismatch in index \"%s\": file version %d, "
							"current version %d, minimum supported version %d",
							RelationGetRelationName(state->rel),
							metad->btm_version, BTREE_VERSION,
							BTREE_MIN_VERSION)));

		/* Finished with metapage checks
		 *
		 * 完成元页面检查
		 */
		return page;
	}

	/*
	 * Deleted pages that still use the old 32-bit XID representation have no
	 * sane "level" field because they type pun the field, but all other pages
	 * (including pages deleted on Postgres 14+) have a valid value.
	 *
	 * 仍然使用旧的 32 位 XID 表示形式的已删除页面没有正常的“级别”字段，因为它们在字段中键入双关语，但所有其他页面（包括在 Postgres 14+ 上删除的页面）都具有有效值。
	 */
	if (!P_ISDELETED(opaque) || P_HAS_FULLXID(opaque))
	{
		/* Okay, no reason not to trust btpo_level field from page
		 *
		 * 好吧，没有理由不信任页面中的 btpo_level 字段
		 */

		if (P_ISLEAF(opaque) && opaque->btpo_level != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg_internal("invalid leaf page level %u for block %u in index \"%s\"",
									 opaque->btpo_level, blocknum,
									 RelationGetRelationName(state->rel))));

		if (!P_ISLEAF(opaque) && opaque->btpo_level == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg_internal("invalid internal page level 0 for block %u in index \"%s\"",
									 blocknum,
									 RelationGetRelationName(state->rel))));
	}

	/*
	 * Sanity checks for number of items on page.
	 *
	 * 健全性检查页面上的项目数量。
	 *
	 * As noted at the beginning of _bt_binsrch(), an internal page must have
	 * children, since there must always be a negative infinity downlink
	 * (there may also be a highkey).  In the case of non-rightmost leaf
	 * pages, there must be at least a highkey.  The exceptions are deleted
	 * pages, which contain no items.
	 *
	 * 正如 _bt_binsrch() 开头所述，内部页面必须有子页面，因为必须始终存在负无穷大下行链路（也可能有高键）。  对于非最右边的叶页，必须至少有一个高键。  例外情况是已删除的页面，其中不包含任何项目。
	 *
	 * This is correct when pages are half-dead, since internal pages are
	 * never half-dead, and leaf pages must have a high key when half-dead
	 * (the rightmost page can never be deleted).  It's also correct with
	 * fully deleted pages: _bt_unlink_halfdead_page() doesn't change anything
	 * about the target page other than setting the page as fully dead, and
	 * setting its xact field.  In particular, it doesn't change the sibling
	 * links in the deletion target itself, since they're required when index
	 * scans land on the deletion target, and then need to move right (or need
	 * to move left, in the case of backward index scans).
	 *
	 * 当页面半死时这是正确的，因为内部页面永远不会半死，而叶子页面在半死时必须具有高键（最右边的页面永远不能被删除）。  对于完全删除的页面也是正确的：_bt_unlink_halfdead_page() 除了将页面设置为完全死亡并设置其 xact 字段之外，不会更改目标页面的任何内容。  特别是，它不会更改删除目标本身中的同级链接，因为当索引扫描到达删除目标时需要它们，然后需要向右移动（或者在向后索引扫描的情况下需要向左移动）。
	 */
	maxoffset = PageGetMaxOffsetNumber(page);
	if (maxoffset > MaxIndexTuplesPerPage)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("Number of items on block %u of index \"%s\" exceeds MaxIndexTuplesPerPage (%u)",
						blocknum, RelationGetRelationName(state->rel),
						MaxIndexTuplesPerPage)));

	if (!P_ISLEAF(opaque) && !P_ISDELETED(opaque) && maxoffset < P_FIRSTDATAKEY(opaque))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("internal block %u in index \"%s\" lacks high key and/or at least one downlink",
						blocknum, RelationGetRelationName(state->rel))));

	if (P_ISLEAF(opaque) && !P_ISDELETED(opaque) && !P_RIGHTMOST(opaque) && maxoffset < P_HIKEY)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("non-rightmost leaf block %u in index \"%s\" lacks high key item",
						blocknum, RelationGetRelationName(state->rel))));

	/*
	 * In general, internal pages are never marked half-dead, except on
	 * versions of Postgres prior to 9.4, where it can be valid transient
	 * state.  This state is nonetheless treated as corruption by VACUUM on
	 * from version 9.4 on, so do the same here.  See _bt_pagedel() for full
	 * details.
	 *
	 * 一般来说，内部页面永远不会被标记为半死状态，除了在 9.4 之前的 Postgres 版本上，它可以是有效的瞬态状态。  尽管如此，从版本 9.4 开始，此状态仍被 VACUUM 视为损坏，因此请在此处执行相同操作。  有关完整详细信息，请参阅 _bt_pagedel()。
	 */
	if (!P_ISLEAF(opaque) && P_ISHALFDEAD(opaque))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("internal page block %u in index \"%s\" is half-dead",
						blocknum, RelationGetRelationName(state->rel)),
				 errhint("This can be caused by an interrupted VACUUM in version 9.3 or older, before upgrade. Please REINDEX it.")));

	/*
	 * Check that internal pages have no garbage items, and that no page has
	 * an invalid combination of deletion-related page level flags
	 *
	 * 检查内部页面是否没有垃圾项，并且没有页面具有与删除相关的页面级别标志的无效组合
	 */
	if (!P_ISLEAF(opaque) && P_HAS_GARBAGE(opaque))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg_internal("internal page block %u in index \"%s\" has garbage items",
								 blocknum, RelationGetRelationName(state->rel))));

	if (P_HAS_FULLXID(opaque) && !P_ISDELETED(opaque))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg_internal("full transaction id page flag appears in non-deleted block %u in index \"%s\"",
								 blocknum, RelationGetRelationName(state->rel))));

	if (P_ISDELETED(opaque) && P_ISHALFDEAD(opaque))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg_internal("deleted page block %u in index \"%s\" is half-dead",
								 blocknum, RelationGetRelationName(state->rel))));

	return page;
}

/*
 * _bt_mkscankey() wrapper that automatically prevents insertion scankey from
 * being considered greater than the pivot tuple that its values originated
 * from (or some other identical pivot tuple) in the common case where there
 * are truncated/minus infinity attributes.  Without this extra step, there
 * are forms of corruption that amcheck could theoretically fail to report.
 *
 * _bt_mkscankey() 包装器，在存在截断/负无穷大属性的常见情况下，自动防止插入扫描键被认为大于其值源自的主元组（或其他相同的主元组）。  如果没有这个额外的步骤，amcheck 理论上可能无法报告某些形式的腐败。
 *
 * For example, invariant_g_offset() might miss a cross-page invariant failure
 * on an internal level if the scankey built from the first item on the
 * target's right sibling page happened to be equal to (not greater than) the
 * last item on target page.  The !backward tiebreaker in _bt_compare() might
 * otherwise cause amcheck to assume (rather than actually verify) that the
 * scankey is greater.
 *
 * 例如，如果从目标右侧同级页面上的第一项构建的扫描键恰好等于（不大于）目标页面上的最后一项，则 invariant_g_offset() 可能会在内部级别上错过跨页面不变性失败。  否则，_bt_compare() 中的 !backward tiebreaker 可能会导致 amcheck 假设（而不是实际验证）扫描密钥更大。
 */
static inline BTScanInsert
bt_mkscankey_pivotsearch(Relation rel, IndexTuple itup)
{
	BTScanInsert skey;

	skey = _bt_mkscankey(rel, itup);
	skey->backward = true;

	return skey;
}

/*
 * PageGetItemId() wrapper that validates returned line pointer.
 *
 * PageGetItemId() 包装器，用于验证返回的行指针。
 *
 * Buffer page/page item access macros generally trust that line pointers are
 * not corrupt, which might cause problems for verification itself.  For
 * example, there is no bounds checking in PageGetItem().  Passing it a
 * corrupt line pointer can cause it to return a tuple/pointer that is unsafe
 * to dereference.
 *
 * 缓冲区页面/页面项访问宏通常相信行指针没有损坏，这可能会导致验证本身出现问题。  例如，PageGetItem() 中没有边界检查。  向它传递一个损坏的行指针可能会导致它返回一个对取消引用不安全的元组/指针。
 *
 * Validating line pointers before tuples avoids undefined behavior and
 * assertion failures with corrupt indexes, making the verification process
 * more robust and predictable.
 *
 * 在元组之前验证行指针可以避免未定义的行为和损坏索引的断言失败，从而使验证过程更加稳健和可预测。
 */
static ItemId
PageGetItemIdCareful(BtreeCheckState *state, BlockNumber block, Page page,
					 OffsetNumber offset)
{
	ItemId		itemid = PageGetItemId(page, offset);

	if (ItemIdGetOffset(itemid) + ItemIdGetLength(itemid) >
		BLCKSZ - MAXALIGN(sizeof(BTPageOpaqueData)))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("line pointer points past end of tuple space in index \"%s\"",
						RelationGetRelationName(state->rel)),
				 errdetail_internal("Index tid=(%u,%u) lp_off=%u, lp_len=%u lp_flags=%u.",
									block, offset, ItemIdGetOffset(itemid),
									ItemIdGetLength(itemid),
									ItemIdGetFlags(itemid))));

	/*
	 * Verify that line pointer isn't LP_REDIRECT or LP_UNUSED, since nbtree
	 * never uses either.  Verify that line pointer has storage, too, since
	 * even LP_DEAD items should within nbtree.
	 *
	 * 验证行指针不是 LP_REDIRECT 或 LP_UNUSED，因为 nbtree 从不使用其中任何一个。  验证行指针也有存储，因为即使 LP_DEAD 项也应该在 nbtree 内。
	 */
	if (ItemIdIsRedirected(itemid) || !ItemIdIsUsed(itemid) ||
		ItemIdGetLength(itemid) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("invalid line pointer storage in index \"%s\"",
						RelationGetRelationName(state->rel)),
				 errdetail_internal("Index tid=(%u,%u) lp_off=%u, lp_len=%u lp_flags=%u.",
									block, offset, ItemIdGetOffset(itemid),
									ItemIdGetLength(itemid),
									ItemIdGetFlags(itemid))));

	return itemid;
}

/*
 * BTreeTupleGetHeapTID() wrapper that enforces that a heap TID is present in
 * cases where that is mandatory (i.e. for non-pivot tuples)
 *
 * BTreeTupleGetHeapTID() 包装器，强制在强制情况下（即对于非枢轴元组）存在堆 TID
 */
static inline ItemPointer
BTreeTupleGetHeapTIDCareful(BtreeCheckState *state, IndexTuple itup,
							bool nonpivot)
{
	ItemPointer htid;

	/*
	 * Caller determines whether this is supposed to be a pivot or non-pivot
	 * tuple using page type and item offset number.  Verify that tuple
	 * metadata agrees with this.
	 *
	 * 调用者使用页面类型和项目偏移量确定这应该是枢轴元组还是非枢轴元组。  验证元组元数据是否与此一致。
	 */
	Assert(state->heapkeyspace);
	if (BTreeTupleIsPivot(itup) && nonpivot)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg_internal("block %u or its right sibling block or child block in index \"%s\" has unexpected pivot tuple",
								 state->targetblock,
								 RelationGetRelationName(state->rel))));

	if (!BTreeTupleIsPivot(itup) && !nonpivot)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg_internal("block %u or its right sibling block or child block in index \"%s\" has unexpected non-pivot tuple",
								 state->targetblock,
								 RelationGetRelationName(state->rel))));

	htid = BTreeTupleGetHeapTID(itup);
	if (!ItemPointerIsValid(htid) && nonpivot)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("block %u or its right sibling block or child block in index \"%s\" contains non-pivot tuple that lacks a heap TID",
						state->targetblock,
						RelationGetRelationName(state->rel))));

	return htid;
}

/*
 * Return the "pointed to" TID for itup, which is used to generate a
 * descriptive error message.  itup must be a "data item" tuple (it wouldn't
 * make much sense to call here with a high key tuple, since there won't be a
 * valid downlink/block number to display).
 *
 * 返回 itup 的“指向”TID，该 TID 用于生成描述性错误消息。  itup 必须是一个“数据项”元组（在此处使用高密钥元组调用没有多大意义，因为不会显示有效的下行链路/块号）。
 *
 * Returns either a heap TID (which will be the first heap TID in posting list
 * if itup is posting list tuple), or a TID that contains downlink block
 * number, plus some encoded metadata (e.g., the number of attributes present
 * in itup).
 *
 * 返回堆 TID（如果 itup 是发布列表元组，则它将是发布列表中的第一个堆 TID），或者包含下行链路块编号以及一些编码元数据（例如，itup 中存在的属性数量）的 TID。
 */
static inline ItemPointer
BTreeTupleGetPointsToTID(IndexTuple itup)
{
	/*
	 * Rely on the assumption that !heapkeyspace internal page data items will
	 * correctly return TID with downlink here -- BTreeTupleGetHeapTID() won't
	 * recognize it as a pivot tuple, but everything still works out because
	 * the t_tid field is still returned
	 *
	 * 依赖于这样的假设：!heapkeyspace 内部页面数据项将正确返回带有下行链路的 TID - BTreeTupleGetHeapTID() 不会将其识别为枢轴元组，但一切仍然有效，因为仍然返回 t_tid 字段
	 */
	if (!BTreeTupleIsPivot(itup))
		return BTreeTupleGetHeapTID(itup);

	/* Pivot tuple returns TID with downlink block (heapkeyspace variant)
	 *
	 * 枢轴元组返回带有下行链路块的 TID（heapkeyspace 变体）
	 */
	return &itup->t_tid;
}

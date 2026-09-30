/*--------------------------------------------------------------------------
 * gin_private.h
 *	  header file for postgres inverted index access method implementation.
 *
 *	Copyright (c) 2006-2025, PostgreSQL Global Development Group
 *
 *	src/include/access/gin_private.h
 *--------------------------------------------------------------------------
 */
#ifndef GIN_PRIVATE_H
#define GIN_PRIVATE_H

#include "access/amapi.h"
#include "access/gin.h"
#include "access/ginblock.h"
#include "access/itup.h"
#include "common/int.h"
#include "catalog/pg_am_d.h"
#include "fmgr.h"
#include "lib/rbtree.h"
#include "storage/bufmgr.h"

/*
 * Storage type for GIN's reloptions
 */

/*
 * GIN 的 reloptions（关系选项）的存储类型。
 */
typedef struct GinOptions
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */

	/* varlena 头部（不要直接触碰！） */
	bool		useFastUpdate;	/* use fast updates? */

	/* 是否使用快速更新？ */
	int			pendingListCleanupSize; /* maximum size of pending list */

	/* 挂起列表的最大大小 */
} GinOptions;

#define GIN_DEFAULT_USE_FASTUPDATE	true
#define GinGetUseFastUpdate(relation) \
	(AssertMacro(relation->rd_rel->relkind == RELKIND_INDEX && \
				 relation->rd_rel->relam == GIN_AM_OID), \
	 (relation)->rd_options ? \
	 ((GinOptions *) (relation)->rd_options)->useFastUpdate : GIN_DEFAULT_USE_FASTUPDATE)
#define GinGetPendingListCleanupSize(relation) \
	(AssertMacro(relation->rd_rel->relkind == RELKIND_INDEX && \
				 relation->rd_rel->relam == GIN_AM_OID), \
	 (relation)->rd_options && \
	 ((GinOptions *) (relation)->rd_options)->pendingListCleanupSize != -1 ? \
	 ((GinOptions *) (relation)->rd_options)->pendingListCleanupSize : \
	 gin_pending_list_limit)


/* Macros for buffer lock/unlock operations */

/* 用于缓冲区加锁/解锁操作的宏 */
#define GIN_UNLOCK	BUFFER_LOCK_UNLOCK
#define GIN_SHARE	BUFFER_LOCK_SHARE
#define GIN_EXCLUSIVE  BUFFER_LOCK_EXCLUSIVE


/*
 * GinState: working data structure describing the index being worked on
 */

/*
 * GinState：描述当前正在处理的索引的工作数据结构。
 */
typedef struct GinState
{
	Relation	index;
	bool		oneCol;			/* true if single-column index */

	/* 若为单列索引则为 true */

	/*
	 * origTupdesc is the nominal tuple descriptor of the index, ie, the i'th
	 * attribute shows the key type (not the input data type!) of the i'th
	 * index column.  In a single-column index this describes the actual leaf
	 * index tuples.  In a multi-column index, the actual leaf tuples contain
	 * a smallint column number followed by a key datum of the appropriate
	 * type for that column.  We set up tupdesc[i] to describe the actual
	 * rowtype of the index tuples for the i'th column, ie, (int2, keytype).
	 * Note that in any case, leaf tuples contain more data than is known to
	 * the TupleDesc; see access/gin/README for details.
	 */

	/*
	 * origTupdesc 是索引的名义元组描述符，即第 i 个属性表示第 i 个索引列的键类型
	 *（而非输入数据类型！）。在单列索引中，它描述的是实际的叶子索引元组。
	 * 在多列索引中，实际的叶子元组包含一个 smallint 列号，
	 * 其后是一个适合该列类型的键 datum。我们将 tupdesc[i] 设置为
	 * 描述第 i 列索引元组的实际行类型，即 (int2, keytype)。
	 * 注意在任何情况下，叶子元组所包含的数据都比 TupleDesc 所知的要多；
	 * 详见 access/gin/README。
	 */
	TupleDesc	origTupdesc;
	TupleDesc	tupdesc[INDEX_MAX_KEYS];

	/*
	 * Per-index-column opclass support functions
	 */

	/*
	 * 每个索引列的操作符类（opclass）支持函数。
	 */
	FmgrInfo	compareFn[INDEX_MAX_KEYS];
	FmgrInfo	extractValueFn[INDEX_MAX_KEYS];
	FmgrInfo	extractQueryFn[INDEX_MAX_KEYS];
	FmgrInfo	consistentFn[INDEX_MAX_KEYS];
	FmgrInfo	triConsistentFn[INDEX_MAX_KEYS];
	FmgrInfo	comparePartialFn[INDEX_MAX_KEYS];	/* optional method */

	/* 可选方法 */
	/* canPartialMatch[i] is true if comparePartialFn[i] is valid */

	/* 若 comparePartialFn[i] 有效，则 canPartialMatch[i] 为 true */
	bool		canPartialMatch[INDEX_MAX_KEYS];
	/* Collations to pass to the support functions */

	/* 传递给支持函数的排序规则（collation） */
	Oid			supportCollation[INDEX_MAX_KEYS];
} GinState;


/* ginutil.c */

/* ginutil.c */

/*
 * Validate and parse a GIN index's reloptions text array into a GinOptions
 * struct; when validate is true, invalid options raise an error.
 *
 * 校验并解析 GIN 索引的 reloptions 文本数组，将其转换为 GinOptions 结构体；
 * 当 validate 为 true 时，非法选项会引发错误。
 */
extern bytea *ginoptions(Datum reloptions, bool validate);

/*
 * Initialize a GinState for the given index by looking up per-column opclass
 * support functions, tuple descriptors, and collations for later use.
 *
 * 为给定索引初始化一个 GinState，查找每列的操作符类支持函数、
 * 元组描述符以及排序规则，供后续使用。
 */
extern void initGinState(GinState *state, Relation index);

/*
 * Allocate a new (or recyclable) page from a GIN index, returning a pinned and
 * exclusively locked buffer ready to be initialized.
 *
 * 从 GIN 索引中分配一个新的（或可回收的）页面，
 * 返回一个已固定并已加排他锁、可供初始化的缓冲区。
 */
extern Buffer GinNewBuffer(Relation index);

/*
 * Initialize the page held in buffer b as a GIN page with the given flags,
 * setting up its opaque data and page header.
 *
 * 将缓冲区 b 中保存的页面初始化为带有给定标志的 GIN 页面，
 * 并设置其不透明数据和页头。
 */
extern void GinInitBuffer(Buffer b, uint32 f);

/*
 * Initialize the given page image as a GIN page of the given size and flags,
 * used when the page is not accessed through a buffer.
 *
 * 将给定的页面映像初始化为具有给定大小和标志的 GIN 页面，
 * 用于该页面不是通过缓冲区访问的场景。
 */
extern void GinInitPage(Page page, uint32 f, Size pageSize);

/*
 * Initialize the buffer b as a GIN metapage, filling in default metadata such
 * as an empty pending list and current version number.
 *
 * 将缓冲区 b 初始化为一个 GIN 元页，填入默认的元数据，
 * 例如空的挂起列表和当前版本号。
 */
extern void GinInitMetabuffer(Buffer b);

/*
 * Compare two key values of the same index attribute using the attribute's
 * compare support function, taking null categories into account.
 *
 * 使用某个索引属性的比较支持函数比较该属性的两个键值，
 * 并将 null 类别考虑在内。
 */
extern int	ginCompareEntries(GinState *ginstate, OffsetNumber attnum,
							  Datum a, GinNullCategory categorya,
							  Datum b, GinNullCategory categoryb);

/*
 * Compare two key entries that may belong to different index attributes,
 * ordering first by attribute number and then by value, for multi-column GIN.
 *
 * 比较可能属于不同索引属性的两个键条目，先按属性编号、再按值排序，
 * 用于多列 GIN 索引。
 */
extern int	ginCompareAttEntries(GinState *ginstate,
								 OffsetNumber attnuma, Datum a, GinNullCategory categorya,
								 OffsetNumber attnumb, Datum b, GinNullCategory categoryb);

/*
 * Extract the set of index key entries (and their null categories) from a
 * single indexed value by calling the column's extractValue support function.
 *
 * 通过调用某列的 extractValue 支持函数，
 * 从单个被索引的值中提取出索引键条目集合（及其 null 类别）。
 */
extern Datum *ginExtractEntries(GinState *ginstate, OffsetNumber attnum,
								Datum value, bool isNull,
								int32 *nentries, GinNullCategory **categories);

/*
 * Determine which index attribute number a given entry-tree index tuple
 * belongs to, handling both single- and multi-column indexes.
 *
 * 确定给定的条目树索引元组属于哪个索引属性编号，
 * 同时处理单列和多列索引的情况。
 */
extern OffsetNumber gintuple_get_attrnum(GinState *ginstate, IndexTuple tuple);

/*
 * Extract the key datum and its null category from an entry-tree index tuple,
 * reconstructing the original key value stored in the index.
 *
 * 从条目树索引元组中提取键 datum 及其 null 类别，
 * 重建存储在索引中的原始键值。
 */
extern Datum gintuple_get_key(GinState *ginstate, IndexTuple tuple,
							  GinNullCategory *category);

/*
 * Return the human-readable name of the GIN index-build progress phase
 * identified by phasenum, for progress reporting.
 *
 * 返回由 phasenum 标识的 GIN 索引构建进度阶段的可读名称，
 * 用于进度报告。
 */
extern char *ginbuildphasename(int64 phasenum);

/* gininsert.c */

/* gininsert.c */

/*
 * Build a complete GIN index over an existing heap: scan the table, accumulate
 * key entries, and write out the entry and posting-tree structures.
 *
 * 在已有的堆表上构建一个完整的 GIN 索引：扫描表、累积键条目，
 * 并写出条目树和倒排树结构。
 */
extern IndexBuildResult *ginbuild(Relation heap, Relation index,
								  struct IndexInfo *indexInfo);

/*
 * Build an empty GIN index (metapage and root) in the init fork, used for
 * unlogged relations.
 *
 * 在 init fork 中构建一个空的 GIN 索引（元页和根页），
 * 用于无日志（unlogged）关系。
 */
extern void ginbuildempty(Relation index);

/*
 * Insert one heap tuple's index entries into a GIN index, either via the
 * fast-update pending list or directly into the main structure.
 *
 * 将单个堆元组的索引条目插入到 GIN 索引中，
 * 既可以通过快速更新的挂起列表，也可以直接写入主结构。
 */
extern bool gininsert(Relation index, Datum *values, bool *isnull,
					  ItemPointer ht_ctid, Relation heapRel,
					  IndexUniqueCheck checkUnique,
					  bool indexUnchanged,
					  struct IndexInfo *indexInfo);

/*
 * Insert a single key and its associated item pointers into the GIN entry
 * tree, creating or extending a posting list or posting tree as needed.
 *
 * 将单个键及其关联的项指针插入到 GIN 条目树中，
 * 并按需创建或扩展倒排列表或倒排树。
 */
extern void ginEntryInsert(GinState *ginstate,
						   OffsetNumber attnum, Datum key, GinNullCategory category,
						   ItemPointerData *items, uint32 nitem,
						   GinStatsData *buildStats);

/* ginbtree.c */

typedef struct GinBtreeStack
{
	BlockNumber blkno;
	Buffer		buffer;
	OffsetNumber off;
	ItemPointerData iptr;
	/* predictNumber contains predicted number of pages on current level */

	/* predictNumber 包含对当前层级页面数量的预测值 */
	uint32		predictNumber;
	struct GinBtreeStack *parent;
} GinBtreeStack;

typedef struct GinBtreeData *GinBtree;

/* Return codes for GinBtreeData.beginPlaceToPage method */

/* GinBtreeData.beginPlaceToPage 方法的返回码 */
typedef enum
{
	GPTP_NO_WORK,
	GPTP_INSERT,
	GPTP_SPLIT,
} GinPlaceToPageRC;

typedef struct GinBtreeData
{
	/* search methods */

	/* 搜索方法 */
	BlockNumber (*findChildPage) (GinBtree, GinBtreeStack *);
	BlockNumber (*getLeftMostChild) (GinBtree, Page);
	bool		(*isMoveRight) (GinBtree, Page);
	bool		(*findItem) (GinBtree, GinBtreeStack *);

	/* insert methods */

	/* 插入方法 */
	OffsetNumber (*findChildPtr) (GinBtree, Page, BlockNumber, OffsetNumber);
	GinPlaceToPageRC (*beginPlaceToPage) (GinBtree, Buffer, GinBtreeStack *, void *, BlockNumber, void **, Page *, Page *);
	void		(*execPlaceToPage) (GinBtree, Buffer, GinBtreeStack *, void *, BlockNumber, void *);
	void	   *(*prepareDownlink) (GinBtree, Buffer);
	void		(*fillRoot) (GinBtree, Page, BlockNumber, Page, BlockNumber, Page);

	bool		isData;

	Relation	index;
	BlockNumber rootBlkno;
	GinState   *ginstate;		/* not valid in a data scan */

	/* 在数据扫描中无效 */
	bool		fullScan;
	bool		isBuild;

	/* Search key for Entry tree */

	/* 用于条目树的搜索键 */
	OffsetNumber entryAttnum;
	Datum		entryKey;
	GinNullCategory entryCategory;

	/* Search key for data tree (posting tree) */

	/* 用于数据树（倒排树）的搜索键 */
	ItemPointerData itemptr;
} GinBtreeData;

/* This represents a tuple to be inserted to entry tree. */

/* 它表示一个将要插入到条目树中的元组。 */
typedef struct
{
	IndexTuple	entry;			/* tuple to insert */

	/* 要插入的元组 */
	bool		isDelete;		/* delete old tuple at same offset? */

	/* 是否删除同一偏移处的旧元组？ */
} GinBtreeEntryInsertData;

/*
 * This represents an itempointer, or many itempointers, to be inserted to
 * a data (posting tree) leaf page
 */

/*
 * 它表示一个或多个将要插入到数据（倒排树）叶子页中的项指针。
 */
typedef struct
{
	ItemPointerData *items;
	uint32		nitem;
	uint32		curitem;
} GinBtreeDataLeafInsertData;

/*
 * For internal data (posting tree) pages, the insertion payload is a
 * PostingItem
 */

/*
 * 对于内部数据（倒排树）页，插入的载荷（payload）是一个 PostingItem。
 */

/*
 * Descend a GIN b-tree (entry or posting tree) from the root to the leaf page
 * where the btree's search key belongs, returning a stack recording the path
 * taken; rootConflictCheck requests predicate-lock conflict checking.
 *
 * 从根节点向下遍历 GIN b-tree（条目树或倒排树），
 * 直到到达该 b-tree 搜索键所属的叶子页，返回记录所经路径的栈；
 * rootConflictCheck 请求进行谓词锁冲突检查。
 */
extern GinBtreeStack *ginFindLeafPage(GinBtree btree, bool searchMode,
									  bool rootConflictCheck);

/*
 * Follow the right link from the given buffer to its right sibling, acquiring
 * the requested lock on the new buffer and releasing the old one.
 *
 * 从给定缓冲区沿右链接前往其右兄弟页，
 * 在新缓冲区上获取所请求的锁并释放旧缓冲区。
 */
extern Buffer ginStepRight(Buffer buffer, Relation index, int lockmode);

/*
 * Free a GinBtreeStack chain, releasing any buffers still pinned by its
 * entries.
 *
 * 释放一条 GinBtreeStack 链，
 * 释放其各项仍固定着的所有缓冲区。
 */
extern void freeGinBtreeStack(GinBtreeStack *stack);

/*
 * Insert a value into a GIN b-tree at the leaf identified by stack, splitting
 * pages and propagating downlinks up the tree as needed to keep it balanced.
 *
 * 将一个值插入到由 stack 所标识的叶子处的 GIN b-tree 中，
 * 并按需分裂页面、将 downlink 向上层传播，以保持树的平衡。
 */
extern void ginInsertValue(GinBtree btree, GinBtreeStack *stack,
						   void *insertdata, GinStatsData *buildStats);

/* ginentrypage.c */

/* ginentrypage.c */

/*
 * Build an entry-tree index tuple for a given key and its posting data,
 * choosing between an inline posting list and a posting-tree pointer, and
 * optionally erroring if the tuple would exceed the maximum item size.
 *
 * 为给定的键及其倒排数据构建一个条目树索引元组，
 * 在内联倒排列表与倒排树指针之间做出选择，
 * 并可选地在该元组会超过最大项大小时报错。
 */
extern IndexTuple GinFormTuple(GinState *ginstate,
							   OffsetNumber attnum, Datum key, GinNullCategory category,
							   Pointer data, Size dataSize, int nipd, bool errorTooBig);

/*
 * Initialize a GinBtree structure so it can search or insert into the entry
 * tree for the given attribute and key, wiring up the entry-tree methods.
 *
 * 初始化一个 GinBtree 结构，使其能够对给定属性和键在条目树中进行搜索或插入，
 * 并挂接好条目树相关的方法。
 */
extern void ginPrepareEntryScan(GinBtree btree, OffsetNumber attnum,
								Datum key, GinNullCategory category,
								GinState *ginstate);

/*
 * Populate a newly created entry-tree root page after a root split, inserting
 * downlinks to the two children that resulted from the split.
 *
 * 在发生根分裂后填充新创建的条目树根页，
 * 插入指向分裂所产生的两个子页的 downlink。
 */
extern void ginEntryFillRoot(GinBtree btree, Page root, BlockNumber lblkno, Page lpage, BlockNumber rblkno, Page rpage);

/*
 * Read the item pointers stored in an entry-tree leaf tuple, decoding its
 * posting list and returning the array of TIDs along with the item count.
 *
 * 读取存储在条目树叶子元组中的项指针，解码其倒排列表，
 * 返回 TID 数组以及项的数量。
 */
extern ItemPointer ginReadTuple(GinState *ginstate, OffsetNumber attnum,
								IndexTuple itup, int *nitems);

/* gindatapage.c */

/* gindatapage.c */

/*
 * Decode and return the array of item pointers stored on a posting-tree leaf
 * page, skipping those at or before advancePast and reporting the item count.
 *
 * 解码并返回存储在倒排树叶子页上的项指针数组，
 * 跳过位于 advancePast 及其之前的项，并报告项的数量。
 */
extern ItemPointer GinDataLeafPageGetItems(Page page, int *nitems, ItemPointerData advancePast);

/*
 * Add all item pointers stored on a posting-tree leaf page into the given
 * TID bitmap, returning the number of items added.
 *
 * 将存储在倒排树叶子页上的所有项指针加入给定的 TID 位图，
 * 返回所添加项的数量。
 */
extern int	GinDataLeafPageGetItemsToTbm(Page page, TIDBitmap *tbm);

/*
 * Create a new posting tree to hold a large set of item pointers that no
 * longer fit inline, returning the block number of its root page.
 *
 * 创建一个新的倒排树来容纳一大批无法再内联存放的项指针，
 * 返回其根页的块号。
 */
extern BlockNumber createPostingTree(Relation index,
									 ItemPointerData *items, uint32 nitems,
									 GinStatsData *buildStats, Buffer entrybuffer);

/*
 * Insert a PostingItem into an internal posting-tree page at the given offset,
 * shifting subsequent items to make room.
 *
 * 在给定偏移处将一个 PostingItem 插入到内部倒排树页中，
 * 并移动后续各项以腾出空间。
 */
extern void GinDataPageAddPostingItem(Page page, PostingItem *data, OffsetNumber offset);

/*
 * Delete the PostingItem at the given offset from an internal posting-tree
 * page, compacting the remaining items.
 *
 * 从内部倒排树页中删除给定偏移处的 PostingItem，
 * 并对剩余各项进行紧缩。
 */
extern void GinPageDeletePostingItem(Page page, OffsetNumber offset);

/*
 * Insert a set of item pointers into an existing posting tree rooted at
 * rootBlkno, descending to the correct leaves and splitting pages as needed.
 *
 * 将一组项指针插入到以 rootBlkno 为根的已有倒排树中，
 * 向下到达正确的叶子页并按需分裂页面。
 */
extern void ginInsertItemPointers(Relation index, BlockNumber rootBlkno,
								  ItemPointerData *items, uint32 nitem,
								  GinStatsData *buildStats);

/*
 * Prepare a GinBtree for scanning a posting tree and descend to its leftmost
 * leaf page, returning a stack positioned at the start of the tree.
 *
 * 准备一个用于扫描倒排树的 GinBtree，并向下到达其最左侧的叶子页，
 * 返回定位在树起始处的栈。
 */
extern GinBtreeStack *ginScanBeginPostingTree(GinBtree btree, Relation index, BlockNumber rootBlkno);

/*
 * Populate a newly created posting-tree root page after a root split,
 * inserting downlinks to the two children produced by the split.
 *
 * 在发生根分裂后填充新创建的倒排树根页，
 * 插入指向分裂所产生的两个子页的 downlink。
 */
extern void ginDataFillRoot(GinBtree btree, Page root, BlockNumber lblkno, Page lpage, BlockNumber rblkno, Page rpage);

/*
 * This is declared in ginvacuum.c, but is passed between ginVacuumItemPointers
 * and ginVacuumPostingTreeLeaf and as an opaque struct, so we need a forward
 * declaration for it.
 */

/*
 * 它在 ginvacuum.c 中声明，但作为一个不透明结构体在 ginVacuumItemPointers
 * 与 ginVacuumPostingTreeLeaf 之间传递，因此我们需要为它提供一个前向声明。
 */
typedef struct GinVacuumState GinVacuumState;

/*
 * Vacuum a single posting-tree leaf page: remove dead item pointers using the
 * vacuum state's callback and recompress the remaining posting lists in place.
 *
 * 对单个倒排树叶子页执行 vacuum：使用 vacuum 状态的回调移除已失效的项指针，
 * 并就地对剩余的倒排列表重新压缩。
 */
extern void ginVacuumPostingTreeLeaf(Relation indexrel, Buffer buffer,
									 GinVacuumState *gvs);

/* ginscan.c */

/*
 * GinScanKeyData describes a single GIN index qualifier expression.
 *
 * From each qual expression, we extract one or more specific index search
 * conditions, which are represented by GinScanEntryData.  It's quite
 * possible for identical search conditions to be requested by more than
 * one qual expression, in which case we merge such conditions to have just
 * one unique GinScanEntry --- this is particularly important for efficiency
 * when dealing with full-index-scan entries.  So there can be multiple
 * GinScanKeyData.scanEntry pointers to the same GinScanEntryData.
 *
 * In each GinScanKeyData, nentries is the true number of entries, while
 * nuserentries is the number that extractQueryFn returned (which is what
 * we report to consistentFn).  The "user" entries must come first.
 */

/*
 * GinScanKeyData 描述单个 GIN 索引限定表达式。
 *
 * 我们从每个限定（qual）表达式中提取出一个或多个具体的索引搜索条件，
 * 它们由 GinScanEntryData 表示。完全有可能出现多个限定表达式请求相同搜索条件的情况，
 * 这种情况下我们会合并这些条件，使其只对应一个唯一的 GinScanEntry ——
 * 在处理全索引扫描条目时，这对效率尤为重要。因此可能存在多个
 * GinScanKeyData.scanEntry 指针指向同一个 GinScanEntryData。
 *
 * 在每个 GinScanKeyData 中，nentries 是条目的真实数量，而 nuserentries 是
 * extractQueryFn 返回的数量（也就是我们报告给 consistentFn 的数量）。
 * “用户”条目必须排在前面。
 */
typedef struct GinScanKeyData *GinScanKey;

typedef struct GinScanEntryData *GinScanEntry;

typedef struct GinScanKeyData
{
	/* Real number of entries in scanEntry[] (always > 0) */

	/* scanEntry[] 中条目的实际数量（始终 > 0） */
	uint32		nentries;
	/* Number of entries that extractQueryFn and consistentFn know about */

	/* extractQueryFn 和 consistentFn 所知晓的条目数量 */
	uint32		nuserentries;

	/* array of GinScanEntry pointers, one per extracted search condition */

	/* GinScanEntry 指针数组，每个提取出的搜索条件对应一个 */
	GinScanEntry *scanEntry;

	/*
	 * At least one of the entries in requiredEntries must be present for a
	 * tuple to match the overall qual.
	 *
	 * additionalEntries contains entries that are needed by the consistent
	 * function to decide if an item matches, but are not sufficient to
	 * satisfy the qual without entries from requiredEntries.
	 */

	/*
	 * requiredEntries 中至少要有一个条目存在，元组才可能匹配整个限定条件。
	 *
	 * additionalEntries 包含一些条目，一致性函数需要它们来判断某个项是否匹配，
	 * 但如果没有来自 requiredEntries 的条目，仅凭它们不足以满足该限定条件。
	 */
	GinScanEntry *requiredEntries;
	int			nrequired;
	GinScanEntry *additionalEntries;
	int			nadditional;

	/* array of check flags, reported to consistentFn */

	/* 检查标志数组，会报告给 consistentFn */
	GinTernaryValue *entryRes;
	bool		(*boolConsistentFn) (GinScanKey key);
	GinTernaryValue (*triConsistentFn) (GinScanKey key);
	FmgrInfo   *consistentFmgrInfo;
	FmgrInfo   *triConsistentFmgrInfo;
	Oid			collation;

	/* other data needed for calling consistentFn */

	/* 调用 consistentFn 所需的其他数据 */
	Datum		query;
	/* NB: these three arrays have only nuserentries elements! */

	/* 注意：这三个数组只有 nuserentries 个元素！ */
	Datum	   *queryValues;
	GinNullCategory *queryCategories;
	Pointer    *extra_data;
	StrategyNumber strategy;
	int32		searchMode;
	OffsetNumber attnum;

	/*
	 * An excludeOnly scan key is not able to enumerate all matching tuples.
	 * That is, to be semantically correct on its own, it would need to have a
	 * GIN_CAT_EMPTY_QUERY scanEntry, but it doesn't.  Such a key can still be
	 * used to filter tuples returned by other scan keys, so we will get the
	 * right answers as long as there's at least one non-excludeOnly scan key
	 * for each index attribute considered by the search.  For efficiency
	 * reasons we don't want to have unnecessary GIN_CAT_EMPTY_QUERY entries,
	 * so we will convert an excludeOnly scan key to non-excludeOnly (by
	 * adding a GIN_CAT_EMPTY_QUERY scanEntry) only if there are no other
	 * non-excludeOnly scan keys.
	 */

	/*
	 * excludeOnly（仅排除）扫描键无法枚举出所有匹配的元组。
	 * 也就是说，若要它自身在语义上是正确的，它就需要有一个 GIN_CAT_EMPTY_QUERY
	 * 的 scanEntry，但它并没有。这样的键仍可用于过滤其他扫描键返回的元组，
	 * 因此只要对于搜索所涉及的每个索引属性都至少有一个非 excludeOnly 扫描键，
	 * 我们就能得到正确的结果。出于效率原因，我们不希望存在不必要的
	 * GIN_CAT_EMPTY_QUERY 条目，因此只有在不存在其他非 excludeOnly 扫描键时，
	 * 我们才会（通过添加一个 GIN_CAT_EMPTY_QUERY 的 scanEntry）将某个
	 * excludeOnly 扫描键转换为非 excludeOnly。
	 */
	bool		excludeOnly;

	/*
	 * Match status data.  curItem is the TID most recently tested (could be a
	 * lossy-page pointer).  curItemMatches is true if it passes the
	 * consistentFn test; if so, recheckCurItem is the recheck flag.
	 * isFinished means that all the input entry streams are finished, so this
	 * key cannot succeed for any later TIDs.
	 */

	/*
	 * 匹配状态数据。curItem 是最近一次测试的 TID（可能是有损页指针）。
	 * 如果它通过了 consistentFn 测试，则 curItemMatches 为 true；若是如此，
	 * recheckCurItem 就是重检查标志。isFinished 意味着所有输入条目流都已结束，
	 * 因此该键对于之后任何 TID 都不可能再成功匹配。
	 */
	ItemPointerData curItem;
	bool		curItemMatches;
	bool		recheckCurItem;
	bool		isFinished;
}			GinScanKeyData;

typedef struct GinScanEntryData
{
	/* query key and other information from extractQueryFn */

	/* 查询键以及来自 extractQueryFn 的其他信息 */
	Datum		queryKey;
	GinNullCategory queryCategory;
	bool		isPartialMatch;
	Pointer		extra_data;
	StrategyNumber strategy;
	int32		searchMode;
	OffsetNumber attnum;

	/* Current page in posting tree */

	/* 倒排树中的当前页 */
	Buffer		buffer;

	/* current ItemPointer to heap */

	/* 指向堆的当前 ItemPointer */
	ItemPointerData curItem;

	/* for a partial-match or full-scan query, we accumulate all TIDs here */

	/* 对于部分匹配或全扫描查询，我们在此累积所有 TID */
	TIDBitmap  *matchBitmap;
	TBMPrivateIterator *matchIterator;

	/*
	 * If blockno is InvalidBlockNumber, all of the other fields in the
	 * matchResult are meaningless.
	 */

	/*
	 * 如果 blockno 为 InvalidBlockNumber，则 matchResult 中所有其他字段都没有意义。
	 */
	TBMIterateResult matchResult;
	OffsetNumber matchOffsets[TBM_MAX_TUPLES_PER_PAGE];
	int			matchNtuples;

	/* used for Posting list and one page in Posting tree */

	/* 用于倒排列表以及倒排树中的某一页 */
	ItemPointerData *list;
	int			nlist;
	OffsetNumber offset;

	bool		isFinished;
	bool		reduceResult;
	uint32		predictNumberResult;
	GinBtreeData btree;
}			GinScanEntryData;

typedef struct GinScanOpaqueData
{
	MemoryContext tempCtx;
	GinState	ginstate;

	GinScanKey	keys;			/* one per scan qualifier expr */

	/* 每个扫描限定表达式对应一个 */
	uint32		nkeys;

	GinScanEntry *entries;		/* one per index search condition */

	/* 每个索引搜索条件对应一个 */
	uint32		totalentries;
	uint32		allocentries;	/* allocated length of entries[] */

	/* entries[] 已分配的长度 */

	MemoryContext keyCtx;		/* used to hold key and entry data */

	/* 用于保存键和条目数据 */

	bool		isVoidRes;		/* true if query is unsatisfiable */

	/* 如果查询不可满足则为 true */
} GinScanOpaqueData;

typedef GinScanOpaqueData *GinScanOpaque;

/*
 * Begin a GIN index scan: allocate and initialize the scan descriptor and its
 * GinScanOpaque state for the given number of scan keys and order-by clauses.
 *
 * 开始一次 GIN 索引扫描：为给定数量的扫描键和 order-by 子句
 * 分配并初始化扫描描述符及其 GinScanOpaque 状态。
 */
extern IndexScanDesc ginbeginscan(Relation rel, int nkeys, int norderbys);

/*
 * End a GIN index scan, releasing the scan keys, temporary memory contexts,
 * and any other resources held by the scan.
 *
 * 结束一次 GIN 索引扫描，释放扫描键、临时内存上下文
 * 以及该扫描持有的任何其他资源。
 */
extern void ginendscan(IndexScanDesc scan);

/*
 * Reset a GIN index scan with new scan keys and order-by clauses so the same
 * scan descriptor can be reused for another set of conditions.
 *
 * 使用新的扫描键和 order-by 子句重置一次 GIN 索引扫描，
 * 以便同一个扫描描述符可以被复用于另一组条件。
 */
extern void ginrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
					  ScanKey orderbys, int norderbys);

/*
 * Build the internal GinScanKey and GinScanEntry structures for a scan by
 * calling each qualifier's extractQuery support function and merging entries.
 *
 * 通过调用每个限定条件的 extractQuery 支持函数并合并条目，
 * 为一次扫描构建内部的 GinScanKey 和 GinScanEntry 结构。
 */
extern void ginNewScanKey(IndexScanDesc scan);

/*
 * Free the scan-key and entry structures held in the given GinScanOpaque,
 * releasing associated buffers and bitmaps.
 *
 * 释放给定 GinScanOpaque 中保存的扫描键和条目结构，
 * 并释放相关的缓冲区和位图。
 */
extern void ginFreeScanKeys(GinScanOpaque so);

/* ginget.c */

/* ginget.c */

/*
 * Execute a GIN index scan and add all matching heap TIDs to the given TID
 * bitmap, returning the number of tuples reported.
 *
 * 执行一次 GIN 索引扫描，将所有匹配的堆 TID 加入给定的 TID 位图，
 * 返回所报告的元组数量。
 */
extern int64 gingetbitmap(IndexScanDesc scan, TIDBitmap *tbm);

/* ginlogic.c */

/* ginlogic.c */

/*
 * Set up the boolean and tri-state consistent-function wrappers on a scan key,
 * choosing shortcuts where the opclass allows for efficient evaluation.
 *
 * 在某个扫描键上设置布尔型和三态一致性函数的包装器，
 * 并在操作符类允许高效求值处选择快捷路径。
 */
extern void ginInitConsistentFunction(GinState *ginstate, GinScanKey key);

/* ginvacuum.c */

/* ginvacuum.c */

/*
 * Bulk-delete phase of GIN vacuum: scan the index, remove index entries whose
 * heap TIDs are reported dead by the callback, and reclaim empty pages.
 *
 * GIN vacuum 的批量删除阶段：扫描索引，移除那些堆 TID 被回调判定为已失效的
 * 索引条目，并回收空页面。
 */
extern IndexBulkDeleteResult *ginbulkdelete(IndexVacuumInfo *info,
											IndexBulkDeleteResult *stats,
											IndexBulkDeleteCallback callback,
											void *callback_state);

/*
 * Cleanup phase of GIN vacuum: flush the pending list, update index statistics
 * in the metapage, and return final bulk-delete statistics.
 *
 * GIN vacuum 的清理阶段：刷新挂起列表，更新元页中的索引统计信息，
 * 并返回最终的批量删除统计数据。
 */
extern IndexBulkDeleteResult *ginvacuumcleanup(IndexVacuumInfo *info,
											   IndexBulkDeleteResult *stats);

/*
 * Remove dead item pointers from an array (typically a posting list), returning
 * the surviving pointers and reporting how many remain via nremaining.
 *
 * 从数组（通常是倒排列表）中移除已失效的项指针，返回存活的指针，
 * 并通过 nremaining 报告剩余的数量。
 */
extern ItemPointer ginVacuumItemPointers(GinVacuumState *gvs,
										 ItemPointerData *items, int nitem, int *nremaining);

/* ginvalidate.c */

/* ginvalidate.c */

/*
 * Validate the operator class with the given OID for GIN, checking that its
 * support functions and operators are complete and correctly typed.
 *
 * 校验给定 OID 的操作符类是否适用于 GIN，
 * 检查其支持函数和操作符是否完整且类型正确。
 */
extern bool ginvalidate(Oid opclassoid);

/*
 * Adjust the members of a GIN operator family during ALTER OPERATOR FAMILY,
 * setting the correct dependency and access-method-specific properties.
 *
 * 在 ALTER OPERATOR FAMILY 期间调整 GIN 操作符族的成员，
 * 设置正确的依赖关系以及访问方法特有的属性。
 */
extern void ginadjustmembers(Oid opfamilyoid,
							 Oid opclassoid,
							 List *operators,
							 List *functions);

/* ginbulk.c */

/* ginbulk.c */
typedef struct GinEntryAccumulator
{
	RBTNode		rbtnode;
	Datum		key;
	GinNullCategory category;
	OffsetNumber attnum;
	bool		shouldSort;
	ItemPointerData *list;
	uint32		maxcount;		/* allocated size of list[] */

	/* list[] 已分配的大小 */
	uint32		count;			/* current number of list[] entries */

	/* list[] 中当前的条目数量 */
} GinEntryAccumulator;

typedef struct
{
	GinState   *ginstate;
	Size		allocatedMemory;
	GinEntryAccumulator *entryallocator;
	uint32		eas_used;
	RBTree	   *tree;
	RBTreeIterator tree_walk;
} BuildAccumulator;

/*
 * Initialize a BuildAccumulator, setting up its red-black tree and memory
 * bookkeeping so key entries can be accumulated during index build.
 *
 * 初始化一个 BuildAccumulator，建立其红黑树和内存记账信息，
 * 以便在索引构建期间累积键条目。
 */
extern void ginInitBA(BuildAccumulator *accum);

/*
 * Add all key entries extracted from one heap tuple into the build
 * accumulator, associating each key with the heap TID for later sorting.
 *
 * 将从单个堆元组中提取的所有键条目加入构建累加器，
 * 并将每个键与其堆 TID 关联，以便后续排序。
 */
extern void ginInsertBAEntries(BuildAccumulator *accum,
							   ItemPointer heapptr, OffsetNumber attnum,
							   Datum *entries, GinNullCategory *categories,
							   int32 nentries);

/*
 * Begin an in-order traversal of the accumulator's red-black tree so that the
 * accumulated key entries can be retrieved in sorted order.
 *
 * 开始对累加器红黑树的中序遍历，
 * 以便按排序顺序取出所累积的键条目。
 */
extern void ginBeginBAScan(BuildAccumulator *accum);

/*
 * Return the next accumulated entry (its attribute, key, category, and the
 * array of TIDs) from the build accumulator's ongoing traversal.
 *
 * 从构建累加器正在进行的遍历中返回下一个累积的条目
 *（其属性、键、类别以及 TID 数组）。
 */
extern ItemPointerData *ginGetBAEntry(BuildAccumulator *accum,
									  OffsetNumber *attnum, Datum *key, GinNullCategory *category,
									  uint32 *n);

/* ginfast.c */

/* ginfast.c */
typedef struct GinTupleCollector
{
	IndexTuple *tuples;
	uint32		ntuples;
	uint32		lentuples;
	uint32		sumsize;
} GinTupleCollector;

/*
 * Insert the index tuples gathered in a GinTupleCollector into the index's
 * pending list, WAL-logging the change as part of the fast-update mechanism.
 *
 * 将 GinTupleCollector 中收集的索引元组插入到索引的挂起列表中，
 * 作为快速更新机制的一部分对该更改记录 WAL。
 */
extern void ginHeapTupleFastInsert(GinState *ginstate,
								   GinTupleCollector *collector);

/*
 * Extract index entries from one indexed value and append the resulting index
 * tuples to a GinTupleCollector for later fast insertion into the pending list.
 *
 * 从一个被索引的值中提取索引条目，并将生成的索引元组追加到
 * GinTupleCollector 中，以便随后快速插入到挂起列表。
 */
extern void ginHeapTupleFastCollect(GinState *ginstate,
									GinTupleCollector *collector,
									OffsetNumber attnum, Datum value, bool isNull,
									ItemPointer ht_ctid);

/*
 * Move entries out of the pending list and into the main GIN structure,
 * emptying the fast-update list; the flags control locking and how much of the
 * list is processed.
 *
 * 将条目从挂起列表移入 GIN 主结构，从而清空快速更新列表；
 * 各标志控制加锁方式以及处理列表的多少部分。
 */
extern void ginInsertCleanup(GinState *ginstate, bool full_clean,
							 bool fill_fsm, bool forceCleanup, IndexBulkDeleteResult *stats);

/* ginpostinglist.c */

/* ginpostinglist.c */

/*
 * Varbyte-encode an array of item pointers into a compact GinPostingList that
 * fits within maxsize bytes, reporting via nwritten how many items were packed.
 *
 * 将项指针数组进行变长字节（varbyte）编码，压缩为一个大小不超过 maxsize 字节的
 * 紧凑 GinPostingList，并通过 nwritten 报告打包了多少个项。
 */
extern GinPostingList *ginCompressPostingList(const ItemPointer ipd, int nipd,
											  int maxsize, int *nwritten);

/*
 * Decode all segments of a compressed posting list of the given byte length
 * and add every decoded item pointer directly into the given TID bitmap.
 *
 * 解码给定字节长度的压缩倒排列表的所有分段，
 * 并将每个解码出的项指针直接加入给定的 TID 位图。
 */
extern int	ginPostingListDecodeAllSegmentsToTbm(GinPostingList *ptr, int len, TIDBitmap *tbm);

/*
 * Decode all segments of a compressed posting list of the given byte length
 * into a newly allocated array of item pointers, reporting the count decoded.
 *
 * 将给定字节长度的压缩倒排列表的所有分段解码到一个新分配的项指针数组中，
 * 并报告解码出的数量。
 */
extern ItemPointer ginPostingListDecodeAllSegments(GinPostingList *segment, int len,
												   int *ndecoded_out);

/*
 * Decode a single compressed posting list into a newly allocated array of item
 * pointers, reporting how many were decoded via ndecoded_out.
 *
 * 将单个压缩倒排列表解码到一个新分配的项指针数组中，
 * 并通过 ndecoded_out 报告解码出的数量。
 */
extern ItemPointer ginPostingListDecode(GinPostingList *plist, int *ndecoded_out);

/*
 * Merge two sorted arrays of item pointers into a single sorted, duplicate-free
 * array, returning it and reporting the merged count via nmerged.
 *
 * 将两个已排序的项指针数组合并为一个已排序、去重的数组，
 * 返回该数组并通过 nmerged 报告合并后的数量。
 */
extern ItemPointer ginMergeItemPointers(ItemPointerData *a, uint32 na,
										ItemPointerData *b, uint32 nb,
										int *nmerged);

/*
 * Merging the results of several gin scans compares item pointers a lot,
 * so we want this to be inlined.
 */

/*
 * 合并多个 gin 扫描的结果会大量地比较项指针，因此我们希望它被内联。
 */

/*
 * Compare two item pointers for sort order by packing their block and offset
 * numbers into 64-bit integers and comparing those, returning negative, zero,
 * or positive as a<b, a==b, or a>b.
 *
 * 通过将两个项指针的块号和偏移号打包进 64 位整数并进行比较，
 * 来比较它们的排序顺序，返回负值、零或正值分别表示 a<b、a==b 或 a>b。
 */
static inline int
ginCompareItemPointers(ItemPointer a, ItemPointer b)
{
	uint64		ia = (uint64) GinItemPointerGetBlockNumber(a) << 32 | GinItemPointerGetOffsetNumber(a);
	uint64		ib = (uint64) GinItemPointerGetBlockNumber(b) << 32 | GinItemPointerGetOffsetNumber(b);

	return pg_cmp_u64(ia, ib);
}

/*
 * Acquire the appropriate lock on a GIN buffer while traversing the tree,
 * choosing share or exclusive mode depending on whether this is a search.
 *
 * 在遍历树时对某个 GIN 缓冲区获取适当的锁，
 * 根据是否为搜索操作来选择共享模式或排他模式。
 */
extern int	ginTraverseLock(Buffer buffer, bool searchMode);

#endif							/* GIN_PRIVATE_H */

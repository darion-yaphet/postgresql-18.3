/*-------------------------------------------------------------------------
 *
 * gist_private.h
 *	  private declarations for GiST -- declarations related to the
 *	  internal implementation of GiST, not the public API
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/gist_private.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef GIST_PRIVATE_H
#define GIST_PRIVATE_H

#include "access/amapi.h"
#include "access/gist.h"
#include "access/itup.h"
#include "lib/pairingheap.h"
#include "storage/bufmgr.h"
#include "storage/buffile.h"
#include "utils/hsearch.h"
#include "access/genam.h"

/*
 * Maximum number of "halves" a page can be split into in one operation.
 * Typically a split produces 2 halves, but can be more if keys have very
 * different lengths, or when inserting multiple keys in one operation (as
 * when inserting downlinks to an internal node).  There is no theoretical
 * limit on this, but in practice if you get more than a handful page halves
 * in one split, there's something wrong with the opclass implementation.
 * GIST_MAX_SPLIT_PAGES is an arbitrary limit on that, used to size some
 * local arrays used during split.  Note that there is also a limit on the
 * number of buffers that can be held locked at a time, MAX_SIMUL_LWLOCKS,
 * so if you raise this higher than that limit, you'll just get a different
 * error.
 */

/*
 * 在一次操作中一个页面可被分裂成的"半页"（halves）的最大数量。通常一次分裂产生
 * 2 个半页，但如果键的长度差异很大，或者在一次操作中插入多个键时（例如向内部节点
 * 插入 downlink），可能会更多。在理论上这没有上限，但在实践中，如果一次分裂产生的
 * 半页数量超过屈指可数的几个，那就说明 opclass 实现有问题。GIST_MAX_SPLIT_PAGES 是
 * 对此设定的一个任意上限，用于确定分裂期间使用的某些局部数组的大小。注意，同一时间
 * 可被持有锁的缓冲区数量还有一个限制 MAX_SIMUL_LWLOCKS，因此如果你把它设得高于该
 * 限制，只会得到一个不同的错误。
 */
#define GIST_MAX_SPLIT_PAGES		75

/* Buffer lock modes */

/* 缓冲区加锁模式 */
#define GIST_SHARE	BUFFER_LOCK_SHARE
#define GIST_EXCLUSIVE	BUFFER_LOCK_EXCLUSIVE
#define GIST_UNLOCK BUFFER_LOCK_UNLOCK

typedef struct
{
	BlockNumber prev;
	uint32		freespace;
	char		tupledata[FLEXIBLE_ARRAY_MEMBER];
} GISTNodeBufferPage;

#define BUFFER_PAGE_DATA_OFFSET MAXALIGN(offsetof(GISTNodeBufferPage, tupledata))
/* Returns free space in node buffer page */

/* 返回节点缓冲区页面中的空闲空间 */
#define PAGE_FREE_SPACE(nbp) (nbp->freespace)
/* Checks if node buffer page is empty */

/* 检查节点缓冲区页面是否为空 */
#define PAGE_IS_EMPTY(nbp) (nbp->freespace == BLCKSZ - BUFFER_PAGE_DATA_OFFSET)
/* Checks if node buffers page don't contain sufficient space for index tuple */

/* 检查节点缓冲区页面是否没有足够的空间容纳索引元组 */
#define PAGE_NO_SPACE(nbp, itup) (PAGE_FREE_SPACE(nbp) < \
										MAXALIGN(IndexTupleSize(itup)))

/*
 * GISTSTATE: information needed for any GiST index operation
 *
 * This struct retains call info for the index's opclass-specific support
 * functions (per index column), plus the index's tuple descriptor.
 *
 * scanCxt holds the GISTSTATE itself as well as any data that lives for the
 * lifetime of the index operation.  We pass this to the support functions
 * via fn_mcxt, so that they can store scan-lifespan data in it.  The
 * functions are invoked in tempCxt, which is typically short-lifespan
 * (that is, it's reset after each tuple).  However, tempCxt can be the same
 * as scanCxt if we're not bothering with per-tuple context resets.
 */

/*
 * GISTSTATE：任何 GiST 索引操作所需的信息。
 *
 * 此结构保存了索引的 opclass 专用支持函数（每个索引列一组）的调用信息，以及索引的
 * 元组描述符。
 *
 * scanCxt 保存 GISTSTATE 本身以及在索引操作生命周期内存活的任何数据。我们通过
 * fn_mcxt 把它传递给支持函数，以便它们可以在其中存储扫描生命周期的数据。这些函数
 * 在 tempCxt 中被调用，而 tempCxt 通常是短生命周期的（也就是说，它在处理每个元组后
 * 都会被重置）。不过，如果我们不需要每元组的上下文重置，tempCxt 也可以与 scanCxt
 * 相同。
 */
typedef struct GISTSTATE
{
	MemoryContext scanCxt;		/* context for scan-lifespan data */

	/* 用于扫描生命周期数据的上下文 */
	MemoryContext tempCxt;		/* short-term context for calling functions */

	/* 用于调用函数的短期上下文 */

	TupleDesc	leafTupdesc;	/* index's tuple descriptor */

	/* 索引的元组描述符 */
	TupleDesc	nonLeafTupdesc; /* truncated tuple descriptor for non-leaf
								 * pages */

	/* 用于非叶子页面的截断元组描述符 */
	TupleDesc	fetchTupdesc;	/* tuple descriptor for tuples returned in an
								 * index-only scan */

	/* 用于 index-only 扫描中返回的元组的元组描述符 */

	FmgrInfo	consistentFn[INDEX_MAX_KEYS];
	FmgrInfo	unionFn[INDEX_MAX_KEYS];
	FmgrInfo	compressFn[INDEX_MAX_KEYS];
	FmgrInfo	decompressFn[INDEX_MAX_KEYS];
	FmgrInfo	penaltyFn[INDEX_MAX_KEYS];
	FmgrInfo	picksplitFn[INDEX_MAX_KEYS];
	FmgrInfo	equalFn[INDEX_MAX_KEYS];
	FmgrInfo	distanceFn[INDEX_MAX_KEYS];
	FmgrInfo	fetchFn[INDEX_MAX_KEYS];

	/* Collations to pass to the support functions */

	/* 要传递给支持函数的排序规则（collation） */
	Oid			supportCollation[INDEX_MAX_KEYS];
} GISTSTATE;


/*
 * During a GiST index search, we must maintain a queue of unvisited items,
 * which can be either individual heap tuples or whole index pages.  If it
 * is an ordered search, the unvisited items should be visited in distance
 * order.  Unvisited items at the same distance should be visited in
 * depth-first order, that is heap items first, then lower index pages, then
 * upper index pages; this rule avoids doing extra work during a search that
 * ends early due to LIMIT.
 *
 * To perform an ordered search, we use a pairing heap to manage the
 * distance-order queue.  In a non-ordered search (no order-by operators),
 * we use it to return heap tuples before unvisited index pages, to
 * ensure depth-first order, but all entries are otherwise considered
 * equal.
 */

/*
 * 在一次 GiST 索引搜索期间，我们必须维护一个尚未访问项的队列，这些项既可以是单个
 * 堆元组，也可以是整个索引页面。如果是有序搜索，则应按距离顺序访问尚未访问的项。
 * 位于相同距离处的尚未访问项应按深度优先顺序访问，即先是堆项，然后是较低层的索引
 * 页面，最后是较高层的索引页面；这一规则可避免在因 LIMIT 而提前结束的搜索中做额外
 * 的工作。
 *
 * 为执行有序搜索，我们使用一个配对堆（pairing heap）来管理按距离排序的队列。在非
 * 有序搜索（没有 order-by 操作符）中，我们用它把堆元组排在尚未访问的索引页面之前，
 * 以确保深度优先顺序，而在其他方面所有条目都被视为相等。
 */

/* Individual heap tuple to be visited */

/* 将被访问的单个堆元组 */
typedef struct GISTSearchHeapItem
{
	ItemPointerData heapPtr;
	bool		recheck;		/* T if quals must be rechecked */

	/* 若为真，表示必须重新检查条件（quals） */
	bool		recheckDistances;	/* T if distances must be rechecked */

	/* 若为真，表示必须重新检查距离 */
	HeapTuple	recontup;		/* data reconstructed from the index, used in
								 * index-only scans */

	/* 从索引重建出的数据，用于 index-only 扫描 */
	OffsetNumber offnum;		/* track offset in page to mark tuple as
								 * LP_DEAD */

	/* 跟踪元组在页面中的偏移量，以便将其标记为 LP_DEAD */
} GISTSearchHeapItem;

/* Unvisited item, either index page or heap tuple */

/* 尚未访问的项，可以是索引页面或堆元组 */
typedef struct GISTSearchItem
{
	pairingheap_node phNode;
	BlockNumber blkno;			/* index page number, or InvalidBlockNumber */

	/* 索引页面编号，或 InvalidBlockNumber */
	union
	{
		GistNSN		parentlsn;	/* parent page's LSN, if index page */

		/* 父页面的 LSN，如果它是索引页面 */
		/* we must store parentlsn to detect whether a split occurred */

		/* 我们必须存储 parentlsn，以便检测是否发生了分裂 */
		GISTSearchHeapItem heap;	/* heap info, if heap tuple */

		/* 堆信息，如果它是堆元组 */
	}			data;

	/* numberOfOrderBys entries */

	/* numberOfOrderBys 个条目 */
	IndexOrderByDistance distances[FLEXIBLE_ARRAY_MEMBER];
} GISTSearchItem;

#define GISTSearchItemIsHeap(item)	((item).blkno == InvalidBlockNumber)

#define SizeOfGISTSearchItem(n_distances) \
	(offsetof(GISTSearchItem, distances) + \
	 sizeof(IndexOrderByDistance) * (n_distances))

/*
 * GISTScanOpaqueData: private state for a scan of a GiST index
 */

/*
 * GISTScanOpaqueData：一次 GiST 索引扫描的私有状态。
 */
typedef struct GISTScanOpaqueData
{
	GISTSTATE  *giststate;		/* index information, see above */

	/* 索引信息，参见上文 */
	Oid		   *orderByTypes;	/* datatypes of ORDER BY expressions */

	/* ORDER BY 表达式的数据类型 */

	pairingheap *queue;			/* queue of unvisited items */

	/* 尚未访问项的队列 */
	MemoryContext queueCxt;		/* context holding the queue */

	/* 持有该队列的内存上下文 */
	bool		qual_ok;		/* false if qual can never be satisfied */

	/* 若条件永远无法被满足，则为假 */
	bool		firstCall;		/* true until first gistgettuple call */

	/* 在第一次调用 gistgettuple 之前一直为真 */

	/* pre-allocated workspace arrays */

	/* 预分配的工作区数组 */
	IndexOrderByDistance *distances;	/* output area for gistindex_keytest */

	/* gistindex_keytest 的输出区域 */

	/* info about killed items if any (killedItems is NULL if never used) */

	/* 关于被杀死项的信息（如果有的话；若从未使用则 killedItems 为 NULL） */
	OffsetNumber *killedItems;	/* offset numbers of killed items */

	/* 被杀死项的偏移量编号 */
	int			numKilled;		/* number of currently stored items */

	/* 当前已存储项的数量 */
	BlockNumber curBlkno;		/* current number of block */

	/* 当前的块编号 */
	GistNSN		curPageLSN;		/* pos in the WAL stream when page was read */

	/* 读取页面时其在 WAL 流中的位置 */

	/* In a non-ordered search, returnable heap items are stored here: */

	/* 在非有序搜索中，可返回的堆项存储在这里： */
	GISTSearchHeapItem pageData[BLCKSZ / sizeof(IndexTupleData)];
	OffsetNumber nPageData;		/* number of valid items in array */

	/* 数组中有效项的数量 */
	OffsetNumber curPageData;	/* next item to return */

	/* 下一个要返回的项 */
	MemoryContext pageDataCxt;	/* context holding the fetched tuples, for
								 * index-only scans */

	/* 持有已获取元组的内存上下文，用于 index-only 扫描 */
} GISTScanOpaqueData;

typedef GISTScanOpaqueData *GISTScanOpaque;

/* despite the name, gistxlogPage is not part of any xlog record */

/* 尽管名字如此，gistxlogPage 并不是任何 xlog 记录的组成部分 */
typedef struct gistxlogPage
{
	BlockNumber blkno;
	int			num;			/* number of index tuples following */

	/* 紧随其后的索引元组的数量 */
} gistxlogPage;

/* SplitPageLayout - gistSplit function result */

/* SplitPageLayout —— gistSplit 函数的结果 */
typedef struct SplitPageLayout
{
	gistxlogPage block;
	IndexTupleData *list;
	int			lenlist;
	IndexTuple	itup;			/* union key for page */

	/* 页面的并集键（union key） */
	Page		page;			/* to operate */

	/* 用于操作的页面 */
	Buffer		buffer;			/* to write after all proceed */

	/* 在全部处理完成之后写入的缓冲区 */

	struct SplitPageLayout *next;
} SplitPageLayout;

/*
 * GISTInsertStack used for locking buffers and transfer arguments during
 * insertion
 */

/*
 * GISTInsertStack 在插入期间用于锁定缓冲区以及传递参数。
 */
typedef struct GISTInsertStack
{
	/* current page */

	/* 当前页面 */
	BlockNumber blkno;
	Buffer		buffer;
	Page		page;

	/*
	 * log sequence number from page->lsn to recognize page update and compare
	 * it with page's nsn to recognize page split
	 */

	/*
	 * 来自 page->lsn 的日志序列号，用于识别页面更新，并将其与页面的 nsn 进行比较
	 * 以识别页面分裂
	 */
	GistNSN		lsn;

	/*
	 * If set, we split the page while descending the tree to find an
	 * insertion target. It means that we need to retry from the parent,
	 * because the downlink of this page might no longer cover the new key.
	 */

	/*
	 * 如果设置了此标志，说明我们在沿树下行寻找插入目标的过程中分裂了该页面。这意味着
	 * 我们需要从父页面重试，因为此页面的 downlink 可能不再覆盖新的键。
	 */
	bool		retry_from_parent;

	/* offset of the downlink in the parent page, that points to this page */

	/* 父页面中指向此页面的 downlink 的偏移量 */
	OffsetNumber downlinkoffnum;

	/* pointer to parent */

	/* 指向父节点的指针 */
	struct GISTInsertStack *parent;
} GISTInsertStack;

/* Working state and results for multi-column split logic in gistsplit.c */

/* gistsplit.c 中多列分裂逻辑的工作状态与结果 */
typedef struct GistSplitVector
{
	GIST_SPLITVEC splitVector;	/* passed to/from user PickSplit method */

	/* 传入/传出用户的 PickSplit 方法 */

	Datum		spl_lattr[INDEX_MAX_KEYS];	/* Union of subkeys in
											 * splitVector.spl_left */

	/* splitVector.spl_left 中各子键的并集 */
	bool		spl_lisnull[INDEX_MAX_KEYS];

	Datum		spl_rattr[INDEX_MAX_KEYS];	/* Union of subkeys in
											 * splitVector.spl_right */

	/* splitVector.spl_right 中各子键的并集 */
	bool		spl_risnull[INDEX_MAX_KEYS];

	bool	   *spl_dontcare;	/* flags tuples which could go to either side
								 * of the split for zero penalty */

	/* 标记那些可以以零惩罚归入分裂任意一侧的元组 */
} GistSplitVector;

typedef struct
{
	Relation	r;
	Relation	heapRel;
	Size		freespace;		/* free space to be left */

	/* 需要保留的空闲空间 */
	bool		is_build;

	GISTInsertStack *stack;
} GISTInsertState;

/* root page of a gist index */

/* gist 索引的根页面 */
#define GIST_ROOT_BLKNO				0

/*
 * Before PostgreSQL 9.1, we used to rely on so-called "invalid tuples" on
 * inner pages to finish crash recovery of incomplete page splits. If a crash
 * happened in the middle of a page split, so that the downlink pointers were
 * not yet inserted, crash recovery inserted a special downlink pointer. The
 * semantics of an invalid tuple was that it if you encounter one in a scan,
 * it must always be followed, because we don't know if the tuples on the
 * child page match or not.
 *
 * We no longer create such invalid tuples, we now mark the left-half of such
 * an incomplete split with the F_FOLLOW_RIGHT flag instead, and finish the
 * split properly the next time we need to insert on that page. To retain
 * on-disk compatibility for the sake of pg_upgrade, we still store 0xffff as
 * the offset number of all inner tuples. If we encounter any invalid tuples
 * with 0xfffe during insertion, we throw an error, though scans still handle
 * them. You should only encounter invalid tuples if you pg_upgrade a pre-9.1
 * gist index which already has invalid tuples in it because of a crash. That
 * should be rare, and you are recommended to REINDEX anyway if you have any
 * invalid tuples in an index, so throwing an error is as far as we go with
 * supporting that.
 */

/*
 * 在 PostgreSQL 9.1 之前，我们曾依赖内部页面上所谓的"无效元组"（invalid tuples）来
 * 完成对未完成页面分裂的崩溃恢复。如果崩溃发生在页面分裂的中途，以致 downlink 指针
 * 尚未被插入，崩溃恢复会插入一个特殊的 downlink 指针。无效元组的语义是：如果你在扫描
 * 中遇到它，就必须总是跟随它，因为我们不知道子页面上的元组是否匹配。
 *
 * 我们不再创建这样的无效元组，现在改为用 F_FOLLOW_RIGHT 标志标记这类未完成分裂的左半
 * 部分，并在下一次需要在该页面上插入时正确地完成分裂。为了保持磁盘上的兼容性以便用于
 * pg_upgrade，我们仍然把所有内部元组的偏移量编号存储为 0xffff。如果在插入期间遇到任何
 * 偏移量为 0xfffe 的无效元组，我们会抛出一个错误，不过扫描仍然会处理它们。只有当你对一个
 * 9.1 之前的、因崩溃而已经含有无效元组的 gist 索引执行 pg_upgrade 时，才会遇到无效元组。
 * 这种情况应该很罕见，而且如果索引中含有任何无效元组，无论如何都建议你执行 REINDEX，
 * 因此抛出错误就是我们对此提供支持的极限。
 */
#define TUPLE_IS_VALID		0xffff
#define TUPLE_IS_INVALID	0xfffe

#define  GistTupleIsInvalid(itup)	( ItemPointerGetOffsetNumber( &((itup)->t_tid) ) == TUPLE_IS_INVALID )
#define  GistTupleSetValid(itup)	ItemPointerSetOffsetNumber( &((itup)->t_tid), TUPLE_IS_VALID )




/*
 * A buffer attached to an internal node, used when building an index in
 * buffering mode.
 */

/*
 * 附加在内部节点上的一个缓冲区，在以缓冲（buffering）模式构建索引时使用。
 */
typedef struct
{
	BlockNumber nodeBlocknum;	/* index block # this buffer is for */

	/* 此缓冲区所对应的索引块编号 */
	int32		blocksCount;	/* current # of blocks occupied by buffer */

	/* 该缓冲区当前占用的块数量 */

	BlockNumber pageBlocknum;	/* temporary file block # */

	/* 临时文件块编号 */
	GISTNodeBufferPage *pageBuffer; /* in-memory buffer page */

	/* 内存中的缓冲区页面 */

	/* is this buffer queued for emptying? */

	/* 此缓冲区是否已排队等待清空？ */
	bool		queuedForEmptying;

	/* is this a temporary copy, not in the hash table? */

	/* 这是否是一个临时副本，而非位于哈希表中？ */
	bool		isTemp;

	int			level;			/* 0 == leaf */

	/* 0 == 叶子层 */
} GISTNodeBuffer;

/*
 * Does specified level have buffers? (Beware of multiple evaluation of
 * arguments.)
 */

/*
 * 指定的层是否具有缓冲区？（注意参数的多次求值问题。）
 */
#define LEVEL_HAS_BUFFERS(nlevel, gfbb) \
	((nlevel) != 0 && (nlevel) % (gfbb)->levelStep == 0 && \
	 (nlevel) != (gfbb)->rootlevel)

/* Is specified buffer at least half-filled (should be queued for emptying)? */

/* 指定的缓冲区是否至少已被填充一半（应当排队等待清空）？ */
#define BUFFER_HALF_FILLED(nodeBuffer, gfbb) \
	((nodeBuffer)->blocksCount > (gfbb)->pagesPerBuffer / 2)

/*
 * Is specified buffer full? Our buffers can actually grow indefinitely,
 * beyond the "maximum" size, so this just means whether the buffer has grown
 * beyond the nominal maximum size.
 */

/*
 * 指定的缓冲区是否已满？我们的缓冲区实际上可以无限增长，超过"最大"尺寸，因此这只是
 * 表示该缓冲区是否已增长到超过其名义上的最大尺寸。
 */
#define BUFFER_OVERFLOWED(nodeBuffer, gfbb) \
	((nodeBuffer)->blocksCount > (gfbb)->pagesPerBuffer)

/*
 * Data structure with general information about build buffers.
 */

/*
 * 保存关于构建缓冲区（build buffers）的一般信息的数据结构。
 */
typedef struct GISTBuildBuffers
{
	/* Persistent memory context for the buffers and metadata. */

	/* 用于缓冲区及其元数据的持久性内存上下文。 */
	MemoryContext context;

	BufFile    *pfile;			/* Temporary file to store buffers in */

	/* 用于存储缓冲区的临时文件 */
	long		nFileBlocks;	/* Current size of the temporary file */

	/* 临时文件的当前大小 */

	/*
	 * resizable array of free blocks.
	 */

	/*
	 * 可调整大小的空闲块数组。
	 */
	long	   *freeBlocks;
	int			nFreeBlocks;	/* # of currently free blocks in the array */

	/* 数组中当前空闲块的数量 */
	int			freeBlocksLen;	/* current allocated length of the array */

	/* 该数组当前已分配的长度 */

	/* Hash for buffers by block number */

	/* 按块编号索引缓冲区的哈希表 */
	HTAB	   *nodeBuffersTab;

	/* List of buffers scheduled for emptying */

	/* 已被安排清空的缓冲区列表 */
	List	   *bufferEmptyingQueue;

	/*
	 * Parameters to the buffering build algorithm. levelStep determines which
	 * levels in the tree have buffers, and pagesPerBuffer determines how
	 * large each buffer is.
	 */

	/*
	 * 缓冲构建算法的参数。levelStep 决定树中哪些层具有缓冲区，pagesPerBuffer 决定
	 * 每个缓冲区有多大。
	 */
	int			levelStep;
	int			pagesPerBuffer;

	/* Array of lists of buffers on each level, for final emptying */

	/* 每一层上缓冲区列表的数组，用于最终的清空 */
	List	  **buffersOnLevels;
	int			buffersOnLevelsLen;

	/*
	 * Dynamically-sized array of buffers that currently have their last page
	 * loaded in main memory.
	 */

	/*
	 * 动态大小的缓冲区数组，其中的缓冲区当前把它们的最后一页加载在主内存中。
	 */
	GISTNodeBuffer **loadedBuffers;
	int			loadedBuffersCount; /* # of entries in loadedBuffers */

	/* loadedBuffers 中的条目数量 */
	int			loadedBuffersLen;	/* allocated size of loadedBuffers */

	/* loadedBuffers 已分配的大小 */

	/* Level of the current root node (= height of the index tree - 1) */

	/* 当前根节点的层级（= 索引树的高度 - 1） */
	int			rootlevel;
} GISTBuildBuffers;

/* GiSTOptions->buffering_mode values */

/* GiSTOptions->buffering_mode 的取值 */
typedef enum GistOptBufferingMode
{
	GIST_OPTION_BUFFERING_AUTO,
	GIST_OPTION_BUFFERING_ON,
	GIST_OPTION_BUFFERING_OFF,
} GistOptBufferingMode;

/*
 * Storage type for GiST's reloptions
 */

/*
 * GiST 的 reloptions 的存储类型。
 */
typedef struct GiSTOptions
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */

	/* varlena 头部（不要直接改动它！） */
	int			fillfactor;		/* page fill factor in percent (0..100) */

	/* 页面填充因子，以百分比表示（0..100） */
	GistOptBufferingMode buffering_mode;	/* buffering build mode */

	/* 缓冲构建模式 */
} GiSTOptions;

/* gist.c */

/*
 * Build an empty GiST index: initialize the metapage/root so that an
 * otherwise-empty (e.g. unlogged) index relation has a valid on-disk layout.
 */

/*
 * 构建一个空的 GiST 索引：初始化元页/根页，使得一个原本为空的（例如 unlogged）
 * 索引关系拥有有效的磁盘布局。
 */
extern void gistbuildempty(Relation index);
/*
 * Insert a single index entry into a GiST index: form an index tuple from the
 * given values and heap TID, then descend the tree and place it, splitting
 * pages as needed.  This is the aminsert entry point for GiST.
 */

/*
 * 向 GiST 索引插入单个索引条目：根据给定的值和堆 TID 构造一个索引元组，然后沿树下行
 * 并放置它，必要时分裂页面。这是 GiST 的 aminsert 入口点。
 */
extern bool gistinsert(Relation r, Datum *values, bool *isnull,
					   ItemPointer ht_ctid, Relation heapRel,
					   IndexUniqueCheck checkUnique,
					   bool indexUnchanged,
					   struct IndexInfo *indexInfo);

/*
 * Create and return a short-lived memory context suitable for use as the
 * per-tuple temporary context during GiST support-function calls.
 */

/*
 * 创建并返回一个短生命周期的内存上下文，适合在调用 GiST 支持函数期间用作每元组的
 * 临时上下文。
 */
extern MemoryContext createTempGistContext(void);

/*
 * Allocate and initialize a GISTSTATE for the given index: look up and cache
 * the opclass support functions and build the leaf/non-leaf/fetch tuple
 * descriptors used throughout an index operation.
 */

/*
 * 为给定索引分配并初始化一个 GISTSTATE：查找并缓存 opclass 支持函数，并构建在整个
 * 索引操作过程中使用的叶子/非叶子/获取（fetch）元组描述符。
 */
extern GISTSTATE *initGISTstate(Relation index);

/*
 * Release a GISTSTATE previously created by initGISTstate, freeing the scan
 * context and all associated resources.
 */

/*
 * 释放先前由 initGISTstate 创建的 GISTSTATE，释放其扫描上下文以及所有相关联的资源。
 */
extern void freeGISTstate(GISTSTATE *giststate);

/*
 * Insert an already-formed index tuple into a GiST index: descend from the
 * root to a suitable leaf page following penalty-minimizing downlinks, then
 * place the tuple, handling page splits and downlink adjustments as required.
 */

/*
 * 将一个已构造好的索引元组插入 GiST 索引：从根页面开始，沿着惩罚（penalty）最小化的
 * downlink 下行到合适的叶子页面，然后放置该元组，并按需处理页面分裂和 downlink 的调整。
 */
extern void gistdoinsert(Relation r,
						 IndexTuple itup,
						 Size freespace,
						 GISTSTATE *giststate,
						 Relation heapRel,
						 bool is_build);

/* A List of these is returned from gistplacetopage() in *splitinfo */

/* gistplacetopage() 通过 *splitinfo 返回由这些结构组成的一个 List */
typedef struct
{
	Buffer		buf;			/* the split page "half" */

	/* 分裂产生的"半个"页面 */
	IndexTuple	downlink;		/* downlink for this half. */

	/* 该半页的 downlink。 */
} GISTPageSplitInfo;

/*
 * Place tuples onto a single GiST page, splitting the page if they do not
 * fit: on a split, return the resulting page halves and their downlinks via
 * *splitinfo and emit the appropriate WAL.  Returns whether a split occurred.
 */

/*
 * 将元组放置到单个 GiST 页面上，如果它们放不下就分裂该页面：发生分裂时，通过
 * *splitinfo 返回所产生的各半页及其 downlink，并写出相应的 WAL。返回是否发生了分裂。
 */
extern bool gistplacetopage(Relation rel, Size freespace, GISTSTATE *giststate,
							Buffer buffer,
							IndexTuple *itup, int ntup,
							OffsetNumber oldoffnum, BlockNumber *newblkno,
							Buffer leftchildbuf,
							List **splitinfo,
							bool markfollowright,
							Relation heapRel,
							bool is_build);

/*
 * Split the tuples of a GiST page into a chain of SplitPageLayout entries:
 * apply the opclass PickSplit logic (recursively across columns) to partition
 * the tuples and compute the union key for each resulting page half.
 */

/*
 * 将一个 GiST 页面的元组分裂成一条 SplitPageLayout 条目链：应用 opclass 的 PickSplit
 * 逻辑（跨列递归地）来划分这些元组，并为每个产生的半页计算其并集键。
 */
extern SplitPageLayout *gistSplit(Relation r, Page page, IndexTuple *itup,
								  int len, GISTSTATE *giststate);

/* gistxlog.c */

/*
 * Write a WAL record for a GiST page deletion and return its LSN: log the
 * deletion of the page along with the parent's downlink offset and the xid
 * beyond which the page becomes reusable.
 */

/*
 * 为一次 GiST 页面删除写入一条 WAL 记录并返回其 LSN：记录该页面的删除，同时记录父页面
 * 的 downlink 偏移量以及在此之后该页面变得可重用的 xid。
 */
extern XLogRecPtr gistXLogPageDelete(Buffer buffer,
									 FullTransactionId xid, Buffer parentBuffer,
									 OffsetNumber downlinkOffset);

/*
 * Write a WAL record noting that a previously deleted GiST page is being
 * reused, recording the information hot standby needs to resolve snapshot
 * conflicts against the reused page.
 */

/*
 * 写入一条 WAL 记录，表明先前被删除的 GiST 页面正在被重用，记录热备（hot standby）为
 * 解决针对被重用页面的快照冲突所需的信息。
 */
extern void gistXLogPageReuse(Relation rel, Relation heaprel, BlockNumber blkno,
							  FullTransactionId deleteXid);

/*
 * Write a WAL record for an in-place GiST page update and return its LSN:
 * log the offsets to delete and the tuples to insert, optionally together
 * with the left child buffer that completes a split.
 */

/*
 * 为一次原地（in-place）GiST 页面更新写入一条 WAL 记录并返回其 LSN：记录要删除的偏移量
 * 以及要插入的元组，并可选地一并记录用于完成分裂的左子缓冲区。
 */
extern XLogRecPtr gistXLogUpdate(Buffer buffer,
								 OffsetNumber *todelete, int ntodelete,
								 IndexTuple *itup, int ituplen,
								 Buffer leftchildbuf);

/*
 * Write a WAL record for deletion of leaf index tuples during vacuum and
 * return its LSN: record the deleted offsets and the snapshot conflict
 * horizon used for recovery-conflict handling on standbys.
 */

/*
 * 为 vacuum 期间删除叶子索引元组写入一条 WAL 记录并返回其 LSN：记录被删除的偏移量以及
 * 用于备库上恢复冲突处理的快照冲突边界（snapshot conflict horizon）。
 */
extern XLogRecPtr gistXLogDelete(Buffer buffer, OffsetNumber *todelete,
								 int ntodelete, TransactionId snapshotConflictHorizon,
								 Relation heaprel);

/*
 * Write a WAL record for a GiST page split and return its LSN: log the chain
 * of resulting pages, the original page's rightlink and NSN, and whether the
 * F_FOLLOW_RIGHT flag should be set during replay.
 */

/*
 * 为一次 GiST 页面分裂写入一条 WAL 记录并返回其 LSN：记录所产生页面的链、原始页面的
 * rightlink 和 NSN，以及在重放期间是否应设置 F_FOLLOW_RIGHT 标志。
 */
extern XLogRecPtr gistXLogSplit(bool page_is_leaf,
								SplitPageLayout *dist,
								BlockNumber origrlink, GistNSN orignsn,
								Buffer leftchildbuf, bool markfollowright);

/*
 * Write a no-op GiST WAL record solely to obtain and assign a new LSN,
 * returning it; used to advance the page LSN when no other WAL is generated.
 */

/*
 * 写入一条空操作（no-op）的 GiST WAL 记录，其唯一目的是获取并分配一个新的 LSN 并返回它；
 * 用于在没有产生其他 WAL 时推进页面 LSN。
 */
extern XLogRecPtr gistXLogAssignLSN(void);

/* gistget.c */

/*
 * Fetch the next matching tuple from an ordered or unordered GiST scan:
 * advance the search queue, returning heap TIDs in the required order, and
 * report whether another tuple was found.
 */

/*
 * 从有序或无序的 GiST 扫描中获取下一个匹配的元组：推进搜索队列，按所需顺序返回堆 TID，
 * 并报告是否还找到了另一个元组。
 */
extern bool gistgettuple(IndexScanDesc scan, ScanDirection dir);

/*
 * Fetch all tuples matching a GiST scan into a TIDBitmap and return the count:
 * traverse the index collecting every qualifying heap TID for a bitmap scan.
 */

/*
 * 将匹配某次 GiST 扫描的所有元组获取到一个 TIDBitmap 中并返回数量：遍历索引，为位图扫描
 * 收集每一个符合条件的堆 TID。
 */
extern int64 gistgetbitmap(IndexScanDesc scan, TIDBitmap *tbm);

/*
 * Report whether the given GiST index column can return original values in an
 * index-only scan, i.e. whether the opclass provides a fetch function for it.
 */

/*
 * 报告给定的 GiST 索引列在 index-only 扫描中是否能够返回原始值，也就是说该 opclass 是否
 * 为它提供了获取（fetch）函数。
 */
extern bool gistcanreturn(Relation index, int attno);

/* gistvalidate.c */

/*
 * Validate the definition of a GiST operator class: check that its required
 * support functions and operators are present and have compatible signatures.
 */

/*
 * 验证一个 GiST 操作符类的定义：检查其所需的支持函数和操作符是否存在，并且具有兼容的
 * 签名。
 */
extern bool gistvalidate(Oid opclassoid);

/*
 * Adjust the dependency and member metadata for a GiST operator family after
 * ALTER OPERATOR FAMILY, setting proper dependencies for its operators and
 * support functions.
 */

/*
 * 在 ALTER OPERATOR FAMILY 之后调整某个 GiST 操作符族的依赖关系和成员元数据，为其操作符
 * 和支持函数设置恰当的依赖关系。
 */
extern void gistadjustmembers(Oid opfamilyoid,
							  Oid opclassoid,
							  List *operators,
							  List *functions);

/* gistutil.c */

#define GiSTPageSize   \
	( BLCKSZ - SizeOfPageHeaderData - MAXALIGN(sizeof(GISTPageOpaqueData)) )

#define GIST_MIN_FILLFACTOR			10
#define GIST_DEFAULT_FILLFACTOR		90

/*
 * Parse and validate the reloptions for a GiST index, returning the packed
 * GiSTOptions bytea used to configure fillfactor and buffering mode.
 */

/*
 * 解析并验证某个 GiST 索引的 reloptions，返回打包后的 GiSTOptions bytea，用于配置
 * 填充因子和缓冲模式。
 */
extern bytea *gistoptions(Datum reloptions, bool validate);

/*
 * Report GiST-specific index properties (amproperty callback): answer queries
 * such as whether a column supports ordering or returning, filling in *res
 * and *isnull accordingly.
 */

/*
 * 报告 GiST 专用的索引属性（amproperty 回调）：回答诸如某列是否支持排序或返回值之类的
 * 查询，并相应地填充 *res 和 *isnull。
 */
extern bool gistproperty(Oid index_oid, int attno,
						 IndexAMProperty prop, const char *propname,
						 bool *res, bool *isnull);

/*
 * Test whether the given vector of index tuples fits onto a single GiST page,
 * returning true if they do.
 */

/*
 * 检测给定的索引元组向量是否能放入单个 GiST 页面，如果能则返回真。
 */
extern bool gistfitpage(IndexTuple *itvec, int len);

/*
 * Test whether a GiST page lacks room to hold the given tuples after removing
 * one existing tuple and reserving the requested free space.
 */

/*
 * 检测在移除一个现有元组并保留所请求的空闲空间之后，某个 GiST 页面是否没有足够空间来
 * 容纳给定的元组。
 */
extern bool gistnospace(Page page, IndexTuple *itvec, int len, OffsetNumber todelete, Size freespace);

/*
 * Sanity-check that a buffer really contains a valid GiST page, raising an
 * error if the page header or special area does not look like a GiST page.
 */

/*
 * 完整性检查，确认某个缓冲区确实包含有效的 GiST 页面，如果页面头部或特殊区域看起来不像
 * GiST 页面则抛出错误。
 */
extern void gistcheckpage(Relation rel, Buffer buf);

/*
 * Obtain a buffer for a new GiST page: reuse a recyclable deleted page if one
 * is available, otherwise extend the relation, returning an exclusively
 * locked buffer.
 */

/*
 * 为一个新的 GiST 页面获取缓冲区：如果有可回收的已删除页面则重用它，否则扩展该关系，
 * 返回一个已加排他锁的缓冲区。
 */
extern Buffer gistNewBuffer(Relation r, Relation heaprel);

/*
 * Test whether a deleted GiST page can now be safely recycled, i.e. whether
 * its recorded delete xid is old enough that no scan could still be using it.
 */

/*
 * 检测一个已删除的 GiST 页面现在是否可以被安全地回收，也就是说其记录的删除 xid 是否已经
 * 足够老，以致没有任何扫描仍可能在使用它。
 */
extern bool gistPageRecyclable(Page page);

/*
 * Add a vector of index tuples to a GiST page starting at the given offset,
 * writing them into the page's line pointer array.
 */

/*
 * 从给定的偏移量开始，将一个索引元组向量添加到某个 GiST 页面中，把它们写入该页面的行指针
 * 数组。
 */
extern void gistfillbuffer(Page page, IndexTuple *itup, int len,
						   OffsetNumber off);

/*
 * Extract all index tuples from a GiST page into a newly palloc'd array and
 * return it, reporting the number of tuples via *len.
 */

/*
 * 将某个 GiST 页面中的所有索引元组提取到一个新 palloc 分配的数组中并返回它，通过 *len
 * 报告元组的数量。
 */
extern IndexTuple *gistextractpage(Page page, int *len /* out */ );

/*
 * Concatenate two vectors of index tuples into a single newly allocated
 * vector and return it, updating *len with the combined length.
 */

/*
 * 将两个索引元组向量拼接成一个新分配的向量并返回它，用合并后的长度更新 *len。
 */
extern IndexTuple *gistjoinvector(IndexTuple *itvec, int *len,
								  IndexTuple *additvec, int addlen);

/*
 * Pack a vector of index tuples into a contiguous IndexTupleData buffer and
 * return it, reporting the total byte length via *memlen.
 */

/*
 * 将一个索引元组向量打包进一段连续的 IndexTupleData 缓冲区并返回它，通过 *memlen 报告
 * 总字节长度。
 */
extern IndexTupleData *gistfillitupvec(IndexTuple *vec, int veclen, int *memlen);

/*
 * Compute the union key covering a vector of index tuples and return it as a
 * new index tuple, by invoking the opclass union support function per column.
 */

/*
 * 计算覆盖某个索引元组向量的并集键，并将其作为一个新的索引元组返回，方法是对每一列调用
 * opclass 的 union 支持函数。
 */
extern IndexTuple gistunion(Relation r, IndexTuple *itvec,
							int len, GISTSTATE *giststate);

/*
 * Return an adjusted copy of oldtup that also covers addtup, or NULL if
 * oldtup already covers it; used to widen a downlink key when needed.
 */

/*
 * 返回 oldtup 的一个经过调整、同时也覆盖 addtup 的副本，如果 oldtup 已经覆盖它则返回
 * NULL；用于在需要时扩宽某个 downlink 键。
 */
extern IndexTuple gistgetadjusted(Relation r,
								  IndexTuple oldtup,
								  IndexTuple addtup,
								  GISTSTATE *giststate);

/*
 * Form a GiST index tuple from the given attribute values by compressing them
 * with the opclass compress functions, producing a leaf or non-leaf tuple.
 */

/*
 * 通过用 opclass 的 compress 函数压缩给定的属性值，从这些值构造一个 GiST 索引元组，
 * 生成叶子或非叶子元组。
 */
extern IndexTuple gistFormTuple(GISTSTATE *giststate,
								Relation r, const Datum *attdata, const bool *isnull, bool isleaf);

/*
 * Compress a set of attribute values using the opclass compress functions,
 * writing the resulting compressed datums into the compatt output array.
 */

/*
 * 使用 opclass 的 compress 函数压缩一组属性值，将得到的压缩后 datum 写入 compatt 输出
 * 数组。
 */
extern void gistCompressValues(GISTSTATE *giststate, Relation r,
							   const Datum *attdata, const bool *isnull, bool isleaf, Datum *compatt);

/*
 * Choose the child downlink on a GiST page that would incur the least penalty
 * when inserting the given tuple, returning its offset number.
 */

/*
 * 在某个 GiST 页面上选择插入给定元组时惩罚（penalty）最小的子 downlink，返回其偏移量编号。
 */
extern OffsetNumber gistchoose(Relation r, Page p,
							   IndexTuple it,
							   GISTSTATE *giststate);

/*
 * Initialize an already-pinned buffer as a fresh GiST page with the given
 * flags, setting up the page header and GiST opaque area.
 */

/*
 * 将一个已被 pin 的缓冲区初始化为一个带有给定标志的全新 GiST 页面，设置好页面头部和 GiST
 * 不透明区域。
 */
extern void GISTInitBuffer(Buffer b, uint32 f);

/*
 * Initialize a page image as an empty GiST page with the given flags, setting
 * up the page header and GiST opaque data without touching a buffer.
 */

/*
 * 将一个页面镜像初始化为一个带有给定标志的空 GiST 页面，设置好页面头部和 GiST 不透明数据，
 * 而不涉及任何缓冲区。
 */
extern void gistinitpage(Page page, uint32 f);

/*
 * Initialize a GISTENTRY for the given key by running the opclass decompress
 * function, preparing the entry for use by other support functions.
 */

/*
 * 通过运行 opclass 的 decompress 函数为给定的键初始化一个 GISTENTRY，使该条目可供其他
 * 支持函数使用。
 */
extern void gistdentryinit(GISTSTATE *giststate, int nkey, GISTENTRY *e,
						   Datum k, Relation r, Page pg, OffsetNumber o,
						   bool l, bool isNull);

/*
 * Compute the penalty of inserting the add entry into the orig entry for one
 * column by calling the opclass penalty function, returning the penalty value.
 */

/*
 * 通过调用 opclass 的 penalty 函数，计算将 add 条目插入某一列的 orig 条目中的惩罚，
 * 返回该惩罚值。
 */
extern float gistpenalty(GISTSTATE *giststate, int attno,
						 GISTENTRY *orig, bool isNullOrig,
						 GISTENTRY *add, bool isNullAdd);

/*
 * Compute the per-column union of a vector of index tuples, writing the union
 * datums and null flags into the attr/isnull output arrays.
 */

/*
 * 计算某个索引元组向量的按列并集，将并集 datum 和空值标志写入 attr/isnull 输出数组。
 */
extern void gistMakeUnionItVec(GISTSTATE *giststate, IndexTuple *itvec, int len,
							   Datum *attr, bool *isnull);

/*
 * Test whether two key datums for the given column are equal according to the
 * opclass equal function, returning true if they are.
 */

/*
 * 根据 opclass 的 equal 函数检测给定列的两个键 datum 是否相等，如果相等则返回真。
 */
extern bool gistKeyIsEQ(GISTSTATE *giststate, int attno, Datum a, Datum b);

/*
 * Decompress all attributes of an index tuple into an array of GISTENTRY
 * structs and null flags, preparing them for use by other support functions.
 */

/*
 * 将某个索引元组的所有属性解压到一个 GISTENTRY 结构体数组及空值标志数组中，使它们可供
 * 其他支持函数使用。
 */
extern void gistDeCompressAtt(GISTSTATE *giststate, Relation r, IndexTuple tuple, Page p,
							  OffsetNumber o, GISTENTRY *attdata, bool *isnull);

/*
 * Reconstruct the original heap tuple values from a compressed GiST index
 * tuple using the opclass fetch functions, for use in index-only scans.
 */

/*
 * 使用 opclass 的 fetch 函数从一个压缩的 GiST 索引元组重建出原始的堆元组值，用于
 * index-only 扫描。
 */
extern HeapTuple gistFetchTuple(GISTSTATE *giststate, Relation r,
								IndexTuple tuple);

/*
 * Compute the union of two GISTENTRY keys for one column, writing the result
 * datum and null flag into *dst and *dstisnull.
 */

/*
 * 计算某一列的两个 GISTENTRY 键的并集，将结果 datum 和空值标志写入 *dst 和 *dstisnull。
 */
extern void gistMakeUnionKey(GISTSTATE *giststate, int attno,
							 GISTENTRY *entry1, bool isnull1,
							 GISTENTRY *entry2, bool isnull2,
							 Datum *dst, bool *dstisnull);

/*
 * Generate a fake LSN for an unlogged or temporary GiST relation, providing a
 * monotonically increasing value usable in place of a real WAL LSN.
 */

/*
 * 为一个 unlogged 或临时的 GiST 关系生成一个伪造的 LSN，提供一个单调递增的值，可用于
 * 替代真实的 WAL LSN。
 */
extern XLogRecPtr gistGetFakeLSN(Relation rel);

/* gistvacuum.c */

/*
 * Bulk-delete index entries during VACUUM by scanning the GiST index and
 * removing tuples for which the callback returns true, accumulating stats.
 */

/*
 * 在 VACUUM 期间批量删除索引条目，方法是扫描 GiST 索引并移除那些回调返回真的元组，同时
 * 累积统计信息。
 */
extern IndexBulkDeleteResult *gistbulkdelete(IndexVacuumInfo *info,
											 IndexBulkDeleteResult *stats,
											 IndexBulkDeleteCallback callback,
											 void *callback_state);

/*
 * Perform post-VACUUM cleanup of a GiST index: reclaim empty pages, update
 * the free space map, and return final index statistics.
 */

/*
 * 执行 GiST 索引在 VACUUM 之后的清理：回收空页面，更新空闲空间映射（FSM），并返回最终的
 * 索引统计信息。
 */
extern IndexBulkDeleteResult *gistvacuumcleanup(IndexVacuumInfo *info,
												IndexBulkDeleteResult *stats);

/* gistsplit.c */

/*
 * Split a set of index tuples by key, recursing across index columns to
 * resolve ties: fill the GistSplitVector with the left/right partition and
 * the union keys for each side.
 */

/*
 * 按键分裂一组索引元组，跨索引列递归以解决平局（ties）：用左/右划分以及每一侧的并集键
 * 填充 GistSplitVector。
 */
extern void gistSplitByKey(Relation r, Page page, IndexTuple *itup,
						   int len, GISTSTATE *giststate,
						   GistSplitVector *v,
						   int attno);

/* gistbuild.c */

/*
 * Build a GiST index over an existing heap (ambuild entry point): scan the
 * heap and insert all rows, optionally using buffering mode, returning build
 * statistics.
 */

/*
 * 在一个已有的堆之上构建 GiST 索引（ambuild 入口点）：扫描堆并插入所有行，可选地使用缓冲
 * 模式，返回构建统计信息。
 */
extern IndexBuildResult *gistbuild(Relation heap, Relation index,
								   struct IndexInfo *indexInfo);

/* gistbuildbuffers.c */

/*
 * Initialize the build-buffers bookkeeping used by buffering-mode index
 * builds, allocating the temporary file and metadata structures.
 */

/*
 * 初始化缓冲模式索引构建所使用的构建缓冲区记账信息，分配临时文件和元数据结构。
 */
extern GISTBuildBuffers *gistInitBuildBuffers(int pagesPerBuffer, int levelStep,
											  int maxLevel);

/*
 * Get (creating if necessary) the node buffer associated with a given index
 * block and tree level, for use during buffering-mode index builds.
 */

/*
 * 获取（必要时创建）与给定索引块和树层级相关联的节点缓冲区，供缓冲模式索引构建期间使用。
 */
extern GISTNodeBuffer *gistGetNodeBuffer(GISTBuildBuffers *gfbb,
										 GISTSTATE *giststate,
										 BlockNumber nodeBlocknum, int level);

/*
 * Push an index tuple into a node buffer, spilling to the temporary file as
 * needed, during a buffering-mode index build.
 */

/*
 * 在缓冲模式索引构建期间，将一个索引元组压入某个节点缓冲区，必要时溢出（spill）到临时
 * 文件中。
 */
extern void gistPushItupToNodeBuffer(GISTBuildBuffers *gfbb,
									 GISTNodeBuffer *nodeBuffer, IndexTuple itup);

/*
 * Pop an index tuple from a node buffer, reading back from the temporary file
 * as needed; return false if the buffer is empty.
 */

/*
 * 从某个节点缓冲区弹出一个索引元组，必要时从临时文件回读；如果缓冲区为空则返回假。
 */
extern bool gistPopItupFromNodeBuffer(GISTBuildBuffers *gfbb,
									  GISTNodeBuffer *nodeBuffer, IndexTuple *itup);

/*
 * Release all resources held by the build buffers, including the temporary
 * file and in-memory metadata, at the end of a buffering-mode build.
 */

/*
 * 在缓冲模式构建结束时，释放构建缓冲区持有的所有资源，包括临时文件和内存中的元数据。
 */
extern void gistFreeBuildBuffers(GISTBuildBuffers *gfbb);

/*
 * Relocate the tuples held in a node buffer among the new pages produced by a
 * page split during a buffering-mode build, keeping buffers consistent with
 * the tree structure.
 */

/*
 * 在缓冲模式构建期间，将某个节点缓冲区中持有的元组重新分配到页面分裂所产生的新页面之间，
 * 使缓冲区与树结构保持一致。
 */
extern void gistRelocateBuildBuffersOnSplit(GISTBuildBuffers *gfbb,
											GISTSTATE *giststate, Relation r,
											int level, Buffer buffer,
											List *splitinfo);

/*
 * Unload all currently in-memory node buffer pages to the temporary file,
 * freeing main memory during a buffering-mode index build.
 */

/*
 * 将当前所有位于内存中的节点缓冲区页面卸载到临时文件，在缓冲模式索引构建期间释放主内存。
 */
extern void gistUnloadNodeBuffers(GISTBuildBuffers *gfbb);

#endif							/* GIST_PRIVATE_H */

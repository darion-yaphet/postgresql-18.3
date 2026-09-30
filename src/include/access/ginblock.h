/*--------------------------------------------------------------------------
 * ginblock.h
 *	  details of structures stored in GIN index blocks
 *
 *	Copyright (c) 2006-2025, PostgreSQL Global Development Group
 *
 *	src/include/access/ginblock.h
 *--------------------------------------------------------------------------
 */
#ifndef GINBLOCK_H
#define GINBLOCK_H

#include "access/transam.h"
#include "storage/block.h"
#include "storage/bufpage.h"
#include "storage/itemptr.h"
#include "storage/off.h"

/*
 * Page opaque data in an inverted index page.
 *
 * Note: GIN does not include a page ID word as do the other index types.
 * This is OK because the opaque data is only 8 bytes and so can be reliably
 * distinguished by size.  Revisit this if the size ever increases.
 * Further note: as of 9.2, SP-GiST also uses 8-byte special space, as does
 * BRIN as of 9.5.  This is still OK, as long as GIN isn't using all of the
 * high-order bits in its flags word, because that way the flags word cannot
 * match the page IDs used by SP-GiST and BRIN.
 */

/*
 * 倒排索引页中的页面不透明数据（opaque data）。
 *
 * 注意：GIN 不像其他索引类型那样包含页 ID 字。
 * 这是可以的，因为不透明数据只有 8 字节，因此可以通过大小可靠地区分。
 * 如果该大小以后有所增加，需重新考虑这一点。
 * 另注：自 9.2 起，SP-GiST 也使用 8 字节的特殊空间，BRIN 自 9.5 起同样如此。
 * 这仍然没有问题，只要 GIN 没有用满其 flags 字中的全部高位，
 * 因为这样 flags 字就不会与 SP-GiST 和 BRIN 使用的页 ID 相匹配。
 */
typedef struct GinPageOpaqueData
{
	BlockNumber rightlink;		/* next page if any */

	/* 下一页（如果有的话） */
	OffsetNumber maxoff;		/* number of PostingItems on GIN_DATA &
								 * ~GIN_LEAF page. On GIN_LIST page, number of
								 * heap tuples. */

	/*
	 * 在 GIN_DATA 且非 GIN_LEAF 的页上，为 PostingItem 的数量。
	 * 在 GIN_LIST 页上，为堆元组的数量。
	 */
	uint16		flags;			/* see bit definitions below */

	/* 参见下面的位定义 */
} GinPageOpaqueData;

typedef GinPageOpaqueData *GinPageOpaque;

#define GIN_DATA		  (1 << 0)
#define GIN_LEAF		  (1 << 1)
#define GIN_DELETED		  (1 << 2)
#define GIN_META		  (1 << 3)
#define GIN_LIST		  (1 << 4)
#define GIN_LIST_FULLROW  (1 << 5)	/* makes sense only on GIN_LIST page */

/* 仅在 GIN_LIST 页上有意义 */
#define GIN_INCOMPLETE_SPLIT (1 << 6)	/* page was split, but parent not
										 * updated */

/* 页面已被分裂，但父页面尚未更新 */
#define GIN_COMPRESSED	  (1 << 7)

/* Page numbers of fixed-location pages */

/* 固定位置页面的页号 */
#define GIN_METAPAGE_BLKNO	(0)
#define GIN_ROOT_BLKNO		(1)

typedef struct GinMetaPageData
{
	/*
	 * Pointers to head and tail of pending list, which consists of GIN_LIST
	 * pages.  These store fast-inserted entries that haven't yet been moved
	 * into the regular GIN structure.
	 */

	/*
	 * 指向挂起列表（pending list）头部和尾部的指针，挂起列表由 GIN_LIST 页组成。
	 * 这些页存放通过快速插入写入、但尚未迁移到常规 GIN 结构中的条目。
	 */
	BlockNumber head;
	BlockNumber tail;

	/*
	 * Free space in bytes in the pending list's tail page.
	 */

	/*
	 * 挂起列表尾页中的空闲空间，以字节为单位。
	 */
	uint32		tailFreeSize;

	/*
	 * We store both number of pages and number of heap tuples that are in the
	 * pending list.
	 */

	/*
	 * 我们同时存储挂起列表中的页数和堆元组数。
	 */
	BlockNumber nPendingPages;
	int64		nPendingHeapTuples;

	/*
	 * Statistics for planner use (accurate as of last VACUUM)
	 */

	/*
	 * 供规划器使用的统计信息（截至上一次 VACUUM 时是准确的）。
	 */
	BlockNumber nTotalPages;
	BlockNumber nEntryPages;
	BlockNumber nDataPages;
	int64		nEntries;

	/*
	 * GIN version number (ideally this should have been at the front, but too
	 * late now.  Don't move it!)
	 *
	 * Currently 2 (for indexes initialized in 9.4 or later)
	 *
	 * Version 1 (indexes initialized in version 9.1, 9.2 or 9.3), is
	 * compatible, but may contain uncompressed posting tree (leaf) pages and
	 * posting lists. They will be converted to compressed format when
	 * modified.
	 *
	 * Version 0 (indexes initialized in 9.0 or before) is compatible but may
	 * be missing null entries, including both null keys and placeholders.
	 * Reject full-index-scan attempts on such indexes.
	 */

	/*
	 * GIN 版本号（理想情况下它本应放在结构体最前面，但现在为时已晚。不要移动它！）
	 *
	 * 当前为 2（对应在 9.4 或更高版本中初始化的索引）
	 *
	 * 版本 1（在 9.1、9.2 或 9.3 版本中初始化的索引）是兼容的，
	 * 但可能包含未压缩的倒排树（叶子）页和倒排列表。
	 * 它们在被修改时会被转换为压缩格式。
	 *
	 * 版本 0（在 9.0 或更早版本中初始化的索引）是兼容的，
	 * 但可能缺少 null 条目，包括 null 键和占位符。
	 * 对此类索引应拒绝全索引扫描的尝试。
	 */
	int32		ginVersion;
} GinMetaPageData;

#define GIN_CURRENT_VERSION		2

#define GinPageGetMeta(p) \
	((GinMetaPageData *) PageGetContents(p))

/*
 * Macros for accessing a GIN index page's opaque data
 */

/*
 * 用于访问 GIN 索引页面不透明数据的宏。
 */
#define GinPageGetOpaque(page) ( (GinPageOpaque) PageGetSpecialPointer(page) )

#define GinPageIsLeaf(page)    ( (GinPageGetOpaque(page)->flags & GIN_LEAF) != 0 )
#define GinPageSetLeaf(page)   ( GinPageGetOpaque(page)->flags |= GIN_LEAF )
#define GinPageSetNonLeaf(page)    ( GinPageGetOpaque(page)->flags &= ~GIN_LEAF )
#define GinPageIsData(page)    ( (GinPageGetOpaque(page)->flags & GIN_DATA) != 0 )
#define GinPageSetData(page)   ( GinPageGetOpaque(page)->flags |= GIN_DATA )
#define GinPageIsList(page)    ( (GinPageGetOpaque(page)->flags & GIN_LIST) != 0 )
#define GinPageSetList(page)   ( GinPageGetOpaque(page)->flags |= GIN_LIST )
#define GinPageHasFullRow(page)    ( (GinPageGetOpaque(page)->flags & GIN_LIST_FULLROW) != 0 )
#define GinPageSetFullRow(page)   ( GinPageGetOpaque(page)->flags |= GIN_LIST_FULLROW )
#define GinPageIsCompressed(page)	 ( (GinPageGetOpaque(page)->flags & GIN_COMPRESSED) != 0 )
#define GinPageSetCompressed(page)	 ( GinPageGetOpaque(page)->flags |= GIN_COMPRESSED )

#define GinPageIsDeleted(page) ( (GinPageGetOpaque(page)->flags & GIN_DELETED) != 0 )
#define GinPageSetDeleted(page)    ( GinPageGetOpaque(page)->flags |= GIN_DELETED)
#define GinPageSetNonDeleted(page) ( GinPageGetOpaque(page)->flags &= ~GIN_DELETED)
#define GinPageIsIncompleteSplit(page) ( (GinPageGetOpaque(page)->flags & GIN_INCOMPLETE_SPLIT) != 0 )

#define GinPageRightMost(page) ( GinPageGetOpaque(page)->rightlink == InvalidBlockNumber)

/*
 * We should reclaim deleted page only once every transaction started before
 * its deletion is over.
 */

/*
 * 只有在删除该页之前启动的每个事务都已结束后，我们才应回收已删除的页面。
 */
#define GinPageGetDeleteXid(page) ( ((PageHeader) (page))->pd_prune_xid )
#define GinPageSetDeleteXid(page, xid) ( ((PageHeader) (page))->pd_prune_xid = xid)

/*
 * Return whether a deleted GIN page can now be safely recycled, i.e. no
 * concurrent transaction that could still have seen it in a scan remains
 * running, by comparing its recorded delete XID against the global horizon.
 *
 * 通过将页面记录的删除 XID 与全局水平线（horizon）进行比较，
 * 返回某个已删除的 GIN 页面现在是否可以被安全回收，
 * 即不再有任何仍可能在扫描中看到它的并发事务在运行。
 */
extern bool GinPageIsRecyclable(Page page);

/*
 * We use our own ItemPointerGet(BlockNumber|OffsetNumber)
 * to avoid Asserts, since sometimes the ip_posid isn't "valid"
 */

/*
 * 我们使用自己的 ItemPointerGet(BlockNumber|OffsetNumber) 来避免 Assert，
 * 因为有时 ip_posid 并不是“有效的”。
 */
#define GinItemPointerGetBlockNumber(pointer) \
	(ItemPointerGetBlockNumberNoCheck(pointer))

#define GinItemPointerGetOffsetNumber(pointer) \
	(ItemPointerGetOffsetNumberNoCheck(pointer))

#define GinItemPointerSetBlockNumber(pointer, blkno) \
	(ItemPointerSetBlockNumber((pointer), (blkno)))

#define GinItemPointerSetOffsetNumber(pointer, offnum) \
	(ItemPointerSetOffsetNumber((pointer), (offnum)))


/*
 * Special-case item pointer values needed by the GIN search logic.
 *	MIN: sorts less than any valid item pointer
 *	MAX: sorts greater than any valid item pointer
 *	LOSSY PAGE: indicates a whole heap page, sorts after normal item
 *				pointers for that page
 * Note that these are all distinguishable from an "invalid" item pointer
 * (which is InvalidBlockNumber/0) as well as from all normal item
 * pointers (which have item numbers in the range 1..MaxHeapTuplesPerPage).
 */

/*
 * GIN 搜索逻辑所需的特殊项指针取值。
 *	MIN：排序时小于任何有效的项指针
 *	MAX：排序时大于任何有效的项指针
 *	LOSSY PAGE（有损页）：表示整个堆页，排序时位于该页正常项指针之后
 * 注意，这些取值都可以与“无效”项指针（即 InvalidBlockNumber/0）
 * 以及所有正常项指针（其项编号范围为 1..MaxHeapTuplesPerPage）相区分。
 */
#define ItemPointerSetMin(p)  \
	ItemPointerSet((p), (BlockNumber)0, (OffsetNumber)0)
#define ItemPointerIsMin(p)  \
	(GinItemPointerGetOffsetNumber(p) == (OffsetNumber)0 && \
	 GinItemPointerGetBlockNumber(p) == (BlockNumber)0)
#define ItemPointerSetMax(p)  \
	ItemPointerSet((p), InvalidBlockNumber, (OffsetNumber)0xffff)
#define ItemPointerSetLossyPage(p, b)  \
	ItemPointerSet((p), (b), (OffsetNumber)0xffff)
#define ItemPointerIsLossyPage(p)  \
	(GinItemPointerGetOffsetNumber(p) == (OffsetNumber)0xffff && \
	 GinItemPointerGetBlockNumber(p) != InvalidBlockNumber)

/*
 * Posting item in a non-leaf posting-tree page
 */

/*
 * 非叶子倒排树页面中的倒排项（posting item）。
 */
typedef struct
{
	/* We use BlockIdData not BlockNumber to avoid padding space wastage */

	/* 我们使用 BlockIdData 而非 BlockNumber，以避免填充空间的浪费 */
	BlockIdData child_blkno;
	ItemPointerData key;
} PostingItem;

#define PostingItemGetBlockNumber(pointer) \
	BlockIdGetBlockNumber(&(pointer)->child_blkno)

#define PostingItemSetBlockNumber(pointer, blockNumber) \
	BlockIdSet(&((pointer)->child_blkno), (blockNumber))

/*
 * Category codes to distinguish placeholder nulls from ordinary NULL keys.
 *
 * The first two code values were chosen to be compatible with the usual usage
 * of bool isNull flags.  However, casting between bool and GinNullCategory is
 * risky because of the possibility of different bit patterns and type sizes,
 * so it is no longer done.
 *
 * GIN_CAT_EMPTY_QUERY is never stored in the index; and notice that it is
 * chosen to sort before not after regular key values.
 */

/*
 * 用于区分占位符 null 和普通 NULL 键的类别码。
 *
 * 前两个码值的选择是为了与 bool isNull 标志的常规用法相兼容。
 * 然而，在 bool 与 GinNullCategory 之间进行强制转换是有风险的，
 * 因为它们的位模式和类型大小可能不同，所以现在不再这样做了。
 *
 * GIN_CAT_EMPTY_QUERY 从不会存储到索引中；并且注意它被选定为
 * 排序时位于普通键值之前而非之后。
 */
typedef signed char GinNullCategory;

#define GIN_CAT_NORM_KEY		0	/* normal, non-null key value */

/* 普通的非空键值 */
#define GIN_CAT_NULL_KEY		1	/* null key value */

/* 空（null）键值 */
#define GIN_CAT_EMPTY_ITEM		2	/* placeholder for zero-key item */

/* 零键项的占位符 */
#define GIN_CAT_NULL_ITEM		3	/* placeholder for null item */

/* null 项的占位符 */
#define GIN_CAT_EMPTY_QUERY		(-1)	/* placeholder for full-scan query */

/* 全扫描查询的占位符 */

/*
 * Access macros for null category byte in entry tuples
 */

/*
 * 用于访问条目元组中 null 类别字节的宏。
 */
#define GinCategoryOffset(itup,ginstate) \
	(IndexInfoFindDataOffset((itup)->t_info) + \
	 ((ginstate)->oneCol ? 0 : sizeof(int16)))
#define GinGetNullCategory(itup,ginstate) \
	(*((GinNullCategory *) ((char*)(itup) + GinCategoryOffset(itup,ginstate))))
#define GinSetNullCategory(itup,ginstate,c) \
	(*((GinNullCategory *) ((char*)(itup) + GinCategoryOffset(itup,ginstate))) = (c))

/*
 * Access macros for leaf-page entry tuples (see discussion in README)
 */

/*
 * 用于访问叶子页条目元组的宏（参见 README 中的讨论）。
 */
#define GinGetNPosting(itup)	GinItemPointerGetOffsetNumber(&(itup)->t_tid)
#define GinSetNPosting(itup,n)	ItemPointerSetOffsetNumber(&(itup)->t_tid,n)
#define GIN_TREE_POSTING		((OffsetNumber)0xffff)
#define GinIsPostingTree(itup)	(GinGetNPosting(itup) == GIN_TREE_POSTING)
#define GinSetPostingTree(itup, blkno)	( GinSetNPosting((itup),GIN_TREE_POSTING), ItemPointerSetBlockNumber(&(itup)->t_tid, blkno) )
#define GinGetPostingTree(itup) GinItemPointerGetBlockNumber(&(itup)->t_tid)

#define GIN_ITUP_COMPRESSED		(1U << 31)
#define GinGetPostingOffset(itup)	(GinItemPointerGetBlockNumber(&(itup)->t_tid) & (~GIN_ITUP_COMPRESSED))
#define GinSetPostingOffset(itup,n) ItemPointerSetBlockNumber(&(itup)->t_tid,(n)|GIN_ITUP_COMPRESSED)
#define GinGetPosting(itup)			((Pointer) ((char*)(itup) + GinGetPostingOffset(itup)))
#define GinItupIsCompressed(itup)	((GinItemPointerGetBlockNumber(&(itup)->t_tid) & GIN_ITUP_COMPRESSED) != 0)

/*
 * Maximum size of an item on entry tree page. Make sure that we fit at least
 * three items on each page. (On regular B-tree indexes, we must fit at least
 * three items: two data items and the "high key". In GIN entry tree, we don't
 * currently store the high key explicitly, we just use the rightmost item on
 * the page, so it would actually be enough to fit two items.)
 */

/*
 * 条目树（entry tree）页面上单个项的最大大小。确保每个页面至少能容纳三个项。
 *（在常规 B-tree 索引上，我们必须至少容纳三个项：两个数据项和一个“高键”。
 * 在 GIN 条目树中，我们目前并不显式存储高键，而只是使用页面上最右侧的项，
 * 因此实际上能容纳两个项就足够了。）
 */
#define GinMaxItemSize \
	Min(INDEX_SIZE_MASK, \
		MAXALIGN_DOWN(((BLCKSZ - \
						MAXALIGN(SizeOfPageHeaderData + 3 * sizeof(ItemIdData)) - \
						MAXALIGN(sizeof(GinPageOpaqueData))) / 3)))

/*
 * Access macros for non-leaf entry tuples
 */

/*
 * 用于访问非叶子条目元组的宏。
 */
#define GinGetDownlink(itup)	GinItemPointerGetBlockNumber(&(itup)->t_tid)
#define GinSetDownlink(itup,blkno)	ItemPointerSet(&(itup)->t_tid, blkno, InvalidOffsetNumber)


/*
 * Data (posting tree) pages
 *
 * Posting tree pages don't store regular tuples. Non-leaf pages contain
 * PostingItems, which are pairs of ItemPointers and child block numbers.
 * Leaf pages contain GinPostingLists and an uncompressed array of item
 * pointers.
 *
 * In a leaf page, the compressed posting lists are stored after the regular
 * page header, one after each other. Although we don't store regular tuples,
 * pd_lower is used to indicate the end of the posting lists. After that, free
 * space follows.  This layout is compatible with the "standard" heap and
 * index page layout described in bufpage.h, so that we can e.g set buffer_std
 * when writing WAL records.
 *
 * In the special space is the GinPageOpaque struct.
 */

/*
 * 数据（倒排树）页面。
 *
 * 倒排树页面不存储常规元组。非叶子页包含 PostingItem，
 * 它是 ItemPointer 与子块号组成的对。
 * 叶子页包含 GinPostingList 以及一个未压缩的项指针数组。
 *
 * 在叶子页中，压缩后的倒排列表依次存储在常规页头之后，一个接一个。
 * 尽管我们并不存储常规元组，但仍使用 pd_lower 来指示倒排列表的结尾。
 * 其后是空闲空间。此布局与 bufpage.h 中描述的“标准”堆页和索引页布局兼容，
 * 因此我们在写入 WAL 记录时可以（例如）设置 buffer_std。
 *
 * 特殊空间中存放的是 GinPageOpaque 结构体。
 */
#define GinDataLeafPageGetPostingList(page) \
	(GinPostingList *) ((PageGetContents(page) + MAXALIGN(sizeof(ItemPointerData))))
#define GinDataLeafPageGetPostingListSize(page) \
	(((PageHeader) page)->pd_lower - MAXALIGN(SizeOfPageHeaderData) - MAXALIGN(sizeof(ItemPointerData)))

#define GinDataLeafPageIsEmpty(page) \
	(GinPageIsCompressed(page) ? (GinDataLeafPageGetPostingListSize(page) == 0) : (GinPageGetOpaque(page)->maxoff < FirstOffsetNumber))

#define GinDataLeafPageGetFreeSpace(page) PageGetExactFreeSpace(page)

#define GinDataPageGetRightBound(page)	((ItemPointer) PageGetContents(page))
/*
 * Pointer to the data portion of a posting tree page. For internal pages,
 * that's the beginning of the array of PostingItems. For compressed leaf
 * pages, the first compressed posting list. For uncompressed (pre-9.4) leaf
 * pages, it's the beginning of the ItemPointer array.
 */

/*
 * 指向倒排树页面数据部分的指针。对于内部页，这是 PostingItem 数组的起始位置；
 * 对于压缩叶子页，这是第一个压缩倒排列表；
 * 对于未压缩（9.4 之前）的叶子页，这是 ItemPointer 数组的起始位置。
 */
#define GinDataPageGetData(page)	\
	(PageGetContents(page) + MAXALIGN(sizeof(ItemPointerData)))
/* non-leaf pages contain PostingItems */

/* 非叶子页包含 PostingItem */
#define GinDataPageGetPostingItem(page, i)	\
	((PostingItem *) (GinDataPageGetData(page) + ((i)-1) * sizeof(PostingItem)))

/*
 * Note: there is no GinDataPageGetDataSize macro, because before version
 * 9.4, we didn't set pd_lower on data pages. There can be pages in the index
 * that were binary-upgraded from earlier versions and still have an invalid
 * pd_lower, so we cannot trust it in general. Compressed posting tree leaf
 * pages are new in 9.4, however, so we can trust them; see
 * GinDataLeafPageGetPostingListSize.
 */

/*
 * 注意：没有 GinDataPageGetDataSize 宏，因为在 9.4 版本之前，
 * 我们并不在数据页上设置 pd_lower。索引中可能存在从更早版本二进制升级而来、
 * pd_lower 仍然无效的页面，因此通常不能信任它。
 * 不过，压缩倒排树叶子页是 9.4 中新增的，所以我们可以信任它们；
 * 参见 GinDataLeafPageGetPostingListSize。
 */
#define GinDataPageSetDataSize(page, size) \
	{ \
		Assert(size <= GinDataPageMaxDataSize); \
		((PageHeader) page)->pd_lower = (size) + MAXALIGN(SizeOfPageHeaderData) + MAXALIGN(sizeof(ItemPointerData)); \
	}

#define GinNonLeafDataPageGetFreeSpace(page)	\
	(GinDataPageMaxDataSize - \
	 GinPageGetOpaque(page)->maxoff * sizeof(PostingItem))

#define GinDataPageMaxDataSize	\
	(BLCKSZ - MAXALIGN(SizeOfPageHeaderData) \
	 - MAXALIGN(sizeof(ItemPointerData)) \
	 - MAXALIGN(sizeof(GinPageOpaqueData)))

/*
 * List pages
 */

/*
 * 列表（list）页面。
 */
#define GinListPageSize  \
	( BLCKSZ - SizeOfPageHeaderData - MAXALIGN(sizeof(GinPageOpaqueData)) )

/*
 * A compressed posting list.
 *
 * Note: This requires 2-byte alignment.
 */

/*
 * 一个压缩后的倒排列表。
 *
 * 注意：它要求 2 字节对齐。
 */
typedef struct
{
	ItemPointerData first;		/* first item in this posting list (unpacked) */

	/* 此倒排列表中的第一个项（未压缩形式） */
	uint16		nbytes;			/* number of bytes that follow */

	/* 其后跟随的字节数 */
	unsigned char bytes[FLEXIBLE_ARRAY_MEMBER]; /* varbyte encoded items */

	/* 采用变长字节（varbyte）编码的项 */
} GinPostingList;

#define SizeOfGinPostingList(plist) (offsetof(GinPostingList, bytes) + SHORTALIGN((plist)->nbytes) )
#define GinNextPostingListSegment(cur) ((GinPostingList *) (((char *) (cur)) + SizeOfGinPostingList((cur))))

#endif							/* GINBLOCK_H */

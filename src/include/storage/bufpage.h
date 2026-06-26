/*-------------------------------------------------------------------------
 *
 * bufpage.h
 *	  Standard POSTGRES buffer page definitions.
 *	  标准的 POSTGRES 缓冲区页面定义。
 *
 * Core Flow Explanation / 核心流程说明:
 * - PostgreSQL uses a "slotted page" layout for its disk blocks.
 * - PostgreSQL 为其磁盘块使用“槽式页面（slotted page）”布局。
 * - Page Format:
 *   1. Header: PageHeaderData at the very beginning.
 *      头部：最开头的 PageHeaderData。
 *   2. Line Pointers: An array of ItemIdData follows the header, growing downwards.
 *      行指针：紧随头部之后的 ItemIdData 数组，向下增长。
 *   3. Free Space: Unallocated space between pointers and tuples.
 *      剩余空间：指针和元组之间未分配的空间。
 *   4. Tuples: Actual data records, growing upwards from the end.
 *      元组：实际的数据记录，从末尾向上增长。
 *   5. Special Space: Optional AM-specific data at the very end.
 *      特殊空间：末尾可选的访问方法（AM）特定数据。
 * - Key Operations:
 *   - PageInit(): Initialize a new page.
 *   - PageAddItem(): Add a new tuple to the page and allocate a line pointer.
 *   - PageGetItem(): Access a tuple using its line pointer.
 *   - PageRepairFragmentation(): Reclaim space by compacting the page.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/bufpage.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BUFPAGE_H
#define BUFPAGE_H

#include "access/xlogdefs.h"
#include "storage/block.h"
#include "storage/item.h"
#include "storage/off.h"

/* GUC variable */
/* 全局配置（GUC）变量 */
extern PGDLLIMPORT bool ignore_checksum_failure;

/*
 * A postgres disk page is an abstraction layered on top of a postgres
 * disk block (which is simply a unit of i/o, see block.h).
 * 一个 postgres 磁盘页面是层叠在 postgres 磁盘块（仅仅是一个 I/O 单元，参见 block.h）之上的抽象。
 *
 * specifically, while a disk block can be unformatted, a postgres
 * disk page is always a slotted page of the form:
 * 具体来说，虽然磁盘块可以是未格式化的，但 postgres 磁盘页面始终是如下形式的槽式页面：
 *
 * +----------------+---------------------------------+
 * | PageHeaderData | linp1 linp2 linp3 ...           |
 * +-----------+----+---------------------------------+
 * | ... linpN |									  |
 * +-----------+--------------------------------------+
 * |		   ^ pd_lower							  |
 * |												  |
 * |			 v pd_upper							  |
 * +-------------+------------------------------------+
 * |			 | tupleN ...                         |
 * +-------------+------------------+-----------------+
 * |	   ... tuple3 tuple2 tuple1 | "special space" |
 * +--------------------------------+-----------------+
 *									^ pd_special
 *
 * a page is full when nothing can be added between pd_lower and
 * pd_upper.
 * 当 pd_lower 和 pd_upper 之间无法再添加任何内容时，页面即满。
 *
 * all blocks written out by an access method must be disk pages.
 * 访问方法写出的所有块必须是磁盘页面。
 *
 * EXCEPTIONS:
 * 例外情况：
 *
 * obviously, a page is not formatted before it is initialized by
 * a call to PageInit.
 * 显然，在通过调用 PageInit 进行初始化之前，页面是未格式化的。
 *
 * NOTES:
 * 备注：
 *
 * linp1..N form an ItemId (line pointer) array.  ItemPointers point
 * to a physical block number and a logical offset (line pointer
 * number) within that block/page.  Note that OffsetNumbers
 * conventionally start at 1, not 0.
 * linp1..N 构成了 ItemId（行指针）数组。ItemPointers 指向磁盘块中的物理块号和逻辑偏移量（行指针编号）。
 * 注意，OffsetNumbers 按惯例从 1 开始，而不是 0。
 *
 * tuple1..N are added "backwards" on the page.  Since an ItemPointer
 * offset is used to access an ItemId entry rather than an actual
 * byte-offset position, tuples can be physically shuffled on a page
 * whenever the need arises.  This indirection also keeps crash recovery
 * relatively simple, because the low-level details of page space
 * management can be controlled by standard buffer page code during
 * logging, and during recovery.
 * tuple1..N 是以“逆向”方式添加到页面上的。由于 ItemPointer 的偏移量是用来访问 ItemId 条目的，
 * 而不是实际的字节偏移位置，因此只要有需要，元组可以在页面内物理上重新排列。
 * 这种间接访问方式也使得崩溃恢复相对简单，因为页面空间管理的底层细节可以在日志记录和恢复期间由标准的缓冲区页面代码控制。
 *
 * AM-generic per-page information is kept in PageHeaderData.
 * 通用的访问方法（AM）每页信息保存在 PageHeaderData 中。
 *
 * AM-specific per-page data (if any) is kept in the area marked "special
 * space"; each AM has an "opaque" structure defined somewhere that is
 * stored as the page trailer.  An access method should always
 * initialize its pages with PageInit and then set its own opaque
 * fields.
 * 特定 AM 的每页数据（如果有）保存在标记为“特殊空间（special space）”的区域内；
 * 每个 AM 在某处都定义了一个“隐藏（opaque）”结构，该结构存储为页面的尾部（trailer）。
 * 访问方法应始终使用 PageInit 初始化其页面，然后设置其自己的隐藏字段。
 */

typedef char PageData;
typedef PageData *Page;


/*
 * location (byte offset) within a page.
 * 页面内的位置（字节偏移量）。
 *
 * note that this is actually limited to 2^15 because we have limited
 * ItemIdData.lp_off and ItemIdData.lp_len to 15 bits (see itemid.h).
 * 注意，这实际上被限制在 2^15 以内，因为我们将 ItemIdData.lp_off 和 ItemIdData.lp_len 限制为 15 位（参见 itemid.h）。
 */
typedef uint16 LocationIndex;


/*
 * For historical reasons, the 64-bit LSN value is stored as two 32-bit
 * values.
 * 由于历史原因，64 位的 LSN（日志序列号）值被存储为两个 32 位的值。
 */
typedef struct
{
	uint32		xlogid;			/* high bits */
								/* 高位 */
	uint32		xrecoff;		/* low bits */
								/* 低位 */
} PageXLogRecPtr;

static inline XLogRecPtr
PageXLogRecPtrGet(PageXLogRecPtr val)
{
	return (uint64) val.xlogid << 32 | val.xrecoff;
}

#define PageXLogRecPtrSet(ptr, lsn) \
	((ptr).xlogid = (uint32) ((lsn) >> 32), (ptr).xrecoff = (uint32) (lsn))

/*
 * disk page organization
 * 磁盘页面组织结构
 *
 * space management information generic to any page
 * 适用于任何页面的通用空间管理信息
 *
 *		pd_lsn		- identifies xlog record for last change to this page.
 *		              标识对此页进行最后更改的 xlog 记录。
 *		pd_checksum - page checksum, if set.
 *		              页面校验和（如果已设置）。
 *		pd_flags	- flag bits.
 *		              标志位。
 *		pd_lower	- offset to start of free space.
 *		              空闲空间起始位置的偏移量。
 *		pd_upper	- offset to end of free space.
 *		              空闲空间结束位置的偏移量。
 *		pd_special	- offset to start of special space.
 *		              特殊空间起始位置的偏移量。
 *		pd_pagesize_version - size in bytes and page layout version number.
 *		              以字节为单位的大小和页面布局版本号。
 *		pd_prune_xid - oldest XID among potentially prunable tuples on page.
 *		              页面上潜在可清理元组中最旧的事务 ID (XID)。
 *
 * The LSN is used by the buffer manager to enforce the basic rule of WAL:
 * "thou shalt write xlog before data".  A dirty buffer cannot be dumped
 * to disk until xlog has been flushed at least as far as the page's LSN.
 * LSN 由缓冲区管理器使用，用于强制执行 WAL（预写日志）的基本规则：“你应该在写数据之前写 xlog”。
 * 在相关的 xlog 刷新到至少与页面的 LSN 一样远之前，脏缓冲区不能被转储到磁盘。
 *
 * pd_checksum stores the page checksum, if it has been set for this page;
 * zero is a valid value for a checksum. If a checksum is not in use then
 * we leave the field unset. This will typically mean the field is zero
 * though non-zero values may also be present if databases have been
 * pg_upgraded from releases prior to 9.3, when the same byte offset was
 * used to store the current timelineid when the page was last updated.
 * pd_checksum 存储页面的校验和（如果已为此页面设置）；零是有效的校验和。
 * 如果未使用校验和，则保持该字段未设置。这通常意味着该字段为零，
 * 尽管如果数据库是从 9.3 之前的版本 pg_upgrade 过来的，可能也存在非零值，
 * 因为当时相同的字节偏移量曾用于存储页面最后更新时的当前时间线 ID（timelineid）。
 *
 * Note that there is no indication on a page as to whether the checksum
 * is valid or not, a deliberate design choice which avoids the problem
 * of relying on the page contents to decide whether to verify it. Hence
 * there are no flag bits relating to checksums.
 * 注意，页面上没有指示校验和是否有效的标记，这是一个刻意的设计选择，
 * 以避免依赖页面内容来决定是否验证它。因此，没有与校验和相关的标志位。
 *
 * pd_prune_xid is a hint field that helps determine whether pruning will be
 * useful.  It is currently unused in index pages.
 * pd_prune_xid 是一个提示字段，有助于确定清理（pruning）是否有效。目前在索引页面中未使用。
 *
 * The page version number and page size are packed together into a single
 * uint16 field.  This is for historical reasons: before PostgreSQL 7.3,
 * there was no concept of a page version number, and doing it this way
 * lets us pretend that pre-7.3 databases have page version number zero.
 * We constrain page sizes to be multiples of 256, leaving the low eight
 * bits available for a version number.
 * 页面版本号和页面大小被打包在一起放入一个 uint16 字段中。
 * 这是出于历史原因：在 PostgreSQL 7.3 之前没有页面版本号的概念，
 * 这样做可以让我们假装 7.3 之前的数据库页面版本号为零。
 * 我们限制页面大小为 256 的倍数，留出低 8 位用于存储版本号。
 *
 * Minimum possible page size is perhaps 64B to fit page header, opaque space
 * and a minimal tuple; of course, in reality you want it much bigger, so
 * the constraint on pagesize mod 256 is not an important restriction.
 * On the high end, we can only support pages up to 32KB because lp_off/lp_len
 * are 15 bits.
 * 最小可能的页面大小也许是 64 字节，以便容纳页面头部、隐藏空间（opaque space）和最小的元组；
 * 当然，在现实中你希望它大得多，因此页面大小必须是 256 倍数的约束并不是一个重要的限制。
 * 在高端，由于 lp_off/lp_len 只有 15 位，我们最多只能支持 32KB 的页面。
 */

typedef struct PageHeaderData
{
	/* XXX LSN is member of *any* block, not only page-organized ones */
	/* XXX LSN 是 *任何* 数据块的成员，而不仅仅是按页面组织的块 */
	PageXLogRecPtr pd_lsn;		/* LSN: next byte after last byte of xlog
								 * record for last change to this page */
								/* LSN：对此页进行最后更改的 xlog 记录最后一个字节后的下一个字节 */
	uint16		pd_checksum;	/* checksum */
								/* 校验和 */
	uint16		pd_flags;		/* flag bits, see below */
								/* 标志位，见下文 */
	LocationIndex pd_lower;		/* offset to start of free space */
								/* 空闲空间起始位置的偏移量 */
	LocationIndex pd_upper;		/* offset to end of free space */
								/* 空闲空间结束位置的偏移量 */
	LocationIndex pd_special;	/* offset to start of special space */
								/* 特殊空间起始位置的偏移量 */
	uint16		pd_pagesize_version;
	TransactionId pd_prune_xid; /* oldest prunable XID, or zero if none */
	ItemIdData	pd_linp[FLEXIBLE_ARRAY_MEMBER]; /* line pointer array */
} PageHeaderData;

typedef PageHeaderData *PageHeader;

/*
 * pd_flags contains the following flag bits.  Undefined bits are initialized
 * to zero and may be used in the future.
 *
 * PD_HAS_FREE_LINES is set if there are any LP_UNUSED line pointers before
 * pd_lower.  This should be considered a hint rather than the truth, since
 * changes to it are not WAL-logged.
 *
 * PD_PAGE_FULL is set if an UPDATE doesn't find enough free space in the
 * page for its new tuple version; this suggests that a prune is needed.
 * Again, this is just a hint.
 */
#define PD_HAS_FREE_LINES	0x0001	/* are there any unused line pointers? */
									/* 页面中是否存在未使用的行指针？ */
#define PD_PAGE_FULL		0x0002	/* not enough free space for new tuple? */
									/* 页面内是否有足够的剩余空间存放新元组？ */
#define PD_ALL_VISIBLE		0x0004	/* all tuples on page are visible to
									 * everyone */
									/* 页面上的所有元组对所有人都是可见的 */

#define PD_VALID_FLAG_BITS	0x0007	/* OR of all valid pd_flags bits */
									/* 所有有效标志位的按位或结果 */

/*
 * Page layout version number 0 is for pre-7.3 Postgres releases.
 * Releases 7.3 and 7.4 use 1, denoting a new HeapTupleHeader layout.
 * Release 8.0 uses 2; it changed the HeapTupleHeader layout again.
 * Release 8.1 uses 3; it redefined HeapTupleHeader infomask bits.
 * Release 8.3 uses 4; it changed the HeapTupleHeader layout again, and
 *		added the pd_flags field (by stealing some bits from pd_tli),
 *		as well as adding the pd_prune_xid field (which enlarges the header).
 * 页面布局版本号 0 用于 7.3 之前的 Postgres 版本。
 * 7.3 和 7.4 版本使用 1，表示采用了新的 HeapTupleHeader 布局。
 * 8.0 版本使用 2；它再次更改了 HeapTupleHeader 布局。
 * 8.1 版本使用 3；它重新定义了 HeapTupleHeader 的 infomask 位。
 * 8.3 版本使用 4；它再次更改了布局，并添加了 pd_flags 字段（通过从 pd_tli 挪用一些位实现），
 * 同时添加了 pd_prune_xid 字段（加大了页面头部）。
 *
 * As of Release 9.3, the checksum version must also be considered when
 * handling pages.
 * 自 9.3 版本起，处理页面时还必须考虑页面校验和版本。
 */
#define PG_PAGE_LAYOUT_VERSION		4
#define PG_DATA_CHECKSUM_VERSION	1

/* ----------------------------------------------------------------
 *						page support functions
 * ----------------------------------------------------------------
 */

/*
 * line pointer(s) do not count as part of header
 * 行指针不计入页面头部的部分。
 */
#define SizeOfPageHeaderData (offsetof(PageHeaderData, pd_linp))

/*
 * PageIsEmpty
 *		returns true iff no itemid has been allocated on the page
 * PageIsEmpty
 *      如果页面上没有分配任何项标识符（itemid），则返回 true。
 */
static inline bool
PageIsEmpty(const PageData *page)
{
	return ((const PageHeaderData *) page)->pd_lower <= SizeOfPageHeaderData;
}

/*
 * PageIsNew
 *		returns true iff page has not been initialized (by PageInit)
 * PageIsNew
 *      如果页面尚未初始化（通过 PageInit 调用），则返回 true。
 */
static inline bool
PageIsNew(const PageData *page)
{
	return ((const PageHeaderData *) page)->pd_upper == 0;
}

/*
 * PageGetItemId
 *		Returns an item identifier of a page.
 * PageGetItemId
 *      返回一个页面的项（item）标识符。
 */
static inline ItemId
PageGetItemId(Page page, OffsetNumber offsetNumber)
{
	return &((PageHeader) page)->pd_linp[offsetNumber - 1];
}

/*
 * PageGetContents
 *		To be used in cases where the page does not contain line pointers.
 * PageGetContents
 *      用于页面中不包含行指针的情况。
 *
 * Note: prior to 8.3 this was not guaranteed to yield a MAXALIGN'd result.
 * Now it is.  Beware of old code that might think the offset to the contents
 * is just SizeOfPageHeaderData rather than MAXALIGN(SizeOfPageHeaderData).
 * 注意：在 8.3 版本之前，不能保证产生内存对齐（MAXALIGN）后的结果。
 * 现在可以了。请注意可能认为偏移量只是 SizeOfPageHeaderData 而不是经过对齐后的 MAXALIGN(SizeOfPageHeaderData) 的旧代码。
 */
static inline char *
PageGetContents(Page page)
{
	return (char *) page + MAXALIGN(SizeOfPageHeaderData);
}

/* ----------------
 *		functions to access page size info
 * ----------------
 */

/*
 * PageGetPageSize
 *		Returns the page size of a page.
 * PageGetPageSize
 *      返回页面的大小。
 *
 * this can only be called on a formatted page (unlike
 * BufferGetPageSize, which can be called on an unformatted page).
 * however, it can be called on a page that is not stored in a buffer.
 * 只能在已格式化的页面上调用（不像 BufferGetPageSize 可以从非格式化页面调用）。
 * 不过，它可以在未存储在缓冲区中的页面上调用。
 */
static inline Size
PageGetPageSize(const PageData *page)
{
	return (Size) (((const PageHeaderData *) page)->pd_pagesize_version & (uint16) 0xFF00);
}

/*
 * PageGetPageLayoutVersion
 *		Returns the page layout version of a page.
 * PageGetPageLayoutVersion
 *      返回页面布局版本号。
 */
static inline uint8
PageGetPageLayoutVersion(const PageData *page)
{
	return (((const PageHeaderData *) page)->pd_pagesize_version & 0x00FF);
}

/*
 * PageSetPageSizeAndVersion
 *		Sets the page size and page layout version number of a page.
 *
 * We could support setting these two values separately, but there's
 * no real need for it at the moment.
 */
static inline void
PageSetPageSizeAndVersion(Page page, Size size, uint8 version)
{
	Assert((size & 0xFF00) == size);
	Assert((version & 0x00FF) == version);

	((PageHeader) page)->pd_pagesize_version = size | version;
}

/* ----------------
 *		page special data functions
 *		页面特殊数据相关函数
 * ----------------
 */
/*
 * PageGetSpecialSize
 *		Returns size of special space on a page.
 * PageGetSpecialSize
 *      返回页面上特殊空间的大小。
 */
static inline uint16
PageGetSpecialSize(const PageData *page)
{
	return (PageGetPageSize(page) - ((const PageHeaderData *) page)->pd_special);
}

/*
 * Using assertions, validate that the page special pointer is OK.
 * 使用断言来验证页面特殊数据指针是否正确。
 *
 * This is intended to catch use of the pointer before page initialization.
 * 旨在捕获页面初始化前对该指针的使用。
 */
static inline void
PageValidateSpecialPointer(const PageData *page)
{
	Assert(page);
	Assert(((const PageHeaderData *) page)->pd_special <= BLCKSZ);
	Assert(((const PageHeaderData *) page)->pd_special >= SizeOfPageHeaderData);
}

/*
 * PageGetSpecialPointer
 *		Returns pointer to special space on a page.
 * PageGetSpecialPointer
 *      返回指向页面中特殊空间的指针。
 */
#define PageGetSpecialPointer(page) \
( \
	PageValidateSpecialPointer(page), \
	((page) + ((PageHeader) (page))->pd_special) \
)

/*
 * PageGetItem
 *		Retrieves an item on the given page.
 * PageGetItem
 *      获取指定页面上的一个项（item）。
 *
 * Note:
 *		This does not change the status of any of the resources passed.
 *		The semantics may change in the future.
 * 注意：这将不会改变任何传递资源的当前状态。语义在将来可能会发生变化。
 */
static inline Item
PageGetItem(const PageData *page, const ItemIdData *itemId)
{
	Assert(page);
	Assert(ItemIdHasStorage(itemId));

	return (Item) (((const char *) page) + ItemIdGetOffset(itemId));
}

/*
 * PageGetMaxOffsetNumber
 *		Returns the maximum offset number used by the given page.
 *		Since offset numbers are 1-based, this is also the number
 *		of items on the page.
 * PageGetMaxOffsetNumber
 *      返回给定页面使用的最大偏移编号。
 *      由于偏移编号是从 1 开始的，因此这也就是页面上元组的数量。
 *
 *		NOTE: if the page is not initialized (pd_lower == 0), we must
 *		return zero to ensure sane behavior.
 *      注意：如果页面尚未初始化（pd_lower == 0），我们必须返回零以保证合理的行为。
 */
static inline OffsetNumber
PageGetMaxOffsetNumber(const PageData *page)
{
	const PageHeaderData *pageheader = (const PageHeaderData *) page;

	if (pageheader->pd_lower <= SizeOfPageHeaderData)
		return 0;
	else
		return (pageheader->pd_lower - SizeOfPageHeaderData) / sizeof(ItemIdData);
}

/*
 * Additional functions for access to page headers.
 * 用于访问页面头部的其他函数。
 */
static inline XLogRecPtr
PageGetLSN(const PageData *page)
{
	return PageXLogRecPtrGet(((const PageHeaderData *) page)->pd_lsn);
}

/*
 * PageSetLSN -
 *    Sets the LSN for the last change to the page.
 */
static inline void
PageSetLSN(Page page, XLogRecPtr lsn)
{
	PageXLogRecPtrSet(((PageHeader) page)->pd_lsn, lsn);
}

static inline bool
PageHasFreeLinePointers(const PageData *page)
{
	return ((const PageHeaderData *) page)->pd_flags & PD_HAS_FREE_LINES;
}

/*
 * PageSetHasFreeLinePointers/PageClearHasFreeLinePointers -
 *    Manage the PD_HAS_FREE_LINES hint bit.
 */
static inline void
PageSetHasFreeLinePointers(Page page)
{
	((PageHeader) page)->pd_flags |= PD_HAS_FREE_LINES;
}
static inline void
PageClearHasFreeLinePointers(Page page)
{
	((PageHeader) page)->pd_flags &= ~PD_HAS_FREE_LINES;
}

static inline bool
PageIsFull(const PageData *page)
{
	return ((const PageHeaderData *) page)->pd_flags & PD_PAGE_FULL;
}

/*
 * PageSetFull/PageClearFull -
 *    Manage the PD_PAGE_FULL hint bit.
 */
static inline void
PageSetFull(Page page)
{
	((PageHeader) page)->pd_flags |= PD_PAGE_FULL;
}
static inline void
PageClearFull(Page page)
{
	((PageHeader) page)->pd_flags &= ~PD_PAGE_FULL;
}

static inline bool
PageIsAllVisible(const PageData *page)
{
	return ((const PageHeaderData *) page)->pd_flags & PD_ALL_VISIBLE;
}

/*
 * PageSetAllVisible/PageClearAllVisible -
 *    Manage the PD_ALL_VISIBLE status bit.
 */
static inline void
PageSetAllVisible(Page page)
{
	((PageHeader) page)->pd_flags |= PD_ALL_VISIBLE;
}
static inline void
PageClearAllVisible(Page page)
{
	((PageHeader) page)->pd_flags &= ~PD_ALL_VISIBLE;
}

/*
 * These two require "access/transam.h", so left as macros.
 * 这两个需要 "access/transam.h"，所以保留为宏。
 */
#define PageSetPrunable(page, xid) \
do { \
	Assert(TransactionIdIsNormal(xid)); \
	if (!TransactionIdIsValid(((PageHeader) (page))->pd_prune_xid) || \
		TransactionIdPrecedes(xid, ((PageHeader) (page))->pd_prune_xid)) \
		((PageHeader) (page))->pd_prune_xid = (xid); \
} while (0)
#define PageClearPrunable(page) \
	(((PageHeader) (page))->pd_prune_xid = InvalidTransactionId)


/* ----------------------------------------------------------------
 *		extern declarations
 * ----------------------------------------------------------------
 */

/* flags for PageAddItemExtended() */
#define PAI_OVERWRITE			(1 << 0)
#define PAI_IS_HEAP				(1 << 1)

/* flags for PageIsVerified() */
#define PIV_LOG_WARNING			(1 << 0)
#define PIV_LOG_LOG				(1 << 1)
#define PIV_IGNORE_CHECKSUM_FAILURE (1 << 2)

/*
 * PageAddItemExtended/PageAddItem -
 *    Add an item to the page.
 * PageAddItemExtended/PageAddItem -
 *    向页面添加一个项。
 */
#define PageAddItem(page, item, size, offsetNumber, overwrite, is_heap) \
	PageAddItemExtended(page, item, size, offsetNumber, \
						((overwrite) ? PAI_OVERWRITE : 0) | \
						((is_heap) ? PAI_IS_HEAP : 0))

/*
 * Check that BLCKSZ is a multiple of sizeof(size_t).  In PageIsVerified(), it
 * is much faster to check if a page is full of zeroes using the native word
 * size.  Note that this assertion is kept within a header to make sure that
 * StaticAssertDecl() works across various combinations of platforms and
 * compilers.
 * 检查 BLCKSZ 是否是 sizeof(size_t) 的倍数。在 PageIsVerified() 中，
 * 使用本地机器字长检查页面是否全由零组成会快得多。
 * 注意，此断言保留在头文件中以确保 StaticAssertDecl() 跨平台和编译器生效。
 */
StaticAssertDecl(BLCKSZ == ((BLCKSZ / sizeof(size_t)) * sizeof(size_t)),
				 "BLCKSZ has to be a multiple of sizeof(size_t)");

/*
 * PageInit -
 *    General initialization for each buffer page.
 * PageInit -
 *    对每个缓冲区页面的通用初始化。
 */
extern void PageInit(Page page, Size pageSize, Size specialSize);

/*
 * PageIsVerified -
 *    Verifies that the page checksum (if any) and headers are correct.
 * PageIsVerified -
 *    验证页面校验和（如果有）和头部是否正确。
 */
extern bool PageIsVerified(PageData *page, BlockNumber blkno, int flags,
						   bool *checksum_failure_p);

/*
 * PageAddItemExtended -
 *    Low-level version of PageAddItem with extra flags.
 * PageAddItemExtended -
 *    PageAddItem 的更底层版本，带有额外的标志。
 */
extern OffsetNumber PageAddItemExtended(Page page, Item item, Size size,
										OffsetNumber offsetNumber, int flags);

/*
 * PageGetTempPage/Copy -
 *    Utilities for creating temporary copies of pages, usually for index construction.
 * PageGetTempPage/Copy -
 *    用于创建页面临时副本的工具，通常用于索引构建。
 */
extern Page PageGetTempPage(const PageData *page);
extern Page PageGetTempPageCopy(const PageData *page);
extern Page PageGetTempPageCopySpecial(const PageData *page);
extern void PageRestoreTempPage(Page tempPage, Page oldPage);

/*
 * PageRepairFragmentation -
 *    Compacts a page by removing deleted items and consolidating free space.
 * PageRepairFragmentation -
 *    通过移除已删除的项并整合空闲空间来平整页面的碎片化。
 */
extern void PageRepairFragmentation(Page page);

/*
 * PageTruncateLinePointerArray -
 *    Removes unused line pointers from the end of the array.
 * PageTruncateLinePointerArray -
 *    移除行指针数组末尾未使用的行指针。
 */
extern void PageTruncateLinePointerArray(Page page);

/*
 * PageGetFreeSpace* -
 *    Calculates available space on the page for different scenarios.
 * PageGetFreeSpace* -
 *    计算页面在不同场景下的可用空间。
 */
extern Size PageGetFreeSpace(const PageData *page);
extern Size PageGetFreeSpaceForMultipleTuples(const PageData *page, int ntups);
extern Size PageGetExactFreeSpace(const PageData *page);
extern Size PageGetHeapFreeSpace(const PageData *page);

/*
 * PageIndexTupleDelete* -
 *    Deletes tuples from an index page.
 * PageIndexTupleDelete* -
 *    从索引页面删除元组。
 */
extern void PageIndexTupleDelete(Page page, OffsetNumber offnum);
extern void PageIndexMultiDelete(Page page, OffsetNumber *itemnos, int nitems);
extern void PageIndexTupleDeleteNoCompact(Page page, OffsetNumber offnum);

/*
 * PageIndexTupleOverwrite -
 *    Replaces a tuple version on an index page.
 * PageIndexTupleOverwrite -
 *    在索引页面上替换一个元组版本。
 */
extern bool PageIndexTupleOverwrite(Page page, OffsetNumber offnum,
									Item newtup, Size newsize);

/*
 * PageSetChecksum* -
 *    Functions for calculating and setting the page checksum.
 * PageSetChecksum* -
 *    用于计算和设置页面校验和的函数。
 */
extern char *PageSetChecksumCopy(Page page, BlockNumber blkno);
extern void PageSetChecksumInplace(Page page, BlockNumber blkno);

#endif							/* BUFPAGE_H */

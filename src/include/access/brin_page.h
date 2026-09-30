/*
 * brin_page.h
 *		Prototypes and definitions for BRIN page layouts
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/include/access/brin_page.h
 *
 * NOTES
 *
 * These structs should really be private to specific BRIN files, but it's
 * useful to have them here so that they can be used by pageinspect and similar
 * tools.
 */
#ifndef BRIN_PAGE_H
#define BRIN_PAGE_H

#include "storage/block.h"
#include "storage/itemptr.h"

/*
 * Special area of BRIN pages.
 *
 * We define it in this odd way so that it always occupies the last
 * MAXALIGN-sized element of each page.
 */

/*
 * BRIN 页面的特殊区域（special area）。
 *
 * 我们以这种别扭的方式来定义它，是为了让它始终占据每个页面中最后一个
 * MAXALIGN 大小的元素。
 */
typedef struct BrinSpecialSpace
{
	uint16		vector[MAXALIGN(1) / sizeof(uint16)];
} BrinSpecialSpace;

/*
 * Make the page type be the last half-word in the page, for consumption by
 * pg_filedump and similar utilities.  We don't really care much about the
 * position of the "flags" half-word, but it's simpler to apply a consistent
 * rule to both.
 *
 * See comments above GinPageOpaqueData.
 */

/*
 * 让页面类型（page type）成为页面中最后一个半字（half-word），以便 pg_filedump
 * 及类似工具读取。我们其实并不太关心 “flags” 半字的位置，但对两者应用一致的
 * 规则会更简单。
 *
 * 参见 GinPageOpaqueData 上方的注释。
 */
#define BrinPageType(page)		\
	(((BrinSpecialSpace *)		\
	  PageGetSpecialPointer(page))->vector[MAXALIGN(1) / sizeof(uint16) - 1])

#define BrinPageFlags(page)		\
	(((BrinSpecialSpace *)		\
	  PageGetSpecialPointer(page))->vector[MAXALIGN(1) / sizeof(uint16) - 2])

/* special space on all BRIN pages stores a "type" identifier */

/* 所有 BRIN 页面的特殊空间中都存储一个 “类型（type）” 标识符 */
#define		BRIN_PAGETYPE_META			0xF091
#define		BRIN_PAGETYPE_REVMAP		0xF092
#define		BRIN_PAGETYPE_REGULAR		0xF093

#define BRIN_IS_META_PAGE(page) (BrinPageType(page) == BRIN_PAGETYPE_META)
#define BRIN_IS_REVMAP_PAGE(page) (BrinPageType(page) == BRIN_PAGETYPE_REVMAP)
#define BRIN_IS_REGULAR_PAGE(page) (BrinPageType(page) == BRIN_PAGETYPE_REGULAR)

/* flags for BrinSpecialSpace */

/* BrinSpecialSpace 使用的标志位 */
#define		BRIN_EVACUATE_PAGE			(1 << 0)


/* Metapage definitions */

/* 元页面（metapage）定义 */
typedef struct BrinMetaPageData
{
	uint32		brinMagic;
	uint32		brinVersion;
	BlockNumber pagesPerRange;
	BlockNumber lastRevmapPage;
} BrinMetaPageData;

#define BRIN_CURRENT_VERSION		1
#define BRIN_META_MAGIC			0xA8109CFA

#define BRIN_METAPAGE_BLKNO		0

/* Definitions for revmap pages */

/* 反向映射页面（revmap page）的定义 */
typedef struct RevmapContents
{
	/*
	 * This array will fill all available space on the page.  It should be
	 * declared [FLEXIBLE_ARRAY_MEMBER], but for some reason you can't do that
	 * in an otherwise-empty struct.
	 */

	/*
	 * 该数组会填满页面上所有可用的空间。它本应声明为 [FLEXIBLE_ARRAY_MEMBER]，
	 * 但由于某些原因，无法在一个原本为空的结构体中这样声明。
	 */
	ItemPointerData rm_tids[1];
} RevmapContents;

#define REVMAP_CONTENT_SIZE \
	(BLCKSZ - MAXALIGN(SizeOfPageHeaderData) - \
	 offsetof(RevmapContents, rm_tids) - \
	 MAXALIGN(sizeof(BrinSpecialSpace)))
/* max num of items in the array */

/* 数组中项（item）的最大数量 */
#define REVMAP_PAGE_MAXITEMS \
	(REVMAP_CONTENT_SIZE / sizeof(ItemPointerData))

#endif							/* BRIN_PAGE_H */

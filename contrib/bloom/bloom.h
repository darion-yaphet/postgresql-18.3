/*-------------------------------------------------------------------------
 *
 * bloom.h
 *	  Header for bloom index.
 *
 * Copyright (c) 2016-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/bloom/bloom.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _BLOOM_H_
#define _BLOOM_H_

#include "access/amapi.h"
#include "access/generic_xlog.h"
#include "access/itup.h"
#include "access/xlog.h"
#include "fmgr.h"
#include "nodes/pathnodes.h"

/* Support procedures numbers
 *
 * 支持程序编号
 */
#define BLOOM_HASH_PROC			1
#define BLOOM_OPTIONS_PROC		2
#define BLOOM_NPROC				2

/* Scan strategies
 *
 * 扫描策略
 */
#define BLOOM_EQUAL_STRATEGY	1
#define BLOOM_NSTRATEGIES		1

/* Opaque for bloom pages
 *
 * 绽放页面不透明
 */
typedef struct BloomPageOpaqueData
{
	OffsetNumber maxoff;		/* number of index tuples on page */
	uint16		flags;			/* see bit definitions below */
	uint16		unused;			/* placeholder to force maxaligning of size of
								 * BloomPageOpaqueData and to place
								 *
								 * BloomPageOpaqueData 并放置
								 * bloom_page_id exactly at the end of page */
	uint16		bloom_page_id;	/* for identification of BLOOM indexes */
} BloomPageOpaqueData;

typedef BloomPageOpaqueData *BloomPageOpaque;

/* Bloom page flags
 *
 * 绽放页面标志
 */
#define BLOOM_META		(1<<0)
#define BLOOM_DELETED	(2<<0)

/*
 * The page ID is for the convenience of pg_filedump and similar utilities,
 * which otherwise would have a hard time telling pages of different index
 * types apart.  It should be the last 2 bytes on the page.  This is more or
 * less "free" due to alignment considerations.
 *
 * 页面 ID 是为了方便 pg_filedump 和类似的实用程序，否则它们将很难区分不同索引类型的页面。  它应该是页面上的最后 2 个字节。  由于对齐考虑，这或多或少是“自由的”。
 *
 * See comments above GinPageOpaqueData.
 *
 * 请参阅 GinPageOpaqueData 上面的评论。
 */
#define BLOOM_PAGE_ID		0xFF83

/* Macros for accessing bloom page structures
 *
 * 用于访问Bloom页面结构的宏
 */
#define BloomPageGetOpaque(page) ((BloomPageOpaque) PageGetSpecialPointer(page))
#define BloomPageGetMaxOffset(page) (BloomPageGetOpaque(page)->maxoff)
#define BloomPageIsMeta(page) \
	((BloomPageGetOpaque(page)->flags & BLOOM_META) != 0)
#define BloomPageIsDeleted(page) \
	((BloomPageGetOpaque(page)->flags & BLOOM_DELETED) != 0)
#define BloomPageSetDeleted(page) \
	(BloomPageGetOpaque(page)->flags |= BLOOM_DELETED)
#define BloomPageSetNonDeleted(page) \
	(BloomPageGetOpaque(page)->flags &= ~BLOOM_DELETED)
#define BloomPageGetData(page)		((BloomTuple *)PageGetContents(page))
#define BloomPageGetTuple(state, page, offset) \
	((BloomTuple *)(PageGetContents(page) \
		+ (state)->sizeOfBloomTuple * ((offset) - 1)))
#define BloomPageGetNextTuple(state, tuple) \
	((BloomTuple *)((Pointer)(tuple) + (state)->sizeOfBloomTuple))

/* Preserved page numbers
 *
 * 保留页码
 */
#define BLOOM_METAPAGE_BLKNO	(0)
#define BLOOM_HEAD_BLKNO		(1) /* first data page */

/*
 * We store Bloom signatures as arrays of uint16 words.
 *
 * 我们将 Bloom 签名存储为 uint16 字数组。
 */
typedef uint16 BloomSignatureWord;

#define SIGNWORDBITS ((int) (BITS_PER_BYTE * sizeof(BloomSignatureWord)))

/*
 * Default and maximum Bloom signature length in bits.
 *
 * 默认和最大 Bloom 签名长度（以位为单位）。
 */
#define DEFAULT_BLOOM_LENGTH	(5 * SIGNWORDBITS)
#define MAX_BLOOM_LENGTH		(256 * SIGNWORDBITS)

/*
 * Default and maximum signature bits generated per index key.
 *
 * 每个索引密钥生成的默认和最大签名位。
 */
#define DEFAULT_BLOOM_BITS		2
#define MAX_BLOOM_BITS			(MAX_BLOOM_LENGTH - 1)

/* Bloom index options
 *
 * 布卢姆索引选项
 */
typedef struct BloomOptions
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int			bloomLength;	/* length of signature in words (not bits!) */
	int			bitSize[INDEX_MAX_KEYS];	/* # of bits generated for each
											 * index key */
} BloomOptions;

/*
 * FreeBlockNumberArray - array of block numbers sized so that metadata fill
 * all space in metapage.
 *
 * FreeBlockNumberArray - 块号数组的大小使元数据填充元页中的所有空间。
 */
typedef BlockNumber FreeBlockNumberArray[MAXALIGN_DOWN(BLCKSZ - SizeOfPageHeaderData - MAXALIGN(sizeof(BloomPageOpaqueData))
													   - MAXALIGN(sizeof(uint16) * 2 + sizeof(uint32) + sizeof(BloomOptions)))
										 / sizeof(BlockNumber)];

/* Metadata of bloom index
 *
 * 布鲁姆指数元数据
 */
typedef struct BloomMetaPageData
{
	uint32		magickNumber;
	uint16		nStart;
	uint16		nEnd;
	BloomOptions opts;
	FreeBlockNumberArray notFullPage;
} BloomMetaPageData;

/* Magic number to distinguish bloom pages from others
 *
 * 用于区分Bloom页面和其他页面的神奇数字
 */
#define BLOOM_MAGICK_NUMBER (0xDBAC0DED)

/* Number of blocks numbers fit in BloomMetaPageData
 *
 * BloomMetaPageData 中适合的块数
 */
#define BloomMetaBlockN		(sizeof(FreeBlockNumberArray) / sizeof(BlockNumber))

#define BloomPageGetMeta(page)	((BloomMetaPageData *) PageGetContents(page))

typedef struct BloomState
{
	FmgrInfo	hashFn[INDEX_MAX_KEYS];
	Oid			collations[INDEX_MAX_KEYS];
	BloomOptions opts;			/* copy of options on index's metapage */
	int32		nColumns;

	/*
	 * sizeOfBloomTuple is index-specific, and it depends on reloptions, so
	 * precompute it
	 *
	 * sizeOfBloomTuple 是特定于索引的，并且它取决于reloptions，因此需要预先计算它
	 */
	Size		sizeOfBloomTuple;
} BloomState;

#define BloomPageGetFreeSpace(state, page) \
	(BLCKSZ - MAXALIGN(SizeOfPageHeaderData) \
		- BloomPageGetMaxOffset(page) * (state)->sizeOfBloomTuple \
		- MAXALIGN(sizeof(BloomPageOpaqueData)))

/*
 * Tuples are very different from all other relations
 *
 * 元组与所有其他关系有很大不同
 */
typedef struct BloomTuple
{
	ItemPointerData heapPtr;
	BloomSignatureWord sign[FLEXIBLE_ARRAY_MEMBER];
} BloomTuple;

#define BLOOMTUPLEHDRSZ offsetof(BloomTuple, sign)

/* Opaque data structure for bloom index scan
 *
 * 布隆索引扫描的不透明数据结构
 */
typedef struct BloomScanOpaqueData
{
	BloomSignatureWord *sign;	/* Scan signature */
	BloomState	state;
} BloomScanOpaqueData;

typedef BloomScanOpaqueData *BloomScanOpaque;

/* blutils.c */
extern void initBloomState(BloomState *state, Relation index);
extern void BloomFillMetapage(Relation index, Page metaPage);
extern void BloomInitMetapage(Relation index, ForkNumber forknum);
extern void BloomInitPage(Page page, uint16 flags);
extern Buffer BloomNewBuffer(Relation index);
extern void signValue(BloomState *state, BloomSignatureWord *sign, Datum value, int attno);
extern BloomTuple *BloomFormTuple(BloomState *state, ItemPointer iptr, Datum *values, bool *isnull);
extern bool BloomPageAddItem(BloomState *state, Page page, BloomTuple *tuple);

/* blvalidate.c */
extern bool blvalidate(Oid opclassoid);

/* index access method interface functions
 *
 * 索引访问方法接口函数
 */
extern bool blinsert(Relation index, Datum *values, bool *isnull,
					 ItemPointer ht_ctid, Relation heapRel,
					 IndexUniqueCheck checkUnique,
					 bool indexUnchanged,
					 struct IndexInfo *indexInfo);
extern IndexScanDesc blbeginscan(Relation r, int nkeys, int norderbys);
extern int64 blgetbitmap(IndexScanDesc scan, TIDBitmap *tbm);
extern void blrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
					 ScanKey orderbys, int norderbys);
extern void blendscan(IndexScanDesc scan);
extern IndexBuildResult *blbuild(Relation heap, Relation index,
								 struct IndexInfo *indexInfo);
extern void blbuildempty(Relation index);
extern IndexBulkDeleteResult *blbulkdelete(IndexVacuumInfo *info,
										   IndexBulkDeleteResult *stats, IndexBulkDeleteCallback callback,
										   void *callback_state);
extern IndexBulkDeleteResult *blvacuumcleanup(IndexVacuumInfo *info,
											  IndexBulkDeleteResult *stats);
extern bytea *bloptions(Datum reloptions, bool validate);
extern void blcostestimate(PlannerInfo *root, IndexPath *path,
						   double loop_count, Cost *indexStartupCost,
						   Cost *indexTotalCost, Selectivity *indexSelectivity,
						   double *indexCorrelation, double *indexPages);

#endif

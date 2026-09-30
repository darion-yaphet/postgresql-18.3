/*-------------------------------------------------------------------------
 *
 * hash.h
 *	  header file for postgres hash access method implementation
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/hash.h
 *
 * NOTES
 *		modeled after Margo Seltzer's hash implementation for unix.
 *
 *-------------------------------------------------------------------------
 */
#ifndef HASH_H
#define HASH_H

#include "access/amapi.h"
#include "access/itup.h"
#include "access/sdir.h"
#include "catalog/pg_am_d.h"
#include "common/hashfn.h"
#include "lib/stringinfo.h"
#include "storage/bufmgr.h"
#include "storage/lockdefs.h"
#include "utils/hsearch.h"
#include "utils/relcache.h"

/*
 * Mapping from hash bucket number to physical block number of bucket's
 * starting page.  Beware of multiple evaluations of argument!
 */

/*
 * 从哈希桶号映射到该桶起始页的物理块号。
 * 注意宏参数可能被多次求值！
 */
typedef uint32 Bucket;

#define InvalidBucket	((Bucket) 0xFFFFFFFF)

#define BUCKET_TO_BLKNO(metap,B) \
		((BlockNumber) ((B) + ((B) ? (metap)->hashm_spares[_hash_spareindex((B)+1)-1] : 0)) + 1)

/*
 * Special space for hash index pages.
 *
 * hasho_flag's LH_PAGE_TYPE bits tell us which type of page we're looking at.
 * Additional bits in the flag word are used for more transient purposes.
 *
 * To test a page's type, do (hasho_flag & LH_PAGE_TYPE) == LH_xxx_PAGE.
 * However, we ensure that each used page type has a distinct bit so that
 * we can OR together page types for uses such as the allowable-page-types
 * argument of _hash_checkpage().
 */

/*
 * 哈希索引页的特殊空间。
 *
 * hasho_flag 中的 LH_PAGE_TYPE 位告诉我们当前查看的是哪种类型的页。
 * 标志字中的其他位则用于更临时的用途。
 *
 * 要检测一个页的类型，可执行 (hasho_flag & LH_PAGE_TYPE) == LH_xxx_PAGE。
 * 但是，我们确保每种使用中的页类型都有一个独立的位，
 * 以便可以将多个页类型按位或组合起来，
 * 例如用于 _hash_checkpage() 的 allowable-page-types 参数。
 */
#define LH_UNUSED_PAGE			(0)
#define LH_OVERFLOW_PAGE		(1 << 0)
#define LH_BUCKET_PAGE			(1 << 1)
#define LH_BITMAP_PAGE			(1 << 2)
#define LH_META_PAGE			(1 << 3)
#define LH_BUCKET_BEING_POPULATED	(1 << 4)
#define LH_BUCKET_BEING_SPLIT	(1 << 5)
#define LH_BUCKET_NEEDS_SPLIT_CLEANUP	(1 << 6)
#define LH_PAGE_HAS_DEAD_TUPLES (1 << 7)

#define LH_PAGE_TYPE \
	(LH_OVERFLOW_PAGE | LH_BUCKET_PAGE | LH_BITMAP_PAGE | LH_META_PAGE)

/*
 * In an overflow page, hasho_prevblkno stores the block number of the previous
 * page in the bucket chain; in a bucket page, hasho_prevblkno stores the
 * hashm_maxbucket value as of the last time the bucket was last split, or
 * else as of the time the bucket was created.  The latter convention is used
 * to determine whether a cached copy of the metapage is too stale to be used
 * without needing to lock or pin the metapage.
 *
 * hasho_nextblkno is always the block number of the next page in the
 * bucket chain, or InvalidBlockNumber if there are no more such pages.
 */

/*
 * 在溢出页中，hasho_prevblkno 存储桶链中前一个页的块号；
 * 在桶页中，hasho_prevblkno 存储该桶上次分裂时的 hashm_maxbucket 值，
 * 若从未分裂则存储桶创建时的该值。后一种约定用于判断
 * 元页的缓存副本是否过于陈旧而不可用，从而无需锁定或钉住元页。
 *
 * hasho_nextblkno 始终是桶链中下一个页的块号，
 * 若不再有此类页则为 InvalidBlockNumber。
 */
typedef struct HashPageOpaqueData
{
	BlockNumber hasho_prevblkno;	/* see above */

	/* 见上文 */
	BlockNumber hasho_nextblkno;	/* see above */

	/* 见上文 */
	Bucket		hasho_bucket;	/* bucket number this pg belongs to */

	/* 此页所属的桶号 */
	uint16		hasho_flag;		/* page type code + flag bits, see above */

	/* 页类型代码 + 标志位，见上文 */
	uint16		hasho_page_id;	/* for identification of hash indexes */

	/* 用于标识哈希索引 */
} HashPageOpaqueData;

typedef HashPageOpaqueData *HashPageOpaque;

#define HashPageGetOpaque(page) ((HashPageOpaque) PageGetSpecialPointer(page))

#define H_NEEDS_SPLIT_CLEANUP(opaque)	(((opaque)->hasho_flag & LH_BUCKET_NEEDS_SPLIT_CLEANUP) != 0)
#define H_BUCKET_BEING_SPLIT(opaque)	(((opaque)->hasho_flag & LH_BUCKET_BEING_SPLIT) != 0)
#define H_BUCKET_BEING_POPULATED(opaque)	(((opaque)->hasho_flag & LH_BUCKET_BEING_POPULATED) != 0)
#define H_HAS_DEAD_TUPLES(opaque)		(((opaque)->hasho_flag & LH_PAGE_HAS_DEAD_TUPLES) != 0)

/*
 * The page ID is for the convenience of pg_filedump and similar utilities,
 * which otherwise would have a hard time telling pages of different index
 * types apart.  It should be the last 2 bytes on the page.  This is more or
 * less "free" due to alignment considerations.
 */

/*
 * 页 ID 是为了方便 pg_filedump 及类似工具，
 * 否则它们将难以区分不同索引类型的页。
 * 它应该是页上最后 2 个字节。由于对齐方面的考虑，
 * 这个字段基本上是"免费"的（不额外占用空间）。
 */
#define HASHO_PAGE_ID		0xFF80

typedef struct HashScanPosItem	/* what we remember about each match */

/* 我们对每个匹配项所记住的信息 */
{
	ItemPointerData heapTid;	/* TID of referenced heap item */

	/* 被引用堆项的 TID */
	OffsetNumber indexOffset;	/* index item's location within page */

	/* 索引项在页内的位置 */
} HashScanPosItem;

typedef struct HashScanPosData
{
	Buffer		buf;			/* if valid, the buffer is pinned */

	/* 若有效，则该缓冲区已被钉住 */
	BlockNumber currPage;		/* current hash index page */

	/* 当前哈希索引页 */
	BlockNumber nextPage;		/* next overflow page */

	/* 下一个溢出页 */
	BlockNumber prevPage;		/* prev overflow or bucket page */

	/* 前一个溢出页或桶页 */

	/*
	 * The items array is always ordered in index order (ie, increasing
	 * indexoffset).  When scanning backwards it is convenient to fill the
	 * array back-to-front, so we start at the last slot and fill downwards.
	 * Hence we need both a first-valid-entry and a last-valid-entry counter.
	 * itemIndex is a cursor showing which entry was last returned to caller.
	 */

	/*
	 * items 数组始终按索引顺序排列（即 indexoffset 递增）。
	 * 反向扫描时，从后往前填充数组更为方便，因此我们从最后一个槽位开始向下填充。
	 * 因此我们需要一个"首个有效项"计数器和一个"末个有效项"计数器。
	 * itemIndex 是一个游标，表示最后返回给调用者的是哪一项。
	 */
	int			firstItem;		/* first valid index in items[] */

	/* items[] 中首个有效下标 */
	int			lastItem;		/* last valid index in items[] */

	/* items[] 中末个有效下标 */
	int			itemIndex;		/* current index in items[] */

	/* items[] 中当前下标 */

	HashScanPosItem items[MaxIndexTuplesPerPage];	/* MUST BE LAST */

	/* 必须为最后一个成员 */
} HashScanPosData;

#define HashScanPosIsPinned(scanpos) \
( \
	AssertMacro(BlockNumberIsValid((scanpos).currPage) || \
				!BufferIsValid((scanpos).buf)), \
	BufferIsValid((scanpos).buf) \
)

#define HashScanPosIsValid(scanpos) \
( \
	AssertMacro(BlockNumberIsValid((scanpos).currPage) || \
				!BufferIsValid((scanpos).buf)), \
	BlockNumberIsValid((scanpos).currPage) \
)

#define HashScanPosInvalidate(scanpos) \
	do { \
		(scanpos).buf = InvalidBuffer; \
		(scanpos).currPage = InvalidBlockNumber; \
		(scanpos).nextPage = InvalidBlockNumber; \
		(scanpos).prevPage = InvalidBlockNumber; \
		(scanpos).firstItem = 0; \
		(scanpos).lastItem = 0; \
		(scanpos).itemIndex = 0; \
	} while (0)

/*
 *	HashScanOpaqueData is private state for a hash index scan.
 */

/*
 *	HashScanOpaqueData 是哈希索引扫描的私有状态。
 */
typedef struct HashScanOpaqueData
{
	/* Hash value of the scan key, ie, the hash key we seek */

	/* 扫描键的哈希值，即我们要查找的哈希键 */
	uint32		hashso_sk_hash;

	/* remember the buffer associated with primary bucket */

	/* 记住与主桶关联的缓冲区 */
	Buffer		hashso_bucket_buf;

	/*
	 * remember the buffer associated with primary bucket page of bucket being
	 * split.  it is required during the scan of the bucket which is being
	 * populated during split operation.
	 */

	/*
	 * 记住与正在被分裂的桶的主桶页关联的缓冲区。
	 * 在分裂操作期间对正在被填充的桶进行扫描时需要用到它。
	 */
	Buffer		hashso_split_bucket_buf;

	/* Whether scan starts on bucket being populated due to split */

	/* 扫描是否从因分裂而正在被填充的桶开始 */
	bool		hashso_buc_populated;

	/*
	 * Whether scanning bucket being split?  The value of this parameter is
	 * referred only when hashso_buc_populated is true.
	 */

	/*
	 * 是否正在扫描被分裂的桶？仅当 hashso_buc_populated 为 true 时
	 * 才会引用该参数的值。
	 */
	bool		hashso_buc_split;
	/* info about killed items if any (killedItems is NULL if never used) */

	/* 关于被删除项的信息（若从未使用则 killedItems 为 NULL） */
	int		   *killedItems;	/* currPos.items indexes of killed items */

	/* 被删除项在 currPos.items 中的下标 */
	int			numKilled;		/* number of currently stored items */

	/* 当前存储的项数 */

	/*
	 * Identify all the matching items on a page and save them in
	 * HashScanPosData
	 */

	/*
	 * 识别页上所有匹配的项并将它们保存在 HashScanPosData 中
	 */
	HashScanPosData currPos;	/* current position data */

	/* 当前位置数据 */
} HashScanOpaqueData;

typedef HashScanOpaqueData *HashScanOpaque;

/*
 * Definitions for metapage.
 */

/*
 * 元页相关定义。
 */

#define HASH_METAPAGE	0		/* metapage is always block 0 */

/* 元页始终是块 0 */

#define HASH_MAGIC		0x6440640
#define HASH_VERSION	4

/*
 * spares[] holds the number of overflow pages currently allocated at or
 * before a certain splitpoint. For example, if spares[3] = 7 then there are
 * 7 ovflpages before splitpoint 3 (compare BUCKET_TO_BLKNO macro).  The
 * value in spares[ovflpoint] increases as overflow pages are added at the
 * end of the index.  Once ovflpoint increases (ie, we have actually allocated
 * the bucket pages belonging to that splitpoint) the number of spares at the
 * prior splitpoint cannot change anymore.
 *
 * ovflpages that have been recycled for reuse can be found by looking at
 * bitmaps that are stored within ovflpages dedicated for the purpose.
 * The blknos of these bitmap pages are kept in mapp[]; nmaps is the
 * number of currently existing bitmaps.
 *
 * The limitation on the size of spares[] comes from the fact that there's
 * no point in having more than 2^32 buckets with only uint32 hashcodes.
 * (Note: The value of HASH_MAX_SPLITPOINTS which is the size of spares[] is
 * adjusted in such a way to accommodate multi phased allocation of buckets
 * after HASH_SPLITPOINT_GROUPS_WITH_ONE_PHASE).
 *
 * There is no particular upper limit on the size of mapp[], other than
 * needing to fit into the metapage.  (With 8K block size, 1024 bitmaps
 * limit us to 256 GB of overflow space...).  For smaller block size we
 * can not use 1024 bitmaps as it will lead to the meta page data crossing
 * the block size boundary.  So we use BLCKSZ to determine the maximum number
 * of bitmaps.
 */

/*
 * spares[] 保存在某个分裂点或之前当前已分配的溢出页数量。例如，
 * 若 spares[3] = 7，则在分裂点 3 之前有 7 个溢出页（参见 BUCKET_TO_BLKNO 宏）。
 * spares[ovflpoint] 中的值会随着溢出页在索引末尾被添加而增大。一旦 ovflpoint
 * 增大（即我们实际已分配了属于该分裂点的桶页），前一个分裂点处的 spares 数量
 * 就不能再改变了。
 *
 * 被回收以供复用的溢出页可以通过查看存储在专用溢出页中的位图找到。
 * 这些位图页的块号保存在 mapp[] 中；nmaps 是当前存在的位图数量。
 *
 * spares[] 大小的限制源于这样一个事实：由于哈希码只有 uint32，
 * 拥有超过 2^32 个桶没有意义。
 *（注意：HASH_MAX_SPLITPOINTS，即 spares[] 的大小，经过调整以便在
 * HASH_SPLITPOINT_GROUPS_WITH_ONE_PHASE 之后容纳桶的多阶段分配。）
 *
 * mapp[] 的大小没有特定的上限，只需能装入元页即可。
 *（以 8K 块大小为例，1024 个位图将我们限制在 256 GB 的溢出空间……）。
 * 对于更小的块大小，我们不能使用 1024 个位图，因为这会导致元页数据
 * 越过块大小边界。因此我们用 BLCKSZ 来确定位图的最大数量。
 */
#define HASH_MAX_BITMAPS			Min(BLCKSZ / 8, 1024)

#define HASH_SPLITPOINT_PHASE_BITS	2
#define HASH_SPLITPOINT_PHASES_PER_GRP	(1 << HASH_SPLITPOINT_PHASE_BITS)
#define HASH_SPLITPOINT_PHASE_MASK		(HASH_SPLITPOINT_PHASES_PER_GRP - 1)
#define HASH_SPLITPOINT_GROUPS_WITH_ONE_PHASE	10

/* defines max number of splitpoint phases a hash index can have */

/* 定义一个哈希索引可以拥有的分裂点阶段的最大数量 */
#define HASH_MAX_SPLITPOINT_GROUP	32
#define HASH_MAX_SPLITPOINTS \
	(((HASH_MAX_SPLITPOINT_GROUP - HASH_SPLITPOINT_GROUPS_WITH_ONE_PHASE) * \
	  HASH_SPLITPOINT_PHASES_PER_GRP) + \
	 HASH_SPLITPOINT_GROUPS_WITH_ONE_PHASE)

typedef struct HashMetaPageData
{
	uint32		hashm_magic;	/* magic no. for hash tables */

	/* 哈希表的幻数 */
	uint32		hashm_version;	/* version ID */

	/* 版本 ID */
	double		hashm_ntuples;	/* number of tuples stored in the table */

	/* 表中存储的元组数量 */
	uint16		hashm_ffactor;	/* target fill factor (tuples/bucket) */

	/* 目标填充因子（每桶元组数） */
	uint16		hashm_bsize;	/* index page size (bytes) */

	/* 索引页大小（字节） */
	uint16		hashm_bmsize;	/* bitmap array size (bytes) - must be a power
								 * of 2 */

	/* 位图数组大小（字节）—— 必须是 2 的幂 */
	uint16		hashm_bmshift;	/* log2(bitmap array size in BITS) */

	/* log2(以位为单位的位图数组大小) */
	uint32		hashm_maxbucket;	/* ID of maximum bucket in use */

	/* 使用中的最大桶的 ID */
	uint32		hashm_highmask; /* mask to modulo into entire table */

	/* 用于对整个表取模的掩码 */
	uint32		hashm_lowmask;	/* mask to modulo into lower half of table */

	/* 用于对表的下半部分取模的掩码 */
	uint32		hashm_ovflpoint;	/* splitpoint from which ovflpage being
									 * allocated */

	/* 正在从其分配溢出页的分裂点 */
	uint32		hashm_firstfree;	/* lowest-number free ovflpage (bit#) */

	/* 编号最小的空闲溢出页（位号） */
	uint32		hashm_nmaps;	/* number of bitmap pages */

	/* 位图页的数量 */
	RegProcedure hashm_procid;	/* hash function id from pg_proc */

	/* 来自 pg_proc 的哈希函数 id */
	uint32		hashm_spares[HASH_MAX_SPLITPOINTS]; /* spare pages before each
													 * splitpoint */

	/* 每个分裂点之前的备用页 */
	BlockNumber hashm_mapp[HASH_MAX_BITMAPS];	/* blknos of ovfl bitmaps */

	/* 溢出位图的块号 */
} HashMetaPageData;

typedef HashMetaPageData *HashMetaPage;

typedef struct HashOptions
{
	int32		varlena_header_;	/* varlena header (do not touch directly!) */

	/* varlena 头部（不要直接操作！） */
	int			fillfactor;		/* page fill factor in percent (0..100) */

	/* 页填充因子，以百分比表示（0..100） */
} HashOptions;

#define HashGetFillFactor(relation) \
	(AssertMacro(relation->rd_rel->relkind == RELKIND_INDEX && \
				 relation->rd_rel->relam == HASH_AM_OID), \
	 (relation)->rd_options ? \
	 ((HashOptions *) (relation)->rd_options)->fillfactor :	\
	 HASH_DEFAULT_FILLFACTOR)
#define HashGetTargetPageUsage(relation) \
	(BLCKSZ * HashGetFillFactor(relation) / 100)

/*
 * Maximum size of a hash index item (it's okay to have only one per page)
 */

/*
 * 哈希索引项的最大大小（每页只有一个也是可以的）
 */
#define HashMaxItemSize(page) \
	MAXALIGN_DOWN(PageGetPageSize(page) - \
				  SizeOfPageHeaderData - \
				  sizeof(ItemIdData) - \
				  MAXALIGN(sizeof(HashPageOpaqueData)))

#define INDEX_MOVED_BY_SPLIT_MASK	INDEX_AM_RESERVED_BIT

#define HASH_MIN_FILLFACTOR			10
#define HASH_DEFAULT_FILLFACTOR		75

/*
 * Constants
 */

/*
 * 常量
 */
#define BYTE_TO_BIT				3	/* 2^3 bits/byte */

/* 2^3 位/字节 */
#define ALL_SET					((uint32) ~0)

/*
 * Bitmap pages do not contain tuples.  They do contain the standard
 * page headers and trailers; however, everything in between is a
 * giant bit array.  The number of bits that fit on a page obviously
 * depends on the page size and the header/trailer overhead.  We require
 * the number of bits per page to be a power of 2.
 */

/*
 * 位图页不包含元组。它们确实包含标准的页头和页尾；
 * 然而两者之间的所有内容都是一个巨大的位数组。一个页上能容纳的
 * 位数显然取决于页大小以及页头/页尾的开销。我们要求每页的位数
 * 必须是 2 的幂。
 */
#define BMPGSZ_BYTE(metap)		((metap)->hashm_bmsize)
#define BMPGSZ_BIT(metap)		((metap)->hashm_bmsize << BYTE_TO_BIT)
#define BMPG_SHIFT(metap)		((metap)->hashm_bmshift)
#define BMPG_MASK(metap)		(BMPGSZ_BIT(metap) - 1)

#define HashPageGetBitmap(page) \
	((uint32 *) PageGetContents(page))

#define HashGetMaxBitmapSize(page) \
	(PageGetPageSize((Page) page) - \
	 (MAXALIGN(SizeOfPageHeaderData) + MAXALIGN(sizeof(HashPageOpaqueData))))

#define HashPageGetMeta(page) \
	((HashMetaPage) PageGetContents(page))

/*
 * The number of bits in an ovflpage bitmap word.
 */

/*
 * 溢出页位图字中的位数。
 */
#define BITS_PER_MAP	32		/* Number of bits in uint32 */

/* uint32 中的位数 */

/* Given the address of the beginning of a bit map, clear/set the nth bit */

/* 给定一个位图起始处的地址，清除/设置第 n 位 */
#define CLRBIT(A, N)	((A)[(N)/BITS_PER_MAP] &= ~(1<<((N)%BITS_PER_MAP)))
#define SETBIT(A, N)	((A)[(N)/BITS_PER_MAP] |= (1<<((N)%BITS_PER_MAP)))
#define ISSET(A, N)		((A)[(N)/BITS_PER_MAP] & (1<<((N)%BITS_PER_MAP)))

/*
 * page-level and high-level locking modes (see README)
 */

/*
 * 页级和高级锁定模式（参见 README）
 */
#define HASH_READ		BUFFER_LOCK_SHARE
#define HASH_WRITE		BUFFER_LOCK_EXCLUSIVE
#define HASH_NOLOCK		(-1)

/*
 * When a new operator class is declared, we require that the user supply
 * us with an amproc function for hashing a key of the new type, returning
 * a 32-bit hash value.  We call this the "standard" hash function.  We
 * also allow an optional "extended" hash function which accepts a salt and
 * returns a 64-bit hash value.  This is highly recommended but, for reasons
 * of backward compatibility, optional.
 *
 * When the salt is 0, the low 32 bits of the value returned by the extended
 * hash function should match the value that would have been returned by the
 * standard hash function.
 */

/*
 * 当声明一个新的操作符类时，我们要求用户为我们提供一个 amproc 函数，
 * 用于对新类型的键进行哈希，返回一个 32 位的哈希值。我们称之为"标准"哈希函数。
 * 我们还允许一个可选的"扩展"哈希函数，它接受一个盐值并返回一个 64 位的哈希值。
 * 强烈推荐使用它，但出于向后兼容的原因，它是可选的。
 *
 * 当盐值为 0 时，扩展哈希函数返回值的低 32 位应当与标准哈希函数
 * 将返回的值相匹配。
 */
#define HASHSTANDARD_PROC		1
#define HASHEXTENDED_PROC		2
#define HASHOPTIONS_PROC		3
#define HASHNProcs				3


/* public routines */

/* 公共例程 */

/*
 * Build a new hash index by scanning the heap relation and inserting all
 * live tuples, then return build statistics such as the tuple counts.
 */

/*
 * 通过扫描堆关系并插入所有存活元组来构建一个新的哈希索引，
 * 然后返回诸如元组计数之类的构建统计信息。
 */
extern IndexBuildResult *hashbuild(Relation heap, Relation index,
								   struct IndexInfo *indexInfo);

/*
 * Initialize an empty hash index, typically used for the init fork of an
 * unlogged index so it can be reset on crash recovery.
 */

/*
 * 初始化一个空的哈希索引，通常用于未记录日志索引的初始化分叉，
 * 以便在崩溃恢复时能够被重置。
 */
extern void hashbuildempty(Relation index);

/*
 * Insert a single index tuple built from the given values into the hash
 * index, dispatching to the low-level insert routine after computing the key.
 */

/*
 * 将根据给定值构建的单个索引元组插入哈希索引，
 * 在计算出键之后转交给底层的插入例程。
 */
extern bool hashinsert(Relation rel, Datum *values, bool *isnull,
					   ItemPointer ht_ctid, Relation heapRel,
					   IndexUniqueCheck checkUnique,
					   bool indexUnchanged,
					   struct IndexInfo *indexInfo);

/*
 * Fetch the next tuple matching the scan key in the given direction, advancing
 * the scan position and returning whether a matching tuple was found.
 */

/*
 * 按给定方向获取下一个与扫描键匹配的元组，推进扫描位置，
 * 并返回是否找到了匹配的元组。
 */
extern bool hashgettuple(IndexScanDesc scan, ScanDirection dir);

/*
 * Collect all tuples matching the scan key into the given TID bitmap and
 * return the number of tuples added.
 */

/*
 * 将所有与扫描键匹配的元组收集到给定的 TID 位图中，
 * 并返回添加的元组数量。
 */
extern int64 hashgetbitmap(IndexScanDesc scan, TIDBitmap *tbm);

/*
 * Begin a hash index scan, allocating and initializing the scan descriptor
 * and its private HashScanOpaque state.
 */

/*
 * 开始一次哈希索引扫描，分配并初始化扫描描述符
 * 及其私有的 HashScanOpaque 状态。
 */
extern IndexScanDesc hashbeginscan(Relation rel, int nkeys, int norderbys);

/*
 * Restart a hash index scan with new scan keys, releasing any resources held
 * by the previous scan position and resetting the scan state.
 */

/*
 * 使用新的扫描键重新启动一次哈希索引扫描，释放前一个扫描位置持有的
 * 任何资源，并重置扫描状态。
 */
extern void hashrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
					   ScanKey orderbys, int norderbys);

/*
 * End a hash index scan, releasing all buffers and freeing the private scan
 * state associated with the scan descriptor.
 */

/*
 * 结束一次哈希索引扫描，释放所有缓冲区并释放与扫描描述符
 * 关联的私有扫描状态。
 */
extern void hashendscan(IndexScanDesc scan);

/*
 * Perform bulk deletion of index tuples during vacuum, invoking the callback
 * to decide which tuples to remove and updating the statistics accordingly.
 */

/*
 * 在 vacuum 期间对索引元组执行批量删除，调用回调函数来决定
 * 移除哪些元组，并相应地更新统计信息。
 */
extern IndexBulkDeleteResult *hashbulkdelete(IndexVacuumInfo *info,
											 IndexBulkDeleteResult *stats,
											 IndexBulkDeleteCallback callback,
											 void *callback_state);

/*
 * Perform post-deletion cleanup after vacuum, updating the metapage and
 * returning the final index statistics.
 */

/*
 * 在 vacuum 之后执行删除后的清理工作，更新元页
 * 并返回最终的索引统计信息。
 */
extern IndexBulkDeleteResult *hashvacuumcleanup(IndexVacuumInfo *info,
												IndexBulkDeleteResult *stats);

/*
 * Parse and validate the reloptions for a hash index, returning the packed
 * option bytea used to configure the index.
 */

/*
 * 解析并验证哈希索引的 reloptions，返回用于配置索引的
 * 打包 option bytea。
 */
extern bytea *hashoptions(Datum reloptions, bool validate);

/*
 * Validate that the given operator class provides a consistent and complete
 * set of support functions and operators for the hash access method.
 */

/*
 * 验证给定的操作符类为哈希访问方法提供了一组一致且完整的
 * 支持函数和操作符。
 */
extern bool hashvalidate(Oid opclassoid);

/*
 * Adjust the dependency membership of operators and functions in a hash
 * operator family so they are correctly tied to the operator family/class.
 */

/*
 * 调整哈希操作符族中操作符和函数的依赖归属，
 * 使它们正确地关联到操作符族/类。
 */
extern void hashadjustmembers(Oid opfamilyoid,
							  Oid opclassoid,
							  List *operators,
							  List *functions);

/*
 * Translate a hash strategy number into the corresponding CompareType for the
 * given operator family.
 */

/*
 * 将哈希策略号转换为给定操作符族对应的 CompareType。
 */
extern CompareType hashtranslatestrategy(StrategyNumber strategy, Oid opfamily);

/*
 * Translate a CompareType into the corresponding hash strategy number for the
 * given operator family.
 */

/*
 * 将 CompareType 转换为给定操作符族对应的哈希策略号。
 */
extern StrategyNumber hashtranslatecmptype(CompareType cmptype, Oid opfamily);

/* private routines */

/* 私有例程 */

/* hashinsert.c */

/*
 * Insert an index tuple into the appropriate bucket page, acquiring the right
 * buckets/locks, splitting or adding overflow pages as necessary.
 */

/*
 * 将一个索引元组插入到合适的桶页中，获取正确的桶/锁，
 * 并在必要时进行分裂或添加溢出页。
 */
extern void _hash_doinsert(Relation rel, IndexTuple itup, Relation heapRel,
						   bool sorted);

/*
 * Add a single tuple to the given page at the correct sorted position, or
 * appended at the end when appendtup is set, returning its offset number.
 */

/*
 * 将单个元组按正确的排序位置添加到给定页，或在设置了 appendtup 时
 * 追加到末尾，返回其偏移号。
 */
extern OffsetNumber _hash_pgaddtup(Relation rel, Buffer buf,
								   Size itemsize, IndexTuple itup,
								   bool appendtup);

/*
 * Add multiple tuples to the given page at the specified offsets in a single
 * operation, used when moving tuples between pages.
 */

/*
 * 在单次操作中将多个元组按指定偏移添加到给定页，
 * 用于在页之间移动元组。
 */
extern void _hash_pgaddmultitup(Relation rel, Buffer buf, IndexTuple *itups,
								OffsetNumber *itup_offsets, uint16 nitups);

/* hashovfl.c */

/*
 * Add and initialize a new overflow page to the bucket chain, linking it after
 * the given page and updating the bitmap and metapage as needed.
 */

/*
 * 向桶链添加并初始化一个新的溢出页，将其链接在给定页之后，
 * 并按需更新位图和元页。
 */
extern Buffer _hash_addovflpage(Relation rel, Buffer metabuf, Buffer buf, bool retain_pin);

/*
 * Free an overflow page by moving its tuples to a write buffer, unlinking it
 * from the chain, clearing its bitmap bit, and returning the previous block.
 */

/*
 * 释放一个溢出页，将其元组移动到写缓冲区，把它从链中解除链接，
 * 清除其位图位，并返回前一个块。
 */
extern BlockNumber _hash_freeovflpage(Relation rel, Buffer bucketbuf, Buffer ovflbuf,
									  Buffer wbuf, IndexTuple *itups, OffsetNumber *itup_offsets,
									  Size *tups_size, uint16 nitups, BufferAccessStrategy bstrategy);

/*
 * Initialize a buffer as a bitmap page, setting all bits to indicate that no
 * overflow pages are allocated yet, optionally initializing the page header.
 */

/*
 * 将一个缓冲区初始化为位图页，设置所有位以表示尚未分配任何溢出页，
 * 并可选择性地初始化页头。
 */
extern void _hash_initbitmapbuffer(Buffer buf, uint16 bmsize, bool initpage);

/*
 * Squeeze a bucket by moving tuples from later overflow pages into earlier
 * pages, freeing emptied overflow pages to compact the bucket chain.
 */

/*
 * 通过将靠后溢出页中的元组移动到靠前的页来压缩一个桶，
 * 释放清空后的溢出页以紧凑桶链。
 */
extern void _hash_squeezebucket(Relation rel,
								Bucket bucket, BlockNumber bucket_blkno,
								Buffer bucket_buf,
								BufferAccessStrategy bstrategy);

/*
 * Convert an overflow page's block number into its corresponding bit number
 * within the overflow page bitmaps.
 */

/*
 * 将一个溢出页的块号转换为它在溢出页位图中对应的位号。
 */
extern uint32 _hash_ovflblkno_to_bitno(HashMetaPage metap, BlockNumber ovflblkno);

/* hashpage.c */

/*
 * Read and lock the page at the given block number with the requested access
 * mode, verifying it matches the allowed page-type flags.
 */

/*
 * 以请求的访问模式读取并锁定给定块号处的页，
 * 校验它是否匹配允许的页类型标志。
 */
extern Buffer _hash_getbuf(Relation rel, BlockNumber blkno,
						   int access, int flags);

/*
 * Attempt to acquire a cleanup lock on the page without blocking, returning
 * the pinned buffer whether or not the cleanup lock was obtained.
 */

/*
 * 尝试在不阻塞的情况下获取页上的清理锁，无论是否获得清理锁，
 * 都返回被钉住的缓冲区。
 */
extern Buffer _hash_getbuf_with_condlock_cleanup(Relation rel,
												 BlockNumber blkno, int flags);

/*
 * Return a cached copy of the metapage, optionally forcing a refresh from disk
 * when the cache may be stale.
 */

/*
 * 返回元页的缓存副本，当缓存可能陈旧时可选择性地强制从磁盘刷新。
 */
extern HashMetaPage _hash_getcachedmetap(Relation rel, Buffer *metabuf,
										 bool force_refresh);

/*
 * Locate and lock the primary bucket page corresponding to a hash key, using
 * the cached metapage and following bucket splits if necessary.
 */

/*
 * 定位并锁定与哈希键对应的主桶页，使用缓存的元页，
 * 并在必要时跟随桶的分裂。
 */
extern Buffer _hash_getbucketbuf_from_hashkey(Relation rel, uint32 hashkey,
											  int access,
											  HashMetaPage *cachedmetap);

/*
 * Get a buffer for the given block for initialization purposes, returning it
 * pinned and exclusively locked without checking its current contents.
 */

/*
 * 出于初始化目的获取给定块的缓冲区，返回时它已被钉住并独占锁定，
 * 而不检查它当前的内容。
 */
extern Buffer _hash_getinitbuf(Relation rel, BlockNumber blkno);

/*
 * Initialize a bucket page buffer in memory, setting its opaque fields such as
 * the bucket number and page flags.
 */

/*
 * 在内存中初始化一个桶页缓冲区，设置其不透明字段，
 * 例如桶号和页标志。
 */
extern void _hash_initbuf(Buffer buf, uint32 max_bucket, uint32 num_bucket,
						  uint32 flag, bool initpage);

/*
 * Extend the relation to allocate a new page at the given block, returning the
 * newly created buffer pinned and exclusively locked.
 */

/*
 * 扩展关系以在给定块处分配一个新页，返回新创建的缓冲区，
 * 它已被钉住并独占锁定。
 */
extern Buffer _hash_getnewbuf(Relation rel, BlockNumber blkno,
							  ForkNumber forkNum);

/*
 * Read and lock a page using the given buffer access strategy, useful for
 * vacuum-like scans that should not pollute the shared buffer cache.
 */

/*
 * 使用给定的缓冲区访问策略读取并锁定一个页，这对于
 * 不应污染共享缓冲区缓存的类 vacuum 扫描很有用。
 */
extern Buffer _hash_getbuf_with_strategy(Relation rel, BlockNumber blkno,
										 int access, int flags,
										 BufferAccessStrategy bstrategy);

/*
 * Unlock and unpin a buffer, releasing both the content lock and the pin held
 * on the page.
 */

/*
 * 解锁并取消钉住一个缓冲区，释放页上持有的内容锁和钉住引用。
 */
extern void _hash_relbuf(Relation rel, Buffer buf);

/*
 * Unpin a buffer without releasing any content lock, dropping only the pin
 * held on the page.
 */

/*
 * 取消钉住一个缓冲区而不释放任何内容锁，仅丢弃页上持有的钉住引用。
 */
extern void _hash_dropbuf(Relation rel, Buffer buf);

/*
 * Release all buffers pinned by an active scan, dropping the bucket and split
 * bucket buffers tracked in the scan's opaque state.
 */

/*
 * 释放活动扫描所钉住的所有缓冲区，丢弃扫描不透明状态中
 * 跟踪的桶缓冲区和分裂桶缓冲区。
 */
extern void _hash_dropscanbuf(Relation rel, HashScanOpaque so);

/*
 * Initialize a brand-new hash index, creating the metapage, initial bucket
 * pages, and bitmap page, and returning the number of buckets created.
 */

/*
 * 初始化一个全新的哈希索引，创建元页、初始桶页和位图页，
 * 并返回创建的桶数量。
 */
extern uint32 _hash_init(Relation rel, double num_tuples,
						 ForkNumber forkNum);

/*
 * Initialize the metapage buffer contents for a new hash index, computing the
 * initial bucket count, masks, and other metadata fields.
 */

/*
 * 初始化新哈希索引的元页缓冲区内容，计算初始桶数量、
 * 掩码及其他元数据字段。
 */
extern void _hash_init_metabuffer(Buffer buf, double num_tuples,
								  RegProcedure procid, uint16 ffactor, bool initpage);

/*
 * Initialize a page's header and special space for use as a hash index page of
 * the given size.
 */

/*
 * 初始化一个页的页头和特殊空间，以便用作给定大小的哈希索引页。
 */
extern void _hash_pageinit(Page page, Size size);

/*
 * Expand the hash table by adding a new bucket, splitting an existing bucket's
 * tuples into the old and new buckets as required.
 */

/*
 * 通过添加一个新桶来扩展哈希表，按需将某个现有桶的元组
 * 分裂到旧桶和新桶中。
 */
extern void _hash_expandtable(Relation rel, Buffer metabuf);

/*
 * Finish an incomplete bucket split, moving any remaining tuples from the old
 * bucket to the new bucket and clearing the in-progress split flags.
 */

/*
 * 完成一个未完成的桶分裂，将剩余的元组从旧桶移动到新桶，
 * 并清除进行中的分裂标志。
 */
extern void _hash_finish_split(Relation rel, Buffer metabuf, Buffer obuf,
							   Bucket obucket, uint32 maxbucket, uint32 highmask,
							   uint32 lowmask);

/* hashsearch.c */

/*
 * Advance the scan to the next matching tuple in the given direction, moving
 * across pages within the bucket chain as needed.
 */

/*
 * 按给定方向将扫描推进到下一个匹配的元组，
 * 并按需在桶链的页之间移动。
 */
extern bool _hash_next(IndexScanDesc scan, ScanDirection dir);

/*
 * Locate the first matching tuple for the scan, positioning the scan at the
 * correct bucket and starting page for the requested direction.
 */

/*
 * 为扫描定位第一个匹配的元组，将扫描定位到所请求方向的
 * 正确桶和起始页。
 */
extern bool _hash_first(IndexScanDesc scan, ScanDirection dir);

/* hashsort.c */
typedef struct HSpool HSpool;	/* opaque struct in hashsort.c */

/* hashsort.c 中的不透明结构 */

/*
 * Initialize a sort spool for building a hash index, preparing the tuplesort
 * state used to order tuples by bucket before insertion.
 */

/*
 * 初始化用于构建哈希索引的排序 spool，准备用于在插入前
 * 按桶对元组排序的 tuplesort 状态。
 */
extern HSpool *_h_spoolinit(Relation heap, Relation index, uint32 num_buckets);

/*
 * Destroy a hash sort spool, freeing the tuplesort state and any resources it
 * holds.
 */

/*
 * 销毁一个哈希排序 spool，释放 tuplesort 状态及其持有的任何资源。
 */
extern void _h_spooldestroy(HSpool *hspool);

/*
 * Add an index entry to the sort spool, feeding the tuple's key and heap TID
 * into the tuplesort for later ordered insertion.
 */

/*
 * 向排序 spool 添加一个索引条目，将元组的键和堆 TID 送入
 * tuplesort，以便稍后有序插入。
 */
extern void _h_spool(HSpool *hspool, ItemPointer self,
					 const Datum *values, const bool *isnull);

/*
 * Perform the sorted build by draining the spool in bucket order and inserting
 * each tuple into the hash index.
 */

/*
 * 通过按桶顺序抽取 spool 并将每个元组插入哈希索引，
 * 执行有序的构建过程。
 */
extern void _h_indexbuild(HSpool *hspool, Relation heapRel);

/* hashutil.c */

/*
 * Check whether an index tuple satisfies the scan's qualifications by
 * comparing its stored hash key against the scan key.
 */

/*
 * 通过将索引元组存储的哈希键与扫描键进行比较，
 * 检查该元组是否满足扫描的限定条件。
 */
extern bool _hash_checkqual(IndexScanDesc scan, IndexTuple itup);

/*
 * Compute the 32-bit hash key for a Datum using the index's hash support
 * function for the column's type.
 */

/*
 * 使用索引针对该列类型的哈希支持函数，为一个 Datum
 * 计算 32 位哈希键。
 */
extern uint32 _hash_datum2hashkey(Relation rel, Datum key);

/*
 * Compute the 32-bit hash key for a Datum of an explicitly given type, looking
 * up the appropriate hash function for that type.
 */

/*
 * 为一个显式给定类型的 Datum 计算 32 位哈希键，
 * 查找该类型对应的哈希函数。
 */
extern uint32 _hash_datum2hashkey_type(Relation rel, Datum key, Oid keytype);

/*
 * Map a hash key to its bucket number using the current maxbucket value and
 * the high/low masks that describe the table's split state.
 */

/*
 * 使用当前的 maxbucket 值以及描述表分裂状态的高/低掩码，
 * 将一个哈希键映射到它的桶号。
 */
extern Bucket _hash_hashkey2bucket(uint32 hashkey, uint32 maxbucket,
								   uint32 highmask, uint32 lowmask);

/*
 * Compute the index into the spares[] array for a given bucket number,
 * accounting for the multi-phase splitpoint allocation scheme.
 */

/*
 * 为给定的桶号计算 spares[] 数组的下标，
 * 并考虑多阶段的分裂点分配方案。
 */
extern uint32 _hash_spareindex(uint32 num_bucket);

/*
 * Return the total number of buckets that exist once the given splitpoint
 * phase has been fully allocated.
 */

/*
 * 返回当给定分裂点阶段被完全分配后所存在的桶总数。
 */
extern uint32 _hash_get_totalbuckets(uint32 splitpoint_phase);

/*
 * Sanity-check a hash page, verifying its magic, page type flags, and other
 * invariants against the allowed flags before use.
 */

/*
 * 对一个哈希页进行完整性检查，在使用前根据允许的标志
 * 校验其幻数、页类型标志及其他不变式。
 */
extern void _hash_checkpage(Relation rel, Buffer buf, int flags);

/*
 * Extract the stored hash key from an index tuple, which is kept as the first
 * column of the tuple.
 */

/*
 * 从一个索引元组中提取存储的哈希键，该哈希键作为元组的
 * 第一列保存。
 */
extern uint32 _hash_get_indextuple_hashkey(IndexTuple itup);

/*
 * Convert a set of user-supplied column values into the index tuple form,
 * computing the hash key that is actually stored in the hash index.
 */

/*
 * 将一组用户提供的列值转换为索引元组形式，
 * 计算实际存储在哈希索引中的哈希键。
 */
extern bool _hash_convert_tuple(Relation index,
								Datum *user_values, bool *user_isnull,
								Datum *index_values, bool *index_isnull);

/*
 * Binary-search a page for the first item with the given hash value, returning
 * the offset at which matching items begin.
 */

/*
 * 在一个页上二分查找具有给定哈希值的第一项，
 * 返回匹配项开始处的偏移。
 */
extern OffsetNumber _hash_binsearch(Page page, uint32 hash_value);

/*
 * Binary-search a page for the last item with the given hash value, returning
 * the offset at which matching items end.
 */

/*
 * 在一个页上二分查找具有给定哈希值的最后一项，
 * 返回匹配项结束处的偏移。
 */
extern OffsetNumber _hash_binsearch_last(Page page, uint32 hash_value);

/*
 * Given a new bucket produced by a split, return the block number of the old
 * bucket page that it was split from.
 */

/*
 * 给定一个由分裂产生的新桶，返回它所分裂自的旧桶页的块号。
 */
extern BlockNumber _hash_get_oldblock_from_newbucket(Relation rel, Bucket new_bucket);

/*
 * Given an old bucket, return the block number of the new bucket page that was
 * created when that bucket was split.
 */

/*
 * 给定一个旧桶，返回该桶分裂时所创建的新桶页的块号。
 */
extern BlockNumber _hash_get_newblock_from_oldbucket(Relation rel, Bucket old_bucket);

/*
 * Compute the new bucket number that an old bucket will split into given the
 * low mask and maxbucket values.
 */

/*
 * 在给定低掩码和 maxbucket 值的情况下，计算一个旧桶将分裂成的
 * 新桶号。
 */
extern Bucket _hash_get_newbucket_from_oldbucket(Relation rel, Bucket old_bucket,
												 uint32 lowmask, uint32 maxbucket);

/*
 * Mark the tuples recorded as killed during the scan as dead on their index
 * pages, allowing them to be reclaimed later.
 */

/*
 * 将扫描期间记录为已删除的元组在其索引页上标记为 dead，
 * 以便稍后可以回收它们。
 */
extern void _hash_kill_items(IndexScanDesc scan);

/* hash.c */

/*
 * Clean up a bucket by removing dead or split-moved tuples across its page
 * chain, optionally invoking the vacuum callback and updating tuple counters.
 */

/*
 * 通过在一个桶的页链中移除 dead 或因分裂而移动的元组来清理该桶，
 * 可选择性地调用 vacuum 回调并更新元组计数器。
 */
extern void hashbucketcleanup(Relation rel, Bucket cur_bucket,
							  Buffer bucket_buf, BlockNumber bucket_blkno,
							  BufferAccessStrategy bstrategy,
							  uint32 maxbucket, uint32 highmask, uint32 lowmask,
							  double *tuples_removed, double *num_index_tuples,
							  bool split_cleanup,
							  IndexBulkDeleteCallback callback, void *callback_state);

#endif							/* HASH_H */

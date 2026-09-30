/*-------------------------------------------------------------------------
 *
 * gist.h
 *	  The public API for GiST indexes. This API is exposed to
 *	  individuals implementing GiST indexes, so backward-incompatible
 *	  changes should be made with care.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/gist.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef GIST_H
#define GIST_H

#include "access/itup.h"
#include "access/stratnum.h"
#include "access/transam.h"
#include "access/xlog.h"
#include "access/xlogdefs.h"
#include "nodes/primnodes.h"
#include "storage/block.h"
#include "storage/bufpage.h"
#include "utils/relcache.h"

/*
 * amproc indexes for GiST indexes.
 */

/*
 * GiST 索引的 amproc（访问方法支持过程）索引编号。
 */
#define GIST_CONSISTENT_PROC			1
#define GIST_UNION_PROC					2
#define GIST_COMPRESS_PROC				3
#define GIST_DECOMPRESS_PROC			4
#define GIST_PENALTY_PROC				5
#define GIST_PICKSPLIT_PROC				6
#define GIST_EQUAL_PROC					7
#define GIST_DISTANCE_PROC				8
#define GIST_FETCH_PROC					9
#define GIST_OPTIONS_PROC				10
#define GIST_SORTSUPPORT_PROC			11
#define GIST_TRANSLATE_CMPTYPE_PROC		12
#define GISTNProcs					12

/*
 * Page opaque data in a GiST index page.
 */

/*
 * GiST 索引页面中的页面不透明数据（page opaque data）。
 */
#define F_LEAF				(1 << 0)	/* leaf page */

/* 叶子页面 */
#define F_DELETED			(1 << 1)	/* the page has been deleted */

/* 该页面已被删除 */
#define F_TUPLES_DELETED	(1 << 2)	/* some tuples on the page were
										 * deleted */

/* 页面上的一些元组已被删除 */
#define F_FOLLOW_RIGHT		(1 << 3)	/* page to the right has no downlink */

/* 右侧页面没有 downlink（下行指针） */
#define F_HAS_GARBAGE		(1 << 4)	/* some tuples on the page are dead,
										 * but not deleted yet */

/* 页面上的一些元组已死亡，但尚未被删除 */

/*
 * NSN (node sequence number) is a special-purpose LSN which is stored on each
 * index page in GISTPageOpaqueData and updated only during page splits.  By
 * recording the parent's LSN in GISTSearchItem.parentlsn, it is possible to
 * detect concurrent child page splits by checking if parentlsn < child's NSN,
 * and handle them properly.  The child page's LSN is insufficient for this
 * purpose since it is updated for every page change.
 */

/*
 * NSN（节点序列号）是一种特殊用途的 LSN，存储在每个索引页面的
 * GISTPageOpaqueData 中，并且仅在页面分裂时才会更新。通过将父页面的 LSN
 * 记录在 GISTSearchItem.parentlsn 中，可以通过检查 parentlsn 是否小于子页面的
 * NSN 来检测并发的子页面分裂，并对其进行正确处理。子页面的 LSN 不足以用于此
 * 目的，因为它在每次页面变更时都会被更新。
 */
typedef XLogRecPtr GistNSN;

/*
 * A fake LSN / NSN value used during index builds. Must be smaller than any
 * real or fake (unlogged) LSN generated after the index build completes so
 * that all splits are considered complete.
 */

/*
 * 在索引构建期间使用的伪造 LSN / NSN 值。它必须小于索引构建完成之后生成的任何
 * 真实或伪造（unlogged）的 LSN，以便所有的分裂都被视为已完成。
 */
#define GistBuildLSN	((XLogRecPtr) 1)

/*
 * For on-disk compatibility with pre-9.3 servers, NSN is stored as two
 * 32-bit fields on disk, same as LSNs.
 */

/*
 * 为了与 9.3 之前的服务器保持磁盘上的兼容性，NSN 在磁盘上被存储为两个 32 位的
 * 字段，与 LSN 相同。
 */
typedef PageXLogRecPtr PageGistNSN;

typedef struct GISTPageOpaqueData
{
	PageGistNSN nsn;			/* this value must change on page split */

	/* 该值在页面分裂时必须发生变化 */
	BlockNumber rightlink;		/* next page if any */

	/* 下一个页面（如果有的话） */
	uint16		flags;			/* see bit definitions above */

	/* 参见上面的位定义 */
	uint16		gist_page_id;	/* for identification of GiST indexes */

	/* 用于标识 GiST 索引 */
} GISTPageOpaqueData;

typedef GISTPageOpaqueData *GISTPageOpaque;

/*
 * Maximum possible sizes for GiST index tuple and index key.  Calculation is
 * based on assumption that GiST page should fit at least 4 tuples.  In theory,
 * GiST index can be functional when page can fit 3 tuples.  But that seems
 * rather inefficient, so we use a bit conservative estimate.
 *
 * The maximum size of index key is true for unicolumn index.  Therefore, this
 * estimation should be used to figure out which maximum size of GiST index key
 * makes sense at all.  For multicolumn indexes, user might be able to tune
 * key size using opclass parameters.
 */

/*
 * GiST 索引元组和索引键可能的最大尺寸。该计算基于这样的假设：GiST 页面至少应能
 * 容纳 4 个元组。理论上，当页面能容纳 3 个元组时 GiST 索引即可正常工作，但那样
 * 似乎相当低效，因此我们采用了一个稍偏保守的估计值。
 *
 * 索引键的最大尺寸对于单列索引是成立的。因此，这个估计应当被用于判断多大的 GiST
 * 索引键尺寸才是有意义的。对于多列索引，用户或许能够通过 opclass 参数来调节键的
 * 尺寸。
 */
#define GISTMaxIndexTupleSize	\
	MAXALIGN_DOWN((BLCKSZ - SizeOfPageHeaderData - sizeof(GISTPageOpaqueData)) / \
				  4 - sizeof(ItemIdData))

#define GISTMaxIndexKeySize	\
	(GISTMaxIndexTupleSize - MAXALIGN(sizeof(IndexTupleData)))

/*
 * The page ID is for the convenience of pg_filedump and similar utilities,
 * which otherwise would have a hard time telling pages of different index
 * types apart.  It should be the last 2 bytes on the page.  This is more or
 * less "free" due to alignment considerations.
 */

/*
 * 页面 ID 是为了方便 pg_filedump 及类似工具而设置的，否则这些工具将很难区分不同
 * 索引类型的页面。它应当是页面上的最后 2 个字节。由于对齐方面的考虑，这几乎是
 * "免费"的。
 */
#define GIST_PAGE_ID		0xFF81

/*
 * This is the Split Vector to be returned by the PickSplit method.
 * PickSplit should fill the indexes of tuples to go to the left side into
 * spl_left[], and those to go to the right into spl_right[] (note the method
 * is responsible for palloc'ing both of these arrays!).  The tuple counts
 * go into spl_nleft/spl_nright, and spl_ldatum/spl_rdatum must be set to
 * the union keys for each side.
 *
 * If spl_ldatum_exists and spl_rdatum_exists are true, then we are performing
 * a "secondary split" using a non-first index column.  In this case some
 * decisions have already been made about a page split, and the set of tuples
 * being passed to PickSplit is just the tuples about which we are undecided.
 * spl_ldatum/spl_rdatum then contain the union keys for the tuples already
 * chosen to go left or right.  Ideally the PickSplit method should take those
 * keys into account while deciding what to do with the remaining tuples, ie
 * it should try to "build out" from those unions so as to minimally expand
 * them.  If it does so, it should union the given tuples' keys into the
 * existing spl_ldatum/spl_rdatum values rather than just setting those values
 * from scratch, and then set spl_ldatum_exists/spl_rdatum_exists to false to
 * show it has done this.
 *
 * If the PickSplit method fails to clear spl_ldatum_exists/spl_rdatum_exists,
 * the core GiST code will make its own decision about how to merge the
 * secondary-split results with the previously-chosen tuples, and will then
 * recompute the union keys from scratch.  This is a workable though often not
 * optimal approach.
 */

/*
 * 这是由 PickSplit 方法返回的分裂向量（Split Vector）。PickSplit 应将要归入左侧的
 * 元组的下标填入 spl_left[]，将要归入右侧的元组的下标填入 spl_right[]（注意该方法
 * 负责为这两个数组进行 palloc 分配！）。元组数量填入 spl_nleft/spl_nright，并且
 * spl_ldatum/spl_rdatum 必须被设置为各自一侧的并集键（union key）。
 *
 * 如果 spl_ldatum_exists 和 spl_rdatum_exists 为真，则说明我们正在使用非第一个索引
 * 列执行"二次分裂"（secondary split）。在这种情况下，关于页面分裂的某些决定已经做出，
 * 而传递给 PickSplit 的这组元组只是我们尚未确定归属的那些元组。此时 spl_ldatum/
 * spl_rdatum 包含的是已被选定归入左侧或右侧的元组的并集键。理想情况下，PickSplit
 * 方法在决定如何处理剩余元组时应当把这些键考虑进去，也就是说它应当试图从这些并集
 * "向外扩展"，以尽可能小地扩大它们。如果它这样做了，就应当把给定元组的键并入现有的
 * spl_ldatum/spl_rdatum 值中，而不是从头设置这些值，然后把
 * spl_ldatum_exists/spl_rdatum_exists 设置为假以表明它已经这样做了。
 *
 * 如果 PickSplit 方法未能清除 spl_ldatum_exists/spl_rdatum_exists，那么 GiST 核心
 * 代码将自行决定如何把二次分裂的结果与先前选定的元组合并，然后从头重新计算并集键。
 * 这是一种可行但往往并非最优的方法。
 */
typedef struct GIST_SPLITVEC
{
	OffsetNumber *spl_left;		/* array of entries that go left */

	/* 归入左侧的条目数组 */
	int			spl_nleft;		/* size of this array */

	/* 该数组的大小 */
	Datum		spl_ldatum;		/* Union of keys in spl_left */

	/* spl_left 中各键的并集 */
	bool		spl_ldatum_exists;	/* true, if spl_ldatum already exists. */

	/* 为真，如果 spl_ldatum 已经存在。 */

	OffsetNumber *spl_right;	/* array of entries that go right */

	/* 归入右侧的条目数组 */
	int			spl_nright;		/* size of the array */

	/* 该数组的大小 */
	Datum		spl_rdatum;		/* Union of keys in spl_right */

	/* spl_right 中各键的并集 */
	bool		spl_rdatum_exists;	/* true, if spl_rdatum already exists. */

	/* 为真，如果 spl_rdatum 已经存在。 */
} GIST_SPLITVEC;

/*
 * An entry on a GiST node.  Contains the key, as well as its own
 * location (rel,page,offset) which can supply the matching pointer.
 * leafkey is a flag to tell us if the entry is in a leaf node.
 */

/*
 * GiST 节点上的一个条目。它包含键，以及其自身的位置（rel、page、offset），
 * 由此可以提供相匹配的指针。leafkey 是一个标志，用于告诉我们该条目是否位于
 * 叶子节点中。
 */
typedef struct GISTENTRY
{
	Datum		key;
	Relation	rel;
	Page		page;
	OffsetNumber offset;
	bool		leafkey;
} GISTENTRY;

#define GistPageGetOpaque(page) ( (GISTPageOpaque) PageGetSpecialPointer(page) )

#define GistPageIsLeaf(page)	( GistPageGetOpaque(page)->flags & F_LEAF)
#define GIST_LEAF(entry) (GistPageIsLeaf((entry)->page))

#define GistPageIsDeleted(page) ( GistPageGetOpaque(page)->flags & F_DELETED)

#define GistTuplesDeleted(page) ( GistPageGetOpaque(page)->flags & F_TUPLES_DELETED)
#define GistMarkTuplesDeleted(page) ( GistPageGetOpaque(page)->flags |= F_TUPLES_DELETED)
#define GistClearTuplesDeleted(page)	( GistPageGetOpaque(page)->flags &= ~F_TUPLES_DELETED)

#define GistPageHasGarbage(page) ( GistPageGetOpaque(page)->flags & F_HAS_GARBAGE)
#define GistMarkPageHasGarbage(page) ( GistPageGetOpaque(page)->flags |= F_HAS_GARBAGE)
#define GistClearPageHasGarbage(page)	( GistPageGetOpaque(page)->flags &= ~F_HAS_GARBAGE)

#define GistFollowRight(page) ( GistPageGetOpaque(page)->flags & F_FOLLOW_RIGHT)
#define GistMarkFollowRight(page) ( GistPageGetOpaque(page)->flags |= F_FOLLOW_RIGHT)
#define GistClearFollowRight(page)	( GistPageGetOpaque(page)->flags &= ~F_FOLLOW_RIGHT)

#define GistPageGetNSN(page) ( PageXLogRecPtrGet(GistPageGetOpaque(page)->nsn))
#define GistPageSetNSN(page, val) ( PageXLogRecPtrSet(GistPageGetOpaque(page)->nsn, val))


/*
 * On a deleted page, we store this struct. A deleted page doesn't contain any
 * tuples, so we don't use the normal page layout with line pointers. Instead,
 * this struct is stored right after the standard page header. pd_lower points
 * to the end of this struct. If we add fields to this struct in the future, we
 * can distinguish the old and new formats by pd_lower.
 */

/*
 * 在一个已删除的页面上，我们存储此结构。已删除的页面不包含任何元组，因此我们不使用
 * 带有行指针的常规页面布局。相反，此结构被存储在标准页面头之后。pd_lower 指向此结构
 * 的末尾。如果将来我们向此结构添加字段，可以通过 pd_lower 来区分新旧格式。
 */
typedef struct GISTDeletedPageContents
{
	/* last xid which could see the page in a scan */

	/* 在扫描中仍有可能看到该页面的最后一个 xid */
	FullTransactionId deleteXid;
} GISTDeletedPageContents;

/*
 * Mark a page as deleted: set the F_DELETED flag, adjust pd_lower to just
 * past the GISTDeletedPageContents header, and record the given transaction
 * id as the last xid that could still see the page in a scan.  The page must
 * already be empty.
 */

/*
 * 将页面标记为已删除：设置 F_DELETED 标志，将 pd_lower 调整到 GISTDeletedPageContents
 * 头部之后的位置，并把给定的事务 id 记录为在扫描中仍可能看到该页面的最后一个 xid。
 * 该页面必须已经为空。
 */
static inline void
GistPageSetDeleted(Page page, FullTransactionId deletexid)
{
	Assert(PageIsEmpty(page));

	GistPageGetOpaque(page)->flags |= F_DELETED;
	((PageHeader) page)->pd_lower = MAXALIGN(SizeOfPageHeaderData) + sizeof(GISTDeletedPageContents);

	((GISTDeletedPageContents *) PageGetContents(page))->deleteXid = deletexid;
}

/*
 * Return the delete xid stored on a deleted page: verify the page is deleted,
 * then, if the deleteXid field is present (as indicated by pd_lower), return
 * it; otherwise fall back to a conservative value for old-format pages that
 * predate the field.
 */

/*
 * 返回存储在已删除页面上的删除 xid：先验证该页面确实已被删除，然后，如果 deleteXid
 * 字段存在（由 pd_lower 指示），则返回它；否则，对于早于该字段的旧格式页面，
 * 退回到一个保守的取值。
 */
static inline FullTransactionId
GistPageGetDeleteXid(Page page)
{
	Assert(GistPageIsDeleted(page));

	/* Is the deleteXid field present? */

	/* deleteXid 字段是否存在？ */
	if (((PageHeader) page)->pd_lower >= MAXALIGN(SizeOfPageHeaderData) +
		offsetof(GISTDeletedPageContents, deleteXid) + sizeof(FullTransactionId))
	{
		return ((GISTDeletedPageContents *) PageGetContents(page))->deleteXid;
	}
	else
		return FullTransactionIdFromEpochAndXid(0, FirstNormalTransactionId);
}

/*
 * Vector of GISTENTRY structs; user-defined methods union and picksplit
 * take it as one of their arguments
 */

/*
 * GISTENTRY 结构体的向量；用户自定义的 union 与 picksplit 方法会把它作为其参数之一。
 */
typedef struct
{
	int32		n;				/* number of elements */

	/* 元素个数 */
	GISTENTRY	vector[FLEXIBLE_ARRAY_MEMBER];
} GistEntryVector;

#define GEVHDRSZ	(offsetof(GistEntryVector, vector))

/*
 * macro to initialize a GISTENTRY
 */

/*
 * 用于初始化一个 GISTENTRY 的宏。
 */
#define gistentryinit(e, k, r, pg, o, l) \
	do { (e).key = (k); (e).rel = (r); (e).page = (pg); \
		 (e).offset = (o); (e).leafkey = (l); } while (0)

/*
 * Translate a generic CompareType into the strategy number used by the given
 * GiST operator family, allowing ordering-related comparisons to be mapped to
 * the opclass-specific strategy for that family.
 */

/*
 * 将一个通用的 CompareType 转换为给定 GiST 操作符族所使用的策略号（strategy number），
 * 从而使与排序相关的比较能够映射到该操作符族对应 opclass 的特定策略。
 */
extern StrategyNumber gisttranslatecmptype(CompareType cmptype, Oid opfamily);

#endif							/* GIST_H */

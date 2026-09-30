/*-------------------------------------------------------------------------
 *
 * spgist_private.h
 *	  Private declarations for SP-GiST access method.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/spgist_private.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SPGIST_PRIVATE_H
#define SPGIST_PRIVATE_H

#include "access/itup.h"
#include "access/spgist.h"
#include "catalog/pg_am_d.h"
#include "nodes/tidbitmap.h"
#include "storage/buf.h"
#include "utils/geo_decls.h"
#include "utils/relcache.h"


typedef struct SpGistOptions
{
	int32		varlena_header_;	/* varlena header (do not touch directly!) */

	/* varlena 头部（不要直接操作！） */
	int			fillfactor;		/* page fill factor in percent (0..100) */

	/* 页面填充因子，以百分比表示（0..100） */
} SpGistOptions;

#define SpGistGetFillFactor(relation) \
	(AssertMacro(relation->rd_rel->relkind == RELKIND_INDEX && \
				 relation->rd_rel->relam == SPGIST_AM_OID), \
	 (relation)->rd_options ? \
	 ((SpGistOptions *) (relation)->rd_options)->fillfactor : \
	 SPGIST_DEFAULT_FILLFACTOR)
#define SpGistGetTargetPageFreeSpace(relation) \
	(BLCKSZ * (100 - SpGistGetFillFactor(relation)) / 100)


/* SPGiST leaf tuples have one key column, optionally have included columns */

/* SP-GiST 叶子元组有一个键列，可选地带有 included 列 */
#define spgKeyColumn 0
#define spgFirstIncludeColumn 1

/* Page numbers of fixed-location pages */

/* 固定位置页面的页号 */
#define SPGIST_METAPAGE_BLKNO	 (0)	/* metapage */

/* 元数据页面 */
#define SPGIST_ROOT_BLKNO		 (1)	/* root for normal entries */

/* 普通条目的根页面 */
#define SPGIST_NULL_BLKNO		 (2)	/* root for null-value entries */

/* 空值条目的根页面 */
#define SPGIST_LAST_FIXED_BLKNO  SPGIST_NULL_BLKNO

#define SpGistBlockIsRoot(blkno) \
	((blkno) == SPGIST_ROOT_BLKNO || (blkno) == SPGIST_NULL_BLKNO)
#define SpGistBlockIsFixed(blkno) \
	((BlockNumber) (blkno) <= (BlockNumber) SPGIST_LAST_FIXED_BLKNO)

/*
 * Contents of page special space on SPGiST index pages
 */

/*
 * SP-GiST 索引页面上页面特殊空间（special space）的内容
 */
typedef struct SpGistPageOpaqueData
{
	uint16		flags;			/* see bit definitions below */

	/* 参见下方的位定义 */
	uint16		nRedirection;	/* number of redirection tuples on page */

	/* 页面上重定向元组的数量 */
	uint16		nPlaceholder;	/* number of placeholder tuples on page */

	/* 页面上 placeholder 元组的数量 */
	/* note there's no count of either LIVE or DEAD tuples ... */

	/* 注意这里没有对 LIVE 或 DEAD 元组的计数…… */
	uint16		spgist_page_id; /* for identification of SP-GiST indexes */

	/* 用于识别 SP-GiST 索引 */
} SpGistPageOpaqueData;

typedef SpGistPageOpaqueData *SpGistPageOpaque;

/* Flag bits in page special space */

/* 页面特殊空间中的标志位 */
#define SPGIST_META			(1<<0)
#define SPGIST_DELETED		(1<<1)	/* never set, but keep for backwards
									 * compatibility */

/* 从不设置，但为了向后兼容而保留 */
#define SPGIST_LEAF			(1<<2)
#define SPGIST_NULLS		(1<<3)

#define SpGistPageGetOpaque(page) ((SpGistPageOpaque) PageGetSpecialPointer(page))
#define SpGistPageIsMeta(page) (SpGistPageGetOpaque(page)->flags & SPGIST_META)
#define SpGistPageIsDeleted(page) (SpGistPageGetOpaque(page)->flags & SPGIST_DELETED)
#define SpGistPageIsLeaf(page) (SpGistPageGetOpaque(page)->flags & SPGIST_LEAF)
#define SpGistPageStoresNulls(page) (SpGistPageGetOpaque(page)->flags & SPGIST_NULLS)

/*
 * The page ID is for the convenience of pg_filedump and similar utilities,
 * which otherwise would have a hard time telling pages of different index
 * types apart.  It should be the last 2 bytes on the page.  This is more or
 * less "free" due to alignment considerations.
 *
 * See comments above GinPageOpaqueData.
 */

/*
 * 页面 ID 是为了方便 pg_filedump 及类似工具，否则这些工具将很难区分
 * 不同索引类型的页面。它应当是页面上最后的 2 个字节。由于对齐方面的
 * 考虑，这基本上是“免费的”（不额外占用空间）。
 *
 * 参见 GinPageOpaqueData 上方的注释。
 */
#define SPGIST_PAGE_ID		0xFF82

/*
 * Each backend keeps a cache of last-used page info in its index->rd_amcache
 * area.  This is initialized from, and occasionally written back to,
 * shared storage in the index metapage.
 */

/*
 * 每个后端进程在其 index->rd_amcache 区域中保存一份最近使用页面信息的
 * 缓存。该缓存从索引元数据页面中的共享存储初始化，并偶尔写回到那里。
 */
typedef struct SpGistLastUsedPage
{
	BlockNumber blkno;			/* block number, or InvalidBlockNumber */

	/* 块号，或 InvalidBlockNumber */
	int			freeSpace;		/* page's free space (could be obsolete!) */

	/* 页面的空闲空间（可能已过期！） */
} SpGistLastUsedPage;

/* Note: indexes in cachedPage[] match flag assignments for SpGistGetBuffer */

/* 注意：cachedPage[] 中的索引与 SpGistGetBuffer 的标志赋值相对应 */
#define SPGIST_CACHED_PAGES 8

typedef struct SpGistLUPCache
{
	SpGistLastUsedPage cachedPage[SPGIST_CACHED_PAGES];
} SpGistLUPCache;

/*
 * metapage
 */

/*
 * 元数据页面
 */
typedef struct SpGistMetaPageData
{
	uint32		magicNumber;	/* for identity cross-check */

	/* 用于身份交叉校验 */
	SpGistLUPCache lastUsedPages;	/* shared storage of last-used info */

	/* 最近使用信息的共享存储 */
} SpGistMetaPageData;

#define SPGIST_MAGIC_NUMBER (0xBA0BABEE)

#define SpGistPageGetMeta(p) \
	((SpGistMetaPageData *) PageGetContents(p))

/*
 * Private state of index AM.  SpGistState is common to both insert and
 * search code; SpGistScanOpaque is for searches only.
 */

/*
 * 索引访问方法的私有状态。SpGistState 同时被插入代码和搜索代码共用；
 * SpGistScanOpaque 仅用于搜索。
 */

typedef struct SpGistLeafTupleData *SpGistLeafTuple;	/* forward reference */

/* 前向引用 */

/* Per-datatype info needed in SpGistState */

/* SpGistState 中所需的每种数据类型的信息 */
typedef struct SpGistTypeDesc
{
	Oid			type;
	int16		attlen;
	bool		attbyval;
	char		attalign;
	char		attstorage;
} SpGistTypeDesc;

typedef struct SpGistState
{
	Relation	index;			/* index we're working with */

	/* 我们正在处理的索引 */

	spgConfigOut config;		/* filled in by opclass config method */

	/* 由操作符类的 config 方法填充 */

	SpGistTypeDesc attType;		/* type of values to be indexed/restored */

	/* 需要被索引/还原的值的类型 */
	SpGistTypeDesc attLeafType; /* type of leaf-tuple values */

	/* 叶子元组值的类型 */
	SpGistTypeDesc attPrefixType;	/* type of inner-tuple prefix values */

	/* 内部元组前缀值的类型 */
	SpGistTypeDesc attLabelType;	/* type of node label values */

	/* 节点标签值的类型 */

	/* leafTupDesc typically points to index's tupdesc, but not always */

	/* leafTupDesc 通常指向索引的 tupdesc，但并非总是如此 */
	TupleDesc	leafTupDesc;	/* descriptor for leaf-level tuples */

	/* 叶子层元组的描述符 */

	char	   *deadTupleStorage;	/* workspace for spgFormDeadTuple */

	/* 供 spgFormDeadTuple 使用的工作空间 */

	TransactionId redirectXid;	/* XID to use when creating a redirect tuple */

	/* 创建重定向元组时使用的 XID */
	bool		isBuild;		/* true if doing index build */

	/* 如果正在进行索引构建则为 true */
} SpGistState;

/* Item to be re-examined later during a search */

/* 在搜索过程中稍后需要重新检查的项 */
typedef struct SpGistSearchItem
{
	pairingheap_node phNode;	/* pairing heap node */

	/* 配对堆（pairing heap）节点 */
	Datum		value;			/* value reconstructed from parent, or
								 * leafValue if isLeaf */

	/* 从父级重建出的值，若 isLeaf 则为 leafValue */
	SpGistLeafTuple leafTuple;	/* whole leaf tuple, if needed */

	/* 整个叶子元组（如果需要） */
	void	   *traversalValue; /* opclass-specific traverse value */

	/* 操作符类特定的遍历值 */
	int			level;			/* level of items on this page */

	/* 此页面上各项的层级 */
	ItemPointerData heapPtr;	/* heap info, if heap tuple */

	/* 堆信息（如果是堆元组） */
	bool		isNull;			/* SearchItem is NULL item */

	/* SearchItem 是否为 NULL 项 */
	bool		isLeaf;			/* SearchItem is heap item */

	/* SearchItem 是否为堆项 */
	bool		recheck;		/* qual recheck is needed */

	/* 是否需要对条件（qual）进行重新检查 */
	bool		recheckDistances;	/* distance recheck is needed */

	/* 是否需要对距离进行重新检查 */

	/* array with numberOfOrderBys entries */

	/* 拥有 numberOfOrderBys 个元素的数组 */
	double		distances[FLEXIBLE_ARRAY_MEMBER];
} SpGistSearchItem;

#define SizeOfSpGistSearchItem(n_distances) \
	(offsetof(SpGistSearchItem, distances) + sizeof(double) * (n_distances))

/*
 * Private state of an index scan
 */

/*
 * 一次索引扫描的私有状态
 */
typedef struct SpGistScanOpaqueData
{
	SpGistState state;			/* see above */

	/* 参见上文 */
	pairingheap *scanQueue;		/* queue of to be visited items */

	/* 待访问项的队列 */
	MemoryContext tempCxt;		/* short-lived memory context */

	/* 短生命周期的内存上下文 */
	MemoryContext traversalCxt; /* single scan lifetime memory context */

	/* 生命周期与单次扫描相同的内存上下文 */

	/* Control flags showing whether to search nulls and/or non-nulls */

	/* 用于表明是否搜索空值和/或非空值的控制标志 */
	bool		searchNulls;	/* scan matches (all) null entries */

	/* 扫描是否匹配（全部）空值条目 */
	bool		searchNonNulls; /* scan matches (some) non-null entries */

	/* 扫描是否匹配（部分）非空条目 */

	/* Index quals to be passed to opclass (null-related quals removed) */

	/* 将传递给操作符类的索引条件（已移除与空值相关的条件） */
	int			numberOfKeys;	/* number of index qualifier conditions */

	/* 索引限定条件的数量 */
	ScanKey		keyData;		/* array of index qualifier descriptors */

	/* 索引限定条件描述符的数组 */
	int			numberOfOrderBys;	/* number of ordering operators */

	/* 排序操作符的数量 */
	int			numberOfNonNullOrderBys;	/* number of ordering operators
											 * with non-NULL arguments */

	/* 带有非 NULL 参数的排序操作符的数量 */
	ScanKey		orderByData;	/* array of ordering op descriptors */

	/* 排序操作符描述符的数组 */
	Oid		   *orderByTypes;	/* array of ordering op return types */

	/* 排序操作符返回类型的数组 */
	int		   *nonNullOrderByOffsets;	/* array of offset of non-NULL
										 * ordering keys in the original array */

	/* 非 NULL 排序键在原始数组中偏移位置的数组 */
	Oid			indexCollation; /* collation of index column */

	/* 索引列的排序规则（collation） */

	/* Opclass defined functions: */

	/* 操作符类定义的函数： */
	FmgrInfo	innerConsistentFn;
	FmgrInfo	leafConsistentFn;

	/* Pre-allocated workspace arrays: */

	/* 预先分配的工作空间数组： */
	double	   *zeroDistances;
	double	   *infDistances;

	/* These fields are only used in amgetbitmap scans: */

	/* 以下字段仅在 amgetbitmap 扫描中使用： */
	TIDBitmap  *tbm;			/* bitmap being filled */

	/* 正在被填充的位图 */
	int64		ntids;			/* number of TIDs passed to bitmap */

	/* 已传递给位图的 TID 数量 */

	/* These fields are only used in amgettuple scans: */

	/* 以下字段仅在 amgettuple 扫描中使用： */
	bool		want_itup;		/* are we reconstructing tuples? */

	/* 我们是否正在重建元组？ */
	TupleDesc	reconTupDesc;	/* if so, descriptor for reconstructed tuples */

	/* 如果是，则为重建元组的描述符 */
	int			nPtrs;			/* number of TIDs found on current page */

	/* 在当前页面上找到的 TID 数量 */
	int			iPtr;			/* index for scanning through same */

	/* 用于遍历上述 TID 的索引 */
	ItemPointerData heapPtrs[MaxIndexTuplesPerPage];	/* TIDs from cur page */

	/* 来自当前页面的 TID */
	bool		recheck[MaxIndexTuplesPerPage]; /* their recheck flags */

	/* 它们的 recheck 标志 */
	bool		recheckDistances[MaxIndexTuplesPerPage];	/* distance recheck
															 * flags */

	/* 距离 recheck 标志 */
	HeapTuple	reconTups[MaxIndexTuplesPerPage];	/* reconstructed tuples */

	/* 重建出的元组 */

	/* distances (for recheck) */

	/* 距离（用于 recheck） */
	IndexOrderByDistance *distances[MaxIndexTuplesPerPage];

	/*
	 * Note: using MaxIndexTuplesPerPage above is a bit hokey since
	 * SpGistLeafTuples aren't exactly IndexTuples; however, they are larger,
	 * so this is safe.
	 */

	/*
	 * 注意：上面使用 MaxIndexTuplesPerPage 有点牵强，因为 SpGistLeafTuple
	 * 并不完全是 IndexTuple；不过，它们更大，所以这样做是安全的。
	 */
} SpGistScanOpaqueData;

typedef SpGistScanOpaqueData *SpGistScanOpaque;

/*
 * This struct is what we actually keep in index->rd_amcache.  It includes
 * static configuration information as well as the lastUsedPages cache.
 */

/*
 * 这个结构体是我们实际保存在 index->rd_amcache 中的内容。它包含静态配置
 * 信息以及 lastUsedPages 缓存。
 */
typedef struct SpGistCache
{
	spgConfigOut config;		/* filled in by opclass config method */

	/* 由操作符类的 config 方法填充 */

	SpGistTypeDesc attType;		/* type of values to be indexed/restored */

	/* 需要被索引/还原的值的类型 */
	SpGistTypeDesc attLeafType; /* type of leaf-tuple values */

	/* 叶子元组值的类型 */
	SpGistTypeDesc attPrefixType;	/* type of inner-tuple prefix values */

	/* 内部元组前缀值的类型 */
	SpGistTypeDesc attLabelType;	/* type of node label values */

	/* 节点标签值的类型 */

	SpGistLUPCache lastUsedPages;	/* local storage of last-used info */

	/* 最近使用信息的本地存储 */
} SpGistCache;


/*
 * SPGiST tuple types.  Note: inner, leaf, and dead tuple structs
 * must have the same tupstate field in the same position!	Real inner and
 * leaf tuples always have tupstate = LIVE; if the state is something else,
 * use the SpGistDeadTuple struct to inspect the tuple.
 */

/*
 * SP-GiST 元组类型。注意：inner、leaf 和 dead 元组结构体必须在相同的位置
 * 拥有相同的 tupstate 字段！真正的 inner 和 leaf 元组的 tupstate 始终为
 * LIVE；如果状态是其他值，则使用 SpGistDeadTuple 结构体来检查该元组。
 */

/* values of tupstate (see README for more info) */

/* tupstate 的取值（更多信息参见 README） */
#define SPGIST_LIVE			0	/* normal live tuple (either inner or leaf) */

/* 正常的存活元组（inner 或 leaf 均可） */
#define SPGIST_REDIRECT		1	/* temporary redirection placeholder */

/* 临时的重定向 placeholder */
#define SPGIST_DEAD			2	/* dead, cannot be removed because of links */

/* 已死亡，但由于存在链接而无法被移除 */
#define SPGIST_PLACEHOLDER	3	/* placeholder, used to preserve offsets */

/* placeholder，用于保持偏移位置不变 */

/*
 * SPGiST inner tuple: list of "nodes" that subdivide a set of tuples
 *
 * Inner tuple layout:
 * header/optional prefix/array of nodes, which are SpGistNodeTuples
 *
 * size and prefixSize must be multiples of MAXALIGN
 *
 * If the prefix datum is of a pass-by-value type, it is stored in its
 * Datum representation, that is its on-disk representation is of length
 * sizeof(Datum).  This is a fairly unfortunate choice, because in no other
 * place does Postgres use Datum as an on-disk representation; it creates
 * an unnecessary incompatibility between 32-bit and 64-bit builds.  But the
 * compatibility loss is mostly theoretical since MAXIMUM_ALIGNOF typically
 * differs between such builds, too.  Anyway we're stuck with it now.
 */

/*
 * SP-GiST 内部元组：一组用于细分元组集合的“节点”列表
 *
 * 内部元组布局：
 * 头部 / 可选前缀 / 节点数组（这些节点是 SpGistNodeTuple）
 *
 * size 和 prefixSize 必须是 MAXALIGN 的倍数
 *
 * 如果前缀 datum 是按值传递（pass-by-value）类型，它会以其 Datum 表示
 * 形式存储，也就是说其磁盘上的表示长度为 sizeof(Datum)。这是一个相当
 * 不幸的选择，因为 Postgres 在其他任何地方都不会将 Datum 用作磁盘表示；
 * 它在 32 位与 64 位构建之间制造了不必要的不兼容。但这种兼容性损失
 * 大多是理论上的，因为 MAXIMUM_ALIGNOF 在这两类构建之间通常也不同。
 * 不管怎样，现在我们已经无法摆脱它了。
 */
typedef struct SpGistInnerTupleData
{
	unsigned int tupstate:2,	/* LIVE/REDIRECT/DEAD/PLACEHOLDER */

	/* LIVE/REDIRECT/DEAD/PLACEHOLDER */
				allTheSame:1,	/* all nodes in tuple are equivalent */

	/* 元组中的所有节点都是等价的 */
				nNodes:13,		/* number of nodes within inner tuple */

	/* 内部元组内的节点数量 */
				prefixSize:16;	/* size of prefix, or 0 if none */

	/* 前缀的大小，若没有前缀则为 0 */
	uint16		size;			/* total size of inner tuple */

	/* 内部元组的总大小 */
	/* On most machines there will be a couple of wasted bytes here */

	/* 在大多数机器上，这里会有几个被浪费的字节 */
	/* prefix datum follows, then nodes */

	/* 随后跟着前缀 datum，然后是节点 */
} SpGistInnerTupleData;

typedef SpGistInnerTupleData *SpGistInnerTuple;

/* these must match largest values that fit in bit fields declared above */

/* 这些值必须与上面声明的位字段所能容纳的最大值相匹配 */
#define SGITMAXNNODES		0x1FFF
#define SGITMAXPREFIXSIZE	0xFFFF
#define SGITMAXSIZE			0xFFFF

#define SGITHDRSZ			MAXALIGN(sizeof(SpGistInnerTupleData))
#define _SGITDATA(x)		(((char *) (x)) + SGITHDRSZ)
#define SGITDATAPTR(x)		((x)->prefixSize ? _SGITDATA(x) : NULL)
#define SGITDATUM(x, s)		((x)->prefixSize ? \
							 ((s)->attPrefixType.attbyval ? \
							  *(Datum *) _SGITDATA(x) : \
							  PointerGetDatum(_SGITDATA(x))) \
							 : (Datum) 0)
#define SGITNODEPTR(x)		((SpGistNodeTuple) (_SGITDATA(x) + (x)->prefixSize))

/* Macro for iterating through the nodes of an inner tuple */

/* 用于遍历一个内部元组中各节点的宏 */
#define SGITITERATE(x, i, nt)	\
	for ((i) = 0, (nt) = SGITNODEPTR(x); \
		 (i) < (x)->nNodes; \
		 (i)++, (nt) = (SpGistNodeTuple) (((char *) (nt)) + IndexTupleSize(nt)))

/*
 * SPGiST node tuple: one node within an inner tuple
 *
 * Node tuples use the same header as ordinary Postgres IndexTuples, but
 * we do not use a null bitmap, because we know there is only one column
 * so the INDEX_NULL_MASK bit suffices.  Also, pass-by-value datums are
 * stored in Datum form, the same convention as for inner tuple prefixes.
 */

/*
 * SP-GiST 节点元组：一个内部元组中的单个节点
 *
 * 节点元组使用与普通 Postgres IndexTuple 相同的头部，但我们不使用空值
 * 位图，因为我们知道只有一个列，所以 INDEX_NULL_MASK 位就足够了。此外，
 * 按值传递的 datum 以 Datum 形式存储，与内部元组前缀采用相同的约定。
 */

typedef IndexTupleData SpGistNodeTupleData;

typedef SpGistNodeTupleData *SpGistNodeTuple;

#define SGNTHDRSZ			MAXALIGN(sizeof(SpGistNodeTupleData))
#define SGNTDATAPTR(x)		(((char *) (x)) + SGNTHDRSZ)
#define SGNTDATUM(x, s)		((s)->attLabelType.attbyval ? \
							 *(Datum *) SGNTDATAPTR(x) : \
							 PointerGetDatum(SGNTDATAPTR(x)))

/*
 * SPGiST leaf tuple: carries a leaf datum and a heap tuple TID,
 * and optionally some "included" columns.
 *
 * In the simplest case, the leaf datum is the same as the indexed value;
 * but it could also be a suffix or some other sort of delta that permits
 * reconstruction given knowledge of the prefix path traversed to get here.
 * Any included columns are stored without modification.
 *
 * A nulls bitmap is present if there are included columns AND any of the
 * datums are NULL.  We do not need a nulls bitmap for the case of a null
 * leaf datum without included columns, as we can infer whether the leaf
 * datum is null from whether the tuple is stored on a nulls page.  (This
 * provision is mostly for backwards compatibility, but it does save space
 * on 32-bit machines.)  As with other PG index tuple designs, if the nulls
 * bitmap exists then it's of size INDEX_MAX_KEYS bits regardless of the
 * actual number of attributes.  For the usual choice of INDEX_MAX_KEYS,
 * this costs nothing because of alignment considerations.
 *
 * The size field is wider than could possibly be needed for an on-disk leaf
 * tuple, but this allows us to form leaf tuples even when the datum is too
 * wide to be stored immediately, and it costs nothing because of alignment
 * considerations.
 *
 * t_info holds the nextOffset field (14 bits wide, enough for supported
 * page sizes) plus the has-nulls-bitmap flag bit; another flag bit is free.
 *
 * Normally, nextOffset links to the next tuple belonging to the same parent
 * node (which must be on the same page), or it's 0 if there is no next tuple.
 * But when the root page is a leaf page, we don't chain its tuples,
 * so nextOffset is always 0 on the root.
 *
 * size must be a multiple of MAXALIGN; also, it must be at least SGDTSIZE
 * so that the tuple can be converted to REDIRECT status later.  (This
 * restriction only adds bytes for a NULL leaf datum stored on a 32-bit
 * machine; otherwise alignment restrictions force it anyway.)
 */

/*
 * SP-GiST 叶子元组：携带一个叶子 datum 和一个堆元组 TID，
 * 并可选地携带一些“included”列。
 *
 * 在最简单的情况下，叶子 datum 与被索引的值相同；但它也可以是一个后缀
 * 或某种其他形式的增量（delta），在已知到达此处所遍历的前缀路径的前提下
 * 允许重建原值。任何 included 列都会原封不动地存储。
 *
 * 当存在 included 列并且其中任一 datum 为 NULL 时，才会出现空值位图。
 * 对于没有 included 列的空叶子 datum 这种情况，我们不需要空值位图，因为
 * 我们可以从元组是否存储在 nulls 页面上来推断该叶子 datum 是否为空。
 *（这一规定主要是为了向后兼容，但它在 32 位机器上确实能节省空间。）与
 * 其他 PG 索引元组设计一样，如果空值位图存在，那么无论实际属性数量是
 * 多少，它的大小都是 INDEX_MAX_KEYS 位。对于通常选取的 INDEX_MAX_KEYS，
 * 由于对齐方面的考虑，这不会带来任何额外开销。
 *
 * size 字段比磁盘上叶子元组可能需要的宽度更宽，但这使我们即使在 datum
 * 太宽而无法立即存储时也能构建叶子元组，而且由于对齐方面的考虑，这不会
 * 带来任何额外开销。
 *
 * t_info 保存 nextOffset 字段（14 位宽，足以覆盖所支持的页面大小）以及
 * “是否有空值位图”的标志位；还有另一个空闲的标志位。
 *
 * 通常，nextOffset 链接到属于同一父节点的下一个元组（该元组必须在同一
 * 页面上），若没有下一个元组则为 0。但当根页面是叶子页面时，我们不会
 * 将其元组串成链，所以根页面上的 nextOffset 始终为 0。
 *
 * size 必须是 MAXALIGN 的倍数；此外，它还必须至少为 SGDTSIZE，以便该元组
 * 之后可以被转换为 REDIRECT 状态。（这一限制仅在 32 位机器上存储 NULL
 * 叶子 datum 时才会增加字节；否则对齐限制无论如何都会强制满足这一点。）
 */
typedef struct SpGistLeafTupleData
{
	unsigned int tupstate:2,	/* LIVE/REDIRECT/DEAD/PLACEHOLDER */

	/* LIVE/REDIRECT/DEAD/PLACEHOLDER */
				size:30;		/* large enough for any palloc'able value */

	/* 足够大以容纳任何可 palloc 的值 */
	uint16		t_info;			/* nextOffset, which links to the next tuple
								 * in chain, plus two flag bits */

	/* nextOffset，链接到链中的下一个元组，外加两个标志位 */
	ItemPointerData heapPtr;	/* TID of represented heap tuple */

	/* 所代表的堆元组的 TID */
	/* nulls bitmap follows if the flag bit for it is set */

	/* 如果对应的标志位被设置，则随后跟着空值位图 */
	/* leaf datum, then any included datums, follows on a MAXALIGN boundary */

	/* 叶子 datum，然后是任何 included datum，跟随在 MAXALIGN 边界之上 */
} SpGistLeafTupleData;

/* Macros to access nextOffset and bit fields inside t_info */

/* 用于访问 t_info 内部的 nextOffset 和各标志位字段的宏 */
#define SGLT_GET_NEXTOFFSET(spgLeafTuple) \
	((spgLeafTuple)->t_info & 0x3FFF)
#define SGLT_GET_HASNULLMASK(spgLeafTuple) \
	(((spgLeafTuple)->t_info & 0x8000) ? true : false)
#define SGLT_SET_NEXTOFFSET(spgLeafTuple, offsetNumber) \
	((spgLeafTuple)->t_info = \
	 ((spgLeafTuple)->t_info & 0xC000) | ((offsetNumber) & 0x3FFF))
#define SGLT_SET_HASNULLMASK(spgLeafTuple, hasnulls) \
	((spgLeafTuple)->t_info = \
	 ((spgLeafTuple)->t_info & 0x7FFF) | ((hasnulls) ? 0x8000 : 0))

#define SGLTHDRSZ(hasnulls) \
	((hasnulls) ? MAXALIGN(sizeof(SpGistLeafTupleData) + \
						   sizeof(IndexAttributeBitMapData)) : \
	 MAXALIGN(sizeof(SpGistLeafTupleData)))
#define SGLTDATAPTR(x)		(((char *) (x)) + SGLTHDRSZ(SGLT_GET_HASNULLMASK(x)))
#define SGLTDATUM(x, s)		fetch_att(SGLTDATAPTR(x), \
									  (s)->attLeafType.attbyval, \
									  (s)->attLeafType.attlen)

/*
 * SPGiST dead tuple: declaration for examining non-live tuples
 *
 * The tupstate field of this struct must match those of regular inner and
 * leaf tuples, and its size field must match a leaf tuple's.
 * Also, the pointer field must be in the same place as a leaf tuple's heapPtr
 * field, to satisfy some Asserts that we make when replacing a leaf tuple
 * with a dead tuple.
 * We don't use t_info, but it's needed to align the pointer field.
 * pointer and xid are only valid when tupstate = REDIRECT, and in some
 * cases xid can be InvalidTransactionId even then; see initSpGistState.
 */

/*
 * SP-GiST 死亡元组：用于检查非存活元组的声明
 *
 * 此结构体的 tupstate 字段必须与常规的 inner 和 leaf 元组相匹配，其 size
 * 字段必须与叶子元组的相匹配。
 * 此外，pointer 字段必须与叶子元组的 heapPtr 字段处于相同的位置，以满足
 * 我们在用死亡元组替换叶子元组时所做的一些 Assert。
 * 我们不使用 t_info，但需要它来对齐 pointer 字段。
 * pointer 和 xid 仅在 tupstate = REDIRECT 时有效，而且在某些情况下即便
 * 此时 xid 也可能是 InvalidTransactionId；参见 initSpGistState。
 */
typedef struct SpGistDeadTupleData
{
	unsigned int tupstate:2,	/* LIVE/REDIRECT/DEAD/PLACEHOLDER */

	/* LIVE/REDIRECT/DEAD/PLACEHOLDER */
				size:30;
	uint16		t_info;			/* not used in dead tuples */

	/* 在死亡元组中不使用 */
	ItemPointerData pointer;	/* redirection inside index */

	/* 索引内部的重定向 */
	TransactionId xid;			/* ID of xact that inserted this tuple */

	/* 插入此元组的事务的 ID */
} SpGistDeadTupleData;

typedef SpGistDeadTupleData *SpGistDeadTuple;

#define SGDTSIZE		MAXALIGN(sizeof(SpGistDeadTupleData))

/*
 * Macros for doing free-space calculations.  Note that when adding up the
 * space needed for tuples, we always consider each tuple to need the tuple's
 * size plus sizeof(ItemIdData) (for the line pointer).  This works correctly
 * so long as tuple sizes are always maxaligned.
 */

/*
 * 用于进行空闲空间计算的宏。注意，在累加元组所需的空间时，我们始终认为
 * 每个元组需要元组自身的大小再加上 sizeof(ItemIdData)（用于行指针）。
 * 只要元组大小始终是 maxalign 对齐的，这样计算就是正确的。
 */

/* Page capacity after allowing for fixed header and special space */

/* 在扣除固定头部和特殊空间之后的页面容量 */
#define SPGIST_PAGE_CAPACITY  \
	MAXALIGN_DOWN(BLCKSZ - \
				  SizeOfPageHeaderData - \
				  MAXALIGN(sizeof(SpGistPageOpaqueData)))

/*
 * Compute free space on page, assuming that up to n placeholders can be
 * recycled if present (n should be the number of tuples to be inserted)
 */

/*
 * 计算页面上的空闲空间，假设若存在 placeholder，则最多可回收 n 个
 *（n 应为将要插入的元组数量）
 */
#define SpGistPageGetFreeSpace(p, n) \
	(PageGetExactFreeSpace(p) + \
	 Min(SpGistPageGetOpaque(p)->nPlaceholder, n) * \
	 (SGDTSIZE + sizeof(ItemIdData)))

/*
 * XLOG stuff
 */

/*
 * XLOG 相关内容
 */

#define STORE_STATE(s, d)  \
	do { \
		(d).redirectXid = (s)->redirectXid; \
		(d).isBuild = (s)->isBuild; \
	} while(0)

/*
 * The "flags" argument for SpGistGetBuffer should be either GBUF_LEAF to
 * get a leaf page, or GBUF_INNER_PARITY(blockNumber) to get an inner
 * page in the same triple-parity group as the specified block number.
 * (Typically, this should be GBUF_INNER_PARITY(parentBlockNumber + 1)
 * to follow the rule described in spgist/README.)
 * In addition, GBUF_NULLS can be OR'd in to get a page for storage of
 * null-valued tuples.
 *
 * Note: these flag values are used as indexes into lastUsedPages.
 */

/*
 * SpGistGetBuffer 的 “flags” 参数应当是 GBUF_LEAF（用于获取一个叶子
 * 页面），或者是 GBUF_INNER_PARITY(blockNumber)（用于获取一个与指定块号
 * 处于同一三重奇偶性组（triple-parity group）中的内部页面）。
 *（通常，为遵循 spgist/README 中描述的规则，它应当是
 * GBUF_INNER_PARITY(parentBlockNumber + 1)。）
 * 此外，可以按位或上 GBUF_NULLS，以获取一个用于存储空值元组的页面。
 *
 * 注意：这些标志值被用作 lastUsedPages 的索引。
 */
#define GBUF_LEAF				0x03
#define GBUF_INNER_PARITY(x)	((x) % 3)
#define GBUF_NULLS				0x04

#define GBUF_PARITY_MASK		0x03
#define GBUF_REQ_LEAF(flags)	(((flags) & GBUF_PARITY_MASK) == GBUF_LEAF)
#define GBUF_REQ_NULLS(flags)	((flags) & GBUF_NULLS)

/* spgutils.c */

/* 以下函数来自 spgutils.c */

/* reloption parameters */

/* reloption 参数 */
#define SPGIST_MIN_FILLFACTOR			10
#define SPGIST_DEFAULT_FILLFACTOR		80

/*
 * spgGetCache: return the per-index SpGistCache from index->rd_amcache,
 * building and caching it (by calling the opclass config method and filling in
 * type descriptors) on first use.
 */

/*
 * spgGetCache：从 index->rd_amcache 返回每个索引对应的 SpGistCache，在首次
 * 使用时构建并缓存它（通过调用操作符类的 config 方法并填充类型描述符）。
 */
extern SpGistCache *spgGetCache(Relation index);

/*
 * getSpGistTupleDesc: build the TupleDesc used for leaf tuples, substituting
 * the given key type descriptor for the index's key column while keeping any
 * included columns as-is.
 */

/*
 * getSpGistTupleDesc：构建用于叶子元组的 TupleDesc，用给定的键类型描述符
 * 替换索引的键列，同时保持任何 included 列不变。
 */
extern TupleDesc getSpGistTupleDesc(Relation index, SpGistTypeDesc *keyType);

/*
 * initSpGistState: initialize an SpGistState for the given index by loading
 * its cache, copying config and type descriptors, and setting up build-time
 * fields ready for insert or search operations.
 */

/*
 * initSpGistState：为给定索引初始化一个 SpGistState，方法是加载其缓存、
 * 复制配置和类型描述符，并设置好构建期字段，以备插入或搜索操作使用。
 */
extern void initSpGistState(SpGistState *state, Relation index);

/*
 * SpGistNewBuffer: obtain a fresh buffer for the index, either by recycling a
 * previously freed page or by extending the relation, returning it locked and
 * ready for initialization.
 */

/*
 * SpGistNewBuffer：为索引获取一个新的缓冲区，方式是回收先前释放的页面，
 * 或者扩展关系文件，返回时该缓冲区已加锁并可供初始化。
 */
extern Buffer SpGistNewBuffer(Relation index);

/*
 * SpGistUpdateMetaPage: write the backend-local lastUsedPages cache back into
 * the shared index metapage so other backends can benefit from it.
 */

/*
 * SpGistUpdateMetaPage：将后端本地的 lastUsedPages 缓存写回到共享的索引
 * 元数据页面中，以便其他后端进程也能从中受益。
 */
extern void SpGistUpdateMetaPage(Relation index);

/*
 * SpGistGetBuffer: find or allocate a buffer of the requested kind (leaf,
 * inner-parity, or nulls) with at least needSpace bytes free, consulting the
 * last-used-page cache and setting *isNew when a brand-new page is returned.
 */

/*
 * SpGistGetBuffer：查找或分配一个所请求类型（叶子、内部奇偶性或 nulls）的
 * 缓冲区，其空闲空间至少为 needSpace 字节，过程中会参考最近使用页面缓存，
 * 并在返回一个全新页面时设置 *isNew。
 */
extern Buffer SpGistGetBuffer(Relation index, int flags,
							  int needSpace, bool *isNew);

/*
 * SpGistSetLastUsedPage: record the given buffer's block number and free space
 * in the backend's last-used-page cache for reuse by later allocations.
 */

/*
 * SpGistSetLastUsedPage：将给定缓冲区的块号和空闲空间记录到后端的最近
 * 使用页面缓存中，以供后续分配复用。
 */
extern void SpGistSetLastUsedPage(Relation index, Buffer buffer);

/*
 * SpGistInitPage: initialize a page as an empty SP-GiST page, setting up its
 * special space with the given flag bits and the page id.
 */

/*
 * SpGistInitPage：将一个页面初始化为空的 SP-GiST 页面，用给定的标志位和
 * 页面 id 设置其特殊空间。
 */
extern void SpGistInitPage(Page page, uint16 f);

/*
 * SpGistInitBuffer: initialize the page held in the given buffer as an empty
 * SP-GiST page with the specified flags.
 */

/*
 * SpGistInitBuffer：将给定缓冲区中保存的页面初始化为带有指定标志的空
 * SP-GiST 页面。
 */
extern void SpGistInitBuffer(Buffer b, uint16 f);

/*
 * SpGistInitMetapage: initialize the SP-GiST metapage, writing the magic
 * number and an empty last-used-pages cache.
 */

/*
 * SpGistInitMetapage：初始化 SP-GiST 元数据页面，写入魔数以及一个空的
 * 最近使用页面缓存。
 */
extern void SpGistInitMetapage(Page page);

/*
 * SpGistGetInnerTypeSize: compute the on-disk size needed to store the given
 * datum for an inner-tuple prefix or node label of the described type.
 */

/*
 * SpGistGetInnerTypeSize：计算存储给定 datum 所需的磁盘大小，该 datum 用作
 * 所描述类型的内部元组前缀或节点标签。
 */
extern unsigned int SpGistGetInnerTypeSize(SpGistTypeDesc *att, Datum datum);

/*
 * SpGistGetLeafTupleSize: compute the total size a leaf tuple would occupy for
 * the given key and included column datums, accounting for a nulls bitmap if
 * needed.
 */

/*
 * SpGistGetLeafTupleSize：计算一个叶子元组针对给定的键列及 included 列 datum
 * 所占用的总大小，必要时会计入空值位图。
 */
extern Size SpGistGetLeafTupleSize(TupleDesc tupleDescriptor,
								   const Datum *datums, const bool *isnulls);

/*
 * spgFormLeafTuple: build a new leaf tuple in palloc'd memory from the given
 * heap TID and key/included datums, laying out the header, optional nulls
 * bitmap, and datum payload.
 */

/*
 * spgFormLeafTuple：根据给定的堆 TID 和键列/included 列 datum，在 palloc
 * 分配的内存中构建一个新的叶子元组，布局其头部、可选的空值位图和 datum
 * 负载数据。
 */
extern SpGistLeafTuple spgFormLeafTuple(SpGistState *state,
										ItemPointer heapPtr,
										const Datum *datums, const bool *isnulls);

/*
 * spgFormNodeTuple: build a node tuple (as used inside an inner tuple) holding
 * the given label datum, or a null label if isnull is true.
 */

/*
 * spgFormNodeTuple：构建一个节点元组（用于内部元组内部），其中保存给定的
 * 标签 datum；若 isnull 为 true，则保存一个空标签。
 */
extern SpGistNodeTuple spgFormNodeTuple(SpGistState *state,
										Datum label, bool isnull);

/*
 * spgFormInnerTuple: assemble an inner tuple from an optional prefix and an
 * array of node tuples, computing and validating its total size against the
 * SP-GiST limits.
 */

/*
 * spgFormInnerTuple：由一个可选的前缀和一个节点元组数组组装出一个内部
 * 元组，计算其总大小并对照 SP-GiST 的各项限制进行校验。
 */
extern SpGistInnerTuple spgFormInnerTuple(SpGistState *state,
										  bool hasPrefix, Datum prefix,
										  int nNodes, SpGistNodeTuple *nodes);

/*
 * spgFormDeadTuple: construct a dead tuple of the given state (REDIRECT, DEAD,
 * or PLACEHOLDER), recording the redirection target block/offset and current
 * XID as appropriate.
 */

/*
 * spgFormDeadTuple：构建一个具有给定状态（REDIRECT、DEAD 或 PLACEHOLDER）
 * 的死亡元组，并酌情记录重定向目标的块号/偏移以及当前 XID。
 */
extern SpGistDeadTuple spgFormDeadTuple(SpGistState *state, int tupstate,
										BlockNumber blkno, OffsetNumber offnum);

/*
 * spgDeformLeafTuple: extract the key and included column datums (and their
 * null flags) from a stored leaf tuple, handling the case where the key column
 * is known to be null.
 */

/*
 * spgDeformLeafTuple：从一个存储的叶子元组中提取键列和 included 列的 datum
 *（及其空值标志），并处理已知键列为空的情况。
 */
extern void spgDeformLeafTuple(SpGistLeafTuple tup, TupleDesc tupleDescriptor,
							   Datum *datums, bool *isnulls,
							   bool keyColumnIsNull);

/*
 * spgExtractNodeLabels: return an array of the node label datums from an inner
 * tuple, or NULL if the tuple's nodes carry no labels.
 */

/*
 * spgExtractNodeLabels：返回一个内部元组中各节点标签 datum 的数组，若该
 * 元组的节点不携带标签则返回 NULL。
 */
extern Datum *spgExtractNodeLabels(SpGistState *state,
								   SpGistInnerTuple innerTuple);

/*
 * SpGistPageAddNewItem: add an item to a page, reusing a placeholder line
 * pointer when possible (scanning from *startOffset), and return the offset at
 * which the item was placed.
 */

/*
 * SpGistPageAddNewItem：向页面添加一个项，尽可能复用 placeholder 行指针
 *（从 *startOffset 处开始扫描），并返回该项被放置的偏移位置。
 */
extern OffsetNumber SpGistPageAddNewItem(SpGistState *state, Page page,
										 Item item, Size size,
										 OffsetNumber *startOffset,
										 bool errorOK);

/*
 * spgproperty: implement the amproperty callback for SP-GiST, answering
 * index-property inquiries (such as distance-ordering support) for the given
 * index and attribute.
 */

/*
 * spgproperty：实现 SP-GiST 的 amproperty 回调，针对给定索引和属性回答
 * 索引属性查询（例如是否支持按距离排序）。
 */
extern bool spgproperty(Oid index_oid, int attno,
						IndexAMProperty prop, const char *propname,
						bool *res, bool *isnull);

/* spgdoinsert.c */

/* 以下函数来自 spgdoinsert.c */

/*
 * spgUpdateNodeLink: set the downlink (block number and offset) of the given
 * node within an inner tuple, so it points at its child page/tuple.
 */

/*
 * spgUpdateNodeLink：设置一个内部元组中给定节点的下行链接（块号和偏移），
 * 使其指向对应的子页面/子元组。
 */
extern void spgUpdateNodeLink(SpGistInnerTuple tup, int nodeN,
							  BlockNumber blkno, OffsetNumber offset);

/*
 * spgPageIndexMultiDelete: delete or convert multiple tuples on a page in one
 * pass, applying firststate to the first affected tuple and reststate to the
 * others (e.g. turning tuples into placeholders or dead/redirect tuples).
 */

/*
 * spgPageIndexMultiDelete：一次性删除或转换页面上的多个元组，对第一个
 * 受影响的元组应用 firststate，对其余元组应用 reststate（例如将元组变为
 * placeholder，或变为 dead/redirect 元组）。
 */
extern void spgPageIndexMultiDelete(SpGistState *state, Page page,
									OffsetNumber *itemnos, int nitems,
									int firststate, int reststate,
									BlockNumber blkno, OffsetNumber offnum);

/*
 * spgdoinsert: the core routine that inserts one indexed value into the
 * SP-GiST tree, descending from the root and invoking the opclass choose
 * method to add leaves, add nodes, split tuples, or pick-split as needed.
 */

/*
 * spgdoinsert：将一个被索引值插入 SP-GiST 树的核心例程，它从根开始向下
 * 遍历，并调用操作符类的 choose 方法，按需执行添加叶子、添加节点、分裂
 * 元组或 pick-split 等操作。
 */
extern bool spgdoinsert(Relation index, SpGistState *state,
						ItemPointer heapPtr, Datum *datums, bool *isnulls);

/* spgproc.c */

/* 以下函数来自 spgproc.c */

/*
 * spg_key_orderbys_distances: compute the array of distances between the given
 * key (leaf or inner) and each ordering operator's comparison value, used to
 * drive nearest-neighbor ordered scans.
 */

/*
 * spg_key_orderbys_distances：计算给定键（叶子或内部）与每个排序操作符的
 * 比较值之间的距离数组，用于驱动最近邻的有序扫描。
 */
extern double *spg_key_orderbys_distances(Datum key, bool isLeaf,
										  ScanKey orderbys, int norderbys);

/*
 * box_copy: return a palloc'd copy of the given BOX, a small helper used by
 * the geometric SP-GiST support routines.
 */

/*
 * box_copy：返回给定 BOX 的一份 palloc 分配的副本，是几何类 SP-GiST 支持
 * 例程所使用的一个小型辅助函数。
 */
extern BOX *box_copy(BOX *orig);

#endif							/* SPGIST_PRIVATE_H */

/*
 * brin_internal.h
 *		internal declarations for BRIN indexes
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/include/access/brin_internal.h
 */
#ifndef BRIN_INTERNAL_H
#define BRIN_INTERNAL_H

#include "access/amapi.h"
#include "storage/bufpage.h"
#include "utils/typcache.h"


/*
 * A BrinDesc is a struct designed to enable decoding a BRIN tuple from the
 * on-disk format to an in-memory tuple and vice-versa.
 */

/*
 * BrinDesc 是一个结构体，用于实现将 BRIN 元组从磁盘格式解码为内存中的元组，
 * 反之亦然。
 */

/* struct returned by "OpcInfo" amproc */

/* 由 “OpcInfo” 支持过程（amproc）返回的结构体 */
typedef struct BrinOpcInfo
{
	/* Number of columns stored in an index column of this opclass */

	/* 该运算符类在一个索引列中所存储的列数 */
	uint16		oi_nstored;

	/* Regular processing of NULLs in BrinValues? */

	/* 是否对 BrinValues 中的 NULL 进行常规处理？ */
	bool		oi_regular_nulls;

	/* Opaque pointer for the opclass' private use */

	/* 供运算符类私有使用的不透明指针 */
	void	   *oi_opaque;

	/* Type cache entries of the stored columns */

	/* 所存储各列的类型缓存（type cache）条目 */
	TypeCacheEntry *oi_typcache[FLEXIBLE_ARRAY_MEMBER];
} BrinOpcInfo;

/* the size of a BrinOpcInfo for the given number of columns */

/* 给定列数时一个 BrinOpcInfo 的大小 */
#define SizeofBrinOpcInfo(ncols) \
	(offsetof(BrinOpcInfo, oi_typcache) + sizeof(TypeCacheEntry *) * ncols)

typedef struct BrinDesc
{
	/* Containing memory context */

	/* 所属的内存上下文（memory context） */
	MemoryContext bd_context;

	/* the index relation itself */

	/* 索引关系（relation）本身 */
	Relation	bd_index;

	/* tuple descriptor of the index relation */

	/* 索引关系的元组描述符（tuple descriptor） */
	TupleDesc	bd_tupdesc;

	/* cached copy for on-disk tuples; generated at first use */

	/* 用于磁盘上元组的缓存副本；在首次使用时生成 */
	TupleDesc	bd_disktdesc;

	/* total number of Datum entries that are stored on-disk for all columns */

	/* 所有列在磁盘上存储的 Datum 条目总数 */
	int			bd_totalstored;

	/* per-column info; bd_tupdesc->natts entries long */

	/* 每列的信息；长度为 bd_tupdesc->natts 个条目 */
	BrinOpcInfo *bd_info[FLEXIBLE_ARRAY_MEMBER];
} BrinDesc;

/*
 * Globally-known function support numbers for BRIN indexes.  Individual
 * opclasses can define more function support numbers, which must fall into
 * BRIN_FIRST_OPTIONAL_PROCNUM .. BRIN_LAST_OPTIONAL_PROCNUM.
 */

/*
 * BRIN 索引全局已知的函数支持号（support number）。各个运算符类可以定义更多的
 * 函数支持号，但这些号必须落在 BRIN_FIRST_OPTIONAL_PROCNUM .. BRIN_LAST_OPTIONAL_PROCNUM
 * 的范围内。
 */
#define BRIN_PROCNUM_OPCINFO		1
#define BRIN_PROCNUM_ADDVALUE		2
#define BRIN_PROCNUM_CONSISTENT		3
#define BRIN_PROCNUM_UNION			4
#define BRIN_MANDATORY_NPROCS		4
#define BRIN_PROCNUM_OPTIONS 		5	/* optional */

/* 可选的 */
/* procedure numbers up to 10 are reserved for BRIN future expansion */

/* 编号直到 10 为止的过程号（procedure number）保留给 BRIN 未来扩展使用 */
#define BRIN_FIRST_OPTIONAL_PROCNUM 11
#define BRIN_LAST_OPTIONAL_PROCNUM	15

#undef BRIN_DEBUG

#ifdef BRIN_DEBUG
#define BRIN_elog(args)			elog args
#else
#define BRIN_elog(args)			((void) 0)
#endif

/* brin.c */

/* brin.c */

/*
 * brin_build_desc
 *		Build an in-memory BrinDesc for the given index relation by looking
 *		up each indexed column's opclass "OpcInfo" support proc, so that BRIN
 *		tuples for this index can later be encoded and decoded.
 */

/*
 * brin_build_desc
 *		为给定的索引关系构建一个内存中的 BrinDesc，方法是查找每个被索引列
 *		运算符类的 “OpcInfo” 支持过程，以便之后能够对该索引的 BRIN 元组进行
 *		编码和解码。
 */
extern BrinDesc *brin_build_desc(Relation rel);

/*
 * brin_free_desc
 *		Release the memory associated with a BrinDesc previously created by
 *		brin_build_desc.
 */

/*
 * brin_free_desc
 *		释放与之前由 brin_build_desc 创建的 BrinDesc 相关联的内存。
 */
extern void brin_free_desc(BrinDesc *bdesc);

/*
 * brinbuild
 *		The BRIN "ambuild" callback: build a brand-new BRIN index over the
 *		given heap by scanning it, summarizing each page range and writing the
 *		resulting index tuples, then return build statistics.
 */

/*
 * brinbuild
 *		BRIN 的 “ambuild” 回调：通过扫描给定的堆来构建一个全新的 BRIN 索引，
 *		对每个页面范围进行汇总并写出相应的索引元组，然后返回构建统计信息。
 */
extern IndexBuildResult *brinbuild(Relation heap, Relation index,
								   struct IndexInfo *indexInfo);

/*
 * brinbuildempty
 *		The BRIN "ambuildempty" callback: initialize an empty BRIN index
 *		(metapage and initial revmap) in the init fork for an unlogged index.
 */

/*
 * brinbuildempty
 *		BRIN 的 “ambuildempty” 回调：为无日志（unlogged）索引在 init fork 中
 *		初始化一个空的 BRIN 索引（元页面和初始反向映射）。
 */
extern void brinbuildempty(Relation index);

/*
 * brininsert
 *		The BRIN "aminsert" callback: on inserting a new heap tuple, update the
 *		summary of the page range that contains it, extending the summary's
 *		value bounds if the new values fall outside them.
 */

/*
 * brininsert
 *		BRIN 的 “aminsert” 回调：在插入一个新的堆元组时，更新包含该元组的页面
 *		范围的汇总信息；如果新值落在汇总的取值边界之外，则相应地扩展这些边界。
 */
extern bool brininsert(Relation idxRel, Datum *values, bool *nulls,
					   ItemPointer heaptid, Relation heapRel,
					   IndexUniqueCheck checkUnique,
					   bool indexUnchanged,
					   struct IndexInfo *indexInfo);

/*
 * brininsertcleanup
 *		The BRIN "aminsertcleanup" callback: release any per-statement state
 *		(such as cached BrinDesc/revmap) accumulated during a series of
 *		brininsert calls.
 */

/*
 * brininsertcleanup
 *		BRIN 的 “aminsertcleanup” 回调：释放在一系列 brininsert 调用过程中
 *		累积的、按语句级别保存的状态（例如缓存的 BrinDesc/反向映射）。
 */
extern void brininsertcleanup(Relation index, struct IndexInfo *indexInfo);

/*
 * brinbeginscan
 *		The BRIN "ambeginscan" callback: allocate and initialize an
 *		IndexScanDesc together with BRIN-specific scan opaque state for a new
 *		bitmap index scan.
 */

/*
 * brinbeginscan
 *		BRIN 的 “ambeginscan” 回调：为一次新的位图索引扫描分配并初始化一个
 *		IndexScanDesc，以及 BRIN 特有的扫描不透明状态。
 */
extern IndexScanDesc brinbeginscan(Relation r, int nkeys, int norderbys);

/*
 * bringetbitmap
 *		The BRIN "amgetbitmap" callback: scan the revmap and range summaries,
 *		and for every page range whose summary is consistent with the scan
 *		keys, add all of its heap blocks to the output TIDBitmap; return the
 *		number of blocks added.
 */

/*
 * bringetbitmap
 *		BRIN 的 “amgetbitmap” 回调：扫描反向映射和各范围汇总信息，对于每个其
 *		汇总与扫描键一致的页面范围，将其所有堆块加入到输出的 TIDBitmap 中；
 *		返回加入的块数。
 */
extern int64 bringetbitmap(IndexScanDesc scan, TIDBitmap *tbm);

/*
 * brinrescan
 *		The BRIN "amrescan" callback: reset an existing scan so that it can be
 *		restarted with a new set of scan keys.
 */

/*
 * brinrescan
 *		BRIN 的 “amrescan” 回调：重置一个已有的扫描，使其能够以一组新的扫描键
 *		重新开始。
 */
extern void brinrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
					   ScanKey orderbys, int norderbys);

/*
 * brinendscan
 *		The BRIN "amendscan" callback: release the resources held by a BRIN
 *		index scan.
 */

/*
 * brinendscan
 *		BRIN 的 “amendscan” 回调：释放一次 BRIN 索引扫描所持有的资源。
 */
extern void brinendscan(IndexScanDesc scan);

/*
 * brinbulkdelete
 *		The BRIN "ambulkdelete" callback.  BRIN indexes do not point at
 *		individual heap tuples, so no tuples are actually removed here; it
 *		mainly exists to satisfy the AM interface and return statistics.
 */

/*
 * brinbulkdelete
 *		BRIN 的 “ambulkdelete” 回调。BRIN 索引并不指向单个堆元组，因此这里实际上
 *		不会删除任何元组；它主要用于满足访问方法（AM）接口并返回统计信息。
 */
extern IndexBulkDeleteResult *brinbulkdelete(IndexVacuumInfo *info,
											 IndexBulkDeleteResult *stats,
											 IndexBulkDeleteCallback callback,
											 void *callback_state);

/*
 * brinvacuumcleanup
 *		The BRIN "amvacuumcleanup" callback: after VACUUM, summarize any
 *		unsummarized page ranges as needed and return final index statistics.
 */

/*
 * brinvacuumcleanup
 *		BRIN 的 “amvacuumcleanup” 回调：在 VACUUM 之后，按需对尚未汇总的页面
 *		范围进行汇总，并返回最终的索引统计信息。
 */
extern IndexBulkDeleteResult *brinvacuumcleanup(IndexVacuumInfo *info,
												IndexBulkDeleteResult *stats);

/*
 * brinoptions
 *		The BRIN "amoptions" callback: parse and validate the reloptions
 *		(such as pages_per_range and autosummarize) for a BRIN index.
 */

/*
 * brinoptions
 *		BRIN 的 “amoptions” 回调：解析并校验 BRIN 索引的关系选项（reloptions），
 *		例如 pages_per_range 和 autosummarize。
 */
extern bytea *brinoptions(Datum reloptions, bool validate);

/* brin_validate.c */

/* brin_validate.c */

/*
 * brinvalidate
 *		The BRIN "amvalidate" callback: check that the given opclass has a
 *		complete and consistent set of operators and support functions,
 *		reporting problems via warnings.
 */

/*
 * brinvalidate
 *		BRIN 的 “amvalidate” 回调：检查给定的运算符类是否具备完整且一致的
 *		运算符和支持函数集合，并通过警告报告发现的问题。
 */
extern bool brinvalidate(Oid opclassoid);

#endif							/* BRIN_INTERNAL_H */

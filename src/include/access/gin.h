/*--------------------------------------------------------------------------
 * gin.h
 *	  Public header file for Generalized Inverted Index access method.
 *
 *	Copyright (c) 2006-2025, PostgreSQL Global Development Group
 *
 *	src/include/access/gin.h
 *--------------------------------------------------------------------------
 */
#ifndef GIN_H
#define GIN_H

#include "access/xlogreader.h"
#include "lib/stringinfo.h"
#include "nodes/execnodes.h"
#include "storage/shm_toc.h"
#include "storage/block.h"
#include "utils/relcache.h"


/*
 * amproc indexes for inverted indexes.
 */

/*
 * 倒排索引使用的 amproc（访问方法支持过程）索引编号。
 */
#define GIN_COMPARE_PROC			   1
#define GIN_EXTRACTVALUE_PROC		   2
#define GIN_EXTRACTQUERY_PROC		   3
#define GIN_CONSISTENT_PROC			   4
#define GIN_COMPARE_PARTIAL_PROC	   5
#define GIN_TRICONSISTENT_PROC		   6
#define GIN_OPTIONS_PROC	   7
#define GINNProcs					   7

/*
 * searchMode settings for extractQueryFn.
 */

/*
 * extractQueryFn 使用的 searchMode（搜索模式）设置。
 */
#define GIN_SEARCH_MODE_DEFAULT			0
#define GIN_SEARCH_MODE_INCLUDE_EMPTY	1
#define GIN_SEARCH_MODE_ALL				2
#define GIN_SEARCH_MODE_EVERYTHING		3	/* for internal use only */

/* 仅供内部使用 */

/*
 * Constant definition for progress reporting.  Phase numbers must match
 * ginbuildphasename.
 */

/*
 * 用于进度报告的常量定义。阶段编号必须与 ginbuildphasename 保持一致。
 */
/* PROGRESS_CREATEIDX_SUBPHASE_INITIALIZE is 1 (see progress.h) */

/* PROGRESS_CREATEIDX_SUBPHASE_INITIALIZE 为 1（参见 progress.h） */
#define PROGRESS_GIN_PHASE_INDEXBUILD_TABLESCAN		2
#define PROGRESS_GIN_PHASE_PERFORMSORT_1			3
#define PROGRESS_GIN_PHASE_MERGE_1					4
#define PROGRESS_GIN_PHASE_PERFORMSORT_2			5
#define PROGRESS_GIN_PHASE_MERGE_2					6

/*
 * GinStatsData represents stats data for planner use
 */

/*
 * GinStatsData 表示供规划器使用的统计数据。
 */
typedef struct GinStatsData
{
	BlockNumber nPendingPages;
	BlockNumber nTotalPages;
	BlockNumber nEntryPages;
	BlockNumber nDataPages;
	int64		nEntries;
	int32		ginVersion;
} GinStatsData;

/*
 * A ternary value used by tri-consistent functions.
 *
 * This must be of the same size as a bool because some code will cast a
 * pointer to a bool to a pointer to a GinTernaryValue.
 */

/*
 * 由三值一致性（tri-consistent）函数使用的三态值。
 *
 * 它的大小必须与 bool 相同，因为某些代码会将指向 bool 的指针
 * 强制转换为指向 GinTernaryValue 的指针。
 */
typedef char GinTernaryValue;

StaticAssertDecl(sizeof(GinTernaryValue) == sizeof(bool),
				 "sizes of GinTernaryValue and bool are not equal");

#define GIN_FALSE		0		/* item is not present / does not match */

/* 项不存在 / 不匹配 */
#define GIN_TRUE		1		/* item is present / matches */

/* 项存在 / 匹配 */
#define GIN_MAYBE		2		/* don't know if item is present / don't know
								 * if matches */

/* 不确定项是否存在 / 不确定是否匹配 */

/*
 * Convert a Datum into a GinTernaryValue by casting the raw value.  Used to
 * unwrap the ternary result carried through the Datum ABI back into its
 * native char-based ternary representation.
 *
 * 通过对原始值进行强制转换，将 Datum 转换为 GinTernaryValue。
 * 用于把通过 Datum ABI 传递的三态结果还原为其原生的、
 * 基于 char 的三态表示形式。
 */
static inline GinTernaryValue
DatumGetGinTernaryValue(Datum X)
{
	return (GinTernaryValue) X;
}

/*
 * Convert a GinTernaryValue into a Datum by casting the raw value.  Used to
 * wrap a ternary result into the Datum ABI so it can be returned from a
 * function like an ordinary value.
 *
 * 通过对原始值进行强制转换，将 GinTernaryValue 转换为 Datum。
 * 用于把三态结果包装进 Datum ABI，从而能像普通值一样
 * 从函数中返回。
 */
static inline Datum
GinTernaryValueGetDatum(GinTernaryValue X)
{
	return (Datum) X;
}

#define PG_RETURN_GIN_TERNARY_VALUE(x) return GinTernaryValueGetDatum(x)

/* GUC parameters */

/* GUC 参数 */

/*
 * GUC that bounds the number of entries a fuzzy (partial-match) GIN search
 * will examine, trading result completeness for bounded runtime.
 *
 * 该 GUC 参数限制模糊（部分匹配）GIN 搜索所检查的条目数量，
 * 以结果完整性换取有界的运行时间。
 */
extern PGDLLIMPORT int GinFuzzySearchLimit;

/*
 * GUC that sets the default maximum size of the pending list used by GIN's
 * fast-update mechanism before it is flushed into the main index.
 *
 * 该 GUC 参数设置 GIN 快速更新机制所用挂起列表（pending list）
 * 在被刷入主索引之前的默认最大大小。
 */
extern PGDLLIMPORT int gin_pending_list_limit;

/* ginutil.c */

/* ginutil.c */

/*
 * Read accumulated statistics (page counts, entry count, version) from a GIN
 * index's metapage into the caller-provided GinStatsData for planner use.
 *
 * 从 GIN 索引的元页读取累积统计信息（各类页数、条目数、版本号），
 * 填入调用者提供的 GinStatsData 中，供规划器使用。
 */
extern void ginGetStats(Relation index, GinStatsData *stats);

/*
 * Write updated statistics back into a GIN index's metapage, WAL-logging the
 * change; the is_build flag indicates whether this happens during index build.
 *
 * 将更新后的统计信息写回 GIN 索引的元页，并对该更改记录 WAL；
 * is_build 标志指示此操作是否发生在索引构建期间。
 */
extern void ginUpdateStats(Relation index, const GinStatsData *stats,
						   bool is_build);

/*
 * Entry point executed by each parallel worker during a parallel GIN index
 * build; it attaches to the shared state described by the DSM segment and toc
 * and performs its share of scanning and sorting work.
 *
 * 并行 GIN 索引构建期间由每个并行工作进程执行的入口点；
 * 它会附加到由 DSM 段和 toc 所描述的共享状态，
 * 并完成属于自己的扫描与排序工作。
 */
extern void _gin_parallel_build_main(dsm_segment *seg, shm_toc *toc);

#endif							/* GIN_H */

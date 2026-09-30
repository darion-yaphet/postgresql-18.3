/*
 * AM-callable functions for BRIN indexes
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/include/access/brin.h
 */
#ifndef BRIN_H
#define BRIN_H

#include "nodes/execnodes.h"
#include "storage/shm_toc.h"
#include "utils/relcache.h"


/*
 * Storage type for BRIN's reloptions
 */

/*
 * BRIN reloptions 的存储类型
 */
typedef struct BrinOptions
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */

	/* varlena 头部（不要直接操作！） */
	BlockNumber pagesPerRange;
	bool		autosummarize;
} BrinOptions;


/*
 * BrinStatsData represents stats data for planner use
 */

/*
 * BrinStatsData 表示供规划器使用的统计数据
 */
typedef struct BrinStatsData
{
	BlockNumber pagesPerRange;
	BlockNumber revmapNumPages;
} BrinStatsData;


#define BRIN_DEFAULT_PAGES_PER_RANGE	128
#define BrinGetPagesPerRange(relation) \
	(AssertMacro(relation->rd_rel->relkind == RELKIND_INDEX && \
				 relation->rd_rel->relam == BRIN_AM_OID), \
	 (relation)->rd_options ? \
	 ((BrinOptions *) (relation)->rd_options)->pagesPerRange : \
	  BRIN_DEFAULT_PAGES_PER_RANGE)
#define BrinGetAutoSummarize(relation) \
	(AssertMacro(relation->rd_rel->relkind == RELKIND_INDEX && \
				 relation->rd_rel->relam == BRIN_AM_OID), \
	 (relation)->rd_options ? \
	 ((BrinOptions *) (relation)->rd_options)->autosummarize : \
	  false)


/*
 * Retrieve BRIN index statistics (pages per range and number of revmap
 * pages) from the index's metapage, populating the given BrinStatsData for
 * use by the query planner.
 *
 * 从索引的元页中获取 BRIN 索引统计信息（每个范围的页数以及
 * revmap 页数），填充给定的 BrinStatsData 以供查询规划器使用。
 */
extern void brinGetStats(Relation index, BrinStatsData *stats);

/*
 * Entry point executed by each parallel worker participating in a BRIN
 * index build; it attaches to the shared memory segment and table-of-
 * contents, then performs its share of the parallel summarization work.
 *
 * 参与 BRIN 索引构建的每个并行工作进程所执行的入口点；它会附加到
 * 共享内存段和内容目录（table-of-contents），然后执行其负责的那部分
 * 并行汇总（summarization）工作。
 */
extern void _brin_parallel_build_main(dsm_segment *seg, shm_toc *toc);

#endif							/* BRIN_H */

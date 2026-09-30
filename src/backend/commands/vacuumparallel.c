/*-------------------------------------------------------------------------
 *
 * vacuumparallel.c
 *	  Support routines for parallel vacuum execution.
 *
 * 并行 vacuum 执行的支持例程。
 *
 * This file contains routines that are intended to support setting up, using,
 * and tearing down a ParallelVacuumState.
 *
 * 本文件包含建立、使用和拆除 ParallelVacuumState 的例程。
 *
 * In a parallel vacuum, we perform both index bulk deletion and index cleanup
 * with parallel worker processes.  Individual indexes are processed by one
 * vacuum process.  ParallelVacuumState contains shared information as well as
 * the memory space for storing dead items allocated in the DSA area.  We
 * launch parallel worker processes at the start of parallel index
 * bulk-deletion and index cleanup and once all indexes are processed, the
 * parallel worker processes exit.  Each time we process indexes in parallel,
 * the parallel context is re-initialized so that the same DSM can be used for
 * multiple passes of index bulk-deletion and index cleanup.
 *
 * 并行 vacuum 用并行 worker 做索引批量删除和索引清理。每个索引由一个 vacuum 进程处理。
 * ParallelVacuumState 含共享信息，以及在 DSA 中为死元组分配的空间。
 * 并行索引批量删除和清理开始时启动 worker，全部索引处理完后 worker 退出。
 * 每次并行处理索引都会重新初始化并行上下文，以便同一 DSM 用于多遍批量删除和清理。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/commands/vacuumparallel.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amapi.h"
#include "access/table.h"
#include "access/xact.h"
#include "commands/progress.h"
#include "commands/vacuum.h"
#include "executor/instrument.h"
#include "optimizer/paths.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "tcop/tcopprot.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

/*
 * 核心流程概览：
 * parallel_vacuum_init 进入并行模式，估算并创建 DSM 中的 ParallelVacuumState
 *（共享状态、死元组 TidStore、索引统计）。
 * parallel_vacuum_bulkdel_all_indexes / parallel_vacuum_cleanup_all_indexes
 * 由 leader 启动 worker；双方经 parallel_vacuum_process_one_index 处理各自索引。
 * parallel_vacuum_main 是 worker 入口；parallel_vacuum_end 把统计拷回本地并结束并行模式。
 */
/*
 * DSM keys for parallel vacuum.  Unlike other parallel execution code, since
 * we don't need to worry about DSM keys conflicting with plan_node_id we can
 * use small integers.
 *
 * 并行 vacuum 的 DSM 键。与其他并行执行不同，不必担心与 plan_node_id 冲突，因此可以用小整数。
 */
#define PARALLEL_VACUUM_KEY_SHARED			1
#define PARALLEL_VACUUM_KEY_QUERY_TEXT		2
#define PARALLEL_VACUUM_KEY_BUFFER_USAGE	3
#define PARALLEL_VACUUM_KEY_WAL_USAGE		4
#define PARALLEL_VACUUM_KEY_INDEX_STATS		5

/*
 * Shared information among parallel workers.  So this is allocated in the DSM
 * segment.
 *
 * 并行 worker 之间的共享信息，因此分配在 DSM 段中。
 */
typedef struct PVShared
{
	/*
	 * Target table relid, log level (for messages about parallel workers
	 * launched during VACUUM VERBOSE) and query ID.  These fields are not
	 * modified during the parallel vacuum.
	 *
	 * 目标表 relid、日志级别（VACUUM VERBOSE 时关于已启动并行 worker 的消息）和查询 ID。
	 * 并行 vacuum 期间这些字段不会被修改。
	 */
	Oid			relid;
	int			elevel;
	int64		queryid;

	/*
	 * Fields for both index vacuum and cleanup.
	 *
	 * 索引 vacuum 和清理共用的字段。
	 *
	 * reltuples is the total number of input heap tuples.  We set either old
	 * live tuples in the index vacuum case or the new live tuples in the
	 * index cleanup case.
	 *
	 * reltuples 是输入堆元组总数。索引 vacuum 时设为旧的活元组数，索引清理时设为新的活元组数。
	 *
	 * estimated_count is true if reltuples is an estimated value.  (Note that
	 * reltuples could be -1 in this case, indicating we have no idea.)
	 *
	 * 若 reltuples 是估计值则 estimated_count 为真。（此时 reltuples 可能为 -1，表示完全不知道。）
	 */
	double		reltuples;
	bool		estimated_count;

	/*
	 * In single process vacuum we could consume more memory during index
	 * vacuuming or cleanup apart from the memory for heap scanning.  In
	 * parallel vacuum, since individual vacuum workers can consume memory
	 * equal to maintenance_work_mem, the new maintenance_work_mem for each
	 * worker is set such that the parallel operation doesn't consume more
	 * memory than single process vacuum.
	 *
	 * 单进程 vacuum 在堆扫描之外，索引 vacuum 或清理还可能多用内存。
	 * 并行时每个 worker 都可能用掉一份 maintenance_work_mem，因此要下调每个 worker 的该值，
	 * 使并行操作的总内存不超过单进程 vacuum。
	 */
	int			maintenance_work_mem_worker;

	/*
	 * The number of buffers each worker's Buffer Access Strategy ring should
	 * contain.
	 *
	 * 每个 worker 的 Buffer Access Strategy 环应包含的缓冲区数量。
	 */
	int			ring_nbuffers;

	/*
	 * Shared vacuum cost balance.  During parallel vacuum,
	 * VacuumSharedCostBalance points to this value and it accumulates the
	 * balance of each parallel vacuum worker.
	 *
	 * 共享的 vacuum 代价余额。并行 vacuum 期间 VacuumSharedCostBalance 指向该值，并累加各 worker 的余额。
	 */
	pg_atomic_uint32 cost_balance;

	/*
	 * Number of active parallel workers.  This is used for computing the
	 * minimum threshold of the vacuum cost balance before a worker sleeps for
	 * cost-based delay.
	 *
	 * 活跃并行 worker 数。用于计算 worker 因基于代价的延迟而睡眠之前，vacuum 代价余额的最小阈值。
	 */
	pg_atomic_uint32 active_nworkers;

	/* Counter for vacuuming and cleanup */
	/*
	 *
	 * vacuum 与清理的计数器。
	 */
	pg_atomic_uint32 idx;

	/* DSA handle where the TidStore lives */
	/*
	 *
	 * TidStore 所在的 DSA 句柄。
	 */
	dsa_handle	dead_items_dsa_handle;

	/* DSA pointer to the shared TidStore */
	/*
	 *
	 * 指向共享 TidStore 的 DSA 指针。
	 */
	dsa_pointer dead_items_handle;

	/* Statistics of shared dead items */
	/*
	 *
	 * 共享死元组的统计。
	 */
	VacDeadItemsInfo dead_items_info;
} PVShared;

/* Status used during parallel index vacuum or cleanup */
/*
 *
 * 并行索引 vacuum 或清理期间使用的状态。
 */
typedef enum PVIndVacStatus
{
	PARALLEL_INDVAC_STATUS_INITIAL = 0,
	PARALLEL_INDVAC_STATUS_NEED_BULKDELETE,
	PARALLEL_INDVAC_STATUS_NEED_CLEANUP,
	PARALLEL_INDVAC_STATUS_COMPLETED,
} PVIndVacStatus;

/*
 * Struct for index vacuum statistics of an index that is used for parallel vacuum.
 * This includes the status of parallel index vacuum as well as index statistics.
 *
 * 并行 vacuum 所用索引的索引 vacuum 统计结构。包含并行索引 vacuum 的状态以及索引统计。
 */
typedef struct PVIndStats
{
	/*
	 * The following two fields are set by leader process before executing
	 * parallel index vacuum or parallel index cleanup.  These fields are not
	 * fixed for the entire VACUUM operation.  They are only fixed for an
	 * individual parallel index vacuum and cleanup.
	 *
	 * 下面两个字段由 leader 在执行并行索引 vacuum 或清理之前设置。
	 * 它们不是整个 VACUUM 期间固定的，只对单次并行索引 vacuum 和清理固定。
	 *
	 * parallel_workers_can_process is true if both leader and worker can
	 * process the index, otherwise only leader can process it.
	 *
	 * 若 leader 和 worker 都能处理该索引，则 parallel_workers_can_process 为真，否则只有 leader 能处理。
	 */
	PVIndVacStatus status;
	bool		parallel_workers_can_process;

	/*
	 * Individual worker or leader stores the result of index vacuum or
	 * cleanup.
	 *
	 * 各个 worker 或 leader 存放索引 vacuum 或清理的结果。
	 */
	bool		istat_updated;	/* are the stats updated? */
	/*
	 *
	 * 统计是否已更新？
	 */
	IndexBulkDeleteResult istat;
} PVIndStats;

/*
 * Struct for maintaining a parallel vacuum state. typedef appears in vacuum.h.
 *
 * 维护并行 vacuum 状态的结构。typedef 出现在 vacuum.h 中。
 */
struct ParallelVacuumState
{
	/* NULL for worker processes */
	/*
	 *
	 * worker 进程中为 NULL。
	 */
	ParallelContext *pcxt;

	/* Parent Heap Relation */
	/*
	 *
	 * 父堆关系。
	 */
	Relation	heaprel;

	/* Target indexes */
	/*
	 *
	 * 目标索引。
	 */
	Relation   *indrels;
	int			nindexes;

	/* Shared information among parallel vacuum workers */
	/*
	 *
	 * 并行 vacuum worker 之间的共享信息。
	 */
	PVShared   *shared;

	/*
	 * Shared index statistics among parallel vacuum workers. The array
	 * element is allocated for every index, even those indexes where parallel
	 * index vacuuming is unsafe or not worthwhile (e.g.,
	 * will_parallel_vacuum[] is false).  During parallel vacuum,
	 * IndexBulkDeleteResult of each index is kept in DSM and is copied into
	 * local memory at the end of parallel vacuum.
	 *
	 * 并行 vacuum worker 共享的索引统计。每个索引都分配数组元素，即使并行索引 vacuum 不安全或不值得
	 * （例如 will_parallel_vacuum[] 为 false）。并行期间每个索引的 IndexBulkDeleteResult 放在 DSM 中，
	 * 结束时复制到本地内存。
	 */
	PVIndStats *indstats;

	/* Shared dead items space among parallel vacuum workers */
	/*
	 *
	 * 并行 vacuum worker 共享的死元组空间。
	 */
	TidStore   *dead_items;

	/* Points to buffer usage area in DSM */
	/*
	 *
	 * 指向 DSM 中的缓冲区使用量区域。
	 */
	BufferUsage *buffer_usage;

	/* Points to WAL usage area in DSM */
	/*
	 *
	 * 指向 DSM 中的 WAL 使用量区域。
	 */
	WalUsage   *wal_usage;

	/*
	 * False if the index is totally unsuitable target for all parallel
	 * processing. For example, the index could be <
	 * min_parallel_index_scan_size cutoff.
	 *
	 * 若该索引完全不适合任何并行处理则为 false。例如索引小于 min_parallel_index_scan_size 阈值。
	 */
	bool	   *will_parallel_vacuum;

	/*
	 * The number of indexes that support parallel index bulk-deletion and
	 * parallel index cleanup respectively.
	 *
	 * 分别支持并行索引批量删除和并行索引清理的索引数量。
	 */
	int			nindexes_parallel_bulkdel;
	int			nindexes_parallel_cleanup;
	int			nindexes_parallel_condcleanup;

	/* Buffer access strategy used by leader process */
	/*
	 *
	 * leader 进程使用的缓冲区访问策略。
	 */
	BufferAccessStrategy bstrategy;

	/*
	 * Error reporting state.  The error callback is set only for workers
	 * processes during parallel index vacuum.
	 *
	 * 错误报告状态。错误回调只在并行索引 vacuum 期间为 worker 进程设置。
	 */
	char	   *relnamespace;
	char	   *relname;
	char	   *indname;
	PVIndVacStatus status;
};

static int	parallel_vacuum_compute_workers(Relation *indrels, int nindexes, int nrequested,
											bool *will_parallel_vacuum);
static void parallel_vacuum_process_all_indexes(ParallelVacuumState *pvs, int num_index_scans,
												bool vacuum);
static void parallel_vacuum_process_safe_indexes(ParallelVacuumState *pvs);
static void parallel_vacuum_process_unsafe_indexes(ParallelVacuumState *pvs);
static void parallel_vacuum_process_one_index(ParallelVacuumState *pvs, Relation indrel,
											  PVIndStats *indstats);
static bool parallel_vacuum_index_is_parallel_safe(Relation indrel, int num_index_scans,
												   bool vacuum);
static void parallel_vacuum_error_callback(void *arg);

/*
 * Try to enter parallel mode and create a parallel context.  Then initialize
 * shared memory state.
 *
 * 尝试进入并行模式并创建并行上下文，然后初始化共享内存状态。
 *
 * On success, return parallel vacuum state.  Otherwise return NULL.
 *
 * 成功则返回并行 vacuum 状态，否则返回 NULL。
 */
ParallelVacuumState *
parallel_vacuum_init(Relation rel, Relation *indrels, int nindexes,
					 int nrequested_workers, int vac_work_mem,
					 int elevel, BufferAccessStrategy bstrategy)
{
	ParallelVacuumState *pvs;
	ParallelContext *pcxt;
	PVShared   *shared;
	TidStore   *dead_items;
	PVIndStats *indstats;
	BufferUsage *buffer_usage;
	WalUsage   *wal_usage;
	bool	   *will_parallel_vacuum;
	Size		est_indstats_len;
	Size		est_shared_len;
	int			nindexes_mwm = 0;
	int			parallel_workers = 0;
	int			querylen;

	/*
	 * A parallel vacuum must be requested and there must be indexes on the
	 * relation
	 *
	 * 必须请求了并行 vacuum，并且关系上必须有索引。
	 */
	Assert(nrequested_workers >= 0);
	Assert(nindexes > 0);

	/*
	 * Compute the number of parallel vacuum workers to launch
	 *
	 * 计算要启动的并行 vacuum worker 数。
	 */
	will_parallel_vacuum = (bool *) palloc0(sizeof(bool) * nindexes);
	parallel_workers = parallel_vacuum_compute_workers(indrels, nindexes,
													   nrequested_workers,
													   will_parallel_vacuum);
	if (parallel_workers <= 0)
	{
		/* Can't perform vacuum in parallel -- return NULL */
		/*
		 *
		 * 无法并行 vacuum，返回 NULL。
		 */
		pfree(will_parallel_vacuum);
		return NULL;
	}

	pvs = (ParallelVacuumState *) palloc0(sizeof(ParallelVacuumState));
	pvs->indrels = indrels;
	pvs->nindexes = nindexes;
	pvs->will_parallel_vacuum = will_parallel_vacuum;
	pvs->bstrategy = bstrategy;
	pvs->heaprel = rel;

	EnterParallelMode();
	pcxt = CreateParallelContext("postgres", "parallel_vacuum_main",
								 parallel_workers);
	Assert(pcxt->nworkers > 0);
	pvs->pcxt = pcxt;

	/* Estimate size for index vacuum stats -- PARALLEL_VACUUM_KEY_INDEX_STATS */
	/*
	 *
	 * 估算索引 vacuum 统计的大小，对应 PARALLEL_VACUUM_KEY_INDEX_STATS。
	 */
	est_indstats_len = mul_size(sizeof(PVIndStats), nindexes);
	shm_toc_estimate_chunk(&pcxt->estimator, est_indstats_len);
	shm_toc_estimate_keys(&pcxt->estimator, 1);

	/* Estimate size for shared information -- PARALLEL_VACUUM_KEY_SHARED */
	/*
	 *
	 * 估算共享信息的大小，对应 PARALLEL_VACUUM_KEY_SHARED。
	 */
	est_shared_len = sizeof(PVShared);
	shm_toc_estimate_chunk(&pcxt->estimator, est_shared_len);
	shm_toc_estimate_keys(&pcxt->estimator, 1);

	/*
	 * Estimate space for BufferUsage and WalUsage --
	 * PARALLEL_VACUUM_KEY_BUFFER_USAGE and PARALLEL_VACUUM_KEY_WAL_USAGE.
	 *
	 * 估算 BufferUsage 和 WalUsage 的空间，对应 PARALLEL_VACUUM_KEY_BUFFER_USAGE 与 PARALLEL_VACUUM_KEY_WAL_USAGE。
	 *
	 * If there are no extensions loaded that care, we could skip this.  We
	 * have no way of knowing whether anyone's looking at pgBufferUsage or
	 * pgWalUsage, so do it unconditionally.
	 *
	 * 若没有关心这些数据的扩展，本可以跳过。但无法知道是否有人在看 pgBufferUsage 或 pgWalUsage，因此无条件做。
	 */
	shm_toc_estimate_chunk(&pcxt->estimator,
						   mul_size(sizeof(BufferUsage), pcxt->nworkers));
	shm_toc_estimate_keys(&pcxt->estimator, 1);
	shm_toc_estimate_chunk(&pcxt->estimator,
						   mul_size(sizeof(WalUsage), pcxt->nworkers));
	shm_toc_estimate_keys(&pcxt->estimator, 1);

	/* Finally, estimate PARALLEL_VACUUM_KEY_QUERY_TEXT space */
	/*
	 *
	 * 最后估算 PARALLEL_VACUUM_KEY_QUERY_TEXT 的空间。
	 */
	if (debug_query_string)
	{
		querylen = strlen(debug_query_string);
		shm_toc_estimate_chunk(&pcxt->estimator, querylen + 1);
		shm_toc_estimate_keys(&pcxt->estimator, 1);
	}
	else
		querylen = 0;			/* keep compiler quiet */
		/*
		 *
		 * 避免编译器告警。
		 */

	InitializeParallelDSM(pcxt);

	/* Prepare index vacuum stats */
	/*
	 *
	 * 准备索引 vacuum 统计。
	 */
	indstats = (PVIndStats *) shm_toc_allocate(pcxt->toc, est_indstats_len);
	MemSet(indstats, 0, est_indstats_len);
	for (int i = 0; i < nindexes; i++)
	{
		Relation	indrel = indrels[i];
		uint8		vacoptions = indrel->rd_indam->amparallelvacuumoptions;

		/*
		 * Cleanup option should be either disabled, always performing in
		 * parallel or conditionally performing in parallel.
		 *
		 * 清理选项应为禁用、始终并行，或有条件地并行。
		 */
		Assert(((vacoptions & VACUUM_OPTION_PARALLEL_CLEANUP) == 0) ||
			   ((vacoptions & VACUUM_OPTION_PARALLEL_COND_CLEANUP) == 0));
		Assert(vacoptions <= VACUUM_OPTION_MAX_VALID_VALUE);

		if (!will_parallel_vacuum[i])
			continue;

		if (indrel->rd_indam->amusemaintenanceworkmem)
			nindexes_mwm++;

		/*
		 * Remember the number of indexes that support parallel operation for
		 * each phase.
		 *
		 * 记住每个阶段支持并行操作的索引数量。
		 */
		if ((vacoptions & VACUUM_OPTION_PARALLEL_BULKDEL) != 0)
			pvs->nindexes_parallel_bulkdel++;
		if ((vacoptions & VACUUM_OPTION_PARALLEL_CLEANUP) != 0)
			pvs->nindexes_parallel_cleanup++;
		if ((vacoptions & VACUUM_OPTION_PARALLEL_COND_CLEANUP) != 0)
			pvs->nindexes_parallel_condcleanup++;
	}
	shm_toc_insert(pcxt->toc, PARALLEL_VACUUM_KEY_INDEX_STATS, indstats);
	pvs->indstats = indstats;

	/* Prepare shared information */
	/*
	 *
	 * 准备共享信息。
	 */
	shared = (PVShared *) shm_toc_allocate(pcxt->toc, est_shared_len);
	MemSet(shared, 0, est_shared_len);
	shared->relid = RelationGetRelid(rel);
	shared->elevel = elevel;
	shared->queryid = pgstat_get_my_query_id();
	shared->maintenance_work_mem_worker =
		(nindexes_mwm > 0) ?
		maintenance_work_mem / Min(parallel_workers, nindexes_mwm) :
		maintenance_work_mem;
	shared->dead_items_info.max_bytes = vac_work_mem * (size_t) 1024;

	/* Prepare DSA space for dead items */
	/*
	 *
	 * 为死元组准备 DSA 空间。
	 */
	dead_items = TidStoreCreateShared(shared->dead_items_info.max_bytes,
									  LWTRANCHE_PARALLEL_VACUUM_DSA);
	pvs->dead_items = dead_items;
	shared->dead_items_handle = TidStoreGetHandle(dead_items);
	shared->dead_items_dsa_handle = dsa_get_handle(TidStoreGetDSA(dead_items));

	/* Use the same buffer size for all workers */
	/*
	 *
	 * 所有 worker 使用相同的缓冲区大小。
	 */
	shared->ring_nbuffers = GetAccessStrategyBufferCount(bstrategy);

	pg_atomic_init_u32(&(shared->cost_balance), 0);
	pg_atomic_init_u32(&(shared->active_nworkers), 0);
	pg_atomic_init_u32(&(shared->idx), 0);

	shm_toc_insert(pcxt->toc, PARALLEL_VACUUM_KEY_SHARED, shared);
	pvs->shared = shared;

	/*
	 * Allocate space for each worker's BufferUsage and WalUsage; no need to
	 * initialize
	 *
	 * 为每个 worker 的 BufferUsage 和 WalUsage 分配空间；不必初始化。
	 */
	buffer_usage = shm_toc_allocate(pcxt->toc,
									mul_size(sizeof(BufferUsage), pcxt->nworkers));
	shm_toc_insert(pcxt->toc, PARALLEL_VACUUM_KEY_BUFFER_USAGE, buffer_usage);
	pvs->buffer_usage = buffer_usage;
	wal_usage = shm_toc_allocate(pcxt->toc,
								 mul_size(sizeof(WalUsage), pcxt->nworkers));
	shm_toc_insert(pcxt->toc, PARALLEL_VACUUM_KEY_WAL_USAGE, wal_usage);
	pvs->wal_usage = wal_usage;

	/* Store query string for workers */
	/*
	 *
	 * 为 worker 保存查询字符串。
	 */
	if (debug_query_string)
	{
		char	   *sharedquery;

		sharedquery = (char *) shm_toc_allocate(pcxt->toc, querylen + 1);
		memcpy(sharedquery, debug_query_string, querylen + 1);
		sharedquery[querylen] = '\0';
		shm_toc_insert(pcxt->toc,
					   PARALLEL_VACUUM_KEY_QUERY_TEXT, sharedquery);
	}

	/* Success -- return parallel vacuum state */
	/*
	 *
	 * 成功，返回并行 vacuum 状态。
	 */
	return pvs;
}

/*
 * Destroy the parallel context, and end parallel mode.
 *
 * 销毁并行上下文，并结束并行模式。
 *
 * Since writes are not allowed during parallel mode, copy the
 * updated index statistics from DSM into local memory and then later use that
 * to update the index statistics.  One might think that we can exit from
 * parallel mode, update the index statistics and then destroy parallel
 * context, but that won't be safe (see ExitParallelMode).
 *
 * 并行模式中不允许写，因此先把 DSM 中更新后的索引统计复制到本地内存，稍后再用它更新索引统计。
 * 不能先退出并行模式、更新统计再销毁并行上下文，那样不安全（见 ExitParallelMode）。
 */
void
parallel_vacuum_end(ParallelVacuumState *pvs, IndexBulkDeleteResult **istats)
{
	Assert(!IsParallelWorker());

	/* Copy the updated statistics */
	/*
	 *
	 * 复制更新后的统计。
	 */
	for (int i = 0; i < pvs->nindexes; i++)
	{
		PVIndStats *indstats = &(pvs->indstats[i]);

		if (indstats->istat_updated)
		{
			istats[i] = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
			memcpy(istats[i], &indstats->istat, sizeof(IndexBulkDeleteResult));
		}
		else
			istats[i] = NULL;
	}

	TidStoreDestroy(pvs->dead_items);

	DestroyParallelContext(pvs->pcxt);
	ExitParallelMode();

	pfree(pvs->will_parallel_vacuum);
	pfree(pvs);
}

/*
 * Returns the dead items space and dead items information.
 *
 * 返回死元组空间和死元组信息。
 */
TidStore *
parallel_vacuum_get_dead_items(ParallelVacuumState *pvs, VacDeadItemsInfo **dead_items_info_p)
{
	*dead_items_info_p = &(pvs->shared->dead_items_info);
	return pvs->dead_items;
}

/* Forget all items in dead_items */
/*
 *
 * 忘掉 dead_items 中的全部项。
 */
void
parallel_vacuum_reset_dead_items(ParallelVacuumState *pvs)
{
	VacDeadItemsInfo *dead_items_info = &(pvs->shared->dead_items_info);

	/*
	 * Free the current tidstore and return allocated DSA segments to the
	 * operating system. Then we recreate the tidstore with the same max_bytes
	 * limitation we just used.
	 *
	 * 释放当前 tidstore，并把已分配的 DSA 段还给操作系统。然后用刚才相同的 max_bytes 限制重建 tidstore。
	 */
	TidStoreDestroy(pvs->dead_items);
	pvs->dead_items = TidStoreCreateShared(dead_items_info->max_bytes,
										   LWTRANCHE_PARALLEL_VACUUM_DSA);

	/* Update the DSA pointer for dead_items to the new one */
	/*
	 *
	 * 把 dead_items 的 DSA 指针更新为新的。
	 */
	pvs->shared->dead_items_dsa_handle = dsa_get_handle(TidStoreGetDSA(pvs->dead_items));
	pvs->shared->dead_items_handle = TidStoreGetHandle(pvs->dead_items);

	/* Reset the counter */
	/*
	 *
	 * 重置计数器。
	 */
	dead_items_info->num_items = 0;
}

/*
 * Do parallel index bulk-deletion with parallel workers.
 *
 * 用并行 worker 做并行索引批量删除。
 */
void
parallel_vacuum_bulkdel_all_indexes(ParallelVacuumState *pvs, long num_table_tuples,
									int num_index_scans)
{
	Assert(!IsParallelWorker());

	/*
	 * We can only provide an approximate value of num_heap_tuples, at least
	 * for now.
	 *
	 * 至少目前只能提供 num_heap_tuples 的近似值。
	 */
	pvs->shared->reltuples = num_table_tuples;
	pvs->shared->estimated_count = true;

	parallel_vacuum_process_all_indexes(pvs, num_index_scans, true);
}

/*
 * Do parallel index cleanup with parallel workers.
 *
 * 用并行 worker 做并行索引清理。
 */
void
parallel_vacuum_cleanup_all_indexes(ParallelVacuumState *pvs, long num_table_tuples,
									int num_index_scans, bool estimated_count)
{
	Assert(!IsParallelWorker());

	/*
	 * We can provide a better estimate of total number of surviving tuples
	 * (we assume indexes are more interested in that than in the number of
	 * nominally live tuples).
	 *
	 * 可以更好地估计存活元组总数（假定索引更关心这个，而不是名义上的活元组数）。
	 */
	pvs->shared->reltuples = num_table_tuples;
	pvs->shared->estimated_count = estimated_count;

	parallel_vacuum_process_all_indexes(pvs, num_index_scans, false);
}

/*
 * Compute the number of parallel worker processes to request.  Both index
 * vacuum and index cleanup can be executed with parallel workers.
 * The index is eligible for parallel vacuum iff its size is greater than
 * min_parallel_index_scan_size as invoking workers for very small indexes
 * can hurt performance.
 *
 * 计算要请求的并行 worker 进程数。索引 vacuum 和索引清理都可以用并行 worker。
 * 仅当索引大于 min_parallel_index_scan_size 时才适合并行 vacuum，为很小的索引启动 worker 会损害性能。
 *
 * nrequested is the number of parallel workers that user requested.  If
 * nrequested is 0, we compute the parallel degree based on nindexes, that is
 * the number of indexes that support parallel vacuum.  This function also
 * sets will_parallel_vacuum to remember indexes that participate in parallel
 * vacuum.
 *
 * nrequested 是用户请求的并行 worker 数。若为 0，则按支持并行 vacuum 的索引数 nindexes 计算并行度。
 * 本函数还设置 will_parallel_vacuum，记下参与并行 vacuum 的索引。
 */
static int
parallel_vacuum_compute_workers(Relation *indrels, int nindexes, int nrequested,
								bool *will_parallel_vacuum)
{
	int			nindexes_parallel = 0;
	int			nindexes_parallel_bulkdel = 0;
	int			nindexes_parallel_cleanup = 0;
	int			parallel_workers;

	/*
	 * We don't allow performing parallel operation in standalone backend or
	 * when parallelism is disabled.
	 *
	 * 独立后端或禁用并行时不允许并行操作。
	 */
	if (!IsUnderPostmaster || max_parallel_maintenance_workers == 0)
		return 0;

	/*
	 * Compute the number of indexes that can participate in parallel vacuum.
	 *
	 * 计算可以参与并行 vacuum 的索引数。
	 */
	for (int i = 0; i < nindexes; i++)
	{
		Relation	indrel = indrels[i];
		uint8		vacoptions = indrel->rd_indam->amparallelvacuumoptions;

		/* Skip index that is not a suitable target for parallel index vacuum */
		/*
		 *
		 * 跳过不适合并行索引 vacuum 的索引。
		 */
		if (vacoptions == VACUUM_OPTION_NO_PARALLEL ||
			RelationGetNumberOfBlocks(indrel) < min_parallel_index_scan_size)
			continue;

		will_parallel_vacuum[i] = true;

		if ((vacoptions & VACUUM_OPTION_PARALLEL_BULKDEL) != 0)
			nindexes_parallel_bulkdel++;
		if (((vacoptions & VACUUM_OPTION_PARALLEL_CLEANUP) != 0) ||
			((vacoptions & VACUUM_OPTION_PARALLEL_COND_CLEANUP) != 0))
			nindexes_parallel_cleanup++;
	}

	nindexes_parallel = Max(nindexes_parallel_bulkdel,
							nindexes_parallel_cleanup);

	/* The leader process takes one index */
	/*
	 *
	 * leader 进程自己处理一个索引。
	 */
	nindexes_parallel--;

	/* No index supports parallel vacuum */
	/*
	 *
	 * 没有索引支持并行 vacuum。
	 */
	if (nindexes_parallel <= 0)
		return 0;

	/* Compute the parallel degree */
	/*
	 *
	 * 计算并行度。
	 */
	parallel_workers = (nrequested > 0) ?
		Min(nrequested, nindexes_parallel) : nindexes_parallel;

	/* Cap by max_parallel_maintenance_workers */
	/*
	 *
	 * 以 max_parallel_maintenance_workers 为上限。
	 */
	parallel_workers = Min(parallel_workers, max_parallel_maintenance_workers);

	return parallel_workers;
}

/*
 * Perform index vacuum or index cleanup with parallel workers.  This function
 * must be used by the parallel vacuum leader process.
 *
 * 用并行 worker 执行索引 vacuum 或索引清理。本函数必须由并行 vacuum 的 leader 进程使用。
 */
static void
parallel_vacuum_process_all_indexes(ParallelVacuumState *pvs, int num_index_scans,
									bool vacuum)
{
	int			nworkers;
	PVIndVacStatus new_status;

	Assert(!IsParallelWorker());

	if (vacuum)
	{
		new_status = PARALLEL_INDVAC_STATUS_NEED_BULKDELETE;

		/* Determine the number of parallel workers to launch */
		/*
		 *
		 * 确定要启动的并行 worker 数。
		 */
		nworkers = pvs->nindexes_parallel_bulkdel;
	}
	else
	{
		new_status = PARALLEL_INDVAC_STATUS_NEED_CLEANUP;

		/* Determine the number of parallel workers to launch */
		/*
		 *
		 * 确定要启动的并行 worker 数。
		 */
		nworkers = pvs->nindexes_parallel_cleanup;

		/* Add conditionally parallel-aware indexes if in the first time call */
		/*
		 *
		 * 若是第一次调用，则加入有条件支持并行的索引。
		 */
		if (num_index_scans == 0)
			nworkers += pvs->nindexes_parallel_condcleanup;
	}

	/* The leader process will participate */
	/*
	 *
	 * leader 进程将参与。
	 */
	nworkers--;

	/*
	 * It is possible that parallel context is initialized with fewer workers
	 * than the number of indexes that need a separate worker in the current
	 * phase, so we need to consider it.  See
	 * parallel_vacuum_compute_workers().
	 *
	 * 并行上下文初始化的 worker 数可能少于当前阶段需要单独 worker 的索引数，因此要考虑这一点。
	 * 见 parallel_vacuum_compute_workers()。
	 */
	nworkers = Min(nworkers, pvs->pcxt->nworkers);

	/*
	 * Set index vacuum status and mark whether parallel vacuum worker can
	 * process it.
	 *
	 * 设置索引 vacuum 状态，并标记并行 vacuum worker 能否处理它。
	 */
	for (int i = 0; i < pvs->nindexes; i++)
	{
		PVIndStats *indstats = &(pvs->indstats[i]);

		Assert(indstats->status == PARALLEL_INDVAC_STATUS_INITIAL);
		indstats->status = new_status;
		indstats->parallel_workers_can_process =
			(pvs->will_parallel_vacuum[i] &&
			 parallel_vacuum_index_is_parallel_safe(pvs->indrels[i],
													num_index_scans,
													vacuum));
	}

	/* Reset the parallel index processing and progress counters */
	/*
	 *
	 * 重置并行索引处理与进度计数器。
	 */
	pg_atomic_write_u32(&(pvs->shared->idx), 0);

	/* Setup the shared cost-based vacuum delay and launch workers */
	/*
	 *
	 * 设置共享的基于代价的 vacuum 延迟，并启动 worker。
	 */
	if (nworkers > 0)
	{
		/* Reinitialize parallel context to relaunch parallel workers */
		/*
		 *
		 * 重新初始化并行上下文，以便再次启动并行 worker。
		 */
		if (num_index_scans > 0)
			ReinitializeParallelDSM(pvs->pcxt);

		/*
		 * Set up shared cost balance and the number of active workers for
		 * vacuum delay.  We need to do this before launching workers as
		 * otherwise, they might not see the updated values for these
		 * parameters.
		 *
		 * 为 vacuum 延迟设置共享代价余额和活跃 worker 数。必须在启动 worker 之前做，否则它们可能看不到这些参数的新值。
		 */
		pg_atomic_write_u32(&(pvs->shared->cost_balance), VacuumCostBalance);
		pg_atomic_write_u32(&(pvs->shared->active_nworkers), 0);

		/*
		 * The number of workers can vary between bulkdelete and cleanup
		 * phase.
		 *
		 * 批量删除阶段和清理阶段的 worker 数可以不同。
		 */
		ReinitializeParallelWorkers(pvs->pcxt, nworkers);

		LaunchParallelWorkers(pvs->pcxt);

		if (pvs->pcxt->nworkers_launched > 0)
		{
			/*
			 * Reset the local cost values for leader backend as we have
			 * already accumulated the remaining balance of heap.
			 *
			 * 重置 leader 后端的本地代价值，因为堆上剩余的余额已经累加过。
			 */
			VacuumCostBalance = 0;
			VacuumCostBalanceLocal = 0;

			/* Enable shared cost balance for leader backend */
			/*
			 *
			 * 为 leader 后端启用共享代价余额。
			 */
			VacuumSharedCostBalance = &(pvs->shared->cost_balance);
			VacuumActiveNWorkers = &(pvs->shared->active_nworkers);
		}

		if (vacuum)
			ereport(pvs->shared->elevel,
					(errmsg(ngettext("launched %d parallel vacuum worker for index vacuuming (planned: %d)",
									 "launched %d parallel vacuum workers for index vacuuming (planned: %d)",
									 pvs->pcxt->nworkers_launched),
							pvs->pcxt->nworkers_launched, nworkers)));
		else
			ereport(pvs->shared->elevel,
					(errmsg(ngettext("launched %d parallel vacuum worker for index cleanup (planned: %d)",
									 "launched %d parallel vacuum workers for index cleanup (planned: %d)",
									 pvs->pcxt->nworkers_launched),
							pvs->pcxt->nworkers_launched, nworkers)));
	}

	/* Vacuum the indexes that can be processed by only leader process */
	/*
	 *
	 * vacuum 只能由 leader 进程处理的索引。
	 */
	parallel_vacuum_process_unsafe_indexes(pvs);

	/*
	 * Join as a parallel worker.  The leader vacuums alone processes all
	 * parallel-safe indexes in the case where no workers are launched.
	 *
	 * 以并行 worker 的身份加入。若没有启动 worker，则由 leader 独自 vacuum 所有并行安全的索引。
	 */
	parallel_vacuum_process_safe_indexes(pvs);

	/*
	 * Next, accumulate buffer and WAL usage.  (This must wait for the workers
	 * to finish, or we might get incomplete data.)
	 *
	 * 接着累加缓冲区和 WAL 使用量。（必须等 worker 结束，否则数据可能不完整。）
	 */
	if (nworkers > 0)
	{
		/* Wait for all vacuum workers to finish */
		/*
		 *
		 * 等待所有 vacuum worker 结束。
		 */
		WaitForParallelWorkersToFinish(pvs->pcxt);

		for (int i = 0; i < pvs->pcxt->nworkers_launched; i++)
			InstrAccumParallelQuery(&pvs->buffer_usage[i], &pvs->wal_usage[i]);
	}

	/*
	 * Reset all index status back to initial (while checking that we have
	 * vacuumed all indexes).
	 *
	 * 把所有索引状态重置为初始值（同时检查是否已 vacuum 全部索引）。
	 */
	for (int i = 0; i < pvs->nindexes; i++)
	{
		PVIndStats *indstats = &(pvs->indstats[i]);

		if (indstats->status != PARALLEL_INDVAC_STATUS_COMPLETED)
			elog(ERROR, "parallel index vacuum on index \"%s\" is not completed",
				 RelationGetRelationName(pvs->indrels[i]));

		indstats->status = PARALLEL_INDVAC_STATUS_INITIAL;
	}

	/*
	 * Carry the shared balance value to heap scan and disable shared costing
	 *
	 * 把共享余额带到堆扫描，并关闭共享代价计算。
	 */
	if (VacuumSharedCostBalance)
	{
		VacuumCostBalance = pg_atomic_read_u32(VacuumSharedCostBalance);
		VacuumSharedCostBalance = NULL;
		VacuumActiveNWorkers = NULL;
	}
}

/*
 * Index vacuum/cleanup routine used by the leader process and parallel
 * vacuum worker processes to vacuum the indexes in parallel.
 *
 * leader 与并行 vacuum worker 用来并行 vacuum 索引的索引 vacuum/清理例程。
 */
static void
parallel_vacuum_process_safe_indexes(ParallelVacuumState *pvs)
{
	/*
	 * Increment the active worker count if we are able to launch any worker.
	 *
	 * 若能启动任何 worker，则增加活跃 worker 计数。
	 */
	if (VacuumActiveNWorkers)
		pg_atomic_add_fetch_u32(VacuumActiveNWorkers, 1);

	/* Loop until all indexes are vacuumed */
	/*
	 *
	 * 循环直到所有索引都被 vacuum。
	 */
	for (;;)
	{
		int			idx;
		PVIndStats *indstats;

		/* Get an index number to process */
		/*
		 *
		 * 取得要处理的索引号。
		 */
		idx = pg_atomic_fetch_add_u32(&(pvs->shared->idx), 1);

		/* Done for all indexes? */
		/*
		 *
		 * 所有索引都完成了？
		 */
		if (idx >= pvs->nindexes)
			break;

		indstats = &(pvs->indstats[idx]);

		/*
		 * Skip vacuuming index that is unsafe for workers or has an
		 * unsuitable target for parallel index vacuum (this is vacuumed in
		 * parallel_vacuum_process_unsafe_indexes() by the leader).
		 *
		 * 跳过对 worker 不安全或不适合并行索引 vacuum 的索引（由 leader 在 parallel_vacuum_process_unsafe_indexes() 中处理）。
		 */
		if (!indstats->parallel_workers_can_process)
			continue;

		/* Do vacuum or cleanup of the index */
		/*
		 *
		 * 对该索引做 vacuum 或清理。
		 */
		parallel_vacuum_process_one_index(pvs, pvs->indrels[idx], indstats);
	}

	/*
	 * We have completed the index vacuum so decrement the active worker
	 * count.
	 *
	 * 索引 vacuum 已完成，因此减少活跃 worker 计数。
	 */
	if (VacuumActiveNWorkers)
		pg_atomic_sub_fetch_u32(VacuumActiveNWorkers, 1);
}

/*
 * Perform parallel vacuuming of indexes in leader process.
 *
 * 在 leader 进程中执行索引的并行 vacuum。
 *
 * Handles index vacuuming (or index cleanup) for indexes that are not
 * parallel safe.  It's possible that this will vary for a given index, based
 * on details like whether we're performing index cleanup right now.
 *
 * 处理对并行不安全的索引的 vacuum（或清理）。对给定索引，这一点可能随当前是否在做索引清理等因素而变化。
 *
 * Also performs vacuuming of smaller indexes that fell under the size cutoff
 * enforced by parallel_vacuum_compute_workers().
 *
 * 也 vacuum 那些低于 parallel_vacuum_compute_workers() 所强制的大小阈值的较小索引。
 */
static void
parallel_vacuum_process_unsafe_indexes(ParallelVacuumState *pvs)
{
	Assert(!IsParallelWorker());

	/*
	 * Increment the active worker count if we are able to launch any worker.
	 *
	 * 若能启动任何 worker，则增加活跃 worker 计数。
	 */
	if (VacuumActiveNWorkers)
		pg_atomic_add_fetch_u32(VacuumActiveNWorkers, 1);

	for (int i = 0; i < pvs->nindexes; i++)
	{
		PVIndStats *indstats = &(pvs->indstats[i]);

		/* Skip, indexes that are safe for workers */
		/*
		 *
		 * 跳过对 worker 安全的索引。
		 */
		if (indstats->parallel_workers_can_process)
			continue;

		/* Do vacuum or cleanup of the index */
		/*
		 *
		 * 对该索引做 vacuum 或清理。
		 */
		parallel_vacuum_process_one_index(pvs, pvs->indrels[i], indstats);
	}

	/*
	 * We have completed the index vacuum so decrement the active worker
	 * count.
	 *
	 * 索引 vacuum 已完成，因此减少活跃 worker 计数。
	 */
	if (VacuumActiveNWorkers)
		pg_atomic_sub_fetch_u32(VacuumActiveNWorkers, 1);
}

/*
 * Vacuum or cleanup index either by leader process or by one of the worker
 * process.  After vacuuming the index this function copies the index
 * statistics returned from ambulkdelete and amvacuumcleanup to the DSM
 * segment.
 *
 * 由 leader 或某个 worker 对索引做 vacuum 或清理。之后把 ambulkdelete 和 amvacuumcleanup 返回的索引统计复制到 DSM。
 */
static void
parallel_vacuum_process_one_index(ParallelVacuumState *pvs, Relation indrel,
								  PVIndStats *indstats)
{
	IndexBulkDeleteResult *istat = NULL;
	IndexBulkDeleteResult *istat_res;
	IndexVacuumInfo ivinfo;

	/*
	 * Update the pointer to the corresponding bulk-deletion result if someone
	 * has already updated it
	 *
	 * 若已有人更新过对应的批量删除结果，则更新指向它的指针。
	 */
	if (indstats->istat_updated)
		istat = &(indstats->istat);

	ivinfo.index = indrel;
	ivinfo.heaprel = pvs->heaprel;
	ivinfo.analyze_only = false;
	ivinfo.report_progress = false;
	ivinfo.message_level = DEBUG2;
	ivinfo.estimated_count = pvs->shared->estimated_count;
	ivinfo.num_heap_tuples = pvs->shared->reltuples;
	ivinfo.strategy = pvs->bstrategy;

	/* Update error traceback information */
	/*
	 *
	 * 更新错误回溯信息。
	 */
	pvs->indname = pstrdup(RelationGetRelationName(indrel));
	pvs->status = indstats->status;

	switch (indstats->status)
	{
		case PARALLEL_INDVAC_STATUS_NEED_BULKDELETE:
			istat_res = vac_bulkdel_one_index(&ivinfo, istat, pvs->dead_items,
											  &pvs->shared->dead_items_info);
			break;
		case PARALLEL_INDVAC_STATUS_NEED_CLEANUP:
			istat_res = vac_cleanup_one_index(&ivinfo, istat);
			break;
		default:
			elog(ERROR, "unexpected parallel vacuum index status %d for index \"%s\"",
				 indstats->status,
				 RelationGetRelationName(indrel));
	}

	/*
	 * Copy the index bulk-deletion result returned from ambulkdelete and
	 * amvacuumcleanup to the DSM segment if it's the first cycle because they
	 * allocate locally and it's possible that an index will be vacuumed by a
	 * different vacuum process the next cycle.  Copying the result normally
	 * happens only the first time an index is vacuumed.  For any additional
	 * vacuum pass, we directly point to the result on the DSM segment and
	 * pass it to vacuum index APIs so that workers can update it directly.
	 *
	 * 若是第一轮，则把 ambulkdelete 和 amvacuumcleanup 返回的索引批量删除结果复制到 DSM，
	 * 因为它们在本地分配，下一轮可能由另一个 vacuum 进程处理该索引。
	 * 通常只在索引第一次被 vacuum 时复制。后续遍数直接指向 DSM 上的结果并传给索引 vacuum API，使 worker 能直接更新。
	 *
	 * Since all vacuum workers write the bulk-deletion result at different
	 * slots we can write them without locking.
	 *
	 * 各 vacuum worker 把批量删除结果写到不同槽位，因此不必加锁。
	 */
	if (!indstats->istat_updated && istat_res != NULL)
	{
		memcpy(&(indstats->istat), istat_res, sizeof(IndexBulkDeleteResult));
		indstats->istat_updated = true;

		/* Free the locally-allocated bulk-deletion result */
		/*
		 *
		 * 释放本地分配的批量删除结果。
		 */
		pfree(istat_res);
	}

	/*
	 * Update the status to completed. No need to lock here since each worker
	 * touches different indexes.
	 *
	 * 把状态更新为已完成。每个 worker 处理不同索引，这里不必加锁。
	 */
	indstats->status = PARALLEL_INDVAC_STATUS_COMPLETED;

	/* Reset error traceback information */
	/*
	 *
	 * 重置错误回溯信息。
	 */
	pvs->status = PARALLEL_INDVAC_STATUS_COMPLETED;
	pfree(pvs->indname);
	pvs->indname = NULL;

	/*
	 * Call the parallel variant of pgstat_progress_incr_param so workers can
	 * report progress of index vacuum to the leader.
	 *
	 * 调用 pgstat_progress_incr_param 的并行版本，使 worker 能向 leader 报告索引 vacuum 进度。
	 */
	pgstat_progress_parallel_incr_param(PROGRESS_VACUUM_INDEXES_PROCESSED, 1);
}

/*
 * Returns false, if the given index can't participate in the next execution of
 * parallel index vacuum or parallel index cleanup.
 *
 * 若给定索引不能参加下一轮并行索引 vacuum 或并行索引清理，则返回 false。
 */
static bool
parallel_vacuum_index_is_parallel_safe(Relation indrel, int num_index_scans,
									   bool vacuum)
{
	uint8		vacoptions;

	vacoptions = indrel->rd_indam->amparallelvacuumoptions;

	/* In parallel vacuum case, check if it supports parallel bulk-deletion */
	/*
	 *
	 * 并行 vacuum 时，检查它是否支持并行批量删除。
	 */
	if (vacuum)
		return ((vacoptions & VACUUM_OPTION_PARALLEL_BULKDEL) != 0);

	/* Not safe, if the index does not support parallel cleanup */
	/*
	 *
	 * 若索引不支持并行清理，则不安全。
	 */
	if (((vacoptions & VACUUM_OPTION_PARALLEL_CLEANUP) == 0) &&
		((vacoptions & VACUUM_OPTION_PARALLEL_COND_CLEANUP) == 0))
		return false;

	/*
	 * Not safe, if the index supports parallel cleanup conditionally, but we
	 * have already processed the index (for bulkdelete).  We do this to avoid
	 * the need to invoke workers when parallel index cleanup doesn't need to
	 * scan the index.  See the comments for option
	 * VACUUM_OPTION_PARALLEL_COND_CLEANUP to know when indexes support
	 * parallel cleanup conditionally.
	 *
	 * 若索引只是有条件地支持并行清理，且该索引（的批量删除）已经处理过，则不安全。
	 * 这样是为了在并行索引清理不需要扫描索引时避免再启动 worker。
	 * 索引何时有条件支持并行清理，见选项 VACUUM_OPTION_PARALLEL_COND_CLEANUP 的注释。
	 */
	if (num_index_scans > 0 &&
		((vacoptions & VACUUM_OPTION_PARALLEL_COND_CLEANUP) != 0))
		return false;

	return true;
}

/*
 * Perform work within a launched parallel process.
 *
 * 在已启动的并行进程中执行工作。
 *
 * Since parallel vacuum workers perform only index vacuum or index cleanup,
 * we don't need to report progress information.
 *
 * 并行 vacuum worker 只做索引 vacuum 或索引清理，因此不必报告进度信息。
 */
void
parallel_vacuum_main(dsm_segment *seg, shm_toc *toc)
{
	ParallelVacuumState pvs;
	Relation	rel;
	Relation   *indrels;
	PVIndStats *indstats;
	PVShared   *shared;
	TidStore   *dead_items;
	BufferUsage *buffer_usage;
	WalUsage   *wal_usage;
	int			nindexes;
	char	   *sharedquery;
	ErrorContextCallback errcallback;

	/*
	 * A parallel vacuum worker must have only PROC_IN_VACUUM flag since we
	 * don't support parallel vacuum for autovacuum as of now.
	 *
	 * 并行 vacuum worker 只能带 PROC_IN_VACUUM 标志，因为目前不支持 autovacuum 的并行 vacuum。
	 */
	Assert(MyProc->statusFlags == PROC_IN_VACUUM);

	elog(DEBUG1, "starting parallel vacuum worker");

	shared = (PVShared *) shm_toc_lookup(toc, PARALLEL_VACUUM_KEY_SHARED, false);

	/* Set debug_query_string for individual workers */
	/*
	 *
	 * 为各个 worker 设置 debug_query_string。
	 */
	sharedquery = shm_toc_lookup(toc, PARALLEL_VACUUM_KEY_QUERY_TEXT, true);
	debug_query_string = sharedquery;
	pgstat_report_activity(STATE_RUNNING, debug_query_string);

	/* Track query ID */
	/*
	 *
	 * 跟踪查询 ID。
	 */
	pgstat_report_query_id(shared->queryid, false);

	/*
	 * Open table.  The lock mode is the same as the leader process.  It's
	 * okay because the lock mode does not conflict among the parallel
	 * workers.
	 *
	 * 打开表。锁模式与 leader 相同。这是可以的，因为该锁模式在并行 worker 之间不冲突。
	 */
	rel = table_open(shared->relid, ShareUpdateExclusiveLock);

	/*
	 * Open all indexes. indrels are sorted in order by OID, which should be
	 * matched to the leader's one.
	 *
	 * 打开全部索引。indrels 按 OID 排序，应与 leader 的顺序一致。
	 */
	vac_open_indexes(rel, RowExclusiveLock, &nindexes, &indrels);
	Assert(nindexes > 0);

	/*
	 * Apply the desired value of maintenance_work_mem within this process.
	 * Really we should use SetConfigOption() to change a GUC, but since we're
	 * already in parallel mode guc.c would complain about that.  Fortunately,
	 * by the same token guc.c will not let any user-defined code change it.
	 * So just avert your eyes while we do this:
	 *
	 * 在本进程中应用所需的 maintenance_work_mem。本应使用 SetConfigOption() 改 GUC，
	 * 但已处于并行模式，guc.c 会抱怨。同样，guc.c 也不会让用户代码去改它。
	 * 所以这里直接改，请别细看：
	 */
	if (shared->maintenance_work_mem_worker > 0)
		maintenance_work_mem = shared->maintenance_work_mem_worker;

	/* Set index statistics */
	/*
	 *
	 * 设置索引统计。
	 */
	indstats = (PVIndStats *) shm_toc_lookup(toc,
											 PARALLEL_VACUUM_KEY_INDEX_STATS,
											 false);

	/* Find dead_items in shared memory */
	/*
	 *
	 * 在共享内存中找到 dead_items。
	 */
	dead_items = TidStoreAttach(shared->dead_items_dsa_handle,
								shared->dead_items_handle);

	/* Set cost-based vacuum delay */
	/*
	 *
	 * 设置基于代价的 vacuum 延迟。
	 */
	VacuumUpdateCosts();
	VacuumCostBalance = 0;
	VacuumCostBalanceLocal = 0;
	VacuumSharedCostBalance = &(shared->cost_balance);
	VacuumActiveNWorkers = &(shared->active_nworkers);

	/* Set parallel vacuum state */
	/*
	 *
	 * 设置并行 vacuum 状态。
	 */
	pvs.indrels = indrels;
	pvs.nindexes = nindexes;
	pvs.indstats = indstats;
	pvs.shared = shared;
	pvs.dead_items = dead_items;
	pvs.relnamespace = get_namespace_name(RelationGetNamespace(rel));
	pvs.relname = pstrdup(RelationGetRelationName(rel));
	pvs.heaprel = rel;

	/* These fields will be filled during index vacuum or cleanup */
	/*
	 *
	 * 这些字段将在索引 vacuum 或清理期间填充。
	 */
	pvs.indname = NULL;
	pvs.status = PARALLEL_INDVAC_STATUS_INITIAL;

	/* Each parallel VACUUM worker gets its own access strategy. */
	/*
	 *
	 * 每个并行 VACUUM worker 有自己的访问策略。
	 */
	pvs.bstrategy = GetAccessStrategyWithSize(BAS_VACUUM,
											  shared->ring_nbuffers * (BLCKSZ / 1024));

	/* Setup error traceback support for ereport() */
	/*
	 *
	 * 为 ereport() 设置错误回溯。
	 */
	errcallback.callback = parallel_vacuum_error_callback;
	errcallback.arg = &pvs;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* Prepare to track buffer usage during parallel execution */
	/*
	 *
	 * 准备在并行执行期间跟踪缓冲区使用量。
	 */
	InstrStartParallelQuery();

	/* Process indexes to perform vacuum/cleanup */
	/*
	 *
	 * 处理索引以执行 vacuum/清理。
	 */
	parallel_vacuum_process_safe_indexes(&pvs);

	/* Report buffer/WAL usage during parallel execution */
	/*
	 *
	 * 报告并行执行期间的缓冲区/WAL 使用量。
	 */
	buffer_usage = shm_toc_lookup(toc, PARALLEL_VACUUM_KEY_BUFFER_USAGE, false);
	wal_usage = shm_toc_lookup(toc, PARALLEL_VACUUM_KEY_WAL_USAGE, false);
	InstrEndParallelQuery(&buffer_usage[ParallelWorkerNumber],
						  &wal_usage[ParallelWorkerNumber]);

	/* Report any remaining cost-based vacuum delay time */
	/*
	 *
	 * 报告剩余的基于代价的 vacuum 延迟时间。
	 */
	if (track_cost_delay_timing)
		pgstat_progress_parallel_incr_param(PROGRESS_VACUUM_DELAY_TIME,
											parallel_vacuum_worker_delay_ns);

	TidStoreDetach(dead_items);

	/* Pop the error context stack */
	/*
	 *
	 * 弹出错误上下文栈。
	 */
	error_context_stack = errcallback.previous;

	vac_close_indexes(nindexes, indrels, RowExclusiveLock);
	table_close(rel, ShareUpdateExclusiveLock);
	FreeAccessStrategy(pvs.bstrategy);
}

/*
 * Error context callback for errors occurring during parallel index vacuum.
 * The error context messages should match the messages set in the lazy vacuum
 * error context.  If you change this function, change vacuum_error_callback()
 * as well.
 *
 * 并行索引 vacuum 期间出错时的错误上下文回调。错误上下文消息应与 lazy vacuum 错误上下文中的消息一致。
 * 若修改本函数，也要修改 vacuum_error_callback()。
 */
static void
parallel_vacuum_error_callback(void *arg)
{
	ParallelVacuumState *errinfo = arg;

	switch (errinfo->status)
	{
		case PARALLEL_INDVAC_STATUS_NEED_BULKDELETE:
			errcontext("while vacuuming index \"%s\" of relation \"%s.%s\"",
					   errinfo->indname,
					   errinfo->relnamespace,
					   errinfo->relname);
			break;
		case PARALLEL_INDVAC_STATUS_NEED_CLEANUP:
			errcontext("while cleaning up index \"%s\" of relation \"%s.%s\"",
					   errinfo->indname,
					   errinfo->relnamespace,
					   errinfo->relname);
			break;
		case PARALLEL_INDVAC_STATUS_INITIAL:
		case PARALLEL_INDVAC_STATUS_COMPLETED:
		default:
			return;
	}
}

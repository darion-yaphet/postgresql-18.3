/*-------------------------------------------------------------------------
 *
 * genam.h
 *	  POSTGRES generalized index access method definitions.
 *
 *
 *	  POSTGRES 通用索引访问方法定义。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/genam.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef GENAM_H
#define GENAM_H

#include "access/htup.h"
#include "access/sdir.h"
#include "access/skey.h"
#include "nodes/tidbitmap.h"
#include "storage/buf.h"
#include "storage/lockdefs.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"

/* We don't want this file to depend on execnodes.h. */

/* 我们不希望此文件依赖 execnodes.h。 */
struct IndexInfo;

/*
 * Struct for statistics maintained by amgettuple and amgetbitmap
 *
 * Note: IndexScanInstrumentation can't contain any pointers, since it is
 * copied into a SharedIndexScanInstrumentation during parallel scans
 */

/*
 * 由 amgettuple 和 amgetbitmap 维护的统计信息结构。
 *
 * 注意：IndexScanInstrumentation 不能包含任何指针，因为在并行扫描期间它会被
 * 复制到 SharedIndexScanInstrumentation 中。
 */
typedef struct IndexScanInstrumentation
{
	/* Index search count (incremented with pgstat_count_index_scan call) */

	/* 索引搜索计数（通过调用 pgstat_count_index_scan 递增）。 */
	uint64		nsearches;
} IndexScanInstrumentation;

/*
 * Struct for every worker's IndexScanInstrumentation, stored in shared memory
 */

/*
 * 每个工作进程的 IndexScanInstrumentation 结构，存储在共享内存中。
 */
typedef struct SharedIndexScanInstrumentation
{
	int			num_workers;
	IndexScanInstrumentation winstrument[FLEXIBLE_ARRAY_MEMBER];
} SharedIndexScanInstrumentation;

/*
 * Struct for statistics returned by ambuild
 */

/*
 * ambuild 返回的统计信息结构。
 */
typedef struct IndexBuildResult
{
	double		heap_tuples;	/* # of tuples seen in parent table */

									/* 父表中看到的元组数量。 */
	double		index_tuples;	/* # of tuples inserted into index */

									/* 插入索引的元组数量。 */
} IndexBuildResult;

/*
 * Struct for input arguments passed to ambulkdelete and amvacuumcleanup
 *
 * num_heap_tuples is accurate only when estimated_count is false;
 * otherwise it's just an estimate (currently, the estimate is the
 * prior value of the relation's pg_class.reltuples field, so it could
 * even be -1).  It will always just be an estimate during ambulkdelete.
 */

/*
 * 传给 ambulkdelete 和 amvacuumcleanup 的输入参数结构。
 *
 * 仅当 estimated_count 为 false 时，num_heap_tuples 才准确；否则它只是估计值
 * （当前估计值是关系 pg_class.reltuples 字段的先前值，因此甚至可能为 -1）。
 * 在 ambulkdelete 期间，它始终只是估计值。
 */
typedef struct IndexVacuumInfo
{
	Relation	index;			/* the index being vacuumed */

									/* 正在清理的索引。 */
	Relation	heaprel;		/* the heap relation the index belongs to */

									/* 该索引所属的堆关系。 */
	bool		analyze_only;	/* ANALYZE (without any actual vacuum) */

									/* ANALYZE（不执行实际 VACUUM）。 */
	bool		report_progress;	/* emit progress.h status reports */

									/* 发出 progress.h 状态报告。 */
	bool		estimated_count;	/* num_heap_tuples is an estimate */

									/* num_heap_tuples 为估计值。 */
	int			message_level;	/* ereport level for progress messages */

									/* 进度消息的 ereport 级别。 */
	double		num_heap_tuples;	/* tuples remaining in heap */

									/* 堆中剩余的元组。 */
	BufferAccessStrategy strategy;	/* access strategy for reads */

									/* 读取的访问策略。 */
} IndexVacuumInfo;

/*
 * Struct for statistics returned by ambulkdelete and amvacuumcleanup
 *
 * This struct is normally allocated by the first ambulkdelete call and then
 * passed along through subsequent ones until amvacuumcleanup; however,
 * amvacuumcleanup must be prepared to allocate it in the case where no
 * ambulkdelete calls were made (because no tuples needed deletion).
 * Note that an index AM could choose to return a larger struct
 * of which this is just the first field; this provides a way for ambulkdelete
 * to communicate additional private data to amvacuumcleanup.
 *
 * Note: pages_newly_deleted is the number of pages in the index that were
 * deleted by the current vacuum operation.  pages_deleted and pages_free
 * refer to free space within the index file.
 *
 * Note: Some index AMs may compute num_index_tuples by reference to
 * num_heap_tuples, in which case they should copy the estimated_count field
 * from IndexVacuumInfo.
 */

/*
 * ambulkdelete 和 amvacuumcleanup 返回的统计信息结构。
 *
 * 此结构通常由第一次 ambulkdelete 调用分配，并在后续调用中传递直到
 * amvacuumcleanup；不过，在没有执行 ambulkdelete 调用（因为没有元组需要删除）
 * 时，amvacuumcleanup 必须能够分配它。索引 AM 可以选择返回一个更大的结构，
 * 而本结构只是其第一个字段；这让 ambulkdelete 可以向 amvacuumcleanup 传递额外的
 * 私有数据。
 *
 * 注意：pages_newly_deleted 是当前 VACUUM 操作删除的索引页数。pages_deleted
 * 和 pages_free 指索引文件中的空闲空间。
 *
 * 注意：一些索引 AM 可能根据 num_heap_tuples 计算 num_index_tuples；此时它们应
 * 从 IndexVacuumInfo 复制 estimated_count 字段。
 */
typedef struct IndexBulkDeleteResult
{
	BlockNumber num_pages;		/* pages remaining in index */

									/* 索引中剩余的页面。 */
	bool		estimated_count;	/* num_index_tuples is an estimate */

									/* num_index_tuples 为估计值。 */
	double		num_index_tuples;	/* tuples remaining */

									/* 剩余的元组。 */
	double		tuples_removed; /* # removed during vacuum operation */

									/* VACUUM 操作期间删除的数量。 */
	BlockNumber pages_newly_deleted;	/* # pages marked deleted by us  */

									/* 由我们标记为删除的页面数量。 */
	BlockNumber pages_deleted;	/* # pages marked deleted (could be by us) */

									/* 标记为删除的页面数量（可能由我们标记）。 */
	BlockNumber pages_free;		/* # pages available for reuse */

									/* 可供重用的页面数量。 */
} IndexBulkDeleteResult;

/* Typedef for callback function to determine if a tuple is bulk-deletable */

/* 用于确定元组能否批量删除的回调函数 typedef。 */
typedef bool (*IndexBulkDeleteCallback) (ItemPointer itemptr, void *state);

/* struct definitions appear in relscan.h */

/* 结构定义位于 relscan.h 中。 */
typedef struct IndexScanDescData *IndexScanDesc;
typedef struct SysScanDescData *SysScanDesc;

typedef struct ParallelIndexScanDescData *ParallelIndexScanDesc;

/*
 * Enumeration specifying the type of uniqueness check to perform in
 * index_insert().
 *
 * UNIQUE_CHECK_YES is the traditional Postgres immediate check, possibly
 * blocking to see if a conflicting transaction commits.
 *
 * For deferrable unique constraints, UNIQUE_CHECK_PARTIAL is specified at
 * insertion time.  The index AM should test if the tuple is unique, but
 * should not throw error, block, or prevent the insertion if the tuple
 * appears not to be unique.  We'll recheck later when it is time for the
 * constraint to be enforced.  The AM must return true if the tuple is
 * known unique, false if it is possibly non-unique.  In the "true" case
 * it is safe to omit the later recheck.
 *
 * When it is time to recheck the deferred constraint, a pseudo-insertion
 * call is made with UNIQUE_CHECK_EXISTING.  The tuple is already in the
 * index in this case, so it should not be inserted again.  Rather, just
 * check for conflicting live tuples (possibly blocking).
 */

/*
 * 指定 index_insert() 要执行的唯一性检查类型的枚举。
 *
 * UNIQUE_CHECK_YES 是传统的 Postgres 立即检查，可能阻塞以确认冲突事务是否提交。
 *
 * 对可延迟唯一约束，插入时指定 UNIQUE_CHECK_PARTIAL。索引 AM 应测试元组是否
 * 唯一，但若元组看似不唯一，不应报错、阻塞或阻止插入。稍后在真正强制约束时会重新
 * 检查。AM 在已知唯一时必须返回 true，可能不唯一时返回 false。返回 true 时可安全
 * 省略后续重新检查。
 *
 * 需要重新检查延迟约束时，会使用 UNIQUE_CHECK_EXISTING 执行伪插入调用。此时元组
 * 已位于索引中，因此不应再次插入；只需检查冲突的活动元组（可能阻塞）。
 */
typedef enum IndexUniqueCheck
{
	UNIQUE_CHECK_NO,			/* Don't do any uniqueness checking */

									/* 不执行任何唯一性检查。 */
	UNIQUE_CHECK_YES,			/* Enforce uniqueness at insertion time */

									/* 在插入时强制唯一性。 */
	UNIQUE_CHECK_PARTIAL,		/* Test uniqueness, but no error */

									/* 测试唯一性，但不报错。 */
	UNIQUE_CHECK_EXISTING,		/* Check if existing tuple is unique */

									/* 检查现有元组是否唯一。 */
} IndexUniqueCheck;


/* Nullable "ORDER BY col op const" distance */

/* 可为 NULL 的“ORDER BY col op const”距离。 */
typedef struct IndexOrderByDistance
{
	double		value;
	bool		isnull;
} IndexOrderByDistance;

/*
 * generalized index_ interface routines (in indexam.c)
 */

/*
 * 通用 index_ 接口例程（位于 indexam.c）。
 */

/*
 * IndexScanIsValid
 *		True iff the index scan is valid.
 */

/*
 * IndexScanIsValid
 *		当且仅当索引扫描有效时为真。
 */
#define IndexScanIsValid(scan) PointerIsValid(scan)

/* Open an index relation with the requested lock mode.
 * The routine resolves the relation OID, acquires the lock, and returns its
 * cached descriptor.
 *
 * 使用请求的锁模式打开索引关系。
 * 此例程解析关系 OID、获取锁，并返回其缓存描述符。
 */
extern Relation index_open(Oid relationId, LOCKMODE lockmode);

/* Try to open an index relation without waiting for a conflicting lock.
 * The routine returns NULL when the lock cannot be acquired immediately.
 *
 * 尝试打开索引关系而不等待冲突锁。
 * 无法立即获取锁时，此例程返回 NULL。
 */
extern Relation try_index_open(Oid relationId, LOCKMODE lockmode);

/* Close an index relation and release its lock.
 * The descriptor is released and the specified lock mode is unwound.
 *
 * 关闭索引关系并释放其锁。
 * 此例程释放描述符并撤销指定的锁模式。
 */
extern void index_close(Relation relation, LOCKMODE lockmode);

/* Insert one heap TID and key values into an index.
 * The wrapper dispatches to the index AM and applies the requested uniqueness
 * semantics and index metadata.
 *
 * 将一个堆 TID 和键值插入索引。
 * 此包装器分派到索引 AM，并应用请求的唯一性语义和索引元数据。
 */
extern bool index_insert(Relation indexRelation,
						 Datum *values, bool *isnull,
						 ItemPointer heap_t_ctid,
						 Relation heapRelation,
						 IndexUniqueCheck checkUnique,
						 bool indexUnchanged,
						 struct IndexInfo *indexInfo);
/* Finish index-insert processing for an index AM.
 * The routine delegates any AM-specific cleanup associated with prior inserts.
 *
 * 完成索引 AM 的索引插入处理。
 * 此例程委托执行与先前插入关联的任何 AM 特定清理。
 */
extern void index_insert_cleanup(Relation indexRelation,
								 struct IndexInfo *indexInfo);

/* Begin a tuple-at-a-time index scan.
 * The wrapper creates an AM scan descriptor with heap, snapshot, key, and
 * instrumentation context.
 *
 * 开始一次逐元组索引扫描。
 * 此包装器使用堆、快照、扫描键和统计上下文创建 AM 扫描描述符。
 */
extern IndexScanDesc index_beginscan(Relation heapRelation,
									 Relation indexRelation,
									 Snapshot snapshot,
									 IndexScanInstrumentation *instrument,
									 int nkeys, int norderbys);
/* Begin an index scan that produces a bitmap of matching TIDs.
 * The wrapper initializes AM state for bitmap collection under the snapshot.
 *
 * 开始生成匹配 TID 位图的索引扫描。
 * 此包装器在给定快照下初始化用于收集位图的 AM 状态。
 */
extern IndexScanDesc index_beginscan_bitmap(Relation indexRelation,
											Snapshot snapshot,
											IndexScanInstrumentation *instrument,
											int nkeys);
/* Rescan an index with replacement keys and ordering keys.
 * The existing descriptor is reset, then the AM receives the new scan state.
 *
 * 使用替换扫描键和排序键重新扫描索引。
 * 此例程重置现有描述符，然后将新扫描状态交给 AM。
 */
extern void index_rescan(IndexScanDesc scan,
						 ScanKey keys, int nkeys,
						 ScanKey orderbys, int norderbys);
/* End an index scan and release AM scan resources.
 * The wrapper delegates finalization before discarding the scan descriptor.
 *
 * 结束索引扫描并释放 AM 扫描资源。
 * 此包装器在丢弃扫描描述符前委托执行收尾操作。
 */
extern void index_endscan(IndexScanDesc scan);

/* Save the current index-scan position.
 * The AM records state so a later restore can resume at this point.
 *
 * 保存当前索引扫描位置。
 * AM 记录状态，使后续恢复可以从此位置继续。
 */
extern void index_markpos(IndexScanDesc scan);

/* Restore a previously marked index-scan position.
 * The AM reloads saved scan state and resumes from the marked location.
 *
 * 恢复先前标记的索引扫描位置。
 * AM 重新加载保存的扫描状态，并从标记位置继续。
 */
extern void index_restrpos(IndexScanDesc scan);

/* Estimate space for a parallel index scan.
 * The wrapper combines AM requirements with key, snapshot, worker, and
 * instrumentation state.
 *
 * 估算并行索引扫描所需空间。
 * 此包装器组合 AM 需求以及扫描键、快照、工作进程和统计状态。
 */
extern Size index_parallelscan_estimate(Relation indexRelation,
										int nkeys, int norderbys, Snapshot snapshot,
										bool instrument, bool parallel_aware,
										int nworkers);
/* Initialize shared state for a parallel index scan.
 * The routine lays out AM and instrumentation data in the supplied parallel
 * scan descriptor for workers to attach.
 *
 * 初始化并行索引扫描的共享状态。
 * 此例程在给定并行扫描描述符中布置 AM 和统计数据，供工作进程附接。
 */
extern void index_parallelscan_initialize(Relation heapRelation,
										  Relation indexRelation, Snapshot snapshot,
										  bool instrument, bool parallel_aware,
										  int nworkers,
										  SharedIndexScanInstrumentation **sharedinfo,
										  ParallelIndexScanDesc target);
/* Reset a parallel index scan for another pass.
 * The wrapper coordinates shared scan-state reset with the index AM.
 *
 * 重置并行索引扫描以执行另一遍扫描。
 * 此包装器协调共享扫描状态重置与索引 AM 的处理。
 */
extern void index_parallelrescan(IndexScanDesc scan);

/* Begin a worker attachment to a parallel index scan.
 * The routine builds a local scan descriptor over the supplied shared state.
 *
 * 开始工作进程对并行索引扫描的附接。
 * 此例程在给定共享状态之上构建本地扫描描述符。
 */
extern IndexScanDesc index_beginscan_parallel(Relation heaprel,
											  Relation indexrel,
											  IndexScanInstrumentation *instrument,
											  int nkeys, int norderbys,
											  ParallelIndexScanDesc pscan);
/* Fetch the next matching heap TID from an index scan.
 * The AM advances in the requested direction and returns the next candidate.
 *
 * 从索引扫描中获取下一个匹配的堆 TID。
 * AM 按请求方向前进，并返回下一个候选项。
 */
extern ItemPointer index_getnext_tid(IndexScanDesc scan,
									 ScanDirection direction);
struct TupleTableSlot;

/* Fetch the heap tuple for the scan's current TID.
 * The routine follows the current index result, applies snapshot visibility,
 * and stores the tuple in the supplied slot.
 *
 * 获取扫描当前 TID 对应的堆元组。
 * 此例程跟随当前索引结果、应用快照可见性，并将元组存入给定槽。
 */
extern bool index_fetch_heap(IndexScanDesc scan, struct TupleTableSlot *slot);

/* Fetch the next visible index result directly into a slot.
 * The wrapper combines TID advancement and heap fetch until it finds a row or
 * exhausts the scan.
 *
 * 将下一个可见索引结果直接获取到槽中。
 * 此包装器组合 TID 前进和堆获取，直到找到行或耗尽扫描。
 */
extern bool index_getnext_slot(IndexScanDesc scan, ScanDirection direction,
							   struct TupleTableSlot *slot);

/* Add all matching TIDs from an index scan to a bitmap.
 * The AM executes the bitmap path and returns the number of entries produced.
 *
 * 将索引扫描的全部匹配 TID 添加到位图。
 * AM 执行位图路径并返回生成的条目数。
 */
extern int64 index_getbitmap(IndexScanDesc scan, TIDBitmap *bitmap);

/* Bulk-delete index entries selected by a callback.
 * The wrapper passes vacuum context and callback state to the AM, which
 * removes deletable entries and returns updated statistics.
 *
 * 批量删除由回调选中的索引条目。
 * 此包装器将 VACUUM 上下文和回调状态传给 AM；AM 删除可删除条目并返回更新后的统计信息。
 */
extern IndexBulkDeleteResult *index_bulk_delete(IndexVacuumInfo *info,
												IndexBulkDeleteResult *istat,
												IndexBulkDeleteCallback callback,
												void *callback_state);
/* Complete an index vacuum operation.
 * The wrapper delegates final AM cleanup and returns the final vacuum stats.
 *
 * 完成一次索引 VACUUM 操作。
 * 此包装器委托执行最终 AM 清理，并返回最终 VACUUM 统计信息。
 */
extern IndexBulkDeleteResult *index_vacuum_cleanup(IndexVacuumInfo *info,
												   IndexBulkDeleteResult *istat);

/* Test whether an index can return a column value without heap access.
 * The AM capability check determines whether the requested attribute supports
 * index-only retrieval.
 *
 * 测试索引能否在不访问堆的情况下返回列值。
 * AM 能力检查确定请求属性是否支持仅索引检索。
 */
extern bool index_can_return(Relation indexRelation, int attno);

/* Look up an index support procedure OID.
 * The routine resolves the operator-class procedure number for an attribute.
 *
 * 查找索引支持过程 OID。
 * 此例程解析某个属性的操作符类过程编号。
 */
extern RegProcedure index_getprocid(Relation irel, AttrNumber attnum,
									uint16 procnum);

/* Look up cached function information for an index support procedure.
 * The routine resolves the procedure and returns its invocation metadata.
 *
 * 查找索引支持过程的缓存函数信息。
 * 此例程解析该过程并返回其调用元数据。
 */
extern FmgrInfo *index_getprocinfo(Relation irel, AttrNumber attnum,
								   uint16 procnum);

/* Store ORDER BY distance values on an index scan descriptor.
 * The routine records distances, their types, and recheck state for executor
 * consumption.
 *
 * 在索引扫描描述符上存储 ORDER BY 距离值。
 * 此例程记录距离、其类型和重新检查状态，供执行器使用。
 */
extern void index_store_float8_orderby_distances(IndexScanDesc scan,
												 Oid *orderByTypes,
												 IndexOrderByDistance *distances,
												 bool recheckOrderBy);
/* Obtain operator-class options for an indexed attribute.
 * The routine interprets the attribute option datum and can validate it on
 * request.
 *
 * 获取索引属性的操作符类选项。
 * 此例程解释属性选项 datum，并可按请求验证它。
 */
extern bytea *index_opclass_options(Relation indrel, AttrNumber attnum,
									Datum attoptions, bool validate);


/*
 * index access method support routines (in genam.c)
 */

/*
 * 索引访问方法支持例程（位于 genam.c）。
 */

/* Create a generic index scan descriptor.
 * The routine allocates common descriptor state with capacity for keys and
 * ordering keys before AM initialization.
 *
 * 创建通用索引扫描描述符。
 * 此例程在 AM 初始化前分配可容纳扫描键和排序键的通用描述符状态。
 */
extern IndexScanDesc RelationGetIndexScan(Relation indexRelation,
										  int nkeys, int norderbys);

/* Release a generic index scan descriptor.
 * The routine frees common scan resources after AM-specific cleanup.
 *
 * 释放通用索引扫描描述符。
 * 此例程在 AM 特定清理后释放通用扫描资源。
 */
extern void IndexScanEnd(IndexScanDesc scan);

/* Build a human-readable description of index key values.
 * The routine formats values and NULL markers using relation metadata.
 *
 * 构建索引键值的人类可读描述。
 * 此例程使用关系元数据格式化值和 NULL 标记。
 */
extern char *BuildIndexValueDescription(Relation indexRelation,
										const Datum *values, const bool *isnull);

/* Compute the transaction horizon needed by index tuple processing.
 * The routine examines supplied index-page items and their heap relation to
 * derive the oldest relevant XID.
 *
 * 计算索引元组处理所需的事务范围。
 * 此例程检查给定索引页条目及其堆关系，以得出最早的相关 XID。
 */
extern TransactionId index_compute_xid_horizon_for_tuples(Relation irel,
														  Relation hrel,
														  Buffer ibuf,
														  OffsetNumber *itemnos,
														  int nitems);

/*
 * heap-or-index access to system catalogs (in genam.c)
 */

/*
 * 对系统目录的堆或索引访问（位于 genam.c）。
 */

/* Begin a system-catalog scan using an index when possible.
 * The routine selects heap or index access from the relation, index, snapshot,
 * and scan keys.
 *
 * 在可能时使用索引开始系统目录扫描。
 * 此例程根据关系、索引、快照和扫描键选择堆或索引访问。
 */
extern SysScanDesc systable_beginscan(Relation heapRelation,
									  Oid indexId,
									  bool indexOK,
									  Snapshot snapshot,
									  int nkeys, ScanKey key);
/* Fetch the next tuple from a system-catalog scan.
 * The routine advances the selected access path and returns its next match.
 *
 * 从系统目录扫描中获取下一个元组。
 * 此例程推进所选访问路径并返回其下一个匹配项。
 */
extern HeapTuple systable_getnext(SysScanDesc sysscan);

/* Recheck a system-catalog tuple against scan qualifications.
 * The routine invokes recheck logic when an index result requires validation.
 *
 * 根据扫描条件重新检查系统目录元组。
 * 索引结果需要验证时，此例程调用重新检查逻辑。
 */
extern bool systable_recheck_tuple(SysScanDesc sysscan, HeapTuple tup);

/* End a system-catalog scan.
 * The routine closes its heap or index path and releases scan resources.
 *
 * 结束系统目录扫描。
 * 此例程关闭其堆或索引路径并释放扫描资源。
 */
extern void systable_endscan(SysScanDesc sysscan);

/* Begin an ordered system-catalog scan.
 * The routine initializes an index-backed scan that preserves index order.
 *
 * 开始有序系统目录扫描。
 * 此例程初始化保留索引顺序的索引支持扫描。
 */
extern SysScanDesc systable_beginscan_ordered(Relation heapRelation,
											  Relation indexRelation,
											  Snapshot snapshot,
											  int nkeys, ScanKey key);
/* Fetch the next tuple from an ordered system-catalog scan.
 * The routine advances in the requested direction while retaining index order.
 *
 * 从有序系统目录扫描中获取下一个元组。
 * 此例程按请求方向前进，同时保留索引顺序。
 */
extern HeapTuple systable_getnext_ordered(SysScanDesc sysscan,
										  ScanDirection direction);

/* End an ordered system-catalog scan.
 * The routine releases the ordered scan's index and descriptor state.
 *
 * 结束有序系统目录扫描。
 * 此例程释放有序扫描的索引和描述符状态。
 */
extern void systable_endscan_ordered(SysScanDesc sysscan);

/* Begin an in-place update scan of a system catalog.
 * The routine establishes the access path, obtains the old tuple copy, and
 * returns opaque state for completion or cancellation.
 *
 * 开始系统目录的就地更新扫描。
 * 此例程建立访问路径、取得旧元组副本，并返回供完成或取消使用的不透明状态。
 */
extern void systable_inplace_update_begin(Relation relation,
										  Oid indexId,
										  bool indexOK,
										  Snapshot snapshot,
										  int nkeys, const ScanKeyData *key,
										  HeapTuple *oldtupcopy,
										  void **state);
/* Finish an in-place system-catalog update.
 * The routine applies the replacement tuple through the state created at
 * begin time and releases that state.
 *
 * 完成系统目录的就地更新。
 * 此例程通过开始阶段创建的状态应用替换元组，并释放该状态。
 */
extern void systable_inplace_update_finish(void *state, HeapTuple tuple);

/* Cancel an in-place system-catalog update.
 * The routine abandons the prepared scan/update state without applying a row.
 *
 * 取消系统目录的就地更新。
 * 此例程放弃已准备的扫描/更新状态，而不应用任何行。
 */
extern void systable_inplace_update_cancel(void *state);

#endif							/* GENAM_H */

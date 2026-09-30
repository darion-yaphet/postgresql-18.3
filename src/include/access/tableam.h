/*-------------------------------------------------------------------------
 *
 * tableam.h
 *	  POSTGRES table access method definitions.
 *
 *
 *	  POSTGRES 表访问方法定义。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/tableam.h
 *
 * NOTES
 *		See tableam.sgml for higher level documentation.
 *
 * 注释
 *		更高层级的文档请参见 tableam.sgml。
 *
 *-------------------------------------------------------------------------
 */
#ifndef TABLEAM_H
#define TABLEAM_H

#include "access/relscan.h"
#include "access/sdir.h"
#include "access/xact.h"
#include "executor/tuptable.h"
#include "storage/read_stream.h"
#include "utils/rel.h"
#include "utils/snapshot.h"


#define DEFAULT_TABLE_ACCESS_METHOD	"heap"

/* GUCs */

/* GUC。 */
extern PGDLLIMPORT char *default_table_access_method;
extern PGDLLIMPORT bool synchronize_seqscans;


struct BulkInsertStateData;
struct IndexInfo;
struct SampleScanState;
struct VacuumParams;
struct ValidateIndexState;

/*
 * Bitmask values for the flags argument to the scan_begin callback.
 */

/*
 * scan_begin 回调 flags 参数的位掩码值。
 */
typedef enum ScanOptions
{
	/* one of SO_TYPE_* may be specified */

	/* 可以指定一个 SO_TYPE_*。 */
	SO_TYPE_SEQSCAN = 1 << 0,
	SO_TYPE_BITMAPSCAN = 1 << 1,
	SO_TYPE_SAMPLESCAN = 1 << 2,
	SO_TYPE_TIDSCAN = 1 << 3,
	SO_TYPE_TIDRANGESCAN = 1 << 4,
	SO_TYPE_ANALYZE = 1 << 5,

	/* several of SO_ALLOW_* may be specified */

	/* 可以指定多个 SO_ALLOW_*。 */
	/* allow or disallow use of access strategy */

	/* 允许或禁止使用访问策略。 */
	SO_ALLOW_STRAT = 1 << 6,
	/* report location to syncscan logic? */

	/* 是否向同步扫描逻辑报告位置？ */
	SO_ALLOW_SYNC = 1 << 7,
	/* verify visibility page-at-a-time? */

	/* 是否逐页验证可见性？ */
	SO_ALLOW_PAGEMODE = 1 << 8,

	/* unregister snapshot at scan end? */

	/* 扫描结束时是否注销快照？ */
	SO_TEMP_SNAPSHOT = 1 << 9,
}			ScanOptions;

/*
 * Result codes for table_{update,delete,lock_tuple}, and for visibility
 * routines inside table AMs.
 */

/*
 * table_{update,delete,lock_tuple} 以及表 AM 内部可见性例程的结果代码。
 */
typedef enum TM_Result
{
	/*
	 * Signals that the action succeeded (i.e. update/delete performed, lock
	 * was acquired)
	 */

	/*
	 * 表示操作成功（即已执行更新/删除，或已获取锁）。
	 */
	TM_Ok,

	/* The affected tuple wasn't visible to the relevant snapshot */

	/* 受影响元组对相关快照不可见。 */
	TM_Invisible,

	/* The affected tuple was already modified by the calling backend */

	/* 受影响元组已被调用后端修改。 */
	TM_SelfModified,

	/*
	 * The affected tuple was updated by another transaction. This includes
	 * the case where tuple was moved to another partition.
	 */

	/*
	 * 受影响元组已被另一事务更新。这包括元组被移动到另一分区的情况。
	 */
	TM_Updated,

	/* The affected tuple was deleted by another transaction */

	/* 受影响元组已被另一事务删除。 */
	TM_Deleted,

	/*
	 * The affected tuple is currently being modified by another session. This
	 * will only be returned if table_(update/delete/lock_tuple) are
	 * instructed not to wait.
	 */

	/*
	 * 受影响元组当前正由另一会话修改。只有在指示
	 * table_(update/delete/lock_tuple) 不等待时才会返回此结果。
	 */
	TM_BeingModified,

	/* lock couldn't be acquired, action skipped. Only used by lock_tuple */

	/* 无法获取锁，操作被跳过。仅由 lock_tuple 使用。 */
	TM_WouldBlock,
} TM_Result;

/*
 * Result codes for table_update(..., update_indexes*..).
 * Used to determine which indexes to update.
 */

/*
 * table_update(..., update_indexes*..) 的结果代码。
 * 用于确定应更新哪些索引。
 */
typedef enum TU_UpdateIndexes
{
	/* No indexed columns were updated (incl. TID addressing of tuple) */

	/* 没有更新任何已索引列（包括元组的 TID 地址）。 */
	TU_None,

	/* A non-summarizing indexed column was updated, or the TID has changed */

	/* 更新了非汇总已索引列，或 TID 已变化。 */
	TU_All,

	/* Only summarized columns were updated, TID is unchanged */

	/* 仅更新了已汇总列，TID 未变化。 */
	TU_Summarizing,
} TU_UpdateIndexes;

/*
 * When table_tuple_update, table_tuple_delete, or table_tuple_lock fail
 * because the target tuple is already outdated, they fill in this struct to
 * provide information to the caller about what happened. When those functions
 * succeed, the contents of this struct should not be relied upon, except for
 * `traversed`, which may be set in both success and failure cases.
 *
 * ctid is the target's ctid link: it is the same as the target's TID if the
 * target was deleted, or the location of the replacement tuple if the target
 * was updated.
 *
 * xmax is the outdating transaction's XID.  If the caller wants to visit the
 * replacement tuple, it must check that this matches before believing the
 * replacement is really a match.  This is InvalidTransactionId if the target
 * was !LP_NORMAL (expected only for a TID retrieved from syscache).
 *
 * cmax is the outdating command's CID, but only when the failure code is
 * TM_SelfModified (i.e., something in the current transaction outdated the
 * tuple); otherwise cmax is zero.  (We make this restriction because
 * HeapTupleHeaderGetCmax doesn't work for tuples outdated in other
 * transactions.)
 *
 * traversed indicates if an update chain was followed in order to try to lock
 * the target tuple.  (This may be set in both success and failure cases.)
 */

/*
 * 当 table_tuple_update、table_tuple_delete 或 table_tuple_lock 因目标元组已
 * 过期而失败时，它们会填充此结构，向调用方提供发生情况的信息。这些函数成功时，
 * 不应依赖本结构内容，除了 traversed；它可能在成功和失败时都被设置。
 *
 * ctid 是目标的 ctid 链接：目标被删除时它等于目标 TID；目标被更新时它是替换元组的位置。
 *
 * xmax 是使目标过期事务的 XID。若调用方想访问替换元组，必须先检查其匹配，才能相信
 * 替换确实是匹配项。若目标不是 LP_NORMAL（预期仅来自 syscache 的 TID），该值为
 * InvalidTransactionId。
 *
 * cmax 是使目标过期命令的 CID，但仅在失败代码为 TM_SelfModified（即当前事务中的内容
 * 使元组过期）时有效；否则 cmax 为零。（作此限制是因为 HeapTupleHeaderGetCmax 无法
 * 处理由其他事务使其过期的元组。）
 *
 * traversed 表明是否为尝试锁定目标元组而跟随了更新链。（成功和失败情况都可设置。）
 */
typedef struct TM_FailureData
{
	ItemPointerData ctid;
	TransactionId xmax;
	CommandId	cmax;
	bool		traversed;
} TM_FailureData;

/*
 * State used when calling table_index_delete_tuples().
 *
 * Represents the status of table tuples, referenced by table TID and taken by
 * index AM from index tuples.  State consists of high level parameters of the
 * deletion operation, plus two mutable palloc()'d arrays for information
 * about the status of individual table tuples.  These are conceptually one
 * single array.  Using two arrays keeps the TM_IndexDelete struct small,
 * which makes sorting the first array (the deltids array) fast.
 *
 * Some index AM callers perform simple index tuple deletion (by specifying
 * bottomup = false), and include only known-dead deltids.  These known-dead
 * entries are all marked knowndeletable = true directly (typically these are
 * TIDs from LP_DEAD-marked index tuples), but that isn't strictly required.
 *
 * Callers that specify bottomup = true are "bottom-up index deletion"
 * callers.  The considerations for the tableam are more subtle with these
 * callers because they ask the tableam to perform highly speculative work,
 * and might only expect the tableam to check a small fraction of all entries.
 * Caller is not allowed to specify knowndeletable = true for any entry
 * because everything is highly speculative.  Bottom-up caller provides
 * context and hints to tableam -- see comments below for details on how index
 * AMs and tableams should coordinate during bottom-up index deletion.
 *
 * Simple index deletion callers may ask the tableam to perform speculative
 * work, too.  This is a little like bottom-up deletion, but not too much.
 * The tableam will only perform speculative work when it's practically free
 * to do so in passing for simple deletion caller (while always performing
 * whatever work is needed to enable knowndeletable/LP_DEAD index tuples to
 * be deleted within index AM).  This is the real reason why it's possible for
 * simple index deletion caller to specify knowndeletable = false up front
 * (this means "check if it's possible for me to delete corresponding index
 * tuple when it's cheap to do so in passing").  The index AM should only
 * include "extra" entries for index tuples whose TIDs point to a table block
 * that tableam is expected to have to visit anyway (in the event of a block
 * orientated tableam).  The tableam isn't strictly obligated to check these
 * "extra" TIDs, but a block-based AM should always manage to do so in
 * practice.
 *
 * The final contents of the deltids/status arrays are interesting to callers
 * that ask tableam to perform speculative work (i.e. when _any_ items have
 * knowndeletable set to false up front).  These index AM callers will
 * naturally need to consult final state to determine which index tuples are
 * in fact deletable.
 *
 * The index AM can keep track of which index tuple relates to which deltid by
 * setting idxoffnum (and/or relying on each entry being uniquely identifiable
 * using tid), which is important when the final contents of the array will
 * need to be interpreted -- the array can shrink from initial size after
 * tableam processing and/or have entries in a new order (tableam may sort
 * deltids array for its own reasons).  Bottom-up callers may find that final
 * ndeltids is 0 on return from call to tableam, in which case no index tuple
 * deletions are possible.  Simple deletion callers can rely on any entries
 * they know to be deletable appearing in the final array as deletable.
 */

/*
 * 调用 table_index_delete_tuples() 时使用的状态。
 *
 * 表示表元组的状态；索引 AM 从索引元组取得这些元组的表 TID。状态由删除操作的高层
 * 参数及两个可变的、通过 palloc() 分配的数组构成，用于保存单个表元组的状态信息。
 * 它们在概念上是一个数组；分开可使 TM_IndexDelete 结构较小，从而快速排序第一个数组
 * （deltids 数组）。
 *
 * 一些索引 AM 调用方执行简单索引元组删除（指定 bottomup = false），且只包含已知死亡的
 * deltids。这些已知死亡条目会直接标记为 knowndeletable = true（通常是来自 LP_DEAD 标记
 * 索引元组的 TID），但这不是严格要求。
 *
 * 指定 bottomup = true 的调用方是“自底向上索引删除”调用方。表 AM 的考量更微妙，因为
 * 这些调用方要求表 AM 执行高度推测性的工作，且可能只期望表 AM 检查所有条目中的一小部分。
 * 调用方不得将任何条目标为 knowndeletable = true，因为一切都是高度推测性的。自底向上
 * 调用方会向表 AM 提供上下文和提示；有关索引 AM 与表 AM 在自底向上删除期间如何协调，
 * 请参见下文注释。
 *
 * 简单索引删除调用方也可能要求表 AM 执行推测性工作。这与自底向上删除有些相似，但并不
 * 完全相同。表 AM 仅在简单删除调用方处理中几乎免费时才会执行推测性工作，同时始终执行
 * 允许在索引 AM 内删除 knowndeletable/LP_DEAD 索引元组所需的工作。这也是简单删除调用方
 * 可以预先指定 knowndeletable = false 的真正原因（其含义是“当顺便执行成本低时，检查我
 * 是否可以删除对应索引元组”）。索引 AM 应只为 TID 指向表 AM 无论如何都必须访问的表块的
 * 索引元组包含“额外”条目（对面向块的表 AM）。表 AM 并无严格义务检查这些“额外”TID，
 * 但基于块的 AM 在实践中总应能做到。
 *
 * 最终 deltids/status 数组内容对要求表 AM 执行推测性工作的调用方很重要（即初始时有任意
 * 条目的 knowndeletable 为 false）。这些索引 AM 调用方自然需要查阅最终状态，以确定哪些
 * 索引元组实际可删除。
 *
 * 索引 AM 可通过设置 idxoffnum（和/或依赖每项可通过 tid 唯一标识）跟踪哪个索引元组对应
 * 哪个 deltid；当需要解释最终数组内容时这很重要——表 AM 处理后数组可缩小，和/或条目可
 * 采用新顺序（表 AM 可出于自身原因排序 deltids 数组）。自底向上调用方从表 AM 返回时可能
 * 发现最终 ndeltids 为 0，此时无法删除任何索引元组。简单删除调用方可依赖其已知可删除的
 * 条目在最终数组中仍表现为可删除。
 */
typedef struct TM_IndexDelete
{
	ItemPointerData tid;		/* table TID from index tuple */

									/* 来自索引元组的表 TID。 */
	int16		id;				/* Offset into TM_IndexStatus array */

									/* TM_IndexStatus 数组中的偏移量。 */
} TM_IndexDelete;

typedef struct TM_IndexStatus
{
	OffsetNumber idxoffnum;		/* Index am page offset number */

									/* 索引 AM 页面偏移编号。 */
	bool		knowndeletable; /* Currently known to be deletable? */

									/* 当前是否已知可删除？ */

	/* Bottom-up index deletion specific fields follow */

	/* 以下是自底向上索引删除专用字段。 */
	bool		promising;		/* Promising (duplicate) index tuple? */

									/* 有希望的（重复）索引元组？ */
	int16		freespace;		/* Space freed in index if deleted */

									/* 删除时在索引中释放的空间。 */
} TM_IndexStatus;

/*
 * Index AM/tableam coordination is central to the design of bottom-up index
 * deletion.  The index AM provides hints about where to look to the tableam
 * by marking some entries as "promising".  Index AM does this with duplicate
 * index tuples that are strongly suspected to be old versions left behind by
 * UPDATEs that did not logically modify indexed values.  Index AM may find it
 * helpful to only mark entries as promising when they're thought to have been
 * affected by such an UPDATE in the recent past.
 *
 * Bottom-up index deletion casts a wide net at first, usually by including
 * all TIDs on a target index page.  It is up to the tableam to worry about
 * the cost of checking transaction status information.  The tableam is in
 * control, but needs careful guidance from the index AM.  Index AM requests
 * that bottomupfreespace target be met, while tableam measures progress
 * towards that goal by tallying the per-entry freespace value for known
 * deletable entries. (All !bottomup callers can just set these space related
 * fields to zero.)
 */

/*
 * 索引 AM/表 AM 协调是自底向上索引删除设计的核心。索引 AM 通过将某些条目标记为
 * “promising”来向表 AM 提供查找位置提示。索引 AM 对强烈怀疑是 UPDATE 遗留旧版本的
 * 重复索引元组进行此标记；这些 UPDATE 在逻辑上没有修改已索引值。仅在认为其最近受此类
 * UPDATE 影响时，索引 AM 可能才适合标记条目。
 *
 * 自底向上索引删除最初通常会包含目标索引页上的所有 TID，以形成宽泛候选集。表 AM 决定
 * 检查事务状态信息的成本，但需要索引 AM 的审慎指导。索引 AM 请求满足 bottomupfreespace
 * 目标，表 AM 通过汇总已知可删除条目的每项 freespace 值来度量向目标的进展。
 * （所有 !bottomup 调用方都可将这些空间相关字段设为零。）
 */
typedef struct TM_IndexDeleteOp
{
	Relation	irel;			/* Target index relation */

									/* 目标索引关系。 */
	BlockNumber iblknum;		/* Index block number (for error reports) */

									/* 索引块编号（用于错误报告）。 */
	bool		bottomup;		/* Bottom-up (not simple) deletion? */

									/* 是否为自底向上（而非简单）删除？ */
	int			bottomupfreespace;	/* Bottom-up space target */

									/* 自底向上空间目标。 */

	/* Mutable per-TID information follows (index AM initializes entries) */

	/* 以下为可变的逐 TID 信息（索引 AM 初始化条目）。 */
	int			ndeltids;		/* Current # of deltids/status elements */

									/* 当前 deltids/status 元素数量。 */
	TM_IndexDelete *deltids;
	TM_IndexStatus *status;
} TM_IndexDeleteOp;

/* "options" flag bits for table_tuple_insert */

/* table_tuple_insert 的 options 标志位。 */
/* TABLE_INSERT_SKIP_WAL was 0x0001; RelationNeedsWAL() now governs */

/* TABLE_INSERT_SKIP_WAL 为 0x0001； RelationNeedsWAL() 现在管辖 */
#define TABLE_INSERT_SKIP_FSM		0x0002
#define TABLE_INSERT_FROZEN			0x0004
#define TABLE_INSERT_NO_LOGICAL		0x0008

/* flag bits for table_tuple_lock */

/* table_tuple_lock 的标志位 */
/* Follow tuples whose update is in progress if lock modes don't conflict  */

/* 如果锁定模式不冲突，则跟踪正在进行更新的元组 */
#define TUPLE_LOCK_FLAG_LOCK_UPDATE_IN_PROGRESS	(1 << 0)
/* Follow update chain and lock latest version of tuple */

/* 跟踪更新链并锁定元组的最新版本 */
#define TUPLE_LOCK_FLAG_FIND_LAST_VERSION		(1 << 1)


/* Typedef for callback function for table_index_build_scan */

/* table_index_build_scan 回调函数的 Typedef */
/*
 * Function: IndexBuildCallback.
 * Purpose: Processes one tuple while building an index.
 * Core flow: The index build scan invokes this callback with the tuple identifier, indexed values, null flags, and liveness state so that the index can record the entry.
 *
 * 函数：IndexBuildCallback。
 * 作用：在构建索引时处理一个元组。
 * 核心流程：索引构建扫描使用元组标识符、索引值、空标志和活动状态调用此回调，以便索引可以记录条目。
 */
typedef void (*IndexBuildCallback) (Relation index,
									ItemPointer tid,
									Datum *values,
									bool *isnull,
									bool tupleIsAlive,
									void *state);

/*
 * API struct for a table AM.  Note this must be allocated in a
 * server-lifetime manner, typically as a static const struct, which then gets
 * returned by FormData_pg_am.amhandler.
 *
 * 表访问方法 API 结构及其回调注册约束。
 *
 * In most cases it's not appropriate to call the callbacks directly, use the
 * table_* wrapper functions instead.
 *
 * 在大多数情况下，直接调用回调是不合适的，应使用 table_* 包装函数。
 *
 * GetTableAmRoutine() asserts that required callbacks are filled in, remember
 * to update when adding a callback.
 *
 * GetTableAmRoutine() 会验证所有必需的回调。
 */
typedef struct TableAmRoutine
{
	/* this must be set to T_TableAmRoutine */

	/* 必须将其设置为 T_TableAmRoutine */
	NodeTag		type;


	/* ------------------------------------------------------------------------
	 * Slot related callbacks.
 *
 * 与插槽相关的回调。
	 * ------------------------------------------------------------------------
	 */

	/*
	 * Return slot implementation suitable for storing a tuple of this AM.
 *
  * 返回槽实现适合存储此 AM 的元组。
	 */
	/*
	 * Function: slot_callbacks.
	 * Purpose: Selects slot operations that can store tuples of this access method.
	 * Core flow: It inspects the relation and returns the TupleTableSlotOps implementation used to create compatible tuple slots.
	 *
	 * 函数：slot_callbacks。
	 * 作用：选择可以存储此访问方法的元组的槽操作。
	 * 核心流程：它检查关系并返回用于创建兼容元组槽的 TupleTableSlotOps 实现。
	 */
	const TupleTableSlotOps *(*slot_callbacks) (Relation rel);


	/* ------------------------------------------------------------------------
	 * Table scan callbacks.
 *
  * 表扫描回调。
	 * ------------------------------------------------------------------------
	 */

	/*
	 * Start a scan of `rel`.  The callback has to return a TableScanDesc,
	 * which will typically be embedded in a larger, AM specific, struct.
 *
  * 开始扫描 `rel`。  回调必须返回一个 TableScanDesc，它通常嵌入到一个更大的、AM 特定的结构中。
	 *
	 * If nkeys != 0, the results need to be filtered by those scan keys.
 *
  * 如果nkeys != 0，则需要通过那些扫描键来过滤结果。
	 *
	 * pscan, if not NULL, will have already been initialized with
	 * parallelscan_initialize(), and has to be for the same relation. Will
	 * only be set coming from table_beginscan_parallel().
 *
  * pscan 如果不为 NULL，则已使用 parallelscan_initialize() 进行初始化，并且必须用于相同的关系。只能从 table_beginscan_parallel() 进行设置。
	 *
	 * `flags` is a bitmask indicating the type of scan (ScanOptions's
	 * SO_TYPE_*, currently only one may be specified), options controlling
	 * the scan's behaviour (ScanOptions's SO_ALLOW_*, several may be
	 * specified, an AM may ignore unsupported ones) and whether the snapshot
	 * needs to be deallocated at scan_end (ScanOptions's SO_TEMP_SNAPSHOT).
 *
 * table_tuple_insert 的 options 标志位。
	 */
	/*
	 * Function: scan_begin.
	 * Purpose: Starts a table scan and creates its scan descriptor.
	 * Core flow: It records the relation, snapshot, scan keys, optional parallel state, and scan flags, then returns the descriptor used by subsequent scan callbacks.
	 *
	 * 函数：scan_begin。
	 * 作用：启动表扫描并创建其扫描描述符。
	 * 核心流程：它记录关系、快照、扫描键、可选并行状态和扫描标志，然后返回后续扫描回调使用的描述符。
	 */
	TableScanDesc (*scan_begin) (Relation rel,
								 Snapshot snapshot,
								 int nkeys, struct ScanKeyData *key,
								 ParallelTableScanDesc pscan,
								 uint32 flags);

	/*
	 * Release resources and deallocate scan. If TableScanDesc.temp_snap,
	 * TableScanDesc.rs_snapshot needs to be unregistered.
 *
  * 释放资源并取消分配扫描。如果是TableScanDesc.temp_snap，则需要注销TableScanDesc.rs_snapshot。
	 */
	/*
	 * Function: scan_end.
	 * Purpose: Releases a completed table scan.
	 * Core flow: It frees scan resources and unregisters the snapshot when the descriptor owns a temporary snapshot.
	 *
	 * 函数：scan_end。
	 * 作用：释放已完成的表扫描。
	 * 核心流程：当描述符拥有临时快照时，它会释放扫描资源并取消注册快照。
	 */
	void		(*scan_end) (TableScanDesc scan);

	/*
	 * Restart relation scan.  If set_params is set to true, allow_{strat,
	 * sync, pagemode} (see scan_begin) changes should be taken into account.
 *
  * 重新启动关系扫描。  如果 set_params 设置为 true，则应考虑allow_{strat,sync,pagemode}（请参阅 scan_begin）更改。
	 */
	/*
	 * Function: scan_rescan.
	 * Purpose: Restarts an existing table scan.
	 * Core flow: It resets scan position and keys, and applies changed buffer strategy, synchronization, and page-mode options when requested.
	 *
	 * 函数：scan_rescan。
	 * 作用：重新启动现有的表扫描。
	 * 核心流程：它会重置扫描位置和键，并在请求时应用更改的缓冲区策略、同步和页面模式选项。
	 */
	void		(*scan_rescan) (TableScanDesc scan, struct ScanKeyData *key,
								bool set_params, bool allow_strat,
								bool allow_sync, bool allow_pagemode);

	/*
	 * Return next tuple from `scan`, store in slot.
 *
  * 从“scan”返回下一个元组，存储在槽中。
	 */
	/*
	 * Function: scan_getnextslot.
	 * Purpose: Fetches the next tuple of a table scan into a slot.
	 * Core flow: It advances in the requested direction, checks the scan conditions, stores a matching tuple in the supplied slot, and reports whether one was found.
	 *
	 * 函数：scan_getnextslot。
	 * 作用：将表扫描的下一个元组取出到槽中。
	 * 核心流程：它沿着请求的方向前进，检查扫描条件，在提供的槽中存储匹配的元组，并报告是否找到了一个。
	 */
	bool		(*scan_getnextslot) (TableScanDesc scan,
									 ScanDirection direction,
									 TupleTableSlot *slot);

	/*-----------
	 * Optional functions to provide scanning for ranges of ItemPointers.
	 * Implementations must either provide both of these functions, or neither
	 * of them.
 *
  * 提供扫描 ItemPointers 范围的可选功能。实现必须要么提供这两个功能，要么都不提供。
	 *
	 * Implementations of scan_set_tidrange must themselves handle
	 * ItemPointers of any value. i.e, they must handle each of the following:
 *
  * scan_set_tidrange 的实现本身必须处理任何值的 ItemPointers。即，他们必须处理以下各项：
	 *
	 * 1) mintid or maxtid is beyond the end of the table; and
	 * 2) mintid is above maxtid; and
	 * 3) item offset for mintid or maxtid is beyond the maximum offset
	 * allowed by the AM.
 *
  * 1）mintid或maxtid超出表尾； 2) mintid 高于 maxtid； 3) mintid 或 maxtid 的项目偏移量超出了 AM 允许的最大偏移量。
	 *
	 * Implementations can assume that scan_set_tidrange is always called
	 * before scan_getnextslot_tidrange or after scan_rescan and before any
	 * further calls to scan_getnextslot_tidrange.
 *
  * 实现可以假设 scan_set_tidrange 始终在 scan_getnextslot_tidrange 之前或 scan_rescan 之后以及对 scan_getnextslot_tidrange 的任何进一步调用之前调用。
	 */
	/*
	 * Function: scan_set_tidrange.
	 * Purpose: Sets the TID bounds for a range scan.
	 * Core flow: It records the inclusive minimum and maximum item pointers so later range fetches return only tuples inside that interval.
	 *
	 * 函数：scan_set_tidrange。
	 * 作用：设置范围扫描的 TID 界限。
	 * 核心流程：它记录了包含的最小和最大项指针，因此以后的范围提取仅返回该间隔内的元组。
	 */
	void		(*scan_set_tidrange) (TableScanDesc scan,
									  ItemPointer mintid,
									  ItemPointer maxtid);

	/*
	 * Return next tuple from `scan` that's in the range of TIDs defined by
	 * scan_set_tidrange.
 *
  * 从 scan_set_tidrange 定义的 TID 范围内的“scan”返回下一个元组。
	 */
	/*
	 * Function: scan_getnextslot_tidrange.
	 * Purpose: Fetches the next tuple within the configured TID range.
	 * Core flow: It advances the scan in the requested direction, stores the next in-range tuple in the slot, and reports whether one was found.
	 *
	 * 函数：scan_getnextslot_tidrange。
	 * 作用：获取配置的 TID 范围内的下一个元组。
	 * 核心流程：它按照请求的方向推进扫描，将下一个范围内的元组存储在槽中，并报告是否找到一个元组。
	 */
	bool		(*scan_getnextslot_tidrange) (TableScanDesc scan,
											  ScanDirection direction,
											  TupleTableSlot *slot);

	/* ------------------------------------------------------------------------
	 * Parallel table scan related functions.
 *
  * 并行表扫描相关函数。
	 * ------------------------------------------------------------------------
	 */

	/*
	 * Estimate the size of shared memory needed for a parallel scan of this
	 * relation. The snapshot does not need to be accounted for.
 *
  * 估计并行扫描该关系所需的共享内存大小。不需要考虑快照。
	 */
	/*
	 * Function: parallelscan_estimate.
	 * Purpose: Estimates shared-memory space for a parallel table scan.
	 * Core flow: It computes the access-method-specific portion of the parallel scan descriptor; the snapshot is excluded from this estimate.
	 *
	 * 函数：parallelscan_estimate。
	 * 作用：估计并行表扫描的共享内存空间。
	 * 核心流程：它计算并行扫描描述符的特定于访问方法的部分；该快照被排除在该估计之外。
	 */
	Size		(*parallelscan_estimate) (Relation rel);

	/*
	 * Initialize ParallelTableScanDesc for a parallel scan of this relation.
	 * `pscan` will be sized according to parallelscan_estimate() for the same
	 * relation.
 *
  * 初始化 ParallelTableScanDesc 以并行扫描此关系。对于相同的关系，‘pscan’将根据parallelscan_estimate()调整大小。
	 */
	/*
	 * Function: parallelscan_initialize.
	 * Purpose: Initializes shared state for a parallel table scan.
	 * Core flow: It fills the preallocated ParallelTableScanDesc for the relation and returns the amount of access-method state initialized.
	 *
	 * 函数：parallelscan_initialize。
	 * 作用：初始化并行表扫描的共享状态。
	 * 核心流程：它填充关系的预分配 ParallelTableScanDesc 并返回初始化的访问方法状态量。
	 */
	Size		(*parallelscan_initialize) (Relation rel,
											ParallelTableScanDesc pscan);

	/*
	 * Reinitialize `pscan` for a new scan. `rel` will be the same relation as
	 * when `pscan` was initialized by parallelscan_initialize.
 *
  * 重新初始化“pscan”以进行新扫描。 `rel` 将与通过 parallelscan_initialize 初始化 `pscan` 时的关系相同。
	 */
	/*
	 * Function: parallelscan_reinitialize.
	 * Purpose: Resets shared state for another parallel scan.
	 * Core flow: It restores the ParallelTableScanDesc to its initial scan state for the same relation.
	 *
	 * 函数：parallelscan_reinitialize。
	 * 作用：重置另一个并行扫描的共享状态。
	 * 核心流程：它将 ParallelTableScanDesc 恢复到同一关系的初始扫描状态。
	 */
	void		(*parallelscan_reinitialize) (Relation rel,
											  ParallelTableScanDesc pscan);


	/* ------------------------------------------------------------------------
	 * Index Scan Callbacks
 *
  * 索引扫描回调
	 * ------------------------------------------------------------------------
	 */

	/*
	 * Prepare to fetch tuples from the relation, as needed when fetching
	 * tuples for an index scan.  The callback has to return an
	 * IndexFetchTableData, which the AM will typically embed in a larger
	 * structure with additional information.
 *
  * 在为索引扫描获取元组时，根据需要准备从关系中获取元组。  回调必须返回 IndexFetchTableData，AM 通常会将其嵌入到带有附加信息的更大结构中。
	 *
	 * Tuples for an index scan can then be fetched via index_fetch_tuple.
 *
  * 然后可以通过index_fetch_tuple 获取用于索引扫描的元组。
	 */
	/*
	 * Function: index_fetch_begin.
	 * Purpose: Prepares relation tuple fetching for an index scan.
	 * Core flow: It allocates and returns access-method fetch state that later index_fetch_tuple calls use to retrieve heap tuples.
	 *
	 * 函数：index_fetch_begin。
	 * 作用：为索引扫描准备关系元组获取。
	 * 核心流程：它分配并返回访问方法获取状态，稍后使用 index_fetch_tuple 调用来检索堆元组。
	 */
	struct IndexFetchTableData *(*index_fetch_begin) (Relation rel);

	/*
	 * Reset index fetch. Typically this will release cross index fetch
	 * resources held in IndexFetchTableData.
 *
  * 重置索引获取。通常，这将释放 IndexFetchTableData 中保存的交叉索引获取资源。
	 */
	/*
	 * Function: index_fetch_reset.
	 * Purpose: Resets index-fetch state between index scans.
	 * Core flow: It releases resources retained across tuple fetches while preserving the fetch descriptor for reuse.
	 *
	 * 函数：index_fetch_reset。
	 * 作用：在索引扫描之间重置索引获取状态。
	 * 核心流程：它释放跨元组提取保留的资源，同时保留提取描述符以供重用。
	 */
	void		(*index_fetch_reset) (struct IndexFetchTableData *data);

	/*
	 * Release resources and deallocate index fetch.
 *
  * 释放资源并取消分配索引提取。
	 */
	/*
	 * Function: index_fetch_end.
	 * Purpose: Ends an index tuple fetch operation.
	 * Core flow: It releases all resources owned by the IndexFetchTableData descriptor and deallocates that descriptor.
	 *
	 * 函数：index_fetch_end。
	 * 作用：结束索引元组获取操作。
	 * 核心流程：它释放 IndexFetchTableData 描述符拥有的所有资源并释放该描述符。
	 */
	void		(*index_fetch_end) (struct IndexFetchTableData *data);

	/*
	 * Fetch tuple at `tid` into `slot`, after doing a visibility test
	 * according to `snapshot`. If a tuple was found and passed the visibility
	 * test, return true, false otherwise.
 *
  * 根据“snapshot”进行可见性测试后，将“tid”处的元组提取到“slot”中。如果找到元组并通过可见性测试，则返回 true，否则返回 false。
	 *
	 * Note that AMs that do not necessarily update indexes when indexed
	 * columns do not change, need to return the current/correct version of
	 * the tuple that is visible to the snapshot, even if the tid points to an
	 * older version of the tuple.
 *
  * 请注意，当索引列不更改时，AM 不一定会更新索引，因此需要返回快照可见的元组的当前/正确版本，即使 tid 指向元组的旧版本也是如此。
	 *
	 * *call_again is false on the first call to index_fetch_tuple for a tid.
	 * If there potentially is another tuple matching the tid, *call_again
	 * needs to be set to true by index_fetch_tuple, signaling to the caller
	 * that index_fetch_tuple should be called again for the same tid.
 *
  * *call_again 在第一次调用 tid 的 index_fetch_tuple 时为 false。如果可能存在另一个与 tid 匹配的元组，则需要通过 index_fetch_tuple 将 *call_again 设置为 true，向调用者发出信号，表明应针对同一 tid 再次调用 index_fetch_tuple。
	 *
	 * *all_dead, if all_dead is not NULL, should be set to true by
	 * index_fetch_tuple iff it is guaranteed that no backend needs to see
	 * that tuple. Index AMs can use that to avoid returning that tid in
	 * future searches.
 *
  * *all_dead，如果all_dead不为NULL，则应通过index_fetch_tuple设置为true，前提是保证没有后端需要看到该元组。索引 AM 可以使用它来避免在将来的搜索中返回该 tid。
	 */
	/*
	 * Function: index_fetch_tuple.
	 * Purpose: Fetches a visible tuple for an index TID.
	 * Core flow: It follows the supplied TID under the snapshot, stores the visible version in the slot, and reports retry or all-dead state when applicable.
	 *
	 * 函数：index_fetch_tuple。
	 * 作用：获取索引 TID 的可见元组。
	 * 核心流程：它遵循快照下提供的 TID，将可见版本存储在槽中，并在适用时报告重试或全死状态。
	 */
	bool		(*index_fetch_tuple) (struct IndexFetchTableData *scan,
									  ItemPointer tid,
									  Snapshot snapshot,
									  TupleTableSlot *slot,
									  bool *call_again, bool *all_dead);


	/* ------------------------------------------------------------------------
	 * Callbacks for non-modifying operations on individual tuples
 *
  * 对单个元组的非修改操作的回调
	 * ------------------------------------------------------------------------
	 */

	/*
	 * Fetch tuple at `tid` into `slot`, after doing a visibility test
	 * according to `snapshot`. If a tuple was found and passed the visibility
	 * test, returns true, false otherwise.
 *
  * 根据“snapshot”进行可见性测试后，将“tid”处的元组提取到“slot”中。如果找到元组并通过可见性测试，则返回 true，否则返回 false。
	 */
	/*
	 * Function: tuple_fetch_row_version.
	 * Purpose: Fetches a row version visible to a snapshot.
	 * Core flow: It finds the tuple at the supplied TID, applies the visibility test, and stores a visible version in the destination slot.
	 *
	 * 函数：tuple_fetch_row_version。
	 * 作用：获取快照可见的行版本。
	 * 核心流程：它在提供的 TID 处查找元组，应用可见性测试，并将可见版本存储在目标槽中。
	 */
	bool		(*tuple_fetch_row_version) (Relation rel,
											ItemPointer tid,
											Snapshot snapshot,
											TupleTableSlot *slot);

	/*
	 * Is tid valid for a scan of this relation.
 *
  * tid 对于扫描该关系是否有效。
	 */
	/*
	 * Function: tuple_tid_valid.
	 * Purpose: Checks whether a TID is valid for the scan relation.
	 * Core flow: It evaluates the supplied item pointer against the relation and scan state and returns whether it can identify a tuple.
	 *
	 * 函数：tuple_tid_valid。
	 * 作用：检查 TID 对于扫描关系是​​否有效。
	 * 核心流程：它根据关系和扫描状态评估提供的项指针，并返回它是否可以识别元组。
	 */
	bool		(*tuple_tid_valid) (TableScanDesc scan,
									ItemPointer tid);

	/*
	 * Return the latest version of the tuple at `tid`, by updating `tid` to
	 * point at the newest version.
 *
  * 通过更新“tid”以指向最新版本，返回“tid”处元组的最新版本。
	 */
	/*
	 * Function: tuple_get_latest_tid.
	 * Purpose: Finds the newest version of a tuple.
	 * Core flow: It follows the update chain starting at the input TID and overwrites that TID with the latest version.
	 *
	 * 函数：tuple_get_latest_tid。
	 * 作用：查找元组的最新版本。
	 * 核心流程：它遵循从输入 TID 开始的更新链，并用最新版本覆盖该 TID。
	 */
	void		(*tuple_get_latest_tid) (TableScanDesc scan,
										 ItemPointer tid);

	/*
	 * Does the tuple in `slot` satisfy `snapshot`?  The slot needs to be of
	 * the appropriate type for the AM.
 *
  * ‘slot’中的元组是否满足‘snapshot’？  该插槽的类型必须适合 AM。
	 */
	/*
	 * Function: tuple_satisfies_snapshot.
	 * Purpose: Checks whether a tuple slot is visible to a snapshot.
	 * Core flow: It evaluates the tuple represented by the slot using the relation and snapshot and returns the visibility result.
	 *
	 * 函数：tuple_satisfies_snapshot。
	 * 作用：检查元组槽对快照是否可见。
	 * 核心流程：它使用关系和快照评估由槽表示的元组，并返回可见性结果。
	 */
	bool		(*tuple_satisfies_snapshot) (Relation rel,
											 TupleTableSlot *slot,
											 Snapshot snapshot);

	/* see table_index_delete_tuples() */

	/* 请参见 table_index_delete_tuples() */
	/*
	 * Function: index_delete_tuples.
	 * Purpose: Removes index entries for deleted tuples.
	 * Core flow: It processes the supplied index-delete state for the relation and returns the transaction ID relevant to the deletion work.
	 *
	 * 函数：index_delete_tuples。
	 * 作用：删除已删除元组的索引条目。
	 * 核心流程：它处理为关系提供的索引删除状态，并返回与删除工作相关的事务 ID。
	 */
	TransactionId (*index_delete_tuples) (Relation rel,
										  TM_IndexDeleteOp *delstate);


	/* ------------------------------------------------------------------------
	 * Manipulations of physical tuples.
 *
 * 物理元组的操作。
	 * ------------------------------------------------------------------------
	 */

	/* see table_tuple_insert() for reference about parameters */

	/* 有关参数的参考，请参阅 table_tuple_insert() */
	/*
	 * Function: tuple_insert.
	 * Purpose: Inserts one tuple into a relation.
	 * Core flow: It takes the tuple from the slot, applies the command and insertion options, and uses the optional bulk-insert state to record it.
	 *
	 * 函数：tuple_insert。
	 * 作用：将一个元组插入到关系中。
	 * 核心流程：它从槽中获取元组，应用命令和插入选项，并使用可选的批量插入状态来记录它。
	 */
	void		(*tuple_insert) (Relation rel, TupleTableSlot *slot,
								 CommandId cid, int options,
								 struct BulkInsertStateData *bistate);

	/* see table_tuple_insert_speculative() for reference about parameters */

	/* 有关参数的参考，请参阅 table_tuple_insert_speculative() */
	/*
	 * Function: tuple_insert_speculative.
	 * Purpose: Inserts one tuple speculatively.
	 * Core flow: It writes the tuple with the speculative token so a later completion callback can confirm or cancel the insertion.
	 *
	 * 函数：tuple_insert_speculative。
	 * 作用：推测性地插入一个元组。
	 * 核心流程：它使用推测令牌写入元组，以便稍后的完成回调可以确认或取消插入。
	 */
	void		(*tuple_insert_speculative) (Relation rel,
											 TupleTableSlot *slot,
											 CommandId cid,
											 int options,
											 struct BulkInsertStateData *bistate,
											 uint32 specToken);

	/* see table_tuple_complete_speculative() for reference about parameters */

	/* 有关参数的参考，请参阅 table_tuple_complete_speculative() */
	/*
	 * Function: tuple_complete_speculative.
	 * Purpose: Completes or aborts a speculative insertion.
	 * Core flow: It locates the slot tuple by its speculative token and finalizes its visibility according to the success flag.
	 *
	 * 函数：tuple_complete_speculative。
	 * 作用：完成或中止推测插入。
	 * 核心流程：它通过推测令牌定位槽元组，并根据成功标志确定其可见性。
	 */
	void		(*tuple_complete_speculative) (Relation rel,
											   TupleTableSlot *slot,
											   uint32 specToken,
											   bool succeeded);

	/* see table_multi_insert() for reference about parameters */

	/* 参数参考table_multi_insert() */
	/*
	 * Function: multi_insert.
	 * Purpose: Inserts a batch of tuples into a relation.
	 * Core flow: It writes the supplied slots under one command context, applying insertion options and the optional bulk-insert state.
	 *
	 * 函数：multi_insert。
	 * 作用：将一批元组插入关系中。
	 * 核心流程：它在一个命令上下文下写入提供的槽，应用插入选项和可选的批量插入状态。
	 */
	void		(*multi_insert) (Relation rel, TupleTableSlot **slots, int nslots,
								 CommandId cid, int options, struct BulkInsertStateData *bistate);

	/* see table_tuple_delete() for reference about parameters */

	/* 参数参考table_tuple_delete() */
	/*
	 * Function: tuple_delete.
	 * Purpose: Deletes a tuple from a relation.
	 * Core flow: It locates the target TID, applies visibility and crosscheck snapshots, waits as requested, and returns the tuple-modification result.
	 *
	 * 函数：tuple_delete。
	 * 作用：从关系中删除元组。
	 * 核心流程：它定位目标 TID，应用可见性和交叉检查快照，根据请求等待，并返回元组修改结果。
	 */
	TM_Result	(*tuple_delete) (Relation rel,
								 ItemPointer tid,
								 CommandId cid,
								 Snapshot snapshot,
								 Snapshot crosscheck,
								 bool wait,
								 TM_FailureData *tmfd,
								 bool changingPart);

	/* see table_tuple_update() for reference about parameters */

	/* 参数参考table_tuple_update() */
	/*
	 * Function: tuple_update.
	 * Purpose: Updates a tuple in a relation.
	 * Core flow: It replaces the old TID with the slot contents under the supplied snapshots and reports locking, failure, and index-update information.
	 *
	 * 函数：tuple_update。
	 * 作用：更新关系中的元组。
	 * 核心流程：它用提供的快照下的槽内容替换旧的 TID，并报告锁定、故障和索引更新信息。
	 */
	TM_Result	(*tuple_update) (Relation rel,
								 ItemPointer otid,
								 TupleTableSlot *slot,
								 CommandId cid,
								 Snapshot snapshot,
								 Snapshot crosscheck,
								 bool wait,
								 TM_FailureData *tmfd,
								 LockTupleMode *lockmode,
								 TU_UpdateIndexes *update_indexes);

	/* see table_tuple_lock() for reference about parameters */

	/* 参数参考table_tuple_lock() */
	/*
	 * Function: tuple_lock.
	 * Purpose: Locks a tuple in the requested mode.
	 * Core flow: It checks the tuple under the supplied snapshot and wait policy, acquires a compatible tuple lock, and reports the modification result.
	 *
	 * 函数：tuple_lock。
	 * 作用：将元组锁定在请求的模式中。
	 * 核心流程：它检查提供的快照和等待策略下的元组，获取兼容的元组锁，并报告修改结果。
	 */
	TM_Result	(*tuple_lock) (Relation rel,
							   ItemPointer tid,
							   Snapshot snapshot,
							   TupleTableSlot *slot,
							   CommandId cid,
							   LockTupleMode mode,
							   LockWaitPolicy wait_policy,
							   uint8 flags,
							   TM_FailureData *tmfd);

	/*
	 * Perform operations necessary to complete insertions made via
	 * tuple_insert and multi_insert with a BulkInsertState specified. In-tree
	 * access methods ceased to use this.
 *
 * 执行必要的操作来完成通过 tuple_insert 和 multi_insert 指定的 BulkInsertState 进行的插入。树内访问方法不再使用此功能。
	 *
	 * Typically callers of tuple_insert and multi_insert will just pass all
	 * the flags that apply to them, and each AM has to decide which of them
	 * make sense for it, and then only take actions in finish_bulk_insert for
	 * those flags, and ignore others.
 *
 * 通常 tuple_insert 和 multi_insert 的调用者只会传递适用于它们的所有标志，每个 AM 必须决定其中哪个标志对其有意义，然后仅在 finish_bulk_insert 中对这些标志执行操作，而忽略其他标志。
	 *
	 * Optional callback.
 *
 * 可选回调。
	 */
	/*
	 * Function: finish_bulk_insert.
	 * Purpose: Finishes a bulk insertion sequence.
	 * Core flow: It applies only the supported insertion options after tuple_insert or multi_insert and releases any access-method bulk state.
	 *
	 * 函数：finish_bulk_insert。
	 * 作用：完成批量插入序列。
	 * 核心流程：它仅在 tuple_insert 或 multi_insert 之后应用受支持的插入选项，并释放任何访问方法批量状态。
	 */
	void		(*finish_bulk_insert) (Relation rel, int options);


	/* ------------------------------------------------------------------------
	 * DDL related functionality.
 *
 * DDL 相关功能。
	 * ------------------------------------------------------------------------
	 */

	/*
	 * This callback needs to create new relation storage for `rel`, with
	 * appropriate durability behaviour for `persistence`.
 *
 * 此回调需要为“rel”创建新的关系存储，并为“persistence”提供适当的持久性行为。
	 *
	 * Note that only the subset of the relcache filled by
	 * RelationBuildLocalRelation() can be relied upon and that the relation's
	 * catalog entries will either not yet exist (new relation), or will still
	 * reference the old relfilelocator.
 *
 * 请注意，只能依赖 RelationBuildLocalRelation() 填充的 relcache 子集，并且关系的目录条目要么尚不存在（新关系），要么仍引用旧的 relfilelocator。
	 *
	 * As output *freezeXid, *minmulti must be set to the values appropriate
	 * for pg_class.{relfrozenxid, relminmxid}. For AMs that don't need those
	 * fields to be filled they can be set to InvalidTransactionId and
	 * InvalidMultiXactId, respectively.
 *
 * 作为输出 *freezeXid，*minmulti 必须设置为适合 pg_class.{relfrozenxid, relminmxid} 的值。对于不需要填充这些字段的 AM，可以将它们分别设置为 InvalidTransactionId 和 InvalidMultiXactId。
	 *
	 * See also table_relation_set_new_filelocator().
 *
 * 另请参见 table_relation_set_new_filelocator()。
	 */
	/*
	 * Function: relation_set_new_filelocator.
	 * Purpose: Creates storage for a new relation file locator.
	 * Core flow: It creates storage with the requested persistence and returns the freeze and multixact cutoffs appropriate for the new relation.
	 *
	 * 函数：relation_set_new_filelocator。
	 * 作用：为新的关系文件定位器创建存储。
	 * 核心流程：它创建具有请求的持久性的存储，并返回适合新关系的冻结和多重行为截止值。
	 */
	void		(*relation_set_new_filelocator) (Relation rel,
												 const RelFileLocator *newrlocator,
												 char persistence,
												 TransactionId *freezeXid,
												 MultiXactId *minmulti);

	/*
	 * This callback needs to remove all contents from `rel`'s current
	 * relfilelocator. No provisions for transactional behaviour need to be
	 * made.  Often this can be implemented by truncating the underlying
	 * storage to its minimal size.
 *
 * 此回调需要从 `rel` 的当前 relfilelocator 中删除所有内容。无需对交易行为做出任何规定。  通常，这可以通过将底层存储截断至最小大小来实现。
	 *
	 * See also table_relation_nontransactional_truncate().
 *
 * 另请参见 table_relation_nontransactional_truncate()。
	 */
	/*
	 * Function: relation_nontransactional_truncate.
	 * Purpose: Removes all data from a relation without transactional guarantees.
	 * Core flow: It truncates or otherwise reduces the current relation storage to its empty state.
	 *
	 * 函数：relation_nontransactional_truncate。
	 * 作用：从没有事务保证的关系中删除所有数据。
	 * 核心流程：它将截断或以其他方式将当前关系存储减少到其空状态。
	 */
	void		(*relation_nontransactional_truncate) (Relation rel);

	/*
	 * See table_relation_copy_data().
 *
 * 请参阅 table_relation_copy_data()。
	 *
	 * This can typically be implemented by directly copying the underlying
	 * storage, unless it contains references to the tablespace internally.
 *
 * 这通常可以通过直接复制底层存储来实现，除非它内部包含对表空间的引用。
	 */
	/*
	 * Function: relation_copy_data.
	 * Purpose: Copies relation data to a new file locator.
	 * Core flow: It copies the relation storage, accounting for access methods whose storage contains tablespace references.
	 *
	 * 函数：relation_copy_data。
	 * 作用：将关系数据复制到新的文件定位器。
	 * 核心流程：它复制关系存储，考虑存储包含表空间引用的访问方法。
	 */
	void		(*relation_copy_data) (Relation rel,
									   const RelFileLocator *newrlocator);

	/* See table_relation_copy_for_cluster() */

	/* 请参阅 table_relation_copy_for_cluster() */
	/*
	 * Function: relation_copy_for_cluster.
	 * Purpose: Copies table data while rebuilding a relation for CLUSTER.
	 * Core flow: It scans the old relation and index, writes the new relation in the requested order, and reports tuple and cutoff statistics.
	 *
	 * 函数：relation_copy_for_cluster。
	 * 作用：在重建 CLUSTER 关系时复制表数据。
	 * 核心流程：它扫描旧的关系和索引，按请求的顺序写入新的关系，并报告元组和截止统计信息。
	 */
	void		(*relation_copy_for_cluster) (Relation OldTable,
											  Relation NewTable,
											  Relation OldIndex,
											  bool use_sort,
											  TransactionId OldestXmin,
											  TransactionId *xid_cutoff,
											  MultiXactId *multi_cutoff,
											  double *num_tuples,
											  double *tups_vacuumed,
											  double *tups_recently_dead);

	/*
	 * React to VACUUM command on the relation. The VACUUM can be triggered by
	 * a user or by autovacuum. The specific actions performed by the AM will
	 * depend heavily on the individual AM.
 *
 * 对关系上的 VACUUM 命令做出反应。 VACUUM 可以由用户或自动真空触发。 AM 执行的具体操作在很大程度上取决于各个 AM。
	 *
	 * On entry a transaction is already established, and the relation is
	 * locked with a ShareUpdateExclusive lock.
 *
 * 在进入时，事务已经建立，并且关系被 ShareUpdateExclusive 锁锁定。
	 *
	 * Note that neither VACUUM FULL (and CLUSTER), nor ANALYZE go through
	 * this routine, even if (for ANALYZE) it is part of the same VACUUM
	 * command.
 *
 * 请注意，VACUUM FULL（和 CLUSTER）和 ANALYZE 都不会执行此例程，即使（对于 ANALYZE）它是同一 VACUUM 命令的一部分。
	 *
	 * There probably, in the future, needs to be a separate callback to
	 * integrate with autovacuum's scheduling.
 *
 * 将来可能需要一个单独的回调来与 autovacuum 的调度集成。
	 */
	/*
	 * Function: relation_vacuum.
	 * Purpose: Carries out access-method work for a VACUUM command.
	 * Core flow: It runs with an established transaction and ShareUpdateExclusive lock, applying the access method work required for ordinary VACUUM.
	 *
	 * 函数：relation_vacuum。
	 * 作用：执行 VACUUM 命令的访问方法工作。
	 * 核心流程：它以已建立的事务和 ShareUpdateExclusive 锁运行，应用普通 VACUUM 所需的访问方法工作。
	 */
	void		(*relation_vacuum) (Relation rel,
									struct VacuumParams *params,
									BufferAccessStrategy bstrategy);

	/*
	 * Prepare to analyze block `blockno` of `scan`. The scan has been started
	 * with table_beginscan_analyze().  See also
	 * table_scan_analyze_next_block().
 *
 * 准备分析“scan”的块“blockno”。扫描已通过 table_beginscan_analyze() 开始。  另请参见 table_scan_analyze_next_block()。
	 *
	 * The callback may acquire resources like locks that are held until
	 * table_scan_analyze_next_tuple() returns false. It e.g. can make sense
	 * to hold a lock until all tuples on a block have been analyzed by
	 * scan_analyze_next_tuple.
 *
 * 回调可能会获取诸如锁之类的资源，这些资源会一直保留到 table_scan_analyze_next_tuple() 返回 false 为止。它例如在 scan_analyze_next_tuple 分析完块上的所有元组之前，保持锁定是有意义的。
	 *
	 * The callback can return false if the block is not suitable for
	 * sampling, e.g. because it's a metapage that could never contain tuples.
 *
 * 如果块不适合采样，回调可以返回 false，例如因为它是一个永远不可能包含元组的元页面。
	 *
	 * XXX: This obviously is primarily suited for block-based AMs. It's not
	 * clear what a good interface for non block based AMs would be, so there
	 * isn't one yet.
 *
 * XXX：这显然主要适用于基于块的 AM。目前尚不清楚非基于块的 AM 的良好接口是什么，因此目前还没有一个接口。
	 */
	/*
	 * Function: scan_analyze_next_block.
	 * Purpose: Prepares one block for ANALYZE sampling.
	 * Core flow: It prepares the block within an ANALYZE scan, may retain block resources until tuple sampling finishes, and reports whether the block is usable.
	 *
	 * 函数：scan_analyze_next_block。
	 * 作用：准备一个块用于分析采样。
	 * 核心流程：它在 ANALYZE 扫描中准备块，可以保留块资源直到元组采样完成，并报告该块是否可用。
	 */
	bool		(*scan_analyze_next_block) (TableScanDesc scan,
											ReadStream *stream);

	/*
	 * See table_scan_analyze_next_tuple().
 *
 * 请参阅 table_scan_analyze_next_tuple()。
	 *
	 * Not every AM might have a meaningful concept of dead rows, in which
	 * case it's OK to not increment *deadrows - but note that that may
	 * influence autovacuum scheduling (see comment for relation_vacuum
	 * callback).
 *
 * 并非每个 AM 都可能有有意义的死行概念；在这种情况下，不增加 *deadrows 是可以的，但请注意这可能影响 autovacuum 调度（参见 relation_vacuum 回调的注释）。
	 */
	/*
	 * Function: scan_analyze_next_tuple.
	 * Purpose: Returns the next tuple for ANALYZE sampling.
	 * Core flow: It examines the prepared scan block, places the next sample tuple in the slot, updates live and dead row counts, and reports whether more tuples remain.
	 *
	 * 函数：scan_analyze_next_tuple。
	 * 作用：返回下一个元组以进行 ANALYZE 采样。
	 * 核心流程：它检查准备好的扫描块，将下一个样本元组放入槽中，更新活行和死行计数，并报告是否还有更多元组。
	 */
	bool		(*scan_analyze_next_tuple) (TableScanDesc scan,
											TransactionId OldestXmin,
											double *liverows,
											double *deadrows,
											TupleTableSlot *slot);

	/* see table_index_build_range_scan for reference about parameters */

	/* 参数参考table_index_build_range_scan */
	/*
	 * Function: index_build_range_scan.
	 * Purpose: Scans a table range while building an index.
	 * Core flow: It walks the requested block range, invokes the index-build callback for qualifying tuples, and returns the number of processed tuples.
	 *
	 * 函数：index_build_range_scan。
	 * 作用：构建索引时扫描表范围。
	 * 核心流程：它遍历请求的块范围，调用符合条件的元组的索引构建回调，并返回已处理元组的数量。
	 */
	double		(*index_build_range_scan) (Relation table_rel,
										   Relation index_rel,
										   struct IndexInfo *index_info,
										   bool allow_sync,
										   bool anyvisible,
										   bool progress,
										   BlockNumber start_blockno,
										   BlockNumber numblocks,
										   IndexBuildCallback callback,
										   void *callback_state,
										   TableScanDesc scan);

	/* see table_index_validate_scan for reference about parameters */

	/* 参数参考table_index_validate_scan */
	/*
	 * Function: index_validate_scan.
	 * Purpose: Validates an index by scanning its table.
	 * Core flow: It scans table tuples visible to the snapshot and records index-validation information in the supplied validation state.
	 *
	 * 函数：index_validate_scan。
	 * 作用：通过扫描表来验证索引。
	 * 核心流程：它扫描快照可见的表元组，并以提供的验证状态记录索引验证信息。
	 */
	void		(*index_validate_scan) (Relation table_rel,
										Relation index_rel,
										struct IndexInfo *index_info,
										Snapshot snapshot,
										struct ValidateIndexState *state);


	/* ------------------------------------------------------------------------
	 * Miscellaneous functions.
 *
 * 杂项功能。
	 * ------------------------------------------------------------------------
	 */

	/*
	 * See table_relation_size().
 *
 * 请参阅 table_relation_size()。
	 *
	 * Note that currently a few callers use the MAIN_FORKNUM size to figure
	 * out the range of potentially interesting blocks (brin, analyze). It's
	 * probable that we'll need to revise the interface for those at some
	 * point.
 *
 * 请注意，当前一些调用者使用 MAIN_FORKNUM 大小来确定可能感兴趣的块的范围（brin、analyze）。我们可能需要在某个时候修改这些接口。
	 */
	/*
	 * Function: relation_size.
	 * Purpose: Returns the current size of a relation fork.
	 * Core flow: It obtains the size for the requested fork number so callers can determine the relation block range.
	 *
	 * 函数：relation_size。
	 * 作用：返回关系叉的当前大小。
	 * 核心流程：它获取所请求的分叉号的大小，以便调用者可以确定关系块范围。
	 */
	uint64		(*relation_size) (Relation rel, ForkNumber forkNumber);


	/*
	 * This callback should return true if the relation requires a TOAST table
	 * and false if it does not.  It may wish to examine the relation's tuple
	 * descriptor before making a decision, but if it uses some other method
	 * of storing large values (or if it does not support them) it can simply
	 * return false.
 *
 * 如果关系需要 TOAST 表，则此回调应返回 true；如果不需要，则返回 false。  它可能希望在做出决定之前检查关系的元组描述符，但如果它使用其他一些存储大值的方法（或者如果它不支持它们），它可以简单地返回 false。
	 */
	/*
	 * Function: relation_needs_toast_table.
	 * Purpose: Determines whether a relation requires a TOAST table.
	 * Core flow: It examines the relation storage capabilities and tuple descriptor as needed, then returns whether separate TOAST storage is necessary.
	 *
	 * 函数：relation_needs_toast_table。
	 * 作用：确定关系是否需要 TOAST 表。
	 * 核心流程：它根据需要检查关系存储功能和元组描述符，然后返回是否需要单独的 TOAST 存储。
	 */
	bool		(*relation_needs_toast_table) (Relation rel);

	/*
	 * This callback should return the OID of the table AM that implements
	 * TOAST tables for this AM.  If the relation_needs_toast_table callback
	 * always returns false, this callback is not required.
 *
 * 此回调应返回表 AM 的 OID，该表实现了该 AM 的 TOAST 表。  如果relation_needs_toast_table回调始终返回false，则不需要该回调。
	 */
	/*
	 * Function: relation_toast_am.
	 * Purpose: Returns the table access method used for TOAST tables.
	 * Core flow: It supplies the OID of the access method that implements this access method’s TOAST relations.
	 *
	 * 函数：relation_toast_am。
	 * 作用：返回用于 TOAST 表的表访问方法。
	 * 核心流程：它提供实现该访问方法的 TOAST 关系的访问方法的 OID。
	 */
	Oid			(*relation_toast_am) (Relation rel);

	/*
	 * This callback is invoked when detoasting a value stored in a toast
	 * table implemented by this AM.  See table_relation_fetch_toast_slice()
	 * for more details.
 *
 * 当取消此 AM 实现的 toast 表中存储的值时，会调用此回调。  有关更多详细信息，请参阅 table_relation_fetch_toast_slice()。
	 */
	/*
	 * Function: relation_fetch_toast_slice.
	 * Purpose: Fetches a slice of a TOAST value.
	 * Core flow: It retrieves the requested byte range for the value stored in the access method’s TOAST relation and writes it to the result varlena.
	 *
	 * 函数：relation_fetch_toast_slice。
	 * 作用：获取 TOAST 值的一部分。
	 * 核心流程：它检索存储在访问方法的 TOAST 关系中的值的请求字节范围，并将其写入结果 varlena。
	 */
	void		(*relation_fetch_toast_slice) (Relation toastrel, Oid valueid,
											   int32 attrsize,
											   int32 sliceoffset,
											   int32 slicelength,
											   struct varlena *result);


	/* ------------------------------------------------------------------------
	 * Planner related functions.
 *
 * 规划器相关功能。
	 * ------------------------------------------------------------------------
	 */

	/*
	 * See table_relation_estimate_size().
 *
 * 请参阅 table_relation_estimate_size()。
	 *
	 * While block oriented, it shouldn't be too hard for an AM that doesn't
	 * internally use blocks to convert into a usable representation.
 *
 * 虽然面向块，但对于内部不使用块来转换为可用表示的 AM 来说应该不会太难。
	 *
	 * This differs from the relation_size callback by returning size
	 * estimates (both relation size and tuple count) for planning purposes,
	 * rather than returning a currently correct estimate.
 *
 * 这与 relation_size 回调不同，它返回大小估计（关系大小和元组计数）以用于规划目的，而不是返回当前正确的估计。
	 */
	/*
	 * Function: relation_estimate_size.
	 * Purpose: Estimates relation size and tuple count for planning.
	 * Core flow: It derives block and tuple estimates for the planner, rather than reporting the current exact physical size.
	 *
	 * 函数：relation_estimate_size。
	 * 作用：估计关系大小和元组计数以进行规划。
	 * 核心流程：它为规划器导出块和元组估计，而不是报告当前的确切物理大小。
	 */
	void		(*relation_estimate_size) (Relation rel, int32 *attr_widths,
										   BlockNumber *pages, double *tuples,
										   double *allvisfrac);


	/* ------------------------------------------------------------------------
	 * Executor related functions.
 *
 * 执行器相关函数。
	 * ------------------------------------------------------------------------
	 */

	/*
	 * Fetch the next tuple of a bitmap table scan into `slot` and return true
	 * if a visible tuple was found, false otherwise.
 *
 * 将位图表扫描的下一个元组读入 `slot`；若找到可见元组则返回 true，否则返回 false。
	 *
	 * `lossy_pages` is incremented if the bitmap is lossy for the selected
	 * page; otherwise, `exact_pages` is incremented. These are tracked for
	 * display in EXPLAIN ANALYZE output.
 *
 * 如果所选页面的位图有损，则“lossy_pages”会递增；否则，“exact_pages”会增加。这些被跟踪并显示在 EXPLAIN ANALYZE 输出中。
	 *
	 * Prefetching additional data from the bitmap is left to the table AM.
 *
 * 从位图预取额外数据由表访问方法负责。
	 *
	 * This is an optional callback.
 *
 * 这是一个可选的回调。
	 */
	/*
	 * Function: scan_bitmap_next_tuple.
	 * Purpose: Fetches the next visible tuple for a bitmap table scan.
	 * Core flow: It obtains the next bitmap-selected tuple into the slot, sets recheck information, updates exact or lossy page counters, and reports success.
	 *
	 * 函数：scan_bitmap_next_tuple。
	 * 作用：获取下一个可见元组以进行位图表扫描。
	 * 核心流程：它将下一个位图选择的元组获取到槽中，设置重新检查信息，更新精确或有损页面计数器，并报告成功。
	 */
	bool		(*scan_bitmap_next_tuple) (TableScanDesc scan,
										   TupleTableSlot *slot,
										   bool *recheck,
										   uint64 *lossy_pages,
										   uint64 *exact_pages);

	/*
	 * Prepare to fetch tuples from the next block in a sample scan. Return
	 * false if the sample scan is finished, true otherwise. `scan` was
	 * started via table_beginscan_sampling().
 *
 * 准备从示例扫描的下一个块中获取元组。如果样本扫描完成则返回 false，否则返回 true。 `scan` 是通过 table_beginscan_sampling() 启动的。
	 *
	 * Typically this will first determine the target block by calling the
	 * TsmRoutine's NextSampleBlock() callback if not NULL, or alternatively
	 * perform a sequential scan over all blocks.  The determined block is
	 * then typically read and pinned.
 *
 * 通常先通过 TsmRoutine 的 NextSampleBlock() 回调确定目标块；若该回调为 NULL，则顺序扫描所有块，随后读取并固定所选块。
	 *
	 * As the TsmRoutine interface is block based, a block needs to be passed
	 * to NextSampleBlock(). If that's not appropriate for an AM, it
	 * internally needs to perform mapping between the internal and a block
	 * based representation.
 *
 * 由于 TsmRoutine 接口是基于块的，因此需要将块传递给 NextSampleBlock()。如果这不适合 AM，它在内部需要在内部表示和基于块的表示之间执行映射。
	 *
	 * Note that it's not acceptable to hold deadlock prone resources such as
	 * lwlocks until scan_sample_next_tuple() has exhausted the tuples on the
	 * block - the tuple is likely to be returned to an upper query node, and
	 * the next call could be off a long while. Holding buffer pins and such
	 * is obviously OK.
 *
 * 在 scan_sample_next_tuple() 耗尽当前块元组前，不得持有 lwlock 等可能导致死锁的资源；缓冲区固定等资源可以持有。
	 *
	 * Currently it is required to implement this interface, as there's no
	 * alternative way (contrary e.g. to bitmap scans) to implement sample
	 * scans. If infeasible to implement, the AM may raise an error.
 *
 * 目前需要实现此接口，因为没有替代方法（与位图扫描相反）来实现样本扫描。如果无法实现，AM 可能会引发错误。
	 */
	/*
	 * Function: scan_sample_next_block.
	 * Purpose: Prepares the next block of a sample scan.
	 * Core flow: It selects, reads, and pins a sample block using the TsmRoutine when appropriate, and reports whether sampling can continue.
	 *
	 * 函数：scan_sample_next_block。
	 * 作用：准备样本扫描的下一个块。
	 * 核心流程：它在适当时使用 TsmRoutine 选择、读取和固定采样块，并报告采样是否可以继续。
	 */
	bool		(*scan_sample_next_block) (TableScanDesc scan,
										   struct SampleScanState *scanstate);

	/*
	 * This callback, only called after scan_sample_next_block has returned
	 * true, should determine the next tuple to be returned from the selected
	 * block using the TsmRoutine's NextSampleTuple() callback.
 *
 * 此回调仅在 scan_sample_next_block 返回 true 后调用，应使用 TsmRoutine 的 NextSampleTuple() 回调确定要从所选块返回的下一个元组。
	 *
	 * The callback needs to perform visibility checks, and only return
	 * visible tuples. That obviously can mean calling NextSampleTuple()
	 * multiple times.
 *
 * 回调需要执行可见性检查，并且仅返回可见元组。这显然意味着多次调用 NextSampleTuple()。
	 *
	 * The TsmRoutine interface assumes that there's a maximum offset on a
	 * given page, so if that doesn't apply to an AM, it needs to emulate that
	 * assumption somehow.
 *
 * TsmRoutine 接口假设给定页面上存在最大偏移量，因此如果这不适用于 AM，则需要以某种方式模拟该假设。
	 */
	/*
	 * Function: scan_sample_next_tuple.
	 * Purpose: Fetches the next visible tuple from a sample block.
	 * Core flow: After a block is prepared, it uses the TsmRoutine to select candidates, applies visibility checks, stores the next tuple in the slot, and reports success.
	 *
	 * 函数：scan_sample_next_tuple。
	 * 作用：从样本块中获取下一个可见元组。
	 * 核心流程：准备好块后，它使用 TsmRoutine 选择候选者，应用可见性检查，将下一个元组存储在槽中，并报告成功。
	 */
	bool		(*scan_sample_next_tuple) (TableScanDesc scan,
										   struct SampleScanState *scanstate,
										   TupleTableSlot *slot);

} TableAmRoutine;


/* ----------------------------------------------------------------------------
 * Slot functions.
 *
 * 槽位（slot）相关函数。
 * ----------------------------------------------------------------------------
 */

/*
 * Returns slot callbacks suitable for holding tuples of the appropriate type
 * for the relation.  Works for tables, views, foreign tables and partitioned
 * tables.
 *
 * 返回适合承载该关系相应类型元组的槽位回调集合。适用于普通表、视图、
 * 外部表以及分区表。
 */
/*
 * Function: table_slot_callbacks.
 * Purpose: Returns the set of TupleTableSlotOps callbacks that describe how to
 *          store and access tuples for the given relation.
 * Core flow: If the relation has a table AM, ask it for its slot callbacks;
 *            otherwise fall back to the default heap/virtual slot ops.
 *
 * 函数：table_slot_callbacks。
 * 作用：返回一组 TupleTableSlotOps 回调，描述如何为给定关系存储和访问元组。
 * 核心流程：若关系拥有表访问方法，则向其请求对应的槽位回调；否则回退到
 *           默认的堆/虚拟槽位操作。
 */
extern const TupleTableSlotOps *table_slot_callbacks(Relation relation);

/*
 * Returns slot using the callbacks returned by table_slot_callbacks(), and
 * registers it on *reglist.
 *
 * 使用 table_slot_callbacks() 返回的回调创建槽位，并将其注册到 *reglist 上。
 */
/*
 * Function: table_slot_create.
 * Purpose: Creates a TupleTableSlot with the relation's slot callbacks and, if
 *          reglist is provided, registers it there for later cleanup.
 * Core flow: Fetch the slot callbacks via table_slot_callbacks(), allocate a
 *            slot with matching tuple descriptor, and append it to *reglist.
 *
 * 函数：table_slot_create。
 * 作用：使用该关系的槽位回调创建 TupleTableSlot；若提供了 reglist，则将其
 *       注册到该列表中以便后续统一释放。
 * 核心流程：通过 table_slot_callbacks() 获取槽位回调，分配一个具有匹配元组
 *           描述符的槽位，并将其追加到 *reglist。
 */
extern TupleTableSlot *table_slot_create(Relation relation, List **reglist);


/* ----------------------------------------------------------------------------
 * Table scan functions.
 *
 * 表扫描函数。
 * ----------------------------------------------------------------------------
 */

/*
 * Start a scan of `rel`. Returned tuples pass a visibility test of
 * `snapshot`, and if nkeys != 0, the results are filtered by those scan keys.
 *
 * 开始对 `rel` 的扫描。返回的元组均通过 `snapshot` 的可见性检查；若
 * nkeys != 0，结果还会按照这些扫描键（scan key）进行过滤。
 */
/*
 * Function: table_beginscan.
 * Purpose: Begins a standard sequential scan of the relation using the given
 *          snapshot and optional scan keys.
 * Core flow: Set the seqscan flags (strategy, syncscan and page-mode all
 *            allowed) and delegate to the table AM's scan_begin callback.
 *
 * 函数：table_beginscan。
 * 作用：使用给定快照和可选扫描键，对该关系发起一次标准顺序扫描。
 * 核心流程：设置顺序扫描标志（允许缓冲策略、同步扫描和页模式），并委托给
 *           表访问方法的 scan_begin 回调。
 */
static inline TableScanDesc
table_beginscan(Relation rel, Snapshot snapshot,
				int nkeys, struct ScanKeyData *key)
{
	uint32		flags = SO_TYPE_SEQSCAN |
		SO_ALLOW_STRAT | SO_ALLOW_SYNC | SO_ALLOW_PAGEMODE;

	return rel->rd_tableam->scan_begin(rel, snapshot, nkeys, key, NULL, flags);
}

/*
 * Like table_beginscan(), but for scanning catalog. It'll automatically use a
 * snapshot appropriate for scanning catalog relations.
 *
 * 与 table_beginscan() 类似，但用于扫描系统目录。它会自动使用适合扫描
 * 目录关系的快照。
 */
/*
 * Function: table_beginscan_catalog.
 * Purpose: Begins a scan of a catalog relation, choosing a snapshot suitable
 *          for reading catalogs consistently.
 * Core flow: Obtain an MVCC catalog snapshot via GetCatalogSnapshot(),
 *            register it, then start a seqscan with the standard flags.
 *
 * 函数：table_beginscan_catalog。
 * 作用：开始扫描一个目录关系，并选择适合一致地读取目录的快照。
 * 核心流程：通过 GetCatalogSnapshot() 获取 MVCC 目录快照并注册，然后以
 *           标准标志发起顺序扫描。
 */
extern TableScanDesc table_beginscan_catalog(Relation relation, int nkeys,
											 struct ScanKeyData *key);

/*
 * Like table_beginscan(), but table_beginscan_strat() offers an extended API
 * that lets the caller control whether a nondefault buffer access strategy
 * can be used, and whether syncscan can be chosen (possibly resulting in the
 * scan not starting from block zero).  Both of these default to true with
 * plain table_beginscan.
 *
 * 与 table_beginscan() 类似，但 table_beginscan_strat() 提供了扩展 API，
 * 允许调用者控制是否可以使用非默认的缓冲区访问策略，以及是否可以选择
 * 同步扫描（这可能导致扫描不从 0 号块开始）。在普通的 table_beginscan
 * 中，这两者都默认为 true。
 */
/*
 * Function: table_beginscan_strat.
 * Purpose: Begins a sequential scan while letting the caller enable or disable
 *          the buffer access strategy and syncscan optimizations.
 * Core flow: Build the seqscan flags from allow_strat/allow_sync, always allow
 *            page mode, then invoke the table AM's scan_begin callback.
 *
 * 函数：table_beginscan_strat。
 * 作用：发起一次顺序扫描，同时允许调用者启用或禁用缓冲区访问策略与
 *       同步扫描优化。
 * 核心流程：根据 allow_strat/allow_sync 构造顺序扫描标志，始终允许页模式，
 *           然后调用表访问方法的 scan_begin 回调。
 */
static inline TableScanDesc
table_beginscan_strat(Relation rel, Snapshot snapshot,
					  int nkeys, struct ScanKeyData *key,
					  bool allow_strat, bool allow_sync)
{
	uint32		flags = SO_TYPE_SEQSCAN | SO_ALLOW_PAGEMODE;

	if (allow_strat)
		flags |= SO_ALLOW_STRAT;
	if (allow_sync)
		flags |= SO_ALLOW_SYNC;

	return rel->rd_tableam->scan_begin(rel, snapshot, nkeys, key, NULL, flags);
}

/*
 * table_beginscan_bm is an alternative entry point for setting up a
 * TableScanDesc for a bitmap heap scan.  Although that scan technology is
 * really quite unlike a standard seqscan, there is just enough commonality to
 * make it worth using the same data structure.
 *
 * table_beginscan_bm 是为位图堆扫描（bitmap heap scan）建立 TableScanDesc
 * 的另一个入口。尽管该扫描技术与标准顺序扫描相当不同，但两者仍有足够的
 * 共性，值得复用同一套数据结构。
 */
/*
 * Function: table_beginscan_bm.
 * Purpose: Begins a scan set up for bitmap heap scanning of the relation.
 * Core flow: Set the bitmap-scan type flag together with page mode and call
 *            the table AM's scan_begin callback (no start-block chosen here).
 *
 * 函数：table_beginscan_bm。
 * 作用：为该关系发起一次用于位图堆扫描的扫描。
 * 核心流程：设置位图扫描类型标志并启用页模式，然后调用表访问方法的
 *           scan_begin 回调（此处不选择起始块）。
 */
static inline TableScanDesc
table_beginscan_bm(Relation rel, Snapshot snapshot,
				   int nkeys, struct ScanKeyData *key)
{
	uint32		flags = SO_TYPE_BITMAPSCAN | SO_ALLOW_PAGEMODE;

	return rel->rd_tableam->scan_begin(rel, snapshot, nkeys, key,
									   NULL, flags);
}

/*
 * table_beginscan_sampling is an alternative entry point for setting up a
 * TableScanDesc for a TABLESAMPLE scan.  As with bitmap scans, it's worth
 * using the same data structure although the behavior is rather different.
 * In addition to the options offered by table_beginscan_strat, this call
 * also allows control of whether page-mode visibility checking is used.
 *
 * table_beginscan_sampling 是为 TABLESAMPLE 扫描建立 TableScanDesc 的另一个
 * 入口。与位图扫描一样，尽管行为差异较大，仍值得复用同一套数据结构。
 * 除了 table_beginscan_strat 提供的选项外，此调用还允许控制是否使用
 * 页模式（page-mode）可见性检查。
 */
/*
 * Function: table_beginscan_sampling.
 * Purpose: Begins a scan configured for a TABLESAMPLE scan, exposing control
 *          over strategy, syncscan and page-mode options.
 * Core flow: Assemble the samplescan flags from the three allow_* arguments
 *            and hand them to the table AM's scan_begin callback.
 *
 * 函数：table_beginscan_sampling。
 * 作用：发起一次为 TABLESAMPLE 扫描配置的扫描，并开放对策略、同步扫描
 *       和页模式选项的控制。
 * 核心流程：根据三个 allow_* 参数组装采样扫描标志，并将其交给表访问方法
 *           的 scan_begin 回调。
 */
static inline TableScanDesc
table_beginscan_sampling(Relation rel, Snapshot snapshot,
						 int nkeys, struct ScanKeyData *key,
						 bool allow_strat, bool allow_sync,
						 bool allow_pagemode)
{
	uint32		flags = SO_TYPE_SAMPLESCAN;

	if (allow_strat)
		flags |= SO_ALLOW_STRAT;
	if (allow_sync)
		flags |= SO_ALLOW_SYNC;
	if (allow_pagemode)
		flags |= SO_ALLOW_PAGEMODE;

	return rel->rd_tableam->scan_begin(rel, snapshot, nkeys, key, NULL, flags);
}

/*
 * table_beginscan_tid is an alternative entry point for setting up a
 * TableScanDesc for a Tid scan. As with bitmap scans, it's worth using
 * the same data structure although the behavior is rather different.
 *
 * table_beginscan_tid 是为 TID 扫描建立 TableScanDesc 的另一个入口。与
 * 位图扫描一样，尽管行为差异较大，仍值得复用同一套数据结构。
 */
/*
 * Function: table_beginscan_tid.
 * Purpose: Begins a scan configured for fetching tuples by TID.
 * Core flow: Set the TID-scan type flag and invoke the table AM's scan_begin
 *            callback with no scan keys (TIDs are supplied later).
 *
 * 函数：table_beginscan_tid。
 * 作用：发起一次为按 TID 取元组而配置的扫描。
 * 核心流程：设置 TID 扫描类型标志，并在不带扫描键的情况下调用表访问方法
 *           的 scan_begin 回调（TID 稍后提供）。
 */
static inline TableScanDesc
table_beginscan_tid(Relation rel, Snapshot snapshot)
{
	uint32		flags = SO_TYPE_TIDSCAN;

	return rel->rd_tableam->scan_begin(rel, snapshot, 0, NULL, NULL, flags);
}

/*
 * table_beginscan_analyze is an alternative entry point for setting up a
 * TableScanDesc for an ANALYZE scan.  As with bitmap scans, it's worth using
 * the same data structure although the behavior is rather different.
 *
 * table_beginscan_analyze 是为 ANALYZE 扫描建立 TableScanDesc 的另一个入口。
 * 与位图扫描一样，尽管行为差异较大，仍值得复用同一套数据结构。
 */
/*
 * Function: table_beginscan_analyze.
 * Purpose: Begins a scan used by ANALYZE to sample blocks and tuples.
 * Core flow: Set the analyze-scan type flag and call the table AM's scan_begin
 *            callback with no snapshot and no scan keys.
 *
 * 函数：table_beginscan_analyze。
 * 作用：发起一次供 ANALYZE 用于采样块和元组的扫描。
 * 核心流程：设置 analyze 扫描类型标志，并在不带快照和扫描键的情况下调用
 *           表访问方法的 scan_begin 回调。
 */
static inline TableScanDesc
table_beginscan_analyze(Relation rel)
{
	uint32		flags = SO_TYPE_ANALYZE;

	return rel->rd_tableam->scan_begin(rel, NULL, 0, NULL, NULL, flags);
}

/*
 * End relation scan.
 *
 * 结束关系扫描。
 */
/*
 * Function: table_endscan.
 * Purpose: Ends a scan started by one of the table_beginscan* entry points and
 *          releases its resources.
 * Core flow: Delegate to the table AM's scan_end callback for the scan's
 *            relation.
 *
 * 函数：table_endscan。
 * 作用：结束由某个 table_beginscan* 入口发起的扫描，并释放其资源。
 * 核心流程：委托给该扫描所属关系的表访问方法 scan_end 回调。
 */
static inline void
table_endscan(TableScanDesc scan)
{
	scan->rs_rd->rd_tableam->scan_end(scan);
}

/*
 * Restart a relation scan.
 *
 * 重新开始一次关系扫描。
 */
/*
 * Function: table_rescan.
 * Purpose: Restarts an existing scan from the beginning, optionally applying
 *          new scan keys.
 * Core flow: Call the table AM's scan_rescan callback with set_params=false so
 *            that strategy/syncscan/pagemode options are left unchanged.
 *
 * 函数：table_rescan。
 * 作用：从头重新开始一个已有的扫描，并可选地应用新的扫描键。
 * 核心流程：以 set_params=false 调用表访问方法的 scan_rescan 回调，从而
 *           保持策略/同步扫描/页模式等选项不变。
 */
static inline void
table_rescan(TableScanDesc scan,
			 struct ScanKeyData *key)
{
	scan->rs_rd->rd_tableam->scan_rescan(scan, key, false, false, false, false);
}

/*
 * Restart a relation scan after changing params.
 *
 * 在更改参数后重新开始一次关系扫描。
 *
 * This call allows changing the buffer strategy, syncscan, and pagemode
 * options before starting a fresh scan.  Note that although the actual use of
 * syncscan might change (effectively, enabling or disabling reporting), the
 * previously selected startblock will be kept.
 *
 * 此调用允许在开始新的扫描之前更改缓冲区策略、同步扫描和页模式等选项。
 * 请注意，尽管同步扫描的实际使用可能发生变化（实际上是启用或禁用位置
 * 上报），但先前选定的起始块仍会被保留。
 */
/*
 * Function: table_rescan_set_params.
 * Purpose: Restarts a scan while also updating the buffer strategy, syncscan
 *          and pagemode options.
 * Core flow: Invoke the table AM's scan_rescan callback with set_params=true
 *            and the caller-provided allow_* flags.
 *
 * 函数：table_rescan_set_params。
 * 作用：在重新开始扫描的同时，更新缓冲区策略、同步扫描和页模式等选项。
 * 核心流程：以 set_params=true 及调用者提供的各 allow_* 标志调用表访问方法
 *           的 scan_rescan 回调。
 */
static inline void
table_rescan_set_params(TableScanDesc scan, struct ScanKeyData *key,
						bool allow_strat, bool allow_sync, bool allow_pagemode)
{
	scan->rs_rd->rd_tableam->scan_rescan(scan, key, true,
										 allow_strat, allow_sync,
										 allow_pagemode);
}

/*
 * Return next tuple from `scan`, store in slot.
 *
 * 从 `scan` 返回下一个元组，并将其存入 slot。
 */
/*
 * Function: table_scan_getnextslot.
 * Purpose: Fetches the next tuple from a table scan into a slot.
 * Core flow: It records the scan relation OID in the slot, rejects no-movement scans and invalid logical-decoding calls, then invokes scan_getnextslot.
 *
 * 函数：table_scan_getnextslot。
 * 作用：将表扫描中的下一个元组取出到槽中。
 * 核心流程：它记录槽中的扫描关系OID，拒绝无移动扫描和无效的逻辑解码调用，然后调用scan_getnextslot。
 */
static inline bool
table_scan_getnextslot(TableScanDesc sscan, ScanDirection direction, TupleTableSlot *slot)
{
	slot->tts_tableOid = RelationGetRelid(sscan->rs_rd);

	/* We don't expect actual scans using NoMovementScanDirection */

	/* 我们不期望实际扫描使用 NoMovementScanDirection。 */
	Assert(direction == ForwardScanDirection ||
		   direction == BackwardScanDirection);

	/*
	 * We don't expect direct calls to table_scan_getnextslot with valid
	 * CheckXidAlive for catalog or regular tables.  See detailed comments in
	 * xact.c where these variables are declared.
 *
 * 对于系统目录或普通表，在 CheckXidAlive 有效时不应直接调用
 * table_scan_getnextslot。详见 xact.c 中这些变量声明处的注释。
	 */
	if (unlikely(TransactionIdIsValid(CheckXidAlive) && !bsysscan))
		elog(ERROR, "unexpected table_scan_getnextslot call during logical decoding");

	return sscan->rs_rd->rd_tableam->scan_getnextslot(sscan, direction, slot);
}

/* ----------------------------------------------------------------------------
 * TID Range scanning related functions.
 *
 * 与 TID 范围扫描相关的函数。
 * ----------------------------------------------------------------------------
 */

/*
 * table_beginscan_tidrange is the entry point for setting up a TableScanDesc
 * for a TID range scan.
 *
 * table_beginscan_tidrange 是为 TID 范围扫描设置 TableScanDesc 的入口点。
 */
/*
 * Function: table_beginscan_tidrange.
 * Purpose: Starts a table scan restricted to a TID range.
 * Core flow: It creates a TID-range scan descriptor with page mode enabled, sets the minimum and maximum TIDs, and returns the descriptor.
 *
 * 函数：table_beginscan_tidrange。
 * 作用：启动仅限于 TID 范围的表扫描。
 * 核心流程：它创建启用页面模式的 TID 范围扫描描述符，设置最小和最大 TID，并返回描述符。
 */
static inline TableScanDesc
table_beginscan_tidrange(Relation rel, Snapshot snapshot,
						 ItemPointer mintid,
						 ItemPointer maxtid)
{
	TableScanDesc sscan;
	uint32		flags = SO_TYPE_TIDRANGESCAN | SO_ALLOW_PAGEMODE;

	sscan = rel->rd_tableam->scan_begin(rel, snapshot, 0, NULL, NULL, flags);

	/* Set the range of TIDs to scan */

	/* 设置要扫描的 TID 范围。 */
	sscan->rs_rd->rd_tableam->scan_set_tidrange(sscan, mintid, maxtid);

	return sscan;
}

/*
 * table_rescan_tidrange resets the scan position and sets the minimum and
 * maximum TID range to scan for a TableScanDesc created by
 * table_beginscan_tidrange.
 *
 * table_rescan_tidrange 会重置扫描位置，并为由
 * table_beginscan_tidrange 创建的 TableScanDesc 设置待扫描的最小和最大 TID 范围。
 */
/*
 * Function: table_rescan_tidrange.
 * Purpose: Restarts a TID-range table scan with new bounds.
 * Core flow: It verifies the scan type, resets the access-method scan state, and installs the new minimum and maximum TIDs.
 *
 * 函数：table_rescan_tidrange。
 * 作用：使用新边界重新启动 TID 范围表扫描。
 * 核心流程：它验证扫描类型，重置访问方法扫描状态，并安装新的最小和最大 TID。
 */
static inline void
table_rescan_tidrange(TableScanDesc sscan, ItemPointer mintid,
					  ItemPointer maxtid)
{
	/* Ensure table_beginscan_tidrange() was used. */

	/* 确保使用了 table_beginscan_tidrange()。 */
	Assert((sscan->rs_flags & SO_TYPE_TIDRANGESCAN) != 0);

	sscan->rs_rd->rd_tableam->scan_rescan(sscan, NULL, false, false, false, false);
	sscan->rs_rd->rd_tableam->scan_set_tidrange(sscan, mintid, maxtid);
}

/*
 * Fetch the next tuple from `sscan` for a TID range scan created by
 * table_beginscan_tidrange().  Stores the tuple in `slot` and returns true,
 * or returns false if no more tuples exist in the range.
 *
 * 从由 table_beginscan_tidrange() 创建的 TID 范围扫描 `sscan` 中获取下一个元组。
 * 该函数将元组存入 `slot`；范围内仍有元组时返回 true，否则返回 false。
 */
/*
 * Function: table_scan_getnextslot_tidrange.
 * Purpose: Fetches the next tuple inside a configured TID range.
 * Core flow: It verifies TID-range scan mode and scan direction, then delegates to scan_getnextslot_tidrange to fill the destination slot.
 *
 * 函数：table_scan_getnextslot_tidrange。
 * 作用：获取配置的 TID 范围内的下一个元组。
 * 核心流程：它验证 TID 范围扫描模式和扫描方向，然后委托 scan_getnextslot_tidrange 填充目标槽。
 */
static inline bool
table_scan_getnextslot_tidrange(TableScanDesc sscan, ScanDirection direction,
								TupleTableSlot *slot)
{
	/* Ensure table_beginscan_tidrange() was used. */

	/* 确保使用了 table_beginscan_tidrange()。 */
	Assert((sscan->rs_flags & SO_TYPE_TIDRANGESCAN) != 0);

	/* We don't expect actual scans using NoMovementScanDirection */

	/* 我们不期望实际扫描使用 NoMovementScanDirection。 */
	Assert(direction == ForwardScanDirection ||
		   direction == BackwardScanDirection);

	return sscan->rs_rd->rd_tableam->scan_getnextslot_tidrange(sscan,
															   direction,
															   slot);
}


/* ----------------------------------------------------------------------------
 * Parallel table scan related functions.
 *
 * 并行表扫描相关函数。
 * ----------------------------------------------------------------------------
 */

/*
 * Estimate the size of shared memory needed for a parallel scan of this
 * relation.
 *
 * 估计并行扫描该关系所需的共享内存大小。
 */
/*
 * Function: table_parallelscan_estimate.
 * Purpose: Estimates shared-memory requirements for a parallel table scan.
 * Core flow: It combines the common parallel scan descriptor requirements with the relation access method’s private parallel-scan estimate.
 *
 * 函数：table_parallelscan_estimate。
 * 作用：估计并行表扫描的共享内存需求。
 * 核心流程：它将常见的并行扫描描述符要求与关系访问方法的私有并行扫描估计相结合。
 */
extern Size table_parallelscan_estimate(Relation rel, Snapshot snapshot);

/*
 * Initialize ParallelTableScanDesc for a parallel scan of this
 * relation. `pscan` needs to be sized according to parallelscan_estimate()
 * for the same relation.  Call this just once in the leader process; then,
 * individual workers attach via table_beginscan_parallel.
 *
 * 初始化 ParallelTableScanDesc 以并行扫描此关系。对于相同的关系，需要根据parallelscan_estimate()调整“pscan”的大小。  在领导进程中只调用一次；然后，各个工作人员通过 table_beginscan_parallel 附加。
 */
/*
 * Function: table_parallelscan_initialize.
 * Purpose: Initializes a shared descriptor for a parallel table scan.
 * Core flow: The leader initializes the descriptor and snapshot state for the relation before workers begin scans using it.
 *
 * 函数：table_parallelscan_initialize。
 * 作用：初始化并行表扫描的共享描述符。
 * 核心流程：在工作人员开始使用关系进行扫描之前，领导者会初始化该关系的描述符和快照状态。
 */
extern void table_parallelscan_initialize(Relation rel,
										  ParallelTableScanDesc pscan,
										  Snapshot snapshot);

/*
 * Begin a parallel scan. `pscan` needs to have been initialized with
 * table_parallelscan_initialize(), for the same relation. The initialization
 * does not need to have happened in this backend.
 *
 * 开始并行扫描。对于相同的关系，需要使用 table_parallelscan_initialize() 初始化 `pscan`。初始化不需要发生在这个后端中。
 *
 * Caller must hold a suitable lock on the relation.
 *
 * 调用者必须在关系上持有适当的锁。
 */
/*
 * Function: table_beginscan_parallel.
 * Purpose: Attaches a backend to an initialized parallel table scan.
 * Core flow: It uses the shared descriptor for the same locked relation and returns that backend’s scan descriptor.
 *
 * 函数：table_beginscan_parallel。
 * 作用：将后端附加到已初始化的并行表扫描。
 * 核心流程：它使用相同锁定关系的共享描述符并返回该后端的扫描描述符。
 */
extern TableScanDesc table_beginscan_parallel(Relation relation,
											  ParallelTableScanDesc pscan);

/*
 * Restart a parallel scan.  Call this in the leader process.  Caller is
 * responsible for making sure that all workers have finished the scan
 * beforehand.
 *
 * 重新启动并行扫描。  在领导者进程中调用此方法。  呼叫者负责确保所有工作人员都已提前完成扫描。
 */
/*
 * Function: table_parallelscan_reinitialize.
 * Purpose: Resets shared state for another parallel scan.
 * Core flow: The leader calls the relation access method’s parallelscan_reinitialize callback after all workers have finished the prior scan.
 *
 * 函数：table_parallelscan_reinitialize。
 * 作用：重置另一个并行扫描的共享状态。
 * 核心流程：在所有工作人员完成先前的扫描后，领导者调用关系访问方法的parallelscan_reinitialize回调。
 */
static inline void
table_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan)
{
	rel->rd_tableam->parallelscan_reinitialize(rel, pscan);
}


/* ----------------------------------------------------------------------------
 *  Index scan related functions.
 *
 * 索引扫描相关功能。
 * ----------------------------------------------------------------------------
 */

/*
 * Function: table_index_fetch_begin.
 * Purpose: Prepares relation tuple fetching for an index scan.
 * Core flow: It creates and returns IndexFetchTableData that later index fetch calls use to retrieve table tuples.
 *
 * 函数：table_index_fetch_begin。
 * 作用：为索引扫描准备关系元组提取。
 * 核心流程：它创建并返回 IndexFetchTableData，后续索引提取调用使用该结构获取表元组。
 */

/*
 * Prepare to fetch tuples from the relation, as needed when fetching tuples
 * for an index scan.
 *
 * 在为索引扫描获取元组时，根据需要准备从关系中获取元组。
 *
 * Tuples for an index scan can then be fetched via table_index_fetch_tuple().
 *
 * 然后可以通过 table_index_fetch_tuple() 获取用于索引扫描的元组。
 */
static inline IndexFetchTableData *
table_index_fetch_begin(Relation rel)
{
	return rel->rd_tableam->index_fetch_begin(rel);
}

/*
 * Reset index fetch. Typically this will release cross index fetch resources
 * held in IndexFetchTableData.
 *
 * 重置索引获取。通常，这将释放 IndexFetchTableData 中保存的交叉索引获取资源。
 */
/*
 * Function: table_index_fetch_reset.
 * Purpose: Resets reusable state for an index tuple fetch.
 * Core flow: It invokes index_fetch_reset so the access method releases resources retained across index fetches.
 *
 * 函数：table_index_fetch_reset。
 * 作用：重置索引元组提取可复用的状态。
 * 核心流程：它调用 index_fetch_reset，使访问方法释放跨索引提取持有的资源。
 */
static inline void
table_index_fetch_reset(struct IndexFetchTableData *scan)
{
	scan->rel->rd_tableam->index_fetch_reset(scan);
}

/*
 * Release resources and deallocate index fetch.
 *
 * 释放资源并取消分配索引提取。
 */
/*
 * Function: table_index_fetch_end.
 * Purpose: Ends an index tuple fetch operation.
 * Core flow: It invokes index_fetch_end so the access method releases and deallocates the fetch state.
 *
 * 函数：table_index_fetch_end。
 * 作用：结束一次索引元组提取操作。
 * 核心流程：它调用 index_fetch_end，使访问方法释放并回收提取状态。
 */
static inline void
table_index_fetch_end(struct IndexFetchTableData *scan)
{
	scan->rel->rd_tableam->index_fetch_end(scan);
}

/*
 * Fetches, as part of an index scan, tuple at `tid` into `slot`, after doing
 * a visibility test according to `snapshot`. If a tuple was found and passed
 * the visibility test, returns true, false otherwise. Note that *tid may be
 * modified when we return true (see later remarks on multiple row versions
 * reachable via a single index entry).
 *
 * 根据“快照”进行可见性测试后，作为索引扫描的一部分，将“tid”处的元组提取到“slot”中。如果找到元组并通过可见性测试，则返回 true，否则返回 false。请注意，当我们返回 true 时，*tid 可能会被修改（请参阅后面关于可通过单个索引条目访问的多个行版本的注释）。
 *
 * *call_again needs to be false on the first call to table_index_fetch_tuple() for
 * a tid. If there potentially is another tuple matching the tid, *call_again
 * will be set to true, signaling that table_index_fetch_tuple() should be called
 * again for the same tid.
 *
 * *call_again 在第一次调用 table_index_fetch_tuple() 获取 tid 时需要为 false。如果可能存在另一个与 tid 匹配的元组，*call_again 将设置为 true，表示应针对同一 tid 再次调用 table_index_fetch_tuple()。
 *
 * *all_dead, if all_dead is not NULL, will be set to true by
 * table_index_fetch_tuple() iff it is guaranteed that no backend needs to see
 * that tuple. Index AMs can use that to avoid returning that tid in future
 * searches.
 *
 * table_index_fetch_tuple() 前提是保证没有后端需要查看该元组。索引 AM 可以使用它来避免在将来的搜索中返回该 tid。
 *
 * The difference between this function and table_tuple_fetch_row_version()
 * is that this function returns the currently visible version of a row if
 * the AM supports storing multiple row versions reachable via a single index
 * entry (like heap's HOT). Whereas table_tuple_fetch_row_version() only
 * evaluates the tuple exactly at `tid`. Outside of index entry ->table tuple
 * lookups, table_tuple_fetch_row_version() is what's usually needed.
 *
 * 此函数与 table_tuple_fetch_row_version() 之间的区别在于，如果 AM 支持存储可通过单个索引条目访问的多个行版本（如堆的 HOT），则此函数将返回行的当前可见版本。而 table_tuple_fetch_row_version() 仅在“tid”处准确评估元组。在索引条目 -> 表元组查找之外，通常需要 table_tuple_fetch_row_version() 。
 */
/*
 * Function: table_index_fetch_tuple.
 * Purpose: Fetches the visible table tuple for an index TID.
 * Core flow: It performs visibility-aware fetching and reports retry or all-dead state through its output parameters.
 *
 * 函数：table_index_fetch_tuple。
 * 作用：提取索引 TID 对应的可见表元组。
 * 核心流程：它执行与可见性相关的提取，并通过输出参数报告重试或全死状态。
 */
static inline bool
table_index_fetch_tuple(struct IndexFetchTableData *scan,
						ItemPointer tid,
						Snapshot snapshot,
						TupleTableSlot *slot,
						bool *call_again, bool *all_dead)
{
	/*
	 * We don't expect direct calls to table_index_fetch_tuple with valid
	 * CheckXidAlive for catalog or regular tables.  See detailed comments in
	 * xact.c where these variables are declared.
 *
 * 我们不希望使用目录或常规表的有效 CheckXidAlive 直接调用 table_index_fetch_tuple 。  请参阅 xact.c 中声明这些变量的详细注释。
	 */
	if (unlikely(TransactionIdIsValid(CheckXidAlive) && !bsysscan))
		elog(ERROR, "unexpected table_index_fetch_tuple call during logical decoding");

	return scan->rel->rd_tableam->index_fetch_tuple(scan, tid, snapshot,
													slot, call_again,
													all_dead);
}

/*
 * This is a convenience wrapper around table_index_fetch_tuple() which
 * returns whether there are table tuple items corresponding to an index
 * entry.  This likely is only useful to verify if there's a conflict in a
 * unique index.
 *
 * 这是 table_index_fetch_tuple() 的便捷包装，它返回是否存在与索引条目对应的表元组项。  这可能仅用于验证唯一索引是否存在冲突。
 */
/*
 * Function: table_index_fetch_tuple_check.
 * Purpose: Checks whether an index entry has a matching table tuple.
 * Core flow: It performs the convenience index tuple lookup used primarily when checking possible unique-index conflicts.
 *
 * 函数：table_index_fetch_tuple_check。
 * 作用：检查索引条目是否有匹配的表元组。
 * 核心流程：它执行便捷的索引元组查找，主要用于检查可能的唯一索引冲突。
 */
extern bool table_index_fetch_tuple_check(Relation rel,
										  ItemPointer tid,
										  Snapshot snapshot,
										  bool *all_dead);


/* ------------------------------------------------------------------------
 * Functions for non-modifying operations on individual tuples
 *
 * 对单个元组进行非修改操作的函数
 * ------------------------------------------------------------------------
 */


/*
 * Fetch tuple at `tid` into `slot`, after doing a visibility test according to
 * `snapshot`. If a tuple was found and passed the visibility test, returns
 * true, false otherwise.
 *
 * 根据“snapshot”进行可见性测试后，将“tid”处的元组提取到“slot”中。如果找到元组并通过可见性测试，则返回 true，否则返回 false。
 *
 * See table_index_fetch_tuple's comment about what the difference between
 * these functions is. It is correct to use this function outside of index
 * entry->table tuple lookups.
 *
 * 有关这些函数之间的区别，请参阅 table_index_fetch_tuple 的评论。在索引条目->表元组查找之外使用此函数是正确的。
 */
/*
 * Function: table_tuple_fetch_row_version.
 * Purpose: Fetches the tuple version at a TID that is visible to a snapshot.
 * Core flow: It delegates the visibility check and fetch to tuple_fetch_row_version, storing a matching tuple in the slot.
 *
 * 函数：table_tuple_fetch_row_version。
 * 作用：提取给定快照下对 TID 可见的元组版本。
 * 核心流程：它将可见性检查和提取委托给 tuple_fetch_row_version，并将匹配元组存入槽中。
 */
static inline bool
table_tuple_fetch_row_version(Relation rel,
							  ItemPointer tid,
							  Snapshot snapshot,
							  TupleTableSlot *slot)
{
	/*
	 * We don't expect direct calls to table_tuple_fetch_row_version with
	 * valid CheckXidAlive for catalog or regular tables.  See detailed
	 * comments in xact.c where these variables are declared.
 *
 * 我们不希望使用目录表或常规表的有效 CheckXidAlive 直接调用 table_tuple_fetch_row_version 。  请参阅 xact.c 中声明这些变量的详细注释。
	 */
	if (unlikely(TransactionIdIsValid(CheckXidAlive) && !bsysscan))
		elog(ERROR, "unexpected table_tuple_fetch_row_version call during logical decoding");

	return rel->rd_tableam->tuple_fetch_row_version(rel, tid, snapshot, slot);
}

/*
 * Verify that `tid` is a potentially valid tuple identifier. That doesn't
 * mean that the pointed to row needs to exist or be visible, but that
 * attempting to fetch the row (e.g. with table_tuple_get_latest_tid() or
 * table_tuple_fetch_row_version()) should not error out if called with that
 * tid.
 *
 * 校验 `tid` 是否为一个潜在有效的元组标识符。这并不意味着所指向的行必须存在或可见，
 * 而是指用该 tid 尝试获取行（例如通过 table_tuple_get_latest_tid() 或
 * table_tuple_fetch_row_version()）时不应报错。
 *
 * `scan` needs to have been started via table_beginscan().
 *
 * `scan` 必须已经通过 table_beginscan() 启动。
 */
/*
 * Function: table_tuple_tid_valid.
 * Purpose: Checks whether the given tid is a potentially valid tuple identifier for the scan's relation.
 * Core flow: Delegates to the relation table AM's tuple_tid_valid callback and returns its boolean verdict.
 *
 * 函数：table_tuple_tid_valid。
 * 作用：检查给定 tid 对于扫描所在关系而言是否为潜在有效的元组标识符。
 * 核心流程：委托给关系表访问方法的 tuple_tid_valid 回调，并返回其布尔判定结果。
 */
static inline bool
table_tuple_tid_valid(TableScanDesc scan, ItemPointer tid)
{
	return scan->rs_rd->rd_tableam->tuple_tid_valid(scan, tid);
}

/*
 * Return the latest version of the tuple at `tid`, by updating `tid` to
 * point at the newest version.
 *
 * 返回 `tid` 处元组的最新版本，方法是将 `tid` 更新为指向该行的最新版本。
 */
/*
 * Function: table_tuple_get_latest_tid.
 * Purpose: Advances the given tid to point at the newest version of the tuple in the scanned relation.
 * Core flow: Resolves the relation from the scan and invokes the table AM's tuple_get_latest_tid callback, updating tid in place.
 *
 * 函数：table_tuple_get_latest_tid。
 * 作用：将给定 tid 推进到所扫描关系中该元组最新版本的位置。
 * 核心流程：从扫描中解析出关系，调用表访问方法的 tuple_get_latest_tid 回调，就地更新 tid。
 */
extern void table_tuple_get_latest_tid(TableScanDesc scan, ItemPointer tid);

/*
 * Return true iff tuple in slot satisfies the snapshot.
 *
 * 当且仅当槽中的元组满足该快照时返回 true。
 *
 * This assumes the slot's tuple is valid, and of the appropriate type for the
 * AM.
 *
 * 这里假定槽中的元组是有效的，且是该 AM 所期望的适当类型。
 *
 * Some AMs might modify the data underlying the tuple as a side-effect. If so
 * they ought to mark the relevant buffer dirty.
 *
 * 某些 AM 可能会作为副作用修改元组底层的数据。若如此，它们应当将相关缓冲区标记为脏。
 */
/*
 * Function: table_tuple_satisfies_snapshot.
 * Purpose: Determines whether the tuple held in the slot is visible under the supplied snapshot.
 * Core flow: Forwards the relation, slot, and snapshot to the table AM's tuple_satisfies_snapshot callback and returns its visibility verdict.
 *
 * 函数：table_tuple_satisfies_snapshot。
 * 作用：判定槽中持有的元组在给定快照下是否可见。
 * 核心流程：将关系、槽与快照转发给表访问方法的 tuple_satisfies_snapshot 回调，并返回其可见性判定结果。
 */
static inline bool
table_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot,
							   Snapshot snapshot)
{
	return rel->rd_tableam->tuple_satisfies_snapshot(rel, slot, snapshot);
}

/*
 * Determine which index tuples are safe to delete based on their table TID.
 *
 * 根据索引元组指向的表 TID，判定哪些索引元组可以安全删除。
 *
 * Determines which entries from index AM caller's TM_IndexDeleteOp state
 * point to vacuumable table tuples.  Entries that are found by tableam to be
 * vacuumable are naturally safe for index AM to delete, and so get directly
 * marked as deletable.  See comments above TM_IndexDelete and comments above
 * TM_IndexDeleteOp for full details.
 *
 * 判定索引 AM 调用者的 TM_IndexDeleteOp 状态中，哪些条目指向可回收（vacuumable）的表元组。
 * 被 tableam 认定为可回收的条目，对于索引 AM 而言天然可以安全删除，因此会被直接标记为可删除。
 * 完整细节参见 TM_IndexDelete 与 TM_IndexDeleteOp 上方的注释。
 *
 * Returns a snapshotConflictHorizon transaction ID that caller places in
 * its index deletion WAL record.  This might be used during subsequent REDO
 * of the WAL record when in Hot Standby mode -- a recovery conflict for the
 * index deletion operation might be required on the standby.
 *
 * 返回一个 snapshotConflictHorizon 事务 ID，调用者会将其放入索引删除的 WAL 记录中。
 * 该值可能在处于热备（Hot Standby）模式时、对该 WAL 记录进行后续 REDO 期间使用——
 * 备库上可能需要针对该索引删除操作触发恢复冲突。
 */
/*
 * Function: table_index_delete_tuples.
 * Purpose: Marks which of the caller's index entries reference vacuumable table tuples so the index AM can delete them.
 * Core flow: Delegates to the relation table AM's index_delete_tuples callback and returns the snapshotConflictHorizon xid for the deletion WAL record.
 *
 * 函数：table_index_delete_tuples。
 * 作用：标记调用者的哪些索引条目引用了可回收的表元组，以便索引 AM 将其删除。
 * 核心流程：委托给关系表访问方法的 index_delete_tuples 回调，并返回用于删除 WAL 记录的 snapshotConflictHorizon 事务 ID。
 */
static inline TransactionId
table_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate)
{
	return rel->rd_tableam->index_delete_tuples(rel, delstate);
}


/* ----------------------------------------------------------------------------
 *  Functions for manipulations of physical tuples.
 *
 * 用于操作物理元组的函数集合（插入、删除、更新等）。
 * ----------------------------------------------------------------------------
 */

/*
 * Insert a tuple from a slot into table AM routine.
 *
  * 将一个元组从槽插入到表 AM 例程中。
 *
 * The options bitmask allows the caller to specify options that may change the
 * behaviour of the AM. The AM will ignore options that it does not support.
 *
  * 选项位掩码允许调用者指定可能改变 AM 行为的选项。 AM 将忽略它不支持的选项。
 *
 * If the TABLE_INSERT_SKIP_FSM option is specified, AMs are free to not reuse
 * free space in the relation. This can save some cycles when we know the
 * relation is new and doesn't contain useful amounts of free space.
 * TABLE_INSERT_SKIP_FSM is commonly passed directly to
 * RelationGetBufferForTuple. See that method for more information.
 *
  * 如果指定了 TABLE_INSERT_SKIP_FSM 选项，AM 可以自由地不重用关系中的可用空间。当我们知道关系是新的并且不包含有用的可用空间时，这可以节省一些周期。 TABLE_INSERT_SKIP_FSM 通常直接传递给 RelationGetBufferForTuple。请参阅该方法以获取更多信息。
 *
 * TABLE_INSERT_FROZEN should only be specified for inserts into
 * relation storage created during the current subtransaction and when
 * there are no prior snapshots or pre-existing portals open.
 * This causes rows to be frozen, which is an MVCC violation and
 * requires explicit options chosen by user.
 *
  * TABLE_INSERT_FROZEN 只应在插入到当前子事务期间创建的关系存储以及没有打开先前快照或预先存在的门户时指定。这会导致行被冻结，这是一种 MVCC 违规，需要用户选择显式选项。
 *
 * TABLE_INSERT_NO_LOGICAL force-disables the emitting of logical decoding
 * information for the tuple. This should solely be used during table rewrites
 * where RelationIsLogicallyLogged(relation) is not yet accurate for the new
 * relation.
 *
  * TABLE_INSERT_NO_LOGICAL 强制禁用元组的逻辑解码信息的发出。这应该仅在表重写期间使用，其中 RelationIsLogicallyLogged(relation) 对于新关系尚不准确。
 *
 * Note that most of these options will be applied when inserting into the
 * heap's TOAST table, too, if the tuple requires any out-of-line data.
 *
  * 请注意，如果元组需要任何外联数据，则在插入堆的 TOAST 表时也会应用大多数选项。
 *
 * The BulkInsertState object (if any; bistate can be NULL for default
 * behavior) is also just passed through to RelationGetBufferForTuple. If
 * `bistate` is provided, table_finish_bulk_insert() needs to be called.
 *
  * BulkInsertState 对象（如果有的话；对于默认行为，双状态可以为 NULL）也只是传递给 RelationGetBufferForTuple。如果提供了“bistate”，则需要调用 table_finish_bulk_insert()。
 *
 * On return the slot's tts_tid and tts_tableOid are updated to reflect the
 * insertion. But note that any toasting of fields within the slot is NOT
 * reflected in the slots contents.
 *
  * 返回时，槽的 tts_tid 和 tts_tableOid 会更新以反映插入情况。但请注意，槽内字段的任何烘烤不会反映在槽内容中。
 */
/*
 * Function: table_tuple_insert.
 * Purpose: Inserts the tuple contained in the slot into the relation, honoring the given command id and option bitmask.
 * Core flow: Passes the slot, cid, options, and bulk-insert state to the table AM's tuple_insert callback, which fills in the slot's tts_tid/tts_tableOid.
 *
 * 函数：table_tuple_insert。
 * 作用：按给定的命令 ID 与选项位掩码，将槽中包含的元组插入到关系中。
 * 核心流程：将槽、cid、options 及批量插入状态传给表访问方法的 tuple_insert 回调，由其回填槽的 tts_tid/tts_tableOid。
 */
static inline void
table_tuple_insert(Relation rel, TupleTableSlot *slot, CommandId cid,
				   int options, struct BulkInsertStateData *bistate)
{
	rel->rd_tableam->tuple_insert(rel, slot, cid, options,
								  bistate);
}

/*
 * Perform a "speculative insertion". These can be backed out afterwards
 * without aborting the whole transaction.  Other sessions can wait for the
 * speculative insertion to be confirmed, turning it into a regular tuple, or
 * aborted, as if it never existed.  Speculatively inserted tuples behave as
 * "value locks" of short duration, used to implement INSERT .. ON CONFLICT.
 *
  * 执行“推测插入”。这些可以在事后撤销，而无需中止整个交易。  其他会话可以等待推测插入被确认，将其转换为常规元组，或者中止，就好像它从未存在过一样。  推测插入的元组表现为短期的“值锁”，用于实现 INSERT .. ON CONFLICT。
 *
 * A transaction having performed a speculative insertion has to either abort,
 * or finish the speculative insertion with
 * table_tuple_complete_speculative(succeeded = ...).
 *
  * 执行了推测性插入的事务必须中止或使用 table_tuple_complete_speculative(succeeded = ...) 完成推测性插入。
 */
/*
 * Function: table_tuple_insert_speculative.
 * Purpose: Performs a speculative insertion of the slot's tuple, tagging it with specToken so it can later be confirmed or backed out.
 * Core flow: Forwards the slot, cid, options, bulk-insert state, and specToken to the table AM's tuple_insert_speculative callback.
 *
 * 函数：table_tuple_insert_speculative。
 * 作用：对槽中的元组执行推测插入，用 specToken 标记它，以便随后确认或撤销。
 * 核心流程：将槽、cid、options、批量插入状态及 specToken 转发给表访问方法的 tuple_insert_speculative 回调。
 */
static inline void
table_tuple_insert_speculative(Relation rel, TupleTableSlot *slot,
							   CommandId cid, int options,
							   struct BulkInsertStateData *bistate,
							   uint32 specToken)
{
	rel->rd_tableam->tuple_insert_speculative(rel, slot, cid, options,
											  bistate, specToken);
}

/*
 * Complete "speculative insertion" started in the same transaction. If
 * succeeded is true, the tuple is fully inserted, if false, it's removed.
 *
  * 完整的“投机插入”在同一事务中开始。如果 success 为 true，则元组被完全插入，如果为 false，则将其删除。
 */
/*
 * Function: table_tuple_complete_speculative.
 * Purpose: Finishes a speculative insertion identified by specToken, either fully inserting the tuple (succeeded) or removing it.
 * Core flow: Forwards the slot, specToken, and succeeded flag to the table AM's tuple_complete_speculative callback.
 *
 * 函数：table_tuple_complete_speculative。
 * 作用：完成由 specToken 标识的推测插入：succeeded 为真则彻底插入该元组，否则将其移除。
 * 核心流程：将槽、specToken 与 succeeded 标志转发给表访问方法的 tuple_complete_speculative 回调。
 */
static inline void
table_tuple_complete_speculative(Relation rel, TupleTableSlot *slot,
								 uint32 specToken, bool succeeded)
{
	rel->rd_tableam->tuple_complete_speculative(rel, slot, specToken,
												succeeded);
}

/*
 * Insert multiple tuples into a table.
 *
  * 将多个元组插入表中。
 *
 * This is like table_tuple_insert(), but inserts multiple tuples in one
 * operation. That's often faster than calling table_tuple_insert() in a loop,
 * because e.g. the AM can reduce WAL logging and page locking overhead.
 *
  * 这类似于 table_tuple_insert()，但在一次操作中插入多个元组。这通常比在循环中调用 table_tuple_insert() 更快，因为例如AM 可以减少 WAL 日志记录和页面锁定开销。
 *
 * Except for taking `nslots` tuples as input, and an array of TupleTableSlots
 * in `slots`, the parameters for table_multi_insert() are the same as for
 * table_tuple_insert().
 *
  * 除了将“nslots”元组作为输入以及“slots”中的 TupleTableSlot 数组之外，table_multi_insert() 的参数与 table_tuple_insert() 相同。
 *
 * Note: this leaks memory into the current memory context. You can create a
 * temporary context before calling this, if that's a problem.
 *
  * 注意：这会将内存泄漏到当前内存上下文中。如果有问题，您可以在调用此方法之前创建一个临时上下文。
 */
/*
 * Function: table_multi_insert.
 * Purpose: Inserts nslots tuples from the slots array in a single operation, typically cheaper than looping over table_tuple_insert().
 * Core flow: Forwards the slot array, count, cid, options, and bulk-insert state to the table AM's multi_insert callback.
 *
 * 函数：table_multi_insert。
 * 作用：在一次操作中插入 slots 数组中的 nslots 个元组，通常比循环调用 table_tuple_insert() 更省开销。
 * 核心流程：将槽数组、数量、cid、options 及批量插入状态转发给表访问方法的 multi_insert 回调。
 */
static inline void
table_multi_insert(Relation rel, TupleTableSlot **slots, int nslots,
				   CommandId cid, int options, struct BulkInsertStateData *bistate)
{
	rel->rd_tableam->multi_insert(rel, slots, nslots,
								  cid, options, bistate);
}

/*
 * Delete a tuple.
 *
  * 删除一个元组。
 *
 * NB: do not call this directly unless prepared to deal with
 * concurrent-update conditions.  Use simple_table_tuple_delete instead.
 *
  * 注意：除非准备好处理并发更新条件，否则不要直接调用它。  请改用 simple_table_tuple_delete。
 *
 * Input parameters:
 *	relation - table to be modified (caller must hold suitable lock)
 *	tid - TID of tuple to be deleted
 *	cid - delete command ID (used for visibility test, and stored into
 *		cmax if successful)
 *	crosscheck - if not InvalidSnapshot, also check tuple against this
 *	wait - true if should wait for any conflicting update to commit/abort
 * Output parameters:
 *	tmfd - filled in failure cases (see below)
 *	changingPart - true iff the tuple is being moved to another partition
 *		table due to an update of the partition key. Otherwise, false.
 *
  * 输入参数：relation - 要修改的表（调用者必须持有合适的锁） tid - 要删除的元组的 TID cid - 删除命令 ID（用于可见性测试，如果成功则存储到 cmax 中） crosscheck - 如果不是 InvalidSnapshot，还针对此检查元组 wait - true 如果应该等待任何冲突的更新来提交/中止 输出参数： tmfd - 填充失败情况（见下文）changingPart - true 如果元组由于以下原因而被移动到另一个分区表分区键的更新。否则为假。
 *
 * Normal, successful return value is TM_Ok, which means we did actually
 * delete it.  Failure return codes are TM_SelfModified, TM_Updated, and
 * TM_BeingModified (the last only possible if wait == false).
 *
  * 正常情况下，成功返回值为TM_Ok，说明我们确实删除了它。  失败返回代码为 TM_SelfModified、TM_Updated 和 TM_BeingModified（最后一个仅在 wait == false 时才可能）。
 *
 * In the failure cases, the routine fills *tmfd with the tuple's t_ctid,
 * t_xmax, and, if possible, t_cmax.  See comments for struct
 * TM_FailureData for additional info.
 *
  * 在失败情况下，例程会使用元组的 t_ctid、t_xmax 和 t_cmax（如果可能）填充 *tmfd。  有关其他信息，请参阅结构 TM_FailureData 的注释。
 */
/*
 * Function: table_tuple_delete.
 * Purpose: Deletes the tuple at tid, returning TM_Ok on success or a TM_Result failure code that describes the concurrent-update conflict.
 * Core flow: Forwards tid, cid, snapshots, wait flag, and output params (tmfd, changingPart) to the table AM's tuple_delete callback and returns its TM_Result.
 *
 * 函数：table_tuple_delete。
 * 作用：删除 tid 处的元组；成功时返回 TM_Ok，否则返回描述并发更新冲突的 TM_Result 失败码。
 * 核心流程：将 tid、cid、快照、wait 标志及输出参数（tmfd、changingPart）转发给表访问方法的 tuple_delete 回调，并返回其 TM_Result。
 */
static inline TM_Result
table_tuple_delete(Relation rel, ItemPointer tid, CommandId cid,
				   Snapshot snapshot, Snapshot crosscheck, bool wait,
				   TM_FailureData *tmfd, bool changingPart)
{
	return rel->rd_tableam->tuple_delete(rel, tid, cid,
										 snapshot, crosscheck,
										 wait, tmfd, changingPart);
}

/*
 * Update a tuple.
 *
  * 更新一个元组。
 *
 * NB: do not call this directly unless you are prepared to deal with
 * concurrent-update conditions.  Use simple_table_tuple_update instead.
 *
  * 注意：除非您准备好处理并发更新条件，否则不要直接调用此函数。  使用 simple_table_tuple_update 代替。
 *
 * Input parameters:
 *	relation - table to be modified (caller must hold suitable lock)
 *	otid - TID of old tuple to be replaced
 *	slot - newly constructed tuple data to store
 *	cid - update command ID (used for visibility test, and stored into
 *		cmax/cmin if successful)
 *	crosscheck - if not InvalidSnapshot, also check old tuple against this
 *	wait - true if should wait for any conflicting update to commit/abort
 * Output parameters:
 *	tmfd - filled in failure cases (see below)
 *	lockmode - filled with lock mode acquired on tuple
 *	update_indexes - in success cases this is set if new index entries
 *		are required for this tuple; see TU_UpdateIndexes
 *
  * 输入参数：relation - 要修改的表（调用者必须持有合适的锁） otid - 要替换的旧元组的 TID slot - 新构建的元组数据来存储 cid - 更新命令 ID（用于可见性测试，如果成功则存储到 cmax/cmin 中） crosscheck - 如果不是 InvalidSnapshot，还针对此等待检查旧元组 - true 如果应该等待任何冲突的更新来提交/中止 输出参数： tmfd - 填写失败案例（见下文） lockmode -填充在元组 update_indexes 上获取的锁定模式 - 在成功的情况下，如果该元组需要新的索引条目，则设置此值；请参阅 TU_UpdateIndexes
 *
 * Normal, successful return value is TM_Ok, which means we did actually
 * update it.  Failure return codes are TM_SelfModified, TM_Updated, and
 * TM_BeingModified (the last only possible if wait == false).
 *
  * 正常情况下，成功的返回值是TM_Ok，这意味着我们确实更新了它。  失败返回代码为 TM_SelfModified、TM_Updated 和 TM_BeingModified（最后一个仅在 wait == false 时才可能）。
 *
 * On success, the slot's tts_tid and tts_tableOid are updated to match the new
 * stored tuple; in particular, slot->tts_tid is set to the TID where the
 * new tuple was inserted, and its HEAP_ONLY_TUPLE flag is set iff a HOT
 * update was done.  However, any TOAST changes in the new tuple's
 * data are not reflected into *newtup.
 *
  * 成功后，槽的 tts_tid 和 tts_tableOid 会更新以匹配新存储的元组；特别是，slot->tts_tid 设置为插入新元组的 TID，并且当完成热更新时设置其 HEAP_ONLY_TUPLE 标志。  但是，新元组数据中的任何 TOAST 更改都不会反映到 *newtup 中。
 *
 * In the failure cases, the routine fills *tmfd with the tuple's t_ctid,
 * t_xmax, and, if possible, t_cmax.  See comments for struct TM_FailureData
 * for additional info.
 *
  * 在失败情况下，例程会使用元组的 t_ctid、t_xmax 和 t_cmax（如果可能）填充 *tmfd。  有关其他信息，请参阅结构 TM_FailureData 的注释。
 */
/*
 * Function: table_tuple_update.
 * Purpose: Replaces the tuple at otid with the slot's data, returning TM_Ok on success or a TM_Result failure code on concurrent-update conflicts.
 * Core flow: Forwards otid, slot, cid, snapshots, wait flag, and output params (tmfd, lockmode, update_indexes) to the table AM's tuple_update callback and returns its TM_Result.
 *
 * 函数：table_tuple_update。
 * 作用：用槽中的数据替换 otid 处的元组；成功时返回 TM_Ok，发生并发更新冲突时返回相应的 TM_Result 失败码。
 * 核心流程：将 otid、槽、cid、快照、wait 标志及输出参数（tmfd、lockmode、update_indexes）转发给表访问方法的 tuple_update 回调，并返回其 TM_Result。
 */
static inline TM_Result
table_tuple_update(Relation rel, ItemPointer otid, TupleTableSlot *slot,
				   CommandId cid, Snapshot snapshot, Snapshot crosscheck,
				   bool wait, TM_FailureData *tmfd, LockTupleMode *lockmode,
				   TU_UpdateIndexes *update_indexes)
{
	return rel->rd_tableam->tuple_update(rel, otid, slot,
										 cid, snapshot, crosscheck,
										 wait, tmfd,
										 lockmode, update_indexes);
}

/*
 * Lock a tuple in the specified mode.
 *
 * 中文翻译：以指定模式锁定元组。
 *
 * Input parameters:
 *	relation: relation containing tuple (caller must hold suitable lock)
 *	tid: TID of tuple to lock (updated if an update chain was followed)
 *	snapshot: snapshot to use for visibility determinations
 *	cid: current command ID (used for visibility test, and stored into
 *		tuple's cmax if lock is successful)
 *	mode: lock mode desired
 *	wait_policy: what to do if tuple lock is not available
 *	flags:
 *		If TUPLE_LOCK_FLAG_LOCK_UPDATE_IN_PROGRESS, follow the update chain to
 *		also lock descendant tuples if lock modes don't conflict.
 *		If TUPLE_LOCK_FLAG_FIND_LAST_VERSION, follow the update chain and lock
 *		latest version.
 *
 * 中文翻译：输入参数：关系：包含元组的关系（调用者必须持有合适的锁） tid：要锁定的元组的 TID（如果遵循更新链则更新） snapshot：用于可见性确定的快照 cid：当前命令 ID（用于可见性测试，如果锁定成功则存储到元组的 cmax 中） mode：所需的锁定模式 wait_policy：如果元组锁不可用该怎么办 flags：如果 TUPLE_LOCK_FLAG_LOCK_UPDATE_IN_PROGRESS，如果锁定模式不冲突，则遵循更新链也锁定后代元组。如果TUPLE_LOCK_FLAG_FIND_LAST_VERSION，则遵循更新链并锁定最新版本。
 *
 * Output parameters:
 *	*slot: contains the target tuple
 *	*tmfd: filled in failure cases (see below)
 *
 * 中文翻译：输出参数： *slot：包含目标元组 *tmfd：填写失败案例（见下文）
 *
 * Function result may be:
 *	TM_Ok: lock was successfully acquired
 *	TM_Invisible: lock failed because tuple was never visible to us
 *	TM_SelfModified: lock failed because tuple updated by self
 *	TM_Updated: lock failed because tuple updated by other xact
 *	TM_Deleted: lock failed because tuple deleted by other xact
 *	TM_WouldBlock: lock couldn't be acquired and wait_policy is skip
 *
 * 中文翻译：函数结果可能是： TM_Ok：成功获取锁 TM_Invisible：锁失败，因为元组对我们永远不可见 TM_SelfModified：锁失败，因为元组被自己更新 TM_Updated：锁失败，因为元组被其他 xact 更新 TM_Deleted：锁失败，因为元组被其他 xact 删除 TM_WouldBlock：无法获取锁，并且 wait_policy 被跳过
 *
 * In the failure cases other than TM_Invisible and TM_Deleted, the routine
 * fills *tmfd with the tuple's t_ctid, t_xmax, and, if possible, t_cmax.
 * Additionally, in both success and failure cases, tmfd->traversed is set if
 * an update chain was followed.  See comments for struct TM_FailureData for
 * additional info.
 *
 * 中文翻译：在除 TM_Invisible 和 TM_Deleted 之外的失败情况下，例程会使用元组的 t_ctid、t_xmax 和 t_cmax（如果可能）填充 *tmfd。此外，在成功和失败的情况下，如果遵循更新链，则设置 tmfd->traversed。有关其他信息，请参阅结构 TM_FailureData 的注释。
 */
/*
 * Function: table_tuple_lock.
 * Purpose: Acquires the requested lock on a tuple visible to the supplied snapshot.
 * Core flow: Passes lock mode, wait policy, flags, and output state to tuple_lock, which reports the lock result and failure data.
 *
 * 函数：table_tuple_lock。
 * 作用：在给定快照下获取目标元组的指定锁。
 * 核心流程：将锁模式、等待策略、标志和输出状态交给 tuple_lock 回调，由其返回锁定结果及失败信息。
 */
static inline TM_Result
table_tuple_lock(Relation rel, ItemPointer tid, Snapshot snapshot,
				 TupleTableSlot *slot, CommandId cid, LockTupleMode mode,
				 LockWaitPolicy wait_policy, uint8 flags,
				 TM_FailureData *tmfd)
{
	return rel->rd_tableam->tuple_lock(rel, tid, snapshot, slot,
									   cid, mode, wait_policy,
									   flags, tmfd);
}

/*
 * Perform operations necessary to complete insertions made via
 * tuple_insert and multi_insert with a BulkInsertState specified.
 *
 * 中文翻译：执行必要的操作来完成通过 tuple_insert 和 multi_insert 指定的 BulkInsertState 进行的插入。
 */
/*
 * Function: table_finish_bulk_insert.
 * Purpose: Completes bulk insert work for a relation when its access method provides a completion callback.
 * Core flow: Tests for the optional finish_bulk_insert callback and invokes it with the relation and insertion options.
 *
 * 函数：table_finish_bulk_insert。
 * 作用：在表访问方法提供完成回调时结束关系的批量插入工作。
 * 核心流程：检查可选的 finish_bulk_insert 回调，并以关系和插入选项调用它。
 */
static inline void
table_finish_bulk_insert(Relation rel, int options)
{
	/* optional callback */

	/* 中文翻译：可选回调 */
	if (rel->rd_tableam && rel->rd_tableam->finish_bulk_insert)
		rel->rd_tableam->finish_bulk_insert(rel, options);
}


/* ------------------------------------------------------------------------
 * DDL related functionality.
 *
 * 中文翻译：DDL 相关功能。
 * ------------------------------------------------------------------------
 */

/*
 * Create storage for `rel` in `newrlocator`, with persistence set to
 * `persistence`.
 *
 * 中文翻译：在“newrlocator”中为“rel”创建存储，并将持久性设置为“persistence”。
 *
 * This is used both during relation creation and various DDL operations to
 * create new rel storage that can be filled from scratch.  When creating
 * new storage for an existing relfilelocator, this should be called before the
 * relcache entry has been updated.
 *
 * 中文翻译：这在关系创建和各种 DDL 操作期间使用，以创建可以从头开始填充的新关系存储。为现有 relfilelocator 创建新存储时，应在更新 relcache 条目之前调用此函数。
 *
 * *freezeXid, *minmulti are set to the xid / multixact horizon for the table
 * that pg_class.{relfrozenxid, relminmxid} have to be set to.
 *
 * 中文翻译：*freezeXid、*minmulti 设置为必须设置为 pg_class.{relfrozenxid, relminmxid} 的表的 xid / multixact 范围。
 */
/*
 * Function: table_relation_set_new_filelocator.
 * Purpose: Creates new storage for a relation at the supplied relfilelocator and persistence.
 * Core flow: Delegates storage creation to the AM and returns the freeze and multixact horizons through the output pointers.
 *
 * 函数：table_relation_set_new_filelocator。
 * 作用：按给定 relfilelocator 和持久化属性为关系创建新存储。
 * 核心流程：委托访问方法创建存储，并通过输出指针返回冻结和多事务视界。
 */
static inline void
table_relation_set_new_filelocator(Relation rel,
								   const RelFileLocator *newrlocator,
								   char persistence,
								   TransactionId *freezeXid,
								   MultiXactId *minmulti)
{
	rel->rd_tableam->relation_set_new_filelocator(rel, newrlocator,
												  persistence, freezeXid,
												  minmulti);
}

/*
 * Remove all table contents from `rel`, in a non-transactional manner.
 * Non-transactional meaning that there's no need to support rollbacks. This
 * commonly only is used to perform truncations for relation storage created in
 * the current transaction.
 *
 * 中文翻译：以非事务方式从“rel”中删除所有表内容。非事务性意味着不需要支持回滚。这通常仅用于对当前事务中创建的关系存储执行截断。
 */
/*
 * Function: table_relation_nontransactional_truncate.
 * Purpose: Truncates a relation's storage without performing transactional catalog work.
 * Core flow: Calls the AM's low-level truncate callback after callers have made the required locking and metadata changes.
 *
 * 函数：table_relation_nontransactional_truncate。
 * 作用：在不执行事务性目录操作的情况下截断关系存储。
 * 核心流程：调用者完成必要的加锁和元数据变更后，调用访问方法的底层截断回调。
 */
static inline void
table_relation_nontransactional_truncate(Relation rel)
{
	rel->rd_tableam->relation_nontransactional_truncate(rel);
}

/*
 * Copy data from `rel` into the new relfilelocator `newrlocator`. The new
 * relfilelocator may not have storage associated before this function is
 * called. This is only supposed to be used for low level operations like
 * changing a relation's tablespace.
 *
 * 中文翻译：将数据从 `rel` 复制到新的 relfilelocator `newrlocator` 中。在调用此函数之前，新的 relfilelocator 可能没有关联的存储。这只应该用于低级操作，例如更改关系的表空间。
 */
/*
 * Function: table_relation_copy_data.
 * Purpose: Copies relation data into storage identified by a new relfilelocator.
 * Core flow: Invokes the AM copy callback after the caller has created the destination storage and arranged required locks.
 *
 * 函数：table_relation_copy_data。
 * 作用：将关系数据复制到新 relfilelocator 标识的存储中。
 * 核心流程：调用者创建目标存储并取得必要锁后，调用访问方法的数据复制回调。
 */
static inline void
table_relation_copy_data(Relation rel, const RelFileLocator *newrlocator)
{
	rel->rd_tableam->relation_copy_data(rel, newrlocator);
}

/*
 * Copy data from `OldTable` into `NewTable`, as part of a CLUSTER or VACUUM
 * FULL.
 *
 * 中文翻译：将数据从“OldTable”复制到“NewTable”，作为 CLUSTER 或 VACUUM FULL 的一部分。
 *
 * Additional Input parameters:
 * - use_sort - if true, the table contents are sorted appropriate for
 *   `OldIndex`; if false and OldIndex is not InvalidOid, the data is copied
 *   in that index's order; if false and OldIndex is InvalidOid, no sorting is
 *   performed
 * - OldIndex - see use_sort
 * - OldestXmin - computed by vacuum_get_cutoffs(), even when
 *   not needed for the relation's AM
 * - *xid_cutoff - ditto
 * - *multi_cutoff - ditto
 *
 * 中文翻译：附加输入参数： - use_sort - 如果为 true，表内容将根据 `OldIndex` 进行排序；如果为 false 并且 OldIndex 不是 InvalidOid，则按照该索引的顺序复制数据；如果 false 且 OldIndex 为 InvalidOid，则不执行排序 - OldIndex - 请参阅 use_sort - OldestXmin - 由 Vacuum_get_cutoffs() 计算，即使关系的 AM 不需要 - *xid_cutoff - 同上 - *multi_cutoff - 同上
 *
 * Output parameters:
 * - *xid_cutoff - rel's new relfrozenxid value, may be invalid
 * - *multi_cutoff - rel's new relminmxid value, may be invalid
 * - *tups_vacuumed - stats, for logging, if appropriate for AM
 * - *tups_recently_dead - stats, for logging, if appropriate for AM
 *
 * 中文翻译：输出参数： - *xid_cutoff - rel 的新 relfrozenxid 值，可能无效 - *multi_cutoff - rel 新 relminmxid 值，可能无效 - *tups_vacuumed - 统计数据，用于日志记录，如果适用于 AM - *tups_recently_dead - 统计数据，用于日志记录，如果适用于 AM
 */
/*
 * Function: table_relation_copy_for_cluster.
 * Purpose: Copies tuples from an old table into a new table for CLUSTER or VACUUM FULL.
 * Core flow: Delegates the ordered or unordered copy to the AM and returns freeze cutoffs plus tuple statistics through output pointers.
 *
 * 函数：table_relation_copy_for_cluster。
 * 作用：为 CLUSTER 或 VACUUM FULL 将旧表元组复制到新表。
 * 核心流程：委托访问方法执行有序或无序复制，并通过输出指针返回冻结截点和元组统计信息。
 */
static inline void
table_relation_copy_for_cluster(Relation OldTable, Relation NewTable,
								Relation OldIndex,
								bool use_sort,
								TransactionId OldestXmin,
								TransactionId *xid_cutoff,
								MultiXactId *multi_cutoff,
								double *num_tuples,
								double *tups_vacuumed,
								double *tups_recently_dead)
{
	OldTable->rd_tableam->relation_copy_for_cluster(OldTable, NewTable, OldIndex,
													use_sort, OldestXmin,
													xid_cutoff, multi_cutoff,
													num_tuples, tups_vacuumed,
													tups_recently_dead);
}

/*
 * Perform VACUUM on the relation. The VACUUM can be triggered by a user or by
 * autovacuum. The specific actions performed by the AM will depend heavily on
 * the individual AM.
 *
 * 中文翻译：对关系执行 VACUUM。 VACUUM 可以由用户或自动真空触发。 AM 执行的具体操作在很大程度上取决于各个 AM。
 *
 * On entry a transaction needs to already been established, and the
 * table is locked with a ShareUpdateExclusive lock.
 *
 * 中文翻译：在进入时需要已经建立一个事务，并且该表被 ShareUpdateExclusive 锁锁定。
 *
 * Note that neither VACUUM FULL (and CLUSTER), nor ANALYZE go through this
 * routine, even if (for ANALYZE) it is part of the same VACUUM command.
 *
 * 中文翻译：请注意，VACUUM FULL（和 CLUSTER）和 ANALYZE 都不会执行此例程，即使（对于 ANALYZE）它是同一 VACUUM 命令的一部分。
 */
/*
 * Function: table_relation_vacuum.
 * Purpose: Runs the table access method's VACUUM processing for a relation.
 * Core flow: Passes vacuum parameters and the buffer strategy to relation_vacuum after the caller has opened and locked the relation.
 *
 * 函数：table_relation_vacuum。
 * 作用：对关系执行表访问方法的 VACUUM 处理。
 * 核心流程：调用者打开并锁定关系后，将 VACUUM 参数和缓冲策略传给 relation_vacuum 回调。
 */
static inline void
table_relation_vacuum(Relation rel, struct VacuumParams *params,
					  BufferAccessStrategy bstrategy)
{
	rel->rd_tableam->relation_vacuum(rel, params, bstrategy);
}

/*
 * Prepare to analyze the next block in the read stream. The scan needs to
 * have been  started with table_beginscan_analyze().  Note that this routine
 * might acquire resources like locks that are held until
 * table_scan_analyze_next_tuple() returns false.
 *
 * 中文翻译：准备分析读取流中的下一个块。扫描需要通过 table_beginscan_analyze() 开始。请注意，此例程可能会获取诸如锁之类的资源，这些资源会一直保留到 table_scan_analyze_next_tuple() 返回 false 为止。
 *
 * Returns false if block is unsuitable for sampling, true otherwise.
 *
 * 中文翻译：如果块不适合采样则返回 false，否则返回 true。
 */
/*
 * Function: table_scan_analyze_next_block.
 * Purpose: Selects the next block suitable for ANALYZE sampling.
 * Core flow: Invokes scan_analyze_next_block with the read stream and returns whether another sample block is available.
 *
 * 函数：table_scan_analyze_next_block。
 * 作用：选择下一个适合 ANALYZE 采样的数据块。
 * 核心流程：使用读取流调用 scan_analyze_next_block，并返回是否还有可采样的数据块。
 */
static inline bool
table_scan_analyze_next_block(TableScanDesc scan, ReadStream *stream)
{
	return scan->rs_rd->rd_tableam->scan_analyze_next_block(scan, stream);
}

/*
 * Iterate over tuples in the block selected with
 * table_scan_analyze_next_block() (which needs to have returned true, and
 * this routine may not have returned false for the same block before). If a
 * tuple that's suitable for sampling is found, true is returned and a tuple
 * is stored in `slot`.
 *
 * 中文翻译：迭代使用 table_scan_analyze_next_block() 选择的块中的元组（需要返回 true，并且此例程之前可能不会为同一块返回 false）。如果找到适合采样的元组，则返回 true 并将元组存储在“slot”中。
 *
 * *liverows and *deadrows are incremented according to the encountered
 * tuples.
 *
 * 中文翻译：*liverows 和 *deadrows 根据遇到的元组递增。
 */
/*
 * Function: table_scan_analyze_next_tuple.
 * Purpose: Finds the next sampleable tuple for ANALYZE and records it in a slot.
 * Core flow: Calls scan_analyze_next_tuple, which updates live and dead row counters and returns whether it stored a tuple.
 *
 * 函数：table_scan_analyze_next_tuple。
 * 作用：为 ANALYZE 查找下一个可采样元组并将其写入槽。
 * 核心流程：调用 scan_analyze_next_tuple，由其更新存活和死亡行计数，并返回是否已存入元组。
 */
static inline bool
table_scan_analyze_next_tuple(TableScanDesc scan, TransactionId OldestXmin,
							  double *liverows, double *deadrows,
							  TupleTableSlot *slot)
{
	return scan->rs_rd->rd_tableam->scan_analyze_next_tuple(scan, OldestXmin,
															liverows, deadrows,
															slot);
}

/*
 * table_index_build_scan - scan the table to find tuples to be indexed
 *
 * 中文翻译：table_index_build_scan - 扫描表以查找要索引的元组
 *
 * This is called back from an access-method-specific index build procedure
 * after the AM has done whatever setup it needs.  The parent table relation
 * is scanned to find tuples that should be entered into the index.  Each
 * such tuple is passed to the AM's callback routine, which does the right
 * things to add it to the new index.  After we return, the AM's index
 * build procedure does whatever cleanup it needs.
 *
 * 中文翻译：在 AM 完成所需的任何设置后，这是从特定于访问方法的索引构建过程回调的。扫描父表关系以查找应输入索引的元组。每个这样的元组都会传递到 AM 的回调例程，该例程会执行正确的操作将其添加到新索引中。我们返回后，AM 的索引构建过程会执行所需的任何清理工作。
 *
 * The total count of live tuples is returned.  This is for updating pg_class
 * statistics.  (It's annoying not to be able to do that here, but we want to
 * merge that update with others; see index_update_stats.)  Note that the
 * index AM itself must keep track of the number of index tuples; we don't do
 * so here because the AM might reject some of the tuples for its own reasons,
 * such as being unable to store NULLs.
 *
 * 中文翻译：返回活动元组的总数。这是为了更新 pg_class 统计信息。 （这里无法执行此操作很烦人，但我们希望将该更新与其他更新合并；请参阅index_update_stats。）请注意，索引 AM 本身必须跟踪索引元组的数量；我们在这里不这样做，因为 AM 可能会因其自身原因而拒绝某些元组，例如无法存储 NULL。
 *
 * If 'progress', the PROGRESS_SCAN_BLOCKS_TOTAL counter is updated when
 * starting the scan, and PROGRESS_SCAN_BLOCKS_DONE is updated as we go along.
 *
 * 中文翻译：如果“progress”，则 PROGRESS_SCAN_BLOCKS_TOTAL 计数器在开始扫描时更新，并且 PROGRESS_SCAN_BLOCKS_DONE 在扫描过程中更新。
 *
 * A side effect is to set indexInfo->ii_BrokenHotChain to true if we detect
 * any potentially broken HOT chains.  Currently, we set this if there are any
 * RECENTLY_DEAD or DELETE_IN_PROGRESS entries in a HOT chain, without trying
 * very hard to detect whether they're really incompatible with the chain tip.
 * This only really makes sense for heap AM, it might need to be generalized
 * for other AMs later.
 *
 * 中文翻译：副作用是，如果我们检测到任何可能损坏的 HOT 链，则将 indexInfo->ii_BrokenHotChain 设置为 true。目前，如果 HOT 链中有任何 RECENTLY_DEAD 或 DELETE_IN_PROGRESS 条目，我们会设置此值，而不是非常努力地检测它们是否真的与链提示不兼容。这仅对堆 AM 真正有意义，稍后可能需要将其推广到其他 AM。
 */
/*
 * Function: table_index_build_scan.
 * Purpose: Scans a table and supplies qualifying tuples to an index-build callback.
 * Core flow: Delegates the scan to index_build_range_scan semantics, updates progress and HOT-chain state, and returns the live tuple count.
 *
 * 函数：table_index_build_scan。
 * 作用：扫描表并将符合条件的元组提供给索引构建回调。
 * 核心流程：按 index_build_range_scan 的语义委托扫描，更新进度及 HOT 链状态，并返回存活元组数。
 */
static inline double
table_index_build_scan(Relation table_rel,
					   Relation index_rel,
					   struct IndexInfo *index_info,
					   bool allow_sync,
					   bool progress,
					   IndexBuildCallback callback,
					   void *callback_state,
					   TableScanDesc scan)
{
	return table_rel->rd_tableam->index_build_range_scan(table_rel,
														 index_rel,
														 index_info,
														 allow_sync,
														 false,
														 progress,
														 0,
														 InvalidBlockNumber,
														 callback,
														 callback_state,
														 scan);
}

/*
 * As table_index_build_scan(), except that instead of scanning the complete
 * table, only the given number of blocks are scanned.  Scan to end-of-rel can
 * be signaled by passing InvalidBlockNumber as numblocks.  Note that
 * restricting the range to scan cannot be done when requesting syncscan.
 *
 * 与 table_index_build_scan() 类似，但不扫描整张表，只扫描给定数量的块。
 * 传入 InvalidBlockNumber 作为 numblocks 可表示扫描到关系末尾。注意，当请求
 * 同步扫描（syncscan）时，无法限制扫描的块范围。
 *
 * When "anyvisible" mode is requested, all tuples visible to any transaction
 * are indexed and counted as live, including those inserted or deleted by
 * transactions that are still in progress.
 *
 * 当请求 "anyvisible" 模式时，对任意事务可见的所有元组都会被索引并计为存活，
 * 包括那些由仍在进行中的事务插入或删除的元组。
 */
/*
 * table_index_build_range_scan：在指定块范围内扫描表以构建索引。
 * 职责：将块范围（start_blockno 与 numblocks）及 anyvisible 等参数一并转发给
 *       表访问方法的 index_build_range_scan 回调，实现受限范围的索引构建扫描。
 * 流程：直接调用 rd_tableam->index_build_range_scan，并返回其统计到的元组数。
 */
static inline double
table_index_build_range_scan(Relation table_rel,
							 Relation index_rel,
							 struct IndexInfo *index_info,
							 bool allow_sync,
							 bool anyvisible,
							 bool progress,
							 BlockNumber start_blockno,
							 BlockNumber numblocks,
							 IndexBuildCallback callback,
							 void *callback_state,
							 TableScanDesc scan)
{
	return table_rel->rd_tableam->index_build_range_scan(table_rel,
														 index_rel,
														 index_info,
														 allow_sync,
														 anyvisible,
														 progress,
														 start_blockno,
														 numblocks,
														 callback,
														 callback_state,
														 scan);
}

/*
 * table_index_validate_scan - second table scan for concurrent index build
 *
 * 表的第二次扫描，用于并发索引构建（CREATE INDEX CONCURRENTLY）。
 *
 * See validate_index() for an explanation.
 *
 * 详细说明参见 validate_index()。
 */
/*
 * table_index_validate_scan：为并发索引构建执行验证阶段的表扫描。
 * 职责：将验证所需的快照与状态转发给表访问方法的 index_validate_scan 回调，
 *       以便将首次扫描后新增的元组补充进正在并发构建的索引中。
 * 流程：调用 rd_tableam->index_validate_scan，无返回值。
 */
static inline void
table_index_validate_scan(Relation table_rel,
						  Relation index_rel,
						  struct IndexInfo *index_info,
						  Snapshot snapshot,
						  struct ValidateIndexState *state)
{
	table_rel->rd_tableam->index_validate_scan(table_rel,
											   index_rel,
											   index_info,
											   snapshot,
											   state);
}


/* ----------------------------------------------------------------------------
 * Miscellaneous functionality
 *
 * 杂项功能：以下为表访问方法中不便归入其他类别的辅助接口。
 * ----------------------------------------------------------------------------
 */

/*
 * Return the current size of `rel` in bytes. If `forkNumber` is
 * InvalidForkNumber, return the relation's overall size, otherwise the size
 * for the indicated fork.
 *
 * 返回 `rel` 当前的大小（字节）。若 `forkNumber` 为 InvalidForkNumber，则返回
 * 关系的整体大小，否则返回指定分叉（fork）的大小。
 *
 * Note that the overall size might not be the equivalent of the sum of sizes
 * for the individual forks for some AMs, e.g. because the AMs storage does
 * not neatly map onto the builtin types of forks.
 *
 * 注意，对某些访问方法而言整体大小未必等于各个分叉大小之和，例如因为该访问方法
 * 的存储无法整齐地映射到内建的分叉类型上。
 */
/*
 * table_relation_size：获取关系（或其某个分叉）的当前字节大小。
 * 职责：将 forkNumber 转发给表访问方法的 relation_size 回调以查询实际占用。
 * 流程：调用 rd_tableam->relation_size 并返回其得到的字节数。
 */
static inline uint64
table_relation_size(Relation rel, ForkNumber forkNumber)
{
	return rel->rd_tableam->relation_size(rel, forkNumber);
}

/*
 * table_relation_needs_toast_table - does this relation need a toast table?
 *
 * 中文翻译：table_relation_needs_toast_table - 该关系是否需要 Toast 表？
 */
/*
 * Function: table_relation_needs_toast_table.
 * Purpose: Determines whether a relation requires an associated TOAST table.
 * Core flow: Calls the AM's relation_needs_toast_table callback and returns its boolean decision.
 *
 * 函数：table_relation_needs_toast_table。
 * 作用：确定关系是否需要关联的 TOAST 表。
 * 核心流程：调用访问方法的 relation_needs_toast_table 回调，并返回其布尔判断。
 */
static inline bool
table_relation_needs_toast_table(Relation rel)
{
	return rel->rd_tableam->relation_needs_toast_table(rel);
}

/*
 * Return the OID of the AM that should be used to implement the TOAST table
 * for this relation.
 *
 * 中文翻译：返回应用于实现此关系的 TOAST 表的 AM 的 OID。
 */
/*
 * Function: table_relation_toast_am.
 * Purpose: Obtains the access-method OID to use for the relation's TOAST table.
 * Core flow: Calls relation_toast_am on the relation's AM and returns the selected handler OID.
 *
 * 函数：table_relation_toast_am。
 * 作用：取得该关系 TOAST 表应使用的访问方法 OID。
 * 核心流程：调用关系访问方法的 relation_toast_am，并返回选定的处理器 OID。
 */
static inline Oid
table_relation_toast_am(Relation rel)
{
	return rel->rd_tableam->relation_toast_am(rel);
}

/*
 * Fetch all or part of a TOAST value from a TOAST table.
 *
 * 中文翻译：从 TOAST 表中获取全部或部分 TOAST 值。
 *
 * If this AM is never used to implement a TOAST table, then this callback
 * is not needed. But, if toasted values are ever stored in a table of this
 * type, then you will need this callback.
 *
 * 中文翻译：如果此 AM 从未用于实现 TOAST 表，则不需要此回调。但是，如果 toasted 值曾经存储在这种类型的表中，那么您将需要此回调。
 *
 * toastrel is the relation in which the toasted value is stored.
 *
 * 中文翻译：toastrel 是存储 toasted 值的关系。
 *
 * valueid identifies which toast value is to be fetched. For the heap,
 * this corresponds to the values stored in the chunk_id column.
 *
 * 中文翻译：valueid 标识要获取哪个 toast 值。对于堆，这对应于存储在 chunk_id 列中的值。
 *
 * attrsize is the total size of the toast value to be fetched.
 *
 * 中文翻译：attrsize 是要获取的 toast 值的总大小。
 *
 * sliceoffset is the offset within the toast value of the first byte that
 * should be fetched.
 *
 * 中文翻译：sliceoffset 是应获取的第一个字节的 toast 值内的偏移量。
 *
 * slicelength is the number of bytes from the toast value that should be
 * fetched.
 *
 * 中文翻译：slicelength 是应获取的 toast 值的字节数。
 *
 * result is caller-allocated space into which the fetched bytes should be
 * stored.
 *
 * 中文翻译：result 是调用者分配的空间，应在其中存储所获取的字节。
 */
/*
 * Function: table_relation_fetch_toast_slice.
 * Purpose: Fetches all or a requested byte slice of a TOAST value into caller-provided storage.
 * Core flow: Passes the TOAST relation, value identity, size, slice bounds, and result buffer to relation_fetch_toast_slice.
 *
 * 函数：table_relation_fetch_toast_slice。
 * 作用：将整个 TOAST 值或请求的字节切片读入调用方提供的存储。
 * 核心流程：把 TOAST 关系、值标识、大小、切片边界和结果缓冲区传给 relation_fetch_toast_slice。
 */
static inline void
table_relation_fetch_toast_slice(Relation toastrel, Oid valueid,
								 int32 attrsize, int32 sliceoffset,
								 int32 slicelength, struct varlena *result)
{
	toastrel->rd_tableam->relation_fetch_toast_slice(toastrel, valueid,
													 attrsize,
													 sliceoffset, slicelength,
													 result);
}


/* ----------------------------------------------------------------------------
 * Planner related functionality
 *
 * 中文翻译：规划器相关功能
 * ----------------------------------------------------------------------------
 */

/*
 * Estimate the current size of the relation, as an AM specific workhorse for
 * estimate_rel_size(). Look there for an explanation of the parameters.
 *
 * 中文翻译：估计关系的当前大小，作为estimate_rel_size() 的 AM 特定主力。在那里查找参数的解释。
 */
/*
 * Function: table_relation_estimate_size.
 * Purpose: Estimates a relation's current physical and logical size for planner costing.
 * Core flow: Calls relation_estimate_size to fill attribute widths, page and tuple estimates, and the all-visible fraction.
 *
 * 函数：table_relation_estimate_size。
 * 作用：为规划器代价估算关系当前的物理和逻辑大小。
 * 核心流程：调用 relation_estimate_size 填充属性宽度、页数和元组估计值以及全可见比例。
 */
static inline void
table_relation_estimate_size(Relation rel, int32 *attr_widths,
							 BlockNumber *pages, double *tuples,
							 double *allvisfrac)
{
	rel->rd_tableam->relation_estimate_size(rel, attr_widths, pages, tuples,
											allvisfrac);
}


/* ----------------------------------------------------------------------------
 * Executor related functionality
 *
 * 中文翻译：执行器相关功能
 * ----------------------------------------------------------------------------
 */

/*
 * Fetch / check / return tuples as part of a bitmap table scan. `scan` needs
 * to have been started via table_beginscan_bm(). Fetch the next tuple of a
 * bitmap table scan into `slot` and return true if a visible tuple was found,
 * false otherwise.
 *
 * 中文翻译：作为位图表扫描的一部分获取/检查/返回元组。 `scan` 需要通过 table_beginscan_bm() 启动。将位图表扫描的下一个元组获取到“slot”中，如果找到可见元组则返回 true，否则返回 false。
 *
 * `recheck` is set by the table AM to indicate whether or not the tuple in
 * `slot` should be rechecked. Tuples from lossy pages will always need to be
 * rechecked, but some non-lossy pages' tuples may also require recheck.
 *
 * 中文翻译：`recheck` 由表 AM 设置，指示是否应重新检查 `slot` 中的元组。有损页面的元组始终需要重新检查，但某些非有损页面的元组也可能需要重新检查。
 *
 * `lossy_pages` is incremented if the block's representation in the bitmap is
 * lossy; otherwise, `exact_pages` is incremented.
 *
 * 中文翻译：如果块在位图中的表示是有损的，则“lossy_pages”会递增；否则，“exact_pages”会增加。
 */
/*
 * Function: table_scan_bitmap_next_tuple.
 * Purpose: Fetches the next visible tuple of a bitmap table scan into a slot.
 * Core flow: Invokes scan_bitmap_next_tuple, which sets recheck state and updates lossy or exact page counters before returning availability.
 *
 * 函数：table_scan_bitmap_next_tuple。
 * 作用：将位图表扫描的下一个可见元组取入槽。
 * 核心流程：调用 scan_bitmap_next_tuple，由其设置重检状态、更新损失或精确页计数并返回是否找到元组。
 */
static inline bool
table_scan_bitmap_next_tuple(TableScanDesc scan,
							 TupleTableSlot *slot,
							 bool *recheck,
							 uint64 *lossy_pages,
							 uint64 *exact_pages)
{
	/*
	 * We don't expect direct calls to table_scan_bitmap_next_tuple with valid
	 * CheckXidAlive for catalog or regular tables.  See detailed comments in
	 * xact.c where these variables are declared.
 *
 * 中文翻译：我们不希望使用目录或常规表的有效 CheckXidAlive 直接调用 table_scan_bitmap_next_tuple 。请参阅 xact.c 中声明这些变量的详细注释。
	 */
	if (unlikely(TransactionIdIsValid(CheckXidAlive) && !bsysscan))
		elog(ERROR, "unexpected table_scan_bitmap_next_tuple call during logical decoding");

	return scan->rs_rd->rd_tableam->scan_bitmap_next_tuple(scan,
														   slot,
														   recheck,
														   lossy_pages,
														   exact_pages);
}

/*
 * Prepare to fetch tuples from the next block in a sample scan. Returns false
 * if the sample scan is finished, true otherwise. `scan` needs to have been
 * started via table_beginscan_sampling().
 *
 * 中文翻译：准备从示例扫描的下一个块中获取元组。如果样本扫描完成，则返回 false，否则返回 true。 `scan` 需要通过 table_beginscan_sampling() 启动。
 *
 * This will call the TsmRoutine's NextSampleBlock() callback if necessary
 * (i.e. NextSampleBlock is not NULL), or perform a sequential scan over the
 * underlying relation.
 *
 * 中文翻译：如果需要的话，这将调用 TsmRoutine 的 NextSampleBlock() 回调（即 NextSampleBlock 不为​​ NULL），或者对底层关系执行顺序扫描。
 */
/*
 * Function: table_scan_sample_next_block.
 * Purpose: Prepares the next block of a sampling scan for tuple sampling.
 * Core flow: Calls scan_sample_next_block, which uses the TSM callback when available or scans the underlying relation sequentially.
 *
 * 函数：table_scan_sample_next_block。
 * 作用：为采样扫描准备下一个用于元组采样的数据块。
 * 核心流程：调用 scan_sample_next_block；有 TSM 回调时使用该回调，否则顺序扫描底层关系。
 */
static inline bool
table_scan_sample_next_block(TableScanDesc scan,
							 struct SampleScanState *scanstate)
{
	/*
	 * We don't expect direct calls to table_scan_sample_next_block with valid
	 * CheckXidAlive for catalog or regular tables.  See detailed comments in
	 * xact.c where these variables are declared.
 *
 * 中文翻译：我们不希望使用目录表或常规表的有效 CheckXidAlive 直接调用 table_scan_sample_next_block。请参阅 xact.c 中声明这些变量的详细注释。
	 */
	if (unlikely(TransactionIdIsValid(CheckXidAlive) && !bsysscan))
		elog(ERROR, "unexpected table_scan_sample_next_block call during logical decoding");
	return scan->rs_rd->rd_tableam->scan_sample_next_block(scan, scanstate);
}

/*
 * Fetch the next sample tuple into `slot` and return true if a visible tuple
 * was found, false otherwise. table_scan_sample_next_block() needs to
 * previously have selected a block (i.e. returned true), and no previous
 * table_scan_sample_next_tuple() for the same block may have returned false.
 *
 * 中文翻译：将下一个样本元组取出到“slot”中，如果找到可见元组则返回 true，否则返回 false。 table_scan_sample_next_block() 需要事先选择一个块（即返回 true），并且同一块的先前 table_scan_sample_next_tuple() 可能不会返回 false。
 *
 * This will call the TsmRoutine's NextSampleTuple() callback.
 *
 * 中文翻译：这将调用 TsmRoutine 的 NextSampleTuple() 回调。
 */
/*
 * Function: table_scan_sample_next_tuple.
 * Purpose: Fetches the next tuple selected from the current sampling-scan block.
 * Core flow: Calls scan_sample_next_tuple with the scan state and slot, returning whether a sampled tuple was found.
 *
 * 函数：table_scan_sample_next_tuple。
 * 作用：从当前采样扫描数据块取得下一个被选中的元组。
 * 核心流程：使用扫描状态和槽调用 scan_sample_next_tuple，并返回是否找到采样元组。
 */
static inline bool
table_scan_sample_next_tuple(TableScanDesc scan,
							 struct SampleScanState *scanstate,
							 TupleTableSlot *slot)
{
	/*
	 * We don't expect direct calls to table_scan_sample_next_tuple with valid
	 * CheckXidAlive for catalog or regular tables.  See detailed comments in
	 * xact.c where these variables are declared.
 *
 * 中文翻译：我们不希望使用目录或常规表的有效 CheckXidAlive 直接调用 table_scan_sample_next_tuple 。请参阅 xact.c 中声明这些变量的详细注释。
	 */
	if (unlikely(TransactionIdIsValid(CheckXidAlive) && !bsysscan))
		elog(ERROR, "unexpected table_scan_sample_next_tuple call during logical decoding");
	return scan->rs_rd->rd_tableam->scan_sample_next_tuple(scan, scanstate,
														   slot);
}


/* ----------------------------------------------------------------------------
 * Functions to make modifications a bit simpler.
 *
 * 用于简化数据修改操作的函数。
 * ----------------------------------------------------------------------------
 */

/*
 * Function: simple_table_tuple_insert.
 * Purpose: Inserts the tuple stored in slot into rel.
 * Core flow: Delegates the relation and slot to the table AM, which performs the write and related WAL work.
 * Detailed role: Inserts the tuple stored in slot into rel.
 * Detailed flow: Delegates the relation and slot to the table AM, which performs the write and related WAL work.
 *
 * 函数：simple_table_tuple_insert。
 * 作用：通过关系的表访问方法插入 slot 中的元组。
 * 核心流程：将关系和槽位传给表访问方法，由访问方法完成写入及其常规 WAL 处理。
 */
extern void simple_table_tuple_insert(Relation rel, TupleTableSlot *slot);
/*
 * Function: simple_table_tuple_delete.
 * Purpose: Deletes the tuple identified by tid when it is visible under snapshot.
 * Core flow: Delegates the relation, target TID, and snapshot to the table AM for visibility checking and deletion.
 * Detailed role: Deletes the tuple identified by tid when it is visible under snapshot.
 * Detailed flow: Delegates the relation, target TID, and snapshot to the table AM for visibility checking and deletion.
 *
 * 函数：simple_table_tuple_delete。
 * 作用：在给定快照下，通过关系的表访问方法删除 tid 指向的元组。
 * 核心流程：将关系、目标 TID 和快照交给访问方法，由其执行可见性检查和删除。
 */
extern void simple_table_tuple_delete(Relation rel, ItemPointer tid,
									  Snapshot snapshot);
/*
 * Function: simple_table_tuple_update.
 * Purpose: Replaces the tuple at otid with the tuple held in slot.
 * Core flow: Supplies the snapshot and index-result pointer so the table AM can update the tuple and report index maintenance.
 * Detailed role: Replaces the tuple at otid with the tuple held in slot.
 * Detailed flow: Supplies the snapshot and index-result pointer so the table AM can update the tuple and report index maintenance.
 *
 * 函数：simple_table_tuple_update。
 * 作用：通过关系的表访问方法用 slot 中的值更新 otid 指向的元组。
 * 核心流程：传入快照和索引更新结果指针，由访问方法更新元组并报告需要维护的索引。
 */
extern void simple_table_tuple_update(Relation rel, ItemPointer otid,
									  TupleTableSlot *slot, Snapshot snapshot,
									  TU_UpdateIndexes *update_indexes);


/* ----------------------------------------------------------------------------
 * Helper functions to implement parallel scans for block oriented AMs.
 *
 * 为面向块的访问方法实现并行扫描的辅助函数。
 * ----------------------------------------------------------------------------
 */

/*
 * Function: table_block_parallelscan_estimate.
 * Purpose: Estimates the shared-memory space required for a block-oriented parallel scan.
 * Core flow: Uses relation metadata to calculate and return the size of the parallel block-scan descriptor.
 * Detailed role: Estimates the shared-memory space required for a block-oriented parallel scan.
 * Detailed flow: Uses relation metadata to calculate and return the size of the parallel block-scan descriptor.
 *
 * 函数：table_block_parallelscan_estimate。
 * 作用：估算面向块的并行扫描所需的额外共享内存大小。
 * 核心流程：根据关系的表访问方法计算并行块扫描描述符所需的空间并返回该大小。
 */
extern Size table_block_parallelscan_estimate(Relation rel);
/*
 * Function: table_block_parallelscan_initialize.
 * Purpose: Initializes a caller-allocated parallel block-scan descriptor.
 * Core flow: Populates pscan from relation metadata and returns the portion used for attached block-scan state.
 * Detailed role: Initializes a caller-allocated parallel block-scan descriptor.
 * Detailed flow: Populates pscan from relation metadata and returns the portion used for attached block-scan state.
 *
 * 函数：table_block_parallelscan_initialize。
 * 作用：初始化调用方分配的并行块扫描共享描述符。
 * 核心流程：结合关系元数据填充 pscan，并返回为附加块扫描状态实际使用的空间大小。
 */
extern Size table_block_parallelscan_initialize(Relation rel,
												ParallelTableScanDesc pscan);
/*
 * Function: table_block_parallelscan_reinitialize.
 * Purpose: Resets a parallel block scan so it can be run again.
 * Core flow: Resets shared progress so later workers obtain pages from a new scan cycle.
 * Detailed role: Resets a parallel block scan so it can be run again.
 * Detailed flow: Resets shared progress so later workers obtain pages from a new scan cycle.
 *
 * 函数：table_block_parallelscan_reinitialize。
 * 作用：将并行块扫描描述符重置为可重新扫描的状态。
 * 核心流程：清除或重置共享扫描进度，使后续工作进程从新的扫描周期重新领取页面。
 */
extern void table_block_parallelscan_reinitialize(Relation rel,
												  ParallelTableScanDesc pscan);
/*
 * Function: table_block_parallelscan_nextpage.
 * Purpose: Obtains the next data block for a parallel worker to scan.
 * Core flow: Coordinates through the shared block-scan state and returns a terminal block number when the scan is exhausted.
 * Detailed role: Obtains the next data block for a parallel worker to scan.
 * Detailed flow: Coordinates through the shared block-scan state and returns a terminal block number when the scan is exhausted.
 *
 * 函数：table_block_parallelscan_nextpage。
 * 作用：为并行工作进程取得下一个要扫描的数据块号。
 * 核心流程：依据共享块扫描状态协调工作进程，分配下一页并在扫描完成时返回终止页号。
 */
extern BlockNumber table_block_parallelscan_nextpage(Relation rel,
													 ParallelBlockTableScanWorker pbscanwork,
													 ParallelBlockTableScanDesc pbscan);
/*
 * Function: table_block_parallelscan_startblock_init.
 * Purpose: Initializes the start block and worker state for a parallel block scan.
 * Core flow: Derives a start position from rel and pscan, then stores it in the worker's block-scan state.
 * Detailed role: Initializes the start block and worker state for a parallel block scan.
 * Detailed flow: Derives a start position from rel and pscan, then stores it in the worker's block-scan state.
 *
 * 函数：table_block_parallelscan_startblock_init。
 * 作用：初始化并行块扫描的起始块及工作进程状态。
 * 核心流程：根据关系和共享扫描描述符确定起始位置，并将该位置写入工作进程的块扫描状态。
 */
extern void table_block_parallelscan_startblock_init(Relation rel,
													 ParallelBlockTableScanWorker pbscanwork,
													 ParallelBlockTableScanDesc pbscan);


/* ----------------------------------------------------------------------------
 * Helper functions to implement relation sizing for block oriented AMs.
 *
 * 为面向块的访问方法实现关系大小估算的辅助函数。
 * ----------------------------------------------------------------------------
 */

/*
 * Function: table_block_relation_size.
 * Purpose: Returns the physical size of the requested relation fork.
 * Core flow: Queries the table AM's target fork and reports its current size in bytes.
 * Detailed role: Returns the physical size of the requested relation fork.
 * Detailed flow: Queries the table AM's target fork and reports its current size in bytes.
 *
 * 函数：table_block_relation_size。
 * 作用：返回关系指定 fork 的物理大小。
 * 核心流程：查询表访问方法管理的目标 fork，并以字节数返回其当前大小。
 */
extern uint64 table_block_relation_size(Relation rel, ForkNumber forkNumber);
/*
 * Function: table_block_relation_estimate_size.
 * Purpose: Estimates relation pages, tuples, visibility fraction, and attribute widths.
 * Core flow: Combines usable page bytes and tuple overhead to produce planner-facing size statistics.
 * Detailed role: Estimates relation pages, tuples, visibility fraction, and attribute widths.
 * Detailed flow: Combines usable page bytes and tuple overhead to produce planner-facing size statistics.
 *
 * 函数：table_block_relation_estimate_size。
 * 作用：估算关系页数、元组数、全可见比例及平均属性宽度。
 * 核心流程：结合每页可用字节和元组开销，计算供规划器使用的关系规模统计信息。
 */
extern void table_block_relation_estimate_size(Relation rel,
											   int32 *attr_widths,
											   BlockNumber *pages,
											   double *tuples,
											   double *allvisfrac,
											   Size overhead_bytes_per_tuple,
											   Size usable_bytes_per_page);

/* ----------------------------------------------------------------------------
 * Functions in tableamapi.c
 *
 * tableamapi.c 中定义的表访问方法函数。
 * ----------------------------------------------------------------------------
 */

/*
 * Function: GetTableAmRoutine.
 * Purpose: Obtains the TableAmRoutine for a table access method handler OID.
 * Core flow: Looks up and validates the handler, invokes it, and returns the initialized method routine table.
 * Detailed role: Obtains the TableAmRoutine for a table access method handler OID.
 * Detailed flow: Looks up and validates the handler, invokes it, and returns the initialized method routine table.
 *
 * 函数：GetTableAmRoutine。
 * 作用：取得指定表访问方法处理器 OID 对应的 TableAmRoutine。
 * 核心流程：查找并验证处理器函数，调用其回调并返回已初始化的访问方法例程表。
 */
extern const TableAmRoutine *GetTableAmRoutine(Oid amhandler);

/* ----------------------------------------------------------------------------
 * Functions in heapam_handler.c
 *
 * heapam_handler.c 中定义的堆表访问方法函数。
 * ----------------------------------------------------------------------------
 */

/*
 * Function: GetHeapamTableAmRoutine.
 * Purpose: Returns the TableAmRoutine for the built-in heap table access method.
 * Core flow: Constructs or obtains heap's callback routine table for use by the table-AM framework.
 * Detailed role: Returns the TableAmRoutine for the built-in heap table access method.
 * Detailed flow: Constructs or obtains heap's callback routine table for use by the table-AM framework.
 *
 * 函数：GetHeapamTableAmRoutine。
 * 作用：返回内置 heap 表访问方法的 TableAmRoutine。
 * 核心流程：构造或取得 heap 访问方法的回调例程表，并将其提供给表访问方法框架。
 */
extern const TableAmRoutine *GetHeapamTableAmRoutine(void);

#endif							/* TABLEAM_H */

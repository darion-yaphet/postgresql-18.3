/*-------------------------------------------------------------------------
 *
 * fdwapi.h
 *	  API for foreign-data wrappers
 *    外部数据包装器（FDW）的 API
 *
 * Copyright (c) 2010-2025, PostgreSQL Global Development Group
 *
 * src/include/foreign/fdwapi.h
 *
 * Core Flow:
 * 核心流程：
 * 1. Registration: A FDW is registered using CREATE FOREIGN DATA WRAPPER, pointing to a handler function.
 *    注册：使用 CREATE FOREIGN DATA WRAPPER 注册 FDW，并指向一个处理函数。
 * 2. Routine Loading: When a query involves a foreign table, PostgreSQL calls the handler to get a FdwRoutine struct.
 *    例程加载：当查询涉及外部表时，PostgreSQL 调用处理程序来获取 FdwRoutine 结构。
 * 3. Planning: The planner invokes callbacks like GetForeignRelSize/Paths/Plan to define how to access the foreign data.
 *    规划：规划器调用 GetForeignRelSize/Paths/Plan 等回调函数来定义如何访问外部数据。
 * 4. Execution: The executor calls BeginForeignScan/IterateForeignScan/EndForeignScan to perform the actual data retrieval.
 *    执行：执行器调用 BeginForeignScan/IterateForeignScan/EndForeignScan 来执行实际的数据检索。
 * 5. Optional Features: FDWs can implement DML (Insert/Update/Delete), join pushdown, and aggregate pushdown.
 *    可选特性：FDW 可以实现 DML（插入/更新/删除）、连接下推和聚合下推。
 *
 *-------------------------------------------------------------------------
 */
#ifndef FDWAPI_H
#define FDWAPI_H

#include "access/parallel.h"
#include "nodes/execnodes.h"
#include "nodes/pathnodes.h"

/* To avoid including explain.h here, reference ExplainState thus: */
/* 为了避免在这里包含 explain.h，通过这种方式引用 ExplainState： */
struct ExplainState;


/*
 * Callback function signatures --- see fdwhandler.sgml for more info.
 * 回调函数签名 —— 更多信息请参考 fdwhandler.sgml。
 */

/* Get estimated size for a foreign relation / 获取外部关系的预计大小（行数和宽度） */
typedef void (*GetForeignRelSize_function) (PlannerInfo *root,
											RelOptInfo *baserel,
											Oid foreigntableid);

/* Get possible access paths for a foreign relation / 为外部关系获取可能的访问路径 */
typedef void (*GetForeignPaths_function) (PlannerInfo *root,
										  RelOptInfo *baserel,
										  Oid foreigntableid);

/* Create a ForeignScan plan node from a selected path / 从所选路径创建 ForeignScan 计划节点 */
typedef ForeignScan *(*GetForeignPlan_function) (PlannerInfo *root,
												 RelOptInfo *baserel,
												 Oid foreigntableid,
												 ForeignPath *best_path,
												 List *tlist,
												 List *scan_clauses,
												 Plan *outer_plan);

/* Begin execution of a foreign scan / 开始执行外部扫描 */
typedef void (*BeginForeignScan_function) (ForeignScanState *node,
										   int eflags);

/* Fetch next tuple from a foreign scan / 从外部扫描中获取下一个元组 */
typedef TupleTableSlot *(*IterateForeignScan_function) (ForeignScanState *node);

/* Recheck a tuple previously returned by IterateForeignScan / 重新检查之前由 IterateForeignScan 返回的元组 */
typedef bool (*RecheckForeignScan_function) (ForeignScanState *node,
											 TupleTableSlot *slot);

/* Restart a foreign scan / 重启外部扫描 */
typedef void (*ReScanForeignScan_function) (ForeignScanState *node);

/* End a foreign scan and cleanup / 结束外部扫描并进行清理 */
typedef void (*EndForeignScan_function) (ForeignScanState *node);

/* Get possible access paths for a foreign join / 为外部连接获取可能的访问路径 */
typedef void (*GetForeignJoinPaths_function) (PlannerInfo *root,
											  RelOptInfo *joinrel,
											  RelOptInfo *outerrel,
											  RelOptInfo *innerrel,
											  JoinType jointype,
											  JoinPathExtraData *extra);

/* Get paths for foreign upper relations (grouping, aggregation, etc) / 为外部上层关系（分组、聚合等）获取路径 */
typedef void (*GetForeignUpperPaths_function) (PlannerInfo *root,
											   UpperRelationKind stage,
											   RelOptInfo *input_rel,
											   RelOptInfo *output_rel,
											   void *extra);

/* Add metadata needed for updating a foreign table / 为更新外部表添加所需的元数据 */
typedef void (*AddForeignUpdateTargets_function) (PlannerInfo *root,
												  Index rtindex,
												  RangeTblEntry *target_rte,
												  Relation target_relation);

/* Plan a modify operation (INSERT/UPDATE/DELETE) / 规划修改操作（插入/更新/删除） */
typedef List *(*PlanForeignModify_function) (PlannerInfo *root,
											 ModifyTable *plan,
											 Index resultRelation,
											 int subplan_index);

/* Begin execution of a modify operation / 开始执行修改操作 */
typedef void (*BeginForeignModify_function) (ModifyTableState *mtstate,
											 ResultRelInfo *rinfo,
											 List *fdw_private,
											 int subplan_index,
											 int eflags);

/* Execute a single row insert / 执行单行插入 */
typedef TupleTableSlot *(*ExecForeignInsert_function) (EState *estate,
													   ResultRelInfo *rinfo,
													   TupleTableSlot *slot,
													   TupleTableSlot *planSlot);

/* Execute a batch of row inserts / 执行批量插入 */
typedef TupleTableSlot **(*ExecForeignBatchInsert_function) (EState *estate,
															 ResultRelInfo *rinfo,
															 TupleTableSlot **slots,
															 TupleTableSlot **planSlots,
															 int *numSlots);

/* Get desired batch size for insert operations / 获取插入操作所需的批处理大小 */
typedef int (*GetForeignModifyBatchSize_function) (ResultRelInfo *rinfo);

/* Execute a single row update / 执行单行更新 */
typedef TupleTableSlot *(*ExecForeignUpdate_function) (EState *estate,
													   ResultRelInfo *rinfo,
													   TupleTableSlot *slot,
													   TupleTableSlot *planSlot);

/* Execute a single row delete / 执行单行删除 */
typedef TupleTableSlot *(*ExecForeignDelete_function) (EState *estate,
													   ResultRelInfo *rinfo,
													   TupleTableSlot *slot,
													   TupleTableSlot *planSlot);

/* End modify operations and cleanup / 结束修改操作并进行清理 */
typedef void (*EndForeignModify_function) (EState *estate,
										   ResultRelInfo *rinfo);

/* Begin row insertion for COPY / 为 COPY 开始执行行插入 */
typedef void (*BeginForeignInsert_function) (ModifyTableState *mtstate,
											 ResultRelInfo *rinfo);

/* End row insertion for COPY / 结束 COPY 的行插入 */
typedef void (*EndForeignInsert_function) (EState *estate,
										   ResultRelInfo *rinfo);

/* Determine if a foreign table is updatable / 确定外部表是否可更新 */
typedef int (*IsForeignRelUpdatable_function) (Relation rel);

/* Plan direct modification of a remote table / 规划对远程表的直接修改 */
typedef bool (*PlanDirectModify_function) (PlannerInfo *root,
										   ModifyTable *plan,
										   Index resultRelation,
										   int subplan_index);

/* Begin direct modification of a remote table / 开始执行远程表的直接修改 */
typedef void (*BeginDirectModify_function) (ForeignScanState *node,
											int eflags);

/* Execute direct modification and fetch results / 执行直接修改并获取结果 */
typedef TupleTableSlot *(*IterateDirectModify_function) (ForeignScanState *node);

/* End direct modification / 结束直接修改 */
typedef void (*EndDirectModify_function) (ForeignScanState *node);

/* Get row mark type for SELECT FOR UPDATE/SHARE / 为 SELECT FOR UPDATE/SHARE 获取行标记类型 */
typedef RowMarkType (*GetForeignRowMarkType_function) (RangeTblEntry *rte,
													   LockClauseStrength strength);

/* Refetch a tuple from the remote for locking / 从远程重新获取元组以进行锁定 */
typedef void (*RefetchForeignRow_function) (EState *estate,
											ExecRowMark *erm,
											Datum rowid,
											TupleTableSlot *slot,
											bool *updated);

/* Provide info for EXPLAIN / 为 EXPLAIN 提供信息 */
typedef void (*ExplainForeignScan_function) (ForeignScanState *node,
											 struct ExplainState *es);

/* Provide info for EXPLAIN on modify operations / 为修改操作的 EXPLAIN 提供信息 */
typedef void (*ExplainForeignModify_function) (ModifyTableState *mtstate,
											   ResultRelInfo *rinfo,
											   List *fdw_private,
											   int subplan_index,
											   struct ExplainState *es);

/* Provide info for EXPLAIN on direct modifications / 为直接修改的 EXPLAIN 提供信息 */
typedef void (*ExplainDirectModify_function) (ForeignScanState *node,
											  struct ExplainState *es);

typedef int (*AcquireSampleRowsFunc) (Relation relation, int elevel,
									  HeapTuple *rows, int targrows,
									  double *totalrows,
									  double *totaldeadrows);

/* Enable sampling and analyze metrics on a foreign table / 启用外部表的采样和分析指标 */
typedef bool (*AnalyzeForeignTable_function) (Relation relation,
											  AcquireSampleRowsFunc *func,
											  BlockNumber *totalpages);

/* Create local definitions from a remote schema / 从远程模式创建本地定义 */
typedef List *(*ImportForeignSchema_function) (ImportForeignSchemaStmt *stmt,
											   Oid serverOid);

/* Execute TRUNCATE on a foreign table / 在外部表上执行 TRUNCATE */
typedef void (*ExecForeignTruncate_function) (List *rels,
											  DropBehavior behavior,
											  bool restart_seqs);

/* Estimate shared memory requirement for a parallel scan / 为并行扫描估算共享内存需求 */
typedef Size (*EstimateDSMForeignScan_function) (ForeignScanState *node,
												 ParallelContext *pcxt);
/* Initialize shared memory state for a parallel scan / 为并行扫描初始化共享内存状态 */
typedef void (*InitializeDSMForeignScan_function) (ForeignScanState *node,
												   ParallelContext *pcxt,
												   void *coordinate);
/* Re-initialize state for a parallel scan / 为并行扫描重新初始化状态 */
typedef void (*ReInitializeDSMForeignScan_function) (ForeignScanState *node,
													 ParallelContext *pcxt,
													 void *coordinate);
/* Initialize a parallel worker for a scan / 为扫描初始化并行工作进程 */
typedef void (*InitializeWorkerForeignScan_function) (ForeignScanState *node,
													  shm_toc *toc,
													  void *coordinate);
/* Cleanup state at end of a parallel scan / 并行扫描结束时清理状态 */
typedef void (*ShutdownForeignScan_function) (ForeignScanState *node);
/* Check if a scan can be run in parallel / 检查扫描是否可以并行运行 */
typedef bool (*IsForeignScanParallelSafe_function) (PlannerInfo *root,
													RelOptInfo *rel,
													RangeTblEntry *rte);
/* Re-parameterize a foreign path for a child relation / 为子关系重新参数化外部路径 */
typedef List *(*ReparameterizeForeignPathByChild_function) (PlannerInfo *root,
															List *fdw_private,
															RelOptInfo *child_rel);

/* Check if a path is capable of asynchronous execution / 检查路径是否具备异步执行能力 */
typedef bool (*IsForeignPathAsyncCapable_function) (ForeignPath *path);

/* Submit an asynchronous IO request / 提交一个异步 IO 请求 */
typedef void (*ForeignAsyncRequest_function) (AsyncRequest *areq);

/* Configure an asynchronous wait / 配置一次异步等待 */
typedef void (*ForeignAsyncConfigureWait_function) (AsyncRequest *areq);

/* Notify that an asynchronous request is complete / 通知异步请求已完成 */
typedef void (*ForeignAsyncNotify_function) (AsyncRequest *areq);

/*
 * FdwRoutine is the struct returned by a foreign-data wrapper's handler
 * function.  It provides pointers to the callback functions needed by the
 * planner and executor.
 * FdwRoutine 是外部数据包装器处理函数返回的结构体。它提供了规划器和执行器所需的回调函数的指针。
 *
 * More function pointers are likely to be added in the future.  Therefore
 * it's recommended that the handler initialize the struct with
 * makeNode(FdwRoutine) so that all fields are set to NULL.  This will
 * ensure that no fields are accidentally left undefined.
 * 未来可能会添加更多函数指针。因此，建议处理程序使用 makeNode(FdwRoutine) 初始化该结构体，
 * 以便将所有字段都设置为 NULL。这将确保不会意外地留下未定义的字段。
 */
typedef struct FdwRoutine
{
	NodeTag		type;

	/* Functions for scanning foreign tables */
	/* 扫描外部表的函数 */
	/* Estimate size of foreign relation / 计算外部关系的预计大小 */
	GetForeignRelSize_function GetForeignRelSize;
	/* Create possible access paths / 创建可能的访问路径 */
	GetForeignPaths_function GetForeignPaths;
	/* Create scanning plan node / 创建扫描计划节点 */
	GetForeignPlan_function GetForeignPlan;
	/* Initialize at beginning of scan / 在扫描开始时初始化 */
	BeginForeignScan_function BeginForeignScan;
	/* Fetch next tuple from source / 从源获取下一个元组 */
	IterateForeignScan_function IterateForeignScan;
	/* Reset at beginning of scan / 在扫描开始时重置 */
	ReScanForeignScan_function ReScanForeignScan;
	/* Cleanup at end of scan / 在扫描结束时清理 */
	EndForeignScan_function EndForeignScan;

	/*
	 * Remaining functions are optional.  Set the pointer to NULL for any that
	 * are not provided.
	 * 其余函数是可选的。对于未提供的任何函数，请将指针设置为 NULL。
	 */

	/* Functions for remote-join planning */
	/* 远程连接规划的函数 */
	/* Create paths for foreign join relations / 为外部连接关系创建路径 */
	GetForeignJoinPaths_function GetForeignJoinPaths;

	/* Functions for remote upper-relation (post scan/join) planning */
	/* 远程上层关系（扫描/连接之后）规划的函数 */
	/* Create paths for foreign upper relations / 为外部上层关系创建路径 */
	GetForeignUpperPaths_function GetForeignUpperPaths;

	/* Functions for updating foreign tables */
	/* 更新外部表的函数 */
	/* Add metadata targets for update / 为更新添加元数据目标 */
	AddForeignUpdateTargets_function AddForeignUpdateTargets;
	/* Plan modify operations / 规划修改操作 */
	PlanForeignModify_function PlanForeignModify;
	/* Initialize modify operations / 初始化修改操作 */
	BeginForeignModify_function BeginForeignModify;
	/* Execute a single row insert / 执行单行插入 */
	ExecForeignInsert_function ExecForeignInsert;
	/* Execute a batch of inserts / 执行批量插入 */
	ExecForeignBatchInsert_function ExecForeignBatchInsert;
	/* Get batch size for inserts / 获取插入的批处理大小 */
	GetForeignModifyBatchSize_function GetForeignModifyBatchSize;
	/* Execute a single row update / 执行单行更新 */
	ExecForeignUpdate_function ExecForeignUpdate;
	/* Execute a single row delete / 执行单行删除 */
	ExecForeignDelete_function ExecForeignDelete;
	/* Cleanup after modify operations / 修改操作后的清理 */
	EndForeignModify_function EndForeignModify;
	/* Initialize insert operations / 初始化插入操作 */
	BeginForeignInsert_function BeginForeignInsert;
	/* Cleanup after insert operations / 插入操作后的清理 */
	EndForeignInsert_function EndForeignInsert;
	/* Check if relation is updatable / 检查关系是否可更新 */
	IsForeignRelUpdatable_function IsForeignRelUpdatable;
	/* Plan direct modification on remote / 规划在远程直接修改 */
	PlanDirectModify_function PlanDirectModify;
	/* Initialize direct modification / 初始化直接修改 */
	BeginDirectModify_function BeginDirectModify;
	/* Execute direct modification / 执行直接修改 */
	IterateDirectModify_function IterateDirectModify;
	/* Cleanup after direct modification / 直接修改后的清理 */
	EndDirectModify_function EndDirectModify;

	/* Functions for SELECT FOR UPDATE/SHARE row locking */
	/* SELECT FOR UPDATE/SHARE 行锁定函数 */
	/* Determine locking type for RTE / 确定 RTE 的锁定类型 */
	GetForeignRowMarkType_function GetForeignRowMarkType;
	/* Refetch row for locking / 重新获取行进行锁定 */
	RefetchForeignRow_function RefetchForeignRow;
	/* Recheck scan integrity / 重新检查扫描完整性 */
	RecheckForeignScan_function RecheckForeignScan;

	/* Support functions for EXPLAIN */
	/* EXPLAIN 的支持函数 */
	/* Add FDW-specific explain info / 添加 FDW 特有的 explain 信息 */
	ExplainForeignScan_function ExplainForeignScan;
	/* Add modify-specific explain info / 添加修改特有的 explain 信息 */
	ExplainForeignModify_function ExplainForeignModify;
	/* Add direct-modify explain info / 添加直接修改的 explain 信息 */
	ExplainDirectModify_function ExplainDirectModify;

	/* Support functions for ANALYZE */
	/* ANALYZE 的支持函数 */
	/* Enable foreign table sampling / 启用外部表采样 */
	AnalyzeForeignTable_function AnalyzeForeignTable;

	/* Support functions for IMPORT FOREIGN SCHEMA */
	/* IMPORT FOREIGN SCHEMA 的支持函数 */
	/* Create local definitions from remote / 从远程创建本地定义 */
	ImportForeignSchema_function ImportForeignSchema;

	/* Support functions for TRUNCATE */
	/* TRUNCATE 的支持函数 */
	/* Execute remote truncate / 执行远程截断 */
	ExecForeignTruncate_function ExecForeignTruncate;

	/* Support functions for parallelism under Gather node */
	/* Gather 节点下的并行扫描支持函数 */
	/* Check if scan is parallel safe / 检查扫描是否并行安全 */
	IsForeignScanParallelSafe_function IsForeignScanParallelSafe;
	/* Estimate DSM requirement / 估算共享内存需求 */
	EstimateDSMForeignScan_function EstimateDSMForeignScan;
	/* Initialize DSM on leader / 在领导者节点初始化共享内存 */
	InitializeDSMForeignScan_function InitializeDSMForeignScan;
	/* Re-initialize DSM / 重新初始化共享内存 */
	ReInitializeDSMForeignScan_function ReInitializeDSMForeignScan;
	/* Initialize state in worker / 在工作进程中初始化状态 */
	InitializeWorkerForeignScan_function InitializeWorkerForeignScan;
	/* Cleanup parallel resources / 清理并行资源 */
	ShutdownForeignScan_function ShutdownForeignScan;

	/* Support functions for path reparameterization. */
	/* 路径重参数化的支持函数。 */
	/* Re-parameterize path for child rel / 为子关系重新参数化路径 */
	ReparameterizeForeignPathByChild_function ReparameterizeForeignPathByChild;

	/* Support functions for asynchronous execution */
	/* 异步执行的支持函数 */
	/* Check async capability / 检查异步能力 */
	IsForeignPathAsyncCapable_function IsForeignPathAsyncCapable;
	/* Submit async request / 提交异步请求 */
	ForeignAsyncRequest_function ForeignAsyncRequest;
	/* Configure wait for async / 配置异步等待 */
	ForeignAsyncConfigureWait_function ForeignAsyncConfigureWait;
	/* Notify when async completes / 异步完成时通知 */
	ForeignAsyncNotify_function ForeignAsyncNotify;
} FdwRoutine;


/* Functions in foreign/foreign.c */
/* foreign/foreign.c 中的函数 */
extern FdwRoutine *GetFdwRoutine(Oid fdwhandler);
extern Oid	GetForeignServerIdByRelId(Oid relid);
extern FdwRoutine *GetFdwRoutineByServerId(Oid serverid);
extern FdwRoutine *GetFdwRoutineByRelId(Oid relid);
extern FdwRoutine *GetFdwRoutineForRelation(Relation relation, bool makecopy);
extern bool IsImportableForeignTable(const char *tablename,
									 ImportForeignSchemaStmt *stmt);
extern Path *GetExistingLocalJoinPath(RelOptInfo *joinrel);

#endif							/* FDWAPI_H */

/*-------------------------------------------------------------------------
 *
 * postgres_fdw.c
 *		  Foreign-data wrapper for remote PostgreSQL servers
 *
 * Portions Copyright (c) 2012-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  contrib/postgres_fdw/postgres_fdw.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/pg_opfamily.h"
#include "commands/defrem.h"
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "executor/execAsync.h"
#include "foreign/fdwapi.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/appendinfo.h"
#include "optimizer/cost.h"
#include "optimizer/inherit.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/prep.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "postgres_fdw.h"
#include "storage/latch.h"
#include "utils/builtins.h"
#include "utils/float.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/sampling.h"
#include "utils/selfuncs.h"

PG_MODULE_MAGIC_EXT(
					.name = "postgres_fdw",
					.version = PG_VERSION
);

/* Default CPU cost to start up a foreign query.
 *
 * 启动外部查询的默认 CPU 成本。
 */
#define DEFAULT_FDW_STARTUP_COST	100.0

/* Default CPU cost to process 1 row (above and beyond cpu_tuple_cost).
 *
 * 处理 1 行的默认 CPU 成本（高于 cpu_tuple_cost）。
 */
#define DEFAULT_FDW_TUPLE_COST		0.2

/* If no remote estimates, assume a sort costs 20% extra
 *
 * 如果没有远程估计，假设排序额外花费 20%
 */
#define DEFAULT_FDW_SORT_MULTIPLIER 1.2

/*
 * Indexes of FDW-private information stored in fdw_private lists.
 *
 * 存储在 fdw_private 列表中的 FDW 私有信息的索引。
 *
 * These items are indexed with the enum FdwScanPrivateIndex, so an item
 * can be fetched with list_nth().  For example, to get the SELECT statement:
 *		sql = strVal(list_nth(fdw_private, FdwScanPrivateSelectSql));
 *
 * 这些项目使用枚举 FdwScanPrivateIndex 进行索引，因此可以使用 list_nth() 获取项目。  例如，要获取 SELECT 语句： sql = strVal(list_nth(fdw_private, FdwScanPrivateSelectSql));
 */
enum FdwScanPrivateIndex
{
	/* SQL statement to execute remotely (as a String node)
	 *
	 * 远程执行的 SQL 语句（作为字符串节点）
	 */
	FdwScanPrivateSelectSql,
	/* Integer list of attribute numbers retrieved by the SELECT
	 *
	 * 由 SELECT 检索的属性号的整数列表
	 */
	FdwScanPrivateRetrievedAttrs,
	/* Integer representing the desired fetch_size
	 *
	 * 表示所需 fetch_size 的整数
	 */
	FdwScanPrivateFetchSize,

	/*
	 * String describing join i.e. names of relations being joined and types
	 * of join, added when the scan is join
	 *
	 * 描述连接的字符串，即正在连接的关系的名称和连接的类型，在扫描连接时添加
	 */
	FdwScanPrivateRelations,
};

/*
 * Similarly, this enum describes what's kept in the fdw_private list for
 * a ModifyTable node referencing a postgres_fdw foreign table.  We store:
 *
 * 类似地，此枚举描述了引用 postgres_fdw 外部表的 ModifyTable 节点的 fdw_private 列表中保留的内容。  我们存储：
 *
 * 1) INSERT/UPDATE/DELETE statement text to be sent to the remote server
 * 2) Integer list of target attribute numbers for INSERT/UPDATE
 *	  (NIL for a DELETE)
 * 3) Length till the end of VALUES clause for INSERT
 *	  (-1 for a DELETE/UPDATE)
 * 4) Boolean flag showing if the remote query has a RETURNING clause
 * 5) Integer list of attribute numbers retrieved by RETURNING, if any
 *
 * 1) 要发送到远程服务器的 INSERT/UPDATE/DELETE 语句文本 2) INSERT/UPDATE 的目标属性号的整数列表（DELETE 为 NIL） 3) INSERT 的 VALUES 子句结束之前的长度（DELETE/UPDATE 为 -1） 4) 显示远程查询是否有 RETURNING 子句的布尔标志 5) 由 检索的属性号的整数列表返回，如果有的话
 */
enum FdwModifyPrivateIndex
{
	/* SQL statement to execute remotely (as a String node)
	 *
	 * 远程执行的 SQL 语句（作为字符串节点）
	 */
	FdwModifyPrivateUpdateSql,
	/* Integer list of target attribute numbers for INSERT/UPDATE
	 *
	 * INSERT/UPDATE 的目标属性号的整数列表
	 */
	FdwModifyPrivateTargetAttnums,
	/* Length till the end of VALUES clause (as an Integer node)
	 *
	 * 直到 VALUES 子句结束的长度（作为整数节点）
	 */
	FdwModifyPrivateLen,
	/* has-returning flag (as a Boolean node)
	 *
	 * has-returning 标志（作为布尔节点）
	 */
	FdwModifyPrivateHasReturning,
	/* Integer list of attribute numbers retrieved by RETURNING
	 *
	 * 通过 RETURNING 检索的属性号的整数列表
	 */
	FdwModifyPrivateRetrievedAttrs,
};

/*
 * Similarly, this enum describes what's kept in the fdw_private list for
 * a ForeignScan node that modifies a foreign table directly.  We store:
 *
 * 类似地，此枚举描述了直接修改外部表的ForeignScan 节点的fdw_private 列表中保存的内容。  我们存储：
 *
 * 1) UPDATE/DELETE statement text to be sent to the remote server
 * 2) Boolean flag showing if the remote query has a RETURNING clause
 * 3) Integer list of attribute numbers retrieved by RETURNING, if any
 * 4) Boolean flag showing if we set the command es_processed
 *
 * 1) 要发送到远程服务器的 UPDATE/DELETE 语句文本 2) 布尔标志，显示远程查询是否有 RETURNING 子句 3) 通过 RETURNING 检索的属性号的整数列表（如果有） 4) 布尔标志，显示我们是否设置命令 es_processed
 */
enum FdwDirectModifyPrivateIndex
{
	/* SQL statement to execute remotely (as a String node)
	 *
	 * 远程执行的 SQL 语句（作为字符串节点）
	 */
	FdwDirectModifyPrivateUpdateSql,
	/* has-returning flag (as a Boolean node)
	 *
	 * has-returning 标志（作为布尔节点）
	 */
	FdwDirectModifyPrivateHasReturning,
	/* Integer list of attribute numbers retrieved by RETURNING
	 *
	 * 通过 RETURNING 检索的属性号的整数列表
	 */
	FdwDirectModifyPrivateRetrievedAttrs,
	/* set-processed flag (as a Boolean node)
	 *
	 * 设置处理标志（作为布尔节点）
	 */
	FdwDirectModifyPrivateSetProcessed,
};

/*
 * Execution state of a foreign scan using postgres_fdw.
 *
 * 使用 postgres_fdw 进行外部扫描的执行状态。
 */
typedef struct PgFdwScanState
{
	Relation	rel;			/* relcache entry for the foreign table. NULL
								 * for a foreign join scan. */
	TupleDesc	tupdesc;		/* tuple descriptor of scan */
	AttInMetadata *attinmeta;	/* attribute datatype conversion metadata */

	/* extracted fdw_private data
	 *
	 * 提取的 fdw_private 数据
	 */
	char	   *query;			/* text of SELECT command */
	List	   *retrieved_attrs;	/* list of retrieved attribute numbers */

	/* for remote query execution
	 *
	 * 用于远程查询执行
	 */
	PGconn	   *conn;			/* connection for the scan */
	PgFdwConnState *conn_state; /* extra per-connection state */
	unsigned int cursor_number; /* quasi-unique ID for my cursor */
	bool		cursor_exists;	/* have we created the cursor? */
	int			numParams;		/* number of parameters passed to query */
	FmgrInfo   *param_flinfo;	/* output conversion functions for them */
	List	   *param_exprs;	/* executable expressions for param values */
	const char **param_values;	/* textual values of query parameters */

	/* for storing result tuples
	 *
	 * 用于存储结果元组
	 */
	HeapTuple  *tuples;			/* array of currently-retrieved tuples */
	int			num_tuples;		/* # of tuples in array */
	int			next_tuple;		/* index of next one to return */

	/* batch-level state, for optimizing rewinds and avoiding useless fetch
	 *
	 * 批处理级状态，用于优化倒带并避免无用的获取
	 */
	int			fetch_ct_2;		/* Min(# of fetches done, 2) */
	bool		eof_reached;	/* true if last fetch reached EOF */

	/* for asynchronous execution
	 *
	 * 用于异步执行
	 */
	bool		async_capable;	/* engage asynchronous-capable logic? */

	/* working memory contexts
	 *
	 * 工作记忆情境
	 */
	MemoryContext batch_cxt;	/* context holding current batch of tuples */
	MemoryContext temp_cxt;		/* context for per-tuple temporary data */

	int			fetch_size;		/* number of tuples per fetch */
} PgFdwScanState;

/*
 * Execution state of a foreign insert/update/delete operation.
 *
 * 外部插入/更新/删除操作的执行状态。
 */
typedef struct PgFdwModifyState
{
	Relation	rel;			/* relcache entry for the foreign table */
	AttInMetadata *attinmeta;	/* attribute datatype conversion metadata */

	/* for remote query execution
	 *
	 * 用于远程查询执行
	 */
	PGconn	   *conn;			/* connection for the scan */
	PgFdwConnState *conn_state; /* extra per-connection state */
	char	   *p_name;			/* name of prepared statement, if created */

	/* extracted fdw_private data
	 *
	 * 提取的 fdw_private 数据
	 */
	char	   *query;			/* text of INSERT/UPDATE/DELETE command */
	char	   *orig_query;		/* original text of INSERT command */
	List	   *target_attrs;	/* list of target attribute numbers */
	int			values_end;		/* length up to the end of VALUES */
	int			batch_size;		/* value of FDW option "batch_size" */
	bool		has_returning;	/* is there a RETURNING clause? */
	List	   *retrieved_attrs;	/* attr numbers retrieved by RETURNING */

	/* info about parameters for prepared statement
	 *
	 * 有关准备好的语句的参数的信息
	 */
	AttrNumber	ctidAttno;		/* attnum of input resjunk ctid column */
	int			p_nums;			/* number of parameters to transmit */
	FmgrInfo   *p_flinfo;		/* output conversion functions for them */

	/* batch operation stuff
	 *
	 * 批量操作的东西
	 */
	int			num_slots;		/* number of slots to insert */

	/* working memory context
	 *
	 * 工作记忆情境
	 */
	MemoryContext temp_cxt;		/* context for per-tuple temporary data */

	/* for update row movement if subplan result rel
	 *
	 * 用于更新行移动如果子计划结果 rel
	 */
	struct PgFdwModifyState *aux_fmstate;	/* foreign-insert state, if
											 * created */
} PgFdwModifyState;

/*
 * Execution state of a foreign scan that modifies a foreign table directly.
 *
 * 直接修改外部表的外部扫描的执行状态。
 */
typedef struct PgFdwDirectModifyState
{
	Relation	rel;			/* relcache entry for the foreign table */
	AttInMetadata *attinmeta;	/* attribute datatype conversion metadata */

	/* extracted fdw_private data
	 *
	 * 提取的 fdw_private 数据
	 */
	char	   *query;			/* text of UPDATE/DELETE command */
	bool		has_returning;	/* is there a RETURNING clause? */
	List	   *retrieved_attrs;	/* attr numbers retrieved by RETURNING */
	bool		set_processed;	/* do we set the command es_processed? */

	/* for remote query execution
	 *
	 * 用于远程查询执行
	 */
	PGconn	   *conn;			/* connection for the update */
	PgFdwConnState *conn_state; /* extra per-connection state */
	int			numParams;		/* number of parameters passed to query */
	FmgrInfo   *param_flinfo;	/* output conversion functions for them */
	List	   *param_exprs;	/* executable expressions for param values */
	const char **param_values;	/* textual values of query parameters */

	/* for storing result tuples
	 *
	 * 用于存储结果元组
	 */
	PGresult   *result;			/* result for query */
	int			num_tuples;		/* # of result tuples */
	int			next_tuple;		/* index of next one to return */
	MemoryContextCallback result_cb;	/* ensures result will get freed */
	Relation	resultRel;		/* relcache entry for the target relation */
	AttrNumber *attnoMap;		/* array of attnums of input user columns */
	AttrNumber	ctidAttno;		/* attnum of input ctid column */
	AttrNumber	oidAttno;		/* attnum of input oid column */
	bool		hasSystemCols;	/* are there system columns of resultRel? */

	/* working memory context
	 *
	 * 工作记忆情境
	 */
	MemoryContext temp_cxt;		/* context for per-tuple temporary data */
} PgFdwDirectModifyState;

/*
 * Workspace for analyzing a foreign table.
 *
 * 用于分析外部表的工作区。
 */
typedef struct PgFdwAnalyzeState
{
	Relation	rel;			/* relcache entry for the foreign table */
	AttInMetadata *attinmeta;	/* attribute datatype conversion metadata */
	List	   *retrieved_attrs;	/* attr numbers retrieved by query */

	/* collected sample rows
	 *
	 * 收集的样本行
	 */
	HeapTuple  *rows;			/* array of size targrows */
	int			targrows;		/* target # of sample rows */
	int			numrows;		/* # of sample rows collected */

	/* for random sampling
	 *
	 * 用于随机抽样
	 */
	double		samplerows;		/* # of rows fetched */
	double		rowstoskip;		/* # of rows to skip before next sample */
	ReservoirStateData rstate;	/* state for reservoir sampling */

	/* working memory contexts
	 *
	 * 工作记忆情境
	 */
	MemoryContext anl_cxt;		/* context for per-analyze lifespan data */
	MemoryContext temp_cxt;		/* context for per-tuple temporary data */
} PgFdwAnalyzeState;

/*
 * This enum describes what's kept in the fdw_private list for a ForeignPath.
 * We store:
 *
 * 该枚举描述了ForeignPath 的fdw_private 列表中保存的内容。我们存储：
 *
 * 1) Boolean flag showing if the remote query has the final sort
 * 2) Boolean flag showing if the remote query has the LIMIT clause
 *
 * 1) 显示远程查询是否具有最终排序的布尔标志 2) 显示远程查询是否具有 LIMIT 子句的布尔标志
 */
enum FdwPathPrivateIndex
{
	/* has-final-sort flag (as a Boolean node)
	 *
	 * has-final-sort 标志（作为布尔节点）
	 */
	FdwPathPrivateHasFinalSort,
	/* has-limit flag (as a Boolean node)
	 *
	 * has-limit 标志（作为布尔节点）
	 */
	FdwPathPrivateHasLimit,
};

/* Struct for extra information passed to estimate_path_cost_size()
 *
 * 传递给estimate_path_cost_size()的额外信息的结构
 */
typedef struct
{
	PathTarget *target;
	bool		has_final_sort;
	bool		has_limit;
	double		limit_tuples;
	int64		count_est;
	int64		offset_est;
} PgFdwPathExtraData;

/*
 * Identify the attribute where data conversion fails.
 *
 * 确定数据转换失败的属性。
 */
typedef struct ConversionLocation
{
	AttrNumber	cur_attno;		/* attribute number being processed, or 0 */
	Relation	rel;			/* foreign table being processed, or NULL */
	ForeignScanState *fsstate;	/* plan node being processed, or NULL */
} ConversionLocation;

/* Callback argument for ec_member_matches_foreign
 *
 * ec_member_matches_foreign 的回调参数
 */
typedef struct
{
	Expr	   *current;		/* current expr, or NULL if not yet found */
	List	   *already_used;	/* expressions already dealt with */
} ec_member_foreign_arg;

/*
 * SQL functions
 *
 * SQL函数
 */
PG_FUNCTION_INFO_V1(postgres_fdw_handler);

/*
 * FDW callback routines
 *
 * FDW 回调例程
 */
static void postgresGetForeignRelSize(PlannerInfo *root,
									  RelOptInfo *baserel,
									  Oid foreigntableid);
static void postgresGetForeignPaths(PlannerInfo *root,
									RelOptInfo *baserel,
									Oid foreigntableid);
static ForeignScan *postgresGetForeignPlan(PlannerInfo *root,
										   RelOptInfo *foreignrel,
										   Oid foreigntableid,
										   ForeignPath *best_path,
										   List *tlist,
										   List *scan_clauses,
										   Plan *outer_plan);
static void postgresBeginForeignScan(ForeignScanState *node, int eflags);
static TupleTableSlot *postgresIterateForeignScan(ForeignScanState *node);
static void postgresReScanForeignScan(ForeignScanState *node);
static void postgresEndForeignScan(ForeignScanState *node);
static void postgresAddForeignUpdateTargets(PlannerInfo *root,
											Index rtindex,
											RangeTblEntry *target_rte,
											Relation target_relation);
static List *postgresPlanForeignModify(PlannerInfo *root,
									   ModifyTable *plan,
									   Index resultRelation,
									   int subplan_index);
static void postgresBeginForeignModify(ModifyTableState *mtstate,
									   ResultRelInfo *resultRelInfo,
									   List *fdw_private,
									   int subplan_index,
									   int eflags);
static TupleTableSlot *postgresExecForeignInsert(EState *estate,
												 ResultRelInfo *resultRelInfo,
												 TupleTableSlot *slot,
												 TupleTableSlot *planSlot);
static TupleTableSlot **postgresExecForeignBatchInsert(EState *estate,
													   ResultRelInfo *resultRelInfo,
													   TupleTableSlot **slots,
													   TupleTableSlot **planSlots,
													   int *numSlots);
static int	postgresGetForeignModifyBatchSize(ResultRelInfo *resultRelInfo);
static TupleTableSlot *postgresExecForeignUpdate(EState *estate,
												 ResultRelInfo *resultRelInfo,
												 TupleTableSlot *slot,
												 TupleTableSlot *planSlot);
static TupleTableSlot *postgresExecForeignDelete(EState *estate,
												 ResultRelInfo *resultRelInfo,
												 TupleTableSlot *slot,
												 TupleTableSlot *planSlot);
static void postgresEndForeignModify(EState *estate,
									 ResultRelInfo *resultRelInfo);
static void postgresBeginForeignInsert(ModifyTableState *mtstate,
									   ResultRelInfo *resultRelInfo);
static void postgresEndForeignInsert(EState *estate,
									 ResultRelInfo *resultRelInfo);
static int	postgresIsForeignRelUpdatable(Relation rel);
static bool postgresPlanDirectModify(PlannerInfo *root,
									 ModifyTable *plan,
									 Index resultRelation,
									 int subplan_index);
static void postgresBeginDirectModify(ForeignScanState *node, int eflags);
static TupleTableSlot *postgresIterateDirectModify(ForeignScanState *node);
static void postgresEndDirectModify(ForeignScanState *node);
static void postgresExplainForeignScan(ForeignScanState *node,
									   ExplainState *es);
static void postgresExplainForeignModify(ModifyTableState *mtstate,
										 ResultRelInfo *rinfo,
										 List *fdw_private,
										 int subplan_index,
										 ExplainState *es);
static void postgresExplainDirectModify(ForeignScanState *node,
										ExplainState *es);
static void postgresExecForeignTruncate(List *rels,
										DropBehavior behavior,
										bool restart_seqs);
static bool postgresAnalyzeForeignTable(Relation relation,
										AcquireSampleRowsFunc *func,
										BlockNumber *totalpages);
static List *postgresImportForeignSchema(ImportForeignSchemaStmt *stmt,
										 Oid serverOid);
static void postgresGetForeignJoinPaths(PlannerInfo *root,
										RelOptInfo *joinrel,
										RelOptInfo *outerrel,
										RelOptInfo *innerrel,
										JoinType jointype,
										JoinPathExtraData *extra);
static bool postgresRecheckForeignScan(ForeignScanState *node,
									   TupleTableSlot *slot);
static void postgresGetForeignUpperPaths(PlannerInfo *root,
										 UpperRelationKind stage,
										 RelOptInfo *input_rel,
										 RelOptInfo *output_rel,
										 void *extra);
static bool postgresIsForeignPathAsyncCapable(ForeignPath *path);
static void postgresForeignAsyncRequest(AsyncRequest *areq);
static void postgresForeignAsyncConfigureWait(AsyncRequest *areq);
static void postgresForeignAsyncNotify(AsyncRequest *areq);

/*
 * Helper functions
 *
 * 辅助函数
 */
static void estimate_path_cost_size(PlannerInfo *root,
									RelOptInfo *foreignrel,
									List *param_join_conds,
									List *pathkeys,
									PgFdwPathExtraData *fpextra,
									double *p_rows, int *p_width,
									int *p_disabled_nodes,
									Cost *p_startup_cost, Cost *p_total_cost);
static void get_remote_estimate(const char *sql,
								PGconn *conn,
								double *rows,
								int *width,
								Cost *startup_cost,
								Cost *total_cost);
static void adjust_foreign_grouping_path_cost(PlannerInfo *root,
											  List *pathkeys,
											  double retrieved_rows,
											  double width,
											  double limit_tuples,
											  int *p_disabled_nodes,
											  Cost *p_startup_cost,
											  Cost *p_run_cost);
static bool ec_member_matches_foreign(PlannerInfo *root, RelOptInfo *rel,
									  EquivalenceClass *ec, EquivalenceMember *em,
									  void *arg);
static void create_cursor(ForeignScanState *node);
static void fetch_more_data(ForeignScanState *node);
static void close_cursor(PGconn *conn, unsigned int cursor_number,
						 PgFdwConnState *conn_state);
static PgFdwModifyState *create_foreign_modify(EState *estate,
											   RangeTblEntry *rte,
											   ResultRelInfo *resultRelInfo,
											   CmdType operation,
											   Plan *subplan,
											   char *query,
											   List *target_attrs,
											   int values_end,
											   bool has_returning,
											   List *retrieved_attrs);
static TupleTableSlot **execute_foreign_modify(EState *estate,
											   ResultRelInfo *resultRelInfo,
											   CmdType operation,
											   TupleTableSlot **slots,
											   TupleTableSlot **planSlots,
											   int *numSlots);
static void prepare_foreign_modify(PgFdwModifyState *fmstate);
static const char **convert_prep_stmt_params(PgFdwModifyState *fmstate,
											 ItemPointer tupleid,
											 TupleTableSlot **slots,
											 int numSlots);
static void store_returning_result(PgFdwModifyState *fmstate,
								   TupleTableSlot *slot, PGresult *res);
static void finish_foreign_modify(PgFdwModifyState *fmstate);
static void deallocate_query(PgFdwModifyState *fmstate);
static List *build_remote_returning(Index rtindex, Relation rel,
									List *returningList);
static void rebuild_fdw_scan_tlist(ForeignScan *fscan, List *tlist);
static void execute_dml_stmt(ForeignScanState *node);
static TupleTableSlot *get_returning_data(ForeignScanState *node);
static void init_returning_filter(PgFdwDirectModifyState *dmstate,
								  List *fdw_scan_tlist,
								  Index rtindex);
static TupleTableSlot *apply_returning_filter(PgFdwDirectModifyState *dmstate,
											  ResultRelInfo *resultRelInfo,
											  TupleTableSlot *slot,
											  EState *estate);
static void prepare_query_params(PlanState *node,
								 List *fdw_exprs,
								 int numParams,
								 FmgrInfo **param_flinfo,
								 List **param_exprs,
								 const char ***param_values);
static void process_query_params(ExprContext *econtext,
								 FmgrInfo *param_flinfo,
								 List *param_exprs,
								 const char **param_values);
static int	postgresAcquireSampleRowsFunc(Relation relation, int elevel,
										  HeapTuple *rows, int targrows,
										  double *totalrows,
										  double *totaldeadrows);
static void analyze_row_processor(PGresult *res, int row,
								  PgFdwAnalyzeState *astate);
static void produce_tuple_asynchronously(AsyncRequest *areq, bool fetch);
static void fetch_more_data_begin(AsyncRequest *areq);
static void complete_pending_request(AsyncRequest *areq);
static HeapTuple make_tuple_from_result_row(PGresult *res,
											int row,
											Relation rel,
											AttInMetadata *attinmeta,
											List *retrieved_attrs,
											ForeignScanState *fsstate,
											MemoryContext temp_context);
static void conversion_error_callback(void *arg);
static bool foreign_join_ok(PlannerInfo *root, RelOptInfo *joinrel,
							JoinType jointype, RelOptInfo *outerrel, RelOptInfo *innerrel,
							JoinPathExtraData *extra);
static bool foreign_grouping_ok(PlannerInfo *root, RelOptInfo *grouped_rel,
								Node *havingQual);
static List *get_useful_pathkeys_for_relation(PlannerInfo *root,
											  RelOptInfo *rel);
static List *get_useful_ecs_for_relation(PlannerInfo *root, RelOptInfo *rel);
static void add_paths_with_pathkeys_for_rel(PlannerInfo *root, RelOptInfo *rel,
											Path *epq_path, List *restrictlist);
static void add_foreign_grouping_paths(PlannerInfo *root,
									   RelOptInfo *input_rel,
									   RelOptInfo *grouped_rel,
									   GroupPathExtraData *extra);
static void add_foreign_ordered_paths(PlannerInfo *root,
									  RelOptInfo *input_rel,
									  RelOptInfo *ordered_rel);
static void add_foreign_final_paths(PlannerInfo *root,
									RelOptInfo *input_rel,
									RelOptInfo *final_rel,
									FinalPathExtraData *extra);
static void apply_server_options(PgFdwRelationInfo *fpinfo);
static void apply_table_options(PgFdwRelationInfo *fpinfo);
static void merge_fdw_options(PgFdwRelationInfo *fpinfo,
							  const PgFdwRelationInfo *fpinfo_o,
							  const PgFdwRelationInfo *fpinfo_i);
static int	get_batch_size_option(Relation rel);


/*
 * Foreign-data wrapper handler function: return a struct with pointers
 * to my callback routines.
 *
 * 外部数据包装处理程序函数：返回一个结构体，其中包含指向我的回调例程的指针。
 */
Datum
postgres_fdw_handler(PG_FUNCTION_ARGS)
{
	FdwRoutine *routine = makeNode(FdwRoutine);

	/* Functions for scanning foreign tables
	 *
	 * 扫描外部表的函数
	 */
	routine->GetForeignRelSize = postgresGetForeignRelSize;
	routine->GetForeignPaths = postgresGetForeignPaths;
	routine->GetForeignPlan = postgresGetForeignPlan;
	routine->BeginForeignScan = postgresBeginForeignScan;
	routine->IterateForeignScan = postgresIterateForeignScan;
	routine->ReScanForeignScan = postgresReScanForeignScan;
	routine->EndForeignScan = postgresEndForeignScan;

	/* Functions for updating foreign tables
	 *
	 * 更新外部表的函数
	 */
	routine->AddForeignUpdateTargets = postgresAddForeignUpdateTargets;
	routine->PlanForeignModify = postgresPlanForeignModify;
	routine->BeginForeignModify = postgresBeginForeignModify;
	routine->ExecForeignInsert = postgresExecForeignInsert;
	routine->ExecForeignBatchInsert = postgresExecForeignBatchInsert;
	routine->GetForeignModifyBatchSize = postgresGetForeignModifyBatchSize;
	routine->ExecForeignUpdate = postgresExecForeignUpdate;
	routine->ExecForeignDelete = postgresExecForeignDelete;
	routine->EndForeignModify = postgresEndForeignModify;
	routine->BeginForeignInsert = postgresBeginForeignInsert;
	routine->EndForeignInsert = postgresEndForeignInsert;
	routine->IsForeignRelUpdatable = postgresIsForeignRelUpdatable;
	routine->PlanDirectModify = postgresPlanDirectModify;
	routine->BeginDirectModify = postgresBeginDirectModify;
	routine->IterateDirectModify = postgresIterateDirectModify;
	routine->EndDirectModify = postgresEndDirectModify;

	/* Function for EvalPlanQual rechecks
	 *
	 * EvalPlanQual 复查功能
	 */
	routine->RecheckForeignScan = postgresRecheckForeignScan;
	/* Support functions for EXPLAIN
	 *
	 * EXPLAIN的支持功能
	 */
	routine->ExplainForeignScan = postgresExplainForeignScan;
	routine->ExplainForeignModify = postgresExplainForeignModify;
	routine->ExplainDirectModify = postgresExplainDirectModify;

	/* Support function for TRUNCATE
	 *
	 * TRUNCATE 支持功能
	 */
	routine->ExecForeignTruncate = postgresExecForeignTruncate;

	/* Support functions for ANALYZE
	 *
	 * ANALYZE 支持功能
	 */
	routine->AnalyzeForeignTable = postgresAnalyzeForeignTable;

	/* Support functions for IMPORT FOREIGN SCHEMA
	 *
	 * IMPORT FOREIGN SCHEMA 的支持功能
	 */
	routine->ImportForeignSchema = postgresImportForeignSchema;

	/* Support functions for join push-down
	 *
	 * 支持连接下推功能
	 */
	routine->GetForeignJoinPaths = postgresGetForeignJoinPaths;

	/* Support functions for upper relation push-down
	 *
	 * 支持上层关系下推功能
	 */
	routine->GetForeignUpperPaths = postgresGetForeignUpperPaths;

	/* Support functions for asynchronous execution
	 *
	 * 支持异步执行的函数
	 */
	routine->IsForeignPathAsyncCapable = postgresIsForeignPathAsyncCapable;
	routine->ForeignAsyncRequest = postgresForeignAsyncRequest;
	routine->ForeignAsyncConfigureWait = postgresForeignAsyncConfigureWait;
	routine->ForeignAsyncNotify = postgresForeignAsyncNotify;

	PG_RETURN_POINTER(routine);
}

/*
 * postgresGetForeignRelSize
 *		Estimate # of rows and width of the result of the scan
 *
 * postgresGetForeignRelSize 估计扫描结果的行数和宽度
 *
 * We should consider the effect of all baserestrictinfo clauses here, but
 * not any join clauses.
 *
 * 我们应该在这里考虑所有 baserestrictinfo 子句的效果，但不考虑任何 join 子句。
 */
static void
postgresGetForeignRelSize(PlannerInfo *root,
						  RelOptInfo *baserel,
						  Oid foreigntableid)
{
	PgFdwRelationInfo *fpinfo;
	ListCell   *lc;

	/*
	 * We use PgFdwRelationInfo to pass various information to subsequent
	 * functions.
	 *
	 * 我们使用PgFdwRelationInfo将各种信息传递给后续函数。
	 */
	fpinfo = (PgFdwRelationInfo *) palloc0(sizeof(PgFdwRelationInfo));
	baserel->fdw_private = fpinfo;

	/* Base foreign tables need to be pushed down always.
	 *
	 * 基础外部表总是需要被下推。
	 */
	fpinfo->pushdown_safe = true;

	/* Look up foreign-table catalog info.
	 *
	 * 查找外部表目录信息。
	 */
	fpinfo->table = GetForeignTable(foreigntableid);
	fpinfo->server = GetForeignServer(fpinfo->table->serverid);

	/*
	 * Extract user-settable option values.  Note that per-table settings of
	 * use_remote_estimate, fetch_size and async_capable override per-server
	 * settings of them, respectively.
	 *
	 * 提取用户可设置的选项值。  请注意，每个表的 use_remote_estimate、fetch_size 和 async_capable 设置分别覆盖它们的每个服务器设置。
	 */
	fpinfo->use_remote_estimate = false;
	fpinfo->fdw_startup_cost = DEFAULT_FDW_STARTUP_COST;
	fpinfo->fdw_tuple_cost = DEFAULT_FDW_TUPLE_COST;
	fpinfo->shippable_extensions = NIL;
	fpinfo->fetch_size = 100;
	fpinfo->async_capable = false;

	apply_server_options(fpinfo);
	apply_table_options(fpinfo);

	/*
	 * If the table or the server is configured to use remote estimates,
	 * identify which user to do remote access as during planning.  This
	 * should match what ExecCheckPermissions() does.  If we fail due to lack
	 * of permissions, the query would have failed at runtime anyway.
	 *
	 * 如果表或服务器配置为使用远程估计，请在规划期间确定执行远程访问的用户。  这应该与 ExecCheckPermissions() 的作用相匹配。  如果由于缺乏权限而失败，查询无论如何都会在运行时失败。
	 */
	if (fpinfo->use_remote_estimate)
	{
		Oid			userid;

		userid = OidIsValid(baserel->userid) ? baserel->userid : GetUserId();
		fpinfo->user = GetUserMapping(userid, fpinfo->server->serverid);
	}
	else
		fpinfo->user = NULL;

	/*
	 * Identify which baserestrictinfo clauses can be sent to the remote
	 * server and which can't.
	 *
	 * 确定哪些 baserestrictinfo 子句可以发送到远程服务器，哪些不能。
	 */
	classifyConditions(root, baserel, baserel->baserestrictinfo,
					   &fpinfo->remote_conds, &fpinfo->local_conds);

	/*
	 * Identify which attributes will need to be retrieved from the remote
	 * server.  These include all attrs needed for joins or final output, plus
	 * all attrs used in the local_conds.  (Note: if we end up using a
	 * parameterized scan, it's possible that some of the join clauses will be
	 * sent to the remote and thus we wouldn't really need to retrieve the
	 * columns used in them.  Doesn't seem worth detecting that case though.)
	 *
	 * 确定需要从远程服务器检索哪些属性。  其中包括连接或最终输出所需的所有属性，以及 local_conds 中使用的所有属性。  （注意：如果我们最终使用参数化扫描，一些连接子句可能会被发送到远程，因此我们实际上不需要检索其中使用的列。不过，似乎不值得检测这种情况。）
	 */
	fpinfo->attrs_used = NULL;
	pull_varattnos((Node *) baserel->reltarget->exprs, baserel->relid,
				   &fpinfo->attrs_used);
	foreach(lc, fpinfo->local_conds)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

		pull_varattnos((Node *) rinfo->clause, baserel->relid,
					   &fpinfo->attrs_used);
	}

	/*
	 * Compute the selectivity and cost of the local_conds, so we don't have
	 * to do it over again for each path.  The best we can do for these
	 * conditions is to estimate selectivity on the basis of local statistics.
	 *
	 * 计算 local_conds 的选择性和成本，这样我们就不必为每条路径重新计算。  针对这些情况，我们能做的最好的事情就是根据当地统计数据来估计选择性。
	 */
	fpinfo->local_conds_sel = clauselist_selectivity(root,
													 fpinfo->local_conds,
													 baserel->relid,
													 JOIN_INNER,
													 NULL);

	cost_qual_eval(&fpinfo->local_conds_cost, fpinfo->local_conds, root);

	/*
	 * Set # of retrieved rows and cached relation costs to some negative
	 * value, so that we can detect when they are set to some sensible values,
	 * during one (usually the first) of the calls to estimate_path_cost_size.
	 *
	 * 将检索行数和缓存关系成本设置为某个负值，以便我们可以在一次（通常是第一次）调用estimate_path_cost_size期间检测它们何时设置为某些合理值。
	 */
	fpinfo->retrieved_rows = -1;
	fpinfo->rel_startup_cost = -1;
	fpinfo->rel_total_cost = -1;

	/*
	 * If the table or the server is configured to use remote estimates,
	 * connect to the foreign server and execute EXPLAIN to estimate the
	 * number of rows selected by the restriction clauses, as well as the
	 * average row width.  Otherwise, estimate using whatever statistics we
	 * have locally, in a way similar to ordinary tables.
	 *
	 * 如果表或服务器配置为使用远程估计，请连接到外部服务器并执行 EXPLAIN 来估计限制子句选择的行数以及平均行宽度。  否则，以类似于普通表格的方式使用我们本地拥有的任何统计数据进行估计。
	 */
	if (fpinfo->use_remote_estimate)
	{
		/*
		 * Get cost/size estimates with help of remote server.  Save the
		 * values in fpinfo so we don't need to do it again to generate the
		 * basic foreign path.
		 *
		 * 在远程服务器的帮助下获得成本/大小估算。  将值保存在 fpinfo 中，这样我们就不需要再次生成基本外部路径。
		 */
		estimate_path_cost_size(root, baserel, NIL, NIL, NULL,
								&fpinfo->rows, &fpinfo->width,
								&fpinfo->disabled_nodes,
								&fpinfo->startup_cost, &fpinfo->total_cost);

		/* Report estimated baserel size to planner.
		 *
		 * 向规划者报告估计的 Baserel 大小。
		 */
		baserel->rows = fpinfo->rows;
		baserel->reltarget->width = fpinfo->width;
	}
	else
	{
		/*
		 * If the foreign table has never been ANALYZEd, it will have
		 * reltuples < 0, meaning "unknown".  We can't do much if we're not
		 * allowed to consult the remote server, but we can use a hack similar
		 * to plancat.c's treatment of empty relations: use a minimum size
		 * estimate of 10 pages, and divide by the column-datatype-based width
		 * estimate to get the corresponding number of tuples.
		 *
		 * 如果外部表从未被分析过，则其关联值将< 0，即“未知”。  如果不允许咨询远程服务器，我们就无能为力，但我们可以使用类似于 plancat.c 处理空关系的 hack：使用 10 页的最小大小估计，然后除以基于列数据类型的宽度估计以获得相应的元组数量。
		 */
		if (baserel->tuples < 0)
		{
			baserel->pages = 10;
			baserel->tuples =
				(10 * BLCKSZ) / (baserel->reltarget->width +
								 MAXALIGN(SizeofHeapTupleHeader));
		}

		/* Estimate baserel size as best we can with local statistics.
		 *
		 * 根据本地统计数据尽可能估计 Baserel 大小。
		 */
		set_baserel_size_estimates(root, baserel);

		/* Fill in basically-bogus cost estimates for use later.
		 *
		 * 填写基本上是虚假的成本估算以供以后使用。
		 */
		estimate_path_cost_size(root, baserel, NIL, NIL, NULL,
								&fpinfo->rows, &fpinfo->width,
								&fpinfo->disabled_nodes,
								&fpinfo->startup_cost, &fpinfo->total_cost);
	}

	/*
	 * fpinfo->relation_name gets the numeric rangetable index of the foreign
	 * table RTE.  (If this query gets EXPLAIN'd, we'll convert that to a
	 * human-readable string at that time.)
	 *
	 * fpinfo->relation_name 获取外部表 RTE 的数字范围表索引。  （如果这个查询得到解释，我们将在那时将其转换为人类可读的字符串。）
	 */
	fpinfo->relation_name = psprintf("%u", baserel->relid);

	/* No outer and inner relations.
	 *
	 * 没有外在和内在的联系。
	 */
	fpinfo->make_outerrel_subquery = false;
	fpinfo->make_innerrel_subquery = false;
	fpinfo->lower_subquery_rels = NULL;
	fpinfo->hidden_subquery_rels = NULL;
	/* Set the relation index.
	 *
	 * 设置关系索引。
	 */
	fpinfo->relation_index = baserel->relid;
}

/*
 * get_useful_ecs_for_relation
 *		Determine which EquivalenceClasses might be involved in useful
 *		orderings of this relation.
 *
 * get_useful_ecs_for_relation 确定哪些 EquivalenceClass 可能参与此关系的有用排序。
 *
 * This function is in some respects a mirror image of the core function
 * pathkeys_useful_for_merging: for a regular table, we know what indexes
 * we have and want to test whether any of them are useful.  For a foreign
 * table, we don't know what indexes are present on the remote side but
 * want to speculate about which ones we'd like to use if they existed.
 *
 * 这个函数在某些方面是核心函数pathkeys_useful_for_merging的镜像：对于常规表，我们知道我们有哪些索引，并且想要测试它们是否有用。  对于外部表，我们不知道远程端存在哪些索引，但想要推测我们想要使用哪些索引（如果存在）。
 *
 * This function returns a list of potentially-useful equivalence classes,
 * but it does not guarantee that an EquivalenceMember exists which contains
 * Vars only from the given relation.  For example, given ft1 JOIN t1 ON
 * ft1.x + t1.x = 0, this function will say that the equivalence class
 * containing ft1.x + t1.x is potentially useful.  Supposing ft1 is remote and
 * t1 is local (or on a different server), it will turn out that no useful
 * ORDER BY clause can be generated.  It's not our job to figure that out
 * here; we're only interested in identifying relevant ECs.
 *
 * 此函数返回潜在有用的等价类的列表，但它不保证存在仅包含给定关系中的 Var 的 EquivalenceMember。  例如，给定 ft1 JOIN t1 ON ft1.x + t1.x = 0，此函数将表示包含 ft1.x + t1.x 的等价类可能有用。  假设 ft1 是远程的，而 t1 是本地的（或在不同的服务器上），则结果将无法生成有用的 ORDER BY 子句。  在这里解决这个问题不是我们的工作；我们的工作就是解决这个问题。我们只对识别相关的 EC 感兴趣。
 */
static List *
get_useful_ecs_for_relation(PlannerInfo *root, RelOptInfo *rel)
{
	List	   *useful_eclass_list = NIL;
	ListCell   *lc;
	Relids		relids;

	/*
	 * First, consider whether any active EC is potentially useful for a merge
	 * join against this relation.
	 *
	 * 首先，考虑任何活动的 EC 是否对于针对此关系的合并联接可能有用。
	 */
	if (rel->has_eclass_joins)
	{
		foreach(lc, root->eq_classes)
		{
			EquivalenceClass *cur_ec = (EquivalenceClass *) lfirst(lc);

			if (eclass_useful_for_merging(root, cur_ec, rel))
				useful_eclass_list = lappend(useful_eclass_list, cur_ec);
		}
	}

	/*
	 * Next, consider whether there are any non-EC derivable join clauses that
	 * are merge-joinable.  If the joininfo list is empty, we can exit
	 * quickly.
	 *
	 * 接下来，考虑是否存在任何可合并连接的非 EC 可派生连接子句。  如果joininfo列表为空，我们可以快速退出。
	 */
	if (rel->joininfo == NIL)
		return useful_eclass_list;

	/* If this is a child rel, we must use the topmost parent rel to search.
	 *
	 * 如果这是一个子rel，我们必须使用最顶层的父rel来搜索。
	 */
	if (IS_OTHER_REL(rel))
	{
		Assert(!bms_is_empty(rel->top_parent_relids));
		relids = rel->top_parent_relids;
	}
	else
		relids = rel->relids;

	/* Check each join clause in turn.
	 *
	 * 依次检查每个连接子句。
	 */
	foreach(lc, rel->joininfo)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(lc);

		/* Consider only mergejoinable clauses
		 *
		 * 仅考虑可合并连接的子句
		 */
		if (restrictinfo->mergeopfamilies == NIL)
			continue;

		/* Make sure we've got canonical ECs.
		 *
		 * 确保我们有规范的 EC。
		 */
		update_mergeclause_eclasses(root, restrictinfo);

		/*
		 * restrictinfo->mergeopfamilies != NIL is sufficient to guarantee
		 * that left_ec and right_ec will be initialized, per comments in
		 * distribute_qual_to_rels.
		 *
		 * 根据 Distribution_qual_to_rels 中的注释，restrictinfo->mergeopfamilies != NIL 足以保证 left_ec 和 right_ec 将被初始化。
		 *
		 * We want to identify which side of this merge-joinable clause
		 * contains columns from the relation produced by this RelOptInfo. We
		 * test for overlap, not containment, because there could be extra
		 * relations on either side.  For example, suppose we've got something
		 * like ((A JOIN B ON A.x = B.x) JOIN C ON A.y = C.y) LEFT JOIN D ON
		 * A.y = D.y.  The input rel might be the joinrel between A and B, and
		 * we'll consider the join clause A.y = D.y. relids contains a
		 * relation not involved in the join class (B) and the equivalence
		 * class for the left-hand side of the clause contains a relation not
		 * involved in the input rel (C).  Despite the fact that we have only
		 * overlap and not containment in either direction, A.y is potentially
		 * useful as a sort column.
		 *
		 * 我们想要确定此合并连接子句的哪一侧包含此 RelOptInfo 生成的关系中的列。我们测试重叠，而不是包含，因为任何一方都可能存在额外的关系。  例如，假设我们有类似 ((A JOIN B ON A.x = B.x) JOIN C ON A.y = C.y) LEFT JOIN D ON A.y = D.y 的内容。  输入rel可能是A和B之间的连接关系，我们将考虑连接子句A.y = D.y。 relids 包含连接类 (B) 中未涉及的关系，子句左侧的等价类包含输入 rel (C) 中未涉及的关系。  尽管事实上我们在任一方向上都只有重叠而没有包含，但 A.y 作为排序列可能很有用。
		 *
		 * Note that it's even possible that relids overlaps neither side of
		 * the join clause.  For example, consider A LEFT JOIN B ON A.x = B.x
		 * AND A.x = 1.  The clause A.x = 1 will appear in B's joininfo list,
		 * but overlaps neither side of B.  In that case, we just skip this
		 * join clause, since it doesn't suggest a useful sort order for this
		 * relation.
		 *
		 * 请注意，relids 甚至有可能不与 join 子句的任何一侧重叠。  例如，考虑 A LEFT JOIN B ON A.x = B.x AND A.x = 1。子句 A.x = 1 将出现在 B 的 joininfo 列表中，但不与 B 的任何一侧重叠。在这种情况下，我们只需跳过此 join 子句，因为它没有建议此关系的有用排序顺序。
		 */
		if (bms_overlap(relids, restrictinfo->right_ec->ec_relids))
			useful_eclass_list = list_append_unique_ptr(useful_eclass_list,
														restrictinfo->right_ec);
		else if (bms_overlap(relids, restrictinfo->left_ec->ec_relids))
			useful_eclass_list = list_append_unique_ptr(useful_eclass_list,
														restrictinfo->left_ec);
	}

	return useful_eclass_list;
}

/*
 * get_useful_pathkeys_for_relation
 *		Determine which orderings of a relation might be useful.
 *
 * get_useful_pathkeys_for_relation 确定关系的哪些排序可能有用。
 *
 * Getting data in sorted order can be useful either because the requested
 * order matches the final output ordering for the overall query we're
 * planning, or because it enables an efficient merge join.  Here, we try
 * to figure out which pathkeys to consider.
 *
 * 按排序顺序获取数据可能很有用，因为请求的顺序与我们计划的整个查询的最终输出顺序相匹配，或者因为它可以实现高效的合并联接。  在这里，我们尝试找出要考虑的路径键。
 */
static List *
get_useful_pathkeys_for_relation(PlannerInfo *root, RelOptInfo *rel)
{
	List	   *useful_pathkeys_list = NIL;
	List	   *useful_eclass_list;
	PgFdwRelationInfo *fpinfo = (PgFdwRelationInfo *) rel->fdw_private;
	EquivalenceClass *query_ec = NULL;
	ListCell   *lc;

	/*
	 * Pushing the query_pathkeys to the remote server is always worth
	 * considering, because it might let us avoid a local sort.
	 *
	 * 将 query_pathkeys 推送到远程服务器始终值得考虑，因为它可能让我们避免本地排序。
	 */
	fpinfo->qp_is_pushdown_safe = false;
	if (root->query_pathkeys)
	{
		bool		query_pathkeys_ok = true;

		foreach(lc, root->query_pathkeys)
		{
			PathKey    *pathkey = (PathKey *) lfirst(lc);

			/*
			 * The planner and executor don't have any clever strategy for
			 * taking data sorted by a prefix of the query's pathkeys and
			 * getting it to be sorted by all of those pathkeys. We'll just
			 * end up resorting the entire data set.  So, unless we can push
			 * down all of the query pathkeys, forget it.
			 *
			 * 规划器和执行器没有任何聪明的策略来获取按查询路径键前缀排序的数据并使其按所有这些路径键排序。我们最终将利用整个数据集。  因此，除非我们可以按下所有查询路径键，否则就忘记它。
			 */
			if (!is_foreign_pathkey(root, rel, pathkey))
			{
				query_pathkeys_ok = false;
				break;
			}
		}

		if (query_pathkeys_ok)
		{
			useful_pathkeys_list = list_make1(list_copy(root->query_pathkeys));
			fpinfo->qp_is_pushdown_safe = true;
		}
	}

	/*
	 * Even if we're not using remote estimates, having the remote side do the
	 * sort generally won't be any worse than doing it locally, and it might
	 * be much better if the remote side can generate data in the right order
	 * without needing a sort at all.  However, what we're going to do next is
	 * try to generate pathkeys that seem promising for possible merge joins,
	 * and that's more speculative.  A wrong choice might hurt quite a bit, so
	 * bail out if we can't use remote estimates.
	 *
	 * 即使我们不使用远程估计，让远程端进行排序通常不会比在本地进行排序更糟糕，而且如果远程端能够以正确的顺序生成数据而不需要排序，情况可能会好得多。  然而，我们接下来要做的是尝试生成似乎有希望实现可能的合并连接的路径键，这更具推测性。  错误的选择可能会造成很大的伤害，所以如果我们不能使用远程估计就应该放弃。
	 */
	if (!fpinfo->use_remote_estimate)
		return useful_pathkeys_list;

	/* Get the list of interesting EquivalenceClasses.
	 *
	 * 获取有趣的 EquivalenceClasses 列表。
	 */
	useful_eclass_list = get_useful_ecs_for_relation(root, rel);

	/* Extract unique EC for query, if any, so we don't consider it again.
	 *
	 * 提取唯一的 EC 进行查询（如果有），因此我们不再考虑它。
	 */
	if (list_length(root->query_pathkeys) == 1)
	{
		PathKey    *query_pathkey = linitial(root->query_pathkeys);

		query_ec = query_pathkey->pk_eclass;
	}

	/*
	 * As a heuristic, the only pathkeys we consider here are those of length
	 * one.  It's surely possible to consider more, but since each one we
	 * choose to consider will generate a round-trip to the remote side, we
	 * need to be a bit cautious here.  It would sure be nice to have a local
	 * cache of information about remote index definitions...
	 *
	 * 作为一种启发式方法，我们在这里考虑的唯一路径键是长度为 1 的路径键。  当然可以考虑更多，但由于我们选择考虑的每个都会生成到远程端的往返，因此我们在这里需要谨慎一点。  拥有有关远程索引定义的信息的本地缓存肯定会很好......
	 */
	foreach(lc, useful_eclass_list)
	{
		EquivalenceClass *cur_ec = lfirst(lc);
		PathKey    *pathkey;

		/* If redundant with what we did above, skip it.
		 *
		 * 如果与我们上面所做的多余，请跳过它。
		 */
		if (cur_ec == query_ec)
			continue;

		/* Can't push down the sort if the EC's opfamily is not shippable.
		 *
		 * 如果 EC 的 opfamily 不可发货，则无法推低排序。
		 */
		if (!is_shippable(linitial_oid(cur_ec->ec_opfamilies),
						  OperatorFamilyRelationId, fpinfo))
			continue;

		/* If no pushable expression for this rel, skip it.
		 *
		 * 如果此 rel 没有可推送的表达式，则跳过它。
		 */
		if (find_em_for_rel(root, cur_ec, rel) == NULL)
			continue;

		/* Looks like we can generate a pathkey, so let's do it.
		 *
		 * 看起来我们可以生成一个路径密钥，所以让我们这样做吧。
		 */
		pathkey = make_canonical_pathkey(root, cur_ec,
										 linitial_oid(cur_ec->ec_opfamilies),
										 COMPARE_LT,
										 false);
		useful_pathkeys_list = lappend(useful_pathkeys_list,
									   list_make1(pathkey));
	}

	return useful_pathkeys_list;
}

/*
 * postgresGetForeignPaths
 *		Create possible scan paths for a scan on the foreign table
 *
 * postgresGetForeignPaths 为外表上的扫描创建可能的扫描路径
 */
static void
postgresGetForeignPaths(PlannerInfo *root,
						RelOptInfo *baserel,
						Oid foreigntableid)
{
	PgFdwRelationInfo *fpinfo = (PgFdwRelationInfo *) baserel->fdw_private;
	ForeignPath *path;
	List	   *ppi_list;
	ListCell   *lc;

	/*
	 * Create simplest ForeignScan path node and add it to baserel.  This path
	 * corresponds to SeqScan path of regular tables (though depending on what
	 * baserestrict conditions we were able to send to remote, there might
	 * actually be an indexscan happening there).  We already did all the work
	 * to estimate cost and size of this path.
	 *
	 * 创建最简单的ForeignScan路径节点并将其添加到baserel。  该路径对应于常规表的 SeqScan 路径（尽管根据我们能够发送到远程的基本限制条件，实际上可能会发生索引扫描）。  我们已经完成了估算这条路径的成本和规模的所有工作。
	 *
	 * Although this path uses no join clauses, it could still have required
	 * parameterization due to LATERAL refs in its tlist.
	 *
	 * 尽管此路径不使用连接子句，但由于其 tlist 中的 LATERAL 引用，它仍然可能需要参数化。
	 */
	path = create_foreignscan_path(root, baserel,
								   NULL,	/* default pathtarget */
								   fpinfo->rows,
								   fpinfo->disabled_nodes,
								   fpinfo->startup_cost,
								   fpinfo->total_cost,
								   NIL, /* no pathkeys */
								   baserel->lateral_relids,
								   NULL,	/* no extra plan */
								   NIL, /* no fdw_restrictinfo list */
								   NIL);	/* no fdw_private list */
	add_path(baserel, (Path *) path);

	/* Add paths with pathkeys
	 *
	 * 使用路径键添加路径
	 */
	add_paths_with_pathkeys_for_rel(root, baserel, NULL, NIL);

	/*
	 * If we're not using remote estimates, stop here.  We have no way to
	 * estimate whether any join clauses would be worth sending across, so
	 * don't bother building parameterized paths.
	 *
	 * 如果我们不使用远程估计，请到此为止。  我们无法估计任何连接子句是否值得发送，因此不必费心构建参数化路径。
	 */
	if (!fpinfo->use_remote_estimate)
		return;

	/*
	 * Thumb through all join clauses for the rel to identify which outer
	 * relations could supply one or more safe-to-send-to-remote join clauses.
	 * We'll build a parameterized path for each such outer relation.
	 *
	 * 浏览 rel 的所有联接子句，以确定哪些外部关系可以提供一个或多个安全发送到远程的联接子句。我们将为每个这样的外部关系构建一个参数化路径。
	 *
	 * It's convenient to manage this by representing each candidate outer
	 * relation by the ParamPathInfo node for it.  We can then use the
	 * ppi_clauses list in the ParamPathInfo node directly as a list of the
	 * interesting join clauses for that rel.  This takes care of the
	 * possibility that there are multiple safe join clauses for such a rel,
	 * and also ensures that we account for unsafe join clauses that we'll
	 * still have to enforce locally (since the parameterized-path machinery
	 * insists that we handle all movable clauses).
	 *
	 * 通过用 ParamPathInfo 节点表示每个候选外部关系，可以方便地进行管理。  然后，我们可以直接使用 ParamPathInfo 节点中的 ppi_clauses 列表作为该 rel 的有趣连接子句列表。  这考虑到了这样的 rel 存在多个安全连接子句的可能性，并且还确保我们考虑到我们仍然必须在本地强制执行的不安全连接子句（因为参数化路径机制坚持要求我们处理所有可移动子句）。
	 */
	ppi_list = NIL;
	foreach(lc, baserel->joininfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
		Relids		required_outer;
		ParamPathInfo *param_info;

		/* Check if clause can be moved to this rel
		 *
		 * 检查子句是否可以移动到此rel
		 */
		if (!join_clause_is_movable_to(rinfo, baserel))
			continue;

		/* See if it is safe to send to remote
		 *
		 * 查看发送到远程是否安全
		 */
		if (!is_foreign_expr(root, baserel, rinfo->clause))
			continue;

		/* Calculate required outer rels for the resulting path
		 *
		 * 计算结果路径所需的外部关系
		 */
		required_outer = bms_union(rinfo->clause_relids,
								   baserel->lateral_relids);
		/* We do not want the foreign rel itself listed in required_outer
		 *
		 * 我们不希望外部关系本身列在 required_outer 中
		 */
		required_outer = bms_del_member(required_outer, baserel->relid);

		/*
		 * required_outer probably can't be empty here, but if it were, we
		 * couldn't make a parameterized path.
		 *
		 * required_outer 这里可能不能为空，但如果是的话，我们就无法创建参数化路径。
		 */
		if (bms_is_empty(required_outer))
			continue;

		/* Get the ParamPathInfo
		 *
		 * 获取ParamPathInfo
		 */
		param_info = get_baserel_parampathinfo(root, baserel,
											   required_outer);
		Assert(param_info != NULL);

		/*
		 * Add it to list unless we already have it.  Testing pointer equality
		 * is OK since get_baserel_parampathinfo won't make duplicates.
		 *
		 * 将其添加到列表中，除非我们已经拥有它。  测试指针相等性是可以的，因为 get_baserel_parampathinfo 不会产生重复。
		 */
		ppi_list = list_append_unique_ptr(ppi_list, param_info);
	}

	/*
	 * The above scan examined only "generic" join clauses, not those that
	 * were absorbed into EquivalenceClauses.  See if we can make anything out
	 * of EquivalenceClauses.
	 *
	 * 上述扫描仅检查“通用”连接子句，而不是那些被吸收到 EquivalenceClauses 中的子句。  看看我们是否可以从等价条款中得到任何东西。
	 */
	if (baserel->has_eclass_joins)
	{
		/*
		 * We repeatedly scan the eclass list looking for column references
		 * (or expressions) belonging to the foreign rel.  Each time we find
		 * one, we generate a list of equivalence joinclauses for it, and then
		 * see if any are safe to send to the remote.  Repeat till there are
		 * no more candidate EC members.
		 *
		 * 我们反复扫描 eclass 列表，查找属于外部 rel 的列引用（或表达式）。  每次我们找到一个，我们都会为其生成一个等价连接子句列表，然后查看是否有任何可以安全地发送到远程。  重复此过程，直到不再有候选 EC 成员为止。
		 */
		ec_member_foreign_arg arg;

		arg.already_used = NIL;
		for (;;)
		{
			List	   *clauses;

			/* Make clauses, skipping any that join to lateral_referencers
			 *
			 * 制作子句，跳过任何连接到 Lateral_referencers 的子句
			 */
			arg.current = NULL;
			clauses = generate_implied_equalities_for_column(root,
															 baserel,
															 ec_member_matches_foreign,
															 &arg,
															 baserel->lateral_referencers);

			/* Done if there are no more expressions in the foreign rel
			 *
			 * 如果foreign rel中没有更多的表达式则完成
			 */
			if (arg.current == NULL)
			{
				Assert(clauses == NIL);
				break;
			}

			/* Scan the extracted join clauses
			 *
			 * 扫描提取的连接子句
			 */
			foreach(lc, clauses)
			{
				RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
				Relids		required_outer;
				ParamPathInfo *param_info;

				/* Check if clause can be moved to this rel
				 *
				 * 检查子句是否可以移动到此rel
				 */
				if (!join_clause_is_movable_to(rinfo, baserel))
					continue;

				/* See if it is safe to send to remote
				 *
				 * 查看发送到远程是否安全
				 */
				if (!is_foreign_expr(root, baserel, rinfo->clause))
					continue;

				/* Calculate required outer rels for the resulting path
				 *
				 * 计算结果路径所需的外部关系
				 */
				required_outer = bms_union(rinfo->clause_relids,
										   baserel->lateral_relids);
				required_outer = bms_del_member(required_outer, baserel->relid);
				if (bms_is_empty(required_outer))
					continue;

				/* Get the ParamPathInfo
				 *
				 * 获取ParamPathInfo
				 */
				param_info = get_baserel_parampathinfo(root, baserel,
													   required_outer);
				Assert(param_info != NULL);

				/* Add it to list unless we already have it
				 *
				 * 将其添加到列表中，除非我们已经拥有它
				 */
				ppi_list = list_append_unique_ptr(ppi_list, param_info);
			}

			/* Try again, now ignoring the expression we found this time
			 *
			 * 再试一次，现在忽略我们这次发现的表达式
			 */
			arg.already_used = lappend(arg.already_used, arg.current);
		}
	}

	/*
	 * Now build a path for each useful outer relation.
	 *
	 * 现在为每个有用的外部关系构建一条路径。
	 */
	foreach(lc, ppi_list)
	{
		ParamPathInfo *param_info = (ParamPathInfo *) lfirst(lc);
		double		rows;
		int			width;
		int			disabled_nodes;
		Cost		startup_cost;
		Cost		total_cost;

		/* Get a cost estimate from the remote
		 *
		 * 从远程获取成本估算
		 */
		estimate_path_cost_size(root, baserel,
								param_info->ppi_clauses, NIL, NULL,
								&rows, &width, &disabled_nodes,
								&startup_cost, &total_cost);

		/*
		 * ppi_rows currently won't get looked at by anything, but still we
		 * may as well ensure that it matches our idea of the rowcount.
		 *
		 * ppi_rows 目前不会被任何东西查看，但我们仍然可以确保它符合我们对行计数的想法。
		 */
		param_info->ppi_rows = rows;

		/* Make the path
		 *
		 * 制作路径
		 */
		path = create_foreignscan_path(root, baserel,
									   NULL,	/* default pathtarget */
									   rows,
									   disabled_nodes,
									   startup_cost,
									   total_cost,
									   NIL, /* no pathkeys */
									   param_info->ppi_req_outer,
									   NULL,
									   NIL, /* no fdw_restrictinfo list */
									   NIL);	/* no fdw_private list */
		add_path(baserel, (Path *) path);
	}
}

/*
 * postgresGetForeignPlan
 *		Create ForeignScan plan node which implements selected best path
 *
 * postgresGetForeignPlan 创建实现选定最佳路径的ForeignScan计划节点
 */
static ForeignScan *
postgresGetForeignPlan(PlannerInfo *root,
					   RelOptInfo *foreignrel,
					   Oid foreigntableid,
					   ForeignPath *best_path,
					   List *tlist,
					   List *scan_clauses,
					   Plan *outer_plan)
{
	PgFdwRelationInfo *fpinfo = (PgFdwRelationInfo *) foreignrel->fdw_private;
	Index		scan_relid;
	List	   *fdw_private;
	List	   *remote_exprs = NIL;
	List	   *local_exprs = NIL;
	List	   *params_list = NIL;
	List	   *fdw_scan_tlist = NIL;
	List	   *fdw_recheck_quals = NIL;
	List	   *retrieved_attrs;
	StringInfoData sql;
	bool		has_final_sort = false;
	bool		has_limit = false;
	ListCell   *lc;

	/*
	 * Get FDW private data created by postgresGetForeignUpperPaths(), if any.
	 *
	 * 获取由 postgresGetForeignUpperPaths() 创建的 FDW 私有数据（如果有）。
	 */
	if (best_path->fdw_private)
	{
		has_final_sort = boolVal(list_nth(best_path->fdw_private,
										  FdwPathPrivateHasFinalSort));
		has_limit = boolVal(list_nth(best_path->fdw_private,
									 FdwPathPrivateHasLimit));
	}

	if (IS_SIMPLE_REL(foreignrel))
	{
		/*
		 * For base relations, set scan_relid as the relid of the relation.
		 *
		 * 对于基础关系，将 scan_relid 设置为关系的 relid。
		 */
		scan_relid = foreignrel->relid;

		/*
		 * In a base-relation scan, we must apply the given scan_clauses.
		 *
		 * 在基本关系扫描中，我们必须应用给定的 scan_clauses。
		 *
		 * Separate the scan_clauses into those that can be executed remotely
		 * and those that can't.  baserestrictinfo clauses that were
		 * previously determined to be safe or unsafe by classifyConditions
		 * are found in fpinfo->remote_conds and fpinfo->local_conds. Anything
		 * else in the scan_clauses list will be a join clause, which we have
		 * to check for remote-safety.
		 *
		 * 将 scan_clauses 分为可以远程执行的和不能远程执行的。  先前由classifyConditions 确定为安全或不安全的baserestrictinfo 子句可在fpinfo->remote_conds 和fpinfo->local_conds 中找到。 scan_clauses 列表中的任何其他内容都将是一个连接子句，我们必须检查它的远程安全性。
		 *
		 * Note: the join clauses we see here should be the exact same ones
		 * previously examined by postgresGetForeignPaths.  Possibly it'd be
		 * worth passing forward the classification work done then, rather
		 * than repeating it here.
		 *
		 * 注意：我们在这里看到的连接子句应该与 postgresGetForeignPaths 之前检查的完全相同。  也许值得将当时完成的分类工作继续下去，而不是在这里重复。
		 *
		 * This code must match "extract_actual_clauses(scan_clauses, false)"
		 * except for the additional decision about remote versus local
		 * execution.
		 *
		 * 此代码必须匹配“extract_actual_clauses(scan_clauses, false)”，除了有关远程执行与本地执行的附加决定之外。
		 */
		foreach(lc, scan_clauses)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

			/* Ignore any pseudoconstants, they're dealt with elsewhere
			 *
			 * 忽略任何伪常数，它们在其他地方处理
			 */
			if (rinfo->pseudoconstant)
				continue;

			if (list_member_ptr(fpinfo->remote_conds, rinfo))
				remote_exprs = lappend(remote_exprs, rinfo->clause);
			else if (list_member_ptr(fpinfo->local_conds, rinfo))
				local_exprs = lappend(local_exprs, rinfo->clause);
			else if (is_foreign_expr(root, foreignrel, rinfo->clause))
				remote_exprs = lappend(remote_exprs, rinfo->clause);
			else
				local_exprs = lappend(local_exprs, rinfo->clause);
		}

		/*
		 * For a base-relation scan, we have to support EPQ recheck, which
		 * should recheck all the remote quals.
		 *
		 * 对于基础关系扫描，我们必须支持 EPQ 重新检查，这应该重新检查所有远程质量。
		 */
		fdw_recheck_quals = remote_exprs;
	}
	else
	{
		/*
		 * Join relation or upper relation - set scan_relid to 0.
		 *
		 * 连接关系或上层关系 - 将 scan_relid 设置为 0。
		 */
		scan_relid = 0;

		/*
		 * For a join rel, baserestrictinfo is NIL and we are not considering
		 * parameterization right now, so there should be no scan_clauses for
		 * a joinrel or an upper rel either.
		 *
		 * 对于 joinrel，baserestrictinfo 是 NIL，我们现在不考虑参数化，所以 joinrel 或 upper rel 也不应该有 scan_clauses。
		 */
		Assert(!scan_clauses);

		/*
		 * Instead we get the conditions to apply from the fdw_private
		 * structure.
		 *
		 * 相反，我们从 fdw_private 结构中获取要应用的条件。
		 */
		remote_exprs = extract_actual_clauses(fpinfo->remote_conds, false);
		local_exprs = extract_actual_clauses(fpinfo->local_conds, false);

		/*
		 * We leave fdw_recheck_quals empty in this case, since we never need
		 * to apply EPQ recheck clauses.  In the case of a joinrel, EPQ
		 * recheck is handled elsewhere --- see postgresGetForeignJoinPaths().
		 * If we're planning an upperrel (ie, remote grouping or aggregation)
		 * then there's no EPQ to do because SELECT FOR UPDATE wouldn't be
		 * allowed, and indeed we *can't* put the remote clauses into
		 * fdw_recheck_quals because the unaggregated Vars won't be available
		 * locally.
		 *
		 * 在这种情况下，我们将 fdw_recheck_quals 留空，因为我们永远不需要应用 EPQ 重新检查子句。  对于 joinrel，EPQ 重新检查在其他地方处理 --- 请参阅 postgresGetForeignJoinPaths()。如果我们计划一个upperrel（即远程分组或聚合），那么就没有EPQ要做，因为不允许SELECT FOR UPDATE，而且实际上我们*不能*将远程子句放入fdw_recheck_quals中，因为未聚合的变量在本地不可用。
		 */

		/* Build the list of columns to be fetched from the foreign server.
		 *
		 * 构建要从外部服务器获取的列的列表。
		 */
		fdw_scan_tlist = build_tlist_to_deparse(foreignrel);

		/*
		 * Ensure that the outer plan produces a tuple whose descriptor
		 * matches our scan tuple slot.  Also, remove the local conditions
		 * from outer plan's quals, lest they be evaluated twice, once by the
		 * local plan and once by the scan.
		 *
		 * 确保外部计划生成一个描述符与我们的扫描元组槽匹配的元组。  另外，从外部计划的限定中删除本地条件，以免它们被评估两次，一次由本地计划评估，一次由扫描评估。
		 */
		if (outer_plan)
		{
			/*
			 * Right now, we only consider grouping and aggregation beyond
			 * joins. Queries involving aggregates or grouping do not require
			 * EPQ mechanism, hence should not have an outer plan here.
			 *
			 * 目前，我们只考虑连接之外的分组和聚合。涉及聚合或分组的查询不需要 EPQ 机制，因此不应在此处有外部计划。
			 */
			Assert(!IS_UPPER_REL(foreignrel));

			/*
			 * First, update the plan's qual list if possible.  In some cases
			 * the quals might be enforced below the topmost plan level, in
			 * which case we'll fail to remove them; it's not worth working
			 * harder than this.
			 *
			 * 首先，如果可能的话，更新计划的资格列表。  在某些情况下，这些限定可能会在最顶层计划级别以下强制执行，在这种情况下，我们将无法删除它们；不值得比这更努力。
			 */
			foreach(lc, local_exprs)
			{
				Node	   *qual = lfirst(lc);

				outer_plan->qual = list_delete(outer_plan->qual, qual);

				/*
				 * For an inner join the local conditions of foreign scan plan
				 * can be part of the joinquals as well.  (They might also be
				 * in the mergequals or hashquals, but we can't touch those
				 * without breaking the plan.)
				 *
				 * 对于内部连接，外部扫描计划的本地条件也可以是连接的一部分。  （它们也可能在 mergequals 或 hashquals 中，但我们不能在不破坏计划的情况下触及它们。）
				 */
				if (IsA(outer_plan, NestLoop) ||
					IsA(outer_plan, MergeJoin) ||
					IsA(outer_plan, HashJoin))
				{
					Join	   *join_plan = (Join *) outer_plan;

					if (join_plan->jointype == JOIN_INNER)
						join_plan->joinqual = list_delete(join_plan->joinqual,
														  qual);
				}
			}

			/*
			 * Now fix the subplan's tlist --- this might result in inserting
			 * a Result node atop the plan tree.
			 *
			 * 现在修复子计划的 tlist --- 这可能会导致在计划树顶部插入一个结果节点。
			 */
			outer_plan = change_plan_targetlist(outer_plan, fdw_scan_tlist,
												best_path->path.parallel_safe);
		}
	}

	/*
	 * Build the query string to be sent for execution, and identify
	 * expressions to be sent as parameters.
	 *
	 * 构建要发送执行的查询字符串，并标识要作为参数发送的表达式。
	 */
	initStringInfo(&sql);
	deparseSelectStmtForRel(&sql, root, foreignrel, fdw_scan_tlist,
							remote_exprs, best_path->path.pathkeys,
							has_final_sort, has_limit, false,
							&retrieved_attrs, &params_list);

	/* Remember remote_exprs for possible use by postgresPlanDirectModify
	 *
	 * 记住remote_exprs以供postgresPlanDirectModify使用
	 */
	fpinfo->final_remote_exprs = remote_exprs;

	/*
	 * Build the fdw_private list that will be available to the executor.
	 * Items in the list must match order in enum FdwScanPrivateIndex.
	 *
	 * 构建可供执行程序使用的 fdw_private 列表。列表中的项目必须与枚举 FdwScanPrivateIndex 中的顺序匹配。
	 */
	fdw_private = list_make3(makeString(sql.data),
							 retrieved_attrs,
							 makeInteger(fpinfo->fetch_size));
	if (IS_JOIN_REL(foreignrel) || IS_UPPER_REL(foreignrel))
		fdw_private = lappend(fdw_private,
							  makeString(fpinfo->relation_name));

	/*
	 * Create the ForeignScan node for the given relation.
	 *
	 * 为给定关系创建ForeignScan 节点。
	 *
	 * Note that the remote parameter expressions are stored in the fdw_exprs
	 * field of the finished plan node; we can't keep them in private state
	 * because then they wouldn't be subject to later planner processing.
	 *
	 * 请注意，远程参数表达式存储在完成的计划节点的 fdw_exprs 字段中；我们不能将它们保留在私有状态，因为这样它们就不会受到后续规划器处理的影响。
	 */
	return make_foreignscan(tlist,
							local_exprs,
							scan_relid,
							params_list,
							fdw_private,
							fdw_scan_tlist,
							fdw_recheck_quals,
							outer_plan);
}

/*
 * Construct a tuple descriptor for the scan tuples handled by a foreign join.
 *
 * 为外部连接处理的扫描元组构造一个元组描述符。
 */
static TupleDesc
get_tupdesc_for_join_scan_tuples(ForeignScanState *node)
{
	ForeignScan *fsplan = (ForeignScan *) node->ss.ps.plan;
	EState	   *estate = node->ss.ps.state;
	TupleDesc	tupdesc;

	/*
	 * The core code has already set up a scan tuple slot based on
	 * fsplan->fdw_scan_tlist, and this slot's tupdesc is mostly good enough,
	 * but there's one case where it isn't.  If we have any whole-row row
	 * identifier Vars, they may have vartype RECORD, and we need to replace
	 * that with the associated table's actual composite type.  This ensures
	 * that when we read those ROW() expression values from the remote server,
	 * we can convert them to a composite type the local server knows.
	 *
	 * 核心代码已经基于 fsplan->fdw_scan_tlist 设置了一个扫描元组槽，并且该槽的 tupdesc 大部分都足够好，但有一种情况还不够。  如果我们有任何整行行标识符 Vars，它们可能具有 vartype RECORD，我们需要将其替换为关联表的实际复合类型。  这确保了当我们从远程服务器读取这些 ROW() 表达式值时，我们可以将它们转换为本地服务器知道的复合类型。
	 */
	tupdesc = CreateTupleDescCopy(node->ss.ss_ScanTupleSlot->tts_tupleDescriptor);
	for (int i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		Var		   *var;
		RangeTblEntry *rte;
		Oid			reltype;

		/* Nothing to do if it's not a generic RECORD attribute
		 *
		 * 如果它不是通用 RECORD 属性，则无需执行任何操作
		 */
		if (att->atttypid != RECORDOID || att->atttypmod >= 0)
			continue;

		/*
		 * If we can't identify the referenced table, do nothing.  This'll
		 * likely lead to failure later, but perhaps we can muddle through.
		 *
		 * 如果我们无法识别引用的表，则不执行任何操作。  这可能会导致以后失败，但也许我们可以蒙混过关。
		 */
		var = (Var *) list_nth_node(TargetEntry, fsplan->fdw_scan_tlist,
									i)->expr;
		if (!IsA(var, Var) || var->varattno != 0)
			continue;
		rte = list_nth(estate->es_range_table, var->varno - 1);
		if (rte->rtekind != RTE_RELATION)
			continue;
		reltype = get_rel_type_id(rte->relid);
		if (!OidIsValid(reltype))
			continue;
		att->atttypid = reltype;
		/* shouldn't need to change anything else
		 *
		 * 不需要改变任何其他东西
		 */
	}
	return tupdesc;
}

/*
 * postgresBeginForeignScan
 *		Initiate an executor scan of a foreign PostgreSQL table.
 *
 * postgresBeginForeignScan 启动外部 PostgreSQL 表的执行程序扫描。
 */
static void
postgresBeginForeignScan(ForeignScanState *node, int eflags)
{
	ForeignScan *fsplan = (ForeignScan *) node->ss.ps.plan;
	EState	   *estate = node->ss.ps.state;
	PgFdwScanState *fsstate;
	RangeTblEntry *rte;
	Oid			userid;
	ForeignTable *table;
	UserMapping *user;
	int			rtindex;
	int			numParams;

	/*
	 * Do nothing in EXPLAIN (no ANALYZE) case.  node->fdw_state stays NULL.
	 *
	 * 在 EXPLAIN（无 ANALYZE）情况下不执行任何操作。  node->fdw_state 保持 NULL。
	 */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	/*
	 * We'll save private state in node->fdw_state.
	 *
	 * 我们将把私有状态保存在node->fdw_state中。
	 */
	fsstate = (PgFdwScanState *) palloc0(sizeof(PgFdwScanState));
	node->fdw_state = fsstate;

	/*
	 * Identify which user to do the remote access as.  This should match what
	 * ExecCheckPermissions() does.
	 *
	 * 确定以哪个用户身份进行远程访问。  这应该与 ExecCheckPermissions() 的作用相匹配。
	 */
	userid = OidIsValid(fsplan->checkAsUser) ? fsplan->checkAsUser : GetUserId();
	if (fsplan->scan.scanrelid > 0)
		rtindex = fsplan->scan.scanrelid;
	else
		rtindex = bms_next_member(fsplan->fs_base_relids, -1);
	rte = exec_rt_fetch(rtindex, estate);

	/* Get info about foreign table.
	 *
	 * 获取有关外部表的信息。
	 */
	table = GetForeignTable(rte->relid);
	user = GetUserMapping(userid, table->serverid);

	/*
	 * Get connection to the foreign server.  Connection manager will
	 * establish new connection if necessary.
	 *
	 * 获取与外部服务器的连接。  如果需要，连接管理器将建立新连接。
	 */
	fsstate->conn = GetConnection(user, false, &fsstate->conn_state);

	/* Assign a unique ID for my cursor
	 *
	 * 为我的光标分配一个唯一的 ID
	 */
	fsstate->cursor_number = GetCursorNumber(fsstate->conn);
	fsstate->cursor_exists = false;

	/* Get private info created by planner functions.
	 *
	 * 获取规划器功能创建的私人信息。
	 */
	fsstate->query = strVal(list_nth(fsplan->fdw_private,
									 FdwScanPrivateSelectSql));
	fsstate->retrieved_attrs = (List *) list_nth(fsplan->fdw_private,
												 FdwScanPrivateRetrievedAttrs);
	fsstate->fetch_size = intVal(list_nth(fsplan->fdw_private,
										  FdwScanPrivateFetchSize));

	/* Create contexts for batches of tuples and per-tuple temp workspace.
	 *
	 * 为批量元组和每个元组临时工作区创建上下文。
	 */
	fsstate->batch_cxt = AllocSetContextCreate(estate->es_query_cxt,
											   "postgres_fdw tuple data",
											   ALLOCSET_DEFAULT_SIZES);
	fsstate->temp_cxt = AllocSetContextCreate(estate->es_query_cxt,
											  "postgres_fdw temporary data",
											  ALLOCSET_SMALL_SIZES);

	/*
	 * Get info we'll need for converting data fetched from the foreign server
	 * into local representation and error reporting during that process.
	 *
	 * 获取我们将从外部服务器获取的数据转换为本地表示和在此过程中的错误报告所需的信息。
	 */
	if (fsplan->scan.scanrelid > 0)
	{
		fsstate->rel = node->ss.ss_currentRelation;
		fsstate->tupdesc = RelationGetDescr(fsstate->rel);
	}
	else
	{
		fsstate->rel = NULL;
		fsstate->tupdesc = get_tupdesc_for_join_scan_tuples(node);
	}

	fsstate->attinmeta = TupleDescGetAttInMetadata(fsstate->tupdesc);

	/*
	 * Prepare for processing of parameters used in remote query, if any.
	 *
	 * 准备处理远程查询中使用的参数（如果有）。
	 */
	numParams = list_length(fsplan->fdw_exprs);
	fsstate->numParams = numParams;
	if (numParams > 0)
		prepare_query_params((PlanState *) node,
							 fsplan->fdw_exprs,
							 numParams,
							 &fsstate->param_flinfo,
							 &fsstate->param_exprs,
							 &fsstate->param_values);

	/* Set the async-capable flag
	 *
	 * 设置支持异步的标志
	 */
	fsstate->async_capable = node->ss.ps.async_capable;
}

/*
 * postgresIterateForeignScan
 *		Retrieve next row from the result set, or clear tuple slot to indicate
 *		EOF.
 *
 * postgresIterateForeignScan 从结果集中检索下一行，或清除元组槽以指示 EOF。
 */
static TupleTableSlot *
postgresIterateForeignScan(ForeignScanState *node)
{
	PgFdwScanState *fsstate = (PgFdwScanState *) node->fdw_state;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;

	/*
	 * In sync mode, if this is the first call after Begin or ReScan, we need
	 * to create the cursor on the remote side.  In async mode, we would have
	 * already created the cursor before we get here, even if this is the
	 * first call after Begin or ReScan.
	 *
	 * 在同步模式下，如果这是 Begin 或 ReScan 之后的第一次调用，我们需要在远程端创建光标。  在异步模式下，我们在到达这里之前就已经创建了光标，即使这是 Begin 或 ReScan 之后的第一次调用。
	 */
	if (!fsstate->cursor_exists)
		create_cursor(node);

	/*
	 * Get some more tuples, if we've run out.
	 *
	 * 如果我们用完了，再获取一些元组。
	 */
	if (fsstate->next_tuple >= fsstate->num_tuples)
	{
		/* In async mode, just clear tuple slot.
		 *
		 * 在异步模式下，只需清除元组槽即可。
		 */
		if (fsstate->async_capable)
			return ExecClearTuple(slot);
		/* No point in another fetch if we already detected EOF, though.
		 *
		 * 不过，如果我们已经检测到 EOF，那么再次获取就没有意义。
		 */
		if (!fsstate->eof_reached)
			fetch_more_data(node);
		/* If we didn't get any tuples, must be end of data.
		 *
		 * 如果我们没有得到任何元组，则一定是数据结束。
		 */
		if (fsstate->next_tuple >= fsstate->num_tuples)
			return ExecClearTuple(slot);
	}

	/*
	 * Return the next tuple.
	 *
	 * 返回下一个元组。
	 */
	ExecStoreHeapTuple(fsstate->tuples[fsstate->next_tuple++],
					   slot,
					   false);

	return slot;
}

/*
 * postgresReScanForeignScan
 *		Restart the scan.
 *
 * postgresReScanForeignScan 重新启动扫描。
 */
static void
postgresReScanForeignScan(ForeignScanState *node)
{
	PgFdwScanState *fsstate = (PgFdwScanState *) node->fdw_state;
	char		sql[64];
	PGresult   *res;

	/* If we haven't created the cursor yet, nothing to do.
	 *
	 * 如果我们还没有创建光标，则无需执行任何操作。
	 */
	if (!fsstate->cursor_exists)
		return;

	/*
	 * If the node is async-capable, and an asynchronous fetch for it has
	 * begun, the asynchronous fetch might not have yet completed.  Check if
	 * the node is async-capable, and an asynchronous fetch for it is still in
	 * progress; if so, complete the asynchronous fetch before restarting the
	 * scan.
	 *
	 * 如果节点支持异步，并且异步获取已开始，则异步获取可能尚未完成。  检查节点是否支持异步，并且异步获取仍在进行中；如果是这样，请在重新启动扫描之前完成异步获取。
	 */
	if (fsstate->async_capable &&
		fsstate->conn_state->pendingAreq &&
		fsstate->conn_state->pendingAreq->requestee == (PlanState *) node)
		fetch_more_data(node);

	/*
	 * If any internal parameters affecting this node have changed, we'd
	 * better destroy and recreate the cursor.  Otherwise, if the remote
	 * server is v14 or older, rewinding it should be good enough; if not,
	 * rewind is only allowed for scrollable cursors, but we don't have a way
	 * to check the scrollability of it, so destroy and recreate it in any
	 * case.  If we've only fetched zero or one batch, we needn't even rewind
	 * the cursor, just rescan what we have.
	 *
	 * 如果影响该节点的任何内部参数发生了变化，我们最好销毁并重新创建游标。  否则，如果远程服务器是 v14 或更早版本，则回滚它应该就足够了；如果不是，则仅允许可滚动游标倒回，但我们没有办法检查它的可滚动性，因此无论如何都要销毁并重新创建它。  如果我们只获取了零个或一批，我们甚至不需要倒回光标，只需重新扫描我们拥有的内容即可。
	 */
	if (node->ss.ps.chgParam != NULL)
	{
		fsstate->cursor_exists = false;
		snprintf(sql, sizeof(sql), "CLOSE c%u",
				 fsstate->cursor_number);
	}
	else if (fsstate->fetch_ct_2 > 1)
	{
		if (PQserverVersion(fsstate->conn) < 150000)
			snprintf(sql, sizeof(sql), "MOVE BACKWARD ALL IN c%u",
					 fsstate->cursor_number);
		else
		{
			fsstate->cursor_exists = false;
			snprintf(sql, sizeof(sql), "CLOSE c%u",
					 fsstate->cursor_number);
		}
	}
	else
	{
		/* Easy: just rescan what we already have in memory, if anything
		 *
		 * 简单：只需重新扫描我们内存中已有的内容（如果有的话）
		 */
		fsstate->next_tuple = 0;
		return;
	}

	/*
	 * We don't use a PG_TRY block here, so be careful not to throw error
	 * without releasing the PGresult.
	 *
	 * 我们在这里不使用 PG_TRY 块，因此请注意不要在未释放 PGresult 的情况下引发错误。
	 */
	res = pgfdw_exec_query(fsstate->conn, sql, fsstate->conn_state);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pgfdw_report_error(ERROR, res, fsstate->conn, true, sql);
	PQclear(res);

	/* Now force a fresh FETCH.
	 *
	 * 现在强制进行新的 FETCH。
	 */
	fsstate->tuples = NULL;
	fsstate->num_tuples = 0;
	fsstate->next_tuple = 0;
	fsstate->fetch_ct_2 = 0;
	fsstate->eof_reached = false;
}

/*
 * postgresEndForeignScan
 *		Finish scanning foreign table and dispose objects used for this scan
 *
 * postgresEndForeignScan 完成外部表扫描并处置用于此扫描的对象
 */
static void
postgresEndForeignScan(ForeignScanState *node)
{
	PgFdwScanState *fsstate = (PgFdwScanState *) node->fdw_state;

	/* if fsstate is NULL, we are in EXPLAIN; nothing to do
	 *
	 * 如果 fsstate 为 NULL，则处于 EXPLAIN 状态；无事可做
	 */
	if (fsstate == NULL)
		return;

	/* Close the cursor if open, to prevent accumulation of cursors
	 *
	 * 如果游标打开，则将其关闭，以防止游标累积
	 */
	if (fsstate->cursor_exists)
		close_cursor(fsstate->conn, fsstate->cursor_number,
					 fsstate->conn_state);

	/* Release remote connection
	 *
	 * 释放远程连接
	 */
	ReleaseConnection(fsstate->conn);
	fsstate->conn = NULL;

	/* MemoryContexts will be deleted automatically.
	 *
	 * MemoryContexts 将被自动删除。
	 */
}

/*
 * postgresAddForeignUpdateTargets
 *		Add resjunk column(s) needed for update/delete on a foreign table
 *
 * postgresAddForeignUpdateTargets 添加外部表上更新/删除所需的 resjunk 列
 */
static void
postgresAddForeignUpdateTargets(PlannerInfo *root,
								Index rtindex,
								RangeTblEntry *target_rte,
								Relation target_relation)
{
	Var		   *var;

	/*
	 * In postgres_fdw, what we need is the ctid, same as for a regular table.
	 *
	 * 在postgres_fdw中，我们需要的是ctid，与常规表相同。
	 */

	/* Make a Var representing the desired value
	 *
	 * 创建一个代表所需值的 Var
	 */
	var = makeVar(rtindex,
				  SelfItemPointerAttributeNumber,
				  TIDOID,
				  -1,
				  InvalidOid,
				  0);

	/* Register it as a row-identity column needed by this target rel
	 *
	 * 将其注册为该目标rel所需的行标识列
	 */
	add_row_identity_var(root, var, rtindex, "ctid");
}

/*
 * postgresPlanForeignModify
 *		Plan an insert/update/delete operation on a foreign table
 *
 * postgresPlanForeignModify 规划外部表上的插入/更新/删除操作
 */
static List *
postgresPlanForeignModify(PlannerInfo *root,
						  ModifyTable *plan,
						  Index resultRelation,
						  int subplan_index)
{
	CmdType		operation = plan->operation;
	RangeTblEntry *rte = planner_rt_fetch(resultRelation, root);
	Relation	rel;
	StringInfoData sql;
	List	   *targetAttrs = NIL;
	List	   *withCheckOptionList = NIL;
	List	   *returningList = NIL;
	List	   *retrieved_attrs = NIL;
	bool		doNothing = false;
	int			values_end_len = -1;

	initStringInfo(&sql);

	/*
	 * Core code already has some lock on each rel being planned, so we can
	 * use NoLock here.
	 *
	 * 核心代码已经对正在规划的每个rel进行了一些锁定，因此我们可以在这里使用NoLock。
	 */
	rel = table_open(rte->relid, NoLock);

	/*
	 * In an INSERT, we transmit all columns that are defined in the foreign
	 * table.  In an UPDATE, if there are BEFORE ROW UPDATE triggers on the
	 * foreign table, we transmit all columns like INSERT; else we transmit
	 * only columns that were explicitly targets of the UPDATE, so as to avoid
	 * unnecessary data transmission.  (We can't do that for INSERT since we
	 * would miss sending default values for columns not listed in the source
	 * statement, and for UPDATE if there are BEFORE ROW UPDATE triggers since
	 * those triggers might change values for non-target columns, in which
	 * case we would miss sending changed values for those columns.)
	 *
	 * 在 INSERT 中，我们传输外部表中定义的所有列。  在 UPDATE 中，如果外表上有 BEFORE ROW UPDATE 触发器，我们会像 INSERT 一样传输所有列；否则，我们仅传输明确作为 UPDATE 目标的列，以避免不必要的数据传输。  （对于 INSERT，我们不能这样做，因为我们会错过发送源语句中未列出的列的默认值；对于 UPDATE，如果存在 BEFORE ROW UPDATE 触发器，则无法这样做，因为这些触发器可能会更改非目标列的值，在这种情况下，我们将错过发送这些列的更改值。）
	 */
	if (operation == CMD_INSERT ||
		(operation == CMD_UPDATE &&
		 rel->trigdesc &&
		 rel->trigdesc->trig_update_before_row))
	{
		TupleDesc	tupdesc = RelationGetDescr(rel);
		int			attnum;

		for (attnum = 1; attnum <= tupdesc->natts; attnum++)
		{
			CompactAttribute *attr = TupleDescCompactAttr(tupdesc, attnum - 1);

			if (!attr->attisdropped)
				targetAttrs = lappend_int(targetAttrs, attnum);
		}
	}
	else if (operation == CMD_UPDATE)
	{
		int			col;
		RelOptInfo *rel = find_base_rel(root, resultRelation);
		Bitmapset  *allUpdatedCols = get_rel_all_updated_cols(root, rel);

		col = -1;
		while ((col = bms_next_member(allUpdatedCols, col)) >= 0)
		{
			/* bit numbers are offset by FirstLowInvalidHeapAttributeNumber
			 *
			 * 位编号由 FirstLowInvalidHeapAttributeNumber 偏移
			 */
			AttrNumber	attno = col + FirstLowInvalidHeapAttributeNumber;

			if (attno <= InvalidAttrNumber) /* shouldn't happen */
				elog(ERROR, "system-column update is not supported");
			targetAttrs = lappend_int(targetAttrs, attno);
		}
	}

	/*
	 * Extract the relevant WITH CHECK OPTION list if any.
	 *
	 * 提取相关的WITH CHECK OPTION 列表（如果有）。
	 */
	if (plan->withCheckOptionLists)
		withCheckOptionList = (List *) list_nth(plan->withCheckOptionLists,
												subplan_index);

	/*
	 * Extract the relevant RETURNING list if any.
	 *
	 * 提取相关的返回列表（如果有）。
	 */
	if (plan->returningLists)
		returningList = (List *) list_nth(plan->returningLists, subplan_index);

	/*
	 * ON CONFLICT DO UPDATE and DO NOTHING case with inference specification
	 * should have already been rejected in the optimizer, as presently there
	 * is no way to recognize an arbiter index on a foreign table.  Only DO
	 * NOTHING is supported without an inference specification.
	 *
	 * ON CONFLICT DO UPDATE 和 DO NOTHING 情况与推理规范应该已经在优化器中被拒绝，因为目前无法识别外部表上的仲裁索引。  在没有推理规范的情况下仅支持 DO NOTHING。
	 */
	if (plan->onConflictAction == ONCONFLICT_NOTHING)
		doNothing = true;
	else if (plan->onConflictAction != ONCONFLICT_NONE)
		elog(ERROR, "unexpected ON CONFLICT specification: %d",
			 (int) plan->onConflictAction);

	/*
	 * Construct the SQL command string.
	 *
	 * 构造 SQL 命令字符串。
	 */
	switch (operation)
	{
		case CMD_INSERT:
			deparseInsertSql(&sql, rte, resultRelation, rel,
							 targetAttrs, doNothing,
							 withCheckOptionList, returningList,
							 &retrieved_attrs, &values_end_len);
			break;
		case CMD_UPDATE:
			deparseUpdateSql(&sql, rte, resultRelation, rel,
							 targetAttrs,
							 withCheckOptionList, returningList,
							 &retrieved_attrs);
			break;
		case CMD_DELETE:
			deparseDeleteSql(&sql, rte, resultRelation, rel,
							 returningList,
							 &retrieved_attrs);
			break;
		default:
			elog(ERROR, "unexpected operation: %d", (int) operation);
			break;
	}

	table_close(rel, NoLock);

	/*
	 * Build the fdw_private list that will be available to the executor.
	 * Items in the list must match enum FdwModifyPrivateIndex, above.
	 *
	 * 构建可供执行程序使用的 fdw_private 列表。列表中的项目必须与上面的枚举 FdwModifyPrivateIndex 匹配。
	 */
	return list_make5(makeString(sql.data),
					  targetAttrs,
					  makeInteger(values_end_len),
					  makeBoolean((retrieved_attrs != NIL)),
					  retrieved_attrs);
}

/*
 * postgresBeginForeignModify
 *		Begin an insert/update/delete operation on a foreign table
 *
 * postgresBeginForeignModify 在外部表上开始插入/更新/删除操作
 */
static void
postgresBeginForeignModify(ModifyTableState *mtstate,
						   ResultRelInfo *resultRelInfo,
						   List *fdw_private,
						   int subplan_index,
						   int eflags)
{
	PgFdwModifyState *fmstate;
	char	   *query;
	List	   *target_attrs;
	bool		has_returning;
	int			values_end_len;
	List	   *retrieved_attrs;
	RangeTblEntry *rte;

	/*
	 * Do nothing in EXPLAIN (no ANALYZE) case.  resultRelInfo->ri_FdwState
	 * stays NULL.
	 *
	 * 在 EXPLAIN（无 ANALYZE）情况下不执行任何操作。  resultRelInfo->ri_FdwState 保持 NULL。
	 */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	/* Deconstruct fdw_private data.
	 *
	 * 解构 fdw_private 数据。
	 */
	query = strVal(list_nth(fdw_private,
							FdwModifyPrivateUpdateSql));
	target_attrs = (List *) list_nth(fdw_private,
									 FdwModifyPrivateTargetAttnums);
	values_end_len = intVal(list_nth(fdw_private,
									 FdwModifyPrivateLen));
	has_returning = boolVal(list_nth(fdw_private,
									 FdwModifyPrivateHasReturning));
	retrieved_attrs = (List *) list_nth(fdw_private,
										FdwModifyPrivateRetrievedAttrs);

	/* Find RTE.
	 *
	 * 找到RTE。
	 */
	rte = exec_rt_fetch(resultRelInfo->ri_RangeTableIndex,
						mtstate->ps.state);

	/* Construct an execution state.
	 *
	 * 构造一个执行状态。
	 */
	fmstate = create_foreign_modify(mtstate->ps.state,
									rte,
									resultRelInfo,
									mtstate->operation,
									outerPlanState(mtstate)->plan,
									query,
									target_attrs,
									values_end_len,
									has_returning,
									retrieved_attrs);

	resultRelInfo->ri_FdwState = fmstate;
}

/*
 * postgresExecForeignInsert
 *		Insert one row into a foreign table
 *
 * postgresExecForeignInsert 将一行插入外部表
 */
static TupleTableSlot *
postgresExecForeignInsert(EState *estate,
						  ResultRelInfo *resultRelInfo,
						  TupleTableSlot *slot,
						  TupleTableSlot *planSlot)
{
	PgFdwModifyState *fmstate = (PgFdwModifyState *) resultRelInfo->ri_FdwState;
	TupleTableSlot **rslot;
	int			numSlots = 1;

	/*
	 * If the fmstate has aux_fmstate set, use the aux_fmstate (see
	 * postgresBeginForeignInsert())
	 *
	 * 如果 fmstate 设置了 aux_fmstate，则使用 aux_fmstate （请参阅 postgresBeginForeignInsert()）
	 */
	if (fmstate->aux_fmstate)
		resultRelInfo->ri_FdwState = fmstate->aux_fmstate;
	rslot = execute_foreign_modify(estate, resultRelInfo, CMD_INSERT,
								   &slot, &planSlot, &numSlots);
	/* Revert that change
	 *
	 * 恢复该更改
	 */
	if (fmstate->aux_fmstate)
		resultRelInfo->ri_FdwState = fmstate;

	return rslot ? *rslot : NULL;
}

/*
 * postgresExecForeignBatchInsert
 *		Insert multiple rows into a foreign table
 *
 * postgresExecForeignBatchInsert 将多行插入外部表
 */
static TupleTableSlot **
postgresExecForeignBatchInsert(EState *estate,
							   ResultRelInfo *resultRelInfo,
							   TupleTableSlot **slots,
							   TupleTableSlot **planSlots,
							   int *numSlots)
{
	PgFdwModifyState *fmstate = (PgFdwModifyState *) resultRelInfo->ri_FdwState;
	TupleTableSlot **rslot;

	/*
	 * If the fmstate has aux_fmstate set, use the aux_fmstate (see
	 * postgresBeginForeignInsert())
	 *
	 * 如果 fmstate 设置了 aux_fmstate，则使用 aux_fmstate （请参阅 postgresBeginForeignInsert()）
	 */
	if (fmstate->aux_fmstate)
		resultRelInfo->ri_FdwState = fmstate->aux_fmstate;
	rslot = execute_foreign_modify(estate, resultRelInfo, CMD_INSERT,
								   slots, planSlots, numSlots);
	/* Revert that change
	 *
	 * 恢复该更改
	 */
	if (fmstate->aux_fmstate)
		resultRelInfo->ri_FdwState = fmstate;

	return rslot;
}

/*
 * postgresGetForeignModifyBatchSize
 *		Determine the maximum number of tuples that can be inserted in bulk
 *
 * postgresGetForeignModifyBatchSize 确定可以批量插入的元组的最大数量
 *
 * Returns the batch size specified for server or table. When batching is not
 * allowed (e.g. for tables with BEFORE/AFTER ROW triggers or with RETURNING
 * clause), returns 1.
 *
 * 返回为服务器或表指定的批量大小。当不允许批处理时（例如，对于具有 BEFORE/AFTER ROW 触发器或 RETURNING 子句的表），返回 1。
 */
static int
postgresGetForeignModifyBatchSize(ResultRelInfo *resultRelInfo)
{
	int			batch_size;
	PgFdwModifyState *fmstate = (PgFdwModifyState *) resultRelInfo->ri_FdwState;

	/* should be called only once
	 *
	 * 应该只调用一次
	 */
	Assert(resultRelInfo->ri_BatchSize == 0);

	/*
	 * Should never get called when the insert is being performed on a table
	 * that is also among the target relations of an UPDATE operation, because
	 * postgresBeginForeignInsert() currently rejects such insert attempts.
	 *
	 * 当在 UPDATE 操作的目标关系中的表上执行插入时，永远不应该调用，因为 postgresBeginForeignInsert() 当前拒绝此类插入尝试。
	 */
	Assert(fmstate == NULL || fmstate->aux_fmstate == NULL);

	/*
	 * In EXPLAIN without ANALYZE, ri_FdwState is NULL, so we have to lookup
	 * the option directly in server/table options. Otherwise just use the
	 * value we determined earlier.
	 *
	 * 在没有 ANALYZE 的 EXPLAIN 中，ri_FdwState 为 NULL，因此我们必须直接在服务器/表选项中查找该选项。否则就使用我们之前确定的值。
	 */
	if (fmstate)
		batch_size = fmstate->batch_size;
	else
		batch_size = get_batch_size_option(resultRelInfo->ri_RelationDesc);

	/*
	 * Disable batching when we have to use RETURNING, there are any
	 * BEFORE/AFTER ROW INSERT triggers on the foreign table, or there are any
	 * WITH CHECK OPTION constraints from parent views.
	 *
	 * 当我们必须使用 RETURNING、外部表上有任何 BEFORE/AFTER ROW INSERT 触发器或来自父视图的任何 WITH CHECK OPTION 约束时，请禁用批处理。
	 *
	 * When there are any BEFORE ROW INSERT triggers on the table, we can't
	 * support it, because such triggers might query the table we're inserting
	 * into and act differently if the tuples that have already been processed
	 * and prepared for insertion are not there.
	 *
	 * 当表上存在任何 BEFORE ROW INSERT 触发器时，我们无法支持它，因为此类触发器可能会查询我们要插入的表，并且如果已处理并准备插入的元组不存在，则其行为会有所不同。
	 */
	if (resultRelInfo->ri_projectReturning != NULL ||
		resultRelInfo->ri_WithCheckOptions != NIL ||
		(resultRelInfo->ri_TrigDesc &&
		 (resultRelInfo->ri_TrigDesc->trig_insert_before_row ||
		  resultRelInfo->ri_TrigDesc->trig_insert_after_row)))
		return 1;

	/*
	 * If the foreign table has no columns, disable batching as the INSERT
	 * syntax doesn't allow batching multiple empty rows into a zero-column
	 * table in a single statement.  This is needed for COPY FROM, in which
	 * case fmstate must be non-NULL.
	 *
	 * 如果外表没有列，请禁用批处理，因为 INSERT 语法不允许在单个语句中将多个空行批处理到零列表中。  这是 COPY FROM 所必需的，在这种情况下，fmstate 必须为非 NULL。
	 */
	if (fmstate && list_length(fmstate->target_attrs) == 0)
		return 1;

	/*
	 * Otherwise use the batch size specified for server/table. The number of
	 * parameters in a batch is limited to 65535 (uint16), so make sure we
	 * don't exceed this limit by using the maximum batch_size possible.
	 *
	 * 否则使用为服务器/表指定的批量大小。批处理中的参数数量限制为 65535 (uint16)，因此请确保我们使用可能的最大batch_size 不超过此限制。
	 */
	if (fmstate && fmstate->p_nums > 0)
		batch_size = Min(batch_size, PQ_QUERY_PARAM_MAX_LIMIT / fmstate->p_nums);

	return batch_size;
}

/*
 * postgresExecForeignUpdate
 *		Update one row in a foreign table
 *
 * postgresExecForeignUpdate 更新外部表中的一行
 */
static TupleTableSlot *
postgresExecForeignUpdate(EState *estate,
						  ResultRelInfo *resultRelInfo,
						  TupleTableSlot *slot,
						  TupleTableSlot *planSlot)
{
	TupleTableSlot **rslot;
	int			numSlots = 1;

	rslot = execute_foreign_modify(estate, resultRelInfo, CMD_UPDATE,
								   &slot, &planSlot, &numSlots);

	return rslot ? rslot[0] : NULL;
}

/*
 * postgresExecForeignDelete
 *		Delete one row from a foreign table
 *
 * postgresExecForeignDelete 从外部表中删除一行
 */
static TupleTableSlot *
postgresExecForeignDelete(EState *estate,
						  ResultRelInfo *resultRelInfo,
						  TupleTableSlot *slot,
						  TupleTableSlot *planSlot)
{
	TupleTableSlot **rslot;
	int			numSlots = 1;

	rslot = execute_foreign_modify(estate, resultRelInfo, CMD_DELETE,
								   &slot, &planSlot, &numSlots);

	return rslot ? rslot[0] : NULL;
}

/*
 * postgresEndForeignModify
 *		Finish an insert/update/delete operation on a foreign table
 *
 * postgresEndForeignModify 完成外部表上的插入/更新/删除操作
 */
static void
postgresEndForeignModify(EState *estate,
						 ResultRelInfo *resultRelInfo)
{
	PgFdwModifyState *fmstate = (PgFdwModifyState *) resultRelInfo->ri_FdwState;

	/* If fmstate is NULL, we are in EXPLAIN; nothing to do
	 *
	 * 如果 fmstate 为 NULL，则处于 EXPLAIN 状态；无事可做
	 */
	if (fmstate == NULL)
		return;

	/* Destroy the execution state
	 *
	 * 销毁执行状态
	 */
	finish_foreign_modify(fmstate);
}

/*
 * postgresBeginForeignInsert
 *		Begin an insert operation on a foreign table
 *
 * postgresBeginForeignInsert 开始对外表进行插入操作
 */
static void
postgresBeginForeignInsert(ModifyTableState *mtstate,
						   ResultRelInfo *resultRelInfo)
{
	PgFdwModifyState *fmstate;
	ModifyTable *plan = castNode(ModifyTable, mtstate->ps.plan);
	EState	   *estate = mtstate->ps.state;
	Index		resultRelation;
	Relation	rel = resultRelInfo->ri_RelationDesc;
	RangeTblEntry *rte;
	TupleDesc	tupdesc = RelationGetDescr(rel);
	int			attnum;
	int			values_end_len;
	StringInfoData sql;
	List	   *targetAttrs = NIL;
	List	   *retrieved_attrs = NIL;
	bool		doNothing = false;

	/*
	 * If the foreign table we are about to insert routed rows into is also an
	 * UPDATE subplan result rel that will be updated later, proceeding with
	 * the INSERT will result in the later UPDATE incorrectly modifying those
	 * routed rows, so prevent the INSERT --- it would be nice if we could
	 * handle this case; but for now, throw an error for safety.
	 *
	 * 如果我们要插入路由行的外部表也是稍后将更新的 UPDATE 子计划结果 rel，则继续 INSERT 将导致稍后的 UPDATE 错误地修改这些路由行，因此请阻止 INSERT --- 如果我们能够处理这种情况，那就太好了；但现在，为了安全起见，请抛出一个错误。
	 */
	if (plan && plan->operation == CMD_UPDATE &&
		(resultRelInfo->ri_usesFdwDirectModify ||
		 resultRelInfo->ri_FdwState))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot route tuples into foreign table to be updated \"%s\"",
						RelationGetRelationName(rel))));

	initStringInfo(&sql);

	/* We transmit all columns that are defined in the foreign table.
	 *
	 * 我们传输外部表中定义的所有列。
	 */
	for (attnum = 1; attnum <= tupdesc->natts; attnum++)
	{
		CompactAttribute *attr = TupleDescCompactAttr(tupdesc, attnum - 1);

		if (!attr->attisdropped)
			targetAttrs = lappend_int(targetAttrs, attnum);
	}

	/* Check if we add the ON CONFLICT clause to the remote query.
	 *
	 * 检查我们是否将 ON CONFLICT 子句添加到远程查询中。
	 */
	if (plan)
	{
		OnConflictAction onConflictAction = plan->onConflictAction;

		/* We only support DO NOTHING without an inference specification.
		 *
		 * 我们只支持没有推理规范的 DO NOTHING。
		 */
		if (onConflictAction == ONCONFLICT_NOTHING)
			doNothing = true;
		else if (onConflictAction != ONCONFLICT_NONE)
			elog(ERROR, "unexpected ON CONFLICT specification: %d",
				 (int) onConflictAction);
	}

	/*
	 * If the foreign table is a partition that doesn't have a corresponding
	 * RTE entry, we need to create a new RTE describing the foreign table for
	 * use by deparseInsertSql and create_foreign_modify() below, after first
	 * copying the parent's RTE and modifying some fields to describe the
	 * foreign partition to work on. However, if this is invoked by UPDATE,
	 * the existing RTE may already correspond to this partition if it is one
	 * of the UPDATE subplan target rels; in that case, we can just use the
	 * existing RTE as-is.
	 *
	 * 如果外部表是没有相应 RTE 条目的分区，则在首先复制父级的 RTE 并修改一些字段来描述要处理的外部分区之后，我们需要创建一个新的 RTE 来描述外部表，以供下面的 deparseInsertSql 和 create_foreign_modify() 使用。但是，如果这是由 UPDATE 调用的，则现有 RTE 可能已经对应于该分区（如果它是 UPDATE 子计划目标关系之一）；在这种情况下，我们可以按原样使用现有的 RTE。
	 */
	if (resultRelInfo->ri_RangeTableIndex == 0)
	{
		ResultRelInfo *rootResultRelInfo = resultRelInfo->ri_RootResultRelInfo;

		rte = exec_rt_fetch(rootResultRelInfo->ri_RangeTableIndex, estate);
		rte = copyObject(rte);
		rte->relid = RelationGetRelid(rel);
		rte->relkind = RELKIND_FOREIGN_TABLE;

		/*
		 * For UPDATE, we must use the RT index of the first subplan target
		 * rel's RTE, because the core code would have built expressions for
		 * the partition, such as RETURNING, using that RT index as varno of
		 * Vars contained in those expressions.
		 *
		 * 对于 UPDATE，我们必须使用第一个子计划目标 rel 的 RTE 的 RT 索引，因为核心代码将为分区构建表达式，例如 RETURNING，使用该 RT 索引作为这些表达式中包含的 Vars 的 varno。
		 */
		if (plan && plan->operation == CMD_UPDATE &&
			rootResultRelInfo->ri_RangeTableIndex == plan->rootRelation)
			resultRelation = mtstate->resultRelInfo[0].ri_RangeTableIndex;
		else
			resultRelation = rootResultRelInfo->ri_RangeTableIndex;
	}
	else
	{
		resultRelation = resultRelInfo->ri_RangeTableIndex;
		rte = exec_rt_fetch(resultRelation, estate);
	}

	/* Construct the SQL command string.
	 *
	 * 构造 SQL 命令字符串。
	 */
	deparseInsertSql(&sql, rte, resultRelation, rel, targetAttrs, doNothing,
					 resultRelInfo->ri_WithCheckOptions,
					 resultRelInfo->ri_returningList,
					 &retrieved_attrs, &values_end_len);

	/* Construct an execution state.
	 *
	 * 构造一个执行状态。
	 */
	fmstate = create_foreign_modify(mtstate->ps.state,
									rte,
									resultRelInfo,
									CMD_INSERT,
									NULL,
									sql.data,
									targetAttrs,
									values_end_len,
									retrieved_attrs != NIL,
									retrieved_attrs);

	/*
	 * If the given resultRelInfo already has PgFdwModifyState set, it means
	 * the foreign table is an UPDATE subplan result rel; in which case, store
	 * the resulting state into the aux_fmstate of the PgFdwModifyState.
	 *
	 * 如果给定的resultRelInfo已经设置了PgFdwModifyState，则意味着外表是一个UPDATE子计划结果rel；在这种情况下，将结果状态存储到 PgFdwModifyState 的 aux_fmstate 中。
	 */
	if (resultRelInfo->ri_FdwState)
	{
		Assert(plan && plan->operation == CMD_UPDATE);
		Assert(resultRelInfo->ri_usesFdwDirectModify == false);
		((PgFdwModifyState *) resultRelInfo->ri_FdwState)->aux_fmstate = fmstate;
	}
	else
		resultRelInfo->ri_FdwState = fmstate;
}

/*
 * postgresEndForeignInsert
 *		Finish an insert operation on a foreign table
 *
 * postgresEndForeignInsert 完成外表的插入操作
 */
static void
postgresEndForeignInsert(EState *estate,
						 ResultRelInfo *resultRelInfo)
{
	PgFdwModifyState *fmstate = (PgFdwModifyState *) resultRelInfo->ri_FdwState;

	Assert(fmstate != NULL);

	/*
	 * If the fmstate has aux_fmstate set, get the aux_fmstate (see
	 * postgresBeginForeignInsert())
	 *
	 * 如果 fmstate 设置了 aux_fmstate，则获取 aux_fmstate（请参阅 postgresBeginForeignInsert()）
	 */
	if (fmstate->aux_fmstate)
		fmstate = fmstate->aux_fmstate;

	/* Destroy the execution state
	 *
	 * 销毁执行状态
	 */
	finish_foreign_modify(fmstate);
}

/*
 * postgresIsForeignRelUpdatable
 *		Determine whether a foreign table supports INSERT, UPDATE and/or
 *		DELETE.
 *
 * postgresIsForeignRelUpdatable 确定外部表是否支持 INSERT、UPDATE 和/或 DELETE。
 */
static int
postgresIsForeignRelUpdatable(Relation rel)
{
	bool		updatable;
	ForeignTable *table;
	ForeignServer *server;
	ListCell   *lc;

	/*
	 * By default, all postgres_fdw foreign tables are assumed updatable. This
	 * can be overridden by a per-server setting, which in turn can be
	 * overridden by a per-table setting.
	 *
	 * 默认情况下，所有 postgres_fdw 外部表都被假定为可更新。这可以被每服务器设置覆盖，而每服务器设置又可以被每表设置覆盖。
	 */
	updatable = true;

	table = GetForeignTable(RelationGetRelid(rel));
	server = GetForeignServer(table->serverid);

	foreach(lc, server->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "updatable") == 0)
			updatable = defGetBoolean(def);
	}
	foreach(lc, table->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "updatable") == 0)
			updatable = defGetBoolean(def);
	}

	/*
	 * Currently "updatable" means support for INSERT, UPDATE and DELETE.
	 *
	 * 目前“可更新”意味着支持 INSERT、UPDATE 和 DELETE。
	 */
	return updatable ?
		(1 << CMD_INSERT) | (1 << CMD_UPDATE) | (1 << CMD_DELETE) : 0;
}

/*
 * postgresRecheckForeignScan
 *		Execute a local join execution plan for a foreign join
 *
 * postgresRecheckForeignScan 对外部连接执行本地连接执行计划
 */
static bool
postgresRecheckForeignScan(ForeignScanState *node, TupleTableSlot *slot)
{
	Index		scanrelid = ((Scan *) node->ss.ps.plan)->scanrelid;
	PlanState  *outerPlan = outerPlanState(node);
	TupleTableSlot *result;

	/* For base foreign relations, it suffices to set fdw_recheck_quals
	 *
	 * 对于基础外交关系，设置 fdw_recheck_quals 就足够了
	 */
	if (scanrelid > 0)
		return true;

	Assert(outerPlan != NULL);

	/* Execute a local join execution plan
	 *
	 * 执行本地连接执行计划
	 */
	result = ExecProcNode(outerPlan);
	if (TupIsNull(result))
		return false;

	/* Store result in the given slot
	 *
	 * 将结果存储在给定的槽中
	 */
	ExecCopySlot(slot, result);

	return true;
}

/*
 * find_modifytable_subplan
 *		Helper routine for postgresPlanDirectModify to find the
 *		ModifyTable subplan node that scans the specified RTI.
 *
 * find_modifytable_subplan postgresPlanDirectModify 的辅助例程，用于查找扫描指定 RTI 的 ModifyTable 子计划节点。
 *
 * Returns NULL if the subplan couldn't be identified.  That's not a fatal
 * error condition, we just abandon trying to do the update directly.
 *
 * 如果无法识别子计划，则返回 NULL。  这不是致命错误情况，我们只是放弃尝试直接进行更新。
 */
static ForeignScan *
find_modifytable_subplan(PlannerInfo *root,
						 ModifyTable *plan,
						 Index rtindex,
						 int subplan_index)
{
	Plan	   *subplan = outerPlan(plan);

	/*
	 * The cases we support are (1) the desired ForeignScan is the immediate
	 * child of ModifyTable, or (2) it is the subplan_index'th child of an
	 * Append node that is the immediate child of ModifyTable.  There is no
	 * point in looking further down, as that would mean that local joins are
	 * involved, so we can't do the update directly.
	 *
	 * 我们支持的情况是（1）所需的ForeignScan是ModifyTable的直接子级，或者（2）它是Append节点的第subplan_index子级，而Append节点是ModifyTable的直接子级。  继续往下看是没有意义的，因为这意味着涉及到本地连接，所以我们不能直接进行更新。
	 *
	 * There could be a Result atop the Append too, acting to compute the
	 * UPDATE targetlist values.  We ignore that here; the tlist will be
	 * checked by our caller.
	 *
	 * 追加顶部也可能有一个结果，用于计算更新目标列表值。  我们在这里忽略这一点；我们的调用者将检查列表。
	 *
	 * In principle we could examine all the children of the Append, but it's
	 * currently unlikely that the core planner would generate such a plan
	 * with the children out-of-order.  Moreover, such a search risks costing
	 * O(N^2) time when there are a lot of children.
	 *
	 * 原则上，我们可以检查 Append 的所有子项，但目前核心规划器不太可能生成这样一个子项无序的计划。  此外，当有很多孩子时，这样的搜索可能会花费 O(N^2) 时间。
	 */
	if (IsA(subplan, Append))
	{
		Append	   *appendplan = (Append *) subplan;

		if (subplan_index < list_length(appendplan->appendplans))
			subplan = (Plan *) list_nth(appendplan->appendplans, subplan_index);
	}
	else if (IsA(subplan, Result) &&
			 outerPlan(subplan) != NULL &&
			 IsA(outerPlan(subplan), Append))
	{
		Append	   *appendplan = (Append *) outerPlan(subplan);

		if (subplan_index < list_length(appendplan->appendplans))
			subplan = (Plan *) list_nth(appendplan->appendplans, subplan_index);
	}

	/* Now, have we got a ForeignScan on the desired rel?
	 *
	 * 现在，我们已经获得了所需相关的ForeignScan 了吗？
	 */
	if (IsA(subplan, ForeignScan))
	{
		ForeignScan *fscan = (ForeignScan *) subplan;

		if (bms_is_member(rtindex, fscan->fs_base_relids))
			return fscan;
	}

	return NULL;
}

/*
 * postgresPlanDirectModify
 *		Consider a direct foreign table modification
 *
 * postgresPlanDirectModify 考虑直接外部表修改
 *
 * Decide whether it is safe to modify a foreign table directly, and if so,
 * rewrite subplan accordingly.
 *
 * 确定直接修改外表是否安全，如果是，则相应地重写子计划。
 */
static bool
postgresPlanDirectModify(PlannerInfo *root,
						 ModifyTable *plan,
						 Index resultRelation,
						 int subplan_index)
{
	CmdType		operation = plan->operation;
	RelOptInfo *foreignrel;
	RangeTblEntry *rte;
	PgFdwRelationInfo *fpinfo;
	Relation	rel;
	StringInfoData sql;
	ForeignScan *fscan;
	List	   *processed_tlist = NIL;
	List	   *targetAttrs = NIL;
	List	   *remote_exprs;
	List	   *params_list = NIL;
	List	   *returningList = NIL;
	List	   *retrieved_attrs = NIL;

	/*
	 * Decide whether it is safe to modify a foreign table directly.
	 *
	 * 确定直接修改外表是否安全。
	 */

	/*
	 * The table modification must be an UPDATE or DELETE.
	 *
	 * 表修改必须是 UPDATE 或 DELETE。
	 */
	if (operation != CMD_UPDATE && operation != CMD_DELETE)
		return false;

	/*
	 * Try to locate the ForeignScan subplan that's scanning resultRelation.
	 *
	 * 尝试找到正在扫描 resultRelation 的ForeignScan 子计划。
	 */
	fscan = find_modifytable_subplan(root, plan, resultRelation, subplan_index);
	if (!fscan)
		return false;

	/*
	 * It's unsafe to modify a foreign table directly if there are any quals
	 * that should be evaluated locally.
	 *
	 * 如果存在任何应在本地评估的限定，则直接修改外部表是不安全的。
	 */
	if (fscan->scan.plan.qual != NIL)
		return false;

	/* Safe to fetch data about the target foreign rel
	 *
	 * 安全地获取有关目标外部关系的数据
	 */
	if (fscan->scan.scanrelid == 0)
	{
		foreignrel = find_join_rel(root, fscan->fs_relids);
		/* We should have a rel for this foreign join.
		 *
		 * 我们应该有一个与此外国连接相关的关系。
		 */
		Assert(foreignrel);
	}
	else
		foreignrel = root->simple_rel_array[resultRelation];
	rte = root->simple_rte_array[resultRelation];
	fpinfo = (PgFdwRelationInfo *) foreignrel->fdw_private;

	/*
	 * It's unsafe to update a foreign table directly, if any expressions to
	 * assign to the target columns are unsafe to evaluate remotely.
	 *
	 * 如果分配给目标列的任何表达式远程计算不安全，那么直接更新外表是不安全的。
	 */
	if (operation == CMD_UPDATE)
	{
		ListCell   *lc,
				   *lc2;

		/*
		 * The expressions of concern are the first N columns of the processed
		 * targetlist, where N is the length of the rel's update_colnos.
		 *
		 * 关注的表达式是已处理目标列表的前 N ​​列，其中 N 是 rel 的 update_colnos 的长度。
		 */
		get_translated_update_targetlist(root, resultRelation,
										 &processed_tlist, &targetAttrs);
		forboth(lc, processed_tlist, lc2, targetAttrs)
		{
			TargetEntry *tle = lfirst_node(TargetEntry, lc);
			AttrNumber	attno = lfirst_int(lc2);

			/* update's new-value expressions shouldn't be resjunk
			 *
			 * update 的新值表达式不应该被 resjunk
			 */
			Assert(!tle->resjunk);

			if (attno <= InvalidAttrNumber) /* shouldn't happen */
				elog(ERROR, "system-column update is not supported");

			if (!is_foreign_expr(root, foreignrel, (Expr *) tle->expr))
				return false;
		}
	}

	/*
	 * Ok, rewrite subplan so as to modify the foreign table directly.
	 *
	 * 好的，重写subplan，直接修改外表。
	 */
	initStringInfo(&sql);

	/*
	 * Core code already has some lock on each rel being planned, so we can
	 * use NoLock here.
	 *
	 * 核心代码已经对正在规划的每个rel进行了一些锁定，因此我们可以在这里使用NoLock。
	 */
	rel = table_open(rte->relid, NoLock);

	/*
	 * Recall the qual clauses that must be evaluated remotely.  (These are
	 * bare clauses not RestrictInfos, but deparse.c's appendConditions()
	 * doesn't care.)
	 *
	 * 回想一下必须远程评估的限定条款。  （这些是裸子句，不是 RestrictInfos，但 deparse.c 的appendConditions() 并不关心。）
	 */
	remote_exprs = fpinfo->final_remote_exprs;

	/*
	 * Extract the relevant RETURNING list if any.
	 *
	 * 提取相关的返回列表（如果有）。
	 */
	if (plan->returningLists)
	{
		returningList = (List *) list_nth(plan->returningLists, subplan_index);

		/*
		 * When performing an UPDATE/DELETE .. RETURNING on a join directly,
		 * we fetch from the foreign server any Vars specified in RETURNING
		 * that refer not only to the target relation but to non-target
		 * relations.  So we'll deparse them into the RETURNING clause of the
		 * remote query; use a targetlist consisting of them instead, which
		 * will be adjusted to be new fdw_scan_tlist of the foreign-scan plan
		 * node below.
		 *
		 * 当直接在连接上执行 UPDATE/DELETE .. RETURNING 时，我们从外部服务器获取 RETURNING 中指定的任何变量，这些变量不仅引用目标关系，而且引用非目标关系。  因此，我们将它们解析为远程查询的 RETURNING 子句；使用由它们组成的targetlist来代替，它将被调整为下面的foreign-scan计划节点的新fdw_scan_tlist。
		 */
		if (fscan->scan.scanrelid == 0)
			returningList = build_remote_returning(resultRelation, rel,
												   returningList);
	}

	/*
	 * Construct the SQL command string.
	 *
	 * 构造 SQL 命令字符串。
	 */
	switch (operation)
	{
		case CMD_UPDATE:
			deparseDirectUpdateSql(&sql, root, resultRelation, rel,
								   foreignrel,
								   processed_tlist,
								   targetAttrs,
								   remote_exprs, &params_list,
								   returningList, &retrieved_attrs);
			break;
		case CMD_DELETE:
			deparseDirectDeleteSql(&sql, root, resultRelation, rel,
								   foreignrel,
								   remote_exprs, &params_list,
								   returningList, &retrieved_attrs);
			break;
		default:
			elog(ERROR, "unexpected operation: %d", (int) operation);
			break;
	}

	/*
	 * Update the operation and target relation info.
	 *
	 * 更新操作和目标关系信息。
	 */
	fscan->operation = operation;
	fscan->resultRelation = resultRelation;

	/*
	 * Update the fdw_exprs list that will be available to the executor.
	 *
	 * 更新可供执行程序使用的 fdw_exprs 列表。
	 */
	fscan->fdw_exprs = params_list;

	/*
	 * Update the fdw_private list that will be available to the executor.
	 * Items in the list must match enum FdwDirectModifyPrivateIndex, above.
	 *
	 * 更新可供执行程序使用的 fdw_private 列表。列表中的项目必须与上面的枚举 FdwDirectModifyPrivateIndex 匹配。
	 */
	fscan->fdw_private = list_make4(makeString(sql.data),
									makeBoolean((retrieved_attrs != NIL)),
									retrieved_attrs,
									makeBoolean(plan->canSetTag));

	/*
	 * Update the foreign-join-related fields.
	 *
	 * 更新foreign-join相关字段。
	 */
	if (fscan->scan.scanrelid == 0)
	{
		/* No need for the outer subplan.
		 *
		 * 不需要外部子计划。
		 */
		fscan->scan.plan.lefttree = NULL;

		/* Build new fdw_scan_tlist if UPDATE/DELETE .. RETURNING.
		 *
		 * 如果更新/删除..正在返回，则构建新的 fdw_scan_tlist。
		 */
		if (returningList)
			rebuild_fdw_scan_tlist(fscan, returningList);
	}

	/*
	 * Finally, unset the async-capable flag if it is set, as we currently
	 * don't support asynchronous execution of direct modifications.
	 *
	 * 最后，如果设置了异步功能标志，请将其取消设置，因为我们目前不支持直接修改的异步执行。
	 */
	if (fscan->scan.plan.async_capable)
		fscan->scan.plan.async_capable = false;

	table_close(rel, NoLock);
	return true;
}

/*
 * postgresBeginDirectModify
 *		Prepare a direct foreign table modification
 *
 * postgresBeginDirectModify 准备直接外表修改
 */
static void
postgresBeginDirectModify(ForeignScanState *node, int eflags)
{
	ForeignScan *fsplan = (ForeignScan *) node->ss.ps.plan;
	EState	   *estate = node->ss.ps.state;
	PgFdwDirectModifyState *dmstate;
	Index		rtindex;
	Oid			userid;
	ForeignTable *table;
	UserMapping *user;
	int			numParams;

	/*
	 * Do nothing in EXPLAIN (no ANALYZE) case.  node->fdw_state stays NULL.
	 *
	 * 在 EXPLAIN（无 ANALYZE）情况下不执行任何操作。  node->fdw_state 保持 NULL。
	 */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	/*
	 * We'll save private state in node->fdw_state.
	 *
	 * 我们将把私有状态保存在node->fdw_state中。
	 */
	dmstate = (PgFdwDirectModifyState *) palloc0(sizeof(PgFdwDirectModifyState));
	node->fdw_state = dmstate;

	/*
	 * We use a memory context callback to ensure that the dmstate's PGresult
	 * (if any) will be released, even if the query fails somewhere that's
	 * outside our control.  The callback is always armed for the duration of
	 * the query; this relies on PQclear(NULL) being a no-op.
	 *
	 * 我们使用内存上下文回调来确保 dmstate 的 PGresult（如果有）将被释放，即使查询在我们无法控制的地方失败也是如此。  在查询期间回调始终处于准备状态；这依赖于 PQclear(NULL) 是无操作。
	 */
	dmstate->result_cb.func = (MemoryContextCallbackFunction) PQclear;
	dmstate->result_cb.arg = NULL;
	MemoryContextRegisterResetCallback(CurrentMemoryContext,
									   &dmstate->result_cb);

	/*
	 * Identify which user to do the remote access as.  This should match what
	 * ExecCheckPermissions() does.
	 *
	 * 确定以哪个用户身份进行远程访问。  这应该与 ExecCheckPermissions() 的作用相匹配。
	 */
	userid = OidIsValid(fsplan->checkAsUser) ? fsplan->checkAsUser : GetUserId();

	/* Get info about foreign table.
	 *
	 * 获取有关外部表的信息。
	 */
	rtindex = node->resultRelInfo->ri_RangeTableIndex;
	if (fsplan->scan.scanrelid == 0)
		dmstate->rel = ExecOpenScanRelation(estate, rtindex, eflags);
	else
		dmstate->rel = node->ss.ss_currentRelation;
	table = GetForeignTable(RelationGetRelid(dmstate->rel));
	user = GetUserMapping(userid, table->serverid);

	/*
	 * Get connection to the foreign server.  Connection manager will
	 * establish new connection if necessary.
	 *
	 * 获取与外部服务器的连接。  如果需要，连接管理器将建立新连接。
	 */
	dmstate->conn = GetConnection(user, false, &dmstate->conn_state);

	/* Update the foreign-join-related fields.
	 *
	 * 更新foreign-join相关字段。
	 */
	if (fsplan->scan.scanrelid == 0)
	{
		/* Save info about foreign table.
		 *
		 * 保存有关外部表的信息。
		 */
		dmstate->resultRel = dmstate->rel;

		/*
		 * Set dmstate->rel to NULL to teach get_returning_data() and
		 * make_tuple_from_result_row() that columns fetched from the remote
		 * server are described by fdw_scan_tlist of the foreign-scan plan
		 * node, not the tuple descriptor for the target relation.
		 *
		 * 将 dmstate->rel 设置为 NULL，以告知 get_returning_data() 和 make_tuple_from_result_row() 从远程服务器获取的列由外部扫描计划节点的 fdw_scan_tlist 描述，而不是目标关系的元组描述符。
		 */
		dmstate->rel = NULL;
	}

	/* Initialize state variable
	 *
	 * 初始化状态变量
	 */
	dmstate->num_tuples = -1;	/* -1 means not set yet */

	/* Get private info created by planner functions.
	 *
	 * 获取规划器功能创建的私人信息。
	 */
	dmstate->query = strVal(list_nth(fsplan->fdw_private,
									 FdwDirectModifyPrivateUpdateSql));
	dmstate->has_returning = boolVal(list_nth(fsplan->fdw_private,
											  FdwDirectModifyPrivateHasReturning));
	dmstate->retrieved_attrs = (List *) list_nth(fsplan->fdw_private,
												 FdwDirectModifyPrivateRetrievedAttrs);
	dmstate->set_processed = boolVal(list_nth(fsplan->fdw_private,
											  FdwDirectModifyPrivateSetProcessed));

	/* Create context for per-tuple temp workspace.
	 *
	 * 为每个元组临时工作区创建上下文。
	 */
	dmstate->temp_cxt = AllocSetContextCreate(estate->es_query_cxt,
											  "postgres_fdw temporary data",
											  ALLOCSET_SMALL_SIZES);

	/* Prepare for input conversion of RETURNING results.
	 *
	 * 准备返回结果的输入转换。
	 */
	if (dmstate->has_returning)
	{
		TupleDesc	tupdesc;

		if (fsplan->scan.scanrelid == 0)
			tupdesc = get_tupdesc_for_join_scan_tuples(node);
		else
			tupdesc = RelationGetDescr(dmstate->rel);

		dmstate->attinmeta = TupleDescGetAttInMetadata(tupdesc);

		/*
		 * When performing an UPDATE/DELETE .. RETURNING on a join directly,
		 * initialize a filter to extract an updated/deleted tuple from a scan
		 * tuple.
		 *
		 * 当直接对连接执行 UPDATE/DELETE .. RETURNING 时，初始化过滤器以从扫描元组中提取更新/删除的元组。
		 */
		if (fsplan->scan.scanrelid == 0)
			init_returning_filter(dmstate, fsplan->fdw_scan_tlist, rtindex);
	}

	/*
	 * Prepare for processing of parameters used in remote query, if any.
	 *
	 * 准备处理远程查询中使用的参数（如果有）。
	 */
	numParams = list_length(fsplan->fdw_exprs);
	dmstate->numParams = numParams;
	if (numParams > 0)
		prepare_query_params((PlanState *) node,
							 fsplan->fdw_exprs,
							 numParams,
							 &dmstate->param_flinfo,
							 &dmstate->param_exprs,
							 &dmstate->param_values);
}

/*
 * postgresIterateDirectModify
 *		Execute a direct foreign table modification
 *
 * postgresIterateDirectModify 执行直接外表修改
 */
static TupleTableSlot *
postgresIterateDirectModify(ForeignScanState *node)
{
	PgFdwDirectModifyState *dmstate = (PgFdwDirectModifyState *) node->fdw_state;
	EState	   *estate = node->ss.ps.state;
	ResultRelInfo *resultRelInfo = node->resultRelInfo;

	/*
	 * If this is the first call after Begin, execute the statement.
	 *
	 * 如果这是 Begin 之后的第一次调用，则执行该语句。
	 */
	if (dmstate->num_tuples == -1)
		execute_dml_stmt(node);

	/*
	 * If the local query doesn't specify RETURNING, just clear tuple slot.
	 *
	 * 如果本地查询没有指定RETURNING，则只需清除元组槽即可。
	 */
	if (!resultRelInfo->ri_projectReturning)
	{
		TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
		Instrumentation *instr = node->ss.ps.instrument;

		Assert(!dmstate->has_returning);

		/* Increment the command es_processed count if necessary.
		 *
		 * 如有必要，增加命令 es_processed 计数。
		 */
		if (dmstate->set_processed)
			estate->es_processed += dmstate->num_tuples;

		/* Increment the tuple count for EXPLAIN ANALYZE if necessary.
		 *
		 * 如有必要，增加 EXPLAIN ANALYZE 的元组计数。
		 */
		if (instr)
			instr->tuplecount += dmstate->num_tuples;

		return ExecClearTuple(slot);
	}

	/*
	 * Get the next RETURNING tuple.
	 *
	 * 获取下一个返回元组。
	 */
	return get_returning_data(node);
}

/*
 * postgresEndDirectModify
 *		Finish a direct foreign table modification
 *
 * postgresEndDirectModify 完成直接外表修改
 */
static void
postgresEndDirectModify(ForeignScanState *node)
{
	PgFdwDirectModifyState *dmstate = (PgFdwDirectModifyState *) node->fdw_state;

	/* if dmstate is NULL, we are in EXPLAIN; nothing to do
	 *
	 * 如果 dmstate 为 NULL，则处于 EXPLAIN 状态；无事可做
	 */
	if (dmstate == NULL)
		return;

	/* Release PGresult
	 *
	 * 发布PG结果
	 */
	if (dmstate->result)
	{
		PQclear(dmstate->result);
		dmstate->result = NULL;
		/* ... and don't forget to disable the callback
		 *
		 * ...并且不要忘记禁用回调
		 */
		dmstate->result_cb.arg = NULL;
	}

	/* Release remote connection
	 *
	 * 释放远程连接
	 */
	ReleaseConnection(dmstate->conn);
	dmstate->conn = NULL;

	/* MemoryContext will be deleted automatically.
	 *
	 * MemoryContext 将被自动删除。
	 */
}

/*
 * postgresExplainForeignScan
 *		Produce extra output for EXPLAIN of a ForeignScan on a foreign table
 *
 * postgresExplainForeignScan 为外部表上的ForeignScan 的EXPLAIN 生成额外的输出
 */
static void
postgresExplainForeignScan(ForeignScanState *node, ExplainState *es)
{
	ForeignScan *plan = castNode(ForeignScan, node->ss.ps.plan);
	List	   *fdw_private = plan->fdw_private;

	/*
	 * Identify foreign scans that are really joins or upper relations.  The
	 * input looks something like "(1) LEFT JOIN (2)", and we must replace the
	 * digit string(s), which are RT indexes, with the correct relation names.
	 * We do that here, not when the plan is created, because we can't know
	 * what aliases ruleutils.c will assign at plan creation time.
	 *
	 * 识别真正的连接或上层关系的外部扫描。  输入看起来像“(1) LEFT JOIN (2)”，我们必须用正确的关系名称替换数字字符串（RT 索引）。我们在这里执行此操作，而不是在创建计划时执行此操作，因为我们无法知道ruleutils.c 在计划创建时将分配哪些别名。
	 */
	if (list_length(fdw_private) > FdwScanPrivateRelations)
	{
		StringInfo	relations;
		char	   *rawrelations;
		char	   *ptr;
		int			minrti,
					rtoffset;

		rawrelations = strVal(list_nth(fdw_private, FdwScanPrivateRelations));

		/*
		 * A difficulty with using a string representation of RT indexes is
		 * that setrefs.c won't update the string when flattening the
		 * rangetable.  To find out what rtoffset was applied, identify the
		 * minimum RT index appearing in the string and compare it to the
		 * minimum member of plan->fs_base_relids.  (We expect all the relids
		 * in the join will have been offset by the same amount; the Asserts
		 * below should catch it if that ever changes.)
		 *
		 * 使用 RT 索引的字符串表示形式的一个困难是 setrefs.c 在展平范围表时不会更新字符串。  要找出应用的 rtoffset，请识别字符串中出现的最小 RT 索引，并将其与 plan->fs_base_relids 的最小成员进行比较。  （我们期望连接中的所有 relids 都将偏移相同的量；如果这种情况发生变化，下面的断言应该捕获它。）
		 */
		minrti = INT_MAX;
		ptr = rawrelations;
		while (*ptr)
		{
			if (isdigit((unsigned char) *ptr))
			{
				int			rti = strtol(ptr, &ptr, 10);

				if (rti < minrti)
					minrti = rti;
			}
			else
				ptr++;
		}
		rtoffset = bms_next_member(plan->fs_base_relids, -1) - minrti;

		/* Now we can translate the string
		 *
		 * 现在我们可以翻译字符串
		 */
		relations = makeStringInfo();
		ptr = rawrelations;
		while (*ptr)
		{
			if (isdigit((unsigned char) *ptr))
			{
				int			rti = strtol(ptr, &ptr, 10);
				RangeTblEntry *rte;
				char	   *relname;
				char	   *refname;

				rti += rtoffset;
				Assert(bms_is_member(rti, plan->fs_base_relids));
				rte = rt_fetch(rti, es->rtable);
				Assert(rte->rtekind == RTE_RELATION);
				/* This logic should agree with explain.c's ExplainTargetRel
				 *
				 * 这个逻辑应该与explain.c的ExplainTargetRel一致
				 */
				relname = get_rel_name(rte->relid);
				if (es->verbose)
				{
					char	   *namespace;

					namespace = get_namespace_name_or_temp(get_rel_namespace(rte->relid));
					appendStringInfo(relations, "%s.%s",
									 quote_identifier(namespace),
									 quote_identifier(relname));
				}
				else
					appendStringInfoString(relations,
										   quote_identifier(relname));
				refname = (char *) list_nth(es->rtable_names, rti - 1);
				if (refname == NULL)
					refname = rte->eref->aliasname;
				if (strcmp(refname, relname) != 0)
					appendStringInfo(relations, " %s",
									 quote_identifier(refname));
			}
			else
				appendStringInfoChar(relations, *ptr++);
		}
		ExplainPropertyText("Relations", relations->data, es);
	}

	/*
	 * Add remote query, when VERBOSE option is specified.
	 *
	 * 当指定 VERBOSE 选项时，添加远程查询。
	 */
	if (es->verbose)
	{
		char	   *sql;

		sql = strVal(list_nth(fdw_private, FdwScanPrivateSelectSql));
		ExplainPropertyText("Remote SQL", sql, es);
	}
}

/*
 * postgresExplainForeignModify
 *		Produce extra output for EXPLAIN of a ModifyTable on a foreign table
 *
 * postgresExplainForeignModify 为外部表上的ModifyTable的EXPLAIN生成额外的输出
 */
static void
postgresExplainForeignModify(ModifyTableState *mtstate,
							 ResultRelInfo *rinfo,
							 List *fdw_private,
							 int subplan_index,
							 ExplainState *es)
{
	if (es->verbose)
	{
		char	   *sql = strVal(list_nth(fdw_private,
										  FdwModifyPrivateUpdateSql));

		ExplainPropertyText("Remote SQL", sql, es);

		/*
		 * For INSERT we should always have batch size >= 1, but UPDATE and
		 * DELETE don't support batching so don't show the property.
		 *
		 * 对于 INSERT，我们应该始终使批处理大小 >= 1，但 UPDATE 和 DELETE 不支持批处理，因此不显示该属性。
		 */
		if (rinfo->ri_BatchSize > 0)
			ExplainPropertyInteger("Batch Size", NULL, rinfo->ri_BatchSize, es);
	}
}

/*
 * postgresExplainDirectModify
 *		Produce extra output for EXPLAIN of a ForeignScan that modifies a
 *		foreign table directly
 *
 * postgresExplainDirectModify 为直接修改外表的ForeignScan的EXPLAIN生成额外的输出
 */
static void
postgresExplainDirectModify(ForeignScanState *node, ExplainState *es)
{
	List	   *fdw_private;
	char	   *sql;

	if (es->verbose)
	{
		fdw_private = ((ForeignScan *) node->ss.ps.plan)->fdw_private;
		sql = strVal(list_nth(fdw_private, FdwDirectModifyPrivateUpdateSql));
		ExplainPropertyText("Remote SQL", sql, es);
	}
}

/*
 * postgresExecForeignTruncate
 *		Truncate one or more foreign tables
 *
 * postgresExecForeignTruncate 截断一个或多个外部表
 */
static void
postgresExecForeignTruncate(List *rels,
							DropBehavior behavior,
							bool restart_seqs)
{
	Oid			serverid = InvalidOid;
	UserMapping *user = NULL;
	PGconn	   *conn = NULL;
	StringInfoData sql;
	ListCell   *lc;
	bool		server_truncatable = true;

	/*
	 * By default, all postgres_fdw foreign tables are assumed truncatable.
	 * This can be overridden by a per-server setting, which in turn can be
	 * overridden by a per-table setting.
	 *
	 * 默认情况下，所有 postgres_fdw 外部表都被假定为可截断。这可以被每服务器设置覆盖，而每服务器设置又可以被每表设置覆盖。
	 */
	foreach(lc, rels)
	{
		ForeignServer *server = NULL;
		Relation	rel = lfirst(lc);
		ForeignTable *table = GetForeignTable(RelationGetRelid(rel));
		ListCell   *cell;
		bool		truncatable;

		/*
		 * First time through, determine whether the foreign server allows
		 * truncates. Since all specified foreign tables are assumed to belong
		 * to the same foreign server, this result can be used for other
		 * foreign tables.
		 *
		 * 第一次通过，判断外部服务器是否允许截断。由于假定所有指定的外部表都属于同一外部服务器，因此该结果可用于其他外部表。
		 */
		if (!OidIsValid(serverid))
		{
			serverid = table->serverid;
			server = GetForeignServer(serverid);

			foreach(cell, server->options)
			{
				DefElem    *defel = (DefElem *) lfirst(cell);

				if (strcmp(defel->defname, "truncatable") == 0)
				{
					server_truncatable = defGetBoolean(defel);
					break;
				}
			}
		}

		/*
		 * Confirm that all specified foreign tables belong to the same
		 * foreign server.
		 *
		 * 确认所有指定的外部表都属于同一外部服务器。
		 */
		Assert(table->serverid == serverid);

		/* Determine whether this foreign table allows truncations
		 *
		 * 确定该外表是否允许截断
		 */
		truncatable = server_truncatable;
		foreach(cell, table->options)
		{
			DefElem    *defel = (DefElem *) lfirst(cell);

			if (strcmp(defel->defname, "truncatable") == 0)
			{
				truncatable = defGetBoolean(defel);
				break;
			}
		}

		if (!truncatable)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("foreign table \"%s\" does not allow truncates",
							RelationGetRelationName(rel))));
	}
	Assert(OidIsValid(serverid));

	/*
	 * Get connection to the foreign server.  Connection manager will
	 * establish new connection if necessary.
	 *
	 * 获取与外部服务器的连接。  如果需要，连接管理器将建立新连接。
	 */
	user = GetUserMapping(GetUserId(), serverid);
	conn = GetConnection(user, false, NULL);

	/* Construct the TRUNCATE command string
	 *
	 * 构造 TRUNCATE 命令字符串
	 */
	initStringInfo(&sql);
	deparseTruncateSql(&sql, rels, behavior, restart_seqs);

	/* Issue the TRUNCATE command to remote server
	 *
	 * 向远程服务器发出 TRUNCATE 命令
	 */
	do_sql_command(conn, sql.data);

	pfree(sql.data);
}

/*
 * estimate_path_cost_size
 *		Get cost and size estimates for a foreign scan on given foreign relation
 *		either a base relation or a join between foreign relations or an upper
 *		relation containing foreign relations.
 *
 * estimate_path_cost_size 获取给定外部关系（基本关系或外部关系之间的联接或包含外部关系的上层关系）上的外部扫描的成本和大小估计。
 *
 * param_join_conds are the parameterization clauses with outer relations.
 * pathkeys specify the expected sort order if any for given path being costed.
 * fpextra specifies additional post-scan/join-processing steps such as the
 * final sort and the LIMIT restriction.
 *
 * param_join_conds 是具有外部关系的参数化子句。路径键指定正在计算成本的给定路径的预期排序顺序（如果有）。 fpextra 指定额外的后扫描/连接处理步骤，例如最终排序和 LIMIT 限制。
 *
 * The function returns the cost and size estimates in p_rows, p_width,
 * p_disabled_nodes, p_startup_cost and p_total_cost variables.
 *
 * 该函数返回 p_rows、p_width、p_disabled_nodes、p_startup_cost 和 p_total_cost 变量中的成本和大小估计值。
 */
static void
estimate_path_cost_size(PlannerInfo *root,
						RelOptInfo *foreignrel,
						List *param_join_conds,
						List *pathkeys,
						PgFdwPathExtraData *fpextra,
						double *p_rows, int *p_width,
						int *p_disabled_nodes,
						Cost *p_startup_cost, Cost *p_total_cost)
{
	PgFdwRelationInfo *fpinfo = (PgFdwRelationInfo *) foreignrel->fdw_private;
	double		rows;
	double		retrieved_rows;
	int			width;
	int			disabled_nodes = 0;
	Cost		startup_cost;
	Cost		total_cost;

	/* Make sure the core code has set up the relation's reltarget
	 *
	 * 确保核心代码已经设置了关系的reltarget
	 */
	Assert(foreignrel->reltarget);

	/*
	 * If the table or the server is configured to use remote estimates,
	 * connect to the foreign server and execute EXPLAIN to estimate the
	 * number of rows selected by the restriction+join clauses.  Otherwise,
	 * estimate rows using whatever statistics we have locally, in a way
	 * similar to ordinary tables.
	 *
	 * 如果表或服务器配置为使用远程估计，请连接到外部服务器并执行 EXPLAIN 来估计限制+连接子句选择的行数。  否则，使用我们本地拥有的任何统计信息以类似于普通表的方式估计行。
	 */
	if (fpinfo->use_remote_estimate)
	{
		List	   *remote_param_join_conds;
		List	   *local_param_join_conds;
		StringInfoData sql;
		PGconn	   *conn;
		Selectivity local_sel;
		QualCost	local_cost;
		List	   *fdw_scan_tlist = NIL;
		List	   *remote_conds;

		/* Required only to be passed to deparseSelectStmtForRel
		 *
		 * 仅需要传递给 deparseSelectStmtForRel
		 */
		List	   *retrieved_attrs;

		/*
		 * param_join_conds might contain both clauses that are safe to send
		 * across, and clauses that aren't.
		 *
		 * param_join_conds 可能包含可以安全发送的子句和不能安全发送的子句。
		 */
		classifyConditions(root, foreignrel, param_join_conds,
						   &remote_param_join_conds, &local_param_join_conds);

		/* Build the list of columns to be fetched from the foreign server.
		 *
		 * 构建要从外部服务器获取的列的列表。
		 */
		if (IS_JOIN_REL(foreignrel) || IS_UPPER_REL(foreignrel))
			fdw_scan_tlist = build_tlist_to_deparse(foreignrel);
		else
			fdw_scan_tlist = NIL;

		/*
		 * The complete list of remote conditions includes everything from
		 * baserestrictinfo plus any extra join_conds relevant to this
		 * particular path.
		 *
		 * 远程条件的完整列表包括来自 baserestrictinfo 的所有内容以及与此特定路径相关的任何额外 join_conds。
		 */
		remote_conds = list_concat(remote_param_join_conds,
								   fpinfo->remote_conds);

		/*
		 * Construct EXPLAIN query including the desired SELECT, FROM, and
		 * WHERE clauses. Params and other-relation Vars are replaced by dummy
		 * values, so don't request params_list.
		 *
		 * 构造 EXPLAIN 查询，包括所需的 SELECT、FROM 和 WHERE 子句。参数和其他关系变量被虚拟值替换，因此不要请求 params_list。
		 */
		initStringInfo(&sql);
		appendStringInfoString(&sql, "EXPLAIN ");
		deparseSelectStmtForRel(&sql, root, foreignrel, fdw_scan_tlist,
								remote_conds, pathkeys,
								fpextra ? fpextra->has_final_sort : false,
								fpextra ? fpextra->has_limit : false,
								false, &retrieved_attrs, NULL);

		/* Get the remote estimate
		 *
		 * 获取远程估算
		 */
		conn = GetConnection(fpinfo->user, false, NULL);
		get_remote_estimate(sql.data, conn, &rows, &width,
							&startup_cost, &total_cost);
		ReleaseConnection(conn);

		retrieved_rows = rows;

		/* Factor in the selectivity of the locally-checked quals
		 *
		 * 考虑本地检查质量的选择性
		 */
		local_sel = clauselist_selectivity(root,
										   local_param_join_conds,
										   foreignrel->relid,
										   JOIN_INNER,
										   NULL);
		local_sel *= fpinfo->local_conds_sel;

		rows = clamp_row_est(rows * local_sel);

		/* Add in the eval cost of the locally-checked quals
		 *
		 * 添加本地检查的 quals 的 eval 成本
		 */
		startup_cost += fpinfo->local_conds_cost.startup;
		total_cost += fpinfo->local_conds_cost.per_tuple * retrieved_rows;
		cost_qual_eval(&local_cost, local_param_join_conds, root);
		startup_cost += local_cost.startup;
		total_cost += local_cost.per_tuple * retrieved_rows;

		/*
		 * Add in tlist eval cost for each output row.  In case of an
		 * aggregate, some of the tlist expressions such as grouping
		 * expressions will be evaluated remotely, so adjust the costs.
		 *
		 * 添加每个输出行的 tlist eval 成本。  在聚合的情况下，某些 tlist 表达式（例如分组表达式）将被远程计算，因此请调整成本。
		 */
		startup_cost += foreignrel->reltarget->cost.startup;
		total_cost += foreignrel->reltarget->cost.startup;
		total_cost += foreignrel->reltarget->cost.per_tuple * rows;
		if (IS_UPPER_REL(foreignrel))
		{
			QualCost	tlist_cost;

			cost_qual_eval(&tlist_cost, fdw_scan_tlist, root);
			startup_cost -= tlist_cost.startup;
			total_cost -= tlist_cost.startup;
			total_cost -= tlist_cost.per_tuple * rows;
		}
	}
	else
	{
		Cost		run_cost = 0;

		/*
		 * We don't support join conditions in this mode (hence, no
		 * parameterized paths can be made).
		 *
		 * 我们不支持此模式下的连接条件（因此，无法创建参数化路径）。
		 */
		Assert(param_join_conds == NIL);

		/*
		 * We will come here again and again with different set of pathkeys or
		 * additional post-scan/join-processing steps that caller wants to
		 * cost.  We don't need to calculate the cost/size estimates for the
		 * underlying scan, join, or grouping each time.  Instead, use those
		 * estimates if we have cached them already.
		 *
		 * 我们将一次又一次地带着调用者想要花费的不同路径键集或额外的后扫描/连接处理步骤来到这里。  我们不需要每次都计算底层扫描、连接或分组的成本/大小估计。  相反，如果我们已经缓存了这些估计值，则使用它们。
		 */
		if (fpinfo->rel_startup_cost >= 0 && fpinfo->rel_total_cost >= 0)
		{
			Assert(fpinfo->retrieved_rows >= 0);

			rows = fpinfo->rows;
			retrieved_rows = fpinfo->retrieved_rows;
			width = fpinfo->width;
			startup_cost = fpinfo->rel_startup_cost;
			run_cost = fpinfo->rel_total_cost - fpinfo->rel_startup_cost;

			/*
			 * If we estimate the costs of a foreign scan or a foreign join
			 * with additional post-scan/join-processing steps, the scan or
			 * join costs obtained from the cache wouldn't yet contain the
			 * eval costs for the final scan/join target, which would've been
			 * updated by apply_scanjoin_target_to_paths(); add the eval costs
			 * now.
			 *
			 * 如果我们通过额外的后扫描/连接处理步骤来估计外部扫描或外部连接的成本，从缓存获得的扫描或连接成本将不包含最终扫描/连接目标的评估成本，该目标将由 apply_scanjoin_target_to_paths() 更新；现在添加评估成本。
			 */
			if (fpextra && !IS_UPPER_REL(foreignrel))
			{
				/* Shouldn't get here unless we have LIMIT
				 *
				 * 除非我们有限制，否则不应该到达这里
				 */
				Assert(fpextra->has_limit);
				Assert(foreignrel->reloptkind == RELOPT_BASEREL ||
					   foreignrel->reloptkind == RELOPT_JOINREL);
				startup_cost += foreignrel->reltarget->cost.startup;
				run_cost += foreignrel->reltarget->cost.per_tuple * rows;
			}
		}
		else if (IS_JOIN_REL(foreignrel))
		{
			PgFdwRelationInfo *fpinfo_i;
			PgFdwRelationInfo *fpinfo_o;
			QualCost	join_cost;
			QualCost	remote_conds_cost;
			double		nrows;

			/* Use rows/width estimates made by the core code.
			 *
			 * 使用核心代码所做的行/宽度估计。
			 */
			rows = foreignrel->rows;
			width = foreignrel->reltarget->width;

			/* For join we expect inner and outer relations set
			 *
			 * 对于连接，我们期望设置内部和外部关系
			 */
			Assert(fpinfo->innerrel && fpinfo->outerrel);

			fpinfo_i = (PgFdwRelationInfo *) fpinfo->innerrel->fdw_private;
			fpinfo_o = (PgFdwRelationInfo *) fpinfo->outerrel->fdw_private;

			/* Estimate of number of rows in cross product
			 *
			 * 叉积行数的估计
			 */
			nrows = fpinfo_i->rows * fpinfo_o->rows;

			/*
			 * Back into an estimate of the number of retrieved rows.  Just in
			 * case this is nuts, clamp to at most nrows.
			 *
			 * 返回到检索行数的估计。  以防万一这是坚果，夹紧最多 n 行。
			 */
			retrieved_rows = clamp_row_est(rows / fpinfo->local_conds_sel);
			retrieved_rows = Min(retrieved_rows, nrows);

			/*
			 * The cost of foreign join is estimated as cost of generating
			 * rows for the joining relations + cost for applying quals on the
			 * rows.
			 *
			 * 外部联接的成本估计为为联接关系生成行的成本 + 在行上应用 quals 的成本。
			 */

			/*
			 * Calculate the cost of clauses pushed down to the foreign server
			 *
			 * 计算推送到外部服务器的子句的成本
			 */
			cost_qual_eval(&remote_conds_cost, fpinfo->remote_conds, root);
			/* Calculate the cost of applying join clauses
			 *
			 * 计算应用连接子句的成本
			 */
			cost_qual_eval(&join_cost, fpinfo->joinclauses, root);

			/*
			 * Startup cost includes startup cost of joining relations and the
			 * startup cost for join and other clauses. We do not include the
			 * startup cost specific to join strategy (e.g. setting up hash
			 * tables) since we do not know what strategy the foreign server
			 * is going to use.
			 *
			 * 启动成本包括连接关系的启动成本和连接等子句的启动成本。我们不包括特定于连接策略（例如设置哈希表）的启动成本，因为我们不知道外部服务器将使用什么策略。
			 */
			startup_cost = fpinfo_i->rel_startup_cost + fpinfo_o->rel_startup_cost;
			startup_cost += join_cost.startup;
			startup_cost += remote_conds_cost.startup;
			startup_cost += fpinfo->local_conds_cost.startup;

			/*
			 * Run time cost includes:
			 *
			 * 运行时成本包括：
			 *
			 * 1. Run time cost (total_cost - startup_cost) of relations being
			 * joined
			 *
			 * 1. 被连接的关系的运行时间成本（total_cost -startup_cost）
			 *
			 * 2. Run time cost of applying join clauses on the cross product
			 * of the joining relations.
			 *
			 * 2. 对连接关系的叉积应用连接子句的运行时间成本。
			 *
			 * 3. Run time cost of applying pushed down other clauses on the
			 * result of join
			 *
			 * 3. 应用的运行时间成本压低了连接结果上的其他子句
			 *
			 * 4. Run time cost of applying nonpushable other clauses locally
			 * on the result fetched from the foreign server.
			 *
			 * 4. 在从外部服务器获取的结果上本地应用不可推送的其他子句的运行时间成本。
			 */
			run_cost = fpinfo_i->rel_total_cost - fpinfo_i->rel_startup_cost;
			run_cost += fpinfo_o->rel_total_cost - fpinfo_o->rel_startup_cost;
			run_cost += nrows * join_cost.per_tuple;
			nrows = clamp_row_est(nrows * fpinfo->joinclause_sel);
			run_cost += nrows * remote_conds_cost.per_tuple;
			run_cost += fpinfo->local_conds_cost.per_tuple * retrieved_rows;

			/* Add in tlist eval cost for each output row
			 *
			 * 添加每个输出行的 tlist eval 成本
			 */
			startup_cost += foreignrel->reltarget->cost.startup;
			run_cost += foreignrel->reltarget->cost.per_tuple * rows;
		}
		else if (IS_UPPER_REL(foreignrel))
		{
			RelOptInfo *outerrel = fpinfo->outerrel;
			PgFdwRelationInfo *ofpinfo;
			AggClauseCosts aggcosts = {0};
			double		input_rows;
			int			numGroupCols;
			double		numGroups = 1;

			/* The upper relation should have its outer relation set
			 *
			 * 上层关系应该有其外层关系集
			 */
			Assert(outerrel);
			/* and that outer relation should have its reltarget set
			 *
			 * 并且该外部关系应该设置其 reltarget
			 */
			Assert(outerrel->reltarget);

			/*
			 * This cost model is mixture of costing done for sorted and
			 * hashed aggregates in cost_agg().  We are not sure which
			 * strategy will be considered at remote side, thus for
			 * simplicity, we put all startup related costs in startup_cost
			 * and all finalization and run cost are added in total_cost.
			 *
			 * 该成本模型是在 cost_agg() 中对排序和散列聚合进行的成本计算的混合。  我们不确定远程端会考虑哪种策略，因此为了简单起见，我们将所有启动相关成本放入startup_cost中，并将所有最终确定和运行成本添加到total_cost中。
			 */

			ofpinfo = (PgFdwRelationInfo *) outerrel->fdw_private;

			/* Get rows from input rel
			 *
			 * 从输入 rel 获取行
			 */
			input_rows = ofpinfo->rows;

			/* Collect statistics about aggregates for estimating costs.
			 *
			 * 收集有关聚合的统计数据以估算成本。
			 */
			if (root->parse->hasAggs)
			{
				get_agg_clause_costs(root, AGGSPLIT_SIMPLE, &aggcosts);
			}

			/* Get number of grouping columns and possible number of groups
			 *
			 * 获取分组列数和可能的组数
			 */
			numGroupCols = list_length(root->processed_groupClause);
			numGroups = estimate_num_groups(root,
											get_sortgrouplist_exprs(root->processed_groupClause,
																	fpinfo->grouped_tlist),
											input_rows, NULL, NULL);

			/*
			 * Get the retrieved_rows and rows estimates.  If there are HAVING
			 * quals, account for their selectivity.
			 *
			 * 获取 returned_rows 和 rows 估计值。  如果存在“具有”资格，请考虑他们的选择性。
			 */
			if (root->hasHavingQual)
			{
				/* Factor in the selectivity of the remotely-checked quals
				 *
				 * 考虑远程检查质量的选择性
				 */
				retrieved_rows =
					clamp_row_est(numGroups *
								  clauselist_selectivity(root,
														 fpinfo->remote_conds,
														 0,
														 JOIN_INNER,
														 NULL));
				/* Factor in the selectivity of the locally-checked quals
				 *
				 * 考虑本地检查质量的选择性
				 */
				rows = clamp_row_est(retrieved_rows * fpinfo->local_conds_sel);
			}
			else
			{
				rows = retrieved_rows = numGroups;
			}

			/* Use width estimate made by the core code.
			 *
			 * 使用核心代码所做的宽度估计。
			 */
			width = foreignrel->reltarget->width;

			/*-----
			 * Startup cost includes:
			 *	  1. Startup cost for underneath input relation, adjusted for
			 *	     tlist replacement by apply_scanjoin_target_to_paths()
			 *	  2. Cost of performing aggregation, per cost_agg()
			 *
			 * 启动成本包括： 1. 底层输入关系的启动成本，通过 apply_scanjoin_target_to_paths() 调整 tlist 替换 2. 根据 cost_agg() 执行聚合的成本
			 *-----
			 */
			startup_cost = ofpinfo->rel_startup_cost;
			startup_cost += outerrel->reltarget->cost.startup;
			startup_cost += aggcosts.transCost.startup;
			startup_cost += aggcosts.transCost.per_tuple * input_rows;
			startup_cost += aggcosts.finalCost.startup;
			startup_cost += (cpu_operator_cost * numGroupCols) * input_rows;

			/*-----
			 * Run time cost includes:
			 *	  1. Run time cost of underneath input relation, adjusted for
			 *	     tlist replacement by apply_scanjoin_target_to_paths()
			 *	  2. Run time cost of performing aggregation, per cost_agg()
			 *
			 * 运行时成本包括： 1. 底层输入关系的运行时成本，通过 apply_scanjoin_target_to_paths() 调整 tlist 替换 2. 根据 cost_agg() 执行聚合的运行时成本
			 *-----
			 */
			run_cost = ofpinfo->rel_total_cost - ofpinfo->rel_startup_cost;
			run_cost += outerrel->reltarget->cost.per_tuple * input_rows;
			run_cost += aggcosts.finalCost.per_tuple * numGroups;
			run_cost += cpu_tuple_cost * numGroups;

			/* Account for the eval cost of HAVING quals, if any
			 *
			 * 考虑 HAVING quals 的评估成本（如果有）
			 */
			if (root->hasHavingQual)
			{
				QualCost	remote_cost;

				/* Add in the eval cost of the remotely-checked quals
				 *
				 * 添加远程检查质量的评估成本
				 */
				cost_qual_eval(&remote_cost, fpinfo->remote_conds, root);
				startup_cost += remote_cost.startup;
				run_cost += remote_cost.per_tuple * numGroups;
				/* Add in the eval cost of the locally-checked quals
				 *
				 * 添加本地检查的 quals 的 eval 成本
				 */
				startup_cost += fpinfo->local_conds_cost.startup;
				run_cost += fpinfo->local_conds_cost.per_tuple * retrieved_rows;
			}

			/* Add in tlist eval cost for each output row
			 *
			 * 添加每个输出行的 tlist eval 成本
			 */
			startup_cost += foreignrel->reltarget->cost.startup;
			run_cost += foreignrel->reltarget->cost.per_tuple * rows;
		}
		else
		{
			Cost		cpu_per_tuple;

			/* Use rows/width estimates made by set_baserel_size_estimates.
			 *
			 * 使用 set_baserel_size_estimates 进行的行/宽度估计。
			 */
			rows = foreignrel->rows;
			width = foreignrel->reltarget->width;

			/*
			 * Back into an estimate of the number of retrieved rows.  Just in
			 * case this is nuts, clamp to at most foreignrel->tuples.
			 *
			 * 返回到检索行数的估计。  以防万一这很疯狂，最多夹住foreignrel->元组。
			 */
			retrieved_rows = clamp_row_est(rows / fpinfo->local_conds_sel);
			retrieved_rows = Min(retrieved_rows, foreignrel->tuples);

			/*
			 * Cost as though this were a seqscan, which is pessimistic.  We
			 * effectively imagine the local_conds are being evaluated
			 * remotely, too.
			 *
			 * 成本就好像这是一个 seqscan，这是悲观的。  我们实际上想象 local_conds 也正在被远程评估。
			 */
			startup_cost = 0;
			run_cost = 0;
			run_cost += seq_page_cost * foreignrel->pages;

			startup_cost += foreignrel->baserestrictcost.startup;
			cpu_per_tuple = cpu_tuple_cost + foreignrel->baserestrictcost.per_tuple;
			run_cost += cpu_per_tuple * foreignrel->tuples;

			/* Add in tlist eval cost for each output row
			 *
			 * 添加每个输出行的 tlist eval 成本
			 */
			startup_cost += foreignrel->reltarget->cost.startup;
			run_cost += foreignrel->reltarget->cost.per_tuple * rows;
		}

		/*
		 * Without remote estimates, we have no real way to estimate the cost
		 * of generating sorted output.  It could be free if the query plan
		 * the remote side would have chosen generates properly-sorted output
		 * anyway, but in most cases it will cost something.  Estimate a value
		 * high enough that we won't pick the sorted path when the ordering
		 * isn't locally useful, but low enough that we'll err on the side of
		 * pushing down the ORDER BY clause when it's useful to do so.
		 *
		 * 如果没有远程估计，我们就没有真正的方法来估计生成排序输出的成本。  如果远程端选择的查询计划无论如何都会生成正确排序的输出，那么它可能是免费的，但在大多数情况下它会花费一些费用。  估计一个足够高的值，以便当排序在本地没有用时我们不会选择排序路径，但又足够低，以至于当有用时我们会错误地选择下推 ORDER BY 子句。
		 */
		if (pathkeys != NIL)
		{
			if (IS_UPPER_REL(foreignrel))
			{
				Assert(foreignrel->reloptkind == RELOPT_UPPER_REL &&
					   fpinfo->stage == UPPERREL_GROUP_AGG);
				adjust_foreign_grouping_path_cost(root, pathkeys,
												  retrieved_rows, width,
												  fpextra->limit_tuples,
												  &disabled_nodes,
												  &startup_cost, &run_cost);
			}
			else
			{
				startup_cost *= DEFAULT_FDW_SORT_MULTIPLIER;
				run_cost *= DEFAULT_FDW_SORT_MULTIPLIER;
			}
		}

		total_cost = startup_cost + run_cost;

		/* Adjust the cost estimates if we have LIMIT
		 *
		 * 如果我们有 LIMIT，请调整成本估算
		 */
		if (fpextra && fpextra->has_limit)
		{
			adjust_limit_rows_costs(&rows, &startup_cost, &total_cost,
									fpextra->offset_est, fpextra->count_est);
			retrieved_rows = rows;
		}
	}

	/*
	 * If this includes the final sort step, the given target, which will be
	 * applied to the resulting path, might have different expressions from
	 * the foreignrel's reltarget (see make_sort_input_target()); adjust tlist
	 * eval costs.
	 *
	 * 如果这包括最终的排序步骤，则将应用于结果路径的给定目标可能具有与foreignrel的reltarget不同的表达式（请参阅make_sort_input_target()）；调整列表评估成本。
	 */
	if (fpextra && fpextra->has_final_sort &&
		fpextra->target != foreignrel->reltarget)
	{
		QualCost	oldcost = foreignrel->reltarget->cost;
		QualCost	newcost = fpextra->target->cost;

		startup_cost += newcost.startup - oldcost.startup;
		total_cost += newcost.startup - oldcost.startup;
		total_cost += (newcost.per_tuple - oldcost.per_tuple) * rows;
	}

	/*
	 * Cache the retrieved rows and cost estimates for scans, joins, or
	 * groupings without any parameterization, pathkeys, or additional
	 * post-scan/join-processing steps, before adding the costs for
	 * transferring data from the foreign server.  These estimates are useful
	 * for costing remote joins involving this relation or costing other
	 * remote operations on this relation such as remote sorts and remote
	 * LIMIT restrictions, when the costs can not be obtained from the foreign
	 * server.  This function will be called at least once for every foreign
	 * relation without any parameterization, pathkeys, or additional
	 * post-scan/join-processing steps.
	 *
	 * 在添加从外部服务器传输数据的成本之前，缓存检索到的行和扫描、联接或分组的成本估算，无需任何参数化、路径键或其他扫描/联接后处理步骤。  当无法从外部服务器获得成本时，这些估计对于计算涉及此关系的远程连接的成本或计算此关系上的其他远程操作（例如远程排序和远程 LIMIT 限制）的成本非常有用。  对于每个外关系，该函数将至少调用一次，无需任何参数化、路径键或额外的后扫描/连接处理步骤。
	 */
	if (pathkeys == NIL && param_join_conds == NIL && fpextra == NULL)
	{
		fpinfo->retrieved_rows = retrieved_rows;
		fpinfo->rel_startup_cost = startup_cost;
		fpinfo->rel_total_cost = total_cost;
	}

	/*
	 * Add some additional cost factors to account for connection overhead
	 * (fdw_startup_cost), transferring data across the network
	 * (fdw_tuple_cost per retrieved row), and local manipulation of the data
	 * (cpu_tuple_cost per retrieved row).
	 *
	 * 添加一些额外的成本因素来考虑连接开销（fdw_startup_cost）、通过网络传输数据（每个检索行的 fdw_tuple_cost）以及数据的本地操作（每个检索行的 cpu_tuple_cost）。
	 */
	startup_cost += fpinfo->fdw_startup_cost;
	total_cost += fpinfo->fdw_startup_cost;
	total_cost += fpinfo->fdw_tuple_cost * retrieved_rows;
	total_cost += cpu_tuple_cost * retrieved_rows;

	/*
	 * If we have LIMIT, we should prefer performing the restriction remotely
	 * rather than locally, as the former avoids extra row fetches from the
	 * remote that the latter might cause.  But since the core code doesn't
	 * account for such fetches when estimating the costs of the local
	 * restriction (see create_limit_path()), there would be no difference
	 * between the costs of the local restriction and the costs of the remote
	 * restriction estimated above if we don't use remote estimates (except
	 * for the case where the foreignrel is a grouping relation, the given
	 * pathkeys is not NIL, and the effects of a bounded sort for that rel is
	 * accounted for in costing the remote restriction).  Tweak the costs of
	 * the remote restriction to ensure we'll prefer it if LIMIT is a useful
	 * one.
	 *
	 * 如果我们有 LIMIT，我们应该更喜欢远程而不是本地执行限制，因为前者可以避免后者可能导致的从远程获取额外的行。  但是，由于核心代码在估计本地限制的成本时没有考虑此类获取（请参阅 create_limit_path()），因此如果我们不使用远程估计，则本地限制的成本和上面估计的远程限制的成本之间不会有任何差异（除了foreignrel是分组关系的情况，给定的路径键不是NIL，并且在计算远程限制的成本时考虑了该rel的有界排序的影响）。  调整远程限制的成本，以确保如果 LIMIT 有用，我们会更喜欢它。
	 */
	if (!fpinfo->use_remote_estimate &&
		fpextra && fpextra->has_limit &&
		fpextra->limit_tuples > 0 &&
		fpextra->limit_tuples < fpinfo->rows)
	{
		Assert(fpinfo->rows > 0);
		total_cost -= (total_cost - startup_cost) * 0.05 *
			(fpinfo->rows - fpextra->limit_tuples) / fpinfo->rows;
	}

	/* Return results.
	 *
	 * 返回结果。
	 */
	*p_rows = rows;
	*p_width = width;
	*p_disabled_nodes = disabled_nodes;
	*p_startup_cost = startup_cost;
	*p_total_cost = total_cost;
}

/*
 * Estimate costs of executing a SQL statement remotely.
 * The given "sql" must be an EXPLAIN command.
 *
 * 估计远程执行 SQL 语句的成本。给定的“sql”必须是 EXPLAIN 命令。
 */
static void
get_remote_estimate(const char *sql, PGconn *conn,
					double *rows, int *width,
					Cost *startup_cost, Cost *total_cost)
{
	PGresult   *volatile res = NULL;

	/* PGresult must be released before leaving this function.
	 *
	 * 在离开此函数之前必须释放 PGresult。
	 */
	PG_TRY();
	{
		char	   *line;
		char	   *p;
		int			n;

		/*
		 * Execute EXPLAIN remotely.
		 *
		 * 远程执行 EXPLAIN。
		 */
		res = pgfdw_exec_query(conn, sql, NULL);
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
			pgfdw_report_error(ERROR, res, conn, false, sql);

		/*
		 * Extract cost numbers for topmost plan node.  Note we search for a
		 * left paren from the end of the line to avoid being confused by
		 * other uses of parentheses.
		 *
		 * 提取最顶层计划节点的成本数字。  请注意，我们从行尾搜索左括号，以避免与括号的其他用法混淆。
		 */
		line = PQgetvalue(res, 0, 0);
		p = strrchr(line, '(');
		if (p == NULL)
			elog(ERROR, "could not interpret EXPLAIN output: \"%s\"", line);
		n = sscanf(p, "(cost=%lf..%lf rows=%lf width=%d)",
				   startup_cost, total_cost, rows, width);
		if (n != 4)
			elog(ERROR, "could not interpret EXPLAIN output: \"%s\"", line);
	}
	PG_FINALLY();
	{
		PQclear(res);
	}
	PG_END_TRY();
}

/*
 * Adjust the cost estimates of a foreign grouping path to include the cost of
 * generating properly-sorted output.
 *
 * 调整外部分组路径的成本估计，以包括生成正确排序的输出的成本。
 */
static void
adjust_foreign_grouping_path_cost(PlannerInfo *root,
								  List *pathkeys,
								  double retrieved_rows,
								  double width,
								  double limit_tuples,
								  int *p_disabled_nodes,
								  Cost *p_startup_cost,
								  Cost *p_run_cost)
{
	/*
	 * If the GROUP BY clause isn't sort-able, the plan chosen by the remote
	 * side is unlikely to generate properly-sorted output, so it would need
	 * an explicit sort; adjust the given costs with cost_sort().  Likewise,
	 * if the GROUP BY clause is sort-able but isn't a superset of the given
	 * pathkeys, adjust the costs with that function.  Otherwise, adjust the
	 * costs by applying the same heuristic as for the scan or join case.
	 *
	 * 如果 GROUP BY 子句不可排序，则远程端选择的计划不太可能生成正确排序的输出，因此需要显式排序；使用 cost_sort() 调整给定的成本。  同样，如果 GROUP BY 子句是可排序的，但不是给定路径键的超集，则使用该函数调整成本。  否则，通过应用与扫描或连接情况相同的启发式来调整成本。
	 */
	if (!grouping_is_sortable(root->processed_groupClause) ||
		!pathkeys_contained_in(pathkeys, root->group_pathkeys))
	{
		Path		sort_path;	/* dummy for result of cost_sort */

		cost_sort(&sort_path,
				  root,
				  pathkeys,
				  0,
				  *p_startup_cost + *p_run_cost,
				  retrieved_rows,
				  width,
				  0.0,
				  work_mem,
				  limit_tuples);

		*p_startup_cost = sort_path.startup_cost;
		*p_run_cost = sort_path.total_cost - sort_path.startup_cost;
	}
	else
	{
		/*
		 * The default extra cost seems too large for foreign-grouping cases;
		 * add 1/4th of that default.
		 *
		 * 对于外国分组情况，默认的额外费用似乎太大；添加默认值的 1/4。
		 */
		double		sort_multiplier = 1.0 + (DEFAULT_FDW_SORT_MULTIPLIER
											 - 1.0) * 0.25;

		*p_startup_cost *= sort_multiplier;
		*p_run_cost *= sort_multiplier;
	}
}

/*
 * Detect whether we want to process an EquivalenceClass member.
 *
 * 检测我们是否要处理 EquivalenceClass 成员。
 *
 * This is a callback for use by generate_implied_equalities_for_column.
 *
 * 这是由generate_implied_equalities_for_column 使用的回调。
 */
static bool
ec_member_matches_foreign(PlannerInfo *root, RelOptInfo *rel,
						  EquivalenceClass *ec, EquivalenceMember *em,
						  void *arg)
{
	ec_member_foreign_arg *state = (ec_member_foreign_arg *) arg;
	Expr	   *expr = em->em_expr;

	/*
	 * If we've identified what we're processing in the current scan, we only
	 * want to match that expression.
	 *
	 * 如果我们已经确定了当前扫描中正在处理的内容，我们只想匹配该表达式。
	 */
	if (state->current != NULL)
		return equal(expr, state->current);

	/*
	 * Otherwise, ignore anything we've already processed.
	 *
	 * 否则，忽略我们已经处理过的任何内容。
	 */
	if (list_member(state->already_used, expr))
		return false;

	/* This is the new target to process.
	 *
	 * 这是要处理的新目标。
	 */
	state->current = expr;
	return true;
}

/*
 * Create cursor for node's query with current parameter values.
 *
 * 使用当前参数值为节点的查询创建游标。
 */
static void
create_cursor(ForeignScanState *node)
{
	PgFdwScanState *fsstate = (PgFdwScanState *) node->fdw_state;
	ExprContext *econtext = node->ss.ps.ps_ExprContext;
	int			numParams = fsstate->numParams;
	const char **values = fsstate->param_values;
	PGconn	   *conn = fsstate->conn;
	StringInfoData buf;
	PGresult   *res;

	/* First, process a pending asynchronous request, if any.
	 *
	 * 首先，处理挂起的异步请求（如果有）。
	 */
	if (fsstate->conn_state->pendingAreq)
		process_pending_request(fsstate->conn_state->pendingAreq);

	/*
	 * Construct array of query parameter values in text format.  We do the
	 * conversions in the short-lived per-tuple context, so as not to cause a
	 * memory leak over repeated scans.
	 *
	 * 以文本格式构造查询参数值数组。  我们在短期的每元组上下文中进行转换，以免重复扫描导致内存泄漏。
	 */
	if (numParams > 0)
	{
		MemoryContext oldcontext;

		oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

		process_query_params(econtext,
							 fsstate->param_flinfo,
							 fsstate->param_exprs,
							 values);

		MemoryContextSwitchTo(oldcontext);
	}

	/* Construct the DECLARE CURSOR command
	 *
	 * 构造 DECLARE CURSOR 命令
	 */
	initStringInfo(&buf);
	appendStringInfo(&buf, "DECLARE c%u CURSOR FOR\n%s",
					 fsstate->cursor_number, fsstate->query);

	/*
	 * Notice that we pass NULL for paramTypes, thus forcing the remote server
	 * to infer types for all parameters.  Since we explicitly cast every
	 * parameter (see deparse.c), the "inference" is trivial and will produce
	 * the desired result.  This allows us to avoid assuming that the remote
	 * server has the same OIDs we do for the parameters' types.
	 *
	 * 请注意，我们为 paramTypes 传递 NULL，从而强制远程服务器推断所有参数的类型。  由于我们显式地转换每个参数（参见 deparse.c），因此“推理”是微不足道的，并且会产生所需的结果。  这使我们能够避免假设远程服务器具有与我们为参数类型所做的相同的 OID。
	 */
	if (!PQsendQueryParams(conn, buf.data, numParams,
						   NULL, values, NULL, NULL, 0))
		pgfdw_report_error(ERROR, NULL, conn, false, buf.data);

	/*
	 * Get the result, and check for success.
	 *
	 * 获取结果，并检查是否成功。
	 *
	 * We don't use a PG_TRY block here, so be careful not to throw error
	 * without releasing the PGresult.
	 *
	 * 我们在这里不使用 PG_TRY 块，因此请注意不要在未释放 PGresult 的情况下引发错误。
	 */
	res = pgfdw_get_result(conn);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pgfdw_report_error(ERROR, res, conn, true, fsstate->query);
	PQclear(res);

	/* Mark the cursor as created, and show no tuples have been retrieved
	 *
	 * 将光标标记为已创建，并显示未检索到任何元组
	 */
	fsstate->cursor_exists = true;
	fsstate->tuples = NULL;
	fsstate->num_tuples = 0;
	fsstate->next_tuple = 0;
	fsstate->fetch_ct_2 = 0;
	fsstate->eof_reached = false;

	/* Clean up
	 *
	 * 清理
	 */
	pfree(buf.data);
}

/*
 * Fetch some more rows from the node's cursor.
 *
 * 从节点的游标中获取更多行。
 */
static void
fetch_more_data(ForeignScanState *node)
{
	PgFdwScanState *fsstate = (PgFdwScanState *) node->fdw_state;
	PGresult   *volatile res = NULL;
	MemoryContext oldcontext;

	/*
	 * We'll store the tuples in the batch_cxt.  First, flush the previous
	 * batch.
	 *
	 * 我们将把元组存储在batch_cxt中。  首先，冲洗前一批。
	 */
	fsstate->tuples = NULL;
	MemoryContextReset(fsstate->batch_cxt);
	oldcontext = MemoryContextSwitchTo(fsstate->batch_cxt);

	/* PGresult must be released before leaving this function.
	 *
	 * 在离开此函数之前必须释放 PGresult。
	 */
	PG_TRY();
	{
		PGconn	   *conn = fsstate->conn;
		int			numrows;
		int			i;

		if (fsstate->async_capable)
		{
			Assert(fsstate->conn_state->pendingAreq);

			/*
			 * The query was already sent by an earlier call to
			 * fetch_more_data_begin.  So now we just fetch the result.
			 *
			 * 该查询已通过之前对 fetch_more_data_begin 的调用发送。  所以现在我们只获取结果。
			 */
			res = pgfdw_get_result(conn);
			/* On error, report the original query, not the FETCH.
			 *
			 * 出错时，报告原始查询，而不是 FETCH。
			 */
			if (PQresultStatus(res) != PGRES_TUPLES_OK)
				pgfdw_report_error(ERROR, res, conn, false, fsstate->query);

			/* Reset per-connection state
			 *
			 * 重置每个连接状态
			 */
			fsstate->conn_state->pendingAreq = NULL;
		}
		else
		{
			char		sql[64];

			/* This is a regular synchronous fetch.
			 *
			 * 这是常规的同步获取。
			 */
			snprintf(sql, sizeof(sql), "FETCH %d FROM c%u",
					 fsstate->fetch_size, fsstate->cursor_number);

			res = pgfdw_exec_query(conn, sql, fsstate->conn_state);
			/* On error, report the original query, not the FETCH.
			 *
			 * 出错时，报告原始查询，而不是 FETCH。
			 */
			if (PQresultStatus(res) != PGRES_TUPLES_OK)
				pgfdw_report_error(ERROR, res, conn, false, fsstate->query);
		}

		/* Convert the data into HeapTuples
		 *
		 * 将数据转换为HeapTuple
		 */
		numrows = PQntuples(res);
		fsstate->tuples = (HeapTuple *) palloc0(numrows * sizeof(HeapTuple));
		fsstate->num_tuples = numrows;
		fsstate->next_tuple = 0;

		for (i = 0; i < numrows; i++)
		{
			Assert(IsA(node->ss.ps.plan, ForeignScan));

			fsstate->tuples[i] =
				make_tuple_from_result_row(res, i,
										   fsstate->rel,
										   fsstate->attinmeta,
										   fsstate->retrieved_attrs,
										   node,
										   fsstate->temp_cxt);
		}

		/* Update fetch_ct_2
		 *
		 * 更新 fetch_ct_2
		 */
		if (fsstate->fetch_ct_2 < 2)
			fsstate->fetch_ct_2++;

		/* Must be EOF if we didn't get as many tuples as we asked for.
		 *
		 * 如果我们没有得到我们要求的那么多元组，那么一定是 EOF。
		 */
		fsstate->eof_reached = (numrows < fsstate->fetch_size);
	}
	PG_FINALLY();
	{
		PQclear(res);
	}
	PG_END_TRY();

	MemoryContextSwitchTo(oldcontext);
}

/*
 * Force assorted GUC parameters to settings that ensure that we'll output
 * data values in a form that is unambiguous to the remote server.
 *
 * 将各种 GUC 参数强制设置为确保我们将以远程服务器明确的形式输出数据值。
 *
 * This is rather expensive and annoying to do once per row, but there's
 * little choice if we want to be sure values are transmitted accurately;
 * we can't leave the settings in place between rows for fear of affecting
 * user-visible computations.
 *
 * 每行执行一次，这是相当昂贵且烦人的，但如果我们想确保值被准确传输，则别无选择；我们不能将设置保留在行之间，因为担心影响用户可见的计算。
 *
 * We use the equivalent of a function SET option to allow the settings to
 * persist only until the caller calls reset_transmission_modes().  If an
 * error is thrown in between, guc.c will take care of undoing the settings.
 *
 * 我们使用函数 SET 选项的等效项来允许设置仅持续到调用者调用 reset_transmission_modes() 为止。  如果中间抛出错误，guc.c 将负责撤消设置。
 *
 * The return value is the nestlevel that must be passed to
 * reset_transmission_modes() to undo things.
 *
 * 返回值是必须传递给reset_transmission_modes() 才能撤消操作的嵌套级别。
 */
int
set_transmission_modes(void)
{
	int			nestlevel = NewGUCNestLevel();

	/*
	 * The values set here should match what pg_dump does.  See also
	 * configure_remote_session in connection.c.
	 *
	 * 此处设置的值应与 pg_dump 的设置相匹配。  另请参见connection.c 中的configure_remote_session。
	 */
	if (DateStyle != USE_ISO_DATES)
		(void) set_config_option("datestyle", "ISO",
								 PGC_USERSET, PGC_S_SESSION,
								 GUC_ACTION_SAVE, true, 0, false);
	if (IntervalStyle != INTSTYLE_POSTGRES)
		(void) set_config_option("intervalstyle", "postgres",
								 PGC_USERSET, PGC_S_SESSION,
								 GUC_ACTION_SAVE, true, 0, false);
	if (extra_float_digits < 3)
		(void) set_config_option("extra_float_digits", "3",
								 PGC_USERSET, PGC_S_SESSION,
								 GUC_ACTION_SAVE, true, 0, false);

	/*
	 * In addition force restrictive search_path, in case there are any
	 * regproc or similar constants to be printed.
	 *
	 * 另外，强制限制 search_path，以防有任何 regproc 或类似的常量要打印。
	 */
	(void) set_config_option("search_path", "pg_catalog",
							 PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_SAVE, true, 0, false);

	return nestlevel;
}

/*
 * Undo the effects of set_transmission_modes().
 *
 * 撤消 set_transmission_modes() 的效果。
 */
void
reset_transmission_modes(int nestlevel)
{
	AtEOXact_GUC(true, nestlevel);
}

/*
 * Utility routine to close a cursor.
 *
 * 用于关闭游标的实用程序。
 */
static void
close_cursor(PGconn *conn, unsigned int cursor_number,
			 PgFdwConnState *conn_state)
{
	char		sql[64];
	PGresult   *res;

	snprintf(sql, sizeof(sql), "CLOSE c%u", cursor_number);

	/*
	 * We don't use a PG_TRY block here, so be careful not to throw error
	 * without releasing the PGresult.
	 *
	 * 我们在这里不使用 PG_TRY 块，因此请注意不要在未释放 PGresult 的情况下引发错误。
	 */
	res = pgfdw_exec_query(conn, sql, conn_state);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pgfdw_report_error(ERROR, res, conn, true, sql);
	PQclear(res);
}

/*
 * create_foreign_modify
 *		Construct an execution state of a foreign insert/update/delete
 *		operation
 *
 * create_foreign_modify 构造外部插入/更新/删除操作的执行状态
 */
static PgFdwModifyState *
create_foreign_modify(EState *estate,
					  RangeTblEntry *rte,
					  ResultRelInfo *resultRelInfo,
					  CmdType operation,
					  Plan *subplan,
					  char *query,
					  List *target_attrs,
					  int values_end,
					  bool has_returning,
					  List *retrieved_attrs)
{
	PgFdwModifyState *fmstate;
	Relation	rel = resultRelInfo->ri_RelationDesc;
	TupleDesc	tupdesc = RelationGetDescr(rel);
	Oid			userid;
	ForeignTable *table;
	UserMapping *user;
	AttrNumber	n_params;
	Oid			typefnoid;
	bool		isvarlena;
	ListCell   *lc;

	/* Begin constructing PgFdwModifyState.
	 *
	 * 开始构造 PgFdwModifyState。
	 */
	fmstate = (PgFdwModifyState *) palloc0(sizeof(PgFdwModifyState));
	fmstate->rel = rel;

	/* Identify which user to do the remote access as.
	 *
	 * 确定以哪个用户身份进行远程访问。
	 */
	userid = ExecGetResultRelCheckAsUser(resultRelInfo, estate);

	/* Get info about foreign table.
	 *
	 * 获取有关外部表的信息。
	 */
	table = GetForeignTable(RelationGetRelid(rel));
	user = GetUserMapping(userid, table->serverid);

	/* Open connection; report that we'll create a prepared statement.
	 *
	 * 打开连接；报告我们将创建一份准备好的声明。
	 */
	fmstate->conn = GetConnection(user, true, &fmstate->conn_state);
	fmstate->p_name = NULL;		/* prepared statement not made yet */

	/* Set up remote query information.
	 *
	 * 设置远程查询信息。
	 */
	fmstate->query = query;
	if (operation == CMD_INSERT)
	{
		fmstate->query = pstrdup(fmstate->query);
		fmstate->orig_query = pstrdup(fmstate->query);
	}
	fmstate->target_attrs = target_attrs;
	fmstate->values_end = values_end;
	fmstate->has_returning = has_returning;
	fmstate->retrieved_attrs = retrieved_attrs;

	/* Create context for per-tuple temp workspace.
	 *
	 * 为每个元组临时工作区创建上下文。
	 */
	fmstate->temp_cxt = AllocSetContextCreate(estate->es_query_cxt,
											  "postgres_fdw temporary data",
											  ALLOCSET_SMALL_SIZES);

	/* Prepare for input conversion of RETURNING results.
	 *
	 * 准备返回结果的输入转换。
	 */
	if (fmstate->has_returning)
		fmstate->attinmeta = TupleDescGetAttInMetadata(tupdesc);

	/* Prepare for output conversion of parameters used in prepared stmt.
	 *
	 * 准备准备stmt中使用的参数的输出转换。
	 */
	n_params = list_length(fmstate->target_attrs) + 1;
	fmstate->p_flinfo = (FmgrInfo *) palloc0(sizeof(FmgrInfo) * n_params);
	fmstate->p_nums = 0;

	if (operation == CMD_UPDATE || operation == CMD_DELETE)
	{
		Assert(subplan != NULL);

		/* Find the ctid resjunk column in the subplan's result
		 *
		 * 在子计划的结果中查找 ctid resjunk 列
		 */
		fmstate->ctidAttno = ExecFindJunkAttributeInTlist(subplan->targetlist,
														  "ctid");
		if (!AttributeNumberIsValid(fmstate->ctidAttno))
			elog(ERROR, "could not find junk ctid column");

		/* First transmittable parameter will be ctid
		 *
		 * 第一个可传输参数是 ctid
		 */
		getTypeOutputInfo(TIDOID, &typefnoid, &isvarlena);
		fmgr_info(typefnoid, &fmstate->p_flinfo[fmstate->p_nums]);
		fmstate->p_nums++;
	}

	if (operation == CMD_INSERT || operation == CMD_UPDATE)
	{
		/* Set up for remaining transmittable parameters
		 *
		 * 设置剩余可传输参数
		 */
		foreach(lc, fmstate->target_attrs)
		{
			int			attnum = lfirst_int(lc);
			Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);

			Assert(!attr->attisdropped);

			/* Ignore generated columns; they are set to DEFAULT
			 *
			 * 忽略生成的列；它们被设置为默认值
			 */
			if (attr->attgenerated)
				continue;
			getTypeOutputInfo(attr->atttypid, &typefnoid, &isvarlena);
			fmgr_info(typefnoid, &fmstate->p_flinfo[fmstate->p_nums]);
			fmstate->p_nums++;
		}
	}

	Assert(fmstate->p_nums <= n_params);

	/* Set batch_size from foreign server/table options.
	 *
	 * 从外部服务器/表选项设置batch_size。
	 */
	if (operation == CMD_INSERT)
		fmstate->batch_size = get_batch_size_option(rel);

	fmstate->num_slots = 1;

	/* Initialize auxiliary state
	 *
	 * 初始化辅助状态
	 */
	fmstate->aux_fmstate = NULL;

	return fmstate;
}

/*
 * execute_foreign_modify
 *		Perform foreign-table modification as required, and fetch RETURNING
 *		result if any.  (This is the shared guts of postgresExecForeignInsert,
 *		postgresExecForeignBatchInsert, postgresExecForeignUpdate, and
 *		postgresExecForeignDelete.)
 *
 * execute_foreign_modify 根据需要执行外表修改，并获取 RETURNING 结果（如果有）。  （这是 postgresExecForeignInsert、postgresExecForeignBatchInsert、postgresExecForeignUpdate 和 postgresExecForeignDelete 的共享内容。）
 */
static TupleTableSlot **
execute_foreign_modify(EState *estate,
					   ResultRelInfo *resultRelInfo,
					   CmdType operation,
					   TupleTableSlot **slots,
					   TupleTableSlot **planSlots,
					   int *numSlots)
{
	PgFdwModifyState *fmstate = (PgFdwModifyState *) resultRelInfo->ri_FdwState;
	ItemPointer ctid = NULL;
	const char **p_values;
	PGresult   *res;
	int			n_rows;
	StringInfoData sql;

	/* The operation should be INSERT, UPDATE, or DELETE
	 *
	 * 操作应该是 INSERT、UPDATE 或 DELETE
	 */
	Assert(operation == CMD_INSERT ||
		   operation == CMD_UPDATE ||
		   operation == CMD_DELETE);

	/* First, process a pending asynchronous request, if any.
	 *
	 * 首先，处理挂起的异步请求（如果有）。
	 */
	if (fmstate->conn_state->pendingAreq)
		process_pending_request(fmstate->conn_state->pendingAreq);

	/*
	 * If the existing query was deparsed and prepared for a different number
	 * of rows, rebuild it for the proper number.
	 *
	 * 如果现有查询已被解析并准备用于不同数量的行，请将其重建为正确的数量。
	 */
	if (operation == CMD_INSERT && fmstate->num_slots != *numSlots)
	{
		/* Destroy the prepared statement created previously
		 *
		 * 销毁之前创建的准备好的语句
		 */
		if (fmstate->p_name)
			deallocate_query(fmstate);

		/* Build INSERT string with numSlots records in its VALUES clause.
		 *
		 * 在其 VALUES 子句中使用 numSlots 记录构建 INSERT 字符串。
		 */
		initStringInfo(&sql);
		rebuildInsertSql(&sql, fmstate->rel,
						 fmstate->orig_query, fmstate->target_attrs,
						 fmstate->values_end, fmstate->p_nums,
						 *numSlots - 1);
		pfree(fmstate->query);
		fmstate->query = sql.data;
		fmstate->num_slots = *numSlots;
	}

	/* Set up the prepared statement on the remote server, if we didn't yet
	 *
	 * 如果我们还没有在远程服务器上设置准备好的语句
	 */
	if (!fmstate->p_name)
		prepare_foreign_modify(fmstate);

	/*
	 * For UPDATE/DELETE, get the ctid that was passed up as a resjunk column
	 *
	 * 对于 UPDATE/DELETE，获取作为 resjunk 列传递的 ctid
	 */
	if (operation == CMD_UPDATE || operation == CMD_DELETE)
	{
		Datum		datum;
		bool		isNull;

		datum = ExecGetJunkAttribute(planSlots[0],
									 fmstate->ctidAttno,
									 &isNull);
		/* shouldn't ever get a null result...
		 *
		 * 永远不应该得到空结果......
		 */
		if (isNull)
			elog(ERROR, "ctid is NULL");
		ctid = (ItemPointer) DatumGetPointer(datum);
	}

	/* Convert parameters needed by prepared statement to text form
	 *
	 * 将准备好的语句所需的参数转换为文本形式
	 */
	p_values = convert_prep_stmt_params(fmstate, ctid, slots, *numSlots);

	/*
	 * Execute the prepared statement.
	 *
	 * 执行准备好的语句。
	 */
	if (!PQsendQueryPrepared(fmstate->conn,
							 fmstate->p_name,
							 fmstate->p_nums * (*numSlots),
							 p_values,
							 NULL,
							 NULL,
							 0))
		pgfdw_report_error(ERROR, NULL, fmstate->conn, false, fmstate->query);

	/*
	 * Get the result, and check for success.
	 *
	 * 获取结果，并检查是否成功。
	 *
	 * We don't use a PG_TRY block here, so be careful not to throw error
	 * without releasing the PGresult.
	 *
	 * 我们在这里不使用 PG_TRY 块，因此请注意不要在未释放 PGresult 的情况下引发错误。
	 */
	res = pgfdw_get_result(fmstate->conn);
	if (PQresultStatus(res) !=
		(fmstate->has_returning ? PGRES_TUPLES_OK : PGRES_COMMAND_OK))
		pgfdw_report_error(ERROR, res, fmstate->conn, true, fmstate->query);

	/* Check number of rows affected, and fetch RETURNING tuple if any
	 *
	 * 检查受影响的行数，并获取 RETURNING 元组（如果有）
	 */
	if (fmstate->has_returning)
	{
		Assert(*numSlots == 1);
		n_rows = PQntuples(res);
		if (n_rows > 0)
			store_returning_result(fmstate, slots[0], res);
	}
	else
		n_rows = atoi(PQcmdTuples(res));

	/* And clean up
	 *
	 * 并清理干净
	 */
	PQclear(res);

	MemoryContextReset(fmstate->temp_cxt);

	*numSlots = n_rows;

	/*
	 * Return NULL if nothing was inserted/updated/deleted on the remote end
	 *
	 * 如果远程端没有插入/更新/删除任何内容，则返回 NULL
	 */
	return (n_rows > 0) ? slots : NULL;
}

/*
 * prepare_foreign_modify
 *		Establish a prepared statement for execution of INSERT/UPDATE/DELETE
 *
 * prepare_foreign_modify 建立准备语句用于执行INSERT/UPDATE/DELETE
 */
static void
prepare_foreign_modify(PgFdwModifyState *fmstate)
{
	char		prep_name[NAMEDATALEN];
	char	   *p_name;
	PGresult   *res;

	/*
	 * The caller would already have processed a pending asynchronous request
	 * if any, so no need to do it here.
	 *
	 * 调用者可能已经处理了挂起的异步请求（如果有），因此无需在此处执行此操作。
	 */

	/* Construct name we'll use for the prepared statement.
	 *
	 * 我们将用于准备好的语句的构造名称。
	 */
	snprintf(prep_name, sizeof(prep_name), "pgsql_fdw_prep_%u",
			 GetPrepStmtNumber(fmstate->conn));
	p_name = pstrdup(prep_name);

	/*
	 * We intentionally do not specify parameter types here, but leave the
	 * remote server to derive them by default.  This avoids possible problems
	 * with the remote server using different type OIDs than we do.  All of
	 * the prepared statements we use in this module are simple enough that
	 * the remote server will make the right choices.
	 *
	 * 我们故意不在这里指定参数类型，而是让远程服务器默认派生它们。  这避免了远程服务器使用与我们不同类型的 OID 时可能出现的问题。  我们在此模块中使用的所有准备好的语句都非常简单，远程服务器将做出正确的选择。
	 */
	if (!PQsendPrepare(fmstate->conn,
					   p_name,
					   fmstate->query,
					   0,
					   NULL))
		pgfdw_report_error(ERROR, NULL, fmstate->conn, false, fmstate->query);

	/*
	 * Get the result, and check for success.
	 *
	 * 获取结果，并检查是否成功。
	 *
	 * We don't use a PG_TRY block here, so be careful not to throw error
	 * without releasing the PGresult.
	 *
	 * 我们在这里不使用 PG_TRY 块，因此请注意不要在未释放 PGresult 的情况下引发错误。
	 */
	res = pgfdw_get_result(fmstate->conn);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pgfdw_report_error(ERROR, res, fmstate->conn, true, fmstate->query);
	PQclear(res);

	/* This action shows that the prepare has been done.
	 *
	 * 这个动作表明准备工作已经完成。
	 */
	fmstate->p_name = p_name;
}

/*
 * convert_prep_stmt_params
 *		Create array of text strings representing parameter values
 *
 * Convert_prep_stmt_params 创建表示参数值的文本字符串数组
 *
 * tupleid is ctid to send, or NULL if none
 * slot is slot to get remaining parameters from, or NULL if none
 *
 * tupleid 是要发送的 ctid，如果没有槽位，则为 NULL 是从中获取剩余参数的槽位，如果没有槽位，则为 NULL
 *
 * Data is constructed in temp_cxt; caller should reset that after use.
 *
 * 数据在temp_cxt中构造；调用者应在使用后重置它。
 */
static const char **
convert_prep_stmt_params(PgFdwModifyState *fmstate,
						 ItemPointer tupleid,
						 TupleTableSlot **slots,
						 int numSlots)
{
	const char **p_values;
	int			i;
	int			j;
	int			pindex = 0;
	MemoryContext oldcontext;

	oldcontext = MemoryContextSwitchTo(fmstate->temp_cxt);

	p_values = (const char **) palloc(sizeof(char *) * fmstate->p_nums * numSlots);

	/* ctid is provided only for UPDATE/DELETE, which don't allow batching
	 *
	 * ctid 仅为 UPDATE/DELETE 提供，不允许批处理
	 */
	Assert(!(tupleid != NULL && numSlots > 1));

	/* 1st parameter should be ctid, if it's in use
	 *
	 * 第一个参数应该是 ctid（如果正在使用）
	 */
	if (tupleid != NULL)
	{
		Assert(numSlots == 1);
		/* don't need set_transmission_modes for TID output
		 *
		 * TID 输出不需要 set_transmission_modes
		 */
		p_values[pindex] = OutputFunctionCall(&fmstate->p_flinfo[pindex],
											  PointerGetDatum(tupleid));
		pindex++;
	}

	/* get following parameters from slots
	 *
	 * 从槽中获取以下参数
	 */
	if (slots != NULL && fmstate->target_attrs != NIL)
	{
		TupleDesc	tupdesc = RelationGetDescr(fmstate->rel);
		int			nestlevel;
		ListCell   *lc;

		nestlevel = set_transmission_modes();

		for (i = 0; i < numSlots; i++)
		{
			j = (tupleid != NULL) ? 1 : 0;
			foreach(lc, fmstate->target_attrs)
			{
				int			attnum = lfirst_int(lc);
				CompactAttribute *attr = TupleDescCompactAttr(tupdesc, attnum - 1);
				Datum		value;
				bool		isnull;

				/* Ignore generated columns; they are set to DEFAULT
				 *
				 * 忽略生成的列；它们被设置为默认值
				 */
				if (attr->attgenerated)
					continue;
				value = slot_getattr(slots[i], attnum, &isnull);
				if (isnull)
					p_values[pindex] = NULL;
				else
					p_values[pindex] = OutputFunctionCall(&fmstate->p_flinfo[j],
														  value);
				pindex++;
				j++;
			}
		}

		reset_transmission_modes(nestlevel);
	}

	Assert(pindex == fmstate->p_nums * numSlots);

	MemoryContextSwitchTo(oldcontext);

	return p_values;
}

/*
 * store_returning_result
 *		Store the result of a RETURNING clause
 *
 * store_returning_result 存储 RETURNING 子句的结果
 *
 * On error, be sure to release the PGresult on the way out.  Callers do not
 * have PG_TRY blocks to ensure this happens.
 *
 * 发生错误时，请务必在退出时释放 PGresult。  调用者没有 PG_TRY 块来确保这种情况发生。
 */
static void
store_returning_result(PgFdwModifyState *fmstate,
					   TupleTableSlot *slot, PGresult *res)
{
	PG_TRY();
	{
		HeapTuple	newtup;

		newtup = make_tuple_from_result_row(res, 0,
											fmstate->rel,
											fmstate->attinmeta,
											fmstate->retrieved_attrs,
											NULL,
											fmstate->temp_cxt);

		/*
		 * The returning slot will not necessarily be suitable to store
		 * heaptuples directly, so allow for conversion.
		 *
		 * 返回槽不一定适合直接存储七元组，因此允许转换。
		 */
		ExecForceStoreHeapTuple(newtup, slot, true);
	}
	PG_CATCH();
	{
		PQclear(res);
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * finish_foreign_modify
 *		Release resources for a foreign insert/update/delete operation
 *
 * finish_foreign_modify 释放外部插入/更新/删除操作的资源
 */
static void
finish_foreign_modify(PgFdwModifyState *fmstate)
{
	Assert(fmstate != NULL);

	/* If we created a prepared statement, destroy it
	 *
	 * 如果我们创建了一个准备好的语句，则销毁它
	 */
	deallocate_query(fmstate);

	/* Release remote connection
	 *
	 * 释放远程连接
	 */
	ReleaseConnection(fmstate->conn);
	fmstate->conn = NULL;
}

/*
 * deallocate_query
 *		Deallocate a prepared statement for a foreign insert/update/delete
 *		operation
 *
 * deallocate_query 为外部插入/更新/删除操作取消分配准备好的语句
 */
static void
deallocate_query(PgFdwModifyState *fmstate)
{
	char		sql[64];
	PGresult   *res;

	/* do nothing if the query is not allocated
	 *
	 * 如果未分配查询，则不执行任何操作
	 */
	if (!fmstate->p_name)
		return;

	snprintf(sql, sizeof(sql), "DEALLOCATE %s", fmstate->p_name);

	/*
	 * We don't use a PG_TRY block here, so be careful not to throw error
	 * without releasing the PGresult.
	 *
	 * 我们在这里不使用 PG_TRY 块，因此请注意不要在未释放 PGresult 的情况下引发错误。
	 */
	res = pgfdw_exec_query(fmstate->conn, sql, fmstate->conn_state);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pgfdw_report_error(ERROR, res, fmstate->conn, true, sql);
	PQclear(res);
	pfree(fmstate->p_name);
	fmstate->p_name = NULL;
}

/*
 * build_remote_returning
 *		Build a RETURNING targetlist of a remote query for performing an
 *		UPDATE/DELETE .. RETURNING on a join directly
 *
 * build_remote_returning 构建远程查询的 RETURNING 目标列表，用于执行 UPDATE/DELETE .. 直接在连接上返回
 */
static List *
build_remote_returning(Index rtindex, Relation rel, List *returningList)
{
	bool		have_wholerow = false;
	List	   *tlist = NIL;
	List	   *vars;
	ListCell   *lc;

	Assert(returningList);

	vars = pull_var_clause((Node *) returningList, PVC_INCLUDE_PLACEHOLDERS);

	/*
	 * If there's a whole-row reference to the target relation, then we'll
	 * need all the columns of the relation.
	 *
	 * 如果存在对目标关系的整行引用，那么我们将需要该关系的所有列。
	 */
	foreach(lc, vars)
	{
		Var		   *var = (Var *) lfirst(lc);

		if (IsA(var, Var) &&
			var->varno == rtindex &&
			var->varattno == InvalidAttrNumber)
		{
			have_wholerow = true;
			break;
		}
	}

	if (have_wholerow)
	{
		TupleDesc	tupdesc = RelationGetDescr(rel);
		int			i;

		for (i = 1; i <= tupdesc->natts; i++)
		{
			Form_pg_attribute attr = TupleDescAttr(tupdesc, i - 1);
			Var		   *var;

			/* Ignore dropped attributes.
			 *
			 * 忽略删除的属性。
			 */
			if (attr->attisdropped)
				continue;

			var = makeVar(rtindex,
						  i,
						  attr->atttypid,
						  attr->atttypmod,
						  attr->attcollation,
						  0);

			tlist = lappend(tlist,
							makeTargetEntry((Expr *) var,
											list_length(tlist) + 1,
											NULL,
											false));
		}
	}

	/* Now add any remaining columns to tlist.
	 *
	 * 现在将所有剩余的列添加到 tlist 中。
	 */
	foreach(lc, vars)
	{
		Var		   *var = (Var *) lfirst(lc);

		/*
		 * No need for whole-row references to the target relation.  We don't
		 * need system columns other than ctid and oid either, since those are
		 * set locally.
		 *
		 * 不需要对目标关系进行整行引用。  除了 ctid 和 oid 之外，我们也不需要系统列，因为它们是本地设置的。
		 */
		if (IsA(var, Var) &&
			var->varno == rtindex &&
			var->varattno <= InvalidAttrNumber &&
			var->varattno != SelfItemPointerAttributeNumber)
			continue;			/* don't need it */

		if (tlist_member((Expr *) var, tlist))
			continue;			/* already got it */

		tlist = lappend(tlist,
						makeTargetEntry((Expr *) var,
										list_length(tlist) + 1,
										NULL,
										false));
	}

	list_free(vars);

	return tlist;
}

/*
 * rebuild_fdw_scan_tlist
 *		Build new fdw_scan_tlist of given foreign-scan plan node from given
 *		tlist
 *
 * rebuild_fdw_scan_tlist 从给定的 tlist 构建给定外部扫描计划节点的新 fdw_scan_tlist
 *
 * There might be columns that the fdw_scan_tlist of the given foreign-scan
 * plan node contains that the given tlist doesn't.  The fdw_scan_tlist would
 * have contained resjunk columns such as 'ctid' of the target relation and
 * 'wholerow' of non-target relations, but the tlist might not contain them,
 * for example.  So, adjust the tlist so it contains all the columns specified
 * in the fdw_scan_tlist; else setrefs.c will get confused.
 *
 * 给定外部扫描计划节点的 fdw_scan_tlist 可能包含给定 tlist 不包含的列。  fdw_scan_tlist 将包含 resjunk 列，例如目标关系的“ctid”和非目标关系的“wholerow”，但 tlist 可能不包含它们。  因此，调整 tlist，使其包含 fdw_scan_tlist 中指定的所有列；否则 setrefs.c 会感到困惑。
 */
static void
rebuild_fdw_scan_tlist(ForeignScan *fscan, List *tlist)
{
	List	   *new_tlist = tlist;
	List	   *old_tlist = fscan->fdw_scan_tlist;
	ListCell   *lc;

	foreach(lc, old_tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);

		if (tlist_member(tle->expr, new_tlist))
			continue;			/* already got it */

		new_tlist = lappend(new_tlist,
							makeTargetEntry(tle->expr,
											list_length(new_tlist) + 1,
											NULL,
											false));
	}
	fscan->fdw_scan_tlist = new_tlist;
}

/*
 * Execute a direct UPDATE/DELETE statement.
 *
 * 执行直接 UPDATE/DELETE 语句。
 */
static void
execute_dml_stmt(ForeignScanState *node)
{
	PgFdwDirectModifyState *dmstate = (PgFdwDirectModifyState *) node->fdw_state;
	ExprContext *econtext = node->ss.ps.ps_ExprContext;
	int			numParams = dmstate->numParams;
	const char **values = dmstate->param_values;

	/* First, process a pending asynchronous request, if any.
	 *
	 * 首先，处理挂起的异步请求（如果有）。
	 */
	if (dmstate->conn_state->pendingAreq)
		process_pending_request(dmstate->conn_state->pendingAreq);

	/*
	 * Construct array of query parameter values in text format.
	 *
	 * 以文本格式构造查询参数值数组。
	 */
	if (numParams > 0)
		process_query_params(econtext,
							 dmstate->param_flinfo,
							 dmstate->param_exprs,
							 values);

	/*
	 * Notice that we pass NULL for paramTypes, thus forcing the remote server
	 * to infer types for all parameters.  Since we explicitly cast every
	 * parameter (see deparse.c), the "inference" is trivial and will produce
	 * the desired result.  This allows us to avoid assuming that the remote
	 * server has the same OIDs we do for the parameters' types.
	 *
	 * 请注意，我们为 paramTypes 传递 NULL，从而强制远程服务器推断所有参数的类型。  由于我们显式地转换每个参数（参见 deparse.c），因此“推理”是微不足道的，并且会产生所需的结果。  这使我们能够避免假设远程服务器具有与我们为参数类型所做的相同的 OID。
	 */
	if (!PQsendQueryParams(dmstate->conn, dmstate->query, numParams,
						   NULL, values, NULL, NULL, 0))
		pgfdw_report_error(ERROR, NULL, dmstate->conn, false, dmstate->query);

	/*
	 * Get the result, and check for success.
	 *
	 * 获取结果，并检查是否成功。
	 *
	 * We use a memory context callback to ensure that the PGresult will be
	 * released, even if the query fails somewhere that's outside our control.
	 * The callback is already registered, just need to fill in its arg.
	 *
	 * 我们使用内存上下文回调来确保 PGresult 被释放，即使查询在我们无法控制的地方失败也是如此。回调已经注册了，只需要填写它的arg即可。
	 */
	Assert(dmstate->result == NULL);
	dmstate->result = pgfdw_get_result(dmstate->conn);
	dmstate->result_cb.arg = dmstate->result;

	if (PQresultStatus(dmstate->result) !=
		(dmstate->has_returning ? PGRES_TUPLES_OK : PGRES_COMMAND_OK))
		pgfdw_report_error(ERROR, dmstate->result, dmstate->conn, false,
						   dmstate->query);

	/* Get the number of rows affected.
	 *
	 * 获取受影响的行数。
	 */
	if (dmstate->has_returning)
		dmstate->num_tuples = PQntuples(dmstate->result);
	else
		dmstate->num_tuples = atoi(PQcmdTuples(dmstate->result));
}

/*
 * Get the result of a RETURNING clause.
 *
 * 获取 RETURNING 子句的结果。
 */
static TupleTableSlot *
get_returning_data(ForeignScanState *node)
{
	PgFdwDirectModifyState *dmstate = (PgFdwDirectModifyState *) node->fdw_state;
	EState	   *estate = node->ss.ps.state;
	ResultRelInfo *resultRelInfo = node->resultRelInfo;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
	TupleTableSlot *resultSlot;

	Assert(resultRelInfo->ri_projectReturning);

	/* If we didn't get any tuples, must be end of data.
	 *
	 * 如果我们没有得到任何元组，则一定是数据结束。
	 */
	if (dmstate->next_tuple >= dmstate->num_tuples)
		return ExecClearTuple(slot);

	/* Increment the command es_processed count if necessary.
	 *
	 * 如有必要，增加命令 es_processed 计数。
	 */
	if (dmstate->set_processed)
		estate->es_processed += 1;

	/*
	 * Store a RETURNING tuple.  If has_returning is false, just emit a dummy
	 * tuple.  (has_returning is false when the local query is of the form
	 * "UPDATE/DELETE .. RETURNING 1" for example.)
	 *
	 * 存储一个返回元组。  如果 has_returning 为 false，则仅发出一个虚拟元组。  （例如，当本地查询的形式为“UPDATE/DELETE .. RETURNING 1”时，has_returning 为 false。）
	 */
	if (!dmstate->has_returning)
	{
		ExecStoreAllNullTuple(slot);
		resultSlot = slot;
	}
	else
	{
		HeapTuple	newtup;

		newtup = make_tuple_from_result_row(dmstate->result,
											dmstate->next_tuple,
											dmstate->rel,
											dmstate->attinmeta,
											dmstate->retrieved_attrs,
											node,
											dmstate->temp_cxt);
		ExecStoreHeapTuple(newtup, slot, false);
		/* Get the updated/deleted tuple.
		 *
		 * 获取更新/删除的元组。
		 */
		if (dmstate->rel)
			resultSlot = slot;
		else
			resultSlot = apply_returning_filter(dmstate, resultRelInfo, slot, estate);
	}
	dmstate->next_tuple++;

	/* Make slot available for evaluation of the local query RETURNING list.
	 *
	 * 使槽可用于评估本地查询返回列表。
	 */
	resultRelInfo->ri_projectReturning->pi_exprContext->ecxt_scantuple =
		resultSlot;

	return slot;
}

/*
 * Initialize a filter to extract an updated/deleted tuple from a scan tuple.
 *
 * 初始化过滤器以从扫描元组中提取更新/删除的元组。
 */
static void
init_returning_filter(PgFdwDirectModifyState *dmstate,
					  List *fdw_scan_tlist,
					  Index rtindex)
{
	TupleDesc	resultTupType = RelationGetDescr(dmstate->resultRel);
	ListCell   *lc;
	int			i;

	/*
	 * Calculate the mapping between the fdw_scan_tlist's entries and the
	 * result tuple's attributes.
	 *
	 * 计算 fdw_scan_tlist 的条目和结果元组的属性之间的映射。
	 *
	 * The "map" is an array of indexes of the result tuple's attributes in
	 * fdw_scan_tlist, i.e., one entry for every attribute of the result
	 * tuple.  We store zero for any attributes that don't have the
	 * corresponding entries in that list, marking that a NULL is needed in
	 * the result tuple.
	 *
	 * “map”是 fdw_scan_tlist 中结果元组属性的索引数组，即结果元组的每个属性都有一个条目。  我们为列表中没有相应条目的任何属性存储零，标记结果元组中需要 NULL。
	 *
	 * Also get the indexes of the entries for ctid and oid if any.
	 *
	 * 还获取 ctid 和 oid 条目的索引（如果有）。
	 */
	dmstate->attnoMap = (AttrNumber *)
		palloc0(resultTupType->natts * sizeof(AttrNumber));

	dmstate->ctidAttno = dmstate->oidAttno = 0;

	i = 1;
	dmstate->hasSystemCols = false;
	foreach(lc, fdw_scan_tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);
		Var		   *var = (Var *) tle->expr;

		Assert(IsA(var, Var));

		/*
		 * If the Var is a column of the target relation to be retrieved from
		 * the foreign server, get the index of the entry.
		 *
		 * 如果Var是要从外部服务器检索的目标关系的列，则获取该条目的索引。
		 */
		if (var->varno == rtindex &&
			list_member_int(dmstate->retrieved_attrs, i))
		{
			int			attrno = var->varattno;

			if (attrno < 0)
			{
				/*
				 * We don't retrieve system columns other than ctid and oid.
				 *
				 * 我们不会检索除 ctid 和 oid 之外的系统列。
				 */
				if (attrno == SelfItemPointerAttributeNumber)
					dmstate->ctidAttno = i;
				else
					Assert(false);
				dmstate->hasSystemCols = true;
			}
			else
			{
				/*
				 * We don't retrieve whole-row references to the target
				 * relation either.
				 *
				 * 我们也不检索对目标关系的整行引用。
				 */
				Assert(attrno > 0);

				dmstate->attnoMap[attrno - 1] = i;
			}
		}
		i++;
	}
}

/*
 * Extract and return an updated/deleted tuple from a scan tuple.
 *
 * 从扫描元组中提取并返回更新/删除的元组。
 */
static TupleTableSlot *
apply_returning_filter(PgFdwDirectModifyState *dmstate,
					   ResultRelInfo *resultRelInfo,
					   TupleTableSlot *slot,
					   EState *estate)
{
	TupleDesc	resultTupType = RelationGetDescr(dmstate->resultRel);
	TupleTableSlot *resultSlot;
	Datum	   *values;
	bool	   *isnull;
	Datum	   *old_values;
	bool	   *old_isnull;
	int			i;

	/*
	 * Use the return tuple slot as a place to store the result tuple.
	 *
	 * 使用返回元组槽作为存储结果元组的地方。
	 */
	resultSlot = ExecGetReturningSlot(estate, resultRelInfo);

	/*
	 * Extract all the values of the scan tuple.
	 *
	 * 提取扫描元组的所有值。
	 */
	slot_getallattrs(slot);
	old_values = slot->tts_values;
	old_isnull = slot->tts_isnull;

	/*
	 * Prepare to build the result tuple.
	 *
	 * 准备构建结果元组。
	 */
	ExecClearTuple(resultSlot);
	values = resultSlot->tts_values;
	isnull = resultSlot->tts_isnull;

	/*
	 * Transpose data into proper fields of the result tuple.
	 *
	 * 将数据转置到结果元组的适当字段中。
	 */
	for (i = 0; i < resultTupType->natts; i++)
	{
		int			j = dmstate->attnoMap[i];

		if (j == 0)
		{
			values[i] = (Datum) 0;
			isnull[i] = true;
		}
		else
		{
			values[i] = old_values[j - 1];
			isnull[i] = old_isnull[j - 1];
		}
	}

	/*
	 * Build the virtual tuple.
	 *
	 * 构建虚拟元组。
	 */
	ExecStoreVirtualTuple(resultSlot);

	/*
	 * If we have any system columns to return, materialize a heap tuple in
	 * the slot from column values set above and install system columns in
	 * that tuple.
	 *
	 * 如果我们有任何系统列要返回，请根据上面设置的列值在槽中具体化一个堆元组，并在该元组中安装系统列。
	 */
	if (dmstate->hasSystemCols)
	{
		HeapTuple	resultTup = ExecFetchSlotHeapTuple(resultSlot, true, NULL);

		/* ctid */
		if (dmstate->ctidAttno)
		{
			ItemPointer ctid = NULL;

			ctid = (ItemPointer) DatumGetPointer(old_values[dmstate->ctidAttno - 1]);
			resultTup->t_self = *ctid;
		}

		/*
		 * And remaining columns
		 *
		 * 以及剩余的列
		 *
		 * Note: since we currently don't allow the target relation to appear
		 * on the nullable side of an outer join, any system columns wouldn't
		 * go to NULL.
		 *
		 * 注意：由于我们当前不允许目标关系出现在外连接的可为空一侧，因此任何系统列都不会变为 NULL。
		 *
		 * Note: no need to care about tableoid here because it will be
		 * initialized in ExecProcessReturning().
		 *
		 * 注意：这里不需要关心tableoid，因为它会在ExecProcessReturning()中初始化。
		 */
		HeapTupleHeaderSetXmin(resultTup->t_data, InvalidTransactionId);
		HeapTupleHeaderSetXmax(resultTup->t_data, InvalidTransactionId);
		HeapTupleHeaderSetCmin(resultTup->t_data, InvalidTransactionId);
	}

	/*
	 * And return the result tuple.
	 *
	 * 并返回结果元组。
	 */
	return resultSlot;
}

/*
 * Prepare for processing of parameters used in remote query.
 *
 * 准备处理远程查询中使用的参数。
 */
static void
prepare_query_params(PlanState *node,
					 List *fdw_exprs,
					 int numParams,
					 FmgrInfo **param_flinfo,
					 List **param_exprs,
					 const char ***param_values)
{
	int			i;
	ListCell   *lc;

	Assert(numParams > 0);

	/* Prepare for output conversion of parameters used in remote query.
	 *
	 * 准备远程查询参数的输出转换。
	 */
	*param_flinfo = (FmgrInfo *) palloc0(sizeof(FmgrInfo) * numParams);

	i = 0;
	foreach(lc, fdw_exprs)
	{
		Node	   *param_expr = (Node *) lfirst(lc);
		Oid			typefnoid;
		bool		isvarlena;

		getTypeOutputInfo(exprType(param_expr), &typefnoid, &isvarlena);
		fmgr_info(typefnoid, &(*param_flinfo)[i]);
		i++;
	}

	/*
	 * Prepare remote-parameter expressions for evaluation.  (Note: in
	 * practice, we expect that all these expressions will be just Params, so
	 * we could possibly do something more efficient than using the full
	 * expression-eval machinery for this.  But probably there would be little
	 * benefit, and it'd require postgres_fdw to know more than is desirable
	 * about Param evaluation.)
	 *
	 * 准备用于评估的远程参数表达式。  （注意：在实践中，我们期望所有这些表达式都只是参数，因此我们可能会做一些比使用完整表达式评估机制更有效的事情。但可能没有什么好处，并且需要 postgres_fdw 了解更多关于参数评估的信息。）
	 */
	*param_exprs = ExecInitExprList(fdw_exprs, node);

	/* Allocate buffer for text form of query parameters.
	 *
	 * 为文本形式的查询参数分配缓冲区。
	 */
	*param_values = (const char **) palloc0(numParams * sizeof(char *));
}

/*
 * Construct array of query parameter values in text format.
 *
 * 以文本格式构造查询参数值数组。
 */
static void
process_query_params(ExprContext *econtext,
					 FmgrInfo *param_flinfo,
					 List *param_exprs,
					 const char **param_values)
{
	int			nestlevel;
	int			i;
	ListCell   *lc;

	nestlevel = set_transmission_modes();

	i = 0;
	foreach(lc, param_exprs)
	{
		ExprState  *expr_state = (ExprState *) lfirst(lc);
		Datum		expr_value;
		bool		isNull;

		/* Evaluate the parameter expression
		 *
		 * 计算参数表达式
		 */
		expr_value = ExecEvalExpr(expr_state, econtext, &isNull);

		/*
		 * Get string representation of each parameter value by invoking
		 * type-specific output function, unless the value is null.
		 *
		 * 通过调用特定于类型的输出函数获取每个参数值的字符串表示形式，除非该值为 null。
		 */
		if (isNull)
			param_values[i] = NULL;
		else
			param_values[i] = OutputFunctionCall(&param_flinfo[i], expr_value);

		i++;
	}

	reset_transmission_modes(nestlevel);
}

/*
 * postgresAnalyzeForeignTable
 *		Test whether analyzing this foreign table is supported
 *
 * postgresAnalyzeForeignTable 测试是否支持分析该外表
 */
static bool
postgresAnalyzeForeignTable(Relation relation,
							AcquireSampleRowsFunc *func,
							BlockNumber *totalpages)
{
	ForeignTable *table;
	UserMapping *user;
	PGconn	   *conn;
	StringInfoData sql;
	PGresult   *volatile res = NULL;

	/* Return the row-analysis function pointer
	 *
	 * 返回行分析函数指针
	 */
	*func = postgresAcquireSampleRowsFunc;

	/*
	 * Now we have to get the number of pages.  It's annoying that the ANALYZE
	 * API requires us to return that now, because it forces some duplication
	 * of effort between this routine and postgresAcquireSampleRowsFunc.  But
	 * it's probably not worth redefining that API at this point.
	 *
	 * 现在我们必须获取页数。  令人烦恼的是，ANALYZE API 要求我们现在返回该值，因为它强制在此例程和 postgresAcquireSampleRowsFunc 之间进行一些重复工作。  但此时可能不值得重新定义该 API。
	 */

	/*
	 * Get the connection to use.  We do the remote access as the table's
	 * owner, even if the ANALYZE was started by some other user.
	 *
	 * 获取要使用的连接。  我们以表所有者的身份进行远程访问，即使 ANALYZE 是由其他用户启动的。
	 */
	table = GetForeignTable(RelationGetRelid(relation));
	user = GetUserMapping(relation->rd_rel->relowner, table->serverid);
	conn = GetConnection(user, false, NULL);

	/*
	 * Construct command to get page count for relation.
	 *
	 * 构造命令来获取关系的页数。
	 */
	initStringInfo(&sql);
	deparseAnalyzeSizeSql(&sql, relation);

	/* In what follows, do not risk leaking any PGresults.
	 *
	 * 在接下来的内容中，不要冒险泄露任何 PGresults。
	 */
	PG_TRY();
	{
		res = pgfdw_exec_query(conn, sql.data, NULL);
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
			pgfdw_report_error(ERROR, res, conn, false, sql.data);

		if (PQntuples(res) != 1 || PQnfields(res) != 1)
			elog(ERROR, "unexpected result from deparseAnalyzeSizeSql query");
		*totalpages = strtoul(PQgetvalue(res, 0, 0), NULL, 10);
	}
	PG_FINALLY();
	{
		PQclear(res);
	}
	PG_END_TRY();

	ReleaseConnection(conn);

	return true;
}

/*
 * postgresGetAnalyzeInfoForForeignTable
 *		Count tuples in foreign table (just get pg_class.reltuples).
 *
 * postgresGetAnalyzeInfoForForeignTable 统计外部表中的元组数量（只需获取 pg_class.reltuples）。
 *
 * can_tablesample determines if the remote relation supports acquiring the
 * sample using TABLESAMPLE.
 *
 * can_tablesample 确定远程关系是否支持使用 TABLESAMPLE 获取样本。
 */
static double
postgresGetAnalyzeInfoForForeignTable(Relation relation, bool *can_tablesample)
{
	ForeignTable *table;
	UserMapping *user;
	PGconn	   *conn;
	StringInfoData sql;
	PGresult   *volatile res = NULL;
	volatile double reltuples = -1;
	volatile char relkind = 0;

	/* assume the remote relation does not support TABLESAMPLE
	 *
	 * 假设远程关系不支持 TABLESAMPLE
	 */
	*can_tablesample = false;

	/*
	 * Get the connection to use.  We do the remote access as the table's
	 * owner, even if the ANALYZE was started by some other user.
	 *
	 * 获取要使用的连接。  我们以表所有者的身份进行远程访问，即使 ANALYZE 是由其他用户启动的。
	 */
	table = GetForeignTable(RelationGetRelid(relation));
	user = GetUserMapping(relation->rd_rel->relowner, table->serverid);
	conn = GetConnection(user, false, NULL);

	/*
	 * Construct command to get page count for relation.
	 *
	 * 构造命令来获取关系的页数。
	 */
	initStringInfo(&sql);
	deparseAnalyzeInfoSql(&sql, relation);

	/* In what follows, do not risk leaking any PGresults.
	 *
	 * 在接下来的内容中，不要冒险泄露任何 PGresults。
	 */
	PG_TRY();
	{
		res = pgfdw_exec_query(conn, sql.data, NULL);
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
			pgfdw_report_error(ERROR, res, conn, false, sql.data);

		if (PQntuples(res) != 1 || PQnfields(res) != 2)
			elog(ERROR, "unexpected result from deparseAnalyzeInfoSql query");
		reltuples = strtod(PQgetvalue(res, 0, 0), NULL);
		relkind = *(PQgetvalue(res, 0, 1));
	}
	PG_FINALLY();
	{
		if (res)
			PQclear(res);
	}
	PG_END_TRY();

	ReleaseConnection(conn);

	/* TABLESAMPLE is supported only for regular tables and matviews
	 *
	 * TABLESAMPLE 仅支持常规表和 matview
	 */
	*can_tablesample = (relkind == RELKIND_RELATION ||
						relkind == RELKIND_MATVIEW ||
						relkind == RELKIND_PARTITIONED_TABLE);

	return reltuples;
}

/*
 * Acquire a random sample of rows from foreign table managed by postgres_fdw.
 *
 * 从 postgres_fdw 管理的外表中获取行的随机样本。
 *
 * Selected rows are returned in the caller-allocated array rows[],
 * which must have at least targrows entries.
 * The actual number of rows selected is returned as the function result.
 * We also count the total number of rows in the table and return it into
 * *totalrows.  Note that *totaldeadrows is always set to 0.
 *
 * 选定的行将在调用者分配的数组 rows[] 中返回，该数组必须至少具有 targrows 条目。实际选择的行数作为函数结果返回。我们还计算表中的总行数并将其返回到 *totalrows 中。  请注意，*totaldeadrows 始终设置为 0。
 *
 * Note that the returned list of rows is not always in order by physical
 * position in the table.  Therefore, correlation estimates derived later
 * may be meaningless, but it's OK because we don't use the estimates
 * currently (the planner only pays attention to correlation for indexscans).
 *
 * 请注意，返回的行列表并不总是按表中的物理位置排序。  因此，稍后得出的相关性估计可能没有意义，但没关系，因为我们当前不使用估计（规划器只关注索引扫描的相关性）。
 */
static int
postgresAcquireSampleRowsFunc(Relation relation, int elevel,
							  HeapTuple *rows, int targrows,
							  double *totalrows,
							  double *totaldeadrows)
{
	PgFdwAnalyzeState astate;
	ForeignTable *table;
	ForeignServer *server;
	UserMapping *user;
	PGconn	   *conn;
	int			server_version_num;
	PgFdwSamplingMethod method = ANALYZE_SAMPLE_AUTO;	/* auto is default */
	double		sample_frac = -1.0;
	double		reltuples;
	unsigned int cursor_number;
	StringInfoData sql;
	PGresult   *volatile res = NULL;
	ListCell   *lc;

	/* Initialize workspace state
	 *
	 * 初始化工作区状态
	 */
	astate.rel = relation;
	astate.attinmeta = TupleDescGetAttInMetadata(RelationGetDescr(relation));

	astate.rows = rows;
	astate.targrows = targrows;
	astate.numrows = 0;
	astate.samplerows = 0;
	astate.rowstoskip = -1;		/* -1 means not set yet */
	reservoir_init_selection_state(&astate.rstate, targrows);

	/* Remember ANALYZE context, and create a per-tuple temp context
	 *
	 * 记住分析上下文，并创建每个元组的临时上下文
	 */
	astate.anl_cxt = CurrentMemoryContext;
	astate.temp_cxt = AllocSetContextCreate(CurrentMemoryContext,
											"postgres_fdw temporary data",
											ALLOCSET_SMALL_SIZES);

	/*
	 * Get the connection to use.  We do the remote access as the table's
	 * owner, even if the ANALYZE was started by some other user.
	 *
	 * 获取要使用的连接。  我们以表所有者的身份进行远程访问，即使 ANALYZE 是由其他用户启动的。
	 */
	table = GetForeignTable(RelationGetRelid(relation));
	server = GetForeignServer(table->serverid);
	user = GetUserMapping(relation->rd_rel->relowner, table->serverid);
	conn = GetConnection(user, false, NULL);

	/* We'll need server version, so fetch it now.
	 *
	 * 我们需要服务器版本，所以现在就获取它。
	 */
	server_version_num = PQserverVersion(conn);

	/*
	 * What sampling method should we use?
	 *
	 * 我们应该使用什么采样方法？
	 */
	foreach(lc, server->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "analyze_sampling") == 0)
		{
			char	   *value = defGetString(def);

			if (strcmp(value, "off") == 0)
				method = ANALYZE_SAMPLE_OFF;
			else if (strcmp(value, "auto") == 0)
				method = ANALYZE_SAMPLE_AUTO;
			else if (strcmp(value, "random") == 0)
				method = ANALYZE_SAMPLE_RANDOM;
			else if (strcmp(value, "system") == 0)
				method = ANALYZE_SAMPLE_SYSTEM;
			else if (strcmp(value, "bernoulli") == 0)
				method = ANALYZE_SAMPLE_BERNOULLI;

			break;
		}
	}

	foreach(lc, table->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "analyze_sampling") == 0)
		{
			char	   *value = defGetString(def);

			if (strcmp(value, "off") == 0)
				method = ANALYZE_SAMPLE_OFF;
			else if (strcmp(value, "auto") == 0)
				method = ANALYZE_SAMPLE_AUTO;
			else if (strcmp(value, "random") == 0)
				method = ANALYZE_SAMPLE_RANDOM;
			else if (strcmp(value, "system") == 0)
				method = ANALYZE_SAMPLE_SYSTEM;
			else if (strcmp(value, "bernoulli") == 0)
				method = ANALYZE_SAMPLE_BERNOULLI;

			break;
		}
	}

	/*
	 * Error-out if explicitly required one of the TABLESAMPLE methods, but
	 * the server does not support it.
	 *
	 * 如果明确需要 TABLESAMPLE 方法之一，但服务器不支持它，则会出错。
	 */
	if ((server_version_num < 95000) &&
		(method == ANALYZE_SAMPLE_SYSTEM ||
		 method == ANALYZE_SAMPLE_BERNOULLI))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("remote server does not support TABLESAMPLE feature")));

	/*
	 * If we've decided to do remote sampling, calculate the sampling rate. We
	 * need to get the number of tuples from the remote server, but skip that
	 * network round-trip if not needed.
	 *
	 * 如果我们决定进行远程采样，请计算采样率。我们需要从远程服务器获取元组的数量，但如果不需要，则跳过网络往返。
	 */
	if (method != ANALYZE_SAMPLE_OFF)
	{
		bool		can_tablesample;

		reltuples = postgresGetAnalyzeInfoForForeignTable(relation,
														  &can_tablesample);

		/*
		 * Make sure we're not choosing TABLESAMPLE when the remote relation
		 * does not support that. But only do this for "auto" - if the user
		 * explicitly requested BERNOULLI/SYSTEM, it's better to fail.
		 *
		 * 确保当远程关系不支持 TABLESAMPLE 时我们不会选择 TABLESAMPLE。但仅对“auto”执行此操作 - 如果用户明确请求 BERNOULLI/SYSTEM，最好失败。
		 */
		if (!can_tablesample && (method == ANALYZE_SAMPLE_AUTO))
			method = ANALYZE_SAMPLE_RANDOM;

		/*
		 * Remote's reltuples could be 0 or -1 if the table has never been
		 * vacuumed/analyzed.  In that case, disable sampling after all.
		 *
		 * 如果表从未被清理/分析过，则 Remote 的 reltuple 可能为 0 或 -1。  在这种情况下，请务必禁用采样。
		 */
		if ((reltuples <= 0) || (targrows >= reltuples))
			method = ANALYZE_SAMPLE_OFF;
		else
		{
			/*
			 * All supported sampling methods require sampling rate, not
			 * target rows directly, so we calculate that using the remote
			 * reltuples value. That's imperfect, because it might be off a
			 * good deal, but that's not something we can (or should) address
			 * here.
			 *
			 * 所有支持的采样方法都需要采样率，而不是直接定位目标行，因此我们使用远程 reltuples 值进行计算。这是不完美的，因为它可能会带来很多好处，但这不是我们可以（或应该）在这里解决的问题。
			 *
			 * If reltuples is too low (i.e. when table grew), we'll end up
			 * sampling more rows - but then we'll apply the local sampling,
			 * so we get the expected sample size. This is the same outcome as
			 * without remote sampling.
			 *
			 * 如果 reltuples 太低（即当表增长时），我们最终将采样更多行 - 但随后我们将应用局部采样，以便我们获得预期的样本大小。这与没有远程采样的结果相同。
			 *
			 * If reltuples is too high (e.g. after bulk DELETE), we will end
			 * up sampling too few rows.
			 *
			 * 如果 reltuples 太高（例如在批量删除之后），我们最终会采样太少的行。
			 *
			 * We can't really do much better here - we could try sampling a
			 * bit more rows, but we don't know how off the reltuples value is
			 * so how much is "a bit more"?
			 *
			 * 我们真的不能在这里做得更好 - 我们可以尝试采样更多的行，但我们不知道 reltuples 值有多大，那么“多一点”是多少？
			 *
			 * Furthermore, the targrows value for partitions is determined
			 * based on table size (relpages), which can be off in different
			 * ways too. Adjusting the sampling rate here might make the issue
			 * worse.
			 *
			 * 此外，分区的 targrows 值是根据表大小（relpages）确定的，也可以通过不同的方式关闭。调整此处的采样率可能会使问题变得更糟。
			 */
			sample_frac = targrows / reltuples;

			/*
			 * We should never get sampling rate outside the valid range
			 * (between 0.0 and 1.0), because those cases should be covered by
			 * the previous branch that sets ANALYZE_SAMPLE_OFF.
			 *
			 * 我们永远不应该让采样率超出有效范围（0.0 和 1.0 之间），因为这些情况应该由设置 ANALYZE_SAMPLE_OFF 的前一个分支涵盖。
			 */
			Assert(sample_frac >= 0.0 && sample_frac <= 1.0);
		}
	}

	/*
	 * For "auto" method, pick the one we believe is best. For servers with
	 * TABLESAMPLE support we pick BERNOULLI, for old servers we fall-back to
	 * random() to at least reduce network transfer.
	 *
	 * 对于“自动”方法，选择我们认为最好的方法。对于支持 TABLESAMPLE 的服务器，我们选择 BERNOULLI，对于旧服务器，我们回退到 random() 以至少减少网络传输。
	 */
	if (method == ANALYZE_SAMPLE_AUTO)
	{
		if (server_version_num < 95000)
			method = ANALYZE_SAMPLE_RANDOM;
		else
			method = ANALYZE_SAMPLE_BERNOULLI;
	}

	/*
	 * Construct cursor that retrieves whole rows from remote.
	 *
	 * 构造从远程检索整行的游标。
	 */
	cursor_number = GetCursorNumber(conn);
	initStringInfo(&sql);
	appendStringInfo(&sql, "DECLARE c%u CURSOR FOR ", cursor_number);

	deparseAnalyzeSql(&sql, relation, method, sample_frac, &astate.retrieved_attrs);

	/* In what follows, do not risk leaking any PGresults.
	 *
	 * 在接下来的内容中，不要冒险泄露任何 PGresults。
	 */
	PG_TRY();
	{
		char		fetch_sql[64];
		int			fetch_size;

		res = pgfdw_exec_query(conn, sql.data, NULL);
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
			pgfdw_report_error(ERROR, res, conn, false, sql.data);
		PQclear(res);
		res = NULL;

		/*
		 * Determine the fetch size.  The default is arbitrary, but shouldn't
		 * be enormous.
		 *
		 * 确定获取大小。  默认值是任意的，但不应太大。
		 */
		fetch_size = 100;
		foreach(lc, server->options)
		{
			DefElem    *def = (DefElem *) lfirst(lc);

			if (strcmp(def->defname, "fetch_size") == 0)
			{
				(void) parse_int(defGetString(def), &fetch_size, 0, NULL);
				break;
			}
		}
		foreach(lc, table->options)
		{
			DefElem    *def = (DefElem *) lfirst(lc);

			if (strcmp(def->defname, "fetch_size") == 0)
			{
				(void) parse_int(defGetString(def), &fetch_size, 0, NULL);
				break;
			}
		}

		/* Construct command to fetch rows from remote.
		 *
		 * 构造命令以从远程获取行。
		 */
		snprintf(fetch_sql, sizeof(fetch_sql), "FETCH %d FROM c%u",
				 fetch_size, cursor_number);

		/* Retrieve and process rows a batch at a time.
		 *
		 * 一次批量检索和处理行。
		 */
		for (;;)
		{
			int			numrows;
			int			i;

			/* Allow users to cancel long query
			 *
			 * 允许用户取消长查询
			 */
			CHECK_FOR_INTERRUPTS();

			/*
			 * XXX possible future improvement: if rowstoskip is large, we
			 * could issue a MOVE rather than physically fetching the rows,
			 * then just adjust rowstoskip and samplerows appropriately.
			 *
			 * XXX 未来可能的改进：如果 rowstoskip 很大，我们可以发出 MOVE 而不是物理获取行，然后只需适当调整 rowstoskip 和 Samplerows 即可。
			 */

			/* Fetch some rows
			 *
			 * 获取一些行
			 */
			res = pgfdw_exec_query(conn, fetch_sql, NULL);
			/* On error, report the original query, not the FETCH.
			 *
			 * 出错时，报告原始查询，而不是 FETCH。
			 */
			if (PQresultStatus(res) != PGRES_TUPLES_OK)
				pgfdw_report_error(ERROR, res, conn, false, sql.data);

			/* Process whatever we got.
			 *
			 * 处理我们得到的一切。
			 */
			numrows = PQntuples(res);
			for (i = 0; i < numrows; i++)
				analyze_row_processor(res, i, &astate);

			PQclear(res);
			res = NULL;

			/* Must be EOF if we didn't get all the rows requested.
			 *
			 * 如果我们没有获得请求的所有行，则必须是 EOF。
			 */
			if (numrows < fetch_size)
				break;
		}

		/* Close the cursor, just to be tidy.
		 *
		 * 关闭光标，只是为了整洁。
		 */
		close_cursor(conn, cursor_number, NULL);
	}
	PG_CATCH();
	{
		PQclear(res);
		PG_RE_THROW();
	}
	PG_END_TRY();

	ReleaseConnection(conn);

	/* We assume that we have no dead tuple.
	 *
	 * 我们假设我们没有死元组。
	 */
	*totaldeadrows = 0.0;

	/*
	 * Without sampling, we've retrieved all living tuples from foreign
	 * server, so report that as totalrows.  Otherwise use the reltuples
	 * estimate we got from the remote side.
	 *
	 * 在没有采样的情况下，我们已经从外部服务器检索了所有活动元组，因此将其报告为总行数。  否则使用我们从远程端获得的 reltuple 估计。
	 */
	if (method == ANALYZE_SAMPLE_OFF)
		*totalrows = astate.samplerows;
	else
		*totalrows = reltuples;

	/*
	 * Emit some interesting relation info
	 *
	 * 发出一些有趣的关系信息
	 */
	ereport(elevel,
			(errmsg("\"%s\": table contains %.0f rows, %d rows in sample",
					RelationGetRelationName(relation),
					*totalrows, astate.numrows)));

	return astate.numrows;
}

/*
 * Collect sample rows from the result of query.
 *	 - Use all tuples in sample until target # of samples are collected.
 *	 - Subsequently, replace already-sampled tuples randomly.
 *
 * 从查询结果中收集样本行。 - 使用样本中的所有元组，直到收集到目标样本数。 - 随后，随机替换已经采样的元组。
 */
static void
analyze_row_processor(PGresult *res, int row, PgFdwAnalyzeState *astate)
{
	int			targrows = astate->targrows;
	int			pos;			/* array index to store tuple in */
	MemoryContext oldcontext;

	/* Always increment sample row counter.
	 *
	 * 始终递增样本行计数器。
	 */
	astate->samplerows += 1;

	/*
	 * Determine the slot where this sample row should be stored.  Set pos to
	 * negative value to indicate the row should be skipped.
	 *
	 * 确定应存储此示例行的槽。  将 pos 设置为负值以指示应跳过该行。
	 */
	if (astate->numrows < targrows)
	{
		/* First targrows rows are always included into the sample
		 *
		 * 第一个 targrows 行始终包含在样本中
		 */
		pos = astate->numrows++;
	}
	else
	{
		/*
		 * Now we start replacing tuples in the sample until we reach the end
		 * of the relation.  Same algorithm as in acquire_sample_rows in
		 * analyze.c; see Jeff Vitter's paper.
		 *
		 * 现在我们开始替换样本中的元组，直到到达关系的末尾。  与analyze.c中acquire_sample_rows相同的算法；参见 Jeff Vitter 的论文。
		 */
		if (astate->rowstoskip < 0)
			astate->rowstoskip = reservoir_get_next_S(&astate->rstate, astate->samplerows, targrows);

		if (astate->rowstoskip <= 0)
		{
			/* Choose a random reservoir element to replace.
			 *
			 * 选择一个随机的储层元件进行替换。
			 */
			pos = (int) (targrows * sampler_random_fract(&astate->rstate.randstate));
			Assert(pos >= 0 && pos < targrows);
			heap_freetuple(astate->rows[pos]);
		}
		else
		{
			/* Skip this tuple.
			 *
			 * 跳过这个元组。
			 */
			pos = -1;
		}

		astate->rowstoskip -= 1;
	}

	if (pos >= 0)
	{
		/*
		 * Create sample tuple from current result row, and store it in the
		 * position determined above.  The tuple has to be created in anl_cxt.
		 *
		 * 从当前结果行创建样本元组，并将其存储在上面确定的位置。  该元组必须在 anl_cxt 中创建。
		 */
		oldcontext = MemoryContextSwitchTo(astate->anl_cxt);

		astate->rows[pos] = make_tuple_from_result_row(res, row,
													   astate->rel,
													   astate->attinmeta,
													   astate->retrieved_attrs,
													   NULL,
													   astate->temp_cxt);

		MemoryContextSwitchTo(oldcontext);
	}
}

/*
 * Import a foreign schema
 *
 * 导入外部模式
 */
static List *
postgresImportForeignSchema(ImportForeignSchemaStmt *stmt, Oid serverOid)
{
	List	   *commands = NIL;
	bool		import_collate = true;
	bool		import_default = false;
	bool		import_generated = true;
	bool		import_not_null = true;
	ForeignServer *server;
	UserMapping *mapping;
	PGconn	   *conn;
	StringInfoData buf;
	PGresult   *volatile res = NULL;
	int			numrows,
				i;
	ListCell   *lc;

	/* Parse statement options
	 *
	 * 解析语句选项
	 */
	foreach(lc, stmt->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "import_collate") == 0)
			import_collate = defGetBoolean(def);
		else if (strcmp(def->defname, "import_default") == 0)
			import_default = defGetBoolean(def);
		else if (strcmp(def->defname, "import_generated") == 0)
			import_generated = defGetBoolean(def);
		else if (strcmp(def->defname, "import_not_null") == 0)
			import_not_null = defGetBoolean(def);
		else
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
					 errmsg("invalid option \"%s\"", def->defname)));
	}

	/*
	 * Get connection to the foreign server.  Connection manager will
	 * establish new connection if necessary.
	 *
	 * 获取与外部服务器的连接。  如果需要，连接管理器将建立新连接。
	 */
	server = GetForeignServer(serverOid);
	mapping = GetUserMapping(GetUserId(), server->serverid);
	conn = GetConnection(mapping, false, NULL);

	/* Don't attempt to import collation if remote server hasn't got it
	 *
	 * 如果远程服务器尚未获取排序规则，则不要尝试导入排序规则
	 */
	if (PQserverVersion(conn) < 90100)
		import_collate = false;

	/* Create workspace for strings
	 *
	 * 为字符串创建工作区
	 */
	initStringInfo(&buf);

	/* In what follows, do not risk leaking any PGresults.
	 *
	 * 在接下来的内容中，不要冒险泄露任何 PGresults。
	 */
	PG_TRY();
	{
		/* Check that the schema really exists
		 *
		 * 检查模式是否确实存在
		 */
		appendStringInfoString(&buf, "SELECT 1 FROM pg_catalog.pg_namespace WHERE nspname = ");
		deparseStringLiteral(&buf, stmt->remote_schema);

		res = pgfdw_exec_query(conn, buf.data, NULL);
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
			pgfdw_report_error(ERROR, res, conn, false, buf.data);

		if (PQntuples(res) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_FDW_SCHEMA_NOT_FOUND),
					 errmsg("schema \"%s\" is not present on foreign server \"%s\"",
							stmt->remote_schema, server->servername)));

		PQclear(res);
		res = NULL;
		resetStringInfo(&buf);

		/*
		 * Fetch all table data from this schema, possibly restricted by
		 * EXCEPT or LIMIT TO.  (We don't actually need to pay any attention
		 * to EXCEPT/LIMIT TO here, because the core code will filter the
		 * statements we return according to those lists anyway.  But it
		 * should save a few cycles to not process excluded tables in the
		 * first place.)
		 *
		 * 从此模式中获取所有表数据，可能受到 EXCEPT 或 LIMIT TO 的限制。  （我们实际上不需要在这里关注 EXCEPT/LIMIT TO ，因为核心代码无论如何都会根据这些列表过滤我们返回的语句。但是它应该节省一些周期，以便首先不处理排除的表。）
		 *
		 * Import table data for partitions only when they are explicitly
		 * specified in LIMIT TO clause. Otherwise ignore them and only
		 * include the definitions of the root partitioned tables to allow
		 * access to the complete remote data set locally in the schema
		 * imported.
		 *
		 * 仅当在 LIMIT TO 子句中显式指定分区时，才导入分区的表数据。否则忽略它们，只包含根分区表的定义，以允许在导入的模式中本地访问完整的远程数据集。
		 *
		 * Note: because we run the connection with search_path restricted to
		 * pg_catalog, the format_type() and pg_get_expr() outputs will always
		 * include a schema name for types/functions in other schemas, which
		 * is what we want.
		 *
		 * 注意：因为我们使用仅限于 pg_catalog 的 search_path 运行连接，所以 format_type() 和 pg_get_expr() 输出将始终包含其他模式中类型/函数的模式名称，这正是我们想要的。
		 */
		appendStringInfoString(&buf,
							   "SELECT relname, "
							   "  attname, "
							   "  format_type(atttypid, atttypmod), "
							   "  attnotnull, "
							   "  pg_get_expr(adbin, adrelid), ");

		/* Generated columns are supported since Postgres 12
		 *
		 * 从 Postgres 12 开始支持生成列
		 */
		if (PQserverVersion(conn) >= 120000)
			appendStringInfoString(&buf,
								   "  attgenerated, ");
		else
			appendStringInfoString(&buf,
								   "  NULL, ");

		if (import_collate)
			appendStringInfoString(&buf,
								   "  collname, "
								   "  collnsp.nspname ");
		else
			appendStringInfoString(&buf,
								   "  NULL, NULL ");

		appendStringInfoString(&buf,
							   "FROM pg_class c "
							   "  JOIN pg_namespace n ON "
							   "    relnamespace = n.oid "
							   "  LEFT JOIN pg_attribute a ON "
							   "    attrelid = c.oid AND attnum > 0 "
							   "      AND NOT attisdropped "
							   "  LEFT JOIN pg_attrdef ad ON "
							   "    adrelid = c.oid AND adnum = attnum ");

		if (import_collate)
			appendStringInfoString(&buf,
								   "  LEFT JOIN pg_collation coll ON "
								   "    coll.oid = attcollation "
								   "  LEFT JOIN pg_namespace collnsp ON "
								   "    collnsp.oid = collnamespace ");

		appendStringInfoString(&buf,
							   "WHERE c.relkind IN ("
							   CppAsString2(RELKIND_RELATION) ","
							   CppAsString2(RELKIND_VIEW) ","
							   CppAsString2(RELKIND_FOREIGN_TABLE) ","
							   CppAsString2(RELKIND_MATVIEW) ","
							   CppAsString2(RELKIND_PARTITIONED_TABLE) ") "
							   "  AND n.nspname = ");
		deparseStringLiteral(&buf, stmt->remote_schema);

		/* Partitions are supported since Postgres 10
		 *
		 * 从 Postgres 10 开始支持分区
		 */
		if (PQserverVersion(conn) >= 100000 &&
			stmt->list_type != FDW_IMPORT_SCHEMA_LIMIT_TO)
			appendStringInfoString(&buf, " AND NOT c.relispartition ");

		/* Apply restrictions for LIMIT TO and EXCEPT
		 *
		 * 应用限制 LIMIT TO 和 EXCEPT
		 */
		if (stmt->list_type == FDW_IMPORT_SCHEMA_LIMIT_TO ||
			stmt->list_type == FDW_IMPORT_SCHEMA_EXCEPT)
		{
			bool		first_item = true;

			appendStringInfoString(&buf, " AND c.relname ");
			if (stmt->list_type == FDW_IMPORT_SCHEMA_EXCEPT)
				appendStringInfoString(&buf, "NOT ");
			appendStringInfoString(&buf, "IN (");

			/* Append list of table names within IN clause
			 *
			 * 在 IN 子句中追加表名列表
			 */
			foreach(lc, stmt->table_list)
			{
				RangeVar   *rv = (RangeVar *) lfirst(lc);

				if (first_item)
					first_item = false;
				else
					appendStringInfoString(&buf, ", ");
				deparseStringLiteral(&buf, rv->relname);
			}
			appendStringInfoChar(&buf, ')');
		}

		/* Append ORDER BY at the end of query to ensure output ordering
		 *
		 * 在查询末尾附加 ORDER BY 以确保输出排序
		 */
		appendStringInfoString(&buf, " ORDER BY c.relname, a.attnum");

		/* Fetch the data
		 *
		 * 获取数据
		 */
		res = pgfdw_exec_query(conn, buf.data, NULL);
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
			pgfdw_report_error(ERROR, res, conn, false, buf.data);

		/* Process results
		 *
		 * 处理结果
		 */
		numrows = PQntuples(res);
		/* note: incrementation of i happens in inner loop's while() test
		 *
		 * 注意： i 的递增发生在内部循环的 while() 测试中
		 */
		for (i = 0; i < numrows;)
		{
			char	   *tablename = PQgetvalue(res, i, 0);
			bool		first_item = true;

			resetStringInfo(&buf);
			appendStringInfo(&buf, "CREATE FOREIGN TABLE %s (\n",
							 quote_identifier(tablename));

			/* Scan all rows for this table
			 *
			 * 扫描该表的所有行
			 */
			do
			{
				char	   *attname;
				char	   *typename;
				char	   *attnotnull;
				char	   *attgenerated;
				char	   *attdefault;
				char	   *collname;
				char	   *collnamespace;

				/* If table has no columns, we'll see nulls here
				 *
				 * 如果表没有列，我们将在此处看到空值
				 */
				if (PQgetisnull(res, i, 1))
					continue;

				attname = PQgetvalue(res, i, 1);
				typename = PQgetvalue(res, i, 2);
				attnotnull = PQgetvalue(res, i, 3);
				attdefault = PQgetisnull(res, i, 4) ? NULL :
					PQgetvalue(res, i, 4);
				attgenerated = PQgetisnull(res, i, 5) ? NULL :
					PQgetvalue(res, i, 5);
				collname = PQgetisnull(res, i, 6) ? NULL :
					PQgetvalue(res, i, 6);
				collnamespace = PQgetisnull(res, i, 7) ? NULL :
					PQgetvalue(res, i, 7);

				if (first_item)
					first_item = false;
				else
					appendStringInfoString(&buf, ",\n");

				/* Print column name and type
				 *
				 * 打印列名称和类型
				 */
				appendStringInfo(&buf, "  %s %s",
								 quote_identifier(attname),
								 typename);

				/*
				 * Add column_name option so that renaming the foreign table's
				 * column doesn't break the association to the underlying
				 * column.
				 *
				 * 添加column_name选项，以便重命名外表的列不会破坏与基础列的关联。
				 */
				appendStringInfoString(&buf, " OPTIONS (column_name ");
				deparseStringLiteral(&buf, attname);
				appendStringInfoChar(&buf, ')');

				/* Add COLLATE if needed
				 *
				 * 如果需要，添加整理
				 */
				if (import_collate && collname != NULL && collnamespace != NULL)
					appendStringInfo(&buf, " COLLATE %s.%s",
									 quote_identifier(collnamespace),
									 quote_identifier(collname));

				/* Add DEFAULT if needed
				 *
				 * 如果需要，添加默认值
				 */
				if (import_default && attdefault != NULL &&
					(!attgenerated || !attgenerated[0]))
					appendStringInfo(&buf, " DEFAULT %s", attdefault);

				/* Add GENERATED if needed
				 *
				 * 如果需要，添加生成
				 */
				if (import_generated && attgenerated != NULL &&
					attgenerated[0] == ATTRIBUTE_GENERATED_STORED)
				{
					Assert(attdefault != NULL);
					appendStringInfo(&buf,
									 " GENERATED ALWAYS AS (%s) STORED",
									 attdefault);
				}

				/* Add NOT NULL if needed
				 *
				 * 如果需要，添加 NOT NULL
				 */
				if (import_not_null && attnotnull[0] == 't')
					appendStringInfoString(&buf, " NOT NULL");
			}
			while (++i < numrows &&
				   strcmp(PQgetvalue(res, i, 0), tablename) == 0);

			/*
			 * Add server name and table-level options.  We specify remote
			 * schema and table name as options (the latter to ensure that
			 * renaming the foreign table doesn't break the association).
			 *
			 * 添加服务器名称和表级选项。  我们指定远程模式和表名称作为选项（后者是为了确保重命名外部表不会破坏关联）。
			 */
			appendStringInfo(&buf, "\n) SERVER %s\nOPTIONS (",
							 quote_identifier(server->servername));

			appendStringInfoString(&buf, "schema_name ");
			deparseStringLiteral(&buf, stmt->remote_schema);
			appendStringInfoString(&buf, ", table_name ");
			deparseStringLiteral(&buf, tablename);

			appendStringInfoString(&buf, ");");

			commands = lappend(commands, pstrdup(buf.data));
		}
	}
	PG_FINALLY();
	{
		PQclear(res);
	}
	PG_END_TRY();

	ReleaseConnection(conn);

	return commands;
}

/*
 * Check if reltarget is safe enough to push down semi-join.  Reltarget is not
 * safe, if it contains references to inner rel relids, which do not belong to
 * outer rel.
 *
 * 检查 reltarget 是否足够安全，可以下推半连接。  如果 Reltarget 包含对不属于外部 rel 的内部 rel relids 的引用，则它是不安全的。
 */
static bool
semijoin_target_ok(PlannerInfo *root, RelOptInfo *joinrel, RelOptInfo *outerrel, RelOptInfo *innerrel)
{
	List	   *vars;
	ListCell   *lc;
	bool		ok = true;

	Assert(joinrel->reltarget);

	vars = pull_var_clause((Node *) joinrel->reltarget->exprs, PVC_INCLUDE_PLACEHOLDERS);

	foreach(lc, vars)
	{
		Var		   *var = (Var *) lfirst(lc);

		if (!IsA(var, Var))
			continue;

		if (bms_is_member(var->varno, innerrel->relids))
		{
			/*
			 * The planner can create semi-join, which refers to inner rel
			 * vars in its target list. However, we deparse semi-join as an
			 * exists() subquery, so can't handle references to inner rel in
			 * the target list.
			 *
			 * 规划器可以创建半连接，它引用其目标列表中的内部相关变量。然而，我们将半连接解析为exists()子查询，因此无法处理对目标列表中内部rel的引用。
			 */
			Assert(!bms_is_member(var->varno, outerrel->relids));
			ok = false;
			break;
		}
	}
	return ok;
}

/*
 * Assess whether the join between inner and outer relations can be pushed down
 * to the foreign server. As a side effect, save information we obtain in this
 * function to PgFdwRelationInfo passed in.
 *
 * 评估内部和外部关系之间的连接是否可以下推到外部服务器。作为副作用，将我们在此函数中获得的信息保存到传入的 PgFdwRelationInfo 中。
 */
static bool
foreign_join_ok(PlannerInfo *root, RelOptInfo *joinrel, JoinType jointype,
				RelOptInfo *outerrel, RelOptInfo *innerrel,
				JoinPathExtraData *extra)
{
	PgFdwRelationInfo *fpinfo;
	PgFdwRelationInfo *fpinfo_o;
	PgFdwRelationInfo *fpinfo_i;
	ListCell   *lc;
	List	   *joinclauses;

	/*
	 * We support pushing down INNER, LEFT, RIGHT, FULL OUTER and SEMI joins.
	 * Constructing queries representing ANTI joins is hard, hence not
	 * considered right now.
	 *
	 * 我们支持下推 INNER、LEFT、RIGHT、FULL OUTER 和 SEMI 连接。构建表示 ANTI 连接的查询很困难，因此现在不考虑。
	 */
	if (jointype != JOIN_INNER && jointype != JOIN_LEFT &&
		jointype != JOIN_RIGHT && jointype != JOIN_FULL &&
		jointype != JOIN_SEMI)
		return false;

	/*
	 * We can't push down semi-join if its reltarget is not safe
	 *
	 * 如果半连接的 reltarget 不安全，我们就无法下推它
	 */
	if ((jointype == JOIN_SEMI) && !semijoin_target_ok(root, joinrel, outerrel, innerrel))
		return false;

	/*
	 * If either of the joining relations is marked as unsafe to pushdown, the
	 * join can not be pushed down.
	 *
	 * 如果任一连接关系被标记为对下推不安全，则该连接不能被下推。
	 */
	fpinfo = (PgFdwRelationInfo *) joinrel->fdw_private;
	fpinfo_o = (PgFdwRelationInfo *) outerrel->fdw_private;
	fpinfo_i = (PgFdwRelationInfo *) innerrel->fdw_private;
	if (!fpinfo_o || !fpinfo_o->pushdown_safe ||
		!fpinfo_i || !fpinfo_i->pushdown_safe)
		return false;

	/*
	 * If joining relations have local conditions, those conditions are
	 * required to be applied before joining the relations. Hence the join can
	 * not be pushed down.
	 *
	 * 如果加入关系有当地条件，则需要在加入关系之前满足这些条件。因此，连接不能被下推。
	 */
	if (fpinfo_o->local_conds || fpinfo_i->local_conds)
		return false;

	/*
	 * Merge FDW options.  We might be tempted to do this after we have deemed
	 * the foreign join to be OK.  But we must do this beforehand so that we
	 * know which quals can be evaluated on the foreign server, which might
	 * depend on shippable_extensions.
	 *
	 * 合并 FDW 选项。  在我们认为外部连接没问题之后，我们可能会想要这样做。  但我们必须事先这样做，以便我们知道可以在外部服务器上评估哪些质量，这可能取决于shippable_extensions。
	 */
	fpinfo->server = fpinfo_o->server;
	merge_fdw_options(fpinfo, fpinfo_o, fpinfo_i);

	/*
	 * Separate restrict list into join quals and pushed-down (other) quals.
	 *
	 * 将限制列表分为连接限定和下推（其他）限定。
	 *
	 * Join quals belonging to an outer join must all be shippable, else we
	 * cannot execute the join remotely.  Add such quals to 'joinclauses'.
	 *
	 * 属于外连接的连接质量必须全部可传送，否则我们无法远程执行连接。  将此类限定词添加到“joinclauses”中。
	 *
	 * Add other quals to fpinfo->remote_conds if they are shippable, else to
	 * fpinfo->local_conds.  In an inner join it's okay to execute conditions
	 * either locally or remotely; the same is true for pushed-down conditions
	 * at an outer join.
	 *
	 * 如果可交付，则将其他质量添加到 fpinfo->remote_conds，否则添加到 fpinfo->local_conds。  在内部联接中，可以在本地或远程执行条件；对于外连接处的下推条件也是如此。
	 *
	 * Note we might return failure after having already scribbled on
	 * fpinfo->remote_conds and fpinfo->local_conds.  That's okay because we
	 * won't consult those lists again if we deem the join unshippable.
	 *
	 * 请注意，在 fpinfo->remote_conds 和 fpinfo->local_conds 上进行书写后，我们可能会返回失败。  没关系，因为如果我们认为连接无法发送，我们就不会再次查阅这些列表。
	 */
	joinclauses = NIL;
	foreach(lc, extra->restrictlist)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
		bool		is_remote_clause = is_foreign_expr(root, joinrel,
													   rinfo->clause);

		if (IS_OUTER_JOIN(jointype) &&
			!RINFO_IS_PUSHED_DOWN(rinfo, joinrel->relids))
		{
			if (!is_remote_clause)
				return false;
			joinclauses = lappend(joinclauses, rinfo);
		}
		else
		{
			if (is_remote_clause)
				fpinfo->remote_conds = lappend(fpinfo->remote_conds, rinfo);
			else
				fpinfo->local_conds = lappend(fpinfo->local_conds, rinfo);
		}
	}

	/*
	 * deparseExplicitTargetList() isn't smart enough to handle anything other
	 * than a Var.  In particular, if there's some PlaceHolderVar that would
	 * need to be evaluated within this join tree (because there's an upper
	 * reference to a quantity that may go to NULL as a result of an outer
	 * join), then we can't try to push the join down because we'll fail when
	 * we get to deparseExplicitTargetList().  However, a PlaceHolderVar that
	 * needs to be evaluated *at the top* of this join tree is OK, because we
	 * can do that locally after fetching the results from the remote side.
	 *
	 * deparseExplicitTargetList() 不够智能，无法处理 Var 以外的任何内容。  特别是，如果需要在此连接树中评估某些 PlaceHolderVar（因为存在对可能因外部连接而变为 NULL 的数量的上层引用），那么我们不能尝试将连接向下推，因为当我们到达 deparseExplicitTargetList() 时我们会失败。  但是，需要在此连接树的“顶部”评估 PlaceHolderVar 是可以的，因为我们可以在从远程端获取结果后在本地执行此操作。
	 */
	foreach(lc, root->placeholder_list)
	{
		PlaceHolderInfo *phinfo = lfirst(lc);
		Relids		relids;

		/* PlaceHolderInfo refers to parent relids, not child relids.
		 *
		 * PlaceHolderInfo 指的是父级 relids，而不是子级 relids。
		 */
		relids = IS_OTHER_REL(joinrel) ?
			joinrel->top_parent_relids : joinrel->relids;

		if (bms_is_subset(phinfo->ph_eval_at, relids) &&
			bms_nonempty_difference(relids, phinfo->ph_eval_at))
			return false;
	}

	/* Save the join clauses, for later use.
	 *
	 * 保存连接子句以供以后使用。
	 */
	fpinfo->joinclauses = joinclauses;

	fpinfo->outerrel = outerrel;
	fpinfo->innerrel = innerrel;
	fpinfo->jointype = jointype;

	/*
	 * By default, both the input relations are not required to be deparsed as
	 * subqueries, but there might be some relations covered by the input
	 * relations that are required to be deparsed as subqueries, so save the
	 * relids of those relations for later use by the deparser.
	 *
	 * 默认情况下，两个输入关系都不需要解析为子查询，但输入关系中可能包含一些需要解析为子查询的关系，因此请保存这些关系的relids以供解析器稍后使用。
	 */
	fpinfo->make_outerrel_subquery = false;
	fpinfo->make_innerrel_subquery = false;
	Assert(bms_is_subset(fpinfo_o->lower_subquery_rels, outerrel->relids));
	Assert(bms_is_subset(fpinfo_i->lower_subquery_rels, innerrel->relids));
	fpinfo->lower_subquery_rels = bms_union(fpinfo_o->lower_subquery_rels,
											fpinfo_i->lower_subquery_rels);
	fpinfo->hidden_subquery_rels = bms_union(fpinfo_o->hidden_subquery_rels,
											 fpinfo_i->hidden_subquery_rels);

	/*
	 * Pull the other remote conditions from the joining relations into join
	 * clauses or other remote clauses (remote_conds) of this relation
	 * wherever possible. This avoids building subqueries at every join step.
	 *
	 * 尽可能将连接关系中的其他远程条件拉入该关系的连接子句或其他远程子句 (remote_conds) 中。这避免了在每个连接步骤中构建子查询。
	 *
	 * For an inner join, clauses from both the relations are added to the
	 * other remote clauses. For LEFT and RIGHT OUTER join, the clauses from
	 * the outer side are added to remote_conds since those can be evaluated
	 * after the join is evaluated. The clauses from inner side are added to
	 * the joinclauses, since they need to be evaluated while constructing the
	 * join.
	 *
	 * 对于内部联接，两个关系中的子句都将添加到其他远程子句中。对于 LEFT 和 RIGHT OUTER 连接，来自外侧的子句将添加到 remote_conds 中，因为可以在评估连接后评估这些子句。来自内侧的子句被添加到连接子句中，因为它们需要在构造连接时进行评估。
	 *
	 * For SEMI-JOIN clauses from inner relation can not be added to
	 * remote_conds, but should be treated as join clauses (as they are
	 * deparsed to EXISTS subquery, where inner relation can be referred). A
	 * list of relation ids, which can't be referred to from higher levels, is
	 * preserved as a hidden_subquery_rels list.
	 *
	 * 对于来自内部关系的 SEMI-JOIN 子句不能添加到remote_conds，但应将其视为连接子句（因为它们被解析为 EXISTS 子查询，其中可以引用内部关系）。无法从更高级别引用的关系 id 列表被保留为hidden_​​subquery_rels 列表。
	 *
	 * For a FULL OUTER JOIN, the other clauses from either relation can not
	 * be added to the joinclauses or remote_conds, since each relation acts
	 * as an outer relation for the other.
	 *
	 * 对于 FULL OUTER JOIN，任一关系中的其他子句都不能添加到 joinclauses 或 remote_conds 中，因为每个关系都充当另一个关系的外部关系。
	 *
	 * The joining sides can not have local conditions, thus no need to test
	 * shippability of the clauses being pulled up.
	 *
	 * 加盟方可以不具备本地条件，因此无需测试所拉条款的可发货性。
	 */
	switch (jointype)
	{
		case JOIN_INNER:
			fpinfo->remote_conds = list_concat(fpinfo->remote_conds,
											   fpinfo_i->remote_conds);
			fpinfo->remote_conds = list_concat(fpinfo->remote_conds,
											   fpinfo_o->remote_conds);
			break;

		case JOIN_LEFT:

			/*
			 * When semi-join is involved in the inner or outer part of the
			 * left join, it's deparsed as a subquery, and we can't refer to
			 * its vars on the upper level.
			 *
			 * 当左连接的内部或外部涉及半连接时，它被解析为子查询，我们无法在上层引用它的变量。
			 */
			if (bms_is_empty(fpinfo_i->hidden_subquery_rels))
				fpinfo->joinclauses = list_concat(fpinfo->joinclauses,
												  fpinfo_i->remote_conds);
			if (bms_is_empty(fpinfo_o->hidden_subquery_rels))
				fpinfo->remote_conds = list_concat(fpinfo->remote_conds,
												   fpinfo_o->remote_conds);
			break;

		case JOIN_RIGHT:

			/*
			 * When semi-join is involved in the inner or outer part of the
			 * right join, it's deparsed as a subquery, and we can't refer to
			 * its vars on the upper level.
			 *
			 * 当右连接的内部或外部涉及半连接时，它被解析为子查询，我们无法在上层引用它的变量。
			 */
			if (bms_is_empty(fpinfo_o->hidden_subquery_rels))
				fpinfo->joinclauses = list_concat(fpinfo->joinclauses,
												  fpinfo_o->remote_conds);
			if (bms_is_empty(fpinfo_i->hidden_subquery_rels))
				fpinfo->remote_conds = list_concat(fpinfo->remote_conds,
												   fpinfo_i->remote_conds);
			break;

		case JOIN_SEMI:
			fpinfo->joinclauses = list_concat(fpinfo->joinclauses,
											  fpinfo_i->remote_conds);
			fpinfo->joinclauses = list_concat(fpinfo->joinclauses,
											  fpinfo->remote_conds);
			fpinfo->remote_conds = list_copy(fpinfo_o->remote_conds);
			fpinfo->hidden_subquery_rels = bms_union(fpinfo->hidden_subquery_rels,
													 innerrel->relids);
			break;

		case JOIN_FULL:

			/*
			 * In this case, if any of the input relations has conditions, we
			 * need to deparse that relation as a subquery so that the
			 * conditions can be evaluated before the join.  Remember it in
			 * the fpinfo of this relation so that the deparser can take
			 * appropriate action.  Also, save the relids of base relations
			 * covered by that relation for later use by the deparser.
			 *
			 * 在这种情况下，如果任何输入关系有条件，我们需要将该关系解析为子查询，以便可以在连接之前评估条件。  将其记在该关系的 fpinfo 中，以便解析器可以采取适当的操作。  另外，保存该关系所涵盖的基本关系的 relids 以供解析器稍后使用。
			 */
			if (fpinfo_o->remote_conds)
			{
				fpinfo->make_outerrel_subquery = true;
				fpinfo->lower_subquery_rels =
					bms_add_members(fpinfo->lower_subquery_rels,
									outerrel->relids);
			}
			if (fpinfo_i->remote_conds)
			{
				fpinfo->make_innerrel_subquery = true;
				fpinfo->lower_subquery_rels =
					bms_add_members(fpinfo->lower_subquery_rels,
									innerrel->relids);
			}
			break;

		default:
			/* Should not happen, we have just checked this above
			 *
			 * 不应该发生，我们刚刚在上面检查过这一点
			 */
			elog(ERROR, "unsupported join type %d", jointype);
	}

	/*
	 * For an inner join, all restrictions can be treated alike. Treating the
	 * pushed down conditions as join conditions allows a top level full outer
	 * join to be deparsed without requiring subqueries.
	 *
	 * 对于内部联接，所有限制都可以同等对待。将下推条件视为连接条件允许解析顶级完整外部连接，而无需子查询。
	 */
	if (jointype == JOIN_INNER)
	{
		Assert(!fpinfo->joinclauses);
		fpinfo->joinclauses = fpinfo->remote_conds;
		fpinfo->remote_conds = NIL;
	}
	else if (jointype == JOIN_LEFT || jointype == JOIN_RIGHT || jointype == JOIN_FULL)
	{
		/*
		 * Conditions, generated from semi-joins, should be evaluated before
		 * LEFT/RIGHT/FULL join.
		 *
		 * 由半连接生成的条件应在 LEFT/RIGHT/FULL 连接之前进行评估。
		 */
		if (!bms_is_empty(fpinfo_o->hidden_subquery_rels))
		{
			fpinfo->make_outerrel_subquery = true;
			fpinfo->lower_subquery_rels = bms_add_members(fpinfo->lower_subquery_rels, outerrel->relids);
		}

		if (!bms_is_empty(fpinfo_i->hidden_subquery_rels))
		{
			fpinfo->make_innerrel_subquery = true;
			fpinfo->lower_subquery_rels = bms_add_members(fpinfo->lower_subquery_rels, innerrel->relids);
		}
	}

	/* Mark that this join can be pushed down safely
	 *
	 * 标记此连接可以安全地下推
	 */
	fpinfo->pushdown_safe = true;

	/* Get user mapping
	 *
	 * 获取用户映射
	 */
	if (fpinfo->use_remote_estimate)
	{
		if (fpinfo_o->use_remote_estimate)
			fpinfo->user = fpinfo_o->user;
		else
			fpinfo->user = fpinfo_i->user;
	}
	else
		fpinfo->user = NULL;

	/*
	 * Set # of retrieved rows and cached relation costs to some negative
	 * value, so that we can detect when they are set to some sensible values,
	 * during one (usually the first) of the calls to estimate_path_cost_size.
	 *
	 * 将检索行数和缓存关系成本设置为某个负值，以便我们可以在一次（通常是第一次）调用estimate_path_cost_size期间检测它们何时设置为某些合理值。
	 */
	fpinfo->retrieved_rows = -1;
	fpinfo->rel_startup_cost = -1;
	fpinfo->rel_total_cost = -1;

	/*
	 * Set the string describing this join relation to be used in EXPLAIN
	 * output of corresponding ForeignScan.  Note that the decoration we add
	 * to the base relation names mustn't include any digits, or it'll confuse
	 * postgresExplainForeignScan.
	 *
	 * 设置描述此连接关系的字符串，用于相应ForeignScan 的EXPLAIN 输出。  请注意，我们添加到基本关系名称的修饰不得包含任何数字，否则会使 postgresExplainForeignScan 感到困惑。
	 */
	fpinfo->relation_name = psprintf("(%s) %s JOIN (%s)",
									 fpinfo_o->relation_name,
									 get_jointype_name(fpinfo->jointype),
									 fpinfo_i->relation_name);

	/*
	 * Set the relation index.  This is defined as the position of this
	 * joinrel in the join_rel_list list plus the length of the rtable list.
	 * Note that since this joinrel is at the end of the join_rel_list list
	 * when we are called, we can get the position by list_length.
	 *
	 * 设置关系索引。  这被定义为该 joinrel 在 join_rel_list 列表中的位置加上 rtable 列表的长度。请注意，由于调用时该 joinrel 位于 join_rel_list 列表的末尾，因此我们可以通过 list_length 获取位置。
	 */
	Assert(fpinfo->relation_index == 0);	/* shouldn't be set yet */
	fpinfo->relation_index =
		list_length(root->parse->rtable) + list_length(root->join_rel_list);

	return true;
}

static void
add_paths_with_pathkeys_for_rel(PlannerInfo *root, RelOptInfo *rel,
								Path *epq_path, List *restrictlist)
{
	List	   *useful_pathkeys_list = NIL; /* List of all pathkeys */
	ListCell   *lc;

	useful_pathkeys_list = get_useful_pathkeys_for_relation(root, rel);

	/*
	 * Before creating sorted paths, arrange for the passed-in EPQ path, if
	 * any, to return columns needed by the parent ForeignScan node so that
	 * they will propagate up through Sort nodes injected below, if necessary.
	 *
	 * 在创建排序路径之前，请安排传入的 EPQ 路径（如果有）返回父foreignscan节点所需的列，以便它们将通过下面注入的排序节点向上传播（如果需要）。
	 */
	if (epq_path != NULL && useful_pathkeys_list != NIL)
	{
		PgFdwRelationInfo *fpinfo = (PgFdwRelationInfo *) rel->fdw_private;
		PathTarget *target = copy_pathtarget(epq_path->pathtarget);

		/* Include columns required for evaluating PHVs in the tlist.
		 *
		 * 在列表中包含评估 PHV 所需的列。
		 */
		add_new_columns_to_pathtarget(target,
									  pull_var_clause((Node *) target->exprs,
													  PVC_RECURSE_PLACEHOLDERS));

		/* Include columns required for evaluating the local conditions.
		 *
		 * 包括评估当地条件所需的列。
		 */
		foreach(lc, fpinfo->local_conds)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

			add_new_columns_to_pathtarget(target,
										  pull_var_clause((Node *) rinfo->clause,
														  PVC_RECURSE_PLACEHOLDERS));
		}

		/*
		 * If we have added any new columns, adjust the tlist of the EPQ path.
		 *
		 * 如果我们添加了任何新列，请调整 EPQ 路径的 tlist。
		 *
		 * Note: the plan created using this path will only be used to execute
		 * EPQ checks, where accuracy of the plan cost and width estimates
		 * would not be important, so we do not do set_pathtarget_cost_width()
		 * for the new pathtarget here.  See also postgresGetForeignPlan().
		 *
		 * Note: the plan created using this path will only be used to execute EPQ checks, where accuracy of the plan cost and width estimates would not be important, so we do not do set_pathtarget_cost_width() for the new pathtarget here.  另请参见 postgresGetForeignPlan()。
		 */
		if (list_length(target->exprs) > list_length(epq_path->pathtarget->exprs))
		{
			/* The EPQ path is a join path, so it is projection-capable.
			 *
			 * EPQ 路径是连接路径，因此它具有投影功能。
			 */
			Assert(is_projection_capable_path(epq_path));

			/*
			 * Use create_projection_path() here, so as to avoid modifying it
			 * in place.
			 *
			 * 这里使用create_projection_path()，以避免就地修改。
			 */
			epq_path = (Path *) create_projection_path(root,
													   rel,
													   epq_path,
													   target);
		}
	}

	/* Create one path for each set of pathkeys we found above.
	 *
	 * 为我们上面找到的每组路径键创建一个路径。
	 */
	foreach(lc, useful_pathkeys_list)
	{
		double		rows;
		int			width;
		int			disabled_nodes;
		Cost		startup_cost;
		Cost		total_cost;
		List	   *useful_pathkeys = lfirst(lc);
		Path	   *sorted_epq_path;

		estimate_path_cost_size(root, rel, NIL, useful_pathkeys, NULL,
								&rows, &width, &disabled_nodes,
								&startup_cost, &total_cost);

		/*
		 * The EPQ path must be at least as well sorted as the path itself, in
		 * case it gets used as input to a mergejoin.
		 *
		 * EPQ 路径必须至少与路径本身一样排序，以防它被用作合并连接的输入。
		 */
		sorted_epq_path = epq_path;
		if (sorted_epq_path != NULL &&
			!pathkeys_contained_in(useful_pathkeys,
								   sorted_epq_path->pathkeys))
			sorted_epq_path = (Path *)
				create_sort_path(root,
								 rel,
								 sorted_epq_path,
								 useful_pathkeys,
								 -1.0);

		if (IS_SIMPLE_REL(rel))
			add_path(rel, (Path *)
					 create_foreignscan_path(root, rel,
											 NULL,
											 rows,
											 disabled_nodes,
											 startup_cost,
											 total_cost,
											 useful_pathkeys,
											 rel->lateral_relids,
											 sorted_epq_path,
											 NIL,	/* no fdw_restrictinfo
													 * list */
											 NIL));
		else
			add_path(rel, (Path *)
					 create_foreign_join_path(root, rel,
											  NULL,
											  rows,
											  disabled_nodes,
											  startup_cost,
											  total_cost,
											  useful_pathkeys,
											  rel->lateral_relids,
											  sorted_epq_path,
											  restrictlist,
											  NIL));
	}
}

/*
 * Parse options from foreign server and apply them to fpinfo.
 *
 * 解析来自外部服务器的选项并将其应用到 f​​pinfo。
 *
 * New options might also require tweaking merge_fdw_options().
 *
 * 新选项可能还需要调整 merge_fdw_options()。
 */
static void
apply_server_options(PgFdwRelationInfo *fpinfo)
{
	ListCell   *lc;

	foreach(lc, fpinfo->server->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "use_remote_estimate") == 0)
			fpinfo->use_remote_estimate = defGetBoolean(def);
		else if (strcmp(def->defname, "fdw_startup_cost") == 0)
			(void) parse_real(defGetString(def), &fpinfo->fdw_startup_cost, 0,
							  NULL);
		else if (strcmp(def->defname, "fdw_tuple_cost") == 0)
			(void) parse_real(defGetString(def), &fpinfo->fdw_tuple_cost, 0,
							  NULL);
		else if (strcmp(def->defname, "extensions") == 0)
			fpinfo->shippable_extensions =
				ExtractExtensionList(defGetString(def), false);
		else if (strcmp(def->defname, "fetch_size") == 0)
			(void) parse_int(defGetString(def), &fpinfo->fetch_size, 0, NULL);
		else if (strcmp(def->defname, "async_capable") == 0)
			fpinfo->async_capable = defGetBoolean(def);
	}
}

/*
 * Parse options from foreign table and apply them to fpinfo.
 *
 * 从外表中解析选项并将其应用到 f​​pinfo。
 *
 * New options might also require tweaking merge_fdw_options().
 *
 * 新选项可能还需要调整 merge_fdw_options()。
 */
static void
apply_table_options(PgFdwRelationInfo *fpinfo)
{
	ListCell   *lc;

	foreach(lc, fpinfo->table->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "use_remote_estimate") == 0)
			fpinfo->use_remote_estimate = defGetBoolean(def);
		else if (strcmp(def->defname, "fetch_size") == 0)
			(void) parse_int(defGetString(def), &fpinfo->fetch_size, 0, NULL);
		else if (strcmp(def->defname, "async_capable") == 0)
			fpinfo->async_capable = defGetBoolean(def);
	}
}

/*
 * Merge FDW options from input relations into a new set of options for a join
 * or an upper rel.
 *
 * 将输入关系中的 FDW 选项合并到连接或上关系的一组新选项中。
 *
 * For a join relation, FDW-specific information about the inner and outer
 * relations is provided using fpinfo_i and fpinfo_o.  For an upper relation,
 * fpinfo_o provides the information for the input relation; fpinfo_i is
 * expected to NULL.
 *
 * 对于连接关系，使用 fpinfo_i 和 fpinfo_o 提供有关内部和外部关系的特定于 FDW 的信息。  对于上层关系，fpinfo_o 提供输入关系的信息； fpinfo_i 预计为 NULL。
 */
static void
merge_fdw_options(PgFdwRelationInfo *fpinfo,
				  const PgFdwRelationInfo *fpinfo_o,
				  const PgFdwRelationInfo *fpinfo_i)
{
	/* We must always have fpinfo_o.
	 *
	 * 我们必须始终拥有 fpinfo_o。
	 */
	Assert(fpinfo_o);

	/* fpinfo_i may be NULL, but if present the servers must both match.
	 *
	 * fpinfo_i 可能为 NULL，但如果存在，服务器必须两者匹配。
	 */
	Assert(!fpinfo_i ||
		   fpinfo_i->server->serverid == fpinfo_o->server->serverid);

	/*
	 * Copy the server specific FDW options.  (For a join, both relations come
	 * from the same server, so the server options should have the same value
	 * for both relations.)
	 *
	 * 复制服务器特定的 FDW 选项。  （对于联接，两个关系都来自同一服务器，因此两个关系的服务器选项应具有相同的值。）
	 */
	fpinfo->fdw_startup_cost = fpinfo_o->fdw_startup_cost;
	fpinfo->fdw_tuple_cost = fpinfo_o->fdw_tuple_cost;
	fpinfo->shippable_extensions = fpinfo_o->shippable_extensions;
	fpinfo->use_remote_estimate = fpinfo_o->use_remote_estimate;
	fpinfo->fetch_size = fpinfo_o->fetch_size;
	fpinfo->async_capable = fpinfo_o->async_capable;

	/* Merge the table level options from either side of the join.
	 *
	 * 合并连接两侧的表级选项。
	 */
	if (fpinfo_i)
	{
		/*
		 * We'll prefer to use remote estimates for this join if any table
		 * from either side of the join is using remote estimates.  This is
		 * most likely going to be preferred since they're already willing to
		 * pay the price of a round trip to get the remote EXPLAIN.  In any
		 * case it's not entirely clear how we might otherwise handle this
		 * best.
		 *
		 * 如果连接任意一侧的任何表正在使用远程估计，我们将更愿意对此连接使用远程估计。  这很可能是首选，因为他们已经愿意支付往返的价格来获得远程解释。  无论如何，目前还不完全清楚我们如何才能最好地处理这个问题。
		 */
		fpinfo->use_remote_estimate = fpinfo_o->use_remote_estimate ||
			fpinfo_i->use_remote_estimate;

		/*
		 * Set fetch size to maximum of the joining sides, since we are
		 * expecting the rows returned by the join to be proportional to the
		 * relation sizes.
		 *
		 * 将获取大小设置为连接边的最大值，因为我们期望连接返回的行与关系大小成比例。
		 */
		fpinfo->fetch_size = Max(fpinfo_o->fetch_size, fpinfo_i->fetch_size);

		/*
		 * We'll prefer to consider this join async-capable if any table from
		 * either side of the join is considered async-capable.  This would be
		 * reasonable because in that case the foreign server would have its
		 * own resources to scan that table asynchronously, and the join could
		 * also be computed asynchronously using the resources.
		 *
		 * 如果连接任意一侧的任何表被视为具有异步功能，我们更愿意考虑此连接具有异步功能。  这是合理的，因为在这种情况下，外部服务器将拥有自己的资源来异步扫描该表，并且也可以使用这些资源异步计算连接。
		 */
		fpinfo->async_capable = fpinfo_o->async_capable ||
			fpinfo_i->async_capable;
	}
}

/*
 * postgresGetForeignJoinPaths
 *		Add possible ForeignPath to joinrel, if join is safe to push down.
 *
 * postgresGetForeignJoinPaths 如果 join 可以安全下推，则将可能的foreignpath 添加到 joinrel。
 */
static void
postgresGetForeignJoinPaths(PlannerInfo *root,
							RelOptInfo *joinrel,
							RelOptInfo *outerrel,
							RelOptInfo *innerrel,
							JoinType jointype,
							JoinPathExtraData *extra)
{
	PgFdwRelationInfo *fpinfo;
	ForeignPath *joinpath;
	double		rows;
	int			width;
	int			disabled_nodes;
	Cost		startup_cost;
	Cost		total_cost;
	Path	   *epq_path;		/* Path to create plan to be executed when
								 * EvalPlanQual gets triggered. */

	/*
	 * Skip if this join combination has been considered already.
	 *
	 * 如果已考虑此连接组合，则跳过。
	 */
	if (joinrel->fdw_private)
		return;

	/*
	 * This code does not work for joins with lateral references, since those
	 * must have parameterized paths, which we don't generate yet.
	 *
	 * 此代码不适用于具有横向引用的连接，因为它们必须具有参数化路径，而我们尚未生成这些路径。
	 */
	if (!bms_is_empty(joinrel->lateral_relids))
		return;

	/*
	 * Create unfinished PgFdwRelationInfo entry which is used to indicate
	 * that the join relation is already considered, so that we won't waste
	 * time in judging safety of join pushdown and adding the same paths again
	 * if found safe. Once we know that this join can be pushed down, we fill
	 * the entry.
	 *
	 * 创建未完成的PgFdwRelationInfo条目，用于指示已经考虑了连接关系，这样我们就不会浪费时间判断连接下推的安全性，如果安全则再次添加相同的路径。一旦我们知道这个连接可以被下推，我们就填充该条目。
	 */
	fpinfo = (PgFdwRelationInfo *) palloc0(sizeof(PgFdwRelationInfo));
	fpinfo->pushdown_safe = false;
	joinrel->fdw_private = fpinfo;
	/* attrs_used is only for base relations.
	 *
	 * attrs_used 仅适用于基础关系。
	 */
	fpinfo->attrs_used = NULL;

	/*
	 * If there is a possibility that EvalPlanQual will be executed, we need
	 * to be able to reconstruct the row using scans of the base relations.
	 * GetExistingLocalJoinPath will find a suitable path for this purpose in
	 * the path list of the joinrel, if one exists.  We must be careful to
	 * call it before adding any ForeignPath, since the ForeignPath might
	 * dominate the only suitable local path available.  We also do it before
	 * calling foreign_join_ok(), since that function updates fpinfo and marks
	 * it as pushable if the join is found to be pushable.
	 *
	 * 如果有可能执行 EvalPlanQual，我们需要能够使用基本关系扫描来重建行。 GetExistingLocalJoinPath 将在 joinrel 的路径列表中找到适合此目的的路径（如果存在）。  在添加任何foreignpath之前我们必须小心地调用它，因为foreignpath可能会支配唯一合适的可用本地路径。  我们也在调用foreign_join_ok()之前执行此操作，因为如果发现连接可推送，该函数会更新fpinfo并将其标记为可推送。
	 */
	if (root->parse->commandType == CMD_DELETE ||
		root->parse->commandType == CMD_UPDATE ||
		root->rowMarks)
	{
		epq_path = GetExistingLocalJoinPath(joinrel);
		if (!epq_path)
		{
			elog(DEBUG3, "could not push down foreign join because a local path suitable for EPQ checks was not found");
			return;
		}
	}
	else
		epq_path = NULL;

	if (!foreign_join_ok(root, joinrel, jointype, outerrel, innerrel, extra))
	{
		/* Free path required for EPQ if we copied one; we don't need it now
		 *
		 * 如果我们复制一个，则 EPQ 需要自由路径；我们现在不需要它
		 */
		if (epq_path)
			pfree(epq_path);
		return;
	}

	/*
	 * Compute the selectivity and cost of the local_conds, so we don't have
	 * to do it over again for each path. The best we can do for these
	 * conditions is to estimate selectivity on the basis of local statistics.
	 * The local conditions are applied after the join has been computed on
	 * the remote side like quals in WHERE clause, so pass jointype as
	 * JOIN_INNER.
	 *
	 * 计算 local_conds 的选择性和成本，这样我们就不必为每条路径重新计算。针对这些情况，我们能做的最好的事情就是根据当地统计数据来估计选择性。本地条件在远程端计算连接后应用，如 WHERE 子句中的 quals，因此将 jointype 作为 JOIN_INNER 传递。
	 */
	fpinfo->local_conds_sel = clauselist_selectivity(root,
													 fpinfo->local_conds,
													 0,
													 JOIN_INNER,
													 NULL);
	cost_qual_eval(&fpinfo->local_conds_cost, fpinfo->local_conds, root);

	/*
	 * If we are going to estimate costs locally, estimate the join clause
	 * selectivity here while we have special join info.
	 *
	 * 如果我们要在本地估计成本，请在我们有特殊连接信息的同时估计此处的连接子句选择性。
	 */
	if (!fpinfo->use_remote_estimate)
		fpinfo->joinclause_sel = clauselist_selectivity(root, fpinfo->joinclauses,
														0, fpinfo->jointype,
														extra->sjinfo);

	/* Estimate costs for bare join relation
	 *
	 * 估计裸连接关系的成本
	 */
	estimate_path_cost_size(root, joinrel, NIL, NIL, NULL,
							&rows, &width, &disabled_nodes,
							&startup_cost, &total_cost);
	/* Now update this information in the joinrel
	 *
	 * 现在在 joinrel 中更新此信息
	 */
	joinrel->rows = rows;
	joinrel->reltarget->width = width;
	fpinfo->rows = rows;
	fpinfo->width = width;
	fpinfo->disabled_nodes = disabled_nodes;
	fpinfo->startup_cost = startup_cost;
	fpinfo->total_cost = total_cost;

	/*
	 * Create a new join path and add it to the joinrel which represents a
	 * join between foreign tables.
	 *
	 * 创建一个新的联接路径并将其添加到代表外部表之间联接的 joinrel 中。
	 */
	joinpath = create_foreign_join_path(root,
										joinrel,
										NULL,	/* default pathtarget */
										rows,
										disabled_nodes,
										startup_cost,
										total_cost,
										NIL,	/* no pathkeys */
										joinrel->lateral_relids,
										epq_path,
										extra->restrictlist,
										NIL);	/* no fdw_private */

	/* Add generated path into joinrel by add_path().
	 *
	 * 通过add_path()将生成的路径添加到joinrel中。
	 */
	add_path(joinrel, (Path *) joinpath);

	/* Consider pathkeys for the join relation
	 *
	 * 考虑连接关系的路径键
	 */
	add_paths_with_pathkeys_for_rel(root, joinrel, epq_path,
									extra->restrictlist);

	/* XXX Consider parameterized paths for the join relation
	 *
	 * XXX 考虑连接关系的参数化路径
	 */
}

/*
 * Assess whether the aggregation, grouping and having operations can be pushed
 * down to the foreign server.  As a side effect, save information we obtain in
 * this function to PgFdwRelationInfo of the input relation.
 *
 * 评估聚合、分组、拥有操作是否可以下推到外部服务器。  作为副作用，将我们在此函数中获得的信息保存到输入关系的 PgFdwRelationInfo 中。
 */
static bool
foreign_grouping_ok(PlannerInfo *root, RelOptInfo *grouped_rel,
					Node *havingQual)
{
	Query	   *query = root->parse;
	PgFdwRelationInfo *fpinfo = (PgFdwRelationInfo *) grouped_rel->fdw_private;
	PathTarget *grouping_target = grouped_rel->reltarget;
	PgFdwRelationInfo *ofpinfo;
	ListCell   *lc;
	int			i;
	List	   *tlist = NIL;

	/* We currently don't support pushing Grouping Sets.
	 *
	 * 我们目前不支持推送分组集。
	 */
	if (query->groupingSets)
		return false;

	/* Get the fpinfo of the underlying scan relation.
	 *
	 * 获取底层扫描关系的fpinfo。
	 */
	ofpinfo = (PgFdwRelationInfo *) fpinfo->outerrel->fdw_private;

	/*
	 * If underlying scan relation has any local conditions, those conditions
	 * are required to be applied before performing aggregation.  Hence the
	 * aggregate cannot be pushed down.
	 *
	 * 如果底层扫描关系有任何本地条件，则需要在执行聚合之前应用这些条件。  因此，总量不能被推低。
	 */
	if (ofpinfo->local_conds)
		return false;

	/*
	 * Examine grouping expressions, as well as other expressions we'd need to
	 * compute, and check whether they are safe to push down to the foreign
	 * server.  All GROUP BY expressions will be part of the grouping target
	 * and thus there is no need to search for them separately.  Add grouping
	 * expressions into target list which will be passed to foreign server.
	 *
	 * 检查分组表达式以及我们需要计算的其他表达式，并检查它们是否可以安全地推送到外部服务器。  所有 GROUP BY 表达式都将成为分组目标的一部分，因此无需单独搜索它们。  将分组表达式添加到目标列表中，该列表将传递到外部服务器。
	 *
	 * A tricky fine point is that we must not put any expression into the
	 * target list that is just a foreign param (that is, something that
	 * deparse.c would conclude has to be sent to the foreign server).  If we
	 * do, the expression will also appear in the fdw_exprs list of the plan
	 * node, and setrefs.c will get confused and decide that the fdw_exprs
	 * entry is actually a reference to the fdw_scan_tlist entry, resulting in
	 * a broken plan.  Somewhat oddly, it's OK if the expression contains such
	 * a node, as long as it's not at top level; then no match is possible.
	 *
	 * 一个棘手的问题是，我们不能将任何只是外部参数的表达式放入目标列表中（也就是说，deparse.c 得出的结论必须发送到外部服务器）。  如果这样做，该表达式也会出现在计划节点的 fdw_exprs 列表中，并且 setrefs.c 会感到困惑，并认为 fdw_exprs 条目实际上是对 fdw_scan_tlist 条目的引用，从而导致计划损坏。  有点奇怪的是，如果表达式包含这样的节点就可以，只要它不在顶层即可；那么不可能有匹配。
	 */
	i = 0;
	foreach(lc, grouping_target->exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc);
		Index		sgref = get_pathtarget_sortgroupref(grouping_target, i);
		ListCell   *l;

		/*
		 * Check whether this expression is part of GROUP BY clause.  Note we
		 * check the whole GROUP BY clause not just processed_groupClause,
		 * because we will ship all of it, cf. appendGroupByClause.
		 *
		 * Check whether this expression is part of GROUP BY clause.  Note we check the whole GROUP BY clause not just processed_groupClause, because we will ship all of it, cf.附加GroupByClause。
		 */
		if (sgref && get_sortgroupref_clause_noerr(sgref, query->groupClause))
		{
			TargetEntry *tle;

			/*
			 * If any GROUP BY expression is not shippable, then we cannot
			 * push down aggregation to the foreign server.
			 *
			 * 如果任何 GROUP BY 表达式不可传送，那么我们就无法将聚合下推到外部服务器。
			 */
			if (!is_foreign_expr(root, grouped_rel, expr))
				return false;

			/*
			 * If it would be a foreign param, we can't put it into the tlist,
			 * so we have to fail.
			 *
			 * 如果它是一个外部参数，我们就不能将它放入 tlist 中，所以我们必须失败。
			 */
			if (is_foreign_param(root, grouped_rel, expr))
				return false;

			/*
			 * Pushable, so add to tlist.  We need to create a TLE for this
			 * expression and apply the sortgroupref to it.  We cannot use
			 * add_to_flat_tlist() here because that avoids making duplicate
			 * entries in the tlist.  If there are duplicate entries with
			 * distinct sortgrouprefs, we have to duplicate that situation in
			 * the output tlist.
			 *
			 * 可推送，因此添加到列表中。  我们需要为此表达式创建一个 TLE 并将 sortgroupref 应用于它。  我们不能在这里使用 add_to_flat_tlist() 因为这可以避免在 tlist 中创建重复的条目。  如果存在具有不同 sortgrouprefs 的重复条目，我们必须在输出 tlist 中复制这种情况。
			 */
			tle = makeTargetEntry(expr, list_length(tlist) + 1, NULL, false);
			tle->ressortgroupref = sgref;
			tlist = lappend(tlist, tle);
		}
		else
		{
			/*
			 * Non-grouping expression we need to compute.  Can we ship it
			 * as-is to the foreign server?
			 *
			 * 我们需要计算非分组表达式。  我们可以将其按原样发送到国外服务器吗？
			 */
			if (is_foreign_expr(root, grouped_rel, expr) &&
				!is_foreign_param(root, grouped_rel, expr))
			{
				/* Yes, so add to tlist as-is; OK to suppress duplicates
				 *
				 * 是的，所以按原样添加到列表中；确定抑制重复项
				 */
				tlist = add_to_flat_tlist(tlist, list_make1(expr));
			}
			else
			{
				/* Not pushable as a whole; extract its Vars and aggregates
				 *
				 * 整体不可推；提取其变量和聚合
				 */
				List	   *aggvars;

				aggvars = pull_var_clause((Node *) expr,
										  PVC_INCLUDE_AGGREGATES);

				/*
				 * If any aggregate expression is not shippable, then we
				 * cannot push down aggregation to the foreign server.  (We
				 * don't have to check is_foreign_param, since that certainly
				 * won't return true for any such expression.)
				 *
				 * 如果任何聚合表达式不可传送，那么我们就无法将聚合下推到外部服务器。  （我们不必检查 is_foreign_param，因为对于任何此类表达式来说，这肯定不会返回 true。）
				 */
				if (!is_foreign_expr(root, grouped_rel, (Expr *) aggvars))
					return false;

				/*
				 * Add aggregates, if any, into the targetlist.  Plain Vars
				 * outside an aggregate can be ignored, because they should be
				 * either same as some GROUP BY column or part of some GROUP
				 * BY expression.  In either case, they are already part of
				 * the targetlist and thus no need to add them again.  In fact
				 * including plain Vars in the tlist when they do not match a
				 * GROUP BY column would cause the foreign server to complain
				 * that the shipped query is invalid.
				 *
				 * 将聚合（如果有）添加到目标列表中。  聚合外部的普通变量可以被忽略，因为它们应该与某些 GROUP BY 列相同或某些 GROUP BY 表达式的一部分。  无论哪种情况，它们都已经是目标列表的一部分，因此无需再次添加它们。  事实上，当它们与 GROUP BY 列不匹配时，在 tlist 中包含普通变量会导致外部服务器抱怨所提供的查询无效。
				 */
				foreach(l, aggvars)
				{
					Expr	   *aggref = (Expr *) lfirst(l);

					if (IsA(aggref, Aggref))
						tlist = add_to_flat_tlist(tlist, list_make1(aggref));
				}
			}
		}

		i++;
	}

	/*
	 * Classify the pushable and non-pushable HAVING clauses and save them in
	 * remote_conds and local_conds of the grouped rel's fpinfo.
	 *
	 * 对可推送和不可推送的HAVING子句进行分类，并将它们保存在分组rel的fpinfo的remote_conds和local_conds中。
	 */
	if (havingQual)
	{
		foreach(lc, (List *) havingQual)
		{
			Expr	   *expr = (Expr *) lfirst(lc);
			RestrictInfo *rinfo;

			/*
			 * Currently, the core code doesn't wrap havingQuals in
			 * RestrictInfos, so we must make our own.
			 *
			 * 目前，核心代码没有将havingQuals包装在RestrictInfos中，所以我们必须自己制作。
			 */
			Assert(!IsA(expr, RestrictInfo));
			rinfo = make_restrictinfo(root,
									  expr,
									  true,
									  false,
									  false,
									  false,
									  root->qual_security_level,
									  grouped_rel->relids,
									  NULL,
									  NULL);
			if (is_foreign_expr(root, grouped_rel, expr))
				fpinfo->remote_conds = lappend(fpinfo->remote_conds, rinfo);
			else
				fpinfo->local_conds = lappend(fpinfo->local_conds, rinfo);
		}
	}

	/*
	 * If there are any local conditions, pull Vars and aggregates from it and
	 * check whether they are safe to pushdown or not.
	 *
	 * 如果有任何本地条件，请从中拉出 Var 和聚合，并检查它们是否可以安全下推。
	 */
	if (fpinfo->local_conds)
	{
		List	   *aggvars = NIL;

		foreach(lc, fpinfo->local_conds)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

			aggvars = list_concat(aggvars,
								  pull_var_clause((Node *) rinfo->clause,
												  PVC_INCLUDE_AGGREGATES));
		}

		foreach(lc, aggvars)
		{
			Expr	   *expr = (Expr *) lfirst(lc);

			/*
			 * If aggregates within local conditions are not safe to push
			 * down, then we cannot push down the query.  Vars are already
			 * part of GROUP BY clause which are checked above, so no need to
			 * access them again here.  Again, we need not check
			 * is_foreign_param for a foreign aggregate.
			 *
			 * 如果局部条件下的聚合不能安全地下推，那么我们就无法下推查询。  变量已经是上面检查过的 GROUP BY 子句的一部分，因此无需在此处再次访问它们。  同样，我们不需要检查 is_foreign_param 的外部聚合。
			 */
			if (IsA(expr, Aggref))
			{
				if (!is_foreign_expr(root, grouped_rel, expr))
					return false;

				tlist = add_to_flat_tlist(tlist, list_make1(expr));
			}
		}
	}

	/* Store generated targetlist
	 *
	 * 存储生成的目标列表
	 */
	fpinfo->grouped_tlist = tlist;

	/* Safe to pushdown
	 *
	 * 安全下推
	 */
	fpinfo->pushdown_safe = true;

	/*
	 * Set # of retrieved rows and cached relation costs to some negative
	 * value, so that we can detect when they are set to some sensible values,
	 * during one (usually the first) of the calls to estimate_path_cost_size.
	 *
	 * 将检索行数和缓存关系成本设置为某个负值，以便我们可以在一次（通常是第一次）调用estimate_path_cost_size期间检测它们何时设置为某些合理值。
	 */
	fpinfo->retrieved_rows = -1;
	fpinfo->rel_startup_cost = -1;
	fpinfo->rel_total_cost = -1;

	/*
	 * Set the string describing this grouped relation to be used in EXPLAIN
	 * output of corresponding ForeignScan.  Note that the decoration we add
	 * to the base relation name mustn't include any digits, or it'll confuse
	 * postgresExplainForeignScan.
	 *
	 * 设置描述此分组关系的字符串，用于相应ForeignScan 的EXPLAIN 输出。  请注意，我们添加到基本关系名称的修饰不得包含任何数字，否则会混淆 postgresExplainForeignScan。
	 */
	fpinfo->relation_name = psprintf("Aggregate on (%s)",
									 ofpinfo->relation_name);

	return true;
}

/*
 * postgresGetForeignUpperPaths
 *		Add paths for post-join operations like aggregation, grouping etc. if
 *		corresponding operations are safe to push down.
 *
 * postgresGetForeignUpperPaths 如果相应的操作可以安全下推，则为聚合、分组等后连接操作添加路径。
 */
static void
postgresGetForeignUpperPaths(PlannerInfo *root, UpperRelationKind stage,
							 RelOptInfo *input_rel, RelOptInfo *output_rel,
							 void *extra)
{
	PgFdwRelationInfo *fpinfo;

	/*
	 * If input rel is not safe to pushdown, then simply return as we cannot
	 * perform any post-join operations on the foreign server.
	 *
	 * 如果输入 rel 不能安全下推，则只需返回，因为我们无法在外部服务器上执行任何后连接操作。
	 */
	if (!input_rel->fdw_private ||
		!((PgFdwRelationInfo *) input_rel->fdw_private)->pushdown_safe)
		return;

	/* Ignore stages we don't support; and skip any duplicate calls.
	 *
	 * 忽略我们不支持的阶段；并跳过任何重复的调用。
	 */
	if ((stage != UPPERREL_GROUP_AGG &&
		 stage != UPPERREL_ORDERED &&
		 stage != UPPERREL_FINAL) ||
		output_rel->fdw_private)
		return;

	fpinfo = (PgFdwRelationInfo *) palloc0(sizeof(PgFdwRelationInfo));
	fpinfo->pushdown_safe = false;
	fpinfo->stage = stage;
	output_rel->fdw_private = fpinfo;

	switch (stage)
	{
		case UPPERREL_GROUP_AGG:
			add_foreign_grouping_paths(root, input_rel, output_rel,
									   (GroupPathExtraData *) extra);
			break;
		case UPPERREL_ORDERED:
			add_foreign_ordered_paths(root, input_rel, output_rel);
			break;
		case UPPERREL_FINAL:
			add_foreign_final_paths(root, input_rel, output_rel,
									(FinalPathExtraData *) extra);
			break;
		default:
			elog(ERROR, "unexpected upper relation: %d", (int) stage);
			break;
	}
}

/*
 * add_foreign_grouping_paths
 *		Add foreign path for grouping and/or aggregation.
 *
 * add_foreign_grouping_paths 添加用于分组和/或聚合的外部路径。
 *
 * Given input_rel represents the underlying scan.  The paths are added to the
 * given grouped_rel.
 *
 * 给定的 input_rel 表示底层扫描。  路径将添加到给定的 grouped_rel 中。
 */
static void
add_foreign_grouping_paths(PlannerInfo *root, RelOptInfo *input_rel,
						   RelOptInfo *grouped_rel,
						   GroupPathExtraData *extra)
{
	Query	   *parse = root->parse;
	PgFdwRelationInfo *ifpinfo = input_rel->fdw_private;
	PgFdwRelationInfo *fpinfo = grouped_rel->fdw_private;
	ForeignPath *grouppath;
	double		rows;
	int			width;
	int			disabled_nodes;
	Cost		startup_cost;
	Cost		total_cost;

	/* Nothing to be done, if there is no grouping or aggregation required.
	 *
	 * 如果不需要分组或聚合，则无需执行任何操作。
	 */
	if (!parse->groupClause && !parse->groupingSets && !parse->hasAggs &&
		!root->hasHavingQual)
		return;

	Assert(extra->patype == PARTITIONWISE_AGGREGATE_NONE ||
		   extra->patype == PARTITIONWISE_AGGREGATE_FULL);

	/* save the input_rel as outerrel in fpinfo
	 *
	 * 将 input_rel 保存为 fpinfo 中的outerrel
	 */
	fpinfo->outerrel = input_rel;

	/*
	 * Copy foreign table, foreign server, user mapping, FDW options etc.
	 * details from the input relation's fpinfo.
	 *
	 * 从输入关系的 fpinfo 中复制外部表、外部服务器、用户映射、FDW 选项等详细信息。
	 */
	fpinfo->table = ifpinfo->table;
	fpinfo->server = ifpinfo->server;
	fpinfo->user = ifpinfo->user;
	merge_fdw_options(fpinfo, ifpinfo, NULL);

	/*
	 * Assess if it is safe to push down aggregation and grouping.
	 *
	 * 评估下推聚合和分组是否安全。
	 *
	 * Use HAVING qual from extra. In case of child partition, it will have
	 * translated Vars.
	 *
	 * 使用 Extra 中的 HAVING qual。如果是子分区，它将具有翻译后的变量。
	 */
	if (!foreign_grouping_ok(root, grouped_rel, extra->havingQual))
		return;

	/*
	 * Compute the selectivity and cost of the local_conds, so we don't have
	 * to do it over again for each path.  (Currently we create just a single
	 * path here, but in future it would be possible that we build more paths
	 * such as pre-sorted paths as in postgresGetForeignPaths and
	 * postgresGetForeignJoinPaths.)  The best we can do for these conditions
	 * is to estimate selectivity on the basis of local statistics.
	 *
	 * 计算 local_conds 的选择性和成本，这样我们就不必为每条路径重新计算。  (Currently we create just a single path here, but in future it would be possible that we build more paths such as pre-sorted paths as in postgresGetForeignPaths and postgresGetForeignJoinPaths.)  The best we can do for these conditions is to estimate selectivity on the basis of local statistics.
	 */
	fpinfo->local_conds_sel = clauselist_selectivity(root,
													 fpinfo->local_conds,
													 0,
													 JOIN_INNER,
													 NULL);

	cost_qual_eval(&fpinfo->local_conds_cost, fpinfo->local_conds, root);

	/* Estimate the cost of push down
	 *
	 * 估算下推成本
	 */
	estimate_path_cost_size(root, grouped_rel, NIL, NIL, NULL,
							&rows, &width, &disabled_nodes,
							&startup_cost, &total_cost);

	/* Now update this information in the fpinfo
	 *
	 * 现在在 fpinfo 中更新此信息
	 */
	fpinfo->rows = rows;
	fpinfo->width = width;
	fpinfo->disabled_nodes = disabled_nodes;
	fpinfo->startup_cost = startup_cost;
	fpinfo->total_cost = total_cost;

	/* Create and add foreign path to the grouping relation.
	 *
	 * 创建外部路径并将其添加到分组关系中。
	 */
	grouppath = create_foreign_upper_path(root,
										  grouped_rel,
										  grouped_rel->reltarget,
										  rows,
										  disabled_nodes,
										  startup_cost,
										  total_cost,
										  NIL,	/* no pathkeys */
										  NULL,
										  NIL,	/* no fdw_restrictinfo list */
										  NIL); /* no fdw_private */

	/* Add generated path into grouped_rel by add_path().
	 *
	 * 通过add_path()将生成的路径添加到grouped_rel中。
	 */
	add_path(grouped_rel, (Path *) grouppath);
}

/*
 * add_foreign_ordered_paths
 *		Add foreign paths for performing the final sort remotely.
 *
 * add_foreign_ordered_pa​​ths 添加外部路径以远程执行最终排序。
 *
 * Given input_rel contains the source-data Paths.  The paths are added to the
 * given ordered_rel.
 *
 * 给定的 input_rel 包含源数据路径。  路径将添加到给定的ordered_rel中。
 */
static void
add_foreign_ordered_paths(PlannerInfo *root, RelOptInfo *input_rel,
						  RelOptInfo *ordered_rel)
{
	Query	   *parse = root->parse;
	PgFdwRelationInfo *ifpinfo = input_rel->fdw_private;
	PgFdwRelationInfo *fpinfo = ordered_rel->fdw_private;
	PgFdwPathExtraData *fpextra;
	double		rows;
	int			width;
	int			disabled_nodes;
	Cost		startup_cost;
	Cost		total_cost;
	List	   *fdw_private;
	ForeignPath *ordered_path;
	ListCell   *lc;

	/* Shouldn't get here unless the query has ORDER BY
	 *
	 * 除非查询有 ORDER BY，否则不应到达此处
	 */
	Assert(parse->sortClause);

	/* We don't support cases where there are any SRFs in the targetlist
	 *
	 * 我们不支持目标列表中存在任何 SRF 的情况
	 */
	if (parse->hasTargetSRFs)
		return;

	/* Save the input_rel as outerrel in fpinfo
	 *
	 * 将 input_rel 保存为 fpinfo 中的outerrel
	 */
	fpinfo->outerrel = input_rel;

	/*
	 * Copy foreign table, foreign server, user mapping, FDW options etc.
	 * details from the input relation's fpinfo.
	 *
	 * 从输入关系的 fpinfo 中复制外部表、外部服务器、用户映射、FDW 选项等详细信息。
	 */
	fpinfo->table = ifpinfo->table;
	fpinfo->server = ifpinfo->server;
	fpinfo->user = ifpinfo->user;
	merge_fdw_options(fpinfo, ifpinfo, NULL);

	/*
	 * If the input_rel is a base or join relation, we would already have
	 * considered pushing down the final sort to the remote server when
	 * creating pre-sorted foreign paths for that relation, because the
	 * query_pathkeys is set to the root->sort_pathkeys in that case (see
	 * standard_qp_callback()).
	 *
	 * 如果 input_rel 是基关系或连接关系，则在为该关系创建预排序的外部路径时，我们已经考虑将最终排序推送到远程服务器，因为在这种情况下，query_pathkeys 设置为 root->sort_pathkeys（请参阅 standard_qp_callback()）。
	 */
	if (input_rel->reloptkind == RELOPT_BASEREL ||
		input_rel->reloptkind == RELOPT_JOINREL)
	{
		Assert(root->query_pathkeys == root->sort_pathkeys);

		/* Safe to push down if the query_pathkeys is safe to push down
		 *
		 * 如果 query_pathkeys 可以安全下推，则可以安全下推
		 */
		fpinfo->pushdown_safe = ifpinfo->qp_is_pushdown_safe;

		return;
	}

	/* The input_rel should be a grouping relation
	 *
	 * input_rel 应该是分组关系
	 */
	Assert(input_rel->reloptkind == RELOPT_UPPER_REL &&
		   ifpinfo->stage == UPPERREL_GROUP_AGG);

	/*
	 * We try to create a path below by extending a simple foreign path for
	 * the underlying grouping relation to perform the final sort remotely,
	 * which is stored into the fdw_private list of the resulting path.
	 *
	 * 我们尝试通过扩展底层分组关系的简单外部路径来创建下面的路径，以远程执行最终排序，该排序存储在结果路径的 fdw_private 列表中。
	 */

	/* Assess if it is safe to push down the final sort
	 *
	 * 评估下推最终排序是否安全
	 */
	foreach(lc, root->sort_pathkeys)
	{
		PathKey    *pathkey = (PathKey *) lfirst(lc);
		EquivalenceClass *pathkey_ec = pathkey->pk_eclass;

		/*
		 * is_foreign_expr would detect volatile expressions as well, but
		 * checking ec_has_volatile here saves some cycles.
		 *
		 * is_foreign_expr 也会检测易失性表达式，但在这里检查 ec_has_volatile 可以节省一些周期。
		 */
		if (pathkey_ec->ec_has_volatile)
			return;

		/*
		 * Can't push down the sort if pathkey's opfamily is not shippable.
		 *
		 * 如果 pathkey 的 opfamily 不可发货，则无法下推排序。
		 */
		if (!is_shippable(pathkey->pk_opfamily, OperatorFamilyRelationId,
						  fpinfo))
			return;

		/*
		 * The EC must contain a shippable EM that is computed in input_rel's
		 * reltarget, else we can't push down the sort.
		 *
		 * EC 必须包含在 input_rel 的 reltarget 中计算的可交付 EM，否则我们无法下推排序。
		 */
		if (find_em_for_rel_target(root,
								   pathkey_ec,
								   input_rel) == NULL)
			return;
	}

	/* Safe to push down
	 *
	 * 安全向下推
	 */
	fpinfo->pushdown_safe = true;

	/* Construct PgFdwPathExtraData
	 *
	 * 构造 PgFdwPathExtraData
	 */
	fpextra = (PgFdwPathExtraData *) palloc0(sizeof(PgFdwPathExtraData));
	fpextra->target = root->upper_targets[UPPERREL_ORDERED];
	fpextra->has_final_sort = true;

	/* Estimate the costs of performing the final sort remotely
	 *
	 * 估算远程执行最终排序的成本
	 */
	estimate_path_cost_size(root, input_rel, NIL, root->sort_pathkeys, fpextra,
							&rows, &width, &disabled_nodes,
							&startup_cost, &total_cost);

	/*
	 * Build the fdw_private list that will be used by postgresGetForeignPlan.
	 * Items in the list must match order in enum FdwPathPrivateIndex.
	 *
	 * 构建将由 postgresGetForeignPlan 使用的 fdw_private 列表。列表中的项目必须与枚举 FdwPathPrivateIndex 中的顺序匹配。
	 */
	fdw_private = list_make2(makeBoolean(true), makeBoolean(false));

	/* Create foreign ordering path
	 *
	 * 创建国外订购路径
	 */
	ordered_path = create_foreign_upper_path(root,
											 input_rel,
											 root->upper_targets[UPPERREL_ORDERED],
											 rows,
											 disabled_nodes,
											 startup_cost,
											 total_cost,
											 root->sort_pathkeys,
											 NULL,	/* no extra plan */
											 NIL,	/* no fdw_restrictinfo
													 * list */
											 fdw_private);

	/* and add it to the ordered_rel
	 *
	 * 并将其添加到ordered_rel中
	 */
	add_path(ordered_rel, (Path *) ordered_path);
}

/*
 * add_foreign_final_paths
 *		Add foreign paths for performing the final processing remotely.
 *
 * add_foreign_final_paths 添加用于远程执行最终处理的外部路径。
 *
 * Given input_rel contains the source-data Paths.  The paths are added to the
 * given final_rel.
 *
 * 给定的 input_rel 包含源数据路径。  路径将添加到给定的 Final_rel 中。
 */
static void
add_foreign_final_paths(PlannerInfo *root, RelOptInfo *input_rel,
						RelOptInfo *final_rel,
						FinalPathExtraData *extra)
{
	Query	   *parse = root->parse;
	PgFdwRelationInfo *ifpinfo = (PgFdwRelationInfo *) input_rel->fdw_private;
	PgFdwRelationInfo *fpinfo = (PgFdwRelationInfo *) final_rel->fdw_private;
	bool		has_final_sort = false;
	List	   *pathkeys = NIL;
	PgFdwPathExtraData *fpextra;
	bool		save_use_remote_estimate = false;
	double		rows;
	int			width;
	int			disabled_nodes;
	Cost		startup_cost;
	Cost		total_cost;
	List	   *fdw_private;
	ForeignPath *final_path;

	/*
	 * Currently, we only support this for SELECT commands
	 *
	 * 目前，我们仅支持 SELECT 命令
	 */
	if (parse->commandType != CMD_SELECT)
		return;

	/*
	 * No work if there is no FOR UPDATE/SHARE clause and if there is no need
	 * to add a LIMIT node
	 *
	 * 如果没有 FOR UPDATE/SHARE 子句并且不需要添加 LIMIT 节点，则不起作用
	 */
	if (!parse->rowMarks && !extra->limit_needed)
		return;

	/* We don't support cases where there are any SRFs in the targetlist
	 *
	 * 我们不支持目标列表中存在任何 SRF 的情况
	 */
	if (parse->hasTargetSRFs)
		return;

	/* Save the input_rel as outerrel in fpinfo
	 *
	 * 将 input_rel 保存为 fpinfo 中的outerrel
	 */
	fpinfo->outerrel = input_rel;

	/*
	 * Copy foreign table, foreign server, user mapping, FDW options etc.
	 * details from the input relation's fpinfo.
	 *
	 * 从输入关系的 fpinfo 中复制外部表、外部服务器、用户映射、FDW 选项等详细信息。
	 */
	fpinfo->table = ifpinfo->table;
	fpinfo->server = ifpinfo->server;
	fpinfo->user = ifpinfo->user;
	merge_fdw_options(fpinfo, ifpinfo, NULL);

	/*
	 * If there is no need to add a LIMIT node, there might be a ForeignPath
	 * in the input_rel's pathlist that implements all behavior of the query.
	 * Note: we would already have accounted for the query's FOR UPDATE/SHARE
	 * (if any) before we get here.
	 *
	 * 如果不需要添加LIMIT节点，则input_rel的路径列表中可能有一个ForeignPath，它实现了查询的所有行为。注意：在我们到达这里之前，我们已经考虑了查询的 FOR UPDATE/SHARE（如果有）。
	 */
	if (!extra->limit_needed)
	{
		ListCell   *lc;

		Assert(parse->rowMarks);

		/*
		 * Grouping and aggregation are not supported with FOR UPDATE/SHARE,
		 * so the input_rel should be a base, join, or ordered relation; and
		 * if it's an ordered relation, its input relation should be a base or
		 * join relation.
		 *
		 * FOR UPDATE/SHARE 不支持分组和聚合，因此 input_rel 应该是基关系、连接关系或有序关系；如果它是有序关系，则其输入关系应该是基关系或连接关系。
		 */
		Assert(input_rel->reloptkind == RELOPT_BASEREL ||
			   input_rel->reloptkind == RELOPT_JOINREL ||
			   (input_rel->reloptkind == RELOPT_UPPER_REL &&
				ifpinfo->stage == UPPERREL_ORDERED &&
				(ifpinfo->outerrel->reloptkind == RELOPT_BASEREL ||
				 ifpinfo->outerrel->reloptkind == RELOPT_JOINREL)));

		foreach(lc, input_rel->pathlist)
		{
			Path	   *path = (Path *) lfirst(lc);

			/*
			 * apply_scanjoin_target_to_paths() uses create_projection_path()
			 * to adjust each of its input paths if needed, whereas
			 * create_ordered_paths() uses apply_projection_to_path() to do
			 * that.  So the former might have put a ProjectionPath on top of
			 * the ForeignPath; look through ProjectionPath and see if the
			 * path underneath it is ForeignPath.
			 *
			 * apply_scanjoin_target_to_paths() 使用 create_projection_path() 来调整其每个输入路径（如果需要），而 create_ordered_pa​​ths() 使用 apply_projection_to_path() 来执行此操作。  所以前者可能在ForeignPath之上放置了一个ProjectionPath；查看ProjectionPath，看看它下面的路径是否是ForeignPath。
			 */
			if (IsA(path, ForeignPath) ||
				(IsA(path, ProjectionPath) &&
				 IsA(((ProjectionPath *) path)->subpath, ForeignPath)))
			{
				/*
				 * Create foreign final path; this gets rid of a
				 * no-longer-needed outer plan (if any), which makes the
				 * EXPLAIN output look cleaner
				 *
				 * 创建国外最终路径；这消除了不再需要的外部计划（如果有的话），这使得 EXPLAIN 输出看起来更干净
				 */
				final_path = create_foreign_upper_path(root,
													   path->parent,
													   path->pathtarget,
													   path->rows,
													   path->disabled_nodes,
													   path->startup_cost,
													   path->total_cost,
													   path->pathkeys,
													   NULL,	/* no extra plan */
													   NIL, /* no fdw_restrictinfo
															 * list */
													   NIL);	/* no fdw_private */

				/* and add it to the final_rel
				 *
				 * 并将其添加到 Final_rel
				 */
				add_path(final_rel, (Path *) final_path);

				/* Safe to push down
				 *
				 * 安全向下推
				 */
				fpinfo->pushdown_safe = true;

				return;
			}
		}

		/*
		 * If we get here it means no ForeignPaths; since we would already
		 * have considered pushing down all operations for the query to the
		 * remote server, give up on it.
		 *
		 * 如果我们到达这里就意味着没有ForeignPaths；因为我们已经考虑过将查询的所有操作推送到远程服务器，所以放弃它。
		 */
		return;
	}

	Assert(extra->limit_needed);

	/*
	 * If the input_rel is an ordered relation, replace the input_rel with its
	 * input relation
	 *
	 * 如果 input_rel 是有序关系，则将 input_rel 替换为其输入关系
	 */
	if (input_rel->reloptkind == RELOPT_UPPER_REL &&
		ifpinfo->stage == UPPERREL_ORDERED)
	{
		input_rel = ifpinfo->outerrel;
		ifpinfo = (PgFdwRelationInfo *) input_rel->fdw_private;
		has_final_sort = true;
		pathkeys = root->sort_pathkeys;
	}

	/* The input_rel should be a base, join, or grouping relation
	 *
	 * input_rel 应该是基、连接或分组关系
	 */
	Assert(input_rel->reloptkind == RELOPT_BASEREL ||
		   input_rel->reloptkind == RELOPT_JOINREL ||
		   (input_rel->reloptkind == RELOPT_UPPER_REL &&
			ifpinfo->stage == UPPERREL_GROUP_AGG));

	/*
	 * We try to create a path below by extending a simple foreign path for
	 * the underlying base, join, or grouping relation to perform the final
	 * sort (if has_final_sort) and the LIMIT restriction remotely, which is
	 * stored into the fdw_private list of the resulting path.  (We
	 * re-estimate the costs of sorting the underlying relation, if
	 * has_final_sort.)
	 *
	 * 我们尝试通过扩展底层基、连接或分组关系的简单外部路径来创建下面的路径，以远程执行最终排序（如果 has_final_sort）和 LIMIT 限制，该限制存储在结果路径的 fdw_private 列表中。  （如果 has_final_sort，我们重新估计对底层关系进行排序的成本。）
	 */

	/*
	 * Assess if it is safe to push down the LIMIT and OFFSET to the remote
	 * server
	 *
	 * 评估将 LIMIT 和 OFFSET 下推到远程服务器是否安全
	 */

	/*
	 * If the underlying relation has any local conditions, the LIMIT/OFFSET
	 * cannot be pushed down.
	 *
	 * 如果底层关系有任何局部条件，则无法下推 LIMIT/OFFSET。
	 */
	if (ifpinfo->local_conds)
		return;

	/*
	 * If the query has FETCH FIRST .. WITH TIES, 1) it must have ORDER BY as
	 * well, which is used to determine which additional rows tie for the last
	 * place in the result set, and 2) ORDER BY must already have been
	 * determined to be safe to push down before we get here.  So in that case
	 * the FETCH clause is safe to push down with ORDER BY if the remote
	 * server is v13 or later, but if not, the remote query will fail entirely
	 * for lack of support for it.  Since we do not currently have a way to do
	 * a remote-version check (without accessing the remote server), disable
	 * pushing the FETCH clause for now.
	 *
	 * 如果查询有 FETCH FIRST ..WITH TIES，1) 它也必须有 ORDER BY，它用于确定哪些附加行与结果集中的最后一个位置相关，2) 在我们到达这里之前，必须已经确定 ORDER BY 可以安全地下推。  因此，在这种情况下，如果远程服务器是 v13 或更高版本，则 FETCH 子句可以安全地使用 ORDER BY 下推，但如果不是，则远程查询将因缺乏对它的支持而完全失败。  由于我们目前没有办法进行远程版本检查（无需访问远程服务器），因此暂时禁用推送 FETCH 子句。
	 */
	if (parse->limitOption == LIMIT_OPTION_WITH_TIES)
		return;

	/*
	 * Also, the LIMIT/OFFSET cannot be pushed down, if their expressions are
	 * not safe to remote.
	 *
	 * 此外，如果 LIMIT/OFFSET 的表达式对于远程不安全，则无法将其下推。
	 */
	if (!is_foreign_expr(root, input_rel, (Expr *) parse->limitOffset) ||
		!is_foreign_expr(root, input_rel, (Expr *) parse->limitCount))
		return;

	/* Safe to push down
	 *
	 * 安全向下推
	 */
	fpinfo->pushdown_safe = true;

	/* Construct PgFdwPathExtraData
	 *
	 * 构造 PgFdwPathExtraData
	 */
	fpextra = (PgFdwPathExtraData *) palloc0(sizeof(PgFdwPathExtraData));
	fpextra->target = root->upper_targets[UPPERREL_FINAL];
	fpextra->has_final_sort = has_final_sort;
	fpextra->has_limit = extra->limit_needed;
	fpextra->limit_tuples = extra->limit_tuples;
	fpextra->count_est = extra->count_est;
	fpextra->offset_est = extra->offset_est;

	/*
	 * Estimate the costs of performing the final sort and the LIMIT
	 * restriction remotely.  If has_final_sort is false, we wouldn't need to
	 * execute EXPLAIN anymore if use_remote_estimate, since the costs can be
	 * roughly estimated using the costs we already have for the underlying
	 * relation, in the same way as when use_remote_estimate is false.  Since
	 * it's pretty expensive to execute EXPLAIN, force use_remote_estimate to
	 * false in that case.
	 *
	 * 估计远程执行最终排序和 LIMIT 限制的成本。  如果 has_final_sort 为 false，则如果 use_remote_estimate 则不再需要执行 EXPLAIN，因为可以使用我们已有的基础关系成本来粗略估计成本，与 use_remote_estimate 为 false 时的方式相同。  由于执行 EXPLAIN 的成本相当高，因此在这种情况下强制 use_remote_estimate 设置为 false。
	 */
	if (!fpextra->has_final_sort)
	{
		save_use_remote_estimate = ifpinfo->use_remote_estimate;
		ifpinfo->use_remote_estimate = false;
	}
	estimate_path_cost_size(root, input_rel, NIL, pathkeys, fpextra,
							&rows, &width, &disabled_nodes,
							&startup_cost, &total_cost);
	if (!fpextra->has_final_sort)
		ifpinfo->use_remote_estimate = save_use_remote_estimate;

	/*
	 * Build the fdw_private list that will be used by postgresGetForeignPlan.
	 * Items in the list must match order in enum FdwPathPrivateIndex.
	 *
	 * 构建将由 postgresGetForeignPlan 使用的 fdw_private 列表。列表中的项目必须与枚举 FdwPathPrivateIndex 中的顺序匹配。
	 */
	fdw_private = list_make2(makeBoolean(has_final_sort),
							 makeBoolean(extra->limit_needed));

	/*
	 * Create foreign final path; this gets rid of a no-longer-needed outer
	 * plan (if any), which makes the EXPLAIN output look cleaner
	 *
	 * 创建国外最终路径；这消除了不再需要的外部计划（如果有的话），这使得 EXPLAIN 输出看起来更干净
	 */
	final_path = create_foreign_upper_path(root,
										   input_rel,
										   root->upper_targets[UPPERREL_FINAL],
										   rows,
										   disabled_nodes,
										   startup_cost,
										   total_cost,
										   pathkeys,
										   NULL,	/* no extra plan */
										   NIL, /* no fdw_restrictinfo list */
										   fdw_private);

	/* and add it to the final_rel
	 *
	 * 并将其添加到 Final_rel
	 */
	add_path(final_rel, (Path *) final_path);
}

/*
 * postgresIsForeignPathAsyncCapable
 *		Check whether a given ForeignPath node is async-capable.
 *
 * postgresIsForeignPathAsyncCapable 检查给定的ForeignPath 节点是否具有异步功能。
 */
static bool
postgresIsForeignPathAsyncCapable(ForeignPath *path)
{
	RelOptInfo *rel = ((Path *) path)->parent;
	PgFdwRelationInfo *fpinfo = (PgFdwRelationInfo *) rel->fdw_private;

	return fpinfo->async_capable;
}

/*
 * postgresForeignAsyncRequest
 *		Asynchronously request next tuple from a foreign PostgreSQL table.
 *
 * postgresForeignAsyncRequest 从外部 PostgreSQL 表异步请求下一个元组。
 */
static void
postgresForeignAsyncRequest(AsyncRequest *areq)
{
	produce_tuple_asynchronously(areq, true);
}

/*
 * postgresForeignAsyncConfigureWait
 *		Configure a file descriptor event for which we wish to wait.
 *
 * postgresForeignAsyncConfigureWait 配置我们希望等待的文件描述符事件。
 */
static void
postgresForeignAsyncConfigureWait(AsyncRequest *areq)
{
	ForeignScanState *node = (ForeignScanState *) areq->requestee;
	PgFdwScanState *fsstate = (PgFdwScanState *) node->fdw_state;
	AsyncRequest *pendingAreq = fsstate->conn_state->pendingAreq;
	AppendState *requestor = (AppendState *) areq->requestor;
	WaitEventSet *set = requestor->as_eventset;

	/* This should not be called unless callback_pending
	 *
	 * 除非callback_pending，否则不应调用此函数
	 */
	Assert(areq->callback_pending);

	/*
	 * If process_pending_request() has been invoked on the given request
	 * before we get here, we might have some tuples already; in which case
	 * complete the request
	 *
	 * 如果在我们到达这里之前已经对给定的请求调用了 process_pending_request() ，那么我们可能已经有了一些元组；在这种情况下完成请求
	 */
	if (fsstate->next_tuple < fsstate->num_tuples)
	{
		complete_pending_request(areq);
		if (areq->request_complete)
			return;
		Assert(areq->callback_pending);
	}

	/* We must have run out of tuples
	 *
	 * 我们肯定已经用完了元组
	 */
	Assert(fsstate->next_tuple >= fsstate->num_tuples);

	/* The core code would have registered postmaster death event
	 *
	 * 核心代码将注册邮政局长死亡事件
	 */
	Assert(GetNumRegisteredWaitEvents(set) >= 1);

	/* Begin an asynchronous data fetch if not already done
	 *
	 * 如果尚未完成，则开始异步数据获取
	 */
	if (!pendingAreq)
		fetch_more_data_begin(areq);
	else if (pendingAreq->requestor != areq->requestor)
	{
		/*
		 * This is the case when the in-process request was made by another
		 * Append.  Note that it might be useless to process the request made
		 * by that Append, because the query might not need tuples from that
		 * Append anymore; so we avoid processing it to begin a fetch for the
		 * given request if possible.  If there are any child subplans of the
		 * same parent that are ready for new requests, skip the given
		 * request.  Likewise, if there are any configured events other than
		 * the postmaster death event, skip it.  Otherwise, process the
		 * in-process request, then begin a fetch to configure the event
		 * below, because we might otherwise end up with no configured events
		 * other than the postmaster death event.
		 *
		 * 当正在处理的请求是由另一个 Append 发出时就会出现这种情况。  请注意，处理该 Append 发出的请求可能是无用的，因为查询可能不再需要该 Append 中的元组；因此，如果可能的话，我们避免处理它来开始获取给定的请求。  如果同一父计划的任何子计划已准备好接受新请求，请跳过给定的请求。  同样，如果除了 postmaster 死亡事件之外还有任何已配置的事件，请跳过它。  否则，处理进程内请求，然后开始获取以配置下面的事件，因为否则我们可能会除了 postmaster 死亡事件之外没有任何已配置的事件。
		 */
		if (!bms_is_empty(requestor->as_needrequest))
			return;
		if (GetNumRegisteredWaitEvents(set) > 1)
			return;
		process_pending_request(pendingAreq);
		fetch_more_data_begin(areq);
	}
	else if (pendingAreq->requestee != areq->requestee)
	{
		/*
		 * This is the case when the in-process request was made by the same
		 * parent but for a different child.  Since we configure only the
		 * event for the request made for that child, skip the given request.
		 *
		 * 当进程内请求是由同一个父级但针对不同的子级发出时，就会出现这种情况。  由于我们仅为该子项的请求配置事件，因此请跳过给定的请求。
		 */
		return;
	}
	else
		Assert(pendingAreq == areq);

	AddWaitEventToSet(set, WL_SOCKET_READABLE, PQsocket(fsstate->conn),
					  NULL, areq);
}

/*
 * postgresForeignAsyncNotify
 *		Fetch some more tuples from a file descriptor that becomes ready,
 *		requesting next tuple.
 *
 * postgresForeignAsyncNotify 从准备就绪的文件描述符中获取更多元组，请求下一个元组。
 */
static void
postgresForeignAsyncNotify(AsyncRequest *areq)
{
	ForeignScanState *node = (ForeignScanState *) areq->requestee;
	PgFdwScanState *fsstate = (PgFdwScanState *) node->fdw_state;

	/* The core code would have initialized the callback_pending flag
	 *
	 * 核心代码将初始化callback_pending标志
	 */
	Assert(!areq->callback_pending);

	/*
	 * If process_pending_request() has been invoked on the given request
	 * before we get here, we might have some tuples already; in which case
	 * produce the next tuple
	 *
	 * 如果在我们到达这里之前已经对给定的请求调用了 process_pending_request() ，那么我们可能已经有了一些元组；在这种情况下产生下一个元组
	 */
	if (fsstate->next_tuple < fsstate->num_tuples)
	{
		produce_tuple_asynchronously(areq, true);
		return;
	}

	/* We must have run out of tuples
	 *
	 * 我们肯定已经用完了元组
	 */
	Assert(fsstate->next_tuple >= fsstate->num_tuples);

	/* The request should be currently in-process
	 *
	 * 该请求当前应该正在处理中
	 */
	Assert(fsstate->conn_state->pendingAreq == areq);

	/* On error, report the original query, not the FETCH.
	 *
	 * 出错时，报告原始查询，而不是 FETCH。
	 */
	if (!PQconsumeInput(fsstate->conn))
		pgfdw_report_error(ERROR, NULL, fsstate->conn, false, fsstate->query);

	fetch_more_data(node);

	produce_tuple_asynchronously(areq, true);
}

/*
 * Asynchronously produce next tuple from a foreign PostgreSQL table.
 *
 * 从外部 PostgreSQL 表异步生成下一个元组。
 */
static void
produce_tuple_asynchronously(AsyncRequest *areq, bool fetch)
{
	ForeignScanState *node = (ForeignScanState *) areq->requestee;
	PgFdwScanState *fsstate = (PgFdwScanState *) node->fdw_state;
	AsyncRequest *pendingAreq = fsstate->conn_state->pendingAreq;
	TupleTableSlot *result;

	/* This should not be called if the request is currently in-process
	 *
	 * 如果请求当前正在处理中，则不应调用此函数
	 */
	Assert(areq != pendingAreq);

	/* Fetch some more tuples, if we've run out
	 *
	 * 如果我们用完了，再获取一些元组
	 */
	if (fsstate->next_tuple >= fsstate->num_tuples)
	{
		/* No point in another fetch if we already detected EOF, though
		 *
		 * 如果我们已经检测到 EOF，那么再次获取就没有意义了
		 */
		if (!fsstate->eof_reached)
		{
			/* Mark the request as pending for a callback
			 *
			 * 将请求标记为等待回调
			 */
			ExecAsyncRequestPending(areq);
			/* Begin another fetch if requested and if no pending request
			 *
			 * 如果有请求并且没有待处理的请求，则开始另一次提取
			 */
			if (fetch && !pendingAreq)
				fetch_more_data_begin(areq);
		}
		else
		{
			/* There's nothing more to do; just return a NULL pointer
			 *
			 * 没有什么可做的；只返回一个NULL指针
			 */
			result = NULL;
			/* Mark the request as complete
			 *
			 * 将请求标记为完成
			 */
			ExecAsyncRequestDone(areq, result);
		}
		return;
	}

	/* Get a tuple from the ForeignScan node
	 *
	 * 从ForeignScan节点获取元组
	 */
	result = areq->requestee->ExecProcNodeReal(areq->requestee);
	if (!TupIsNull(result))
	{
		/* Mark the request as complete
		 *
		 * 将请求标记为完成
		 */
		ExecAsyncRequestDone(areq, result);
		return;
	}

	/* We must have run out of tuples
	 *
	 * 我们肯定已经用完了元组
	 */
	Assert(fsstate->next_tuple >= fsstate->num_tuples);

	/* Fetch some more tuples, if we've not detected EOF yet
	 *
	 * 如果我们还没有检测到 EOF，则获取更多元组
	 */
	if (!fsstate->eof_reached)
	{
		/* Mark the request as pending for a callback
		 *
		 * 将请求标记为等待回调
		 */
		ExecAsyncRequestPending(areq);
		/* Begin another fetch if requested and if no pending request
		 *
		 * 如果有请求并且没有待处理的请求，则开始另一次提取
		 */
		if (fetch && !pendingAreq)
			fetch_more_data_begin(areq);
	}
	else
	{
		/* There's nothing more to do; just return a NULL pointer
		 *
		 * 没有什么可做的；只返回一个NULL指针
		 */
		result = NULL;
		/* Mark the request as complete
		 *
		 * 将请求标记为完成
		 */
		ExecAsyncRequestDone(areq, result);
	}
}

/*
 * Begin an asynchronous data fetch.
 *
 * 开始异步数据获取。
 *
 * Note: this function assumes there is no currently-in-progress asynchronous
 * data fetch.
 *
 * 注意：此函数假设当前没有正在进行的异步数据获取。
 *
 * Note: fetch_more_data must be called to fetch the result.
 *
 * 注意：必须调用 fetch_more_data 来获取结果。
 */
static void
fetch_more_data_begin(AsyncRequest *areq)
{
	ForeignScanState *node = (ForeignScanState *) areq->requestee;
	PgFdwScanState *fsstate = (PgFdwScanState *) node->fdw_state;
	char		sql[64];

	Assert(!fsstate->conn_state->pendingAreq);

	/* Create the cursor synchronously.
	 *
	 * 同步创建游标。
	 */
	if (!fsstate->cursor_exists)
		create_cursor(node);

	/* We will send this query, but not wait for the response.
	 *
	 * 我们将发送此查询，但不等待响应。
	 */
	snprintf(sql, sizeof(sql), "FETCH %d FROM c%u",
			 fsstate->fetch_size, fsstate->cursor_number);

	if (!PQsendQuery(fsstate->conn, sql))
		pgfdw_report_error(ERROR, NULL, fsstate->conn, false, fsstate->query);

	/* Remember that the request is in process
	 *
	 * 请记住，请求正在处理中
	 */
	fsstate->conn_state->pendingAreq = areq;
}

/*
 * Process a pending asynchronous request.
 *
 * 处理挂起的异步请求。
 */
void
process_pending_request(AsyncRequest *areq)
{
	ForeignScanState *node = (ForeignScanState *) areq->requestee;
	PgFdwScanState *fsstate = (PgFdwScanState *) node->fdw_state;

	/* The request would have been pending for a callback
	 *
	 * 该请求将等待回调
	 */
	Assert(areq->callback_pending);

	/* The request should be currently in-process
	 *
	 * 该请求当前应该正在处理中
	 */
	Assert(fsstate->conn_state->pendingAreq == areq);

	fetch_more_data(node);

	/*
	 * If we didn't get any tuples, must be end of data; complete the request
	 * now.  Otherwise, we postpone completing the request until we are called
	 * from postgresForeignAsyncConfigureWait()/postgresForeignAsyncNotify().
	 *
	 * 如果我们没有得到任何元组，则一定是数据结束；立即完成请求。  否则，我们将推迟完成请求，直到从 postgresForeignAsyncConfigureWait()/postgresForeignAsyncNotify() 调用我们。
	 */
	if (fsstate->next_tuple >= fsstate->num_tuples)
	{
		/* Unlike AsyncNotify, we unset callback_pending ourselves
		 *
		 * 与AsyncNotify不同，我们自己取消设置callback_pending
		 */
		areq->callback_pending = false;
		/* Mark the request as complete
		 *
		 * 将请求标记为完成
		 */
		ExecAsyncRequestDone(areq, NULL);
		/* Unlike AsyncNotify, we call ExecAsyncResponse ourselves
		 *
		 * 与AsyncNotify不同，我们自己调用ExecAsyncResponse
		 */
		ExecAsyncResponse(areq);
	}
}

/*
 * Complete a pending asynchronous request.
 *
 * 完成待处理的异步请求。
 */
static void
complete_pending_request(AsyncRequest *areq)
{
	/* The request would have been pending for a callback
	 *
	 * 该请求将等待回调
	 */
	Assert(areq->callback_pending);

	/* Unlike AsyncNotify, we unset callback_pending ourselves
	 *
	 * 与AsyncNotify不同，我们自己取消设置callback_pending
	 */
	areq->callback_pending = false;

	/* We begin a fetch afterwards if necessary; don't fetch
	 *
	 * 如有必要，我们随后开始获取；不要获取
	 */
	produce_tuple_asynchronously(areq, false);

	/* Unlike AsyncNotify, we call ExecAsyncResponse ourselves
	 *
	 * 与AsyncNotify不同，我们自己调用ExecAsyncResponse
	 */
	ExecAsyncResponse(areq);

	/* Also, we do instrumentation ourselves, if required
	 *
	 * 此外，如果需要的话，我们自己做仪器
	 */
	if (areq->requestee->instrument)
		InstrUpdateTupleCount(areq->requestee->instrument,
							  TupIsNull(areq->result) ? 0.0 : 1.0);
}

/*
 * Create a tuple from the specified row of the PGresult.
 *
 * 从 PGresult 的指定行创建一个元组。
 *
 * rel is the local representation of the foreign table, attinmeta is
 * conversion data for the rel's tupdesc, and retrieved_attrs is an
 * integer list of the table column numbers present in the PGresult.
 * fsstate is the ForeignScan plan node's execution state.
 * temp_context is a working context that can be reset after each tuple.
 *
 * rel 是外部表的本地表示，attinmeta 是 rel 的 tupdesc 的转换数据，retrieved_attrs 是 PGresult 中存在的表列号的整数列表。 fsstate 是ForeignScan 计划节点的执行状态。 temp_context 是一个工作上下文，可以在每个元组之后重置。
 *
 * Note: either rel or fsstate, but not both, can be NULL.  rel is NULL
 * if we're processing a remote join, while fsstate is NULL in a non-query
 * context such as ANALYZE, or if we're processing a non-scan query node.
 *
 * 注意：rel 或 fsstate 可以为 NULL，但不能同时为两者。  如果我们正在处理远程连接，则 rel 为 NULL，而在非查询上下文（例如 ANALYZE）中，或者如果我们正在处理非扫描查询节点，则 fsstate 为 NULL。
 */
static HeapTuple
make_tuple_from_result_row(PGresult *res,
						   int row,
						   Relation rel,
						   AttInMetadata *attinmeta,
						   List *retrieved_attrs,
						   ForeignScanState *fsstate,
						   MemoryContext temp_context)
{
	HeapTuple	tuple;
	TupleDesc	tupdesc;
	Datum	   *values;
	bool	   *nulls;
	ItemPointer ctid = NULL;
	ConversionLocation errpos;
	ErrorContextCallback errcallback;
	MemoryContext oldcontext;
	ListCell   *lc;
	int			j;

	Assert(row < PQntuples(res));

	/*
	 * Do the following work in a temp context that we reset after each tuple.
	 * This cleans up not only the data we have direct access to, but any
	 * cruft the I/O functions might leak.
	 *
	 * 在我们在每个元组之后重置的临时上下文中执行以下工作。这不仅清理了我们可以直接访问的数据，还清理了 I/O 函数可能泄漏的任何残骸。
	 */
	oldcontext = MemoryContextSwitchTo(temp_context);

	/*
	 * Get the tuple descriptor for the row.  Use the rel's tupdesc if rel is
	 * provided, otherwise look to the scan node's ScanTupleSlot.
	 *
	 * 获取该行的元组描述符。  如果提供了 rel，则使用 rel 的 tupdesc，否则查看扫描节点的 ScanTupleSlot。
	 */
	if (rel)
		tupdesc = RelationGetDescr(rel);
	else
	{
		Assert(fsstate);
		tupdesc = fsstate->ss.ss_ScanTupleSlot->tts_tupleDescriptor;
	}

	values = (Datum *) palloc0(tupdesc->natts * sizeof(Datum));
	nulls = (bool *) palloc(tupdesc->natts * sizeof(bool));
	/* Initialize to nulls for any columns not present in result
	 *
	 * 对于结果中不存在的任何列初始化为空
	 */
	memset(nulls, true, tupdesc->natts * sizeof(bool));

	/*
	 * Set up and install callback to report where conversion error occurs.
	 *
	 * 设置并安装回调以报告发生转换错误的位置。
	 */
	errpos.cur_attno = 0;
	errpos.rel = rel;
	errpos.fsstate = fsstate;
	errcallback.callback = conversion_error_callback;
	errcallback.arg = &errpos;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/*
	 * i indexes columns in the relation, j indexes columns in the PGresult.
	 *
	 * i 对关系中的列进行索引，j 对 PGresult 中的列进行索引。
	 */
	j = 0;
	foreach(lc, retrieved_attrs)
	{
		int			i = lfirst_int(lc);
		char	   *valstr;

		/* fetch next column's textual value
		 *
		 * 获取下一列的文本值
		 */
		if (PQgetisnull(res, row, j))
			valstr = NULL;
		else
			valstr = PQgetvalue(res, row, j);

		/*
		 * convert value to internal representation
		 *
		 * 将值转换为内部表示
		 *
		 * Note: we ignore system columns other than ctid and oid in result
		 *
		 * 注意：我们忽略结果中除 ctid 和 oid 之外的系统列
		 */
		errpos.cur_attno = i;
		if (i > 0)
		{
			/* ordinary column
			 *
			 * 普通柱
			 */
			Assert(i <= tupdesc->natts);
			nulls[i - 1] = (valstr == NULL);
			/* Apply the input function even to nulls, to support domains
			 *
			 * 甚至将输入函数应用于空值，以支持域
			 */
			values[i - 1] = InputFunctionCall(&attinmeta->attinfuncs[i - 1],
											  valstr,
											  attinmeta->attioparams[i - 1],
											  attinmeta->atttypmods[i - 1]);
		}
		else if (i == SelfItemPointerAttributeNumber)
		{
			/* ctid */
			if (valstr != NULL)
			{
				Datum		datum;

				datum = DirectFunctionCall1(tidin, CStringGetDatum(valstr));
				ctid = (ItemPointer) DatumGetPointer(datum);
			}
		}
		errpos.cur_attno = 0;

		j++;
	}

	/* Uninstall error context callback.
	 *
	 * 卸载错误上下文回调。
	 */
	error_context_stack = errcallback.previous;

	/*
	 * Check we got the expected number of columns.  Note: j == 0 and
	 * PQnfields == 1 is expected, since deparse emits a NULL if no columns.
	 *
	 * 检查我们是否获得了预期的列数。  注意：j == 0 和 PQnfields == 1 是预期的，因为如果没有列，deparse 会发出 NULL。
	 */
	if (j > 0 && j != PQnfields(res))
		elog(ERROR, "remote query result does not match the foreign table");

	/*
	 * Build the result tuple in caller's memory context.
	 *
	 * 在调用者的内存上下文中构建结果元组。
	 */
	MemoryContextSwitchTo(oldcontext);

	tuple = heap_form_tuple(tupdesc, values, nulls);

	/*
	 * If we have a CTID to return, install it in both t_self and t_ctid.
	 * t_self is the normal place, but if the tuple is converted to a
	 * composite Datum, t_self will be lost; setting t_ctid allows CTID to be
	 * preserved during EvalPlanQual re-evaluations (see ROW_MARK_COPY code).
	 *
	 * 如果我们要返回 CTID，请将其安装在 t_self 和 t_ctid 中。 t_self是正常的地方，但是如果tuple转换为复合Datum，t_self就会丢失；设置 t_ctid 允许在 EvalPlanQual 重新评估期间保留 CTID（请参阅 ROW_MARK_COPY 代码）。
	 */
	if (ctid)
		tuple->t_self = tuple->t_data->t_ctid = *ctid;

	/*
	 * Stomp on the xmin, xmax, and cmin fields from the tuple created by
	 * heap_form_tuple.  heap_form_tuple actually creates the tuple with
	 * DatumTupleFields, not HeapTupleFields, but the executor expects
	 * HeapTupleFields and will happily extract system columns on that
	 * assumption.  If we don't do this then, for example, the tuple length
	 * ends up in the xmin field, which isn't what we want.
	 *
	 * 踩踏 heap_form_tuple 创建的元组中的 xmin、xmax 和 cmin 字段。  heap_form_tuple 实际上使用 DatumTupleFields 创建元组，而不是 HeapTupleFields，但执行器需要 HeapTupleFields，并且会在该假设下愉快地提取系统列。  例如，如果我们不这样做，则元组长度最终会出现在 xmin 字段中，这不是我们想要的。
	 */
	HeapTupleHeaderSetXmax(tuple->t_data, InvalidTransactionId);
	HeapTupleHeaderSetXmin(tuple->t_data, InvalidTransactionId);
	HeapTupleHeaderSetCmin(tuple->t_data, InvalidTransactionId);

	/* Clean up
	 *
	 * 清理
	 */
	MemoryContextReset(temp_context);

	return tuple;
}

/*
 * Callback function which is called when error occurs during column value
 * conversion.  Print names of column and relation.
 *
 * 列值转换过程中发生错误时调用的回调函数。  打印列和关系的名称。
 *
 * Note that this function mustn't do any catalog lookups, since we are in
 * an already-failed transaction.  Fortunately, we can get the needed info
 * from the relation or the query's rangetable instead.
 *
 * 请注意，此函数不得执行任何目录查找，因为我们处于已失败的事务中。  幸运的是，我们可以从关系或查询的范围表中获取所需的信息。
 */
static void
conversion_error_callback(void *arg)
{
	ConversionLocation *errpos = (ConversionLocation *) arg;
	Relation	rel = errpos->rel;
	ForeignScanState *fsstate = errpos->fsstate;
	const char *attname = NULL;
	const char *relname = NULL;
	bool		is_wholerow = false;

	/*
	 * If we're in a scan node, always use aliases from the rangetable, for
	 * consistency between the simple-relation and remote-join cases.  Look at
	 * the relation's tupdesc only if we're not in a scan node.
	 *
	 * 如果我们位于扫描节点中，请始终使用范围表中的别名，以保持简单关系和远程连接情况之间的一致性。  仅当我们不在扫描节点中时才查看关系的 tupdesc。
	 */
	if (fsstate)
	{
		/* ForeignScan case
		 *
		 * 国外扫描案例
		 */
		ForeignScan *fsplan = castNode(ForeignScan, fsstate->ss.ps.plan);
		int			varno = 0;
		AttrNumber	colno = 0;

		if (fsplan->scan.scanrelid > 0)
		{
			/* error occurred in a scan against a foreign table
			 *
			 * 对外部表进行扫描时发生错误
			 */
			varno = fsplan->scan.scanrelid;
			colno = errpos->cur_attno;
		}
		else
		{
			/* error occurred in a scan against a foreign join
			 *
			 * 针对外部连接的扫描中发生错误
			 */
			TargetEntry *tle;

			tle = list_nth_node(TargetEntry, fsplan->fdw_scan_tlist,
								errpos->cur_attno - 1);

			/*
			 * Target list can have Vars and expressions.  For Vars, we can
			 * get some information, however for expressions we can't.  Thus
			 * for expressions, just show generic context message.
			 *
			 * 目标列表可以有变量和表达式。  对于变量，我们可以获得一些信息，但是对于表达式我们不能。  因此，对于表达式，只需显示通用上下文消息。
			 */
			if (IsA(tle->expr, Var))
			{
				Var		   *var = (Var *) tle->expr;

				varno = var->varno;
				colno = var->varattno;
			}
		}

		if (varno > 0)
		{
			EState	   *estate = fsstate->ss.ps.state;
			RangeTblEntry *rte = exec_rt_fetch(varno, estate);

			relname = rte->eref->aliasname;

			if (colno == 0)
				is_wholerow = true;
			else if (colno > 0 && colno <= list_length(rte->eref->colnames))
				attname = strVal(list_nth(rte->eref->colnames, colno - 1));
			else if (colno == SelfItemPointerAttributeNumber)
				attname = "ctid";
		}
	}
	else if (rel)
	{
		/* Non-ForeignScan case (we should always have a rel here)
		 *
		 * Non-ForeignScan 案例（我们应该始终在这里有一个 rel ）
		 */
		TupleDesc	tupdesc = RelationGetDescr(rel);

		relname = RelationGetRelationName(rel);
		if (errpos->cur_attno > 0 && errpos->cur_attno <= tupdesc->natts)
		{
			Form_pg_attribute attr = TupleDescAttr(tupdesc,
												   errpos->cur_attno - 1);

			attname = NameStr(attr->attname);
		}
		else if (errpos->cur_attno == SelfItemPointerAttributeNumber)
			attname = "ctid";
	}

	if (relname && is_wholerow)
		errcontext("whole-row reference to foreign table \"%s\"", relname);
	else if (relname && attname)
		errcontext("column \"%s\" of foreign table \"%s\"", attname, relname);
	else
		errcontext("processing expression at position %d in select list",
				   errpos->cur_attno);
}

/*
 * Given an EquivalenceClass and a foreign relation, find an EC member
 * that can be used to sort the relation remotely according to a pathkey
 * using this EC.
 *
 * 给定一个 EquivalenceClass 和一个外部关系，找到一个 EC 成员，该成员可用于根据使用此 EC 的路径键远程对关系进行排序。
 *
 * If there is more than one suitable candidate, return an arbitrary
 * one of them.  If there is none, return NULL.
 *
 * 如果有多个合适的候选者，则返回其中任意一个。  如果没有，则返回 NULL。
 *
 * This checks that the EC member expression uses only Vars from the given
 * rel and is shippable.  Caller must separately verify that the pathkey's
 * ordering operator is shippable.
 *
 * 这会检查 EC 成员表达式是否仅使用给定 rel 中的 Var，并且是否可发布。  调用者必须单独验证路径密钥的订购操作员是否可发货。
 */
EquivalenceMember *
find_em_for_rel(PlannerInfo *root, EquivalenceClass *ec, RelOptInfo *rel)
{
	PgFdwRelationInfo *fpinfo = (PgFdwRelationInfo *) rel->fdw_private;
	EquivalenceMemberIterator it;
	EquivalenceMember *em;

	setup_eclass_member_iterator(&it, ec, rel->relids);
	while ((em = eclass_member_iterator_next(&it)) != NULL)
	{
		/*
		 * Note we require !bms_is_empty, else we'd accept constant
		 * expressions which are not suitable for the purpose.
		 *
		 * 请注意，我们需要 !bms_is_empty，否则我们将接受不适合该目的的常量表达式。
		 */
		if (bms_is_subset(em->em_relids, rel->relids) &&
			!bms_is_empty(em->em_relids) &&
			bms_is_empty(bms_intersect(em->em_relids, fpinfo->hidden_subquery_rels)) &&
			is_foreign_expr(root, rel, em->em_expr))
			return em;
	}

	return NULL;
}

/*
 * Find an EquivalenceClass member that is to be computed as a sort column
 * in the given rel's reltarget, and is shippable.
 *
 * 查找一个 EquivalenceClass 成员，该成员将作为给定 rel 的 reltarget 中的排序列进行计算，并且可交付。
 *
 * If there is more than one suitable candidate, return an arbitrary
 * one of them.  If there is none, return NULL.
 *
 * 如果有多个合适的候选者，则返回其中任意一个。  如果没有，则返回 NULL。
 *
 * This checks that the EC member expression uses only Vars from the given
 * rel and is shippable.  Caller must separately verify that the pathkey's
 * ordering operator is shippable.
 *
 * 这会检查 EC 成员表达式是否仅使用给定 rel 中的 Var，并且是否可发布。  调用者必须单独验证路径密钥的订购操作员是否可发货。
 */
EquivalenceMember *
find_em_for_rel_target(PlannerInfo *root, EquivalenceClass *ec,
					   RelOptInfo *rel)
{
	PathTarget *target = rel->reltarget;
	ListCell   *lc1;
	int			i;

	i = 0;
	foreach(lc1, target->exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc1);
		Index		sgref = get_pathtarget_sortgroupref(target, i);
		ListCell   *lc2;

		/* Ignore non-sort expressions
		 *
		 * 忽略非排序表达式
		 */
		if (sgref == 0 ||
			get_sortgroupref_clause_noerr(sgref,
										  root->parse->sortClause) == NULL)
		{
			i++;
			continue;
		}

		/* We ignore binary-compatible relabeling on both ends
		 *
		 * 我们忽略两端二进制兼容的重新标记
		 */
		while (expr && IsA(expr, RelabelType))
			expr = ((RelabelType *) expr)->arg;

		/*
		 * Locate an EquivalenceClass member matching this expr, if any.
		 * Ignore child members.
		 *
		 * 找到与此表达式匹配的 EquivalenceClass 成员（如果有）。忽略儿童成员。
		 */
		foreach(lc2, ec->ec_members)
		{
			EquivalenceMember *em = (EquivalenceMember *) lfirst(lc2);
			Expr	   *em_expr;

			/* Don't match constants
			 *
			 * 不匹配常量
			 */
			if (em->em_is_const)
				continue;

			/* Child members should not exist in ec_members
			 *
			 * ec_members 中不应存在子成员
			 */
			Assert(!em->em_is_child);

			/* Match if same expression (after stripping relabel)
			 *
			 * 如果表达式相同则匹配（剥离重新标签后）
			 */
			em_expr = em->em_expr;
			while (em_expr && IsA(em_expr, RelabelType))
				em_expr = ((RelabelType *) em_expr)->arg;

			if (!equal(em_expr, expr))
				continue;

			/* Check that expression (including relabels!) is shippable
			 *
			 * 检查表达式（包括重新标签！）是否可发货
			 */
			if (is_foreign_expr(root, rel, em->em_expr))
				return em;
		}

		i++;
	}

	return NULL;
}

/*
 * Determine batch size for a given foreign table. The option specified for
 * a table has precedence.
 *
 * 确定给定外部表的批量大小。为表指定的选项具有优先权。
 */
static int
get_batch_size_option(Relation rel)
{
	Oid			foreigntableid = RelationGetRelid(rel);
	ForeignTable *table;
	ForeignServer *server;
	List	   *options;
	ListCell   *lc;

	/* we use 1 by default, which means "no batching"
	 *
	 * 我们默认使用1，这意味着“无批处理”
	 */
	int			batch_size = 1;

	/*
	 * Load options for table and server. We append server options after table
	 * options, because table options take precedence.
	 *
	 * 表和服务器的加载选项。我们将服务器选项附加在表选项之后，因为表选项优先。
	 */
	table = GetForeignTable(foreigntableid);
	server = GetForeignServer(table->serverid);

	options = NIL;
	options = list_concat(options, table->options);
	options = list_concat(options, server->options);

	/* See if either table or server specifies batch_size.
	 *
	 * 查看表或服务器是否指定了batch_size。
	 */
	foreach(lc, options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "batch_size") == 0)
		{
			(void) parse_int(defGetString(def), &batch_size, 0, NULL);
			break;
		}
	}

	return batch_size;
}

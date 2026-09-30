/*-------------------------------------------------------------------------
 *
 * explain.c
 *	  Explain query execution plans
 *
 *	  解释查询执行计划。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/commands/explain.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/pg_type.h"
#include "commands/createas.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "commands/explain_dr.h"
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "commands/prepare.h"
#include "foreign/fdwapi.h"
#include "jit/jit.h"
#include "libpq/pqformat.h"
#include "libpq/protocol.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/analyze.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteHandler.h"
#include "storage/bufmgr.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/guc_tables.h"
#include "utils/json.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/snapmgr.h"
#include "utils/tuplesort.h"
#include "utils/typcache.h"
#include "utils/xml.h"


/* Hook for plugins to get control in ExplainOneQuery() */
/*
 *
 * 供插件在 ExplainOneQuery() 中取得控制权的钩子。
 */
ExplainOneQuery_hook_type ExplainOneQuery_hook = NULL;

/* Hook for plugins to get control in explain_get_index_name() */
/*
 *
 * 供插件在 explain_get_index_name() 中取得控制权的钩子。
 */
explain_get_index_name_hook_type explain_get_index_name_hook = NULL;

/* per-plan and per-node hooks for plugins to print additional info */
/*
 *
 * 供插件在每个计划以及每个节点上打印附加信息的钩子。
 */
explain_per_plan_hook_type explain_per_plan_hook = NULL;
explain_per_node_hook_type explain_per_node_hook = NULL;

/*
 * Various places within need to convert bytes to kilobytes.  Round these up
 * to the next whole kilobyte.
 *
 * 本文件多处需要把字节转换成千字节。这些值向上取整到下一个整千字节。
 */
#define BYTES_TO_KILOBYTES(b) (((b) + 1023) / 1024)

static void ExplainOneQuery(Query *query, int cursorOptions,
							IntoClause *into, ExplainState *es,
							ParseState *pstate, ParamListInfo params);
static void ExplainPrintJIT(ExplainState *es, int jit_flags,
							JitInstrumentation *ji);
static void ExplainPrintSerialize(ExplainState *es,
								  SerializeMetrics *metrics);
static void report_triggers(ResultRelInfo *rInfo, bool show_relname,
							ExplainState *es);
static double elapsed_time(instr_time *starttime);
static bool ExplainPreScanNode(PlanState *planstate, Bitmapset **rels_used);
static void ExplainNode(PlanState *planstate, List *ancestors,
						const char *relationship, const char *plan_name,
						ExplainState *es);
static void show_plan_tlist(PlanState *planstate, List *ancestors,
							ExplainState *es);
static void show_expression(Node *node, const char *qlabel,
							PlanState *planstate, List *ancestors,
							bool useprefix, ExplainState *es);
static void show_qual(List *qual, const char *qlabel,
					  PlanState *planstate, List *ancestors,
					  bool useprefix, ExplainState *es);
static void show_scan_qual(List *qual, const char *qlabel,
						   PlanState *planstate, List *ancestors,
						   ExplainState *es);
static void show_upper_qual(List *qual, const char *qlabel,
							PlanState *planstate, List *ancestors,
							ExplainState *es);
static void show_sort_keys(SortState *sortstate, List *ancestors,
						   ExplainState *es);
static void show_incremental_sort_keys(IncrementalSortState *incrsortstate,
									   List *ancestors, ExplainState *es);
static void show_merge_append_keys(MergeAppendState *mstate, List *ancestors,
								   ExplainState *es);
static void show_agg_keys(AggState *astate, List *ancestors,
						  ExplainState *es);
static void show_grouping_sets(PlanState *planstate, Agg *agg,
							   List *ancestors, ExplainState *es);
static void show_grouping_set_keys(PlanState *planstate,
								   Agg *aggnode, Sort *sortnode,
								   List *context, bool useprefix,
								   List *ancestors, ExplainState *es);
static void show_group_keys(GroupState *gstate, List *ancestors,
							ExplainState *es);
static void show_sort_group_keys(PlanState *planstate, const char *qlabel,
								 int nkeys, int nPresortedKeys, AttrNumber *keycols,
								 Oid *sortOperators, Oid *collations, bool *nullsFirst,
								 List *ancestors, ExplainState *es);
static void show_sortorder_options(StringInfo buf, Node *sortexpr,
								   Oid sortOperator, Oid collation, bool nullsFirst);
static void show_window_def(WindowAggState *planstate,
							List *ancestors, ExplainState *es);
static void show_window_keys(StringInfo buf, PlanState *planstate,
							 int nkeys, AttrNumber *keycols,
							 List *ancestors, ExplainState *es);
static void show_storage_info(char *maxStorageType, int64 maxSpaceUsed,
							  ExplainState *es);
static void show_tablesample(TableSampleClause *tsc, PlanState *planstate,
							 List *ancestors, ExplainState *es);
static void show_sort_info(SortState *sortstate, ExplainState *es);
static void show_incremental_sort_info(IncrementalSortState *incrsortstate,
									   ExplainState *es);
static void show_hash_info(HashState *hashstate, ExplainState *es);
static void show_material_info(MaterialState *mstate, ExplainState *es);
static void show_windowagg_info(WindowAggState *winstate, ExplainState *es);
static void show_ctescan_info(CteScanState *ctescanstate, ExplainState *es);
static void show_table_func_scan_info(TableFuncScanState *tscanstate,
									  ExplainState *es);
static void show_recursive_union_info(RecursiveUnionState *rstate,
									  ExplainState *es);
static void show_memoize_info(MemoizeState *mstate, List *ancestors,
							  ExplainState *es);
static void show_hashagg_info(AggState *aggstate, ExplainState *es);
static void show_indexsearches_info(PlanState *planstate, ExplainState *es);
static void show_tidbitmap_info(BitmapHeapScanState *planstate,
								ExplainState *es);
static void show_instrumentation_count(const char *qlabel, int which,
									   PlanState *planstate, ExplainState *es);
static void show_foreignscan_info(ForeignScanState *fsstate, ExplainState *es);
static const char *explain_get_index_name(Oid indexId);
static bool peek_buffer_usage(ExplainState *es, const BufferUsage *usage);
static void show_buffer_usage(ExplainState *es, const BufferUsage *usage);
static void show_wal_usage(ExplainState *es, const WalUsage *usage);
static void show_memory_counters(ExplainState *es,
								 const MemoryContextCounters *mem_counters);
static void ExplainIndexScanDetails(Oid indexid, ScanDirection indexorderdir,
									ExplainState *es);
static void ExplainScanTarget(Scan *plan, ExplainState *es);
static void ExplainModifyTarget(ModifyTable *plan, ExplainState *es);
static void ExplainTargetRel(Plan *plan, Index rti, ExplainState *es);
static void show_modifytable_info(ModifyTableState *mtstate, List *ancestors,
								  ExplainState *es);
static void ExplainMemberNodes(PlanState **planstates, int nplans,
							   List *ancestors, ExplainState *es);
static void ExplainMissingMembers(int nplans, int nchildren, ExplainState *es);
static void ExplainSubPlans(List *plans, List *ancestors,
							const char *relationship, ExplainState *es);
static void ExplainCustomChildren(CustomScanState *css,
								  List *ancestors, ExplainState *es);
static ExplainWorkersState *ExplainCreateWorkersState(int num_workers);
static void ExplainOpenWorker(int n, ExplainState *es);
static void ExplainCloseWorker(int n, ExplainState *es);
static void ExplainFlushWorkersState(ExplainState *es);

/*
 * 核心流程：
 * ExplainQuery() 是 EXPLAIN 主入口：配置 ExplainState、重写查询，
 * 对每个 Query 调用 ExplainOneQuery()，再输出结果。
 * ExplainOneQuery() 把实用语句交给 ExplainOneUtility()，其余交给
 * standard_ExplainOneQuery()（或插件钩子）做规划。
 * ExplainOnePlan() 在需要时执行计划（ANALYZE），再经 ExplainPrintPlan()
 * 走到 ExplainNode() 递归打印计划树，并附带触发器、JIT 与序列化信息。
 */



/*
 * ExplainQuery -
 *	  execute an EXPLAIN command
 *
 *	  执行一条 EXPLAIN 命令。
 */
void
ExplainQuery(ParseState *pstate, ExplainStmt *stmt,
			 ParamListInfo params, DestReceiver *dest)
{
	ExplainState *es = NewExplainState();
	TupOutputState *tstate;
	JumbleState *jstate = NULL;
	Query	   *query;
	List	   *rewritten;

	/* Configure the ExplainState based on the provided options */
	/*
	 *
	 * 根据给定选项配置 ExplainState。
	 */
	ParseExplainOptionList(es, stmt->options, pstate);

	/* Extract the query and, if enabled, jumble it */
	/*
	 *
	 * 取出查询，并在启用时计算其 jumble。
	 */
	query = castNode(Query, stmt->query);
	if (IsQueryIdEnabled())
		jstate = JumbleQuery(query);

	if (post_parse_analyze_hook)
		(*post_parse_analyze_hook) (pstate, query, jstate);

	/*
	 * Parse analysis was done already, but we still have to run the rule
	 * rewriter.  We do not do AcquireRewriteLocks: we assume the query either
	 * came straight from the parser, or suitable locks were acquired by
	 * plancache.c.
	 *
	 * 语法分析已经完成，但仍必须运行规则重写器。这里不做 AcquireRewriteLocks：
	 * 假定查询要么直接来自解析器，要么 plancache.c 已经取得了合适的锁。
	 */
	rewritten = QueryRewrite(castNode(Query, stmt->query));

	/* emit opening boilerplate */
	/*
	 *
	 * 输出开头的固定文本。
	 */
	ExplainBeginOutput(es);

	if (rewritten == NIL)
	{
		/*
		 * In the case of an INSTEAD NOTHING, tell at least that.  But in
		 * non-text format, the output is delimited, so this isn't necessary.
		 *
		 * 若是 INSTEAD NOTHING，至少要说明这一点。非文本格式的输出本身有定界，
		 * 因此不必这样做。
		 */
		if (es->format == EXPLAIN_FORMAT_TEXT)
			appendStringInfoString(es->str, "Query rewrites to nothing\n");
	}
	else
	{
		ListCell   *l;

		/* Explain every plan */
		/*
		 *
		 * 解释每一个计划。
		 */
		foreach(l, rewritten)
		{
			ExplainOneQuery(lfirst_node(Query, l),
							CURSOR_OPT_PARALLEL_OK, NULL, es,
							pstate, params);

			/* Separate plans with an appropriate separator */
			/*
			 *
			 * 用合适的分隔符隔开各个计划。
			 */
			if (lnext(rewritten, l) != NULL)
				ExplainSeparatePlans(es);
		}
	}

	/* emit closing boilerplate */
	/*
	 *
	 * 输出结尾的固定文本。
	 */
	ExplainEndOutput(es);
	Assert(es->indent == 0);

	/* output tuples */
	/*
	 *
	 * 输出元组。
	 */
	tstate = begin_tup_output_tupdesc(dest, ExplainResultDesc(stmt),
									  &TTSOpsVirtual);
	if (es->format == EXPLAIN_FORMAT_TEXT)
		do_text_output_multiline(tstate, es->str->data);
	else
		do_text_output_oneline(tstate, es->str->data);
	end_tup_output(tstate);

	pfree(es->str->data);
}

/*
 * ExplainResultDesc -
 *	  construct the result tupledesc for an EXPLAIN
 *
 *	  为 EXPLAIN 构造结果的元组描述符。
 */
TupleDesc
ExplainResultDesc(ExplainStmt *stmt)
{
	TupleDesc	tupdesc;
	ListCell   *lc;
	Oid			result_type = TEXTOID;

	/* Check for XML format option */
	/*
	 *
	 * 检查是否指定了 XML 格式选项。
	 */
	foreach(lc, stmt->options)
	{
		DefElem    *opt = (DefElem *) lfirst(lc);

		if (strcmp(opt->defname, "format") == 0)
		{
			char	   *p = defGetString(opt);

			if (strcmp(p, "xml") == 0)
				result_type = XMLOID;
			else if (strcmp(p, "json") == 0)
				result_type = JSONOID;
			else
				result_type = TEXTOID;
			/* don't "break", as ExplainQuery will use the last value */
			/*
			 *
			 * 不要 break，因为 ExplainQuery 会使用最后一个值。
			 */
		}
	}

	/* Need a tuple descriptor representing a single TEXT or XML column */
	/*
	 *
	 * 需要一个表示单列 TEXT 或 XML 的元组描述符。
	 */
	tupdesc = CreateTemplateTupleDesc(1);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "QUERY PLAN",
					   result_type, -1, 0);
	return tupdesc;
}

/*
 * ExplainOneQuery -
 *	  print out the execution plan for one Query
 *
 *	  打印一个 Query 的执行计划。
 *
 * "into" is NULL unless we are explaining the contents of a CreateTableAsStmt.
 *
 * 除非正在解释 CreateTableAsStmt 的内容，否则 into 为 NULL。
 */
static void
ExplainOneQuery(Query *query, int cursorOptions,
				IntoClause *into, ExplainState *es,
				ParseState *pstate, ParamListInfo params)
{
	/* planner will not cope with utility statements */
	/*
	 *
	 * 规划器无法处理实用语句。
	 */
	if (query->commandType == CMD_UTILITY)
	{
		ExplainOneUtility(query->utilityStmt, into, es, pstate, params);
		return;
	}

	/* if an advisor plugin is present, let it manage things */
	/*
	 *
	 * 若存在顾问插件，则交给它处理。
	 */
	if (ExplainOneQuery_hook)
		(*ExplainOneQuery_hook) (query, cursorOptions, into, es,
								 pstate->p_sourcetext, params, pstate->p_queryEnv);
	else
		standard_ExplainOneQuery(query, cursorOptions, into, es,
								 pstate->p_sourcetext, params, pstate->p_queryEnv);
}

/*
 * standard_ExplainOneQuery -
 *	  print out the execution plan for one Query, without calling a hook.
 *
 *	  打印一个 Query 的执行计划，且不调用钩子。
 */
void
standard_ExplainOneQuery(Query *query, int cursorOptions,
						 IntoClause *into, ExplainState *es,
						 const char *queryString, ParamListInfo params,
						 QueryEnvironment *queryEnv)
{
	PlannedStmt *plan;
	instr_time	planstart,
				planduration;
	BufferUsage bufusage_start,
				bufusage;
	MemoryContextCounters mem_counters;
	MemoryContext planner_ctx = NULL;
	MemoryContext saved_ctx = NULL;

	if (es->memory)
	{
		/*
		 * Create a new memory context to measure planner's memory consumption
		 * accurately.  Note that if the planner were to be modified to use a
		 * different memory context type, here we would be changing that to
		 * AllocSet, which might be undesirable.  However, we don't have a way
		 * to create a context of the same type as another, so we pray and
		 * hope that this is OK.
		 *
		 * 新建一个内存上下文，以便准确测量规划器的内存消耗。注意若规划器将来
		 * 改用另一种内存上下文类型，这里会把它换成 AllocSet，这可能并不理想。
		 * 但我们无法按另一个上下文的类型来创建上下文，因此只好假定这样做可以
		 * 接受。
		 */
		planner_ctx = AllocSetContextCreate(CurrentMemoryContext,
											"explain analyze planner context",
											ALLOCSET_DEFAULT_SIZES);
		saved_ctx = MemoryContextSwitchTo(planner_ctx);
	}

	if (es->buffers)
		bufusage_start = pgBufferUsage;
	INSTR_TIME_SET_CURRENT(planstart);

	/* plan the query */
	/*
	 *
	 * 规划该查询。
	 */
	plan = pg_plan_query(query, queryString, cursorOptions, params);

	INSTR_TIME_SET_CURRENT(planduration);
	INSTR_TIME_SUBTRACT(planduration, planstart);

	if (es->memory)
	{
		MemoryContextSwitchTo(saved_ctx);
		MemoryContextMemConsumed(planner_ctx, &mem_counters);
	}

	/* calc differences of buffer counters. */
	/*
	 *
	 * 计算缓冲区计数器的差值。
	 */
	if (es->buffers)
	{
		memset(&bufusage, 0, sizeof(BufferUsage));
		BufferUsageAccumDiff(&bufusage, &pgBufferUsage, &bufusage_start);
	}

	/* run it (if needed) and produce output */
	/*
	 *
	 * 在需要时执行它并产生输出。
	 */
	ExplainOnePlan(plan, into, es, queryString, params, queryEnv,
				   &planduration, (es->buffers ? &bufusage : NULL),
				   es->memory ? &mem_counters : NULL);
}

/*
 * ExplainOneUtility -
 *	  print out the execution plan for one utility statement
 *	  (In general, utility statements don't have plans, but there are some
 *	  we treat as special cases)
 *
 *	  打印一条实用语句的执行计划。（一般而言实用语句没有计划，但有些被当作特例。）
 *
 * "into" is NULL unless we are explaining the contents of a CreateTableAsStmt.
 *
 * 除非正在解释 CreateTableAsStmt 的内容，否则 into 为 NULL。
 *
 * This is exported because it's called back from prepare.c in the
 * EXPLAIN EXECUTE case.  In that case, we'll be dealing with a statement
 * that's in the plan cache, so we have to ensure we don't modify it.
 *
 * 本函数被导出，是因为 EXPLAIN EXECUTE 时由 prepare.c 回调。那种情况下语句位
 * 于计划缓存中，必须保证不去修改它。
 */
void
ExplainOneUtility(Node *utilityStmt, IntoClause *into, ExplainState *es,
				  ParseState *pstate, ParamListInfo params)
{
	if (utilityStmt == NULL)
		return;

	if (IsA(utilityStmt, CreateTableAsStmt))
	{
		/*
		 * We have to rewrite the contained SELECT and then pass it back to
		 * ExplainOneQuery.  Copy to be safe in the EXPLAIN EXECUTE case.
		 *
		 * 必须重写其中的 SELECT，再交回 ExplainOneQuery。在 EXPLAIN EXECUTE
		 * 情况下先复制一份以求安全。
		 */
		CreateTableAsStmt *ctas = (CreateTableAsStmt *) utilityStmt;
		Query	   *ctas_query;
		List	   *rewritten;
		JumbleState *jstate = NULL;

		/*
		 * Check if the relation exists or not.  This is done at this stage to
		 * avoid query planning or execution.
		 *
		 * 检查关系是否存在。放在这个阶段是为了避免查询规划或执行。
		 */
		if (CreateTableAsRelExists(ctas))
		{
			if (ctas->objtype == OBJECT_TABLE)
				ExplainDummyGroup("CREATE TABLE AS", NULL, es);
			else if (ctas->objtype == OBJECT_MATVIEW)
				ExplainDummyGroup("CREATE MATERIALIZED VIEW", NULL, es);
			else
				elog(ERROR, "unexpected object type: %d",
					 (int) ctas->objtype);
			return;
		}

		ctas_query = castNode(Query, copyObject(ctas->query));
		if (IsQueryIdEnabled())
			jstate = JumbleQuery(ctas_query);
		if (post_parse_analyze_hook)
			(*post_parse_analyze_hook) (pstate, ctas_query, jstate);
		rewritten = QueryRewrite(ctas_query);
		Assert(list_length(rewritten) == 1);
		ExplainOneQuery(linitial_node(Query, rewritten),
						CURSOR_OPT_PARALLEL_OK, ctas->into, es,
						pstate, params);
	}
	else if (IsA(utilityStmt, DeclareCursorStmt))
	{
		/*
		 * Likewise for DECLARE CURSOR.
		 *
		 * DECLARE CURSOR 同样处理。
		 *
		 * Notice that if you say EXPLAIN ANALYZE DECLARE CURSOR then we'll
		 * actually run the query.  This is different from pre-8.3 behavior
		 * but seems more useful than not running the query.  No cursor will
		 * be created, however.
		 *
		 * 注意，若执行 EXPLAIN ANALYZE DECLARE CURSOR，我们确实会运行该查询。
		 * 这与 8.3 之前的行为不同，但比不运行查询更有用。不过不会创建游标。
		 */
		DeclareCursorStmt *dcs = (DeclareCursorStmt *) utilityStmt;
		Query	   *dcs_query;
		List	   *rewritten;
		JumbleState *jstate = NULL;

		dcs_query = castNode(Query, copyObject(dcs->query));
		if (IsQueryIdEnabled())
			jstate = JumbleQuery(dcs_query);
		if (post_parse_analyze_hook)
			(*post_parse_analyze_hook) (pstate, dcs_query, jstate);

		rewritten = QueryRewrite(dcs_query);
		Assert(list_length(rewritten) == 1);
		ExplainOneQuery(linitial_node(Query, rewritten),
						dcs->options, NULL, es,
						pstate, params);
	}
	else if (IsA(utilityStmt, ExecuteStmt))
		ExplainExecuteQuery((ExecuteStmt *) utilityStmt, into, es,
							pstate, params);
	else if (IsA(utilityStmt, NotifyStmt))
	{
		if (es->format == EXPLAIN_FORMAT_TEXT)
			appendStringInfoString(es->str, "NOTIFY\n");
		else
			ExplainDummyGroup("Notify", NULL, es);
	}
	else
	{
		if (es->format == EXPLAIN_FORMAT_TEXT)
			appendStringInfoString(es->str,
								   "Utility statements have no plan structure\n");
		else
			ExplainDummyGroup("Utility Statement", NULL, es);
	}
}

/*
 * ExplainOnePlan -
 *		given a planned query, execute it if needed, and then print
 *		EXPLAIN output
 *
 *		给定已规划的查询，在需要时执行它，然后打印 EXPLAIN 输出。
 *
 * "into" is NULL unless we are explaining the contents of a CreateTableAsStmt,
 * in which case executing the query should result in creating that table.
 *
 * 除非正在解释 CreateTableAsStmt 的内容，否则 into 为 NULL；那种情况下执行查
 * 询应创建该表。
 *
 * This is exported because it's called back from prepare.c in the
 * EXPLAIN EXECUTE case, and because an index advisor plugin would need
 * to call it.
 *
 * 本函数被导出，是因为 EXPLAIN EXECUTE 时由 prepare.c 回调，而且索引顾问插件
 * 也需要调用它。
 */
void
ExplainOnePlan(PlannedStmt *plannedstmt, IntoClause *into, ExplainState *es,
			   const char *queryString, ParamListInfo params,
			   QueryEnvironment *queryEnv, const instr_time *planduration,
			   const BufferUsage *bufusage,
			   const MemoryContextCounters *mem_counters)
{
	DestReceiver *dest;
	QueryDesc  *queryDesc;
	instr_time	starttime;
	double		totaltime = 0;
	int			eflags;
	int			instrument_option = 0;
	SerializeMetrics serializeMetrics = {0};

	Assert(plannedstmt->commandType != CMD_UTILITY);

	if (es->analyze && es->timing)
		instrument_option |= INSTRUMENT_TIMER;
	else if (es->analyze)
		instrument_option |= INSTRUMENT_ROWS;

	if (es->buffers)
		instrument_option |= INSTRUMENT_BUFFERS;
	if (es->wal)
		instrument_option |= INSTRUMENT_WAL;

	/*
	 * We always collect timing for the entire statement, even when node-level
	 * timing is off, so we don't look at es->timing here.  (We could skip
	 * this if !es->summary, but it's hardly worth the complication.)
	 *
	 * 即使关闭了节点级计时，也始终收集整条语句的计时，因此这里不看 es->timing。
	 * （若 !es->summary 可以跳过，但为此增加复杂度不太值得。）
	 */
	INSTR_TIME_SET_CURRENT(starttime);

	/*
	 * Use a snapshot with an updated command ID to ensure this query sees
	 * results of any previously executed queries.
	 *
	 * 使用命令 ID 已更新的快照，确保本查询能看见先前已执行查询的结果。
	 */
	PushCopiedSnapshot(GetActiveSnapshot());
	UpdateActiveSnapshotCommandId();

	/*
	 * We discard the output if we have no use for it.  If we're explaining
	 * CREATE TABLE AS, we'd better use the appropriate tuple receiver, while
	 * the SERIALIZE option requires its own tuple receiver.  (If you specify
	 * SERIALIZE while explaining CREATE TABLE AS, you'll see zeroes for the
	 * results, which is appropriate since no data would have gone to the
	 * client.)
	 *
	 * 若用不到输出就丢弃它。解释 CREATE TABLE AS 时最好使用相应的元组接收器，
	 * 而 SERIALIZE 选项需要自己的元组接收器。（在解释 CREATE TABLE AS 时指定
	 * SERIALIZE，结果会显示为零，这是合适的，因为没有数据会发给客户端。）
	 */
	if (into)
		dest = CreateIntoRelDestReceiver(into);
	else if (es->serialize != EXPLAIN_SERIALIZE_NONE)
		dest = CreateExplainSerializeDestReceiver(es);
	else
		dest = None_Receiver;

	/* Create a QueryDesc for the query */
	/*
	 *
	 * 为该查询创建 QueryDesc。
	 */
	queryDesc = CreateQueryDesc(plannedstmt, queryString,
								GetActiveSnapshot(), InvalidSnapshot,
								dest, params, queryEnv, instrument_option);

	/* Select execution options */
	/*
	 *
	 * 选择执行选项。
	 */
	if (es->analyze)
		eflags = 0;				/* default run-to-completion flags */
		/*
		 *
		 * 默认的运行至完成标志。
		 */
	else
		eflags = EXEC_FLAG_EXPLAIN_ONLY;
	if (es->generic)
		eflags |= EXEC_FLAG_EXPLAIN_GENERIC;
	if (into)
		eflags |= GetIntoRelEFlags(into);

	/* call ExecutorStart to prepare the plan for execution */
	/*
	 *
	 * 调用 ExecutorStart 为执行准备计划。
	 */
	ExecutorStart(queryDesc, eflags);

	/* Execute the plan for statistics if asked for */
	/*
	 *
	 * 若被要求，则执行计划以收集统计信息。
	 */
	if (es->analyze)
	{
		ScanDirection dir;

		/* EXPLAIN ANALYZE CREATE TABLE AS WITH NO DATA is weird */
		/*
		 *
		 * EXPLAIN ANALYZE CREATE TABLE AS WITH NO DATA 的情况很特殊。
		 */
		if (into && into->skipData)
			dir = NoMovementScanDirection;
		else
			dir = ForwardScanDirection;

		/* run the plan */
		/*
		 *
		 * 运行该计划。
		 */
		ExecutorRun(queryDesc, dir, 0);

		/* run cleanup too */
		/*
		 *
		 * 同时运行清理。
		 */
		ExecutorFinish(queryDesc);

		/* We can't run ExecutorEnd 'till we're done printing the stats... */
		/*
		 *
		 * 在打印完统计信息之前不能运行 ExecutorEnd……
		 */
		totaltime += elapsed_time(&starttime);
	}

	/* grab serialization metrics before we destroy the DestReceiver */
	/*
	 *
	 * 在销毁 DestReceiver 之前取得序列化指标。
	 */
	if (es->serialize != EXPLAIN_SERIALIZE_NONE)
		serializeMetrics = GetSerializationMetrics(dest);

	/* call the DestReceiver's destroy method even during explain */
	/*
	 *
	 * 即使在 explain 期间也调用 DestReceiver 的 destroy 方法。
	 */
	dest->rDestroy(dest);

	ExplainOpenGroup("Query", NULL, true, es);

	/* Create textual dump of plan tree */
	/*
	 *
	 * 生成计划树的文本输出。
	 */
	ExplainPrintPlan(es, queryDesc);

	/* Show buffer and/or memory usage in planning */
	/*
	 *
	 * 显示规划期间的缓冲区和/或内存用量。
	 */
	if (peek_buffer_usage(es, bufusage) || mem_counters)
	{
		ExplainOpenGroup("Planning", "Planning", true, es);

		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			ExplainIndentText(es);
			appendStringInfoString(es->str, "Planning:\n");
			es->indent++;
		}

		if (bufusage)
			show_buffer_usage(es, bufusage);

		if (mem_counters)
			show_memory_counters(es, mem_counters);

		if (es->format == EXPLAIN_FORMAT_TEXT)
			es->indent--;

		ExplainCloseGroup("Planning", "Planning", true, es);
	}

	if (es->summary && planduration)
	{
		double		plantime = INSTR_TIME_GET_DOUBLE(*planduration);

		ExplainPropertyFloat("Planning Time", "ms", 1000.0 * plantime, 3, es);
	}

	/* Print info about runtime of triggers */
	/*
	 *
	 * 打印触发器运行时信息。
	 */
	if (es->analyze)
		ExplainPrintTriggers(es, queryDesc);

	/*
	 * Print info about JITing. Tied to es->costs because we don't want to
	 * display this in regression tests, as it'd cause output differences
	 * depending on build options.  Might want to separate that out from COSTS
	 * at a later stage.
	 *
	 * 打印 JIT 信息。把它绑在 es->costs 上，是因为不希望在回归测试里显示它，
	 * 否则会因编译选项不同而产生输出差异。以后也许要把它从 COSTS 里拆出来。
	 */
	if (es->costs)
		ExplainPrintJITSummary(es, queryDesc);

	/* Print info about serialization of output */
	/*
	 *
	 * 打印输出序列化的信息。
	 */
	if (es->serialize != EXPLAIN_SERIALIZE_NONE)
		ExplainPrintSerialize(es, &serializeMetrics);

	/* Allow plugins to print additional information */
	/*
	 *
	 * 允许插件打印附加信息。
	 */
	if (explain_per_plan_hook)
		(*explain_per_plan_hook) (plannedstmt, into, es, queryString,
								  params, queryEnv);

	/*
	 * Close down the query and free resources.  Include time for this in the
	 * total execution time (although it should be pretty minimal).
	 *
	 * 关闭查询并释放资源。把这段时间计入总执行时间（虽然它应该很短）。
	 */
	INSTR_TIME_SET_CURRENT(starttime);

	ExecutorEnd(queryDesc);

	FreeQueryDesc(queryDesc);

	PopActiveSnapshot();

	/* We need a CCI just in case query expanded to multiple plans */
	/*
	 *
	 * 需要一次 CommandCounterIncrement，以防查询展开成多个计划。
	 */
	if (es->analyze)
		CommandCounterIncrement();

	totaltime += elapsed_time(&starttime);

	/*
	 * We only report execution time if we actually ran the query (that is,
	 * the user specified ANALYZE), and if summary reporting is enabled (the
	 * user can set SUMMARY OFF to not have the timing information included in
	 * the output).  By default, ANALYZE sets SUMMARY to true.
	 *
	 * 只有真正运行了查询（即用户指定了 ANALYZE）且启用了摘要报告时，才报告执
	 * 行时间（用户可以 SET SUMMARY OFF 不把计时信息放进输出）。默认情况下
	 * ANALYZE 会把 SUMMARY 设为真。
	 */
	if (es->summary && es->analyze)
		ExplainPropertyFloat("Execution Time", "ms", 1000.0 * totaltime, 3,
							 es);

	ExplainCloseGroup("Query", NULL, true, es);
}

/*
 * ExplainPrintSettings -
 *    Print summary of modified settings affecting query planning.
 *
 *    打印影响查询规划的已修改设置的摘要。
 */
static void
ExplainPrintSettings(ExplainState *es)
{
	int			num;
	struct config_generic **gucs;

	/* bail out if information about settings not requested */
	/*
	 *
	 * 若未请求设置信息则直接返回。
	 */
	if (!es->settings)
		return;

	/* request an array of relevant settings */
	/*
	 *
	 * 请求相关设置的数组。
	 */
	gucs = get_explain_guc_options(&num);

	if (es->format != EXPLAIN_FORMAT_TEXT)
	{
		ExplainOpenGroup("Settings", "Settings", true, es);

		for (int i = 0; i < num; i++)
		{
			char	   *setting;
			struct config_generic *conf = gucs[i];

			setting = GetConfigOptionByName(conf->name, NULL, true);

			ExplainPropertyText(conf->name, setting, es);
		}

		ExplainCloseGroup("Settings", "Settings", true, es);
	}
	else
	{
		StringInfoData str;

		/* In TEXT mode, print nothing if there are no options */
		/*
		 *
		 * TEXT 模式下若没有任何选项则什么都不打印。
		 */
		if (num <= 0)
			return;

		initStringInfo(&str);

		for (int i = 0; i < num; i++)
		{
			char	   *setting;
			struct config_generic *conf = gucs[i];

			if (i > 0)
				appendStringInfoString(&str, ", ");

			setting = GetConfigOptionByName(conf->name, NULL, true);

			if (setting)
				appendStringInfo(&str, "%s = '%s'", conf->name, setting);
			else
				appendStringInfo(&str, "%s = NULL", conf->name);
		}

		ExplainPropertyText("Settings", str.data, es);
	}
}

/*
 * ExplainPrintPlan -
 *	  convert a QueryDesc's plan tree to text and append it to es->str
 *
 *	  把 QueryDesc 的计划树转换成文本并追加到 es->str。
 *
 * The caller should have set up the options fields of *es, as well as
 * initializing the output buffer es->str.  Also, output formatting state
 * such as the indent level is assumed valid.  Plan-tree-specific fields
 * in *es are initialized here.
 *
 * 调用方应已设置 *es 的选项字段，并初始化输出缓冲区 es->str。另外假定缩进级别
 * 等输出格式状态有效。*es 中与计划树相关的字段在这里初始化。
 *
 * NB: will not work on utility statements
 *
 * 注意：不能用于实用语句。
 */
void
ExplainPrintPlan(ExplainState *es, QueryDesc *queryDesc)
{
	Bitmapset  *rels_used = NULL;
	PlanState  *ps;
	ListCell   *lc;

	/* Set up ExplainState fields associated with this plan tree */
	/*
	 *
	 * 设置与该计划树相关的 ExplainState 字段。
	 */
	Assert(queryDesc->plannedstmt != NULL);
	es->pstmt = queryDesc->plannedstmt;
	es->rtable = queryDesc->plannedstmt->rtable;
	ExplainPreScanNode(queryDesc->planstate, &rels_used);
	es->rtable_names = select_rtable_names_for_explain(es->rtable, rels_used);
	es->deparse_cxt = deparse_context_for_plan_tree(queryDesc->plannedstmt,
													es->rtable_names);
	es->printed_subplans = NULL;
	es->rtable_size = list_length(es->rtable);
	foreach(lc, es->rtable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, lc);

		if (rte->rtekind == RTE_GROUP)
		{
			es->rtable_size--;
			break;
		}
	}

	/*
	 * Sometimes we mark a Gather node as "invisible", which means that it's
	 * not to be displayed in EXPLAIN output.  The purpose of this is to allow
	 * running regression tests with debug_parallel_query=regress to get the
	 * same results as running the same tests with debug_parallel_query=off.
	 * Such marking is currently only supported on a Gather at the top of the
	 * plan.  We skip that node, and we must also hide per-worker detail data
	 * further down in the plan tree.
	 *
	 * 有时把 Gather 节点标为 invisible，表示不在 EXPLAIN 输出中显示。目的是让
	 * 回归测试在 debug_parallel_query=regress 下得到与 debug_parallel_query=
	 * off 相同的结果。目前只支持标记计划顶层的 Gather。我们跳过该节点，并且必
	 * 须隐藏计划树更深处的逐 worker 细节。
	 */
	ps = queryDesc->planstate;
	if (IsA(ps, GatherState) && ((Gather *) ps->plan)->invisible)
	{
		ps = outerPlanState(ps);
		es->hide_workers = true;
	}
	ExplainNode(ps, NIL, NULL, NULL, es);

	/*
	 * If requested, include information about GUC parameters with values that
	 * don't match the built-in defaults.
	 *
	 * 若被请求，则包含取值与内建默认值不同的 GUC 参数信息。
	 */
	ExplainPrintSettings(es);

	/*
	 * COMPUTE_QUERY_ID_REGRESS means COMPUTE_QUERY_ID_AUTO, but we don't show
	 * the queryid in any of the EXPLAIN plans to keep stable the results
	 * generated by regression test suites.
	 *
	 * COMPUTE_QUERY_ID_REGRESS 的含义同 COMPUTE_QUERY_ID_AUTO，但我们不在任何
	 * EXPLAIN 计划中显示 queryid，以便回归测试套件的结果保持稳定。
	 */
	if (es->verbose && queryDesc->plannedstmt->queryId != INT64CONST(0) &&
		compute_query_id != COMPUTE_QUERY_ID_REGRESS)
	{
		ExplainPropertyInteger("Query Identifier", NULL,
							   queryDesc->plannedstmt->queryId, es);
	}
}

/*
 * ExplainPrintTriggers -
 *	  convert a QueryDesc's trigger statistics to text and append it to
 *	  es->str
 *
 *	  把 QueryDesc 的触发器统计转换成文本并追加到 es->str。
 *
 * The caller should have set up the options fields of *es, as well as
 * initializing the output buffer es->str.  Other fields in *es are
 * initialized here.
 *
 * 调用方应已设置 *es 的选项字段，并初始化输出缓冲区 es->str。*es 的其他字段在
 * 这里初始化。
 */
void
ExplainPrintTriggers(ExplainState *es, QueryDesc *queryDesc)
{
	ResultRelInfo *rInfo;
	bool		show_relname;
	List	   *resultrels;
	List	   *routerels;
	List	   *targrels;
	ListCell   *l;

	resultrels = queryDesc->estate->es_opened_result_relations;
	routerels = queryDesc->estate->es_tuple_routing_result_relations;
	targrels = queryDesc->estate->es_trig_target_relations;

	ExplainOpenGroup("Triggers", "Triggers", false, es);

	show_relname = (list_length(resultrels) > 1 ||
					routerels != NIL || targrels != NIL);
	foreach(l, resultrels)
	{
		rInfo = (ResultRelInfo *) lfirst(l);
		report_triggers(rInfo, show_relname, es);
	}

	foreach(l, routerels)
	{
		rInfo = (ResultRelInfo *) lfirst(l);
		report_triggers(rInfo, show_relname, es);
	}

	foreach(l, targrels)
	{
		rInfo = (ResultRelInfo *) lfirst(l);
		report_triggers(rInfo, show_relname, es);
	}

	ExplainCloseGroup("Triggers", "Triggers", false, es);
}

/*
 * ExplainPrintJITSummary -
 *    Print summarized JIT instrumentation from leader and workers
 *
 *    打印 leader 与 worker 的 JIT 插桩汇总。
 */
void
ExplainPrintJITSummary(ExplainState *es, QueryDesc *queryDesc)
{
	JitInstrumentation ji = {0};

	if (!(queryDesc->estate->es_jit_flags & PGJIT_PERFORM))
		return;

	/*
	 * Work with a copy instead of modifying the leader state, since this
	 * function may be called twice
	 *
	 * 在副本上工作，而不是修改 leader 的状态，因为本函数可能被调用两次。
	 */
	if (queryDesc->estate->es_jit)
		InstrJitAgg(&ji, &queryDesc->estate->es_jit->instr);

	/* If this process has done JIT in parallel workers, merge stats */
	/*
	 *
	 * 若本进程在并行 worker 中做过 JIT，则合并统计信息。
	 */
	if (queryDesc->estate->es_jit_worker_instr)
		InstrJitAgg(&ji, queryDesc->estate->es_jit_worker_instr);

	ExplainPrintJIT(es, queryDesc->estate->es_jit_flags, &ji);
}

/*
 * ExplainPrintJIT -
 *	  Append information about JITing to es->str.
 *
 *	  把 JIT 信息追加到 es->str。
 */
static void
ExplainPrintJIT(ExplainState *es, int jit_flags, JitInstrumentation *ji)
{
	instr_time	total_time;

	/* don't print information if no JITing happened */
	/*
	 *
	 * 若没有发生 JIT 则不打印信息。
	 */
	if (!ji || ji->created_functions == 0)
		return;

	/* calculate total time */
	/*
	 *
	 * 计算总时间。
	 */
	INSTR_TIME_SET_ZERO(total_time);
	/* don't add deform_counter, it's included in generation_counter */
	/*
	 *
	 * 不要加上 deform_counter，它已包含在 generation_counter 中。
	 */
	INSTR_TIME_ADD(total_time, ji->generation_counter);
	INSTR_TIME_ADD(total_time, ji->inlining_counter);
	INSTR_TIME_ADD(total_time, ji->optimization_counter);
	INSTR_TIME_ADD(total_time, ji->emission_counter);

	ExplainOpenGroup("JIT", "JIT", true, es);

	/* for higher density, open code the text output format */
	/*
	 *
	 * 为了更紧凑，文本输出格式在这里直接展开编写。
	 */
	if (es->format == EXPLAIN_FORMAT_TEXT)
	{
		ExplainIndentText(es);
		appendStringInfoString(es->str, "JIT:\n");
		es->indent++;

		ExplainPropertyInteger("Functions", NULL, ji->created_functions, es);

		ExplainIndentText(es);
		appendStringInfo(es->str, "Options: %s %s, %s %s, %s %s, %s %s\n",
						 "Inlining", jit_flags & PGJIT_INLINE ? "true" : "false",
						 "Optimization", jit_flags & PGJIT_OPT3 ? "true" : "false",
						 "Expressions", jit_flags & PGJIT_EXPR ? "true" : "false",
						 "Deforming", jit_flags & PGJIT_DEFORM ? "true" : "false");

		if (es->analyze && es->timing)
		{
			ExplainIndentText(es);
			appendStringInfo(es->str,
							 "Timing: %s %.3f ms (%s %.3f ms), %s %.3f ms, %s %.3f ms, %s %.3f ms, %s %.3f ms\n",
							 "Generation", 1000.0 * INSTR_TIME_GET_DOUBLE(ji->generation_counter),
							 "Deform", 1000.0 * INSTR_TIME_GET_DOUBLE(ji->deform_counter),
							 "Inlining", 1000.0 * INSTR_TIME_GET_DOUBLE(ji->inlining_counter),
							 "Optimization", 1000.0 * INSTR_TIME_GET_DOUBLE(ji->optimization_counter),
							 "Emission", 1000.0 * INSTR_TIME_GET_DOUBLE(ji->emission_counter),
							 "Total", 1000.0 * INSTR_TIME_GET_DOUBLE(total_time));
		}

		es->indent--;
	}
	else
	{
		ExplainPropertyInteger("Functions", NULL, ji->created_functions, es);

		ExplainOpenGroup("Options", "Options", true, es);
		ExplainPropertyBool("Inlining", jit_flags & PGJIT_INLINE, es);
		ExplainPropertyBool("Optimization", jit_flags & PGJIT_OPT3, es);
		ExplainPropertyBool("Expressions", jit_flags & PGJIT_EXPR, es);
		ExplainPropertyBool("Deforming", jit_flags & PGJIT_DEFORM, es);
		ExplainCloseGroup("Options", "Options", true, es);

		if (es->analyze && es->timing)
		{
			ExplainOpenGroup("Timing", "Timing", true, es);

			ExplainOpenGroup("Generation", "Generation", true, es);
			ExplainPropertyFloat("Deform", "ms",
								 1000.0 * INSTR_TIME_GET_DOUBLE(ji->deform_counter),
								 3, es);
			ExplainPropertyFloat("Total", "ms",
								 1000.0 * INSTR_TIME_GET_DOUBLE(ji->generation_counter),
								 3, es);
			ExplainCloseGroup("Generation", "Generation", true, es);

			ExplainPropertyFloat("Inlining", "ms",
								 1000.0 * INSTR_TIME_GET_DOUBLE(ji->inlining_counter),
								 3, es);
			ExplainPropertyFloat("Optimization", "ms",
								 1000.0 * INSTR_TIME_GET_DOUBLE(ji->optimization_counter),
								 3, es);
			ExplainPropertyFloat("Emission", "ms",
								 1000.0 * INSTR_TIME_GET_DOUBLE(ji->emission_counter),
								 3, es);
			ExplainPropertyFloat("Total", "ms",
								 1000.0 * INSTR_TIME_GET_DOUBLE(total_time),
								 3, es);

			ExplainCloseGroup("Timing", "Timing", true, es);
		}
	}

	ExplainCloseGroup("JIT", "JIT", true, es);
}

/*
 * ExplainPrintSerialize -
 *	  Append information about query output volume to es->str.
 *
 *	  把查询输出量信息追加到 es->str。
 */
static void
ExplainPrintSerialize(ExplainState *es, SerializeMetrics *metrics)
{
	const char *format;

	/* We shouldn't get called for EXPLAIN_SERIALIZE_NONE */
	/*
	 *
	 * EXPLAIN_SERIALIZE_NONE 时不应调用到这里。
	 */
	if (es->serialize == EXPLAIN_SERIALIZE_TEXT)
		format = "text";
	else
	{
		Assert(es->serialize == EXPLAIN_SERIALIZE_BINARY);
		format = "binary";
	}

	ExplainOpenGroup("Serialization", "Serialization", true, es);

	if (es->format == EXPLAIN_FORMAT_TEXT)
	{
		ExplainIndentText(es);
		if (es->timing)
			appendStringInfo(es->str, "Serialization: time=%.3f ms  output=" UINT64_FORMAT "kB  format=%s\n",
							 1000.0 * INSTR_TIME_GET_DOUBLE(metrics->timeSpent),
							 BYTES_TO_KILOBYTES(metrics->bytesSent),
							 format);
		else
			appendStringInfo(es->str, "Serialization: output=" UINT64_FORMAT "kB  format=%s\n",
							 BYTES_TO_KILOBYTES(metrics->bytesSent),
							 format);

		if (es->buffers && peek_buffer_usage(es, &metrics->bufferUsage))
		{
			es->indent++;
			show_buffer_usage(es, &metrics->bufferUsage);
			es->indent--;
		}
	}
	else
	{
		if (es->timing)
			ExplainPropertyFloat("Time", "ms",
								 1000.0 * INSTR_TIME_GET_DOUBLE(metrics->timeSpent),
								 3, es);
		ExplainPropertyUInteger("Output Volume", "kB",
								BYTES_TO_KILOBYTES(metrics->bytesSent), es);
		ExplainPropertyText("Format", format, es);
		if (es->buffers)
			show_buffer_usage(es, &metrics->bufferUsage);
	}

	ExplainCloseGroup("Serialization", "Serialization", true, es);
}

/*
 * ExplainQueryText -
 *	  add a "Query Text" node that contains the actual text of the query
 *
 *	  添加包含查询实际文本的 Query Text 节点。
 *
 * The caller should have set up the options fields of *es, as well as
 * initializing the output buffer es->str.
 *
 * 调用方应已设置 *es 的选项字段，并初始化输出缓冲区 es->str。
 *
 */
void
ExplainQueryText(ExplainState *es, QueryDesc *queryDesc)
{
	if (queryDesc->sourceText)
		ExplainPropertyText("Query Text", queryDesc->sourceText, es);
}

/*
 * ExplainQueryParameters -
 *	  add a "Query Parameters" node that describes the parameters of the query
 *
 *	  添加描述查询参数的 Query Parameters 节点。
 *
 * The caller should have set up the options fields of *es, as well as
 * initializing the output buffer es->str.
 *
 * 调用方应已设置 *es 的选项字段，并初始化输出缓冲区 es->str。
 *
 */
void
ExplainQueryParameters(ExplainState *es, ParamListInfo params, int maxlen)
{
	char	   *str;

	/* This check is consistent with errdetail_params() */
	/*
	 *
	 * 这项检查与 errdetail_params() 一致。
	 */
	if (params == NULL || params->numParams <= 0 || maxlen == 0)
		return;

	str = BuildParamLogString(params, NULL, maxlen);
	if (str && str[0] != '\0')
		ExplainPropertyText("Query Parameters", str, es);
}

/*
 * report_triggers -
 *		report execution stats for a single relation's triggers
 *
 *		报告单个关系的触发器执行统计。
 */
static void
report_triggers(ResultRelInfo *rInfo, bool show_relname, ExplainState *es)
{
	int			nt;

	if (!rInfo->ri_TrigDesc || !rInfo->ri_TrigInstrument)
		return;
	for (nt = 0; nt < rInfo->ri_TrigDesc->numtriggers; nt++)
	{
		Trigger    *trig = rInfo->ri_TrigDesc->triggers + nt;
		Instrumentation *instr = rInfo->ri_TrigInstrument + nt;
		char	   *relname;
		char	   *conname = NULL;

		/* Must clean up instrumentation state */
		/*
		 *
		 * 必须清理插桩状态。
		 */
		InstrEndLoop(instr);

		/*
		 * We ignore triggers that were never invoked; they likely aren't
		 * relevant to the current query type.
		 *
		 * 忽略从未被调用的触发器；它们多半与当前查询类型无关。
		 */
		if (instr->ntuples == 0)
			continue;

		ExplainOpenGroup("Trigger", NULL, true, es);

		relname = RelationGetRelationName(rInfo->ri_RelationDesc);
		if (OidIsValid(trig->tgconstraint))
			conname = get_constraint_name(trig->tgconstraint);

		/*
		 * In text format, we avoid printing both the trigger name and the
		 * constraint name unless VERBOSE is specified.  In non-text formats
		 * we just print everything.
		 *
		 * 文本格式下，除非指定了 VERBOSE，否则避免同时打印触发器名和约束名。
		 * 非文本格式则全部打印。
		 */
		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			if (es->verbose || conname == NULL)
				appendStringInfo(es->str, "Trigger %s", trig->tgname);
			else
				appendStringInfoString(es->str, "Trigger");
			if (conname)
				appendStringInfo(es->str, " for constraint %s", conname);
			if (show_relname)
				appendStringInfo(es->str, " on %s", relname);
			if (es->timing)
				appendStringInfo(es->str, ": time=%.3f calls=%.0f\n",
								 1000.0 * instr->total, instr->ntuples);
			else
				appendStringInfo(es->str, ": calls=%.0f\n", instr->ntuples);
		}
		else
		{
			ExplainPropertyText("Trigger Name", trig->tgname, es);
			if (conname)
				ExplainPropertyText("Constraint Name", conname, es);
			ExplainPropertyText("Relation", relname, es);
			if (es->timing)
				ExplainPropertyFloat("Time", "ms", 1000.0 * instr->total, 3,
									 es);
			ExplainPropertyFloat("Calls", NULL, instr->ntuples, 0, es);
		}

		if (conname)
			pfree(conname);

		ExplainCloseGroup("Trigger", NULL, true, es);
	}
}

/* Compute elapsed time in seconds since given timestamp */
/*
 *
 * 计算自给定时间戳以来经过的秒数。
 */
static double
elapsed_time(instr_time *starttime)
{
	instr_time	endtime;

	INSTR_TIME_SET_CURRENT(endtime);
	INSTR_TIME_SUBTRACT(endtime, *starttime);
	return INSTR_TIME_GET_DOUBLE(endtime);
}

/*
 * ExplainPreScanNode -
 *	  Prescan the planstate tree to identify which RTEs are referenced
 *
 *	  预扫描计划状态树，识别被引用的 RTE。
 *
 * Adds the relid of each referenced RTE to *rels_used.  The result controls
 * which RTEs are assigned aliases by select_rtable_names_for_explain.
 * This ensures that we don't confusingly assign un-suffixed aliases to RTEs
 * that never appear in the EXPLAIN output (such as inheritance parents).
 *
 * 把每个被引用 RTE 的 relid 加入 *rels_used。结果控制
 * select_rtable_names_for_explain 为哪些 RTE 分配别名。这样就不会把不带后缀的
 * 别名混乱地分给从未出现在 EXPLAIN 输出中的 RTE（例如继承父表）。
 */
static bool
ExplainPreScanNode(PlanState *planstate, Bitmapset **rels_used)
{
	Plan	   *plan = planstate->plan;

	switch (nodeTag(plan))
	{
		case T_SeqScan:
		case T_SampleScan:
		case T_IndexScan:
		case T_IndexOnlyScan:
		case T_BitmapHeapScan:
		case T_TidScan:
		case T_TidRangeScan:
		case T_SubqueryScan:
		case T_FunctionScan:
		case T_TableFuncScan:
		case T_ValuesScan:
		case T_CteScan:
		case T_NamedTuplestoreScan:
		case T_WorkTableScan:
			*rels_used = bms_add_member(*rels_used,
										((Scan *) plan)->scanrelid);
			break;
		case T_ForeignScan:
			*rels_used = bms_add_members(*rels_used,
										 ((ForeignScan *) plan)->fs_base_relids);
			break;
		case T_CustomScan:
			*rels_used = bms_add_members(*rels_used,
										 ((CustomScan *) plan)->custom_relids);
			break;
		case T_ModifyTable:
			*rels_used = bms_add_member(*rels_used,
										((ModifyTable *) plan)->nominalRelation);
			if (((ModifyTable *) plan)->exclRelRTI)
				*rels_used = bms_add_member(*rels_used,
											((ModifyTable *) plan)->exclRelRTI);
			/* Ensure Vars used in RETURNING will have refnames */
			/*
			 *
			 * 确保 RETURNING 中使用的 Var 都有引用名。
			 */
			if (plan->targetlist)
				*rels_used = bms_add_member(*rels_used,
											linitial_int(((ModifyTable *) plan)->resultRelations));
			break;
		case T_Append:
			*rels_used = bms_add_members(*rels_used,
										 ((Append *) plan)->apprelids);
			break;
		case T_MergeAppend:
			*rels_used = bms_add_members(*rels_used,
										 ((MergeAppend *) plan)->apprelids);
			break;
		default:
			break;
	}

	return planstate_tree_walker(planstate, ExplainPreScanNode, rels_used);
}

/*
 * plan_is_disabled
 *		Checks if the given plan node type was disabled during query planning.
 *		This is evident by the disabled_nodes field being higher than the sum of
 *		the disabled_nodes field from the plan's children.
 *
 *		检查给定计划节点类型是否在查询规划期间被禁用。依据是 disabled_nodes 高
 *		于其子计划 disabled_nodes 之和。
 */
static bool
plan_is_disabled(Plan *plan)
{
	int			child_disabled_nodes;

	/* The node is certainly not disabled if this is zero */
	/*
	 *
	 * 若该值为零，则节点肯定没有被禁用。
	 */
	if (plan->disabled_nodes == 0)
		return false;

	child_disabled_nodes = 0;

	/*
	 * Handle special nodes first.  Children of BitmapOrs and BitmapAnds can't
	 * be disabled, so no need to handle those specifically.
	 *
	 * 先处理特殊节点。BitmapOr 和 BitmapAnd 的子节点不能被禁用，因此不必专门
	 * 处理它们。
	 */
	if (IsA(plan, Append))
	{
		ListCell   *lc;
		Append	   *aplan = (Append *) plan;

		/*
		 * Sum the Append childrens' disabled_nodes.  This purposefully
		 * includes any run-time pruned children.  Ignoring those could give
		 * us the incorrect number of disabled nodes.
		 *
		 * 累加 Append 子节点的 disabled_nodes。这里有意包含运行时被剪枝的子节
		 * 点。忽略它们会得到错误的禁用节点数。
		 */
		foreach(lc, aplan->appendplans)
		{
			Plan	   *subplan = lfirst(lc);

			child_disabled_nodes += subplan->disabled_nodes;
		}
	}
	else if (IsA(plan, MergeAppend))
	{
		ListCell   *lc;
		MergeAppend *maplan = (MergeAppend *) plan;

		/*
		 * Sum the MergeAppend childrens' disabled_nodes.  This purposefully
		 * includes any run-time pruned children.  Ignoring those could give
		 * us the incorrect number of disabled nodes.
		 *
		 * 累加 MergeAppend 子节点的 disabled_nodes。这里有意包含运行时被剪枝
		 * 的子节点。忽略它们会得到错误的禁用节点数。
		 */
		foreach(lc, maplan->mergeplans)
		{
			Plan	   *subplan = lfirst(lc);

			child_disabled_nodes += subplan->disabled_nodes;
		}
	}
	else if (IsA(plan, SubqueryScan))
		child_disabled_nodes += ((SubqueryScan *) plan)->subplan->disabled_nodes;
	else if (IsA(plan, CustomScan))
	{
		ListCell   *lc;
		CustomScan *cplan = (CustomScan *) plan;

		foreach(lc, cplan->custom_plans)
		{
			Plan	   *subplan = lfirst(lc);

			child_disabled_nodes += subplan->disabled_nodes;
		}
	}
	else
	{
		/*
		 * Else, sum up disabled_nodes from the plan's inner and outer side.
		 *
		 * 否则，累加计划内侧和外侧的 disabled_nodes。
		 */
		if (outerPlan(plan))
			child_disabled_nodes += outerPlan(plan)->disabled_nodes;
		if (innerPlan(plan))
			child_disabled_nodes += innerPlan(plan)->disabled_nodes;
	}

	/*
	 * It's disabled if the plan's disabled_nodes is higher than the sum of
	 * its child's plan disabled_nodes.
	 *
	 * 若计划的 disabled_nodes 高于其子计划 disabled_nodes 之和，则它被禁用。
	 */
	if (plan->disabled_nodes > child_disabled_nodes)
		return true;

	return false;
}

/*
 * ExplainNode -
 *	  Appends a description of a plan tree to es->str
 *
 *	  把计划树的描述追加到 es->str。
 *
 * planstate points to the executor state node for the current plan node.
 * We need to work from a PlanState node, not just a Plan node, in order to
 * get at the instrumentation data (if any) as well as the list of subplans.
 *
 * planstate 指向当前计划节点的执行器状态节点。必须从 PlanState 而不是仅仅从
 * Plan 出发，才能取得插桩数据（若有）以及子计划列表。
 *
 * ancestors is a list of parent Plan and SubPlan nodes, most-closely-nested
 * first.  These are needed in order to interpret PARAM_EXEC Params.
 *
 * ancestors 是父 Plan 和 SubPlan 节点的列表，嵌套最深的在前。解释 PARAM_EXEC
 * 参数时需要它们。
 *
 * relationship describes the relationship of this plan node to its parent
 * (eg, "Outer", "Inner"); it can be null at top level.  plan_name is an
 * optional name to be attached to the node.
 *
 * relationship 描述该计划节点与其父节点的关系（例如 Outer、Inner）；顶层可以
 * 为空。plan_name 是可选的、要附在节点上的名字。
 *
 * In text format, es->indent is controlled in this function since we only
 * want it to change at plan-node boundaries (but a few subroutines will
 * transiently increment it).  In non-text formats, es->indent corresponds
 * to the nesting depth of logical output groups, and therefore is controlled
 * by ExplainOpenGroup/ExplainCloseGroup.
 *
 * 文本格式下，es->indent 由本函数控制，因为只希望它在计划节点边界变化（少数子
 * 程序会临时增加它）。非文本格式下，es->indent 对应逻辑输出组的嵌套深度，因此
 * 由 ExplainOpenGroup/ExplainCloseGroup 控制。
 */
static void
ExplainNode(PlanState *planstate, List *ancestors,
			const char *relationship, const char *plan_name,
			ExplainState *es)
{
	Plan	   *plan = planstate->plan;
	const char *pname;			/* node type name for text output */
	/*
	 *
	 * 文本输出用的节点类型名。
	 */
	const char *sname;			/* node type name for non-text output */
	/*
	 *
	 * 非文本输出用的节点类型名。
	 */
	const char *strategy = NULL;
	const char *partialmode = NULL;
	const char *operation = NULL;
	const char *custom_name = NULL;
	ExplainWorkersState *save_workers_state = es->workers_state;
	int			save_indent = es->indent;
	bool		haschildren;
	bool		isdisabled;

	/*
	 * Prepare per-worker output buffers, if needed.  We'll append the data in
	 * these to the main output string further down.
	 *
	 * 若需要，准备逐 worker 的输出缓冲区。稍后会把这些数据追加到主输出字符串。
	 */
	if (planstate->worker_instrument && es->analyze && !es->hide_workers)
		es->workers_state = ExplainCreateWorkersState(planstate->worker_instrument->num_workers);
	else
		es->workers_state = NULL;

	/* Identify plan node type, and print generic details */
	/*
	 *
	 * 识别计划节点类型，并打印通用细节。
	 */
	switch (nodeTag(plan))
	{
		case T_Result:
			pname = sname = "Result";
			break;
		case T_ProjectSet:
			pname = sname = "ProjectSet";
			break;
		case T_ModifyTable:
			sname = "ModifyTable";
			switch (((ModifyTable *) plan)->operation)
			{
				case CMD_INSERT:
					pname = operation = "Insert";
					break;
				case CMD_UPDATE:
					pname = operation = "Update";
					break;
				case CMD_DELETE:
					pname = operation = "Delete";
					break;
				case CMD_MERGE:
					pname = operation = "Merge";
					break;
				default:
					pname = "???";
					break;
			}
			break;
		case T_Append:
			pname = sname = "Append";
			break;
		case T_MergeAppend:
			pname = sname = "Merge Append";
			break;
		case T_RecursiveUnion:
			pname = sname = "Recursive Union";
			break;
		case T_BitmapAnd:
			pname = sname = "BitmapAnd";
			break;
		case T_BitmapOr:
			pname = sname = "BitmapOr";
			break;
		case T_NestLoop:
			pname = sname = "Nested Loop";
			break;
		case T_MergeJoin:
			pname = "Merge";	/* "Join" gets added by jointype switch */
			/*
			 *
			 * Join 由 jointype 的 switch 补上。
			 */
			sname = "Merge Join";
			break;
		case T_HashJoin:
			pname = "Hash";		/* "Join" gets added by jointype switch */
			/*
			 *
			 * Join 由 jointype 的 switch 补上。
			 */
			sname = "Hash Join";
			break;
		case T_SeqScan:
			pname = sname = "Seq Scan";
			break;
		case T_SampleScan:
			pname = sname = "Sample Scan";
			break;
		case T_Gather:
			pname = sname = "Gather";
			break;
		case T_GatherMerge:
			pname = sname = "Gather Merge";
			break;
		case T_IndexScan:
			pname = sname = "Index Scan";
			break;
		case T_IndexOnlyScan:
			pname = sname = "Index Only Scan";
			break;
		case T_BitmapIndexScan:
			pname = sname = "Bitmap Index Scan";
			break;
		case T_BitmapHeapScan:
			pname = sname = "Bitmap Heap Scan";
			break;
		case T_TidScan:
			pname = sname = "Tid Scan";
			break;
		case T_TidRangeScan:
			pname = sname = "Tid Range Scan";
			break;
		case T_SubqueryScan:
			pname = sname = "Subquery Scan";
			break;
		case T_FunctionScan:
			pname = sname = "Function Scan";
			break;
		case T_TableFuncScan:
			pname = sname = "Table Function Scan";
			break;
		case T_ValuesScan:
			pname = sname = "Values Scan";
			break;
		case T_CteScan:
			pname = sname = "CTE Scan";
			break;
		case T_NamedTuplestoreScan:
			pname = sname = "Named Tuplestore Scan";
			break;
		case T_WorkTableScan:
			pname = sname = "WorkTable Scan";
			break;
		case T_ForeignScan:
			sname = "Foreign Scan";
			switch (((ForeignScan *) plan)->operation)
			{
				case CMD_SELECT:
					pname = "Foreign Scan";
					operation = "Select";
					break;
				case CMD_INSERT:
					pname = "Foreign Insert";
					operation = "Insert";
					break;
				case CMD_UPDATE:
					pname = "Foreign Update";
					operation = "Update";
					break;
				case CMD_DELETE:
					pname = "Foreign Delete";
					operation = "Delete";
					break;
				default:
					pname = "???";
					break;
			}
			break;
		case T_CustomScan:
			sname = "Custom Scan";
			custom_name = ((CustomScan *) plan)->methods->CustomName;
			if (custom_name)
				pname = psprintf("Custom Scan (%s)", custom_name);
			else
				pname = sname;
			break;
		case T_Material:
			pname = sname = "Materialize";
			break;
		case T_Memoize:
			pname = sname = "Memoize";
			break;
		case T_Sort:
			pname = sname = "Sort";
			break;
		case T_IncrementalSort:
			pname = sname = "Incremental Sort";
			break;
		case T_Group:
			pname = sname = "Group";
			break;
		case T_Agg:
			{
				Agg		   *agg = (Agg *) plan;

				sname = "Aggregate";
				switch (agg->aggstrategy)
				{
					case AGG_PLAIN:
						pname = "Aggregate";
						strategy = "Plain";
						break;
					case AGG_SORTED:
						pname = "GroupAggregate";
						strategy = "Sorted";
						break;
					case AGG_HASHED:
						pname = "HashAggregate";
						strategy = "Hashed";
						break;
					case AGG_MIXED:
						pname = "MixedAggregate";
						strategy = "Mixed";
						break;
					default:
						pname = "Aggregate ???";
						strategy = "???";
						break;
				}

				if (DO_AGGSPLIT_SKIPFINAL(agg->aggsplit))
				{
					partialmode = "Partial";
					pname = psprintf("%s %s", partialmode, pname);
				}
				else if (DO_AGGSPLIT_COMBINE(agg->aggsplit))
				{
					partialmode = "Finalize";
					pname = psprintf("%s %s", partialmode, pname);
				}
				else
					partialmode = "Simple";
			}
			break;
		case T_WindowAgg:
			pname = sname = "WindowAgg";
			break;
		case T_Unique:
			pname = sname = "Unique";
			break;
		case T_SetOp:
			sname = "SetOp";
			switch (((SetOp *) plan)->strategy)
			{
				case SETOP_SORTED:
					pname = "SetOp";
					strategy = "Sorted";
					break;
				case SETOP_HASHED:
					pname = "HashSetOp";
					strategy = "Hashed";
					break;
				default:
					pname = "SetOp ???";
					strategy = "???";
					break;
			}
			break;
		case T_LockRows:
			pname = sname = "LockRows";
			break;
		case T_Limit:
			pname = sname = "Limit";
			break;
		case T_Hash:
			pname = sname = "Hash";
			break;
		default:
			pname = sname = "???";
			break;
	}

	ExplainOpenGroup("Plan",
					 relationship ? NULL : "Plan",
					 true, es);

	if (es->format == EXPLAIN_FORMAT_TEXT)
	{
		if (plan_name)
		{
			ExplainIndentText(es);
			appendStringInfo(es->str, "%s\n", plan_name);
			es->indent++;
		}
		if (es->indent)
		{
			ExplainIndentText(es);
			appendStringInfoString(es->str, "->  ");
			es->indent += 2;
		}
		if (plan->parallel_aware)
			appendStringInfoString(es->str, "Parallel ");
		if (plan->async_capable)
			appendStringInfoString(es->str, "Async ");
		appendStringInfoString(es->str, pname);
		es->indent++;
	}
	else
	{
		ExplainPropertyText("Node Type", sname, es);
		if (strategy)
			ExplainPropertyText("Strategy", strategy, es);
		if (partialmode)
			ExplainPropertyText("Partial Mode", partialmode, es);
		if (operation)
			ExplainPropertyText("Operation", operation, es);
		if (relationship)
			ExplainPropertyText("Parent Relationship", relationship, es);
		if (plan_name)
			ExplainPropertyText("Subplan Name", plan_name, es);
		if (custom_name)
			ExplainPropertyText("Custom Plan Provider", custom_name, es);
		ExplainPropertyBool("Parallel Aware", plan->parallel_aware, es);
		ExplainPropertyBool("Async Capable", plan->async_capable, es);
	}

	switch (nodeTag(plan))
	{
		case T_SeqScan:
		case T_SampleScan:
		case T_BitmapHeapScan:
		case T_TidScan:
		case T_TidRangeScan:
		case T_SubqueryScan:
		case T_FunctionScan:
		case T_TableFuncScan:
		case T_ValuesScan:
		case T_CteScan:
		case T_WorkTableScan:
			ExplainScanTarget((Scan *) plan, es);
			break;
		case T_ForeignScan:
		case T_CustomScan:
			if (((Scan *) plan)->scanrelid > 0)
				ExplainScanTarget((Scan *) plan, es);
			break;
		case T_IndexScan:
			{
				IndexScan  *indexscan = (IndexScan *) plan;

				ExplainIndexScanDetails(indexscan->indexid,
										indexscan->indexorderdir,
										es);
				ExplainScanTarget((Scan *) indexscan, es);
			}
			break;
		case T_IndexOnlyScan:
			{
				IndexOnlyScan *indexonlyscan = (IndexOnlyScan *) plan;

				ExplainIndexScanDetails(indexonlyscan->indexid,
										indexonlyscan->indexorderdir,
										es);
				ExplainScanTarget((Scan *) indexonlyscan, es);
			}
			break;
		case T_BitmapIndexScan:
			{
				BitmapIndexScan *bitmapindexscan = (BitmapIndexScan *) plan;
				const char *indexname =
					explain_get_index_name(bitmapindexscan->indexid);

				if (es->format == EXPLAIN_FORMAT_TEXT)
					appendStringInfo(es->str, " on %s",
									 quote_identifier(indexname));
				else
					ExplainPropertyText("Index Name", indexname, es);
			}
			break;
		case T_ModifyTable:
			ExplainModifyTarget((ModifyTable *) plan, es);
			break;
		case T_NestLoop:
		case T_MergeJoin:
		case T_HashJoin:
			{
				const char *jointype;

				switch (((Join *) plan)->jointype)
				{
					case JOIN_INNER:
						jointype = "Inner";
						break;
					case JOIN_LEFT:
						jointype = "Left";
						break;
					case JOIN_FULL:
						jointype = "Full";
						break;
					case JOIN_RIGHT:
						jointype = "Right";
						break;
					case JOIN_SEMI:
						jointype = "Semi";
						break;
					case JOIN_ANTI:
						jointype = "Anti";
						break;
					case JOIN_RIGHT_SEMI:
						jointype = "Right Semi";
						break;
					case JOIN_RIGHT_ANTI:
						jointype = "Right Anti";
						break;
					default:
						jointype = "???";
						break;
				}
				if (es->format == EXPLAIN_FORMAT_TEXT)
				{
					/*
					 * For historical reasons, the join type is interpolated
					 * into the node type name...
					 *
					 * 由于历史原因，连接类型被插入节点类型名中……
					 */
					if (((Join *) plan)->jointype != JOIN_INNER)
						appendStringInfo(es->str, " %s Join", jointype);
					else if (!IsA(plan, NestLoop))
						appendStringInfoString(es->str, " Join");
				}
				else
					ExplainPropertyText("Join Type", jointype, es);
			}
			break;
		case T_SetOp:
			{
				const char *setopcmd;

				switch (((SetOp *) plan)->cmd)
				{
					case SETOPCMD_INTERSECT:
						setopcmd = "Intersect";
						break;
					case SETOPCMD_INTERSECT_ALL:
						setopcmd = "Intersect All";
						break;
					case SETOPCMD_EXCEPT:
						setopcmd = "Except";
						break;
					case SETOPCMD_EXCEPT_ALL:
						setopcmd = "Except All";
						break;
					default:
						setopcmd = "???";
						break;
				}
				if (es->format == EXPLAIN_FORMAT_TEXT)
					appendStringInfo(es->str, " %s", setopcmd);
				else
					ExplainPropertyText("Command", setopcmd, es);
			}
			break;
		default:
			break;
	}

	if (es->costs)
	{
		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			appendStringInfo(es->str, "  (cost=%.2f..%.2f rows=%.0f width=%d)",
							 plan->startup_cost, plan->total_cost,
							 plan->plan_rows, plan->plan_width);
		}
		else
		{
			ExplainPropertyFloat("Startup Cost", NULL, plan->startup_cost,
								 2, es);
			ExplainPropertyFloat("Total Cost", NULL, plan->total_cost,
								 2, es);
			ExplainPropertyFloat("Plan Rows", NULL, plan->plan_rows,
								 0, es);
			ExplainPropertyInteger("Plan Width", NULL, plan->plan_width,
								   es);
		}
	}

	/*
	 * We have to forcibly clean up the instrumentation state because we
	 * haven't done ExecutorEnd yet.  This is pretty grotty ...
	 *
	 * 必须强制清理插桩状态，因为还没有做 ExecutorEnd。这相当粗糙……
	 *
	 * Note: contrib/auto_explain could cause instrumentation to be set up
	 * even though we didn't ask for it here.  Be careful not to print any
	 * instrumentation results the user didn't ask for.  But we do the
	 * InstrEndLoop call anyway, if possible, to reduce the number of cases
	 * auto_explain has to contend with.
	 *
	 * 注意：contrib/auto_explain 可能在我们这里并未请求的情况下也装上插桩。小
	 * 心不要打印用户没有要求的插桩结果。但只要可能，仍然调用 InstrEndLoop，以
	 * 减少 auto_explain 要应对的情况。
	 */
	if (planstate->instrument)
		InstrEndLoop(planstate->instrument);

	if (es->analyze &&
		planstate->instrument && planstate->instrument->nloops > 0)
	{
		double		nloops = planstate->instrument->nloops;
		double		startup_ms = 1000.0 * planstate->instrument->startup / nloops;
		double		total_ms = 1000.0 * planstate->instrument->total / nloops;
		double		rows = planstate->instrument->ntuples / nloops;

		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			appendStringInfoString(es->str, " (actual ");

			if (es->timing)
				appendStringInfo(es->str, "time=%.3f..%.3f ", startup_ms, total_ms);

			appendStringInfo(es->str, "rows=%.2f loops=%.0f)", rows, nloops);
		}
		else
		{
			if (es->timing)
			{
				ExplainPropertyFloat("Actual Startup Time", "ms", startup_ms,
									 3, es);
				ExplainPropertyFloat("Actual Total Time", "ms", total_ms,
									 3, es);
			}
			ExplainPropertyFloat("Actual Rows", NULL, rows, 2, es);
			ExplainPropertyFloat("Actual Loops", NULL, nloops, 0, es);
		}
	}
	else if (es->analyze)
	{
		if (es->format == EXPLAIN_FORMAT_TEXT)
			appendStringInfoString(es->str, " (never executed)");
		else
		{
			if (es->timing)
			{
				ExplainPropertyFloat("Actual Startup Time", "ms", 0.0, 3, es);
				ExplainPropertyFloat("Actual Total Time", "ms", 0.0, 3, es);
			}
			ExplainPropertyFloat("Actual Rows", NULL, 0.0, 0, es);
			ExplainPropertyFloat("Actual Loops", NULL, 0.0, 0, es);
		}
	}

	/* in text format, first line ends here */
	/*
	 *
	 * 文本格式下，第一行到此结束。
	 */
	if (es->format == EXPLAIN_FORMAT_TEXT)
		appendStringInfoChar(es->str, '\n');


	isdisabled = plan_is_disabled(plan);
	if (es->format != EXPLAIN_FORMAT_TEXT || isdisabled)
		ExplainPropertyBool("Disabled", isdisabled, es);

	/* prepare per-worker general execution details */
	/*
	 *
	 * 准备逐 worker 的通用执行细节。
	 */
	if (es->workers_state && es->verbose)
	{
		WorkerInstrumentation *w = planstate->worker_instrument;

		for (int n = 0; n < w->num_workers; n++)
		{
			Instrumentation *instrument = &w->instrument[n];
			double		nloops = instrument->nloops;
			double		startup_ms;
			double		total_ms;
			double		rows;

			if (nloops <= 0)
				continue;
			startup_ms = 1000.0 * instrument->startup / nloops;
			total_ms = 1000.0 * instrument->total / nloops;
			rows = instrument->ntuples / nloops;

			ExplainOpenWorker(n, es);

			if (es->format == EXPLAIN_FORMAT_TEXT)
			{
				ExplainIndentText(es);
				appendStringInfoString(es->str, "actual ");
				if (es->timing)
					appendStringInfo(es->str, "time=%.3f..%.3f ", startup_ms, total_ms);

				appendStringInfo(es->str, "rows=%.2f loops=%.0f\n", rows, nloops);
			}
			else
			{
				if (es->timing)
				{
					ExplainPropertyFloat("Actual Startup Time", "ms",
										 startup_ms, 3, es);
					ExplainPropertyFloat("Actual Total Time", "ms",
										 total_ms, 3, es);
				}

				ExplainPropertyFloat("Actual Rows", NULL, rows, 2, es);
				ExplainPropertyFloat("Actual Loops", NULL, nloops, 0, es);
			}

			ExplainCloseWorker(n, es);
		}
	}

	/* target list */
	/*
	 *
	 * 目标列表。
	 */
	if (es->verbose)
		show_plan_tlist(planstate, ancestors, es);

	/* unique join */
	/*
	 *
	 * 唯一连接。
	 */
	switch (nodeTag(plan))
	{
		case T_NestLoop:
		case T_MergeJoin:
		case T_HashJoin:
			/* try not to be too chatty about this in text mode */
			/*
			 *
			 * 文本模式下尽量不要对此过于啰嗦。
			 */
			if (es->format != EXPLAIN_FORMAT_TEXT ||
				(es->verbose && ((Join *) plan)->inner_unique))
				ExplainPropertyBool("Inner Unique",
									((Join *) plan)->inner_unique,
									es);
			break;
		default:
			break;
	}

	/* quals, sort keys, etc */
	/*
	 *
	 * 限定条件、排序键等。
	 */
	switch (nodeTag(plan))
	{
		case T_IndexScan:
			show_scan_qual(((IndexScan *) plan)->indexqualorig,
						   "Index Cond", planstate, ancestors, es);
			if (((IndexScan *) plan)->indexqualorig)
				show_instrumentation_count("Rows Removed by Index Recheck", 2,
										   planstate, es);
			show_scan_qual(((IndexScan *) plan)->indexorderbyorig,
						   "Order By", planstate, ancestors, es);
			show_scan_qual(plan->qual, "Filter", planstate, ancestors, es);
			if (plan->qual)
				show_instrumentation_count("Rows Removed by Filter", 1,
										   planstate, es);
			show_indexsearches_info(planstate, es);
			break;
		case T_IndexOnlyScan:
			show_scan_qual(((IndexOnlyScan *) plan)->indexqual,
						   "Index Cond", planstate, ancestors, es);
			if (((IndexOnlyScan *) plan)->recheckqual)
				show_instrumentation_count("Rows Removed by Index Recheck", 2,
										   planstate, es);
			show_scan_qual(((IndexOnlyScan *) plan)->indexorderby,
						   "Order By", planstate, ancestors, es);
			show_scan_qual(plan->qual, "Filter", planstate, ancestors, es);
			if (plan->qual)
				show_instrumentation_count("Rows Removed by Filter", 1,
										   planstate, es);
			if (es->analyze)
				ExplainPropertyFloat("Heap Fetches", NULL,
									 planstate->instrument->ntuples2, 0, es);
			show_indexsearches_info(planstate, es);
			break;
		case T_BitmapIndexScan:
			show_scan_qual(((BitmapIndexScan *) plan)->indexqualorig,
						   "Index Cond", planstate, ancestors, es);
			show_indexsearches_info(planstate, es);
			break;
		case T_BitmapHeapScan:
			show_scan_qual(((BitmapHeapScan *) plan)->bitmapqualorig,
						   "Recheck Cond", planstate, ancestors, es);
			if (((BitmapHeapScan *) plan)->bitmapqualorig)
				show_instrumentation_count("Rows Removed by Index Recheck", 2,
										   planstate, es);
			show_scan_qual(plan->qual, "Filter", planstate, ancestors, es);
			if (plan->qual)
				show_instrumentation_count("Rows Removed by Filter", 1,
										   planstate, es);
			show_tidbitmap_info((BitmapHeapScanState *) planstate, es);
			break;
		case T_SampleScan:
			show_tablesample(((SampleScan *) plan)->tablesample,
							 planstate, ancestors, es);
			/* fall through to print additional fields the same as SeqScan */
			/*
			 *
			 * 落入下一分支，像 SeqScan 一样打印附加字段。
			 */
			/* FALLTHROUGH */
			/*
			 *
			 * 落入下一 case。
			 */
		case T_SeqScan:
		case T_ValuesScan:
		case T_CteScan:
		case T_NamedTuplestoreScan:
		case T_WorkTableScan:
		case T_SubqueryScan:
			show_scan_qual(plan->qual, "Filter", planstate, ancestors, es);
			if (plan->qual)
				show_instrumentation_count("Rows Removed by Filter", 1,
										   planstate, es);
			if (IsA(plan, CteScan))
				show_ctescan_info(castNode(CteScanState, planstate), es);
			break;
		case T_Gather:
			{
				Gather	   *gather = (Gather *) plan;

				show_scan_qual(plan->qual, "Filter", planstate, ancestors, es);
				if (plan->qual)
					show_instrumentation_count("Rows Removed by Filter", 1,
											   planstate, es);
				ExplainPropertyInteger("Workers Planned", NULL,
									   gather->num_workers, es);

				if (es->analyze)
				{
					int			nworkers;

					nworkers = ((GatherState *) planstate)->nworkers_launched;
					ExplainPropertyInteger("Workers Launched", NULL,
										   nworkers, es);
				}

				if (gather->single_copy || es->format != EXPLAIN_FORMAT_TEXT)
					ExplainPropertyBool("Single Copy", gather->single_copy, es);
			}
			break;
		case T_GatherMerge:
			{
				GatherMerge *gm = (GatherMerge *) plan;

				show_scan_qual(plan->qual, "Filter", planstate, ancestors, es);
				if (plan->qual)
					show_instrumentation_count("Rows Removed by Filter", 1,
											   planstate, es);
				ExplainPropertyInteger("Workers Planned", NULL,
									   gm->num_workers, es);

				if (es->analyze)
				{
					int			nworkers;

					nworkers = ((GatherMergeState *) planstate)->nworkers_launched;
					ExplainPropertyInteger("Workers Launched", NULL,
										   nworkers, es);
				}
			}
			break;
		case T_FunctionScan:
			if (es->verbose)
			{
				List	   *fexprs = NIL;
				ListCell   *lc;

				foreach(lc, ((FunctionScan *) plan)->functions)
				{
					RangeTblFunction *rtfunc = (RangeTblFunction *) lfirst(lc);

					fexprs = lappend(fexprs, rtfunc->funcexpr);
				}
				/* We rely on show_expression to insert commas as needed */
				/*
				 *
				 * 依赖 show_expression 在需要时插入逗号。
				 */
				show_expression((Node *) fexprs,
								"Function Call", planstate, ancestors,
								es->verbose, es);
			}
			show_scan_qual(plan->qual, "Filter", planstate, ancestors, es);
			if (plan->qual)
				show_instrumentation_count("Rows Removed by Filter", 1,
										   planstate, es);
			break;
		case T_TableFuncScan:
			if (es->verbose)
			{
				TableFunc  *tablefunc = ((TableFuncScan *) plan)->tablefunc;

				show_expression((Node *) tablefunc,
								"Table Function Call", planstate, ancestors,
								es->verbose, es);
			}
			show_scan_qual(plan->qual, "Filter", planstate, ancestors, es);
			if (plan->qual)
				show_instrumentation_count("Rows Removed by Filter", 1,
										   planstate, es);
			show_table_func_scan_info(castNode(TableFuncScanState,
											   planstate), es);
			break;
		case T_TidScan:
			{
				/*
				 * The tidquals list has OR semantics, so be sure to show it
				 * as an OR condition.
				 *
				 * tidquals 列表具有 OR 语义，因此务必显示为 OR 条件。
				 */
				List	   *tidquals = ((TidScan *) plan)->tidquals;

				if (list_length(tidquals) > 1)
					tidquals = list_make1(make_orclause(tidquals));
				show_scan_qual(tidquals, "TID Cond", planstate, ancestors, es);
				show_scan_qual(plan->qual, "Filter", planstate, ancestors, es);
				if (plan->qual)
					show_instrumentation_count("Rows Removed by Filter", 1,
											   planstate, es);
			}
			break;
		case T_TidRangeScan:
			{
				/*
				 * The tidrangequals list has AND semantics, so be sure to
				 * show it as an AND condition.
				 *
				 * tidrangequals 列表具有 AND 语义，因此务必显示为 AND 条件。
				 */
				List	   *tidquals = ((TidRangeScan *) plan)->tidrangequals;

				if (list_length(tidquals) > 1)
					tidquals = list_make1(make_andclause(tidquals));
				show_scan_qual(tidquals, "TID Cond", planstate, ancestors, es);
				show_scan_qual(plan->qual, "Filter", planstate, ancestors, es);
				if (plan->qual)
					show_instrumentation_count("Rows Removed by Filter", 1,
											   planstate, es);
			}
			break;
		case T_ForeignScan:
			show_scan_qual(plan->qual, "Filter", planstate, ancestors, es);
			if (plan->qual)
				show_instrumentation_count("Rows Removed by Filter", 1,
										   planstate, es);
			show_foreignscan_info((ForeignScanState *) planstate, es);
			break;
		case T_CustomScan:
			{
				CustomScanState *css = (CustomScanState *) planstate;

				show_scan_qual(plan->qual, "Filter", planstate, ancestors, es);
				if (plan->qual)
					show_instrumentation_count("Rows Removed by Filter", 1,
											   planstate, es);
				if (css->methods->ExplainCustomScan)
					css->methods->ExplainCustomScan(css, ancestors, es);
			}
			break;
		case T_NestLoop:
			show_upper_qual(((NestLoop *) plan)->join.joinqual,
							"Join Filter", planstate, ancestors, es);
			if (((NestLoop *) plan)->join.joinqual)
				show_instrumentation_count("Rows Removed by Join Filter", 1,
										   planstate, es);
			show_upper_qual(plan->qual, "Filter", planstate, ancestors, es);
			if (plan->qual)
				show_instrumentation_count("Rows Removed by Filter", 2,
										   planstate, es);
			break;
		case T_MergeJoin:
			show_upper_qual(((MergeJoin *) plan)->mergeclauses,
							"Merge Cond", planstate, ancestors, es);
			show_upper_qual(((MergeJoin *) plan)->join.joinqual,
							"Join Filter", planstate, ancestors, es);
			if (((MergeJoin *) plan)->join.joinqual)
				show_instrumentation_count("Rows Removed by Join Filter", 1,
										   planstate, es);
			show_upper_qual(plan->qual, "Filter", planstate, ancestors, es);
			if (plan->qual)
				show_instrumentation_count("Rows Removed by Filter", 2,
										   planstate, es);
			break;
		case T_HashJoin:
			show_upper_qual(((HashJoin *) plan)->hashclauses,
							"Hash Cond", planstate, ancestors, es);
			show_upper_qual(((HashJoin *) plan)->join.joinqual,
							"Join Filter", planstate, ancestors, es);
			if (((HashJoin *) plan)->join.joinqual)
				show_instrumentation_count("Rows Removed by Join Filter", 1,
										   planstate, es);
			show_upper_qual(plan->qual, "Filter", planstate, ancestors, es);
			if (plan->qual)
				show_instrumentation_count("Rows Removed by Filter", 2,
										   planstate, es);
			break;
		case T_Agg:
			show_agg_keys(castNode(AggState, planstate), ancestors, es);
			show_upper_qual(plan->qual, "Filter", planstate, ancestors, es);
			show_hashagg_info((AggState *) planstate, es);
			if (plan->qual)
				show_instrumentation_count("Rows Removed by Filter", 1,
										   planstate, es);
			break;
		case T_WindowAgg:
			show_window_def(castNode(WindowAggState, planstate), ancestors, es);
			show_upper_qual(((WindowAgg *) plan)->runConditionOrig,
							"Run Condition", planstate, ancestors, es);
			show_upper_qual(plan->qual, "Filter", planstate, ancestors, es);
			if (plan->qual)
				show_instrumentation_count("Rows Removed by Filter", 1,
										   planstate, es);
			show_windowagg_info(castNode(WindowAggState, planstate), es);
			break;
		case T_Group:
			show_group_keys(castNode(GroupState, planstate), ancestors, es);
			show_upper_qual(plan->qual, "Filter", planstate, ancestors, es);
			if (plan->qual)
				show_instrumentation_count("Rows Removed by Filter", 1,
										   planstate, es);
			break;
		case T_Sort:
			show_sort_keys(castNode(SortState, planstate), ancestors, es);
			show_sort_info(castNode(SortState, planstate), es);
			break;
		case T_IncrementalSort:
			show_incremental_sort_keys(castNode(IncrementalSortState, planstate),
									   ancestors, es);
			show_incremental_sort_info(castNode(IncrementalSortState, planstate),
									   es);
			break;
		case T_MergeAppend:
			show_merge_append_keys(castNode(MergeAppendState, planstate),
								   ancestors, es);
			break;
		case T_Result:
			show_upper_qual((List *) ((Result *) plan)->resconstantqual,
							"One-Time Filter", planstate, ancestors, es);
			show_upper_qual(plan->qual, "Filter", planstate, ancestors, es);
			if (plan->qual)
				show_instrumentation_count("Rows Removed by Filter", 1,
										   planstate, es);
			break;
		case T_ModifyTable:
			show_modifytable_info(castNode(ModifyTableState, planstate), ancestors,
								  es);
			break;
		case T_Hash:
			show_hash_info(castNode(HashState, planstate), es);
			break;
		case T_Material:
			show_material_info(castNode(MaterialState, planstate), es);
			break;
		case T_Memoize:
			show_memoize_info(castNode(MemoizeState, planstate), ancestors,
							  es);
			break;
		case T_RecursiveUnion:
			show_recursive_union_info(castNode(RecursiveUnionState,
											   planstate), es);
			break;
		default:
			break;
	}

	/*
	 * Prepare per-worker JIT instrumentation.  As with the overall JIT
	 * summary, this is printed only if printing costs is enabled.
	 *
	 * 准备逐 worker 的 JIT 插桩。与总体 JIT 摘要一样，只有在启用代价打印时才
	 * 打印它。
	 */
	if (es->workers_state && es->costs && es->verbose)
	{
		SharedJitInstrumentation *w = planstate->worker_jit_instrument;

		if (w)
		{
			for (int n = 0; n < w->num_workers; n++)
			{
				ExplainOpenWorker(n, es);
				ExplainPrintJIT(es, planstate->state->es_jit_flags,
								&w->jit_instr[n]);
				ExplainCloseWorker(n, es);
			}
		}
	}

	/* Show buffer/WAL usage */
	/*
	 *
	 * 显示缓冲区与 WAL 用量。
	 */
	if (es->buffers && planstate->instrument)
		show_buffer_usage(es, &planstate->instrument->bufusage);
	if (es->wal && planstate->instrument)
		show_wal_usage(es, &planstate->instrument->walusage);

	/* Prepare per-worker buffer/WAL usage */
	/*
	 *
	 * 准备逐 worker 的缓冲区与 WAL 用量。
	 */
	if (es->workers_state && (es->buffers || es->wal) && es->verbose)
	{
		WorkerInstrumentation *w = planstate->worker_instrument;

		for (int n = 0; n < w->num_workers; n++)
		{
			Instrumentation *instrument = &w->instrument[n];
			double		nloops = instrument->nloops;

			if (nloops <= 0)
				continue;

			ExplainOpenWorker(n, es);
			if (es->buffers)
				show_buffer_usage(es, &instrument->bufusage);
			if (es->wal)
				show_wal_usage(es, &instrument->walusage);
			ExplainCloseWorker(n, es);
		}
	}

	/* Show per-worker details for this plan node, then pop that stack */
	/*
	 *
	 * 显示该计划节点的逐 worker 细节，然后弹出该栈。
	 */
	if (es->workers_state)
		ExplainFlushWorkersState(es);
	es->workers_state = save_workers_state;

	/* Allow plugins to print additional information */
	/*
	 *
	 * 允许插件打印附加信息。
	 */
	if (explain_per_node_hook)
		(*explain_per_node_hook) (planstate, ancestors, relationship,
								  plan_name, es);

	/*
	 * If partition pruning was done during executor initialization, the
	 * number of child plans we'll display below will be less than the number
	 * of subplans that was specified in the plan.  To make this a bit less
	 * mysterious, emit an indication that this happened.  Note that this
	 * field is emitted now because we want it to be a property of the parent
	 * node; it *cannot* be emitted within the Plans sub-node we'll open next.
	 *
	 * 若执行器初始化期间做了分区剪枝，下面要显示的子计划数会少于计划中指定的
	 * 子计划数。为了减少困惑，发出一条说明。注意该字段现在就输出，因为它应是
	 * 父节点的属性；不能放到接下来要打开的 Plans 子节点里面。
	 */
	switch (nodeTag(plan))
	{
		case T_Append:
			ExplainMissingMembers(((AppendState *) planstate)->as_nplans,
								  list_length(((Append *) plan)->appendplans),
								  es);
			break;
		case T_MergeAppend:
			ExplainMissingMembers(((MergeAppendState *) planstate)->ms_nplans,
								  list_length(((MergeAppend *) plan)->mergeplans),
								  es);
			break;
		default:
			break;
	}

	/* Get ready to display the child plans */
	/*
	 *
	 * 准备显示子计划。
	 */
	haschildren = planstate->initPlan ||
		outerPlanState(planstate) ||
		innerPlanState(planstate) ||
		IsA(plan, Append) ||
		IsA(plan, MergeAppend) ||
		IsA(plan, BitmapAnd) ||
		IsA(plan, BitmapOr) ||
		IsA(plan, SubqueryScan) ||
		(IsA(planstate, CustomScanState) &&
		 ((CustomScanState *) planstate)->custom_ps != NIL) ||
		planstate->subPlan;
	if (haschildren)
	{
		ExplainOpenGroup("Plans", "Plans", false, es);
		/* Pass current Plan as head of ancestors list for children */
		/*
		 *
		 * 把当前 Plan 作为子节点 ancestors 列表的表头传入。
		 */
		ancestors = lcons(plan, ancestors);
	}

	/* initPlan-s */
	/*
	 *
	 * 初始化计划 initPlan。
	 */
	if (planstate->initPlan)
		ExplainSubPlans(planstate->initPlan, ancestors, "InitPlan", es);

	/* lefttree */
	/*
	 *
	 * 左子树。
	 */
	if (outerPlanState(planstate))
		ExplainNode(outerPlanState(planstate), ancestors,
					"Outer", NULL, es);

	/* righttree */
	/*
	 *
	 * 右子树。
	 */
	if (innerPlanState(planstate))
		ExplainNode(innerPlanState(planstate), ancestors,
					"Inner", NULL, es);

	/* special child plans */
	/*
	 *
	 * 特殊子计划。
	 */
	switch (nodeTag(plan))
	{
		case T_Append:
			ExplainMemberNodes(((AppendState *) planstate)->appendplans,
							   ((AppendState *) planstate)->as_nplans,
							   ancestors, es);
			break;
		case T_MergeAppend:
			ExplainMemberNodes(((MergeAppendState *) planstate)->mergeplans,
							   ((MergeAppendState *) planstate)->ms_nplans,
							   ancestors, es);
			break;
		case T_BitmapAnd:
			ExplainMemberNodes(((BitmapAndState *) planstate)->bitmapplans,
							   ((BitmapAndState *) planstate)->nplans,
							   ancestors, es);
			break;
		case T_BitmapOr:
			ExplainMemberNodes(((BitmapOrState *) planstate)->bitmapplans,
							   ((BitmapOrState *) planstate)->nplans,
							   ancestors, es);
			break;
		case T_SubqueryScan:
			ExplainNode(((SubqueryScanState *) planstate)->subplan, ancestors,
						"Subquery", NULL, es);
			break;
		case T_CustomScan:
			ExplainCustomChildren((CustomScanState *) planstate,
								  ancestors, es);
			break;
		default:
			break;
	}

	/* subPlan-s */
	/*
	 *
	 * 子计划 subPlan。
	 */
	if (planstate->subPlan)
		ExplainSubPlans(planstate->subPlan, ancestors, "SubPlan", es);

	/* end of child plans */
	/*
	 *
	 * 子计划结束。
	 */
	if (haschildren)
	{
		ancestors = list_delete_first(ancestors);
		ExplainCloseGroup("Plans", "Plans", false, es);
	}

	/* in text format, undo whatever indentation we added */
	/*
	 *
	 * 文本格式下，撤销我们加上的缩进。
	 */
	if (es->format == EXPLAIN_FORMAT_TEXT)
		es->indent = save_indent;

	ExplainCloseGroup("Plan",
					  relationship ? NULL : "Plan",
					  true, es);
}

/*
 * Show the targetlist of a plan node
 *
 * 显示计划节点的目标列表。
 */
static void
show_plan_tlist(PlanState *planstate, List *ancestors, ExplainState *es)
{
	Plan	   *plan = planstate->plan;
	List	   *context;
	List	   *result = NIL;
	bool		useprefix;
	ListCell   *lc;

	/* No work if empty tlist (this occurs eg in bitmap indexscans) */
	/*
	 *
	 * 目标列表为空则无事可做（例如位图索引扫描会出现这种情况）。
	 */
	if (plan->targetlist == NIL)
		return;
	/* The tlist of an Append isn't real helpful, so suppress it */
	/*
	 *
	 * Append 的目标列表没什么帮助，因此抑制它。
	 */
	if (IsA(plan, Append))
		return;
	/* Likewise for MergeAppend and RecursiveUnion */
	/*
	 *
	 * MergeAppend 和 RecursiveUnion 同样处理。
	 */
	if (IsA(plan, MergeAppend))
		return;
	if (IsA(plan, RecursiveUnion))
		return;

	/*
	 * Likewise for ForeignScan that executes a direct INSERT/UPDATE/DELETE
	 *
	 * 直接执行 INSERT/UPDATE/DELETE 的 ForeignScan 同样处理。
	 *
	 * Note: the tlist for a ForeignScan that executes a direct INSERT/UPDATE
	 * might contain subplan output expressions that are confusing in this
	 * context.  The tlist for a ForeignScan that executes a direct UPDATE/
	 * DELETE always contains "junk" target columns to identify the exact row
	 * to update or delete, which would be confusing in this context.  So, we
	 * suppress it in all the cases.
	 *
	 * 注意：直接执行 INSERT/UPDATE 的 ForeignScan，其目标列表可能含有在此上下
	 * 文中令人困惑的子计划输出表达式。直接执行 UPDATE/DELETE 的 ForeignScan，
	 * 其目标列表总含有用于精确标识要更新或删除的行的 junk 目标列，在此上下文
	 * 中也会令人困惑。因此这些情况全部抑制。
	 */
	if (IsA(plan, ForeignScan) &&
		((ForeignScan *) plan)->operation != CMD_SELECT)
		return;

	/* Set up deparsing context */
	/*
	 *
	 * 设置反解析上下文。
	 */
	context = set_deparse_context_plan(es->deparse_cxt,
									   plan,
									   ancestors);
	useprefix = es->rtable_size > 1;

	/* Deparse each result column (we now include resjunk ones) */
	/*
	 *
	 * 反解析每个结果列（现在包含 resjunk 列）。
	 */
	foreach(lc, plan->targetlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);

		result = lappend(result,
						 deparse_expression((Node *) tle->expr, context,
											useprefix, false));
	}

	/* Print results */
	/*
	 *
	 * 打印结果。
	 */
	ExplainPropertyList("Output", result, es);
}

/*
 * Show a generic expression
 *
 * 显示一个通用表达式。
 */
static void
show_expression(Node *node, const char *qlabel,
				PlanState *planstate, List *ancestors,
				bool useprefix, ExplainState *es)
{
	List	   *context;
	char	   *exprstr;

	/* Set up deparsing context */
	/*
	 *
	 * 设置反解析上下文。
	 */
	context = set_deparse_context_plan(es->deparse_cxt,
									   planstate->plan,
									   ancestors);

	/* Deparse the expression */
	/*
	 *
	 * 反解析该表达式。
	 */
	exprstr = deparse_expression(node, context, useprefix, false);

	/* And add to es->str */
	/*
	 *
	 * 并追加到 es->str。
	 */
	ExplainPropertyText(qlabel, exprstr, es);
}

/*
 * Show a qualifier expression (which is a List with implicit AND semantics)
 *
 * 显示限定表达式（它是具有隐式 AND 语义的 List）。
 */
static void
show_qual(List *qual, const char *qlabel,
		  PlanState *planstate, List *ancestors,
		  bool useprefix, ExplainState *es)
{
	Node	   *node;

	/* No work if empty qual */
	/*
	 *
	 * 限定条件为空则无事可做。
	 */
	if (qual == NIL)
		return;

	/* Convert AND list to explicit AND */
	/*
	 *
	 * 把 AND 列表转换成显式 AND。
	 */
	node = (Node *) make_ands_explicit(qual);

	/* And show it */
	/*
	 *
	 * 并显示它。
	 */
	show_expression(node, qlabel, planstate, ancestors, useprefix, es);
}

/*
 * Show a qualifier expression for a scan plan node
 *
 * 显示扫描计划节点的限定表达式。
 */
static void
show_scan_qual(List *qual, const char *qlabel,
			   PlanState *planstate, List *ancestors,
			   ExplainState *es)
{
	bool		useprefix;

	useprefix = (IsA(planstate->plan, SubqueryScan) || es->verbose);
	show_qual(qual, qlabel, planstate, ancestors, useprefix, es);
}

/*
 * Show a qualifier expression for an upper-level plan node
 *
 * 显示上层计划节点的限定表达式。
 */
static void
show_upper_qual(List *qual, const char *qlabel,
				PlanState *planstate, List *ancestors,
				ExplainState *es)
{
	bool		useprefix;

	useprefix = (es->rtable_size > 1 || es->verbose);
	show_qual(qual, qlabel, planstate, ancestors, useprefix, es);
}

/*
 * Show the sort keys for a Sort node.
 *
 * 显示 Sort 节点的排序键。
 */
static void
show_sort_keys(SortState *sortstate, List *ancestors, ExplainState *es)
{
	Sort	   *plan = (Sort *) sortstate->ss.ps.plan;

	show_sort_group_keys((PlanState *) sortstate, "Sort Key",
						 plan->numCols, 0, plan->sortColIdx,
						 plan->sortOperators, plan->collations,
						 plan->nullsFirst,
						 ancestors, es);
}

/*
 * Show the sort keys for an IncrementalSort node.
 *
 * 显示 IncrementalSort 节点的排序键。
 */
static void
show_incremental_sort_keys(IncrementalSortState *incrsortstate,
						   List *ancestors, ExplainState *es)
{
	IncrementalSort *plan = (IncrementalSort *) incrsortstate->ss.ps.plan;

	show_sort_group_keys((PlanState *) incrsortstate, "Sort Key",
						 plan->sort.numCols, plan->nPresortedCols,
						 plan->sort.sortColIdx,
						 plan->sort.sortOperators, plan->sort.collations,
						 plan->sort.nullsFirst,
						 ancestors, es);
}

/*
 * Likewise, for a MergeAppend node.
 *
 * MergeAppend 节点同样处理。
 */
static void
show_merge_append_keys(MergeAppendState *mstate, List *ancestors,
					   ExplainState *es)
{
	MergeAppend *plan = (MergeAppend *) mstate->ps.plan;

	show_sort_group_keys((PlanState *) mstate, "Sort Key",
						 plan->numCols, 0, plan->sortColIdx,
						 plan->sortOperators, plan->collations,
						 plan->nullsFirst,
						 ancestors, es);
}

/*
 * Show the grouping keys for an Agg node.
 *
 * 显示 Agg 节点的分组键。
 */
static void
show_agg_keys(AggState *astate, List *ancestors,
			  ExplainState *es)
{
	Agg		   *plan = (Agg *) astate->ss.ps.plan;

	if (plan->numCols > 0 || plan->groupingSets)
	{
		/* The key columns refer to the tlist of the child plan */
		/*
		 *
		 * 键列引用子计划的目标列表。
		 */
		ancestors = lcons(plan, ancestors);

		if (plan->groupingSets)
			show_grouping_sets(outerPlanState(astate), plan, ancestors, es);
		else
			show_sort_group_keys(outerPlanState(astate), "Group Key",
								 plan->numCols, 0, plan->grpColIdx,
								 NULL, NULL, NULL,
								 ancestors, es);

		ancestors = list_delete_first(ancestors);
	}
}

/*
 * 打印 Agg 节点的 grouping sets，包括链上每个集合的分组键。
 */

static void
show_grouping_sets(PlanState *planstate, Agg *agg,
				   List *ancestors, ExplainState *es)
{
	List	   *context;
	bool		useprefix;
	ListCell   *lc;

	/* Set up deparsing context */
	/*
	 *
	 * 设置反解析上下文。
	 */
	context = set_deparse_context_plan(es->deparse_cxt,
									   planstate->plan,
									   ancestors);
	useprefix = (es->rtable_size > 1 || es->verbose);

	ExplainOpenGroup("Grouping Sets", "Grouping Sets", false, es);

	show_grouping_set_keys(planstate, agg, NULL,
						   context, useprefix, ancestors, es);

	foreach(lc, agg->chain)
	{
		Agg		   *aggnode = lfirst(lc);
		Sort	   *sortnode = (Sort *) aggnode->plan.lefttree;

		show_grouping_set_keys(planstate, aggnode, sortnode,
							   context, useprefix, ancestors, es);
	}

	ExplainCloseGroup("Grouping Sets", "Grouping Sets", false, es);
}

/*
 * 打印单个 grouping set 的分组键；若带排序节点，同时打印排序键。
 */

static void
show_grouping_set_keys(PlanState *planstate,
					   Agg *aggnode, Sort *sortnode,
					   List *context, bool useprefix,
					   List *ancestors, ExplainState *es)
{
	Plan	   *plan = planstate->plan;
	char	   *exprstr;
	ListCell   *lc;
	List	   *gsets = aggnode->groupingSets;
	AttrNumber *keycols = aggnode->grpColIdx;
	const char *keyname;
	const char *keysetname;

	if (aggnode->aggstrategy == AGG_HASHED || aggnode->aggstrategy == AGG_MIXED)
	{
		keyname = "Hash Key";
		keysetname = "Hash Keys";
	}
	else
	{
		keyname = "Group Key";
		keysetname = "Group Keys";
	}

	ExplainOpenGroup("Grouping Set", NULL, true, es);

	if (sortnode)
	{
		show_sort_group_keys(planstate, "Sort Key",
							 sortnode->numCols, 0, sortnode->sortColIdx,
							 sortnode->sortOperators, sortnode->collations,
							 sortnode->nullsFirst,
							 ancestors, es);
		if (es->format == EXPLAIN_FORMAT_TEXT)
			es->indent++;
	}

	ExplainOpenGroup(keysetname, keysetname, false, es);

	foreach(lc, gsets)
	{
		List	   *result = NIL;
		ListCell   *lc2;

		foreach(lc2, (List *) lfirst(lc))
		{
			Index		i = lfirst_int(lc2);
			AttrNumber	keyresno = keycols[i];
			TargetEntry *target = get_tle_by_resno(plan->targetlist,
												   keyresno);

			if (!target)
				elog(ERROR, "no tlist entry for key %d", keyresno);
			/* Deparse the expression, showing any top-level cast */
			/*
			 *
			 * 反解析表达式，并显示任何顶层强制转换。
			 */
			exprstr = deparse_expression((Node *) target->expr, context,
										 useprefix, true);

			result = lappend(result, exprstr);
		}

		if (!result && es->format == EXPLAIN_FORMAT_TEXT)
			ExplainPropertyText(keyname, "()", es);
		else
			ExplainPropertyListNested(keyname, result, es);
	}

	ExplainCloseGroup(keysetname, keysetname, false, es);

	if (sortnode && es->format == EXPLAIN_FORMAT_TEXT)
		es->indent--;

	ExplainCloseGroup("Grouping Set", NULL, true, es);
}

/*
 * Show the grouping keys for a Group node.
 *
 * 显示 Group 节点的分组键。
 */
static void
show_group_keys(GroupState *gstate, List *ancestors,
				ExplainState *es)
{
	Group	   *plan = (Group *) gstate->ss.ps.plan;

	/* The key columns refer to the tlist of the child plan */
	/*
	 *
	 * 键列引用子计划的目标列表。
	 */
	ancestors = lcons(plan, ancestors);
	show_sort_group_keys(outerPlanState(gstate), "Group Key",
						 plan->numCols, 0, plan->grpColIdx,
						 NULL, NULL, NULL,
						 ancestors, es);
	ancestors = list_delete_first(ancestors);
}

/*
 * Common code to show sort/group keys, which are represented in plan nodes
 * as arrays of targetlist indexes.  If it's a sort key rather than a group
 * key, also pass sort operators/collations/nullsFirst arrays.
 *
 * 显示排序键或分组键的公共代码。计划节点把它们表示为目标列表下标的数组。若是
 * 排序键而不是分组键，还要传入排序操作符、排序规则和 nullsFirst 数组。
 */
static void
show_sort_group_keys(PlanState *planstate, const char *qlabel,
					 int nkeys, int nPresortedKeys, AttrNumber *keycols,
					 Oid *sortOperators, Oid *collations, bool *nullsFirst,
					 List *ancestors, ExplainState *es)
{
	Plan	   *plan = planstate->plan;
	List	   *context;
	List	   *result = NIL;
	List	   *resultPresorted = NIL;
	StringInfoData sortkeybuf;
	bool		useprefix;
	int			keyno;

	if (nkeys <= 0)
		return;

	initStringInfo(&sortkeybuf);

	/* Set up deparsing context */
	/*
	 *
	 * 设置反解析上下文。
	 */
	context = set_deparse_context_plan(es->deparse_cxt,
									   plan,
									   ancestors);
	useprefix = (es->rtable_size > 1 || es->verbose);

	for (keyno = 0; keyno < nkeys; keyno++)
	{
		/* find key expression in tlist */
		/*
		 *
		 * 在目标列表中查找键表达式。
		 */
		AttrNumber	keyresno = keycols[keyno];
		TargetEntry *target = get_tle_by_resno(plan->targetlist,
											   keyresno);
		char	   *exprstr;

		if (!target)
			elog(ERROR, "no tlist entry for key %d", keyresno);
		/* Deparse the expression, showing any top-level cast */
		/*
		 *
		 * 反解析表达式，并显示任何顶层强制转换。
		 */
		exprstr = deparse_expression((Node *) target->expr, context,
									 useprefix, true);
		resetStringInfo(&sortkeybuf);
		appendStringInfoString(&sortkeybuf, exprstr);
		/* Append sort order information, if relevant */
		/*
		 *
		 * 若相关，则追加排序次序信息。
		 */
		if (sortOperators != NULL)
			show_sortorder_options(&sortkeybuf,
								   (Node *) target->expr,
								   sortOperators[keyno],
								   collations[keyno],
								   nullsFirst[keyno]);
		/* Emit one property-list item per sort key */
		/*
		 *
		 * 每个排序键输出一个属性列表项。
		 */
		result = lappend(result, pstrdup(sortkeybuf.data));
		if (keyno < nPresortedKeys)
			resultPresorted = lappend(resultPresorted, exprstr);
	}

	ExplainPropertyList(qlabel, result, es);
	if (nPresortedKeys > 0)
		ExplainPropertyList("Presorted Key", resultPresorted, es);
}

/*
 * Append nondefault characteristics of the sort ordering of a column to buf
 * (collation, direction, NULLS FIRST/LAST)
 *
 * 把一列排序次序中的非默认特征追加到 buf（排序规则、方向、NULLS FIRST/LAST）。
 */
static void
show_sortorder_options(StringInfo buf, Node *sortexpr,
					   Oid sortOperator, Oid collation, bool nullsFirst)
{
	Oid			sortcoltype = exprType(sortexpr);
	bool		reverse = false;
	TypeCacheEntry *typentry;

	typentry = lookup_type_cache(sortcoltype,
								 TYPECACHE_LT_OPR | TYPECACHE_GT_OPR);

	/*
	 * Print COLLATE if it's not default for the column's type.  There are
	 * some cases where this is redundant, eg if expression is a column whose
	 * declared collation is that collation, but it's hard to distinguish that
	 * here (and arguably, printing COLLATE explicitly is a good idea anyway
	 * in such cases).
	 *
	 * 若 COLLATE 不是该列类型的默认值则打印它。有些情况下这是多余的，例如表达
	 * 式就是声明了该排序规则的列，但这里很难区分（而且即便如此，显式打印
	 * COLLATE 也说得通）。
	 */
	if (OidIsValid(collation) && collation != get_typcollation(sortcoltype))
	{
		char	   *collname = get_collation_name(collation);

		if (collname == NULL)
			elog(ERROR, "cache lookup failed for collation %u", collation);
		appendStringInfo(buf, " COLLATE %s", quote_identifier(collname));
	}

	/* Print direction if not ASC, or USING if non-default sort operator */
	/*
	 *
	 * 若方向不是 ASC 则打印方向；若排序操作符不是默认值则打印 USING。
	 */
	if (sortOperator == typentry->gt_opr)
	{
		appendStringInfoString(buf, " DESC");
		reverse = true;
	}
	else if (sortOperator != typentry->lt_opr)
	{
		char	   *opname = get_opname(sortOperator);

		if (opname == NULL)
			elog(ERROR, "cache lookup failed for operator %u", sortOperator);
		appendStringInfo(buf, " USING %s", opname);
		/* Determine whether operator would be considered ASC or DESC */
		/*
		 *
		 * 判断该操作符会被视为 ASC 还是 DESC。
		 */
		(void) get_equality_op_for_ordering_op(sortOperator, &reverse);
	}

	/* Add NULLS FIRST/LAST only if it wouldn't be default */
	/*
	 *
	 * 只有在不是默认值时才加上 NULLS FIRST/LAST。
	 */
	if (nullsFirst && !reverse)
	{
		appendStringInfoString(buf, " NULLS FIRST");
	}
	else if (!nullsFirst && reverse)
	{
		appendStringInfoString(buf, " NULLS LAST");
	}
}

/*
 * Show the window definition for a WindowAgg node.
 *
 * 显示 WindowAgg 节点的窗口定义。
 */
static void
show_window_def(WindowAggState *planstate, List *ancestors, ExplainState *es)
{
	WindowAgg  *wagg = (WindowAgg *) planstate->ss.ps.plan;
	StringInfoData wbuf;
	bool		needspace = false;

	initStringInfo(&wbuf);
	appendStringInfo(&wbuf, "%s AS (", quote_identifier(wagg->winname));

	/* The key columns refer to the tlist of the child plan */
	/*
	 *
	 * 键列引用子计划的目标列表。
	 */
	ancestors = lcons(wagg, ancestors);
	if (wagg->partNumCols > 0)
	{
		appendStringInfoString(&wbuf, "PARTITION BY ");
		show_window_keys(&wbuf, outerPlanState(planstate),
						 wagg->partNumCols, wagg->partColIdx,
						 ancestors, es);
		needspace = true;
	}
	if (wagg->ordNumCols > 0)
	{
		if (needspace)
			appendStringInfoChar(&wbuf, ' ');
		appendStringInfoString(&wbuf, "ORDER BY ");
		show_window_keys(&wbuf, outerPlanState(planstate),
						 wagg->ordNumCols, wagg->ordColIdx,
						 ancestors, es);
		needspace = true;
	}
	ancestors = list_delete_first(ancestors);
	if (wagg->frameOptions & FRAMEOPTION_NONDEFAULT)
	{
		List	   *context;
		bool		useprefix;
		char	   *framestr;

		/* Set up deparsing context for possible frame expressions */
		/*
		 *
		 * 为可能的帧表达式设置反解析上下文。
		 */
		context = set_deparse_context_plan(es->deparse_cxt,
										   (Plan *) wagg,
										   ancestors);
		useprefix = (es->rtable_size > 1 || es->verbose);
		framestr = get_window_frame_options_for_explain(wagg->frameOptions,
														wagg->startOffset,
														wagg->endOffset,
														context,
														useprefix);
		if (needspace)
			appendStringInfoChar(&wbuf, ' ');
		appendStringInfoString(&wbuf, framestr);
		pfree(framestr);
	}
	appendStringInfoChar(&wbuf, ')');
	ExplainPropertyText("Window", wbuf.data, es);
	pfree(wbuf.data);
}

/*
 * Append the keys of a window's PARTITION BY or ORDER BY clause to buf.
 * We can't use show_sort_group_keys for this because that's too opinionated
 * about how the result will be displayed.
 * Note that the "planstate" node should be the WindowAgg's child.
 *
 * 把窗口的 PARTITION BY 或 ORDER BY 子句的键追加到 buf。不能用
 * show_sort_group_keys，因为它对结果如何显示的假设太强。注意这里的 planstate
 * 节点应是 WindowAgg 的子节点。
 */
static void
show_window_keys(StringInfo buf, PlanState *planstate,
				 int nkeys, AttrNumber *keycols,
				 List *ancestors, ExplainState *es)
{
	Plan	   *plan = planstate->plan;
	List	   *context;
	bool		useprefix;

	/* Set up deparsing context */
	/*
	 *
	 * 设置反解析上下文。
	 */
	context = set_deparse_context_plan(es->deparse_cxt,
									   plan,
									   ancestors);
	useprefix = (es->rtable_size > 1 || es->verbose);

	for (int keyno = 0; keyno < nkeys; keyno++)
	{
		/* find key expression in tlist */
		/*
		 *
		 * 在目标列表中查找键表达式。
		 */
		AttrNumber	keyresno = keycols[keyno];
		TargetEntry *target = get_tle_by_resno(plan->targetlist,
											   keyresno);
		char	   *exprstr;

		if (!target)
			elog(ERROR, "no tlist entry for key %d", keyresno);
		/* Deparse the expression, showing any top-level cast */
		/*
		 *
		 * 反解析表达式，并显示任何顶层强制转换。
		 */
		exprstr = deparse_expression((Node *) target->expr, context,
									 useprefix, true);
		if (keyno > 0)
			appendStringInfoString(buf, ", ");
		appendStringInfoString(buf, exprstr);
		pfree(exprstr);

		/*
		 * We don't attempt to provide sort order information because
		 * WindowAgg carries equality operators not comparison operators;
		 * compare show_agg_keys.
		 *
		 * 我们不尝试提供排序次序信息，因为 WindowAgg 携带的是相等操作符而不是
		 * 比较操作符；对照 show_agg_keys。
		 */
	}
}

/*
 * Show information on storage method and maximum memory/disk space used.
 *
 * 显示存储方法以及使用的最大内存或磁盘空间。
 */
static void
show_storage_info(char *maxStorageType, int64 maxSpaceUsed, ExplainState *es)
{
	int64		maxSpaceUsedKB = BYTES_TO_KILOBYTES(maxSpaceUsed);

	if (es->format != EXPLAIN_FORMAT_TEXT)
	{
		ExplainPropertyText("Storage", maxStorageType, es);
		ExplainPropertyInteger("Maximum Storage", "kB", maxSpaceUsedKB, es);
	}
	else
	{
		ExplainIndentText(es);
		appendStringInfo(es->str,
						 "Storage: %s  Maximum Storage: " INT64_FORMAT "kB\n",
						 maxStorageType,
						 maxSpaceUsedKB);
	}
}

/*
 * Show TABLESAMPLE properties
 *
 * 显示 TABLESAMPLE 属性。
 */
static void
show_tablesample(TableSampleClause *tsc, PlanState *planstate,
				 List *ancestors, ExplainState *es)
{
	List	   *context;
	bool		useprefix;
	char	   *method_name;
	List	   *params = NIL;
	char	   *repeatable;
	ListCell   *lc;

	/* Set up deparsing context */
	/*
	 *
	 * 设置反解析上下文。
	 */
	context = set_deparse_context_plan(es->deparse_cxt,
									   planstate->plan,
									   ancestors);
	useprefix = es->rtable_size > 1;

	/* Get the tablesample method name */
	/*
	 *
	 * 取得 tablesample 方法名。
	 */
	method_name = get_func_name(tsc->tsmhandler);

	/* Deparse parameter expressions */
	/*
	 *
	 * 反解析参数表达式。
	 */
	foreach(lc, tsc->args)
	{
		Node	   *arg = (Node *) lfirst(lc);

		params = lappend(params,
						 deparse_expression(arg, context,
											useprefix, false));
	}
	if (tsc->repeatable)
		repeatable = deparse_expression((Node *) tsc->repeatable, context,
										useprefix, false);
	else
		repeatable = NULL;

	/* Print results */
	/*
	 *
	 * 打印结果。
	 */
	if (es->format == EXPLAIN_FORMAT_TEXT)
	{
		bool		first = true;

		ExplainIndentText(es);
		appendStringInfo(es->str, "Sampling: %s (", method_name);
		foreach(lc, params)
		{
			if (!first)
				appendStringInfoString(es->str, ", ");
			appendStringInfoString(es->str, (const char *) lfirst(lc));
			first = false;
		}
		appendStringInfoChar(es->str, ')');
		if (repeatable)
			appendStringInfo(es->str, " REPEATABLE (%s)", repeatable);
		appendStringInfoChar(es->str, '\n');
	}
	else
	{
		ExplainPropertyText("Sampling Method", method_name, es);
		ExplainPropertyList("Sampling Parameters", params, es);
		if (repeatable)
			ExplainPropertyText("Repeatable Seed", repeatable, es);
	}
}

/*
 * If it's EXPLAIN ANALYZE, show tuplesort stats for a sort node
 *
 * 若是 EXPLAIN ANALYZE，则显示排序节点的 tuplesort 统计。
 */
static void
show_sort_info(SortState *sortstate, ExplainState *es)
{
	if (!es->analyze)
		return;

	if (sortstate->sort_Done && sortstate->tuplesortstate != NULL)
	{
		Tuplesortstate *state = (Tuplesortstate *) sortstate->tuplesortstate;
		TuplesortInstrumentation stats;
		const char *sortMethod;
		const char *spaceType;
		int64		spaceUsed;

		tuplesort_get_stats(state, &stats);
		sortMethod = tuplesort_method_name(stats.sortMethod);
		spaceType = tuplesort_space_type_name(stats.spaceType);
		spaceUsed = stats.spaceUsed;

		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			ExplainIndentText(es);
			appendStringInfo(es->str, "Sort Method: %s  %s: " INT64_FORMAT "kB\n",
							 sortMethod, spaceType, spaceUsed);
		}
		else
		{
			ExplainPropertyText("Sort Method", sortMethod, es);
			ExplainPropertyInteger("Sort Space Used", "kB", spaceUsed, es);
			ExplainPropertyText("Sort Space Type", spaceType, es);
		}
	}

	/*
	 * You might think we should just skip this stanza entirely when
	 * es->hide_workers is true, but then we'd get no sort-method output at
	 * all.  We have to make it look like worker 0's data is top-level data.
	 * This is easily done by just skipping the OpenWorker/CloseWorker calls.
	 * Currently, we don't worry about the possibility that there are multiple
	 * workers in such a case; if there are, duplicate output fields will be
	 * emitted.
	 *
	 * 也许会以为 es->hide_workers 为真时应当整段跳过，但那样就完全没有排序方
	 * 法输出了。必须让 worker 0 的数据看起来像顶层数据。只要跳过 OpenWorker/
	 * CloseWorker 调用即可。目前不担心这种情况下有多个 worker；若有，会输出重
	 * 复字段。
	 */
	if (sortstate->shared_info != NULL)
	{
		int			n;

		for (n = 0; n < sortstate->shared_info->num_workers; n++)
		{
			TuplesortInstrumentation *sinstrument;
			const char *sortMethod;
			const char *spaceType;
			int64		spaceUsed;

			sinstrument = &sortstate->shared_info->sinstrument[n];
			if (sinstrument->sortMethod == SORT_TYPE_STILL_IN_PROGRESS)
				continue;		/* ignore any unfilled slots */
				/*
				 *
				 * 忽略任何未填充的槽位。
				 */
			sortMethod = tuplesort_method_name(sinstrument->sortMethod);
			spaceType = tuplesort_space_type_name(sinstrument->spaceType);
			spaceUsed = sinstrument->spaceUsed;

			if (es->workers_state)
				ExplainOpenWorker(n, es);

			if (es->format == EXPLAIN_FORMAT_TEXT)
			{
				ExplainIndentText(es);
				appendStringInfo(es->str,
								 "Sort Method: %s  %s: " INT64_FORMAT "kB\n",
								 sortMethod, spaceType, spaceUsed);
			}
			else
			{
				ExplainPropertyText("Sort Method", sortMethod, es);
				ExplainPropertyInteger("Sort Space Used", "kB", spaceUsed, es);
				ExplainPropertyText("Sort Space Type", spaceType, es);
			}

			if (es->workers_state)
				ExplainCloseWorker(n, es);
		}
	}
}

/*
 * Incremental sort nodes sort in (a potentially very large number of) batches,
 * so EXPLAIN ANALYZE needs to roll up the tuplesort stats from each batch into
 * an intelligible summary.
 *
 * 增量排序节点按（可能非常多的）批次排序，因此 EXPLAIN ANALYZE 需要把每个批次
 * 的 tuplesort 统计汇总成可理解的摘要。
 *
 * This function is used for both a non-parallel node and each worker in a
 * parallel incremental sort node.
 *
 * 本函数既用于非并行节点，也用于并行增量排序节点中的每个 worker。
 */
static void
show_incremental_sort_group_info(IncrementalSortGroupInfo *groupInfo,
								 const char *groupLabel, bool indent, ExplainState *es)
{
	ListCell   *methodCell;
	List	   *methodNames = NIL;

	/* Generate a list of sort methods used across all groups. */
	/*
	 *
	 * 生成所有组使用过的排序方法列表。
	 */
	for (int bit = 0; bit < NUM_TUPLESORTMETHODS; bit++)
	{
		TuplesortMethod sortMethod = (1 << bit);

		if (groupInfo->sortMethods & sortMethod)
		{
			const char *methodName = tuplesort_method_name(sortMethod);

			methodNames = lappend(methodNames, unconstify(char *, methodName));
		}
	}

	if (es->format == EXPLAIN_FORMAT_TEXT)
	{
		if (indent)
			appendStringInfoSpaces(es->str, es->indent * 2);
		appendStringInfo(es->str, "%s Groups: " INT64_FORMAT "  Sort Method", groupLabel,
						 groupInfo->groupCount);
		/* plural/singular based on methodNames size */
		/*
		 *
		 * 按 methodNames 的大小选择复数或单数。
		 */
		if (list_length(methodNames) > 1)
			appendStringInfoString(es->str, "s: ");
		else
			appendStringInfoString(es->str, ": ");
		foreach(methodCell, methodNames)
		{
			appendStringInfoString(es->str, (char *) methodCell->ptr_value);
			if (foreach_current_index(methodCell) < list_length(methodNames) - 1)
				appendStringInfoString(es->str, ", ");
		}

		if (groupInfo->maxMemorySpaceUsed > 0)
		{
			int64		avgSpace = groupInfo->totalMemorySpaceUsed / groupInfo->groupCount;
			const char *spaceTypeName;

			spaceTypeName = tuplesort_space_type_name(SORT_SPACE_TYPE_MEMORY);
			appendStringInfo(es->str, "  Average %s: " INT64_FORMAT "kB  Peak %s: " INT64_FORMAT "kB",
							 spaceTypeName, avgSpace,
							 spaceTypeName, groupInfo->maxMemorySpaceUsed);
		}

		if (groupInfo->maxDiskSpaceUsed > 0)
		{
			int64		avgSpace = groupInfo->totalDiskSpaceUsed / groupInfo->groupCount;

			const char *spaceTypeName;

			spaceTypeName = tuplesort_space_type_name(SORT_SPACE_TYPE_DISK);
			appendStringInfo(es->str, "  Average %s: " INT64_FORMAT "kB  Peak %s: " INT64_FORMAT "kB",
							 spaceTypeName, avgSpace,
							 spaceTypeName, groupInfo->maxDiskSpaceUsed);
		}
	}
	else
	{
		StringInfoData groupName;

		initStringInfo(&groupName);
		appendStringInfo(&groupName, "%s Groups", groupLabel);
		ExplainOpenGroup("Incremental Sort Groups", groupName.data, true, es);
		ExplainPropertyInteger("Group Count", NULL, groupInfo->groupCount, es);

		ExplainPropertyList("Sort Methods Used", methodNames, es);

		if (groupInfo->maxMemorySpaceUsed > 0)
		{
			int64		avgSpace = groupInfo->totalMemorySpaceUsed / groupInfo->groupCount;
			const char *spaceTypeName;
			StringInfoData memoryName;

			spaceTypeName = tuplesort_space_type_name(SORT_SPACE_TYPE_MEMORY);
			initStringInfo(&memoryName);
			appendStringInfo(&memoryName, "Sort Space %s", spaceTypeName);
			ExplainOpenGroup("Sort Space", memoryName.data, true, es);

			ExplainPropertyInteger("Average Sort Space Used", "kB", avgSpace, es);
			ExplainPropertyInteger("Peak Sort Space Used", "kB",
								   groupInfo->maxMemorySpaceUsed, es);

			ExplainCloseGroup("Sort Space", memoryName.data, true, es);
		}
		if (groupInfo->maxDiskSpaceUsed > 0)
		{
			int64		avgSpace = groupInfo->totalDiskSpaceUsed / groupInfo->groupCount;
			const char *spaceTypeName;
			StringInfoData diskName;

			spaceTypeName = tuplesort_space_type_name(SORT_SPACE_TYPE_DISK);
			initStringInfo(&diskName);
			appendStringInfo(&diskName, "Sort Space %s", spaceTypeName);
			ExplainOpenGroup("Sort Space", diskName.data, true, es);

			ExplainPropertyInteger("Average Sort Space Used", "kB", avgSpace, es);
			ExplainPropertyInteger("Peak Sort Space Used", "kB",
								   groupInfo->maxDiskSpaceUsed, es);

			ExplainCloseGroup("Sort Space", diskName.data, true, es);
		}

		ExplainCloseGroup("Incremental Sort Groups", groupName.data, true, es);
	}
}

/*
 * If it's EXPLAIN ANALYZE, show tuplesort stats for an incremental sort node
 *
 * 若是 EXPLAIN ANALYZE，则显示增量排序节点的 tuplesort 统计。
 */
static void
show_incremental_sort_info(IncrementalSortState *incrsortstate,
						   ExplainState *es)
{
	IncrementalSortGroupInfo *fullsortGroupInfo;
	IncrementalSortGroupInfo *prefixsortGroupInfo;

	fullsortGroupInfo = &incrsortstate->incsort_info.fullsortGroupInfo;

	if (!es->analyze)
		return;

	/*
	 * Since we never have any prefix groups unless we've first sorted a full
	 * groups and transitioned modes (copying the tuples into a prefix group),
	 * we don't need to do anything if there were 0 full groups.
	 *
	 * 除非先排序过一个完整组并切换了模式（把元组复制进前缀组），否则不会有前
	 * 缀组，因此若完整组数为 0 就不必做任何事。
	 *
	 * We still have to continue after this block if there are no full groups,
	 * though, since it's possible that we have workers that did real work
	 * even if the leader didn't participate.
	 *
	 * 即使没有完整组，这个块之后仍必须继续，因为即使 leader 没有参与，worker
	 * 也可能做了实际工作。
	 */
	if (fullsortGroupInfo->groupCount > 0)
	{
		show_incremental_sort_group_info(fullsortGroupInfo, "Full-sort", true, es);
		prefixsortGroupInfo = &incrsortstate->incsort_info.prefixsortGroupInfo;
		if (prefixsortGroupInfo->groupCount > 0)
		{
			if (es->format == EXPLAIN_FORMAT_TEXT)
				appendStringInfoChar(es->str, '\n');
			show_incremental_sort_group_info(prefixsortGroupInfo, "Pre-sorted", true, es);
		}
		if (es->format == EXPLAIN_FORMAT_TEXT)
			appendStringInfoChar(es->str, '\n');
	}

	if (incrsortstate->shared_info != NULL)
	{
		int			n;
		bool		indent_first_line;

		for (n = 0; n < incrsortstate->shared_info->num_workers; n++)
		{
			IncrementalSortInfo *incsort_info =
				&incrsortstate->shared_info->sinfo[n];

			/*
			 * If a worker hasn't processed any sort groups at all, then
			 * exclude it from output since it either didn't launch or didn't
			 * contribute anything meaningful.
			 *
			 * 若某个 worker 完全没有处理过排序组，则把它排除在输出之外，因为
			 * 它要么没有启动，要么没有贡献任何有意义的结果。
			 */
			fullsortGroupInfo = &incsort_info->fullsortGroupInfo;

			/*
			 * Since we never have any prefix groups unless we've first sorted
			 * a full groups and transitioned modes (copying the tuples into a
			 * prefix group), we don't need to do anything if there were 0
			 * full groups.
			 *
			 * 除非先排序过一个完整组并切换了模式（把元组复制进前缀组），否则
			 * 不会有前缀组，因此若完整组数为 0 就不必做任何事。
			 */
			if (fullsortGroupInfo->groupCount == 0)
				continue;

			if (es->workers_state)
				ExplainOpenWorker(n, es);

			indent_first_line = es->workers_state == NULL || es->verbose;
			show_incremental_sort_group_info(fullsortGroupInfo, "Full-sort",
											 indent_first_line, es);
			prefixsortGroupInfo = &incsort_info->prefixsortGroupInfo;
			if (prefixsortGroupInfo->groupCount > 0)
			{
				if (es->format == EXPLAIN_FORMAT_TEXT)
					appendStringInfoChar(es->str, '\n');
				show_incremental_sort_group_info(prefixsortGroupInfo, "Pre-sorted", true, es);
			}
			if (es->format == EXPLAIN_FORMAT_TEXT)
				appendStringInfoChar(es->str, '\n');

			if (es->workers_state)
				ExplainCloseWorker(n, es);
		}
	}
}

/*
 * Show information on hash buckets/batches.
 *
 * 显示哈希桶与批次的信息。
 */
static void
show_hash_info(HashState *hashstate, ExplainState *es)
{
	HashInstrumentation hinstrument = {0};

	/*
	 * Collect stats from the local process, even when it's a parallel query.
	 * In a parallel query, the leader process may or may not have run the
	 * hash join, and even if it did it may not have built a hash table due to
	 * timing (if it started late it might have seen no tuples in the outer
	 * relation and skipped building the hash table).  Therefore we have to be
	 * prepared to get instrumentation data from all participants.
	 *
	 * 即使是并行查询，也收集本地进程的统计。并行查询中 leader 进程可能运行过
	 * 哈希连接，也可能没有；即使运行了，也可能因时序没有建哈希表（若启动较晚，
	 * 可能看到外表没有元组而跳过建表）。因此必须准备好从所有参与者取得插桩数
	 * 据。
	 */
	if (hashstate->hinstrument)
		memcpy(&hinstrument, hashstate->hinstrument,
			   sizeof(HashInstrumentation));

	/*
	 * Merge results from workers.  In the parallel-oblivious case, the
	 * results from all participants should be identical, except where
	 * participants didn't run the join at all so have no data.  In the
	 * parallel-aware case, we need to consider all the results.  Each worker
	 * may have seen a different subset of batches and we want to report the
	 * highest memory usage across all batches.  We take the maxima of other
	 * values too, for the same reasons as in ExecHashAccumInstrumentation.
	 *
	 * 合并 worker 的结果。在 parallel-oblivious 情况下，所有参与者的结果应当
	 * 相同，除非参与者根本没运行连接因而没有数据。在 parallel-aware 情况下必
	 * 须考虑全部结果。每个 worker 可能看到不同的批次子集，我们要报告所有批次
	 * 中的最高内存用量。其他值也取最大值，理由与 ExecHashAccumInstrumentation
	 * 相同。
	 */
	if (hashstate->shared_info)
	{
		SharedHashInfo *shared_info = hashstate->shared_info;
		int			i;

		for (i = 0; i < shared_info->num_workers; ++i)
		{
			HashInstrumentation *worker_hi = &shared_info->hinstrument[i];

			hinstrument.nbuckets = Max(hinstrument.nbuckets,
									   worker_hi->nbuckets);
			hinstrument.nbuckets_original = Max(hinstrument.nbuckets_original,
												worker_hi->nbuckets_original);
			hinstrument.nbatch = Max(hinstrument.nbatch,
									 worker_hi->nbatch);
			hinstrument.nbatch_original = Max(hinstrument.nbatch_original,
											  worker_hi->nbatch_original);
			hinstrument.space_peak = Max(hinstrument.space_peak,
										 worker_hi->space_peak);
		}
	}

	if (hinstrument.nbatch > 0)
	{
		uint64		spacePeakKb = BYTES_TO_KILOBYTES(hinstrument.space_peak);

		if (es->format != EXPLAIN_FORMAT_TEXT)
		{
			ExplainPropertyInteger("Hash Buckets", NULL,
								   hinstrument.nbuckets, es);
			ExplainPropertyInteger("Original Hash Buckets", NULL,
								   hinstrument.nbuckets_original, es);
			ExplainPropertyInteger("Hash Batches", NULL,
								   hinstrument.nbatch, es);
			ExplainPropertyInteger("Original Hash Batches", NULL,
								   hinstrument.nbatch_original, es);
			ExplainPropertyUInteger("Peak Memory Usage", "kB",
									spacePeakKb, es);
		}
		else if (hinstrument.nbatch_original != hinstrument.nbatch ||
				 hinstrument.nbuckets_original != hinstrument.nbuckets)
		{
			ExplainIndentText(es);
			appendStringInfo(es->str,
							 "Buckets: %d (originally %d)  Batches: %d (originally %d)  Memory Usage: " UINT64_FORMAT "kB\n",
							 hinstrument.nbuckets,
							 hinstrument.nbuckets_original,
							 hinstrument.nbatch,
							 hinstrument.nbatch_original,
							 spacePeakKb);
		}
		else
		{
			ExplainIndentText(es);
			appendStringInfo(es->str,
							 "Buckets: %d  Batches: %d  Memory Usage: " UINT64_FORMAT "kB\n",
							 hinstrument.nbuckets, hinstrument.nbatch,
							 spacePeakKb);
		}
	}
}

/*
 * Show information on material node, storage method and maximum memory/disk
 * space used.
 *
 * 显示物化节点的信息、存储方法以及使用的最大内存或磁盘空间。
 */
static void
show_material_info(MaterialState *mstate, ExplainState *es)
{
	char	   *maxStorageType;
	int64		maxSpaceUsed;

	Tuplestorestate *tupstore = mstate->tuplestorestate;

	/*
	 * Nothing to show if ANALYZE option wasn't used or if execution didn't
	 * get as far as creating the tuplestore.
	 *
	 * 若未使用 ANALYZE 选项，或执行还没走到创建 tuplestore，则无内容可显示。
	 */
	if (!es->analyze || tupstore == NULL)
		return;

	tuplestore_get_stats(tupstore, &maxStorageType, &maxSpaceUsed);
	show_storage_info(maxStorageType, maxSpaceUsed, es);
}

/*
 * Show information on WindowAgg node, storage method and maximum memory/disk
 * space used.
 *
 * 显示 WindowAgg 节点的信息、存储方法以及使用的最大内存或磁盘空间。
 */
static void
show_windowagg_info(WindowAggState *winstate, ExplainState *es)
{
	char	   *maxStorageType;
	int64		maxSpaceUsed;

	Tuplestorestate *tupstore = winstate->buffer;

	/*
	 * Nothing to show if ANALYZE option wasn't used or if execution didn't
	 * get as far as creating the tuplestore.
	 *
	 * 若未使用 ANALYZE 选项，或执行还没走到创建 tuplestore，则无内容可显示。
	 */
	if (!es->analyze || tupstore == NULL)
		return;

	tuplestore_get_stats(tupstore, &maxStorageType, &maxSpaceUsed);
	show_storage_info(maxStorageType, maxSpaceUsed, es);
}

/*
 * Show information on CTE Scan node, storage method and maximum memory/disk
 * space used.
 *
 * 显示 CTE Scan 节点的信息、存储方法以及使用的最大内存或磁盘空间。
 */
static void
show_ctescan_info(CteScanState *ctescanstate, ExplainState *es)
{
	char	   *maxStorageType;
	int64		maxSpaceUsed;

	Tuplestorestate *tupstore = ctescanstate->leader->cte_table;

	if (!es->analyze || tupstore == NULL)
		return;

	tuplestore_get_stats(tupstore, &maxStorageType, &maxSpaceUsed);
	show_storage_info(maxStorageType, maxSpaceUsed, es);
}

/*
 * Show information on Table Function Scan node, storage method and maximum
 * memory/disk space used.
 *
 * 显示表函数扫描节点的信息、存储方法以及使用的最大内存或磁盘空间。
 */
static void
show_table_func_scan_info(TableFuncScanState *tscanstate, ExplainState *es)
{
	char	   *maxStorageType;
	int64		maxSpaceUsed;

	Tuplestorestate *tupstore = tscanstate->tupstore;

	if (!es->analyze || tupstore == NULL)
		return;

	tuplestore_get_stats(tupstore, &maxStorageType, &maxSpaceUsed);
	show_storage_info(maxStorageType, maxSpaceUsed, es);
}

/*
 * Show information on Recursive Union node, storage method and maximum
 * memory/disk space used.
 *
 * 显示 Recursive Union 节点的信息、存储方法以及使用的最大内存或磁盘空间。
 */
static void
show_recursive_union_info(RecursiveUnionState *rstate, ExplainState *es)
{
	char	   *maxStorageType,
			   *tempStorageType;
	int64		maxSpaceUsed,
				tempSpaceUsed;

	if (!es->analyze)
		return;

	/*
	 * Recursive union node uses two tuplestores.  We employ the storage type
	 * from one of them which consumed more memory/disk than the other.  The
	 * storage size is sum of the two.
	 *
	 * 递归并集节点使用两个 tuplestore。我们采用其中消耗内存或磁盘更多的那个的
	 * 存储类型。存储大小是两者之和。
	 */
	tuplestore_get_stats(rstate->working_table, &tempStorageType,
						 &tempSpaceUsed);
	tuplestore_get_stats(rstate->intermediate_table, &maxStorageType,
						 &maxSpaceUsed);

	if (tempSpaceUsed > maxSpaceUsed)
		maxStorageType = tempStorageType;

	maxSpaceUsed += tempSpaceUsed;
	show_storage_info(maxStorageType, maxSpaceUsed, es);
}

/*
 * Show information on memoize hits/misses/evictions and memory usage.
 *
 * 显示 memoize 的命中、未命中、驱逐以及内存用量。
 */
static void
show_memoize_info(MemoizeState *mstate, List *ancestors, ExplainState *es)
{
	Plan	   *plan = ((PlanState *) mstate)->plan;
	ListCell   *lc;
	List	   *context;
	StringInfoData keystr;
	char	   *separator = "";
	bool		useprefix;
	int64		memPeakKb;

	initStringInfo(&keystr);

	/*
	 * It's hard to imagine having a memoize node with fewer than 2 RTEs, but
	 * let's just keep the same useprefix logic as elsewhere in this file.
	 *
	 * 很难想象 memoize 节点的 RTE 少于 2 个，但仍与本文件其他地方保持同样的
	 * useprefix 逻辑。
	 */
	useprefix = es->rtable_size > 1 || es->verbose;

	/* Set up deparsing context */
	/*
	 *
	 * 设置反解析上下文。
	 */
	context = set_deparse_context_plan(es->deparse_cxt,
									   plan,
									   ancestors);

	foreach(lc, ((Memoize *) plan)->param_exprs)
	{
		Node	   *expr = (Node *) lfirst(lc);

		appendStringInfoString(&keystr, separator);

		appendStringInfoString(&keystr, deparse_expression(expr, context,
														   useprefix, false));
		separator = ", ";
	}

	ExplainPropertyText("Cache Key", keystr.data, es);
	ExplainPropertyText("Cache Mode", mstate->binary_mode ? "binary" : "logical", es);

	pfree(keystr.data);

	if (!es->analyze)
		return;

	if (mstate->stats.cache_misses > 0)
	{
		/*
		 * mem_peak is only set when we freed memory, so we must use mem_used
		 * when mem_peak is 0.
		 *
		 * 只有在释放内存时才会设置 mem_peak，因此 mem_peak 为 0 时必须使用
		 * mem_used。
		 */
		if (mstate->stats.mem_peak > 0)
			memPeakKb = BYTES_TO_KILOBYTES(mstate->stats.mem_peak);
		else
			memPeakKb = BYTES_TO_KILOBYTES(mstate->mem_used);

		if (es->format != EXPLAIN_FORMAT_TEXT)
		{
			ExplainPropertyInteger("Cache Hits", NULL, mstate->stats.cache_hits, es);
			ExplainPropertyInteger("Cache Misses", NULL, mstate->stats.cache_misses, es);
			ExplainPropertyInteger("Cache Evictions", NULL, mstate->stats.cache_evictions, es);
			ExplainPropertyInteger("Cache Overflows", NULL, mstate->stats.cache_overflows, es);
			ExplainPropertyInteger("Peak Memory Usage", "kB", memPeakKb, es);
		}
		else
		{
			ExplainIndentText(es);
			appendStringInfo(es->str,
							 "Hits: " UINT64_FORMAT "  Misses: " UINT64_FORMAT "  Evictions: " UINT64_FORMAT "  Overflows: " UINT64_FORMAT "  Memory Usage: " INT64_FORMAT "kB\n",
							 mstate->stats.cache_hits,
							 mstate->stats.cache_misses,
							 mstate->stats.cache_evictions,
							 mstate->stats.cache_overflows,
							 memPeakKb);
		}
	}

	if (mstate->shared_info == NULL)
		return;

	/* Show details from parallel workers */
	/*
	 *
	 * 显示并行 worker 的细节。
	 */
	for (int n = 0; n < mstate->shared_info->num_workers; n++)
	{
		MemoizeInstrumentation *si;

		si = &mstate->shared_info->sinstrument[n];

		/*
		 * Skip workers that didn't do any work.  We needn't bother checking
		 * for cache hits as a miss will always occur before a cache hit.
		 *
		 * 跳过没有做任何工作的 worker。不必检查缓存命中，因为未命中总是发生在
		 * 命中之前。
		 */
		if (si->cache_misses == 0)
			continue;

		if (es->workers_state)
			ExplainOpenWorker(n, es);

		/*
		 * Since the worker's MemoizeState.mem_used field is unavailable to
		 * us, ExecEndMemoize will have set the
		 * MemoizeInstrumentation.mem_peak field for us.  No need to do the
		 * zero checks like we did for the serial case above.
		 *
		 * worker 的 MemoizeState.mem_used 字段我们拿不到，ExecEndMemoize 会为
		 * 我们设置 MemoizeInstrumentation.mem_peak。不必像上面串行情况那样做
		 * 零值检查。
		 */
		memPeakKb = BYTES_TO_KILOBYTES(si->mem_peak);

		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			ExplainIndentText(es);
			appendStringInfo(es->str,
							 "Hits: " UINT64_FORMAT "  Misses: " UINT64_FORMAT "  Evictions: " UINT64_FORMAT "  Overflows: " UINT64_FORMAT "  Memory Usage: " INT64_FORMAT "kB\n",
							 si->cache_hits, si->cache_misses,
							 si->cache_evictions, si->cache_overflows,
							 memPeakKb);
		}
		else
		{
			ExplainPropertyInteger("Cache Hits", NULL,
								   si->cache_hits, es);
			ExplainPropertyInteger("Cache Misses", NULL,
								   si->cache_misses, es);
			ExplainPropertyInteger("Cache Evictions", NULL,
								   si->cache_evictions, es);
			ExplainPropertyInteger("Cache Overflows", NULL,
								   si->cache_overflows, es);
			ExplainPropertyInteger("Peak Memory Usage", "kB", memPeakKb,
								   es);
		}

		if (es->workers_state)
			ExplainCloseWorker(n, es);
	}
}

/*
 * Show information on hash aggregate memory usage and batches.
 *
 * 显示哈希聚合的内存用量和批次。
 */
static void
show_hashagg_info(AggState *aggstate, ExplainState *es)
{
	Agg		   *agg = (Agg *) aggstate->ss.ps.plan;
	int64		memPeakKb = BYTES_TO_KILOBYTES(aggstate->hash_mem_peak);

	if (agg->aggstrategy != AGG_HASHED &&
		agg->aggstrategy != AGG_MIXED)
		return;

	if (es->format != EXPLAIN_FORMAT_TEXT)
	{
		if (es->costs)
			ExplainPropertyInteger("Planned Partitions", NULL,
								   aggstate->hash_planned_partitions, es);

		/*
		 * During parallel query the leader may have not helped out.  We
		 * detect this by checking how much memory it used.  If we find it
		 * didn't do any work then we don't show its properties.
		 *
		 * 并行查询期间 leader 可能没有参与工作。通过它使用了多少内存来检测。
		 * 若发现它没有做任何工作，就不显示它的属性。
		 */
		if (es->analyze && aggstate->hash_mem_peak > 0)
		{
			ExplainPropertyInteger("HashAgg Batches", NULL,
								   aggstate->hash_batches_used, es);
			ExplainPropertyInteger("Peak Memory Usage", "kB", memPeakKb, es);
			ExplainPropertyInteger("Disk Usage", "kB",
								   aggstate->hash_disk_used, es);
		}
	}
	else
	{
		bool		gotone = false;

		if (es->costs && aggstate->hash_planned_partitions > 0)
		{
			ExplainIndentText(es);
			appendStringInfo(es->str, "Planned Partitions: %d",
							 aggstate->hash_planned_partitions);
			gotone = true;
		}

		/*
		 * During parallel query the leader may have not helped out.  We
		 * detect this by checking how much memory it used.  If we find it
		 * didn't do any work then we don't show its properties.
		 *
		 * 并行查询期间 leader 可能没有参与工作。通过它使用了多少内存来检测。
		 * 若发现它没有做任何工作，就不显示它的属性。
		 */
		if (es->analyze && aggstate->hash_mem_peak > 0)
		{
			if (!gotone)
				ExplainIndentText(es);
			else
				appendStringInfoSpaces(es->str, 2);

			appendStringInfo(es->str, "Batches: %d  Memory Usage: " INT64_FORMAT "kB",
							 aggstate->hash_batches_used, memPeakKb);
			gotone = true;

			/* Only display disk usage if we spilled to disk */
			/*
			 *
			 * 只有溢出到磁盘时才显示磁盘用量。
			 */
			if (aggstate->hash_batches_used > 1)
			{
				appendStringInfo(es->str, "  Disk Usage: " UINT64_FORMAT "kB",
								 aggstate->hash_disk_used);
			}
		}

		if (gotone)
			appendStringInfoChar(es->str, '\n');
	}

	/* Display stats for each parallel worker */
	/*
	 *
	 * 显示每个并行 worker 的统计。
	 */
	if (es->analyze && aggstate->shared_info != NULL)
	{
		for (int n = 0; n < aggstate->shared_info->num_workers; n++)
		{
			AggregateInstrumentation *sinstrument;
			uint64		hash_disk_used;
			int			hash_batches_used;

			sinstrument = &aggstate->shared_info->sinstrument[n];
			/* Skip workers that didn't do anything */
			/*
			 *
			 * 跳过什么都没做的 worker。
			 */
			if (sinstrument->hash_mem_peak == 0)
				continue;
			hash_disk_used = sinstrument->hash_disk_used;
			hash_batches_used = sinstrument->hash_batches_used;
			memPeakKb = BYTES_TO_KILOBYTES(sinstrument->hash_mem_peak);

			if (es->workers_state)
				ExplainOpenWorker(n, es);

			if (es->format == EXPLAIN_FORMAT_TEXT)
			{
				ExplainIndentText(es);

				appendStringInfo(es->str, "Batches: %d  Memory Usage: " INT64_FORMAT "kB",
								 hash_batches_used, memPeakKb);

				/* Only display disk usage if we spilled to disk */
				/*
				 *
				 * 只有溢出到磁盘时才显示磁盘用量。
				 */
				if (hash_batches_used > 1)
					appendStringInfo(es->str, "  Disk Usage: " UINT64_FORMAT "kB",
									 hash_disk_used);
				appendStringInfoChar(es->str, '\n');
			}
			else
			{
				ExplainPropertyInteger("HashAgg Batches", NULL,
									   hash_batches_used, es);
				ExplainPropertyInteger("Peak Memory Usage", "kB", memPeakKb,
									   es);
				ExplainPropertyInteger("Disk Usage", "kB", hash_disk_used, es);
			}

			if (es->workers_state)
				ExplainCloseWorker(n, es);
		}
	}
}

/*
 * Show the total number of index searches for a
 * IndexScan/IndexOnlyScan/BitmapIndexScan node
 *
 * 显示 IndexScan、IndexOnlyScan 或 BitmapIndexScan 节点的索引搜索总次数。
 */
static void
show_indexsearches_info(PlanState *planstate, ExplainState *es)
{
	Plan	   *plan = planstate->plan;
	SharedIndexScanInstrumentation *SharedInfo = NULL;
	uint64		nsearches = 0;

	if (!es->analyze)
		return;

	/* Initialize counters with stats from the local process first */
	/*
	 *
	 * 先用本地进程的统计初始化计数器。
	 */
	switch (nodeTag(plan))
	{
		case T_IndexScan:
			{
				IndexScanState *indexstate = ((IndexScanState *) planstate);

				nsearches = indexstate->iss_Instrument.nsearches;
				SharedInfo = indexstate->iss_SharedInfo;
				break;
			}
		case T_IndexOnlyScan:
			{
				IndexOnlyScanState *indexstate = ((IndexOnlyScanState *) planstate);

				nsearches = indexstate->ioss_Instrument.nsearches;
				SharedInfo = indexstate->ioss_SharedInfo;
				break;
			}
		case T_BitmapIndexScan:
			{
				BitmapIndexScanState *indexstate = ((BitmapIndexScanState *) planstate);

				nsearches = indexstate->biss_Instrument.nsearches;
				SharedInfo = indexstate->biss_SharedInfo;
				break;
			}
		default:
			break;
	}

	/* Next get the sum of the counters set within each and every process */
	/*
	 *
	 * 然后对每个进程中设置的计数器求和。
	 */
	if (SharedInfo)
	{
		for (int i = 0; i < SharedInfo->num_workers; ++i)
		{
			IndexScanInstrumentation *winstrument = &SharedInfo->winstrument[i];

			nsearches += winstrument->nsearches;
		}
	}

	ExplainPropertyUInteger("Index Searches", NULL, nsearches, es);
}

/*
 * Show exact/lossy pages for a BitmapHeapScan node
 *
 * 显示 BitmapHeapScan 节点的精确页与有损页。
 */
static void
show_tidbitmap_info(BitmapHeapScanState *planstate, ExplainState *es)
{
	if (!es->analyze)
		return;

	if (es->format != EXPLAIN_FORMAT_TEXT)
	{
		ExplainPropertyUInteger("Exact Heap Blocks", NULL,
								planstate->stats.exact_pages, es);
		ExplainPropertyUInteger("Lossy Heap Blocks", NULL,
								planstate->stats.lossy_pages, es);
	}
	else
	{
		if (planstate->stats.exact_pages > 0 || planstate->stats.lossy_pages > 0)
		{
			ExplainIndentText(es);
			appendStringInfoString(es->str, "Heap Blocks:");
			if (planstate->stats.exact_pages > 0)
				appendStringInfo(es->str, " exact=" UINT64_FORMAT, planstate->stats.exact_pages);
			if (planstate->stats.lossy_pages > 0)
				appendStringInfo(es->str, " lossy=" UINT64_FORMAT, planstate->stats.lossy_pages);
			appendStringInfoChar(es->str, '\n');
		}
	}

	/* Display stats for each parallel worker */
	/*
	 *
	 * 显示每个并行 worker 的统计。
	 */
	if (planstate->pstate != NULL)
	{
		for (int n = 0; n < planstate->sinstrument->num_workers; n++)
		{
			BitmapHeapScanInstrumentation *si = &planstate->sinstrument->sinstrument[n];

			if (si->exact_pages == 0 && si->lossy_pages == 0)
				continue;

			if (es->workers_state)
				ExplainOpenWorker(n, es);

			if (es->format == EXPLAIN_FORMAT_TEXT)
			{
				ExplainIndentText(es);
				appendStringInfoString(es->str, "Heap Blocks:");
				if (si->exact_pages > 0)
					appendStringInfo(es->str, " exact=" UINT64_FORMAT, si->exact_pages);
				if (si->lossy_pages > 0)
					appendStringInfo(es->str, " lossy=" UINT64_FORMAT, si->lossy_pages);
				appendStringInfoChar(es->str, '\n');
			}
			else
			{
				ExplainPropertyUInteger("Exact Heap Blocks", NULL,
										si->exact_pages, es);
				ExplainPropertyUInteger("Lossy Heap Blocks", NULL,
										si->lossy_pages, es);
			}

			if (es->workers_state)
				ExplainCloseWorker(n, es);
		}
	}
}

/*
 * If it's EXPLAIN ANALYZE, show instrumentation information for a plan node
 *
 * 若是 EXPLAIN ANALYZE，则显示计划节点的插桩信息。
 *
 * "which" identifies which instrumentation counter to print
 *
 * which 指明要打印哪个插桩计数器。
 */
static void
show_instrumentation_count(const char *qlabel, int which,
						   PlanState *planstate, ExplainState *es)
{
	double		nfiltered;
	double		nloops;

	if (!es->analyze || !planstate->instrument)
		return;

	if (which == 2)
		nfiltered = planstate->instrument->nfiltered2;
	else
		nfiltered = planstate->instrument->nfiltered1;
	nloops = planstate->instrument->nloops;

	/* In text mode, suppress zero counts; they're not interesting enough */
	/*
	 *
	 * 文本模式下抑制为零的计数；它们不够有意思。
	 */
	if (nfiltered > 0 || es->format != EXPLAIN_FORMAT_TEXT)
	{
		if (nloops > 0)
			ExplainPropertyFloat(qlabel, NULL, nfiltered / nloops, 0, es);
		else
			ExplainPropertyFloat(qlabel, NULL, 0.0, 0, es);
	}
}

/*
 * Show extra information for a ForeignScan node.
 *
 * 显示 ForeignScan 节点的额外信息。
 */
static void
show_foreignscan_info(ForeignScanState *fsstate, ExplainState *es)
{
	FdwRoutine *fdwroutine = fsstate->fdwroutine;

	/* Let the FDW emit whatever fields it wants */
	/*
	 *
	 * 让 FDW 输出它想要的任何字段。
	 */
	if (((ForeignScan *) fsstate->ss.ps.plan)->operation != CMD_SELECT)
	{
		if (fdwroutine->ExplainDirectModify != NULL)
			fdwroutine->ExplainDirectModify(fsstate, es);
	}
	else
	{
		if (fdwroutine->ExplainForeignScan != NULL)
			fdwroutine->ExplainForeignScan(fsstate, es);
	}
}

/*
 * Fetch the name of an index in an EXPLAIN
 *
 * 在 EXPLAIN 中取得索引的名字。
 *
 * We allow plugins to get control here so that plans involving hypothetical
 * indexes can be explained.
 *
 * 允许插件在这里取得控制权，以便解释涉及假设索引的计划。
 *
 * Note: names returned by this function should be "raw"; the caller will
 * apply quoting if needed.  Formerly the convention was to do quoting here,
 * but we don't want that in non-text output formats.
 *
 * 注意：本函数返回的名字应当是原始的；调用方会在需要时加引号。以前的约定是在
 * 这里加引号，但非文本输出格式不希望那样。
 */
static const char *
explain_get_index_name(Oid indexId)
{
	const char *result;

	if (explain_get_index_name_hook)
		result = (*explain_get_index_name_hook) (indexId);
	else
		result = NULL;
	if (result == NULL)
	{
		/* default behavior: look it up in the catalogs */
		/*
		 *
		 * 默认行为：到系统目录中查找。
		 */
		result = get_rel_name(indexId);
		if (result == NULL)
			elog(ERROR, "cache lookup failed for index %u", indexId);
	}
	return result;
}

/*
 * Return whether show_buffer_usage would have anything to print, if given
 * the same 'usage' data.  Note that when the format is anything other than
 * text, we print even if the counters are all zeroes.
 *
 * 返回在给定同样的 usage 数据时，show_buffer_usage 是否会打印任何内容。注意格
 * 式不是文本时，即使计数器全为零也会打印。
 */
static bool
peek_buffer_usage(ExplainState *es, const BufferUsage *usage)
{
	bool		has_shared;
	bool		has_local;
	bool		has_temp;
	bool		has_shared_timing;
	bool		has_local_timing;
	bool		has_temp_timing;

	if (usage == NULL)
		return false;

	if (es->format != EXPLAIN_FORMAT_TEXT)
		return true;

	has_shared = (usage->shared_blks_hit > 0 ||
				  usage->shared_blks_read > 0 ||
				  usage->shared_blks_dirtied > 0 ||
				  usage->shared_blks_written > 0);
	has_local = (usage->local_blks_hit > 0 ||
				 usage->local_blks_read > 0 ||
				 usage->local_blks_dirtied > 0 ||
				 usage->local_blks_written > 0);
	has_temp = (usage->temp_blks_read > 0 ||
				usage->temp_blks_written > 0);
	has_shared_timing = (!INSTR_TIME_IS_ZERO(usage->shared_blk_read_time) ||
						 !INSTR_TIME_IS_ZERO(usage->shared_blk_write_time));
	has_local_timing = (!INSTR_TIME_IS_ZERO(usage->local_blk_read_time) ||
						!INSTR_TIME_IS_ZERO(usage->local_blk_write_time));
	has_temp_timing = (!INSTR_TIME_IS_ZERO(usage->temp_blk_read_time) ||
					   !INSTR_TIME_IS_ZERO(usage->temp_blk_write_time));

	return has_shared || has_local || has_temp || has_shared_timing ||
		has_local_timing || has_temp_timing;
}

/*
 * Show buffer usage details.  This better be sync with peek_buffer_usage.
 *
 * 显示缓冲区用量细节。这里必须与 peek_buffer_usage 保持同步。
 */
static void
show_buffer_usage(ExplainState *es, const BufferUsage *usage)
{
	if (es->format == EXPLAIN_FORMAT_TEXT)
	{
		bool		has_shared = (usage->shared_blks_hit > 0 ||
								  usage->shared_blks_read > 0 ||
								  usage->shared_blks_dirtied > 0 ||
								  usage->shared_blks_written > 0);
		bool		has_local = (usage->local_blks_hit > 0 ||
								 usage->local_blks_read > 0 ||
								 usage->local_blks_dirtied > 0 ||
								 usage->local_blks_written > 0);
		bool		has_temp = (usage->temp_blks_read > 0 ||
								usage->temp_blks_written > 0);
		bool		has_shared_timing = (!INSTR_TIME_IS_ZERO(usage->shared_blk_read_time) ||
										 !INSTR_TIME_IS_ZERO(usage->shared_blk_write_time));
		bool		has_local_timing = (!INSTR_TIME_IS_ZERO(usage->local_blk_read_time) ||
										!INSTR_TIME_IS_ZERO(usage->local_blk_write_time));
		bool		has_temp_timing = (!INSTR_TIME_IS_ZERO(usage->temp_blk_read_time) ||
									   !INSTR_TIME_IS_ZERO(usage->temp_blk_write_time));

		/* Show only positive counter values. */
		/*
		 *
		 * 只显示为正的计数值。
		 */
		if (has_shared || has_local || has_temp)
		{
			ExplainIndentText(es);
			appendStringInfoString(es->str, "Buffers:");

			if (has_shared)
			{
				appendStringInfoString(es->str, " shared");
				if (usage->shared_blks_hit > 0)
					appendStringInfo(es->str, " hit=%" PRId64,
									 usage->shared_blks_hit);
				if (usage->shared_blks_read > 0)
					appendStringInfo(es->str, " read=%" PRId64,
									 usage->shared_blks_read);
				if (usage->shared_blks_dirtied > 0)
					appendStringInfo(es->str, " dirtied=%" PRId64,
									 usage->shared_blks_dirtied);
				if (usage->shared_blks_written > 0)
					appendStringInfo(es->str, " written=%" PRId64,
									 usage->shared_blks_written);
				if (has_local || has_temp)
					appendStringInfoChar(es->str, ',');
			}
			if (has_local)
			{
				appendStringInfoString(es->str, " local");
				if (usage->local_blks_hit > 0)
					appendStringInfo(es->str, " hit=%" PRId64,
									 usage->local_blks_hit);
				if (usage->local_blks_read > 0)
					appendStringInfo(es->str, " read=%" PRId64,
									 usage->local_blks_read);
				if (usage->local_blks_dirtied > 0)
					appendStringInfo(es->str, " dirtied=%" PRId64,
									 usage->local_blks_dirtied);
				if (usage->local_blks_written > 0)
					appendStringInfo(es->str, " written=%" PRId64,
									 usage->local_blks_written);
				if (has_temp)
					appendStringInfoChar(es->str, ',');
			}
			if (has_temp)
			{
				appendStringInfoString(es->str, " temp");
				if (usage->temp_blks_read > 0)
					appendStringInfo(es->str, " read=%" PRId64,
									 usage->temp_blks_read);
				if (usage->temp_blks_written > 0)
					appendStringInfo(es->str, " written=%" PRId64,
									 usage->temp_blks_written);
			}
			appendStringInfoChar(es->str, '\n');
		}

		/* As above, show only positive counter values. */
		/*
		 *
		 * 同上，只显示为正的计数值。
		 */
		if (has_shared_timing || has_local_timing || has_temp_timing)
		{
			ExplainIndentText(es);
			appendStringInfoString(es->str, "I/O Timings:");

			if (has_shared_timing)
			{
				appendStringInfoString(es->str, " shared");
				if (!INSTR_TIME_IS_ZERO(usage->shared_blk_read_time))
					appendStringInfo(es->str, " read=%0.3f",
									 INSTR_TIME_GET_MILLISEC(usage->shared_blk_read_time));
				if (!INSTR_TIME_IS_ZERO(usage->shared_blk_write_time))
					appendStringInfo(es->str, " write=%0.3f",
									 INSTR_TIME_GET_MILLISEC(usage->shared_blk_write_time));
				if (has_local_timing || has_temp_timing)
					appendStringInfoChar(es->str, ',');
			}
			if (has_local_timing)
			{
				appendStringInfoString(es->str, " local");
				if (!INSTR_TIME_IS_ZERO(usage->local_blk_read_time))
					appendStringInfo(es->str, " read=%0.3f",
									 INSTR_TIME_GET_MILLISEC(usage->local_blk_read_time));
				if (!INSTR_TIME_IS_ZERO(usage->local_blk_write_time))
					appendStringInfo(es->str, " write=%0.3f",
									 INSTR_TIME_GET_MILLISEC(usage->local_blk_write_time));
				if (has_temp_timing)
					appendStringInfoChar(es->str, ',');
			}
			if (has_temp_timing)
			{
				appendStringInfoString(es->str, " temp");
				if (!INSTR_TIME_IS_ZERO(usage->temp_blk_read_time))
					appendStringInfo(es->str, " read=%0.3f",
									 INSTR_TIME_GET_MILLISEC(usage->temp_blk_read_time));
				if (!INSTR_TIME_IS_ZERO(usage->temp_blk_write_time))
					appendStringInfo(es->str, " write=%0.3f",
									 INSTR_TIME_GET_MILLISEC(usage->temp_blk_write_time));
			}
			appendStringInfoChar(es->str, '\n');
		}
	}
	else
	{
		ExplainPropertyInteger("Shared Hit Blocks", NULL,
							   usage->shared_blks_hit, es);
		ExplainPropertyInteger("Shared Read Blocks", NULL,
							   usage->shared_blks_read, es);
		ExplainPropertyInteger("Shared Dirtied Blocks", NULL,
							   usage->shared_blks_dirtied, es);
		ExplainPropertyInteger("Shared Written Blocks", NULL,
							   usage->shared_blks_written, es);
		ExplainPropertyInteger("Local Hit Blocks", NULL,
							   usage->local_blks_hit, es);
		ExplainPropertyInteger("Local Read Blocks", NULL,
							   usage->local_blks_read, es);
		ExplainPropertyInteger("Local Dirtied Blocks", NULL,
							   usage->local_blks_dirtied, es);
		ExplainPropertyInteger("Local Written Blocks", NULL,
							   usage->local_blks_written, es);
		ExplainPropertyInteger("Temp Read Blocks", NULL,
							   usage->temp_blks_read, es);
		ExplainPropertyInteger("Temp Written Blocks", NULL,
							   usage->temp_blks_written, es);
		if (track_io_timing)
		{
			ExplainPropertyFloat("Shared I/O Read Time", "ms",
								 INSTR_TIME_GET_MILLISEC(usage->shared_blk_read_time),
								 3, es);
			ExplainPropertyFloat("Shared I/O Write Time", "ms",
								 INSTR_TIME_GET_MILLISEC(usage->shared_blk_write_time),
								 3, es);
			ExplainPropertyFloat("Local I/O Read Time", "ms",
								 INSTR_TIME_GET_MILLISEC(usage->local_blk_read_time),
								 3, es);
			ExplainPropertyFloat("Local I/O Write Time", "ms",
								 INSTR_TIME_GET_MILLISEC(usage->local_blk_write_time),
								 3, es);
			ExplainPropertyFloat("Temp I/O Read Time", "ms",
								 INSTR_TIME_GET_MILLISEC(usage->temp_blk_read_time),
								 3, es);
			ExplainPropertyFloat("Temp I/O Write Time", "ms",
								 INSTR_TIME_GET_MILLISEC(usage->temp_blk_write_time),
								 3, es);
		}
	}
}

/*
 * Show WAL usage details.
 *
 * 显示 WAL 用量细节。
 */
static void
show_wal_usage(ExplainState *es, const WalUsage *usage)
{
	if (es->format == EXPLAIN_FORMAT_TEXT)
	{
		/* Show only positive counter values. */
		/*
		 *
		 * 只显示为正的计数值。
		 */
		if ((usage->wal_records > 0) || (usage->wal_fpi > 0) ||
			(usage->wal_bytes > 0) || (usage->wal_buffers_full > 0))
		{
			ExplainIndentText(es);
			appendStringInfoString(es->str, "WAL:");

			if (usage->wal_records > 0)
				appendStringInfo(es->str, " records=%" PRId64,
								 usage->wal_records);
			if (usage->wal_fpi > 0)
				appendStringInfo(es->str, " fpi=%" PRId64,
								 usage->wal_fpi);
			if (usage->wal_bytes > 0)
				appendStringInfo(es->str, " bytes=%" PRIu64,
								 usage->wal_bytes);
			if (usage->wal_buffers_full > 0)
				appendStringInfo(es->str, " buffers full=%" PRId64,
								 usage->wal_buffers_full);
			appendStringInfoChar(es->str, '\n');
		}
	}
	else
	{
		ExplainPropertyInteger("WAL Records", NULL,
							   usage->wal_records, es);
		ExplainPropertyInteger("WAL FPI", NULL,
							   usage->wal_fpi, es);
		ExplainPropertyUInteger("WAL Bytes", NULL,
								usage->wal_bytes, es);
		ExplainPropertyInteger("WAL Buffers Full", NULL,
							   usage->wal_buffers_full, es);
	}
}

/*
 * Show memory usage details.
 *
 * 显示内存用量细节。
 */
static void
show_memory_counters(ExplainState *es, const MemoryContextCounters *mem_counters)
{
	int64		memUsedkB = BYTES_TO_KILOBYTES(mem_counters->totalspace -
											   mem_counters->freespace);
	int64		memAllocatedkB = BYTES_TO_KILOBYTES(mem_counters->totalspace);

	if (es->format == EXPLAIN_FORMAT_TEXT)
	{
		ExplainIndentText(es);
		appendStringInfo(es->str,
						 "Memory: used=" INT64_FORMAT "kB  allocated=" INT64_FORMAT "kB",
						 memUsedkB, memAllocatedkB);
		appendStringInfoChar(es->str, '\n');
	}
	else
	{
		ExplainPropertyInteger("Memory Used", "kB", memUsedkB, es);
		ExplainPropertyInteger("Memory Allocated", "kB", memAllocatedkB, es);
	}
}


/*
 * Add some additional details about an IndexScan or IndexOnlyScan
 *
 * 为 IndexScan 或 IndexOnlyScan 补充一些细节。
 */
static void
ExplainIndexScanDetails(Oid indexid, ScanDirection indexorderdir,
						ExplainState *es)
{
	const char *indexname = explain_get_index_name(indexid);

	if (es->format == EXPLAIN_FORMAT_TEXT)
	{
		if (ScanDirectionIsBackward(indexorderdir))
			appendStringInfoString(es->str, " Backward");
		appendStringInfo(es->str, " using %s", quote_identifier(indexname));
	}
	else
	{
		const char *scandir;

		switch (indexorderdir)
		{
			case BackwardScanDirection:
				scandir = "Backward";
				break;
			case ForwardScanDirection:
				scandir = "Forward";
				break;
			default:
				scandir = "???";
				break;
		}
		ExplainPropertyText("Scan Direction", scandir, es);
		ExplainPropertyText("Index Name", indexname, es);
	}
}

/*
 * Show the target of a Scan node
 *
 * 显示 Scan 节点的目标。
 */
static void
ExplainScanTarget(Scan *plan, ExplainState *es)
{
	ExplainTargetRel((Plan *) plan, plan->scanrelid, es);
}

/*
 * Show the target of a ModifyTable node
 *
 * 显示 ModifyTable 节点的目标。
 *
 * Here we show the nominal target (ie, the relation that was named in the
 * original query).  If the actual target(s) is/are different, we'll show them
 * in show_modifytable_info().
 *
 * 这里显示名义目标（即原始查询中指名的关系）。若实际目标不同，会在
 * show_modifytable_info() 中显示。
 */
static void
ExplainModifyTarget(ModifyTable *plan, ExplainState *es)
{
	ExplainTargetRel((Plan *) plan, plan->nominalRelation, es);
}

/*
 * Show the target relation of a scan or modify node
 *
 * 显示扫描或修改节点的目标关系。
 */
static void
ExplainTargetRel(Plan *plan, Index rti, ExplainState *es)
{
	char	   *objectname = NULL;
	char	   *namespace = NULL;
	const char *objecttag = NULL;
	RangeTblEntry *rte;
	char	   *refname;

	rte = rt_fetch(rti, es->rtable);
	refname = (char *) list_nth(es->rtable_names, rti - 1);
	if (refname == NULL)
		refname = rte->eref->aliasname;

	switch (nodeTag(plan))
	{
		case T_SeqScan:
		case T_SampleScan:
		case T_IndexScan:
		case T_IndexOnlyScan:
		case T_BitmapHeapScan:
		case T_TidScan:
		case T_TidRangeScan:
		case T_ForeignScan:
		case T_CustomScan:
		case T_ModifyTable:
			/* Assert it's on a real relation */
			/*
			 *
			 * 断言它针对的是真实关系。
			 */
			Assert(rte->rtekind == RTE_RELATION);
			objectname = get_rel_name(rte->relid);
			if (es->verbose)
				namespace = get_namespace_name_or_temp(get_rel_namespace(rte->relid));
			objecttag = "Relation Name";
			break;
		case T_FunctionScan:
			{
				FunctionScan *fscan = (FunctionScan *) plan;

				/* Assert it's on a RangeFunction */
				/*
				 *
				 * 断言它针对的是 RangeFunction。
				 */
				Assert(rte->rtekind == RTE_FUNCTION);

				/*
				 * If the expression is still a function call of a single
				 * function, we can get the real name of the function.
				 * Otherwise, punt.  (Even if it was a single function call
				 * originally, the optimizer could have simplified it away.)
				 *
				 * 若表达式仍是对单个函数的函数调用，可以取得该函数的真实名字。
				 * 否则放弃。（即使它最初是单个函数调用，优化器也可能已经把它
				 * 简化掉了。）
				 */
				if (list_length(fscan->functions) == 1)
				{
					RangeTblFunction *rtfunc = (RangeTblFunction *) linitial(fscan->functions);

					if (IsA(rtfunc->funcexpr, FuncExpr))
					{
						FuncExpr   *funcexpr = (FuncExpr *) rtfunc->funcexpr;
						Oid			funcid = funcexpr->funcid;

						objectname = get_func_name(funcid);
						if (es->verbose)
							namespace = get_namespace_name_or_temp(get_func_namespace(funcid));
					}
				}
				objecttag = "Function Name";
			}
			break;
		case T_TableFuncScan:
			{
				TableFunc  *tablefunc = ((TableFuncScan *) plan)->tablefunc;

				Assert(rte->rtekind == RTE_TABLEFUNC);
				switch (tablefunc->functype)
				{
					case TFT_XMLTABLE:
						objectname = "xmltable";
						break;
					case TFT_JSON_TABLE:
						objectname = "json_table";
						break;
					default:
						elog(ERROR, "invalid TableFunc type %d",
							 (int) tablefunc->functype);
				}
				objecttag = "Table Function Name";
			}
			break;
		case T_ValuesScan:
			Assert(rte->rtekind == RTE_VALUES);
			break;
		case T_CteScan:
			/* Assert it's on a non-self-reference CTE */
			/*
			 *
			 * 断言它针对的是非自引用 CTE。
			 */
			Assert(rte->rtekind == RTE_CTE);
			Assert(!rte->self_reference);
			objectname = rte->ctename;
			objecttag = "CTE Name";
			break;
		case T_NamedTuplestoreScan:
			Assert(rte->rtekind == RTE_NAMEDTUPLESTORE);
			objectname = rte->enrname;
			objecttag = "Tuplestore Name";
			break;
		case T_WorkTableScan:
			/* Assert it's on a self-reference CTE */
			/*
			 *
			 * 断言它针对的是自引用 CTE。
			 */
			Assert(rte->rtekind == RTE_CTE);
			Assert(rte->self_reference);
			objectname = rte->ctename;
			objecttag = "CTE Name";
			break;
		default:
			break;
	}

	if (es->format == EXPLAIN_FORMAT_TEXT)
	{
		appendStringInfoString(es->str, " on");
		if (namespace != NULL)
			appendStringInfo(es->str, " %s.%s", quote_identifier(namespace),
							 quote_identifier(objectname));
		else if (objectname != NULL)
			appendStringInfo(es->str, " %s", quote_identifier(objectname));
		if (objectname == NULL || strcmp(refname, objectname) != 0)
			appendStringInfo(es->str, " %s", quote_identifier(refname));
	}
	else
	{
		if (objecttag != NULL && objectname != NULL)
			ExplainPropertyText(objecttag, objectname, es);
		if (namespace != NULL)
			ExplainPropertyText("Schema", namespace, es);
		ExplainPropertyText("Alias", refname, es);
	}
}

/*
 * Show extra information for a ModifyTable node
 *
 * 显示 ModifyTable 节点的额外信息。
 *
 * We have three objectives here.  First, if there's more than one target
 * table or it's different from the nominal target, identify the actual
 * target(s).  Second, give FDWs a chance to display extra info about foreign
 * targets.  Third, show information about ON CONFLICT.
 *
 * 这里有三个目标。第一，若目标表不止一个或不同于名义目标，则标识实际目标。第
 * 二，给 FDW 机会显示外部目标的额外信息。第三，显示 ON CONFLICT 的信息。
 */
static void
show_modifytable_info(ModifyTableState *mtstate, List *ancestors,
					  ExplainState *es)
{
	ModifyTable *node = (ModifyTable *) mtstate->ps.plan;
	const char *operation;
	const char *foperation;
	bool		labeltargets;
	int			j;
	List	   *idxNames = NIL;
	ListCell   *lst;

	switch (node->operation)
	{
		case CMD_INSERT:
			operation = "Insert";
			foperation = "Foreign Insert";
			break;
		case CMD_UPDATE:
			operation = "Update";
			foperation = "Foreign Update";
			break;
		case CMD_DELETE:
			operation = "Delete";
			foperation = "Foreign Delete";
			break;
		case CMD_MERGE:
			operation = "Merge";
			/* XXX unsupported for now, but avoid compiler noise */
			/*
			 *
			 * XXX：目前不支持，但避免编译器告警。
			 */
			foperation = "Foreign Merge";
			break;
		default:
			operation = "???";
			foperation = "Foreign ???";
			break;
	}

	/*
	 * Should we explicitly label target relations?
	 *
	 * 是否应显式标出目标关系？
	 *
	 * If there's only one target relation, do not list it if it's the
	 * relation named in the query, or if it has been pruned.  (Normally
	 * mtstate->resultRelInfo doesn't include pruned relations, but a single
	 * pruned target relation may be present, if all other target relations
	 * have been pruned.  See ExecInitModifyTable().)
	 *
	 * 若只有一个目标关系，当它就是查询中指名的关系，或它已被剪枝时，不要列出
	 * 它。（通常 mtstate->resultRelInfo 不含被剪枝的关系，但若其他目标关系都
	 * 已被剪枝，可能仍留下一个被剪枝的目标关系。见 ExecInitModifyTable()。）
	 */
	labeltargets = (mtstate->mt_nrels > 1 ||
					(mtstate->mt_nrels == 1 &&
					 mtstate->resultRelInfo[0].ri_RangeTableIndex != node->nominalRelation &&
					 bms_is_member(mtstate->resultRelInfo[0].ri_RangeTableIndex,
								   mtstate->ps.state->es_unpruned_relids)));

	if (labeltargets)
		ExplainOpenGroup("Target Tables", "Target Tables", false, es);

	for (j = 0; j < mtstate->mt_nrels; j++)
	{
		ResultRelInfo *resultRelInfo = mtstate->resultRelInfo + j;
		FdwRoutine *fdwroutine = resultRelInfo->ri_FdwRoutine;

		if (labeltargets)
		{
			/* Open a group for this target */
			/*
			 *
			 * 为该目标打开一个组。
			 */
			ExplainOpenGroup("Target Table", NULL, true, es);

			/*
			 * In text mode, decorate each target with operation type, so that
			 * ExplainTargetRel's output of " on foo" will read nicely.
			 *
			 * 文本模式下用操作类型装饰每个目标，使 ExplainTargetRel 输出的 on
			 * foo 读起来顺畅。
			 */
			if (es->format == EXPLAIN_FORMAT_TEXT)
			{
				ExplainIndentText(es);
				appendStringInfoString(es->str,
									   fdwroutine ? foperation : operation);
			}

			/* Identify target */
			/*
			 *
			 * 标识目标。
			 */
			ExplainTargetRel((Plan *) node,
							 resultRelInfo->ri_RangeTableIndex,
							 es);

			if (es->format == EXPLAIN_FORMAT_TEXT)
			{
				appendStringInfoChar(es->str, '\n');
				es->indent++;
			}
		}

		/* Give FDW a chance if needed */
		/*
		 *
		 * 若需要，给 FDW 一个机会。
		 */
		if (!resultRelInfo->ri_usesFdwDirectModify &&
			fdwroutine != NULL &&
			fdwroutine->ExplainForeignModify != NULL)
		{
			List	   *fdw_private = (List *) list_nth(node->fdwPrivLists, j);

			fdwroutine->ExplainForeignModify(mtstate,
											 resultRelInfo,
											 fdw_private,
											 j,
											 es);
		}

		if (labeltargets)
		{
			/* Undo the indentation we added in text format */
			/*
			 *
			 * 撤销文本格式下加上的缩进。
			 */
			if (es->format == EXPLAIN_FORMAT_TEXT)
				es->indent--;

			/* Close the group */
			/*
			 *
			 * 关闭该组。
			 */
			ExplainCloseGroup("Target Table", NULL, true, es);
		}
	}

	/* Gather names of ON CONFLICT arbiter indexes */
	/*
	 *
	 * 收集 ON CONFLICT 仲裁索引的名字。
	 */
	foreach(lst, node->arbiterIndexes)
	{
		char	   *indexname = get_rel_name(lfirst_oid(lst));

		idxNames = lappend(idxNames, indexname);
	}

	if (node->onConflictAction != ONCONFLICT_NONE)
	{
		ExplainPropertyText("Conflict Resolution",
							node->onConflictAction == ONCONFLICT_NOTHING ?
							"NOTHING" : "UPDATE",
							es);

		/*
		 * Don't display arbiter indexes at all when DO NOTHING variant
		 * implicitly ignores all conflicts
		 *
		 * 当 DO NOTHING 变体隐式忽略全部冲突时，完全不显示仲裁索引。
		 */
		if (idxNames)
			ExplainPropertyList("Conflict Arbiter Indexes", idxNames, es);

		/* ON CONFLICT DO UPDATE WHERE qual is specially displayed */
		/*
		 *
		 * ON CONFLICT DO UPDATE 的 WHERE 限定条件单独显示。
		 */
		if (node->onConflictWhere)
		{
			show_upper_qual((List *) node->onConflictWhere, "Conflict Filter",
							&mtstate->ps, ancestors, es);
			show_instrumentation_count("Rows Removed by Conflict Filter", 1, &mtstate->ps, es);
		}

		/* EXPLAIN ANALYZE display of actual outcome for each tuple proposed */
		/*
		 *
		 * EXPLAIN ANALYZE 下显示每个被提议元组的实际结果。
		 */
		if (es->analyze && mtstate->ps.instrument)
		{
			double		total;
			double		insert_path;
			double		other_path;

			InstrEndLoop(outerPlanState(mtstate)->instrument);

			/* count the number of source rows */
			/*
			 *
			 * 统计源行数。
			 */
			total = outerPlanState(mtstate)->instrument->ntuples;
			other_path = mtstate->ps.instrument->ntuples2;
			insert_path = total - other_path;

			ExplainPropertyFloat("Tuples Inserted", NULL,
								 insert_path, 0, es);
			ExplainPropertyFloat("Conflicting Tuples", NULL,
								 other_path, 0, es);
		}
	}
	else if (node->operation == CMD_MERGE)
	{
		/* EXPLAIN ANALYZE display of tuples processed */
		/*
		 *
		 * EXPLAIN ANALYZE 下显示已处理的元组。
		 */
		if (es->analyze && mtstate->ps.instrument)
		{
			double		total;
			double		insert_path;
			double		update_path;
			double		delete_path;
			double		skipped_path;

			InstrEndLoop(outerPlanState(mtstate)->instrument);

			/* count the number of source rows */
			/*
			 *
			 * 统计源行数。
			 */
			total = outerPlanState(mtstate)->instrument->ntuples;
			insert_path = mtstate->mt_merge_inserted;
			update_path = mtstate->mt_merge_updated;
			delete_path = mtstate->mt_merge_deleted;
			skipped_path = total - insert_path - update_path - delete_path;
			Assert(skipped_path >= 0);

			if (es->format == EXPLAIN_FORMAT_TEXT)
			{
				if (total > 0)
				{
					ExplainIndentText(es);
					appendStringInfoString(es->str, "Tuples:");
					if (insert_path > 0)
						appendStringInfo(es->str, " inserted=%.0f", insert_path);
					if (update_path > 0)
						appendStringInfo(es->str, " updated=%.0f", update_path);
					if (delete_path > 0)
						appendStringInfo(es->str, " deleted=%.0f", delete_path);
					if (skipped_path > 0)
						appendStringInfo(es->str, " skipped=%.0f", skipped_path);
					appendStringInfoChar(es->str, '\n');
				}
			}
			else
			{
				ExplainPropertyFloat("Tuples Inserted", NULL, insert_path, 0, es);
				ExplainPropertyFloat("Tuples Updated", NULL, update_path, 0, es);
				ExplainPropertyFloat("Tuples Deleted", NULL, delete_path, 0, es);
				ExplainPropertyFloat("Tuples Skipped", NULL, skipped_path, 0, es);
			}
		}
	}

	if (labeltargets)
		ExplainCloseGroup("Target Tables", "Target Tables", false, es);
}

/*
 * Explain the constituent plans of an Append, MergeAppend,
 * BitmapAnd, or BitmapOr node.
 *
 * 解释 Append、MergeAppend、BitmapAnd 或 BitmapOr 节点的组成计划。
 *
 * The ancestors list should already contain the immediate parent of these
 * plans.
 *
 * ancestors 列表应已包含这些计划的直接父节点。
 */
static void
ExplainMemberNodes(PlanState **planstates, int nplans,
				   List *ancestors, ExplainState *es)
{
	int			j;

	for (j = 0; j < nplans; j++)
		ExplainNode(planstates[j], ancestors,
					"Member", NULL, es);
}

/*
 * Report about any pruned subnodes of an Append or MergeAppend node.
 *
 * 报告 Append 或 MergeAppend 节点中被剪枝的子节点。
 *
 * nplans indicates the number of live subplans.
 * nchildren indicates the original number of subnodes in the Plan;
 * some of these may have been pruned by the run-time pruning code.
 *
 * nplans 表示仍然存活的子计划数。nchildren 表示 Plan 中原来的子节点数；其中一
 * 些可能已被运行时剪枝代码剪掉。
 */
static void
ExplainMissingMembers(int nplans, int nchildren, ExplainState *es)
{
	if (nplans < nchildren || es->format != EXPLAIN_FORMAT_TEXT)
		ExplainPropertyInteger("Subplans Removed", NULL,
							   nchildren - nplans, es);
}

/*
 * Explain a list of SubPlans (or initPlans, which also use SubPlan nodes).
 *
 * 解释一组 SubPlan（或同样使用 SubPlan 节点的 initPlan）。
 *
 * The ancestors list should already contain the immediate parent of these
 * SubPlans.
 *
 * ancestors 列表应已包含这些 SubPlan 的直接父节点。
 */
static void
ExplainSubPlans(List *plans, List *ancestors,
				const char *relationship, ExplainState *es)
{
	ListCell   *lst;

	foreach(lst, plans)
	{
		SubPlanState *sps = (SubPlanState *) lfirst(lst);
		SubPlan    *sp = sps->subplan;

		/*
		 * There can be multiple SubPlan nodes referencing the same physical
		 * subplan (same plan_id, which is its index in PlannedStmt.subplans).
		 * We should print a subplan only once, so track which ones we already
		 * printed.  This state must be global across the plan tree, since the
		 * duplicate nodes could be in different plan nodes, eg both a bitmap
		 * indexscan's indexqual and its parent heapscan's recheck qual.  (We
		 * do not worry too much about which plan node we show the subplan as
		 * attached to in such cases.)
		 *
		 * 可能有多个 SubPlan 节点引用同一个物理子计划（相同的 plan_id，即它在
		 * PlannedStmt.subplans 中的下标）。子计划只应打印一次，因此记录已经打
		 * 印过的那些。这个状态必须在整棵计划树上全局共享，因为重复节点可能位
		 * 于不同的计划节点，例如位图索引扫描的 indexqual 及其父堆扫描的
		 * recheck qual。（这种情况下我们不太在意把子计划显示成挂在哪个计划节
		 * 点上。）
		 */
		if (bms_is_member(sp->plan_id, es->printed_subplans))
			continue;
		es->printed_subplans = bms_add_member(es->printed_subplans,
											  sp->plan_id);

		/*
		 * Treat the SubPlan node as an ancestor of the plan node(s) within
		 * it, so that ruleutils.c can find the referents of subplan
		 * parameters.
		 *
		 * 把 SubPlan 节点当作其内部计划节点的祖先，以便 ruleutils.c 能找到子
		 * 计划参数的指称对象。
		 */
		ancestors = lcons(sp, ancestors);

		ExplainNode(sps->planstate, ancestors,
					relationship, sp->plan_name, es);

		ancestors = list_delete_first(ancestors);
	}
}

/*
 * Explain a list of children of a CustomScan.
 *
 * 解释 CustomScan 的一组子节点。
 */
static void
ExplainCustomChildren(CustomScanState *css, List *ancestors, ExplainState *es)
{
	ListCell   *cell;
	const char *label =
		(list_length(css->custom_ps) != 1 ? "children" : "child");

	foreach(cell, css->custom_ps)
		ExplainNode((PlanState *) lfirst(cell), ancestors, label, NULL, es);
}

/*
 * Create a per-plan-node workspace for collecting per-worker data.
 *
 * 为收集逐 worker 数据创建一个按计划节点划分的工作区。
 *
 * Output related to each worker will be temporarily "set aside" into a
 * separate buffer, which we'll merge into the main output stream once
 * we've processed all data for the plan node.  This makes it feasible to
 * generate a coherent sub-group of fields for each worker, even though the
 * code that produces the fields is in several different places in this file.
 * Formatting of such a set-aside field group is managed by
 * ExplainOpenSetAsideGroup and ExplainSaveGroup/ExplainRestoreGroup.
 *
 * 与每个 worker 相关的输出会暂时放到单独的缓冲区，等处理完该计划节点的全部数
 * 据后再并入主输出流。这样即使产生这些字段的代码分散在本文件的多处，仍能为每
 * 个 worker 生成连贯的字段子组。这种暂存字段组的格式由
 * ExplainOpenSetAsideGroup 以及 ExplainSaveGroup/ExplainRestoreGroup 管理。
 */
static ExplainWorkersState *
ExplainCreateWorkersState(int num_workers)
{
	ExplainWorkersState *wstate;

	wstate = (ExplainWorkersState *) palloc(sizeof(ExplainWorkersState));
	wstate->num_workers = num_workers;
	wstate->worker_inited = (bool *) palloc0(num_workers * sizeof(bool));
	wstate->worker_str = (StringInfoData *)
		palloc0(num_workers * sizeof(StringInfoData));
	wstate->worker_state_save = (int *) palloc(num_workers * sizeof(int));
	return wstate;
}

/*
 * Begin or resume output into the set-aside group for worker N.
 *
 * 开始或继续向 worker N 的暂存组输出。
 */
static void
ExplainOpenWorker(int n, ExplainState *es)
{
	ExplainWorkersState *wstate = es->workers_state;

	Assert(wstate);
	Assert(n >= 0 && n < wstate->num_workers);

	/* Save prior output buffer pointer */
	/*
	 *
	 * 保存先前的输出缓冲区指针。
	 */
	wstate->prev_str = es->str;

	if (!wstate->worker_inited[n])
	{
		/* First time through, so create the buffer for this worker */
		/*
		 *
		 * 第一次经过，因此为该 worker 创建缓冲区。
		 */
		initStringInfo(&wstate->worker_str[n]);
		es->str = &wstate->worker_str[n];

		/*
		 * Push suitable initial formatting state for this worker's field
		 * group.  We allow one extra logical nesting level, since this group
		 * will eventually be wrapped in an outer "Workers" group.
		 *
		 * 为该 worker 的字段组压入合适的初始格式状态。允许多一层逻辑嵌套，因
		 * 为这个组最终会被包进外层的 Workers 组。
		 */
		ExplainOpenSetAsideGroup("Worker", NULL, true, 2, es);

		/*
		 * In non-TEXT formats we always emit a "Worker Number" field, even if
		 * there's no other data for this worker.
		 *
		 * 非 TEXT 格式下总是输出 Worker Number 字段，即使该 worker 没有其他数
		 * 据。
		 */
		if (es->format != EXPLAIN_FORMAT_TEXT)
			ExplainPropertyInteger("Worker Number", NULL, n, es);

		wstate->worker_inited[n] = true;
	}
	else
	{
		/* Resuming output for a worker we've already emitted some data for */
		/*
		 *
		 * 继续为一个已经输出过一些数据的 worker 输出。
		 */
		es->str = &wstate->worker_str[n];

		/* Restore formatting state saved by last ExplainCloseWorker() */
		/*
		 *
		 * 恢复上一次 ExplainCloseWorker() 保存的格式状态。
		 */
		ExplainRestoreGroup(es, 2, &wstate->worker_state_save[n]);
	}

	/*
	 * In TEXT format, prefix the first output line for this worker with
	 * "Worker N:".  Then, any additional lines should be indented one more
	 * stop than the "Worker N" line is.
	 *
	 * TEXT 格式下，该 worker 的第一行输出以 Worker N: 为前缀。此后任何附加行
	 * 都应比 Worker N 那一行再多缩进一档。
	 */
	if (es->format == EXPLAIN_FORMAT_TEXT)
	{
		if (es->str->len == 0)
		{
			ExplainIndentText(es);
			appendStringInfo(es->str, "Worker %d:  ", n);
		}

		es->indent++;
	}
}

/*
 * End output for worker N --- must pair with previous ExplainOpenWorker call
 *
 * 结束 worker N 的输出——必须与先前的 ExplainOpenWorker 调用配对。
 */
static void
ExplainCloseWorker(int n, ExplainState *es)
{
	ExplainWorkersState *wstate = es->workers_state;

	Assert(wstate);
	Assert(n >= 0 && n < wstate->num_workers);
	Assert(wstate->worker_inited[n]);

	/*
	 * Save formatting state in case we do another ExplainOpenWorker(), then
	 * pop the formatting stack.
	 *
	 * 保存格式状态，以便再次调用 ExplainOpenWorker()，然后弹出格式栈。
	 */
	ExplainSaveGroup(es, 2, &wstate->worker_state_save[n]);

	/*
	 * In TEXT format, if we didn't actually produce any output line(s) then
	 * truncate off the partial line emitted by ExplainOpenWorker.  (This is
	 * to avoid bogus output if, say, show_buffer_usage chooses not to print
	 * anything for the worker.)  Also fix up the indent level.
	 *
	 * TEXT 格式下，若实际上没有产生任何输出行，则截掉 ExplainOpenWorker 发出
	 * 的不完整行。（这是为了避免例如 show_buffer_usage 选择不给该 worker 打印
	 * 任何内容时出现虚假输出。）同时修正缩进级别。
	 */
	if (es->format == EXPLAIN_FORMAT_TEXT)
	{
		while (es->str->len > 0 && es->str->data[es->str->len - 1] != '\n')
			es->str->data[--(es->str->len)] = '\0';

		es->indent--;
	}

	/* Restore prior output buffer pointer */
	/*
	 *
	 * 恢复先前的输出缓冲区指针。
	 */
	es->str = wstate->prev_str;
}

/*
 * Print per-worker info for current node, then free the ExplainWorkersState.
 *
 * 打印当前节点的逐 worker 信息，然后释放 ExplainWorkersState。
 */
static void
ExplainFlushWorkersState(ExplainState *es)
{
	ExplainWorkersState *wstate = es->workers_state;

	ExplainOpenGroup("Workers", "Workers", false, es);
	for (int i = 0; i < wstate->num_workers; i++)
	{
		if (wstate->worker_inited[i])
		{
			/* This must match previous ExplainOpenSetAsideGroup call */
			/*
			 *
			 * 这里必须与先前的 ExplainOpenSetAsideGroup 调用匹配。
			 */
			ExplainOpenGroup("Worker", NULL, true, es);
			appendStringInfoString(es->str, wstate->worker_str[i].data);
			ExplainCloseGroup("Worker", NULL, true, es);

			pfree(wstate->worker_str[i].data);
		}
	}
	ExplainCloseGroup("Workers", "Workers", false, es);

	pfree(wstate->worker_inited);
	pfree(wstate->worker_str);
	pfree(wstate->worker_state_save);
	pfree(wstate);
}

/*-------------------------------------------------------------------------
 *
 * pg_overexplain.c
 *	  allow EXPLAIN to dump even more details
 *
 * Copyright (c) 2016-2025, PostgreSQL Global Development Group
 *
 *	  contrib/pg_overexplain/pg_overexplain.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_class.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "fmgr.h"
#include "parser/parsetree.h"
#include "storage/lock.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"

PG_MODULE_MAGIC_EXT(
					.name = "pg_overexplain",
					.version = PG_VERSION
);

typedef struct
{
	bool		debug;
	bool		range_table;
} overexplain_options;

static overexplain_options *overexplain_ensure_options(ExplainState *es);
static void overexplain_debug_handler(ExplainState *es, DefElem *opt,
									  ParseState *pstate);
static void overexplain_range_table_handler(ExplainState *es, DefElem *opt,
											ParseState *pstate);
static void overexplain_per_node_hook(PlanState *planstate, List *ancestors,
									  const char *relationship,
									  const char *plan_name,
									  ExplainState *es);
static void overexplain_per_plan_hook(PlannedStmt *plannedstmt,
									  IntoClause *into,
									  ExplainState *es,
									  const char *queryString,
									  ParamListInfo params,
									  QueryEnvironment *queryEnv);
static void overexplain_debug(PlannedStmt *plannedstmt, ExplainState *es);
static void overexplain_range_table(PlannedStmt *plannedstmt,
									ExplainState *es);
static void overexplain_alias(const char *qlabel, Alias *alias,
							  ExplainState *es);
static void overexplain_bitmapset(const char *qlabel, Bitmapset *bms,
								  ExplainState *es);
static void overexplain_intlist(const char *qlabel, List *list,
								ExplainState *es);

static int	es_extension_id;
static explain_per_node_hook_type prev_explain_per_node_hook;
static explain_per_plan_hook_type prev_explain_per_plan_hook;

/*
 * Initialization we do when this module is loaded.
 *
 * 我们在加载该模块时进行初始化。
 */
void
_PG_init(void)
{
	/* Get an ID that we can use to cache data in an ExplainState.
	 *
	 * 获取一个 ID，我们可以使用它来缓存 ExpandState 中的数据。
	 */
	es_extension_id = GetExplainExtensionId("pg_overexplain");

	/* Register the new EXPLAIN options implemented by this module.
	 *
	 * 注册此模块实现的新 EXPLAIN 选项。
	 */
	RegisterExtensionExplainOption("debug", overexplain_debug_handler);
	RegisterExtensionExplainOption("range_table",
								   overexplain_range_table_handler);

	/* Use the per-node and per-plan hooks to make our options do something.
	 *
	 * 使用每个节点和每个计划的钩子让我们的选项做一些事情。
	 */
	prev_explain_per_node_hook = explain_per_node_hook;
	explain_per_node_hook = overexplain_per_node_hook;
	prev_explain_per_plan_hook = explain_per_plan_hook;
	explain_per_plan_hook = overexplain_per_plan_hook;
}

/*
 * Get the overexplain_options structure from an ExplainState; if there is
 * none, create one, attach it to the ExplainState, and return it.
 *
 * 从ExplainState中获取overexplain_options结构；如果没有，则创建一个，将其附加到ExplainState，然后返回它。
 */
static overexplain_options *
overexplain_ensure_options(ExplainState *es)
{
	overexplain_options *options;

	options = GetExplainExtensionState(es, es_extension_id);

	if (options == NULL)
	{
		options = palloc0(sizeof(overexplain_options));
		SetExplainExtensionState(es, es_extension_id, options);
	}

	return options;
}

/*
 * Parse handler for EXPLAIN (DEBUG).
 *
 * 解释（调试）的解析处理程序。
 */
static void
overexplain_debug_handler(ExplainState *es, DefElem *opt, ParseState *pstate)
{
	overexplain_options *options = overexplain_ensure_options(es);

	options->debug = defGetBoolean(opt);
}

/*
 * Parse handler for EXPLAIN (RANGE_TABLE).
 *
 * EXPLAIN (RANGE_TABLE) 的解析处理程序。
 */
static void
overexplain_range_table_handler(ExplainState *es, DefElem *opt,
								ParseState *pstate)
{
	overexplain_options *options = overexplain_ensure_options(es);

	options->range_table = defGetBoolean(opt);
}

/*
 * Print out additional per-node information as appropriate. If the user didn't
 * specify any of the options we support, do nothing; else, print whatever is
 * relevant to the specified options.
 *
 * 根据需要打印出每个节点的附加信息。如果用户没有指定我们支持的任何选项，则不执行任何操作；否则，打印与指定选项相关的任何内容。
 */
static void
overexplain_per_node_hook(PlanState *planstate, List *ancestors,
						  const char *relationship, const char *plan_name,
						  ExplainState *es)
{
	overexplain_options *options;
	Plan	   *plan = planstate->plan;

	if (prev_explain_per_node_hook)
		(*prev_explain_per_node_hook) (planstate, ancestors, relationship,
									   plan_name, es);

	options = GetExplainExtensionState(es, es_extension_id);
	if (options == NULL)
		return;

	/*
	 * If the "debug" option was given, display miscellaneous fields from the
	 * "Plan" node that would not otherwise be displayed.
	 *
	 * 如果给出了“调试”选项，则显示“计划”节点中原本不会显示的其他字段。
	 */
	if (options->debug)
	{
		/*
		 * Normal EXPLAIN will display "Disabled: true" if the node is
		 * disabled; but that is based on noticing that plan->disabled_nodes
		 * is higher than the sum of its children; here, we display the raw
		 * value, for debugging purposes.
		 *
		 * 如果节点被禁用，普通EXPLAIN会显示“Disabled: true”；但这是基于注意到 plan->disabled_nodes 高于其子节点的总和；在这里，我们显示原始值，用于调试目的。
		 */
		ExplainPropertyInteger("Disabled Nodes", NULL, plan->disabled_nodes,
							   es);

		/*
		 * Normal EXPLAIN will display the parallel_aware flag; here, we show
		 * the parallel_safe flag as well.
		 *
		 * 正常EXPLAIN会显示parallel_aware标志；在这里，我们还显示了parallel_safe 标志。
		 */
		ExplainPropertyBool("Parallel Safe", plan->parallel_safe, es);

		/*
		 * The plan node ID isn't normally displayed, since it is only useful
		 * for debugging.
		 *
		 * 计划节点 ID 通常不会显示，因为它仅对调试有用。
		 */
		ExplainPropertyInteger("Plan Node ID", NULL, plan->plan_node_id, es);

		/*
		 * It is difficult to explain what extParam and allParam mean in plain
		 * language, so we simply display these fields labelled with the
		 * structure member name. For compactness, the text format omits the
		 * display of this information when the bitmapset is empty.
		 *
		 * 很难用简单的语言解释 extParam 和 allParam 的含义，因此我们只是显示这些标有结构体成员名称的字段。为了简洁起见，当位图集为空时，文本格式会省略此信息的显示。
		 */
		if (es->format != EXPLAIN_FORMAT_TEXT || !bms_is_empty(plan->extParam))
			overexplain_bitmapset("extParam", plan->extParam, es);
		if (es->format != EXPLAIN_FORMAT_TEXT || !bms_is_empty(plan->allParam))
			overexplain_bitmapset("allParam", plan->allParam, es);
	}

	/*
	 * If the "range_table" option was specified, display information about
	 * the range table indexes for this node.
	 *
	 * 如果指定了“range_table”选项，则显示有关该节点的范围表索引的信息。
	 */
	if (options->range_table)
	{
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
				ExplainPropertyInteger("Scan RTI", NULL,
									   ((Scan *) plan)->scanrelid, es);
				break;
			case T_ForeignScan:
				overexplain_bitmapset("Scan RTIs",
									  ((ForeignScan *) plan)->fs_base_relids,
									  es);
				break;
			case T_CustomScan:
				overexplain_bitmapset("Scan RTIs",
									  ((CustomScan *) plan)->custom_relids,
									  es);
				break;
			case T_ModifyTable:
				ExplainPropertyInteger("Nominal RTI", NULL,
									   ((ModifyTable *) plan)->nominalRelation, es);
				ExplainPropertyInteger("Exclude Relation RTI", NULL,
									   ((ModifyTable *) plan)->exclRelRTI, es);
				break;
			case T_Append:
				overexplain_bitmapset("Append RTIs",
									  ((Append *) plan)->apprelids,
									  es);
				break;
			case T_MergeAppend:
				overexplain_bitmapset("Append RTIs",
									  ((MergeAppend *) plan)->apprelids,
									  es);
				break;
			default:
				break;
		}
	}
}

/*
 * Print out additional per-query information as appropriate. Here again, if
 * the user didn't specify any of the options implemented by this module, do
 * nothing; otherwise, call the appropriate function for each specified
 * option.
 *
 * 根据需要打印出每个查询的附加信息。同样，如果用户没有指定该模块实现的任何选项，则不执行任何操作；否则，为每个指定选项调用适当的函数。
 */
static void
overexplain_per_plan_hook(PlannedStmt *plannedstmt,
						  IntoClause *into,
						  ExplainState *es,
						  const char *queryString,
						  ParamListInfo params,
						  QueryEnvironment *queryEnv)
{
	overexplain_options *options;

	if (prev_explain_per_plan_hook)
		(*prev_explain_per_plan_hook) (plannedstmt, into, es, queryString,
									   params, queryEnv);

	options = GetExplainExtensionState(es, es_extension_id);
	if (options == NULL)
		return;

	if (options->debug)
		overexplain_debug(plannedstmt, es);

	if (options->range_table)
		overexplain_range_table(plannedstmt, es);
}

/*
 * Print out various details from the PlannedStmt that wouldn't otherwise
 * be displayed.
 *
 * 从 PlannedStmt 中打印出原本不会显示的各种详细信息。
 *
 * We don't try to print everything here. Information that would be displayed
 * anyway doesn't need to be printed again here, and things with lots of
 * substructure probably should be printed via separate options, or not at all.
 *
 * 我们并不试图在这里打印所有内容。无论如何都会显示的信息不需要在这里再次打印，并且具有大量子结构的内容可能应该通过单独的选项打印，或者根本不打印。
 */
static void
overexplain_debug(PlannedStmt *plannedstmt, ExplainState *es)
{
	char	   *commandType = NULL;
	StringInfoData flags;

	/* Even in text mode, we want to set this output apart as its own group.
	 *
	 * 即使在文本模式下，我们也希望将此输出单独设置为自己的组。
	 */
	ExplainOpenGroup("PlannedStmt", "PlannedStmt", true, es);
	if (es->format == EXPLAIN_FORMAT_TEXT)
	{
		ExplainIndentText(es);
		appendStringInfoString(es->str, "PlannedStmt:\n");
		es->indent++;
	}

	/* Print the command type.
	 *
	 * 打印命令类型。
	 */
	switch (plannedstmt->commandType)
	{
		case CMD_UNKNOWN:
			commandType = "unknown";
			break;
		case CMD_SELECT:
			commandType = "select";
			break;
		case CMD_UPDATE:
			commandType = "update";
			break;
		case CMD_INSERT:
			commandType = "insert";
			break;
		case CMD_DELETE:
			commandType = "delete";
			break;
		case CMD_MERGE:
			commandType = "merge";
			break;
		case CMD_UTILITY:
			commandType = "utility";
			break;
		case CMD_NOTHING:
			commandType = "nothing";
			break;
	}
	ExplainPropertyText("Command Type", commandType, es);

	/* Print various properties as a comma-separated list of flags.
	 *
	 * 将各种属性打印为逗号分隔的标志列表。
	 */
	initStringInfo(&flags);
	if (plannedstmt->hasReturning)
		appendStringInfoString(&flags, ", hasReturning");
	if (plannedstmt->hasModifyingCTE)
		appendStringInfoString(&flags, ", hasModifyingCTE");
	if (plannedstmt->canSetTag)
		appendStringInfoString(&flags, ", canSetTag");
	if (plannedstmt->transientPlan)
		appendStringInfoString(&flags, ", transientPlan");
	if (plannedstmt->dependsOnRole)
		appendStringInfoString(&flags, ", dependsOnRole");
	if (plannedstmt->parallelModeNeeded)
		appendStringInfoString(&flags, ", parallelModeNeeded");
	if (flags.len == 0)
		appendStringInfoString(&flags, ", none");
	ExplainPropertyText("Flags", flags.data + 2, es);

	/* Various lists of integers.
	 *
	 * 各种整数列表。
	 */
	overexplain_bitmapset("Subplans Needing Rewind",
						  plannedstmt->rewindPlanIDs, es);
	overexplain_intlist("Relation OIDs",
						plannedstmt->relationOids, es);
	overexplain_intlist("Executor Parameter Types",
						plannedstmt->paramExecTypes, es);

	/*
	 * Print the statement location. (If desired, we could alternatively print
	 * stmt_location and stmt_len as two separate fields.)
	 *
	 * 打印语句位置。 （如果需要，我们也可以将 stmt_location 和 stmt_len 作为两个单独的字段打印。）
	 */
	if (plannedstmt->stmt_location == -1)
		ExplainPropertyText("Parse Location", "Unknown", es);
	else if (plannedstmt->stmt_len == 0)
		ExplainPropertyText("Parse Location",
							psprintf("%d to end", plannedstmt->stmt_location),
							es);
	else
		ExplainPropertyText("Parse Location",
							psprintf("%d for %d bytes",
									 plannedstmt->stmt_location,
									 plannedstmt->stmt_len),
							es);

	/* Done with this group.
	 *
	 * 这个组就完事了。
	 */
	if (es->format == EXPLAIN_FORMAT_TEXT)
		es->indent--;
	ExplainCloseGroup("PlannedStmt", "PlannedStmt", true, es);
}

/*
 * Provide detailed information about the contents of the PlannedStmt's
 * range table.
 *
 * 提供有关 PlannedStmt 范围表内容的详细信息。
 */
static void
overexplain_range_table(PlannedStmt *plannedstmt, ExplainState *es)
{
	Index		rti;

	/* Open group, one entry per RangeTblEntry
	 *
	 * 开放组，每个 RangeTblEntry 一个条目
	 */
	ExplainOpenGroup("Range Table", "Range Table", false, es);

	/* Iterate over the range table
	 *
	 * 迭代范围表
	 */
	for (rti = 1; rti <= list_length(plannedstmt->rtable); ++rti)
	{
		RangeTblEntry *rte = rt_fetch(rti, plannedstmt->rtable);
		char	   *kind = NULL;
		char	   *relkind;

		/* NULL entries are possible; skip them
		 *
		 * NULL 条目是可能的；跳过它们
		 */
		if (rte == NULL)
			continue;

		/* Translate rtekind to a string
		 *
		 * 将 rtekind 转换为字符串
		 */
		switch (rte->rtekind)
		{
			case RTE_RELATION:
				kind = "relation";
				break;
			case RTE_SUBQUERY:
				kind = "subquery";
				break;
			case RTE_JOIN:
				kind = "join";
				break;
			case RTE_FUNCTION:
				kind = "function";
				break;
			case RTE_TABLEFUNC:
				kind = "tablefunc";
				break;
			case RTE_VALUES:
				kind = "values";
				break;
			case RTE_CTE:
				kind = "cte";
				break;
			case RTE_NAMEDTUPLESTORE:
				kind = "namedtuplestore";
				break;
			case RTE_RESULT:
				kind = "result";
				break;
			case RTE_GROUP:
				kind = "group";
				break;
		}

		/* Begin group for this specific RTE
		 *
		 * 开始此特定 RTE 的小组
		 */
		ExplainOpenGroup("Range Table Entry", NULL, true, es);

		/*
		 * In text format, the summary line displays the range table index and
		 * rtekind, plus indications if rte->inh and/or rte->inFromCl are set.
		 * In other formats, we display those as separate properties.
		 *
		 * 在文本格式中，摘要行显示范围表索引和 rtekind，以及是否设置了 rte->inh 和/或 rte->inFromCl 的指示。在其他格式中，我们将它们显示为单独的属性。
		 */
		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			ExplainIndentText(es);
			appendStringInfo(es->str, "RTI %u (%s%s%s):\n", rti, kind,
							 rte->inh ? ", inherited" : "",
							 rte->inFromCl ? ", in-from-clause" : "");
			es->indent++;
		}
		else
		{
			ExplainPropertyUInteger("RTI", NULL, rti, es);
			ExplainPropertyText("Kind", kind, es);
			ExplainPropertyBool("Inherited", rte->inh, es);
			ExplainPropertyBool("In From Clause", rte->inFromCl, es);
		}

		/* rte->alias is optional; rte->eref is requested
		 *
		 * rte->alias 是可选的；请求 rte->eref
		 */
		if (rte->alias != NULL)
			overexplain_alias("Alias", rte->alias, es);
		overexplain_alias("Eref", rte->eref, es);

		/*
		 * We adhere to the usual EXPLAIN convention that schema names are
		 * displayed only in verbose mode, and we emit nothing if there is no
		 * relation OID.
		 *
		 * 我们遵守通常的 EXPLAIN 约定，即模式名称仅以详细模式显示，并且如果没有关系 OID，我们不会发出任何内容。
		 */
		if (rte->relid != 0)
		{
			const char *relname;
			const char *qualname;

			relname = quote_identifier(get_rel_name(rte->relid));

			if (es->verbose)
			{
				Oid			nspoid = get_rel_namespace(rte->relid);
				char	   *nspname;

				nspname = get_namespace_name_or_temp(nspoid);
				qualname = psprintf("%s.%s", quote_identifier(nspname),
									relname);
			}
			else
				qualname = relname;

			ExplainPropertyText("Relation", qualname, es);
		}

		/* Translate relkind, if any, to a string
		 *
		 * 将 relkind（如果有）转换为字符串
		 */
		switch (rte->relkind)
		{
			case RELKIND_RELATION:
				relkind = "relation";
				break;
			case RELKIND_INDEX:
				relkind = "index";
				break;
			case RELKIND_SEQUENCE:
				relkind = "sequence";
				break;
			case RELKIND_TOASTVALUE:
				relkind = "toastvalue";
				break;
			case RELKIND_VIEW:
				relkind = "view";
				break;
			case RELKIND_MATVIEW:
				relkind = "matview";
				break;
			case RELKIND_COMPOSITE_TYPE:
				relkind = "composite_type";
				break;
			case RELKIND_FOREIGN_TABLE:
				relkind = "foreign_table";
				break;
			case RELKIND_PARTITIONED_TABLE:
				relkind = "partitioned_table";
				break;
			case RELKIND_PARTITIONED_INDEX:
				relkind = "partitioned_index";
				break;
			case '\0':
				relkind = NULL;
				break;
			default:
				relkind = psprintf("%c", rte->relkind);
				break;
		}

		/* If there is a relkind, show it
		 *
		 * 如果有亲戚，请展示出来
		 */
		if (relkind != NULL)
			ExplainPropertyText("Relation Kind", relkind, es);

		/* If there is a lock mode, show it
		 *
		 * 如果有锁定模式，请显示
		 */
		if (rte->rellockmode != 0)
			ExplainPropertyText("Relation Lock Mode",
								GetLockmodeName(DEFAULT_LOCKMETHOD,
												rte->rellockmode), es);

		/*
		 * If there is a perminfoindex, show it. We don't try to display
		 * information from the RTEPermissionInfo node here because they are
		 * just indexes plannedstmt->permInfos which could be separately
		 * dumped if someone wants to add EXPLAIN (PERMISSIONS) or similar.
		 *
		 * 如果有 perminfoindex，请显示它。我们不会尝试在此处显示来自 RTEPermissionInfo 节点的信息，因为它们只是索引plannedstmt->permInfos，如果有人想要添加 EXPLAIN (PERMISSIONS) 或类似内容，可以单独转储这些索引。
		 */
		if (rte->perminfoindex != 0)
			ExplainPropertyInteger("Permission Info Index", NULL,
								   rte->perminfoindex, es);

		/*
		 * add_rte_to_flat_rtable will clear rte->tablesample and
		 * rte->subquery in the finished plan, so skip those fields.
		 *
		 * add_rte_to_flat_rtable 将清除完成计划中的 rte->tablesample 和 rte->subquery，因此请跳过这些字段。
		 *
		 * However, the security_barrier flag is not shown by the core code,
		 * so let's print it here.
		 *
		 * 但是，核心代码没有显示 security_barrier 标志，所以让我们在这里打印它。
		 */
		if (es->format != EXPLAIN_FORMAT_TEXT || rte->security_barrier)
			ExplainPropertyBool("Security Barrier", rte->security_barrier, es);

		/*
		 * If this is a join, print out the fields that are specifically valid
		 * for joins.
		 *
		 * 如果这是联接，则打印出专门对联接有效的字段。
		 */
		if (rte->rtekind == RTE_JOIN)
		{
			char	   *jointype;

			switch (rte->jointype)
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

			/* Join type
			 *
			 * 加盟类型
			 */
			ExplainPropertyText("Join Type", jointype, es);

			/* # of JOIN USING columns
			 *
			 * JOIN USING 列数
			 */
			if (es->format != EXPLAIN_FORMAT_TEXT || rte->joinmergedcols != 0)
				ExplainPropertyInteger("JOIN USING Columns", NULL,
									   rte->joinmergedcols, es);

			/*
			 * add_rte_to_flat_rtable will clear joinaliasvars, joinleftcols,
			 * joinrightcols, and join_using_alias here, so skip those fields.
			 *
			 * add_rte_to_flat_rtable 将清除此处的 joinaliasvars、joinleftcols、joinrightcols 和 join_using_alias，因此请跳过这些字段。
			 */
		}

		/*
		 * add_rte_to_flat_rtable will clear functions, tablefunc, and
		 * values_lists, but we can display funcordinality.
		 *
		 * add_rte_to_flat_rtable将清除函数、tablefunc和values_lists，但我们可以显示funcordinality。
		 */
		if (rte->rtekind == RTE_FUNCTION)
			ExplainPropertyBool("WITH ORDINALITY", rte->funcordinality, es);

		/*
		 * If this is a CTE, print out CTE-related properties.
		 *
		 * 如果这是 CTE，则打印出与 CTE 相关的属性。
		 */
		if (rte->rtekind == RTE_CTE)
		{
			ExplainPropertyText("CTE Name", rte->ctename, es);
			ExplainPropertyUInteger("CTE Levels Up", NULL, rte->ctelevelsup,
									es);
			ExplainPropertyBool("CTE Self-Reference", rte->self_reference, es);
		}

		/*
		 * add_rte_to_flat_rtable will clear coltypes, coltypmods, and
		 * colcollations, so skip those fields.
		 *
		 * add_rte_to_flat_rtable 将清除 coltypes、coltypmods 和 colcollat​​ions，因此请跳过这些字段。
		 *
		 * If this is an ephemeral named relation, print out ENR-related
		 * properties.
		 *
		 * 如果这是一个临时命名关系，则打印出与 ENR 相关的属性。
		 */
		if (rte->rtekind == RTE_NAMEDTUPLESTORE)
		{
			ExplainPropertyText("ENR Name", rte->enrname, es);
			ExplainPropertyFloat("ENR Tuples", NULL, rte->enrtuples, 0, es);
		}

		/*
		 * add_rte_to_flat_rtable will clear groupexprs and securityQuals, so
		 * skip that field. We have handled inFromCl above, so the only thing
		 * left to handle here is rte->lateral.
		 *
		 * add_rte_to_flat_rtable 将清除 groupexprs 和 securityQuals，因此跳过该字段。上面我们已经处理了inFromCl，所以这里唯一需要处理的是rte->side。
		 */
		if (es->format != EXPLAIN_FORMAT_TEXT || rte->lateral)
			ExplainPropertyBool("Lateral", rte->lateral, es);

		/* Done with this RTE
		 *
		 * 完成此 RTE
		 */
		if (es->format == EXPLAIN_FORMAT_TEXT)
			es->indent--;
		ExplainCloseGroup("Range Table Entry", NULL, true, es);
	}

	/* Print PlannedStmt fields that contain RTIs.
	 *
	 * 打印包含 RTI 的 PlannedStmt 字段。
	 */
	if (es->format != EXPLAIN_FORMAT_TEXT ||
		!bms_is_empty(plannedstmt->unprunableRelids))
		overexplain_bitmapset("Unprunable RTIs", plannedstmt->unprunableRelids,
							  es);
	if (es->format != EXPLAIN_FORMAT_TEXT ||
		plannedstmt->resultRelations != NIL)
		overexplain_intlist("Result RTIs", plannedstmt->resultRelations, es);

	/* Close group, we're all done
	 *
	 * 关闭小组，我们就完成了
	 */
	ExplainCloseGroup("Range Table", "Range Table", false, es);
}

/*
 * Emit a text property describing the contents of an Alias.
 *
 * 发出描述别名内容的文本属性。
 *
 * Column lists can be quite long here, so perhaps we should have an option
 * to limit the display length by # of column or # of characters, but for
 * now, just display everything.
 *
 * 此处的列列表可能会很长，因此也许我们应该选择通过列数或字符数来限制显示长度，但现在只显示所有内容。
 */
static void
overexplain_alias(const char *qlabel, Alias *alias, ExplainState *es)
{
	StringInfoData buf;
	bool		first = true;

	Assert(alias != NULL);

	initStringInfo(&buf);
	appendStringInfo(&buf, "%s (", quote_identifier(alias->aliasname));

	foreach_node(String, cn, alias->colnames)
	{
		appendStringInfo(&buf, "%s%s",
						 first ? "" : ", ",
						 quote_identifier(cn->sval));
		first = false;
	}

	appendStringInfoChar(&buf, ')');
	ExplainPropertyText(qlabel, buf.data, es);
	pfree(buf.data);
}

/*
 * Emit a text property describing the contents of a bitmapset -- either a
 * space-separated list of integer members, or the word "none" if the bitmapset
 * is empty.
 *
 * 发出一个描述位图集内容的文本属性 - 要么是空格分隔的整数成员列表，要么是单词“none”（如果位图集为空）。
 */
static void
overexplain_bitmapset(const char *qlabel, Bitmapset *bms, ExplainState *es)
{
	int			x = -1;

	StringInfoData buf;

	if (bms_is_empty(bms))
	{
		ExplainPropertyText(qlabel, "none", es);
		return;
	}

	initStringInfo(&buf);
	while ((x = bms_next_member(bms, x)) >= 0)
		appendStringInfo(&buf, " %d", x);
	Assert(buf.data[0] == ' ');
	ExplainPropertyText(qlabel, buf.data + 1, es);
	pfree(buf.data);
}

/*
 * Emit a text property describing the contents of a list of integers, OIDs,
 * or XIDs -- either a space-separated list of integer members, or the word
 * "none" if the list is empty.
 *
 * 发出一个文本属性，描述整数、OID 或 XID 列表的内容——要么是空格分隔的整数成员列表，要么是单词“none”（如果列表为空）。
 */
static void
overexplain_intlist(const char *qlabel, List *list, ExplainState *es)
{
	StringInfoData buf;

	initStringInfo(&buf);

	if (list == NIL)
	{
		ExplainPropertyText(qlabel, "none", es);
		return;
	}

	if (IsA(list, IntList))
	{
		foreach_int(i, list)
			appendStringInfo(&buf, " %d", i);
	}
	else if (IsA(list, OidList))
	{
		foreach_oid(o, list)
			appendStringInfo(&buf, " %u", o);
	}
	else if (IsA(list, XidList))
	{
		foreach_xid(x, list)
			appendStringInfo(&buf, " %u", x);
	}
	else
	{
		appendStringInfoString(&buf, " not an integer list");
		Assert(false);
	}

	if (buf.len > 0)
		ExplainPropertyText(qlabel, buf.data + 1, es);

	pfree(buf.data);
}

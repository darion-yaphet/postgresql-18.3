/*-------------------------------------------------------------------------
 *
 * deparse.c
 *		  Query deparser for postgres_fdw
 *
 * This file includes functions that examine query WHERE clauses to see
 * whether they're safe to send to the remote server for execution, as
 * well as functions to construct the query text to be sent.  The latter
 * functionality is annoyingly duplicative of ruleutils.c, but there are
 * enough special considerations that it seems best to keep this separate.
 * One saving grace is that we only need deparse logic for node types that
 * we consider safe to send.
 *
 * We assume that the remote session's search_path is exactly "pg_catalog",
 * and thus we need schema-qualify all and only names outside pg_catalog.
 *
 * We do not consider that it is ever safe to send COLLATE expressions to
 * the remote server: it might not have the same collation names we do.
 * (Later we might consider it safe to send COLLATE "C", but even that would
 * fail on old remote servers.)  An expression is considered safe to send
 * only if all operator/function input collations used in it are traceable to
 * Var(s) of the foreign table.  That implies that if the remote server gets
 * a different answer than we do, the foreign table's columns are not marked
 * with collations that match the remote table's columns, which we can
 * consider to be user error.
 *
 * Portions Copyright (c) 2012-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  contrib/postgres_fdw/deparse.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_ts_config.h"
#include "catalog/pg_ts_dict.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "nodes/nodeFuncs.h"
#include "nodes/plannodes.h"
#include "optimizer/optimizer.h"
#include "optimizer/prep.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "postgres_fdw.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

/*
 * Global context for foreign_expr_walker's search of an expression tree.
 *
 * foreign_expr_walker 搜索表达式树的全局上下文。
 */
typedef struct foreign_glob_cxt
{
	PlannerInfo *root;			/* global planner state */
	RelOptInfo *foreignrel;		/* the foreign relation we are planning for */
	Relids		relids;			/* relids of base relations in the underlying
								 * scan */
} foreign_glob_cxt;

/*
 * Local (per-tree-level) context for foreign_expr_walker's search.
 * This is concerned with identifying collations used in the expression.
 *
 * foreign_expr_walker 搜索的本地（每树级别）上下文。这涉及识别表达式中使用的排序规则。
 */
typedef enum
{
	FDW_COLLATE_NONE,			/* expression is of a noncollatable type, or
								 * it has default collation that is not
								 *
								 * 它的默认排序规则不是
								 * traceable to a foreign Var */
	FDW_COLLATE_SAFE,			/* collation derives from a foreign Var */
	FDW_COLLATE_UNSAFE,			/* collation is non-default and derives from
								 * something other than a foreign Var */
} FDWCollateState;

typedef struct foreign_loc_cxt
{
	Oid			collation;		/* OID of current collation, if any */
	FDWCollateState state;		/* state of current collation choice */
} foreign_loc_cxt;

/*
 * Context for deparseExpr
 *
 * deparseExpr 的上下文
 */
typedef struct deparse_expr_cxt
{
	PlannerInfo *root;			/* global planner state */
	RelOptInfo *foreignrel;		/* the foreign relation we are planning for */
	RelOptInfo *scanrel;		/* the underlying scan relation. Same as
								 * foreignrel, when that represents a join or
								 *
								 * foreignrel，当它代表一个连接或
								 * a base relation. */
	StringInfo	buf;			/* output buffer to append to */
	List	  **params_list;	/* exprs that will become remote Params */
} deparse_expr_cxt;

#define REL_ALIAS_PREFIX	"r"
/* Handy macro to add relation name qualification
 *
 * 用于添加关系名称限定的方便宏
 */
#define ADD_REL_QUALIFIER(buf, varno)	\
		appendStringInfo((buf), "%s%d.", REL_ALIAS_PREFIX, (varno))
#define SUBQUERY_REL_ALIAS_PREFIX	"s"
#define SUBQUERY_COL_ALIAS_PREFIX	"c"

/*
 * Functions to determine whether an expression can be evaluated safely on
 * remote server.
 *
 * 用于确定是否可以在远程服务器上安全地计算表达式的函数。
 */
static bool foreign_expr_walker(Node *node,
								foreign_glob_cxt *glob_cxt,
								foreign_loc_cxt *outer_cxt,
								foreign_loc_cxt *case_arg_cxt);
static char *deparse_type_name(Oid type_oid, int32 typemod);

/*
 * Functions to construct string representation of a node tree.
 *
 * 构造节点树的字符串表示的函数。
 */
static void deparseTargetList(StringInfo buf,
							  RangeTblEntry *rte,
							  Index rtindex,
							  Relation rel,
							  bool is_returning,
							  Bitmapset *attrs_used,
							  bool qualify_col,
							  List **retrieved_attrs);
static void deparseExplicitTargetList(List *tlist,
									  bool is_returning,
									  List **retrieved_attrs,
									  deparse_expr_cxt *context);
static void deparseSubqueryTargetList(deparse_expr_cxt *context);
static void deparseReturningList(StringInfo buf, RangeTblEntry *rte,
								 Index rtindex, Relation rel,
								 bool trig_after_row,
								 List *withCheckOptionList,
								 List *returningList,
								 List **retrieved_attrs);
static void deparseColumnRef(StringInfo buf, int varno, int varattno,
							 RangeTblEntry *rte, bool qualify_col);
static void deparseRelation(StringInfo buf, Relation rel);
static void deparseExpr(Expr *node, deparse_expr_cxt *context);
static void deparseVar(Var *node, deparse_expr_cxt *context);
static void deparseConst(Const *node, deparse_expr_cxt *context, int showtype);
static void deparseParam(Param *node, deparse_expr_cxt *context);
static void deparseSubscriptingRef(SubscriptingRef *node, deparse_expr_cxt *context);
static void deparseFuncExpr(FuncExpr *node, deparse_expr_cxt *context);
static void deparseOpExpr(OpExpr *node, deparse_expr_cxt *context);
static bool isPlainForeignVar(Expr *node, deparse_expr_cxt *context);
static void deparseOperatorName(StringInfo buf, Form_pg_operator opform);
static void deparseDistinctExpr(DistinctExpr *node, deparse_expr_cxt *context);
static void deparseScalarArrayOpExpr(ScalarArrayOpExpr *node,
									 deparse_expr_cxt *context);
static void deparseRelabelType(RelabelType *node, deparse_expr_cxt *context);
static void deparseBoolExpr(BoolExpr *node, deparse_expr_cxt *context);
static void deparseNullTest(NullTest *node, deparse_expr_cxt *context);
static void deparseCaseExpr(CaseExpr *node, deparse_expr_cxt *context);
static void deparseArrayExpr(ArrayExpr *node, deparse_expr_cxt *context);
static void printRemoteParam(int paramindex, Oid paramtype, int32 paramtypmod,
							 deparse_expr_cxt *context);
static void printRemotePlaceholder(Oid paramtype, int32 paramtypmod,
								   deparse_expr_cxt *context);
static void deparseSelectSql(List *tlist, bool is_subquery, List **retrieved_attrs,
							 deparse_expr_cxt *context);
static void deparseLockingClause(deparse_expr_cxt *context);
static void appendOrderByClause(List *pathkeys, bool has_final_sort,
								deparse_expr_cxt *context);
static void appendLimitClause(deparse_expr_cxt *context);
static void appendConditions(List *exprs, deparse_expr_cxt *context);
static void deparseFromExprForRel(StringInfo buf, PlannerInfo *root,
								  RelOptInfo *foreignrel, bool use_alias,
								  Index ignore_rel, List **ignore_conds,
								  List **additional_conds,
								  List **params_list);
static void appendWhereClause(List *exprs, List *additional_conds,
							  deparse_expr_cxt *context);
static void deparseFromExpr(List *quals, deparse_expr_cxt *context);
static void deparseRangeTblRef(StringInfo buf, PlannerInfo *root,
							   RelOptInfo *foreignrel, bool make_subquery,
							   Index ignore_rel, List **ignore_conds,
							   List **additional_conds, List **params_list);
static void deparseAggref(Aggref *node, deparse_expr_cxt *context);
static void appendGroupByClause(List *tlist, deparse_expr_cxt *context);
static void appendOrderBySuffix(Oid sortop, Oid sortcoltype, bool nulls_first,
								deparse_expr_cxt *context);
static void appendAggOrderBy(List *orderList, List *targetList,
							 deparse_expr_cxt *context);
static void appendFunctionName(Oid funcid, deparse_expr_cxt *context);
static Node *deparseSortGroupClause(Index ref, List *tlist, bool force_colno,
									deparse_expr_cxt *context);

/*
 * Helper functions
 *
 * 辅助函数
 */
static bool is_subquery_var(Var *node, RelOptInfo *foreignrel,
							int *relno, int *colno);
static void get_relation_column_alias_ids(Var *node, RelOptInfo *foreignrel,
										  int *relno, int *colno);


/*
 * Examine each qual clause in input_conds, and classify them into two groups,
 * which are returned as two lists:
 *	- remote_conds contains expressions that can be evaluated remotely
 *	- local_conds contains expressions that can't be evaluated remotely
 *
 * 检查 input_conds 中的每个 qual 子句，并将它们分为两组，并以两个列表的形式返回： - remote_conds 包含可以远程计算的表达式 - local_conds 包含无法远程计算的表达式
 */
void
classifyConditions(PlannerInfo *root,
				   RelOptInfo *baserel,
				   List *input_conds,
				   List **remote_conds,
				   List **local_conds)
{
	ListCell   *lc;

	*remote_conds = NIL;
	*local_conds = NIL;

	foreach(lc, input_conds)
	{
		RestrictInfo *ri = lfirst_node(RestrictInfo, lc);

		if (is_foreign_expr(root, baserel, ri->clause))
			*remote_conds = lappend(*remote_conds, ri);
		else
			*local_conds = lappend(*local_conds, ri);
	}
}

/*
 * Returns true if given expr is safe to evaluate on the foreign server.
 *
 * 如果给定的 expr 在外部服务器上可以安全计算，则返回 true。
 */
bool
is_foreign_expr(PlannerInfo *root,
				RelOptInfo *baserel,
				Expr *expr)
{
	foreign_glob_cxt glob_cxt;
	foreign_loc_cxt loc_cxt;
	PgFdwRelationInfo *fpinfo = (PgFdwRelationInfo *) (baserel->fdw_private);

	/*
	 * Check that the expression consists of nodes that are safe to execute
	 * remotely.
	 *
	 * 检查表达式是否包含可以安全远程执行的节点。
	 */
	glob_cxt.root = root;
	glob_cxt.foreignrel = baserel;

	/*
	 * For an upper relation, use relids from its underneath scan relation,
	 * because the upperrel's own relids currently aren't set to anything
	 * meaningful by the core code.  For other relation, use their own relids.
	 *
	 * 对于上层关系，请使用其下层扫描关系中的 relids，因为上层关系自己的 relids 当前未由核心代码设置为任何有意义的内容。  对于其他关系，使用他们自己的relids。
	 */
	if (IS_UPPER_REL(baserel))
		glob_cxt.relids = fpinfo->outerrel->relids;
	else
		glob_cxt.relids = baserel->relids;
	loc_cxt.collation = InvalidOid;
	loc_cxt.state = FDW_COLLATE_NONE;
	if (!foreign_expr_walker((Node *) expr, &glob_cxt, &loc_cxt, NULL))
		return false;

	/*
	 * If the expression has a valid collation that does not arise from a
	 * foreign var, the expression can not be sent over.
	 *
	 * 如果表达式具有不是由外部变量产生的有效排序规则，则无法发送该表达式。
	 */
	if (loc_cxt.state == FDW_COLLATE_UNSAFE)
		return false;

	/*
	 * An expression which includes any mutable functions can't be sent over
	 * because its result is not stable.  For example, sending now() remote
	 * side could cause confusion from clock offsets.  Future versions might
	 * be able to make this choice with more granularity.  (We check this last
	 * because it requires a lot of expensive catalog lookups.)
	 *
	 * 包含任何可变函数的表达式无法发送，因为其结果不稳定。  例如，发送 now() 远程端可能会导致时钟偏移的混乱。  未来的版本可能能够以更细粒度做出此选择。  （我们最后检查这一点，因为它需要大量昂贵的目录查找。）
	 */
	if (contain_mutable_functions((Node *) expr))
		return false;

	/* OK to evaluate on the remote server
	 *
	 * 可以在远程服务器上进行评估
	 */
	return true;
}

/*
 * Check if expression is safe to execute remotely, and return true if so.
 *
 * 检查表达式是否可以安全地远程执行，如果可以则返回 true。
 *
 * In addition, *outer_cxt is updated with collation information.
 *
 * 此外，*outer_cxt 会使用排序规则信息进行更新。
 *
 * case_arg_cxt is NULL if this subexpression is not inside a CASE-with-arg.
 * Otherwise, it points to the collation info derived from the arg expression,
 * which must be consulted by any CaseTestExpr.
 *
 * 如果此子表达式不在 CASE-with-arg 内，则 case_arg_cxt 为 NULL。否则，它指向从 arg 表达式派生的排序规则信息，任何 CaseTestExpr 都必须查阅该信息。
 *
 * We must check that the expression contains only node types we can deparse,
 * that all types/functions/operators are safe to send (they are "shippable"),
 * and that all collations used in the expression derive from Vars of the
 * foreign table.  Because of the latter, the logic is pretty close to
 * assign_collations_walker() in parse_collate.c, though we can assume here
 * that the given expression is valid.  Note function mutability is not
 * currently considered here.
 *
 * 我们必须检查表达式是否只包含我们可以解析的节点类型，所有类型/函数/运算符都可以安全发送（它们是“可交付的”），并且表达式中使用的所有排序规则都源自外部表的 Var。  由于后者，逻辑与 parse_collat​​e.c 中的 allocate_collat​​ions_walker() 非常接近，尽管我们可以在这里假设给定的表达式是有效的。  注意，这里目前不考虑函数的可变性。
 */
static bool
foreign_expr_walker(Node *node,
					foreign_glob_cxt *glob_cxt,
					foreign_loc_cxt *outer_cxt,
					foreign_loc_cxt *case_arg_cxt)
{
	bool		check_type = true;
	PgFdwRelationInfo *fpinfo;
	foreign_loc_cxt inner_cxt;
	Oid			collation;
	FDWCollateState state;

	/* Need do nothing for empty subexpressions
	 *
	 * 不需要对空子表达式执行任何操作
	 */
	if (node == NULL)
		return true;

	/* May need server info from baserel's fdw_private struct
	 *
	 * 可能需要来自 Baserel 的 fdw_private 结构的服务器信息
	 */
	fpinfo = (PgFdwRelationInfo *) (glob_cxt->foreignrel->fdw_private);

	/* Set up inner_cxt for possible recursion to child nodes
	 *
	 * 设置inner_cxt以可能递归到子节点
	 */
	inner_cxt.collation = InvalidOid;
	inner_cxt.state = FDW_COLLATE_NONE;

	switch (nodeTag(node))
	{
		case T_Var:
			{
				Var		   *var = (Var *) node;

				/*
				 * If the Var is from the foreign table, we consider its
				 * collation (if any) safe to use.  If it is from another
				 * table, we treat its collation the same way as we would a
				 * Param's collation, ie it's not safe for it to have a
				 * non-default collation.
				 *
				 * 如果 Var 来自外部表，我们认为它的排序规则（如果有）可以安全使用。  如果它来自另一个表，我们会像对待 Param 的排序规则一样对待它的排序规则，即它具有非默认排序规则是不安全的。
				 */
				if (bms_is_member(var->varno, glob_cxt->relids) &&
					var->varlevelsup == 0)
				{
					/* Var belongs to foreign table
					 *
					 * var 属于外部表
					 */

					/*
					 * System columns other than ctid should not be sent to
					 * the remote, since we don't make any effort to ensure
					 * that local and remote values match (tableoid, in
					 * particular, almost certainly doesn't match).
					 *
					 * 除 ctid 之外的系统列不应发送到远程，因为我们没有做出任何努力来确保本地值和远程值匹配（特别是 tableoid，几乎肯定不匹配）。
					 */
					if (var->varattno < 0 &&
						var->varattno != SelfItemPointerAttributeNumber)
						return false;

					/* Else check the collation
					 *
					 * 否则检查排序规则
					 */
					collation = var->varcollid;
					state = OidIsValid(collation) ? FDW_COLLATE_SAFE : FDW_COLLATE_NONE;
				}
				else
				{
					/* Var belongs to some other table
					 *
					 * Var 属于其他表
					 */
					collation = var->varcollid;
					if (collation == InvalidOid ||
						collation == DEFAULT_COLLATION_OID)
					{
						/*
						 * It's noncollatable, or it's safe to combine with a
						 * collatable foreign Var, so set state to NONE.
						 *
						 * 它是不可整理的，或者与可整理的外部 Var 组合是安全的，因此将状态设置为 NONE。
						 */
						state = FDW_COLLATE_NONE;
					}
					else
					{
						/*
						 * Do not fail right away, since the Var might appear
						 * in a collation-insensitive context.
						 *
						 * 不要立即失败，因为 Var 可能出现在排序规则不敏感的上下文中。
						 */
						state = FDW_COLLATE_UNSAFE;
					}
				}
			}
			break;
		case T_Const:
			{
				Const	   *c = (Const *) node;

				/*
				 * Constants of regproc and related types can't be shipped
				 * unless the referenced object is shippable.  But NULL's ok.
				 * (See also the related code in dependency.c.)
				 *
				 * 除非引用的对象可传送，否则无法传送 regproc 和相关类型的常量。  但 NULL 没问题。 （另请参阅 dependency.c 中的相关代码。）
				 */
				if (!c->constisnull)
				{
					switch (c->consttype)
					{
						case REGPROCOID:
						case REGPROCEDUREOID:
							if (!is_shippable(DatumGetObjectId(c->constvalue),
											  ProcedureRelationId, fpinfo))
								return false;
							break;
						case REGOPEROID:
						case REGOPERATOROID:
							if (!is_shippable(DatumGetObjectId(c->constvalue),
											  OperatorRelationId, fpinfo))
								return false;
							break;
						case REGCLASSOID:
							if (!is_shippable(DatumGetObjectId(c->constvalue),
											  RelationRelationId, fpinfo))
								return false;
							break;
						case REGTYPEOID:
							if (!is_shippable(DatumGetObjectId(c->constvalue),
											  TypeRelationId, fpinfo))
								return false;
							break;
						case REGCOLLATIONOID:
							if (!is_shippable(DatumGetObjectId(c->constvalue),
											  CollationRelationId, fpinfo))
								return false;
							break;
						case REGCONFIGOID:

							/*
							 * For text search objects only, we weaken the
							 * normal shippability criterion to allow all OIDs
							 * below FirstNormalObjectId.  Without this, none
							 * of the initdb-installed TS configurations would
							 * be shippable, which would be quite annoying.
							 *
							 * 仅对于文本搜索对象，我们削弱了正常可发货性标准以允许低于 FirstNormalObjectId 的所有 OID。  如果没有这个，initdb 安装的 TS 配置将无法交付，这将是非常烦人的。
							 */
							if (DatumGetObjectId(c->constvalue) >= FirstNormalObjectId &&
								!is_shippable(DatumGetObjectId(c->constvalue),
											  TSConfigRelationId, fpinfo))
								return false;
							break;
						case REGDICTIONARYOID:
							if (DatumGetObjectId(c->constvalue) >= FirstNormalObjectId &&
								!is_shippable(DatumGetObjectId(c->constvalue),
											  TSDictionaryRelationId, fpinfo))
								return false;
							break;
						case REGNAMESPACEOID:
							if (!is_shippable(DatumGetObjectId(c->constvalue),
											  NamespaceRelationId, fpinfo))
								return false;
							break;
						case REGROLEOID:
							if (!is_shippable(DatumGetObjectId(c->constvalue),
											  AuthIdRelationId, fpinfo))
								return false;
							break;
					}
				}

				/*
				 * If the constant has nondefault collation, either it's of a
				 * non-builtin type, or it reflects folding of a CollateExpr.
				 * It's unsafe to send to the remote unless it's used in a
				 * non-collation-sensitive context.
				 *
				 * 如果常量具有非默认排序规则，则它要么是非内置类型，要么反映 Collat​​eExpr 的折叠。除非在非排序规则敏感的上下文中使用，否则发送到远程是不安全的。
				 */
				collation = c->constcollid;
				if (collation == InvalidOid ||
					collation == DEFAULT_COLLATION_OID)
					state = FDW_COLLATE_NONE;
				else
					state = FDW_COLLATE_UNSAFE;
			}
			break;
		case T_Param:
			{
				Param	   *p = (Param *) node;

				/*
				 * If it's a MULTIEXPR Param, punt.  We can't tell from here
				 * whether the referenced sublink/subplan contains any remote
				 * Vars; if it does, handling that is too complicated to
				 * consider supporting at present.  Fortunately, MULTIEXPR
				 * Params are not reduced to plain PARAM_EXEC until the end of
				 * planning, so we can easily detect this case.  (Normal
				 * PARAM_EXEC Params are safe to ship because their values
				 * come from somewhere else in the plan tree; but a MULTIEXPR
				 * references a sub-select elsewhere in the same targetlist,
				 * so we'd be on the hook to evaluate it somehow if we wanted
				 * to handle such cases as direct foreign updates.)
				 *
				 * 如果它是 MULTIEXPR 参数，则弃用。  我们无法从这里判断引用的子链接/子计划是否包含任何远程变量；如果是的话，处理起来太复杂，目前不考虑支持。  幸运的是，直到规划结束，MULTIEXPR Params 才被简化为普通的 PARAM_EXEC，因此我们可以轻松检测到这种情况。  （普通 PARAM_EXEC 参数可以安全传送，因为它们的值来自计划树中的其他位置；但是 MULTIEXPR 引用同一目标列表中其他位置的子选择，因此如果我们想要处理直接外部更新等情况，我们就必须以某种方式对其进行评估。）
				 */
				if (p->paramkind == PARAM_MULTIEXPR)
					return false;

				/*
				 * Collation rule is same as for Consts and non-foreign Vars.
				 *
				 * 排序规则与常量和非外来变量相同。
				 */
				collation = p->paramcollid;
				if (collation == InvalidOid ||
					collation == DEFAULT_COLLATION_OID)
					state = FDW_COLLATE_NONE;
				else
					state = FDW_COLLATE_UNSAFE;
			}
			break;
		case T_SubscriptingRef:
			{
				SubscriptingRef *sr = (SubscriptingRef *) node;

				/* Assignment should not be in restrictions.
				 *
				 * 分配不应受到限制。
				 */
				if (sr->refassgnexpr != NULL)
					return false;

				/*
				 * Recurse into the remaining subexpressions.  The container
				 * subscripts will not affect collation of the SubscriptingRef
				 * result, so do those first and reset inner_cxt afterwards.
				 *
				 * 递归到剩余的子表达式。  容器下标不会影响 SubscriptingRef 结果的排序规则，因此首先执行这些操作，然后重置 inner_cxt。
				 */
				if (!foreign_expr_walker((Node *) sr->refupperindexpr,
										 glob_cxt, &inner_cxt, case_arg_cxt))
					return false;
				inner_cxt.collation = InvalidOid;
				inner_cxt.state = FDW_COLLATE_NONE;
				if (!foreign_expr_walker((Node *) sr->reflowerindexpr,
										 glob_cxt, &inner_cxt, case_arg_cxt))
					return false;
				inner_cxt.collation = InvalidOid;
				inner_cxt.state = FDW_COLLATE_NONE;
				if (!foreign_expr_walker((Node *) sr->refexpr,
										 glob_cxt, &inner_cxt, case_arg_cxt))
					return false;

				/*
				 * Container subscripting typically yields same collation as
				 * refexpr's, but in case it doesn't, use same logic as for
				 * function nodes.
				 *
				 * 容器下标通常会产生与 refexpr 相同的排序规则，但如果没有，请使用与函数节点相同的逻辑。
				 */
				collation = sr->refcollid;
				if (collation == InvalidOid)
					state = FDW_COLLATE_NONE;
				else if (inner_cxt.state == FDW_COLLATE_SAFE &&
						 collation == inner_cxt.collation)
					state = FDW_COLLATE_SAFE;
				else if (collation == DEFAULT_COLLATION_OID)
					state = FDW_COLLATE_NONE;
				else
					state = FDW_COLLATE_UNSAFE;
			}
			break;
		case T_FuncExpr:
			{
				FuncExpr   *fe = (FuncExpr *) node;

				/*
				 * If function used by the expression is not shippable, it
				 * can't be sent to remote because it might have incompatible
				 * semantics on remote side.
				 *
				 * 如果表达式使用的函数不可传送，则无法将其发送到远程，因为它在远程端可能具有不兼容的语义。
				 */
				if (!is_shippable(fe->funcid, ProcedureRelationId, fpinfo))
					return false;

				/*
				 * Recurse to input subexpressions.
				 *
				 * 递归输入子表达式。
				 */
				if (!foreign_expr_walker((Node *) fe->args,
										 glob_cxt, &inner_cxt, case_arg_cxt))
					return false;

				/*
				 * If function's input collation is not derived from a foreign
				 * Var, it can't be sent to remote.
				 *
				 * 如果函数的输入排序规则不是从外部 Var 派生的，则无法将其发送到远程。
				 */
				if (fe->inputcollid == InvalidOid)
					 /* OK, inputs are all noncollatable */ ;
				else if (inner_cxt.state != FDW_COLLATE_SAFE ||
						 fe->inputcollid != inner_cxt.collation)
					return false;

				/*
				 * Detect whether node is introducing a collation not derived
				 * from a foreign Var.  (If so, we just mark it unsafe for now
				 * rather than immediately returning false, since the parent
				 * node might not care.)
				 *
				 * 检测节点是否正在引入不是从外部 Var 派生的排序规则。  （如果是这样，我们只是暂时将其标记为不安全，而不是立即返回 false，因为父节点可能不在乎。）
				 */
				collation = fe->funccollid;
				if (collation == InvalidOid)
					state = FDW_COLLATE_NONE;
				else if (inner_cxt.state == FDW_COLLATE_SAFE &&
						 collation == inner_cxt.collation)
					state = FDW_COLLATE_SAFE;
				else if (collation == DEFAULT_COLLATION_OID)
					state = FDW_COLLATE_NONE;
				else
					state = FDW_COLLATE_UNSAFE;
			}
			break;
		case T_OpExpr:
		case T_DistinctExpr:	/* struct-equivalent to OpExpr */
			{
				OpExpr	   *oe = (OpExpr *) node;

				/*
				 * Similarly, only shippable operators can be sent to remote.
				 * (If the operator is shippable, we assume its underlying
				 * function is too.)
				 *
				 * 同样，只有可运送的操作员才能被发送到远程。 （如果操作符是可交付的，我们假设它的底层功能也是可交付的。）
				 */
				if (!is_shippable(oe->opno, OperatorRelationId, fpinfo))
					return false;

				/*
				 * Recurse to input subexpressions.
				 *
				 * 递归输入子表达式。
				 */
				if (!foreign_expr_walker((Node *) oe->args,
										 glob_cxt, &inner_cxt, case_arg_cxt))
					return false;

				/*
				 * If operator's input collation is not derived from a foreign
				 * Var, it can't be sent to remote.
				 *
				 * 如果操作员的输入排序规则不是从外部 Var 派生的，则无法将其发送到远程。
				 */
				if (oe->inputcollid == InvalidOid)
					 /* OK, inputs are all noncollatable */ ;
				else if (inner_cxt.state != FDW_COLLATE_SAFE ||
						 oe->inputcollid != inner_cxt.collation)
					return false;

				/* Result-collation handling is same as for functions
				 *
				 * 结果排序处理与函数相同
				 */
				collation = oe->opcollid;
				if (collation == InvalidOid)
					state = FDW_COLLATE_NONE;
				else if (inner_cxt.state == FDW_COLLATE_SAFE &&
						 collation == inner_cxt.collation)
					state = FDW_COLLATE_SAFE;
				else if (collation == DEFAULT_COLLATION_OID)
					state = FDW_COLLATE_NONE;
				else
					state = FDW_COLLATE_UNSAFE;
			}
			break;
		case T_ScalarArrayOpExpr:
			{
				ScalarArrayOpExpr *oe = (ScalarArrayOpExpr *) node;

				/*
				 * Again, only shippable operators can be sent to remote.
				 *
				 * 同样，只有可运送的操作员才能被发送到远程。
				 */
				if (!is_shippable(oe->opno, OperatorRelationId, fpinfo))
					return false;

				/*
				 * Recurse to input subexpressions.
				 *
				 * 递归输入子表达式。
				 */
				if (!foreign_expr_walker((Node *) oe->args,
										 glob_cxt, &inner_cxt, case_arg_cxt))
					return false;

				/*
				 * If operator's input collation is not derived from a foreign
				 * Var, it can't be sent to remote.
				 *
				 * 如果操作员的输入排序规则不是从外部 Var 派生的，则无法将其发送到远程。
				 */
				if (oe->inputcollid == InvalidOid)
					 /* OK, inputs are all noncollatable */ ;
				else if (inner_cxt.state != FDW_COLLATE_SAFE ||
						 oe->inputcollid != inner_cxt.collation)
					return false;

				/* Output is always boolean and so noncollatable.
				 *
				 * 输出始终是布尔值，因此不可整理。
				 */
				collation = InvalidOid;
				state = FDW_COLLATE_NONE;
			}
			break;
		case T_RelabelType:
			{
				RelabelType *r = (RelabelType *) node;

				/*
				 * Recurse to input subexpression.
				 *
				 * 递归到输入子表达式。
				 */
				if (!foreign_expr_walker((Node *) r->arg,
										 glob_cxt, &inner_cxt, case_arg_cxt))
					return false;

				/*
				 * RelabelType must not introduce a collation not derived from
				 * an input foreign Var (same logic as for a real function).
				 *
				 * RelabelType 不得引入不是从输入外部 Var 派生的排序规则（与实际函数的逻辑相同）。
				 */
				collation = r->resultcollid;
				if (collation == InvalidOid)
					state = FDW_COLLATE_NONE;
				else if (inner_cxt.state == FDW_COLLATE_SAFE &&
						 collation == inner_cxt.collation)
					state = FDW_COLLATE_SAFE;
				else if (collation == DEFAULT_COLLATION_OID)
					state = FDW_COLLATE_NONE;
				else
					state = FDW_COLLATE_UNSAFE;
			}
			break;
		case T_BoolExpr:
			{
				BoolExpr   *b = (BoolExpr *) node;

				/*
				 * Recurse to input subexpressions.
				 *
				 * 递归输入子表达式。
				 */
				if (!foreign_expr_walker((Node *) b->args,
										 glob_cxt, &inner_cxt, case_arg_cxt))
					return false;

				/* Output is always boolean and so noncollatable.
				 *
				 * 输出始终是布尔值，因此不可整理。
				 */
				collation = InvalidOid;
				state = FDW_COLLATE_NONE;
			}
			break;
		case T_NullTest:
			{
				NullTest   *nt = (NullTest *) node;

				/*
				 * Recurse to input subexpressions.
				 *
				 * 递归输入子表达式。
				 */
				if (!foreign_expr_walker((Node *) nt->arg,
										 glob_cxt, &inner_cxt, case_arg_cxt))
					return false;

				/* Output is always boolean and so noncollatable.
				 *
				 * 输出始终是布尔值，因此不可整理。
				 */
				collation = InvalidOid;
				state = FDW_COLLATE_NONE;
			}
			break;
		case T_CaseExpr:
			{
				CaseExpr   *ce = (CaseExpr *) node;
				foreign_loc_cxt arg_cxt;
				foreign_loc_cxt tmp_cxt;
				ListCell   *lc;

				/*
				 * Recurse to CASE's arg expression, if any.  Its collation
				 * has to be saved aside for use while examining CaseTestExprs
				 * within the WHEN expressions.
				 *
				 * 递归到 CASE 的 arg 表达式（如果有）。  必须将其排序规则保存起来，以便在检查 WHEN 表达式中的 CaseTestExprs 时使用。
				 */
				arg_cxt.collation = InvalidOid;
				arg_cxt.state = FDW_COLLATE_NONE;
				if (ce->arg)
				{
					if (!foreign_expr_walker((Node *) ce->arg,
											 glob_cxt, &arg_cxt, case_arg_cxt))
						return false;
				}

				/* Examine the CaseWhen subexpressions.
				 *
				 * 检查 CaseWhen 子表达式。
				 */
				foreach(lc, ce->args)
				{
					CaseWhen   *cw = lfirst_node(CaseWhen, lc);

					if (ce->arg)
					{
						/*
						 * In a CASE-with-arg, the parser should have produced
						 * WHEN clauses of the form "CaseTestExpr = RHS",
						 * possibly with an implicit coercion inserted above
						 * the CaseTestExpr.  However in an expression that's
						 * been through the optimizer, the WHEN clause could
						 * be almost anything (since the equality operator
						 * could have been expanded into an inline function).
						 * In such cases forbid pushdown, because
						 * deparseCaseExpr can't handle it.
						 *
						 * 在 CASE-with-arg 中，解析器应该生成“CaseTestExpr = RHS”形式的 WHEN 子句，可能在 CaseTestExpr 上方插入隐式强制转换。  然而，在经过优化器的表达式中，WHEN 子句几乎可以是任何内容（因为相等运算符可以扩展为内联函数）。在这种情况下禁止下推，因为 deparseCaseExpr 无法处理它。
						 */
						Node	   *whenExpr = (Node *) cw->expr;
						List	   *opArgs;

						if (!IsA(whenExpr, OpExpr))
							return false;

						opArgs = ((OpExpr *) whenExpr)->args;
						if (list_length(opArgs) != 2 ||
							!IsA(strip_implicit_coercions(linitial(opArgs)),
								 CaseTestExpr))
							return false;
					}

					/*
					 * Recurse to WHEN expression, passing down the arg info.
					 * Its collation doesn't affect the result (really, it
					 * should be boolean and thus not have a collation).
					 *
					 * 递归到 WHEN 表达式，传递 arg 信息。它的排序规则不会影响结果（实际上，它应该是布尔值，因此没有排序规则）。
					 */
					tmp_cxt.collation = InvalidOid;
					tmp_cxt.state = FDW_COLLATE_NONE;
					if (!foreign_expr_walker((Node *) cw->expr,
											 glob_cxt, &tmp_cxt, &arg_cxt))
						return false;

					/* Recurse to THEN expression.
					 *
					 * 递归到 THEN 表达式。
					 */
					if (!foreign_expr_walker((Node *) cw->result,
											 glob_cxt, &inner_cxt, case_arg_cxt))
						return false;
				}

				/* Recurse to ELSE expression.
				 *
				 * 递归到 ELSE 表达式。
				 */
				if (!foreign_expr_walker((Node *) ce->defresult,
										 glob_cxt, &inner_cxt, case_arg_cxt))
					return false;

				/*
				 * Detect whether node is introducing a collation not derived
				 * from a foreign Var.  (If so, we just mark it unsafe for now
				 * rather than immediately returning false, since the parent
				 * node might not care.)  This is the same as for function
				 * nodes, except that the input collation is derived from only
				 * the THEN and ELSE subexpressions.
				 *
				 * 检测节点是否正在引入不是从外部 Var 派生的排序规则。  （如果是这样，我们只是暂时将其标记为不安全，而不是立即返回 false，因为父节点可能不关心。）这与函数节点相同，只是输入排序规则仅源自 THEN 和 ELSE 子表达式。
				 */
				collation = ce->casecollid;
				if (collation == InvalidOid)
					state = FDW_COLLATE_NONE;
				else if (inner_cxt.state == FDW_COLLATE_SAFE &&
						 collation == inner_cxt.collation)
					state = FDW_COLLATE_SAFE;
				else if (collation == DEFAULT_COLLATION_OID)
					state = FDW_COLLATE_NONE;
				else
					state = FDW_COLLATE_UNSAFE;
			}
			break;
		case T_CaseTestExpr:
			{
				CaseTestExpr *c = (CaseTestExpr *) node;

				/* Punt if we seem not to be inside a CASE arg WHEN.
				 *
				 * 如果我们似乎不在 CASE arg WHEN 内，则平底船。
				 */
				if (!case_arg_cxt)
					return false;

				/*
				 * Otherwise, any nondefault collation attached to the
				 * CaseTestExpr node must be derived from foreign Var(s) in
				 * the CASE arg.
				 *
				 * 否则，附加到 CaseTestExpr 节点的任何非默认排序规则都必须从 CASE arg 中的外部 Var 派生。
				 */
				collation = c->collation;
				if (collation == InvalidOid)
					state = FDW_COLLATE_NONE;
				else if (case_arg_cxt->state == FDW_COLLATE_SAFE &&
						 collation == case_arg_cxt->collation)
					state = FDW_COLLATE_SAFE;
				else if (collation == DEFAULT_COLLATION_OID)
					state = FDW_COLLATE_NONE;
				else
					state = FDW_COLLATE_UNSAFE;
			}
			break;
		case T_ArrayExpr:
			{
				ArrayExpr  *a = (ArrayExpr *) node;

				/*
				 * Recurse to input subexpressions.
				 *
				 * 递归输入子表达式。
				 */
				if (!foreign_expr_walker((Node *) a->elements,
										 glob_cxt, &inner_cxt, case_arg_cxt))
					return false;

				/*
				 * ArrayExpr must not introduce a collation not derived from
				 * an input foreign Var (same logic as for a function).
				 *
				 * ArrayExpr 不得引入不是从输入外部 Var 派生的排序规则（与函数的逻辑相同）。
				 */
				collation = a->array_collid;
				if (collation == InvalidOid)
					state = FDW_COLLATE_NONE;
				else if (inner_cxt.state == FDW_COLLATE_SAFE &&
						 collation == inner_cxt.collation)
					state = FDW_COLLATE_SAFE;
				else if (collation == DEFAULT_COLLATION_OID)
					state = FDW_COLLATE_NONE;
				else
					state = FDW_COLLATE_UNSAFE;
			}
			break;
		case T_List:
			{
				List	   *l = (List *) node;
				ListCell   *lc;

				/*
				 * Recurse to component subexpressions.
				 *
				 * 递归到组件子表达式。
				 */
				foreach(lc, l)
				{
					if (!foreign_expr_walker((Node *) lfirst(lc),
											 glob_cxt, &inner_cxt, case_arg_cxt))
						return false;
				}

				/*
				 * When processing a list, collation state just bubbles up
				 * from the list elements.
				 *
				 * 处理列表时，排序规则状态只是从列表元素中冒出来。
				 */
				collation = inner_cxt.collation;
				state = inner_cxt.state;

				/* Don't apply exprType() to the list.
				 *
				 * 不要将 exprType() 应用于列表。
				 */
				check_type = false;
			}
			break;
		case T_Aggref:
			{
				Aggref	   *agg = (Aggref *) node;
				ListCell   *lc;

				/* Not safe to pushdown when not in grouping context
				 *
				 * 不在分组上下文中时下推不安全
				 */
				if (!IS_UPPER_REL(glob_cxt->foreignrel))
					return false;

				/* Only non-split aggregates are pushable.
				 *
				 * 只有非分裂聚合是可推送的。
				 */
				if (agg->aggsplit != AGGSPLIT_SIMPLE)
					return false;

				/* As usual, it must be shippable.
				 *
				 * 像往常一样，它必须是可发货的。
				 */
				if (!is_shippable(agg->aggfnoid, ProcedureRelationId, fpinfo))
					return false;

				/*
				 * Recurse to input args. aggdirectargs, aggorder and
				 * aggdistinct are all present in args, so no need to check
				 * their shippability explicitly.
				 *
				 * 递归到输入参数。 aggdirectargs、aggorder 和 aggdistinct 都存在于 args 中，因此无需显式检查它们的可发货性。
				 */
				foreach(lc, agg->args)
				{
					Node	   *n = (Node *) lfirst(lc);

					/* If TargetEntry, extract the expression from it
					 *
					 * 如果是TargetEntry，则从中提取表达式
					 */
					if (IsA(n, TargetEntry))
					{
						TargetEntry *tle = (TargetEntry *) n;

						n = (Node *) tle->expr;
					}

					if (!foreign_expr_walker(n,
											 glob_cxt, &inner_cxt, case_arg_cxt))
						return false;
				}

				/*
				 * For aggorder elements, check whether the sort operator, if
				 * specified, is shippable or not.
				 *
				 * 对于 aggorder 元素，检查排序运算符（如果指定）是否可传送。
				 */
				if (agg->aggorder)
				{
					foreach(lc, agg->aggorder)
					{
						SortGroupClause *srt = (SortGroupClause *) lfirst(lc);
						Oid			sortcoltype;
						TypeCacheEntry *typentry;
						TargetEntry *tle;

						tle = get_sortgroupref_tle(srt->tleSortGroupRef,
												   agg->args);
						sortcoltype = exprType((Node *) tle->expr);
						typentry = lookup_type_cache(sortcoltype,
													 TYPECACHE_LT_OPR | TYPECACHE_GT_OPR);
						/* Check shippability of non-default sort operator.
						 *
						 * 检查非默认排序运算符的可传送性。
						 */
						if (srt->sortop != typentry->lt_opr &&
							srt->sortop != typentry->gt_opr &&
							!is_shippable(srt->sortop, OperatorRelationId,
										  fpinfo))
							return false;
					}
				}

				/* Check aggregate filter
				 *
				 * 检查聚合过滤器
				 */
				if (!foreign_expr_walker((Node *) agg->aggfilter,
										 glob_cxt, &inner_cxt, case_arg_cxt))
					return false;

				/*
				 * If aggregate's input collation is not derived from a
				 * foreign Var, it can't be sent to remote.
				 *
				 * 如果聚合的输入排序规则不是从外部 Var 派生的，则无法将其发送到远程。
				 */
				if (agg->inputcollid == InvalidOid)
					 /* OK, inputs are all noncollatable */ ;
				else if (inner_cxt.state != FDW_COLLATE_SAFE ||
						 agg->inputcollid != inner_cxt.collation)
					return false;

				/*
				 * Detect whether node is introducing a collation not derived
				 * from a foreign Var.  (If so, we just mark it unsafe for now
				 * rather than immediately returning false, since the parent
				 * node might not care.)
				 *
				 * 检测节点是否正在引入不是从外部 Var 派生的排序规则。  （如果是这样，我们只是暂时将其标记为不安全，而不是立即返回 false，因为父节点可能不在乎。）
				 */
				collation = agg->aggcollid;
				if (collation == InvalidOid)
					state = FDW_COLLATE_NONE;
				else if (inner_cxt.state == FDW_COLLATE_SAFE &&
						 collation == inner_cxt.collation)
					state = FDW_COLLATE_SAFE;
				else if (collation == DEFAULT_COLLATION_OID)
					state = FDW_COLLATE_NONE;
				else
					state = FDW_COLLATE_UNSAFE;
			}
			break;
		default:

			/*
			 * If it's anything else, assume it's unsafe.  This list can be
			 * expanded later, but don't forget to add deparse support below.
			 *
			 * 如果有其他情况，则假设它不安全。  稍后可以扩展此列表，但不要忘记在下面添加 deparse 支持。
			 */
			return false;
	}

	/*
	 * If result type of given expression is not shippable, it can't be sent
	 * to remote because it might have incompatible semantics on remote side.
	 *
	 * 如果给定表达式的结果类型不可传送，则无法将其发送到远程，因为它在远程端可能具有不兼容的语义。
	 */
	if (check_type && !is_shippable(exprType(node), TypeRelationId, fpinfo))
		return false;

	/*
	 * Now, merge my collation information into my parent's state.
	 *
	 * 现在，将我的整理信息合并到我父母的状态中。
	 */
	if (state > outer_cxt->state)
	{
		/* Override previous parent state
		 *
		 * 覆盖之前的父状态
		 */
		outer_cxt->collation = collation;
		outer_cxt->state = state;
	}
	else if (state == outer_cxt->state)
	{
		/* Merge, or detect error if there's a collation conflict
		 *
		 * 合并，或在存在排序规则冲突时检测错误
		 */
		switch (state)
		{
			case FDW_COLLATE_NONE:
				/* Nothing + nothing is still nothing
				 *
				 * 什么都没有+什么都没有还是什么都没有
				 */
				break;
			case FDW_COLLATE_SAFE:
				if (collation != outer_cxt->collation)
				{
					/*
					 * Non-default collation always beats default.
					 *
					 * 非默认排序规则总是优于默认排序规则。
					 */
					if (outer_cxt->collation == DEFAULT_COLLATION_OID)
					{
						/* Override previous parent state
						 *
						 * 覆盖之前的父状态
						 */
						outer_cxt->collation = collation;
					}
					else if (collation != DEFAULT_COLLATION_OID)
					{
						/*
						 * Conflict; show state as indeterminate.  We don't
						 * want to "return false" right away, since parent
						 * node might not care about collation.
						 *
						 * 冲突;将状态显示为不确定。  我们不想立即“返回 false”，因为父节点可能不关心排序规则。
						 */
						outer_cxt->state = FDW_COLLATE_UNSAFE;
					}
				}
				break;
			case FDW_COLLATE_UNSAFE:
				/* We're still conflicted ...
				 *
				 * 我们还是很矛盾...
				 */
				break;
		}
	}

	/* It looks OK
	 *
	 * 看起来不错
	 */
	return true;
}

/*
 * Returns true if given expr is something we'd have to send the value of
 * to the foreign server.
 *
 * 如果给定的 expr 是我们必须将其值发送到外部服务器的值，则返回 true。
 *
 * This should return true when the expression is a shippable node that
 * deparseExpr would add to context->params_list.  Note that we don't care
 * if the expression *contains* such a node, only whether one appears at top
 * level.  We need this to detect cases where setrefs.c would recognize a
 * false match between an fdw_exprs item (which came from the params_list)
 * and an entry in fdw_scan_tlist (which we're considering putting the given
 * expression into).
 *
 * 当表达式是 deparseExpr 将添加到 context->params_list 的可传送节点时，这应该返回 true。  请注意，我们并不关心表达式是否*包含*这样的节点，只关心它是否出现在顶层。  我们需要它来检测 setrefs.c 识别 fdw_exprs 项（来自 params_list）和 fdw_scan_tlist 中的条目（我们正在考虑将给定表达式放入其中）之间的错误匹配的情况。
 */
bool
is_foreign_param(PlannerInfo *root,
				 RelOptInfo *baserel,
				 Expr *expr)
{
	if (expr == NULL)
		return false;

	switch (nodeTag(expr))
	{
		case T_Var:
			{
				/* It would have to be sent unless it's a foreign Var
				 *
				 * 除非是外国 Var，否则必须发送
				 */
				Var		   *var = (Var *) expr;
				PgFdwRelationInfo *fpinfo = (PgFdwRelationInfo *) (baserel->fdw_private);
				Relids		relids;

				if (IS_UPPER_REL(baserel))
					relids = fpinfo->outerrel->relids;
				else
					relids = baserel->relids;

				if (bms_is_member(var->varno, relids) && var->varlevelsup == 0)
					return false;	/* foreign Var, so not a param */
				else
					return true;	/* it'd have to be a param */
				break;
			}
		case T_Param:
			/* Params always have to be sent to the foreign server
			 *
			 * 参数始终必须发送到外部服务器
			 */
			return true;
		default:
			break;
	}
	return false;
}

/*
 * Returns true if it's safe to push down the sort expression described by
 * 'pathkey' to the foreign server.
 *
 * 如果可以安全地将“pathkey”描述的排序表达式下推到外部服务器，则返回 true。
 */
bool
is_foreign_pathkey(PlannerInfo *root,
				   RelOptInfo *baserel,
				   PathKey *pathkey)
{
	EquivalenceClass *pathkey_ec = pathkey->pk_eclass;
	PgFdwRelationInfo *fpinfo = (PgFdwRelationInfo *) baserel->fdw_private;

	/*
	 * is_foreign_expr would detect volatile expressions as well, but checking
	 * ec_has_volatile here saves some cycles.
	 *
	 * is_foreign_expr 也会检测易失性表达式，但在这里检查 ec_has_volatile 可以节省一些周期。
	 */
	if (pathkey_ec->ec_has_volatile)
		return false;

	/* can't push down the sort if the pathkey's opfamily is not shippable
	 *
	 * 如果路径键的 opfamily 不可发货，则无法下推排序
	 */
	if (!is_shippable(pathkey->pk_opfamily, OperatorFamilyRelationId, fpinfo))
		return false;

	/* can push if a suitable EC member exists
	 *
	 * 如果存在合适的 EC 成员，可以推动
	 */
	return (find_em_for_rel(root, pathkey_ec, baserel) != NULL);
}

/*
 * Convert type OID + typmod info into a type name we can ship to the remote
 * server.  Someplace else had better have verified that this type name is
 * expected to be known on the remote end.
 *
 * 将类型 OID +typmod 信息转换为我们可以发送到远程服务器的类型名称。  其他地方最好已经验证该类型名称预计在远程端是已知的。
 *
 * This is almost just format_type_with_typemod(), except that if left to its
 * own devices, that function will make schema-qualification decisions based
 * on the local search_path, which is wrong.  We must schema-qualify all
 * type names that are not in pg_catalog.  We assume here that built-in types
 * are all in pg_catalog and need not be qualified; otherwise, qualify.
 *
 * 这几乎只是 format_type_with_typemod()，除了如果留给它自己的设备，该函数将根据本地 search_path 做出模式限定决策，这是错误的。  我们必须对 pg_catalog 中没有的所有类型名称进行模式限定。  这里我们假设内置类型都在pg_catalog中，不需要限定；否则，符合资格。
 */
static char *
deparse_type_name(Oid type_oid, int32 typemod)
{
	bits16		flags = FORMAT_TYPE_TYPEMOD_GIVEN;

	if (!is_builtin(type_oid))
		flags |= FORMAT_TYPE_FORCE_QUALIFY;

	return format_type_extended(type_oid, typemod, flags);
}

/*
 * Build the targetlist for given relation to be deparsed as SELECT clause.
 *
 * 为要解析为 SELECT 子句的给定关系构建目标列表。
 *
 * The output targetlist contains the columns that need to be fetched from the
 * foreign server for the given relation.  If foreignrel is an upper relation,
 * then the output targetlist can also contain expressions to be evaluated on
 * foreign server.
 *
 * 输出目标列表包含需要从外部服务器获取给定关系的列。  如果foreignrel是上层关系，则输出目标列表还可以包含要在外部服务器上计算的表达式。
 */
List *
build_tlist_to_deparse(RelOptInfo *foreignrel)
{
	List	   *tlist = NIL;
	PgFdwRelationInfo *fpinfo = (PgFdwRelationInfo *) foreignrel->fdw_private;
	ListCell   *lc;

	/*
	 * For an upper relation, we have already built the target list while
	 * checking shippability, so just return that.
	 *
	 * 对于上层关系，我们在检查可发货性时已经构建了目标列表，因此只需返回即可。
	 */
	if (IS_UPPER_REL(foreignrel))
		return fpinfo->grouped_tlist;

	/*
	 * We require columns specified in foreignrel->reltarget->exprs and those
	 * required for evaluating the local conditions.
	 *
	 * 我们需要在foreignrel->reltarget->exprs中指定的列以及评估本地条件所需的列。
	 */
	tlist = add_to_flat_tlist(tlist,
							  pull_var_clause((Node *) foreignrel->reltarget->exprs,
											  PVC_RECURSE_PLACEHOLDERS));
	foreach(lc, fpinfo->local_conds)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

		tlist = add_to_flat_tlist(tlist,
								  pull_var_clause((Node *) rinfo->clause,
												  PVC_RECURSE_PLACEHOLDERS));
	}

	return tlist;
}

/*
 * Deparse SELECT statement for given relation into buf.
 *
 * 将给定关系的 SELECT 语句解析到 buf 中。
 *
 * tlist contains the list of desired columns to be fetched from foreign server.
 * For a base relation fpinfo->attrs_used is used to construct SELECT clause,
 * hence the tlist is ignored for a base relation.
 *
 * tlist 包含要从外部服务器获取的所需列的列表。对于基本关系，fpinfo->attrs_used 用于构造 SELECT 子句，因此对于基本关系，tlist 被忽略。
 *
 * remote_conds is the list of conditions to be deparsed into the WHERE clause
 * (or, in the case of upper relations, into the HAVING clause).
 *
 * remote_conds 是要解析到 WHERE 子句中的条件列表（或者，如果是上层关系，则解析到 HAVING 子句中）。
 *
 * If params_list is not NULL, it receives a list of Params and other-relation
 * Vars used in the clauses; these values must be transmitted to the remote
 * server as parameter values.
 *
 * 如果 params_list 不为 NULL，则它接收子句中使用的 Params 和其他关系变量的列表；这些值必须作为参数值传输到远程服务器。
 *
 * If params_list is NULL, we're generating the query for EXPLAIN purposes,
 * so Params and other-relation Vars should be replaced by dummy values.
 *
 * 如果 params_list 为 NULL，我们将出于 EXPLAIN 目的生成查询，因此 Params 和其他关系变量应替换为虚拟值。
 *
 * pathkeys is the list of pathkeys to order the result by.
 *
 * pathkeys 是用于对结果进行排序的路径键列表。
 *
 * is_subquery is the flag to indicate whether to deparse the specified
 * relation as a subquery.
 *
 * is_subquery 是指示是否将指定关系解析为子查询的标志。
 *
 * List of columns selected is returned in retrieved_attrs.
 *
 * 所选列的列表在retrieved_attrs 中返回。
 */
void
deparseSelectStmtForRel(StringInfo buf, PlannerInfo *root, RelOptInfo *rel,
						List *tlist, List *remote_conds, List *pathkeys,
						bool has_final_sort, bool has_limit, bool is_subquery,
						List **retrieved_attrs, List **params_list)
{
	deparse_expr_cxt context;
	PgFdwRelationInfo *fpinfo = (PgFdwRelationInfo *) rel->fdw_private;
	List	   *quals;

	/*
	 * We handle relations for foreign tables, joins between those and upper
	 * relations.
	 *
	 * 我们处理外部表的关系、这些表与上层关系之间的连接。
	 */
	Assert(IS_JOIN_REL(rel) || IS_SIMPLE_REL(rel) || IS_UPPER_REL(rel));

	/* Fill portions of context common to upper, join and base relation
	 *
	 * 填充上层关系、连接关系和基础关系共有的上下文部分
	 */
	context.buf = buf;
	context.root = root;
	context.foreignrel = rel;
	context.scanrel = IS_UPPER_REL(rel) ? fpinfo->outerrel : rel;
	context.params_list = params_list;

	/* Construct SELECT clause
	 *
	 * 构造 SELECT 子句
	 */
	deparseSelectSql(tlist, is_subquery, retrieved_attrs, &context);

	/*
	 * For upper relations, the WHERE clause is built from the remote
	 * conditions of the underlying scan relation; otherwise, we can use the
	 * supplied list of remote conditions directly.
	 *
	 * 对于上层关系，WHERE 子句是根据底层扫描关系的远程条件构建的；否则，我们可以直接使用提供的远程条件列表。
	 */
	if (IS_UPPER_REL(rel))
	{
		PgFdwRelationInfo *ofpinfo;

		ofpinfo = (PgFdwRelationInfo *) fpinfo->outerrel->fdw_private;
		quals = ofpinfo->remote_conds;
	}
	else
		quals = remote_conds;

	/* Construct FROM and WHERE clauses
	 *
	 * 构造 FROM 和 WHERE 子句
	 */
	deparseFromExpr(quals, &context);

	if (IS_UPPER_REL(rel))
	{
		/* Append GROUP BY clause
		 *
		 * 附加 GROUP BY 子句
		 */
		appendGroupByClause(tlist, &context);

		/* Append HAVING clause
		 *
		 * 附加 HAVING 子句
		 */
		if (remote_conds)
		{
			appendStringInfoString(buf, " HAVING ");
			appendConditions(remote_conds, &context);
		}
	}

	/* Add ORDER BY clause if we found any useful pathkeys
	 *
	 * 如果我们发现任何有用的路径键，请添加 ORDER BY 子句
	 */
	if (pathkeys)
		appendOrderByClause(pathkeys, has_final_sort, &context);

	/* Add LIMIT clause if necessary
	 *
	 * 如有必要，添加 LIMIT 子句
	 */
	if (has_limit)
		appendLimitClause(&context);

	/* Add any necessary FOR UPDATE/SHARE.
	 *
	 * 添加任何必要的更新/共享。
	 */
	deparseLockingClause(&context);
}

/*
 * Construct a simple SELECT statement that retrieves desired columns
 * of the specified foreign table, and append it to "buf".  The output
 * contains just "SELECT ... ".
 *
 * 构造一个简单的 SELECT 语句，检索指定外部表的所需列，并将其附加到“buf”。  输出仅包含“SELECT ...”。
 *
 * We also create an integer List of the columns being retrieved, which is
 * returned to *retrieved_attrs, unless we deparse the specified relation
 * as a subquery.
 *
 * 我们还创建一个正在检索的列的整数列表，该列表将返回到 *retrieved_attrs，除非我们将指定的关系解析为子查询。
 *
 * tlist is the list of desired columns.  is_subquery is the flag to
 * indicate whether to deparse the specified relation as a subquery.
 * Read prologue of deparseSelectStmtForRel() for details.
 *
 * tlist 是所需列的列表。  is_subquery 是指示是否将指定关系解析为子查询的标志。有关详细信息，请阅读 deparseSelectStmtForRel() 的序言​​。
 */
static void
deparseSelectSql(List *tlist, bool is_subquery, List **retrieved_attrs,
				 deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;
	RelOptInfo *foreignrel = context->foreignrel;
	PlannerInfo *root = context->root;
	PgFdwRelationInfo *fpinfo = (PgFdwRelationInfo *) foreignrel->fdw_private;

	/*
	 * Construct SELECT list
	 *
	 * 构造 SELECT 列表
	 */
	appendStringInfoString(buf, "SELECT ");

	if (is_subquery)
	{
		/*
		 * For a relation that is deparsed as a subquery, emit expressions
		 * specified in the relation's reltarget.  Note that since this is for
		 * the subquery, no need to care about *retrieved_attrs.
		 *
		 * 对于解析为子查询的关系，发出关系的 reltarget 中指定的表达式。  请注意，由于这是针对子查询的，因此无需关心 *retrieved_attrs。
		 */
		deparseSubqueryTargetList(context);
	}
	else if (IS_JOIN_REL(foreignrel) || IS_UPPER_REL(foreignrel))
	{
		/*
		 * For a join or upper relation the input tlist gives the list of
		 * columns required to be fetched from the foreign server.
		 *
		 * 对于连接或上层关系，输入 tlist 给出需要从外部服务器获取的列的列表。
		 */
		deparseExplicitTargetList(tlist, false, retrieved_attrs, context);
	}
	else
	{
		/*
		 * For a base relation fpinfo->attrs_used gives the list of columns
		 * required to be fetched from the foreign server.
		 *
		 * 对于基本关系 fpinfo->attrs_used 给出需要从外部服务器获取的列的列表。
		 */
		RangeTblEntry *rte = planner_rt_fetch(foreignrel->relid, root);

		/*
		 * Core code already has some lock on each rel being planned, so we
		 * can use NoLock here.
		 *
		 * 核心代码已经对正在规划的每个rel进行了一些锁定，因此我们可以在这里使用NoLock。
		 */
		Relation	rel = table_open(rte->relid, NoLock);

		deparseTargetList(buf, rte, foreignrel->relid, rel, false,
						  fpinfo->attrs_used, false, retrieved_attrs);
		table_close(rel, NoLock);
	}
}

/*
 * Construct a FROM clause and, if needed, a WHERE clause, and append those to
 * "buf".
 *
 * 构造一个 FROM 子句，如果需要，构造一个 WHERE 子句，并将它们附加到“buf”。
 *
 * quals is the list of clauses to be included in the WHERE clause.
 * (These may or may not include RestrictInfo decoration.)
 *
 * quals 是要包含在 WHERE 子句中的子句列表。 （这些可能包括也可能不包括 RestrictInfo 装饰。）
 */
static void
deparseFromExpr(List *quals, deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;
	RelOptInfo *scanrel = context->scanrel;
	List	   *additional_conds = NIL;

	/* For upper relations, scanrel must be either a joinrel or a baserel
	 *
	 * 对于上层关系，scanrel 必须是 joinrel 或 baserel
	 */
	Assert(!IS_UPPER_REL(context->foreignrel) ||
		   IS_JOIN_REL(scanrel) || IS_SIMPLE_REL(scanrel));

	/* Construct FROM clause
	 *
	 * 构造 FROM 子句
	 */
	appendStringInfoString(buf, " FROM ");
	deparseFromExprForRel(buf, context->root, scanrel,
						  (bms_membership(scanrel->relids) == BMS_MULTIPLE),
						  (Index) 0, NULL, &additional_conds,
						  context->params_list);
	appendWhereClause(quals, additional_conds, context);
	if (additional_conds != NIL)
		list_free_deep(additional_conds);
}

/*
 * Emit a target list that retrieves the columns specified in attrs_used.
 * This is used for both SELECT and RETURNING targetlists; the is_returning
 * parameter is true only for a RETURNING targetlist.
 *
 * 发出一个目标列表，该列表检索 attrs_used 中指定的列。这用于 SELECT 和 RETURNING 目标列表； is_returning 参数仅对于 RETURNING 目标列表为 true。
 *
 * The tlist text is appended to buf, and we also create an integer List
 * of the columns being retrieved, which is returned to *retrieved_attrs.
 *
 * tlist 文本被附加到 buf，我们还创建一个正在检索的列的整数列表，该列表返回到 *retrieved_attrs。
 *
 * If qualify_col is true, add relation alias before the column name.
 *
 * 如果qualify_col为true，则在列名前添加关系别名。
 */
static void
deparseTargetList(StringInfo buf,
				  RangeTblEntry *rte,
				  Index rtindex,
				  Relation rel,
				  bool is_returning,
				  Bitmapset *attrs_used,
				  bool qualify_col,
				  List **retrieved_attrs)
{
	TupleDesc	tupdesc = RelationGetDescr(rel);
	bool		have_wholerow;
	bool		first;
	int			i;

	*retrieved_attrs = NIL;

	/* If there's a whole-row reference, we'll need all the columns.
	 *
	 * 如果有整行引用，我们将需要所有列。
	 */
	have_wholerow = bms_is_member(0 - FirstLowInvalidHeapAttributeNumber,
								  attrs_used);

	first = true;
	for (i = 1; i <= tupdesc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, i - 1);

		/* Ignore dropped attributes.
		 *
		 * 忽略删除的属性。
		 */
		if (attr->attisdropped)
			continue;

		if (have_wholerow ||
			bms_is_member(i - FirstLowInvalidHeapAttributeNumber,
						  attrs_used))
		{
			if (!first)
				appendStringInfoString(buf, ", ");
			else if (is_returning)
				appendStringInfoString(buf, " RETURNING ");
			first = false;

			deparseColumnRef(buf, rtindex, i, rte, qualify_col);

			*retrieved_attrs = lappend_int(*retrieved_attrs, i);
		}
	}

	/*
	 * Add ctid if needed.  We currently don't support retrieving any other
	 * system columns.
	 *
	 * 如果需要，添加 ctid。  我们目前不支持检索任何其他系统列。
	 */
	if (bms_is_member(SelfItemPointerAttributeNumber - FirstLowInvalidHeapAttributeNumber,
					  attrs_used))
	{
		if (!first)
			appendStringInfoString(buf, ", ");
		else if (is_returning)
			appendStringInfoString(buf, " RETURNING ");
		first = false;

		if (qualify_col)
			ADD_REL_QUALIFIER(buf, rtindex);
		appendStringInfoString(buf, "ctid");

		*retrieved_attrs = lappend_int(*retrieved_attrs,
									   SelfItemPointerAttributeNumber);
	}

	/* Don't generate bad syntax if no undropped columns
	 *
	 * 如果没有未删除的列，则不会生成错误的语法
	 */
	if (first && !is_returning)
		appendStringInfoString(buf, "NULL");
}

/*
 * Deparse the appropriate locking clause (FOR UPDATE or FOR SHARE) for a
 * given relation (context->scanrel).
 *
 * 为给定关系（context->scanrel）解析适当的锁定子句（FOR UPDATE 或 FOR SHARE）。
 */
static void
deparseLockingClause(deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;
	PlannerInfo *root = context->root;
	RelOptInfo *rel = context->scanrel;
	PgFdwRelationInfo *fpinfo = (PgFdwRelationInfo *) rel->fdw_private;
	int			relid = -1;

	while ((relid = bms_next_member(rel->relids, relid)) >= 0)
	{
		/*
		 * Ignore relation if it appears in a lower subquery.  Locking clause
		 * for such a relation is included in the subquery if necessary.
		 *
		 * 如果关系出现在较低的子查询中，则忽略该关系。  如有必要，此类关系的锁定子句将包含在子查询中。
		 */
		if (bms_is_member(relid, fpinfo->lower_subquery_rels))
			continue;

		/*
		 * Add FOR UPDATE/SHARE if appropriate.  We apply locking during the
		 * initial row fetch, rather than later on as is done for local
		 * tables. The extra roundtrips involved in trying to duplicate the
		 * local semantics exactly don't seem worthwhile (see also comments
		 * for RowMarkType).
		 *
		 * 如果适用，请添加“更新/共享”。  我们在初始行获取期间应用锁定，而不是像本地表那样稍后应用锁定。尝试完全复制本地语义所涉及的额外往返似乎不值得（另请参阅 RowMarkType 的注释）。
		 *
		 * Note: because we actually run the query as a cursor, this assumes
		 * that DECLARE CURSOR ... FOR UPDATE is supported, which it isn't
		 * before 8.3.
		 *
		 * 注意：因为我们实际上将查询作为游标运行，所以这假设支持 DECLARE CURSOR ... FOR UPDATE，但在 8.3 之前不支持。
		 */
		if (bms_is_member(relid, root->all_result_relids) &&
			(root->parse->commandType == CMD_UPDATE ||
			 root->parse->commandType == CMD_DELETE))
		{
			/* Relation is UPDATE/DELETE target, so use FOR UPDATE
			 *
			 * 关系是 UPDATE/DELETE 目标，因此使用 FOR UPDATE
			 */
			appendStringInfoString(buf, " FOR UPDATE");

			/* Add the relation alias if we are here for a join relation
			 *
			 * 如果我们在这里是为了连接关系，请添加关系别名
			 */
			if (IS_JOIN_REL(rel))
				appendStringInfo(buf, " OF %s%d", REL_ALIAS_PREFIX, relid);
		}
		else
		{
			PlanRowMark *rc = get_plan_rowmark(root->rowMarks, relid);

			if (rc)
			{
				/*
				 * Relation is specified as a FOR UPDATE/SHARE target, so
				 * handle that.  (But we could also see LCS_NONE, meaning this
				 * isn't a target relation after all.)
				 *
				 * 关系被指定为 FOR UPDATE/SHARE 目标，因此请处理该目标。  （但我们也可以看到 LCS_NONE，这意味着这毕竟不是目标关系。）
				 *
				 * For now, just ignore any [NO] KEY specification, since (a)
				 * it's not clear what that means for a remote table that we
				 * don't have complete information about, and (b) it wouldn't
				 * work anyway on older remote servers.  Likewise, we don't
				 * worry about NOWAIT.
				 *
				 * 现在，只需忽略任何 [NO] KEY 规范，因为 (a) 不清楚这对于我们没有完整信息的远程表意味着什么，并且 (b) 它在较旧的远程服务器上无论如何都不起作用。  同样，我们也不担心NOWAIT。
				 */
				switch (rc->strength)
				{
					case LCS_NONE:
						/* No locking needed
						 *
						 * 无需锁定
						 */
						break;
					case LCS_FORKEYSHARE:
					case LCS_FORSHARE:
						appendStringInfoString(buf, " FOR SHARE");
						break;
					case LCS_FORNOKEYUPDATE:
					case LCS_FORUPDATE:
						appendStringInfoString(buf, " FOR UPDATE");
						break;
				}

				/* Add the relation alias if we are here for a join relation
				 *
				 * 如果我们在这里是为了连接关系，请添加关系别名
				 */
				if (bms_membership(rel->relids) == BMS_MULTIPLE &&
					rc->strength != LCS_NONE)
					appendStringInfo(buf, " OF %s%d", REL_ALIAS_PREFIX, relid);
			}
		}
	}
}

/*
 * Deparse conditions from the provided list and append them to buf.
 *
 * 从提供的列表中分离条件并将其附加到 buf。
 *
 * The conditions in the list are assumed to be ANDed. This function is used to
 * deparse WHERE clauses, JOIN .. ON clauses and HAVING clauses.
 *
 * 列表中的条件假定为 AND 运算。该函数用于解析 WHERE 子句、JOIN .. ON 子句和 HAVING 子句。
 *
 * Depending on the caller, the list elements might be either RestrictInfos
 * or bare clauses.
 *
 * 根据调用者的不同，列表元素可能是 RestrictInfos 或裸子句。
 */
static void
appendConditions(List *exprs, deparse_expr_cxt *context)
{
	int			nestlevel;
	ListCell   *lc;
	bool		is_first = true;
	StringInfo	buf = context->buf;

	/* Make sure any constants in the exprs are printed portably
	 *
	 * 确保表达式中的任何常量都可移植地打印
	 */
	nestlevel = set_transmission_modes();

	foreach(lc, exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc);

		/* Extract clause from RestrictInfo, if required
		 *
		 * 如果需要，从 RestrictInfo 中提取子句
		 */
		if (IsA(expr, RestrictInfo))
			expr = ((RestrictInfo *) expr)->clause;

		/* Connect expressions with "AND" and parenthesize each condition.
		 *
		 * 用“AND”连接表达式并将每个条件括起来。
		 */
		if (!is_first)
			appendStringInfoString(buf, " AND ");

		appendStringInfoChar(buf, '(');
		deparseExpr(expr, context);
		appendStringInfoChar(buf, ')');

		is_first = false;
	}

	reset_transmission_modes(nestlevel);
}

/*
 * Append WHERE clause, containing conditions from exprs and additional_conds,
 * to context->buf.
 *
 * 将包含 exprs 和 extra_conds 条件的 WHERE 子句附加到 context->buf。
 */
static void
appendWhereClause(List *exprs, List *additional_conds, deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;
	bool		need_and = false;
	ListCell   *lc;

	if (exprs != NIL || additional_conds != NIL)
		appendStringInfoString(buf, " WHERE ");

	/*
	 * If there are some filters, append them.
	 *
	 * 如果有一些过滤器，请附加它们。
	 */
	if (exprs != NIL)
	{
		appendConditions(exprs, context);
		need_and = true;
	}

	/*
	 * If there are some EXISTS conditions, coming from SEMI-JOINS, append
	 * them.
	 *
	 * 如果有一些来自半连接的 EXISTS 条件，请附加它们。
	 */
	foreach(lc, additional_conds)
	{
		if (need_and)
			appendStringInfoString(buf, " AND ");
		appendStringInfoString(buf, (char *) lfirst(lc));
		need_and = true;
	}
}

/* Output join name for given join type
 *
 * 给定连接类型的输出连接名称
 */
const char *
get_jointype_name(JoinType jointype)
{
	switch (jointype)
	{
		case JOIN_INNER:
			return "INNER";

		case JOIN_LEFT:
			return "LEFT";

		case JOIN_RIGHT:
			return "RIGHT";

		case JOIN_FULL:
			return "FULL";

		case JOIN_SEMI:
			return "SEMI";

		default:
			/* Shouldn't come here, but protect from buggy code.
			 *
			 * 不应该来这里，但要防止有错误的代码。
			 */
			elog(ERROR, "unsupported join type %d", jointype);
	}

	/* Keep compiler happy
	 *
	 * 让编译器高兴
	 */
	return NULL;
}

/*
 * Deparse given targetlist and append it to context->buf.
 *
 * 解析给定的目标列表并将其附加到 context->buf。
 *
 * tlist is list of TargetEntry's which in turn contain Var nodes.
 *
 * tlist 是 TargetEntry 的列表，其中又包含 Var 节点。
 *
 * retrieved_attrs is the list of continuously increasing integers starting
 * from 1. It has same number of entries as tlist.
 *
 * retrieved_attrs 是从 1 开始连续递增的整数的列表。它的条目数与 tlist 相同。
 *
 * This is used for both SELECT and RETURNING targetlists; the is_returning
 * parameter is true only for a RETURNING targetlist.
 *
 * 这用于 SELECT 和 RETURNING 目标列表； is_returning 参数仅对于 RETURNING 目标列表为 true。
 */
static void
deparseExplicitTargetList(List *tlist,
						  bool is_returning,
						  List **retrieved_attrs,
						  deparse_expr_cxt *context)
{
	ListCell   *lc;
	StringInfo	buf = context->buf;
	int			i = 0;

	*retrieved_attrs = NIL;

	foreach(lc, tlist)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);

		if (i > 0)
			appendStringInfoString(buf, ", ");
		else if (is_returning)
			appendStringInfoString(buf, " RETURNING ");

		deparseExpr((Expr *) tle->expr, context);

		*retrieved_attrs = lappend_int(*retrieved_attrs, i + 1);
		i++;
	}

	if (i == 0 && !is_returning)
		appendStringInfoString(buf, "NULL");
}

/*
 * Emit expressions specified in the given relation's reltarget.
 *
 * 发出给定关系的 reltarget 中指定的表达式。
 *
 * This is used for deparsing the given relation as a subquery.
 *
 * 这用于将给定关系解析为子查询。
 */
static void
deparseSubqueryTargetList(deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;
	RelOptInfo *foreignrel = context->foreignrel;
	bool		first;
	ListCell   *lc;

	/* Should only be called in these cases.
	 *
	 * 仅应在这些情况下调用。
	 */
	Assert(IS_SIMPLE_REL(foreignrel) || IS_JOIN_REL(foreignrel));

	first = true;
	foreach(lc, foreignrel->reltarget->exprs)
	{
		Node	   *node = (Node *) lfirst(lc);

		if (!first)
			appendStringInfoString(buf, ", ");
		first = false;

		deparseExpr((Expr *) node, context);
	}

	/* Don't generate bad syntax if no expressions
	 *
	 * 如果没有表达式，不要生成错误的语法
	 */
	if (first)
		appendStringInfoString(buf, "NULL");
}

/*
 * Construct FROM clause for given relation
 *
 * 为给定关系构造 FROM 子句
 *
 * The function constructs ... JOIN ... ON ... for join relation. For a base
 * relation it just returns schema-qualified tablename, with the appropriate
 * alias if so requested.
 *
 * 该函数构造... JOIN ... ON ...用于连接关系。对于基本关系，它只返回模式限定的表名，如果需要的话，还可以使用适当的别名。
 *
 * 'ignore_rel' is either zero or the RT index of a target relation.  In the
 * latter case the function constructs FROM clause of UPDATE or USING clause
 * of DELETE; it deparses the join relation as if the relation never contained
 * the target relation, and creates a List of conditions to be deparsed into
 * the top-level WHERE clause, which is returned to *ignore_conds.
 *
 * 'ignore_rel' 为零或目标关系的 RT 索引。  在后一种情况下，函数构造 UPDATE 的 FROM 子句或 DELETE 的 USING 子句；它解析连接关系，就好像该关系从未包含目标关系一样，并创建一个要解析到顶级 WHERE 子句的条件列表，该条件列表返回到 *ignore_conds。
 *
 * 'additional_conds' is a pointer to a list of strings to be appended to
 * the WHERE clause, coming from lower-level SEMI-JOINs.
 *
 * “additional_conds”是指向要附加到 WHERE 子句的字符串列表的指针，来自较低级别的 SEMI-JOIN。
 */
static void
deparseFromExprForRel(StringInfo buf, PlannerInfo *root, RelOptInfo *foreignrel,
					  bool use_alias, Index ignore_rel, List **ignore_conds,
					  List **additional_conds, List **params_list)
{
	PgFdwRelationInfo *fpinfo = (PgFdwRelationInfo *) foreignrel->fdw_private;

	if (IS_JOIN_REL(foreignrel))
	{
		StringInfoData join_sql_o;
		StringInfoData join_sql_i;
		RelOptInfo *outerrel = fpinfo->outerrel;
		RelOptInfo *innerrel = fpinfo->innerrel;
		bool		outerrel_is_target = false;
		bool		innerrel_is_target = false;
		List	   *additional_conds_i = NIL;
		List	   *additional_conds_o = NIL;

		if (ignore_rel > 0 && bms_is_member(ignore_rel, foreignrel->relids))
		{
			/*
			 * If this is an inner join, add joinclauses to *ignore_conds and
			 * set it to empty so that those can be deparsed into the WHERE
			 * clause.  Note that since the target relation can never be
			 * within the nullable side of an outer join, those could safely
			 * be pulled up into the WHERE clause (see foreign_join_ok()).
			 * Note also that since the target relation is only inner-joined
			 * to any other relation in the query, all conditions in the join
			 * tree mentioning the target relation could be deparsed into the
			 * WHERE clause by doing this recursively.
			 *
			 * 如果这是内部联接，请将 joinclauses 添加到 *ignore_conds 并将其设置为空，以便可以将它们解析到 WHERE 子句中。  请注意，由于目标关系永远不能位于外连接的可为空一侧，因此可以安全地将它们拉入 WHERE 子句（请参阅foreign_join_ok()）。另请注意，由于目标关系仅内连接到查询中的任何其他关系，因此可以通过递归执行此操作将连接树中提及目标关系的所有条件解析为 WHERE 子句。
			 */
			if (fpinfo->jointype == JOIN_INNER)
			{
				*ignore_conds = list_concat(*ignore_conds,
											fpinfo->joinclauses);
				fpinfo->joinclauses = NIL;
			}

			/*
			 * Check if either of the input relations is the target relation.
			 *
			 * 检查任一输入关系是否是目标关系。
			 */
			if (outerrel->relid == ignore_rel)
				outerrel_is_target = true;
			else if (innerrel->relid == ignore_rel)
				innerrel_is_target = true;
		}

		/* Deparse outer relation if not the target relation.
		 *
		 * 如果不是目标关系，则解析外部关系。
		 */
		if (!outerrel_is_target)
		{
			initStringInfo(&join_sql_o);
			deparseRangeTblRef(&join_sql_o, root, outerrel,
							   fpinfo->make_outerrel_subquery,
							   ignore_rel, ignore_conds, &additional_conds_o,
							   params_list);

			/*
			 * If inner relation is the target relation, skip deparsing it.
			 * Note that since the join of the target relation with any other
			 * relation in the query is an inner join and can never be within
			 * the nullable side of an outer join, the join could be
			 * interchanged with higher-level joins (cf. identity 1 on outer
			 * join reordering shown in src/backend/optimizer/README), which
			 * means it's safe to skip the target-relation deparsing here.
			 *
			 * 如果内部关系是目标关系，则跳过对其进行解析。请注意，由于目标关系与查询中任何其他关系的连接是内部连接，并且永远不能位于外部连接的可为空一侧，因此该连接可以与更高级别的连接互换（参见 src/backend/optimizer/README 中显示的外部连接重新排序的标识 1），这意味着可以安全地跳过此处的目标关系解析。
			 */
			if (innerrel_is_target)
			{
				Assert(fpinfo->jointype == JOIN_INNER);
				Assert(fpinfo->joinclauses == NIL);
				appendBinaryStringInfo(buf, join_sql_o.data, join_sql_o.len);
				/* Pass EXISTS conditions to upper level
				 *
				 * 将 EXISTS 条件传递给上层
				 */
				if (additional_conds_o != NIL)
				{
					Assert(*additional_conds == NIL);
					*additional_conds = additional_conds_o;
				}
				return;
			}
		}

		/* Deparse inner relation if not the target relation.
		 *
		 * 如果不是目标关系，则解析内部关系。
		 */
		if (!innerrel_is_target)
		{
			initStringInfo(&join_sql_i);
			deparseRangeTblRef(&join_sql_i, root, innerrel,
							   fpinfo->make_innerrel_subquery,
							   ignore_rel, ignore_conds, &additional_conds_i,
							   params_list);

			/*
			 * SEMI-JOIN is deparsed as the EXISTS subquery. It references
			 * outer and inner relations, so it should be evaluated as the
			 * condition in the upper-level WHERE clause. We deparse the
			 * condition and pass it to upper level callers as an
			 * additional_conds list. Upper level callers are responsible for
			 * inserting conditions from the list where appropriate.
			 *
			 * SEMI-JOIN 被解析为 EXISTS 子查询。它引用外部和内部关系，因此应将其计算为上层 WHERE 子句中的条件。我们解析条件并将其作为 extra_conds 列表传递给上层调用者。上层调用者负责在适当的情况下从列表中插入条件。
			 */
			if (fpinfo->jointype == JOIN_SEMI)
			{
				deparse_expr_cxt context;
				StringInfoData str;

				/* Construct deparsed condition from this SEMI-JOIN
				 *
				 * 从此 SEMI-JOIN 构造分离条件
				 */
				initStringInfo(&str);
				appendStringInfo(&str, "EXISTS (SELECT NULL FROM %s",
								 join_sql_i.data);

				context.buf = &str;
				context.foreignrel = foreignrel;
				context.scanrel = foreignrel;
				context.root = root;
				context.params_list = params_list;

				/*
				 * Append SEMI-JOIN clauses and EXISTS conditions from lower
				 * levels to the current EXISTS subquery
				 *
				 * 将较低级别的 SEMI-JOIN 子句和 EXISTS 条件附加到当前 EXISTS 子查询
				 */
				appendWhereClause(fpinfo->joinclauses, additional_conds_i, &context);

				/*
				 * EXISTS conditions, coming from lower join levels, have just
				 * been processed.
				 *
				 * 来自较低连接级别的 EXISTS 条件刚刚被处理。
				 */
				if (additional_conds_i != NIL)
				{
					list_free_deep(additional_conds_i);
					additional_conds_i = NIL;
				}

				/* Close parentheses for EXISTS subquery
				 *
				 * EXISTS 子查询的右括号
				 */
				appendStringInfoChar(&str, ')');

				*additional_conds = lappend(*additional_conds, str.data);
			}

			/*
			 * If outer relation is the target relation, skip deparsing it.
			 * See the above note about safety.
			 *
			 * 如果外部关系是目标关系，则跳过对其进行解析。请参阅上面有关安全的说明。
			 */
			if (outerrel_is_target)
			{
				Assert(fpinfo->jointype == JOIN_INNER);
				Assert(fpinfo->joinclauses == NIL);
				appendBinaryStringInfo(buf, join_sql_i.data, join_sql_i.len);
				/* Pass EXISTS conditions to the upper call
				 *
				 * 将 EXISTS 条件传递给上层调用
				 */
				if (additional_conds_i != NIL)
				{
					Assert(*additional_conds == NIL);
					*additional_conds = additional_conds_i;
				}
				return;
			}
		}

		/* Neither of the relations is the target relation.
		 *
		 * 这两个关系都不是目标关系。
		 */
		Assert(!outerrel_is_target && !innerrel_is_target);

		/*
		 * For semijoin FROM clause is deparsed as an outer relation. An inner
		 * relation and join clauses are converted to EXISTS condition and
		 * passed to the upper level.
		 *
		 * For semijoin FROM 子句被解析为外部关系。内部关系和连接子句被转换为 EXISTS 条件并传递到上层。
		 */
		if (fpinfo->jointype == JOIN_SEMI)
		{
			appendBinaryStringInfo(buf, join_sql_o.data, join_sql_o.len);
		}
		else
		{
			/*
			 * For a join relation FROM clause, entry is deparsed as
			 *
			 * 对于连接关系 FROM 子句，条目被解析为
			 *
			 * ((outer relation) <join type> (inner relation) ON
			 * (joinclauses))
			 *
			 * ((外关系) <连接类型> (内关系) ON (连接子句))
			 */
			appendStringInfo(buf, "(%s %s JOIN %s ON ", join_sql_o.data,
							 get_jointype_name(fpinfo->jointype), join_sql_i.data);

			/* Append join clause; (TRUE) if no join clause
			 *
			 * 追加连接子句； (TRUE) 如果没有连接子句
			 */
			if (fpinfo->joinclauses)
			{
				deparse_expr_cxt context;

				context.buf = buf;
				context.foreignrel = foreignrel;
				context.scanrel = foreignrel;
				context.root = root;
				context.params_list = params_list;

				appendStringInfoChar(buf, '(');
				appendConditions(fpinfo->joinclauses, &context);
				appendStringInfoChar(buf, ')');
			}
			else
				appendStringInfoString(buf, "(TRUE)");

			/* End the FROM clause entry.
			 *
			 * 结束 FROM 子句条目。
			 */
			appendStringInfoChar(buf, ')');
		}

		/*
		 * Construct additional_conds to be passed to the upper caller from
		 * current level additional_conds and additional_conds, coming from
		 * inner and outer rels.
		 *
		 * 构造要从当前级别的additional_conds 和来自内部和外部rels 的additional_conds 传递给上层调用者的additional_conds。
		 */
		if (additional_conds_o != NIL)
		{
			*additional_conds = list_concat(*additional_conds,
											additional_conds_o);
			list_free(additional_conds_o);
		}

		if (additional_conds_i != NIL)
		{
			*additional_conds = list_concat(*additional_conds,
											additional_conds_i);
			list_free(additional_conds_i);
		}
	}
	else
	{
		RangeTblEntry *rte = planner_rt_fetch(foreignrel->relid, root);

		/*
		 * Core code already has some lock on each rel being planned, so we
		 * can use NoLock here.
		 *
		 * 核心代码已经对正在规划的每个rel进行了一些锁定，因此我们可以在这里使用NoLock。
		 */
		Relation	rel = table_open(rte->relid, NoLock);

		deparseRelation(buf, rel);

		/*
		 * Add a unique alias to avoid any conflict in relation names due to
		 * pulled up subqueries in the query being built for a pushed down
		 * join.
		 *
		 * 添加唯一的别名，以避免由于为下推联接构建的查询中的上拉子查询而导致关系名称发生任何冲突。
		 */
		if (use_alias)
			appendStringInfo(buf, " %s%d", REL_ALIAS_PREFIX, foreignrel->relid);

		table_close(rel, NoLock);
	}
}

/*
 * Append FROM clause entry for the given relation into buf.
 * Conditions from lower-level SEMI-JOINs are appended to additional_conds
 * and should be added to upper level WHERE clause.
 *
 * 将给定关系的 FROM 子句条目追加到 buf 中。来自较低级别 SEMI-JOIN 的条件将附加到additional_conds，并且应添加到较高级别的 WHERE 子句。
 */
static void
deparseRangeTblRef(StringInfo buf, PlannerInfo *root, RelOptInfo *foreignrel,
				   bool make_subquery, Index ignore_rel, List **ignore_conds,
				   List **additional_conds, List **params_list)
{
	PgFdwRelationInfo *fpinfo = (PgFdwRelationInfo *) foreignrel->fdw_private;

	/* Should only be called in these cases.
	 *
	 * 仅应在这些情况下调用。
	 */
	Assert(IS_SIMPLE_REL(foreignrel) || IS_JOIN_REL(foreignrel));

	Assert(fpinfo->local_conds == NIL);

	/* If make_subquery is true, deparse the relation as a subquery.
	 *
	 * 如果 make_subquery 为 true，则将关系解析为子查询。
	 */
	if (make_subquery)
	{
		List	   *retrieved_attrs;
		int			ncols;

		/*
		 * The given relation shouldn't contain the target relation, because
		 * this should only happen for input relations for a full join, and
		 * such relations can never contain an UPDATE/DELETE target.
		 *
		 * 给定的关系不应包含目标关系，因为这只应发生在完全联接的输入关系中，并且此类关系永远不能包含 UPDATE/DELETE 目标。
		 */
		Assert(ignore_rel == 0 ||
			   !bms_is_member(ignore_rel, foreignrel->relids));

		/* Deparse the subquery representing the relation.
		 *
		 * 解析表示关系的子查询。
		 */
		appendStringInfoChar(buf, '(');
		deparseSelectStmtForRel(buf, root, foreignrel, NIL,
								fpinfo->remote_conds, NIL,
								false, false, true,
								&retrieved_attrs, params_list);
		appendStringInfoChar(buf, ')');

		/* Append the relation alias.
		 *
		 * 附加关系别名。
		 */
		appendStringInfo(buf, " %s%d", SUBQUERY_REL_ALIAS_PREFIX,
						 fpinfo->relation_index);

		/*
		 * Append the column aliases if needed.  Note that the subquery emits
		 * expressions specified in the relation's reltarget (see
		 * deparseSubqueryTargetList).
		 *
		 * 如果需要，请附加列别名。  请注意，子查询发出关系的 reltarget 中指定的表达式（请参阅 deparseSubqueryTargetList）。
		 */
		ncols = list_length(foreignrel->reltarget->exprs);
		if (ncols > 0)
		{
			int			i;

			appendStringInfoChar(buf, '(');
			for (i = 1; i <= ncols; i++)
			{
				if (i > 1)
					appendStringInfoString(buf, ", ");

				appendStringInfo(buf, "%s%d", SUBQUERY_COL_ALIAS_PREFIX, i);
			}
			appendStringInfoChar(buf, ')');
		}
	}
	else
		deparseFromExprForRel(buf, root, foreignrel, true, ignore_rel,
							  ignore_conds, additional_conds,
							  params_list);
}

/*
 * deparse remote INSERT statement
 *
 * 解析远程 INSERT 语句
 *
 * The statement text is appended to buf, and we also create an integer List
 * of the columns being retrieved by WITH CHECK OPTION or RETURNING (if any),
 * which is returned to *retrieved_attrs.
 *
 * 语句文本被追加到 buf 中，我们还创建一个由WITH CHECK OPTION 或 RETURNING（如果有）检索的列的整数列表，该列表返回到 *retrieved_attrs。
 *
 * This also stores end position of the VALUES clause, so that we can rebuild
 * an INSERT for a batch of rows later.
 *
 * 它还存储 VALUES 子句的结束位置，以便我们稍后可以为一批行重建 INSERT。
 */
void
deparseInsertSql(StringInfo buf, RangeTblEntry *rte,
				 Index rtindex, Relation rel,
				 List *targetAttrs, bool doNothing,
				 List *withCheckOptionList, List *returningList,
				 List **retrieved_attrs, int *values_end_len)
{
	TupleDesc	tupdesc = RelationGetDescr(rel);
	AttrNumber	pindex;
	bool		first;
	ListCell   *lc;

	appendStringInfoString(buf, "INSERT INTO ");
	deparseRelation(buf, rel);

	if (targetAttrs)
	{
		appendStringInfoChar(buf, '(');

		first = true;
		foreach(lc, targetAttrs)
		{
			int			attnum = lfirst_int(lc);

			if (!first)
				appendStringInfoString(buf, ", ");
			first = false;

			deparseColumnRef(buf, rtindex, attnum, rte, false);
		}

		appendStringInfoString(buf, ") VALUES (");

		pindex = 1;
		first = true;
		foreach(lc, targetAttrs)
		{
			int			attnum = lfirst_int(lc);
			Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);

			if (!first)
				appendStringInfoString(buf, ", ");
			first = false;

			if (attr->attgenerated)
				appendStringInfoString(buf, "DEFAULT");
			else
			{
				appendStringInfo(buf, "$%d", pindex);
				pindex++;
			}
		}

		appendStringInfoChar(buf, ')');
	}
	else
		appendStringInfoString(buf, " DEFAULT VALUES");
	*values_end_len = buf->len;

	if (doNothing)
		appendStringInfoString(buf, " ON CONFLICT DO NOTHING");

	deparseReturningList(buf, rte, rtindex, rel,
						 rel->trigdesc && rel->trigdesc->trig_insert_after_row,
						 withCheckOptionList, returningList, retrieved_attrs);
}

/*
 * rebuild remote INSERT statement
 *
 * 重建远程 INSERT 语句
 *
 * Provided a number of rows in a batch, builds INSERT statement with the
 * right number of parameters.
 *
 * 批量提供多行，使用正确数量的参数构建 INSERT 语句。
 */
void
rebuildInsertSql(StringInfo buf, Relation rel,
				 char *orig_query, List *target_attrs,
				 int values_end_len, int num_params,
				 int num_rows)
{
	TupleDesc	tupdesc = RelationGetDescr(rel);
	int			i;
	int			pindex;
	bool		first;
	ListCell   *lc;

	/* Make sure the values_end_len is sensible
	 *
	 * 确保values_end_len合理
	 */
	Assert((values_end_len > 0) && (values_end_len <= strlen(orig_query)));

	/* Copy up to the end of the first record from the original query
	 *
	 * 从原始查询中复制到第一条记录的末尾
	 */
	appendBinaryStringInfo(buf, orig_query, values_end_len);

	/*
	 * Add records to VALUES clause (we already have parameters for the first
	 * row, so start at the right offset).
	 *
	 * 将记录添加到 VALUES 子句（我们已经有第一行的参数，因此从正确的偏移量开始）。
	 */
	pindex = num_params + 1;
	for (i = 0; i < num_rows; i++)
	{
		appendStringInfoString(buf, ", (");

		first = true;
		foreach(lc, target_attrs)
		{
			int			attnum = lfirst_int(lc);
			Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);

			if (!first)
				appendStringInfoString(buf, ", ");
			first = false;

			if (attr->attgenerated)
				appendStringInfoString(buf, "DEFAULT");
			else
			{
				appendStringInfo(buf, "$%d", pindex);
				pindex++;
			}
		}

		appendStringInfoChar(buf, ')');
	}

	/* Copy stuff after VALUES clause from the original query
	 *
	 * 从原始查询中复制 VALUES 子句后的内容
	 */
	appendStringInfoString(buf, orig_query + values_end_len);
}

/*
 * deparse remote UPDATE statement
 *
 * 解析远程 UPDATE 语句
 *
 * The statement text is appended to buf, and we also create an integer List
 * of the columns being retrieved by WITH CHECK OPTION or RETURNING (if any),
 * which is returned to *retrieved_attrs.
 *
 * 语句文本被追加到 buf 中，我们还创建一个由WITH CHECK OPTION 或 RETURNING（如果有）检索的列的整数列表，该列表返回到 *retrieved_attrs。
 */
void
deparseUpdateSql(StringInfo buf, RangeTblEntry *rte,
				 Index rtindex, Relation rel,
				 List *targetAttrs,
				 List *withCheckOptionList, List *returningList,
				 List **retrieved_attrs)
{
	TupleDesc	tupdesc = RelationGetDescr(rel);
	AttrNumber	pindex;
	bool		first;
	ListCell   *lc;

	appendStringInfoString(buf, "UPDATE ");
	deparseRelation(buf, rel);
	appendStringInfoString(buf, " SET ");

	pindex = 2;					/* ctid is always the first param */
	first = true;
	foreach(lc, targetAttrs)
	{
		int			attnum = lfirst_int(lc);
		Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);

		if (!first)
			appendStringInfoString(buf, ", ");
		first = false;

		deparseColumnRef(buf, rtindex, attnum, rte, false);
		if (attr->attgenerated)
			appendStringInfoString(buf, " = DEFAULT");
		else
		{
			appendStringInfo(buf, " = $%d", pindex);
			pindex++;
		}
	}
	appendStringInfoString(buf, " WHERE ctid = $1");

	deparseReturningList(buf, rte, rtindex, rel,
						 rel->trigdesc && rel->trigdesc->trig_update_after_row,
						 withCheckOptionList, returningList, retrieved_attrs);
}

/*
 * deparse remote UPDATE statement
 *
 * 解析远程 UPDATE 语句
 *
 * 'buf' is the output buffer to append the statement to
 * 'rtindex' is the RT index of the associated target relation
 * 'rel' is the relation descriptor for the target relation
 * 'foreignrel' is the RelOptInfo for the target relation or the join relation
 *		containing all base relations in the query
 * 'targetlist' is the tlist of the underlying foreign-scan plan node
 *		(note that this only contains new-value expressions and junk attrs)
 * 'targetAttrs' is the target columns of the UPDATE
 * 'remote_conds' is the qual clauses that must be evaluated remotely
 * '*params_list' is an output list of exprs that will become remote Params
 * 'returningList' is the RETURNING targetlist
 * '*retrieved_attrs' is an output list of integers of columns being retrieved
 *		by RETURNING (if any)
 *
 * 'buf' 是将语句附加到的输出缓冲区 'rtindex' 是关联目标关系的 RT 索引 'rel' 是目标关系的关系描述符 'foreignrel' 是目标关系的 RelOptInfo 或包含查询中所有基本关系的连接关系 'targetlist' 是底层外部扫描计划节点的 tlist（注意，这仅包含新值表达式和垃圾属性） 'targetAttrs' 是目标关系的目标列UPDATE 'remote_conds' 是必须远程评估的 qual 子句 '*params_list' 是将成为远程参数的 exprs 的输出列表 'returningList' 是 RETURNING 目标列表 '*retrieved_attrs' 是通过 RETURNING 检索的列整数的输出列表（如果有）
 */
void
deparseDirectUpdateSql(StringInfo buf, PlannerInfo *root,
					   Index rtindex, Relation rel,
					   RelOptInfo *foreignrel,
					   List *targetlist,
					   List *targetAttrs,
					   List *remote_conds,
					   List **params_list,
					   List *returningList,
					   List **retrieved_attrs)
{
	deparse_expr_cxt context;
	int			nestlevel;
	bool		first;
	RangeTblEntry *rte = planner_rt_fetch(rtindex, root);
	ListCell   *lc,
			   *lc2;
	List	   *additional_conds = NIL;

	/* Set up context struct for recursion
	 *
	 * 设置递归的上下文结构
	 */
	context.root = root;
	context.foreignrel = foreignrel;
	context.scanrel = foreignrel;
	context.buf = buf;
	context.params_list = params_list;

	appendStringInfoString(buf, "UPDATE ");
	deparseRelation(buf, rel);
	if (foreignrel->reloptkind == RELOPT_JOINREL)
		appendStringInfo(buf, " %s%d", REL_ALIAS_PREFIX, rtindex);
	appendStringInfoString(buf, " SET ");

	/* Make sure any constants in the exprs are printed portably
	 *
	 * 确保表达式中的任何常量都可移植地打印
	 */
	nestlevel = set_transmission_modes();

	first = true;
	forboth(lc, targetlist, lc2, targetAttrs)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);
		int			attnum = lfirst_int(lc2);

		/* update's new-value expressions shouldn't be resjunk
		 *
		 * update 的新值表达式不应该被 resjunk
		 */
		Assert(!tle->resjunk);

		if (!first)
			appendStringInfoString(buf, ", ");
		first = false;

		deparseColumnRef(buf, rtindex, attnum, rte, false);
		appendStringInfoString(buf, " = ");
		deparseExpr((Expr *) tle->expr, &context);
	}

	reset_transmission_modes(nestlevel);

	if (foreignrel->reloptkind == RELOPT_JOINREL)
	{
		List	   *ignore_conds = NIL;


		appendStringInfoString(buf, " FROM ");
		deparseFromExprForRel(buf, root, foreignrel, true, rtindex,
							  &ignore_conds, &additional_conds, params_list);
		remote_conds = list_concat(remote_conds, ignore_conds);
	}

	appendWhereClause(remote_conds, additional_conds, &context);

	if (additional_conds != NIL)
		list_free_deep(additional_conds);

	if (foreignrel->reloptkind == RELOPT_JOINREL)
		deparseExplicitTargetList(returningList, true, retrieved_attrs,
								  &context);
	else
		deparseReturningList(buf, rte, rtindex, rel, false,
							 NIL, returningList, retrieved_attrs);
}

/*
 * deparse remote DELETE statement
 *
 * 解析远程 DELETE 语句
 *
 * The statement text is appended to buf, and we also create an integer List
 * of the columns being retrieved by RETURNING (if any), which is returned
 * to *retrieved_attrs.
 *
 * 语句文本被追加到 buf 中，我们还创建一个通过 RETURNING 检索的列的整数列表（如果有），该列表返回到 *retrieved_attrs。
 */
void
deparseDeleteSql(StringInfo buf, RangeTblEntry *rte,
				 Index rtindex, Relation rel,
				 List *returningList,
				 List **retrieved_attrs)
{
	appendStringInfoString(buf, "DELETE FROM ");
	deparseRelation(buf, rel);
	appendStringInfoString(buf, " WHERE ctid = $1");

	deparseReturningList(buf, rte, rtindex, rel,
						 rel->trigdesc && rel->trigdesc->trig_delete_after_row,
						 NIL, returningList, retrieved_attrs);
}

/*
 * deparse remote DELETE statement
 *
 * 解析远程 DELETE 语句
 *
 * 'buf' is the output buffer to append the statement to
 * 'rtindex' is the RT index of the associated target relation
 * 'rel' is the relation descriptor for the target relation
 * 'foreignrel' is the RelOptInfo for the target relation or the join relation
 *		containing all base relations in the query
 * 'remote_conds' is the qual clauses that must be evaluated remotely
 * '*params_list' is an output list of exprs that will become remote Params
 * 'returningList' is the RETURNING targetlist
 * '*retrieved_attrs' is an output list of integers of columns being retrieved
 *		by RETURNING (if any)
 *
 * 'buf' 是将语句附加到的输出缓冲区 'rtindex' 是关联目标关系的 RT 索引 'rel' 是目标关系的关系描述符 'foreignrel' 是目标关系的 RelOptInfo 或包含查询中所有基本关系的连接关系 'remote_conds' 是必须远程评估的 qual 子句 '*params_list' 是将成为远程参数的表达式的输出列表 'returningList' 是RETURNING targetlist '*retrieved_attrs' 是 RETURNING 检索的列的整数的输出列表（如果有）
 */
void
deparseDirectDeleteSql(StringInfo buf, PlannerInfo *root,
					   Index rtindex, Relation rel,
					   RelOptInfo *foreignrel,
					   List *remote_conds,
					   List **params_list,
					   List *returningList,
					   List **retrieved_attrs)
{
	deparse_expr_cxt context;
	List	   *additional_conds = NIL;

	/* Set up context struct for recursion
	 *
	 * 设置递归的上下文结构
	 */
	context.root = root;
	context.foreignrel = foreignrel;
	context.scanrel = foreignrel;
	context.buf = buf;
	context.params_list = params_list;

	appendStringInfoString(buf, "DELETE FROM ");
	deparseRelation(buf, rel);
	if (foreignrel->reloptkind == RELOPT_JOINREL)
		appendStringInfo(buf, " %s%d", REL_ALIAS_PREFIX, rtindex);

	if (foreignrel->reloptkind == RELOPT_JOINREL)
	{
		List	   *ignore_conds = NIL;

		appendStringInfoString(buf, " USING ");
		deparseFromExprForRel(buf, root, foreignrel, true, rtindex,
							  &ignore_conds, &additional_conds, params_list);
		remote_conds = list_concat(remote_conds, ignore_conds);
	}

	appendWhereClause(remote_conds, additional_conds, &context);

	if (additional_conds != NIL)
		list_free_deep(additional_conds);

	if (foreignrel->reloptkind == RELOPT_JOINREL)
		deparseExplicitTargetList(returningList, true, retrieved_attrs,
								  &context);
	else
		deparseReturningList(buf, planner_rt_fetch(rtindex, root),
							 rtindex, rel, false,
							 NIL, returningList, retrieved_attrs);
}

/*
 * Add a RETURNING clause, if needed, to an INSERT/UPDATE/DELETE.
 *
 * 如果需要，向 INSERT/UPDATE/DELETE 添加 RETURNING 子句。
 */
static void
deparseReturningList(StringInfo buf, RangeTblEntry *rte,
					 Index rtindex, Relation rel,
					 bool trig_after_row,
					 List *withCheckOptionList,
					 List *returningList,
					 List **retrieved_attrs)
{
	Bitmapset  *attrs_used = NULL;

	if (trig_after_row)
	{
		/* whole-row reference acquires all non-system columns
		 *
		 * 整行引用获取所有非系统列
		 */
		attrs_used =
			bms_make_singleton(0 - FirstLowInvalidHeapAttributeNumber);
	}

	if (withCheckOptionList != NIL)
	{
		/*
		 * We need the attrs, non-system and system, mentioned in the local
		 * query's WITH CHECK OPTION list.
		 *
		 * 我们需要本地查询的WITH CHECK OPTION 列表中提到的属性（非系统属性和系统属性）。
		 *
		 * Note: we do this to ensure that WCO constraints will be evaluated
		 * on the data actually inserted/updated on the remote side, which
		 * might differ from the data supplied by the core code, for example
		 * as a result of remote triggers.
		 *
		 * 注意：我们这样做是为了确保将在远程端实际插入/更新的数据上评估 WCO 约束，这可能与核心代码提供的数据不同，例如由于远程触发器的结果。
		 */
		pull_varattnos((Node *) withCheckOptionList, rtindex,
					   &attrs_used);
	}

	if (returningList != NIL)
	{
		/*
		 * We need the attrs, non-system and system, mentioned in the local
		 * query's RETURNING list.
		 *
		 * 我们需要本地查询的返回列表中提到的属性，非系统和系统。
		 */
		pull_varattnos((Node *) returningList, rtindex,
					   &attrs_used);
	}

	if (attrs_used != NULL)
		deparseTargetList(buf, rte, rtindex, rel, true, attrs_used, false,
						  retrieved_attrs);
	else
		*retrieved_attrs = NIL;
}

/*
 * Construct SELECT statement to acquire size in blocks of given relation.
 *
 * 构造 SELECT 语句以获取给定关系的块中的大小。
 *
 * Note: we use local definition of block size, not remote definition.
 * This is perhaps debatable.
 *
 * 注意：我们使用本地定义块大小，而不是远程定义。这也许是值得商榷的。
 *
 * Note: pg_relation_size() exists in 8.1 and later.
 *
 * 注意： pg_relation_size() 存在于 8.1 及更高版本中。
 */
void
deparseAnalyzeSizeSql(StringInfo buf, Relation rel)
{
	StringInfoData relname;

	/* We'll need the remote relation name as a literal.
	 *
	 * 我们需要远程关系名称作为文字。
	 */
	initStringInfo(&relname);
	deparseRelation(&relname, rel);

	appendStringInfoString(buf, "SELECT pg_catalog.pg_relation_size(");
	deparseStringLiteral(buf, relname.data);
	appendStringInfo(buf, "::pg_catalog.regclass) / %d", BLCKSZ);
}

/*
 * Construct SELECT statement to acquire the number of rows and the relkind of
 * a relation.
 *
 * 构造 SELECT 语句来获取关系的行数和relkind。
 *
 * Note: we just return the remote server's reltuples value, which might
 * be off a good deal, but it doesn't seem worth working harder.  See
 * comments in postgresAcquireSampleRowsFunc.
 *
 * 注意：我们只是返回远程服务器的 reltuples 值，这可能很划算，但似乎不值得更加努力。  请参阅 postgresAcquireSampleRowsFunc 中的注释。
 */
void
deparseAnalyzeInfoSql(StringInfo buf, Relation rel)
{
	StringInfoData relname;

	/* We'll need the remote relation name as a literal.
	 *
	 * 我们需要远程关系名称作为文字。
	 */
	initStringInfo(&relname);
	deparseRelation(&relname, rel);

	appendStringInfoString(buf, "SELECT reltuples, relkind FROM pg_catalog.pg_class WHERE oid = ");
	deparseStringLiteral(buf, relname.data);
	appendStringInfoString(buf, "::pg_catalog.regclass");
}

/*
 * Construct SELECT statement to acquire sample rows of given relation.
 *
 * 构造 SELECT 语句来获取给定关系的样本行。
 *
 * SELECT command is appended to buf, and list of columns retrieved
 * is returned to *retrieved_attrs.
 *
 * SELECT 命令附加到 buf，检索到的列列表返回到 *retrieved_attrs。
 *
 * We only support sampling methods we can decide based on server version.
 * Allowing custom TSM modules (like tsm_system_rows) might be useful, but it
 * would require detecting which extensions are installed, to allow automatic
 * fall-back. Moreover, the methods may use different parameters like number
 * of rows (and not sampling rate). So we leave this for future improvements.
 *
 * 我们只支持根据服务器版本决定的采样方法。允许自定义 TSM 模块（如 tsm_system_rows）可能很有用，但需要检测安装了哪些扩展，以允许自动回退。此外，这些方法可以使用不同的参数，例如行数（而不是采样率）。因此，我们将其留待将来改进。
 *
 * Using random() to sample rows on the remote server has the advantage that
 * this works on all PostgreSQL versions (unlike TABLESAMPLE), and that it
 * does the sampling on the remote side (without transferring everything and
 * then discarding most rows).
 *
 * 使用 random() 对远程服务器上的行进行采样的优点是，它适用于所有 PostgreSQL 版本（与 TABLESAMPLE 不同），并且它在远程端进行采样（无需传输所有内容，然后丢弃大多数行）。
 *
 * The disadvantage is that we still have to read all rows and evaluate the
 * random(), while TABLESAMPLE (at least with the "system" method) may skip.
 * It's not that different from the "bernoulli" method, though.
 *
 * 缺点是我们仍然必须读取所有行并评估 random()，而 TABLESAMPLE（至少使用“系统”方法）可能会跳过。不过，它与“伯努利”方法并没有太大不同。
 *
 * We could also do "ORDER BY random() LIMIT x", which would always pick
 * the expected number of rows, but it requires sorting so it may be much
 * more expensive (particularly on large tables, which is what the
 * remote sampling is meant to improve).
 *
 * 我们还可以执行“ORDER BY random() LIMIT x”，它总是选择预期的行数，但它需要排序，因此可能会更昂贵（特别是在大型表上，这就是远程采样要改进的）。
 */
void
deparseAnalyzeSql(StringInfo buf, Relation rel,
				  PgFdwSamplingMethod sample_method, double sample_frac,
				  List **retrieved_attrs)
{
	Oid			relid = RelationGetRelid(rel);
	TupleDesc	tupdesc = RelationGetDescr(rel);
	int			i;
	char	   *colname;
	List	   *options;
	ListCell   *lc;
	bool		first = true;

	*retrieved_attrs = NIL;

	appendStringInfoString(buf, "SELECT ");
	for (i = 0; i < tupdesc->natts; i++)
	{
		/* Ignore dropped columns.
		 *
		 * 忽略删除的列。
		 */
		if (TupleDescAttr(tupdesc, i)->attisdropped)
			continue;

		if (!first)
			appendStringInfoString(buf, ", ");
		first = false;

		/* Use attribute name or column_name option.
		 *
		 * 使用属性名称或column_name选项。
		 */
		colname = NameStr(TupleDescAttr(tupdesc, i)->attname);
		options = GetForeignColumnOptions(relid, i + 1);

		foreach(lc, options)
		{
			DefElem    *def = (DefElem *) lfirst(lc);

			if (strcmp(def->defname, "column_name") == 0)
			{
				colname = defGetString(def);
				break;
			}
		}

		appendStringInfoString(buf, quote_identifier(colname));

		*retrieved_attrs = lappend_int(*retrieved_attrs, i + 1);
	}

	/* Don't generate bad syntax for zero-column relation.
	 *
	 * 不要为零列关系生成错误的语法。
	 */
	if (first)
		appendStringInfoString(buf, "NULL");

	/*
	 * Construct FROM clause, and perhaps WHERE clause too, depending on the
	 * selected sampling method.
	 *
	 * 构造 FROM 子句，也许还构造 WHERE 子句，具体取决于所选的采样方法。
	 */
	appendStringInfoString(buf, " FROM ");
	deparseRelation(buf, rel);

	switch (sample_method)
	{
		case ANALYZE_SAMPLE_OFF:
			/* nothing to do here
			 *
			 * 这里没什么可做的
			 */
			break;

		case ANALYZE_SAMPLE_RANDOM:
			appendStringInfo(buf, " WHERE pg_catalog.random() < %f", sample_frac);
			break;

		case ANALYZE_SAMPLE_SYSTEM:
			appendStringInfo(buf, " TABLESAMPLE SYSTEM(%f)", (100.0 * sample_frac));
			break;

		case ANALYZE_SAMPLE_BERNOULLI:
			appendStringInfo(buf, " TABLESAMPLE BERNOULLI(%f)", (100.0 * sample_frac));
			break;

		case ANALYZE_SAMPLE_AUTO:
			/* should have been resolved into actual method
			 *
			 * 应该已经解析为实际方法
			 */
			elog(ERROR, "unexpected sampling method");
			break;
	}
}

/*
 * Construct a simple "TRUNCATE rel" statement
 *
 * 构造一个简单的“TRUNCATE rel”语句
 */
void
deparseTruncateSql(StringInfo buf,
				   List *rels,
				   DropBehavior behavior,
				   bool restart_seqs)
{
	ListCell   *cell;

	appendStringInfoString(buf, "TRUNCATE ");

	foreach(cell, rels)
	{
		Relation	rel = lfirst(cell);

		if (cell != list_head(rels))
			appendStringInfoString(buf, ", ");

		deparseRelation(buf, rel);
	}

	appendStringInfo(buf, " %s IDENTITY",
					 restart_seqs ? "RESTART" : "CONTINUE");

	if (behavior == DROP_RESTRICT)
		appendStringInfoString(buf, " RESTRICT");
	else if (behavior == DROP_CASCADE)
		appendStringInfoString(buf, " CASCADE");
}

/*
 * Construct name to use for given column, and emit it into buf.
 * If it has a column_name FDW option, use that instead of attribute name.
 *
 * 构造用于给定列的名称，并将其发送到 buf 中。如果它具有 column_name FDW 选项，请使用该选项而不是属性名称。
 *
 * If qualify_col is true, qualify column name with the alias of relation.
 *
 * 如果qualify_col为true，则使用关系的别名限定列名。
 */
static void
deparseColumnRef(StringInfo buf, int varno, int varattno, RangeTblEntry *rte,
				 bool qualify_col)
{
	/* We support fetching the remote side's CTID and OID.
	 *
	 * 我们支持获取远端的CTID和OID。
	 */
	if (varattno == SelfItemPointerAttributeNumber)
	{
		if (qualify_col)
			ADD_REL_QUALIFIER(buf, varno);
		appendStringInfoString(buf, "ctid");
	}
	else if (varattno < 0)
	{
		/*
		 * All other system attributes are fetched as 0, except for table OID,
		 * which is fetched as the local table OID.  However, we must be
		 * careful; the table could be beneath an outer join, in which case it
		 * must go to NULL whenever the rest of the row does.
		 *
		 * 所有其他系统属性均获取为 0，但表 OID 除外，它作为本地表 OID 获取。  然而，我们必须小心；该表可能位于外连接之下，在这种情况下，每当行的其余部分变为 NULL 时，该表都必须变为 NULL。
		 */
		Oid			fetchval = 0;

		if (varattno == TableOidAttributeNumber)
			fetchval = rte->relid;

		if (qualify_col)
		{
			appendStringInfoString(buf, "CASE WHEN (");
			ADD_REL_QUALIFIER(buf, varno);
			appendStringInfo(buf, "*)::text IS NOT NULL THEN %u END", fetchval);
		}
		else
			appendStringInfo(buf, "%u", fetchval);
	}
	else if (varattno == 0)
	{
		/* Whole row reference
		 *
		 * 整行参考
		 */
		Relation	rel;
		Bitmapset  *attrs_used;

		/* Required only to be passed down to deparseTargetList().
		 *
		 * 仅需要传递给 deparseTargetList()。
		 */
		List	   *retrieved_attrs;

		/*
		 * The lock on the relation will be held by upper callers, so it's
		 * fine to open it with no lock here.
		 *
		 * 关系上的锁将由上层调用者持有，因此可以在此处不加锁地打开它。
		 */
		rel = table_open(rte->relid, NoLock);

		/*
		 * The local name of the foreign table can not be recognized by the
		 * foreign server and the table it references on foreign server might
		 * have different column ordering or different columns than those
		 * declared locally. Hence we have to deparse whole-row reference as
		 * ROW(columns referenced locally). Construct this by deparsing a
		 * "whole row" attribute.
		 *
		 * 外部服务器无法识别外部表的本地名称，并且它在外部服务器上引用的表可能具有与本地声明的列顺序或列不同的列顺序或列。因此，我们必须将整行引用解析为 ROW（本地引用的列）。通过解析“整行”属性来构造它。
		 */
		attrs_used = bms_add_member(NULL,
									0 - FirstLowInvalidHeapAttributeNumber);

		/*
		 * In case the whole-row reference is under an outer join then it has
		 * to go NULL whenever the rest of the row goes NULL. Deparsing a join
		 * query would always involve multiple relations, thus qualify_col
		 * would be true.
		 *
		 * 如果整行引用位于外连接下，则只要该行的其余部分变为 NULL，它就必须变为 NULL。解析连接查询总是涉及多个关系，因此 Qualify_col 将为 true。
		 */
		if (qualify_col)
		{
			appendStringInfoString(buf, "CASE WHEN (");
			ADD_REL_QUALIFIER(buf, varno);
			appendStringInfoString(buf, "*)::text IS NOT NULL THEN ");
		}

		appendStringInfoString(buf, "ROW(");
		deparseTargetList(buf, rte, varno, rel, false, attrs_used, qualify_col,
						  &retrieved_attrs);
		appendStringInfoChar(buf, ')');

		/* Complete the CASE WHEN statement started above.
		 *
		 * 完成上面开始的 CASE WHEN 语句。
		 */
		if (qualify_col)
			appendStringInfoString(buf, " END");

		table_close(rel, NoLock);
		bms_free(attrs_used);
	}
	else
	{
		char	   *colname = NULL;
		List	   *options;
		ListCell   *lc;

		/* varno must not be any of OUTER_VAR, INNER_VAR and INDEX_VAR.
		 *
		 * varno 不能是 OUTER_VAR、INNER_VAR 和 INDEX_VAR 中的任何一个。
		 */
		Assert(!IS_SPECIAL_VARNO(varno));

		/*
		 * If it's a column of a foreign table, and it has the column_name FDW
		 * option, use that value.
		 *
		 * 如果它是外部表的列，并且具有 column_name FDW 选项，请使用该值。
		 */
		options = GetForeignColumnOptions(rte->relid, varattno);
		foreach(lc, options)
		{
			DefElem    *def = (DefElem *) lfirst(lc);

			if (strcmp(def->defname, "column_name") == 0)
			{
				colname = defGetString(def);
				break;
			}
		}

		/*
		 * If it's a column of a regular table or it doesn't have column_name
		 * FDW option, use attribute name.
		 *
		 * 如果它是常规表的列或者没有column_name FDW选项，则使用属性名称。
		 */
		if (colname == NULL)
			colname = get_attname(rte->relid, varattno, false);

		if (qualify_col)
			ADD_REL_QUALIFIER(buf, varno);

		appendStringInfoString(buf, quote_identifier(colname));
	}
}

/*
 * Append remote name of specified foreign table to buf.
 * Use value of table_name FDW option (if any) instead of relation's name.
 * Similarly, schema_name FDW option overrides schema name.
 *
 * 将指定外部表的远程名称追加到 buf。使用 table_name FDW 选项（如果有）的值代替关系的名称。同样，schema_name FDW 选项会覆盖模式名称。
 */
static void
deparseRelation(StringInfo buf, Relation rel)
{
	ForeignTable *table;
	const char *nspname = NULL;
	const char *relname = NULL;
	ListCell   *lc;

	/* obtain additional catalog information.
	 *
	 * 获取附加目录信息。
	 */
	table = GetForeignTable(RelationGetRelid(rel));

	/*
	 * Use value of FDW options if any, instead of the name of object itself.
	 *
	 * 使用 FDW 选项的值（如果有），而不是对象本身的名称。
	 */
	foreach(lc, table->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "schema_name") == 0)
			nspname = defGetString(def);
		else if (strcmp(def->defname, "table_name") == 0)
			relname = defGetString(def);
	}

	/*
	 * Note: we could skip printing the schema name if it's pg_catalog, but
	 * that doesn't seem worth the trouble.
	 *
	 * 注意：如果模式名称是 pg_catalog，我们可以跳过打印模式名称，但这似乎不值得这么麻烦。
	 */
	if (nspname == NULL)
		nspname = get_namespace_name(RelationGetNamespace(rel));
	if (relname == NULL)
		relname = RelationGetRelationName(rel);

	appendStringInfo(buf, "%s.%s",
					 quote_identifier(nspname), quote_identifier(relname));
}

/*
 * Append a SQL string literal representing "val" to buf.
 *
 * 将表示“val”的 SQL 字符串文字附加到 buf。
 */
void
deparseStringLiteral(StringInfo buf, const char *val)
{
	const char *valptr;

	/*
	 * Rather than making assumptions about the remote server's value of
	 * standard_conforming_strings, always use E'foo' syntax if there are any
	 * backslashes.  This will fail on remote servers before 8.1, but those
	 * are long out of support.
	 *
	 * 如果存在任何反斜杠，请始终使用 E'foo' 语法，而不是对远程服务器的 standard_conforming_strings 值进行假设。  这在 8.1 之前的远程服务器上会失败，但这些服务器早已不再支持。
	 */
	if (strchr(val, '\\') != NULL)
		appendStringInfoChar(buf, ESCAPE_STRING_SYNTAX);
	appendStringInfoChar(buf, '\'');
	for (valptr = val; *valptr; valptr++)
	{
		char		ch = *valptr;

		if (SQL_STR_DOUBLE(ch, true))
			appendStringInfoChar(buf, ch);
		appendStringInfoChar(buf, ch);
	}
	appendStringInfoChar(buf, '\'');
}

/*
 * Deparse given expression into context->buf.
 *
 * 将给定表达式解析到 context->buf 中。
 *
 * This function must support all the same node types that foreign_expr_walker
 * accepts.
 *
 * 此函数必须支持foreign_expr_walker 接受的所有相同节点类型。
 *
 * Note: unlike ruleutils.c, we just use a simple hard-wired parenthesization
 * scheme: anything more complex than a Var, Const, function call or cast
 * should be self-parenthesized.
 *
 * 注意：与ruleutils.c不同，我们只使用一个简单的硬连线括号方案：任何比Var、Const、函数调用或转换更复杂的东西都应该是自括号的。
 */
static void
deparseExpr(Expr *node, deparse_expr_cxt *context)
{
	if (node == NULL)
		return;

	switch (nodeTag(node))
	{
		case T_Var:
			deparseVar((Var *) node, context);
			break;
		case T_Const:
			deparseConst((Const *) node, context, 0);
			break;
		case T_Param:
			deparseParam((Param *) node, context);
			break;
		case T_SubscriptingRef:
			deparseSubscriptingRef((SubscriptingRef *) node, context);
			break;
		case T_FuncExpr:
			deparseFuncExpr((FuncExpr *) node, context);
			break;
		case T_OpExpr:
			deparseOpExpr((OpExpr *) node, context);
			break;
		case T_DistinctExpr:
			deparseDistinctExpr((DistinctExpr *) node, context);
			break;
		case T_ScalarArrayOpExpr:
			deparseScalarArrayOpExpr((ScalarArrayOpExpr *) node, context);
			break;
		case T_RelabelType:
			deparseRelabelType((RelabelType *) node, context);
			break;
		case T_BoolExpr:
			deparseBoolExpr((BoolExpr *) node, context);
			break;
		case T_NullTest:
			deparseNullTest((NullTest *) node, context);
			break;
		case T_CaseExpr:
			deparseCaseExpr((CaseExpr *) node, context);
			break;
		case T_ArrayExpr:
			deparseArrayExpr((ArrayExpr *) node, context);
			break;
		case T_Aggref:
			deparseAggref((Aggref *) node, context);
			break;
		default:
			elog(ERROR, "unsupported expression type for deparse: %d",
				 (int) nodeTag(node));
			break;
	}
}

/*
 * Deparse given Var node into context->buf.
 *
 * 将给定的 Var 节点解析到 context->buf 中。
 *
 * If the Var belongs to the foreign relation, just print its remote name.
 * Otherwise, it's effectively a Param (and will in fact be a Param at
 * run time).  Handle it the same way we handle plain Params --- see
 * deparseParam for comments.
 *
 * 如果 Var 属于外关系，则仅打印其远程名称。否则，它实际上是一个 Param（实际上在运行时也是一个 Param）。  以与处理普通参数相同的方式处理它 --- 请参阅 deparseParam 获取注释。
 */
static void
deparseVar(Var *node, deparse_expr_cxt *context)
{
	Relids		relids = context->scanrel->relids;
	int			relno;
	int			colno;

	/* Qualify columns when multiple relations are involved.
	 *
	 * 当涉及多个关系时限定列。
	 */
	bool		qualify_col = (bms_membership(relids) == BMS_MULTIPLE);

	/*
	 * If the Var belongs to the foreign relation that is deparsed as a
	 * subquery, use the relation and column alias to the Var provided by the
	 * subquery, instead of the remote name.
	 *
	 * 如果 Var 属于作为子查询解析的外关系，请使用子查询提供的 Var 的关系和列别名，而不是远程名称。
	 */
	if (is_subquery_var(node, context->scanrel, &relno, &colno))
	{
		appendStringInfo(context->buf, "%s%d.%s%d",
						 SUBQUERY_REL_ALIAS_PREFIX, relno,
						 SUBQUERY_COL_ALIAS_PREFIX, colno);
		return;
	}

	if (bms_is_member(node->varno, relids) && node->varlevelsup == 0)
		deparseColumnRef(context->buf, node->varno, node->varattno,
						 planner_rt_fetch(node->varno, context->root),
						 qualify_col);
	else
	{
		/* Treat like a Param
		 *
		 * 像帕拉姆一样对待
		 */
		if (context->params_list)
		{
			int			pindex = 0;
			ListCell   *lc;

			/* find its index in params_list
			 *
			 * 在 params_list 中找到它的索引
			 */
			foreach(lc, *context->params_list)
			{
				pindex++;
				if (equal(node, (Node *) lfirst(lc)))
					break;
			}
			if (lc == NULL)
			{
				/* not in list, so add it
				 *
				 * 不在列表中，所以添加它
				 */
				pindex++;
				*context->params_list = lappend(*context->params_list, node);
			}

			printRemoteParam(pindex, node->vartype, node->vartypmod, context);
		}
		else
		{
			printRemotePlaceholder(node->vartype, node->vartypmod, context);
		}
	}
}

/*
 * Deparse given constant value into context->buf.
 *
 * 将给定的常量值解析到 context->buf 中。
 *
 * This function has to be kept in sync with ruleutils.c's get_const_expr.
 *
 * 该函数必须与ruleutils.c 的get_const_expr 保持同步。
 *
 * As in that function, showtype can be -1 to never show "::typename"
 * decoration, +1 to always show it, or 0 to show it only if the constant
 * wouldn't be assumed to be the right type by default.
 *
 * 与该函数一样，showtype 可以为 -1 表示从不显示“::typename”装饰，+1 表示始终显示它，或者为 0 表示仅当默认情况下常量不被假定为正确类型时才显示它。
 *
 * In addition, this code allows showtype to be -2 to indicate that we should
 * not show "::typename" decoration if the constant is printed as an untyped
 * literal or NULL (while in other cases, behaving as for showtype == 0).
 *
 * 此外，此代码允许 showtype 为 -2，以指示如果常量打印为无类型文本或 NULL，则我们不应显示“::typename”修饰（而在其他情况下，行为与 showtype == 0 相同）。
 */
static void
deparseConst(Const *node, deparse_expr_cxt *context, int showtype)
{
	StringInfo	buf = context->buf;
	Oid			typoutput;
	bool		typIsVarlena;
	char	   *extval;
	bool		isfloat = false;
	bool		isstring = false;
	bool		needlabel;

	if (node->constisnull)
	{
		appendStringInfoString(buf, "NULL");
		if (showtype >= 0)
			appendStringInfo(buf, "::%s",
							 deparse_type_name(node->consttype,
											   node->consttypmod));
		return;
	}

	getTypeOutputInfo(node->consttype,
					  &typoutput, &typIsVarlena);
	extval = OidOutputFunctionCall(typoutput, node->constvalue);

	switch (node->consttype)
	{
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case OIDOID:
		case FLOAT4OID:
		case FLOAT8OID:
		case NUMERICOID:
			{
				/*
				 * No need to quote unless it's a special value such as 'NaN'.
				 * See comments in get_const_expr().
				 *
				 * 不需要引用，除非它是特殊值，例如“NaN”。请参阅 get_const_expr() 中的注释。
				 */
				if (strspn(extval, "0123456789+-eE.") == strlen(extval))
				{
					if (extval[0] == '+' || extval[0] == '-')
						appendStringInfo(buf, "(%s)", extval);
					else
						appendStringInfoString(buf, extval);
					if (strcspn(extval, "eE.") != strlen(extval))
						isfloat = true; /* it looks like a float */
				}
				else
					appendStringInfo(buf, "'%s'", extval);
			}
			break;
		case BITOID:
		case VARBITOID:
			appendStringInfo(buf, "B'%s'", extval);
			break;
		case BOOLOID:
			if (strcmp(extval, "t") == 0)
				appendStringInfoString(buf, "true");
			else
				appendStringInfoString(buf, "false");
			break;
		default:
			deparseStringLiteral(buf, extval);
			isstring = true;
			break;
	}

	pfree(extval);

	if (showtype == -1)
		return;					/* never print type label */

	/*
	 * For showtype == 0, append ::typename unless the constant will be
	 * implicitly typed as the right type when it is read in.
	 *
	 * 对于 showtype == 0，请附加 ::typename，除非常量在读入时将隐式键入为正确的类型。
	 *
	 * XXX this code has to be kept in sync with the behavior of the parser,
	 * especially make_const.
	 *
	 * XXX 此代码必须与解析器的行为保持同步，尤其是 make_const。
	 */
	switch (node->consttype)
	{
		case BOOLOID:
		case INT4OID:
		case UNKNOWNOID:
			needlabel = false;
			break;
		case NUMERICOID:
			needlabel = !isfloat || (node->consttypmod >= 0);
			break;
		default:
			if (showtype == -2)
			{
				/* label unless we printed it as an untyped string
				 *
				 * 标签，除非我们将其打印为无类型字符串
				 */
				needlabel = !isstring;
			}
			else
				needlabel = true;
			break;
	}
	if (needlabel || showtype > 0)
		appendStringInfo(buf, "::%s",
						 deparse_type_name(node->consttype,
										   node->consttypmod));
}

/*
 * Deparse given Param node.
 *
 * 解析给定的 Param 节点。
 *
 * If we're generating the query "for real", add the Param to
 * context->params_list if it's not already present, and then use its index
 * in that list as the remote parameter number.  During EXPLAIN, there's
 * no need to identify a parameter number.
 *
 * 如果我们“真正”生成查询，则将 Param 添加到 context->params_list （如果尚不存在），然后使用其在该列表中的索引作为远程参数编号。  在 EXPLAIN 期间，无需识别参数编号。
 */
static void
deparseParam(Param *node, deparse_expr_cxt *context)
{
	if (context->params_list)
	{
		int			pindex = 0;
		ListCell   *lc;

		/* find its index in params_list
		 *
		 * 在 params_list 中找到它的索引
		 */
		foreach(lc, *context->params_list)
		{
			pindex++;
			if (equal(node, (Node *) lfirst(lc)))
				break;
		}
		if (lc == NULL)
		{
			/* not in list, so add it
			 *
			 * 不在列表中，所以添加它
			 */
			pindex++;
			*context->params_list = lappend(*context->params_list, node);
		}

		printRemoteParam(pindex, node->paramtype, node->paramtypmod, context);
	}
	else
	{
		printRemotePlaceholder(node->paramtype, node->paramtypmod, context);
	}
}

/*
 * Deparse a container subscript expression.
 *
 * 解析容器下标表达式。
 */
static void
deparseSubscriptingRef(SubscriptingRef *node, deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;
	ListCell   *lowlist_item;
	ListCell   *uplist_item;

	/* Always parenthesize the expression.
	 *
	 * 始终将表达式括起来。
	 */
	appendStringInfoChar(buf, '(');

	/*
	 * Deparse referenced array expression first.  If that expression includes
	 * a cast, we have to parenthesize to prevent the array subscript from
	 * being taken as typename decoration.  We can avoid that in the typical
	 * case of subscripting a Var, but otherwise do it.
	 *
	 * 首先解析引用的数组表达式。  如果该表达式包含强制转换，我们必须添加括号以防止数组下标被视为类型名装饰。  在为 Var 下标的典型情况下，我们可以避免这种情况，但除此之外也可以这样做。
	 */
	if (IsA(node->refexpr, Var))
		deparseExpr(node->refexpr, context);
	else
	{
		appendStringInfoChar(buf, '(');
		deparseExpr(node->refexpr, context);
		appendStringInfoChar(buf, ')');
	}

	/* Deparse subscript expressions.
	 *
	 * 解析下标表达式。
	 */
	lowlist_item = list_head(node->reflowerindexpr);	/* could be NULL */
	foreach(uplist_item, node->refupperindexpr)
	{
		appendStringInfoChar(buf, '[');
		if (lowlist_item)
		{
			deparseExpr(lfirst(lowlist_item), context);
			appendStringInfoChar(buf, ':');
			lowlist_item = lnext(node->reflowerindexpr, lowlist_item);
		}
		deparseExpr(lfirst(uplist_item), context);
		appendStringInfoChar(buf, ']');
	}

	appendStringInfoChar(buf, ')');
}

/*
 * Deparse a function call.
 *
 * 解析函数调用。
 */
static void
deparseFuncExpr(FuncExpr *node, deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;
	bool		use_variadic;
	bool		first;
	ListCell   *arg;

	/*
	 * If the function call came from an implicit coercion, then just show the
	 * first argument.
	 *
	 * 如果函数调用来自隐式强制，则仅显示第一个参数。
	 */
	if (node->funcformat == COERCE_IMPLICIT_CAST)
	{
		deparseExpr((Expr *) linitial(node->args), context);
		return;
	}

	/*
	 * If the function call came from a cast, then show the first argument
	 * plus an explicit cast operation.
	 *
	 * 如果函数调用来自强制转换，则显示第一个参数加上显式强制转换操作。
	 */
	if (node->funcformat == COERCE_EXPLICIT_CAST)
	{
		Oid			rettype = node->funcresulttype;
		int32		coercedTypmod;

		/* Get the typmod if this is a length-coercion function
		 *
		 * 如果这是一个长度强制函数，则获取typmod
		 */
		(void) exprIsLengthCoercion((Node *) node, &coercedTypmod);

		deparseExpr((Expr *) linitial(node->args), context);
		appendStringInfo(buf, "::%s",
						 deparse_type_name(rettype, coercedTypmod));
		return;
	}

	/* Check if need to print VARIADIC (cf. ruleutils.c)
	 *
	 * 检查是否需要打印 VARIADIC（参见ruleutils.c）
	 */
	use_variadic = node->funcvariadic;

	/*
	 * Normal function: display as proname(args).
	 *
	 * 正常功能：显示为 proname(args)。
	 */
	appendFunctionName(node->funcid, context);
	appendStringInfoChar(buf, '(');

	/* ... and all the arguments
	 *
	 * ...以及所有的论点
	 */
	first = true;
	foreach(arg, node->args)
	{
		if (!first)
			appendStringInfoString(buf, ", ");
		if (use_variadic && lnext(node->args, arg) == NULL)
			appendStringInfoString(buf, "VARIADIC ");
		deparseExpr((Expr *) lfirst(arg), context);
		first = false;
	}
	appendStringInfoChar(buf, ')');
}

/*
 * Deparse given operator expression.   To avoid problems around
 * priority of operations, we always parenthesize the arguments.
 *
 * 解析给定的运算符表达式。   为了避免操作优先级出现问题，我们总是将参数括起来。
 */
static void
deparseOpExpr(OpExpr *node, deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;
	HeapTuple	tuple;
	Form_pg_operator form;
	Expr	   *right;
	bool		canSuppressRightConstCast = false;
	char		oprkind;

	/* Retrieve information about the operator from system catalog.
	 *
	 * 从系统目录中检索有关操作员的信息。
	 */
	tuple = SearchSysCache1(OPEROID, ObjectIdGetDatum(node->opno));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for operator %u", node->opno);
	form = (Form_pg_operator) GETSTRUCT(tuple);
	oprkind = form->oprkind;

	/* Sanity check.
	 *
	 * 健全性检查。
	 */
	Assert((oprkind == 'l' && list_length(node->args) == 1) ||
		   (oprkind == 'b' && list_length(node->args) == 2));

	right = llast(node->args);

	/* Always parenthesize the expression.
	 *
	 * 始终将表达式括起来。
	 */
	appendStringInfoChar(buf, '(');

	/* Deparse left operand, if any.
	 *
	 * 解析左操作数（如果有）。
	 */
	if (oprkind == 'b')
	{
		Expr	   *left = linitial(node->args);
		Oid			leftType = exprType((Node *) left);
		Oid			rightType = exprType((Node *) right);
		bool		canSuppressLeftConstCast = false;

		/*
		 * When considering a binary operator, if one operand is a Const that
		 * can be printed as a bare string literal or NULL (i.e., it will look
		 * like type UNKNOWN to the remote parser), the Const normally
		 * receives an explicit cast to the operator's input type.  However,
		 * in Const-to-Var comparisons where both operands are of the same
		 * type, we prefer to suppress the explicit cast, leaving the Const's
		 * type resolution up to the remote parser.  The remote's resolution
		 * heuristic will assume that an unknown input type being compared to
		 * a known input type is of that known type as well.
		 *
		 * 当考虑二元运算符时，如果一个操作数是一个可以打印为裸字符串文字或 NULL 的 Const（即，对于远程解析器来说，它看起来像是 UNKNOWN 类型），则 Const 通常会收到对运算符输入类型的显式转换。  然而，在 Const 与 Var 的比较中，两个操作数的类型相同，我们更喜欢抑制显式强制转换，将 Const 的类型解析留给远程解析器。  遥控器的分辨率启发式将假设与已知输入类型进行比较的未知输入类型也属于该已知类型。
		 *
		 * This hack allows some cases to succeed where a remote column is
		 * declared with a different type in the local (foreign) table.  By
		 * emitting "foreigncol = 'foo'" not "foreigncol = 'foo'::text" or the
		 * like, we allow the remote parser to pick an "=" operator that's
		 * compatible with whatever type the remote column really is, such as
		 * an enum.
		 *
		 * 此技巧允许在某些情况下成功，其中在本地（外部）表中使用不同类型声明远程列。  通过发出“foreigncol = 'foo'”而不是“foreigncol = 'foo'::text”等，我们允许远程解析器选择与远程列实际类型（例如枚举）兼容的“=”运算符。
		 *
		 * We allow cast suppression to happen only when the other operand is
		 * a plain foreign Var.  Although the remote's unknown-type heuristic
		 * would apply to other cases just as well, we would be taking a
		 * bigger risk that the inferred type is something unexpected.  With
		 * this restriction, if anything goes wrong it's the user's fault for
		 * not declaring the local column with the same type as the remote
		 * column.
		 *
		 * 仅当另一个操作数是普通的外来 Var 时，我们才允许发生强制转换。  尽管远程的未知类型启发式也适用于其他情况，但我们将冒更大的风险，因为推断的类型是意外的。  有了这个限制，如果出现任何问题，都是用户的错误，因为没有声明与远程列具有相同类型的本地列。
		 */
		if (leftType == rightType)
		{
			if (IsA(left, Const))
				canSuppressLeftConstCast = isPlainForeignVar(right, context);
			else if (IsA(right, Const))
				canSuppressRightConstCast = isPlainForeignVar(left, context);
		}

		if (canSuppressLeftConstCast)
			deparseConst((Const *) left, context, -2);
		else
			deparseExpr(left, context);

		appendStringInfoChar(buf, ' ');
	}

	/* Deparse operator name.
	 *
	 * 解析操作员名称。
	 */
	deparseOperatorName(buf, form);

	/* Deparse right operand.
	 *
	 * 分离右操作数。
	 */
	appendStringInfoChar(buf, ' ');

	if (canSuppressRightConstCast)
		deparseConst((Const *) right, context, -2);
	else
		deparseExpr(right, context);

	appendStringInfoChar(buf, ')');

	ReleaseSysCache(tuple);
}

/*
 * Will "node" deparse as a plain foreign Var?
 *
 * “node”会解析为普通的外来 Var 吗？
 */
static bool
isPlainForeignVar(Expr *node, deparse_expr_cxt *context)
{
	/*
	 * We allow the foreign Var to have an implicit RelabelType, mainly so
	 * that this'll work with varchar columns.  Note that deparseRelabelType
	 * will not print such a cast, so we're not breaking the restriction that
	 * the expression print as a plain Var.  We won't risk it for an implicit
	 * cast that requires a function, nor for non-implicit RelabelType; such
	 * cases seem too likely to involve semantics changes compared to what
	 * would happen on the remote side.
	 *
	 * 我们允许外部 Var 有一个隐式的 RelabelType，主要是为了它可以与 varchar 列一起使用。  请注意，deparseRelabelType 不会打印这样的转换，因此我们不会打破表达式打印为普通 Var 的限制。  我们不会冒险进行需要函数的隐式转换，也不会冒险进行非隐式 RelabelType；与远程端发生的情况相比，这种情况似乎太可能涉及语义更改。
	 */
	if (IsA(node, RelabelType) &&
		((RelabelType *) node)->relabelformat == COERCE_IMPLICIT_CAST)
		node = ((RelabelType *) node)->arg;

	if (IsA(node, Var))
	{
		/*
		 * The Var must be one that'll deparse as a foreign column reference
		 * (cf. deparseVar).
		 *
		 * Var 必须是一个将作为外部列引用进行解析的变量（参见 deparseVar）。
		 */
		Var		   *var = (Var *) node;
		Relids		relids = context->scanrel->relids;

		if (bms_is_member(var->varno, relids) && var->varlevelsup == 0)
			return true;
	}

	return false;
}

/*
 * Print the name of an operator.
 *
 * 打印操作员的姓名。
 */
static void
deparseOperatorName(StringInfo buf, Form_pg_operator opform)
{
	char	   *opname;

	/* opname is not a SQL identifier, so we should not quote it.
	 *
	 * opname 不是 SQL 标识符，因此我们不应该引用它。
	 */
	opname = NameStr(opform->oprname);

	/* Print schema name only if it's not pg_catalog
	 *
	 * 仅当模式名称不是 pg_catalog 时才打印模式名称
	 */
	if (opform->oprnamespace != PG_CATALOG_NAMESPACE)
	{
		const char *opnspname;

		opnspname = get_namespace_name(opform->oprnamespace);
		/* Print fully qualified operator name.
		 *
		 * 打印完全限定的操作员名称。
		 */
		appendStringInfo(buf, "OPERATOR(%s.%s)",
						 quote_identifier(opnspname), opname);
	}
	else
	{
		/* Just print operator name.
		 *
		 * 只需打印操作员姓名即可。
		 */
		appendStringInfoString(buf, opname);
	}
}

/*
 * Deparse IS DISTINCT FROM.
 *
 * Deparse 不同于。
 */
static void
deparseDistinctExpr(DistinctExpr *node, deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;

	Assert(list_length(node->args) == 2);

	appendStringInfoChar(buf, '(');
	deparseExpr(linitial(node->args), context);
	appendStringInfoString(buf, " IS DISTINCT FROM ");
	deparseExpr(lsecond(node->args), context);
	appendStringInfoChar(buf, ')');
}

/*
 * Deparse given ScalarArrayOpExpr expression.  To avoid problems
 * around priority of operations, we always parenthesize the arguments.
 *
 * 解析给定的 ScalarArrayOpExpr 表达式。  为了避免操作优先级出现问题，我们总是将参数括起来。
 */
static void
deparseScalarArrayOpExpr(ScalarArrayOpExpr *node, deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;
	HeapTuple	tuple;
	Form_pg_operator form;
	Expr	   *arg1;
	Expr	   *arg2;

	/* Retrieve information about the operator from system catalog.
	 *
	 * 从系统目录中检索有关操作员的信息。
	 */
	tuple = SearchSysCache1(OPEROID, ObjectIdGetDatum(node->opno));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for operator %u", node->opno);
	form = (Form_pg_operator) GETSTRUCT(tuple);

	/* Sanity check.
	 *
	 * 健全性检查。
	 */
	Assert(list_length(node->args) == 2);

	/* Always parenthesize the expression.
	 *
	 * 始终将表达式括起来。
	 */
	appendStringInfoChar(buf, '(');

	/* Deparse left operand.
	 *
	 * 解析左操作数。
	 */
	arg1 = linitial(node->args);
	deparseExpr(arg1, context);
	appendStringInfoChar(buf, ' ');

	/* Deparse operator name plus decoration.
	 *
	 * 解析运算符名称加修饰。
	 */
	deparseOperatorName(buf, form);
	appendStringInfo(buf, " %s (", node->useOr ? "ANY" : "ALL");

	/* Deparse right operand.
	 *
	 * 分离右操作数。
	 */
	arg2 = lsecond(node->args);
	deparseExpr(arg2, context);

	appendStringInfoChar(buf, ')');

	/* Always parenthesize the expression.
	 *
	 * 始终将表达式括起来。
	 */
	appendStringInfoChar(buf, ')');

	ReleaseSysCache(tuple);
}

/*
 * Deparse a RelabelType (binary-compatible cast) node.
 *
 * 解析 RelabelType（二进制兼容转换）节点。
 */
static void
deparseRelabelType(RelabelType *node, deparse_expr_cxt *context)
{
	deparseExpr(node->arg, context);
	if (node->relabelformat != COERCE_IMPLICIT_CAST)
		appendStringInfo(context->buf, "::%s",
						 deparse_type_name(node->resulttype,
										   node->resulttypmod));
}

/*
 * Deparse a BoolExpr node.
 *
 * 解析 BoolExpr 节点。
 */
static void
deparseBoolExpr(BoolExpr *node, deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;
	const char *op = NULL;		/* keep compiler quiet */
	bool		first;
	ListCell   *lc;

	switch (node->boolop)
	{
		case AND_EXPR:
			op = "AND";
			break;
		case OR_EXPR:
			op = "OR";
			break;
		case NOT_EXPR:
			appendStringInfoString(buf, "(NOT ");
			deparseExpr(linitial(node->args), context);
			appendStringInfoChar(buf, ')');
			return;
	}

	appendStringInfoChar(buf, '(');
	first = true;
	foreach(lc, node->args)
	{
		if (!first)
			appendStringInfo(buf, " %s ", op);
		deparseExpr((Expr *) lfirst(lc), context);
		first = false;
	}
	appendStringInfoChar(buf, ')');
}

/*
 * Deparse IS [NOT] NULL expression.
 *
 * 解析 IS [NOT] NULL 表达式。
 */
static void
deparseNullTest(NullTest *node, deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;

	appendStringInfoChar(buf, '(');
	deparseExpr(node->arg, context);

	/*
	 * For scalar inputs, we prefer to print as IS [NOT] NULL, which is
	 * shorter and traditional.  If it's a rowtype input but we're applying a
	 * scalar test, must print IS [NOT] DISTINCT FROM NULL to be semantically
	 * correct.
	 *
	 * 对于标量输入，我们更喜欢打印为 IS [NOT] NULL，这更短且传统。  如果它是行类型输入，但我们正在应用标量测试，则必须打印 IS [NOT] DISTINCT FROM NULL 才能在语义上正确。
	 */
	if (node->argisrow || !type_is_rowtype(exprType((Node *) node->arg)))
	{
		if (node->nulltesttype == IS_NULL)
			appendStringInfoString(buf, " IS NULL)");
		else
			appendStringInfoString(buf, " IS NOT NULL)");
	}
	else
	{
		if (node->nulltesttype == IS_NULL)
			appendStringInfoString(buf, " IS NOT DISTINCT FROM NULL)");
		else
			appendStringInfoString(buf, " IS DISTINCT FROM NULL)");
	}
}

/*
 * Deparse CASE expression
 *
 * 解析 CASE 表达式
 */
static void
deparseCaseExpr(CaseExpr *node, deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;
	ListCell   *lc;

	appendStringInfoString(buf, "(CASE");

	/* If this is a CASE arg WHEN then emit the arg expression
	 *
	 * 如果这是一个 CASE arg WHEN 则发出 arg 表达式
	 */
	if (node->arg != NULL)
	{
		appendStringInfoChar(buf, ' ');
		deparseExpr(node->arg, context);
	}

	/* Add each condition/result of the CASE clause
	 *
	 * 添加 CASE 子句的每个条件/结果
	 */
	foreach(lc, node->args)
	{
		CaseWhen   *whenclause = (CaseWhen *) lfirst(lc);

		/* WHEN */
		appendStringInfoString(buf, " WHEN ");
		if (node->arg == NULL)	/* CASE WHEN */
			deparseExpr(whenclause->expr, context);
		else					/* CASE arg WHEN */
		{
			/* Ignore the CaseTestExpr and equality operator.
			 *
			 * 忽略 CaseTestExpr 和相等运算符。
			 */
			deparseExpr(lsecond(castNode(OpExpr, whenclause->expr)->args),
						context);
		}

		/* THEN */
		appendStringInfoString(buf, " THEN ");
		deparseExpr(whenclause->result, context);
	}

	/* add ELSE if present
	 *
	 * 添加 ELSE（如果存在）
	 */
	if (node->defresult != NULL)
	{
		appendStringInfoString(buf, " ELSE ");
		deparseExpr(node->defresult, context);
	}

	/* append END
	 *
	 * 追加结束
	 */
	appendStringInfoString(buf, " END)");
}

/*
 * Deparse ARRAY[...] construct.
 *
 * 解析 ARRAY[...] 构造。
 */
static void
deparseArrayExpr(ArrayExpr *node, deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;
	bool		first = true;
	ListCell   *lc;

	appendStringInfoString(buf, "ARRAY[");
	foreach(lc, node->elements)
	{
		if (!first)
			appendStringInfoString(buf, ", ");
		deparseExpr(lfirst(lc), context);
		first = false;
	}
	appendStringInfoChar(buf, ']');

	/* If the array is empty, we need an explicit cast to the array type.
	 *
	 * 如果数组为空，我们需要显式转换为数组类型。
	 */
	if (node->elements == NIL)
		appendStringInfo(buf, "::%s",
						 deparse_type_name(node->array_typeid, -1));
}

/*
 * Deparse an Aggref node.
 *
 * 解析 Aggref 节点。
 */
static void
deparseAggref(Aggref *node, deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;
	bool		use_variadic;

	/* Only basic, non-split aggregation accepted.
	 *
	 * 只接受基本的、非分割的聚合。
	 */
	Assert(node->aggsplit == AGGSPLIT_SIMPLE);

	/* Check if need to print VARIADIC (cf. ruleutils.c)
	 *
	 * 检查是否需要打印 VARIADIC（参见ruleutils.c）
	 */
	use_variadic = node->aggvariadic;

	/* Find aggregate name from aggfnoid which is a pg_proc entry
	 *
	 * 从 aggfnoid 中查找聚合名称，这是一个 pg_proc 条目
	 */
	appendFunctionName(node->aggfnoid, context);
	appendStringInfoChar(buf, '(');

	/* Add DISTINCT
	 *
	 * 添加不同的
	 */
	appendStringInfoString(buf, (node->aggdistinct != NIL) ? "DISTINCT " : "");

	if (AGGKIND_IS_ORDERED_SET(node->aggkind))
	{
		/* Add WITHIN GROUP (ORDER BY ..)
		 *
		 * 在组内添加（按 .. 排序）
		 */
		ListCell   *arg;
		bool		first = true;

		Assert(!node->aggvariadic);
		Assert(node->aggorder != NIL);

		foreach(arg, node->aggdirectargs)
		{
			if (!first)
				appendStringInfoString(buf, ", ");
			first = false;

			deparseExpr((Expr *) lfirst(arg), context);
		}

		appendStringInfoString(buf, ") WITHIN GROUP (ORDER BY ");
		appendAggOrderBy(node->aggorder, node->args, context);
	}
	else
	{
		/* aggstar can be set only in zero-argument aggregates
		 *
		 * aggstar 只能在零参数聚合中设置
		 */
		if (node->aggstar)
			appendStringInfoChar(buf, '*');
		else
		{
			ListCell   *arg;
			bool		first = true;

			/* Add all the arguments
			 *
			 * 添加所有参数
			 */
			foreach(arg, node->args)
			{
				TargetEntry *tle = (TargetEntry *) lfirst(arg);
				Node	   *n = (Node *) tle->expr;

				if (tle->resjunk)
					continue;

				if (!first)
					appendStringInfoString(buf, ", ");
				first = false;

				/* Add VARIADIC
				 *
				 * 添加 VARIADIC
				 */
				if (use_variadic && lnext(node->args, arg) == NULL)
					appendStringInfoString(buf, "VARIADIC ");

				deparseExpr((Expr *) n, context);
			}
		}

		/* Add ORDER BY
		 *
		 * 添加排序依据
		 */
		if (node->aggorder != NIL)
		{
			appendStringInfoString(buf, " ORDER BY ");
			appendAggOrderBy(node->aggorder, node->args, context);
		}
	}

	/* Add FILTER (WHERE ..)
	 *
	 * 添加过滤器（其中..）
	 */
	if (node->aggfilter != NULL)
	{
		appendStringInfoString(buf, ") FILTER (WHERE ");
		deparseExpr((Expr *) node->aggfilter, context);
	}

	appendStringInfoChar(buf, ')');
}

/*
 * Append ORDER BY within aggregate function.
 *
 * 在聚合函数中附加 ORDER BY。
 */
static void
appendAggOrderBy(List *orderList, List *targetList, deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;
	ListCell   *lc;
	bool		first = true;

	foreach(lc, orderList)
	{
		SortGroupClause *srt = (SortGroupClause *) lfirst(lc);
		Node	   *sortexpr;

		if (!first)
			appendStringInfoString(buf, ", ");
		first = false;

		/* Deparse the sort expression proper.
		 *
		 * 正确解析排序表达式。
		 */
		sortexpr = deparseSortGroupClause(srt->tleSortGroupRef, targetList,
										  false, context);
		/* Add decoration as needed.
		 *
		 * 根据需要添加装饰。
		 */
		appendOrderBySuffix(srt->sortop, exprType(sortexpr), srt->nulls_first,
							context);
	}
}

/*
 * Append the ASC, DESC, USING <OPERATOR> and NULLS FIRST / NULLS LAST parts
 * of an ORDER BY clause.
 *
 * 追加 ORDER BY 子句的 ASC、DESC、USING <OPERATOR> 和 NULLS FIRST / NULLS LAST 部分。
 */
static void
appendOrderBySuffix(Oid sortop, Oid sortcoltype, bool nulls_first,
					deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;
	TypeCacheEntry *typentry;

	/* See whether operator is default < or > for sort expr's datatype.
	 *
	 * 查看排序 expr 数据类型的默认运算符是否为 < 或 >。
	 */
	typentry = lookup_type_cache(sortcoltype,
								 TYPECACHE_LT_OPR | TYPECACHE_GT_OPR);

	if (sortop == typentry->lt_opr)
		appendStringInfoString(buf, " ASC");
	else if (sortop == typentry->gt_opr)
		appendStringInfoString(buf, " DESC");
	else
	{
		HeapTuple	opertup;
		Form_pg_operator operform;

		appendStringInfoString(buf, " USING ");

		/* Append operator name.
		 *
		 * 附加操作员名称。
		 */
		opertup = SearchSysCache1(OPEROID, ObjectIdGetDatum(sortop));
		if (!HeapTupleIsValid(opertup))
			elog(ERROR, "cache lookup failed for operator %u", sortop);
		operform = (Form_pg_operator) GETSTRUCT(opertup);
		deparseOperatorName(buf, operform);
		ReleaseSysCache(opertup);
	}

	if (nulls_first)
		appendStringInfoString(buf, " NULLS FIRST");
	else
		appendStringInfoString(buf, " NULLS LAST");
}

/*
 * Print the representation of a parameter to be sent to the remote side.
 *
 * 打印要发送到远程端的参数的表示形式。
 *
 * Note: we always label the Param's type explicitly rather than relying on
 * transmitting a numeric type OID in PQsendQueryParams().  This allows us to
 * avoid assuming that types have the same OIDs on the remote side as they
 * do locally --- they need only have the same names.
 *
 * 注意：我们总是显式地标记 Param 的类型，而不是依赖于在 PQsendQueryParams() 中传输数字类型的 OID。  这使我们能够避免假设类型在远程​​端具有与本地相同的 OID —— 它们只需要具有相同的名称即可。
 */
static void
printRemoteParam(int paramindex, Oid paramtype, int32 paramtypmod,
				 deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;
	char	   *ptypename = deparse_type_name(paramtype, paramtypmod);

	appendStringInfo(buf, "$%d::%s", paramindex, ptypename);
}

/*
 * Print the representation of a placeholder for a parameter that will be
 * sent to the remote side at execution time.
 *
 * 打印将在执行时发送到远程端的参数的占位符表示。
 *
 * This is used when we're just trying to EXPLAIN the remote query.
 * We don't have the actual value of the runtime parameter yet, and we don't
 * want the remote planner to generate a plan that depends on such a value
 * anyway.  Thus, we can't do something simple like "$1::paramtype".
 * Instead, we emit "((SELECT null::paramtype)::paramtype)".
 * In all extant versions of Postgres, the planner will see that as an unknown
 * constant value, which is what we want.  This might need adjustment if we
 * ever make the planner flatten scalar subqueries.  Note: the reason for the
 * apparently useless outer cast is to ensure that the representation as a
 * whole will be parsed as an a_expr and not a select_with_parens; the latter
 * would do the wrong thing in the context "x = ANY(...)".
 *
 * 当我们只是试图解释远程查询时使用它。我们还没有运行时参数的实际值，而且我们也不希望远程规划器生成依赖于该值的计划。  因此，我们不能做像“$1::paramtype”这样简单的事情。相反，我们发出“((SELECT null::paramtype)::paramtype)”。在 Postgres 的所有现有版本中，规划器会将其视为未知的常量值，这正是我们想要的。  如果我们让规划器展平标量子查询，这可能需要调整。  注意：显然无用的外部转换的原因是为了确保整个表示将被解析为 a_expr 而不是 select_with_parens；后者会在“x = ANY(...)”上下文中做错误的事情。
 */
static void
printRemotePlaceholder(Oid paramtype, int32 paramtypmod,
					   deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;
	char	   *ptypename = deparse_type_name(paramtype, paramtypmod);

	appendStringInfo(buf, "((SELECT null::%s)::%s)", ptypename, ptypename);
}

/*
 * Deparse GROUP BY clause.
 *
 * 解析 GROUP BY 子句。
 */
static void
appendGroupByClause(List *tlist, deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;
	Query	   *query = context->root->parse;
	ListCell   *lc;
	bool		first = true;

	/* Nothing to be done, if there's no GROUP BY clause in the query.
	 *
	 * 如果查询中没有 GROUP BY 子句，则无需执行任何操作。
	 */
	if (!query->groupClause)
		return;

	appendStringInfoString(buf, " GROUP BY ");

	/*
	 * Queries with grouping sets are not pushed down, so we don't expect
	 * grouping sets here.
	 *
	 * 具有分组集的查询不会被下推，因此我们不期望这里有分组集。
	 */
	Assert(!query->groupingSets);

	/*
	 * We intentionally print query->groupClause not processed_groupClause,
	 * leaving it to the remote planner to get rid of any redundant GROUP BY
	 * items again.  This is necessary in case processed_groupClause reduced
	 * to empty, and in any case the redundancy situation on the remote might
	 * be different than what we think here.
	 *
	 * 我们故意打印query->groupClause而不是processed_groupClause，将其留给远程规划器来再次删除任何多余的GROUP BY项。  如果processed_groupClause减少为空，这是必要的，并且在任何情况下，远程上的冗余情况可能与我们在这里想象的不同。
	 */
	foreach(lc, query->groupClause)
	{
		SortGroupClause *grp = (SortGroupClause *) lfirst(lc);

		if (!first)
			appendStringInfoString(buf, ", ");
		first = false;

		deparseSortGroupClause(grp->tleSortGroupRef, tlist, true, context);
	}
}

/*
 * Deparse ORDER BY clause defined by the given pathkeys.
 *
 * 解析给定路径键定义的 ORDER BY 子句。
 *
 * The clause should use Vars from context->scanrel if !has_final_sort,
 * or from context->foreignrel's targetlist if has_final_sort.
 *
 * 该子句应使用来自 context->scanrel if !has_final_sort 的变量，或来自 context->foreignrel 的 targetlist if has_final_sort 的变量。
 *
 * We find a suitable pathkey expression (some earlier step
 * should have verified that there is one) and deparse it.
 *
 * 我们找到一个合适的路径键表达式（某些早期步骤应该已经验证是否存在）并解析它。
 */
static void
appendOrderByClause(List *pathkeys, bool has_final_sort,
					deparse_expr_cxt *context)
{
	ListCell   *lcell;
	int			nestlevel;
	StringInfo	buf = context->buf;
	bool		gotone = false;

	/* Make sure any constants in the exprs are printed portably
	 *
	 * 确保表达式中的任何常量都可移植地打印
	 */
	nestlevel = set_transmission_modes();

	foreach(lcell, pathkeys)
	{
		PathKey    *pathkey = lfirst(lcell);
		EquivalenceMember *em;
		Expr	   *em_expr;
		Oid			oprid;

		if (has_final_sort)
		{
			/*
			 * By construction, context->foreignrel is the input relation to
			 * the final sort.
			 *
			 * 通过构造，context->foreignrel 是最终排序的输入关系。
			 */
			em = find_em_for_rel_target(context->root,
										pathkey->pk_eclass,
										context->foreignrel);
		}
		else
			em = find_em_for_rel(context->root,
								 pathkey->pk_eclass,
								 context->scanrel);

		/*
		 * We don't expect any error here; it would mean that shippability
		 * wasn't verified earlier.  For the same reason, we don't recheck
		 * shippability of the sort operator.
		 *
		 * 我们预计这里不会出现任何错误；这意味着可发货性没有得到提前验证。  出于同样的原因，我们不会重新检查排序运算符的可运输性。
		 */
		if (em == NULL)
			elog(ERROR, "could not find pathkey item to sort");

		em_expr = em->em_expr;

		/*
		 * If the member is a Const expression then we needn't add it to the
		 * ORDER BY clause.  This can happen in UNION ALL queries where the
		 * union child targetlist has a Const.  Adding these would be
		 * wasteful, but also, for INT columns, an integer literal would be
		 * seen as an ordinal column position rather than a value to sort by.
		 * deparseConst() does have code to handle this, but it seems less
		 * effort on all accounts just to skip these for ORDER BY clauses.
		 *
		 * 如果成员是 Const 表达式，那么我们不需要将其添加到 ORDER BY 子句中。  这可能发生在 UNION ALL 查询中，其中并集子目标列表具有 Const。  添加这些会很浪费，而且对于 INT 列，整数文字将被视为序数列位置而不是排序依据的值。 deparseConst() 确实有代码来处理这个问题，但对于所有帐户来说，跳过这些 ORDER BY 子句似乎更省力。
		 */
		if (IsA(em_expr, Const))
			continue;

		if (!gotone)
		{
			appendStringInfoString(buf, " ORDER BY ");
			gotone = true;
		}
		else
			appendStringInfoString(buf, ", ");

		/*
		 * Lookup the operator corresponding to the compare type in the
		 * opclass. The datatype used by the opfamily is not necessarily the
		 * same as the expression type (for array types for example).
		 *
		 * 在opclass中查找与比较类型对应的运算符。 opfamily 使用的数据类型不一定与表达式类型相同（例如数组类型）。
		 */
		oprid = get_opfamily_member_for_cmptype(pathkey->pk_opfamily,
												em->em_datatype,
												em->em_datatype,
												pathkey->pk_cmptype);
		if (!OidIsValid(oprid))
			elog(ERROR, "missing operator %d(%u,%u) in opfamily %u",
				 pathkey->pk_cmptype, em->em_datatype, em->em_datatype,
				 pathkey->pk_opfamily);

		deparseExpr(em_expr, context);

		/*
		 * Here we need to use the expression's actual type to discover
		 * whether the desired operator will be the default or not.
		 *
		 * 这里我们需要使用表达式的实际类型来确定所需的运算符是否为默认运算符。
		 */
		appendOrderBySuffix(oprid, exprType((Node *) em_expr),
							pathkey->pk_nulls_first, context);

	}
	reset_transmission_modes(nestlevel);
}

/*
 * Deparse LIMIT/OFFSET clause.
 *
 * 解析 LIMIT/OFFSET 子句。
 */
static void
appendLimitClause(deparse_expr_cxt *context)
{
	PlannerInfo *root = context->root;
	StringInfo	buf = context->buf;
	int			nestlevel;

	/* Make sure any constants in the exprs are printed portably
	 *
	 * 确保表达式中的任何常量都可移植地打印
	 */
	nestlevel = set_transmission_modes();

	if (root->parse->limitCount)
	{
		appendStringInfoString(buf, " LIMIT ");
		deparseExpr((Expr *) root->parse->limitCount, context);
	}
	if (root->parse->limitOffset)
	{
		appendStringInfoString(buf, " OFFSET ");
		deparseExpr((Expr *) root->parse->limitOffset, context);
	}

	reset_transmission_modes(nestlevel);
}

/*
 * appendFunctionName
 *		Deparses function name from given function oid.
 *
 * appendFunctionName 从给定函数 oid 中解析函数名称。
 */
static void
appendFunctionName(Oid funcid, deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;
	HeapTuple	proctup;
	Form_pg_proc procform;
	const char *proname;

	proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(proctup))
		elog(ERROR, "cache lookup failed for function %u", funcid);
	procform = (Form_pg_proc) GETSTRUCT(proctup);

	/* Print schema name only if it's not pg_catalog
	 *
	 * 仅当模式名称不是 pg_catalog 时才打印模式名称
	 */
	if (procform->pronamespace != PG_CATALOG_NAMESPACE)
	{
		const char *schemaname;

		schemaname = get_namespace_name(procform->pronamespace);
		appendStringInfo(buf, "%s.", quote_identifier(schemaname));
	}

	/* Always print the function name
	 *
	 * 始终打印函数名称
	 */
	proname = NameStr(procform->proname);
	appendStringInfoString(buf, quote_identifier(proname));

	ReleaseSysCache(proctup);
}

/*
 * Appends a sort or group clause.
 *
 * 附加排序或组子句。
 *
 * Like get_rule_sortgroupclause(), returns the expression tree, so caller
 * need not find it again.
 *
 * 与 get_rule_sortgroupclause() 类似，返回表达式树，因此调用者无需再次查找它。
 */
static Node *
deparseSortGroupClause(Index ref, List *tlist, bool force_colno,
					   deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;
	TargetEntry *tle;
	Expr	   *expr;

	tle = get_sortgroupref_tle(ref, tlist);
	expr = tle->expr;

	if (force_colno)
	{
		/* Use column-number form when requested by caller.
		 *
		 * 当调用者要求时，使用列号形式。
		 */
		Assert(!tle->resjunk);
		appendStringInfo(buf, "%d", tle->resno);
	}
	else if (expr && IsA(expr, Const))
	{
		/*
		 * Force a typecast here so that we don't emit something like "GROUP
		 * BY 2", which will be misconstrued as a column position rather than
		 * a constant.
		 *
		 * 在这里强制进行类型转换，这样我们就不会发出类似“GROUP BY 2”的东西，它会被误解为列位置而不是常量。
		 */
		deparseConst((Const *) expr, context, 1);
	}
	else if (!expr || IsA(expr, Var))
		deparseExpr(expr, context);
	else
	{
		/* Always parenthesize the expression.
		 *
		 * 始终将表达式括起来。
		 */
		appendStringInfoChar(buf, '(');
		deparseExpr(expr, context);
		appendStringInfoChar(buf, ')');
	}

	return (Node *) expr;
}


/*
 * Returns true if given Var is deparsed as a subquery output column, in
 * which case, *relno and *colno are set to the IDs for the relation and
 * column alias to the Var provided by the subquery.
 *
 * 如果给定的 Var 被解析为子查询输出列，则返回 true，在这种情况下，*relno 和 *colno 设置为关系的 ID，以及子查询提供的 Var 的列别名。
 */
static bool
is_subquery_var(Var *node, RelOptInfo *foreignrel, int *relno, int *colno)
{
	PgFdwRelationInfo *fpinfo = (PgFdwRelationInfo *) foreignrel->fdw_private;
	RelOptInfo *outerrel = fpinfo->outerrel;
	RelOptInfo *innerrel = fpinfo->innerrel;

	/* Should only be called in these cases.
	 *
	 * 仅应在这些情况下调用。
	 */
	Assert(IS_SIMPLE_REL(foreignrel) || IS_JOIN_REL(foreignrel));

	/*
	 * If the given relation isn't a join relation, it doesn't have any lower
	 * subqueries, so the Var isn't a subquery output column.
	 *
	 * 如果给定关系不是联接关系，则它没有任何较低的子查询，因此 Var 不是子查询输出列。
	 */
	if (!IS_JOIN_REL(foreignrel))
		return false;

	/*
	 * If the Var doesn't belong to any lower subqueries, it isn't a subquery
	 * output column.
	 *
	 * 如果 Var 不属于任何较低的子查询，则它不是子查询输出列。
	 */
	if (!bms_is_member(node->varno, fpinfo->lower_subquery_rels))
		return false;

	if (bms_is_member(node->varno, outerrel->relids))
	{
		/*
		 * If outer relation is deparsed as a subquery, the Var is an output
		 * column of the subquery; get the IDs for the relation/column alias.
		 *
		 * 如果将外关系解析为子查询，则Var是子查询的输出列；获取关系/列别名的 ID。
		 */
		if (fpinfo->make_outerrel_subquery)
		{
			get_relation_column_alias_ids(node, outerrel, relno, colno);
			return true;
		}

		/* Otherwise, recurse into the outer relation.
		 *
		 * 否则，递归到外部关系。
		 */
		return is_subquery_var(node, outerrel, relno, colno);
	}
	else
	{
		Assert(bms_is_member(node->varno, innerrel->relids));

		/*
		 * If inner relation is deparsed as a subquery, the Var is an output
		 * column of the subquery; get the IDs for the relation/column alias.
		 *
		 * 如果将内关系解析为子查询，则Var是子查询的输出列；获取关系/列别名的 ID。
		 */
		if (fpinfo->make_innerrel_subquery)
		{
			get_relation_column_alias_ids(node, innerrel, relno, colno);
			return true;
		}

		/* Otherwise, recurse into the inner relation.
		 *
		 * 否则，递归到内部关系。
		 */
		return is_subquery_var(node, innerrel, relno, colno);
	}
}

/*
 * Get the IDs for the relation and column alias to given Var belonging to
 * given relation, which are returned into *relno and *colno.
 *
 * 获取属于给定关系的给定 Var 的关系 ID 和列别名，这些 ID 将返回到 *relno 和 *colno 中。
 */
static void
get_relation_column_alias_ids(Var *node, RelOptInfo *foreignrel,
							  int *relno, int *colno)
{
	PgFdwRelationInfo *fpinfo = (PgFdwRelationInfo *) foreignrel->fdw_private;
	int			i;
	ListCell   *lc;

	/* Get the relation alias ID
	 *
	 * 获取关系别名 ID
	 */
	*relno = fpinfo->relation_index;

	/* Get the column alias ID
	 *
	 * 获取列别名 ID
	 */
	i = 1;
	foreach(lc, foreignrel->reltarget->exprs)
	{
		Var		   *tlvar = (Var *) lfirst(lc);

		/*
		 * Match reltarget entries only on varno/varattno.  Ideally there
		 * would be some cross-check on varnullingrels, but it's unclear what
		 * to do exactly; we don't have enough context to know what that value
		 * should be.
		 *
		 * 仅在 varno/varattno 上匹配 reltarget 条目。  理想情况下，会对 varnullingrels 进行一些交叉检查，但目前还不清楚到底要做什么；我们没有足够的上下文来知道该值应该是什么。
		 */
		if (IsA(tlvar, Var) &&
			tlvar->varno == node->varno &&
			tlvar->varattno == node->varattno)
		{
			*colno = i;
			return;
		}
		i++;
	}

	/* Shouldn't get here
	 *
	 * 不应该到这里
	 */
	elog(ERROR, "unexpected expression in subquery output");
}

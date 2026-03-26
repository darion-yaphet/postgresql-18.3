/*-------------------------------------------------------------------------
 *
 * print.c
 *	  various print routines (used mostly for debugging)
 *	  各种打印例程（主要用于调试）
 *
 * 实现核心流程概述：
 * 本文件提供了一组用于调试的打印工具，能够将 PostgreSQL 的内部节点结构（Node）
 * 转换为人类可读的字符串格式。
 *
 * 核心流程如下：
 * 1. 序列化：利用 nodeToString() 或 nodeToStringWithLocations() 将 Node 树转换为紧凑的字符串。
 * 2. 格式化：
 *    - format_node_dump()：简单的换行处理，确保不超出屏幕宽度。
 *    - pretty_format_node_dump()：基于花括号、冒号和缩进的格式化逻辑，使复杂树结构（如查询树、计划树）清晰可见。
 * 3. 输出：支持输出到标准输出（stdout）或系统日志（elog/ereport）。
 * 4. 特化打印：针对 RangeTable、表达式（Expr）、目标列表（TargetList）、PathKeys 和 TupleSlot 提供了专门的缩略打印逻辑。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/nodes/print.c
 *
 * HISTORY
 *	  AUTHOR			DATE			MAJOR EVENT
 *	  Andrew Yu			Oct 26, 1994	file creation
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/printtup.h"
#include "lib/stringinfo.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "nodes/print.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"


/*
 * =========================================================================
 * 1. 通用节点打印接口
 * 提供基础的打印功能，支持普通格式、美化格式以及向日志系统输出。
 * =========================================================================
 */

/*
 * print
 *	  print contents of Node to stdout
 *	  将 Node 的内容打印到标准输出
 */
void
print(const void *obj)
{
	char	   *s;
	char	   *f;

	s = nodeToStringWithLocations(obj);
	f = format_node_dump(s);
	pfree(s);
	printf("%s\n", f);
	fflush(stdout);
	pfree(f);
}

/*
 * pprint
 *	  pretty-print contents of Node to stdout
 *	  将 Node 的内容“美化打印”到标准输出
 */
void
pprint(const void *obj)
{
	char	   *s;
	char	   *f;

	s = nodeToStringWithLocations(obj);
	f = pretty_format_node_dump(s);
	pfree(s);
	printf("%s\n", f);
	fflush(stdout);
	pfree(f);
}

/*
 * elog_node_display
 *	  send pretty-printed contents of Node to postmaster log
 *	  将节点的美化打印内容发送到 postmaster 日志
 */
void
elog_node_display(int lev, const char *title, const void *obj, bool pretty)
{
	char	   *s;
	char	   *f;

	s = nodeToStringWithLocations(obj);
	if (pretty)
		f = pretty_format_node_dump(s);
	else
		f = format_node_dump(s);
	pfree(s);
	ereport(lev,
			(errmsg_internal("%s:", title),
			 errdetail_internal("%s", f)));
	pfree(f);
}

/*
 * =========================================================================
 * 2. 字符串格式化逻辑
 * 负责将 nodeToString 生成的原始字符串处理为带有换行和缩进的易读格式。
 * =========================================================================
 */

/*
 * Format a nodeToString output for display on a terminal.
 * 格式化 nodeToString 的输出以在终端显示。
 *
 * The result is a palloc'd string.
 * 结果是一个 palloc 分配的字符串。
 *
 * This version just tries to break at whitespace.
 * 此版本仅尝试在空白处换行。
 */
char *
format_node_dump(const char *dump)
{
#define LINELEN		78
	char		line[LINELEN + 1];
	StringInfoData str;
	int			i;
	int			j;
	int			k;

	initStringInfo(&str);
	i = 0;
	for (;;)
	{
		for (j = 0; j < LINELEN && dump[i] != '\0'; i++, j++)
			line[j] = dump[i];
		if (dump[i] == '\0')
			break;
		if (dump[i] == ' ')
		{
			/* ok to break at adjacent space
			 * 可以在相邻空格处换行 */
			i++;
		}
		else
		{
			for (k = j - 1; k > 0; k--)
				if (line[k] == ' ')
					break;
			if (k > 0)
			{
				/* back up; will reprint all after space
				 * 回退；将在空格后重新打印所有内容 */
				i -= (j - k - 1);
				j = k;
			}
		}
		line[j] = '\0';
		appendStringInfo(&str, "%s\n", line);
	}
	if (j > 0)
	{
		line[j] = '\0';
		appendStringInfo(&str, "%s\n", line);
	}
	return str.data;
#undef LINELEN
}

/*
 * Format a nodeToString output for display on a terminal.
 * 格式化 nodeToString 的输出以在终端显示。
 *
 * The result is a palloc'd string.
 * 结果是一个 palloc 分配的字符串。
 *
 * This version tries to indent intelligently.
 * 此版本尝试智能地进行缩进。
 */
char *
pretty_format_node_dump(const char *dump)
{
#define INDENTSTOP	3
#define MAXINDENT	60
#define LINELEN		78
	char		line[LINELEN + 1];
	StringInfoData str;
	int			indentLev;
	int			indentDist;
	int			i;
	int			j;

	initStringInfo(&str);
	indentLev = 0;				/* logical indent level
								 * 逻辑缩进级别 */
	indentDist = 0;				/* physical indent distance
								 * 物理缩进距离 */
	i = 0;
	for (;;)
	{
		for (j = 0; j < indentDist; j++)
			line[j] = ' ';
		for (; j < LINELEN && dump[i] != '\0'; i++, j++)
		{
			line[j] = dump[i];
			switch (line[j])
			{
				case '}':
					if (j != indentDist)
					{
						/* print data before the }
						 * 在 } 之前打印数据 */
						line[j] = '\0';
						appendStringInfo(&str, "%s\n", line);
					}
					/* print the } at indentDist
					 * 在 indentDist 处打印 } */
					line[indentDist] = '}';
					line[indentDist + 1] = '\0';
					appendStringInfo(&str, "%s\n", line);
					/* outdent
					 * 减少缩进 */
					if (indentLev > 0)
					{
						indentLev--;
						indentDist = Min(indentLev * INDENTSTOP, MAXINDENT);
					}
					j = indentDist - 1;
					/* j will equal indentDist on next loop iteration
					 * 在下次循环迭代中 j 将等于 indentDist */
					/* suppress whitespace just after }
					 * 抑制紧跟在 } 之后的空白 */
					while (dump[i + 1] == ' ')
						i++;
					break;
				case ')':
					/* force line break after ), unless another ) follows
					 * 除非后面紧跟另一个 )，否则强制在 ) 之后换行 */
					if (dump[i + 1] != ')')
					{
						line[j + 1] = '\0';
						appendStringInfo(&str, "%s\n", line);
						j = indentDist - 1;
						while (dump[i + 1] == ' ')
							i++;
					}
					break;
				case '{':
					/* force line break before {
					 * 强制在 { 之前换行 */
					if (j != indentDist)
					{
						line[j] = '\0';
						appendStringInfo(&str, "%s\n", line);
					}
					/* indent
					 * 增加缩进 */
					indentLev++;
					indentDist = Min(indentLev * INDENTSTOP, MAXINDENT);
					for (j = 0; j < indentDist; j++)
						line[j] = ' ';
					line[j] = dump[i];
					break;
				case ':':
					/* force line break before :
					 * 强制在 : 之前换行 */
					if (j != indentDist)
					{
						line[j] = '\0';
						appendStringInfo(&str, "%s\n", line);
					}
					j = indentDist;
					line[j] = dump[i];
					break;
			}
		}
		line[j] = '\0';
		if (dump[i] == '\0')
			break;
		appendStringInfo(&str, "%s\n", line);
	}
	if (j > 0)
		appendStringInfo(&str, "%s\n", line);
	return str.data;
#undef INDENTSTOP
#undef MAXINDENT
#undef LINELEN
}

/*
 * =========================================================================
 * 3. 特定数据结构特化打印
 * 针对 RangeTable、表达式、路径键、目标列表等核心数据结构提供简洁的摘要打印。
 * =========================================================================
 */

/*
 * print_rt
 *	  print contents of range table
 *	  打印范围表（Range Table）的内容
 */
void
print_rt(const List *rtable)
{
	const ListCell *l;
	int			i = 1;

	printf("resno\trefname  \trelid\tinFromCl\n");
	printf("-----\t---------\t-----\t--------\n");
	foreach(l, rtable)
	{
		RangeTblEntry *rte = lfirst(l);

		switch (rte->rtekind)
		{
			case RTE_RELATION:
				printf("%d\t%s\t%u\t%c",
					   i, rte->eref->aliasname, rte->relid, rte->relkind);
				break;
			case RTE_SUBQUERY:
				printf("%d\t%s\t[subquery]",
					   i, rte->eref->aliasname);
				break;
			case RTE_JOIN:
				printf("%d\t%s\t[join]",
					   i, rte->eref->aliasname);
				break;
			case RTE_FUNCTION:
				printf("%d\t%s\t[rangefunction]",
					   i, rte->eref->aliasname);
				break;
			case RTE_TABLEFUNC:
				printf("%d\t%s\t[table function]",
					   i, rte->eref->aliasname);
				break;
			case RTE_VALUES:
				printf("%d\t%s\t[values list]",
					   i, rte->eref->aliasname);
				break;
			case RTE_CTE:
				printf("%d\t%s\t[cte]",
					   i, rte->eref->aliasname);
				break;
			case RTE_NAMEDTUPLESTORE:
				printf("%d\t%s\t[tuplestore]",
					   i, rte->eref->aliasname);
				break;
			case RTE_RESULT:
				printf("%d\t%s\t[result]",
					   i, rte->eref->aliasname);
				break;
			case RTE_GROUP:
				printf("%d\t%s\t[group]",
					   i, rte->eref->aliasname);
				break;
			default:
				printf("%d\t%s\t[unknown rtekind]",
					   i, rte->eref->aliasname);
		}

		printf("\t%s\t%s\n",
			   (rte->inh ? "inh" : ""),
			   (rte->inFromCl ? "inFromCl" : ""));
		i++;
	}
}


/*
 * print_expr
 *	  print an expression
 *	  打印表达式
 */
void
print_expr(const Node *expr, const List *rtable)
{
	if (expr == NULL)
	{
		printf("<>");
		return;
	}

	if (IsA(expr, Var))
	{
		const Var  *var = (const Var *) expr;
		char	   *relname,
				   *attname;

		switch (var->varno)
		{
			case INNER_VAR:
				relname = "INNER";
				attname = "?";
				break;
			case OUTER_VAR:
				relname = "OUTER";
				attname = "?";
				break;
			case INDEX_VAR:
				relname = "INDEX";
				attname = "?";
				break;
			default:
				{
					RangeTblEntry *rte;

					Assert(var->varno > 0 &&
						   (int) var->varno <= list_length(rtable));
					rte = rt_fetch(var->varno, rtable);
					relname = rte->eref->aliasname;
					attname = get_rte_attribute_name(rte, var->varattno);
				}
				break;
		}
		printf("%s.%s", relname, attname);
	}
	else if (IsA(expr, Const))
	{
		const Const *c = (const Const *) expr;
		Oid			typoutput;
		bool		typIsVarlena;
		char	   *outputstr;

		if (c->constisnull)
		{
			printf("NULL");
			return;
		}

		getTypeOutputInfo(c->consttype,
						  &typoutput, &typIsVarlena);

		outputstr = OidOutputFunctionCall(typoutput, c->constvalue);
		printf("%s", outputstr);
		pfree(outputstr);
	}
	else if (IsA(expr, OpExpr))
	{
		const OpExpr *e = (const OpExpr *) expr;
		char	   *opname;

		opname = get_opname(e->opno);
		if (list_length(e->args) > 1)
		{
			print_expr(get_leftop((const Expr *) e), rtable);
			printf(" %s ", ((opname != NULL) ? opname : "(invalid operator)"));
			print_expr(get_rightop((const Expr *) e), rtable);
		}
		else
		{
			printf("%s ", ((opname != NULL) ? opname : "(invalid operator)"));
			print_expr(get_leftop((const Expr *) e), rtable);
		}
	}
	else if (IsA(expr, FuncExpr))
	{
		const FuncExpr *e = (const FuncExpr *) expr;
		char	   *funcname;
		ListCell   *l;

		funcname = get_func_name(e->funcid);
		printf("%s(", ((funcname != NULL) ? funcname : "(invalid function)"));
		foreach(l, e->args)
		{
			print_expr(lfirst(l), rtable);
			if (lnext(e->args, l))
				printf(",");
		}
		printf(")");
	}
	else
		printf("unknown expr");
}

/*
 * print_pathkeys -
 *	  pathkeys list of PathKeys
 *	  打印 PathKeys 列表
 */
void
print_pathkeys(const List *pathkeys, const List *rtable)
{
	const ListCell *i;

	printf("(");
	foreach(i, pathkeys)
	{
		PathKey    *pathkey = (PathKey *) lfirst(i);
		EquivalenceClass *eclass;
		ListCell   *k;
		bool		first = true;

		eclass = pathkey->pk_eclass;
		/* chase up, in case pathkey is non-canonical
		 * 追溯，以防 pathkey 是非规范的 */
		while (eclass->ec_merged)
			eclass = eclass->ec_merged;

		printf("(");
		foreach(k, eclass->ec_members)
		{
			EquivalenceMember *mem = (EquivalenceMember *) lfirst(k);

			if (first)
				first = false;
			else
				printf(", ");
			print_expr((Node *) mem->em_expr, rtable);
		}
		printf(")");
		if (lnext(pathkeys, i))
			printf(", ");
	}
	printf(")\n");
}

/*
 * print_tl
 *	  print targetlist in a more legible way.
 *	  以更易读的方式打印目标列表（targetlist）。
 */
void
print_tl(const List *tlist, const List *rtable)
{
	const ListCell *tl;

	printf("(\n");
	foreach(tl, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(tl);

		printf("\t%d %s\t", tle->resno,
			   tle->resname ? tle->resname : "<null>");
		if (tle->ressortgroupref != 0)
			printf("(%u):\t", tle->ressortgroupref);
		else
			printf("    :\t");
		print_expr((Node *) tle->expr, rtable);
		printf("\n");
	}
	printf(")\n");
}

/*
 * print_slot
 *	  print out the tuple with the given TupleTableSlot
 *	  打印给定 TupleTableSlot 中的元组
 */
void
print_slot(TupleTableSlot *slot)
{
	if (TupIsNull(slot))
	{
		printf("tuple is null.\n");
		return;
	}
	if (!slot->tts_tupleDescriptor)
	{
		printf("no tuple descriptor.\n");
		return;
	}

	debugtup(slot, NULL);
}

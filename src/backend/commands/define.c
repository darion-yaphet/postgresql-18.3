/*-------------------------------------------------------------------------
 *
 * define.c
 *	  Support routines for various kinds of object creation.
 *
 * 各类对象创建命令的支持例程。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/define.c
 *
 * DESCRIPTION
 *	  Support routines for dealing with DefElem nodes.
 *
 * 处理 DefElem 节点的支持例程。
 *
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <math.h>

#include "catalog/namespace.h"
#include "commands/defrem.h"
#include "nodes/makefuncs.h"
#include "parser/parse_type.h"
#include "utils/fmgrprotos.h"

/*
 * 核心流程概览：
 * 各 defGet* 从 DefElem 取出字符串、数值、布尔、int32/int64、OID、
 * 限定名、TypeName、类型长度或字符串列表；类型不符则报错。
 * errorConflictingDefElem 报告冲突或重复的选项。
 */
/*
 * Extract a string value (otherwise uninterpreted) from a DefElem.
 *
 * 从 DefElem 取出字符串值（不再解释其含义）。
 */
char *
defGetString(DefElem *def)
{
	if (def->arg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("%s requires a parameter",
						def->defname)));
	switch (nodeTag(def->arg))
	{
		case T_Integer:
			return psprintf("%ld", (long) intVal(def->arg));
		case T_Float:
			return castNode(Float, def->arg)->fval;
		case T_Boolean:
			return boolVal(def->arg) ? "true" : "false";
		case T_String:
			return strVal(def->arg);
		case T_TypeName:
			return TypeNameToString((TypeName *) def->arg);
		case T_List:
			return NameListToString((List *) def->arg);
		case T_A_Star:
			return pstrdup("*");
		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(def->arg));
	}
	return NULL;				/* keep compiler quiet */
	/*
	 *
	 * 避免编译器因未使用的返回值告警。
	 */
}

/*
 * Extract a numeric value (actually double) from a DefElem.
 *
 * 从 DefElem 取出数值（实际为 double）。
 */
double
defGetNumeric(DefElem *def)
{
	if (def->arg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("%s requires a numeric value",
						def->defname)));
	switch (nodeTag(def->arg))
	{
		case T_Integer:
			return (double) intVal(def->arg);
		case T_Float:
			return floatVal(def->arg);
		default:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("%s requires a numeric value",
							def->defname)));
	}
	return 0;					/* keep compiler quiet */
	/*
	 *
	 * 避免编译器因未使用的返回值告警。
	 */
}

/*
 * Extract a boolean value from a DefElem.
 *
 * 从 DefElem 取出布尔值。
 */
bool
defGetBoolean(DefElem *def)
{
	/*
	 * If no parameter value given, assume "true" is meant.
	 *
	 * 未给出参数值时，视为 true。
	 */
	if (def->arg == NULL)
		return true;

	/*
	 * Allow 0, 1, "true", "false", "on", "off"
	 *
	 * 允许 0、1、"true"、"false"、"on"、"off"。
	 */
	switch (nodeTag(def->arg))
	{
		case T_Integer:
			switch (intVal(def->arg))
			{
				case 0:
					return false;
				case 1:
					return true;
				default:
					/* otherwise, error out below */
					/*
					 *
					 * 其它值在下面报错。
					 */
					break;
			}
			break;
		default:
			{
				char	   *sval = defGetString(def);

				/*
				 * The set of strings accepted here should match up with the
				 * grammar's opt_boolean_or_string production.
				 *
				 * 这里接受的字符串集合应与语法的 opt_boolean_or_string 产生式一致。
				 */
				if (pg_strcasecmp(sval, "true") == 0)
					return true;
				if (pg_strcasecmp(sval, "false") == 0)
					return false;
				if (pg_strcasecmp(sval, "on") == 0)
					return true;
				if (pg_strcasecmp(sval, "off") == 0)
					return false;
			}
			break;
	}
	ereport(ERROR,
			(errcode(ERRCODE_SYNTAX_ERROR),
			 errmsg("%s requires a Boolean value",
					def->defname)));
	return false;				/* keep compiler quiet */
	/*
	 *
	 * 避免编译器因未使用的返回值告警。
	 */
}

/*
 * Extract an int32 value from a DefElem.
 *
 * 从 DefElem 取出 int32 值。
 */
int32
defGetInt32(DefElem *def)
{
	if (def->arg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("%s requires an integer value",
						def->defname)));
	switch (nodeTag(def->arg))
	{
		case T_Integer:
			return (int32) intVal(def->arg);
		default:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("%s requires an integer value",
							def->defname)));
	}
	return 0;					/* keep compiler quiet */
	/*
	 *
	 * 避免编译器因未使用的返回值告警。
	 */
}

/*
 * Extract an int64 value from a DefElem.
 *
 * 从 DefElem 取出 int64 值。
 */
int64
defGetInt64(DefElem *def)
{
	if (def->arg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("%s requires a numeric value",
						def->defname)));
	switch (nodeTag(def->arg))
	{
		case T_Integer:
			return (int64) intVal(def->arg);
		case T_Float:

			/*
			 * Values too large for int4 will be represented as Float
			 * constants by the lexer.  Accept these if they are valid int8
			 * strings.
			 *
			 * 超出 int4 的值会被词法分析器表示成 Float 常量。
			 * 若它是合法的 int8 字符串则接受。
			 */
			return DatumGetInt64(DirectFunctionCall1(int8in,
													 CStringGetDatum(castNode(Float, def->arg)->fval)));
		default:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("%s requires a numeric value",
							def->defname)));
	}
	return 0;					/* keep compiler quiet */
	/*
	 *
	 * 避免编译器因未使用的返回值告警。
	 */
}

/*
 * Extract an OID value from a DefElem.
 *
 * 从 DefElem 取出 OID 值。
 */
Oid
defGetObjectId(DefElem *def)
{
	if (def->arg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("%s requires a numeric value",
						def->defname)));
	switch (nodeTag(def->arg))
	{
		case T_Integer:
			return (Oid) intVal(def->arg);
		case T_Float:

			/*
			 * Values too large for int4 will be represented as Float
			 * constants by the lexer.  Accept these if they are valid OID
			 * strings.
			 *
			 * 超出 int4 的值会被词法分析器表示成 Float 常量。
			 * 若它是合法的 OID 字符串则接受。
			 */
			return DatumGetObjectId(DirectFunctionCall1(oidin,
														CStringGetDatum(castNode(Float, def->arg)->fval)));
		default:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("%s requires a numeric value",
							def->defname)));
	}
	return 0;					/* keep compiler quiet */
	/*
	 *
	 * 避免编译器因未使用的返回值告警。
	 */
}

/*
 * Extract a possibly-qualified name (as a List of Strings) from a DefElem.
 *
 * 从 DefElem 取出可能带模式限定的名字（String 的 List）。
 */
List *
defGetQualifiedName(DefElem *def)
{
	if (def->arg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("%s requires a parameter",
						def->defname)));
	switch (nodeTag(def->arg))
	{
		case T_TypeName:
			return ((TypeName *) def->arg)->names;
		case T_List:
			return (List *) def->arg;
		case T_String:
			/* Allow quoted name for backwards compatibility */
			/*
			 *
			 * 为兼容旧写法，允许带引号的名字。
			 */
			return list_make1(def->arg);
		default:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("argument of %s must be a name",
							def->defname)));
	}
	return NIL;					/* keep compiler quiet */
	/*
	 *
	 * 避免编译器因未使用的返回值告警。
	 */
}

/*
 * Extract a TypeName from a DefElem.
 *
 * 从 DefElem 取出 TypeName。
 *
 * Note: we do not accept a List arg here, because the parser will only
 * return a bare List when the name looks like an operator name.
 *
 * 这里不接受 List 参数，因为解析器只在名字看起来像操作符名时才返回裸 List。
 */
TypeName *
defGetTypeName(DefElem *def)
{
	if (def->arg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("%s requires a parameter",
						def->defname)));
	switch (nodeTag(def->arg))
	{
		case T_TypeName:
			return (TypeName *) def->arg;
		case T_String:
			/* Allow quoted typename for backwards compatibility */
			/*
			 *
			 * 为兼容旧写法，允许带引号的类型名。
			 */
			return makeTypeNameFromNameList(list_make1(def->arg));
		default:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("argument of %s must be a type name",
							def->defname)));
	}
	return NULL;				/* keep compiler quiet */
	/*
	 *
	 * 避免编译器因未使用的返回值告警。
	 */
}

/*
 * Extract a type length indicator (either absolute bytes, or
 * -1 for "variable") from a DefElem.
 *
 * 从 DefElem 取出类型长度：绝对字节数，或 -1 表示 "variable"。
 */
int
defGetTypeLength(DefElem *def)
{
	if (def->arg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("%s requires a parameter",
						def->defname)));
	switch (nodeTag(def->arg))
	{
		case T_Integer:
			return intVal(def->arg);
		case T_Float:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("%s requires an integer value",
							def->defname)));
			break;
		case T_String:
			if (pg_strcasecmp(strVal(def->arg), "variable") == 0)
				return -1;		/* variable length */
				/*
				 *
				 * 变长。
				 */
			break;
		case T_TypeName:
			/* cope if grammar chooses to believe "variable" is a typename */
			/*
			 *
			 * 若语法把 "variable" 当成类型名，在这里兜住。
			 */
			if (pg_strcasecmp(TypeNameToString((TypeName *) def->arg),
							  "variable") == 0)
				return -1;		/* variable length */
				/*
				 *
				 * 变长。
				 */
			break;
		case T_List:
			/* must be an operator name */
			/*
			 *
			 * 应当是操作符名。
			 */
			break;
		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(def->arg));
	}
	ereport(ERROR,
			(errcode(ERRCODE_SYNTAX_ERROR),
			 errmsg("invalid argument for %s: \"%s\"",
					def->defname, defGetString(def))));
	return 0;					/* keep compiler quiet */
	/*
	 *
	 * 避免编译器因未使用的返回值告警。
	 */
}

/*
 * Extract a list of string values (otherwise uninterpreted) from a DefElem.
 *
 * 从 DefElem 取出字符串列表（不再解释其含义）。
 */
List *
defGetStringList(DefElem *def)
{
	ListCell   *cell;

	if (def->arg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("%s requires a parameter",
						def->defname)));
	if (nodeTag(def->arg) != T_List)
		elog(ERROR, "unrecognized node type: %d", (int) nodeTag(def->arg));

	foreach(cell, (List *) def->arg)
	{
		Node	   *str = (Node *) lfirst(cell);

		if (!IsA(str, String))
			elog(ERROR, "unexpected node type in name list: %d",
				 (int) nodeTag(str));
	}

	return (List *) def->arg;
}

/*
 * Raise an error about a conflicting DefElem.
 *
 * 对互相冲突的 DefElem 报错。
 */
void
errorConflictingDefElem(DefElem *defel, ParseState *pstate)
{
	ereport(ERROR,
			errcode(ERRCODE_SYNTAX_ERROR),
			errmsg("conflicting or redundant options"),
			parser_errposition(pstate, defel->location));
}

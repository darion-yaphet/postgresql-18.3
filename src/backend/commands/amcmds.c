/*-------------------------------------------------------------------------
 *
 * amcmds.c
 *	  Routines for SQL commands that manipulate access methods.
 *
 * 操纵访问方法的 SQL 命令例程。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/amcmds.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_am.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "parser/parse_func.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"


/*
 * 核心流程概览：
 * CreateAccessMethod 要求超级用户，查重后解析 handler，插入 pg_am 并记录依赖。
 * get_am_type_oid 及其包装按名称查找 AM OID，并可校验索引或表类型。
 */
static Oid	lookup_am_handler_func(List *handler_name, char amtype);
static const char *get_am_type_string(char amtype);


/*
 * CreateAccessMethod
 *		Registers a new access method.
 *
 * CreateAccessMethod：注册一个新的访问方法。
 */
ObjectAddress
CreateAccessMethod(CreateAmStmt *stmt)
{
	Relation	rel;
	ObjectAddress myself;
	ObjectAddress referenced;
	Oid			amoid;
	Oid			amhandler;
	bool		nulls[Natts_pg_am];
	Datum		values[Natts_pg_am];
	HeapTuple	tup;

	rel = table_open(AccessMethodRelationId, RowExclusiveLock);

	/* Must be superuser */
	/*
	 *
	 * 必须是超级用户。
	 */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to create access method \"%s\"",
						stmt->amname),
				 errhint("Must be superuser to create an access method.")));

	/* Check if name is used */
	/*
	 *
	 * 检查名称是否已被使用。
	 */
	amoid = GetSysCacheOid1(AMNAME, Anum_pg_am_oid,
							CStringGetDatum(stmt->amname));
	if (OidIsValid(amoid))
	{
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("access method \"%s\" already exists",
						stmt->amname)));
	}

	/*
	 * Get the handler function oid, verifying the AM type while at it.
	 *
	 * 取得处理函数 OID，同时校验 AM 类型。
	 */
	amhandler = lookup_am_handler_func(stmt->handler_name, stmt->amtype);

	/*
	 * Insert tuple into pg_am.
	 *
	 * 向 pg_am 插入元组。
	 */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	amoid = GetNewOidWithIndex(rel, AmOidIndexId, Anum_pg_am_oid);
	values[Anum_pg_am_oid - 1] = ObjectIdGetDatum(amoid);
	values[Anum_pg_am_amname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(stmt->amname));
	values[Anum_pg_am_amhandler - 1] = ObjectIdGetDatum(amhandler);
	values[Anum_pg_am_amtype - 1] = CharGetDatum(stmt->amtype);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);

	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	myself.classId = AccessMethodRelationId;
	myself.objectId = amoid;
	myself.objectSubId = 0;

	/* Record dependency on handler function */
	/*
	 *
	 * 记录对处理函数的依赖。
	 */
	referenced.classId = ProcedureRelationId;
	referenced.objectId = amhandler;
	referenced.objectSubId = 0;

	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	recordDependencyOnCurrentExtension(&myself, false);

	InvokeObjectPostCreateHook(AccessMethodRelationId, amoid, 0);

	table_close(rel, RowExclusiveLock);

	return myself;
}

/*
 * get_am_type_oid
 *		Worker for various get_am_*_oid variants
 *
 * get_am_type_oid：各 get_am_*_oid 变体的公共实现。
 *
 * If missing_ok is false, throw an error if access method not found.  If
 * true, just return InvalidOid.
 *
 * missing_ok 为 false 时，找不到访问方法就报错；为 true 时返回 InvalidOid。
 *
 * If amtype is not '\0', an error is raised if the AM found is not of the
 * given type.
 *
 * amtype 不是 '\0' 时，若找到的 AM 类型不符则报错。
 */
static Oid
get_am_type_oid(const char *amname, char amtype, bool missing_ok)
{
	HeapTuple	tup;
	Oid			oid = InvalidOid;

	tup = SearchSysCache1(AMNAME, CStringGetDatum(amname));
	if (HeapTupleIsValid(tup))
	{
		Form_pg_am	amform = (Form_pg_am) GETSTRUCT(tup);

		if (amtype != '\0' &&
			amform->amtype != amtype)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("access method \"%s\" is not of type %s",
							NameStr(amform->amname),
							get_am_type_string(amtype))));

		oid = amform->oid;
		ReleaseSysCache(tup);
	}

	if (!OidIsValid(oid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("access method \"%s\" does not exist", amname)));
	return oid;
}

/*
 * get_index_am_oid - given an access method name, look up its OID
 *		and verify it corresponds to an index AM.
 *
 * 按访问方法名查找 OID，并确认它是索引 AM。
 */
Oid
get_index_am_oid(const char *amname, bool missing_ok)
{
	return get_am_type_oid(amname, AMTYPE_INDEX, missing_ok);
}

/*
 * get_table_am_oid - given an access method name, look up its OID
 *		and verify it corresponds to a table AM.
 *
 * 按访问方法名查找 OID，并确认它是表 AM。
 */
Oid
get_table_am_oid(const char *amname, bool missing_ok)
{
	return get_am_type_oid(amname, AMTYPE_TABLE, missing_ok);
}

/*
 * get_am_oid - given an access method name, look up its OID.
 *		The type is not checked.
 *
 * 按访问方法名查找 OID，不检查类型。
 */
Oid
get_am_oid(const char *amname, bool missing_ok)
{
	return get_am_type_oid(amname, '\0', missing_ok);
}

/*
 * get_am_name - given an access method OID, look up its name.
 *
 * 按访问方法 OID 查找名称。
 */
char *
get_am_name(Oid amOid)
{
	HeapTuple	tup;
	char	   *result = NULL;

	tup = SearchSysCache1(AMOID, ObjectIdGetDatum(amOid));
	if (HeapTupleIsValid(tup))
	{
		Form_pg_am	amform = (Form_pg_am) GETSTRUCT(tup);

		result = pstrdup(NameStr(amform->amname));
		ReleaseSysCache(tup);
	}
	return result;
}

/*
 * Convert single-character access method type into string for error reporting.
 *
 * 把单字符访问方法类型转成用于报错的字符串。
 */
static const char *
get_am_type_string(char amtype)
{
	switch (amtype)
	{
		case AMTYPE_INDEX:
			return "INDEX";
		case AMTYPE_TABLE:
			return "TABLE";
		default:
			/* shouldn't happen */
			/*
			 *
			 * 不应发生。
			 */
			elog(ERROR, "invalid access method type '%c'", amtype);
			return NULL;		/* keep compiler quiet */
			/*
			 *
			 * 避免编译器因未使用的返回值告警。
			 */
	}
}

/*
 * Convert a handler function name to an Oid.  If the return type of the
 * function doesn't match the given AM type, an error is raised.
 *
 * 把处理函数名解析为 Oid。返回类型与给定 AM 类型不符则报错。
 *
 * This function either return valid function Oid or throw an error.
 *
 * 本函数要么返回有效的函数 Oid，要么抛出错误。
 */
static Oid
lookup_am_handler_func(List *handler_name, char amtype)
{
	Oid			handlerOid;
	Oid			funcargtypes[1] = {INTERNALOID};
	Oid			expectedType = InvalidOid;

	if (handler_name == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("handler function is not specified")));

	/* handlers have one argument of type internal */
	/*
	 *
	 * 处理函数有一个 internal 类型参数。
	 */
	handlerOid = LookupFuncName(handler_name, 1, funcargtypes, false);

	/* check that handler has the correct return type */
	/*
	 *
	 * 检查处理函数的返回类型是否正确。
	 */
	switch (amtype)
	{
		case AMTYPE_INDEX:
			expectedType = INDEX_AM_HANDLEROID;
			break;
		case AMTYPE_TABLE:
			expectedType = TABLE_AM_HANDLEROID;
			break;
		default:
			elog(ERROR, "unrecognized access method type \"%c\"", amtype);
	}

	if (get_func_rettype(handlerOid) != expectedType)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("function %s must return type %s",
						get_func_name(handlerOid),
						format_type_extended(expectedType, -1, 0))));

	return handlerOid;
}

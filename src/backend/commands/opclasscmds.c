/*-------------------------------------------------------------------------
 *
 * opclasscmds.c
 *
 *	  Routines for opclass (and opfamily) manipulation commands
 *
 * 操作符类（opclass）与操作符族（opfamily）维护命令的实现。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/opclasscmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "access/genam.h"
#include "access/hash.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/table.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_am.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/event_trigger.h"
#include "miscadmin.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "parser/parse_type.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

/*
 * 核心流程概览：
 * DefineOpClass 解析操作符类名称、访问方法与数据类型（当前要求超级用户），
 * 写入 pg_opclass，并由 storeOperators / storeProcedures 登记 pg_amop、pg_amproc 及依赖。
 * DefineOpFamily 创建 opfamily；AlterOpFamily 按 ADD/DROP 调用
 * AlterOpFamilyAdd 或 AlterOpFamilyDrop。
 * get_opclass_oid / get_opfamily_oid 按可能带 schema 限定的名称查找 OID。
 */
static void AlterOpFamilyAdd(AlterOpFamilyStmt *stmt,
							 Oid amoid, Oid opfamilyoid,
							 int maxOpNumber, int maxProcNumber,
							 int optsProcNumber, List *items);
static void AlterOpFamilyDrop(AlterOpFamilyStmt *stmt,
							  Oid amoid, Oid opfamilyoid,
							  int maxOpNumber, int maxProcNumber,
							  List *items);
static void processTypesSpec(List *args, Oid *lefttype, Oid *righttype);
static void assignOperTypes(OpFamilyMember *member, Oid amoid, Oid typeoid);
static void assignProcTypes(OpFamilyMember *member, Oid amoid, Oid typeoid,
							int opclassOptsProcNum);
static void addFamilyMember(List **list, OpFamilyMember *member);
static void storeOperators(List *opfamilyname, Oid amoid, Oid opfamilyoid,
						   List *operators, bool isAdd);
static void storeProcedures(List *opfamilyname, Oid amoid, Oid opfamilyoid,
							List *procedures, bool isAdd);
static bool typeDepNeeded(Oid typid, OpFamilyMember *member);
static void dropOperators(List *opfamilyname, Oid amoid, Oid opfamilyoid,
						  List *operators);
static void dropProcedures(List *opfamilyname, Oid amoid, Oid opfamilyoid,
						   List *procedures);

/*
 * OpFamilyCacheLookup
 *		Look up an existing opfamily by name.
 *
 * 按名称查找已有的 opfamily。
 *
 * Returns a syscache tuple reference, or NULL if not found.
 *
 * 返回 syscache 元组引用；未找到则返回 NULL。
 */
static HeapTuple
OpFamilyCacheLookup(Oid amID, List *opfamilyname, bool missing_ok)
{
	char	   *schemaname;
	char	   *opfname;
	HeapTuple	htup;

	/* deconstruct the name list */
	/*
	 *
	 * 拆开名称列表。
	 */
	DeconstructQualifiedName(opfamilyname, &schemaname, &opfname);

	if (schemaname)
	{
		/* Look in specific schema only */
		/*
		 *
		 * 只在指定的 schema 中查找。
		 */
		Oid			namespaceId;

		namespaceId = LookupExplicitNamespace(schemaname, missing_ok);
		if (!OidIsValid(namespaceId))
			htup = NULL;
		else
			htup = SearchSysCache3(OPFAMILYAMNAMENSP,
								   ObjectIdGetDatum(amID),
								   PointerGetDatum(opfname),
								   ObjectIdGetDatum(namespaceId));
	}
	else
	{
		/* Unqualified opfamily name, so search the search path */
		/*
		 *
		 * opfamily 名未限定 schema，因此沿 search_path 查找。
		 */
		Oid			opfID = OpfamilynameGetOpfid(amID, opfname);

		if (!OidIsValid(opfID))
			htup = NULL;
		else
			htup = SearchSysCache1(OPFAMILYOID, ObjectIdGetDatum(opfID));
	}

	if (!HeapTupleIsValid(htup) && !missing_ok)
	{
		HeapTuple	amtup;

		amtup = SearchSysCache1(AMOID, ObjectIdGetDatum(amID));
		if (!HeapTupleIsValid(amtup))
			elog(ERROR, "cache lookup failed for access method %u", amID);
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("operator family \"%s\" does not exist for access method \"%s\"",
						NameListToString(opfamilyname),
						NameStr(((Form_pg_am) GETSTRUCT(amtup))->amname))));
	}

	return htup;
}

/*
 * get_opfamily_oid
 *	  find an opfamily OID by possibly qualified name
 *
 * 按可能带 schema 限定的名称查找 opfamily OID。
 *
 * If not found, returns InvalidOid if missing_ok, else throws error.
 *
 * 未找到时：missing_ok 为真则返回 InvalidOid，否则报错。
 */
Oid
get_opfamily_oid(Oid amID, List *opfamilyname, bool missing_ok)
{
	HeapTuple	htup;
	Form_pg_opfamily opfamform;
	Oid			opfID;

	htup = OpFamilyCacheLookup(amID, opfamilyname, missing_ok);
	if (!HeapTupleIsValid(htup))
		return InvalidOid;
	opfamform = (Form_pg_opfamily) GETSTRUCT(htup);
	opfID = opfamform->oid;
	ReleaseSysCache(htup);

	return opfID;
}

/*
 * OpClassCacheLookup
 *		Look up an existing opclass by name.
 *
 * 按名称查找已有的 opclass。
 *
 * Returns a syscache tuple reference, or NULL if not found.
 *
 * 返回 syscache 元组引用；未找到则返回 NULL。
 */
static HeapTuple
OpClassCacheLookup(Oid amID, List *opclassname, bool missing_ok)
{
	char	   *schemaname;
	char	   *opcname;
	HeapTuple	htup;

	/* deconstruct the name list */
	/*
	 *
	 * 拆开名称列表。
	 */
	DeconstructQualifiedName(opclassname, &schemaname, &opcname);

	if (schemaname)
	{
		/* Look in specific schema only */
		/*
		 *
		 * 只在指定的 schema 中查找。
		 */
		Oid			namespaceId;

		namespaceId = LookupExplicitNamespace(schemaname, missing_ok);
		if (!OidIsValid(namespaceId))
			htup = NULL;
		else
			htup = SearchSysCache3(CLAAMNAMENSP,
								   ObjectIdGetDatum(amID),
								   PointerGetDatum(opcname),
								   ObjectIdGetDatum(namespaceId));
	}
	else
	{
		/* Unqualified opclass name, so search the search path */
		/*
		 *
		 * opclass 名未限定 schema，因此沿 search_path 查找。
		 */
		Oid			opcID = OpclassnameGetOpcid(amID, opcname);

		if (!OidIsValid(opcID))
			htup = NULL;
		else
			htup = SearchSysCache1(CLAOID, ObjectIdGetDatum(opcID));
	}

	if (!HeapTupleIsValid(htup) && !missing_ok)
	{
		HeapTuple	amtup;

		amtup = SearchSysCache1(AMOID, ObjectIdGetDatum(amID));
		if (!HeapTupleIsValid(amtup))
			elog(ERROR, "cache lookup failed for access method %u", amID);
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("operator class \"%s\" does not exist for access method \"%s\"",
						NameListToString(opclassname),
						NameStr(((Form_pg_am) GETSTRUCT(amtup))->amname))));
	}

	return htup;
}

/*
 * get_opclass_oid
 *	  find an opclass OID by possibly qualified name
 *
 * 按可能带 schema 限定的名称查找 opclass OID。
 *
 * If not found, returns InvalidOid if missing_ok, else throws error.
 *
 * 未找到时：missing_ok 为真则返回 InvalidOid，否则报错。
 */
Oid
get_opclass_oid(Oid amID, List *opclassname, bool missing_ok)
{
	HeapTuple	htup;
	Form_pg_opclass opcform;
	Oid			opcID;

	htup = OpClassCacheLookup(amID, opclassname, missing_ok);
	if (!HeapTupleIsValid(htup))
		return InvalidOid;
	opcform = (Form_pg_opclass) GETSTRUCT(htup);
	opcID = opcform->oid;
	ReleaseSysCache(htup);

	return opcID;
}

/*
 * CreateOpFamily
 *		Internal routine to make the catalog entry for a new operator family.
 *
 * 内部例程：为新建的 operator family 写入目录项。
 *
 * Caller must have done permissions checks etc. already.
 *
 * 调用方须已完成权限等检查。
 */
static ObjectAddress
CreateOpFamily(CreateOpFamilyStmt *stmt, const char *opfname,
			   Oid namespaceoid, Oid amoid)
{
	Oid			opfamilyoid;
	Relation	rel;
	HeapTuple	tup;
	Datum		values[Natts_pg_opfamily];
	bool		nulls[Natts_pg_opfamily];
	NameData	opfName;
	ObjectAddress myself,
				referenced;

	rel = table_open(OperatorFamilyRelationId, RowExclusiveLock);

	/*
	 * Make sure there is no existing opfamily of this name (this is just to
	 * give a more friendly error message than "duplicate key").
	 *
	 * 确认尚无同名 opfamily（只为给出比 duplicate key 更友好的错误信息）。
	 */
	if (SearchSysCacheExists3(OPFAMILYAMNAMENSP,
							  ObjectIdGetDatum(amoid),
							  CStringGetDatum(opfname),
							  ObjectIdGetDatum(namespaceoid)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("operator family \"%s\" for access method \"%s\" already exists",
						opfname, stmt->amname)));

	/*
	 * Okay, let's create the pg_opfamily entry.
	 *
	 * 开始创建 pg_opfamily 元组。
	 */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	opfamilyoid = GetNewOidWithIndex(rel, OpfamilyOidIndexId,
									 Anum_pg_opfamily_oid);
	values[Anum_pg_opfamily_oid - 1] = ObjectIdGetDatum(opfamilyoid);
	values[Anum_pg_opfamily_opfmethod - 1] = ObjectIdGetDatum(amoid);
	namestrcpy(&opfName, opfname);
	values[Anum_pg_opfamily_opfname - 1] = NameGetDatum(&opfName);
	values[Anum_pg_opfamily_opfnamespace - 1] = ObjectIdGetDatum(namespaceoid);
	values[Anum_pg_opfamily_opfowner - 1] = ObjectIdGetDatum(GetUserId());

	tup = heap_form_tuple(rel->rd_att, values, nulls);

	CatalogTupleInsert(rel, tup);

	heap_freetuple(tup);

	/*
	 * Create dependencies for the opfamily proper.
	 *
	 * 为 opfamily 本身建立依赖。
	 */
	myself.classId = OperatorFamilyRelationId;
	myself.objectId = opfamilyoid;
	myself.objectSubId = 0;

	/* dependency on access method */
	/*
	 *
	 * 依赖于访问方法。
	 */
	referenced.classId = AccessMethodRelationId;
	referenced.objectId = amoid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_AUTO);

	/* dependency on namespace */
	/*
	 *
	 * 依赖于命名空间。
	 */
	referenced.classId = NamespaceRelationId;
	referenced.objectId = namespaceoid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	/* dependency on owner */
	/*
	 *
	 * 依赖于属主。
	 */
	recordDependencyOnOwner(OperatorFamilyRelationId, opfamilyoid, GetUserId());

	/* dependency on extension */
	/*
	 *
	 * 依赖于扩展。
	 */
	recordDependencyOnCurrentExtension(&myself, false);

	/* Report the new operator family to possibly interested event triggers */
	/*
	 *
	 * 把新建的 operator family 报告给可能关心的事件触发器。
	 */
	EventTriggerCollectSimpleCommand(myself, InvalidObjectAddress,
									 (Node *) stmt);

	/* Post creation hook for new operator family */
	/*
	 *
	 * 新建 operator family 的创建后钩子。
	 */
	InvokeObjectPostCreateHook(OperatorFamilyRelationId, opfamilyoid, 0);

	table_close(rel, RowExclusiveLock);

	return myself;
}

/*
 * DefineOpClass
 *		Define a new index operator class.
 *
 * 定义新的索引操作符类。
 */
ObjectAddress
DefineOpClass(CreateOpClassStmt *stmt)
{
	char	   *opcname;		/* name of opclass we're creating */
	/*
	 *
	 * 正在创建的 opclass 名称。
	 */
	Oid			amoid,			/* our AM's oid */
	/*
	 *
	 * 所属访问方法的 OID。
	 */
				typeoid,		/* indexable datatype oid */
				/*
				 *
				 * 可索引数据类型的 OID。
				 */
				storageoid,		/* storage datatype oid, if any */
				/*
				 *
				 * 存储数据类型的 OID（如有）。
				 */
				namespaceoid,	/* namespace to create opclass in */
				/*
				 *
				 * 创建 opclass 所在的命名空间。
				 */
				opfamilyoid,	/* oid of containing opfamily */
				/*
				 *
				 * 所属 opfamily 的 OID。
				 */
				opclassoid;		/* oid of opclass we create */
				/*
				 *
				 * 所创建 opclass 的 OID。
				 */
	int			maxOpNumber,	/* amstrategies value */
	/*
	 *
	 * amstrategies 的值。
	 */
				optsProcNumber, /* amoptsprocnum value */
				/*
				 *
				 * amoptsprocnum 的值。
				 */
				maxProcNumber;	/* amsupport value */
				/*
				 *
				 * amsupport 的值。
				 */
	bool		amstorage;		/* amstorage flag */
	/*
	 *
	 * amstorage 标志。
	 */
	List	   *operators;		/* OpFamilyMember list for operators */
	/*
	 *
	 * 运算符对应的 OpFamilyMember 列表。
	 */
	List	   *procedures;		/* OpFamilyMember list for support procs */
	/*
	 *
	 * 支持函数对应的 OpFamilyMember 列表。
	 */
	ListCell   *l;
	Relation	rel;
	HeapTuple	tup;
	Form_pg_am	amform;
	IndexAmRoutine *amroutine;
	Datum		values[Natts_pg_opclass];
	bool		nulls[Natts_pg_opclass];
	AclResult	aclresult;
	NameData	opcName;
	ObjectAddress myself,
				referenced;

	/* Convert list of names to a name and namespace */
	/*
	 *
	 * 把名称列表拆成名字和命名空间。
	 */
	namespaceoid = QualifiedNameGetCreationNamespace(stmt->opclassname,
													 &opcname);

	/* Check we have creation rights in target namespace */
	/*
	 *
	 * 检查在目标命名空间中是否有创建权限。
	 */
	aclresult = object_aclcheck(NamespaceRelationId, namespaceoid, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
					   get_namespace_name(namespaceoid));

	/* Get necessary info about access method */
	/*
	 *
	 * 取得访问方法所需的信息。
	 */
	tup = SearchSysCache1(AMNAME, CStringGetDatum(stmt->amname));
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("access method \"%s\" does not exist",
						stmt->amname)));

	amform = (Form_pg_am) GETSTRUCT(tup);
	amoid = amform->oid;
	amroutine = GetIndexAmRoutineByAmId(amoid, false);
	ReleaseSysCache(tup);

	maxOpNumber = amroutine->amstrategies;
	/* if amstrategies is zero, just enforce that op numbers fit in int16 */
	/*
	 *
	 * 若 amstrategies 为 0，则只要求策略号能放入 int16。
	 */
	if (maxOpNumber <= 0)
		maxOpNumber = SHRT_MAX;
	maxProcNumber = amroutine->amsupport;
	optsProcNumber = amroutine->amoptsprocnum;
	amstorage = amroutine->amstorage;

	/* XXX Should we make any privilege check against the AM? */
	/*
	 *
	 * XXX 是否应对访问方法做权限检查？
	 */

	/*
	 * The question of appropriate permissions for CREATE OPERATOR CLASS is
	 * interesting.  Creating an opclass is tantamount to granting public
	 * execute access on the functions involved, since the index machinery
	 * generally does not check access permission before using the functions.
	 * A minimum expectation therefore is that the caller have execute
	 * privilege with grant option.  Since we don't have a way to make the
	 * opclass go away if the grant option is revoked, we choose instead to
	 * require ownership of the functions.  It's also not entirely clear what
	 * permissions should be required on the datatype, but ownership seems
	 * like a safe choice.
	 *
	 * CREATE OPERATOR CLASS 应要求何种权限并不直观。创建 opclass 几乎等于
	 * 向 public 授予相关函数的执行权，因为索引机制在调用这些函数前通常不检查权限。
	 * 最低预期是调用者拥有带 grant option 的 EXECUTE。授权被收回时无法让 opclass 消失，
	 * 因此改为要求调用者拥有这些函数。数据类型上应要求什么权限也不完全清楚，
	 * 要求拥有权是较稳妥的选择。
	 *
	 * Currently, we require superuser privileges to create an opclass. This
	 * seems necessary because we have no way to validate that the offered set
	 * of operators and functions are consistent with the AM's expectations.
	 * It would be nice to provide such a check someday, if it can be done
	 * without solving the halting problem :-(
	 *
	 * 目前创建 opclass 要求超级用户。这是必要的，因为无法验证给出的运算符
	 * 和函数是否符合访问方法的预期。若将来能在不解决停机问题的前提下做这种检查，
	 * 会更好 :-(
	 *
	 * XXX re-enable NOT_USED code sections below if you remove this test.
	 *
	 * XXX 若去掉此检查，请重新启用下面标为 NOT_USED 的代码段。
	 */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to create an operator class")));

	/* Look up the datatype */
	/*
	 *
	 * 查找数据类型。
	 */
	typeoid = typenameTypeId(NULL, stmt->datatype);

#ifdef NOT_USED
	/* XXX this is unnecessary given the superuser check above */
	/*
	 *
	 * XXX 上面已有超级用户检查，此处并不必要。
	 */
	/* Check we have ownership of the datatype */
	/*
	 *
	 * 检查当前用户是否拥有该数据类型。
	 */
	if (!object_ownercheck(TypeRelationId, typeoid, GetUserId()))
		aclcheck_error_type(ACLCHECK_NOT_OWNER, typeoid);
#endif

	/*
	 * Look up the containing operator family, or create one if FAMILY option
	 * was omitted and there's not a match already.
	 *
	 * 查找所属 operator family；若省略了 FAMILY 且尚无同名匹配，则创建一个。
	 */
	if (stmt->opfamilyname)
	{
		opfamilyoid = get_opfamily_oid(amoid, stmt->opfamilyname, false);
	}
	else
	{
		/* Lookup existing family of same name and namespace */
		/*
		 *
		 * 查找同名且同命名空间的已有 family。
		 */
		tup = SearchSysCache3(OPFAMILYAMNAMENSP,
							  ObjectIdGetDatum(amoid),
							  PointerGetDatum(opcname),
							  ObjectIdGetDatum(namespaceoid));
		if (HeapTupleIsValid(tup))
		{
			opfamilyoid = ((Form_pg_opfamily) GETSTRUCT(tup))->oid;

			/*
			 * XXX given the superuser check above, there's no need for an
			 * ownership check here
			 *
			 * XXX 上面已有超级用户检查，这里不必再检查拥有权。
			 */
			ReleaseSysCache(tup);
		}
		else
		{
			CreateOpFamilyStmt *opfstmt;
			ObjectAddress tmpAddr;

			opfstmt = makeNode(CreateOpFamilyStmt);
			opfstmt->opfamilyname = stmt->opclassname;
			opfstmt->amname = stmt->amname;

			/*
			 * Create it ... again no need for more permissions ...
			 *
			 * 创建它……同样不必再做额外权限检查。
			 */
			tmpAddr = CreateOpFamily(opfstmt, opcname, namespaceoid, amoid);
			opfamilyoid = tmpAddr.objectId;
		}
	}

	operators = NIL;
	procedures = NIL;

	/* Storage datatype is optional */
	/*
	 *
	 * 存储数据类型是可选的。
	 */
	storageoid = InvalidOid;

	/*
	 * Scan the "items" list to obtain additional info.
	 *
	 * 扫描 items 列表以取得附加信息。
	 */
	foreach(l, stmt->items)
	{
		CreateOpClassItem *item = lfirst_node(CreateOpClassItem, l);
		Oid			operOid;
		Oid			funcOid;
		Oid			sortfamilyOid;
		OpFamilyMember *member;

		switch (item->itemtype)
		{
			case OPCLASS_ITEM_OPERATOR:
				if (item->number <= 0 || item->number > maxOpNumber)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("invalid operator number %d,"
									" must be between 1 and %d",
									item->number, maxOpNumber)));
				if (item->name->objargs != NIL)
					operOid = LookupOperWithArgs(item->name, false);
				else
				{
					/* Default to binary op on input datatype */
					/*
					 *
					 * 默认当作输入数据类型上的二元运算符。
					 */
					operOid = LookupOperName(NULL, item->name->objname,
											 typeoid, typeoid,
											 false, -1);
				}

				if (item->order_family)
					sortfamilyOid = get_opfamily_oid(BTREE_AM_OID,
													 item->order_family,
													 false);
				else
					sortfamilyOid = InvalidOid;

#ifdef NOT_USED
				/* XXX this is unnecessary given the superuser check above */
				/*
				 *
				 * XXX 上面已有超级用户检查，此处并不必要。
				 */
				/* Caller must own operator and its underlying function */
				/*
				 *
				 * 调用方必须拥有该运算符及其底层函数。
				 */
				if (!object_ownercheck(OperatorRelationId, operOid, GetUserId()))
					aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_OPERATOR,
								   get_opname(operOid));
				funcOid = get_opcode(operOid);
				if (!object_ownercheck(ProcedureRelationId, funcOid, GetUserId()))
					aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FUNCTION,
								   get_func_name(funcOid));
#endif

				/* Save the info */
				/*
				 *
				 * 保存该信息。
				 */
				member = (OpFamilyMember *) palloc0(sizeof(OpFamilyMember));
				member->is_func = false;
				member->object = operOid;
				member->number = item->number;
				member->sortfamily = sortfamilyOid;
				assignOperTypes(member, amoid, typeoid);
				addFamilyMember(&operators, member);
				break;
			case OPCLASS_ITEM_FUNCTION:
				if (item->number <= 0 || item->number > maxProcNumber)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("invalid function number %d,"
									" must be between 1 and %d",
									item->number, maxProcNumber)));
				funcOid = LookupFuncWithArgs(OBJECT_FUNCTION, item->name, false);
#ifdef NOT_USED
				/* XXX this is unnecessary given the superuser check above */
				/*
				 *
				 * XXX 上面已有超级用户检查，此处并不必要。
				 */
				/* Caller must own function */
				/*
				 *
				 * 调用方必须拥有该函数。
				 */
				if (!object_ownercheck(ProcedureRelationId, funcOid, GetUserId()))
					aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FUNCTION,
								   get_func_name(funcOid));
#endif
				/* Save the info */
				/*
				 *
				 * 保存该信息。
				 */
				member = (OpFamilyMember *) palloc0(sizeof(OpFamilyMember));
				member->is_func = true;
				member->object = funcOid;
				member->number = item->number;

				/* allow overriding of the function's actual arg types */
				/*
				 *
				 * 允许覆盖函数的实际参数类型。
				 */
				if (item->class_args)
					processTypesSpec(item->class_args,
									 &member->lefttype, &member->righttype);

				assignProcTypes(member, amoid, typeoid, optsProcNumber);
				addFamilyMember(&procedures, member);
				break;
			case OPCLASS_ITEM_STORAGETYPE:
				if (OidIsValid(storageoid))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("storage type specified more than once")));
				storageoid = typenameTypeId(NULL, item->storedtype);

#ifdef NOT_USED
				/* XXX this is unnecessary given the superuser check above */
				/*
				 *
				 * XXX 上面已有超级用户检查，此处并不必要。
				 */
				/* Check we have ownership of the datatype */
				/*
				 *
				 * 检查当前用户是否拥有该数据类型。
				 */
				if (!object_ownercheck(TypeRelationId, storageoid, GetUserId()))
					aclcheck_error_type(ACLCHECK_NOT_OWNER, storageoid);
#endif
				break;
			default:
				elog(ERROR, "unrecognized item type: %d", item->itemtype);
				break;
		}
	}

	/*
	 * If storagetype is specified, make sure it's legal.
	 *
	 * 若指定了 storagetype，则确认它合法。
	 */
	if (OidIsValid(storageoid))
	{
		/* Just drop the spec if same as column datatype */
		/*
		 *
		 * 若与列数据类型相同，则忽略该指定。
		 */
		if (storageoid == typeoid)
			storageoid = InvalidOid;
		else if (!amstorage)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("storage type cannot be different from data type for access method \"%s\"",
							stmt->amname)));
	}

	rel = table_open(OperatorClassRelationId, RowExclusiveLock);

	/*
	 * Make sure there is no existing opclass of this name (this is just to
	 * give a more friendly error message than "duplicate key").
	 *
	 * 确认尚无同名 opclass（只为给出比 duplicate key 更友好的错误信息）。
	 */
	if (SearchSysCacheExists3(CLAAMNAMENSP,
							  ObjectIdGetDatum(amoid),
							  CStringGetDatum(opcname),
							  ObjectIdGetDatum(namespaceoid)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("operator class \"%s\" for access method \"%s\" already exists",
						opcname, stmt->amname)));

	/*
	 * If we are creating a default opclass, check there isn't one already.
	 * (Note we do not restrict this test to visible opclasses; this ensures
	 * that typcache.c can find unique solutions to its questions.)
	 *
	 * 若正在创建默认 opclass，则检查是否已存在。
	 * 此检查不限于可见的 opclass，以便 typcache.c 能得到唯一答案。
	 */
	if (stmt->isDefault)
	{
		ScanKeyData skey[1];
		SysScanDesc scan;

		ScanKeyInit(&skey[0],
					Anum_pg_opclass_opcmethod,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(amoid));

		scan = systable_beginscan(rel, OpclassAmNameNspIndexId, true,
								  NULL, 1, skey);

		while (HeapTupleIsValid(tup = systable_getnext(scan)))
		{
			Form_pg_opclass opclass = (Form_pg_opclass) GETSTRUCT(tup);

			if (opclass->opcintype == typeoid && opclass->opcdefault)
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_OBJECT),
						 errmsg("could not make operator class \"%s\" be default for type %s",
								opcname,
								TypeNameToString(stmt->datatype)),
						 errdetail("Operator class \"%s\" already is the default.",
								   NameStr(opclass->opcname))));
		}

		systable_endscan(scan);
	}

	/*
	 * Okay, let's create the pg_opclass entry.
	 *
	 * 开始创建 pg_opclass 元组。
	 */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	opclassoid = GetNewOidWithIndex(rel, OpclassOidIndexId,
									Anum_pg_opclass_oid);
	values[Anum_pg_opclass_oid - 1] = ObjectIdGetDatum(opclassoid);
	values[Anum_pg_opclass_opcmethod - 1] = ObjectIdGetDatum(amoid);
	namestrcpy(&opcName, opcname);
	values[Anum_pg_opclass_opcname - 1] = NameGetDatum(&opcName);
	values[Anum_pg_opclass_opcnamespace - 1] = ObjectIdGetDatum(namespaceoid);
	values[Anum_pg_opclass_opcowner - 1] = ObjectIdGetDatum(GetUserId());
	values[Anum_pg_opclass_opcfamily - 1] = ObjectIdGetDatum(opfamilyoid);
	values[Anum_pg_opclass_opcintype - 1] = ObjectIdGetDatum(typeoid);
	values[Anum_pg_opclass_opcdefault - 1] = BoolGetDatum(stmt->isDefault);
	values[Anum_pg_opclass_opckeytype - 1] = ObjectIdGetDatum(storageoid);

	tup = heap_form_tuple(rel->rd_att, values, nulls);

	CatalogTupleInsert(rel, tup);

	heap_freetuple(tup);

	/*
	 * Now that we have the opclass OID, set up default dependency info for
	 * the pg_amop and pg_amproc entries.  Historically, CREATE OPERATOR CLASS
	 * has created hard dependencies on the opclass, so that's what we use.
	 *
	 * 已有 opclass OID 后，为 pg_amop 与 pg_amproc 项设置默认依赖。
	 * 历史上 CREATE OPERATOR CLASS 对 opclass 建立硬依赖，这里沿用该做法。
	 */
	foreach(l, operators)
	{
		OpFamilyMember *op = (OpFamilyMember *) lfirst(l);

		op->ref_is_hard = true;
		op->ref_is_family = false;
		op->refobjid = opclassoid;
	}
	foreach(l, procedures)
	{
		OpFamilyMember *proc = (OpFamilyMember *) lfirst(l);

		proc->ref_is_hard = true;
		proc->ref_is_family = false;
		proc->refobjid = opclassoid;
	}

	/*
	 * Let the index AM editorialize on the dependency choices.  It could also
	 * do further validation on the operators and functions, if it likes.
	 *
	 * 让索引访问方法自行决定依赖强度，也可按需进一步校验运算符和函数。
	 */
	if (amroutine->amadjustmembers)
		amroutine->amadjustmembers(opfamilyoid,
								   opclassoid,
								   operators,
								   procedures);

	/*
	 * Now add tuples to pg_amop and pg_amproc tying in the operators and
	 * functions.  Dependencies on them are inserted, too.
	 *
	 * 向 pg_amop 与 pg_amproc 插入元组，把运算符和函数挂上，并写入对它们的依赖。
	 */
	storeOperators(stmt->opfamilyname, amoid, opfamilyoid,
				   operators, false);
	storeProcedures(stmt->opfamilyname, amoid, opfamilyoid,
					procedures, false);

	/* let event triggers know what happened */
	/*
	 *
	 * 通知事件触发器发生了什么。
	 */
	EventTriggerCollectCreateOpClass(stmt, opclassoid, operators, procedures);

	/*
	 * Create dependencies for the opclass proper.  Note: we do not need a
	 * dependency link to the AM, because that exists through the opfamily.
	 *
	 * 为 opclass 本身建立依赖。不必再单独依赖访问方法，因为经由 opfamily 已经存在。
	 */
	myself.classId = OperatorClassRelationId;
	myself.objectId = opclassoid;
	myself.objectSubId = 0;

	/* dependency on namespace */
	/*
	 *
	 * 依赖于命名空间。
	 */
	referenced.classId = NamespaceRelationId;
	referenced.objectId = namespaceoid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	/* dependency on opfamily */
	/*
	 *
	 * 依赖于 opfamily。
	 */
	referenced.classId = OperatorFamilyRelationId;
	referenced.objectId = opfamilyoid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_AUTO);

	/* dependency on indexed datatype */
	/*
	 *
	 * 依赖于被索引的数据类型。
	 */
	referenced.classId = TypeRelationId;
	referenced.objectId = typeoid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	/* dependency on storage datatype */
	/*
	 *
	 * 依赖于存储数据类型。
	 */
	if (OidIsValid(storageoid))
	{
		referenced.classId = TypeRelationId;
		referenced.objectId = storageoid;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	/* dependency on owner */
	/*
	 *
	 * 依赖于属主。
	 */
	recordDependencyOnOwner(OperatorClassRelationId, opclassoid, GetUserId());

	/* dependency on extension */
	/*
	 *
	 * 依赖于扩展。
	 */
	recordDependencyOnCurrentExtension(&myself, false);

	/* Post creation hook for new operator class */
	/*
	 *
	 * 新建 operator class 的创建后钩子。
	 */
	InvokeObjectPostCreateHook(OperatorClassRelationId, opclassoid, 0);

	table_close(rel, RowExclusiveLock);

	return myself;
}


/*
 * DefineOpFamily
 *		Define a new index operator family.
 *
 * 定义新的索引操作符族。
 */
ObjectAddress
DefineOpFamily(CreateOpFamilyStmt *stmt)
{
	char	   *opfname;		/* name of opfamily we're creating */
	/*
	 *
	 * 正在创建的 opfamily 名称。
	 */
	Oid			amoid,			/* our AM's oid */
	/*
	 *
	 * 所属访问方法的 OID。
	 */
				namespaceoid;	/* namespace to create opfamily in */
				/*
				 *
				 * 创建 opfamily 所在的命名空间。
				 */
	AclResult	aclresult;

	/* Convert list of names to a name and namespace */
	/*
	 *
	 * 把名称列表拆成名字和命名空间。
	 */
	namespaceoid = QualifiedNameGetCreationNamespace(stmt->opfamilyname,
													 &opfname);

	/* Check we have creation rights in target namespace */
	/*
	 *
	 * 检查在目标命名空间中是否有创建权限。
	 */
	aclresult = object_aclcheck(NamespaceRelationId, namespaceoid, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
					   get_namespace_name(namespaceoid));

	/* Get access method OID, throwing an error if it doesn't exist. */
	/*
	 *
	 * 取得访问方法 OID；不存在则报错。
	 */
	amoid = get_index_am_oid(stmt->amname, false);

	/* XXX Should we make any privilege check against the AM? */
	/*
	 *
	 * XXX 是否应对访问方法做权限检查？
	 */

	/*
	 * Currently, we require superuser privileges to create an opfamily. See
	 * comments in DefineOpClass.
	 *
	 * 目前创建 opfamily 要求超级用户。原因见 DefineOpClass 的注释。
	 */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to create an operator family")));

	/* Insert pg_opfamily catalog entry */
	/*
	 *
	 * 插入 pg_opfamily 目录项。
	 */
	return CreateOpFamily(stmt, opfname, namespaceoid, amoid);
}


/*
 * AlterOpFamily
 *		Add or remove operators/procedures within an existing operator family.
 *
 * 在已有 operator family 中增加或删除运算符与支持过程。
 *
 * Note: this implements only ALTER OPERATOR FAMILY ... ADD/DROP.  Some
 * other commands called ALTER OPERATOR FAMILY exist, but go through
 * different code paths.
 *
 * 这里只实现 ALTER OPERATOR FAMILY ... ADD/DROP。
 * 其他同名命令走不同的代码路径。
 */
Oid
AlterOpFamily(AlterOpFamilyStmt *stmt)
{
	Oid			amoid,			/* our AM's oid */
	/*
	 *
	 * 所属访问方法的 OID。
	 */
				opfamilyoid;	/* oid of opfamily */
				/*
				 *
				 * opfamily 的 OID。
				 */
	int			maxOpNumber,	/* amstrategies value */
	/*
	 *
	 * amstrategies 的值。
	 */
				optsProcNumber, /* amoptsprocnum value */
				/*
				 *
				 * amoptsprocnum 的值。
				 */
				maxProcNumber;	/* amsupport value */
				/*
				 *
				 * amsupport 的值。
				 */
	HeapTuple	tup;
	Form_pg_am	amform;
	IndexAmRoutine *amroutine;

	/* Get necessary info about access method */
	/*
	 *
	 * 取得访问方法所需的信息。
	 */
	tup = SearchSysCache1(AMNAME, CStringGetDatum(stmt->amname));
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("access method \"%s\" does not exist",
						stmt->amname)));

	amform = (Form_pg_am) GETSTRUCT(tup);
	amoid = amform->oid;
	amroutine = GetIndexAmRoutineByAmId(amoid, false);
	ReleaseSysCache(tup);

	maxOpNumber = amroutine->amstrategies;
	/* if amstrategies is zero, just enforce that op numbers fit in int16 */
	/*
	 *
	 * 若 amstrategies 为 0，则只要求策略号能放入 int16。
	 */
	if (maxOpNumber <= 0)
		maxOpNumber = SHRT_MAX;
	maxProcNumber = amroutine->amsupport;
	optsProcNumber = amroutine->amoptsprocnum;

	/* XXX Should we make any privilege check against the AM? */
	/*
	 *
	 * XXX 是否应对访问方法做权限检查？
	 */

	/* Look up the opfamily */
	/*
	 *
	 * 查找该 opfamily。
	 */
	opfamilyoid = get_opfamily_oid(amoid, stmt->opfamilyname, false);

	/*
	 * Currently, we require superuser privileges to alter an opfamily.
	 *
	 * 目前修改 opfamily 要求超级用户。
	 *
	 * XXX re-enable NOT_USED code sections below if you remove this test.
	 *
	 * XXX 若去掉此检查，请重新启用下面标为 NOT_USED 的代码段。
	 */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to alter an operator family")));

	/*
	 * ADD and DROP cases need separate code from here on down.
	 *
	 * 从这里起，ADD 与 DROP 需要分开处理。
	 */
	if (stmt->isDrop)
		AlterOpFamilyDrop(stmt, amoid, opfamilyoid,
						  maxOpNumber, maxProcNumber, stmt->items);
	else
		AlterOpFamilyAdd(stmt, amoid, opfamilyoid,
						 maxOpNumber, maxProcNumber, optsProcNumber,
						 stmt->items);

	return opfamilyoid;
}

/*
 * ADD part of ALTER OP FAMILY
 *
 * ALTER OP FAMILY 的 ADD 分支。
 */
static void
AlterOpFamilyAdd(AlterOpFamilyStmt *stmt, Oid amoid, Oid opfamilyoid,
				 int maxOpNumber, int maxProcNumber, int optsProcNumber,
				 List *items)
{
	IndexAmRoutine *amroutine = GetIndexAmRoutineByAmId(amoid, false);
	List	   *operators;		/* OpFamilyMember list for operators */
	/*
	 *
	 * 运算符对应的 OpFamilyMember 列表。
	 */
	List	   *procedures;		/* OpFamilyMember list for support procs */
	/*
	 *
	 * 支持函数对应的 OpFamilyMember 列表。
	 */
	ListCell   *l;

	operators = NIL;
	procedures = NIL;

	/*
	 * Scan the "items" list to obtain additional info.
	 *
	 * 扫描 items 列表以取得附加信息。
	 */
	foreach(l, items)
	{
		CreateOpClassItem *item = lfirst_node(CreateOpClassItem, l);
		Oid			operOid;
		Oid			funcOid;
		Oid			sortfamilyOid;
		OpFamilyMember *member;

		switch (item->itemtype)
		{
			case OPCLASS_ITEM_OPERATOR:
				if (item->number <= 0 || item->number > maxOpNumber)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("invalid operator number %d,"
									" must be between 1 and %d",
									item->number, maxOpNumber)));
				if (item->name->objargs != NIL)
					operOid = LookupOperWithArgs(item->name, false);
				else
				{
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("operator argument types must be specified in ALTER OPERATOR FAMILY")));
					operOid = InvalidOid;	/* keep compiler quiet */
					/*
					 *
					 * 避免编译器告警。
					 */
				}

				if (item->order_family)
					sortfamilyOid = get_opfamily_oid(BTREE_AM_OID,
													 item->order_family,
													 false);
				else
					sortfamilyOid = InvalidOid;

#ifdef NOT_USED
				/* XXX this is unnecessary given the superuser check above */
				/*
				 *
				 * XXX 上面已有超级用户检查，此处并不必要。
				 */
				/* Caller must own operator and its underlying function */
				/*
				 *
				 * 调用方必须拥有该运算符及其底层函数。
				 */
				if (!object_ownercheck(OperatorRelationId, operOid, GetUserId()))
					aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_OPERATOR,
								   get_opname(operOid));
				funcOid = get_opcode(operOid);
				if (!object_ownercheck(ProcedureRelationId, funcOid, GetUserId()))
					aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FUNCTION,
								   get_func_name(funcOid));
#endif

				/* Save the info */
				/*
				 *
				 * 保存该信息。
				 */
				member = (OpFamilyMember *) palloc0(sizeof(OpFamilyMember));
				member->is_func = false;
				member->object = operOid;
				member->number = item->number;
				member->sortfamily = sortfamilyOid;
				/* We can set up dependency fields immediately */
				/*
				 *
				 * 可以立刻填好依赖相关字段。
				 */
				/* Historically, ALTER ADD has created soft dependencies */
				/*
				 *
				 * 历史上 ALTER ADD 建立的是软依赖。
				 */
				member->ref_is_hard = false;
				member->ref_is_family = true;
				member->refobjid = opfamilyoid;
				assignOperTypes(member, amoid, InvalidOid);
				addFamilyMember(&operators, member);
				break;
			case OPCLASS_ITEM_FUNCTION:
				if (item->number <= 0 || item->number > maxProcNumber)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("invalid function number %d,"
									" must be between 1 and %d",
									item->number, maxProcNumber)));
				funcOid = LookupFuncWithArgs(OBJECT_FUNCTION, item->name, false);
#ifdef NOT_USED
				/* XXX this is unnecessary given the superuser check above */
				/*
				 *
				 * XXX 上面已有超级用户检查，此处并不必要。
				 */
				/* Caller must own function */
				/*
				 *
				 * 调用方必须拥有该函数。
				 */
				if (!object_ownercheck(ProcedureRelationId, funcOid, GetUserId()))
					aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FUNCTION,
								   get_func_name(funcOid));
#endif

				/* Save the info */
				/*
				 *
				 * 保存该信息。
				 */
				member = (OpFamilyMember *) palloc0(sizeof(OpFamilyMember));
				member->is_func = true;
				member->object = funcOid;
				member->number = item->number;
				/* We can set up dependency fields immediately */
				/*
				 *
				 * 可以立刻填好依赖相关字段。
				 */
				/* Historically, ALTER ADD has created soft dependencies */
				/*
				 *
				 * 历史上 ALTER ADD 建立的是软依赖。
				 */
				member->ref_is_hard = false;
				member->ref_is_family = true;
				member->refobjid = opfamilyoid;

				/* allow overriding of the function's actual arg types */
				/*
				 *
				 * 允许覆盖函数的实际参数类型。
				 */
				if (item->class_args)
					processTypesSpec(item->class_args,
									 &member->lefttype, &member->righttype);

				assignProcTypes(member, amoid, InvalidOid, optsProcNumber);
				addFamilyMember(&procedures, member);
				break;
			case OPCLASS_ITEM_STORAGETYPE:
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("STORAGE cannot be specified in ALTER OPERATOR FAMILY")));
				break;
			default:
				elog(ERROR, "unrecognized item type: %d", item->itemtype);
				break;
		}
	}

	/*
	 * Let the index AM editorialize on the dependency choices.  It could also
	 * do further validation on the operators and functions, if it likes.
	 *
	 * 让索引访问方法自行决定依赖强度，也可按需进一步校验运算符和函数。
	 */
	if (amroutine->amadjustmembers)
		amroutine->amadjustmembers(opfamilyoid,
								   InvalidOid,	/* no specific opclass */
								   /*
								    *
								    * 不针对某个具体 opclass。
								    */
								   operators,
								   procedures);

	/*
	 * Add tuples to pg_amop and pg_amproc tying in the operators and
	 * functions.  Dependencies on them are inserted, too.
	 *
	 * 向 pg_amop 与 pg_amproc 插入元组，把运算符和函数挂上，并写入对它们的依赖。
	 */
	storeOperators(stmt->opfamilyname, amoid, opfamilyoid,
				   operators, true);
	storeProcedures(stmt->opfamilyname, amoid, opfamilyoid,
					procedures, true);

	/* make information available to event triggers */
	/*
	 *
	 * 把信息提供给事件触发器。
	 */
	EventTriggerCollectAlterOpFam(stmt, opfamilyoid,
								  operators, procedures);
}

/*
 * DROP part of ALTER OP FAMILY
 *
 * ALTER OP FAMILY 的 DROP 分支。
 */
static void
AlterOpFamilyDrop(AlterOpFamilyStmt *stmt, Oid amoid, Oid opfamilyoid,
				  int maxOpNumber, int maxProcNumber, List *items)
{
	List	   *operators;		/* OpFamilyMember list for operators */
	/*
	 *
	 * 运算符对应的 OpFamilyMember 列表。
	 */
	List	   *procedures;		/* OpFamilyMember list for support procs */
	/*
	 *
	 * 支持函数对应的 OpFamilyMember 列表。
	 */
	ListCell   *l;

	operators = NIL;
	procedures = NIL;

	/*
	 * Scan the "items" list to obtain additional info.
	 *
	 * 扫描 items 列表以取得附加信息。
	 */
	foreach(l, items)
	{
		CreateOpClassItem *item = lfirst_node(CreateOpClassItem, l);
		Oid			lefttype,
					righttype;
		OpFamilyMember *member;

		switch (item->itemtype)
		{
			case OPCLASS_ITEM_OPERATOR:
				if (item->number <= 0 || item->number > maxOpNumber)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("invalid operator number %d,"
									" must be between 1 and %d",
									item->number, maxOpNumber)));
				processTypesSpec(item->class_args, &lefttype, &righttype);
				/* Save the info */
				/*
				 *
				 * 保存该信息。
				 */
				member = (OpFamilyMember *) palloc0(sizeof(OpFamilyMember));
				member->is_func = false;
				member->number = item->number;
				member->lefttype = lefttype;
				member->righttype = righttype;
				addFamilyMember(&operators, member);
				break;
			case OPCLASS_ITEM_FUNCTION:
				if (item->number <= 0 || item->number > maxProcNumber)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("invalid function number %d,"
									" must be between 1 and %d",
									item->number, maxProcNumber)));
				processTypesSpec(item->class_args, &lefttype, &righttype);
				/* Save the info */
				/*
				 *
				 * 保存该信息。
				 */
				member = (OpFamilyMember *) palloc0(sizeof(OpFamilyMember));
				member->is_func = true;
				member->number = item->number;
				member->lefttype = lefttype;
				member->righttype = righttype;
				addFamilyMember(&procedures, member);
				break;
			case OPCLASS_ITEM_STORAGETYPE:
				/* grammar prevents this from appearing */
				/*
				 *
				 * 语法不允许出现这种情况。
				 */
			default:
				elog(ERROR, "unrecognized item type: %d", item->itemtype);
				break;
		}
	}

	/*
	 * Remove tuples from pg_amop and pg_amproc.
	 *
	 * 从 pg_amop 与 pg_amproc 删除元组。
	 */
	dropOperators(stmt->opfamilyname, amoid, opfamilyoid, operators);
	dropProcedures(stmt->opfamilyname, amoid, opfamilyoid, procedures);

	/* make information available to event triggers */
	/*
	 *
	 * 把信息提供给事件触发器。
	 */
	EventTriggerCollectAlterOpFam(stmt, opfamilyoid,
								  operators, procedures);
}


/*
 * Deal with explicit arg types used in ALTER ADD/DROP
 *
 * 处理 ALTER ADD/DROP 中显式给出的参数类型。
 */
static void
processTypesSpec(List *args, Oid *lefttype, Oid *righttype)
{
	TypeName   *typeName;

	Assert(args != NIL);

	typeName = (TypeName *) linitial(args);
	*lefttype = typenameTypeId(NULL, typeName);

	if (list_length(args) > 1)
	{
		typeName = (TypeName *) lsecond(args);
		*righttype = typenameTypeId(NULL, typeName);
	}
	else
		*righttype = *lefttype;

	if (list_length(args) > 2)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("one or two argument types must be specified")));
}


/*
 * Determine the lefttype/righttype to assign to an operator,
 * and do any validity checking we can manage.
 *
 * 决定赋给运算符的 lefttype/righttype，并做力所能及的合法性检查。
 */
static void
assignOperTypes(OpFamilyMember *member, Oid amoid, Oid typeoid)
{
	Operator	optup;
	Form_pg_operator opform;

	/* Fetch the operator definition */
	/*
	 *
	 * 取出运算符定义。
	 */
	optup = SearchSysCache1(OPEROID, ObjectIdGetDatum(member->object));
	if (!HeapTupleIsValid(optup))
		elog(ERROR, "cache lookup failed for operator %u", member->object);
	opform = (Form_pg_operator) GETSTRUCT(optup);

	/*
	 * Opfamily operators must be binary.
	 *
	 * opfamily 中的运算符必须是二元的。
	 */
	if (opform->oprkind != 'b')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("index operators must be binary")));

	if (OidIsValid(member->sortfamily))
	{
		/*
		 * Ordering op, check index supports that.  (We could perhaps also
		 * check that the operator returns a type supported by the sortfamily,
		 * but that seems more trouble than it's worth here.  If it does not,
		 * the operator will never be matchable to any ORDER BY clause, but no
		 * worse consequences can ensue.  Also, trying to check that would
		 * create an ordering hazard during dump/reload: it's possible that
		 * the family has been created but not yet populated with the required
		 * operators.)
		 *
		 * 排序运算符要检查索引是否支持。也可以再检查返回类型是否被 sortfamily 支持，
		 * 但在这里不值得。若不支持，该运算符只是无法匹配任何 ORDER BY，不会有更坏后果。
		 * 而且检查会在转储/恢复时造成顺序问题：family 可能已创建，但所需运算符尚未填入。
		 */
		IndexAmRoutine *amroutine = GetIndexAmRoutineByAmId(amoid, false);

		if (!amroutine->amcanorderbyop)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("access method \"%s\" does not support ordering operators",
							get_am_name(amoid))));
	}
	else
	{
		/*
		 * Search operators must return boolean.
		 *
		 * 搜索运算符必须返回 boolean。
		 */
		if (opform->oprresult != BOOLOID)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("index search operators must return boolean")));
	}

	/*
	 * If lefttype/righttype isn't specified, use the operator's input types
	 *
	 * 若未指定 lefttype/righttype，则使用运算符的输入类型。
	 */
	if (!OidIsValid(member->lefttype))
		member->lefttype = opform->oprleft;
	if (!OidIsValid(member->righttype))
		member->righttype = opform->oprright;

	ReleaseSysCache(optup);
}

/*
 * Determine the lefttype/righttype to assign to a support procedure,
 * and do any validity checking we can manage.
 *
 * 决定赋给支持过程的 lefttype/righttype，并做力所能及的合法性检查。
 */
static void
assignProcTypes(OpFamilyMember *member, Oid amoid, Oid typeoid,
				int opclassOptsProcNum)
{
	HeapTuple	proctup;
	Form_pg_proc procform;

	/* Fetch the procedure definition */
	/*
	 *
	 * 取出过程定义。
	 */
	proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(member->object));
	if (!HeapTupleIsValid(proctup))
		elog(ERROR, "cache lookup failed for function %u", member->object);
	procform = (Form_pg_proc) GETSTRUCT(proctup);

	/* Check the signature of the opclass options parsing function */
	/*
	 *
	 * 检查 opclass 选项解析函数的签名。
	 */
	if (member->number == opclassOptsProcNum)
	{
		if (OidIsValid(typeoid))
		{
			if ((OidIsValid(member->lefttype) && member->lefttype != typeoid) ||
				(OidIsValid(member->righttype) && member->righttype != typeoid))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("associated data types for operator class options parsing functions must match opclass input type")));
		}
		else
		{
			if (member->lefttype != member->righttype)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("left and right associated data types for operator class options parsing functions must match")));
		}

		if (procform->prorettype != VOIDOID ||
			procform->pronargs != 1 ||
			procform->proargtypes.values[0] != INTERNALOID)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("invalid operator class options parsing function"),
					 errhint("Valid signature of operator class options parsing function is %s.",
							 "(internal) RETURNS void")));
	}

	/*
	 * Ordering comparison procs must be 2-arg procs returning int4.  Ordering
	 * sortsupport procs must take internal and return void.  Ordering
	 * in_range procs must be 5-arg procs returning bool.  Ordering equalimage
	 * procs must take 1 arg and return bool.  Hashing support proc 1 must be
	 * a 1-arg proc returning int4, while proc 2 must be a 2-arg proc
	 * returning int8. Otherwise we don't know.
	 *
	 * 排序比较过程必须是返回 int4 的二元过程。排序 sortsupport 过程接收 internal 并返回 void。
	 * 排序 in_range 过程必须是返回 bool 的五元过程。排序 equalimage 过程接收 1 个参数并返回 bool。
	 * 哈希支持过程 1 必须是返回 int4 的一元过程，过程 2 必须是返回 int8 的二元过程。
	 * 其余情况无法判断。
	 */
	else if (GetIndexAmRoutineByAmId(amoid, false)->amcanorder)
	{
		if (member->number == BTORDER_PROC)
		{
			if (procform->pronargs != 2)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("ordering comparison functions must have two arguments")));
			if (procform->prorettype != INT4OID)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("ordering comparison functions must return integer")));

			/*
			 * If lefttype/righttype isn't specified, use the proc's input
			 * types
			 *
			 * 若未指定 lefttype/righttype，则使用该过程的输入类型。
			 */
			if (!OidIsValid(member->lefttype))
				member->lefttype = procform->proargtypes.values[0];
			if (!OidIsValid(member->righttype))
				member->righttype = procform->proargtypes.values[1];
		}
		else if (member->number == BTSORTSUPPORT_PROC)
		{
			if (procform->pronargs != 1 ||
				procform->proargtypes.values[0] != INTERNALOID)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("ordering sort support functions must accept type \"internal\"")));
			if (procform->prorettype != VOIDOID)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("ordering sort support functions must return void")));

			/*
			 * Can't infer lefttype/righttype from proc, so use default rule
			 *
			 * 无法从过程推断 lefttype/righttype，因此使用默认规则。
			 */
		}
		else if (member->number == BTINRANGE_PROC)
		{
			if (procform->pronargs != 5)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("ordering in_range functions must have five arguments")));
			if (procform->prorettype != BOOLOID)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("ordering in_range functions must return boolean")));

			/*
			 * If lefttype/righttype isn't specified, use the proc's input
			 * types (we look at the test-value and offset arguments)
			 *
			 * 若未指定 lefttype/righttype，则使用该过程的输入类型（查看 test-value 与 offset 参数）。
			 */
			if (!OidIsValid(member->lefttype))
				member->lefttype = procform->proargtypes.values[0];
			if (!OidIsValid(member->righttype))
				member->righttype = procform->proargtypes.values[2];
		}
		else if (member->number == BTEQUALIMAGE_PROC)
		{
			if (procform->pronargs != 1)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("ordering equal image functions must have one argument")));
			if (procform->prorettype != BOOLOID)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("ordering equal image functions must return boolean")));

			/*
			 * pg_amproc functions are indexed by (lefttype, righttype), but
			 * an equalimage function can only be called at CREATE INDEX time.
			 * The same opclass opcintype OID is always used for lefttype and
			 * righttype.  Providing a cross-type routine isn't sensible.
			 * Reject cross-type ALTER OPERATOR FAMILY ...  ADD FUNCTION 4
			 * statements here.
			 *
			 * pg_amproc 函数按 (lefttype, righttype) 索引，但 equalimage 函数只在 CREATE INDEX 时调用。
			 * lefttype 与 righttype 始终使用同一个 opclass 的 opcintype OID。
			 * 提供跨类型例程没有意义。此处拒绝跨类型的
			 * ALTER OPERATOR FAMILY ... ADD FUNCTION 4。
			 */
			if (member->lefttype != member->righttype)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("ordering equal image functions must not be cross-type")));
		}
		else if (member->number == BTSKIPSUPPORT_PROC)
		{
			if (procform->pronargs != 1 ||
				procform->proargtypes.values[0] != INTERNALOID)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("btree skip support functions must accept type \"internal\"")));
			if (procform->prorettype != VOIDOID)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("btree skip support functions must return void")));

			/*
			 * pg_amproc functions are indexed by (lefttype, righttype), but a
			 * skip support function doesn't make sense in cross-type
			 * scenarios.  The same opclass opcintype OID is always used for
			 * lefttype and righttype.  Providing a cross-type routine isn't
			 * sensible.  Reject cross-type ALTER OPERATOR FAMILY ...  ADD
			 * FUNCTION 6 statements here.
			 *
			 * pg_amproc 函数按 (lefttype, righttype) 索引，但 skip 支持函数在跨类型场景下没有意义。
			 * lefttype 与 righttype 始终使用同一个 opclass 的 opcintype OID。
			 * 提供跨类型例程没有意义。此处拒绝跨类型的
			 * ALTER OPERATOR FAMILY ... ADD FUNCTION 6。
			 */
			if (member->lefttype != member->righttype)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("btree skip support functions must not be cross-type")));
		}
	}
	else if (GetIndexAmRoutineByAmId(amoid, false)->amcanhash)
	{
		if (member->number == HASHSTANDARD_PROC)
		{
			if (procform->pronargs != 1)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("hash function 1 must have one argument")));
			if (procform->prorettype != INT4OID)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("hash function 1 must return integer")));
		}
		else if (member->number == HASHEXTENDED_PROC)
		{
			if (procform->pronargs != 2)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("hash function 2 must have two arguments")));
			if (procform->prorettype != INT8OID)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("hash function 2 must return bigint")));
		}

		/*
		 * If lefttype/righttype isn't specified, use the proc's input type
		 *
		 * 若未指定 lefttype/righttype，则使用该过程的输入类型。
		 */
		if (!OidIsValid(member->lefttype))
			member->lefttype = procform->proargtypes.values[0];
		if (!OidIsValid(member->righttype))
			member->righttype = procform->proargtypes.values[0];
	}

	/*
	 * The default in CREATE OPERATOR CLASS is to use the class' opcintype as
	 * lefttype and righttype.  In CREATE or ALTER OPERATOR FAMILY, opcintype
	 * isn't available, so make the user specify the types.
	 *
	 * CREATE OPERATOR CLASS 默认用本类的 opcintype 作为 lefttype 和 righttype。
	 * CREATE 或 ALTER OPERATOR FAMILY 时没有 opcintype，因此必须由用户指定类型。
	 */
	if (!OidIsValid(member->lefttype))
		member->lefttype = typeoid;
	if (!OidIsValid(member->righttype))
		member->righttype = typeoid;

	if (!OidIsValid(member->lefttype) || !OidIsValid(member->righttype))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("associated data types must be specified for index support function")));

	ReleaseSysCache(proctup);
}

/*
 * Add a new family member to the appropriate list, after checking for
 * duplicated strategy or proc number.
 *
 * 检查策略号或过程号没有重复后，把新的 family 成员加入相应列表。
 */
static void
addFamilyMember(List **list, OpFamilyMember *member)
{
	ListCell   *l;

	foreach(l, *list)
	{
		OpFamilyMember *old = (OpFamilyMember *) lfirst(l);

		if (old->number == member->number &&
			old->lefttype == member->lefttype &&
			old->righttype == member->righttype)
		{
			if (member->is_func)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("function number %d for (%s,%s) appears more than once",
								member->number,
								format_type_be(member->lefttype),
								format_type_be(member->righttype))));
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("operator number %d for (%s,%s) appears more than once",
								member->number,
								format_type_be(member->lefttype),
								format_type_be(member->righttype))));
		}
	}
	*list = lappend(*list, member);
}

/*
 * Dump the operators to pg_amop
 *
 * 把运算符写入 pg_amop。
 *
 * We also make dependency entries in pg_depend for the pg_amop entries.
 *
 * 同时为这些 pg_amop 项在 pg_depend 中建立依赖。
 */
static void
storeOperators(List *opfamilyname, Oid amoid, Oid opfamilyoid,
			   List *operators, bool isAdd)
{
	Relation	rel;
	Datum		values[Natts_pg_amop];
	bool		nulls[Natts_pg_amop];
	HeapTuple	tup;
	Oid			entryoid;
	ObjectAddress myself,
				referenced;
	ListCell   *l;

	rel = table_open(AccessMethodOperatorRelationId, RowExclusiveLock);

	foreach(l, operators)
	{
		OpFamilyMember *op = (OpFamilyMember *) lfirst(l);
		char		oppurpose;

		/*
		 * If adding to an existing family, check for conflict with an
		 * existing pg_amop entry (just to give a nicer error message)
		 *
		 * 若向已有 family 添加，则检查是否与现有 pg_amop 项冲突（只为给出更清晰的错误信息）。
		 */
		if (isAdd &&
			SearchSysCacheExists4(AMOPSTRATEGY,
								  ObjectIdGetDatum(opfamilyoid),
								  ObjectIdGetDatum(op->lefttype),
								  ObjectIdGetDatum(op->righttype),
								  Int16GetDatum(op->number)))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("operator %d(%s,%s) already exists in operator family \"%s\"",
							op->number,
							format_type_be(op->lefttype),
							format_type_be(op->righttype),
							NameListToString(opfamilyname))));

		oppurpose = OidIsValid(op->sortfamily) ? AMOP_ORDER : AMOP_SEARCH;

		/* Create the pg_amop entry */
		/*
		 *
		 * 创建 pg_amop 元组。
		 */
		memset(values, 0, sizeof(values));
		memset(nulls, false, sizeof(nulls));

		entryoid = GetNewOidWithIndex(rel, AccessMethodOperatorOidIndexId,
									  Anum_pg_amop_oid);
		values[Anum_pg_amop_oid - 1] = ObjectIdGetDatum(entryoid);
		values[Anum_pg_amop_amopfamily - 1] = ObjectIdGetDatum(opfamilyoid);
		values[Anum_pg_amop_amoplefttype - 1] = ObjectIdGetDatum(op->lefttype);
		values[Anum_pg_amop_amoprighttype - 1] = ObjectIdGetDatum(op->righttype);
		values[Anum_pg_amop_amopstrategy - 1] = Int16GetDatum(op->number);
		values[Anum_pg_amop_amoppurpose - 1] = CharGetDatum(oppurpose);
		values[Anum_pg_amop_amopopr - 1] = ObjectIdGetDatum(op->object);
		values[Anum_pg_amop_amopmethod - 1] = ObjectIdGetDatum(amoid);
		values[Anum_pg_amop_amopsortfamily - 1] = ObjectIdGetDatum(op->sortfamily);

		tup = heap_form_tuple(rel->rd_att, values, nulls);

		CatalogTupleInsert(rel, tup);

		heap_freetuple(tup);

		/* Make its dependencies */
		/*
		 *
		 * 建立它的依赖。
		 */
		myself.classId = AccessMethodOperatorRelationId;
		myself.objectId = entryoid;
		myself.objectSubId = 0;

		referenced.classId = OperatorRelationId;
		referenced.objectId = op->object;
		referenced.objectSubId = 0;

		/* see comments in amapi.h about dependency strength */
		/*
		 *
		 * 依赖强度见 amapi.h 中的注释。
		 */
		recordDependencyOn(&myself, &referenced,
						   op->ref_is_hard ? DEPENDENCY_NORMAL : DEPENDENCY_AUTO);

		referenced.classId = op->ref_is_family ? OperatorFamilyRelationId :
			OperatorClassRelationId;
		referenced.objectId = op->refobjid;
		referenced.objectSubId = 0;

		recordDependencyOn(&myself, &referenced,
						   op->ref_is_hard ? DEPENDENCY_INTERNAL : DEPENDENCY_AUTO);

		if (typeDepNeeded(op->lefttype, op))
		{
			referenced.classId = TypeRelationId;
			referenced.objectId = op->lefttype;
			referenced.objectSubId = 0;

			/* see comments in amapi.h about dependency strength */
			/*
			 *
			 * 依赖强度见 amapi.h 中的注释。
			 */
			recordDependencyOn(&myself, &referenced,
							   op->ref_is_hard ? DEPENDENCY_NORMAL : DEPENDENCY_AUTO);
		}

		if (op->lefttype != op->righttype &&
			typeDepNeeded(op->righttype, op))
		{
			referenced.classId = TypeRelationId;
			referenced.objectId = op->righttype;
			referenced.objectSubId = 0;

			/* see comments in amapi.h about dependency strength */
			/*
			 *
			 * 依赖强度见 amapi.h 中的注释。
			 */
			recordDependencyOn(&myself, &referenced,
							   op->ref_is_hard ? DEPENDENCY_NORMAL : DEPENDENCY_AUTO);
		}

		/* A search operator also needs a dep on the referenced opfamily */
		/*
		 *
		 * 搜索运算符还需要依赖所引用的 opfamily。
		 */
		if (OidIsValid(op->sortfamily))
		{
			referenced.classId = OperatorFamilyRelationId;
			referenced.objectId = op->sortfamily;
			referenced.objectSubId = 0;

			recordDependencyOn(&myself, &referenced,
							   op->ref_is_hard ? DEPENDENCY_NORMAL : DEPENDENCY_AUTO);
		}

		/* Post create hook of this access method operator */
		/*
		 *
		 * 该访问方法运算符的创建后钩子。
		 */
		InvokeObjectPostCreateHook(AccessMethodOperatorRelationId,
								   entryoid, 0);
	}

	table_close(rel, RowExclusiveLock);
}

/*
 * Dump the procedures (support routines) to pg_amproc
 *
 * 把支持过程写入 pg_amproc。
 *
 * We also make dependency entries in pg_depend for the pg_amproc entries.
 *
 * 同时为这些 pg_amproc 项在 pg_depend 中建立依赖。
 */
static void
storeProcedures(List *opfamilyname, Oid amoid, Oid opfamilyoid,
				List *procedures, bool isAdd)
{
	Relation	rel;
	Datum		values[Natts_pg_amproc];
	bool		nulls[Natts_pg_amproc];
	HeapTuple	tup;
	Oid			entryoid;
	ObjectAddress myself,
				referenced;
	ListCell   *l;

	rel = table_open(AccessMethodProcedureRelationId, RowExclusiveLock);

	foreach(l, procedures)
	{
		OpFamilyMember *proc = (OpFamilyMember *) lfirst(l);

		/*
		 * If adding to an existing family, check for conflict with an
		 * existing pg_amproc entry (just to give a nicer error message)
		 *
		 * 若向已有 family 添加，则检查是否与现有 pg_amproc 项冲突（只为给出更清晰的错误信息）。
		 */
		if (isAdd &&
			SearchSysCacheExists4(AMPROCNUM,
								  ObjectIdGetDatum(opfamilyoid),
								  ObjectIdGetDatum(proc->lefttype),
								  ObjectIdGetDatum(proc->righttype),
								  Int16GetDatum(proc->number)))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("function %d(%s,%s) already exists in operator family \"%s\"",
							proc->number,
							format_type_be(proc->lefttype),
							format_type_be(proc->righttype),
							NameListToString(opfamilyname))));

		/* Create the pg_amproc entry */
		/*
		 *
		 * 创建 pg_amproc 元组。
		 */
		memset(values, 0, sizeof(values));
		memset(nulls, false, sizeof(nulls));

		entryoid = GetNewOidWithIndex(rel, AccessMethodProcedureOidIndexId,
									  Anum_pg_amproc_oid);
		values[Anum_pg_amproc_oid - 1] = ObjectIdGetDatum(entryoid);
		values[Anum_pg_amproc_amprocfamily - 1] = ObjectIdGetDatum(opfamilyoid);
		values[Anum_pg_amproc_amproclefttype - 1] = ObjectIdGetDatum(proc->lefttype);
		values[Anum_pg_amproc_amprocrighttype - 1] = ObjectIdGetDatum(proc->righttype);
		values[Anum_pg_amproc_amprocnum - 1] = Int16GetDatum(proc->number);
		values[Anum_pg_amproc_amproc - 1] = ObjectIdGetDatum(proc->object);

		tup = heap_form_tuple(rel->rd_att, values, nulls);

		CatalogTupleInsert(rel, tup);

		heap_freetuple(tup);

		/* Make its dependencies */
		/*
		 *
		 * 建立它的依赖。
		 */
		myself.classId = AccessMethodProcedureRelationId;
		myself.objectId = entryoid;
		myself.objectSubId = 0;

		referenced.classId = ProcedureRelationId;
		referenced.objectId = proc->object;
		referenced.objectSubId = 0;

		/* see comments in amapi.h about dependency strength */
		/*
		 *
		 * 依赖强度见 amapi.h 中的注释。
		 */
		recordDependencyOn(&myself, &referenced,
						   proc->ref_is_hard ? DEPENDENCY_NORMAL : DEPENDENCY_AUTO);

		referenced.classId = proc->ref_is_family ? OperatorFamilyRelationId :
			OperatorClassRelationId;
		referenced.objectId = proc->refobjid;
		referenced.objectSubId = 0;

		recordDependencyOn(&myself, &referenced,
						   proc->ref_is_hard ? DEPENDENCY_INTERNAL : DEPENDENCY_AUTO);

		if (typeDepNeeded(proc->lefttype, proc))
		{
			referenced.classId = TypeRelationId;
			referenced.objectId = proc->lefttype;
			referenced.objectSubId = 0;

			/* see comments in amapi.h about dependency strength */
			/*
			 *
			 * 依赖强度见 amapi.h 中的注释。
			 */
			recordDependencyOn(&myself, &referenced,
							   proc->ref_is_hard ? DEPENDENCY_NORMAL : DEPENDENCY_AUTO);
		}

		if (proc->lefttype != proc->righttype &&
			typeDepNeeded(proc->righttype, proc))
		{
			referenced.classId = TypeRelationId;
			referenced.objectId = proc->righttype;
			referenced.objectSubId = 0;

			/* see comments in amapi.h about dependency strength */
			/*
			 *
			 * 依赖强度见 amapi.h 中的注释。
			 */
			recordDependencyOn(&myself, &referenced,
							   proc->ref_is_hard ? DEPENDENCY_NORMAL : DEPENDENCY_AUTO);
		}

		/* Post create hook of access method procedure */
		/*
		 *
		 * 访问方法支持过程的创建后钩子。
		 */
		InvokeObjectPostCreateHook(AccessMethodProcedureRelationId,
								   entryoid, 0);
	}

	table_close(rel, RowExclusiveLock);
}

/*
 * Detect whether a pg_amop or pg_amproc entry needs an explicit dependency
 * on its lefttype or righttype.
 *
 * 判断 pg_amop 或 pg_amproc 项是否需要显式依赖其 lefttype 或 righttype。
 *
 * We make such a dependency unless the entry has an indirect dependency
 * via its referenced operator or function.  That's nearly always true
 * for operators, but might well not be true for support functions.
 *
 * 除非该项已通过所引用的运算符或函数间接依赖该类型，否则建立这种依赖。
 * 运算符几乎总是如此，支持函数则未必。
 */
static bool
typeDepNeeded(Oid typid, OpFamilyMember *member)
{
	bool		result = true;

	/*
	 * If the type is pinned, we don't need a dependency.  This is a bit of a
	 * layering violation perhaps (recordDependencyOn would ignore the request
	 * anyway), but it's a cheap test and will frequently save a syscache
	 * lookup here.
	 *
	 * 若该类型已被钉住，则不需要依赖。这或许略微破坏分层
	 * （recordDependencyOn 本来也会忽略该请求），但检查很便宜，常能省一次 syscache 查找。
	 */
	if (IsPinnedObject(TypeRelationId, typid))
		return false;

	/* Nope, so check the input types of the function or operator. */
	/*
	 *
	 * 并非如此，因此检查函数或运算符的输入类型。
	 */
	if (member->is_func)
	{
		Oid		   *argtypes;
		int			nargs;

		(void) get_func_signature(member->object, &argtypes, &nargs);
		for (int i = 0; i < nargs; i++)
		{
			if (typid == argtypes[i])
			{
				result = false; /* match, no dependency needed */
				/*
				 *
				 * 匹配成功，不需要依赖。
				 */
				break;
			}
		}
		pfree(argtypes);
	}
	else
	{
		Oid			lefttype,
					righttype;

		op_input_types(member->object, &lefttype, &righttype);
		if (typid == lefttype || typid == righttype)
			result = false;		/* match, no dependency needed */
			/*
			 *
			 * 匹配成功，不需要依赖。
			 */
	}
	return result;
}


/*
 * Remove operator entries from an opfamily.
 *
 * 从 opfamily 中删除运算符项。
 *
 * Note: this is only allowed for "loose" members of an opfamily, hence
 * behavior is always RESTRICT.
 *
 * 只允许删除 opfamily 的松散成员，因此行为始终是 RESTRICT。
 */
static void
dropOperators(List *opfamilyname, Oid amoid, Oid opfamilyoid,
			  List *operators)
{
	ListCell   *l;

	foreach(l, operators)
	{
		OpFamilyMember *op = (OpFamilyMember *) lfirst(l);
		Oid			amopid;
		ObjectAddress object;

		amopid = GetSysCacheOid4(AMOPSTRATEGY, Anum_pg_amop_oid,
								 ObjectIdGetDatum(opfamilyoid),
								 ObjectIdGetDatum(op->lefttype),
								 ObjectIdGetDatum(op->righttype),
								 Int16GetDatum(op->number));
		if (!OidIsValid(amopid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("operator %d(%s,%s) does not exist in operator family \"%s\"",
							op->number,
							format_type_be(op->lefttype),
							format_type_be(op->righttype),
							NameListToString(opfamilyname))));

		object.classId = AccessMethodOperatorRelationId;
		object.objectId = amopid;
		object.objectSubId = 0;

		performDeletion(&object, DROP_RESTRICT, 0);
	}
}

/*
 * Remove procedure entries from an opfamily.
 *
 * 从 opfamily 中删除支持过程项。
 *
 * Note: this is only allowed for "loose" members of an opfamily, hence
 * behavior is always RESTRICT.
 *
 * 只允许删除 opfamily 的松散成员，因此行为始终是 RESTRICT。
 */
static void
dropProcedures(List *opfamilyname, Oid amoid, Oid opfamilyoid,
			   List *procedures)
{
	ListCell   *l;

	foreach(l, procedures)
	{
		OpFamilyMember *op = (OpFamilyMember *) lfirst(l);
		Oid			amprocid;
		ObjectAddress object;

		amprocid = GetSysCacheOid4(AMPROCNUM, Anum_pg_amproc_oid,
								   ObjectIdGetDatum(opfamilyoid),
								   ObjectIdGetDatum(op->lefttype),
								   ObjectIdGetDatum(op->righttype),
								   Int16GetDatum(op->number));
		if (!OidIsValid(amprocid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("function %d(%s,%s) does not exist in operator family \"%s\"",
							op->number,
							format_type_be(op->lefttype),
							format_type_be(op->righttype),
							NameListToString(opfamilyname))));

		object.classId = AccessMethodProcedureRelationId;
		object.objectId = amprocid;
		object.objectSubId = 0;

		performDeletion(&object, DROP_RESTRICT, 0);
	}
}

/*
 * Subroutine for ALTER OPERATOR CLASS SET SCHEMA/RENAME
 *
 * ALTER OPERATOR CLASS SET SCHEMA/RENAME 的子例程。
 *
 * Is there an operator class with the given name and signature already
 * in the given namespace?	If so, raise an appropriate error message.
 *
 * 给定命名空间中是否已有同名且同签名的 operator class？若有则报出相应错误。
 */
void
IsThereOpClassInNamespace(const char *opcname, Oid opcmethod,
						  Oid opcnamespace)
{
	/* make sure the new name doesn't exist */
	/*
	 *
	 * 确认新名称尚不存在。
	 */
	if (SearchSysCacheExists3(CLAAMNAMENSP,
							  ObjectIdGetDatum(opcmethod),
							  CStringGetDatum(opcname),
							  ObjectIdGetDatum(opcnamespace)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("operator class \"%s\" for access method \"%s\" already exists in schema \"%s\"",
						opcname,
						get_am_name(opcmethod),
						get_namespace_name(opcnamespace))));
}

/*
 * Subroutine for ALTER OPERATOR FAMILY SET SCHEMA/RENAME
 *
 * ALTER OPERATOR FAMILY SET SCHEMA/RENAME 的子例程。
 *
 * Is there an operator family with the given name and signature already
 * in the given namespace?	If so, raise an appropriate error message.
 *
 * 给定命名空间中是否已有同名且同签名的 operator family？若有则报出相应错误。
 */
void
IsThereOpFamilyInNamespace(const char *opfname, Oid opfmethod,
						   Oid opfnamespace)
{
	/* make sure the new name doesn't exist */
	/*
	 *
	 * 确认新名称尚不存在。
	 */
	if (SearchSysCacheExists3(OPFAMILYAMNAMENSP,
							  ObjectIdGetDatum(opfmethod),
							  CStringGetDatum(opfname),
							  ObjectIdGetDatum(opfnamespace)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("operator family \"%s\" for access method \"%s\" already exists in schema \"%s\"",
						opfname,
						get_am_name(opfmethod),
						get_namespace_name(opfnamespace))));
}

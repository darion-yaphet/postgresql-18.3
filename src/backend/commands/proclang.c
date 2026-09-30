/*-------------------------------------------------------------------------
 *
 * proclang.c
 *	  PostgreSQL LANGUAGE support code.
 *
 * PostgreSQL LANGUAGE（过程语言）支持代码。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/commands/proclang.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_language.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/proclang.h"
#include "miscadmin.h"
#include "parser/parse_func.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"


/*
 * 核心流程概览：
 * CreateProceduralLanguage 校验超级用户与 handler/inline/validator 函数，
 * 插入或替换 pg_language，并维护属主、扩展和函数依赖。
 * get_language_oid 按语言名查找 OID。
 */
/*
 * CREATE LANGUAGE
 *
 * 执行 CREATE LANGUAGE。
 */
ObjectAddress
CreateProceduralLanguage(CreatePLangStmt *stmt)
{
	const char *languageName = stmt->plname;
	Oid			languageOwner = GetUserId();
	Oid			handlerOid,
				inlineOid,
				valOid;
	Oid			funcrettype;
	Oid			funcargtypes[1];
	Relation	rel;
	TupleDesc	tupDesc;
	Datum		values[Natts_pg_language];
	bool		nulls[Natts_pg_language];
	bool		replaces[Natts_pg_language];
	NameData	langname;
	HeapTuple	oldtup;
	HeapTuple	tup;
	Oid			langoid;
	bool		is_update;
	ObjectAddress myself,
				referenced;
	ObjectAddresses *addrs;

	/*
	 * Check permission
	 *
	 * 检查权限。
	 */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to create custom procedural language")));

	/*
	 * Lookup the PL handler function and check that it is of the expected
	 * return type
	 *
	 * 查找 PL 处理函数，并检查返回类型是否符合预期。
	 */
	Assert(stmt->plhandler);
	handlerOid = LookupFuncName(stmt->plhandler, 0, NULL, false);
	funcrettype = get_func_rettype(handlerOid);
	if (funcrettype != LANGUAGE_HANDLEROID)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("function %s must return type %s",
						NameListToString(stmt->plhandler), "language_handler")));

	/* validate the inline function */
	/*
	 *
	 * 校验 inline 函数。
	 */
	if (stmt->plinline)
	{
		funcargtypes[0] = INTERNALOID;
		inlineOid = LookupFuncName(stmt->plinline, 1, funcargtypes, false);
		/* return value is ignored, so we don't check the type */
		/*
		 *
		 * 返回值被忽略，因此不检查类型。
		 */
	}
	else
		inlineOid = InvalidOid;

	/* validate the validator function */
	/*
	 *
	 * 校验 validator 函数。
	 */
	if (stmt->plvalidator)
	{
		funcargtypes[0] = OIDOID;
		valOid = LookupFuncName(stmt->plvalidator, 1, funcargtypes, false);
		/* return value is ignored, so we don't check the type */
		/*
		 *
		 * 返回值被忽略，因此不检查类型。
		 */
	}
	else
		valOid = InvalidOid;

	/* ok to create it */
	/*
	 *
	 * 可以创建。
	 */
	rel = table_open(LanguageRelationId, RowExclusiveLock);
	tupDesc = RelationGetDescr(rel);

	/* Prepare data to be inserted */
	/*
	 *
	 * 准备待插入的数据。
	 */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));
	memset(replaces, true, sizeof(replaces));

	namestrcpy(&langname, languageName);
	values[Anum_pg_language_lanname - 1] = NameGetDatum(&langname);
	values[Anum_pg_language_lanowner - 1] = ObjectIdGetDatum(languageOwner);
	values[Anum_pg_language_lanispl - 1] = BoolGetDatum(true);
	values[Anum_pg_language_lanpltrusted - 1] = BoolGetDatum(stmt->pltrusted);
	values[Anum_pg_language_lanplcallfoid - 1] = ObjectIdGetDatum(handlerOid);
	values[Anum_pg_language_laninline - 1] = ObjectIdGetDatum(inlineOid);
	values[Anum_pg_language_lanvalidator - 1] = ObjectIdGetDatum(valOid);
	nulls[Anum_pg_language_lanacl - 1] = true;

	/* Check for pre-existing definition */
	/*
	 *
	 * 检查是否已有定义。
	 */
	oldtup = SearchSysCache1(LANGNAME, PointerGetDatum(languageName));

	if (HeapTupleIsValid(oldtup))
	{
		Form_pg_language oldform = (Form_pg_language) GETSTRUCT(oldtup);

		/* There is one; okay to replace it? */
		/*
		 *
		 * 已存在；是否允许替换？
		 */
		if (!stmt->replace)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("language \"%s\" already exists", languageName)));

		/* This is currently pointless, since we already checked superuser */
		/*
		 *
		 * 此处检查目前无意义，因为前面已经要求超级用户。
		 */
#ifdef NOT_USED
		if (!object_ownercheck(LanguageRelationId, oldform->oid, languageOwner))
			aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_LANGUAGE,
						   languageName);
#endif

		/*
		 * Do not change existing oid, ownership or permissions.  Note
		 * dependency-update code below has to agree with this decision.
		 *
		 * 不改变已有 oid、属主或权限。下面的依赖更新代码必须与此一致。
		 */
		replaces[Anum_pg_language_oid - 1] = false;
		replaces[Anum_pg_language_lanowner - 1] = false;
		replaces[Anum_pg_language_lanacl - 1] = false;

		/* Okay, do it... */
		/*
		 *
		 * 可以，执行更新。
		 */
		tup = heap_modify_tuple(oldtup, tupDesc, values, nulls, replaces);
		CatalogTupleUpdate(rel, &tup->t_self, tup);

		langoid = oldform->oid;
		ReleaseSysCache(oldtup);
		is_update = true;
	}
	else
	{
		/* Creating a new language */
		/*
		 *
		 * 正在创建新语言。
		 */
		langoid = GetNewOidWithIndex(rel, LanguageOidIndexId,
									 Anum_pg_language_oid);
		values[Anum_pg_language_oid - 1] = ObjectIdGetDatum(langoid);
		tup = heap_form_tuple(tupDesc, values, nulls);
		CatalogTupleInsert(rel, tup);
		is_update = false;
	}

	/*
	 * Create dependencies for the new language.  If we are updating an
	 * existing language, first delete any existing pg_depend entries.
	 * (However, since we are not changing ownership or permissions, the
	 * shared dependencies do *not* need to change, and we leave them alone.)
	 *
	 * 为新语言建立依赖。若是更新已有语言，先删除现有 pg_depend 项。
	 * 属主和权限不变，因此共享依赖不需要改动。
	 */
	myself.classId = LanguageRelationId;
	myself.objectId = langoid;
	myself.objectSubId = 0;

	if (is_update)
		deleteDependencyRecordsFor(myself.classId, myself.objectId, true);

	/* dependency on owner of language */
	/*
	 *
	 * 对语言属主的依赖。
	 */
	if (!is_update)
		recordDependencyOnOwner(myself.classId, myself.objectId,
								languageOwner);

	/* dependency on extension */
	/*
	 *
	 * 对扩展的依赖。
	 */
	recordDependencyOnCurrentExtension(&myself, is_update);

	addrs = new_object_addresses();

	/* dependency on the PL handler function */
	/*
	 *
	 * 对 PL 处理函数的依赖。
	 */
	ObjectAddressSet(referenced, ProcedureRelationId, handlerOid);
	add_exact_object_address(&referenced, addrs);

	/* dependency on the inline handler function, if any */
	/*
	 *
	 * 对 inline 处理函数的依赖（若有）。
	 */
	if (OidIsValid(inlineOid))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, inlineOid);
		add_exact_object_address(&referenced, addrs);
	}

	/* dependency on the validator function, if any */
	/*
	 *
	 * 对 validator 函数的依赖（若有）。
	 */
	if (OidIsValid(valOid))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, valOid);
		add_exact_object_address(&referenced, addrs);
	}

	record_object_address_dependencies(&myself, addrs, DEPENDENCY_NORMAL);
	free_object_addresses(addrs);

	/* Post creation hook for new procedural language */
	/*
	 *
	 * 新过程语言的创建后钩子。
	 */
	InvokeObjectPostCreateHook(LanguageRelationId, myself.objectId, 0);

	table_close(rel, RowExclusiveLock);

	return myself;
}

/*
 * get_language_oid - given a language name, look up the OID
 *
 * 按语言名查找 OID。
 *
 * If missing_ok is false, throw an error if language name not found.  If
 * true, just return InvalidOid.
 *
 * missing_ok 为 false 时，找不到语言名就报错；为 true 时返回 InvalidOid。
 */
Oid
get_language_oid(const char *langname, bool missing_ok)
{
	Oid			oid;

	oid = GetSysCacheOid1(LANGNAME, Anum_pg_language_oid,
						  CStringGetDatum(langname));
	if (!OidIsValid(oid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("language \"%s\" does not exist", langname)));
	return oid;
}

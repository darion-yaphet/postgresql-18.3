/*-------------------------------------------------------------------------
 *
 * collationcmds.c
 *	  collation-related commands support code
 *
 * 排序规则相关命令的支持代码。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/collationcmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_database.h"
#include "catalog/pg_namespace.h"
#include "commands/collationcmds.h"
#include "commands/comment.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "common/string.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/pg_locale.h"
#include "utils/rel.h"
#include "utils/syscache.h"


/*
 * 核心流程概览：
 * DefineCollation 根据 libc 或 ICU 区域创建 pg_collation 项，并校验提供者与编码。
 * AlterCollation 刷新版本。pg_import_system_collations 用 locale -a、ICU 或
 * EnumSystemLocalesEx 枚举系统区域，生成排序规则及别名，已存在则跳过。
 */
typedef struct
{
	char	   *localename;		/* name of locale, as per "locale -a" */
	/*
	 *
	 * 区域名，与 "locale -a" 的输出一致。
	 */
	char	   *alias;			/* shortened alias for same */
	/*
	 *
	 * 同一区域的缩短别名。
	 */
	int			enc;			/* encoding */
	/*
	 *
	 * 编码。
	 */
} CollAliasData;


/*
 * CREATE COLLATION
 *
 * 执行 CREATE COLLATION。
 */
ObjectAddress
DefineCollation(ParseState *pstate, List *names, List *parameters, bool if_not_exists)
{
	char	   *collName;
	Oid			collNamespace;
	AclResult	aclresult;
	ListCell   *pl;
	DefElem    *fromEl = NULL;
	DefElem    *localeEl = NULL;
	DefElem    *lccollateEl = NULL;
	DefElem    *lcctypeEl = NULL;
	DefElem    *providerEl = NULL;
	DefElem    *deterministicEl = NULL;
	DefElem    *rulesEl = NULL;
	DefElem    *versionEl = NULL;
	char	   *collcollate;
	char	   *collctype;
	const char *colllocale;
	char	   *collicurules;
	bool		collisdeterministic;
	int			collencoding;
	char		collprovider;
	char	   *collversion = NULL;
	Oid			newoid;
	ObjectAddress address;

	collNamespace = QualifiedNameGetCreationNamespace(names, &collName);

	aclresult = object_aclcheck(NamespaceRelationId, collNamespace, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
					   get_namespace_name(collNamespace));

	foreach(pl, parameters)
	{
		DefElem    *defel = lfirst_node(DefElem, pl);
		DefElem   **defelp;

		if (strcmp(defel->defname, "from") == 0)
			defelp = &fromEl;
		else if (strcmp(defel->defname, "locale") == 0)
			defelp = &localeEl;
		else if (strcmp(defel->defname, "lc_collate") == 0)
			defelp = &lccollateEl;
		else if (strcmp(defel->defname, "lc_ctype") == 0)
			defelp = &lcctypeEl;
		else if (strcmp(defel->defname, "provider") == 0)
			defelp = &providerEl;
		else if (strcmp(defel->defname, "deterministic") == 0)
			defelp = &deterministicEl;
		else if (strcmp(defel->defname, "rules") == 0)
			defelp = &rulesEl;
		else if (strcmp(defel->defname, "version") == 0)
			defelp = &versionEl;
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("collation attribute \"%s\" not recognized",
							defel->defname),
					 parser_errposition(pstate, defel->location)));
			break;
		}
		if (*defelp != NULL)
			errorConflictingDefElem(defel, pstate);
		*defelp = defel;
	}

	if (localeEl && (lccollateEl || lcctypeEl))
		ereport(ERROR,
				errcode(ERRCODE_SYNTAX_ERROR),
				errmsg("conflicting or redundant options"),
				errdetail("LOCALE cannot be specified together with LC_COLLATE or LC_CTYPE."));

	if (fromEl && list_length(parameters) != 1)
		ereport(ERROR,
				errcode(ERRCODE_SYNTAX_ERROR),
				errmsg("conflicting or redundant options"),
				errdetail("FROM cannot be specified together with any other options."));

	if (fromEl)
	{
		Oid			collid;
		HeapTuple	tp;
		Datum		datum;
		bool		isnull;

		collid = get_collation_oid(defGetQualifiedName(fromEl), false);
		tp = SearchSysCache1(COLLOID, ObjectIdGetDatum(collid));
		if (!HeapTupleIsValid(tp))
			elog(ERROR, "cache lookup failed for collation %u", collid);

		collprovider = ((Form_pg_collation) GETSTRUCT(tp))->collprovider;
		collisdeterministic = ((Form_pg_collation) GETSTRUCT(tp))->collisdeterministic;
		collencoding = ((Form_pg_collation) GETSTRUCT(tp))->collencoding;

		datum = SysCacheGetAttr(COLLOID, tp, Anum_pg_collation_collcollate, &isnull);
		if (!isnull)
			collcollate = TextDatumGetCString(datum);
		else
			collcollate = NULL;

		datum = SysCacheGetAttr(COLLOID, tp, Anum_pg_collation_collctype, &isnull);
		if (!isnull)
			collctype = TextDatumGetCString(datum);
		else
			collctype = NULL;

		datum = SysCacheGetAttr(COLLOID, tp, Anum_pg_collation_colllocale, &isnull);
		if (!isnull)
			colllocale = TextDatumGetCString(datum);
		else
			colllocale = NULL;

		/*
		 * When the ICU locale comes from an existing collation, do not
		 * canonicalize to a language tag.
		 *
		 * 若 ICU 区域来自已有排序规则，不要规范成语言标签。
		 */

		datum = SysCacheGetAttr(COLLOID, tp, Anum_pg_collation_collicurules, &isnull);
		if (!isnull)
			collicurules = TextDatumGetCString(datum);
		else
			collicurules = NULL;

		ReleaseSysCache(tp);

		/*
		 * Copying the "default" collation is not allowed because most code
		 * checks for DEFAULT_COLLATION_OID instead of COLLPROVIDER_DEFAULT,
		 * and so having a second collation with COLLPROVIDER_DEFAULT would
		 * not work and potentially confuse or crash some code.  This could be
		 * fixed with some legwork.
		 *
		 * 不允许复制 "default" 排序规则。多数代码检查的是 DEFAULT_COLLATION_OID
		 * 而不是 COLLPROVIDER_DEFAULT，再出现一个 COLLPROVIDER_DEFAULT 的排序规则
		 * 会失效，甚至让某些代码混乱或崩溃。
		 */
		if (collprovider == COLLPROVIDER_DEFAULT)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("collation \"default\" cannot be copied")));
	}
	else
	{
		char	   *collproviderstr = NULL;

		collcollate = NULL;
		collctype = NULL;
		colllocale = NULL;
		collicurules = NULL;

		if (providerEl)
			collproviderstr = defGetString(providerEl);

		if (deterministicEl)
			collisdeterministic = defGetBoolean(deterministicEl);
		else
			collisdeterministic = true;

		if (rulesEl)
			collicurules = defGetString(rulesEl);

		if (versionEl)
			collversion = defGetString(versionEl);

		if (collproviderstr)
		{
			if (pg_strcasecmp(collproviderstr, "builtin") == 0)
				collprovider = COLLPROVIDER_BUILTIN;
			else if (pg_strcasecmp(collproviderstr, "icu") == 0)
				collprovider = COLLPROVIDER_ICU;
			else if (pg_strcasecmp(collproviderstr, "libc") == 0)
				collprovider = COLLPROVIDER_LIBC;
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("unrecognized collation provider: %s",
								collproviderstr)));
		}
		else
			collprovider = COLLPROVIDER_LIBC;

		if (localeEl)
		{
			if (collprovider == COLLPROVIDER_LIBC)
			{
				collcollate = defGetString(localeEl);
				collctype = defGetString(localeEl);
			}
			else
				colllocale = defGetString(localeEl);
		}

		if (lccollateEl)
			collcollate = defGetString(lccollateEl);

		if (lcctypeEl)
			collctype = defGetString(lcctypeEl);

		if (collprovider == COLLPROVIDER_BUILTIN)
		{
			if (!colllocale)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("parameter \"%s\" must be specified",
								"locale")));

			colllocale = builtin_validate_locale(GetDatabaseEncoding(),
												 colllocale);
		}
		else if (collprovider == COLLPROVIDER_LIBC)
		{
			if (!collcollate)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("parameter \"%s\" must be specified",
								"lc_collate")));

			if (!collctype)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("parameter \"%s\" must be specified",
								"lc_ctype")));
		}
		else if (collprovider == COLLPROVIDER_ICU)
		{
			if (!colllocale)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("parameter \"%s\" must be specified",
								"locale")));

			/*
			 * During binary upgrade, preserve the locale string. Otherwise,
			 * canonicalize to a language tag.
			 *
			 * 二进制升级时保留区域字符串。否则规范成语言标签。
			 */
			if (!IsBinaryUpgrade)
			{
				char	   *langtag = icu_language_tag(colllocale,
													   icu_validation_level);

				if (langtag && strcmp(colllocale, langtag) != 0)
				{
					ereport(NOTICE,
							(errmsg("using standard form \"%s\" for ICU locale \"%s\"",
									langtag, colllocale)));

					colllocale = langtag;
				}
			}

			icu_validate_locale(colllocale);
		}

		/*
		 * Nondeterministic collations are currently only supported with ICU
		 * because that's the only case where it can actually make a
		 * difference. So we can save writing the code for the other
		 * providers.
		 *
		 * 非确定性排序规则目前只支持 ICU，因为只有这种情况才真有区别。
		 * 因此不必为其它提供者写相应代码。
		 */
		if (!collisdeterministic && collprovider != COLLPROVIDER_ICU)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("nondeterministic collations not supported with this provider")));

		if (collicurules && collprovider != COLLPROVIDER_ICU)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("ICU rules cannot be specified unless locale provider is ICU")));

		if (collprovider == COLLPROVIDER_BUILTIN)
		{
			collencoding = builtin_locale_encoding(colllocale);
		}
		else if (collprovider == COLLPROVIDER_ICU)
		{
#ifdef USE_ICU
			/*
			 * We could create ICU collations with collencoding == database
			 * encoding, but it seems better to use -1 so that it matches the
			 * way initdb would create ICU collations.  However, only allow
			 * one to be created when the current database's encoding is
			 * supported.  Otherwise the collation is useless, plus we get
			 * surprising behaviors like not being able to drop the collation.
			 *
			 * 可以把 ICU 排序规则的 collencoding 设成数据库编码，但用 -1 更好，以便与 initdb 创建的方式一致。
			 * 仅当当前数据库编码受支持时才允许创建，否则排序规则无用，还可能出现无法删除等意外行为。
			 *
			 * Skip this test when !USE_ICU, because the error we want to
			 * throw for that isn't thrown till later.
			 *
			 * 在 !USE_ICU 时跳过此检查，因为相应错误要到后面才会抛出。
			 */
			if (!is_encoding_supported_by_icu(GetDatabaseEncoding()))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("current database's encoding is not supported with this provider")));
#endif
			collencoding = -1;
		}
		else
		{
			collencoding = GetDatabaseEncoding();
			check_encoding_locale_matches(collencoding, collcollate, collctype);
		}
	}

	if (!collversion)
	{
		const char *locale;

		if (collprovider == COLLPROVIDER_LIBC)
			locale = collcollate;
		else
			locale = colllocale;

		collversion = get_collation_actual_version(collprovider, locale);
	}

	newoid = CollationCreate(collName,
							 collNamespace,
							 GetUserId(),
							 collprovider,
							 collisdeterministic,
							 collencoding,
							 collcollate,
							 collctype,
							 colllocale,
							 collicurules,
							 collversion,
							 if_not_exists,
							 false);	/* not quiet */
							 /*
							  *
							  * 非 quiet。
							  */

	if (!OidIsValid(newoid))
		return InvalidObjectAddress;

	/* Check that the locales can be loaded. */
	/*
	 *
	 * 检查区域设置能否加载。
	 */
	CommandCounterIncrement();
	(void) pg_newlocale_from_collation(newoid);

	ObjectAddressSet(address, CollationRelationId, newoid);

	return address;
}

/*
 * Subroutine for ALTER COLLATION SET SCHEMA and RENAME
 *
 * ALTER COLLATION SET SCHEMA 和 RENAME 的子程序。
 *
 * Is there a collation with the same name of the given collation already in
 * the given namespace?  If so, raise an appropriate error message.
 *
 * 给定命名空间中是否已有与该排序规则同名的排序规则？若有则报相应错误。
 */
void
IsThereCollationInNamespace(const char *collname, Oid nspOid)
{
	/* make sure the name doesn't already exist in new schema */
	/*
	 *
	 * 确认新模式中尚无该名字。
	 */
	if (SearchSysCacheExists3(COLLNAMEENCNSP,
							  CStringGetDatum(collname),
							  Int32GetDatum(GetDatabaseEncoding()),
							  ObjectIdGetDatum(nspOid)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("collation \"%s\" for encoding \"%s\" already exists in schema \"%s\"",
						collname, GetDatabaseEncodingName(),
						get_namespace_name(nspOid))));

	/* mustn't match an any-encoding entry, either */
	/*
	 *
	 * 也不能与任意编码的项重名。
	 */
	if (SearchSysCacheExists3(COLLNAMEENCNSP,
							  CStringGetDatum(collname),
							  Int32GetDatum(-1),
							  ObjectIdGetDatum(nspOid)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("collation \"%s\" already exists in schema \"%s\"",
						collname, get_namespace_name(nspOid))));
}

/*
 * ALTER COLLATION
 *
 * 执行 ALTER COLLATION。
 */
ObjectAddress
AlterCollation(AlterCollationStmt *stmt)
{
	Relation	rel;
	Oid			collOid;
	HeapTuple	tup;
	Form_pg_collation collForm;
	Datum		datum;
	bool		isnull;
	char	   *oldversion;
	char	   *newversion;
	ObjectAddress address;

	rel = table_open(CollationRelationId, RowExclusiveLock);
	collOid = get_collation_oid(stmt->collname, false);

	if (collOid == DEFAULT_COLLATION_OID)
		ereport(ERROR,
				(errmsg("cannot refresh version of default collation"),
		/* translator: %s is an SQL command */
		/*
		 *
		 * 翻译提示：%s 是一条 SQL 命令。
		 */
				 errhint("Use %s instead.",
						 "ALTER DATABASE ... REFRESH COLLATION VERSION")));

	if (!object_ownercheck(CollationRelationId, collOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_COLLATION,
					   NameListToString(stmt->collname));

	tup = SearchSysCacheCopy1(COLLOID, ObjectIdGetDatum(collOid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for collation %u", collOid);

	collForm = (Form_pg_collation) GETSTRUCT(tup);
	datum = SysCacheGetAttr(COLLOID, tup, Anum_pg_collation_collversion, &isnull);
	oldversion = isnull ? NULL : TextDatumGetCString(datum);

	if (collForm->collprovider == COLLPROVIDER_LIBC)
		datum = SysCacheGetAttrNotNull(COLLOID, tup, Anum_pg_collation_collcollate);
	else
		datum = SysCacheGetAttrNotNull(COLLOID, tup, Anum_pg_collation_colllocale);

	newversion = get_collation_actual_version(collForm->collprovider,
											  TextDatumGetCString(datum));

	/* cannot change from NULL to non-NULL or vice versa */
	/*
	 *
	 * 不能在 NULL 与非 NULL 之间切换。
	 */
	if ((!oldversion && newversion) || (oldversion && !newversion))
		elog(ERROR, "invalid collation version change");
	else if (oldversion && newversion && strcmp(newversion, oldversion) != 0)
	{
		bool		nulls[Natts_pg_collation];
		bool		replaces[Natts_pg_collation];
		Datum		values[Natts_pg_collation];

		ereport(NOTICE,
				(errmsg("changing version from %s to %s",
						oldversion, newversion)));

		memset(values, 0, sizeof(values));
		memset(nulls, false, sizeof(nulls));
		memset(replaces, false, sizeof(replaces));

		values[Anum_pg_collation_collversion - 1] = CStringGetTextDatum(newversion);
		replaces[Anum_pg_collation_collversion - 1] = true;

		tup = heap_modify_tuple(tup, RelationGetDescr(rel),
								values, nulls, replaces);
	}
	else
		ereport(NOTICE,
				(errmsg("version has not changed")));

	CatalogTupleUpdate(rel, &tup->t_self, tup);

	InvokeObjectPostAlterHook(CollationRelationId, collOid, 0);

	ObjectAddressSet(address, CollationRelationId, collOid);

	heap_freetuple(tup);
	table_close(rel, NoLock);

	return address;
}


/*
 * 返回排序规则在当前系统上的实际版本字符串。
 */
Datum
pg_collation_actual_version(PG_FUNCTION_ARGS)
{
	Oid			collid = PG_GETARG_OID(0);
	char		provider;
	char	   *locale;
	char	   *version;
	Datum		datum;

	if (collid == DEFAULT_COLLATION_OID)
	{
		/* retrieve from pg_database */
		/*
		 *
		 * 从 pg_database 读取。
		 */

		HeapTuple	dbtup = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(MyDatabaseId));

		if (!HeapTupleIsValid(dbtup))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("database with OID %u does not exist", MyDatabaseId)));

		provider = ((Form_pg_database) GETSTRUCT(dbtup))->datlocprovider;

		if (provider == COLLPROVIDER_LIBC)
		{
			datum = SysCacheGetAttrNotNull(DATABASEOID, dbtup, Anum_pg_database_datcollate);
			locale = TextDatumGetCString(datum);
		}
		else
		{
			datum = SysCacheGetAttrNotNull(DATABASEOID, dbtup, Anum_pg_database_datlocale);
			locale = TextDatumGetCString(datum);
		}

		ReleaseSysCache(dbtup);
	}
	else
	{
		/* retrieve from pg_collation */
		/*
		 *
		 * 从 pg_collation 读取。
		 */

		HeapTuple	colltp = SearchSysCache1(COLLOID, ObjectIdGetDatum(collid));

		if (!HeapTupleIsValid(colltp))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("collation with OID %u does not exist", collid)));

		provider = ((Form_pg_collation) GETSTRUCT(colltp))->collprovider;
		Assert(provider != COLLPROVIDER_DEFAULT);

		if (provider == COLLPROVIDER_LIBC)
		{
			datum = SysCacheGetAttrNotNull(COLLOID, colltp, Anum_pg_collation_collcollate);
			locale = TextDatumGetCString(datum);
		}
		else
		{
			datum = SysCacheGetAttrNotNull(COLLOID, colltp, Anum_pg_collation_colllocale);
			locale = TextDatumGetCString(datum);
		}

		ReleaseSysCache(colltp);
	}

	version = get_collation_actual_version(provider, locale);
	if (version)
		PG_RETURN_TEXT_P(cstring_to_text(version));
	else
		PG_RETURN_NULL();
}


/* will we use "locale -a" in pg_import_system_collations? */
/*
 *
 * pg_import_system_collations 是否会使用 "locale -a"？
 */
#if !defined(WIN32)
#define READ_LOCALE_A_OUTPUT
#endif

/* will we use EnumSystemLocalesEx in pg_import_system_collations? */
/*
 *
 * pg_import_system_collations 是否会使用 EnumSystemLocalesEx？
 */
#ifdef WIN32
#define ENUM_SYSTEM_LOCALE
#endif


#ifdef READ_LOCALE_A_OUTPUT
/*
 * "Normalize" a libc locale name, stripping off encoding tags such as
 * ".utf8" (e.g., "en_US.utf8" -> "en_US", but "br_FR.iso885915@euro"
 * -> "br_FR@euro").  Return true if a new, different name was
 * generated.
 *
 * 规范化 libc 区域名，去掉 ".utf8" 这类编码标记
 * （例如 "en_US.utf8" 变成 "en_US"，"br_FR.iso885915@euro" 变成 "br_FR@euro"）。
 * 若生成了不同的新名字则返回 true。
 */
static bool
normalize_libc_locale_name(char *new, const char *old)
{
	char	   *n = new;
	const char *o = old;
	bool		changed = false;

	while (*o)
	{
		if (*o == '.')
		{
			/* skip over encoding tag such as ".utf8" or ".UTF-8" */
			/*
			 *
			 * 跳过 ".utf8" 或 ".UTF-8" 这类编码标记。
			 */
			o++;
			while ((*o >= 'A' && *o <= 'Z')
				   || (*o >= 'a' && *o <= 'z')
				   || (*o >= '0' && *o <= '9')
				   || (*o == '-'))
				o++;
			changed = true;
		}
		else
			*n++ = *o++;
	}
	*n = '\0';

	return changed;
}

/*
 * qsort comparator for CollAliasData items
 *
 * CollAliasData 项的 qsort 比较函数。
 */
static int
cmpaliases(const void *a, const void *b)
{
	const CollAliasData *ca = (const CollAliasData *) a;
	const CollAliasData *cb = (const CollAliasData *) b;

	/* comparing localename is enough because other fields are derived */
	/*
	 *
	 * 比较 localename 即可，其它字段都是派生的。
	 */
	return strcmp(ca->localename, cb->localename);
}
#endif							/* READ_LOCALE_A_OUTPUT */
/*
 *
 * READ_LOCALE_A_OUTPUT 条件编译部分。
 */


#ifdef USE_ICU
/*
 * Get a comment (specifically, the display name) for an ICU locale.
 * The result is a palloc'd string, or NULL if we can't get a comment
 * or find that it's not all ASCII.  (We can *not* accept non-ASCII
 * comments, because the contents of template0 must be encoding-agnostic.)
 *
 * 取 ICU 区域的注释（即显示名）。结果是 palloc 的字符串；
 * 取不到或含非 ASCII 字符时返回 NULL。template0 的内容必须与编码无关，因此不能接受非 ASCII 注释。
 */
static char *
get_icu_locale_comment(const char *localename)
{
	UErrorCode	status;
	UChar		displayname[128];
	int32		len_uchar;
	int32		i;
	char	   *result;

	status = U_ZERO_ERROR;
	len_uchar = uloc_getDisplayName(localename, "en",
									displayname, lengthof(displayname),
									&status);
	if (U_FAILURE(status))
		return NULL;			/* no good reason to raise an error */
		/*
		 *
		 * 没有充分理由报错。
		 */

	/* Check for non-ASCII comment (can't use pg_is_ascii for this) */
	/*
	 *
	 * 检查注释是否含非 ASCII（这里不能用 pg_is_ascii）。
	 */
	for (i = 0; i < len_uchar; i++)
	{
		if (displayname[i] > 127)
			return NULL;
	}

	/* OK, transcribe */
	/*
	 *
	 * 可以，抄录下来。
	 */
	result = palloc(len_uchar + 1);
	for (i = 0; i < len_uchar; i++)
		result[i] = displayname[i];
	result[len_uchar] = '\0';

	return result;
}
#endif							/* USE_ICU */
/*
 *
 * USE_ICU 条件编译部分。
 */


/*
 * Create a new collation using the input locale 'locale'. (subroutine for
 * pg_import_system_collations())
 *
 * 用输入区域 locale 创建排序规则。pg_import_system_collations() 的子程序。
 *
 * 'nspid' is the namespace id where the collation will be created.
 *
 * nspid 是创建排序规则的命名空间 ID。
 *
 * 'nvalidp' is incremented if the locale has a valid encoding.
 *
 * 区域编码有效时递增 nvalidp。
 *
 * 'ncreatedp' is incremented if the collation is actually created.  If the
 * collation already exists it will quietly do nothing.
 *
 * 真正创建了排序规则时递增 ncreatedp。若已存在则静默不做任何事。
 *
 * The returned value is the encoding of the locale, -1 if the locale is not
 * valid for creating a collation.
 *
 * 返回值是区域的编码；若该区域不能用来创建排序规则，则返回 -1。
 *
 */
pg_attribute_unused()
static int
create_collation_from_locale(const char *locale, int nspid,
							 int *nvalidp, int *ncreatedp)
{
	int			enc;
	Oid			collid;

	/*
	 * Some systems have locale names that don't consist entirely of ASCII
	 * letters (such as "bokm&aring;l" or "fran&ccedil;ais"). This is pretty
	 * silly, since we need the locale itself to interpret the non-ASCII
	 * characters. We can't do much with those, so we filter them out.
	 *
	 * 有些系统的区域名并不全是 ASCII 字母。解释其中的非 ASCII 字符本身就需要该区域设置，
	 * 因此这里把它们过滤掉。
	 */
	if (!pg_is_ascii(locale))
	{
		elog(DEBUG1, "skipping locale with non-ASCII name: \"%s\"", locale);
		return -1;
	}

	enc = pg_get_encoding_from_locale(locale, false);
	if (enc < 0)
	{
		elog(DEBUG1, "skipping locale with unrecognized encoding: \"%s\"", locale);
		return -1;
	}
	if (!PG_VALID_BE_ENCODING(enc))
	{
		elog(DEBUG1, "skipping locale with client-only encoding: \"%s\"", locale);
		return -1;
	}
	if (enc == PG_SQL_ASCII)
		return -1;				/* C/POSIX are already in the catalog */
		/*
		 *
		 * C/POSIX 已经在目录中。
		 */

	/* count valid locales found in operating system */
	/*
	 *
	 * 统计操作系统中找到的有效区域数。
	 */
	(*nvalidp)++;

	/*
	 * Create a collation named the same as the locale, but quietly doing
	 * nothing if it already exists.  This is the behavior we need even at
	 * initdb time, because some versions of "locale -a" can report the same
	 * locale name more than once.  And it's convenient for later import runs,
	 * too, since you just about always want to add on new locales without a
	 * lot of chatter about existing ones.
	 *
	 * 创建与区域同名的排序规则；已存在则静默跳过。initdb 时也需要这样，
	 * 因为某些版本的 "locale -a" 会重复报告同一区域名。以后再次导入时也很方便，
	 * 只需补充新区域，不必对已有项喋喋不休。
	 */
	collid = CollationCreate(locale, nspid, GetUserId(),
							 COLLPROVIDER_LIBC, true, enc,
							 locale, locale, NULL, NULL,
							 get_collation_actual_version(COLLPROVIDER_LIBC, locale),
							 true, true);
	if (OidIsValid(collid))
	{
		(*ncreatedp)++;

		/* Must do CCI between inserts to handle duplicates correctly */
		/*
		 *
		 * 插入之间必须做 CCI，才能正确处理重复项。
		 */
		CommandCounterIncrement();
	}

	return enc;
}


#ifdef ENUM_SYSTEM_LOCALE
/* parameter to be passed to the callback function win32_read_locale() */
/*
 *
 * 传给回调函数 win32_read_locale() 的参数。
 */
typedef struct
{
	Oid			nspid;
	int		   *ncreatedp;
	int		   *nvalidp;
} CollParam;

/*
 * Callback function for EnumSystemLocalesEx() in
 * pg_import_system_collations().  Creates a collation for every valid locale
 * and a POSIX alias collation.
 *
 * pg_import_system_collations() 中 EnumSystemLocalesEx() 的回调。
 * 为每个有效区域创建排序规则，并创建一个 POSIX 别名排序规则。
 *
 * The callback contract is to return TRUE to continue enumerating and FALSE
 * to stop enumerating.  We always want to continue.
 *
 * 回调约定：返回 TRUE 继续枚举，返回 FALSE 停止。这里始终继续。
 */
static BOOL CALLBACK
win32_read_locale(LPWSTR pStr, DWORD dwFlags, LPARAM lparam)
{
	CollParam  *param = (CollParam *) lparam;
	char		localebuf[NAMEDATALEN];
	int			result;
	int			enc;

	(void) dwFlags;

	result = WideCharToMultiByte(CP_ACP, 0, pStr, -1, localebuf, NAMEDATALEN,
								 NULL, NULL);

	if (result == 0)
	{
		if (GetLastError() == ERROR_INSUFFICIENT_BUFFER)
			elog(DEBUG1, "skipping locale with too-long name: \"%s\"", localebuf);
		return TRUE;
	}
	if (localebuf[0] == '\0')
		return TRUE;

	enc = create_collation_from_locale(localebuf, param->nspid,
									   param->nvalidp, param->ncreatedp);
	if (enc < 0)
		return TRUE;

	/*
	 * Windows will use hyphens between language and territory, where POSIX
	 * uses an underscore. Simply create a POSIX alias.
	 *
	 * Windows 在语言和地区之间用连字符，POSIX 用下划线。直接创建一个 POSIX 别名。
	 */
	if (strchr(localebuf, '-'))
	{
		char		alias[NAMEDATALEN];
		Oid			collid;

		strcpy(alias, localebuf);
		for (char *p = alias; *p; p++)
			if (*p == '-')
				*p = '_';

		collid = CollationCreate(alias, param->nspid, GetUserId(),
								 COLLPROVIDER_LIBC, true, enc,
								 localebuf, localebuf, NULL, NULL,
								 get_collation_actual_version(COLLPROVIDER_LIBC, localebuf),
								 true, true);
		if (OidIsValid(collid))
		{
			(*param->ncreatedp)++;

			CommandCounterIncrement();
		}
	}

	return TRUE;
}
#endif							/* ENUM_SYSTEM_LOCALE */
/*
 *
 * ENUM_SYSTEM_LOCALE 条件编译部分。
 */


/*
 * pg_import_system_collations: add known system collations to pg_collation
 *
 * pg_import_system_collations：把已知的系统排序规则加入 pg_collation。
 */
Datum
pg_import_system_collations(PG_FUNCTION_ARGS)
{
	Oid			nspid = PG_GETARG_OID(0);
	int			ncreated = 0;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to import system collations")));

	if (!SearchSysCacheExists1(NAMESPACEOID, ObjectIdGetDatum(nspid)))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_SCHEMA),
				 errmsg("schema with OID %u does not exist", nspid)));

	/* Load collations known to libc, using "locale -a" to enumerate them */
	/*
	 *
	 * 用 "locale -a" 枚举并加载 libc 已知的排序规则。
	 */
#ifdef READ_LOCALE_A_OUTPUT
	{
		FILE	   *locale_a_handle;
		char		localebuf[LOCALE_NAME_BUFLEN];
		int			nvalid = 0;
		Oid			collid;
		CollAliasData *aliases;
		int			naliases,
					maxaliases,
					i;

		/* expansible array of aliases */
		/*
		 *
		 * 可扩展的别名数组。
		 */
		maxaliases = 100;
		aliases = (CollAliasData *) palloc(maxaliases * sizeof(CollAliasData));
		naliases = 0;

		locale_a_handle = OpenPipeStream("locale -a", "r");
		if (locale_a_handle == NULL)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not execute command \"%s\": %m",
							"locale -a")));

		while (fgets(localebuf, sizeof(localebuf), locale_a_handle))
		{
			size_t		len;
			int			enc;
			char		alias[LOCALE_NAME_BUFLEN];

			len = strlen(localebuf);

			if (len == 0 || localebuf[len - 1] != '\n')
			{
				elog(DEBUG1, "skipping locale with too-long name: \"%s\"", localebuf);
				continue;
			}
			localebuf[len - 1] = '\0';

			enc = create_collation_from_locale(localebuf, nspid, &nvalid, &ncreated);
			if (enc < 0)
				continue;

			/*
			 * Generate aliases such as "en_US" in addition to "en_US.utf8"
			 * for ease of use.  Note that collation names are unique per
			 * encoding only, so this doesn't clash with "en_US" for LATIN1,
			 * say.
			 *
			 * 除 "en_US.utf8" 外再生成 "en_US" 这类别名以便使用。
			 * 排序规则名只在每种编码内唯一，因此不会与 LATIN1 的 "en_US" 冲突。
			 *
			 * However, it might conflict with a name we'll see later in the
			 * "locale -a" output.  So save up the aliases and try to add them
			 * after we've read all the output.
			 *
			 * 但它可能与稍后 "locale -a" 输出中的名字冲突。因此先攒下别名，读完全部输出后再添加。
			 */
			if (normalize_libc_locale_name(alias, localebuf))
			{
				if (naliases >= maxaliases)
				{
					maxaliases *= 2;
					aliases = (CollAliasData *)
						repalloc(aliases, maxaliases * sizeof(CollAliasData));
				}
				aliases[naliases].localename = pstrdup(localebuf);
				aliases[naliases].alias = pstrdup(alias);
				aliases[naliases].enc = enc;
				naliases++;
			}
		}

		/*
		 * We don't check the return value of this, because we want to support
		 * the case where there "locale" command does not exist.  (This is
		 * unusual but can happen on minimalized Linux distributions, for
		 * example.)  We will warn below if no locales could be found.
		 *
		 * 不检查返回值，以便支持不存在 locale 命令的情况（精简的 Linux 发行版上可能如此）。
		 * 若一个区域都找不到，下面会发出警告。
		 */
		ClosePipeStream(locale_a_handle);

		/*
		 * Before processing the aliases, sort them by locale name.  The point
		 * here is that if "locale -a" gives us multiple locale names with the
		 * same encoding and base name, say "en_US.utf8" and "en_US.utf-8", we
		 * want to pick a deterministic one of them.  First in ASCII sort
		 * order is a good enough rule.  (Before PG 10, the code corresponding
		 * to this logic in initdb.c had an additional ordering rule, to
		 * prefer the locale name exactly matching the alias, if any.  We
		 * don't need to consider that here, because we would have already
		 * created such a pg_collation entry above, and that one will win.)
		 *
		 * 处理别名前按区域名排序。若 "locale -a" 给出编码和基名相同的多个名字，
		 * 例如 "en_US.utf8" 和 "en_US.utf-8"，需要确定性地选一个。按 ASCII 序取第一个即可。
		 * PG 10 之前 initdb.c 还会优先完全匹配别名的名字；这里不必考虑，因为上面已经创建了对应项，那一项会胜出。
		 */
		if (naliases > 1)
			qsort(aliases, naliases, sizeof(CollAliasData), cmpaliases);

		/* Now add aliases, ignoring any that match pre-existing entries */
		/*
		 *
		 * 现在添加别名，忽略与已有项重名的。
		 */
		for (i = 0; i < naliases; i++)
		{
			char	   *locale = aliases[i].localename;
			char	   *alias = aliases[i].alias;
			int			enc = aliases[i].enc;

			collid = CollationCreate(alias, nspid, GetUserId(),
									 COLLPROVIDER_LIBC, true, enc,
									 locale, locale, NULL, NULL,
									 get_collation_actual_version(COLLPROVIDER_LIBC, locale),
									 true, true);
			if (OidIsValid(collid))
			{
				ncreated++;

				CommandCounterIncrement();
			}
		}

		/* Give a warning if "locale -a" seems to be malfunctioning */
		/*
		 *
		 * 若 "locale -a" 似乎工作异常，则发出警告。
		 */
		if (nvalid == 0)
			ereport(WARNING,
					(errmsg("no usable system locales were found")));
	}
#endif							/* READ_LOCALE_A_OUTPUT */
/*
 *
 * READ_LOCALE_A_OUTPUT 条件编译部分。
 */

	/*
	 * Load collations known to ICU
	 *
	 * 加载 ICU 已知的排序规则。
	 *
	 * We use uloc_countAvailable()/uloc_getAvailable() rather than
	 * ucol_countAvailable()/ucol_getAvailable().  The former returns a full
	 * set of language+region combinations, whereas the latter only returns
	 * language+region combinations if they are distinct from the language's
	 * base collation.  So there might not be a de-DE or en-GB, which would be
	 * confusing.
	 *
	 * 使用 uloc_countAvailable()/uloc_getAvailable()，而不是 ucol_countAvailable()/ucol_getAvailable()。
	 * 前者返回完整的语言加地区组合；后者只在与该语言的基础排序规则不同时才返回。
	 * 因此可能没有 de-DE 或 en-GB，这会让人困惑。
	 */
#ifdef USE_ICU
	{
		int			i;

		/*
		 * Start the loop at -1 to sneak in the root locale without too much
		 * code duplication.
		 *
		 * 循环从 -1 开始，以便在较少重复代码的情况下纳入根区域。
		 */
		for (i = -1; i < uloc_countAvailable(); i++)
		{
			const char *name;
			char	   *langtag;
			char	   *icucomment;
			Oid			collid;

			if (i == -1)
				name = "";		/* ICU root locale */
				/*
				 *
				 * ICU 根区域。
				 */
			else
				name = uloc_getAvailable(i);

			langtag = icu_language_tag(name, ERROR);

			/*
			 * Be paranoid about not allowing any non-ASCII strings into
			 * pg_collation
			 *
			 * 务必不要让任何非 ASCII 字符串进入 pg_collation。
			 */
			if (!pg_is_ascii(langtag))
				continue;

			collid = CollationCreate(psprintf("%s-x-icu", langtag),
									 nspid, GetUserId(),
									 COLLPROVIDER_ICU, true, -1,
									 NULL, NULL, langtag, NULL,
									 get_collation_actual_version(COLLPROVIDER_ICU, langtag),
									 true, true);
			if (OidIsValid(collid))
			{
				ncreated++;

				CommandCounterIncrement();

				icucomment = get_icu_locale_comment(name);
				if (icucomment)
					CreateComments(collid, CollationRelationId, 0,
								   icucomment);
			}
		}
	}
#endif							/* USE_ICU */
/*
 *
 * USE_ICU 条件编译部分。
 */

	/* Load collations known to WIN32 */
	/*
	 *
	 * 加载 WIN32 已知的排序规则。
	 */
#ifdef ENUM_SYSTEM_LOCALE
	{
		int			nvalid = 0;
		CollParam	param;

		param.nspid = nspid;
		param.ncreatedp = &ncreated;
		param.nvalidp = &nvalid;

		/*
		 * Enumerate the locales that are either installed on or supported by
		 * the OS.
		 *
		 * 枚举操作系统已安装或支持的区域。
		 */
		if (!EnumSystemLocalesEx(win32_read_locale, LOCALE_ALL,
								 (LPARAM) &param, NULL))
			_dosmaperr(GetLastError());

		/* Give a warning if EnumSystemLocalesEx seems to be malfunctioning */
		/*
		 *
		 * 若 EnumSystemLocalesEx 似乎工作异常，则发出警告。
		 */
		if (nvalid == 0)
			ereport(WARNING,
					(errmsg("no usable system locales were found")));
	}
#endif							/* ENUM_SYSTEM_LOCALE */
/*
 *
 * ENUM_SYSTEM_LOCALE 条件编译部分。
 */

	PG_RETURN_INT32(ncreated);
}

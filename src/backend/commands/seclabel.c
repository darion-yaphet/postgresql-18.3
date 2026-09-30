/* -------------------------------------------------------------------------
 *
 * seclabel.c
 *	  routines to support security label feature.
 *
 * 安全标签功能的支持例程。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/pg_seclabel.h"
#include "catalog/pg_shseclabel.h"
#include "commands/seclabel.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/*
 * 核心流程概览：
 * ExecSecLabelStmt 解析对象、检查属主，交给标签提供者，再写入
 * pg_seclabel 或共享目录 pg_shseclabel。
 * Get/Set/DeleteSecurityLabel 按对象是否共享选择对应目录维护标签。
 */
typedef struct
{
	const char *provider_name;
	check_object_relabel_type hook;
} LabelProvider;

static List *label_provider_list = NIL;

/*
 * 判断该 ObjectType 是否支持安全标签。
 */
static bool
SecLabelSupportsObjectType(ObjectType objtype)
{
	switch (objtype)
	{
		case OBJECT_AGGREGATE:
		case OBJECT_COLUMN:
		case OBJECT_DATABASE:
		case OBJECT_DOMAIN:
		case OBJECT_EVENT_TRIGGER:
		case OBJECT_FOREIGN_TABLE:
		case OBJECT_FUNCTION:
		case OBJECT_LANGUAGE:
		case OBJECT_LARGEOBJECT:
		case OBJECT_MATVIEW:
		case OBJECT_PROCEDURE:
		case OBJECT_PUBLICATION:
		case OBJECT_ROLE:
		case OBJECT_ROUTINE:
		case OBJECT_SCHEMA:
		case OBJECT_SEQUENCE:
		case OBJECT_SUBSCRIPTION:
		case OBJECT_TABLE:
		case OBJECT_TABLESPACE:
		case OBJECT_TYPE:
		case OBJECT_VIEW:
			return true;

		case OBJECT_ACCESS_METHOD:
		case OBJECT_AMOP:
		case OBJECT_AMPROC:
		case OBJECT_ATTRIBUTE:
		case OBJECT_CAST:
		case OBJECT_COLLATION:
		case OBJECT_CONVERSION:
		case OBJECT_DEFAULT:
		case OBJECT_DEFACL:
		case OBJECT_DOMCONSTRAINT:
		case OBJECT_EXTENSION:
		case OBJECT_FDW:
		case OBJECT_FOREIGN_SERVER:
		case OBJECT_INDEX:
		case OBJECT_OPCLASS:
		case OBJECT_OPERATOR:
		case OBJECT_OPFAMILY:
		case OBJECT_PARAMETER_ACL:
		case OBJECT_POLICY:
		case OBJECT_PUBLICATION_NAMESPACE:
		case OBJECT_PUBLICATION_REL:
		case OBJECT_RULE:
		case OBJECT_STATISTIC_EXT:
		case OBJECT_TABCONSTRAINT:
		case OBJECT_TRANSFORM:
		case OBJECT_TRIGGER:
		case OBJECT_TSCONFIGURATION:
		case OBJECT_TSDICTIONARY:
		case OBJECT_TSPARSER:
		case OBJECT_TSTEMPLATE:
		case OBJECT_USER_MAPPING:
			return false;

			/*
			 * There's intentionally no default: case here; we want the
			 * compiler to warn if a new ObjectType hasn't been handled above.
			 *
			 * 故意不写 default。若新增的 ObjectType 上面没处理，编译器会告警。
			 */
	}

	/* Shouldn't get here, but if we do, say "no support" */
	/*
	 *
	 * 不应到达这里；若到达，则报告不支持。
	 */
	return false;
}

/*
 * ExecSecLabelStmt --
 *
 * ExecSecLabelStmt：
 *
 * Apply a security label to a database object.
 *
 * 给数据库对象设置安全标签。
 *
 * Returns the ObjectAddress of the object to which the policy was applied.
 *
 * 返回被施加策略的对象的 ObjectAddress。
 */
ObjectAddress
ExecSecLabelStmt(SecLabelStmt *stmt)
{
	LabelProvider *provider = NULL;
	ObjectAddress address;
	Relation	relation;
	ListCell   *lc;

	/*
	 * Find the named label provider, or if none specified, check whether
	 * there's exactly one, and if so use it.
	 *
	 * 查找指定的标签提供者；若未指定，则在恰好只有一个时使用它。
	 */
	if (stmt->provider == NULL)
	{
		if (label_provider_list == NIL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("no security label providers have been loaded")));
		if (list_length(label_provider_list) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("must specify provider when multiple security label providers have been loaded")));
		provider = (LabelProvider *) linitial(label_provider_list);
	}
	else
	{
		foreach(lc, label_provider_list)
		{
			LabelProvider *lp = lfirst(lc);

			if (strcmp(stmt->provider, lp->provider_name) == 0)
			{
				provider = lp;
				break;
			}
		}
		if (provider == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("security label provider \"%s\" is not loaded",
							stmt->provider)));
	}

	if (!SecLabelSupportsObjectType(stmt->objtype))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("security labels are not supported for this type of object")));

	/*
	 * Translate the parser representation which identifies this object into
	 * an ObjectAddress. get_object_address() will throw an error if the
	 * object does not exist, and will also acquire a lock on the target to
	 * guard against concurrent modifications.
	 *
	 * 把解析器中的对象标识转成 ObjectAddress。对象不存在时
	 * get_object_address() 会报错，并锁住目标以防并发修改。
	 */
	address = get_object_address(stmt->objtype, stmt->object,
								 &relation, ShareUpdateExclusiveLock, false);

	/* Require ownership of the target object. */
	/*
	 *
	 * 要求拥有目标对象。
	 */
	check_object_ownership(GetUserId(), stmt->objtype, address,
						   stmt->object, relation);

	/* Perform other integrity checks as needed. */
	/*
	 *
	 * 按需要做其它完整性检查。
	 */
	switch (stmt->objtype)
	{
		case OBJECT_COLUMN:

			/*
			 * Allow security labels only on columns of tables, views,
			 * materialized views, composite types, and foreign tables (which
			 * are the only relkinds for which pg_dump will dump labels).
			 *
			 * 只允许在表、视图、物化视图、复合类型和外部表的列上设置安全标签
			 * （pg_dump 也只转储这些 relkind 的标签）。
			 */
			if (relation->rd_rel->relkind != RELKIND_RELATION &&
				relation->rd_rel->relkind != RELKIND_VIEW &&
				relation->rd_rel->relkind != RELKIND_MATVIEW &&
				relation->rd_rel->relkind != RELKIND_COMPOSITE_TYPE &&
				relation->rd_rel->relkind != RELKIND_FOREIGN_TABLE &&
				relation->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("cannot set security label on relation \"%s\"",
								RelationGetRelationName(relation)),
						 errdetail_relkind_not_supported(relation->rd_rel->relkind)));
			break;
		default:
			break;
	}

	/* Provider gets control here, may throw ERROR to veto new label. */
	/*
	 *
	 * 控制权交给提供者，它可以报 ERROR 以否决新标签。
	 */
	provider->hook(&address, stmt->label);

	/* Apply new label. */
	/*
	 *
	 * 应用新标签。
	 */
	SetSecurityLabel(&address, provider->provider_name, stmt->label);

	/*
	 * If get_object_address() opened the relation for us, we close it to keep
	 * the reference count correct - but we retain any locks acquired by
	 * get_object_address() until commit time, to guard against concurrent
	 * activity.
	 *
	 * 若 get_object_address() 打开了关系，这里关闭它以保持引用计数，
	 * 但把它取得的锁保留到提交，以防并发操作。
	 */
	if (relation != NULL)
		relation_close(relation, NoLock);

	return address;
}

/*
 * GetSharedSecurityLabel returns the security label for a shared object for
 * a given provider, or NULL if there is no such label.
 *
 * GetSharedSecurityLabel 返回共享对象在给定提供者下的安全标签，没有则返回 NULL。
 */
static char *
GetSharedSecurityLabel(const ObjectAddress *object, const char *provider)
{
	Relation	pg_shseclabel;
	ScanKeyData keys[3];
	SysScanDesc scan;
	HeapTuple	tuple;
	Datum		datum;
	bool		isnull;
	char	   *seclabel = NULL;

	ScanKeyInit(&keys[0],
				Anum_pg_shseclabel_objoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->objectId));
	ScanKeyInit(&keys[1],
				Anum_pg_shseclabel_classoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->classId));
	ScanKeyInit(&keys[2],
				Anum_pg_shseclabel_provider,
				BTEqualStrategyNumber, F_TEXTEQ,
				CStringGetTextDatum(provider));

	pg_shseclabel = table_open(SharedSecLabelRelationId, AccessShareLock);

	scan = systable_beginscan(pg_shseclabel, SharedSecLabelObjectIndexId,
							  criticalSharedRelcachesBuilt, NULL, 3, keys);

	tuple = systable_getnext(scan);
	if (HeapTupleIsValid(tuple))
	{
		datum = heap_getattr(tuple, Anum_pg_shseclabel_label,
							 RelationGetDescr(pg_shseclabel), &isnull);
		if (!isnull)
			seclabel = TextDatumGetCString(datum);
	}
	systable_endscan(scan);

	table_close(pg_shseclabel, AccessShareLock);

	return seclabel;
}

/*
 * GetSecurityLabel returns the security label for a shared or database object
 * for a given provider, or NULL if there is no such label.
 *
 * GetSecurityLabel 返回共享对象或数据库对象在给定提供者下的安全标签，没有则返回 NULL。
 */
char *
GetSecurityLabel(const ObjectAddress *object, const char *provider)
{
	Relation	pg_seclabel;
	ScanKeyData keys[4];
	SysScanDesc scan;
	HeapTuple	tuple;
	Datum		datum;
	bool		isnull;
	char	   *seclabel = NULL;

	/* Shared objects have their own security label catalog. */
	/*
	 *
	 * 共享对象使用单独的安全标签目录。
	 */
	if (IsSharedRelation(object->classId))
		return GetSharedSecurityLabel(object, provider);

	/* Must be an unshared object, so examine pg_seclabel. */
	/*
	 *
	 * 必须是非共享对象，因此查 pg_seclabel。
	 */
	ScanKeyInit(&keys[0],
				Anum_pg_seclabel_objoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->objectId));
	ScanKeyInit(&keys[1],
				Anum_pg_seclabel_classoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->classId));
	ScanKeyInit(&keys[2],
				Anum_pg_seclabel_objsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(object->objectSubId));
	ScanKeyInit(&keys[3],
				Anum_pg_seclabel_provider,
				BTEqualStrategyNumber, F_TEXTEQ,
				CStringGetTextDatum(provider));

	pg_seclabel = table_open(SecLabelRelationId, AccessShareLock);

	scan = systable_beginscan(pg_seclabel, SecLabelObjectIndexId, true,
							  NULL, 4, keys);

	tuple = systable_getnext(scan);
	if (HeapTupleIsValid(tuple))
	{
		datum = heap_getattr(tuple, Anum_pg_seclabel_label,
							 RelationGetDescr(pg_seclabel), &isnull);
		if (!isnull)
			seclabel = TextDatumGetCString(datum);
	}
	systable_endscan(scan);

	table_close(pg_seclabel, AccessShareLock);

	return seclabel;
}

/*
 * SetSharedSecurityLabel is a helper function of SetSecurityLabel to
 * handle shared database objects.
 *
 * SetSharedSecurityLabel 是 SetSecurityLabel 处理共享数据库对象的辅助函数。
 */
static void
SetSharedSecurityLabel(const ObjectAddress *object,
					   const char *provider, const char *label)
{
	Relation	pg_shseclabel;
	ScanKeyData keys[4];
	SysScanDesc scan;
	HeapTuple	oldtup;
	HeapTuple	newtup = NULL;
	Datum		values[Natts_pg_shseclabel];
	bool		nulls[Natts_pg_shseclabel];
	bool		replaces[Natts_pg_shseclabel];

	/* Prepare to form or update a tuple, if necessary. */
	/*
	 *
	 * 必要时准备构造或更新元组。
	 */
	memset(nulls, false, sizeof(nulls));
	memset(replaces, false, sizeof(replaces));
	values[Anum_pg_shseclabel_objoid - 1] = ObjectIdGetDatum(object->objectId);
	values[Anum_pg_shseclabel_classoid - 1] = ObjectIdGetDatum(object->classId);
	values[Anum_pg_shseclabel_provider - 1] = CStringGetTextDatum(provider);
	if (label != NULL)
		values[Anum_pg_shseclabel_label - 1] = CStringGetTextDatum(label);

	/* Use the index to search for a matching old tuple */
	/*
	 *
	 * 用索引查找匹配的旧元组。
	 */
	ScanKeyInit(&keys[0],
				Anum_pg_shseclabel_objoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->objectId));
	ScanKeyInit(&keys[1],
				Anum_pg_shseclabel_classoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->classId));
	ScanKeyInit(&keys[2],
				Anum_pg_shseclabel_provider,
				BTEqualStrategyNumber, F_TEXTEQ,
				CStringGetTextDatum(provider));

	pg_shseclabel = table_open(SharedSecLabelRelationId, RowExclusiveLock);

	scan = systable_beginscan(pg_shseclabel, SharedSecLabelObjectIndexId, true,
							  NULL, 3, keys);

	oldtup = systable_getnext(scan);
	if (HeapTupleIsValid(oldtup))
	{
		if (label == NULL)
			CatalogTupleDelete(pg_shseclabel, &oldtup->t_self);
		else
		{
			replaces[Anum_pg_shseclabel_label - 1] = true;
			newtup = heap_modify_tuple(oldtup, RelationGetDescr(pg_shseclabel),
									   values, nulls, replaces);
			CatalogTupleUpdate(pg_shseclabel, &oldtup->t_self, newtup);
		}
	}
	systable_endscan(scan);

	/* If we didn't find an old tuple, insert a new one */
	/*
	 *
	 * 没有旧元组则插入新元组。
	 */
	if (newtup == NULL && label != NULL)
	{
		newtup = heap_form_tuple(RelationGetDescr(pg_shseclabel),
								 values, nulls);
		CatalogTupleInsert(pg_shseclabel, newtup);
	}

	if (newtup != NULL)
		heap_freetuple(newtup);

	table_close(pg_shseclabel, RowExclusiveLock);
}

/*
 * SetSecurityLabel attempts to set the security label for the specified
 * provider on the specified object to the given value.  NULL means that any
 * existing label should be deleted.
 *
 * SetSecurityLabel 把指定对象在指定提供者下的安全标签设为给定值。
 * NULL 表示删除已有标签。
 */
void
SetSecurityLabel(const ObjectAddress *object,
				 const char *provider, const char *label)
{
	Relation	pg_seclabel;
	ScanKeyData keys[4];
	SysScanDesc scan;
	HeapTuple	oldtup;
	HeapTuple	newtup = NULL;
	Datum		values[Natts_pg_seclabel];
	bool		nulls[Natts_pg_seclabel];
	bool		replaces[Natts_pg_seclabel];

	/* Shared objects have their own security label catalog. */
	/*
	 *
	 * 共享对象使用单独的安全标签目录。
	 */
	if (IsSharedRelation(object->classId))
	{
		SetSharedSecurityLabel(object, provider, label);
		return;
	}

	/* Prepare to form or update a tuple, if necessary. */
	/*
	 *
	 * 必要时准备构造或更新元组。
	 */
	memset(nulls, false, sizeof(nulls));
	memset(replaces, false, sizeof(replaces));
	values[Anum_pg_seclabel_objoid - 1] = ObjectIdGetDatum(object->objectId);
	values[Anum_pg_seclabel_classoid - 1] = ObjectIdGetDatum(object->classId);
	values[Anum_pg_seclabel_objsubid - 1] = Int32GetDatum(object->objectSubId);
	values[Anum_pg_seclabel_provider - 1] = CStringGetTextDatum(provider);
	if (label != NULL)
		values[Anum_pg_seclabel_label - 1] = CStringGetTextDatum(label);

	/* Use the index to search for a matching old tuple */
	/*
	 *
	 * 用索引查找匹配的旧元组。
	 */
	ScanKeyInit(&keys[0],
				Anum_pg_seclabel_objoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->objectId));
	ScanKeyInit(&keys[1],
				Anum_pg_seclabel_classoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->classId));
	ScanKeyInit(&keys[2],
				Anum_pg_seclabel_objsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(object->objectSubId));
	ScanKeyInit(&keys[3],
				Anum_pg_seclabel_provider,
				BTEqualStrategyNumber, F_TEXTEQ,
				CStringGetTextDatum(provider));

	pg_seclabel = table_open(SecLabelRelationId, RowExclusiveLock);

	scan = systable_beginscan(pg_seclabel, SecLabelObjectIndexId, true,
							  NULL, 4, keys);

	oldtup = systable_getnext(scan);
	if (HeapTupleIsValid(oldtup))
	{
		if (label == NULL)
			CatalogTupleDelete(pg_seclabel, &oldtup->t_self);
		else
		{
			replaces[Anum_pg_seclabel_label - 1] = true;
			newtup = heap_modify_tuple(oldtup, RelationGetDescr(pg_seclabel),
									   values, nulls, replaces);
			CatalogTupleUpdate(pg_seclabel, &oldtup->t_self, newtup);
		}
	}
	systable_endscan(scan);

	/* If we didn't find an old tuple, insert a new one */
	/*
	 *
	 * 没有旧元组则插入新元组。
	 */
	if (newtup == NULL && label != NULL)
	{
		newtup = heap_form_tuple(RelationGetDescr(pg_seclabel),
								 values, nulls);
		CatalogTupleInsert(pg_seclabel, newtup);
	}

	/* Update indexes, if necessary */
	/*
	 *
	 * 必要时更新索引。
	 */
	if (newtup != NULL)
		heap_freetuple(newtup);

	table_close(pg_seclabel, RowExclusiveLock);
}

/*
 * DeleteSharedSecurityLabel is a helper function of DeleteSecurityLabel
 * to handle shared database objects.
 *
 * DeleteSharedSecurityLabel 是 DeleteSecurityLabel 处理共享数据库对象的辅助函数。
 */
void
DeleteSharedSecurityLabel(Oid objectId, Oid classId)
{
	Relation	pg_shseclabel;
	ScanKeyData skey[2];
	SysScanDesc scan;
	HeapTuple	oldtup;

	ScanKeyInit(&skey[0],
				Anum_pg_shseclabel_objoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(objectId));
	ScanKeyInit(&skey[1],
				Anum_pg_shseclabel_classoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classId));

	pg_shseclabel = table_open(SharedSecLabelRelationId, RowExclusiveLock);

	scan = systable_beginscan(pg_shseclabel, SharedSecLabelObjectIndexId, true,
							  NULL, 2, skey);
	while (HeapTupleIsValid(oldtup = systable_getnext(scan)))
		CatalogTupleDelete(pg_shseclabel, &oldtup->t_self);
	systable_endscan(scan);

	table_close(pg_shseclabel, RowExclusiveLock);
}

/*
 * DeleteSecurityLabel removes all security labels for an object (and any
 * sub-objects, if applicable).
 *
 * DeleteSecurityLabel 删除对象（及适用时的子对象）的全部安全标签。
 */
void
DeleteSecurityLabel(const ObjectAddress *object)
{
	Relation	pg_seclabel;
	ScanKeyData skey[3];
	SysScanDesc scan;
	HeapTuple	oldtup;
	int			nkeys;

	/* Shared objects have their own security label catalog. */
	/*
	 *
	 * 共享对象使用单独的安全标签目录。
	 */
	if (IsSharedRelation(object->classId))
	{
		Assert(object->objectSubId == 0);
		DeleteSharedSecurityLabel(object->objectId, object->classId);
		return;
	}

	ScanKeyInit(&skey[0],
				Anum_pg_seclabel_objoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->objectId));
	ScanKeyInit(&skey[1],
				Anum_pg_seclabel_classoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->classId));
	if (object->objectSubId != 0)
	{
		ScanKeyInit(&skey[2],
					Anum_pg_seclabel_objsubid,
					BTEqualStrategyNumber, F_INT4EQ,
					Int32GetDatum(object->objectSubId));
		nkeys = 3;
	}
	else
		nkeys = 2;

	pg_seclabel = table_open(SecLabelRelationId, RowExclusiveLock);

	scan = systable_beginscan(pg_seclabel, SecLabelObjectIndexId, true,
							  NULL, nkeys, skey);
	while (HeapTupleIsValid(oldtup = systable_getnext(scan)))
		CatalogTupleDelete(pg_seclabel, &oldtup->t_self);
	systable_endscan(scan);

	table_close(pg_seclabel, RowExclusiveLock);
}

/*
 * 注册一个安全标签提供者及其检查钩子。
 */
void
register_label_provider(const char *provider_name, check_object_relabel_type hook)
{
	LabelProvider *provider;
	MemoryContext oldcxt;

	oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	provider = palloc(sizeof(LabelProvider));
	provider->provider_name = pstrdup(provider_name);
	provider->hook = hook;
	label_provider_list = lappend(label_provider_list, provider);
	MemoryContextSwitchTo(oldcxt);
}

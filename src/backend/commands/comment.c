/*-------------------------------------------------------------------------
 *
 * comment.c
 *
 * PostgreSQL object comments utility code.
 *
 * PostgreSQL 对象注释的实用代码。
 *
 * Copyright (c) 1996-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/commands/comment.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "catalog/indexing.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_description.h"
#include "catalog/pg_shdescription.h"
#include "commands/comment.h"
#include "commands/dbcommands.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"


/*
 * 核心流程概览：
 * CommentObject 把 COMMENT 语句解析成 ObjectAddress，检查属主，
 * 集群级对象写入 pg_shdescription，其它对象写入 pg_description。
 * CreateComments / DeleteComments 维护 pg_description，
 * CreateSharedComments / DeleteSharedComments 维护 pg_shdescription，
 * GetComment 按对象键读取注释。
 */
/*
 * CommentObject --
 *
 * CommentObject：
 *
 * This routine is used to add the associated comment into
 * pg_description for the object specified by the given SQL command.
 *
 * 把 SQL 命令指定对象的注释写入 pg_description。
 */
ObjectAddress
CommentObject(CommentStmt *stmt)
{
	Relation	relation;
	ObjectAddress address = InvalidObjectAddress;

	/*
	 * When loading a dump, we may see a COMMENT ON DATABASE for the old name
	 * of the database.  Erroring out would prevent pg_restore from completing
	 * (which is really pg_restore's fault, but for now we will work around
	 * the problem here).  Consensus is that the best fix is to treat wrong
	 * database name as a WARNING not an ERROR; hence, the following special
	 * case.
	 *
	 * 恢复转储时可能看到针对数据库旧名的 COMMENT ON DATABASE。
	 * 直接报错会让 pg_restore 无法完成。当前的处理是把错误的数据库名
	 * 当成 WARNING 而不是 ERROR。
	 */
	if (stmt->objtype == OBJECT_DATABASE)
	{
		char	   *database = strVal(stmt->object);

		if (!OidIsValid(get_database_oid(database, true)))
		{
			ereport(WARNING,
					(errcode(ERRCODE_UNDEFINED_DATABASE),
					 errmsg("database \"%s\" does not exist", database)));
			return address;
		}
	}

	/*
	 * Translate the parser representation that identifies this object into an
	 * ObjectAddress.  get_object_address() will throw an error if the object
	 * does not exist, and will also acquire a lock on the target to guard
	 * against concurrent DROP operations.
	 *
	 * 把解析器中的对象标识转成 ObjectAddress。对象不存在时
	 * get_object_address() 会报错，并锁住目标以防并发 DROP。
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
			 * Allow comments only on columns of tables, views, materialized
			 * views, composite types, and foreign tables (which are the only
			 * relkinds for which pg_dump will dump per-column comments).  In
			 * particular we wish to disallow comments on index columns,
			 * because the naming of an index's columns may change across PG
			 * versions, so dumping per-column comments could create reload
			 * failures.
			 *
			 * 只允许在表、视图、物化视图、复合类型和外部表的列上加注释
			 * （pg_dump 也只转储这些 relkind 的列注释）。
			 * 禁止索引列注释，因为索引列名可能随版本变化，转储后重载会失败。
			 */
			if (relation->rd_rel->relkind != RELKIND_RELATION &&
				relation->rd_rel->relkind != RELKIND_VIEW &&
				relation->rd_rel->relkind != RELKIND_MATVIEW &&
				relation->rd_rel->relkind != RELKIND_COMPOSITE_TYPE &&
				relation->rd_rel->relkind != RELKIND_FOREIGN_TABLE &&
				relation->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("cannot set comment on relation \"%s\"",
								RelationGetRelationName(relation)),
						 errdetail_relkind_not_supported(relation->rd_rel->relkind)));
			break;
		default:
			break;
	}

	/*
	 * Databases, tablespaces, and roles are cluster-wide objects, so any
	 * comments on those objects are recorded in the shared pg_shdescription
	 * catalog.  Comments on all other objects are recorded in pg_description.
	 *
	 * 数据库、表空间和角色是集群级对象，注释记在共享的 pg_shdescription。
	 * 其它对象的注释记在 pg_description。
	 */
	if (stmt->objtype == OBJECT_DATABASE || stmt->objtype == OBJECT_TABLESPACE
		|| stmt->objtype == OBJECT_ROLE)
		CreateSharedComments(address.objectId, address.classId, stmt->comment);
	else
		CreateComments(address.objectId, address.classId, address.objectSubId,
					   stmt->comment);

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
 * CreateComments --
 *
 * CreateComments：
 *
 * Create a comment for the specified object descriptor.  Inserts a new
 * pg_description tuple, or replaces an existing one with the same key.
 *
 * 为指定对象描述符创建注释：插入新的 pg_description 元组，或替换同键的旧元组。
 *
 * If the comment given is null or an empty string, instead delete any
 * existing comment for the specified key.
 *
 * 若注释为 null 或空串，则删除该键上已有的注释。
 */
void
CreateComments(Oid oid, Oid classoid, int32 subid, const char *comment)
{
	Relation	description;
	ScanKeyData skey[3];
	SysScanDesc sd;
	HeapTuple	oldtuple;
	HeapTuple	newtuple = NULL;
	Datum		values[Natts_pg_description];
	bool		nulls[Natts_pg_description];
	bool		replaces[Natts_pg_description];
	int			i;

	/* Reduce empty-string to NULL case */
	/*
	 *
	 * 把空串归并为 NULL。
	 */
	if (comment != NULL && strlen(comment) == 0)
		comment = NULL;

	/* Prepare to form or update a tuple, if necessary */
	/*
	 *
	 * 必要时准备构造或更新元组。
	 */
	if (comment != NULL)
	{
		for (i = 0; i < Natts_pg_description; i++)
		{
			nulls[i] = false;
			replaces[i] = true;
		}
		values[Anum_pg_description_objoid - 1] = ObjectIdGetDatum(oid);
		values[Anum_pg_description_classoid - 1] = ObjectIdGetDatum(classoid);
		values[Anum_pg_description_objsubid - 1] = Int32GetDatum(subid);
		values[Anum_pg_description_description - 1] = CStringGetTextDatum(comment);
	}

	/* Use the index to search for a matching old tuple */
	/*
	 *
	 * 用索引查找匹配的旧元组。
	 */

	ScanKeyInit(&skey[0],
				Anum_pg_description_objoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(oid));
	ScanKeyInit(&skey[1],
				Anum_pg_description_classoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classoid));
	ScanKeyInit(&skey[2],
				Anum_pg_description_objsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(subid));

	description = table_open(DescriptionRelationId, RowExclusiveLock);

	sd = systable_beginscan(description, DescriptionObjIndexId, true,
							NULL, 3, skey);

	while ((oldtuple = systable_getnext(sd)) != NULL)
	{
		/* Found the old tuple, so delete or update it */
		/*
		 *
		 * 找到旧元组，删除或更新它。
		 */

		if (comment == NULL)
			CatalogTupleDelete(description, &oldtuple->t_self);
		else
		{
			newtuple = heap_modify_tuple(oldtuple, RelationGetDescr(description), values,
										 nulls, replaces);
			CatalogTupleUpdate(description, &oldtuple->t_self, newtuple);
		}

		break;					/* Assume there can be only one match */
		/*
		 *
		 * 假定至多一条匹配。
		 */
	}

	systable_endscan(sd);

	/* If we didn't find an old tuple, insert a new one */
	/*
	 *
	 * 没有旧元组则插入新元组。
	 */

	if (newtuple == NULL && comment != NULL)
	{
		newtuple = heap_form_tuple(RelationGetDescr(description),
								   values, nulls);
		CatalogTupleInsert(description, newtuple);
	}

	if (newtuple != NULL)
		heap_freetuple(newtuple);

	/* Done */
	/*
	 *
	 * 完成。
	 */

	table_close(description, NoLock);
}

/*
 * CreateSharedComments --
 *
 * CreateSharedComments：
 *
 * Create a comment for the specified shared object descriptor.  Inserts a
 * new pg_shdescription tuple, or replaces an existing one with the same key.
 *
 * 为指定共享对象创建注释：插入新的 pg_shdescription 元组，或替换同键的旧元组。
 *
 * If the comment given is null or an empty string, instead delete any
 * existing comment for the specified key.
 *
 * 若注释为 null 或空串，则删除该键上已有的注释。
 */
void
CreateSharedComments(Oid oid, Oid classoid, const char *comment)
{
	Relation	shdescription;
	ScanKeyData skey[2];
	SysScanDesc sd;
	HeapTuple	oldtuple;
	HeapTuple	newtuple = NULL;
	Datum		values[Natts_pg_shdescription];
	bool		nulls[Natts_pg_shdescription];
	bool		replaces[Natts_pg_shdescription];
	int			i;

	/* Reduce empty-string to NULL case */
	/*
	 *
	 * 把空串归并为 NULL。
	 */
	if (comment != NULL && strlen(comment) == 0)
		comment = NULL;

	/* Prepare to form or update a tuple, if necessary */
	/*
	 *
	 * 必要时准备构造或更新元组。
	 */
	if (comment != NULL)
	{
		for (i = 0; i < Natts_pg_shdescription; i++)
		{
			nulls[i] = false;
			replaces[i] = true;
		}
		values[Anum_pg_shdescription_objoid - 1] = ObjectIdGetDatum(oid);
		values[Anum_pg_shdescription_classoid - 1] = ObjectIdGetDatum(classoid);
		values[Anum_pg_shdescription_description - 1] = CStringGetTextDatum(comment);
	}

	/* Use the index to search for a matching old tuple */
	/*
	 *
	 * 用索引查找匹配的旧元组。
	 */

	ScanKeyInit(&skey[0],
				Anum_pg_shdescription_objoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(oid));
	ScanKeyInit(&skey[1],
				Anum_pg_shdescription_classoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classoid));

	shdescription = table_open(SharedDescriptionRelationId, RowExclusiveLock);

	sd = systable_beginscan(shdescription, SharedDescriptionObjIndexId, true,
							NULL, 2, skey);

	while ((oldtuple = systable_getnext(sd)) != NULL)
	{
		/* Found the old tuple, so delete or update it */
		/*
		 *
		 * 找到旧元组，删除或更新它。
		 */

		if (comment == NULL)
			CatalogTupleDelete(shdescription, &oldtuple->t_self);
		else
		{
			newtuple = heap_modify_tuple(oldtuple, RelationGetDescr(shdescription),
										 values, nulls, replaces);
			CatalogTupleUpdate(shdescription, &oldtuple->t_self, newtuple);
		}

		break;					/* Assume there can be only one match */
		/*
		 *
		 * 假定至多一条匹配。
		 */
	}

	systable_endscan(sd);

	/* If we didn't find an old tuple, insert a new one */
	/*
	 *
	 * 没有旧元组则插入新元组。
	 */

	if (newtuple == NULL && comment != NULL)
	{
		newtuple = heap_form_tuple(RelationGetDescr(shdescription),
								   values, nulls);
		CatalogTupleInsert(shdescription, newtuple);
	}

	if (newtuple != NULL)
		heap_freetuple(newtuple);

	/* Done */
	/*
	 *
	 * 完成。
	 */

	table_close(shdescription, NoLock);
}

/*
 * DeleteComments -- remove comments for an object
 *
 * DeleteComments：删除对象的注释。
 *
 * If subid is nonzero then only comments matching it will be removed.
 * If subid is zero, all comments matching the oid/classoid will be removed
 * (this corresponds to deleting a whole object).
 *
 * subid 非零时只删除匹配它的注释。
 * subid 为零时删除匹配 oid/classoid 的全部注释（即删除整个对象）。
 */
void
DeleteComments(Oid oid, Oid classoid, int32 subid)
{
	Relation	description;
	ScanKeyData skey[3];
	int			nkeys;
	SysScanDesc sd;
	HeapTuple	oldtuple;

	/* Use the index to search for all matching old tuples */
	/*
	 *
	 * 用索引查找所有匹配的旧元组。
	 */

	ScanKeyInit(&skey[0],
				Anum_pg_description_objoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(oid));
	ScanKeyInit(&skey[1],
				Anum_pg_description_classoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classoid));

	if (subid != 0)
	{
		ScanKeyInit(&skey[2],
					Anum_pg_description_objsubid,
					BTEqualStrategyNumber, F_INT4EQ,
					Int32GetDatum(subid));
		nkeys = 3;
	}
	else
		nkeys = 2;

	description = table_open(DescriptionRelationId, RowExclusiveLock);

	sd = systable_beginscan(description, DescriptionObjIndexId, true,
							NULL, nkeys, skey);

	while ((oldtuple = systable_getnext(sd)) != NULL)
		CatalogTupleDelete(description, &oldtuple->t_self);

	/* Done */
	/*
	 *
	 * 完成。
	 */

	systable_endscan(sd);
	table_close(description, RowExclusiveLock);
}

/*
 * DeleteSharedComments -- remove comments for a shared object
 *
 * DeleteSharedComments：删除共享对象的注释。
 */
void
DeleteSharedComments(Oid oid, Oid classoid)
{
	Relation	shdescription;
	ScanKeyData skey[2];
	SysScanDesc sd;
	HeapTuple	oldtuple;

	/* Use the index to search for all matching old tuples */
	/*
	 *
	 * 用索引查找所有匹配的旧元组。
	 */

	ScanKeyInit(&skey[0],
				Anum_pg_shdescription_objoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(oid));
	ScanKeyInit(&skey[1],
				Anum_pg_shdescription_classoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classoid));

	shdescription = table_open(SharedDescriptionRelationId, RowExclusiveLock);

	sd = systable_beginscan(shdescription, SharedDescriptionObjIndexId, true,
							NULL, 2, skey);

	while ((oldtuple = systable_getnext(sd)) != NULL)
		CatalogTupleDelete(shdescription, &oldtuple->t_self);

	/* Done */
	/*
	 *
	 * 完成。
	 */

	systable_endscan(sd);
	table_close(shdescription, RowExclusiveLock);
}

/*
 * GetComment -- get the comment for an object, or null if not found.
 *
 * GetComment：取对象注释，找不到则返回 null。
 */
char *
GetComment(Oid oid, Oid classoid, int32 subid)
{
	Relation	description;
	ScanKeyData skey[3];
	SysScanDesc sd;
	TupleDesc	tupdesc;
	HeapTuple	tuple;
	char	   *comment;

	/* Use the index to search for a matching old tuple */
	/*
	 *
	 * 用索引查找匹配的旧元组。
	 */

	ScanKeyInit(&skey[0],
				Anum_pg_description_objoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(oid));
	ScanKeyInit(&skey[1],
				Anum_pg_description_classoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classoid));
	ScanKeyInit(&skey[2],
				Anum_pg_description_objsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(subid));

	description = table_open(DescriptionRelationId, AccessShareLock);
	tupdesc = RelationGetDescr(description);

	sd = systable_beginscan(description, DescriptionObjIndexId, true,
							NULL, 3, skey);

	comment = NULL;
	while ((tuple = systable_getnext(sd)) != NULL)
	{
		Datum		value;
		bool		isnull;

		/* Found the tuple, get description field */
		/*
		 *
		 * 找到元组，读取 description 字段。
		 */
		value = heap_getattr(tuple, Anum_pg_description_description, tupdesc, &isnull);
		if (!isnull)
			comment = TextDatumGetCString(value);
		break;					/* Assume there can be only one match */
		/*
		 *
		 * 假定至多一条匹配。
		 */
	}

	systable_endscan(sd);

	/* Done */
	/*
	 *
	 * 完成。
	 */
	table_close(description, AccessShareLock);

	return comment;
}

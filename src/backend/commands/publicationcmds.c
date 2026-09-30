/*-------------------------------------------------------------------------
 *
 * publicationcmds.c
 *		publication manipulation
 *
 * 发布（publication）的维护命令。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/backend/commands/publicationcmds.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_database.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_publication.h"
#include "catalog/pg_publication_namespace.h"
#include "catalog/pg_publication_rel.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/event_trigger.h"
#include "commands/publicationcmds.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_clause.h"
#include "parser/parse_collate.h"
#include "parser/parse_relation.h"
#include "rewrite/rewriteHandler.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/varlena.h"


/*
 * 核心流程概览：
 * CreatePublication 创建发布并登记表或模式；parse_publication_options 解析 publish 动作、
 * publish_via_partition_root 与 publish_generated_columns。
 * AlterPublication 分发到 AlterPublicationOptions、AlterPublicationTables 与
 * AlterPublicationSchemas；PublicationAddTables 等维护目录项。
 * 行过滤表达式经 TransformPubWhereClauses 与 check_simple_rowfilter_expr 校验。
 */
/*
 * Information used to validate the columns in the row filter expression. See
 * contain_invalid_rfcolumn_walker for details.
 *
 * 用于校验行过滤表达式中列的信息。详见 contain_invalid_rfcolumn_walker。
 */
typedef struct rf_context
{
	Bitmapset  *bms_replident;	/* bitset of replica identity columns */
	/*
	 *
	 * 复制标识列的位集。
	 */
	bool		pubviaroot;		/* true if we are validating the parent
								 * relation's row filter */
	/*
	 *
	 * 正在校验父关系的行过滤器时为 true。
	 */

	Oid			relid;			/* relid of the relation */
	/*
	 *
	 * 该关系的 relid。
	 */
	Oid			parentid;		/* relid of the parent relation */
	/*
	 *
	 * 父关系的 relid。
	 */
} rf_context;

static List *OpenTableList(List *tables);
static void CloseTableList(List *rels);
static void LockSchemaList(List *schemalist);
static void PublicationAddTables(Oid pubid, List *rels, bool if_not_exists,
								 AlterPublicationStmt *stmt);
static void PublicationDropTables(Oid pubid, List *rels, bool missing_ok);
static void PublicationAddSchemas(Oid pubid, List *schemas, bool if_not_exists,
								  AlterPublicationStmt *stmt);
static void PublicationDropSchemas(Oid pubid, List *schemas, bool missing_ok);
static char defGetGeneratedColsOption(DefElem *def);


/*
 * 解析 CREATE/ALTER PUBLICATION 的选项，
 * 设置 publish 动作、publish_via_partition_root 与生成列发布方式。
 */
static void
parse_publication_options(ParseState *pstate,
						  List *options,
						  bool *publish_given,
						  PublicationActions *pubactions,
						  bool *publish_via_partition_root_given,
						  bool *publish_via_partition_root,
						  bool *publish_generated_columns_given,
						  char *publish_generated_columns)
{
	ListCell   *lc;

	*publish_given = false;
	*publish_via_partition_root_given = false;
	*publish_generated_columns_given = false;

	/* defaults */
	/*
	 *
	 * 默认值。
	 */
	pubactions->pubinsert = true;
	pubactions->pubupdate = true;
	pubactions->pubdelete = true;
	pubactions->pubtruncate = true;
	*publish_via_partition_root = false;
	*publish_generated_columns = PUBLISH_GENCOLS_NONE;

	/* Parse options */
	/*
	 *
	 * 解析选项。
	 */
	foreach(lc, options)
	{
		DefElem    *defel = (DefElem *) lfirst(lc);

		if (strcmp(defel->defname, "publish") == 0)
		{
			char	   *publish;
			List	   *publish_list;
			ListCell   *lc2;

			if (*publish_given)
				errorConflictingDefElem(defel, pstate);

			/*
			 * If publish option was given only the explicitly listed actions
			 * should be published.
			 *
			 * 若给出了 publish 选项，则只发布显式列出的动作。
			 */
			pubactions->pubinsert = false;
			pubactions->pubupdate = false;
			pubactions->pubdelete = false;
			pubactions->pubtruncate = false;

			*publish_given = true;

			/*
			 * SplitIdentifierString destructively modifies its input, so make
			 * a copy so we don't modify the memory of the executing statement
			 *
			 * SplitIdentifierString 会破坏性地修改输入，因此先复制一份，以免改到正在执行的语句的内存。
			 */
			publish = pstrdup(defGetString(defel));

			if (!SplitIdentifierString(publish, ',', &publish_list))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("invalid list syntax in parameter \"%s\"",
								"publish")));

			/* Process the option list. */
			/*
			 *
			 * 处理选项列表。
			 */
			foreach(lc2, publish_list)
			{
				char	   *publish_opt = (char *) lfirst(lc2);

				if (strcmp(publish_opt, "insert") == 0)
					pubactions->pubinsert = true;
				else if (strcmp(publish_opt, "update") == 0)
					pubactions->pubupdate = true;
				else if (strcmp(publish_opt, "delete") == 0)
					pubactions->pubdelete = true;
				else if (strcmp(publish_opt, "truncate") == 0)
					pubactions->pubtruncate = true;
				else
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("unrecognized value for publication option \"%s\": \"%s\"",
									"publish", publish_opt)));
			}
		}
		else if (strcmp(defel->defname, "publish_via_partition_root") == 0)
		{
			if (*publish_via_partition_root_given)
				errorConflictingDefElem(defel, pstate);
			*publish_via_partition_root_given = true;
			*publish_via_partition_root = defGetBoolean(defel);
		}
		else if (strcmp(defel->defname, "publish_generated_columns") == 0)
		{
			if (*publish_generated_columns_given)
				errorConflictingDefElem(defel, pstate);
			*publish_generated_columns_given = true;
			*publish_generated_columns = defGetGeneratedColsOption(defel);
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized publication parameter: \"%s\"", defel->defname)));
	}
}

/*
 * Convert the PublicationObjSpecType list into schema oid list and
 * PublicationTable list.
 *
 * 把 PublicationObjSpecType 列表转换成 schema OID 列表和 PublicationTable 列表。
 */
static void
ObjectsInPublicationToOids(List *pubobjspec_list, ParseState *pstate,
						   List **rels, List **schemas)
{
	ListCell   *cell;
	PublicationObjSpec *pubobj;

	if (!pubobjspec_list)
		return;

	foreach(cell, pubobjspec_list)
	{
		Oid			schemaid;
		List	   *search_path;

		pubobj = (PublicationObjSpec *) lfirst(cell);

		switch (pubobj->pubobjtype)
		{
			case PUBLICATIONOBJ_TABLE:
				*rels = lappend(*rels, pubobj->pubtable);
				break;
			case PUBLICATIONOBJ_TABLES_IN_SCHEMA:
				schemaid = get_namespace_oid(pubobj->name, false);

				/* Filter out duplicates if user specifies "sch1, sch1" */
				/*
				 *
				 * 若用户写成 sch1, sch1，则去掉重复项。
				 */
				*schemas = list_append_unique_oid(*schemas, schemaid);
				break;
			case PUBLICATIONOBJ_TABLES_IN_CUR_SCHEMA:
				search_path = fetch_search_path(false);
				if (search_path == NIL) /* nothing valid in search_path? */
				/*
				 *
				 * search_path 里没有有效项？
				 */
					ereport(ERROR,
							errcode(ERRCODE_UNDEFINED_SCHEMA),
							errmsg("no schema has been selected for CURRENT_SCHEMA"));

				schemaid = linitial_oid(search_path);
				list_free(search_path);

				/* Filter out duplicates if user specifies "sch1, sch1" */
				/*
				 *
				 * 若用户写成 sch1, sch1，则去掉重复项。
				 */
				*schemas = list_append_unique_oid(*schemas, schemaid);
				break;
			default:
				/* shouldn't happen */
				/*
				 *
				 * 不应发生。
				 */
				elog(ERROR, "invalid publication object type %d", pubobj->pubobjtype);
				break;
		}
	}
}

/*
 * Returns true if any of the columns used in the row filter WHERE expression is
 * not part of REPLICA IDENTITY, false otherwise.
 *
 * 若行过滤 WHERE 表达式用到的任一列不属于 REPLICA IDENTITY，则返回 true，否则返回 false。
 */
static bool
contain_invalid_rfcolumn_walker(Node *node, rf_context *context)
{
	if (node == NULL)
		return false;

	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;
		AttrNumber	attnum = var->varattno;

		/*
		 * If pubviaroot is true, we are validating the row filter of the
		 * parent table, but the bitmap contains the replica identity
		 * information of the child table. So, get the column number of the
		 * child table as parent and child column order could be different.
		 *
		 * pubviaroot 为真时，校验的是父表的行过滤器，但位图里是子表的复制标识信息。
		 * 父子列顺序可能不同，因此要取子表上的列号。
		 */
		if (context->pubviaroot)
		{
			char	   *colname = get_attname(context->parentid, attnum, false);

			attnum = get_attnum(context->relid, colname);
		}

		if (!bms_is_member(attnum - FirstLowInvalidHeapAttributeNumber,
						   context->bms_replident))
			return true;
	}

	return expression_tree_walker(node, contain_invalid_rfcolumn_walker,
								  context);
}

/*
 * Check if all columns referenced in the filter expression are part of the
 * REPLICA IDENTITY index or not.
 *
 * 检查过滤表达式引用的所有列是否都属于 REPLICA IDENTITY 索引。
 *
 * Returns true if any invalid column is found.
 *
 * 若发现任何非法列则返回 true。
 */
bool
pub_rf_contains_invalid_column(Oid pubid, Relation relation, List *ancestors,
							   bool pubviaroot)
{
	HeapTuple	rftuple;
	Oid			relid = RelationGetRelid(relation);
	Oid			publish_as_relid = RelationGetRelid(relation);
	bool		result = false;
	Datum		rfdatum;
	bool		rfisnull;

	/*
	 * FULL means all columns are in the REPLICA IDENTITY, so all columns are
	 * allowed in the row filter and we can skip the validation.
	 *
	 * FULL 表示所有列都在 REPLICA IDENTITY 中，行过滤器可以使用全部列，因此可以跳过校验。
	 */
	if (relation->rd_rel->relreplident == REPLICA_IDENTITY_FULL)
		return false;

	/*
	 * For a partition, if pubviaroot is true, find the topmost ancestor that
	 * is published via this publication as we need to use its row filter
	 * expression to filter the partition's changes.
	 *
	 * 对分区而言，若 pubviaroot 为真，则找到经此发布发布的最顶层祖先，
	 * 因为要用它的行过滤表达式过滤该分区的变更。
	 *
	 * Note that even though the row filter used is for an ancestor, the
	 * REPLICA IDENTITY used will be for the actual child table.
	 *
	 * 注意：即使用的是祖先的行过滤器，REPLICA IDENTITY 仍用实际子表的。
	 */
	if (pubviaroot && relation->rd_rel->relispartition)
	{
		publish_as_relid
			= GetTopMostAncestorInPublication(pubid, ancestors, NULL);

		if (!OidIsValid(publish_as_relid))
			publish_as_relid = relid;
	}

	rftuple = SearchSysCache2(PUBLICATIONRELMAP,
							  ObjectIdGetDatum(publish_as_relid),
							  ObjectIdGetDatum(pubid));

	if (!HeapTupleIsValid(rftuple))
		return false;

	rfdatum = SysCacheGetAttr(PUBLICATIONRELMAP, rftuple,
							  Anum_pg_publication_rel_prqual,
							  &rfisnull);

	if (!rfisnull)
	{
		rf_context	context = {0};
		Node	   *rfnode;
		Bitmapset  *bms = NULL;

		context.pubviaroot = pubviaroot;
		context.parentid = publish_as_relid;
		context.relid = relid;

		/* Remember columns that are part of the REPLICA IDENTITY */
		/*
		 *
		 * 记住属于 REPLICA IDENTITY 的列。
		 */
		bms = RelationGetIndexAttrBitmap(relation,
										 INDEX_ATTR_BITMAP_IDENTITY_KEY);

		context.bms_replident = bms;
		rfnode = stringToNode(TextDatumGetCString(rfdatum));
		result = contain_invalid_rfcolumn_walker(rfnode, &context);
	}

	ReleaseSysCache(rftuple);

	return result;
}

/*
 * Check for invalid columns in the publication table definition.
 *
 * 检查发布表定义中的非法列。
 *
 * This function evaluates two conditions:
 *
 * 本函数检查两个条件：
 *
 * 1. Ensures that all columns referenced in the REPLICA IDENTITY are covered
 *    by the column list. If any column is missing, *invalid_column_list is set
 *    to true.
 * 2. Ensures that all the generated columns referenced in the REPLICA IDENTITY
 *    are published, either by being explicitly named in the column list or, if
 *    no column list is specified, by setting the option
 *    publish_generated_columns to stored. If any unpublished
 *    generated column is found, *invalid_gen_col is set to true.
 *
 * 1. 确保 REPLICA IDENTITY 引用的所有列都被列清单覆盖。缺任何一列则把 *invalid_column_list 设为 true。
 * 2. 确保 REPLICA IDENTITY 引用的生成列都被发布：要么在列清单中显式点名，
 * 要么在未指定列清单时把 publish_generated_columns 设为 stored。
 * 若发现未发布的生成列，则把 *invalid_gen_col 设为 true。
 *
 * Returns true if any of the above conditions are not met.
 *
 * 若上述任一条件不满足则返回 true。
 */
bool
pub_contains_invalid_column(Oid pubid, Relation relation, List *ancestors,
							bool pubviaroot, char pubgencols_type,
							bool *invalid_column_list,
							bool *invalid_gen_col)
{
	Oid			relid = RelationGetRelid(relation);
	Oid			publish_as_relid = RelationGetRelid(relation);
	Bitmapset  *idattrs;
	Bitmapset  *columns = NULL;
	TupleDesc	desc = RelationGetDescr(relation);
	Publication *pub;
	int			x;

	*invalid_column_list = false;
	*invalid_gen_col = false;

	/*
	 * For a partition, if pubviaroot is true, find the topmost ancestor that
	 * is published via this publication as we need to use its column list for
	 * the changes.
	 *
	 * 对分区而言，若 pubviaroot 为真，则找到经此发布发布的最顶层祖先，因为变更要用它的列清单。
	 *
	 * Note that even though the column list used is for an ancestor, the
	 * REPLICA IDENTITY used will be for the actual child table.
	 *
	 * 注意：即使用的是祖先的列清单，REPLICA IDENTITY 仍用实际子表的。
	 */
	if (pubviaroot && relation->rd_rel->relispartition)
	{
		publish_as_relid = GetTopMostAncestorInPublication(pubid, ancestors, NULL);

		if (!OidIsValid(publish_as_relid))
			publish_as_relid = relid;
	}

	/* Fetch the column list */
	/*
	 *
	 * 取出列清单。
	 */
	pub = GetPublication(pubid);
	check_and_fetch_column_list(pub, publish_as_relid, NULL, &columns);

	if (relation->rd_rel->relreplident == REPLICA_IDENTITY_FULL)
	{
		/* With REPLICA IDENTITY FULL, no column list is allowed. */
		/*
		 *
		 * REPLICA IDENTITY FULL 时不允许列清单。
		 */
		*invalid_column_list = (columns != NULL);

		/*
		 * As we don't allow a column list with REPLICA IDENTITY FULL, the
		 * publish_generated_columns option must be set to stored if the table
		 * has any stored generated columns.
		 *
		 * 既然 REPLICA IDENTITY FULL 不允许列清单，若表有 stored 生成列，
		 * 则必须把 publish_generated_columns 设为 stored。
		 */
		if (pubgencols_type != PUBLISH_GENCOLS_STORED &&
			relation->rd_att->constr &&
			relation->rd_att->constr->has_generated_stored)
			*invalid_gen_col = true;

		/*
		 * Virtual generated columns are currently not supported for logical
		 * replication at all.
		 *
		 * 目前逻辑复制完全不支持虚拟生成列。
		 */
		if (relation->rd_att->constr &&
			relation->rd_att->constr->has_generated_virtual)
			*invalid_gen_col = true;

		if (*invalid_gen_col && *invalid_column_list)
			return true;
	}

	/* Remember columns that are part of the REPLICA IDENTITY */
	/*
	 *
	 * 记住属于 REPLICA IDENTITY 的列。
	 */
	idattrs = RelationGetIndexAttrBitmap(relation,
										 INDEX_ATTR_BITMAP_IDENTITY_KEY);

	/*
	 * Attnums in the bitmap returned by RelationGetIndexAttrBitmap are offset
	 * (to handle system columns the usual way), while column list does not
	 * use offset, so we can't do bms_is_subset(). Instead, we have to loop
	 * over the idattrs and check all of them are in the list.
	 *
	 * RelationGetIndexAttrBitmap 返回的位图中的 attnum 有偏移（以便按常规处理系统列），
	 * 而列清单没有偏移，因此不能用 bms_is_subset()。必须遍历 idattrs，检查它们都在清单中。
	 */
	x = -1;
	while ((x = bms_next_member(idattrs, x)) >= 0)
	{
		AttrNumber	attnum = (x + FirstLowInvalidHeapAttributeNumber);
		Form_pg_attribute att = TupleDescAttr(desc, attnum - 1);

		if (columns == NULL)
		{
			/*
			 * The publish_generated_columns option must be set to stored if
			 * the REPLICA IDENTITY contains any stored generated column.
			 *
			 * 若 REPLICA IDENTITY 包含任何 stored 生成列，则必须把 publish_generated_columns 设为 stored。
			 */
			if (att->attgenerated == ATTRIBUTE_GENERATED_STORED && pubgencols_type != PUBLISH_GENCOLS_STORED)
			{
				*invalid_gen_col = true;
				break;
			}

			/*
			 * The equivalent setting for virtual generated columns does not
			 * exist yet.
			 *
			 * 虚拟生成列还没有对应的设置。
			 */
			if (att->attgenerated == ATTRIBUTE_GENERATED_VIRTUAL)
			{
				*invalid_gen_col = true;
				break;
			}

			/* Skip validating the column list since it is not defined */
			/*
			 *
			 * 列清单未定义，跳过校验。
			 */
			continue;
		}

		/*
		 * If pubviaroot is true, we are validating the column list of the
		 * parent table, but the bitmap contains the replica identity
		 * information of the child table. The parent/child attnums may not
		 * match, so translate them to the parent - get the attname from the
		 * child, and look it up in the parent.
		 *
		 * pubviaroot 为真时，校验的是父表列清单，但位图是子表的复制标识。
		 * 父子 attnum 可能不一致，因此要换成父表的：从子表取 attname，再在父表中查找。
		 */
		if (pubviaroot)
		{
			/* attribute name in the child table */
			/*
			 *
			 * 子表中的属性名。
			 */
			char	   *colname = get_attname(relid, attnum, false);

			/*
			 * Determine the attnum for the attribute name in parent (we are
			 * using the column list defined on the parent).
			 *
			 * 确定该属性名在父表中的 attnum（使用的是父表上定义的列清单）。
			 */
			attnum = get_attnum(publish_as_relid, colname);
		}

		/* replica identity column, not covered by the column list */
		/*
		 *
		 * 复制标识列未被列清单覆盖。
		 */
		*invalid_column_list |= !bms_is_member(attnum, columns);

		if (*invalid_column_list && *invalid_gen_col)
			break;
	}

	bms_free(columns);
	bms_free(idattrs);

	return *invalid_column_list || *invalid_gen_col;
}

/*
 * Invalidate entries in the RelationSyncCache for relations included in the
 * specified publication, either via FOR TABLE or FOR TABLES IN SCHEMA.
 *
 * 使指定发布中的关系（经 FOR TABLE 或 FOR TABLES IN SCHEMA 纳入）在 RelationSyncCache 中的项失效。
 *
 * If 'puballtables' is true, invalidate all cache entries.
 *
 * 若 puballtables 为真，则使全部缓存项失效。
 */
void
InvalidatePubRelSyncCache(Oid pubid, bool puballtables)
{
	if (puballtables)
	{
		CacheInvalidateRelSyncAll();
	}
	else
	{
		List	   *relids = NIL;
		List	   *schemarelids = NIL;

		/*
		 * For partitioned tables, we must invalidate all partitions and
		 * itself. WAL records for INSERT/UPDATE/DELETE specify leaf tables as
		 * a target. However, WAL records for TRUNCATE specify both a root and
		 * its leaves.
		 *
		 * 对分区表必须使所有分区及其自身失效。INSERT/UPDATE/DELETE 的 WAL 记录以叶子表为目标，
		 * 但 TRUNCATE 的 WAL 记录同时指定根和叶子。
		 */
		relids = GetPublicationRelations(pubid,
										 PUBLICATION_PART_ALL);
		schemarelids = GetAllSchemaPublicationRelations(pubid,
														PUBLICATION_PART_ALL);

		relids = list_concat_unique_oid(relids, schemarelids);

		/* Invalidate the relsyncache */
		/*
		 *
		 * 使 relsyncache 失效。
		 */
		foreach_oid(relid, relids)
			CacheInvalidateRelSync(relid);
	}

	return;
}

/* check_functions_in_node callback */
/*
 *
 * check_functions_in_node 的回调。
 */
static bool
contain_mutable_or_user_functions_checker(Oid func_id, void *context)
{
	return (func_volatile(func_id) != PROVOLATILE_IMMUTABLE ||
			func_id >= FirstNormalObjectId);
}

/*
 * The row filter walker checks if the row filter expression is a "simple
 * expression".
 *
 * 行过滤器遍历器检查行过滤表达式是否为简单表达式。
 *
 * It allows only simple or compound expressions such as:
 * - (Var Op Const)
 * - (Var Op Var)
 * - (Var Op Const) AND/OR (Var Op Const)
 * - etc
 * (where Var is a column of the table this filter belongs to)
 *
 * 只允许如下简单或复合表达式：
 * - (Var Op Const)
 * - (Var Op Var)
 * - (Var Op Const) AND/OR (Var Op Const)
 * - 等等
 * （Var 是该过滤器所属表的列）
 *
 * The simple expression has the following restrictions:
 * - User-defined operators are not allowed;
 * - User-defined functions are not allowed;
 * - User-defined types are not allowed;
 * - User-defined collations are not allowed;
 * - Non-immutable built-in functions are not allowed;
 * - System columns are not allowed.
 *
 * 简单表达式有以下限制：
 * - 不允许用户定义运算符；
 * - 不允许用户定义函数；
 * - 不允许用户定义类型；
 * - 不允许用户定义排序规则；
 * - 不允许非 immutable 的内建函数；
 * - 不允许系统列。
 *
 * NOTES
 *
 * 说明。
 *
 * We don't allow user-defined functions/operators/types/collations because
 * (a) if a user drops a user-defined object used in a row filter expression or
 * if there is any other error while using it, the logical decoding
 * infrastructure won't be able to recover from such an error even if the
 * object is recreated again because a historic snapshot is used to evaluate
 * the row filter;
 * (b) a user-defined function can be used to access tables that could have
 * unpleasant results because a historic snapshot is used. That's why only
 * immutable built-in functions are allowed in row filter expressions.
 *
 * 不允许用户定义的函数、运算符、类型和排序规则，因为：
 * (a) 若用户删除行过滤表达式所用的对象，或使用时出现其他错误，逻辑解码无法恢复，
 * 即使对象被重建也不行，因为求值行过滤器时用的是历史快照；
 * (b) 用户定义函数可以访问表，在历史快照下可能产生不良后果。
 * 因此行过滤表达式只允许 immutable 内建函数。
 *
 * We don't allow system columns because currently, we don't have that
 * information in the tuple passed to downstream. Also, as we don't replicate
 * those to subscribers, there doesn't seem to be a need for a filter on those
 * columns.
 *
 * 不允许系统列，因为目前传给下游的元组里没有这些信息。而且它们不会复制到订阅端，似乎也没有按这些列过滤的必要。
 *
 * We can allow other node types after more analysis and testing.
 *
 * 经过更多分析和测试后，可以允许其他节点类型。
 */
static bool
check_simple_rowfilter_expr_walker(Node *node, ParseState *pstate)
{
	char	   *errdetail_msg = NULL;

	if (node == NULL)
		return false;

	switch (nodeTag(node))
	{
		case T_Var:
			/* System columns are not allowed. */
			/*
			 *
			 * 不允许系统列。
			 */
			if (((Var *) node)->varattno < InvalidAttrNumber)
				errdetail_msg = _("System columns are not allowed.");
			break;
		case T_OpExpr:
		case T_DistinctExpr:
		case T_NullIfExpr:
			/* OK, except user-defined operators are not allowed. */
			/*
			 *
			 * 可以，但不允许用户定义运算符。
			 */
			if (((OpExpr *) node)->opno >= FirstNormalObjectId)
				errdetail_msg = _("User-defined operators are not allowed.");
			break;
		case T_ScalarArrayOpExpr:
			/* OK, except user-defined operators are not allowed. */
			/*
			 *
			 * 可以，但不允许用户定义运算符。
			 */
			if (((ScalarArrayOpExpr *) node)->opno >= FirstNormalObjectId)
				errdetail_msg = _("User-defined operators are not allowed.");

			/*
			 * We don't need to check the hashfuncid and negfuncid of
			 * ScalarArrayOpExpr as those functions are only built for a
			 * subquery.
			 *
			 * 不必检查 ScalarArrayOpExpr 的 hashfuncid 和 negfuncid，那些函数只为子查询构建。
			 */
			break;
		case T_RowCompareExpr:
			{
				ListCell   *opid;

				/* OK, except user-defined operators are not allowed. */
				/*
				 *
				 * 可以，但不允许用户定义运算符。
				 */
				foreach(opid, ((RowCompareExpr *) node)->opnos)
				{
					if (lfirst_oid(opid) >= FirstNormalObjectId)
					{
						errdetail_msg = _("User-defined operators are not allowed.");
						break;
					}
				}
			}
			break;
		case T_Const:
		case T_FuncExpr:
		case T_BoolExpr:
		case T_RelabelType:
		case T_CollateExpr:
		case T_CaseExpr:
		case T_CaseTestExpr:
		case T_ArrayExpr:
		case T_RowExpr:
		case T_CoalesceExpr:
		case T_MinMaxExpr:
		case T_XmlExpr:
		case T_NullTest:
		case T_BooleanTest:
		case T_List:
			/* OK, supported */
			/*
			 *
			 * 可以，受支持。
			 */
			break;
		default:
			errdetail_msg = _("Only columns, constants, built-in operators, built-in data types, built-in collations, and immutable built-in functions are allowed.");
			break;
	}

	/*
	 * For all the supported nodes, if we haven't already found a problem,
	 * check the types, functions, and collations used in it.  We check List
	 * by walking through each element.
	 *
	 * 对所有受支持的节点，若尚未发现问题，则检查其中使用的类型、函数和排序规则。List 则逐个元素检查。
	 */
	if (!errdetail_msg && !IsA(node, List))
	{
		if (exprType(node) >= FirstNormalObjectId)
			errdetail_msg = _("User-defined types are not allowed.");
		else if (check_functions_in_node(node, contain_mutable_or_user_functions_checker,
										 pstate))
			errdetail_msg = _("User-defined or built-in mutable functions are not allowed.");
		else if (exprCollation(node) >= FirstNormalObjectId ||
				 exprInputCollation(node) >= FirstNormalObjectId)
			errdetail_msg = _("User-defined collations are not allowed.");
	}

	/*
	 * If we found a problem in this node, throw error now. Otherwise keep
	 * going.
	 *
	 * 若本节点已发现问题则立刻报错，否则继续。
	 */
	if (errdetail_msg)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("invalid publication WHERE expression"),
				 errdetail_internal("%s", errdetail_msg),
				 parser_errposition(pstate, exprLocation(node))));

	return expression_tree_walker(node, check_simple_rowfilter_expr_walker,
								  pstate);
}

/*
 * Check if the row filter expression is a "simple expression".
 *
 * 检查行过滤表达式是否为简单表达式。
 *
 * See check_simple_rowfilter_expr_walker for details.
 *
 * 详见 check_simple_rowfilter_expr_walker。
 */
static bool
check_simple_rowfilter_expr(Node *node, ParseState *pstate)
{
	return check_simple_rowfilter_expr_walker(node, pstate);
}

/*
 * Transform the publication WHERE expression for all the relations in the list,
 * ensuring it is coerced to boolean and necessary collation information is
 * added if required, and add a new nsitem/RTE for the associated relation to
 * the ParseState's namespace list.
 *
 * 转换列表中所有关系的发布 WHERE 表达式，确保强制为 boolean，并在需要时补上排序规则信息，
 * 同时把关联关系的新 nsitem/RTE 加入 ParseState 的命名空间列表。
 *
 * Also check the publication row filter expression and throw an error if
 * anything not permitted or unexpected is encountered.
 *
 * 同时检查发布的行过滤表达式，遇到不允许或意外的内容则报错。
 */
static void
TransformPubWhereClauses(List *tables, const char *queryString,
						 bool pubviaroot)
{
	ListCell   *lc;

	foreach(lc, tables)
	{
		ParseNamespaceItem *nsitem;
		Node	   *whereclause = NULL;
		ParseState *pstate;
		PublicationRelInfo *pri = (PublicationRelInfo *) lfirst(lc);

		if (pri->whereClause == NULL)
			continue;

		/*
		 * If the publication doesn't publish changes via the root partitioned
		 * table, the partition's row filter will be used. So disallow using
		 * WHERE clause on partitioned table in this case.
		 *
		 * 若发布不经由根分区表发布变更，则会使用分区自己的行过滤器。因此这种情况下禁止在分区表上使用 WHERE 子句。
		 */
		if (!pubviaroot &&
			pri->relation->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("cannot use publication WHERE clause for relation \"%s\"",
							RelationGetRelationName(pri->relation)),
					 errdetail("WHERE clause cannot be used for a partitioned table when %s is false.",
							   "publish_via_partition_root")));

		/*
		 * A fresh pstate is required so that we only have "this" table in its
		 * rangetable
		 *
		 * 需要全新的 pstate，使范围表里只有这张表。
		 */
		pstate = make_parsestate(NULL);
		pstate->p_sourcetext = queryString;
		nsitem = addRangeTableEntryForRelation(pstate, pri->relation,
											   AccessShareLock, NULL,
											   false, false);
		addNSItemToQuery(pstate, nsitem, false, true, true);

		whereclause = transformWhereClause(pstate,
										   copyObject(pri->whereClause),
										   EXPR_KIND_WHERE,
										   "PUBLICATION WHERE");

		/* Fix up collation information */
		/*
		 *
		 * 修正排序规则信息。
		 */
		assign_expr_collations(pstate, whereclause);

		whereclause = expand_generated_columns_in_expr(whereclause, pri->relation, 1);

		/*
		 * We allow only simple expressions in row filters. See
		 * check_simple_rowfilter_expr_walker.
		 *
		 * 行过滤器只允许简单表达式。见 check_simple_rowfilter_expr_walker。
		 */
		check_simple_rowfilter_expr(whereclause, pstate);

		free_parsestate(pstate);

		pri->whereClause = whereclause;
	}
}


/*
 * Given a list of tables that are going to be added to a publication,
 * verify that they fulfill the necessary preconditions, namely: no tables
 * have a column list if any schema is published; and partitioned tables do
 * not have column lists if publish_via_partition_root is not set.
 *
 * 给定即将加入发布的表列表，确认它们满足前提：若发布了任何模式，则表不能有列清单；
 * 若未设置 publish_via_partition_root，则分区表不能有列清单。
 *
 * 'publish_schema' indicates that the publication contains any TABLES IN
 * SCHEMA elements (newly added in this command, or preexisting).
 * 'pubviaroot' is the value of publish_via_partition_root.
 *
 * publish_schema 表示发布包含任何 TABLES IN SCHEMA 元素（本命令新加的或原有的）。
 * pubviaroot 是 publish_via_partition_root 的值。
 */
static void
CheckPubRelationColumnList(char *pubname, List *tables,
						   bool publish_schema, bool pubviaroot)
{
	ListCell   *lc;

	foreach(lc, tables)
	{
		PublicationRelInfo *pri = (PublicationRelInfo *) lfirst(lc);

		if (pri->columns == NIL)
			continue;

		/*
		 * Disallow specifying column list if any schema is in the
		 * publication.
		 *
		 * 若发布中有任何模式，则禁止指定列清单。
		 *
		 * XXX We could instead just forbid the case when the publication
		 * tries to publish the table with a column list and a schema for that
		 * table. However, if we do that then we need a restriction during
		 * ALTER TABLE ... SET SCHEMA to prevent such a case which doesn't
		 * seem to be a good idea.
		 *
		 * XXX 也可以只禁止发布既带列清单又包含该表所在模式的情况。
		 * 但那样就得在 ALTER TABLE ... SET SCHEMA 时加限制来防止这种情况，似乎不是好办法。
		 */
		if (publish_schema)
			ereport(ERROR,
					errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg("cannot use column list for relation \"%s.%s\" in publication \"%s\"",
						   get_namespace_name(RelationGetNamespace(pri->relation)),
						   RelationGetRelationName(pri->relation), pubname),
					errdetail("Column lists cannot be specified in publications containing FOR TABLES IN SCHEMA elements."));

		/*
		 * If the publication doesn't publish changes via the root partitioned
		 * table, the partition's column list will be used. So disallow using
		 * a column list on the partitioned table in this case.
		 *
		 * 若发布不经由根分区表发布变更，则会使用分区自己的列清单。因此这种情况下禁止在分区表上使用列清单。
		 */
		if (!pubviaroot &&
			pri->relation->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("cannot use column list for relation \"%s.%s\" in publication \"%s\"",
							get_namespace_name(RelationGetNamespace(pri->relation)),
							RelationGetRelationName(pri->relation), pubname),
					 errdetail("Column lists cannot be specified for partitioned tables when %s is false.",
							   "publish_via_partition_root")));
	}
}

/*
 * Create new publication.
 *
 * 创建新发布。
 */
ObjectAddress
CreatePublication(ParseState *pstate, CreatePublicationStmt *stmt)
{
	Relation	rel;
	ObjectAddress myself;
	Oid			puboid;
	bool		nulls[Natts_pg_publication];
	Datum		values[Natts_pg_publication];
	HeapTuple	tup;
	bool		publish_given;
	PublicationActions pubactions;
	bool		publish_via_partition_root_given;
	bool		publish_via_partition_root;
	bool		publish_generated_columns_given;
	char		publish_generated_columns;
	AclResult	aclresult;
	List	   *relations = NIL;
	List	   *schemaidlist = NIL;

	/* must have CREATE privilege on database */
	/*
	 *
	 * 必须对数据库有 CREATE 权限。
	 */
	aclresult = object_aclcheck(DatabaseRelationId, MyDatabaseId, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_DATABASE,
					   get_database_name(MyDatabaseId));

	/* FOR ALL TABLES requires superuser */
	/*
	 *
	 * FOR ALL TABLES 要求超级用户。
	 */
	if (stmt->for_all_tables && !superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to create FOR ALL TABLES publication")));

	rel = table_open(PublicationRelationId, RowExclusiveLock);

	/* Check if name is used */
	/*
	 *
	 * 检查名称是否已被使用。
	 */
	puboid = GetSysCacheOid1(PUBLICATIONNAME, Anum_pg_publication_oid,
							 CStringGetDatum(stmt->pubname));
	if (OidIsValid(puboid))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("publication \"%s\" already exists",
						stmt->pubname)));

	/* Form a tuple. */
	/*
	 *
	 * 构造元组。
	 */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	values[Anum_pg_publication_pubname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(stmt->pubname));
	values[Anum_pg_publication_pubowner - 1] = ObjectIdGetDatum(GetUserId());

	parse_publication_options(pstate,
							  stmt->options,
							  &publish_given, &pubactions,
							  &publish_via_partition_root_given,
							  &publish_via_partition_root,
							  &publish_generated_columns_given,
							  &publish_generated_columns);

	puboid = GetNewOidWithIndex(rel, PublicationObjectIndexId,
								Anum_pg_publication_oid);
	values[Anum_pg_publication_oid - 1] = ObjectIdGetDatum(puboid);
	values[Anum_pg_publication_puballtables - 1] =
		BoolGetDatum(stmt->for_all_tables);
	values[Anum_pg_publication_pubinsert - 1] =
		BoolGetDatum(pubactions.pubinsert);
	values[Anum_pg_publication_pubupdate - 1] =
		BoolGetDatum(pubactions.pubupdate);
	values[Anum_pg_publication_pubdelete - 1] =
		BoolGetDatum(pubactions.pubdelete);
	values[Anum_pg_publication_pubtruncate - 1] =
		BoolGetDatum(pubactions.pubtruncate);
	values[Anum_pg_publication_pubviaroot - 1] =
		BoolGetDatum(publish_via_partition_root);
	values[Anum_pg_publication_pubgencols - 1] =
		CharGetDatum(publish_generated_columns);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);

	/* Insert tuple into catalog. */
	/*
	 *
	 * 把元组插入目录。
	 */
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	recordDependencyOnOwner(PublicationRelationId, puboid, GetUserId());

	ObjectAddressSet(myself, PublicationRelationId, puboid);

	/* Make the changes visible. */
	/*
	 *
	 * 使变更可见。
	 */
	CommandCounterIncrement();

	/* Associate objects with the publication. */
	/*
	 *
	 * 把对象关联到该发布。
	 */
	if (stmt->for_all_tables)
	{
		/* Invalidate relcache so that publication info is rebuilt. */
		/*
		 *
		 * 使 relcache 失效，以便重建发布信息。
		 */
		CacheInvalidateRelcacheAll();
	}
	else
	{
		ObjectsInPublicationToOids(stmt->pubobjects, pstate, &relations,
								   &schemaidlist);

		/* FOR TABLES IN SCHEMA requires superuser */
		/*
		 *
		 * FOR TABLES IN SCHEMA 要求超级用户。
		 */
		if (schemaidlist != NIL && !superuser())
			ereport(ERROR,
					errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					errmsg("must be superuser to create FOR TABLES IN SCHEMA publication"));

		if (relations != NIL)
		{
			List	   *rels;

			rels = OpenTableList(relations);
			TransformPubWhereClauses(rels, pstate->p_sourcetext,
									 publish_via_partition_root);

			CheckPubRelationColumnList(stmt->pubname, rels,
									   schemaidlist != NIL,
									   publish_via_partition_root);

			PublicationAddTables(puboid, rels, true, NULL);
			CloseTableList(rels);
		}

		if (schemaidlist != NIL)
		{
			/*
			 * Schema lock is held until the publication is created to prevent
			 * concurrent schema deletion.
			 *
			 * 模式锁保持到发布创建完成，以防止并发删除模式。
			 */
			LockSchemaList(schemaidlist);
			PublicationAddSchemas(puboid, schemaidlist, true, NULL);
		}
	}

	table_close(rel, RowExclusiveLock);

	InvokeObjectPostCreateHook(PublicationRelationId, puboid, 0);

	if (wal_level != WAL_LEVEL_LOGICAL)
		ereport(WARNING,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("\"wal_level\" is insufficient to publish logical changes"),
				 errhint("Set \"wal_level\" to \"logical\" before creating subscriptions.")));

	return myself;
}

/*
 * Change options of a publication.
 *
 * 修改发布的选项。
 */
static void
AlterPublicationOptions(ParseState *pstate, AlterPublicationStmt *stmt,
						Relation rel, HeapTuple tup)
{
	bool		nulls[Natts_pg_publication];
	bool		replaces[Natts_pg_publication];
	Datum		values[Natts_pg_publication];
	bool		publish_given;
	PublicationActions pubactions;
	bool		publish_via_partition_root_given;
	bool		publish_via_partition_root;
	bool		publish_generated_columns_given;
	char		publish_generated_columns;
	ObjectAddress obj;
	Form_pg_publication pubform;
	List	   *root_relids = NIL;
	ListCell   *lc;

	parse_publication_options(pstate,
							  stmt->options,
							  &publish_given, &pubactions,
							  &publish_via_partition_root_given,
							  &publish_via_partition_root,
							  &publish_generated_columns_given,
							  &publish_generated_columns);

	pubform = (Form_pg_publication) GETSTRUCT(tup);

	/*
	 * If the publication doesn't publish changes via the root partitioned
	 * table, the partition's row filter and column list will be used. So
	 * disallow using WHERE clause and column lists on partitioned table in
	 * this case.
	 *
	 * 若发布不经由根分区表发布变更，则会使用分区自己的行过滤器和列清单。
	 * 因此这种情况下禁止在分区表上使用 WHERE 子句和列清单。
	 */
	if (!pubform->puballtables && publish_via_partition_root_given &&
		!publish_via_partition_root)
	{
		/*
		 * Lock the publication so nobody else can do anything with it. This
		 * prevents concurrent alter to add partitioned table(s) with WHERE
		 * clause(s) and/or column lists which we don't allow when not
		 * publishing via root.
		 *
		 * 锁住发布，使别人不能同时操作它。这样可以防止并发 ALTER 加入带 WHERE 子句和/或列清单的分区表，
		 * 而在不经由根发布时这些是不允许的。
		 */
		LockDatabaseObject(PublicationRelationId, pubform->oid, 0,
						   AccessShareLock);

		root_relids = GetPublicationRelations(pubform->oid,
											  PUBLICATION_PART_ROOT);

		foreach(lc, root_relids)
		{
			Oid			relid = lfirst_oid(lc);
			HeapTuple	rftuple;
			char		relkind;
			char	   *relname;
			bool		has_rowfilter;
			bool		has_collist;

			/*
			 * Beware: we don't have lock on the relations, so cope silently
			 * with the cache lookups returning NULL.
			 *
			 * 注意：我们没有锁住这些关系，因此缓存查找返回 NULL 时要静默处理。
			 */

			rftuple = SearchSysCache2(PUBLICATIONRELMAP,
									  ObjectIdGetDatum(relid),
									  ObjectIdGetDatum(pubform->oid));
			if (!HeapTupleIsValid(rftuple))
				continue;
			has_rowfilter = !heap_attisnull(rftuple, Anum_pg_publication_rel_prqual, NULL);
			has_collist = !heap_attisnull(rftuple, Anum_pg_publication_rel_prattrs, NULL);
			if (!has_rowfilter && !has_collist)
			{
				ReleaseSysCache(rftuple);
				continue;
			}

			relkind = get_rel_relkind(relid);
			if (relkind != RELKIND_PARTITIONED_TABLE)
			{
				ReleaseSysCache(rftuple);
				continue;
			}
			relname = get_rel_name(relid);
			if (relname == NULL)	/* table concurrently dropped */
			/*
			 *
			 * 表被并发删除。
			 */
			{
				ReleaseSysCache(rftuple);
				continue;
			}

			if (has_rowfilter)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("cannot set parameter \"%s\" to false for publication \"%s\"",
								"publish_via_partition_root",
								stmt->pubname),
						 errdetail("The publication contains a WHERE clause for partitioned table \"%s\", which is not allowed when \"%s\" is false.",
								   relname, "publish_via_partition_root")));
			Assert(has_collist);
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("cannot set parameter \"%s\" to false for publication \"%s\"",
							"publish_via_partition_root",
							stmt->pubname),
					 errdetail("The publication contains a column list for partitioned table \"%s\", which is not allowed when \"%s\" is false.",
							   relname, "publish_via_partition_root")));
		}
	}

	/* Everything ok, form a new tuple. */
	/*
	 *
	 * 一切正常，构造新元组。
	 */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));
	memset(replaces, false, sizeof(replaces));

	if (publish_given)
	{
		values[Anum_pg_publication_pubinsert - 1] = BoolGetDatum(pubactions.pubinsert);
		replaces[Anum_pg_publication_pubinsert - 1] = true;

		values[Anum_pg_publication_pubupdate - 1] = BoolGetDatum(pubactions.pubupdate);
		replaces[Anum_pg_publication_pubupdate - 1] = true;

		values[Anum_pg_publication_pubdelete - 1] = BoolGetDatum(pubactions.pubdelete);
		replaces[Anum_pg_publication_pubdelete - 1] = true;

		values[Anum_pg_publication_pubtruncate - 1] = BoolGetDatum(pubactions.pubtruncate);
		replaces[Anum_pg_publication_pubtruncate - 1] = true;
	}

	if (publish_via_partition_root_given)
	{
		values[Anum_pg_publication_pubviaroot - 1] = BoolGetDatum(publish_via_partition_root);
		replaces[Anum_pg_publication_pubviaroot - 1] = true;
	}

	if (publish_generated_columns_given)
	{
		values[Anum_pg_publication_pubgencols - 1] = CharGetDatum(publish_generated_columns);
		replaces[Anum_pg_publication_pubgencols - 1] = true;
	}

	tup = heap_modify_tuple(tup, RelationGetDescr(rel), values, nulls,
							replaces);

	/* Update the catalog. */
	/*
	 *
	 * 更新目录。
	 */
	CatalogTupleUpdate(rel, &tup->t_self, tup);

	CommandCounterIncrement();

	pubform = (Form_pg_publication) GETSTRUCT(tup);

	/* Invalidate the relcache. */
	/*
	 *
	 * 使 relcache 失效。
	 */
	if (pubform->puballtables)
	{
		CacheInvalidateRelcacheAll();
	}
	else
	{
		List	   *relids = NIL;
		List	   *schemarelids = NIL;

		/*
		 * For any partitioned tables contained in the publication, we must
		 * invalidate all partitions contained in the respective partition
		 * trees, not just those explicitly mentioned in the publication.
		 *
		 * 对发布中的任何分区表，必须使相应分区树中的所有分区失效，而不只是发布里显式提到的那些。
		 */
		if (root_relids == NIL)
			relids = GetPublicationRelations(pubform->oid,
											 PUBLICATION_PART_ALL);
		else
		{
			/*
			 * We already got tables explicitly mentioned in the publication.
			 * Now get all partitions for the partitioned table in the list.
			 *
			 * 已经取得发布中显式提到的表。现在取得列表中分区表的所有分区。
			 */
			foreach(lc, root_relids)
				relids = GetPubPartitionOptionRelations(relids,
														PUBLICATION_PART_ALL,
														lfirst_oid(lc));
		}

		schemarelids = GetAllSchemaPublicationRelations(pubform->oid,
														PUBLICATION_PART_ALL);
		relids = list_concat_unique_oid(relids, schemarelids);

		InvalidatePublicationRels(relids);
	}

	ObjectAddressSet(obj, PublicationRelationId, pubform->oid);
	EventTriggerCollectSimpleCommand(obj, InvalidObjectAddress,
									 (Node *) stmt);

	InvokeObjectPostAlterHook(PublicationRelationId, pubform->oid, 0);
}

/*
 * Invalidate the relations.
 *
 * 使这些关系失效。
 */
void
InvalidatePublicationRels(List *relids)
{
	/*
	 * We don't want to send too many individual messages, at some point it's
	 * cheaper to just reset whole relcache.
	 *
	 * 不想发送太多单独的消息，到一定程度直接重置整个 relcache 更便宜。
	 */
	if (list_length(relids) < MAX_RELCACHE_INVAL_MSGS)
	{
		ListCell   *lc;

		foreach(lc, relids)
			CacheInvalidateRelcacheByRelid(lfirst_oid(lc));
	}
	else
		CacheInvalidateRelcacheAll();
}

/*
 * Add or remove table to/from publication.
 *
 * 向发布添加表或从发布中移除表。
 */
static void
AlterPublicationTables(AlterPublicationStmt *stmt, HeapTuple tup,
					   List *tables, const char *queryString,
					   bool publish_schema)
{
	List	   *rels = NIL;
	Form_pg_publication pubform = (Form_pg_publication) GETSTRUCT(tup);
	Oid			pubid = pubform->oid;

	/*
	 * Nothing to do if no objects, except in SET: for that it is quite
	 * possible that user has not specified any tables in which case we need
	 * to remove all the existing tables.
	 *
	 * 没有对象时通常无事可做，但 SET 例外：用户可能没指定任何表，此时需要移除全部现有表。
	 */
	if (!tables && stmt->action != AP_SetObjects)
		return;

	rels = OpenTableList(tables);

	if (stmt->action == AP_AddObjects)
	{
		TransformPubWhereClauses(rels, queryString, pubform->pubviaroot);

		publish_schema |= is_schema_publication(pubid);

		CheckPubRelationColumnList(stmt->pubname, rels, publish_schema,
								   pubform->pubviaroot);

		PublicationAddTables(pubid, rels, false, stmt);
	}
	else if (stmt->action == AP_DropObjects)
		PublicationDropTables(pubid, rels, false);
	else						/* AP_SetObjects */
	/*
	 *
	 * AP_SetObjects。
	 */
	{
		List	   *oldrelids = GetPublicationRelations(pubid,
														PUBLICATION_PART_ROOT);
		List	   *delrels = NIL;
		ListCell   *oldlc;

		TransformPubWhereClauses(rels, queryString, pubform->pubviaroot);

		CheckPubRelationColumnList(stmt->pubname, rels, publish_schema,
								   pubform->pubviaroot);

		/*
		 * To recreate the relation list for the publication, look for
		 * existing relations that do not need to be dropped.
		 *
		 * 为重建发布的关系列表，找出不必删除的现有关系。
		 */
		foreach(oldlc, oldrelids)
		{
			Oid			oldrelid = lfirst_oid(oldlc);
			ListCell   *newlc;
			PublicationRelInfo *oldrel;
			bool		found = false;
			HeapTuple	rftuple;
			Node	   *oldrelwhereclause = NULL;
			Bitmapset  *oldcolumns = NULL;

			/* look up the cache for the old relmap */
			/*
			 *
			 * 在缓存中查找旧的 relmap。
			 */
			rftuple = SearchSysCache2(PUBLICATIONRELMAP,
									  ObjectIdGetDatum(oldrelid),
									  ObjectIdGetDatum(pubid));

			/*
			 * See if the existing relation currently has a WHERE clause or a
			 * column list. We need to compare those too.
			 *
			 * 查看现有关系当前是否有 WHERE 子句或列清单。这些也要比较。
			 */
			if (HeapTupleIsValid(rftuple))
			{
				bool		isnull = true;
				Datum		whereClauseDatum;
				Datum		columnListDatum;

				/* Load the WHERE clause for this table. */
				/*
				 *
				 * 装载该表的 WHERE 子句。
				 */
				whereClauseDatum = SysCacheGetAttr(PUBLICATIONRELMAP, rftuple,
												   Anum_pg_publication_rel_prqual,
												   &isnull);
				if (!isnull)
					oldrelwhereclause = stringToNode(TextDatumGetCString(whereClauseDatum));

				/* Transform the int2vector column list to a bitmap. */
				/*
				 *
				 * 把 int2vector 列清单转换成位图。
				 */
				columnListDatum = SysCacheGetAttr(PUBLICATIONRELMAP, rftuple,
												  Anum_pg_publication_rel_prattrs,
												  &isnull);

				if (!isnull)
					oldcolumns = pub_collist_to_bitmapset(NULL, columnListDatum, NULL);

				ReleaseSysCache(rftuple);
			}

			foreach(newlc, rels)
			{
				PublicationRelInfo *newpubrel;
				Oid			newrelid;
				Bitmapset  *newcolumns = NULL;

				newpubrel = (PublicationRelInfo *) lfirst(newlc);
				newrelid = RelationGetRelid(newpubrel->relation);

				/*
				 * Validate the column list.  If the column list or WHERE
				 * clause changes, then the validation done here will be
				 * duplicated inside PublicationAddTables().  The validation
				 * is cheap enough that that seems harmless.
				 *
				 * 校验列清单。若列清单或 WHERE 子句有变化，这里的校验会在 PublicationAddTables() 里再做一次。
				 * 校验足够便宜，重复也无妨。
				 */
				newcolumns = pub_collist_validate(newpubrel->relation,
												  newpubrel->columns);

				/*
				 * Check if any of the new set of relations matches with the
				 * existing relations in the publication. Additionally, if the
				 * relation has an associated WHERE clause, check the WHERE
				 * expressions also match. Same for the column list. Drop the
				 * rest.
				 *
				 * 检查新关系集合中是否有与发布中现有关系匹配的。若关系有 WHERE 子句，还要检查表达式也匹配。列清单同样处理。其余的删除。
				 */
				if (newrelid == oldrelid)
				{
					if (equal(oldrelwhereclause, newpubrel->whereClause) &&
						bms_equal(oldcolumns, newcolumns))
					{
						found = true;
						break;
					}
				}
			}

			/*
			 * Add the non-matched relations to a list so that they can be
			 * dropped.
			 *
			 * 把未匹配的关系加入列表以便删除。
			 */
			if (!found)
			{
				oldrel = palloc(sizeof(PublicationRelInfo));
				oldrel->whereClause = NULL;
				oldrel->columns = NIL;
				oldrel->relation = table_open(oldrelid,
											  ShareUpdateExclusiveLock);
				delrels = lappend(delrels, oldrel);
			}
		}

		/* And drop them. */
		/*
		 *
		 * 然后删除它们。
		 */
		PublicationDropTables(pubid, delrels, true);

		/*
		 * Don't bother calculating the difference for adding, we'll catch and
		 * skip existing ones when doing catalog update.
		 *
		 * 添加时不必计算差集，更新目录时会发现并跳过已存在的。
		 */
		PublicationAddTables(pubid, rels, true, stmt);

		CloseTableList(delrels);
	}

	CloseTableList(rels);
}

/*
 * Alter the publication schemas.
 *
 * 修改发布的模式。
 *
 * Add or remove schemas to/from publication.
 *
 * 向发布添加模式或从发布中移除模式。
 */
static void
AlterPublicationSchemas(AlterPublicationStmt *stmt,
						HeapTuple tup, List *schemaidlist)
{
	Form_pg_publication pubform = (Form_pg_publication) GETSTRUCT(tup);

	/*
	 * Nothing to do if no objects, except in SET: for that it is quite
	 * possible that user has not specified any schemas in which case we need
	 * to remove all the existing schemas.
	 *
	 * 没有对象时通常无事可做，但 SET 例外：用户可能没指定任何模式，此时需要移除全部现有模式。
	 */
	if (!schemaidlist && stmt->action != AP_SetObjects)
		return;

	/*
	 * Schema lock is held until the publication is altered to prevent
	 * concurrent schema deletion.
	 *
	 * 模式锁保持到发布修改完成，以防止并发删除模式。
	 */
	LockSchemaList(schemaidlist);
	if (stmt->action == AP_AddObjects)
	{
		ListCell   *lc;
		List	   *reloids;

		reloids = GetPublicationRelations(pubform->oid, PUBLICATION_PART_ROOT);

		foreach(lc, reloids)
		{
			HeapTuple	coltuple;

			coltuple = SearchSysCache2(PUBLICATIONRELMAP,
									   ObjectIdGetDatum(lfirst_oid(lc)),
									   ObjectIdGetDatum(pubform->oid));

			if (!HeapTupleIsValid(coltuple))
				continue;

			/*
			 * Disallow adding schema if column list is already part of the
			 * publication. See CheckPubRelationColumnList.
			 *
			 * 若发布已包含列清单，则禁止再添加模式。见 CheckPubRelationColumnList。
			 */
			if (!heap_attisnull(coltuple, Anum_pg_publication_rel_prattrs, NULL))
				ereport(ERROR,
						errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("cannot add schema to publication \"%s\"",
							   stmt->pubname),
						errdetail("Schemas cannot be added if any tables that specify a column list are already part of the publication."));

			ReleaseSysCache(coltuple);
		}

		PublicationAddSchemas(pubform->oid, schemaidlist, false, stmt);
	}
	else if (stmt->action == AP_DropObjects)
		PublicationDropSchemas(pubform->oid, schemaidlist, false);
	else						/* AP_SetObjects */
	/*
	 *
	 * AP_SetObjects。
	 */
	{
		List	   *oldschemaids = GetPublicationSchemas(pubform->oid);
		List	   *delschemas = NIL;

		/* Identify which schemas should be dropped */
		/*
		 *
		 * 确定应删除哪些模式。
		 */
		delschemas = list_difference_oid(oldschemaids, schemaidlist);

		/*
		 * Schema lock is held until the publication is altered to prevent
		 * concurrent schema deletion.
		 *
		 * 模式锁保持到发布修改完成，以防止并发删除模式。
		 */
		LockSchemaList(delschemas);

		/* And drop them */
		/*
		 *
		 * 然后删除它们。
		 */
		PublicationDropSchemas(pubform->oid, delschemas, true);

		/*
		 * Don't bother calculating the difference for adding, we'll catch and
		 * skip existing ones when doing catalog update.
		 *
		 * 添加时不必计算差集，更新目录时会发现并跳过已存在的。
		 */
		PublicationAddSchemas(pubform->oid, schemaidlist, true, stmt);
	}
}

/*
 * Check if relations and schemas can be in a given publication and throw
 * appropriate error if not.
 *
 * 检查关系和模式能否属于给定发布，不能则报出相应错误。
 */
static void
CheckAlterPublication(AlterPublicationStmt *stmt, HeapTuple tup,
					  List *tables, List *schemaidlist)
{
	Form_pg_publication pubform = (Form_pg_publication) GETSTRUCT(tup);

	if ((stmt->action == AP_AddObjects || stmt->action == AP_SetObjects) &&
		schemaidlist && !superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to add or set schemas")));

	/*
	 * Check that user is allowed to manipulate the publication tables in
	 * schema
	 *
	 * 检查用户是否被允许操作该模式中的发布表。
	 */
	if (schemaidlist && pubform->puballtables)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("publication \"%s\" is defined as FOR ALL TABLES",
						NameStr(pubform->pubname)),
				 errdetail("Schemas cannot be added to or dropped from FOR ALL TABLES publications.")));

	/* Check that user is allowed to manipulate the publication tables. */
	/*
	 *
	 * 检查用户是否被允许操作这些发布表。
	 */
	if (tables && pubform->puballtables)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("publication \"%s\" is defined as FOR ALL TABLES",
						NameStr(pubform->pubname)),
				 errdetail("Tables cannot be added to or dropped from FOR ALL TABLES publications.")));
}

/*
 * Alter the existing publication.
 *
 * 修改现有发布。
 *
 * This is dispatcher function for AlterPublicationOptions,
 * AlterPublicationSchemas and AlterPublicationTables.
 *
 * 这是 AlterPublicationOptions、AlterPublicationSchemas 和 AlterPublicationTables 的分发函数。
 */
void
AlterPublication(ParseState *pstate, AlterPublicationStmt *stmt)
{
	Relation	rel;
	HeapTuple	tup;
	Form_pg_publication pubform;

	rel = table_open(PublicationRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(PUBLICATIONNAME,
							  CStringGetDatum(stmt->pubname));

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("publication \"%s\" does not exist",
						stmt->pubname)));

	pubform = (Form_pg_publication) GETSTRUCT(tup);

	/* must be owner */
	/*
	 *
	 * 必须是属主。
	 */
	if (!object_ownercheck(PublicationRelationId, pubform->oid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_PUBLICATION,
					   stmt->pubname);

	if (stmt->options)
		AlterPublicationOptions(pstate, stmt, rel, tup);
	else
	{
		List	   *relations = NIL;
		List	   *schemaidlist = NIL;
		Oid			pubid = pubform->oid;

		ObjectsInPublicationToOids(stmt->pubobjects, pstate, &relations,
								   &schemaidlist);

		CheckAlterPublication(stmt, tup, relations, schemaidlist);

		heap_freetuple(tup);

		/* Lock the publication so nobody else can do anything with it. */
		/*
		 *
		 * 锁住发布，使别人不能同时操作它。
		 */
		LockDatabaseObject(PublicationRelationId, pubid, 0,
						   AccessExclusiveLock);

		/*
		 * It is possible that by the time we acquire the lock on publication,
		 * concurrent DDL has removed it. We can test this by checking the
		 * existence of publication. We get the tuple again to avoid the risk
		 * of any publication option getting changed.
		 *
		 * 等到取得发布锁时，并发 DDL 可能已经删掉它。可以通过检查发布是否存在来发现。
		 * 再次取元组，以免发布选项在此期间被改掉。
		 */
		tup = SearchSysCacheCopy1(PUBLICATIONOID, ObjectIdGetDatum(pubid));
		if (!HeapTupleIsValid(tup))
			ereport(ERROR,
					errcode(ERRCODE_UNDEFINED_OBJECT),
					errmsg("publication \"%s\" does not exist",
						   stmt->pubname));

		AlterPublicationTables(stmt, tup, relations, pstate->p_sourcetext,
							   schemaidlist != NIL);
		AlterPublicationSchemas(stmt, tup, schemaidlist);
	}

	/* Cleanup. */
	/*
	 *
	 * 清理。
	 */
	heap_freetuple(tup);
	table_close(rel, RowExclusiveLock);
}

/*
 * Remove relation from publication by mapping OID.
 *
 * 按映射 OID 把关系从发布中移除。
 */
void
RemovePublicationRelById(Oid proid)
{
	Relation	rel;
	HeapTuple	tup;
	Form_pg_publication_rel pubrel;
	List	   *relids = NIL;

	rel = table_open(PublicationRelRelationId, RowExclusiveLock);

	tup = SearchSysCache1(PUBLICATIONREL, ObjectIdGetDatum(proid));

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for publication table %u",
			 proid);

	pubrel = (Form_pg_publication_rel) GETSTRUCT(tup);

	/*
	 * Invalidate relcache so that publication info is rebuilt.
	 *
	 * 使 relcache 失效，以便重建发布信息。
	 *
	 * For the partitioned tables, we must invalidate all partitions contained
	 * in the respective partition hierarchies, not just the one explicitly
	 * mentioned in the publication. This is required because we implicitly
	 * publish the child tables when the parent table is published.
	 *
	 * 对分区表，必须使相应分区层次中的所有分区失效，而不只是发布里显式提到的那个。
	 * 这是因为发布父表时会隐式发布子表。
	 */
	relids = GetPubPartitionOptionRelations(relids, PUBLICATION_PART_ALL,
											pubrel->prrelid);

	InvalidatePublicationRels(relids);

	CatalogTupleDelete(rel, &tup->t_self);

	ReleaseSysCache(tup);

	table_close(rel, RowExclusiveLock);
}

/*
 * Remove the publication by mapping OID.
 *
 * 按映射 OID 删除发布。
 */
void
RemovePublicationById(Oid pubid)
{
	Relation	rel;
	HeapTuple	tup;
	Form_pg_publication pubform;

	rel = table_open(PublicationRelationId, RowExclusiveLock);

	tup = SearchSysCache1(PUBLICATIONOID, ObjectIdGetDatum(pubid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for publication %u", pubid);

	pubform = (Form_pg_publication) GETSTRUCT(tup);

	/* Invalidate relcache so that publication info is rebuilt. */
	/*
	 *
	 * 使 relcache 失效，以便重建发布信息。
	 */
	if (pubform->puballtables)
		CacheInvalidateRelcacheAll();

	CatalogTupleDelete(rel, &tup->t_self);

	ReleaseSysCache(tup);

	table_close(rel, RowExclusiveLock);
}

/*
 * Remove schema from publication by mapping OID.
 *
 * 按映射 OID 把模式从发布中移除。
 */
void
RemovePublicationSchemaById(Oid psoid)
{
	Relation	rel;
	HeapTuple	tup;
	List	   *schemaRels = NIL;
	Form_pg_publication_namespace pubsch;

	rel = table_open(PublicationNamespaceRelationId, RowExclusiveLock);

	tup = SearchSysCache1(PUBLICATIONNAMESPACE, ObjectIdGetDatum(psoid));

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for publication schema %u", psoid);

	pubsch = (Form_pg_publication_namespace) GETSTRUCT(tup);

	/*
	 * Invalidate relcache so that publication info is rebuilt. See
	 * RemovePublicationRelById for why we need to consider all the
	 * partitions.
	 *
	 * 使 relcache 失效以便重建发布信息。为何要考虑所有分区见 RemovePublicationRelById。
	 */
	schemaRels = GetSchemaPublicationRelations(pubsch->pnnspid,
											   PUBLICATION_PART_ALL);
	InvalidatePublicationRels(schemaRels);

	CatalogTupleDelete(rel, &tup->t_self);

	ReleaseSysCache(tup);

	table_close(rel, RowExclusiveLock);
}

/*
 * Open relations specified by a PublicationTable list.
 * The returned tables are locked in ShareUpdateExclusiveLock mode in order to
 * add them to a publication.
 *
 * 打开 PublicationTable 列表指定的关系。返回的表以 ShareUpdateExclusiveLock 加锁，以便加入发布。
 */
static List *
OpenTableList(List *tables)
{
	List	   *relids = NIL;
	List	   *rels = NIL;
	ListCell   *lc;
	List	   *relids_with_rf = NIL;
	List	   *relids_with_collist = NIL;

	/*
	 * Open, share-lock, and check all the explicitly-specified relations
	 *
	 * 打开、共享锁并检查所有显式指定的关系。
	 */
	foreach(lc, tables)
	{
		PublicationTable *t = lfirst_node(PublicationTable, lc);
		bool		recurse = t->relation->inh;
		Relation	rel;
		Oid			myrelid;
		PublicationRelInfo *pub_rel;

		/* Allow query cancel in case this takes a long time */
		/*
		 *
		 * 若耗时很长，允许取消查询。
		 */
		CHECK_FOR_INTERRUPTS();

		rel = table_openrv(t->relation, ShareUpdateExclusiveLock);
		myrelid = RelationGetRelid(rel);

		/*
		 * Filter out duplicates if user specifies "foo, foo".
		 *
		 * 若用户写成 foo, foo，则去掉重复项。
		 *
		 * Note that this algorithm is known to not be very efficient (O(N^2))
		 * but given that it only works on list of tables given to us by user
		 * it's deemed acceptable.
		 *
		 * 这个算法效率不高（O(N^2)），但它只处理用户给出的表列表，因此可以接受。
		 */
		if (list_member_oid(relids, myrelid))
		{
			/* Disallow duplicate tables if there are any with row filters. */
			/*
			 *
			 * 若其中有带行过滤器的表，则禁止重复表。
			 */
			if (t->whereClause || list_member_oid(relids_with_rf, myrelid))
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_OBJECT),
						 errmsg("conflicting or redundant WHERE clauses for table \"%s\"",
								RelationGetRelationName(rel))));

			/* Disallow duplicate tables if there are any with column lists. */
			/*
			 *
			 * 若其中有带列清单的表，则禁止重复表。
			 */
			if (t->columns || list_member_oid(relids_with_collist, myrelid))
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_OBJECT),
						 errmsg("conflicting or redundant column lists for table \"%s\"",
								RelationGetRelationName(rel))));

			table_close(rel, ShareUpdateExclusiveLock);
			continue;
		}

		pub_rel = palloc(sizeof(PublicationRelInfo));
		pub_rel->relation = rel;
		pub_rel->whereClause = t->whereClause;
		pub_rel->columns = t->columns;
		rels = lappend(rels, pub_rel);
		relids = lappend_oid(relids, myrelid);

		if (t->whereClause)
			relids_with_rf = lappend_oid(relids_with_rf, myrelid);

		if (t->columns)
			relids_with_collist = lappend_oid(relids_with_collist, myrelid);

		/*
		 * Add children of this rel, if requested, so that they too are added
		 * to the publication.  A partitioned table can't have any inheritance
		 * children other than its partitions, which need not be explicitly
		 * added to the publication.
		 *
		 * 若要求，则加入该关系的子表，使它们也被加入发布。分区表除分区外没有其他继承子表，
		 * 而这些分区不必显式加入发布。
		 */
		if (recurse && rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		{
			List	   *children;
			ListCell   *child;

			children = find_all_inheritors(myrelid, ShareUpdateExclusiveLock,
										   NULL);

			foreach(child, children)
			{
				Oid			childrelid = lfirst_oid(child);

				/* Allow query cancel in case this takes a long time */
				/*
				 *
				 * 若耗时很长，允许取消查询。
				 */
				CHECK_FOR_INTERRUPTS();

				/*
				 * Skip duplicates if user specified both parent and child
				 * tables.
				 *
				 * 若用户同时指定了父表和子表，则跳过重复项。
				 */
				if (list_member_oid(relids, childrelid))
				{
					/*
					 * We don't allow to specify row filter for both parent
					 * and child table at the same time as it is not very
					 * clear which one should be given preference.
					 *
					 * 不允许同时为父表和子表指定行过滤器，因为不清楚该优先用哪一个。
					 */
					if (childrelid != myrelid &&
						(t->whereClause || list_member_oid(relids_with_rf, childrelid)))
						ereport(ERROR,
								(errcode(ERRCODE_DUPLICATE_OBJECT),
								 errmsg("conflicting or redundant WHERE clauses for table \"%s\"",
										RelationGetRelationName(rel))));

					/*
					 * We don't allow to specify column list for both parent
					 * and child table at the same time as it is not very
					 * clear which one should be given preference.
					 *
					 * 不允许同时为父表和子表指定列清单，因为不清楚该优先用哪一个。
					 */
					if (childrelid != myrelid &&
						(t->columns || list_member_oid(relids_with_collist, childrelid)))
						ereport(ERROR,
								(errcode(ERRCODE_DUPLICATE_OBJECT),
								 errmsg("conflicting or redundant column lists for table \"%s\"",
										RelationGetRelationName(rel))));

					continue;
				}

				/* find_all_inheritors already got lock */
				/*
				 *
				 * find_all_inheritors 已经取得锁。
				 */
				rel = table_open(childrelid, NoLock);
				pub_rel = palloc(sizeof(PublicationRelInfo));
				pub_rel->relation = rel;
				/* child inherits WHERE clause from parent */
				/*
				 *
				 * 子表继承父表的 WHERE 子句。
				 */
				pub_rel->whereClause = t->whereClause;

				/* child inherits column list from parent */
				/*
				 *
				 * 子表继承父表的列清单。
				 */
				pub_rel->columns = t->columns;
				rels = lappend(rels, pub_rel);
				relids = lappend_oid(relids, childrelid);

				if (t->whereClause)
					relids_with_rf = lappend_oid(relids_with_rf, childrelid);

				if (t->columns)
					relids_with_collist = lappend_oid(relids_with_collist, childrelid);
			}
		}
	}

	list_free(relids);
	list_free(relids_with_rf);

	return rels;
}

/*
 * Close all relations in the list.
 *
 * 关闭列表中的所有关系。
 */
static void
CloseTableList(List *rels)
{
	ListCell   *lc;

	foreach(lc, rels)
	{
		PublicationRelInfo *pub_rel;

		pub_rel = (PublicationRelInfo *) lfirst(lc);
		table_close(pub_rel->relation, NoLock);
	}

	list_free_deep(rels);
}

/*
 * Lock the schemas specified in the schema list in AccessShareLock mode in
 * order to prevent concurrent schema deletion.
 *
 * 以 AccessShareLock 锁住模式列表中的模式，以防止并发删除模式。
 */
static void
LockSchemaList(List *schemalist)
{
	ListCell   *lc;

	foreach(lc, schemalist)
	{
		Oid			schemaid = lfirst_oid(lc);

		/* Allow query cancel in case this takes a long time */
		/*
		 *
		 * 若耗时很长，允许取消查询。
		 */
		CHECK_FOR_INTERRUPTS();
		LockDatabaseObject(NamespaceRelationId, schemaid, 0, AccessShareLock);

		/*
		 * It is possible that by the time we acquire the lock on schema,
		 * concurrent DDL has removed it. We can test this by checking the
		 * existence of schema.
		 *
		 * 等到取得模式锁时，并发 DDL 可能已经删掉它。可以通过检查模式是否存在来发现。
		 */
		if (!SearchSysCacheExists1(NAMESPACEOID, ObjectIdGetDatum(schemaid)))
			ereport(ERROR,
					errcode(ERRCODE_UNDEFINED_SCHEMA),
					errmsg("schema with OID %u does not exist", schemaid));
	}
}

/*
 * Add listed tables to the publication.
 *
 * 把列出的表加入发布。
 */
static void
PublicationAddTables(Oid pubid, List *rels, bool if_not_exists,
					 AlterPublicationStmt *stmt)
{
	ListCell   *lc;

	Assert(!stmt || !stmt->for_all_tables);

	foreach(lc, rels)
	{
		PublicationRelInfo *pub_rel = (PublicationRelInfo *) lfirst(lc);
		Relation	rel = pub_rel->relation;
		ObjectAddress obj;

		/* Must be owner of the table or superuser. */
		/*
		 *
		 * 必须是表的属主或超级用户。
		 */
		if (!object_ownercheck(RelationRelationId, RelationGetRelid(rel), GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(rel->rd_rel->relkind),
						   RelationGetRelationName(rel));

		obj = publication_add_relation(pubid, pub_rel, if_not_exists);
		if (stmt)
		{
			EventTriggerCollectSimpleCommand(obj, InvalidObjectAddress,
											 (Node *) stmt);

			InvokeObjectPostCreateHook(PublicationRelRelationId,
									   obj.objectId, 0);
		}
	}
}

/*
 * Remove listed tables from the publication.
 *
 * 把列出的表从发布中移除。
 */
static void
PublicationDropTables(Oid pubid, List *rels, bool missing_ok)
{
	ObjectAddress obj;
	ListCell   *lc;
	Oid			prid;

	foreach(lc, rels)
	{
		PublicationRelInfo *pubrel = (PublicationRelInfo *) lfirst(lc);
		Relation	rel = pubrel->relation;
		Oid			relid = RelationGetRelid(rel);

		if (pubrel->columns)
			ereport(ERROR,
					errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("column list must not be specified in ALTER PUBLICATION ... DROP"));

		prid = GetSysCacheOid2(PUBLICATIONRELMAP, Anum_pg_publication_rel_oid,
							   ObjectIdGetDatum(relid),
							   ObjectIdGetDatum(pubid));
		if (!OidIsValid(prid))
		{
			if (missing_ok)
				continue;

			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("relation \"%s\" is not part of the publication",
							RelationGetRelationName(rel))));
		}

		if (pubrel->whereClause)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("cannot use a WHERE clause when removing a table from a publication")));

		ObjectAddressSet(obj, PublicationRelRelationId, prid);
		performDeletion(&obj, DROP_CASCADE, 0);
	}
}

/*
 * Add listed schemas to the publication.
 *
 * 把列出的模式加入发布。
 */
static void
PublicationAddSchemas(Oid pubid, List *schemas, bool if_not_exists,
					  AlterPublicationStmt *stmt)
{
	ListCell   *lc;

	Assert(!stmt || !stmt->for_all_tables);

	foreach(lc, schemas)
	{
		Oid			schemaid = lfirst_oid(lc);
		ObjectAddress obj;

		obj = publication_add_schema(pubid, schemaid, if_not_exists);
		if (stmt)
		{
			EventTriggerCollectSimpleCommand(obj, InvalidObjectAddress,
											 (Node *) stmt);

			InvokeObjectPostCreateHook(PublicationNamespaceRelationId,
									   obj.objectId, 0);
		}
	}
}

/*
 * Remove listed schemas from the publication.
 *
 * 把列出的模式从发布中移除。
 */
static void
PublicationDropSchemas(Oid pubid, List *schemas, bool missing_ok)
{
	ObjectAddress obj;
	ListCell   *lc;
	Oid			psid;

	foreach(lc, schemas)
	{
		Oid			schemaid = lfirst_oid(lc);

		psid = GetSysCacheOid2(PUBLICATIONNAMESPACEMAP,
							   Anum_pg_publication_namespace_oid,
							   ObjectIdGetDatum(schemaid),
							   ObjectIdGetDatum(pubid));
		if (!OidIsValid(psid))
		{
			if (missing_ok)
				continue;

			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("tables from schema \"%s\" are not part of the publication",
							get_namespace_name(schemaid))));
		}

		ObjectAddressSet(obj, PublicationNamespaceRelationId, psid);
		performDeletion(&obj, DROP_CASCADE, 0);
	}
}

/*
 * Internal workhorse for changing a publication owner
 *
 * 更改发布属主的内部实现。
 */
static void
AlterPublicationOwner_internal(Relation rel, HeapTuple tup, Oid newOwnerId)
{
	Form_pg_publication form;

	form = (Form_pg_publication) GETSTRUCT(tup);

	if (form->pubowner == newOwnerId)
		return;

	if (!superuser())
	{
		AclResult	aclresult;

		/* Must be owner */
		/*
		 *
		 * 必须是属主。
		 */
		if (!object_ownercheck(PublicationRelationId, form->oid, GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_PUBLICATION,
						   NameStr(form->pubname));

		/* Must be able to become new owner */
		/*
		 *
		 * 必须能够成为新属主。
		 */
		check_can_set_role(GetUserId(), newOwnerId);

		/* New owner must have CREATE privilege on database */
		/*
		 *
		 * 新属主必须对数据库有 CREATE 权限。
		 */
		aclresult = object_aclcheck(DatabaseRelationId, MyDatabaseId, newOwnerId, ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_DATABASE,
						   get_database_name(MyDatabaseId));

		if (form->puballtables && !superuser_arg(newOwnerId))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to change owner of publication \"%s\"",
							NameStr(form->pubname)),
					 errhint("The owner of a FOR ALL TABLES publication must be a superuser.")));

		if (!superuser_arg(newOwnerId) && is_schema_publication(form->oid))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to change owner of publication \"%s\"",
							NameStr(form->pubname)),
					 errhint("The owner of a FOR TABLES IN SCHEMA publication must be a superuser.")));
	}

	form->pubowner = newOwnerId;
	CatalogTupleUpdate(rel, &tup->t_self, tup);

	/* Update owner dependency reference */
	/*
	 *
	 * 更新对属主的依赖引用。
	 */
	changeDependencyOnOwner(PublicationRelationId,
							form->oid,
							newOwnerId);

	InvokeObjectPostAlterHook(PublicationRelationId,
							  form->oid, 0);
}

/*
 * Change publication owner -- by name
 *
 * 按名称更改发布的属主。
 */
ObjectAddress
AlterPublicationOwner(const char *name, Oid newOwnerId)
{
	Oid			pubid;
	HeapTuple	tup;
	Relation	rel;
	ObjectAddress address;
	Form_pg_publication pubform;

	rel = table_open(PublicationRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(PUBLICATIONNAME, CStringGetDatum(name));

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("publication \"%s\" does not exist", name)));

	pubform = (Form_pg_publication) GETSTRUCT(tup);
	pubid = pubform->oid;

	AlterPublicationOwner_internal(rel, tup, newOwnerId);

	ObjectAddressSet(address, PublicationRelationId, pubid);

	heap_freetuple(tup);

	table_close(rel, RowExclusiveLock);

	return address;
}

/*
 * Change publication owner -- by OID
 *
 * 按 OID 更改发布的属主。
 */
void
AlterPublicationOwner_oid(Oid pubid, Oid newOwnerId)
{
	HeapTuple	tup;
	Relation	rel;

	rel = table_open(PublicationRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(PUBLICATIONOID, ObjectIdGetDatum(pubid));

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("publication with OID %u does not exist", pubid)));

	AlterPublicationOwner_internal(rel, tup, newOwnerId);

	heap_freetuple(tup);

	table_close(rel, RowExclusiveLock);
}

/*
 * Extract the publish_generated_columns option value from a DefElem. "stored"
 * and "none" values are accepted.
 *
 * 从 DefElem 取出 publish_generated_columns 选项值。接受 stored 和 none。
 */
static char
defGetGeneratedColsOption(DefElem *def)
{
	char	   *sval = "";

	/*
	 * A parameter value is required.
	 *
	 * 必须给出参数值。
	 */
	if (def->arg)
	{
		sval = defGetString(def);

		if (pg_strcasecmp(sval, "none") == 0)
			return PUBLISH_GENCOLS_NONE;
		if (pg_strcasecmp(sval, "stored") == 0)
			return PUBLISH_GENCOLS_STORED;
	}

	ereport(ERROR,
			errcode(ERRCODE_SYNTAX_ERROR),
			errmsg("invalid value for publication parameter \"%s\": \"%s\"", def->defname, sval),
			errdetail("Valid values are \"%s\" and \"%s\".", "none", "stored"));

	return PUBLISH_GENCOLS_NONE;	/* keep compiler quiet */
	/*
	 *
	 * 避免编译器告警。
	 */
}

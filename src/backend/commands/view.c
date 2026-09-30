/*-------------------------------------------------------------------------
 *
 * view.c
 *	  use rewrite rules to construct views
 *
 * 用重写规则构造视图。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/view.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relation.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "commands/tablecmds.h"
#include "commands/view.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/analyze.h"
#include "parser/parse_relation.h"
#include "rewrite/rewriteDefine.h"
#include "rewrite/rewriteHandler.h"
#include "rewrite/rewriteSupport.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

/*
 * 核心流程概览：
 * DefineView 分析 SELECT，处理 WITH CHECK OPTION、列别名和临时视图，
 * 再调用 DefineVirtualRelation 创建或替换视图。
 * DefineVirtualRelation 构造列定义，替换时核对列兼容性并补列，
 * 最后由 StoreViewQuery / DefineViewRules 把查询存成 ON SELECT 规则。
 */
static void checkViewColumns(TupleDesc newdesc, TupleDesc olddesc);

/*---------------------------------------------------------------------
 * DefineVirtualRelation
 *
 * DefineVirtualRelation：
 *
 * Create a view relation and use the rules system to store the query
 * for the view.
 *
 * 创建视图关系，并用规则系统保存视图查询。
 *
 * EventTriggerAlterTableStart must have been called already.
 *
 * EventTriggerAlterTableStart 必须已经调用过。
 *---------------------------------------------------------------------
 */
static ObjectAddress
DefineVirtualRelation(RangeVar *relation, List *tlist, bool replace,
					  List *options, Query *viewParse)
{
	Oid			viewOid;
	LOCKMODE	lockmode;
	List	   *attrList;
	ListCell   *t;

	/*
	 * create a list of ColumnDef nodes based on the names and types of the
	 * (non-junk) targetlist items from the view's SELECT list.
	 *
	 * 根据视图 SELECT 列表中非 junk 目标项的名字和类型，构造 ColumnDef 列表。
	 */
	attrList = NIL;
	foreach(t, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(t);

		if (!tle->resjunk)
		{
			ColumnDef  *def = makeColumnDef(tle->resname,
											exprType((Node *) tle->expr),
											exprTypmod((Node *) tle->expr),
											exprCollation((Node *) tle->expr));

			/*
			 * It's possible that the column is of a collatable type but the
			 * collation could not be resolved, so double-check.
			 *
			 * 列可能是可排序类型，但排序规则未能解析，这里再检查一次。
			 */
			if (type_is_collatable(exprType((Node *) tle->expr)))
			{
				if (!OidIsValid(def->collOid))
					ereport(ERROR,
							(errcode(ERRCODE_INDETERMINATE_COLLATION),
							 errmsg("could not determine which collation to use for view column \"%s\"",
									def->colname),
							 errhint("Use the COLLATE clause to set the collation explicitly.")));
			}
			else
				Assert(!OidIsValid(def->collOid));

			attrList = lappend(attrList, def);
		}
	}

	/*
	 * Look up, check permissions on, and lock the creation namespace; also
	 * check for a preexisting view with the same name.  This will also set
	 * relation->relpersistence to RELPERSISTENCE_TEMP if the selected
	 * namespace is temporary.
	 *
	 * 查找创建用的命名空间，检查权限并加锁，同时检查是否已有同名视图。
	 * 若选中的命名空间是临时的，会把 relation->relpersistence 设为 RELPERSISTENCE_TEMP。
	 */
	lockmode = replace ? AccessExclusiveLock : NoLock;
	(void) RangeVarGetAndCheckCreationNamespace(relation, lockmode, &viewOid);

	if (OidIsValid(viewOid) && replace)
	{
		Relation	rel;
		TupleDesc	descriptor;
		List	   *atcmds = NIL;
		AlterTableCmd *atcmd;
		ObjectAddress address;

		/* Relation is already locked, but we must build a relcache entry. */
		/*
		 *
		 * 关系已经加锁，但仍须建立 relcache 项。
		 */
		rel = relation_open(viewOid, NoLock);

		/* Make sure it *is* a view. */
		/*
		 *
		 * 确认它确实是视图。
		 */
		if (rel->rd_rel->relkind != RELKIND_VIEW)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is not a view",
							RelationGetRelationName(rel))));

		/* Also check it's not in use already */
		/*
		 *
		 * 同时确认它当前未被使用。
		 */
		CheckTableNotInUse(rel, "CREATE OR REPLACE VIEW");

		/*
		 * Due to the namespace visibility rules for temporary objects, we
		 * should only end up replacing a temporary view with another
		 * temporary view, and similarly for permanent views.
		 *
		 * 按临时对象的命名空间可见性规则，临时视图只能替换临时视图，永久视图同理。
		 */
		Assert(relation->relpersistence == rel->rd_rel->relpersistence);

		/*
		 * Create a tuple descriptor to compare against the existing view, and
		 * verify that the old column list is an initial prefix of the new
		 * column list.
		 *
		 * 构造元组描述符与现有视图比较，确认旧列清单是新列清单的前缀。
		 */
		descriptor = BuildDescForRelation(attrList);
		checkViewColumns(descriptor, rel->rd_att);

		/*
		 * If new attributes have been added, we must add pg_attribute entries
		 * for them.  It is convenient (although overkill) to use the ALTER
		 * TABLE ADD COLUMN infrastructure for this.
		 *
		 * 若新增了属性，必须补上 pg_attribute 项。这里借用 ALTER TABLE ADD COLUMN 的设施，虽然偏重。
		 *
		 * Note that we must do this before updating the query for the view,
		 * since the rules system requires that the correct view columns be in
		 * place when defining the new rules.
		 *
		 * 必须先做这一步再更新视图查询，因为定义新规则时规则系统要求视图列已经就位。
		 *
		 * Also note that ALTER TABLE doesn't run parse transformation on
		 * AT_AddColumnToView commands.  The ColumnDef we supply must be ready
		 * to execute as-is.
		 *
		 * ALTER TABLE 不会对 AT_AddColumnToView 做语法分析变换。提供的 ColumnDef 必须可以直接执行。
		 */
		if (list_length(attrList) > rel->rd_att->natts)
		{
			ListCell   *c;
			int			skip = rel->rd_att->natts;

			foreach(c, attrList)
			{
				if (skip > 0)
				{
					skip--;
					continue;
				}
				atcmd = makeNode(AlterTableCmd);
				atcmd->subtype = AT_AddColumnToView;
				atcmd->def = (Node *) lfirst(c);
				atcmds = lappend(atcmds, atcmd);
			}

			/* EventTriggerAlterTableStart called by ProcessUtilitySlow */
			/*
			 *
			 * EventTriggerAlterTableStart 已由 ProcessUtilitySlow 调用。
			 */
			AlterTableInternal(viewOid, atcmds, true);

			/* Make the new view columns visible */
			/*
			 *
			 * 使新的视图列可见。
			 */
			CommandCounterIncrement();
		}

		/*
		 * Update the query for the view.
		 *
		 * 更新视图的查询。
		 *
		 * Note that we must do this before updating the view options, because
		 * the new options may not be compatible with the old view query (for
		 * example if we attempt to add the WITH CHECK OPTION, we require that
		 * the new view be automatically updatable, but the old view may not
		 * have been).
		 *
		 * 必须先做这一步再更新视图选项。新选项可能与旧查询不兼容，
		 * 例如加上 WITH CHECK OPTION 时要求新视图可自动更新，而旧视图未必如此。
		 */
		StoreViewQuery(viewOid, viewParse, replace);

		/* Make the new view query visible */
		/*
		 *
		 * 使新的视图查询可见。
		 */
		CommandCounterIncrement();

		/*
		 * Update the view's options.
		 *
		 * 更新视图选项。
		 *
		 * The new options list replaces the existing options list, even if
		 * it's empty.
		 *
		 * 新选项列表替换旧列表，即使新列表为空。
		 */
		atcmd = makeNode(AlterTableCmd);
		atcmd->subtype = AT_ReplaceRelOptions;
		atcmd->def = (Node *) options;
		atcmds = list_make1(atcmd);

		/* EventTriggerAlterTableStart called by ProcessUtilitySlow */
		/*
		 *
		 * EventTriggerAlterTableStart 已由 ProcessUtilitySlow 调用。
		 */
		AlterTableInternal(viewOid, atcmds, true);

		/*
		 * There is very little to do here to update the view's dependencies.
		 * Most view-level dependency relationships, such as those on the
		 * owner, schema, and associated composite type, aren't changing.
		 * Because we don't allow changing type or collation of an existing
		 * view column, those dependencies of the existing columns don't
		 * change either, while the AT_AddColumnToView machinery took care of
		 * adding such dependencies for new view columns.  The dependencies of
		 * the view's query could have changed arbitrarily, but that was dealt
		 * with inside StoreViewQuery.  What remains is only to check that
		 * view replacement is allowed when we're creating an extension.
		 *
		 * 这里几乎不用更新视图依赖。属主、模式和关联复合类型等视图级依赖没有变化。
		 * 已有列的类型和排序规则不允许改，其依赖也不变；新列的依赖由 AT_AddColumnToView 处理。
		 * 查询依赖可能任意变化，但已在 StoreViewQuery 中处理。剩下只需检查创建扩展时是否允许替换视图。
		 */
		ObjectAddressSet(address, RelationRelationId, viewOid);

		recordDependencyOnCurrentExtension(&address, true);

		/*
		 * Seems okay, so return the OID of the pre-existing view.
		 *
		 * 检查通过，返回已有视图的 OID。
		 */
		relation_close(rel, NoLock);	/* keep the lock! */
		/*
		 *
		 * 保留锁。
		 */

		return address;
	}
	else
	{
		CreateStmt *createStmt = makeNode(CreateStmt);
		ObjectAddress address;

		/*
		 * Set the parameters for keys/inheritance etc. All of these are
		 * uninteresting for views...
		 *
		 * 设置键、继承等参数。对视图来说这些都不重要。
		 */
		createStmt->relation = relation;
		createStmt->tableElts = attrList;
		createStmt->inhRelations = NIL;
		createStmt->constraints = NIL;
		createStmt->options = options;
		createStmt->oncommit = ONCOMMIT_NOOP;
		createStmt->tablespacename = NULL;
		createStmt->if_not_exists = false;

		/*
		 * Create the relation (this will error out if there's an existing
		 * view, so we don't need more code to complain if "replace" is
		 * false).
		 *
		 * 创建关系。若已有视图会在这里报错，因此 replace 为 false 时不必再额外报错。
		 */
		address = DefineRelation(createStmt, RELKIND_VIEW, InvalidOid, NULL,
								 NULL);
		Assert(address.objectId != InvalidOid);

		/* Make the new view relation visible */
		/*
		 *
		 * 使新的视图关系可见。
		 */
		CommandCounterIncrement();

		/* Store the query for the view */
		/*
		 *
		 * 保存视图查询。
		 */
		StoreViewQuery(address.objectId, viewParse, replace);

		return address;
	}
}

/*
 * Verify that the columns associated with proposed new view definition match
 * the columns of the old view.  This is similar to equalRowTypes(), with code
 * added to generate specific complaints.  Also, we allow the new view to have
 * more columns than the old.
 *
 * 核对新视图定义的列与旧视图是否匹配。类似 equalRowTypes()，但会给出更具体的报错。
 * 允许新视图比旧视图多列。
 */
static void
checkViewColumns(TupleDesc newdesc, TupleDesc olddesc)
{
	int			i;

	if (newdesc->natts < olddesc->natts)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot drop columns from view")));

	for (i = 0; i < olddesc->natts; i++)
	{
		Form_pg_attribute newattr = TupleDescAttr(newdesc, i);
		Form_pg_attribute oldattr = TupleDescAttr(olddesc, i);

		/* XXX msg not right, but we don't support DROP COL on view anyway */
		/*
		 *
		 * XXX：这条消息不准确，但视图本来就不支持 DROP COLUMN。
		 */
		if (newattr->attisdropped != oldattr->attisdropped)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot drop columns from view")));

		if (strcmp(NameStr(newattr->attname), NameStr(oldattr->attname)) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot change name of view column \"%s\" to \"%s\"",
							NameStr(oldattr->attname),
							NameStr(newattr->attname)),
					 errhint("Use ALTER VIEW ... RENAME COLUMN ... to change name of view column instead.")));

		/*
		 * We cannot allow type, typmod, or collation to change, since these
		 * properties may be embedded in Vars of other views/rules referencing
		 * this one.  Other column attributes can be ignored.
		 *
		 * 不允许改变类型、typmod 或排序规则，因为引用本视图的其它视图或规则的 Var 可能嵌入了这些属性。
		 * 其它列属性可以忽略。
		 */
		if (newattr->atttypid != oldattr->atttypid ||
			newattr->atttypmod != oldattr->atttypmod)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot change data type of view column \"%s\" from %s to %s",
							NameStr(oldattr->attname),
							format_type_with_typemod(oldattr->atttypid,
													 oldattr->atttypmod),
							format_type_with_typemod(newattr->atttypid,
													 newattr->atttypmod))));

		/*
		 * At this point, attcollations should be both valid or both invalid,
		 * so applying get_collation_name unconditionally should be fine.
		 *
		 * 此时两边的 attcollation 应同为有效或同为无效，无条件调用 get_collation_name 是安全的。
		 */
		if (newattr->attcollation != oldattr->attcollation)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot change collation of view column \"%s\" from \"%s\" to \"%s\"",
							NameStr(oldattr->attname),
							get_collation_name(oldattr->attcollation),
							get_collation_name(newattr->attcollation))));
	}

	/*
	 * We ignore the constraint fields.  The new view desc can't have any
	 * constraints, and the only ones that could be on the old view are
	 * defaults, which we are happy to leave in place.
	 *
	 * 忽略约束字段。新视图描述符不会有约束；旧视图上可能只有默认值，保留即可。
	 */
}

/*
 * 为视图建立 ON SELECT 重写规则。
 */
static void
DefineViewRules(Oid viewOid, Query *viewParse, bool replace)
{
	/*
	 * Set up the ON SELECT rule.  Since the query has already been through
	 * parse analysis, we use DefineQueryRewrite() directly.
	 *
	 * 建立 ON SELECT 规则。查询已经过语法分析，直接调用 DefineQueryRewrite()。
	 */
	DefineQueryRewrite(pstrdup(ViewSelectRuleName),
					   viewOid,
					   NULL,
					   CMD_SELECT,
					   true,
					   replace,
					   list_make1(viewParse));

	/*
	 * Someday: automatic ON INSERT, etc
	 *
	 * 将来：自动的 ON INSERT 等。
	 */
}

/*
 * DefineView
 *		Execute a CREATE VIEW command.
 *
 * DefineView：执行 CREATE VIEW。
 */
ObjectAddress
DefineView(ViewStmt *stmt, const char *queryString,
		   int stmt_location, int stmt_len)
{
	RawStmt    *rawstmt;
	Query	   *viewParse;
	RangeVar   *view;
	ListCell   *cell;
	bool		check_option;
	ObjectAddress address;

	/*
	 * Run parse analysis to convert the raw parse tree to a Query.  Note this
	 * also acquires sufficient locks on the source table(s).
	 *
	 * 做语法分析，把原始分析树变成 Query。这也会给源表加上足够的锁。
	 */
	rawstmt = makeNode(RawStmt);
	rawstmt->stmt = stmt->query;
	rawstmt->stmt_location = stmt_location;
	rawstmt->stmt_len = stmt_len;

	viewParse = parse_analyze_fixedparams(rawstmt, queryString, NULL, 0, NULL);

	/*
	 * The grammar should ensure that the result is a single SELECT Query.
	 * However, it doesn't forbid SELECT INTO, so we have to check for that.
	 *
	 * 语法应保证结果是单条 SELECT Query。但它不禁止 SELECT INTO，因此这里要检查。
	 */
	if (!IsA(viewParse, Query))
		elog(ERROR, "unexpected parse analysis result");
	if (viewParse->utilityStmt != NULL &&
		IsA(viewParse->utilityStmt, CreateTableAsStmt))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("views must not contain SELECT INTO")));
	if (viewParse->commandType != CMD_SELECT)
		elog(ERROR, "unexpected parse analysis result");

	/*
	 * Check for unsupported cases.  These tests are redundant with ones in
	 * DefineQueryRewrite(), but that function will complain about a bogus ON
	 * SELECT rule, and we'd rather the message complain about a view.
	 *
	 * 检查不支持的情况。这些检查与 DefineQueryRewrite() 重复，
	 * 但那个函数会抱怨非法的 ON SELECT 规则，我们更希望错误指向视图。
	 */
	if (viewParse->hasModifyingCTE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("views must not contain data-modifying statements in WITH")));

	/*
	 * If the user specified the WITH CHECK OPTION, add it to the list of
	 * reloptions.
	 *
	 * 若用户指定了 WITH CHECK OPTION，把它加入 reloptions。
	 */
	if (stmt->withCheckOption == LOCAL_CHECK_OPTION)
		stmt->options = lappend(stmt->options,
								makeDefElem("check_option",
											(Node *) makeString("local"), -1));
	else if (stmt->withCheckOption == CASCADED_CHECK_OPTION)
		stmt->options = lappend(stmt->options,
								makeDefElem("check_option",
											(Node *) makeString("cascaded"), -1));

	/*
	 * Check that the view is auto-updatable if WITH CHECK OPTION was
	 * specified.
	 *
	 * 若指定了 WITH CHECK OPTION，检查视图是否可自动更新。
	 */
	check_option = false;

	foreach(cell, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(cell);

		if (strcmp(defel->defname, "check_option") == 0)
			check_option = true;
	}

	/*
	 * If the check option is specified, look to see if the view is actually
	 * auto-updatable or not.
	 *
	 * 若指定了 check option，判断视图实际上是否可自动更新。
	 */
	if (check_option)
	{
		const char *view_updatable_error =
			view_query_is_auto_updatable(viewParse, true);

		if (view_updatable_error)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("WITH CHECK OPTION is supported only on automatically updatable views"),
					 errhint("%s", _(view_updatable_error))));
	}

	/*
	 * If a list of column names was given, run through and insert these into
	 * the actual query tree. - thomas 2000-03-08
	 *
	 * 若给出了列名列表，把它们填进实际的查询树。
	 */
	if (stmt->aliases != NIL)
	{
		ListCell   *alist_item = list_head(stmt->aliases);
		ListCell   *targetList;

		foreach(targetList, viewParse->targetList)
		{
			TargetEntry *te = lfirst_node(TargetEntry, targetList);

			/* junk columns don't get aliases */
			/*
			 *
			 * junk 列不分配别名。
			 */
			if (te->resjunk)
				continue;
			te->resname = pstrdup(strVal(lfirst(alist_item)));
			alist_item = lnext(stmt->aliases, alist_item);
			if (alist_item == NULL)
				break;			/* done assigning aliases */
				/*
				 *
				 * 别名分配完成。
				 */
		}

		if (alist_item != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("CREATE VIEW specifies more column "
							"names than columns")));
	}

	/* Unlogged views are not sensible. */
	/*
	 *
	 * UNLOGGED 视图没有意义。
	 */
	if (stmt->view->relpersistence == RELPERSISTENCE_UNLOGGED)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("views cannot be unlogged because they do not have storage")));

	/*
	 * If the user didn't explicitly ask for a temporary view, check whether
	 * we need one implicitly.  We allow TEMP to be inserted automatically as
	 * long as the CREATE command is consistent with that --- no explicit
	 * schema name.
	 *
	 * 若用户没有显式要求临时视图，检查是否需要隐式做成临时的。
	 * 只要 CREATE 与此一致（没有显式模式名），就允许自动加上 TEMP。
	 */
	view = copyObject(stmt->view);	/* don't corrupt original command */
	/*
	 *
	 * 不要改坏原来的命令。
	 */
	if (view->relpersistence == RELPERSISTENCE_PERMANENT
		&& isQueryUsingTempRelation(viewParse))
	{
		view->relpersistence = RELPERSISTENCE_TEMP;
		ereport(NOTICE,
				(errmsg("view \"%s\" will be a temporary view",
						view->relname)));
	}

	/*
	 * Create the view relation
	 *
	 * 创建视图关系。
	 *
	 * NOTE: if it already exists and replace is false, the xact will be
	 * aborted.
	 *
	 * 注意：若已存在且 replace 为 false，事务会被中止。
	 */
	address = DefineVirtualRelation(view, viewParse->targetList,
									stmt->replace, stmt->options, viewParse);

	return address;
}

/*
 * Use the rules system to store the query for the view.
 *
 * 用规则系统保存视图查询。
 */
void
StoreViewQuery(Oid viewOid, Query *viewParse, bool replace)
{
	/*
	 * Now create the rules associated with the view.
	 *
	 * 现在创建与该视图关联的规则。
	 */
	DefineViewRules(viewOid, viewParse, replace);
}

/*-------------------------------------------------------------------------
 *
 * statscmds.c
 *	  Commands for creating and altering extended statistics objects
 *
 * 创建和修改扩展统计对象的命令。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/statscmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relation.h"
#include "access/table.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_statistic_ext_data.h"
#include "commands/comment.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "statistics/statistics.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/typcache.h"


/*
 * 核心流程概览：
 * 扩展统计对象的主路径是 CreateStatistics、AlterStatistics 以及删除例程。
 * 创建时解析 FROM 子句和统计类型，写入 pg_statistic_ext 并登记依赖。
 */
static char *ChooseExtendedStatisticName(const char *name1, const char *name2,
										 const char *label, Oid namespaceid);
static char *ChooseExtendedStatisticNameAddition(List *exprs);


/* qsort comparator for the attnums in CreateStatistics */
/*
 *
 * CreateStatistics 中对 attnum 做 qsort 的比较函数。
 */
static int
compare_int16(const void *a, const void *b)
{
	int			av = *(const int16 *) a;
	int			bv = *(const int16 *) b;

	/* this can't overflow if int is wider than int16 */
	/*
	 *
	 * 若 int 比 int16 更宽，这里不会溢出。
	 */
	return (av - bv);
}

/*
 *		CREATE STATISTICS
 *
 * 执行 CREATE STATISTICS。
 */
ObjectAddress
CreateStatistics(CreateStatsStmt *stmt, bool check_rights)
{
	int16		attnums[STATS_MAX_DIMENSIONS];
	int			nattnums = 0;
	int			numcols;
	char	   *namestr;
	NameData	stxname;
	Oid			statoid;
	Oid			namespaceId;
	Oid			stxowner = GetUserId();
	HeapTuple	htup;
	Datum		values[Natts_pg_statistic_ext];
	bool		nulls[Natts_pg_statistic_ext];
	int2vector *stxkeys;
	List	   *stxexprs = NIL;
	Datum		exprsDatum;
	Relation	statrel;
	Relation	rel = NULL;
	Oid			relid;
	ObjectAddress parentobject,
				myself;
	Datum		types[4];		/* one for each possible type of statistic */
	/*
	 *
	 * 每种可能的统计类型各一项。
	 */
	int			ntypes;
	ArrayType  *stxkind;
	bool		build_ndistinct;
	bool		build_dependencies;
	bool		build_mcv;
	bool		build_expressions;
	bool		requested_type = false;
	int			i;
	ListCell   *cell;
	ListCell   *cell2;

	Assert(IsA(stmt, CreateStatsStmt));

	/*
	 * Examine the FROM clause.  Currently, we only allow it to be a single
	 * simple table, but later we'll probably allow multiple tables and JOIN
	 * syntax.  The grammar is already prepared for that, so we have to check
	 * here that what we got is what we can support.
	 *
	 * 检查 FROM 子句。目前只允许它是一张简单表，以后可能会允许多表和 JOIN 语法。语法已经为此做好准备，因此必须在这里确认得到的是当前能支持的形式。
	 */
	if (list_length(stmt->relations) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("only a single relation is allowed in CREATE STATISTICS")));

	foreach(cell, stmt->relations)
	{
		Node	   *rln = (Node *) lfirst(cell);

		if (!IsA(rln, RangeVar))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("only a single relation is allowed in CREATE STATISTICS")));

		/*
		 * CREATE STATISTICS will influence future execution plans but does
		 * not interfere with currently executing plans.  So it should be
		 * enough to take only ShareUpdateExclusiveLock on relation,
		 * conflicting with ANALYZE and other DDL that sets statistical
		 * information, but not with normal queries.
		 *
		 * CREATE STATISTICS 会影响以后的执行计划，但不会干扰正在执行的计划。因此对关系只取 ShareUpdateExclusiveLock 就够了。它与 ANALYZE 以及其它设置统计信息的 DDL 冲突，但与普通查询不冲突。
		 */
		rel = relation_openrv((RangeVar *) rln, ShareUpdateExclusiveLock);

		/* Restrict to allowed relation types */
		/*
		 *
		 * 限制为允许的关系类型。
		 */
		if (rel->rd_rel->relkind != RELKIND_RELATION &&
			rel->rd_rel->relkind != RELKIND_MATVIEW &&
			rel->rd_rel->relkind != RELKIND_FOREIGN_TABLE &&
			rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot define statistics for relation \"%s\"",
							RelationGetRelationName(rel)),
					 errdetail_relkind_not_supported(rel->rd_rel->relkind)));

		/*
		 * You must own the relation to create stats on it.
		 *
		 * 必须拥有该关系才能在其上创建统计信息。
		 *
		 * NB: Concurrent changes could cause this function's lookup to find a
		 * different relation than a previous lookup by the caller, so we must
		 * perform this check even when check_rights == false.
		 *
		 * 注意：并发修改可能使本函数查到的关系与调用者先前查到的不同，因此即使 check_rights 为 false 也必须做这项检查。
		 */
		if (!object_ownercheck(RelationRelationId, RelationGetRelid(rel), stxowner))
			aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(rel->rd_rel->relkind),
						   RelationGetRelationName(rel));

		/* Creating statistics on system catalogs is not allowed */
		/*
		 *
		 * 不允许在系统目录上创建统计信息。
		 */
		if (!allowSystemTableMods && IsSystemRelation(rel))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied: \"%s\" is a system catalog",
							RelationGetRelationName(rel))));
	}

	Assert(rel);
	relid = RelationGetRelid(rel);

	/*
	 * If the node has a name, split it up and determine creation namespace.
	 * If not, put the object in the same namespace as the relation, and cons
	 * up a name for it.  (This can happen either via "CREATE STATISTICS ..."
	 * or via "CREATE TABLE ... (LIKE)".)
	 *
	 * 若节点有名字，则拆开并确定创建所用的命名空间。若没有，则把对象放在与关系相同的命名空间，并给它造一个名字。这可能来自 CREATE STATISTICS，也可能来自 CREATE TABLE ... (LIKE)。
	 */
	if (stmt->defnames)
		namespaceId = QualifiedNameGetCreationNamespace(stmt->defnames,
														&namestr);
	else
	{
		namespaceId = RelationGetNamespace(rel);
		namestr = ChooseExtendedStatisticName(RelationGetRelationName(rel),
											  ChooseExtendedStatisticNameAddition(stmt->exprs),
											  "stat",
											  namespaceId);
	}
	namestrcpy(&stxname, namestr);

	/*
	 * Check we have creation rights in target namespace.  Skip check if
	 * caller doesn't want it.
	 *
	 * 检查在目标命名空间中是否有创建权限。调用者不需要时跳过检查。
	 */
	if (check_rights)
	{
		AclResult	aclresult;

		aclresult = object_aclcheck(NamespaceRelationId, namespaceId,
									GetUserId(), ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_SCHEMA,
						   get_namespace_name(namespaceId));
	}

	/*
	 * Deal with the possibility that the statistics object already exists.
	 *
	 * 处理统计对象已经存在的情况。
	 */
	if (SearchSysCacheExists2(STATEXTNAMENSP,
							  CStringGetDatum(namestr),
							  ObjectIdGetDatum(namespaceId)))
	{
		if (stmt->if_not_exists)
		{
			/*
			 * Since stats objects aren't members of extensions (see comments
			 * below), no need for checkMembershipInCurrentExtension here.
			 *
			 * 统计对象不是扩展的成员（见下面的注释），因此这里不需要 checkMembershipInCurrentExtension。
			 */
			ereport(NOTICE,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("statistics object \"%s\" already exists, skipping",
							namestr)));
			relation_close(rel, NoLock);
			return InvalidObjectAddress;
		}

		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("statistics object \"%s\" already exists", namestr)));
	}

	/*
	 * Make sure no more than STATS_MAX_DIMENSIONS columns are used. There
	 * might be duplicates and so on, but we'll deal with those later.
	 *
	 * 确保使用的列不超过 STATS_MAX_DIMENSIONS。可能有重复等情况，但稍后再处理。
	 */
	numcols = list_length(stmt->exprs);
	if (numcols > STATS_MAX_DIMENSIONS)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("cannot have more than %d columns in statistics",
						STATS_MAX_DIMENSIONS)));

	/*
	 * Convert the expression list to a simple array of attnums, but also keep
	 * a list of more complex expressions.  While at it, enforce some
	 * constraints - we don't allow extended statistics on system attributes,
	 * and we require the data type to have a less-than operator.
	 *
	 * 把表达式列表转换成简单的 attnum 数组，同时保留较复杂表达式的列表。顺便强制一些约束：不允许在系统属性上建扩展统计，并且数据类型必须有小于运算符。
	 *
	 * There are many ways to "mask" a simple attribute reference as an
	 * expression, for example "(a+0)" etc. We can't possibly detect all of
	 * them, but we handle at least the simple case with the attribute in
	 * parens. There'll always be a way around this, if the user is determined
	 * (like the "(a+0)" example), but this makes it somewhat consistent with
	 * how indexes treat attributes/expressions.
	 *
	 * 有很多办法把简单属性引用伪装成表达式，例如 (a+0)。不可能全部检测出来，但至少处理属性被括号包起来的简单情况。下定决心的用户总能绕过（就像 (a+0)），但这与索引对待属性与表达式的方式大体一致。
	 */
	foreach(cell, stmt->exprs)
	{
		StatsElem  *selem = lfirst_node(StatsElem, cell);

		if (selem->name)		/* column reference */
		/*
		 *
		 * 列引用。
		 */
		{
			char	   *attname;
			HeapTuple	atttuple;
			Form_pg_attribute attForm;
			TypeCacheEntry *type;

			attname = selem->name;

			atttuple = SearchSysCacheAttName(relid, attname);
			if (!HeapTupleIsValid(atttuple))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("column \"%s\" does not exist",
								attname)));
			attForm = (Form_pg_attribute) GETSTRUCT(atttuple);

			/* Disallow use of system attributes in extended stats */
			/*
			 *
			 * 扩展统计中不允许使用系统属性。
			 */
			if (attForm->attnum <= 0)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("statistics creation on system columns is not supported")));

			/* Disallow use of virtual generated columns in extended stats */
			/*
			 *
			 * 扩展统计中不允许使用虚拟生成列。
			 */
			if (attForm->attgenerated == ATTRIBUTE_GENERATED_VIRTUAL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("statistics creation on virtual generated columns is not supported")));

			/* Disallow data types without a less-than operator */
			/*
			 *
			 * 不允许没有小于运算符的数据类型。
			 */
			type = lookup_type_cache(attForm->atttypid, TYPECACHE_LT_OPR);
			if (type->lt_opr == InvalidOid)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("column \"%s\" cannot be used in statistics because its type %s has no default btree operator class",
								attname, format_type_be(attForm->atttypid))));

			attnums[nattnums] = attForm->attnum;
			nattnums++;
			ReleaseSysCache(atttuple);
		}
		else if (IsA(selem->expr, Var)) /* column reference in parens */
		/*
		 *
		 * 括号中的列引用。
		 */
		{
			Var		   *var = (Var *) selem->expr;
			TypeCacheEntry *type;

			/* Disallow use of system attributes in extended stats */
			/*
			 *
			 * 扩展统计中不允许使用系统属性。
			 */
			if (var->varattno <= 0)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("statistics creation on system columns is not supported")));

			/* Disallow use of virtual generated columns in extended stats */
			/*
			 *
			 * 扩展统计中不允许使用虚拟生成列。
			 */
			if (get_attgenerated(relid, var->varattno) == ATTRIBUTE_GENERATED_VIRTUAL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("statistics creation on virtual generated columns is not supported")));

			/* Disallow data types without a less-than operator */
			/*
			 *
			 * 不允许没有小于运算符的数据类型。
			 */
			type = lookup_type_cache(var->vartype, TYPECACHE_LT_OPR);
			if (type->lt_opr == InvalidOid)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("column \"%s\" cannot be used in statistics because its type %s has no default btree operator class",
								get_attname(relid, var->varattno, false), format_type_be(var->vartype))));

			attnums[nattnums] = var->varattno;
			nattnums++;
		}
		else					/* expression */
		/*
		 *
		 * 表达式。
		 */
		{
			Node	   *expr = selem->expr;
			Oid			atttype;
			TypeCacheEntry *type;
			Bitmapset  *attnums = NULL;
			int			k;

			Assert(expr != NULL);

			pull_varattnos(expr, 1, &attnums);

			k = -1;
			while ((k = bms_next_member(attnums, k)) >= 0)
			{
				AttrNumber	attnum = k + FirstLowInvalidHeapAttributeNumber;

				/* Disallow expressions referencing system attributes. */
				/*
				 *
				 * 不允许表达式引用系统属性。
				 */
				if (attnum <= 0)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("statistics creation on system columns is not supported")));

				/* Disallow use of virtual generated columns in extended stats */
				/*
				 *
				 * 扩展统计中不允许使用虚拟生成列。
				 */
				if (get_attgenerated(relid, attnum) == ATTRIBUTE_GENERATED_VIRTUAL)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("statistics creation on virtual generated columns is not supported")));
			}

			/*
			 * Disallow data types without a less-than operator.
			 *
			 * 不允许没有小于运算符的数据类型。
			 *
			 * We ignore this for statistics on a single expression, in which
			 * case we'll build the regular statistics only (and that code can
			 * deal with such data types).
			 *
			 * 对单个表达式上的统计忽略这一点。那种情况只建立常规统计，而那段代码能够处理这类数据类型。
			 */
			if (list_length(stmt->exprs) > 1)
			{
				atttype = exprType(expr);
				type = lookup_type_cache(atttype, TYPECACHE_LT_OPR);
				if (type->lt_opr == InvalidOid)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("expression cannot be used in multivariate statistics because its type %s has no default btree operator class",
									format_type_be(atttype))));
			}

			stxexprs = lappend(stxexprs, expr);
		}
	}

	/*
	 * Parse the statistics kinds.
	 *
	 * 解析统计种类。
	 *
	 * First check that if this is the case with a single expression, there
	 * are no statistics kinds specified (we don't allow that for the simple
	 * CREATE STATISTICS form).
	 *
	 * 首先检查：若是单个表达式的情形，则不得指定统计种类（简单形式的 CREATE STATISTICS 不允许这样做）。
	 */
	if ((list_length(stmt->exprs) == 1) && (list_length(stxexprs) == 1))
	{
		/* statistics kinds not specified */
		/*
		 *
		 * 未指定统计种类。
		 */
		if (stmt->stat_types != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("when building statistics on a single expression, statistics kinds may not be specified")));
	}

	/* OK, let's check that we recognize the statistics kinds. */
	/*
	 *
	 * 确认我们认识这些统计种类。
	 */
	build_ndistinct = false;
	build_dependencies = false;
	build_mcv = false;
	foreach(cell, stmt->stat_types)
	{
		char	   *type = strVal(lfirst(cell));

		if (strcmp(type, "ndistinct") == 0)
		{
			build_ndistinct = true;
			requested_type = true;
		}
		else if (strcmp(type, "dependencies") == 0)
		{
			build_dependencies = true;
			requested_type = true;
		}
		else if (strcmp(type, "mcv") == 0)
		{
			build_mcv = true;
			requested_type = true;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized statistics kind \"%s\"",
							type)));
	}

	/*
	 * If no statistic type was specified, build them all (but only when the
	 * statistics is defined on more than one column/expression).
	 *
	 * 若未指定统计类型，则全部建立（但仅当统计定义在多于一个列或表达式上时）。
	 */
	if ((!requested_type) && (numcols >= 2))
	{
		build_ndistinct = true;
		build_dependencies = true;
		build_mcv = true;
	}

	/*
	 * When there are non-trivial expressions, build the expression stats
	 * automatically. This allows calculating good estimates for stats that
	 * consider per-clause estimates (e.g. functional dependencies).
	 *
	 * 存在非平凡表达式时，自动建立表达式统计。这样可以为考虑逐子句估计的统计项算出较好的估计值，例如函数依赖。
	 */
	build_expressions = (stxexprs != NIL);

	/*
	 * Check that at least two columns were specified in the statement, or
	 * that we're building statistics on a single expression.
	 *
	 * 检查语句中至少指定了两列，或者正在为单个表达式建立统计。
	 */
	if ((numcols < 2) && (list_length(stxexprs) != 1))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("extended statistics require at least 2 columns")));

	/*
	 * Sort the attnums, which makes detecting duplicates somewhat easier, and
	 * it does not hurt (it does not matter for the contents, unlike for
	 * indexes, for example).
	 *
	 * 对 attnum 排序，这样检测重复更容易一些，而且没有坏处（与索引不同，这里内容不依赖于顺序）。
	 */
	qsort(attnums, nattnums, sizeof(int16), compare_int16);

	/*
	 * Check for duplicates in the list of columns. The attnums are sorted so
	 * just check consecutive elements.
	 *
	 * 检查列列表中的重复。attnum 已排序，因此只检查相邻元素。
	 */
	for (i = 1; i < nattnums; i++)
	{
		if (attnums[i] == attnums[i - 1])
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_COLUMN),
					 errmsg("duplicate column name in statistics definition")));
	}

	/*
	 * Check for duplicate expressions. We do two loops, counting the
	 * occurrences of each expression. This is O(N^2) but we only allow small
	 * number of expressions and it's not executed often.
	 *
	 * 检查重复的表达式。用两层循环统计每个表达式的出现次数。这是 O(N^2)，但只允许少量表达式，而且不常执行。
	 *
	 * XXX We don't cross-check attributes and expressions, because it does
	 * not seem worth it. In principle we could check that expressions don't
	 * contain trivial attribute references like "(a)", but the reasoning is
	 * similar to why we don't bother with extracting columns from
	 * expressions. It's either expensive or very easy to defeat for
	 * determined user, and there's no risk if we allow such statistics (the
	 * statistics is useless, but harmless).
	 *
	 * 不交叉检查属性与表达式，因为似乎不值得。原则上可以检查表达式是否包含 (a) 这种平凡属性引用，但理由与不去从表达式中提取列类似：要么代价高，要么很容易被下定决心的用户绕过。允许这种统计没有风险（统计没有用，但无害）。
	 */
	foreach(cell, stxexprs)
	{
		Node	   *expr1 = (Node *) lfirst(cell);
		int			cnt = 0;

		foreach(cell2, stxexprs)
		{
			Node	   *expr2 = (Node *) lfirst(cell2);

			if (equal(expr1, expr2))
				cnt += 1;
		}

		/* every expression should find at least itself */
		/*
		 *
		 * 每个表达式至少应找到它自己。
		 */
		Assert(cnt >= 1);

		if (cnt > 1)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_COLUMN),
					 errmsg("duplicate expression in statistics definition")));
	}

	/* Form an int2vector representation of the sorted column list */
	/*
	 *
	 * 把排好序的列列表做成 int2vector。
	 */
	stxkeys = buildint2vector(attnums, nattnums);

	/* construct the char array of enabled statistic types */
	/*
	 *
	 * 构造已启用统计类型的字符数组。
	 */
	ntypes = 0;
	if (build_ndistinct)
		types[ntypes++] = CharGetDatum(STATS_EXT_NDISTINCT);
	if (build_dependencies)
		types[ntypes++] = CharGetDatum(STATS_EXT_DEPENDENCIES);
	if (build_mcv)
		types[ntypes++] = CharGetDatum(STATS_EXT_MCV);
	if (build_expressions)
		types[ntypes++] = CharGetDatum(STATS_EXT_EXPRESSIONS);
	Assert(ntypes > 0 && ntypes <= lengthof(types));
	stxkind = construct_array_builtin(types, ntypes, CHAROID);

	/* convert the expressions (if any) to a text datum */
	/*
	 *
	 * 把表达式（若有）转换成 text 数据。
	 */
	if (stxexprs != NIL)
	{
		char	   *exprsString;

		exprsString = nodeToString(stxexprs);
		exprsDatum = CStringGetTextDatum(exprsString);
		pfree(exprsString);
	}
	else
		exprsDatum = (Datum) 0;

	statrel = table_open(StatisticExtRelationId, RowExclusiveLock);

	/*
	 * Everything seems fine, so let's build the pg_statistic_ext tuple.
	 *
	 * 看起来都没问题，开始构造 pg_statistic_ext 元组。
	 */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	statoid = GetNewOidWithIndex(statrel, StatisticExtOidIndexId,
								 Anum_pg_statistic_ext_oid);
	values[Anum_pg_statistic_ext_oid - 1] = ObjectIdGetDatum(statoid);
	values[Anum_pg_statistic_ext_stxrelid - 1] = ObjectIdGetDatum(relid);
	values[Anum_pg_statistic_ext_stxname - 1] = NameGetDatum(&stxname);
	values[Anum_pg_statistic_ext_stxnamespace - 1] = ObjectIdGetDatum(namespaceId);
	values[Anum_pg_statistic_ext_stxowner - 1] = ObjectIdGetDatum(stxowner);
	values[Anum_pg_statistic_ext_stxkeys - 1] = PointerGetDatum(stxkeys);
	nulls[Anum_pg_statistic_ext_stxstattarget - 1] = true;
	values[Anum_pg_statistic_ext_stxkind - 1] = PointerGetDatum(stxkind);

	values[Anum_pg_statistic_ext_stxexprs - 1] = exprsDatum;
	if (exprsDatum == (Datum) 0)
		nulls[Anum_pg_statistic_ext_stxexprs - 1] = true;

	/* insert it into pg_statistic_ext */
	/*
	 *
	 * 把它插入 pg_statistic_ext。
	 */
	htup = heap_form_tuple(statrel->rd_att, values, nulls);
	CatalogTupleInsert(statrel, htup);
	heap_freetuple(htup);

	relation_close(statrel, RowExclusiveLock);

	/*
	 * We used to create the pg_statistic_ext_data tuple too, but it's not
	 * clear what value should the stxdinherit flag have (it depends on
	 * whether the rel is partitioned, contains data, etc.)
	 *
	 * 以前还会创建 pg_statistic_ext_data 元组，但不清楚 stxdinherit 标志应该取什么值（取决于关系是否分区、是否含有数据等）。
	 */

	InvokeObjectPostCreateHook(StatisticExtRelationId, statoid, 0);

	/*
	 * Invalidate relcache so that others see the new statistics object.
	 *
	 * 使 relcache 失效，以便其它人看到新的统计对象。
	 */
	CacheInvalidateRelcache(rel);

	relation_close(rel, NoLock);

	/*
	 * Add an AUTO dependency on each column used in the stats, so that the
	 * stats object goes away if any or all of them get dropped.
	 *
	 * 为统计中用到的每一列加上 AUTO 依赖，这样其中任意列或全部列被删除时，统计对象也会消失。
	 */
	ObjectAddressSet(myself, StatisticExtRelationId, statoid);

	/* add dependencies for plain column references */
	/*
	 *
	 * 为普通列引用添加依赖。
	 */
	for (i = 0; i < nattnums; i++)
	{
		ObjectAddressSubSet(parentobject, RelationRelationId, relid, attnums[i]);
		recordDependencyOn(&myself, &parentobject, DEPENDENCY_AUTO);
	}

	/*
	 * If there are no dependencies on a column, give the statistics object an
	 * auto dependency on the whole table.  In most cases, this will be
	 * redundant, but it might not be if the statistics expressions contain no
	 * Vars (which might seem strange but possible). This is consistent with
	 * what we do for indexes in index_create.
	 *
	 * 若对任何列都没有依赖，则让统计对象对整张表有一条自动依赖。多数情况下这是多余的，但若统计表达式不含 Var（这看起来奇怪但可能发生），就不是多余的。这与 index_create 中对索引的做法一致。
	 *
	 * XXX We intentionally don't consider the expressions before adding this
	 * dependency, because recordDependencyOnSingleRelExpr may not create any
	 * dependencies for whole-row Vars.
	 *
	 * 在添加这条依赖之前故意不考虑表达式，因为 recordDependencyOnSingleRelExpr 可能不会为整行 Var 创建任何依赖。
	 */
	if (!nattnums)
	{
		ObjectAddressSet(parentobject, RelationRelationId, relid);
		recordDependencyOn(&myself, &parentobject, DEPENDENCY_AUTO);
	}

	/*
	 * Store dependencies on anything mentioned in statistics expressions,
	 * just like we do for index expressions.
	 *
	 * 为统计表达式中提到的任何东西记录依赖，就像索引表达式那样。
	 */
	if (stxexprs)
		recordDependencyOnSingleRelExpr(&myself,
										(Node *) stxexprs,
										relid,
										DEPENDENCY_NORMAL,
										DEPENDENCY_AUTO, false);

	/*
	 * Also add dependencies on namespace and owner.  These are required
	 * because the stats object might have a different namespace and/or owner
	 * than the underlying table(s).
	 *
	 * 同时加上对命名空间和所有者的依赖。这是必需的，因为统计对象的命名空间和所有者可能与底层表不同。
	 */
	ObjectAddressSet(parentobject, NamespaceRelationId, namespaceId);
	recordDependencyOn(&myself, &parentobject, DEPENDENCY_NORMAL);

	recordDependencyOnOwner(StatisticExtRelationId, statoid, stxowner);

	/*
	 * XXX probably there should be a recordDependencyOnCurrentExtension call
	 * here too, but we'd have to add support for ALTER EXTENSION ADD/DROP
	 * STATISTICS, which is more work than it seems worth.
	 *
	 * 这里或许也应该调用 recordDependencyOnCurrentExtension，但那就得支持 ALTER EXTENSION ADD/DROP STATISTICS，工作量看起来不值得。
	 */

	/* Add any requested comment */
	/*
	 *
	 * 添加请求的注释。
	 */
	if (stmt->stxcomment != NULL)
		CreateComments(statoid, StatisticExtRelationId, 0,
					   stmt->stxcomment);

	/* Return stats object's address */
	/*
	 *
	 * 返回统计对象的地址。
	 */
	return myself;
}

/*
 *		ALTER STATISTICS
 *
 * 执行 ALTER STATISTICS。
 */
ObjectAddress
AlterStatistics(AlterStatsStmt *stmt)
{
	Relation	rel;
	Oid			stxoid;
	HeapTuple	oldtup;
	HeapTuple	newtup;
	Datum		repl_val[Natts_pg_statistic_ext];
	bool		repl_null[Natts_pg_statistic_ext];
	bool		repl_repl[Natts_pg_statistic_ext];
	ObjectAddress address;
	int			newtarget = 0;
	bool		newtarget_default;

	/* -1 was used in previous versions for the default setting */
	/*
	 *
	 * 以前的版本用 -1 表示默认设置。
	 */
	if (stmt->stxstattarget && intVal(stmt->stxstattarget) != -1)
	{
		newtarget = intVal(stmt->stxstattarget);
		newtarget_default = false;
	}
	else
		newtarget_default = true;

	if (!newtarget_default)
	{
		/* Limit statistics target to a sane range */
		/*
		 *
		 * 把统计目标限制在合理范围内。
		 */
		if (newtarget < 0)
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("statistics target %d is too low",
							newtarget)));
		}
		else if (newtarget > MAX_STATISTICS_TARGET)
		{
			newtarget = MAX_STATISTICS_TARGET;
			ereport(WARNING,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("lowering statistics target to %d",
							newtarget)));
		}
	}

	/* lookup OID of the statistics object */
	/*
	 *
	 * 查找统计对象的 OID。
	 */
	stxoid = get_statistics_object_oid(stmt->defnames, stmt->missing_ok);

	/*
	 * If we got here and the OID is not valid, it means the statistics object
	 * does not exist, but the command specified IF EXISTS. So report this as
	 * a simple NOTICE and we're done.
	 *
	 * 若走到这里而 OID 无效，说明统计对象不存在，但命令指定了 IF EXISTS。因此发一条 NOTICE 后结束。
	 */
	if (!OidIsValid(stxoid))
	{
		char	   *schemaname;
		char	   *statname;

		Assert(stmt->missing_ok);

		DeconstructQualifiedName(stmt->defnames, &schemaname, &statname);

		if (schemaname)
			ereport(NOTICE,
					(errmsg("statistics object \"%s.%s\" does not exist, skipping",
							schemaname, statname)));
		else
			ereport(NOTICE,
					(errmsg("statistics object \"%s\" does not exist, skipping",
							statname)));

		return InvalidObjectAddress;
	}

	/* Search pg_statistic_ext */
	/*
	 *
	 * 搜索 pg_statistic_ext。
	 */
	rel = table_open(StatisticExtRelationId, RowExclusiveLock);

	oldtup = SearchSysCache1(STATEXTOID, ObjectIdGetDatum(stxoid));
	if (!HeapTupleIsValid(oldtup))
		elog(ERROR, "cache lookup failed for extended statistics object %u", stxoid);

	/* Must be owner of the existing statistics object */
	/*
	 *
	 * 必须是现有统计对象的所有者。
	 */
	if (!object_ownercheck(StatisticExtRelationId, stxoid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_STATISTIC_EXT,
					   NameListToString(stmt->defnames));

	/* Build new tuple. */
	/*
	 *
	 * 构造新元组。
	 */
	memset(repl_val, 0, sizeof(repl_val));
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));

	/* replace the stxstattarget column */
	/*
	 *
	 * 替换 stxstattarget 列。
	 */
	repl_repl[Anum_pg_statistic_ext_stxstattarget - 1] = true;
	if (!newtarget_default)
		repl_val[Anum_pg_statistic_ext_stxstattarget - 1] = Int16GetDatum(newtarget);
	else
		repl_null[Anum_pg_statistic_ext_stxstattarget - 1] = true;

	newtup = heap_modify_tuple(oldtup, RelationGetDescr(rel),
							   repl_val, repl_null, repl_repl);

	/* Update system catalog. */
	/*
	 *
	 * 更新系统目录。
	 */
	CatalogTupleUpdate(rel, &newtup->t_self, newtup);

	InvokeObjectPostAlterHook(StatisticExtRelationId, stxoid, 0);

	ObjectAddressSet(address, StatisticExtRelationId, stxoid);

	/*
	 * NOTE: because we only support altering the statistics target, not the
	 * other fields, there is no need to update dependencies.
	 *
	 * 因为目前只支持修改统计目标，不支持修改其它字段，所以不需要更新依赖。
	 */

	heap_freetuple(newtup);
	ReleaseSysCache(oldtup);

	table_close(rel, RowExclusiveLock);

	return address;
}

/*
 * Delete entry in pg_statistic_ext_data catalog. We don't know if the row
 * exists, so don't error out.
 *
 * 删除 pg_statistic_ext_data 目录中的项。不知道该行是否存在，因此不要报错。
 */
void
RemoveStatisticsDataById(Oid statsOid, bool inh)
{
	Relation	relation;
	HeapTuple	tup;

	relation = table_open(StatisticExtDataRelationId, RowExclusiveLock);

	tup = SearchSysCache2(STATEXTDATASTXOID, ObjectIdGetDatum(statsOid),
						  BoolGetDatum(inh));

	/* We don't know if the data row for inh value exists. */
	/*
	 *
	 * 不知道该 inh 值对应的数据行是否存在。
	 */
	if (HeapTupleIsValid(tup))
	{
		CatalogTupleDelete(relation, &tup->t_self);

		ReleaseSysCache(tup);
	}

	table_close(relation, RowExclusiveLock);
}

/*
 * Guts of statistics object deletion.
 *
 * 删除统计对象的主体逻辑。
 */
void
RemoveStatisticsById(Oid statsOid)
{
	Relation	relation;
	Relation	rel;
	HeapTuple	tup;
	Form_pg_statistic_ext statext;
	Oid			relid;

	/*
	 * Delete the pg_statistic_ext tuple.  Also send out a cache inval on the
	 * associated table, so that dependent plans will be rebuilt.
	 *
	 * 删除 pg_statistic_ext 元组。同时对相关表发出缓存失效，以便依赖它的计划被重建。
	 */
	relation = table_open(StatisticExtRelationId, RowExclusiveLock);

	tup = SearchSysCache1(STATEXTOID, ObjectIdGetDatum(statsOid));

	if (!HeapTupleIsValid(tup)) /* should not happen */
	/*
	 *
	 * 不应发生。
	 */
		elog(ERROR, "cache lookup failed for statistics object %u", statsOid);

	statext = (Form_pg_statistic_ext) GETSTRUCT(tup);
	relid = statext->stxrelid;

	/*
	 * Delete the pg_statistic_ext_data tuples holding the actual statistical
	 * data. There might be data with/without inheritance, so attempt deleting
	 * both. We lock the user table first, to prevent other processes (e.g.
	 * DROP STATISTICS) from removing the row concurrently.
	 *
	 * 删除存放实际统计数据的 pg_statistic_ext_data 元组。数据可能有继承或无继承两种，因此两种都尝试删除。先锁住用户表，防止其它进程（例如 DROP STATISTICS）并发删掉该行。
	 */
	rel = table_open(relid, ShareUpdateExclusiveLock);

	RemoveStatisticsDataById(statsOid, true);
	RemoveStatisticsDataById(statsOid, false);

	CacheInvalidateRelcacheByRelid(relid);

	CatalogTupleDelete(relation, &tup->t_self);

	ReleaseSysCache(tup);

	/* Keep lock until the end of the transaction. */
	/*
	 *
	 * 把锁保持到事务结束。
	 */
	table_close(rel, NoLock);

	table_close(relation, RowExclusiveLock);
}

/*
 * Select a nonconflicting name for a new statistics object.
 *
 * 为新的统计对象选择一个不冲突的名字。
 *
 * name1, name2, and label are used the same way as for makeObjectName(),
 * except that the label can't be NULL; digits will be appended to the label
 * if needed to create a name that is unique within the specified namespace.
 *
 * name1、name2 和 label 的用法与 makeObjectName() 相同，只是 label 不能为 NULL；必要时会在 label 后追加数字，以在指定命名空间内生成唯一名字。
 *
 * Returns a palloc'd string.
 *
 * 返回一段 palloc 分配的字符串。
 *
 * Note: it is theoretically possible to get a collision anyway, if someone
 * else chooses the same name concurrently.  This is fairly unlikely to be
 * a problem in practice, especially if one is holding a share update
 * exclusive lock on the relation identified by name1.  However, if choosing
 * multiple names within a single command, you'd better create the new object
 * and do CommandCounterIncrement before choosing the next one!
 *
 * 理论上别人仍可能同时选中同一个名字而发生冲突。实践中这相当少见，尤其是已经对 name1 标识的关系持有 ShareUpdateExclusiveLock 时。不过若在一条命令里选择多个名字，最好先创建新对象并做 CommandCounterIncrement，再选择下一个名字。
 */
static char *
ChooseExtendedStatisticName(const char *name1, const char *name2,
							const char *label, Oid namespaceid)
{
	int			pass = 0;
	char	   *stxname = NULL;
	char		modlabel[NAMEDATALEN];

	/* try the unmodified label first */
	/*
	 *
	 * 先尝试未修改的 label。
	 */
	strlcpy(modlabel, label, sizeof(modlabel));

	for (;;)
	{
		Oid			existingstats;

		stxname = makeObjectName(name1, name2, modlabel);

		existingstats = GetSysCacheOid2(STATEXTNAMENSP, Anum_pg_statistic_ext_oid,
										PointerGetDatum(stxname),
										ObjectIdGetDatum(namespaceid));
		if (!OidIsValid(existingstats))
			break;

		/* found a conflict, so try a new name component */
		/*
		 *
		 * 发现冲突，因此尝试一个新的名字成分。
		 */
		pfree(stxname);
		snprintf(modlabel, sizeof(modlabel), "%s%d", label, ++pass);
	}

	return stxname;
}

/*
 * Generate "name2" for a new statistics object given the list of column
 * names for it.  This will be passed to ChooseExtendedStatisticName along
 * with the parent table name and a suitable label.
 *
 * 根据列名列表为新的统计对象生成 name2。它会与父表名和合适的 label 一起传给 ChooseExtendedStatisticName。
 *
 * We know that less than NAMEDATALEN characters will actually be used,
 * so we can truncate the result once we've generated that many.
 *
 * 已知实际使用的字符少于 NAMEDATALEN，因此生成到这个长度后就可以截断结果。
 *
 * XXX see also ChooseForeignKeyConstraintNameAddition and
 * ChooseIndexNameAddition.
 *
 * 另见 ChooseForeignKeyConstraintNameAddition 和 ChooseIndexNameAddition。
 */
static char *
ChooseExtendedStatisticNameAddition(List *exprs)
{
	char		buf[NAMEDATALEN * 2];
	int			buflen = 0;
	ListCell   *lc;

	buf[0] = '\0';
	foreach(lc, exprs)
	{
		StatsElem  *selem = (StatsElem *) lfirst(lc);
		const char *name;

		/* It should be one of these, but just skip if it happens not to be */
		/*
		 *
		 * 应当是其中之一；若碰巧不是，就跳过。
		 */
		if (!IsA(selem, StatsElem))
			continue;

		name = selem->name;

		if (buflen > 0)
			buf[buflen++] = '_';	/* insert _ between names */
			/*
			 *
			 * 在名字之间插入下划线。
			 */

		/*
		 * We use fixed 'expr' for expressions, which have empty column names.
		 * For indexes this is handled in ChooseIndexColumnNames, but we have
		 * no such function for stats and it does not seem worth adding. If a
		 * better name is needed, the user can specify it explicitly.
		 *
		 * 表达式的列名为空，因此固定使用 expr。索引在 ChooseIndexColumnNames 中处理这一点，统计没有对应函数，似乎也不值得加。若需要更好的名字，用户可以显式指定。
		 */
		if (!name)
			name = "expr";

		/*
		 * At this point we have buflen <= NAMEDATALEN.  name should be less
		 * than NAMEDATALEN already, but use strlcpy for paranoia.
		 *
		 * 此时 buflen 小于等于 NAMEDATALEN。name 应当已经短于 NAMEDATALEN，但为稳妥仍使用 strlcpy。
		 */
		strlcpy(buf + buflen, name, NAMEDATALEN);
		buflen += strlen(buf + buflen);
		if (buflen >= NAMEDATALEN)
			break;
	}
	return pstrdup(buf);
}

/*
 * StatisticsGetRelation: given a statistics object's OID, get the OID of
 * the relation it is defined on.  Uses the system cache.
 *
 * StatisticsGetRelation：给定统计对象的 OID，取得它所定义于的关系的 OID。使用系统缓存。
 */
Oid
StatisticsGetRelation(Oid statId, bool missing_ok)
{
	HeapTuple	tuple;
	Form_pg_statistic_ext stx;
	Oid			result;

	tuple = SearchSysCache1(STATEXTOID, ObjectIdGetDatum(statId));
	if (!HeapTupleIsValid(tuple))
	{
		if (missing_ok)
			return InvalidOid;
		elog(ERROR, "cache lookup failed for statistics object %u", statId);
	}
	stx = (Form_pg_statistic_ext) GETSTRUCT(tuple);
	Assert(stx->oid == statId);

	result = stx->stxrelid;
	ReleaseSysCache(tuple);
	return result;
}

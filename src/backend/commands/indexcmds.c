/*-------------------------------------------------------------------------
 *
 * indexcmds.c
 *	  POSTGRES define and remove index code.
 *
 *	  本文件实现索引的定义与删除。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/indexcmds.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/amapi.h"
#include "access/gist.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/sysattr.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_am.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_database.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_type.h"
#include "commands/comment.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/event_trigger.h"
#include "commands/progress.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_coerce.h"
#include "parser/parse_oper.h"
#include "parser/parse_utilcmd.h"
#include "partitioning/partdesc.h"
#include "pgstat.h"
#include "rewrite/rewriteManip.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/injection_point.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/partcache.h"
#include "utils/pg_rusage.h"
#include "utils/regproc.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"


/* non-export function prototypes */
/*
 *
 * 非导出函数的原型。
 */
static bool CompareOpclassOptions(const Datum *opts1, const Datum *opts2, int natts);
static void CheckPredicate(Expr *predicate);
static void ComputeIndexAttrs(IndexInfo *indexInfo,
							  Oid *typeOids,
							  Oid *collationOids,
							  Oid *opclassOids,
							  Datum *opclassOptions,
							  int16 *colOptions,
							  const List *attList,
							  const List *exclusionOpNames,
							  Oid relId,
							  const char *accessMethodName,
							  Oid accessMethodId,
							  bool amcanorder,
							  bool isconstraint,
							  bool iswithoutoverlaps,
							  Oid ddl_userid,
							  int ddl_sec_context,
							  int *ddl_save_nestlevel);
static char *ChooseIndexName(const char *tabname, Oid namespaceId,
							 const List *colnames, const List *exclusionOpNames,
							 bool primary, bool isconstraint);
static char *ChooseIndexNameAddition(const List *colnames);
static List *ChooseIndexColumnNames(const List *indexElems);
static void ReindexIndex(const ReindexStmt *stmt, const ReindexParams *params,
						 bool isTopLevel);
static void RangeVarCallbackForReindexIndex(const RangeVar *relation,
											Oid relId, Oid oldRelId, void *arg);
static Oid	ReindexTable(const ReindexStmt *stmt, const ReindexParams *params,
						 bool isTopLevel);
static void ReindexMultipleTables(const ReindexStmt *stmt,
								  const ReindexParams *params);
static void reindex_error_callback(void *arg);
static void ReindexPartitions(const ReindexStmt *stmt, Oid relid,
							  const ReindexParams *params, bool isTopLevel);
static void ReindexMultipleInternal(const ReindexStmt *stmt, const List *relids,
									const ReindexParams *params);
static bool ReindexRelationConcurrently(const ReindexStmt *stmt,
										Oid relationOid,
										const ReindexParams *params);
static void update_relispartition(Oid relationId, bool newval);
static inline void set_indexsafe_procflags(void);

/*
 * 核心流程：
 * DefineIndex() 是 CREATE INDEX 主路径：校验关系种类、锁与权限，解析访问方法、
 * 列、谓词与表空间，写入系统目录并构建索引。CONCURRENTLY 分阶段构建；
 * 分区表只建立父索引目录项，再对每个分区递归。
 * ExecReindex() 是 REINDEX 入口，按对象类型分发到 ReindexIndex()、
 * ReindexTable()、ReindexPartitions() 或 ReindexMultipleTables()。
 * ReindexRelationConcurrently() 完成并发重建的六个阶段：建立新索引目录、
 * 构建、追平、交换名称、把旧索引标为失效、删除旧索引。
 * CheckIndexCompatible() 供 ALTER TABLE ALTER TYPE 判断旧索引存储能否复用。
 */

/*
 * callback argument type for RangeVarCallbackForReindexIndex()
 *
 * RangeVarCallbackForReindexIndex() 的回调参数类型。
 */
struct ReindexIndexCallbackState
{
	ReindexParams params;		/* options from statement */
	/*
	 *
	 * 来自语句的选项。
	 */
	Oid			locked_table_oid;	/* tracks previously locked table */
	/*
	 *
	 * 记录先前已锁定的表。
	 */
};

/*
 * callback arguments for reindex_error_callback()
 *
 * reindex_error_callback() 的回调参数。
 */
typedef struct ReindexErrorInfo
{
	char	   *relname;
	char	   *relnamespace;
	char		relkind;
} ReindexErrorInfo;

/*
 * CheckIndexCompatible
 *		Determine whether an existing index definition is compatible with a
 *		prospective index definition, such that the existing index storage
 *		could become the storage of the new index, avoiding a rebuild.
 *
 *		判断已有索引定义是否与拟建索引兼容，从而让现有索引存储成为新索引的存储，
 *		避免重建。
 *
 * 'oldId': the OID of the existing index
 * 'accessMethodName': name of the AM to use.
 * 'attributeList': a list of IndexElem specifying columns and expressions
 *		to index on.
 * 'exclusionOpNames': list of names of exclusion-constraint operators,
 *		or NIL if not an exclusion constraint.
 * 'isWithoutOverlaps': true iff this index has a WITHOUT OVERLAPS clause.
 *
 * 'oldId'：已有索引的 OID。
 * 'accessMethodName'：要使用的访问方法名。
 * 'attributeList'：IndexElem 列表，指定要索引的列与表达式。
 * 'exclusionOpNames'：排他约束操作符的名字列表；若不是排他约束则为 NIL。
 * 'isWithoutOverlaps'：当且仅当该索引带有 WITHOUT OVERLAPS 子句时为真。
 *
 * This is tailored to the needs of ALTER TABLE ALTER TYPE, which recreates
 * any indexes that depended on a changing column from their pg_get_indexdef
 * or pg_get_constraintdef definitions.  We omit some of the sanity checks of
 * DefineIndex.  We assume that the old and new indexes have the same number
 * of columns and that if one has an expression column or predicate, both do.
 * Errors arising from the attribute list still apply.
 *
 * 本函数专为 ALTER TABLE ALTER TYPE 服务：该命令会按 pg_get_indexdef 或
 * pg_get_constraintdef
 * 重建依赖于被改列的索引。这里省略了 DefineIndex 的部分健全性检查。假定新旧索
 * 引列数相同，
 * 并且若一方有表达式列或谓词，另一方也有。属性列表引起的错误仍然适用。
 *
 * Most column type changes that can skip a table rewrite do not invalidate
 * indexes.  We acknowledge this when all operator classes, collations and
 * exclusion operators match.  Though we could further permit intra-opfamily
 * changes for btree and hash indexes, that adds subtle complexity with no
 * concrete benefit for core types. Note, that INCLUDE columns aren't
 * checked by this function, for them it's enough that table rewrite is
 * skipped.
 *
 * 多数可以跳过表重写的列类型变更并不会使索引失效。当操作符类、排序规则和排他
 * 操作符全部一致时，
 * 即认为索引仍然有效。虽然还可以进一步允许 btree 与 hash 索引在同一操作符族内
 * 变更，
 * 但那会增加微妙的复杂度，对核心类型又没有实际好处。注意本函数不检查 INCLUDE
 * 列，
 * 对它们而言，只要跳过了表重写就足够。
 *
 * When a comparison or exclusion operator has a polymorphic input type, the
 * actual input types must also match.  This defends against the possibility
 * that operators could vary behavior in response to get_fn_expr_argtype().
 * At present, this hazard is theoretical: check_exclusion_constraint() and
 * all core index access methods decline to set fn_expr for such calls.
 *
 * 比较或排他操作符的输入类型为多态时，实际输入类型也必须一致。这是为了防止操
 * 作符随
 * get_fn_expr_argtype() 改变行为。目前这种风险只是理论上的：
 * check_exclusion_constraint()
 * 以及所有核心索引访问方法都不会为这类调用设置 fn_expr。
 *
 * We do not yet implement a test to verify compatibility of expression
 * columns or predicates, so assume any such index is incompatible.
 *
 * 尚未实现对表达式列或谓词的兼容性检查，因此凡是带有它们的索引都视为不兼容。
 */
bool
CheckIndexCompatible(Oid oldId,
					 const char *accessMethodName,
					 const List *attributeList,
					 const List *exclusionOpNames,
					 bool isWithoutOverlaps)
{
	bool		isconstraint;
	Oid		   *typeIds;
	Oid		   *collationIds;
	Oid		   *opclassIds;
	Datum	   *opclassOptions;
	Oid			accessMethodId;
	Oid			relationId;
	HeapTuple	tuple;
	Form_pg_index indexForm;
	Form_pg_am	accessMethodForm;
	IndexAmRoutine *amRoutine;
	bool		amcanorder;
	bool		amsummarizing;
	int16	   *coloptions;
	IndexInfo  *indexInfo;
	int			numberOfAttributes;
	int			old_natts;
	bool		ret = true;
	oidvector  *old_indclass;
	oidvector  *old_indcollation;
	Relation	irel;
	int			i;
	Datum		d;

	/* Caller should already have the relation locked in some way. */
	/*
	 *
	 * 调用方应已以某种方式锁住该关系。
	 */
	relationId = IndexGetRelation(oldId, false);

	/*
	 * We can pretend isconstraint = false unconditionally.  It only serves to
	 * decide the text of an error message that should never happen for us.
	 *
	 * 可以无条件把 isconstraint 当作 false。它只用来决定一条本不该出现的错误
	 * 信息文本。
	 */
	isconstraint = false;

	numberOfAttributes = list_length(attributeList);
	Assert(numberOfAttributes > 0);
	Assert(numberOfAttributes <= INDEX_MAX_KEYS);

	/* look up the access method */
	/*
	 *
	 * 查找访问方法。
	 */
	tuple = SearchSysCache1(AMNAME, PointerGetDatum(accessMethodName));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("access method \"%s\" does not exist",
						accessMethodName)));
	accessMethodForm = (Form_pg_am) GETSTRUCT(tuple);
	accessMethodId = accessMethodForm->oid;
	amRoutine = GetIndexAmRoutine(accessMethodForm->amhandler);
	ReleaseSysCache(tuple);

	amcanorder = amRoutine->amcanorder;
	amsummarizing = amRoutine->amsummarizing;

	/*
	 * Compute the operator classes, collations, and exclusion operators for
	 * the new index, so we can test whether it's compatible with the existing
	 * one.  Note that ComputeIndexAttrs might fail here, but that's OK:
	 * DefineIndex would have failed later.  Our attributeList contains only
	 * key attributes, thus we're filling ii_NumIndexAttrs and
	 * ii_NumIndexKeyAttrs with same value.
	 *
	 * 计算新索引的操作符类、排序规则和排他操作符，以便与已有索引比较是否兼容。
	 * 此处 ComputeIndexAttrs 可能失败，但这没有关系：DefineIndex 稍后同样会失
	 * 败。
	 * attributeList 只含键列，因此 ii_NumIndexAttrs 与 ii_NumIndexKeyAttrs 填
	 * 入相同的值。
	 */
	indexInfo = makeIndexInfo(numberOfAttributes, numberOfAttributes,
							  accessMethodId, NIL, NIL, false, false,
							  false, false, amsummarizing, isWithoutOverlaps);
	typeIds = palloc_array(Oid, numberOfAttributes);
	collationIds = palloc_array(Oid, numberOfAttributes);
	opclassIds = palloc_array(Oid, numberOfAttributes);
	opclassOptions = palloc_array(Datum, numberOfAttributes);
	coloptions = palloc_array(int16, numberOfAttributes);
	ComputeIndexAttrs(indexInfo,
					  typeIds, collationIds, opclassIds, opclassOptions,
					  coloptions, attributeList,
					  exclusionOpNames, relationId,
					  accessMethodName, accessMethodId,
					  amcanorder, isconstraint, isWithoutOverlaps, InvalidOid,
					  0, NULL);

	/* Get the soon-obsolete pg_index tuple. */
	/*
	 *
	 * 读取即将过时的 pg_index 元组。
	 */
	tuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(oldId));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for index %u", oldId);
	indexForm = (Form_pg_index) GETSTRUCT(tuple);

	/*
	 * We don't assess expressions or predicates; assume incompatibility.
	 * Also, if the index is invalid for any reason, treat it as incompatible.
	 *
	 * 不评估表达式或谓词，一律视为不兼容。索引若因任何原因无效，也视为不兼容。
	 */
	if (!(heap_attisnull(tuple, Anum_pg_index_indpred, NULL) &&
		  heap_attisnull(tuple, Anum_pg_index_indexprs, NULL) &&
		  indexForm->indisvalid))
	{
		ReleaseSysCache(tuple);
		return false;
	}

	/* Any change in operator class or collation breaks compatibility. */
	/*
	 *
	 * 操作符类或排序规则有任何变化都会破坏兼容性。
	 */
	old_natts = indexForm->indnkeyatts;
	Assert(old_natts == numberOfAttributes);

	d = SysCacheGetAttrNotNull(INDEXRELID, tuple, Anum_pg_index_indcollation);
	old_indcollation = (oidvector *) DatumGetPointer(d);

	d = SysCacheGetAttrNotNull(INDEXRELID, tuple, Anum_pg_index_indclass);
	old_indclass = (oidvector *) DatumGetPointer(d);

	ret = (memcmp(old_indclass->values, opclassIds, old_natts * sizeof(Oid)) == 0 &&
		   memcmp(old_indcollation->values, collationIds, old_natts * sizeof(Oid)) == 0);

	ReleaseSysCache(tuple);

	if (!ret)
		return false;

	/* For polymorphic opcintype, column type changes break compatibility. */
	/*
	 *
	 * 对于多态 opcintype，列类型变化会破坏兼容性。
	 */
	irel = index_open(oldId, AccessShareLock);	/* caller probably has a lock */
	/*
	 *
	 * 调用方很可能已持有锁。
	 */
	for (i = 0; i < old_natts; i++)
	{
		if (IsPolymorphicType(get_opclass_input_type(opclassIds[i])) &&
			TupleDescAttr(irel->rd_att, i)->atttypid != typeIds[i])
		{
			ret = false;
			break;
		}
	}

	/* Any change in opclass options break compatibility. */
	/*
	 *
	 * 操作符类选项有任何变化都会破坏兼容性。
	 */
	if (ret)
	{
		Datum	   *oldOpclassOptions = palloc_array(Datum, old_natts);

		for (i = 0; i < old_natts; i++)
			oldOpclassOptions[i] = get_attoptions(oldId, i + 1);

		ret = CompareOpclassOptions(oldOpclassOptions, opclassOptions, old_natts);

		pfree(oldOpclassOptions);
	}

	/* Any change in exclusion operator selections breaks compatibility. */
	/*
	 *
	 * 排他操作符的选择有任何变化都会破坏兼容性。
	 */
	if (ret && indexInfo->ii_ExclusionOps != NULL)
	{
		Oid		   *old_operators,
				   *old_procs;
		uint16	   *old_strats;

		RelationGetExclusionInfo(irel, &old_operators, &old_procs, &old_strats);
		ret = memcmp(old_operators, indexInfo->ii_ExclusionOps,
					 old_natts * sizeof(Oid)) == 0;

		/* Require an exact input type match for polymorphic operators. */
		/*
		 *
		 * 多态操作符要求输入类型完全一致。
		 */
		if (ret)
		{
			for (i = 0; i < old_natts && ret; i++)
			{
				Oid			left,
							right;

				op_input_types(indexInfo->ii_ExclusionOps[i], &left, &right);
				if ((IsPolymorphicType(left) || IsPolymorphicType(right)) &&
					TupleDescAttr(irel->rd_att, i)->atttypid != typeIds[i])
				{
					ret = false;
					break;
				}
			}
		}
	}

	index_close(irel, NoLock);
	return ret;
}

/*
 * CompareOpclassOptions
 *
 * Compare per-column opclass options which are represented by arrays of text[]
 * datums.  Both elements of arrays and array themselves can be NULL.
 *
 * 比较逐列的操作符类选项。它们表示为 text[] 数组的 datum。数组元素以及数组本
 * 身都可以是 NULL。
 */
static bool
CompareOpclassOptions(const Datum *opts1, const Datum *opts2, int natts)
{
	int			i;
	FmgrInfo	fm;

	if (!opts1 && !opts2)
		return true;

	fmgr_info(F_ARRAY_EQ, &fm);
	for (i = 0; i < natts; i++)
	{
		Datum		opt1 = opts1 ? opts1[i] : (Datum) 0;
		Datum		opt2 = opts2 ? opts2[i] : (Datum) 0;

		if (opt1 == (Datum) 0)
		{
			if (opt2 == (Datum) 0)
				continue;
			else
				return false;
		}
		else if (opt2 == (Datum) 0)
			return false;

		/*
		 * Compare non-NULL text[] datums.  Use C collation to enforce binary
		 * equivalence of texts, because we don't know anything about the
		 * semantics of opclass options.
		 *
		 * 比较非 NULL 的 text[] datum。使用 C 排序规则强制文本的二进制等价，
		 * 因为我们不了解操作符类选项的语义。
		 */
		if (!DatumGetBool(FunctionCall2Coll(&fm, C_COLLATION_OID, opt1, opt2)))
			return false;
	}

	return true;
}

/*
 * WaitForOlderSnapshots
 *
 * Wait for transactions that might have an older snapshot than the given xmin
 * limit, because it might not contain tuples deleted just before it has
 * been taken. Obtain a list of VXIDs of such transactions, and wait for them
 * individually. This is used when building an index concurrently.
 *
 * 等待那些快照可能比给定 xmin 界限更旧的事务，因为该快照可能不包含就在其取得
 * 之前被删除的元组。
 * 收集这些事务的 VXID 并逐个等待。并发构建索引时使用。
 *
 * We can exclude any running transactions that have xmin > the xmin given;
 * their oldest snapshot must be newer than our xmin limit.
 * We can also exclude any transactions that have xmin = zero, since they
 * evidently have no live snapshot at all (and any one they might be in
 * process of taking is certainly newer than ours).  Transactions in other
 * DBs can be ignored too, since they'll never even be able to see the
 * index being worked on.
 *
 * 可以排除 xmin 大于给定 xmin 的运行中事务，它们最旧的快照必然新于我们的 xmin
 * 界限。
 * 也可以排除 xmin 为零的事务，它们显然没有任何活动快照（正在取得的快照也必然
 * 比我们的新）。
 * 其他数据库中的事务也可以忽略，它们根本看不到正在处理的索引。
 *
 * We can also exclude autovacuum processes and processes running manual
 * lazy VACUUMs, because they won't be fazed by missing index entries
 * either.  (Manual ANALYZEs, however, can't be excluded because they
 * might be within transactions that are going to do arbitrary operations
 * later.)  Processes running CREATE INDEX CONCURRENTLY or REINDEX CONCURRENTLY
 * on indexes that are neither expressional nor partial are also safe to
 * ignore, since we know that those processes won't examine any data
 * outside the table they're indexing.
 *
 * 还可以排除 autovacuum 进程以及正在执行手动 lazy VACUUM 的进程，缺少索引项不
 * 会影响它们。
 * （手动 ANALYZE 不能排除，因为它们可能处在稍后还会做任意操作的事务里。）
 * 对既非表达式索引也非部分索引执行 CREATE INDEX CONCURRENTLY 或 REINDEX
 * CONCURRENTLY 的进程
 * 也可以安全忽略，因为它们不会检查正在索引的那张表以外的数据。
 *
 * Also, GetCurrentVirtualXIDs never reports our own vxid, so we need not
 * check for that.
 *
 * 另外，GetCurrentVirtualXIDs 从不上报我们自己的 vxid，因此不必为此检查。
 *
 * If a process goes idle-in-transaction with xmin zero, we do not need to
 * wait for it anymore, per the above argument.  We do not have the
 * infrastructure right now to stop waiting if that happens, but we can at
 * least avoid the folly of waiting when it is idle at the time we would
 * begin to wait.  We do this by repeatedly rechecking the output of
 * GetCurrentVirtualXIDs.  If, during any iteration, a particular vxid
 * doesn't show up in the output, we know we can forget about it.
 *
 * 若某进程进入 idle-in-transaction 且 xmin 为零，按上面的理由就不必再等它。
 * 目前没有基础设施能在等待过程中发现这种情况并停止等待，但至少可以避免在我们
 * 即将开始等待时
 * 它已经空闲还去等它。做法是反复复查 GetCurrentVirtualXIDs 的输出。
 * 若某次迭代中某个 vxid 不再出现，就可以忘掉它。
 */
void
WaitForOlderSnapshots(TransactionId limitXmin, bool progress)
{
	int			n_old_snapshots;
	int			i;
	VirtualTransactionId *old_snapshots;

	old_snapshots = GetCurrentVirtualXIDs(limitXmin, true, false,
										  PROC_IS_AUTOVACUUM | PROC_IN_VACUUM
										  | PROC_IN_SAFE_IC,
										  &n_old_snapshots);
	if (progress)
		pgstat_progress_update_param(PROGRESS_WAITFOR_TOTAL, n_old_snapshots);

	for (i = 0; i < n_old_snapshots; i++)
	{
		if (!VirtualTransactionIdIsValid(old_snapshots[i]))
			continue;			/* found uninteresting in previous cycle */
			/*
			 *
			 * 上一轮已判定无需关注。
			 */

		if (i > 0)
		{
			/* see if anything's changed ... */
			/*
			 *
			 * 查看是否有变化……
			 */
			VirtualTransactionId *newer_snapshots;
			int			n_newer_snapshots;
			int			j;
			int			k;

			newer_snapshots = GetCurrentVirtualXIDs(limitXmin,
													true, false,
													PROC_IS_AUTOVACUUM | PROC_IN_VACUUM
													| PROC_IN_SAFE_IC,
													&n_newer_snapshots);
			for (j = i; j < n_old_snapshots; j++)
			{
				if (!VirtualTransactionIdIsValid(old_snapshots[j]))
					continue;	/* found uninteresting in previous cycle */
					/*
					 *
					 * 上一轮已判定无需关注。
					 */
				for (k = 0; k < n_newer_snapshots; k++)
				{
					if (VirtualTransactionIdEquals(old_snapshots[j],
												   newer_snapshots[k]))
						break;
				}
				if (k >= n_newer_snapshots) /* not there anymore */
				/*
				 *
				 * 已经不在了。
				 */
					SetInvalidVirtualTransactionId(old_snapshots[j]);
			}
			pfree(newer_snapshots);
		}

		if (VirtualTransactionIdIsValid(old_snapshots[i]))
		{
			/* If requested, publish who we're going to wait for. */
			/*
			 *
			 * 若有请求，公布将要等待的对象。
			 */
			if (progress)
			{
				PGPROC	   *holder = ProcNumberGetProc(old_snapshots[i].procNumber);

				if (holder)
					pgstat_progress_update_param(PROGRESS_WAITFOR_CURRENT_PID,
												 holder->pid);
			}
			VirtualXactLock(old_snapshots[i], true);
		}

		if (progress)
			pgstat_progress_update_param(PROGRESS_WAITFOR_DONE, i + 1);
	}
}


/*
 * DefineIndex
 *		Creates a new index.
 *
 *		创建一个新索引。
 *
 * This function manages the current userid according to the needs of pg_dump.
 * Recreating old-database catalog entries in new-database is fine, regardless
 * of which users would have permission to recreate those entries now.  That's
 * just preservation of state.  Running opaque expressions, like calling a
 * function named in a catalog entry or evaluating a pg_node_tree in a catalog
 * entry, as anyone other than the object owner, is not fine.  To adhere to
 * those principles and to remain fail-safe, use the table owner userid for
 * most ACL checks.  Use the original userid for ACL checks reached without
 * traversing opaque expressions.  (pg_dump can predict such ACL checks from
 * catalogs.)  Overall, this is a mess.  Future DDL development should
 * consider offering one DDL command for catalog setup and a separate DDL
 * command for steps that run opaque expressions.
 *
 * 本函数按 pg_dump 的需要管理当前 userid。在新库中重建旧库的目录项是可以的，
 * 无论现在哪些用户
 * 还有权重建这些对象，那只是保留状态。以对象所有者以外的身份执行不透明表达式
 * 则不可以，
 * 例如调用目录项中指名的函数，或求值目录项中的 pg_node_tree。
 * 为遵守这些原则并保持故障安全，大多数 ACL 检查使用表所有者的 userid。
 * 不经过不透明表达式就能到达的 ACL 检查则使用原来的 userid（pg_dump 可以根据
 * 目录预测这类检查）。
 * 总体上这很混乱。以后的 DDL 应当考虑把目录建立和执行不透明表达式分成两条命令。
 *
 * 'tableId': the OID of the table relation on which the index is to be
 *		created
 * 'stmt': IndexStmt describing the properties of the new index.
 * 'indexRelationId': normally InvalidOid, but during bootstrap can be
 *		nonzero to specify a preselected OID for the index.
 * 'parentIndexId': the OID of the parent index; InvalidOid if not the child
 *		of a partitioned index.
 * 'parentConstraintId': the OID of the parent constraint; InvalidOid if not
 *		the child of a constraint (only used when recursing)
 * 'total_parts': total number of direct and indirect partitions of relation;
 *		pass -1 if not known or rel is not partitioned.
 * 'is_alter_table': this is due to an ALTER rather than a CREATE operation.
 * 'check_rights': check for CREATE rights in namespace and tablespace.  (This
 *		should be true except when ALTER is deleting/recreating an index.)
 * 'check_not_in_use': check for table not already in use in current session.
 *		This should be true unless caller is holding the table open, in which
 *		case the caller had better have checked it earlier.
 * 'skip_build': make the catalog entries but don't create the index files
 * 'quiet': suppress the NOTICE chatter ordinarily provided for constraints.
 *
 * 'tableId'：要在其上创建索引的表关系 OID。
 * 'stmt'：描述新索引属性的 IndexStmt。
 * 'indexRelationId'：通常为 InvalidOid；引导阶段可以为非零，以指定预先选好的
 * 索引 OID。
 * 'parentIndexId'：父索引 OID；若不是分区索引的子索引则为 InvalidOid。
 * 'parentConstraintId'：父约束 OID；若不是约束的子项则为 InvalidOid（仅在递归
 * 时使用）。
 * 'total_parts'：关系的直接与间接分区总数；未知或关系未分区时传 -1。
 * 'is_alter_table'：本次来自 ALTER 而不是 CREATE。
 * 'check_rights'：检查命名空间和表空间上的 CREATE 权限。（除 ALTER 正在删除并
 * 重建索引外应为真。）
 * 'check_not_in_use'：检查表尚未被当前会话使用。除非调用方正打开着该表，否则
 * 应为真；
 * 那种情况下调用方应已提前检查过。
 * 'skip_build'：只写目录项，不创建索引文件。
 * 'quiet'：抑制通常为约束打印的 NOTICE。
 *
 * Returns the object address of the created index.
 *
 * 返回所创建索引的对象地址。
 */
ObjectAddress
DefineIndex(Oid tableId,
			IndexStmt *stmt,
			Oid indexRelationId,
			Oid parentIndexId,
			Oid parentConstraintId,
			int total_parts,
			bool is_alter_table,
			bool check_rights,
			bool check_not_in_use,
			bool skip_build,
			bool quiet)
{
	bool		concurrent;
	char	   *indexRelationName;
	char	   *accessMethodName;
	Oid		   *typeIds;
	Oid		   *collationIds;
	Oid		   *opclassIds;
	Datum	   *opclassOptions;
	Oid			accessMethodId;
	Oid			namespaceId;
	Oid			tablespaceId;
	Oid			createdConstraintId = InvalidOid;
	List	   *indexColNames;
	List	   *allIndexParams;
	Relation	rel;
	HeapTuple	tuple;
	Form_pg_am	accessMethodForm;
	IndexAmRoutine *amRoutine;
	bool		amcanorder;
	bool		amissummarizing;
	amoptions_function amoptions;
	bool		exclusion;
	bool		partitioned;
	bool		safe_index;
	Datum		reloptions;
	int16	   *coloptions;
	IndexInfo  *indexInfo;
	bits16		flags;
	bits16		constr_flags;
	int			numberOfAttributes;
	int			numberOfKeyAttributes;
	TransactionId limitXmin;
	ObjectAddress address;
	LockRelId	heaprelid;
	LOCKTAG		heaplocktag;
	LOCKMODE	lockmode;
	Snapshot	snapshot;
	Oid			root_save_userid;
	int			root_save_sec_context;
	int			root_save_nestlevel;

	root_save_nestlevel = NewGUCNestLevel();

	RestrictSearchPath();

	/*
	 * Some callers need us to run with an empty default_tablespace; this is a
	 * necessary hack to be able to reproduce catalog state accurately when
	 * recreating indexes after table-rewriting ALTER TABLE.
	 *
	 * 有些调用方要求在空的 default_tablespace 下运行。这是必要的权宜之计，以
	 * 便在表重写式
	 * ALTER TABLE 之后重建索引时准确复现目录状态。
	 */
	if (stmt->reset_default_tblspc)
		(void) set_config_option("default_tablespace", "",
								 PGC_USERSET, PGC_S_SESSION,
								 GUC_ACTION_SAVE, true, 0, false);

	/*
	 * Force non-concurrent build on temporary relations, even if CONCURRENTLY
	 * was requested.  Other backends can't access a temporary relation, so
	 * there's no harm in grabbing a stronger lock, and a non-concurrent DROP
	 * is more efficient.  Do this before any use of the concurrent option is
	 * done.
	 *
	 * 对临时关系强制非并发构建，即使请求了 CONCURRENTLY。其他后端无法访问临时
	 * 关系，
	 * 因此取更强的锁没有害处，而非并发的 DROP 更高效。必须在使用并发选项之前
	 * 完成这一处理。
	 */
	if (stmt->concurrent && get_rel_persistence(tableId) != RELPERSISTENCE_TEMP)
		concurrent = true;
	else
		concurrent = false;

	/*
	 * Start progress report.  If we're building a partition, this was already
	 * done.
	 *
	 * 开始进度报告。若正在构建分区，则此前已经做过。
	 */
	if (!OidIsValid(parentIndexId))
	{
		pgstat_progress_start_command(PROGRESS_COMMAND_CREATE_INDEX, tableId);
		pgstat_progress_update_param(PROGRESS_CREATEIDX_COMMAND,
									 concurrent ?
									 PROGRESS_CREATEIDX_COMMAND_CREATE_CONCURRENTLY :
									 PROGRESS_CREATEIDX_COMMAND_CREATE);
	}

	/*
	 * No index OID to report yet
	 *
	 * 尚无索引 OID 可报告。
	 */
	pgstat_progress_update_param(PROGRESS_CREATEIDX_INDEX_OID,
								 InvalidOid);

	/*
	 * count key attributes in index
	 *
	 * 统计索引中的键属性个数。
	 */
	numberOfKeyAttributes = list_length(stmt->indexParams);

	/*
	 * Calculate the new list of index columns including both key columns and
	 * INCLUDE columns.  Later we can determine which of these are key
	 * columns, and which are just part of the INCLUDE list by checking the
	 * list position.  A list item in a position less than ii_NumIndexKeyAttrs
	 * is part of the key columns, and anything equal to and over is part of
	 * the INCLUDE columns.
	 *
	 * 计算新的索引列清单，同时包含键列和 INCLUDE 列。之后可根据列表位置区分哪
	 * 些是键列、
	 * 哪些只属于 INCLUDE 列表。位置小于 ii_NumIndexKeyAttrs 的项是键列，等于
	 * 或大于该值的项是 INCLUDE 列。
	 */
	allIndexParams = list_concat_copy(stmt->indexParams,
									  stmt->indexIncludingParams);
	numberOfAttributes = list_length(allIndexParams);

	if (numberOfKeyAttributes <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("must specify at least one column")));
	if (numberOfAttributes > INDEX_MAX_KEYS)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("cannot use more than %d columns in an index",
						INDEX_MAX_KEYS)));

	/*
	 * Only SELECT ... FOR UPDATE/SHARE are allowed while doing a standard
	 * index build; but for concurrent builds we allow INSERT/UPDATE/DELETE
	 * (but not VACUUM).
	 *
	 * 普通索引构建期间只允许 SELECT ... FOR UPDATE/SHARE；并发构建则允许
	 * INSERT/UPDATE/DELETE
	 * （但不允许 VACUUM）。
	 *
	 * NB: Caller is responsible for making sure that tableId refers to the
	 * relation on which the index should be built; except in bootstrap mode,
	 * this will typically require the caller to have already locked the
	 * relation.  To avoid lock upgrade hazards, that lock should be at least
	 * as strong as the one we take here.
	 *
	 * 注意：调用方必须保证 tableId 指向应当建索引的关系；除引导模式外，这通常
	 * 要求调用方已经锁住该关系。
	 * 为避免锁升级风险，那把锁至少应与这里取得的锁一样强。
	 *
	 * NB: If the lock strength here ever changes, code that is run by
	 * parallel workers under the control of certain particular ambuild
	 * functions will need to be updated, too.
	 *
	 * 注意：若这里的锁强度将来改变，某些特定 ambuild 函数控制下由并行 worker
	 * 运行的代码也必须一并更新。
	 */
	lockmode = concurrent ? ShareUpdateExclusiveLock : ShareLock;
	rel = table_open(tableId, lockmode);

	/*
	 * Switch to the table owner's userid, so that any index functions are run
	 * as that user.  Also lock down security-restricted operations.  We
	 * already arranged to make GUC variable changes local to this command.
	 *
	 * 切换到表所有者的 userid，使索引函数以该用户身份运行，并收紧安全受限操作。
	 * 本命令的 GUC 变更已被安排为局部生效。
	 */
	GetUserIdAndSecContext(&root_save_userid, &root_save_sec_context);
	SetUserIdAndSecContext(rel->rd_rel->relowner,
						   root_save_sec_context | SECURITY_RESTRICTED_OPERATION);

	namespaceId = RelationGetNamespace(rel);

	/*
	 * It has exclusion constraint behavior if it's an EXCLUDE constraint or a
	 * temporal PRIMARY KEY/UNIQUE constraint
	 *
	 * 若它是 EXCLUDE 约束，或是带时间维度的 PRIMARY KEY/UNIQUE 约束，则具有排
	 * 他约束行为。
	 */
	exclusion = stmt->excludeOpNames || stmt->iswithoutoverlaps;

	/* Ensure that it makes sense to index this kind of relation */
	/*
	 *
	 * 确认对这种关系建立索引是合理的。
	 */
	switch (rel->rd_rel->relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_MATVIEW:
		case RELKIND_PARTITIONED_TABLE:
			/* OK */
			/*
			 *
			 * 允许建立索引。
			 */
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot create index on relation \"%s\"",
							RelationGetRelationName(rel)),
					 errdetail_relkind_not_supported(rel->rd_rel->relkind)));
			break;
	}

	/*
	 * Establish behavior for partitioned tables, and verify sanity of
	 * parameters.
	 *
	 * 确定分区表的行为，并校验参数是否合理。
	 *
	 * We do not build an actual index in this case; we only create a few
	 * catalog entries.  The actual indexes are built by recursing for each
	 * partition.
	 *
	 * 这种情况下不构建真正的索引，只创建少量目录项。真正的索引通过为每个分区
	 * 递归来构建。
	 */
	partitioned = rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE;
	if (partitioned)
	{
		/*
		 * Note: we check 'stmt->concurrent' rather than 'concurrent', so that
		 * the error is thrown also for temporary tables.  Seems better to be
		 * consistent, even though we could do it on temporary table because
		 * we're not actually doing it concurrently.
		 *
		 * 注意：这里检查的是 stmt->concurrent 而不是 concurrent，这样临时表也
		 * 会报错。
		 * 即使临时表实际上并没有并发执行，保持一致似乎更好。
		 */
		if (stmt->concurrent)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot create index on partitioned table \"%s\" concurrently",
							RelationGetRelationName(rel))));
	}

	/*
	 * Don't try to CREATE INDEX on temp tables of other backends.
	 *
	 * 不要对其他后端的临时表执行 CREATE INDEX。
	 */
	if (RELATION_IS_OTHER_TEMP(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot create indexes on temporary tables of other sessions")));

	/*
	 * Unless our caller vouches for having checked this already, insist that
	 * the table not be in use by our own session, either.  Otherwise we might
	 * fail to make entries in the new index (for instance, if an INSERT or
	 * UPDATE is in progress and has already made its list of target indexes).
	 *
	 * 除非调用方保证已经检查过，否则坚持要求本会话也没有正在使用该表。否则可
	 * 能无法往新索引里写入项
	 * （例如 INSERT 或 UPDATE 已在进行，并且已经生成了目标索引列表）。
	 */
	if (check_not_in_use)
		CheckTableNotInUse(rel, "CREATE INDEX");

	/*
	 * Verify we (still) have CREATE rights in the rel's namespace.
	 * (Presumably we did when the rel was created, but maybe not anymore.)
	 * Skip check if caller doesn't want it.  Also skip check if
	 * bootstrapping, since permissions machinery may not be working yet.
	 *
	 * 确认我们（仍然）在该关系的命名空间中拥有 CREATE 权限。（创建关系时想必
	 * 有过，但现在未必。）
	 * 调用方不要求时跳过检查。引导阶段也跳过，因为权限机制可能尚未工作。
	 */
	if (check_rights && !IsBootstrapProcessingMode())
	{
		AclResult	aclresult;

		aclresult = object_aclcheck(NamespaceRelationId, namespaceId, root_save_userid,
									ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_SCHEMA,
						   get_namespace_name(namespaceId));
	}

	/*
	 * Select tablespace to use.  If not specified, use default tablespace
	 * (which may in turn default to database's default).
	 *
	 * 选择要使用的表空间。未指定时使用默认表空间（它本身又可能默认为数据库的
	 * 默认表空间）。
	 */
	if (stmt->tableSpace)
	{
		tablespaceId = get_tablespace_oid(stmt->tableSpace, false);
		if (partitioned && tablespaceId == MyDatabaseTableSpace)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot specify default tablespace for partitioned relations")));
	}
	else
	{
		tablespaceId = GetDefaultTablespace(rel->rd_rel->relpersistence,
											partitioned);
		/* note InvalidOid is OK in this case */
		/*
		 *
		 * 注意此时 InvalidOid 是允许的。
		 */
	}

	/* Check tablespace permissions */
	/*
	 *
	 * 检查表空间权限。
	 */
	if (check_rights &&
		OidIsValid(tablespaceId) && tablespaceId != MyDatabaseTableSpace)
	{
		AclResult	aclresult;

		aclresult = object_aclcheck(TableSpaceRelationId, tablespaceId, root_save_userid,
									ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_TABLESPACE,
						   get_tablespace_name(tablespaceId));
	}

	/*
	 * Force shared indexes into the pg_global tablespace.  This is a bit of a
	 * hack but seems simpler than marking them in the BKI commands.  On the
	 * other hand, if it's not shared, don't allow it to be placed there.
	 *
	 * 强制把共享索引放进 pg_global 表空间。这有点取巧，但比在 BKI 命令里标记
	 * 它们更简单。
	 * 另一方面，非共享索引不允许放在那里。
	 */
	if (rel->rd_rel->relisshared)
		tablespaceId = GLOBALTABLESPACE_OID;
	else if (tablespaceId == GLOBALTABLESPACE_OID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("only shared relations can be placed in pg_global tablespace")));

	/*
	 * Choose the index column names.
	 *
	 * 选择索引列名。
	 */
	indexColNames = ChooseIndexColumnNames(allIndexParams);

	/*
	 * Select name for index if caller didn't specify
	 *
	 * 调用方未指定时为索引选择名字。
	 */
	indexRelationName = stmt->idxname;
	if (indexRelationName == NULL)
		indexRelationName = ChooseIndexName(RelationGetRelationName(rel),
											namespaceId,
											indexColNames,
											stmt->excludeOpNames,
											stmt->primary,
											stmt->isconstraint);

	/*
	 * look up the access method, verify it can handle the requested features
	 *
	 * 查找访问方法，并确认它能处理所请求的特性。
	 */
	accessMethodName = stmt->accessMethod;
	tuple = SearchSysCache1(AMNAME, PointerGetDatum(accessMethodName));
	if (!HeapTupleIsValid(tuple))
	{
		/*
		 * Hack to provide more-or-less-transparent updating of old RTREE
		 * indexes to GiST: if RTREE is requested and not found, use GIST.
		 *
		 * 把旧 RTREE 索引大致透明地升级为 GiST 的权宜做法：若请求 RTREE 但找
		 * 不到，则改用 GIST。
		 */
		if (strcmp(accessMethodName, "rtree") == 0)
		{
			ereport(NOTICE,
					(errmsg("substituting access method \"gist\" for obsolete method \"rtree\"")));
			accessMethodName = "gist";
			tuple = SearchSysCache1(AMNAME, PointerGetDatum(accessMethodName));
		}

		if (!HeapTupleIsValid(tuple))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("access method \"%s\" does not exist",
							accessMethodName)));
	}
	accessMethodForm = (Form_pg_am) GETSTRUCT(tuple);
	accessMethodId = accessMethodForm->oid;
	amRoutine = GetIndexAmRoutine(accessMethodForm->amhandler);

	pgstat_progress_update_param(PROGRESS_CREATEIDX_ACCESS_METHOD_OID,
								 accessMethodId);

	if (stmt->unique && !stmt->iswithoutoverlaps && !amRoutine->amcanunique)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("access method \"%s\" does not support unique indexes",
						accessMethodName)));
	if (stmt->indexIncludingParams != NIL && !amRoutine->amcaninclude)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("access method \"%s\" does not support included columns",
						accessMethodName)));
	if (numberOfKeyAttributes > 1 && !amRoutine->amcanmulticol)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("access method \"%s\" does not support multicolumn indexes",
						accessMethodName)));
	if (exclusion && amRoutine->amgettuple == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("access method \"%s\" does not support exclusion constraints",
						accessMethodName)));
	if (stmt->iswithoutoverlaps && strcmp(accessMethodName, "gist") != 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("access method \"%s\" does not support WITHOUT OVERLAPS constraints",
						accessMethodName)));

	amcanorder = amRoutine->amcanorder;
	amoptions = amRoutine->amoptions;
	amissummarizing = amRoutine->amsummarizing;

	pfree(amRoutine);
	ReleaseSysCache(tuple);

	/*
	 * Validate predicate, if given
	 *
	 * 若给出了谓词，则校验它。
	 */
	if (stmt->whereClause)
		CheckPredicate((Expr *) stmt->whereClause);

	/*
	 * Parse AM-specific options, convert to text array form, validate.
	 *
	 * 解析访问方法专用选项，转换成 text 数组形式并校验。
	 */
	reloptions = transformRelOptions((Datum) 0, stmt->options,
									 NULL, NULL, false, false);

	(void) index_reloptions(amoptions, reloptions, true);

	/*
	 * Prepare arguments for index_create, primarily an IndexInfo structure.
	 * Note that predicates must be in implicit-AND format.  In a concurrent
	 * build, mark it not-ready-for-inserts.
	 *
	 * 为 index_create 准备参数，主要是 IndexInfo 结构。注意谓词必须是隐式 AND
	 * 形式。
	 * 并发构建时把它标为尚未准备好接受插入。
	 */
	indexInfo = makeIndexInfo(numberOfAttributes,
							  numberOfKeyAttributes,
							  accessMethodId,
							  NIL,	/* expressions, NIL for now */
							  /*
							   *
							   * 表达式，目前为 NIL。
							   */
							  make_ands_implicit((Expr *) stmt->whereClause),
							  stmt->unique,
							  stmt->nulls_not_distinct,
							  !concurrent,
							  concurrent,
							  amissummarizing,
							  stmt->iswithoutoverlaps);

	typeIds = palloc_array(Oid, numberOfAttributes);
	collationIds = palloc_array(Oid, numberOfAttributes);
	opclassIds = palloc_array(Oid, numberOfAttributes);
	opclassOptions = palloc_array(Datum, numberOfAttributes);
	coloptions = palloc_array(int16, numberOfAttributes);
	ComputeIndexAttrs(indexInfo,
					  typeIds, collationIds, opclassIds, opclassOptions,
					  coloptions, allIndexParams,
					  stmt->excludeOpNames, tableId,
					  accessMethodName, accessMethodId,
					  amcanorder, stmt->isconstraint, stmt->iswithoutoverlaps,
					  root_save_userid, root_save_sec_context,
					  &root_save_nestlevel);

	/*
	 * Extra checks when creating a PRIMARY KEY index.
	 *
	 * 创建 PRIMARY KEY 索引时的额外检查。
	 */
	if (stmt->primary)
		index_check_primary_key(rel, indexInfo, is_alter_table, stmt);

	/*
	 * If this table is partitioned and we're creating a unique index, primary
	 * key, or exclusion constraint, make sure that the partition key is a
	 * subset of the index's columns.  Otherwise it would be possible to
	 * violate uniqueness by putting values that ought to be unique in
	 * different partitions.
	 *
	 * 若该表已分区，且正在创建唯一索引、主键或排他约束，则分区键必须是索引列
	 * 的子集。
	 * 否则可以把本应唯一的值放进不同分区，从而破坏唯一性。
	 *
	 * We could lift this limitation if we had global indexes, but those have
	 * their own problems, so this is a useful feature combination.
	 *
	 * 若有全局索引，这条限制或许可以放宽，但全局索引自身也有问题，因此当前这
	 * 种特性组合仍然有用。
	 */
	if (partitioned && (stmt->unique || exclusion))
	{
		PartitionKey key = RelationGetPartitionKey(rel);
		const char *constraint_type;
		int			i;

		if (stmt->primary)
			constraint_type = "PRIMARY KEY";
		else if (stmt->unique)
			constraint_type = "UNIQUE";
		else if (stmt->excludeOpNames)
			constraint_type = "EXCLUDE";
		else
		{
			elog(ERROR, "unknown constraint type");
			constraint_type = NULL; /* keep compiler quiet */
			/*
			 *
			 * 避免编译器因未使用变量告警。
			 */
		}

		/*
		 * Verify that all the columns in the partition key appear in the
		 * unique key definition, with the same notion of equality.
		 *
		 * 确认分区键中的每一列都以相同的相等语义出现在唯一键定义中。
		 */
		for (i = 0; i < key->partnatts; i++)
		{
			bool		found = false;
			int			eq_strategy;
			Oid			ptkey_eqop;
			int			j;

			/*
			 * Identify the equality operator associated with this partkey
			 * column.  For list and range partitioning, partkeys use btree
			 * operator classes; hash partitioning uses hash operator classes.
			 * (Keep this in sync with ComputePartitionAttrs!)
			 *
			 * 找出与该分区键列关联的相等操作符。列表与范围分区的分区键使用
			 * btree 操作符类；
			 * 哈希分区使用 hash 操作符类。（请与 ComputePartitionAttrs 保持同
			 * 步。）
			 */
			if (key->strategy == PARTITION_STRATEGY_HASH)
				eq_strategy = HTEqualStrategyNumber;
			else
				eq_strategy = BTEqualStrategyNumber;

			ptkey_eqop = get_opfamily_member(key->partopfamily[i],
											 key->partopcintype[i],
											 key->partopcintype[i],
											 eq_strategy);
			if (!OidIsValid(ptkey_eqop))
				elog(ERROR, "missing operator %d(%u,%u) in partition opfamily %u",
					 eq_strategy, key->partopcintype[i], key->partopcintype[i],
					 key->partopfamily[i]);

			/*
			 * It may be possible to support UNIQUE constraints when partition
			 * keys are expressions, but is it worth it?  Give up for now.
			 *
			 * 分区键为表达式时或许可以支持 UNIQUE 约束，但值不值得？目前先放
			 * 弃。
			 */
			if (key->partattrs[i] == 0)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("unsupported %s constraint with partition key definition",
								constraint_type),
						 errdetail("%s constraints cannot be used when partition keys include expressions.",
								   constraint_type)));

			/* Search the index column(s) for a match */
			/*
			 *
			 * 在索引列中查找匹配项。
			 */
			for (j = 0; j < indexInfo->ii_NumIndexKeyAttrs; j++)
			{
				if (key->partattrs[i] == indexInfo->ii_IndexAttrNumbers[j])
				{
					/*
					 * Matched the column, now what about the collation and
					 * equality op?
					 *
					 * 列已匹配，接下来检查排序规则和相等操作符。
					 */
					Oid			idx_opfamily;
					Oid			idx_opcintype;

					if (key->partcollation[i] != collationIds[j])
						continue;

					if (get_opclass_opfamily_and_input_type(opclassIds[j],
															&idx_opfamily,
															&idx_opcintype))
					{
						Oid			idx_eqop = InvalidOid;

						if (stmt->unique && !stmt->iswithoutoverlaps)
							idx_eqop = get_opfamily_member_for_cmptype(idx_opfamily,
																	   idx_opcintype,
																	   idx_opcintype,
																	   COMPARE_EQ);
						else if (exclusion)
							idx_eqop = indexInfo->ii_ExclusionOps[j];

						if (!idx_eqop)
							ereport(ERROR,
									errcode(ERRCODE_UNDEFINED_OBJECT),
									errmsg("could not identify an equality operator for type %s", format_type_be(idx_opcintype)),
									errdetail("There is no suitable operator in operator family \"%s\" for access method \"%s\".",
											  get_opfamily_name(idx_opfamily, false), get_am_name(get_opfamily_method(idx_opfamily))));

						if (ptkey_eqop == idx_eqop)
						{
							found = true;
							break;
						}
						else if (exclusion)
						{
							/*
							 * We found a match, but it's not an equality
							 * operator. Instead of failing below with an
							 * error message about a missing column, fail now
							 * and explain that the operator is wrong.
							 *
							 * 找到了匹配，但它不是相等操作符。与其在下面用缺
							 * 少列的错误失败，不如现在就失败并说明操作符不对。
							 */
							Form_pg_attribute att = TupleDescAttr(RelationGetDescr(rel), key->partattrs[i] - 1);

							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
									 errmsg("cannot match partition key to index on column \"%s\" using non-equal operator \"%s\"",
											NameStr(att->attname),
											get_opname(indexInfo->ii_ExclusionOps[j]))));
						}
					}
				}
			}

			if (!found)
			{
				Form_pg_attribute att;

				att = TupleDescAttr(RelationGetDescr(rel),
									key->partattrs[i] - 1);
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("unique constraint on partitioned table must include all partitioning columns"),
						 errdetail("%s constraint on table \"%s\" lacks column \"%s\" which is part of the partition key.",
								   constraint_type, RelationGetRelationName(rel),
								   NameStr(att->attname))));
			}
		}
	}


	/*
	 * We disallow indexes on system columns.  They would not necessarily get
	 * updated correctly, and they don't seem useful anyway.
	 *
	 * 不允许在系统列上建索引。它们不一定能被正确更新，而且似乎也没什么用。
	 *
	 * Also disallow virtual generated columns in indexes (use expression
	 * index instead).
	 *
	 * 也不允许把虚拟生成列直接放进索引（请改用表达式索引）。
	 */
	for (int i = 0; i < indexInfo->ii_NumIndexAttrs; i++)
	{
		AttrNumber	attno = indexInfo->ii_IndexAttrNumbers[i];

		if (attno < 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("index creation on system columns is not supported")));


		if (TupleDescAttr(RelationGetDescr(rel), attno - 1)->attgenerated == ATTRIBUTE_GENERATED_VIRTUAL)
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					stmt->primary ?
					errmsg("primary keys on virtual generated columns are not supported") :
					stmt->isconstraint ?
					errmsg("unique constraints on virtual generated columns are not supported") :
					errmsg("indexes on virtual generated columns are not supported"));
	}

	/*
	 * Also check for system and generated columns used in expressions or
	 * predicates.
	 *
	 * 同时检查表达式或谓词中使用的系统列和生成列。
	 */
	if (indexInfo->ii_Expressions || indexInfo->ii_Predicate)
	{
		Bitmapset  *indexattrs = NULL;
		int			j;

		pull_varattnos((Node *) indexInfo->ii_Expressions, 1, &indexattrs);
		pull_varattnos((Node *) indexInfo->ii_Predicate, 1, &indexattrs);

		for (int i = FirstLowInvalidHeapAttributeNumber + 1; i < 0; i++)
		{
			if (bms_is_member(i - FirstLowInvalidHeapAttributeNumber,
							  indexattrs))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("index creation on system columns is not supported")));
		}

		/*
		 * XXX Virtual generated columns in index expressions or predicates
		 * could be supported, but it needs support in
		 * RelationGetIndexExpressions() and RelationGetIndexPredicate().
		 *
		 * XXX：索引表达式或谓词中的虚拟生成列本来可以支持，但需要
		 * RelationGetIndexExpressions()
		 * 和 RelationGetIndexPredicate() 配合。
		 */
		j = -1;
		while ((j = bms_next_member(indexattrs, j)) >= 0)
		{
			AttrNumber	attno = j + FirstLowInvalidHeapAttributeNumber;

			if (TupleDescAttr(RelationGetDescr(rel), attno - 1)->attgenerated == ATTRIBUTE_GENERATED_VIRTUAL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 stmt->isconstraint ?
						 errmsg("unique constraints on virtual generated columns are not supported") :
						 errmsg("indexes on virtual generated columns are not supported")));
		}
	}

	/* Is index safe for others to ignore?  See set_indexsafe_procflags() */
	/*
	 *
	 * 该索引是否可以安全地被其他进程忽略？见 set_indexsafe_procflags()。
	 */
	safe_index = indexInfo->ii_Expressions == NIL &&
		indexInfo->ii_Predicate == NIL;

	/*
	 * Report index creation if appropriate (delay this till after most of the
	 * error checks)
	 *
	 * 在适当时报告索引创建（推迟到大部分错误检查之后）。
	 */
	if (stmt->isconstraint && !quiet)
	{
		const char *constraint_type;

		if (stmt->primary)
			constraint_type = "PRIMARY KEY";
		else if (stmt->unique)
			constraint_type = "UNIQUE";
		else if (stmt->excludeOpNames)
			constraint_type = "EXCLUDE";
		else
		{
			elog(ERROR, "unknown constraint type");
			constraint_type = NULL; /* keep compiler quiet */
			/*
			 *
			 * 避免编译器因未使用变量告警。
			 */
		}

		ereport(DEBUG1,
				(errmsg_internal("%s %s will create implicit index \"%s\" for table \"%s\"",
								 is_alter_table ? "ALTER TABLE / ADD" : "CREATE TABLE /",
								 constraint_type,
								 indexRelationName, RelationGetRelationName(rel))));
	}

	/*
	 * A valid stmt->oldNumber implies that we already have a built form of
	 * the index.  The caller should also decline any index build.
	 *
	 * 有效的 stmt->oldNumber 表示索引的构建形态已经存在。调用方也应拒绝任何索
	 * 引构建。
	 */
	Assert(!RelFileNumberIsValid(stmt->oldNumber) || (skip_build && !concurrent));

	/*
	 * Make the catalog entries for the index, including constraints. This
	 * step also actually builds the index, except if caller requested not to
	 * or in concurrent mode, in which case it'll be done later, or doing a
	 * partitioned index (because those don't have storage).
	 *
	 * 为索引建立目录项，包括约束。这一步通常也会真正构建索引，除非调用方要求
	 * 跳过、处于并发模式
	 * （那种情况下稍后构建），或正在做分区索引（分区索引没有存储）。
	 */
	flags = constr_flags = 0;
	if (stmt->isconstraint)
		flags |= INDEX_CREATE_ADD_CONSTRAINT;
	if (skip_build || concurrent || partitioned)
		flags |= INDEX_CREATE_SKIP_BUILD;
	if (stmt->if_not_exists)
		flags |= INDEX_CREATE_IF_NOT_EXISTS;
	if (concurrent)
		flags |= INDEX_CREATE_CONCURRENT;
	if (partitioned)
		flags |= INDEX_CREATE_PARTITIONED;
	if (stmt->primary)
		flags |= INDEX_CREATE_IS_PRIMARY;

	/*
	 * If the table is partitioned, and recursion was declined but partitions
	 * exist, mark the index as invalid.
	 *
	 * 若表已分区、调用方拒绝递归但分区存在，则把索引标为无效。
	 */
	if (partitioned && stmt->relation && !stmt->relation->inh)
	{
		PartitionDesc pd = RelationGetPartitionDesc(rel, true);

		if (pd->nparts != 0)
			flags |= INDEX_CREATE_INVALID;
	}

	if (stmt->deferrable)
		constr_flags |= INDEX_CONSTR_CREATE_DEFERRABLE;
	if (stmt->initdeferred)
		constr_flags |= INDEX_CONSTR_CREATE_INIT_DEFERRED;
	if (stmt->iswithoutoverlaps)
		constr_flags |= INDEX_CONSTR_CREATE_WITHOUT_OVERLAPS;

	indexRelationId =
		index_create(rel, indexRelationName, indexRelationId, parentIndexId,
					 parentConstraintId,
					 stmt->oldNumber, indexInfo, indexColNames,
					 accessMethodId, tablespaceId,
					 collationIds, opclassIds, opclassOptions,
					 coloptions, NULL, reloptions,
					 flags, constr_flags,
					 allowSystemTableMods, !check_rights,
					 &createdConstraintId);

	ObjectAddressSet(address, RelationRelationId, indexRelationId);

	if (!OidIsValid(indexRelationId))
	{
		/*
		 * Roll back any GUC changes executed by index functions.  Also revert
		 * to original default_tablespace if we changed it above.
		 *
		 * 回滚索引函数执行过的任何 GUC 变更。若上面改过 default_tablespace，
		 * 也恢复原来的值。
		 */
		AtEOXact_GUC(false, root_save_nestlevel);

		/* Restore userid and security context */
		/*
		 *
		 * 恢复 userid 和安全上下文。
		 */
		SetUserIdAndSecContext(root_save_userid, root_save_sec_context);

		table_close(rel, NoLock);

		/* If this is the top-level index, we're done */
		/*
		 *
		 * 若这是顶层索引，则到此结束。
		 */
		if (!OidIsValid(parentIndexId))
			pgstat_progress_end_command();

		return address;
	}

	/*
	 * Roll back any GUC changes executed by index functions, and keep
	 * subsequent changes local to this command.  This is essential if some
	 * index function changed a behavior-affecting GUC, e.g. search_path.
	 *
	 * 回滚索引函数执行过的任何 GUC 变更，并使后续变更局部于本命令。若某个索引
	 * 函数改了会影响行为的
	 * GUC（例如 search_path），这一点至关重要。
	 */
	AtEOXact_GUC(false, root_save_nestlevel);
	root_save_nestlevel = NewGUCNestLevel();
	RestrictSearchPath();

	/* Add any requested comment */
	/*
	 *
	 * 添加所请求的注释。
	 */
	if (stmt->idxcomment != NULL)
		CreateComments(indexRelationId, RelationRelationId, 0,
					   stmt->idxcomment);

	if (partitioned)
	{
		PartitionDesc partdesc;

		/*
		 * Unless caller specified to skip this step (via ONLY), process each
		 * partition to make sure they all contain a corresponding index.
		 *
		 * 除非调用方用 ONLY 要求跳过这一步，否则处理每个分区，确保它们都有对
		 * 应的索引。
		 *
		 * If we're called internally (no stmt->relation), recurse always.
		 *
		 * 若是内部调用（没有 stmt->relation），则总是递归。
		 */
		partdesc = RelationGetPartitionDesc(rel, true);
		if ((!stmt->relation || stmt->relation->inh) && partdesc->nparts > 0)
		{
			int			nparts = partdesc->nparts;
			Oid		   *part_oids = palloc_array(Oid, nparts);
			bool		invalidate_parent = false;
			Relation	parentIndex;
			TupleDesc	parentDesc;

			/*
			 * Report the total number of partitions at the start of the
			 * command; don't update it when being called recursively.
			 *
			 * 在命令开始时报告分区总数；递归调用时不要更新它。
			 */
			if (!OidIsValid(parentIndexId))
			{
				/*
				 * When called by ProcessUtilitySlow, the number of partitions
				 * is passed in as an optimization; but other callers pass -1
				 * since they don't have the value handy.  This should count
				 * partitions the same way, ie one less than the number of
				 * relations find_all_inheritors reports.
				 *
				 * 由 ProcessUtilitySlow 调用时，分区数作为优化传入；其他调用
				 * 方因为手头没有这个值而传 -1。
				 * 这里的计数方式应与之一致，即比 find_all_inheritors 报告的关
				 * 系数少一。
				 *
				 * We assume we needn't ask find_all_inheritors to take locks,
				 * because that should have happened already for all callers.
				 * Even if it did not, this is safe as long as we don't try to
				 * touch the partitions here; the worst consequence would be a
				 * bogus progress-reporting total.
				 *
				 * 假定不必让 find_all_inheritors 加锁，因为所有调用方应该已经
				 * 加过锁。即使没有，只要这里不去碰分区，
				 * 也是安全的；最坏后果只是进度报告的总数不准。
				 */
				if (total_parts < 0)
				{
					List	   *children = find_all_inheritors(tableId, NoLock, NULL);

					total_parts = list_length(children) - 1;
					list_free(children);
				}

				pgstat_progress_update_param(PROGRESS_CREATEIDX_PARTITIONS_TOTAL,
											 total_parts);
			}

			/* Make a local copy of partdesc->oids[], just for safety */
			/*
			 *
			 * 为安全起见，复制一份 partdesc->oids[] 的本地副本。
			 */
			memcpy(part_oids, partdesc->oids, sizeof(Oid) * nparts);

			/*
			 * We'll need an IndexInfo describing the parent index.  The one
			 * built above is almost good enough, but not quite, because (for
			 * example) its predicate expression if any hasn't been through
			 * expression preprocessing.  The most reliable way to get an
			 * IndexInfo that will match those for child indexes is to build
			 * it the same way, using BuildIndexInfo().
			 *
			 * 需要一份描述父索引的 IndexInfo。上面建的那份几乎够用，但还不完
			 * 整，例如它的谓词表达式（若有）
			 * 还没有经过表达式预处理。最可靠的办法是用同样的方式、通过
			 * BuildIndexInfo() 得到与子索引匹配的 IndexInfo。
			 */
			parentIndex = index_open(indexRelationId, lockmode);
			indexInfo = BuildIndexInfo(parentIndex);

			parentDesc = RelationGetDescr(rel);

			/*
			 * For each partition, scan all existing indexes; if one matches
			 * our index definition and is not already attached to some other
			 * parent index, attach it to the one we just created.
			 *
			 * 对每个分区，扫描全部已有索引；若某个索引匹配我们的定义，且尚未
			 * 挂到别的父索引上，就把它挂到刚创建的父索引。
			 *
			 * If none matches, build a new index by calling ourselves
			 * recursively with the same options (except for the index name).
			 *
			 * 若没有匹配项，则用相同选项（索引名除外）递归调用自身来构建新索
			 * 引。
			 */
			for (int i = 0; i < nparts; i++)
			{
				Oid			childRelid = part_oids[i];
				Relation	childrel;
				Oid			child_save_userid;
				int			child_save_sec_context;
				int			child_save_nestlevel;
				List	   *childidxs;
				ListCell   *cell;
				AttrMap    *attmap;
				bool		found = false;

				childrel = table_open(childRelid, lockmode);

				GetUserIdAndSecContext(&child_save_userid,
									   &child_save_sec_context);
				SetUserIdAndSecContext(childrel->rd_rel->relowner,
									   child_save_sec_context | SECURITY_RESTRICTED_OPERATION);
				child_save_nestlevel = NewGUCNestLevel();
				RestrictSearchPath();

				/*
				 * Don't try to create indexes on foreign tables, though. Skip
				 * those if a regular index, or fail if trying to create a
				 * constraint index.
				 *
				 * 不过不要尝试在外部表上创建索引。普通索引则跳过这些表；若要
				 * 创建约束索引则失败。
				 */
				if (childrel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
				{
					if (stmt->unique || stmt->primary)
						ereport(ERROR,
								(errcode(ERRCODE_WRONG_OBJECT_TYPE),
								 errmsg("cannot create unique index on partitioned table \"%s\"",
										RelationGetRelationName(rel)),
								 errdetail("Table \"%s\" contains partitions that are foreign tables.",
										   RelationGetRelationName(rel))));

					AtEOXact_GUC(false, child_save_nestlevel);
					SetUserIdAndSecContext(child_save_userid,
										   child_save_sec_context);
					table_close(childrel, lockmode);
					continue;
				}

				childidxs = RelationGetIndexList(childrel);
				attmap =
					build_attrmap_by_name(RelationGetDescr(childrel),
										  parentDesc,
										  false);

				foreach(cell, childidxs)
				{
					Oid			cldidxid = lfirst_oid(cell);
					Relation	cldidx;
					IndexInfo  *cldIdxInfo;

					/* this index is already partition of another one */
					/*
					 *
					 * 该索引已经是另一个索引的分区。
					 */
					if (has_superclass(cldidxid))
						continue;

					cldidx = index_open(cldidxid, lockmode);
					cldIdxInfo = BuildIndexInfo(cldidx);
					if (CompareIndexInfo(cldIdxInfo, indexInfo,
										 cldidx->rd_indcollation,
										 parentIndex->rd_indcollation,
										 cldidx->rd_opfamily,
										 parentIndex->rd_opfamily,
										 attmap))
					{
						Oid			cldConstrOid = InvalidOid;

						/*
						 * Found a match.
						 *
						 * 找到匹配项。
						 *
						 * If this index is being created in the parent
						 * because of a constraint, then the child needs to
						 * have a constraint also, so look for one.  If there
						 * is no such constraint, this index is no good, so
						 * keep looking.
						 *
						 * 若父表上的这个索引是因约束而创建的，子表也必须有对
						 * 应约束，因此去找一个。
						 * 若没有这样的约束，这个索引就不合适，继续查找。
						 */
						if (createdConstraintId != InvalidOid)
						{
							cldConstrOid =
								get_relation_idx_constraint_oid(childRelid,
																cldidxid);
							if (cldConstrOid == InvalidOid)
							{
								index_close(cldidx, lockmode);
								continue;
							}
						}

						/* Attach index to parent and we're done. */
						/*
						 *
						 * 把索引挂到父索引上，即告完成。
						 */
						IndexSetParentIndex(cldidx, indexRelationId);
						if (createdConstraintId != InvalidOid)
							ConstraintSetParentConstraint(cldConstrOid,
														  createdConstraintId,
														  childRelid);

						if (!cldidx->rd_index->indisvalid)
							invalidate_parent = true;

						found = true;

						/*
						 * Report this partition as processed.  Note that if
						 * the partition has children itself, we'd ideally
						 * count the children and update the progress report
						 * for all of them; but that seems unduly expensive.
						 * Instead, the progress report will act like all such
						 * indirect children were processed in zero time at
						 * the end of the command.
						 *
						 * 把该分区报告为已处理。注意若分区自身还有子分区，理
						 * 想情况下应计入子分区并更新它们的进度；
						 * 但那样代价过高。进度报告会表现成所有这些间接子分区
						 * 在命令结束时以零耗时被处理完。
						 */
						pgstat_progress_incr_param(PROGRESS_CREATEIDX_PARTITIONS_DONE, 1);

						/* keep lock till commit */
						/*
						 *
						 * 把锁保持到提交。
						 */
						index_close(cldidx, NoLock);
						break;
					}

					index_close(cldidx, lockmode);
				}

				list_free(childidxs);
				AtEOXact_GUC(false, child_save_nestlevel);
				SetUserIdAndSecContext(child_save_userid,
									   child_save_sec_context);
				table_close(childrel, NoLock);

				/*
				 * If no matching index was found, create our own.
				 *
				 * 若未找到匹配索引，则自己创建一个。
				 */
				if (!found)
				{
					IndexStmt  *childStmt;
					ObjectAddress childAddr;

					/*
					 * Build an IndexStmt describing the desired child index
					 * in the same way that we do during ATTACH PARTITION.
					 * Notably, we rely on generateClonedIndexStmt to produce
					 * a search-path-independent representation, which the
					 * original IndexStmt might not be.
					 *
					 * 构造描述所需子索引的 IndexStmt，方式与 ATTACH PARTITION
					 * 时相同。
					 * 尤其依赖 generateClonedIndexStmt 生成与 search_path 无
					 * 关的表示，原始 IndexStmt 未必如此。
					 */
					childStmt = generateClonedIndexStmt(NULL,
														parentIndex,
														attmap,
														NULL);

					/*
					 * Recurse as the starting user ID.  Callee will use that
					 * for permission checks, then switch again.
					 *
					 * 以起始用户 ID 递归。被调用方会用它做权限检查，然后再切
					 * 换。
					 */
					Assert(GetUserId() == child_save_userid);
					SetUserIdAndSecContext(root_save_userid,
										   root_save_sec_context);
					childAddr =
						DefineIndex(childRelid, childStmt,
									InvalidOid, /* no predefined OID */
									/*
									 *
									 * 没有预定义 OID。
									 */
									indexRelationId,	/* this is our child */
									/*
									 *
									 * 这是我们的子索引。
									 */
									createdConstraintId,
									-1,
									is_alter_table, check_rights,
									check_not_in_use,
									skip_build, quiet);
					SetUserIdAndSecContext(child_save_userid,
										   child_save_sec_context);

					/*
					 * Check if the index just created is valid or not, as it
					 * could be possible that it has been switched as invalid
					 * when recursing across multiple partition levels.
					 *
					 * 检查刚创建的索引是否有效。跨多层分区递归时，它可能已被
					 * 改成无效。
					 */
					if (!get_index_isvalid(childAddr.objectId))
						invalidate_parent = true;
				}

				free_attrmap(attmap);
			}

			index_close(parentIndex, lockmode);

			/*
			 * The pg_index row we inserted for this index was marked
			 * indisvalid=true.  But if we attached an existing index that is
			 * invalid, this is incorrect, so update our row to invalid too.
			 *
			 * 我们为该索引插入的 pg_index 行被标为 indisvalid=true。但若挂上
			 * 的已有索引是无效的，
			 * 这就不对了，因此也要把我们的行更新为无效。
			 */
			if (invalidate_parent)
			{
				Relation	pg_index = table_open(IndexRelationId, RowExclusiveLock);
				HeapTuple	tup,
							newtup;

				tup = SearchSysCache1(INDEXRELID,
									  ObjectIdGetDatum(indexRelationId));
				if (!HeapTupleIsValid(tup))
					elog(ERROR, "cache lookup failed for index %u",
						 indexRelationId);
				newtup = heap_copytuple(tup);
				((Form_pg_index) GETSTRUCT(newtup))->indisvalid = false;
				CatalogTupleUpdate(pg_index, &tup->t_self, newtup);
				ReleaseSysCache(tup);
				table_close(pg_index, RowExclusiveLock);
				heap_freetuple(newtup);

				/*
				 * CCI here to make this update visible, in case this recurses
				 * across multiple partition levels.
				 *
				 * 在此执行 CommandCounterIncrement，使本次更新可见，以备跨多
				 * 层分区递归。
				 */
				CommandCounterIncrement();
			}
		}

		/*
		 * Indexes on partitioned tables are not themselves built, so we're
		 * done here.
		 *
		 * 分区表上的索引本身不会被构建，因此到此结束。
		 */
		AtEOXact_GUC(false, root_save_nestlevel);
		SetUserIdAndSecContext(root_save_userid, root_save_sec_context);
		table_close(rel, NoLock);
		if (!OidIsValid(parentIndexId))
			pgstat_progress_end_command();
		else
		{
			/* Update progress for an intermediate partitioned index itself */
			/*
			 *
			 * 为中间层分区索引本身更新进度。
			 */
			pgstat_progress_incr_param(PROGRESS_CREATEIDX_PARTITIONS_DONE, 1);
		}

		return address;
	}

	AtEOXact_GUC(false, root_save_nestlevel);
	SetUserIdAndSecContext(root_save_userid, root_save_sec_context);

	if (!concurrent)
	{
		/* Close the heap and we're done, in the non-concurrent case */
		/*
		 *
		 * 非并发情况下关闭堆并结束。
		 */
		table_close(rel, NoLock);

		/*
		 * If this is the top-level index, the command is done overall;
		 * otherwise, increment progress to report one child index is done.
		 *
		 * 若这是顶层索引，整条命令即告完成；否则增加进度，报告一个子索引已完
		 * 成。
		 */
		if (!OidIsValid(parentIndexId))
			pgstat_progress_end_command();
		else
			pgstat_progress_incr_param(PROGRESS_CREATEIDX_PARTITIONS_DONE, 1);

		return address;
	}

	/* save lockrelid and locktag for below, then close rel */
	/*
	 *
	 * 保存 lockrelid 和 locktag 供后面使用，然后关闭关系。
	 */
	heaprelid = rel->rd_lockInfo.lockRelId;
	SET_LOCKTAG_RELATION(heaplocktag, heaprelid.dbId, heaprelid.relId);
	table_close(rel, NoLock);

	/*
	 * For a concurrent build, it's important to make the catalog entries
	 * visible to other transactions before we start to build the index. That
	 * will prevent them from making incompatible HOT updates.  The new index
	 * will be marked not indisready and not indisvalid, so that no one else
	 * tries to either insert into it or use it for queries.
	 *
	 * 并发构建时，必须在开始构建索引之前让目录项对其他事务可见。这样可以防止
	 * 它们做出不兼容的 HOT 更新。
	 * 新索引会被标为既非 indisready 也非 indisvalid，以免别人向它插入或用它做
	 * 查询。
	 *
	 * We must commit our current transaction so that the index becomes
	 * visible; then start another.  Note that all the data structures we just
	 * built are lost in the commit.  The only data we keep past here are the
	 * relation IDs.
	 *
	 * 必须提交当前事务，索引才会变得可见；然后再开始一个新事务。注意刚才建好
	 * 的数据结构会在提交时丢失。
	 * 此后保留的只有关系 ID。
	 *
	 * Before committing, get a session-level lock on the table, to ensure
	 * that neither it nor the index can be dropped before we finish. This
	 * cannot block, even if someone else is waiting for access, because we
	 * already have the same lock within our transaction.
	 *
	 * 提交前先取得表上的会话级锁，确保在我们完成之前，表和索引都不会被删除。
	 * 这不会阻塞，即使别人正在等待访问，
	 * 因为我们在本事务内已经持有同样的锁。
	 *
	 * Note: we don't currently bother with a session lock on the index,
	 * because there are no operations that could change its state while we
	 * hold lock on the parent table.  This might need to change later.
	 *
	 * 注意：目前不为索引本身加会话锁，因为在持有父表锁期间，没有操作能改变它
	 * 的状态。以后也许需要改变这一点。
	 */
	LockRelationIdForSession(&heaprelid, ShareUpdateExclusiveLock);

	PopActiveSnapshot();
	CommitTransactionCommand();
	StartTransactionCommand();

	/* Tell concurrent index builds to ignore us, if index qualifies */
	/*
	 *
	 * 若索引符合条件，告诉其他并发索引构建忽略我们。
	 */
	if (safe_index)
		set_indexsafe_procflags();

	/*
	 * The index is now visible, so we can report the OID.  While on it,
	 * include the report for the beginning of phase 2.
	 *
	 * 索引现已可见，因此可以报告 OID。同时报告第 2 阶段开始。
	 */
	{
		const int	progress_cols[] = {
			PROGRESS_CREATEIDX_INDEX_OID,
			PROGRESS_CREATEIDX_PHASE
		};
		const int64 progress_vals[] = {
			indexRelationId,
			PROGRESS_CREATEIDX_PHASE_WAIT_1
		};

		pgstat_progress_update_multi_param(2, progress_cols, progress_vals);
	}

	/*
	 * Phase 2 of concurrent index build (see comments for validate_index()
	 * for an overview of how this works)
	 *
	 * 并发索引构建的第 2 阶段（工作方式概述见 validate_index() 的注释）。
	 *
	 * Now we must wait until no running transaction could have the table open
	 * with the old list of indexes.  Use ShareLock to consider running
	 * transactions that hold locks that permit writing to the table.  Note we
	 * do not need to worry about xacts that open the table for writing after
	 * this point; they will see the new index when they open it.
	 *
	 * 现在必须等到没有任何运行中的事务还拿着旧的索引列表打开该表。用
	 * ShareLock 来考虑那些持有允许写表的锁的事务。
	 * 不必担心此点之后才打开表准备写入的事务，它们打开时会看到新索引。
	 *
	 * Note: the reason we use actual lock acquisition here, rather than just
	 * checking the ProcArray and sleeping, is that deadlock is possible if
	 * one of the transactions in question is blocked trying to acquire an
	 * exclusive lock on our table.  The lock code will detect deadlock and
	 * error out properly.
	 *
	 * 注意：这里真正去获取锁，而不是只检查 ProcArray 然后睡眠，是因为若相关事
	 * 务正阻塞在获取我们表的排他锁上，
	 * 就可能死锁。锁代码会正确检测死锁并报错。
	 */
	WaitForLockers(heaplocktag, ShareLock, true);

	/*
	 * At this moment we are sure that there are no transactions with the
	 * table open for write that don't have this new index in their list of
	 * indexes.  We have waited out all the existing transactions and any new
	 * transaction will have the new index in its list, but the index is still
	 * marked as "not-ready-for-inserts".  The index is consulted while
	 * deciding HOT-safety though.  This arrangement ensures that no new HOT
	 * chains can be created where the new tuple and the old tuple in the
	 * chain have different index keys.
	 *
	 * 此时可以确定：没有任何为写而打开该表、却没有把新索引列入索引列表的事务。
	 * 已有事务都已等完，
	 * 新事务的列表里会有新索引，但索引仍被标为 not-ready-for-inserts。决定
	 * HOT 安全性时仍会参考该索引。
	 * 这样可以保证不会出现新的 HOT 链，使得链上新旧元组的索引键不同。
	 *
	 * We now take a new snapshot, and build the index using all tuples that
	 * are visible in this snapshot.  We can be sure that any HOT updates to
	 * these tuples will be compatible with the index, since any updates made
	 * by transactions that didn't know about the index are now committed or
	 * rolled back.  Thus, each visible tuple is either the end of its
	 * HOT-chain or the extension of the chain is HOT-safe for this index.
	 *
	 * 现在取一个新快照，并用该快照中可见的全部元组构建索引。可以确定对这些元
	 * 组的任何 HOT 更新都与索引兼容，
	 * 因为不知道该索引的事务所做的更新都已提交或回滚。因此每个可见元组要么是
	 * 其 HOT 链的末端，
	 * 要么链的延伸对该索引是 HOT 安全的。
	 */

	/* Set ActiveSnapshot since functions in the indexes may need it */
	/*
	 *
	 * 设置 ActiveSnapshot，因为索引中的函数可能需要它。
	 */
	PushActiveSnapshot(GetTransactionSnapshot());

	/* Perform concurrent build of index */
	/*
	 *
	 * 执行索引的并发构建。
	 */
	index_concurrently_build(tableId, indexRelationId);

	/* we can do away with our snapshot */
	/*
	 *
	 * 可以丢掉我们的快照了。
	 */
	PopActiveSnapshot();

	/*
	 * Commit this transaction to make the indisready update visible.
	 *
	 * 提交本事务，使 indisready 更新可见。
	 */
	CommitTransactionCommand();
	StartTransactionCommand();

	/* Tell concurrent index builds to ignore us, if index qualifies */
	/*
	 *
	 * 若索引符合条件，告诉其他并发索引构建忽略我们。
	 */
	if (safe_index)
		set_indexsafe_procflags();

	/*
	 * Phase 3 of concurrent index build
	 *
	 * 并发索引构建的第 3 阶段。
	 *
	 * We once again wait until no transaction can have the table open with
	 * the index marked as read-only for updates.
	 *
	 * 再次等待，直到没有事务还把该索引当作只读更新而打开着表。
	 */
	pgstat_progress_update_param(PROGRESS_CREATEIDX_PHASE,
								 PROGRESS_CREATEIDX_PHASE_WAIT_2);
	WaitForLockers(heaplocktag, ShareLock, true);

	/*
	 * Now take the "reference snapshot" that will be used by validate_index()
	 * to filter candidate tuples.  Beware!  There might still be snapshots in
	 * use that treat some transaction as in-progress that our reference
	 * snapshot treats as committed.  If such a recently-committed transaction
	 * deleted tuples in the table, we will not include them in the index; yet
	 * those transactions which see the deleting one as still-in-progress will
	 * expect such tuples to be there once we mark the index as valid.
	 *
	 * 现在取得 validate_index() 用来过滤候选元组的参考快照。当心：仍可能有快
	 * 照把某个事务看成进行中，
	 * 而我们的参考快照把它看成已提交。若这样一个刚提交的事务删除了表中的元组，
	 * 我们不会把它们放进索引；
	 * 但那些仍把删除事务看成进行中的事务，会期望在我们把索引标为有效之后这些
	 * 元组还在。
	 *
	 * We solve this by waiting for all endangered transactions to exit before
	 * we mark the index as valid.
	 *
	 * 解决办法是在把索引标为有效之前，等所有处于危险中的事务退出。
	 *
	 * We also set ActiveSnapshot to this snap, since functions in indexes may
	 * need a snapshot.
	 *
	 * 同时把 ActiveSnapshot 设为该快照，因为索引中的函数可能需要快照。
	 */
	snapshot = RegisterSnapshot(GetTransactionSnapshot());
	PushActiveSnapshot(snapshot);

	/*
	 * Scan the index and the heap, insert any missing index entries.
	 *
	 * 扫描索引和堆，插入任何缺失的索引项。
	 */
	validate_index(tableId, indexRelationId, snapshot);

	/*
	 * Drop the reference snapshot.  We must do this before waiting out other
	 * snapshot holders, else we will deadlock against other processes also
	 * doing CREATE INDEX CONCURRENTLY, which would see our snapshot as one
	 * they must wait for.  But first, save the snapshot's xmin to use as
	 * limitXmin for GetCurrentVirtualXIDs().
	 *
	 * 丢掉参考快照。必须在等待其他快照持有者之前做这件事，否则会与同样在做
	 * CREATE INDEX CONCURRENTLY 的进程死锁，
	 * 它们会把我们的快照当成必须等待的对象。但首先保存快照的 xmin，作为
	 * GetCurrentVirtualXIDs() 的 limitXmin。
	 */
	limitXmin = snapshot->xmin;

	PopActiveSnapshot();
	UnregisterSnapshot(snapshot);

	/*
	 * The snapshot subsystem could still contain registered snapshots that
	 * are holding back our process's advertised xmin; in particular, if
	 * default_transaction_isolation = serializable, there is a transaction
	 * snapshot that is still active.  The CatalogSnapshot is likewise a
	 * hazard.  To ensure no deadlocks, we must commit and start yet another
	 * transaction, and do our wait before any snapshot has been taken in it.
	 *
	 * 快照子系统里仍可能有已注册快照拉住本进程对外公布的 xmin；特别是当
	 * default_transaction_isolation = serializable 时，
	 * 还有一个仍然活动的事务快照。CatalogSnapshot 同样是隐患。为避免死锁，必
	 * 须提交并再开一个事务，
	 * 并在该事务取得任何快照之前完成等待。
	 */
	CommitTransactionCommand();
	StartTransactionCommand();

	/* Tell concurrent index builds to ignore us, if index qualifies */
	/*
	 *
	 * 若索引符合条件，告诉其他并发索引构建忽略我们。
	 */
	if (safe_index)
		set_indexsafe_procflags();

	/* We should now definitely not be advertising any xmin. */
	/*
	 *
	 * 现在肯定不应再对外公布任何 xmin。
	 */
	Assert(MyProc->xmin == InvalidTransactionId);

	/*
	 * The index is now valid in the sense that it contains all currently
	 * interesting tuples.  But since it might not contain tuples deleted just
	 * before the reference snap was taken, we have to wait out any
	 * transactions that might have older snapshots.
	 *
	 * 就包含当前所有有关元组而言，索引现在是有效的。但它可能不含参考快照取得
	 * 之前刚被删除的元组，
	 * 因此必须等完任何可能持有更旧快照的事务。
	 */
	pgstat_progress_update_param(PROGRESS_CREATEIDX_PHASE,
								 PROGRESS_CREATEIDX_PHASE_WAIT_3);
	WaitForOlderSnapshots(limitXmin, true);

	/*
	 * Updating pg_index might involve TOAST table access, so ensure we have a
	 * valid snapshot.
	 *
	 * 更新 pg_index 可能访问 TOAST 表，因此要确保有一个有效快照。
	 */
	PushActiveSnapshot(GetTransactionSnapshot());

	/*
	 * Index can now be marked valid -- update its pg_index entry
	 *
	 * 现在可以把索引标为有效——更新它的 pg_index 项。
	 */
	index_set_state_flags(indexRelationId, INDEX_CREATE_SET_VALID);

	PopActiveSnapshot();

	/*
	 * The pg_index update will cause backends (including this one) to update
	 * relcache entries for the index itself, but we should also send a
	 * relcache inval on the parent table to force replanning of cached plans.
	 * Otherwise existing sessions might fail to use the new index where it
	 * would be useful.  (Note that our earlier commits did not create reasons
	 * to replan; so relcache flush on the index itself was sufficient.)
	 *
	 * pg_index 更新会使各后端（包括本后端）更新索引自身的 relcache 项，但还应
	 * 向父表发送 relcache 失效，
	 * 以强制重新规划已缓存的计划。否则现有会话可能在新索引有用时却不使用它。
	 * （先前的提交并没有产生重新规划的理由，因此只刷新索引自身的 relcache 就
	 * 够了。）
	 */
	CacheInvalidateRelcacheByRelid(heaprelid.relId);

	/*
	 * Last thing to do is release the session-level lock on the parent table.
	 *
	 * 最后释放父表上的会话级锁。
	 */
	UnlockRelationIdForSession(&heaprelid, ShareUpdateExclusiveLock);

	pgstat_progress_end_command();

	return address;
}


/*
 * CheckPredicate
 *		Checks that the given partial-index predicate is valid.
 *
 *		检查给定的部分索引谓词是否有效。
 *
 * This used to also constrain the form of the predicate to forms that
 * indxpath.c could do something with.  However, that seems overly
 * restrictive.  One useful application of partial indexes is to apply
 * a UNIQUE constraint across a subset of a table, and in that scenario
 * any evaluable predicate will work.  So accept any predicate here
 * (except ones requiring a plan), and let indxpath.c fend for itself.
 *
 * 这里曾经还把谓词形式限制为 indxpath.c 能处理的样子。但那似乎过于严格。部分
 * 索引的一个有用场景是
 * 对表的一个子集施加 UNIQUE 约束，此时任何可求值的谓词都可以。因此这里接受任
 * 何谓词（需要计划的除外），
 * 让 indxpath.c 自己应对。
 */
static void
CheckPredicate(Expr *predicate)
{
	/*
	 * transformExpr() should have already rejected subqueries, aggregates,
	 * and window functions, based on the EXPR_KIND_ for a predicate.
	 *
	 * transformExpr() 应已根据谓词的 EXPR_KIND_ 拒绝了子查询、聚合和窗口函数。
	 */

	/*
	 * A predicate using mutable functions is probably wrong, for the same
	 * reasons that we don't allow an index expression to use one.
	 *
	 * 谓词使用易变函数多半是错的，理由与不允许索引表达式使用易变函数相同。
	 */
	if (contain_mutable_functions_after_planning(predicate))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("functions in index predicate must be marked IMMUTABLE")));
}

/*
 * Compute per-index-column information, including indexed column numbers
 * or index expressions, opclasses and their options. Note, all output vectors
 * should be allocated for all columns, including "including" ones.
 *
 * 计算每个索引列的信息，包括被索引的列号或索引表达式、操作符类及其选项。
 * 注意所有输出向量都应按全部列分配，包括 INCLUDE 列。
 *
 * If the caller switched to the table owner, ddl_userid is the role for ACL
 * checks reached without traversing opaque expressions.  Otherwise, it's
 * InvalidOid, and other ddl_* arguments are undefined.
 *
 * 若调用方已切换到表所有者，ddl_userid 就是不经过不透明表达式即可到达的 ACL
 * 检查所用角色。
 * 否则它是 InvalidOid，其他 ddl_* 参数无定义。
 */
static void
ComputeIndexAttrs(IndexInfo *indexInfo,
				  Oid *typeOids,
				  Oid *collationOids,
				  Oid *opclassOids,
				  Datum *opclassOptions,
				  int16 *colOptions,
				  const List *attList,	/* list of IndexElem's */
				  /*
				   *
				   * IndexElem 列表。
				   */
				  const List *exclusionOpNames,
				  Oid relId,
				  const char *accessMethodName,
				  Oid accessMethodId,
				  bool amcanorder,
				  bool isconstraint,
				  bool iswithoutoverlaps,
				  Oid ddl_userid,
				  int ddl_sec_context,
				  int *ddl_save_nestlevel)
{
	ListCell   *nextExclOp;
	ListCell   *lc;
	int			attn;
	int			nkeycols = indexInfo->ii_NumIndexKeyAttrs;
	Oid			save_userid;
	int			save_sec_context;

	/* Allocate space for exclusion operator info, if needed */
	/*
	 *
	 * 若需要，为排他操作符信息分配空间。
	 */
	if (exclusionOpNames)
	{
		Assert(list_length(exclusionOpNames) == nkeycols);
		indexInfo->ii_ExclusionOps = palloc_array(Oid, nkeycols);
		indexInfo->ii_ExclusionProcs = palloc_array(Oid, nkeycols);
		indexInfo->ii_ExclusionStrats = palloc_array(uint16, nkeycols);
		nextExclOp = list_head(exclusionOpNames);
	}
	else
		nextExclOp = NULL;

	/*
	 * If this is a WITHOUT OVERLAPS constraint, we need space for exclusion
	 * ops, but we don't need to parse anything, so we can let nextExclOp be
	 * NULL. Note that for partitions/inheriting/LIKE, exclusionOpNames will
	 * be set, so we already allocated above.
	 *
	 * 若这是 WITHOUT OVERLAPS 约束，需要为排他操作符留出空间，但不必解析任何
	 * 内容，因此可以让 nextExclOp 为 NULL。
	 * 注意对分区、继承或 LIKE，exclusionOpNames 会被设置，所以上面已经分配过。
	 */
	if (iswithoutoverlaps)
	{
		if (exclusionOpNames == NIL)
		{
			indexInfo->ii_ExclusionOps = palloc_array(Oid, nkeycols);
			indexInfo->ii_ExclusionProcs = palloc_array(Oid, nkeycols);
			indexInfo->ii_ExclusionStrats = palloc_array(uint16, nkeycols);
		}
		nextExclOp = NULL;
	}

	if (OidIsValid(ddl_userid))
		GetUserIdAndSecContext(&save_userid, &save_sec_context);

	/*
	 * process attributeList
	 *
	 * 处理 attributeList。
	 */
	attn = 0;
	foreach(lc, attList)
	{
		IndexElem  *attribute = (IndexElem *) lfirst(lc);
		Oid			atttype;
		Oid			attcollation;

		/*
		 * Process the column-or-expression to be indexed.
		 *
		 * 处理要索引的列或表达式。
		 */
		if (attribute->name != NULL)
		{
			/* Simple index attribute */
			/*
			 *
			 * 简单索引属性。
			 */
			HeapTuple	atttuple;
			Form_pg_attribute attform;

			Assert(attribute->expr == NULL);
			atttuple = SearchSysCacheAttName(relId, attribute->name);
			if (!HeapTupleIsValid(atttuple))
			{
				/* difference in error message spellings is historical */
				/*
				 *
				 * 错误信息措辞的差异是历史原因。
				 */
				if (isconstraint)
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_COLUMN),
							 errmsg("column \"%s\" named in key does not exist",
									attribute->name)));
				else
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_COLUMN),
							 errmsg("column \"%s\" does not exist",
									attribute->name)));
			}
			attform = (Form_pg_attribute) GETSTRUCT(atttuple);
			indexInfo->ii_IndexAttrNumbers[attn] = attform->attnum;
			atttype = attform->atttypid;
			attcollation = attform->attcollation;
			ReleaseSysCache(atttuple);
		}
		else
		{
			/* Index expression */
			/*
			 *
			 * 索引表达式。
			 */
			Node	   *expr = attribute->expr;

			Assert(expr != NULL);

			if (attn >= nkeycols)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("expressions are not supported in included columns")));
			atttype = exprType(expr);
			attcollation = exprCollation(expr);

			/*
			 * Strip any top-level COLLATE clause.  This ensures that we treat
			 * "x COLLATE y" and "(x COLLATE y)" alike.
			 *
			 * 剥掉顶层 COLLATE 子句。这样 "x COLLATE y" 与 "(x COLLATE y)" 会
			 * 被同样对待。
			 */
			while (IsA(expr, CollateExpr))
				expr = (Node *) ((CollateExpr *) expr)->arg;

			if (IsA(expr, Var) &&
				((Var *) expr)->varattno != InvalidAttrNumber)
			{
				/*
				 * User wrote "(column)" or "(column COLLATE something)".
				 * Treat it like simple attribute anyway.
				 *
				 * 用户写了 "(column)" 或 "(column COLLATE something)"。仍然按
				 * 简单属性处理。
				 */
				indexInfo->ii_IndexAttrNumbers[attn] = ((Var *) expr)->varattno;
			}
			else
			{
				indexInfo->ii_IndexAttrNumbers[attn] = 0;	/* marks expression */
				/*
				 *
				 * 标记为表达式。
				 */
				indexInfo->ii_Expressions = lappend(indexInfo->ii_Expressions,
													expr);

				/*
				 * transformExpr() should have already rejected subqueries,
				 * aggregates, and window functions, based on the EXPR_KIND_
				 * for an index expression.
				 *
				 * transformExpr() 应已根据索引表达式的 EXPR_KIND_ 拒绝了子查
				 * 询、聚合和窗口函数。
				 */

				/*
				 * An expression using mutable functions is probably wrong,
				 * since if you aren't going to get the same result for the
				 * same data every time, it's not clear what the index entries
				 * mean at all.
				 *
				 * 表达式使用易变函数多半是错的：若同样的数据每次得不到同样的
				 * 结果，索引项的含义就不清楚了。
				 */
				if (contain_mutable_functions_after_planning((Expr *) expr))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("functions in index expression must be marked IMMUTABLE")));
			}
		}

		typeOids[attn] = atttype;

		/*
		 * Included columns have no collation, no opclass and no ordering
		 * options.
		 *
		 * INCLUDE 列没有排序规则、操作符类和排序选项。
		 */
		if (attn >= nkeycols)
		{
			if (attribute->collation)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("including column does not support a collation")));
			if (attribute->opclass)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("including column does not support an operator class")));
			if (attribute->ordering != SORTBY_DEFAULT)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("including column does not support ASC/DESC options")));
			if (attribute->nulls_ordering != SORTBY_NULLS_DEFAULT)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("including column does not support NULLS FIRST/LAST options")));

			opclassOids[attn] = InvalidOid;
			opclassOptions[attn] = (Datum) 0;
			colOptions[attn] = 0;
			collationOids[attn] = InvalidOid;
			attn++;

			continue;
		}

		/*
		 * Apply collation override if any.  Use of ddl_userid is necessary
		 * due to ACL checks therein, and it's safe because collations don't
		 * contain opaque expressions (or non-opaque expressions).
		 *
		 * 若有排序规则覆盖则应用它。这里必须使用 ddl_userid，因为其中有 ACL
		 * 检查；
		 * 这是安全的，因为排序规则不含不透明表达式（也不含非不透明表达式）。
		 */
		if (attribute->collation)
		{
			if (OidIsValid(ddl_userid))
			{
				AtEOXact_GUC(false, *ddl_save_nestlevel);
				SetUserIdAndSecContext(ddl_userid, ddl_sec_context);
			}
			attcollation = get_collation_oid(attribute->collation, false);
			if (OidIsValid(ddl_userid))
			{
				SetUserIdAndSecContext(save_userid, save_sec_context);
				*ddl_save_nestlevel = NewGUCNestLevel();
				RestrictSearchPath();
			}
		}

		/*
		 * Check we have a collation iff it's a collatable type.  The only
		 * expected failures here are (1) COLLATE applied to a noncollatable
		 * type, or (2) index expression had an unresolved collation.  But we
		 * might as well code this to be a complete consistency check.
		 *
		 * 当且仅当类型可排序时，才应有排序规则。这里预期的失败只有：（1）对不
		 * 可排序类型使用了 COLLATE，
		 * 或（2）索引表达式的排序规则未能解析。不妨把这里写成完整的一致性检查。
		 */
		if (type_is_collatable(atttype))
		{
			if (!OidIsValid(attcollation))
				ereport(ERROR,
						(errcode(ERRCODE_INDETERMINATE_COLLATION),
						 errmsg("could not determine which collation to use for index expression"),
						 errhint("Use the COLLATE clause to set the collation explicitly.")));
		}
		else
		{
			if (OidIsValid(attcollation))
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("collations are not supported by type %s",
								format_type_be(atttype))));
		}

		collationOids[attn] = attcollation;

		/*
		 * Identify the opclass to use.  Use of ddl_userid is necessary due to
		 * ACL checks therein.  This is safe despite opclasses containing
		 * opaque expressions (specifically, functions), because only
		 * superusers can define opclasses.
		 *
		 * 确定要使用的操作符类。必须使用 ddl_userid，因为其中有 ACL 检查。尽
		 * 管操作符类含有不透明表达式
		 * （具体说是函数），这仍然安全，因为只有超级用户才能定义操作符类。
		 */
		if (OidIsValid(ddl_userid))
		{
			AtEOXact_GUC(false, *ddl_save_nestlevel);
			SetUserIdAndSecContext(ddl_userid, ddl_sec_context);
		}
		opclassOids[attn] = ResolveOpClass(attribute->opclass,
										   atttype,
										   accessMethodName,
										   accessMethodId);
		if (OidIsValid(ddl_userid))
		{
			SetUserIdAndSecContext(save_userid, save_sec_context);
			*ddl_save_nestlevel = NewGUCNestLevel();
			RestrictSearchPath();
		}

		/*
		 * Identify the exclusion operator, if any.
		 *
		 * 若有排他操作符，则确定它。
		 */
		if (nextExclOp)
		{
			List	   *opname = (List *) lfirst(nextExclOp);
			Oid			opid;
			Oid			opfamily;
			int			strat;

			/*
			 * Find the operator --- it must accept the column datatype
			 * without runtime coercion (but binary compatibility is OK).
			 * Operators contain opaque expressions (specifically, functions).
			 * compatible_oper_opid() boils down to oper() and
			 * IsBinaryCoercible().  PostgreSQL would have security problems
			 * elsewhere if oper() started calling opaque expressions.
			 *
			 * 查找操作符——它必须能接受列的数据类型，且不需要运行时强制转换（
			 * 但二进制兼容是可以的）。
			 * 操作符含有不透明表达式（具体说是函数）。compatible_oper_opid()
			 * 最终归结为 oper() 和 IsBinaryCoercible()。
			 * 若 oper() 开始调用不透明表达式，PostgreSQL 在别处就会有安全问题。
			 */
			if (OidIsValid(ddl_userid))
			{
				AtEOXact_GUC(false, *ddl_save_nestlevel);
				SetUserIdAndSecContext(ddl_userid, ddl_sec_context);
			}
			opid = compatible_oper_opid(opname, atttype, atttype, false);
			if (OidIsValid(ddl_userid))
			{
				SetUserIdAndSecContext(save_userid, save_sec_context);
				*ddl_save_nestlevel = NewGUCNestLevel();
				RestrictSearchPath();
			}

			/*
			 * Only allow commutative operators to be used in exclusion
			 * constraints. If X conflicts with Y, but Y does not conflict
			 * with X, bad things will happen.
			 *
			 * 排他约束只允许使用可交换操作符。若 X 与 Y 冲突但 Y 与 X 不冲突，
			 * 就会出问题。
			 */
			if (get_commutator(opid) != opid)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("operator %s is not commutative",
								format_operator(opid)),
						 errdetail("Only commutative operators can be used in exclusion constraints.")));

			/*
			 * Operator must be a member of the right opfamily, too
			 *
			 * 操作符还必须属于正确的操作符族。
			 */
			opfamily = get_opclass_family(opclassOids[attn]);
			strat = get_op_opfamily_strategy(opid, opfamily);
			if (strat == 0)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("operator %s is not a member of operator family \"%s\"",
								format_operator(opid),
								get_opfamily_name(opfamily, false)),
						 errdetail("The exclusion operator must be related to the index operator class for the constraint.")));

			indexInfo->ii_ExclusionOps[attn] = opid;
			indexInfo->ii_ExclusionProcs[attn] = get_opcode(opid);
			indexInfo->ii_ExclusionStrats[attn] = strat;
			nextExclOp = lnext(exclusionOpNames, nextExclOp);
		}
		else if (iswithoutoverlaps)
		{
			CompareType cmptype;
			StrategyNumber strat;
			Oid			opid;

			if (attn == nkeycols - 1)
				cmptype = COMPARE_OVERLAP;
			else
				cmptype = COMPARE_EQ;
			GetOperatorFromCompareType(opclassOids[attn], InvalidOid, cmptype, &opid, &strat);
			indexInfo->ii_ExclusionOps[attn] = opid;
			indexInfo->ii_ExclusionProcs[attn] = get_opcode(opid);
			indexInfo->ii_ExclusionStrats[attn] = strat;
		}

		/*
		 * Set up the per-column options (indoption field).  For now, this is
		 * zero for any un-ordered index, while ordered indexes have DESC and
		 * NULLS FIRST/LAST options.
		 *
		 * 设置逐列选项（indoption 字段）。目前，无序索引这里为零；有序索引则
		 * 有 DESC 以及 NULLS FIRST/LAST 选项。
		 */
		colOptions[attn] = 0;
		if (amcanorder)
		{
			/* default ordering is ASC */
			/*
			 *
			 * 默认排序为 ASC。
			 */
			if (attribute->ordering == SORTBY_DESC)
				colOptions[attn] |= INDOPTION_DESC;
			/* default null ordering is LAST for ASC, FIRST for DESC */
			/*
			 *
			 * NULL 的默认次序：ASC 为 LAST，DESC 为 FIRST。
			 */
			if (attribute->nulls_ordering == SORTBY_NULLS_DEFAULT)
			{
				if (attribute->ordering == SORTBY_DESC)
					colOptions[attn] |= INDOPTION_NULLS_FIRST;
			}
			else if (attribute->nulls_ordering == SORTBY_NULLS_FIRST)
				colOptions[attn] |= INDOPTION_NULLS_FIRST;
		}
		else
		{
			/* index AM does not support ordering */
			/*
			 *
			 * 索引访问方法不支持排序。
			 */
			if (attribute->ordering != SORTBY_DEFAULT)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("access method \"%s\" does not support ASC/DESC options",
								accessMethodName)));
			if (attribute->nulls_ordering != SORTBY_NULLS_DEFAULT)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("access method \"%s\" does not support NULLS FIRST/LAST options",
								accessMethodName)));
		}

		/* Set up the per-column opclass options (attoptions field). */
		/*
		 *
		 * 设置逐列操作符类选项（attoptions 字段）。
		 */
		if (attribute->opclassopts)
		{
			Assert(attn < nkeycols);

			opclassOptions[attn] =
				transformRelOptions((Datum) 0, attribute->opclassopts,
									NULL, NULL, false, false);
		}
		else
			opclassOptions[attn] = (Datum) 0;

		attn++;
	}
}

/*
 * Resolve possibly-defaulted operator class specification
 *
 * 解析可能使用默认值的操作符类说明。
 *
 * Note: This is used to resolve operator class specifications in index and
 * partition key definitions.
 *
 * 注意：本函数用于解析索引和分区键定义中的操作符类说明。
 */
Oid
ResolveOpClass(const List *opclass, Oid attrType,
			   const char *accessMethodName, Oid accessMethodId)
{
	char	   *schemaname;
	char	   *opcname;
	HeapTuple	tuple;
	Form_pg_opclass opform;
	Oid			opClassId,
				opInputType;

	if (opclass == NIL)
	{
		/* no operator class specified, so find the default */
		/*
		 *
		 * 未指定操作符类，因此查找默认值。
		 */
		opClassId = GetDefaultOpClass(attrType, accessMethodId);
		if (!OidIsValid(opClassId))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("data type %s has no default operator class for access method \"%s\"",
							format_type_be(attrType), accessMethodName),
					 errhint("You must specify an operator class for the index or define a default operator class for the data type.")));
		return opClassId;
	}

	/*
	 * Specific opclass name given, so look up the opclass.
	 *
	 * 给出了具体的操作符类名，因此查找该操作符类。
	 */

	/* deconstruct the name list */
	/*
	 *
	 * 拆解名字列表。
	 */
	DeconstructQualifiedName(opclass, &schemaname, &opcname);

	if (schemaname)
	{
		/* Look in specific schema only */
		/*
		 *
		 * 只在指定模式中查找。
		 */
		Oid			namespaceId;

		namespaceId = LookupExplicitNamespace(schemaname, false);
		tuple = SearchSysCache3(CLAAMNAMENSP,
								ObjectIdGetDatum(accessMethodId),
								PointerGetDatum(opcname),
								ObjectIdGetDatum(namespaceId));
	}
	else
	{
		/* Unqualified opclass name, so search the search path */
		/*
		 *
		 * 操作符类名未限定模式，因此沿搜索路径查找。
		 */
		opClassId = OpclassnameGetOpcid(accessMethodId, opcname);
		if (!OidIsValid(opClassId))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("operator class \"%s\" does not exist for access method \"%s\"",
							opcname, accessMethodName)));
		tuple = SearchSysCache1(CLAOID, ObjectIdGetDatum(opClassId));
	}

	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("operator class \"%s\" does not exist for access method \"%s\"",
						NameListToString(opclass), accessMethodName)));

	/*
	 * Verify that the index operator class accepts this datatype.  Note we
	 * will accept binary compatibility.
	 *
	 * 确认索引操作符类接受该数据类型。注意我们接受二进制兼容。
	 */
	opform = (Form_pg_opclass) GETSTRUCT(tuple);
	opClassId = opform->oid;
	opInputType = opform->opcintype;

	if (!IsBinaryCoercible(attrType, opInputType))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("operator class \"%s\" does not accept data type %s",
						NameListToString(opclass), format_type_be(attrType))));

	ReleaseSysCache(tuple);

	return opClassId;
}

/*
 * GetDefaultOpClass
 *
 * Given the OIDs of a datatype and an access method, find the default
 * operator class, if any.  Returns InvalidOid if there is none.
 *
 * 给定数据类型和访问方法的 OID，查找默认操作符类（若有）。没有则返回
 * InvalidOid。
 */
Oid
GetDefaultOpClass(Oid type_id, Oid am_id)
{
	Oid			result = InvalidOid;
	int			nexact = 0;
	int			ncompatible = 0;
	int			ncompatiblepreferred = 0;
	Relation	rel;
	ScanKeyData skey[1];
	SysScanDesc scan;
	HeapTuple	tup;
	TYPCATEGORY tcategory;

	/* If it's a domain, look at the base type instead */
	/*
	 *
	 * 若是域，则改为查看基类型。
	 */
	type_id = getBaseType(type_id);

	tcategory = TypeCategory(type_id);

	/*
	 * We scan through all the opclasses available for the access method,
	 * looking for one that is marked default and matches the target type
	 * (either exactly or binary-compatibly, but prefer an exact match).
	 *
	 * 扫描该访问方法可用的全部操作符类，寻找标记为默认且与目标类型匹配的项
	 * （精确匹配或二进制兼容，但优先精确匹配）。
	 *
	 * We could find more than one binary-compatible match.  If just one is
	 * for a preferred type, use that one; otherwise we fail, forcing the user
	 * to specify which one he wants.  (The preferred-type special case is a
	 * kluge for varchar: it's binary-compatible to both text and bpchar, so
	 * we need a tiebreaker.)  If we find more than one exact match, then
	 * someone put bogus entries in pg_opclass.
	 *
	 * 可能找到多个二进制兼容的匹配。若其中只有一个对应首选类型，就用它；否则
	 * 失败，强制用户指定想要的那个。
	 * （首选类型这个特例是给 varchar 的权宜之计：它与 text 和 bpchar 都二进制
	 * 兼容，因此需要决胜规则。）
	 * 若找到多个精确匹配，说明有人往 pg_opclass 里放了非法项。
	 */
	rel = table_open(OperatorClassRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_opclass_opcmethod,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(am_id));

	scan = systable_beginscan(rel, OpclassAmNameNspIndexId, true,
							  NULL, 1, skey);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_opclass opclass = (Form_pg_opclass) GETSTRUCT(tup);

		/* ignore altogether if not a default opclass */
		/*
		 *
		 * 若不是默认操作符类则完全忽略。
		 */
		if (!opclass->opcdefault)
			continue;
		if (opclass->opcintype == type_id)
		{
			nexact++;
			result = opclass->oid;
		}
		else if (nexact == 0 &&
				 IsBinaryCoercible(type_id, opclass->opcintype))
		{
			if (IsPreferredType(tcategory, opclass->opcintype))
			{
				ncompatiblepreferred++;
				result = opclass->oid;
			}
			else if (ncompatiblepreferred == 0)
			{
				ncompatible++;
				result = opclass->oid;
			}
		}
	}

	systable_endscan(scan);

	table_close(rel, AccessShareLock);

	/* raise error if pg_opclass contains inconsistent data */
	/*
	 *
	 * 若 pg_opclass 含有不一致的数据则报错。
	 */
	if (nexact > 1)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("there are multiple default operator classes for data type %s",
						format_type_be(type_id))));

	if (nexact == 1 ||
		ncompatiblepreferred == 1 ||
		(ncompatiblepreferred == 0 && ncompatible == 1))
		return result;

	return InvalidOid;
}

/*
 * GetOperatorFromCompareType
 *
 * opclass - the opclass to use
 * rhstype - the type for the right-hand side, or InvalidOid to use the type of the given opclass.
 * cmptype - kind of operator to find
 * opid - holds the operator we found
 * strat - holds the output strategy number
 *
 * opclass：要使用的操作符类。
 * rhstype：右操作数类型；InvalidOid 表示使用给定操作符类的类型。
 * cmptype：要查找的操作符种类。
 * opid：存放找到的操作符。
 * strat：存放输出的策略号。
 *
 * Finds an operator from a CompareType.  This is used for temporal index
 * constraints (and other temporal features) to look up equality and overlaps
 * operators.  We ask an opclass support function to translate from the
 * compare type to the internal strategy numbers.  Raises ERROR on search
 * failure.
 *
 * 根据 CompareType 查找操作符。时间索引约束（以及其他时间特性）用它查找相等和
 * overlaps 操作符。
 * 我们请操作符类的支持函数把比较类型翻译成内部策略号。查找失败时抛出 ERROR。
 */
void
GetOperatorFromCompareType(Oid opclass, Oid rhstype, CompareType cmptype,
						   Oid *opid, StrategyNumber *strat)
{
	Oid			amid;
	Oid			opfamily;
	Oid			opcintype;

	Assert(cmptype == COMPARE_EQ || cmptype == COMPARE_OVERLAP || cmptype == COMPARE_CONTAINED_BY);

	amid = get_opclass_method(opclass);

	*opid = InvalidOid;

	if (get_opclass_opfamily_and_input_type(opclass, &opfamily, &opcintype))
	{
		/*
		 * Ask the index AM to translate to its internal stratnum
		 *
		 * 请索引访问方法把它翻译成内部 stratnum。
		 */
		*strat = IndexAmTranslateCompareType(cmptype, amid, opfamily, true);
		if (*strat == InvalidStrategy)
			ereport(ERROR,
					errcode(ERRCODE_UNDEFINED_OBJECT),
					cmptype == COMPARE_EQ ? errmsg("could not identify an equality operator for type %s", format_type_be(opcintype)) :
					cmptype == COMPARE_OVERLAP ? errmsg("could not identify an overlaps operator for type %s", format_type_be(opcintype)) :
					cmptype == COMPARE_CONTAINED_BY ? errmsg("could not identify a contained-by operator for type %s", format_type_be(opcintype)) : 0,
					errdetail("Could not translate compare type %d for operator family \"%s\" of access method \"%s\".",
							  cmptype, get_opfamily_name(opfamily, false), get_am_name(amid)));

		/*
		 * We parameterize rhstype so foreign keys can ask for a <@ operator
		 * whose rhs matches the aggregate function. For example range_agg
		 * returns anymultirange.
		 *
		 * 把 rhstype 参数化，以便外键可以请求右操作数与聚合函数匹配的包含操作
		 * 符。例如 range_agg 返回 anymultirange。
		 */
		if (!OidIsValid(rhstype))
			rhstype = opcintype;
		*opid = get_opfamily_member(opfamily, opcintype, rhstype, *strat);
	}

	if (!OidIsValid(*opid))
		ereport(ERROR,
				errcode(ERRCODE_UNDEFINED_OBJECT),
				cmptype == COMPARE_EQ ? errmsg("could not identify an equality operator for type %s", format_type_be(opcintype)) :
				cmptype == COMPARE_OVERLAP ? errmsg("could not identify an overlaps operator for type %s", format_type_be(opcintype)) :
				cmptype == COMPARE_CONTAINED_BY ? errmsg("could not identify a contained-by operator for type %s", format_type_be(opcintype)) : 0,
				errdetail("There is no suitable operator in operator family \"%s\" for access method \"%s\".",
						  get_opfamily_name(opfamily, false), get_am_name(amid)));
}

/*
 *	makeObjectName()
 *
 *	Create a name for an implicitly created index, sequence, constraint,
 *	extended statistics, etc.
 *
 *	为隐式创建的索引、序列、约束、扩展统计信息等生成名字。
 *
 *	The parameters are typically: the original table name, the original field
 *	name, and a "type" string (such as "seq" or "pkey").    The field name
 *	and/or type can be NULL if not relevant.
 *
 *	参数通常是：原始表名、原始字段名，以及一个类型字符串（如 "seq" 或 "pkey"）。
 *	字段名和/或类型在无关时可以为 NULL。
 *
 *	The result is a palloc'd string.
 *
 *	结果是 palloc 分配的字符串。
 *
 *	The basic result we want is "name1_name2_label", omitting "_name2" or
 *	"_label" when those parameters are NULL.  However, we must generate
 *	a name with less than NAMEDATALEN characters!  So, we truncate one or
 *	both names if necessary to make a short-enough string.  The label part
 *	is never truncated (so it had better be reasonably short).
 *
 *	基本结果是 "name1_name2_label"；参数为 NULL 时省略 "_name2" 或 "_label"。
 *	但生成的名字必须短于 NAMEDATALEN 个字符，因此必要时截断一个或两个名字以得
 *	到足够短的字符串。
 *	label 部分从不截断（所以它最好相当短）。
 *
 *	The caller is responsible for checking uniqueness of the generated
 *	name and retrying as needed; retrying will be done by altering the
 *	"label" string (which is why we never truncate that part).
 *
 *	调用方负责检查生成的名字是否唯一，并在需要时重试；重试通过改动 label 字符
 *	串完成（这也是该部分从不截断的原因）。
 */
char *
makeObjectName(const char *name1, const char *name2, const char *label)
{
	char	   *name;
	int			overhead = 0;	/* chars needed for label and underscores */
	/*
	 *
	 * label 和下划线所需的字符数。
	 */
	int			availchars;		/* chars available for name(s) */
	/*
	 *
	 * 名字可用的字符数。
	 */
	int			name1chars;		/* chars allocated to name1 */
	/*
	 *
	 * 分配给 name1 的字符数。
	 */
	int			name2chars;		/* chars allocated to name2 */
	/*
	 *
	 * 分配给 name2 的字符数。
	 */
	int			ndx;

	name1chars = strlen(name1);
	if (name2)
	{
		name2chars = strlen(name2);
		overhead++;				/* allow for separating underscore */
		/*
		 *
		 * 为分隔用的下划线留出位置。
		 */
	}
	else
		name2chars = 0;
	if (label)
		overhead += strlen(label) + 1;

	availchars = NAMEDATALEN - 1 - overhead;
	Assert(availchars > 0);		/* else caller chose a bad label */
	/*
	 *
	 * 否则就是调用方选了一个不合适的 label。
	 */

	/*
	 * If we must truncate, preferentially truncate the longer name. This
	 * logic could be expressed without a loop, but it's simple and obvious as
	 * a loop.
	 *
	 * 若必须截断，优先截断较长的名字。这段逻辑可以不用循环表达，但写成循环更
	 * 简单明了。
	 */
	while (name1chars + name2chars > availchars)
	{
		if (name1chars > name2chars)
			name1chars--;
		else
			name2chars--;
	}

	name1chars = pg_mbcliplen(name1, name1chars, name1chars);
	if (name2)
		name2chars = pg_mbcliplen(name2, name2chars, name2chars);

	/* Now construct the string using the chosen lengths */
	/*
	 *
	 * 现在用选定的长度构造字符串。
	 */
	name = palloc(name1chars + name2chars + overhead + 1);
	memcpy(name, name1, name1chars);
	ndx = name1chars;
	if (name2)
	{
		name[ndx++] = '_';
		memcpy(name + ndx, name2, name2chars);
		ndx += name2chars;
	}
	if (label)
	{
		name[ndx++] = '_';
		strcpy(name + ndx, label);
	}
	else
		name[ndx] = '\0';

	return name;
}

/*
 * Select a nonconflicting name for a new relation.  This is ordinarily
 * used to choose index names (which is why it's here) but it can also
 * be used for sequences, or any autogenerated relation kind.
 *
 * 为新关系选择一个不冲突的名字。通常用于选择索引名（所以放在这里），也可用于
 * 序列或任何自动生成的关系种类。
 *
 * name1, name2, and label are used the same way as for makeObjectName(),
 * except that the label can't be NULL; digits will be appended to the label
 * if needed to create a name that is unique within the specified namespace.
 *
 * name1、name2 和 label 的用法与 makeObjectName() 相同，只是 label 不能为
 * NULL；
 * 若需要在指定命名空间内得到唯一名字，会在 label 后追加数字。
 *
 * If isconstraint is true, we also avoid choosing a name matching any
 * existing constraint in the same namespace.  (This is stricter than what
 * Postgres itself requires, but the SQL standard says that constraint names
 * should be unique within schemas, so we follow that for autogenerated
 * constraint names.)
 *
 * 若 isconstraint 为真，还要避免选择与同一命名空间中已有约束同名的名字。
 * （这比 Postgres 自身的要求更严，但 SQL 标准说约束名应在模式内唯一，因此自动
 * 生成的约束名遵循这一点。）
 *
 * Note: it is theoretically possible to get a collision anyway, if someone
 * else chooses the same name concurrently.  We shorten the race condition
 * window by checking for conflicting relations using SnapshotDirty, but
 * that doesn't close the window entirely.  This is fairly unlikely to be
 * a problem in practice, especially if one is holding an exclusive lock on
 * the relation identified by name1.  However, if choosing multiple names
 * within a single command, you'd better create the new object and do
 * CommandCounterIncrement before choosing the next one!
 *
 * 注意：理论上仍可能冲突，若别人同时选择了同一个名字。我们用 SnapshotDirty 检
 * 查冲突关系来缩短竞争窗口，
 * 但窗口并未完全关闭。实践中这相当少见，尤其是当调用方对 name1 所指关系持有排
 * 他锁时。
 * 不过若在一条命令里选择多个名字，最好先创建新对象并做
 * CommandCounterIncrement，再选择下一个。
 *
 * Returns a palloc'd string.
 *
 * 返回 palloc 分配的字符串。
 */
char *
ChooseRelationName(const char *name1, const char *name2,
				   const char *label, Oid namespaceid,
				   bool isconstraint)
{
	int			pass = 0;
	char	   *relname = NULL;
	char		modlabel[NAMEDATALEN];
	SnapshotData SnapshotDirty;
	Relation	pgclassrel;

	/* prepare to search pg_class with a dirty snapshot */
	/*
	 *
	 * 准备用脏快照搜索 pg_class。
	 */
	InitDirtySnapshot(SnapshotDirty);
	pgclassrel = table_open(RelationRelationId, AccessShareLock);

	/* try the unmodified label first */
	/*
	 *
	 * 先尝试未修改的 label。
	 */
	strlcpy(modlabel, label, sizeof(modlabel));

	for (;;)
	{
		ScanKeyData key[2];
		SysScanDesc scan;
		bool		collides;

		relname = makeObjectName(name1, name2, modlabel);

		/* is there any conflicting relation name? */
		/*
		 *
		 * 是否有冲突的关系名？
		 */
		ScanKeyInit(&key[0],
					Anum_pg_class_relname,
					BTEqualStrategyNumber, F_NAMEEQ,
					CStringGetDatum(relname));
		ScanKeyInit(&key[1],
					Anum_pg_class_relnamespace,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(namespaceid));

		scan = systable_beginscan(pgclassrel, ClassNameNspIndexId,
								  true /* indexOK */ ,
								  /*
								   *
								   * 允许使用索引扫描。
								   */
								  &SnapshotDirty,
								  2, key);

		collides = HeapTupleIsValid(systable_getnext(scan));

		systable_endscan(scan);

		/* break out of loop if no conflict */
		/*
		 *
		 * 若无冲突则跳出循环。
		 */
		if (!collides)
		{
			if (!isconstraint ||
				!ConstraintNameExists(relname, namespaceid))
				break;
		}

		/* found a conflict, so try a new name component */
		/*
		 *
		 * 发现冲突，因此尝试新的名字成分。
		 */
		pfree(relname);
		snprintf(modlabel, sizeof(modlabel), "%s%d", label, ++pass);
	}

	table_close(pgclassrel, AccessShareLock);

	return relname;
}

/*
 * Select the name to be used for an index.
 *
 * 选择索引要使用的名字。
 *
 * The argument list is pretty ad-hoc :-(
 *
 * 参数列表相当随意。
 */
static char *
ChooseIndexName(const char *tabname, Oid namespaceId,
				const List *colnames, const List *exclusionOpNames,
				bool primary, bool isconstraint)
{
	char	   *indexname;

	if (primary)
	{
		/* the primary key's name does not depend on the specific column(s) */
		/*
		 *
		 * 主键的名字不依赖于具体的列。
		 */
		indexname = ChooseRelationName(tabname,
									   NULL,
									   "pkey",
									   namespaceId,
									   true);
	}
	else if (exclusionOpNames != NIL)
	{
		indexname = ChooseRelationName(tabname,
									   ChooseIndexNameAddition(colnames),
									   "excl",
									   namespaceId,
									   true);
	}
	else if (isconstraint)
	{
		indexname = ChooseRelationName(tabname,
									   ChooseIndexNameAddition(colnames),
									   "key",
									   namespaceId,
									   true);
	}
	else
	{
		indexname = ChooseRelationName(tabname,
									   ChooseIndexNameAddition(colnames),
									   "idx",
									   namespaceId,
									   false);
	}

	return indexname;
}

/*
 * Generate "name2" for a new index given the list of column names for it
 * (as produced by ChooseIndexColumnNames).  This will be passed to
 * ChooseRelationName along with the parent table name and a suitable label.
 *
 * 根据列名列表（由 ChooseIndexColumnNames 产生）为新索引生成 "name2"。
 * 它会连同父表名和合适的 label 一起传给 ChooseRelationName。
 *
 * We know that less than NAMEDATALEN characters will actually be used,
 * so we can truncate the result once we've generated that many.
 *
 * 我们知道实际使用的字符会少于 NAMEDATALEN，因此生成到这个长度后就可以截断结
 * 果。
 *
 * XXX See also ChooseForeignKeyConstraintNameAddition and
 * ChooseExtendedStatisticNameAddition.
 *
 * XXX：另见 ChooseForeignKeyConstraintNameAddition 和
 * ChooseExtendedStatisticNameAddition。
 */
static char *
ChooseIndexNameAddition(const List *colnames)
{
	char		buf[NAMEDATALEN * 2];
	int			buflen = 0;
	ListCell   *lc;

	buf[0] = '\0';
	foreach(lc, colnames)
	{
		const char *name = (const char *) lfirst(lc);

		if (buflen > 0)
			buf[buflen++] = '_';	/* insert _ between names */
			/*
			 *
			 * 在名字之间插入下划线。
			 */

		/*
		 * At this point we have buflen <= NAMEDATALEN.  name should be less
		 * than NAMEDATALEN already, but use strlcpy for paranoia.
		 *
		 * 此时 buflen 小于等于 NAMEDATALEN。name 应该已经短于 NAMEDATALEN，但
		 * 为稳妥仍使用 strlcpy。
		 */
		strlcpy(buf + buflen, name, NAMEDATALEN);
		buflen += strlen(buf + buflen);
		if (buflen >= NAMEDATALEN)
			break;
	}
	return pstrdup(buf);
}

/*
 * Select the actual names to be used for the columns of an index, given the
 * list of IndexElems for the columns.  This is mostly about ensuring the
 * names are unique so we don't get a conflicting-attribute-names error.
 *
 * 根据列的 IndexElem 列表，选择索引列实际使用的名字。主要是保证名字唯一，以免
 * 出现属性名冲突错误。
 *
 * Returns a List of plain strings (char *, not String nodes).
 *
 * 返回普通字符串（char *，不是 String 节点）的 List。
 */
static List *
ChooseIndexColumnNames(const List *indexElems)
{
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, indexElems)
	{
		IndexElem  *ielem = (IndexElem *) lfirst(lc);
		const char *origname;
		const char *curname;
		int			i;
		char		buf[NAMEDATALEN];

		/* Get the preliminary name from the IndexElem */
		/*
		 *
		 * 从 IndexElem 取得初步名字。
		 */
		if (ielem->indexcolname)
			origname = ielem->indexcolname; /* caller-specified name */
			/*
			 *
			 * 调用方指定的名字。
			 */
		else if (ielem->name)
			origname = ielem->name; /* simple column reference */
			/*
			 *
			 * 简单列引用。
			 */
		else
			origname = "expr";	/* default name for expression */
			/*
			 *
			 * 表达式的默认名字。
			 */

		/* If it conflicts with any previous column, tweak it */
		/*
		 *
		 * 若与前面任一列冲突，则调整它。
		 */
		curname = origname;
		for (i = 1;; i++)
		{
			ListCell   *lc2;
			char		nbuf[32];
			int			nlen;

			foreach(lc2, result)
			{
				if (strcmp(curname, (char *) lfirst(lc2)) == 0)
					break;
			}
			if (lc2 == NULL)
				break;			/* found nonconflicting name */
				/*
				 *
				 * 找到了不冲突的名字。
				 */

			sprintf(nbuf, "%d", i);

			/* Ensure generated names are shorter than NAMEDATALEN */
			/*
			 *
			 * 确保生成的名字短于 NAMEDATALEN。
			 */
			nlen = pg_mbcliplen(origname, strlen(origname),
								NAMEDATALEN - 1 - strlen(nbuf));
			memcpy(buf, origname, nlen);
			strcpy(buf + nlen, nbuf);
			curname = buf;
		}

		/* And attach to the result list */
		/*
		 *
		 * 并挂到结果列表上。
		 */
		result = lappend(result, pstrdup(curname));
	}
	return result;
}

/*
 * ExecReindex
 *
 * Primary entry point for manual REINDEX commands.  This is mainly a
 * preparation wrapper for the real operations that will happen in
 * each subroutine of REINDEX.
 *
 * 手动 REINDEX 命令的主入口。这里主要是准备工作，真正的操作在 REINDEX 的各个
 * 子程序中进行。
 */
void
ExecReindex(ParseState *pstate, const ReindexStmt *stmt, bool isTopLevel)
{
	ReindexParams params = {0};
	ListCell   *lc;
	bool		concurrently = false;
	bool		verbose = false;
	char	   *tablespacename = NULL;

	/* Parse option list */
	/*
	 *
	 * 解析选项列表。
	 */
	foreach(lc, stmt->params)
	{
		DefElem    *opt = (DefElem *) lfirst(lc);

		if (strcmp(opt->defname, "verbose") == 0)
			verbose = defGetBoolean(opt);
		else if (strcmp(opt->defname, "concurrently") == 0)
			concurrently = defGetBoolean(opt);
		else if (strcmp(opt->defname, "tablespace") == 0)
			tablespacename = defGetString(opt);
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized %s option \"%s\"",
							"REINDEX", opt->defname),
					 parser_errposition(pstate, opt->location)));
	}

	if (concurrently)
		PreventInTransactionBlock(isTopLevel,
								  "REINDEX CONCURRENTLY");

	params.options =
		(verbose ? REINDEXOPT_VERBOSE : 0) |
		(concurrently ? REINDEXOPT_CONCURRENTLY : 0);

	/*
	 * Assign the tablespace OID to move indexes to, with InvalidOid to do
	 * nothing.
	 *
	 * 指定要把索引移入的表空间 OID；InvalidOid 表示不做移动。
	 */
	if (tablespacename != NULL)
	{
		params.tablespaceOid = get_tablespace_oid(tablespacename, false);

		/* Check permissions except when moving to database's default */
		/*
		 *
		 * 检查权限，但移到数据库默认表空间时除外。
		 */
		if (OidIsValid(params.tablespaceOid) &&
			params.tablespaceOid != MyDatabaseTableSpace)
		{
			AclResult	aclresult;

			aclresult = object_aclcheck(TableSpaceRelationId, params.tablespaceOid,
										GetUserId(), ACL_CREATE);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, OBJECT_TABLESPACE,
							   get_tablespace_name(params.tablespaceOid));
		}
	}
	else
		params.tablespaceOid = InvalidOid;

	switch (stmt->kind)
	{
		case REINDEX_OBJECT_INDEX:
			ReindexIndex(stmt, &params, isTopLevel);
			break;
		case REINDEX_OBJECT_TABLE:
			ReindexTable(stmt, &params, isTopLevel);
			break;
		case REINDEX_OBJECT_SCHEMA:
		case REINDEX_OBJECT_SYSTEM:
		case REINDEX_OBJECT_DATABASE:

			/*
			 * This cannot run inside a user transaction block; if we were
			 * inside a transaction, then its commit- and
			 * start-transaction-command calls would not have the intended
			 * effect!
			 *
			 * 不能在用户事务块内运行；若已经在事务中，它的提交与开始事务命令
			 * 就不会产生预期效果。
			 */
			PreventInTransactionBlock(isTopLevel,
									  (stmt->kind == REINDEX_OBJECT_SCHEMA) ? "REINDEX SCHEMA" :
									  (stmt->kind == REINDEX_OBJECT_SYSTEM) ? "REINDEX SYSTEM" :
									  "REINDEX DATABASE");
			ReindexMultipleTables(stmt, &params);
			break;
		default:
			elog(ERROR, "unrecognized object type: %d",
				 (int) stmt->kind);
			break;
	}
}

/*
 * ReindexIndex
 *		Recreate a specific index.
 *
 *		重建指定的索引。
 */
static void
ReindexIndex(const ReindexStmt *stmt, const ReindexParams *params, bool isTopLevel)
{
	const RangeVar *indexRelation = stmt->relation;
	struct ReindexIndexCallbackState state;
	Oid			indOid;
	char		persistence;
	char		relkind;

	/*
	 * Find and lock index, and check permissions on table; use callback to
	 * obtain lock on table first, to avoid deadlock hazard.  The lock level
	 * used here must match the index lock obtained in reindex_index().
	 *
	 * 查找并锁定索引，并检查表上的权限；用回调先锁表，以避免死锁风险。这里的
	 * 锁级别必须与 reindex_index() 取得的索引锁一致。
	 *
	 * If it's a temporary index, we will perform a non-concurrent reindex,
	 * even if CONCURRENTLY was requested.  In that case, reindex_index() will
	 * upgrade the lock, but that's OK, because other sessions can't hold
	 * locks on our temporary table.
	 *
	 * 若是临时索引，即使请求了 CONCURRENTLY，也执行非并发重建。那种情况下
	 * reindex_index() 会升级锁，
	 * 但这没问题，因为其他会话不能持有我们临时表上的锁。
	 */
	state.params = *params;
	state.locked_table_oid = InvalidOid;
	indOid = RangeVarGetRelidExtended(indexRelation,
									  (params->options & REINDEXOPT_CONCURRENTLY) != 0 ?
									  ShareUpdateExclusiveLock : AccessExclusiveLock,
									  0,
									  RangeVarCallbackForReindexIndex,
									  &state);

	/*
	 * Obtain the current persistence and kind of the existing index.  We
	 * already hold a lock on the index.
	 *
	 * 取得现有索引当前的持久性和种类。我们已经持有该索引上的锁。
	 */
	persistence = get_rel_persistence(indOid);
	relkind = get_rel_relkind(indOid);

	if (relkind == RELKIND_PARTITIONED_INDEX)
		ReindexPartitions(stmt, indOid, params, isTopLevel);
	else if ((params->options & REINDEXOPT_CONCURRENTLY) != 0 &&
			 persistence != RELPERSISTENCE_TEMP)
		ReindexRelationConcurrently(stmt, indOid, params);
	else
	{
		ReindexParams newparams = *params;

		newparams.options |= REINDEXOPT_REPORT_PROGRESS;
		reindex_index(stmt, indOid, false, persistence, &newparams);
	}
}

/*
 * Check permissions on table before acquiring relation lock; also lock
 * the heap before the RangeVarGetRelidExtended takes the index lock, to avoid
 * deadlocks.
 *
 * 在取得关系锁之前先检查表上的权限；并在 RangeVarGetRelidExtended 取得索引锁
 * 之前先锁堆，以避免死锁。
 */
static void
RangeVarCallbackForReindexIndex(const RangeVar *relation,
								Oid relId, Oid oldRelId, void *arg)
{
	char		relkind;
	struct ReindexIndexCallbackState *state = arg;
	LOCKMODE	table_lockmode;
	Oid			table_oid;

	/*
	 * Lock level here should match table lock in reindex_index() for
	 * non-concurrent case and table locks used by index_concurrently_*() for
	 * concurrent case.
	 *
	 * 这里的锁级别应与非并发情况下 reindex_index() 的表锁，以及并发情况下
	 * index_concurrently_*() 使用的表锁一致。
	 */
	table_lockmode = (state->params.options & REINDEXOPT_CONCURRENTLY) != 0 ?
		ShareUpdateExclusiveLock : ShareLock;

	/*
	 * If we previously locked some other index's heap, and the name we're
	 * looking up no longer refers to that relation, release the now-useless
	 * lock.
	 *
	 * 若先前锁过另一个索引的堆，而正在查找的名字已不再指向那个关系，则释放这
	 * 把现在无用的锁。
	 */
	if (relId != oldRelId && OidIsValid(oldRelId))
	{
		UnlockRelationOid(state->locked_table_oid, table_lockmode);
		state->locked_table_oid = InvalidOid;
	}

	/* If the relation does not exist, there's nothing more to do. */
	/*
	 *
	 * 若关系不存在，就没有更多事情可做。
	 */
	if (!OidIsValid(relId))
		return;

	/*
	 * If the relation does exist, check whether it's an index.  But note that
	 * the relation might have been dropped between the time we did the name
	 * lookup and now.  In that case, there's nothing to do.
	 *
	 * 若关系确实存在，检查它是不是索引。但注意在名字查找和现在之间，关系可能
	 * 已被删除。那种情况下也无事可做。
	 */
	relkind = get_rel_relkind(relId);
	if (!relkind)
		return;
	if (relkind != RELKIND_INDEX &&
		relkind != RELKIND_PARTITIONED_INDEX)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not an index", relation->relname)));

	/* Check permissions */
	/*
	 *
	 * 检查权限。
	 */
	table_oid = IndexGetRelation(relId, true);
	if (OidIsValid(table_oid))
	{
		AclResult	aclresult;

		aclresult = pg_class_aclcheck(table_oid, GetUserId(), ACL_MAINTAIN);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_INDEX, relation->relname);
	}

	/* Lock heap before index to avoid deadlock. */
	/*
	 *
	 * 先锁堆再锁索引，以避免死锁。
	 */
	if (relId != oldRelId)
	{
		/*
		 * If the OID isn't valid, it means the index was concurrently
		 * dropped, which is not a problem for us; just return normally.
		 *
		 * 若 OID 无效，说明索引已被并发删除，这对我们不是问题；正常返回即可。
		 */
		if (OidIsValid(table_oid))
		{
			LockRelationOid(table_oid, table_lockmode);
			state->locked_table_oid = table_oid;
		}
	}
}

/*
 * ReindexTable
 *		Recreate all indexes of a table (and of its toast table, if any)
 *
 *		重建表的全部索引（以及它的 toast 表索引，若有）。
 */
static Oid
ReindexTable(const ReindexStmt *stmt, const ReindexParams *params, bool isTopLevel)
{
	Oid			heapOid;
	bool		result;
	const RangeVar *relation = stmt->relation;

	/*
	 * The lock level used here should match reindex_relation().
	 *
	 * 这里使用的锁级别应与 reindex_relation() 一致。
	 *
	 * If it's a temporary table, we will perform a non-concurrent reindex,
	 * even if CONCURRENTLY was requested.  In that case, reindex_relation()
	 * will upgrade the lock, but that's OK, because other sessions can't hold
	 * locks on our temporary table.
	 *
	 * 若是临时表，即使请求了 CONCURRENTLY，也执行非并发重建。那种情况下
	 * reindex_relation() 会升级锁，
	 * 但这没问题，因为其他会话不能持有我们临时表上的锁。
	 */
	heapOid = RangeVarGetRelidExtended(relation,
									   (params->options & REINDEXOPT_CONCURRENTLY) != 0 ?
									   ShareUpdateExclusiveLock : ShareLock,
									   0,
									   RangeVarCallbackMaintainsTable, NULL);

	if (get_rel_relkind(heapOid) == RELKIND_PARTITIONED_TABLE)
		ReindexPartitions(stmt, heapOid, params, isTopLevel);
	else if ((params->options & REINDEXOPT_CONCURRENTLY) != 0 &&
			 get_rel_persistence(heapOid) != RELPERSISTENCE_TEMP)
	{
		result = ReindexRelationConcurrently(stmt, heapOid, params);

		if (!result)
			ereport(NOTICE,
					(errmsg("table \"%s\" has no indexes that can be reindexed concurrently",
							relation->relname)));
	}
	else
	{
		ReindexParams newparams = *params;

		newparams.options |= REINDEXOPT_REPORT_PROGRESS;
		result = reindex_relation(stmt, heapOid,
								  REINDEX_REL_PROCESS_TOAST |
								  REINDEX_REL_CHECK_CONSTRAINTS,
								  &newparams);
		if (!result)
			ereport(NOTICE,
					(errmsg("table \"%s\" has no indexes to reindex",
							relation->relname)));
	}

	return heapOid;
}

/*
 * ReindexMultipleTables
 *		Recreate indexes of tables selected by objectName/objectKind.
 *
 *		按 objectName/objectKind 选出的表重建索引。
 *
 * To reduce the probability of deadlocks, each table is reindexed in a
 * separate transaction, so we can release the lock on it right away.
 * That means this must not be called within a user transaction block!
 *
 * 为降低死锁概率，每张表在单独的事务中重建，以便马上释放锁。因此不能在用户事
 * 务块内调用本函数。
 */
static void
ReindexMultipleTables(const ReindexStmt *stmt, const ReindexParams *params)
{

	Oid			objectOid;
	Relation	relationRelation;
	TableScanDesc scan;
	ScanKeyData scan_keys[1];
	HeapTuple	tuple;
	MemoryContext private_context;
	MemoryContext old;
	List	   *relids = NIL;
	int			num_keys;
	bool		concurrent_warning = false;
	bool		tablespace_warning = false;
	const char *objectName = stmt->name;
	const ReindexObjectType objectKind = stmt->kind;

	Assert(objectKind == REINDEX_OBJECT_SCHEMA ||
		   objectKind == REINDEX_OBJECT_SYSTEM ||
		   objectKind == REINDEX_OBJECT_DATABASE);

	/*
	 * This matches the options enforced by the grammar, where the object name
	 * is optional for DATABASE and SYSTEM.
	 *
	 * 这与语法所强制的选项一致：对 DATABASE 和 SYSTEM，对象名是可选的。
	 */
	Assert(objectName || objectKind != REINDEX_OBJECT_SCHEMA);

	if (objectKind == REINDEX_OBJECT_SYSTEM &&
		(params->options & REINDEXOPT_CONCURRENTLY) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot reindex system catalogs concurrently")));

	/*
	 * Get OID of object to reindex, being the database currently being used
	 * by session for a database or for system catalogs, or the schema defined
	 * by caller. At the same time do permission checks that need different
	 * processing depending on the object type.
	 *
	 * 取得要重建索引的对象 OID：对数据库或系统目录，是会话当前使用的数据库；
	 * 对模式，是调用方指定的模式。
	 * 同时做因对象类型而不同的权限检查。
	 */
	if (objectKind == REINDEX_OBJECT_SCHEMA)
	{
		objectOid = get_namespace_oid(objectName, false);

		if (!object_ownercheck(NamespaceRelationId, objectOid, GetUserId()) &&
			!has_privs_of_role(GetUserId(), ROLE_PG_MAINTAIN))
			aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_SCHEMA,
						   objectName);
	}
	else
	{
		objectOid = MyDatabaseId;

		if (objectName && strcmp(objectName, get_database_name(objectOid)) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("can only reindex the currently open database")));
		if (!object_ownercheck(DatabaseRelationId, objectOid, GetUserId()) &&
			!has_privs_of_role(GetUserId(), ROLE_PG_MAINTAIN))
			aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_DATABASE,
						   get_database_name(objectOid));
	}

	/*
	 * Create a memory context that will survive forced transaction commits we
	 * do below.  Since it is a child of PortalContext, it will go away
	 * eventually even if we suffer an error; there's no need for special
	 * abort cleanup logic.
	 *
	 * 创建一个能在下面强制事务提交后仍然存活的内存上下文。因为它是
	 * PortalContext 的子上下文，
	 * 即使出错最终也会消失，不需要专门的中止清理逻辑。
	 */
	private_context = AllocSetContextCreate(PortalContext,
											"ReindexMultipleTables",
											ALLOCSET_SMALL_SIZES);

	/*
	 * Define the search keys to find the objects to reindex. For a schema, we
	 * select target relations using relnamespace, something not necessary for
	 * a database-wide operation.
	 *
	 * 定义查找待重建对象的搜索键。对模式，用 relnamespace 选择目标关系；数据
	 * 库范围的操作则不需要。
	 */
	if (objectKind == REINDEX_OBJECT_SCHEMA)
	{
		num_keys = 1;
		ScanKeyInit(&scan_keys[0],
					Anum_pg_class_relnamespace,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(objectOid));
	}
	else
		num_keys = 0;

	/*
	 * Scan pg_class to build a list of the relations we need to reindex.
	 *
	 * 扫描 pg_class，建立需要重建索引的关系列表。
	 *
	 * We only consider plain relations and materialized views here (toast
	 * rels will be processed indirectly by reindex_relation).
	 *
	 * 这里只考虑普通关系和物化视图（toast 关系会由 reindex_relation 间接处理）。
	 */
	relationRelation = table_open(RelationRelationId, AccessShareLock);
	scan = table_beginscan_catalog(relationRelation, num_keys, scan_keys);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_class classtuple = (Form_pg_class) GETSTRUCT(tuple);
		Oid			relid = classtuple->oid;

		/*
		 * Only regular tables and matviews can have indexes, so ignore any
		 * other kind of relation.
		 *
		 * 只有普通表和物化视图才能有索引，因此忽略其他种类的关系。
		 *
		 * Partitioned tables/indexes are skipped but matching leaf partitions
		 * are processed.
		 *
		 * 跳过分区表和分区索引，但处理匹配的叶子分区。
		 */
		if (classtuple->relkind != RELKIND_RELATION &&
			classtuple->relkind != RELKIND_MATVIEW)
			continue;

		/* Skip temp tables of other backends; we can't reindex them at all */
		/*
		 *
		 * 跳过其他后端的临时表；我们根本无法重建它们的索引。
		 */
		if (classtuple->relpersistence == RELPERSISTENCE_TEMP &&
			!isTempNamespace(classtuple->relnamespace))
			continue;

		/*
		 * Check user/system classification.  SYSTEM processes all the
		 * catalogs, and DATABASE processes everything that's not a catalog.
		 *
		 * 检查用户/系统分类。SYSTEM 处理全部目录，DATABASE 处理一切非目录对象。
		 */
		if (objectKind == REINDEX_OBJECT_SYSTEM &&
			!IsCatalogRelationOid(relid))
			continue;
		else if (objectKind == REINDEX_OBJECT_DATABASE &&
				 IsCatalogRelationOid(relid))
			continue;

		/*
		 * We already checked privileges on the database or schema, but we
		 * further restrict reindexing shared catalogs to roles with the
		 * MAINTAIN privilege on the relation.
		 *
		 * 已经检查过数据库或模式上的权限，但进一步把共享目录的重建限制为对该
		 * 关系拥有 MAINTAIN 权限的角色。
		 */
		if (classtuple->relisshared &&
			pg_class_aclcheck(relid, GetUserId(), ACL_MAINTAIN) != ACLCHECK_OK)
			continue;

		/*
		 * Skip system tables, since index_create() would reject indexing them
		 * concurrently (and it would likely fail if we tried).
		 *
		 * 跳过系统表，因为 index_create() 会拒绝并发为它们建索引（若尝试也多
		 * 半会失败）。
		 */
		if ((params->options & REINDEXOPT_CONCURRENTLY) != 0 &&
			IsCatalogRelationOid(relid))
		{
			if (!concurrent_warning)
				ereport(WARNING,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot reindex system catalogs concurrently, skipping all")));
			concurrent_warning = true;
			continue;
		}

		/*
		 * If a new tablespace is set, check if this relation has to be
		 * skipped.
		 *
		 * 若设置了新表空间，检查该关系是否必须跳过。
		 */
		if (OidIsValid(params->tablespaceOid))
		{
			bool		skip_rel = false;

			/*
			 * Mapped relations cannot be moved to different tablespaces (in
			 * particular this eliminates all shared catalogs.).
			 *
			 * 映射关系不能移动到其他表空间（这尤其排除了全部共享目录）。
			 */
			if (RELKIND_HAS_STORAGE(classtuple->relkind) &&
				!RelFileNumberIsValid(classtuple->relfilenode))
				skip_rel = true;

			/*
			 * A system relation is always skipped, even with
			 * allow_system_table_mods enabled.
			 *
			 * 系统关系总是被跳过，即使启用了 allow_system_table_mods。
			 */
			if (IsSystemClass(relid, classtuple))
				skip_rel = true;

			if (skip_rel)
			{
				if (!tablespace_warning)
					ereport(WARNING,
							(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
							 errmsg("cannot move system relations, skipping all")));
				tablespace_warning = true;
				continue;
			}
		}

		/* Save the list of relation OIDs in private context */
		/*
		 *
		 * 把关系 OID 列表保存到私有上下文。
		 */
		old = MemoryContextSwitchTo(private_context);

		/*
		 * We always want to reindex pg_class first if it's selected to be
		 * reindexed.  This ensures that if there is any corruption in
		 * pg_class' indexes, they will be fixed before we process any other
		 * tables.  This is critical because reindexing itself will try to
		 * update pg_class.
		 *
		 * 若选中了 pg_class，总是最先重建它的索引。这样若 pg_class 的索引有损
		 * 坏，会在处理其他表之前修好。
		 * 这一点很关键，因为重建索引本身就会尝试更新 pg_class。
		 */
		if (relid == RelationRelationId)
			relids = lcons_oid(relid, relids);
		else
			relids = lappend_oid(relids, relid);

		MemoryContextSwitchTo(old);
	}
	table_endscan(scan);
	table_close(relationRelation, AccessShareLock);

	/*
	 * Process each relation listed in a separate transaction.  Note that this
	 * commits and then starts a new transaction immediately.
	 *
	 * 在单独的事务中处理列出的每个关系。注意这里会提交，然后立刻开始一个新事
	 * 务。
	 */
	ReindexMultipleInternal(stmt, relids, params);

	MemoryContextDelete(private_context);
}

/*
 * Error callback specific to ReindexPartitions().
 *
 * 专用于 ReindexPartitions() 的错误回调。
 */
static void
reindex_error_callback(void *arg)
{
	ReindexErrorInfo *errinfo = (ReindexErrorInfo *) arg;

	Assert(RELKIND_HAS_PARTITIONS(errinfo->relkind));

	if (errinfo->relkind == RELKIND_PARTITIONED_TABLE)
		errcontext("while reindexing partitioned table \"%s.%s\"",
				   errinfo->relnamespace, errinfo->relname);
	else if (errinfo->relkind == RELKIND_PARTITIONED_INDEX)
		errcontext("while reindexing partitioned index \"%s.%s\"",
				   errinfo->relnamespace, errinfo->relname);
}

/*
 * ReindexPartitions
 *
 * Reindex a set of partitions, per the partitioned index or table given
 * by the caller.
 *
 * 按调用方给出的分区索引或分区表，重建一组分区的索引。
 */
static void
ReindexPartitions(const ReindexStmt *stmt, Oid relid, const ReindexParams *params, bool isTopLevel)
{
	List	   *partitions = NIL;
	char		relkind = get_rel_relkind(relid);
	char	   *relname = get_rel_name(relid);
	char	   *relnamespace = get_namespace_name(get_rel_namespace(relid));
	MemoryContext reindex_context;
	List	   *inhoids;
	ListCell   *lc;
	ErrorContextCallback errcallback;
	ReindexErrorInfo errinfo;

	Assert(RELKIND_HAS_PARTITIONS(relkind));

	/*
	 * Check if this runs in a transaction block, with an error callback to
	 * provide more context under which a problem happens.
	 *
	 * 检查是否运行在事务块中，并用错误回调提供问题发生时的更多上下文。
	 */
	errinfo.relname = pstrdup(relname);
	errinfo.relnamespace = pstrdup(relnamespace);
	errinfo.relkind = relkind;
	errcallback.callback = reindex_error_callback;
	errcallback.arg = &errinfo;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	PreventInTransactionBlock(isTopLevel,
							  relkind == RELKIND_PARTITIONED_TABLE ?
							  "REINDEX TABLE" : "REINDEX INDEX");

	/* Pop the error context stack */
	/*
	 *
	 * 弹出错误上下文栈。
	 */
	error_context_stack = errcallback.previous;

	/*
	 * Create special memory context for cross-transaction storage.
	 *
	 * 为跨事务存储创建专用内存上下文。
	 *
	 * Since it is a child of PortalContext, it will go away eventually even
	 * if we suffer an error so there is no need for special abort cleanup
	 * logic.
	 *
	 * 因为它是 PortalContext 的子上下文，即使出错最终也会消失，不需要专门的中
	 * 止清理逻辑。
	 */
	reindex_context = AllocSetContextCreate(PortalContext, "Reindex",
											ALLOCSET_DEFAULT_SIZES);

	/* ShareLock is enough to prevent schema modifications */
	/*
	 *
	 * ShareLock 足以防止模式修改。
	 */
	inhoids = find_all_inheritors(relid, ShareLock, NULL);

	/*
	 * The list of relations to reindex are the physical partitions of the
	 * tree so discard any partitioned table or index.
	 *
	 * 待重建索引的关系列表是该树的物理分区，因此丢弃任何分区表或分区索引。
	 */
	foreach(lc, inhoids)
	{
		Oid			partoid = lfirst_oid(lc);
		char		partkind = get_rel_relkind(partoid);
		MemoryContext old_context;

		/*
		 * This discards partitioned tables, partitioned indexes and foreign
		 * tables.
		 *
		 * 这里丢弃分区表、分区索引和外部表。
		 */
		if (!RELKIND_HAS_STORAGE(partkind))
			continue;

		Assert(partkind == RELKIND_INDEX ||
			   partkind == RELKIND_RELATION);

		/* Save partition OID */
		/*
		 *
		 * 保存分区 OID。
		 */
		old_context = MemoryContextSwitchTo(reindex_context);
		partitions = lappend_oid(partitions, partoid);
		MemoryContextSwitchTo(old_context);
	}

	/*
	 * Process each partition listed in a separate transaction.  Note that
	 * this commits and then starts a new transaction immediately.
	 *
	 * 在单独的事务中处理列出的每个分区。注意这里会提交，然后立刻开始一个新事
	 * 务。
	 */
	ReindexMultipleInternal(stmt, partitions, params);

	/*
	 * Clean up working storage --- note we must do this after
	 * StartTransactionCommand, else we might be trying to delete the active
	 * context!
	 *
	 * 清理工作存储——必须在 StartTransactionCommand 之后做，否则可能试图删除当
	 * 前活动的上下文。
	 */
	MemoryContextDelete(reindex_context);
}

/*
 * ReindexMultipleInternal
 *
 * Reindex a list of relations, each one being processed in its own
 * transaction.  This commits the existing transaction immediately,
 * and starts a new transaction when finished.
 *
 * 重建一组关系的索引，每个关系在自己的事务中处理。这会立刻提交现有事务，并在
 * 结束时开始一个新事务。
 */
static void
ReindexMultipleInternal(const ReindexStmt *stmt, const List *relids, const ReindexParams *params)
{
	ListCell   *l;

	PopActiveSnapshot();
	CommitTransactionCommand();

	foreach(l, relids)
	{
		Oid			relid = lfirst_oid(l);
		char		relkind;
		char		relpersistence;

		StartTransactionCommand();

		/* functions in indexes may want a snapshot set */
		/*
		 *
		 * 索引中的函数可能需要已设置的快照。
		 */
		PushActiveSnapshot(GetTransactionSnapshot());

		/* check if the relation still exists */
		/*
		 *
		 * 检查关系是否仍然存在。
		 */
		if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(relid)))
		{
			PopActiveSnapshot();
			CommitTransactionCommand();
			continue;
		}

		/*
		 * Check permissions except when moving to database's default if a new
		 * tablespace is chosen.  Note that this check also happens in
		 * ExecReindex(), but we do an extra check here as this runs across
		 * multiple transactions.
		 *
		 * 若选择了新表空间，则检查权限，但移到数据库默认表空间时除外。注意
		 * ExecReindex() 里也有这项检查，
		 * 这里再做一次，因为本函数跨越多个事务。
		 */
		if (OidIsValid(params->tablespaceOid) &&
			params->tablespaceOid != MyDatabaseTableSpace)
		{
			AclResult	aclresult;

			aclresult = object_aclcheck(TableSpaceRelationId, params->tablespaceOid,
										GetUserId(), ACL_CREATE);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, OBJECT_TABLESPACE,
							   get_tablespace_name(params->tablespaceOid));
		}

		relkind = get_rel_relkind(relid);
		relpersistence = get_rel_persistence(relid);

		/*
		 * Partitioned tables and indexes can never be processed directly, and
		 * a list of their leaves should be built first.
		 *
		 * 分区表和分区索引永远不能直接处理，应先建立它们的叶子列表。
		 */
		Assert(!RELKIND_HAS_PARTITIONS(relkind));

		if ((params->options & REINDEXOPT_CONCURRENTLY) != 0 &&
			relpersistence != RELPERSISTENCE_TEMP)
		{
			ReindexParams newparams = *params;

			newparams.options |= REINDEXOPT_MISSING_OK;
			(void) ReindexRelationConcurrently(stmt, relid, &newparams);
			if (ActiveSnapshotSet())
				PopActiveSnapshot();
			/* ReindexRelationConcurrently() does the verbose output */
			/*
			 *
			 * 详细输出由 ReindexRelationConcurrently() 完成。
			 */
		}
		else if (relkind == RELKIND_INDEX)
		{
			ReindexParams newparams = *params;

			newparams.options |=
				REINDEXOPT_REPORT_PROGRESS | REINDEXOPT_MISSING_OK;
			reindex_index(stmt, relid, false, relpersistence, &newparams);
			PopActiveSnapshot();
			/* reindex_index() does the verbose output */
			/*
			 *
			 * 详细输出由 reindex_index() 完成。
			 */
		}
		else
		{
			bool		result;
			ReindexParams newparams = *params;

			newparams.options |=
				REINDEXOPT_REPORT_PROGRESS | REINDEXOPT_MISSING_OK;
			result = reindex_relation(stmt, relid,
									  REINDEX_REL_PROCESS_TOAST |
									  REINDEX_REL_CHECK_CONSTRAINTS,
									  &newparams);

			if (result && (params->options & REINDEXOPT_VERBOSE) != 0)
				ereport(INFO,
						(errmsg("table \"%s.%s\" was reindexed",
								get_namespace_name(get_rel_namespace(relid)),
								get_rel_name(relid))));

			PopActiveSnapshot();
		}

		CommitTransactionCommand();
	}

	StartTransactionCommand();
}


/*
 * ReindexRelationConcurrently - process REINDEX CONCURRENTLY for given
 * relation OID
 *
 * 对给定的关系 OID 执行 REINDEX CONCURRENTLY。
 *
 * 'relationOid' can either belong to an index, a table or a materialized
 * view.  For tables and materialized views, all its indexes will be rebuilt,
 * excluding invalid indexes and any indexes used in exclusion constraints,
 * but including its associated toast table indexes.  For indexes, the index
 * itself will be rebuilt.
 *
 * 'relationOid' 可以属于索引、表或物化视图。对表和物化视图，将重建其全部索引，
 * 排除无效索引和用于排他约束的索引，
 * 但包括关联 toast 表的索引。对索引，则重建该索引本身。
 *
 * The locks taken on parent tables and involved indexes are kept until the
 * transaction is committed, at which point a session lock is taken on each
 * relation.  Both of these protect against concurrent schema changes.
 *
 * 父表和相关索引上取得的锁会保持到事务提交，届时再对每个关系取得会话锁。两者
 * 都防止并发的模式变更。
 *
 * Returns true if any indexes have been rebuilt (including toast table's
 * indexes, when relevant), otherwise returns false.
 *
 * 若有任何索引被重建（在相关时包括 toast 表的索引）则返回真，否则返回假。
 *
 * NOTE: This cannot be used on temporary relations.  A concurrent build would
 * cause issues with ON COMMIT actions triggered by the transactions of the
 * concurrent build.  Temporary relations are not subject to concurrent
 * concerns, so there's no need for the more complicated concurrent build,
 * anyway, and a non-concurrent reindex is more efficient.
 *
 * 注意：不能用于临时关系。并发构建会与并发构建事务所触发的 ON COMMIT 动作冲突。
 * 临时关系本来就没有并发方面的顾虑，因此不需要更复杂的并发构建，而非并发重建
 * 更高效。
 */
static bool
ReindexRelationConcurrently(const ReindexStmt *stmt, Oid relationOid, const ReindexParams *params)
{
	typedef struct ReindexIndexInfo
	{
		Oid			indexId;
		Oid			tableId;
		Oid			amId;
		bool		safe;		/* for set_indexsafe_procflags */
		/*
		 *
		 * 供 set_indexsafe_procflags 使用。
		 */
	} ReindexIndexInfo;
	List	   *heapRelationIds = NIL;
	List	   *indexIds = NIL;
	List	   *newIndexIds = NIL;
	List	   *relationLocks = NIL;
	List	   *lockTags = NIL;
	ListCell   *lc,
			   *lc2;
	MemoryContext private_context;
	MemoryContext oldcontext;
	char		relkind;
	char	   *relationName = NULL;
	char	   *relationNamespace = NULL;
	PGRUsage	ru0;
	const int	progress_index[] = {
		PROGRESS_CREATEIDX_COMMAND,
		PROGRESS_CREATEIDX_PHASE,
		PROGRESS_CREATEIDX_INDEX_OID,
		PROGRESS_CREATEIDX_ACCESS_METHOD_OID
	};
	int64		progress_vals[4];

	/*
	 * Create a memory context that will survive forced transaction commits we
	 * do below.  Since it is a child of PortalContext, it will go away
	 * eventually even if we suffer an error; there's no need for special
	 * abort cleanup logic.
	 *
	 * 创建一个能在下面强制事务提交后仍然存活的内存上下文。因为它是
	 * PortalContext 的子上下文，
	 * 即使出错最终也会消失，不需要专门的中止清理逻辑。
	 */
	private_context = AllocSetContextCreate(PortalContext,
											"ReindexConcurrent",
											ALLOCSET_SMALL_SIZES);

	if ((params->options & REINDEXOPT_VERBOSE) != 0)
	{
		/* Save data needed by REINDEX VERBOSE in private context */
		/*
		 *
		 * 把 REINDEX VERBOSE 所需的数据保存到私有上下文。
		 */
		oldcontext = MemoryContextSwitchTo(private_context);

		relationName = get_rel_name(relationOid);
		relationNamespace = get_namespace_name(get_rel_namespace(relationOid));

		pg_rusage_init(&ru0);

		MemoryContextSwitchTo(oldcontext);
	}

	relkind = get_rel_relkind(relationOid);

	/*
	 * Extract the list of indexes that are going to be rebuilt based on the
	 * relation Oid given by caller.
	 *
	 * 根据调用方给出的关系 OID，提取将要重建的索引列表。
	 */
	switch (relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_MATVIEW:
		case RELKIND_TOASTVALUE:
			{
				/*
				 * In the case of a relation, find all its indexes including
				 * toast indexes.
				 *
				 * 若是关系，则找出它的全部索引，包括 toast 索引。
				 */
				Relation	heapRelation;

				/* Save the list of relation OIDs in private context */
				/*
				 *
				 * 把关系 OID 列表保存到私有上下文。
				 */
				oldcontext = MemoryContextSwitchTo(private_context);

				/* Track this relation for session locks */
				/*
				 *
				 * 记录该关系以便加会话锁。
				 */
				heapRelationIds = lappend_oid(heapRelationIds, relationOid);

				MemoryContextSwitchTo(oldcontext);

				if (IsCatalogRelationOid(relationOid))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot reindex system catalogs concurrently")));

				/* Open relation to get its indexes */
				/*
				 *
				 * 打开关系以取得它的索引。
				 */
				if ((params->options & REINDEXOPT_MISSING_OK) != 0)
				{
					heapRelation = try_table_open(relationOid,
												  ShareUpdateExclusiveLock);
					/* leave if relation does not exist */
					/*
					 *
					 * 若关系不存在则离开。
					 */
					if (!heapRelation)
						break;
				}
				else
					heapRelation = table_open(relationOid,
											  ShareUpdateExclusiveLock);

				if (OidIsValid(params->tablespaceOid) &&
					IsSystemRelation(heapRelation))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot move system relation \"%s\"",
									RelationGetRelationName(heapRelation))));

				/* Add all the valid indexes of relation to list */
				/*
				 *
				 * 把该关系的全部有效索引加入列表。
				 */
				foreach(lc, RelationGetIndexList(heapRelation))
				{
					Oid			cellOid = lfirst_oid(lc);
					Relation	indexRelation = index_open(cellOid,
														   ShareUpdateExclusiveLock);

					if (!indexRelation->rd_index->indisvalid)
						ereport(WARNING,
								(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
								 errmsg("skipping reindex of invalid index \"%s.%s\"",
										get_namespace_name(get_rel_namespace(cellOid)),
										get_rel_name(cellOid)),
								 errhint("Use DROP INDEX or REINDEX INDEX.")));
					else if (indexRelation->rd_index->indisexclusion)
						ereport(WARNING,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("cannot reindex exclusion constraint index \"%s.%s\" concurrently, skipping",
										get_namespace_name(get_rel_namespace(cellOid)),
										get_rel_name(cellOid))));
					else
					{
						ReindexIndexInfo *idx;

						/* Save the list of relation OIDs in private context */
						/*
						 *
						 * 把关系 OID 列表保存到私有上下文。
						 */
						oldcontext = MemoryContextSwitchTo(private_context);

						idx = palloc_object(ReindexIndexInfo);
						idx->indexId = cellOid;
						/* other fields set later */
						/*
						 *
						 * 其他字段稍后设置。
						 */

						indexIds = lappend(indexIds, idx);

						MemoryContextSwitchTo(oldcontext);
					}

					index_close(indexRelation, NoLock);
				}

				/* Also add the toast indexes */
				/*
				 *
				 * 同时加入 toast 索引。
				 */
				if (OidIsValid(heapRelation->rd_rel->reltoastrelid))
				{
					Oid			toastOid = heapRelation->rd_rel->reltoastrelid;
					Relation	toastRelation = table_open(toastOid,
														   ShareUpdateExclusiveLock);

					/* Save the list of relation OIDs in private context */
					/*
					 *
					 * 把关系 OID 列表保存到私有上下文。
					 */
					oldcontext = MemoryContextSwitchTo(private_context);

					/* Track this relation for session locks */
					/*
					 *
					 * 记录该关系以便加会话锁。
					 */
					heapRelationIds = lappend_oid(heapRelationIds, toastOid);

					MemoryContextSwitchTo(oldcontext);

					foreach(lc2, RelationGetIndexList(toastRelation))
					{
						Oid			cellOid = lfirst_oid(lc2);
						Relation	indexRelation = index_open(cellOid,
															   ShareUpdateExclusiveLock);

						if (!indexRelation->rd_index->indisvalid)
							ereport(WARNING,
									(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
									 errmsg("skipping reindex of invalid index \"%s.%s\"",
											get_namespace_name(get_rel_namespace(cellOid)),
											get_rel_name(cellOid)),
									 errhint("Use DROP INDEX or REINDEX INDEX.")));
						else
						{
							ReindexIndexInfo *idx;

							/*
							 * Save the list of relation OIDs in private
							 * context
							 *
							 * 把关系 OID 列表保存到私有上下文。
							 */
							oldcontext = MemoryContextSwitchTo(private_context);

							idx = palloc_object(ReindexIndexInfo);
							idx->indexId = cellOid;
							indexIds = lappend(indexIds, idx);
							/* other fields set later */
							/*
							 *
							 * 其他字段稍后设置。
							 */

							MemoryContextSwitchTo(oldcontext);
						}

						index_close(indexRelation, NoLock);
					}

					table_close(toastRelation, NoLock);
				}

				table_close(heapRelation, NoLock);
				break;
			}
		case RELKIND_INDEX:
			{
				Oid			heapId = IndexGetRelation(relationOid,
													  (params->options & REINDEXOPT_MISSING_OK) != 0);
				Relation	heapRelation;
				ReindexIndexInfo *idx;

				/* if relation is missing, leave */
				/*
				 *
				 * 若关系缺失则离开。
				 */
				if (!OidIsValid(heapId))
					break;

				if (IsCatalogRelationOid(heapId))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot reindex system catalogs concurrently")));

				/*
				 * Don't allow reindex for an invalid index on TOAST table, as
				 * if rebuilt it would not be possible to drop it.  Match
				 * error message in reindex_index().
				 *
				 * 不允许重建 TOAST 表上的无效索引，因为重建之后将无法删除它。
				 * 错误信息与 reindex_index() 保持一致。
				 */
				if (IsToastNamespace(get_rel_namespace(relationOid)) &&
					!get_index_isvalid(relationOid))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot reindex invalid index on TOAST table")));

				/*
				 * Check if parent relation can be locked and if it exists,
				 * this needs to be done at this stage as the list of indexes
				 * to rebuild is not complete yet, and REINDEXOPT_MISSING_OK
				 * should not be used once all the session locks are taken.
				 *
				 * 检查能否锁定父关系以及它是否存在。必须在此阶段做，因为待重
				 * 建索引的列表尚未完整，
				 * 而一旦取得全部会话锁，就不应再使用 REINDEXOPT_MISSING_OK。
				 */
				if ((params->options & REINDEXOPT_MISSING_OK) != 0)
				{
					heapRelation = try_table_open(heapId,
												  ShareUpdateExclusiveLock);
					/* leave if relation does not exist */
					/*
					 *
					 * 若关系不存在则离开。
					 */
					if (!heapRelation)
						break;
				}
				else
					heapRelation = table_open(heapId,
											  ShareUpdateExclusiveLock);

				if (OidIsValid(params->tablespaceOid) &&
					IsSystemRelation(heapRelation))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot move system relation \"%s\"",
									get_rel_name(relationOid))));

				table_close(heapRelation, NoLock);

				/* Save the list of relation OIDs in private context */
				/*
				 *
				 * 把关系 OID 列表保存到私有上下文。
				 */
				oldcontext = MemoryContextSwitchTo(private_context);

				/* Track the heap relation of this index for session locks */
				/*
				 *
				 * 记录该索引的堆关系以便加会话锁。
				 */
				heapRelationIds = list_make1_oid(heapId);

				/*
				 * Save the list of relation OIDs in private context.  Note
				 * that invalid indexes are allowed here.
				 *
				 * 把关系 OID 列表保存到私有上下文。注意这里允许无效索引。
				 */
				idx = palloc_object(ReindexIndexInfo);
				idx->indexId = relationOid;
				indexIds = lappend(indexIds, idx);
				/* other fields set later */
				/*
				 *
				 * 其他字段稍后设置。
				 */

				MemoryContextSwitchTo(oldcontext);
				break;
			}

		case RELKIND_PARTITIONED_TABLE:
		case RELKIND_PARTITIONED_INDEX:
		default:
			/* Return error if type of relation is not supported */
			/*
			 *
			 * 若关系类型不受支持则报错。
			 */
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot reindex this type of relation concurrently")));
			break;
	}

	/*
	 * Definitely no indexes, so leave.  Any checks based on
	 * REINDEXOPT_MISSING_OK should be done only while the list of indexes to
	 * work on is built as the session locks taken before this transaction
	 * commits will make sure that they cannot be dropped by a concurrent
	 * session until this operation completes.
	 *
	 * 确实没有任何索引，因此离开。基于 REINDEXOPT_MISSING_OK 的检查只应在建立
	 * 待处理索引列表时进行，
	 * 因为本事务提交前取得的会话锁会保证并发会话在本操作完成前无法删除它们。
	 */
	if (indexIds == NIL)
		return false;

	/* It's not a shared catalog, so refuse to move it to shared tablespace */
	/*
	 *
	 * 它不是共享目录，因此拒绝把它移到共享表空间。
	 */
	if (params->tablespaceOid == GLOBALTABLESPACE_OID)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot move non-shared relation to tablespace \"%s\"",
						get_tablespace_name(params->tablespaceOid))));

	Assert(heapRelationIds != NIL);

	/*-----
	 * Now we have all the indexes we want to process in indexIds.
	 *
	 * 现在 indexIds 中已有全部要处理的索引。
	 *
	 * The phases now are:
	 *
	 * 接下来的阶段是：
	 *
	 * 1. create new indexes in the catalog
	 * 2. build new indexes
	 * 3. let new indexes catch up with tuples inserted in the meantime
	 * 4. swap index names
	 * 5. mark old indexes as dead
	 * 6. drop old indexes
	 *
	 * 1. 在目录中创建新索引
	 * 2. 构建新索引
	 * 3. 让新索引追上其间插入的元组
	 * 4. 交换索引名
	 * 5. 把旧索引标为失效
	 * 6. 删除旧索引
	 *
	 * We process each phase for all indexes before moving to the next phase,
	 * for efficiency.
	 *
	 * 为了效率，对全部索引做完一个阶段，再进入下一阶段。
	 */

	/*
	 * Phase 1 of REINDEX CONCURRENTLY
	 *
	 * REINDEX CONCURRENTLY 的第 1 阶段。
	 *
	 * Create a new index with the same properties as the old one, but it is
	 * only registered in catalogs and will be built later.  Then get session
	 * locks on all involved tables.  See analogous code in DefineIndex() for
	 * more detailed comments.
	 *
	 * 创建一个与旧索引属性相同的新索引，但目前只在目录中登记，稍后才构建。然
	 * 后取得所有相关表上的会话锁。
	 * 更详细的说明见 DefineIndex() 中的类似代码。
	 */

	foreach(lc, indexIds)
	{
		char	   *concurrentName;
		ReindexIndexInfo *idx = lfirst(lc);
		ReindexIndexInfo *newidx;
		Oid			newIndexId;
		Relation	indexRel;
		Relation	heapRel;
		Oid			save_userid;
		int			save_sec_context;
		int			save_nestlevel;
		Relation	newIndexRel;
		LockRelId  *lockrelid;
		Oid			tablespaceid;

		indexRel = index_open(idx->indexId, ShareUpdateExclusiveLock);
		heapRel = table_open(indexRel->rd_index->indrelid,
							 ShareUpdateExclusiveLock);

		/*
		 * Switch to the table owner's userid, so that any index functions are
		 * run as that user.  Also lock down security-restricted operations
		 * and arrange to make GUC variable changes local to this command.
		 *
		 * 切换到表所有者的 userid，使索引函数以该用户身份运行，并收紧安全受限
		 * 操作，
		 * 同时使本命令的 GUC 变更局部生效。
		 */
		GetUserIdAndSecContext(&save_userid, &save_sec_context);
		SetUserIdAndSecContext(heapRel->rd_rel->relowner,
							   save_sec_context | SECURITY_RESTRICTED_OPERATION);
		save_nestlevel = NewGUCNestLevel();
		RestrictSearchPath();

		/* determine safety of this index for set_indexsafe_procflags */
		/*
		 *
		 * 判断该索引对 set_indexsafe_procflags 是否安全。
		 */
		idx->safe = (RelationGetIndexExpressions(indexRel) == NIL &&
					 RelationGetIndexPredicate(indexRel) == NIL);

#ifdef USE_INJECTION_POINTS
		if (idx->safe)
			INJECTION_POINT("reindex-conc-index-safe", NULL);
		else
			INJECTION_POINT("reindex-conc-index-not-safe", NULL);
#endif

		idx->tableId = RelationGetRelid(heapRel);
		idx->amId = indexRel->rd_rel->relam;

		/* This function shouldn't be called for temporary relations. */
		/*
		 *
		 * 不应针对临时关系调用本函数。
		 */
		if (indexRel->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
			elog(ERROR, "cannot reindex a temporary table concurrently");

		pgstat_progress_start_command(PROGRESS_COMMAND_CREATE_INDEX, idx->tableId);

		progress_vals[0] = PROGRESS_CREATEIDX_COMMAND_REINDEX_CONCURRENTLY;
		progress_vals[1] = 0;	/* initializing */
		/*
		 *
		 * 初始化阶段。
		 */
		progress_vals[2] = idx->indexId;
		progress_vals[3] = idx->amId;
		pgstat_progress_update_multi_param(4, progress_index, progress_vals);

		/* Choose a temporary relation name for the new index */
		/*
		 *
		 * 为新索引选择一个临时关系名。
		 */
		concurrentName = ChooseRelationName(get_rel_name(idx->indexId),
											NULL,
											"ccnew",
											get_rel_namespace(indexRel->rd_index->indrelid),
											false);

		/* Choose the new tablespace, indexes of toast tables are not moved */
		/*
		 *
		 * 选择新表空间；toast 表的索引不会被移动。
		 */
		if (OidIsValid(params->tablespaceOid) &&
			heapRel->rd_rel->relkind != RELKIND_TOASTVALUE)
			tablespaceid = params->tablespaceOid;
		else
			tablespaceid = indexRel->rd_rel->reltablespace;

		/* Create new index definition based on given index */
		/*
		 *
		 * 根据给定索引创建新的索引定义。
		 */
		newIndexId = index_concurrently_create_copy(heapRel,
													idx->indexId,
													tablespaceid,
													concurrentName);

		/*
		 * Now open the relation of the new index, a session-level lock is
		 * also needed on it.
		 *
		 * 现在打开新索引的关系，它也需要一把会话级锁。
		 */
		newIndexRel = index_open(newIndexId, ShareUpdateExclusiveLock);

		/*
		 * Save the list of OIDs and locks in private context
		 *
		 * 把 OID 和锁的列表保存到私有上下文。
		 */
		oldcontext = MemoryContextSwitchTo(private_context);

		newidx = palloc_object(ReindexIndexInfo);
		newidx->indexId = newIndexId;
		newidx->safe = idx->safe;
		newidx->tableId = idx->tableId;
		newidx->amId = idx->amId;

		newIndexIds = lappend(newIndexIds, newidx);

		/*
		 * Save lockrelid to protect each relation from drop then close
		 * relations. The lockrelid on parent relation is not taken here to
		 * avoid multiple locks taken on the same relation, instead we rely on
		 * parentRelationIds built earlier.
		 *
		 * 保存 lockrelid 以防止每个关系被删除，然后关闭关系。这里不对父关系取
		 * lockrelid，
		 * 以免对同一关系重复加锁，而是依赖先前建好的 parentRelationIds。
		 */
		lockrelid = palloc_object(LockRelId);
		*lockrelid = indexRel->rd_lockInfo.lockRelId;
		relationLocks = lappend(relationLocks, lockrelid);
		lockrelid = palloc_object(LockRelId);
		*lockrelid = newIndexRel->rd_lockInfo.lockRelId;
		relationLocks = lappend(relationLocks, lockrelid);

		MemoryContextSwitchTo(oldcontext);

		index_close(indexRel, NoLock);
		index_close(newIndexRel, NoLock);

		/* Roll back any GUC changes executed by index functions */
		/*
		 *
		 * 回滚索引函数执行过的任何 GUC 变更。
		 */
		AtEOXact_GUC(false, save_nestlevel);

		/* Restore userid and security context */
		/*
		 *
		 * 恢复 userid 和安全上下文。
		 */
		SetUserIdAndSecContext(save_userid, save_sec_context);

		table_close(heapRel, NoLock);

		/*
		 * If a statement is available, telling that this comes from a REINDEX
		 * command, collect the new index for event triggers.
		 *
		 * 若有语句表明这来自 REINDEX 命令，则为事件触发器收集新索引。
		 */
		if (stmt)
		{
			ObjectAddress address;

			ObjectAddressSet(address, RelationRelationId, newIndexId);
			EventTriggerCollectSimpleCommand(address,
											 InvalidObjectAddress,
											 (Node *) stmt);
		}
	}

	/*
	 * Save the heap lock for following visibility checks with other backends
	 * might conflict with this session.
	 *
	 * 保存堆锁，供随后与可能和本会话冲突的其他后端做可见性检查。
	 */
	foreach(lc, heapRelationIds)
	{
		Relation	heapRelation = table_open(lfirst_oid(lc), ShareUpdateExclusiveLock);
		LockRelId  *lockrelid;
		LOCKTAG    *heaplocktag;

		/* Save the list of locks in private context */
		/*
		 *
		 * 把锁列表保存到私有上下文。
		 */
		oldcontext = MemoryContextSwitchTo(private_context);

		/* Add lockrelid of heap relation to the list of locked relations */
		/*
		 *
		 * 把堆关系的 lockrelid 加入已锁关系列表。
		 */
		lockrelid = palloc_object(LockRelId);
		*lockrelid = heapRelation->rd_lockInfo.lockRelId;
		relationLocks = lappend(relationLocks, lockrelid);

		heaplocktag = palloc_object(LOCKTAG);

		/* Save the LOCKTAG for this parent relation for the wait phase */
		/*
		 *
		 * 保存该父关系的 LOCKTAG，供等待阶段使用。
		 */
		SET_LOCKTAG_RELATION(*heaplocktag, lockrelid->dbId, lockrelid->relId);
		lockTags = lappend(lockTags, heaplocktag);

		MemoryContextSwitchTo(oldcontext);

		/* Close heap relation */
		/*
		 *
		 * 关闭堆关系。
		 */
		table_close(heapRelation, NoLock);
	}

	/* Get a session-level lock on each table. */
	/*
	 *
	 * 对每张表取得会话级锁。
	 */
	foreach(lc, relationLocks)
	{
		LockRelId  *lockrelid = (LockRelId *) lfirst(lc);

		LockRelationIdForSession(lockrelid, ShareUpdateExclusiveLock);
	}

	PopActiveSnapshot();
	CommitTransactionCommand();
	StartTransactionCommand();

	/*
	 * Because we don't take a snapshot in this transaction, there's no need
	 * to set the PROC_IN_SAFE_IC flag here.
	 *
	 * 因为本事务不取快照，所以不必在这里设置 PROC_IN_SAFE_IC 标志。
	 */

	/*
	 * Phase 2 of REINDEX CONCURRENTLY
	 *
	 * REINDEX CONCURRENTLY 的第 2 阶段。
	 *
	 * Build the new indexes in a separate transaction for each index to avoid
	 * having open transactions for an unnecessary long time.  But before
	 * doing that, wait until no running transactions could have the table of
	 * the index open with the old list of indexes.  See "phase 2" in
	 * DefineIndex() for more details.
	 *
	 * 为每个索引在单独的事务中构建新索引，避免事务打开过久。但在此之前，要等
	 * 到没有运行中的事务
	 * 还拿着旧的索引列表打开该索引的表。更多细节见 DefineIndex() 的第 2 阶段。
	 */

	pgstat_progress_update_param(PROGRESS_CREATEIDX_PHASE,
								 PROGRESS_CREATEIDX_PHASE_WAIT_1);
	WaitForLockersMultiple(lockTags, ShareLock, true);
	CommitTransactionCommand();

	foreach(lc, newIndexIds)
	{
		ReindexIndexInfo *newidx = lfirst(lc);

		/* Start new transaction for this index's concurrent build */
		/*
		 *
		 * 为该索引的并发构建开始新事务。
		 */
		StartTransactionCommand();

		/*
		 * Check for user-requested abort.  This is inside a transaction so as
		 * xact.c does not issue a useless WARNING, and ensures that
		 * session-level locks are cleaned up on abort.
		 *
		 * 检查用户是否请求中止。放在事务内部，这样 xact.c 不会发出无用的
		 * WARNING，并保证中止时清理会话级锁。
		 */
		CHECK_FOR_INTERRUPTS();

		/* Tell concurrent indexing to ignore us, if index qualifies */
		/*
		 *
		 * 若索引符合条件，告诉其他并发建索引过程忽略我们。
		 */
		if (newidx->safe)
			set_indexsafe_procflags();

		/* Set ActiveSnapshot since functions in the indexes may need it */
		/*
		 *
		 * 设置 ActiveSnapshot，因为索引中的函数可能需要它。
		 */
		PushActiveSnapshot(GetTransactionSnapshot());

		/*
		 * Update progress for the index to build, with the correct parent
		 * table involved.
		 *
		 * 用正确的父表更新待构建索引的进度。
		 */
		pgstat_progress_start_command(PROGRESS_COMMAND_CREATE_INDEX, newidx->tableId);
		progress_vals[0] = PROGRESS_CREATEIDX_COMMAND_REINDEX_CONCURRENTLY;
		progress_vals[1] = PROGRESS_CREATEIDX_PHASE_BUILD;
		progress_vals[2] = newidx->indexId;
		progress_vals[3] = newidx->amId;
		pgstat_progress_update_multi_param(4, progress_index, progress_vals);

		/* Perform concurrent build of new index */
		/*
		 *
		 * 执行新索引的并发构建。
		 */
		index_concurrently_build(newidx->tableId, newidx->indexId);

		PopActiveSnapshot();
		CommitTransactionCommand();
	}

	StartTransactionCommand();

	/*
	 * Because we don't take a snapshot or Xid in this transaction, there's no
	 * need to set the PROC_IN_SAFE_IC flag here.
	 *
	 * 因为本事务不取快照也不取 Xid，所以不必在这里设置 PROC_IN_SAFE_IC 标志。
	 */

	/*
	 * Phase 3 of REINDEX CONCURRENTLY
	 *
	 * REINDEX CONCURRENTLY 的第 3 阶段。
	 *
	 * During this phase the old indexes catch up with any new tuples that
	 * were created during the previous phase.  See "phase 3" in DefineIndex()
	 * for more details.
	 *
	 * 本阶段让旧索引追上上一阶段期间创建的新元组。更多细节见 DefineIndex() 的
	 * 第 3 阶段。
	 */

	pgstat_progress_update_param(PROGRESS_CREATEIDX_PHASE,
								 PROGRESS_CREATEIDX_PHASE_WAIT_2);
	WaitForLockersMultiple(lockTags, ShareLock, true);
	CommitTransactionCommand();

	foreach(lc, newIndexIds)
	{
		ReindexIndexInfo *newidx = lfirst(lc);
		TransactionId limitXmin;
		Snapshot	snapshot;

		StartTransactionCommand();

		/*
		 * Check for user-requested abort.  This is inside a transaction so as
		 * xact.c does not issue a useless WARNING, and ensures that
		 * session-level locks are cleaned up on abort.
		 *
		 * 检查用户是否请求中止。放在事务内部，这样 xact.c 不会发出无用的
		 * WARNING，并保证中止时清理会话级锁。
		 */
		CHECK_FOR_INTERRUPTS();

		/* Tell concurrent indexing to ignore us, if index qualifies */
		/*
		 *
		 * 若索引符合条件，告诉其他并发建索引过程忽略我们。
		 */
		if (newidx->safe)
			set_indexsafe_procflags();

		/*
		 * Take the "reference snapshot" that will be used by validate_index()
		 * to filter candidate tuples.
		 *
		 * 取得 validate_index() 用来过滤候选元组的参考快照。
		 */
		snapshot = RegisterSnapshot(GetTransactionSnapshot());
		PushActiveSnapshot(snapshot);

		/*
		 * Update progress for the index to build, with the correct parent
		 * table involved.
		 *
		 * 用正确的父表更新待构建索引的进度。
		 */
		pgstat_progress_start_command(PROGRESS_COMMAND_CREATE_INDEX, newidx->tableId);
		progress_vals[0] = PROGRESS_CREATEIDX_COMMAND_REINDEX_CONCURRENTLY;
		progress_vals[1] = PROGRESS_CREATEIDX_PHASE_VALIDATE_IDXSCAN;
		progress_vals[2] = newidx->indexId;
		progress_vals[3] = newidx->amId;
		pgstat_progress_update_multi_param(4, progress_index, progress_vals);

		validate_index(newidx->tableId, newidx->indexId, snapshot);

		/*
		 * We can now do away with our active snapshot, we still need to save
		 * the xmin limit to wait for older snapshots.
		 *
		 * 现在可以丢掉活动快照，但仍需保存 xmin 界限，以便等待更旧的快照。
		 */
		limitXmin = snapshot->xmin;

		PopActiveSnapshot();
		UnregisterSnapshot(snapshot);

		/*
		 * To ensure no deadlocks, we must commit and start yet another
		 * transaction, and do our wait before any snapshot has been taken in
		 * it.
		 *
		 * 为确保不死锁，必须提交并再开一个事务，并在该事务取得任何快照之前完
		 * 成等待。
		 */
		CommitTransactionCommand();
		StartTransactionCommand();

		/*
		 * The index is now valid in the sense that it contains all currently
		 * interesting tuples.  But since it might not contain tuples deleted
		 * just before the reference snap was taken, we have to wait out any
		 * transactions that might have older snapshots.
		 *
		 * 就包含当前所有有关元组而言，索引现在是有效的。但它可能不含参考快照
		 * 取得之前刚被删除的元组，
		 * 因此必须等完任何可能持有更旧快照的事务。
		 *
		 * Because we don't take a snapshot or Xid in this transaction,
		 * there's no need to set the PROC_IN_SAFE_IC flag here.
		 *
		 * 因为本事务不取快照也不取 Xid，所以不必在这里设置 PROC_IN_SAFE_IC 标
		 * 志。
		 */
		pgstat_progress_update_param(PROGRESS_CREATEIDX_PHASE,
									 PROGRESS_CREATEIDX_PHASE_WAIT_3);
		WaitForOlderSnapshots(limitXmin, true);

		CommitTransactionCommand();
	}

	/*
	 * Phase 4 of REINDEX CONCURRENTLY
	 *
	 * REINDEX CONCURRENTLY 的第 4 阶段。
	 *
	 * Now that the new indexes have been validated, swap each new index with
	 * its corresponding old index.
	 *
	 * 新索引已经校验完成，现在把每个新索引与对应的旧索引交换。
	 *
	 * We mark the new indexes as valid and the old indexes as not valid at
	 * the same time to make sure we only get constraint violations from the
	 * indexes with the correct names.
	 *
	 * 同时把新索引标为有效、旧索引标为无效，以确保约束违反只来自名字正确的索
	 * 引。
	 */

	StartTransactionCommand();

	/*
	 * Because this transaction only does catalog manipulations and doesn't do
	 * any index operations, we can set the PROC_IN_SAFE_IC flag here
	 * unconditionally.
	 *
	 * 因为本事务只做目录操作、不做任何索引操作，可以无条件设置
	 * PROC_IN_SAFE_IC 标志。
	 */
	set_indexsafe_procflags();

	forboth(lc, indexIds, lc2, newIndexIds)
	{
		ReindexIndexInfo *oldidx = lfirst(lc);
		ReindexIndexInfo *newidx = lfirst(lc2);
		char	   *oldName;

		/*
		 * Check for user-requested abort.  This is inside a transaction so as
		 * xact.c does not issue a useless WARNING, and ensures that
		 * session-level locks are cleaned up on abort.
		 *
		 * 检查用户是否请求中止。放在事务内部，这样 xact.c 不会发出无用的
		 * WARNING，并保证中止时清理会话级锁。
		 */
		CHECK_FOR_INTERRUPTS();

		/* Choose a relation name for old index */
		/*
		 *
		 * 为旧索引选择一个关系名。
		 */
		oldName = ChooseRelationName(get_rel_name(oldidx->indexId),
									 NULL,
									 "ccold",
									 get_rel_namespace(oldidx->tableId),
									 false);

		/*
		 * Swapping the indexes might involve TOAST table access, so ensure we
		 * have a valid snapshot.
		 *
		 * 交换索引可能访问 TOAST 表，因此要确保有一个有效快照。
		 */
		PushActiveSnapshot(GetTransactionSnapshot());

		/*
		 * Swap old index with the new one.  This also marks the new one as
		 * valid and the old one as not valid.
		 *
		 * 把旧索引与新索引交换。同时把新的标为有效、旧的标为无效。
		 */
		index_concurrently_swap(newidx->indexId, oldidx->indexId, oldName);

		PopActiveSnapshot();

		/*
		 * Invalidate the relcache for the table, so that after this commit
		 * all sessions will refresh any cached plans that might reference the
		 * index.
		 *
		 * 使该表的 relcache 失效，以便本次提交之后所有会话刷新可能引用该索引
		 * 的已缓存计划。
		 */
		CacheInvalidateRelcacheByRelid(oldidx->tableId);

		/*
		 * CCI here so that subsequent iterations see the oldName in the
		 * catalog and can choose a nonconflicting name for their oldName.
		 * Otherwise, this could lead to conflicts if a table has two indexes
		 * whose names are equal for the first NAMEDATALEN-minus-a-few
		 * characters.
		 *
		 * 在此执行 CommandCounterIncrement，使后续迭代能在目录中看到 oldName，
		 * 并为自己的 oldName 选择不冲突的名字。
		 * 否则，若一张表有两个索引的名字在前 NAMEDATALEN 减几个字符处相同，就
		 * 可能冲突。
		 */
		CommandCounterIncrement();
	}

	/* Commit this transaction and make index swaps visible */
	/*
	 *
	 * 提交本事务，使索引交换可见。
	 */
	CommitTransactionCommand();
	StartTransactionCommand();

	/*
	 * While we could set PROC_IN_SAFE_IC if all indexes qualified, there's no
	 * real need for that, because we only acquire an Xid after the wait is
	 * done, and that lasts for a very short period.
	 *
	 * 虽然若全部索引都符合条件可以设置 PROC_IN_SAFE_IC，但没有必要：我们只在
	 * 等待结束之后才取得 Xid，
	 * 而且那段时间非常短。
	 */

	/*
	 * Phase 5 of REINDEX CONCURRENTLY
	 *
	 * REINDEX CONCURRENTLY 的第 5 阶段。
	 *
	 * Mark the old indexes as dead.  First we must wait until no running
	 * transaction could be using the index for a query.  See also
	 * index_drop() for more details.
	 *
	 * 把旧索引标为失效。首先必须等到没有运行中的事务还在查询中使用该索引。更
	 * 多细节另见 index_drop()。
	 */

	pgstat_progress_update_param(PROGRESS_CREATEIDX_PHASE,
								 PROGRESS_CREATEIDX_PHASE_WAIT_4);
	WaitForLockersMultiple(lockTags, AccessExclusiveLock, true);

	foreach(lc, indexIds)
	{
		ReindexIndexInfo *oldidx = lfirst(lc);

		/*
		 * Check for user-requested abort.  This is inside a transaction so as
		 * xact.c does not issue a useless WARNING, and ensures that
		 * session-level locks are cleaned up on abort.
		 *
		 * 检查用户是否请求中止。放在事务内部，这样 xact.c 不会发出无用的
		 * WARNING，并保证中止时清理会话级锁。
		 */
		CHECK_FOR_INTERRUPTS();

		/*
		 * Updating pg_index might involve TOAST table access, so ensure we
		 * have a valid snapshot.
		 *
		 * 更新 pg_index 可能访问 TOAST 表，因此要确保有一个有效快照。
		 */
		PushActiveSnapshot(GetTransactionSnapshot());

		index_concurrently_set_dead(oldidx->tableId, oldidx->indexId);

		PopActiveSnapshot();
	}

	/* Commit this transaction to make the updates visible. */
	/*
	 *
	 * 提交本事务，使更新可见。
	 */
	CommitTransactionCommand();
	StartTransactionCommand();

	/*
	 * While we could set PROC_IN_SAFE_IC if all indexes qualified, there's no
	 * real need for that, because we only acquire an Xid after the wait is
	 * done, and that lasts for a very short period.
	 *
	 * 虽然若全部索引都符合条件可以设置 PROC_IN_SAFE_IC，但没有必要：我们只在
	 * 等待结束之后才取得 Xid，
	 * 而且那段时间非常短。
	 */

	/*
	 * Phase 6 of REINDEX CONCURRENTLY
	 *
	 * REINDEX CONCURRENTLY 的第 6 阶段。
	 *
	 * Drop the old indexes.
	 *
	 * 删除旧索引。
	 */

	pgstat_progress_update_param(PROGRESS_CREATEIDX_PHASE,
								 PROGRESS_CREATEIDX_PHASE_WAIT_5);
	WaitForLockersMultiple(lockTags, AccessExclusiveLock, true);

	PushActiveSnapshot(GetTransactionSnapshot());

	{
		ObjectAddresses *objects = new_object_addresses();

		foreach(lc, indexIds)
		{
			ReindexIndexInfo *idx = lfirst(lc);
			ObjectAddress object;

			object.classId = RelationRelationId;
			object.objectId = idx->indexId;
			object.objectSubId = 0;

			add_exact_object_address(&object, objects);
		}

		/*
		 * Use PERFORM_DELETION_CONCURRENT_LOCK so that index_drop() uses the
		 * right lock level.
		 *
		 * 使用 PERFORM_DELETION_CONCURRENT_LOCK，使 index_drop() 采用正确的锁
		 * 级别。
		 */
		performMultipleDeletions(objects, DROP_RESTRICT,
								 PERFORM_DELETION_CONCURRENT_LOCK | PERFORM_DELETION_INTERNAL);
	}

	PopActiveSnapshot();
	CommitTransactionCommand();

	/*
	 * Finally, release the session-level lock on the table.
	 *
	 * 最后释放表上的会话级锁。
	 */
	foreach(lc, relationLocks)
	{
		LockRelId  *lockrelid = (LockRelId *) lfirst(lc);

		UnlockRelationIdForSession(lockrelid, ShareUpdateExclusiveLock);
	}

	/* Start a new transaction to finish process properly */
	/*
	 *
	 * 开始一个新事务，以便正确结束处理。
	 */
	StartTransactionCommand();

	/* Log what we did */
	/*
	 *
	 * 记录我们所做的事。
	 */
	if ((params->options & REINDEXOPT_VERBOSE) != 0)
	{
		if (relkind == RELKIND_INDEX)
			ereport(INFO,
					(errmsg("index \"%s.%s\" was reindexed",
							relationNamespace, relationName),
					 errdetail("%s.",
							   pg_rusage_show(&ru0))));
		else
		{
			foreach(lc, newIndexIds)
			{
				ReindexIndexInfo *idx = lfirst(lc);
				Oid			indOid = idx->indexId;

				ereport(INFO,
						(errmsg("index \"%s.%s\" was reindexed",
								get_namespace_name(get_rel_namespace(indOid)),
								get_rel_name(indOid))));
				/* Don't show rusage here, since it's not per index. */
				/*
				 *
				 * 这里不显示 rusage，因为它不是按索引统计的。
				 */
			}

			ereport(INFO,
					(errmsg("table \"%s.%s\" was reindexed",
							relationNamespace, relationName),
					 errdetail("%s.",
							   pg_rusage_show(&ru0))));
		}
	}

	MemoryContextDelete(private_context);

	pgstat_progress_end_command();

	return true;
}

/*
 * Insert or delete an appropriate pg_inherits tuple to make the given index
 * be a partition of the indicated parent index.
 *
 * 插入或删除合适的 pg_inherits 元组，使给定索引成为指定父索引的分区。
 *
 * This also corrects the pg_depend information for the affected index.
 *
 * 同时修正受影响索引的 pg_depend 信息。
 */
void
IndexSetParentIndex(Relation partitionIdx, Oid parentOid)
{
	Relation	pg_inherits;
	ScanKeyData key[2];
	SysScanDesc scan;
	Oid			partRelid = RelationGetRelid(partitionIdx);
	HeapTuple	tuple;
	bool		fix_dependencies;

	/* Make sure this is an index */
	/*
	 *
	 * 确认这是一个索引。
	 */
	Assert(partitionIdx->rd_rel->relkind == RELKIND_INDEX ||
		   partitionIdx->rd_rel->relkind == RELKIND_PARTITIONED_INDEX);

	/*
	 * Scan pg_inherits for rows linking our index to some parent.
	 *
	 * 扫描 pg_inherits，查找把我们的索引连到某个父索引的行。
	 */
	pg_inherits = relation_open(InheritsRelationId, RowExclusiveLock);
	ScanKeyInit(&key[0],
				Anum_pg_inherits_inhrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(partRelid));
	ScanKeyInit(&key[1],
				Anum_pg_inherits_inhseqno,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(1));
	scan = systable_beginscan(pg_inherits, InheritsRelidSeqnoIndexId, true,
							  NULL, 2, key);
	tuple = systable_getnext(scan);

	if (!HeapTupleIsValid(tuple))
	{
		if (parentOid == InvalidOid)
		{
			/*
			 * No pg_inherits row, and no parent wanted: nothing to do in this
			 * case.
			 *
			 * 没有 pg_inherits 行，也不需要父索引：这种情况下无事可做。
			 */
			fix_dependencies = false;
		}
		else
		{
			StoreSingleInheritance(partRelid, parentOid, 1);
			fix_dependencies = true;
		}
	}
	else
	{
		Form_pg_inherits inhForm = (Form_pg_inherits) GETSTRUCT(tuple);

		if (parentOid == InvalidOid)
		{
			/*
			 * There exists a pg_inherits row, which we want to clear; do so.
			 *
			 * 存在一行 pg_inherits，我们要清除它；现在就清除。
			 */
			CatalogTupleDelete(pg_inherits, &tuple->t_self);
			fix_dependencies = true;
		}
		else
		{
			/*
			 * A pg_inherits row exists.  If it's the same we want, then we're
			 * good; if it differs, that amounts to a corrupt catalog and
			 * should not happen.
			 *
			 * 存在一行 pg_inherits。若它正是我们想要的，那就没问题；若不同，
			 * 则相当于目录损坏，不应发生。
			 */
			if (inhForm->inhparent != parentOid)
			{
				/* unexpected: we should not get called in this case */
				/*
				 *
				 * 意外：这种情况下不该被调用。
				 */
				elog(ERROR, "bogus pg_inherit row: inhrelid %u inhparent %u",
					 inhForm->inhrelid, inhForm->inhparent);
			}

			/* already in the right state */
			/*
			 *
			 * 已经处于正确状态。
			 */
			fix_dependencies = false;
		}
	}

	/* done with pg_inherits */
	/*
	 *
	 * pg_inherits 处理完毕。
	 */
	systable_endscan(scan);
	relation_close(pg_inherits, RowExclusiveLock);

	/* set relhassubclass if an index partition has been added to the parent */
	/*
	 *
	 * 若已向父索引加入索引分区，则设置 relhassubclass。
	 */
	if (OidIsValid(parentOid))
	{
		LockRelationOid(parentOid, ShareUpdateExclusiveLock);
		SetRelationHasSubclass(parentOid, true);
	}

	/* set relispartition correctly on the partition */
	/*
	 *
	 * 在分区上正确设置 relispartition。
	 */
	update_relispartition(partRelid, OidIsValid(parentOid));

	if (fix_dependencies)
	{
		/*
		 * Insert/delete pg_depend rows.  If setting a parent, add PARTITION
		 * dependencies on the parent index and the table; if removing a
		 * parent, delete PARTITION dependencies.
		 *
		 * 插入或删除 pg_depend 行。若设置父索引，则对父索引和表添加 PARTITION
		 * 依赖；若去掉父索引，则删除 PARTITION 依赖。
		 */
		if (OidIsValid(parentOid))
		{
			ObjectAddress partIdx;
			ObjectAddress parentIdx;
			ObjectAddress partitionTbl;

			ObjectAddressSet(partIdx, RelationRelationId, partRelid);
			ObjectAddressSet(parentIdx, RelationRelationId, parentOid);
			ObjectAddressSet(partitionTbl, RelationRelationId,
							 partitionIdx->rd_index->indrelid);
			recordDependencyOn(&partIdx, &parentIdx,
							   DEPENDENCY_PARTITION_PRI);
			recordDependencyOn(&partIdx, &partitionTbl,
							   DEPENDENCY_PARTITION_SEC);
		}
		else
		{
			deleteDependencyRecordsForClass(RelationRelationId, partRelid,
											RelationRelationId,
											DEPENDENCY_PARTITION_PRI);
			deleteDependencyRecordsForClass(RelationRelationId, partRelid,
											RelationRelationId,
											DEPENDENCY_PARTITION_SEC);
		}

		/* make our updates visible */
		/*
		 *
		 * 使我们的更新可见。
		 */
		CommandCounterIncrement();
	}
}

/*
 * Subroutine of IndexSetParentIndex to update the relispartition flag of the
 * given index to the given value.
 *
 * IndexSetParentIndex 的子程序，把给定索引的 relispartition 标志更新为给定值。
 */
static void
update_relispartition(Oid relationId, bool newval)
{
	HeapTuple	tup;
	Relation	classRel;
	ItemPointerData otid;

	classRel = table_open(RelationRelationId, RowExclusiveLock);
	tup = SearchSysCacheLockedCopy1(RELOID, ObjectIdGetDatum(relationId));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for relation %u", relationId);
	otid = tup->t_self;
	Assert(((Form_pg_class) GETSTRUCT(tup))->relispartition != newval);
	((Form_pg_class) GETSTRUCT(tup))->relispartition = newval;
	CatalogTupleUpdate(classRel, &otid, tup);
	UnlockTuple(classRel, &otid, InplaceUpdateTupleLock);
	heap_freetuple(tup);
	table_close(classRel, RowExclusiveLock);
}

/*
 * Set the PROC_IN_SAFE_IC flag in MyProc->statusFlags.
 *
 * 在 MyProc->statusFlags 中设置 PROC_IN_SAFE_IC 标志。
 *
 * When doing concurrent index builds, we can set this flag
 * to tell other processes concurrently running CREATE
 * INDEX CONCURRENTLY or REINDEX CONCURRENTLY to ignore us when
 * doing their waits for concurrent snapshots.  On one hand it
 * avoids pointlessly waiting for a process that's not interesting
 * anyway; but more importantly it avoids deadlocks in some cases.
 *
 * 并发构建索引时可以设置该标志，告诉同时运行 CREATE INDEX CONCURRENTLY 或
 * REINDEX CONCURRENTLY 的其他进程
 * 在等待并发快照时忽略我们。一方面避免白白等待一个本来就不相关的进程；更重要
 * 的是在某些情况下避免死锁。
 *
 * This can be done safely only for indexes that don't execute any
 * expressions that could access other tables, so index must not be
 * expressional nor partial.  Caller is responsible for only calling
 * this routine when that assumption holds true.
 *
 * 只有当索引不执行任何可能访问其他表的表达式时，这样做才安全，因此索引既不能
 * 是表达式索引也不能是部分索引。
 * 调用方必须只在该假设成立时调用本例程。
 *
 * (The flag is reset automatically at transaction end, so it must be
 * set for each transaction.)
 *
 * 该标志在事务结束时自动复位，因此每个事务都必须设置。
 */
static inline void
set_indexsafe_procflags(void)
{
	/*
	 * This should only be called before installing xid or xmin in MyProc;
	 * otherwise, concurrent processes could see an Xmin that moves backwards.
	 *
	 * 只能在把 xid 或 xmin 装入 MyProc 之前调用；否则并发进程可能看到向后移动
	 * 的 Xmin。
	 */
	Assert(MyProc->xid == InvalidTransactionId &&
		   MyProc->xmin == InvalidTransactionId);

	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
	MyProc->statusFlags |= PROC_IN_SAFE_IC;
	ProcGlobal->statusFlags[MyProc->pgxactoff] = MyProc->statusFlags;
	LWLockRelease(ProcArrayLock);
}

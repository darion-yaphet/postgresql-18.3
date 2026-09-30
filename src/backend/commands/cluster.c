/*-------------------------------------------------------------------------
 *
 * cluster.c
 *	  CLUSTER a table on an index.  This is now also used for VACUUM FULL.
 *
 * 按索引对表做 CLUSTER。现在也用于 VACUUM FULL。
 *
 * There is hardly anything left of Paul Brown's original implementation...
 *
 * Paul Brown 的最初实现几乎已经不剩什么了。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/cluster.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amapi.h"
#include "access/heapam.h"
#include "access/multixact.h"
#include "access/relscan.h"
#include "access/tableam.h"
#include "access/toast_internals.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_am.h"
#include "catalog/pg_inherits.h"
#include "catalog/toasting.h"
#include "commands/cluster.h"
#include "commands/defrem.h"
#include "commands/progress.h"
#include "commands/tablecmds.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "optimizer/optimizer.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "utils/acl.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_rusage.h"
#include "utils/relmapper.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/*
 * 核心流程概览：
 * cluster() 解析 CLUSTER 选项。单表时直接 cluster_rel；多表或未指定表时收集关系，
 * 由 cluster_multiple_rels 逐表独立事务处理，以免同时持有全部 AccessExclusiveLock。
 * cluster_rel 校验权限与索引后调用 rebuild_relation：make_new_heap 建临时堆，
 * copy_table_data 按索引或物理顺序复制，swap_relation_files 交换文件，
 * finish_heap_swap 重建索引并删除临时表。indexOid 为 InvalidOid 时即 VACUUM FULL。
 */
/*
 * This struct is used to pass around the information on tables to be
 * clustered. We need this so we can make a list of them when invoked without
 * a specific table/index pair.
 *
 * 该结构用来传递待聚类的表信息。未指定具体表/索引对时，需要用它把这些表做成列表。
 */
typedef struct
{
	Oid			tableOid;
	Oid			indexOid;
} RelToCluster;


static void cluster_multiple_rels(List *rtcs, ClusterParams *params);
static void rebuild_relation(Relation OldHeap, Relation index, bool verbose);
static void copy_table_data(Relation NewHeap, Relation OldHeap, Relation OldIndex,
							bool verbose, bool *pSwapToastByContent,
							TransactionId *pFreezeXid, MultiXactId *pCutoffMulti);
static List *get_tables_to_cluster(MemoryContext cluster_context);
static List *get_tables_to_cluster_partitioned(MemoryContext cluster_context,
											   Oid indexOid);
static bool cluster_is_permitted_for_relation(Oid relid, Oid userid);


/*---------------------------------------------------------------------------
 * This cluster code allows for clustering multiple tables at once. Because
 * of this, we cannot just run everything on a single transaction, or we
 * would be forced to acquire exclusive locks on all the tables being
 * clustered, simultaneously --- very likely leading to deadlock.
 *
 * 这段 CLUSTER 代码允许一次处理多张表。因此不能全放在同一个事务里，
 * 否则必须同时对所有表加排他锁，很容易死锁。
 *
 * To solve this we follow a similar strategy to VACUUM code,
 * clustering each relation in a separate transaction. For this to work,
 * we need to:
 *	- provide a separate memory context so that we can pass information in
 *	  a way that survives across transactions
 *	- start a new transaction every time a new relation is clustered
 *	- check for validity of the information on to-be-clustered relations,
 *	  as someone might have deleted a relation behind our back, or
 *	  clustered one on a different index
 *	- end the transaction
 *
 * 解决办法与 VACUUM 类似：每个关系单独一个事务。为此需要：
 * - 单独的内存上下文，使信息能跨事务保留
 * - 每聚类一个新关系就开启新事务
 * - 复核待聚类关系是否仍然有效，因为可能已被别人删除，或已改到别的索引上聚类
 * - 结束事务
 *
 * The single-relation case does not have any such overhead.
 *
 * 单关系的情况没有这些额外开销。
 *
 * We also allow a relation to be specified without index.  In that case,
 * the indisclustered bit will be looked up, and an ERROR will be thrown
 * if there is no index with the bit set.
 *
 * 也允许只指定关系、不指定索引。此时查找 indisclustered 位；
 * 若没有索引设置该位，则报错。
 *---------------------------------------------------------------------------
 */
void
cluster(ParseState *pstate, ClusterStmt *stmt, bool isTopLevel)
{
	ListCell   *lc;
	ClusterParams params = {0};
	bool		verbose = false;
	Relation	rel = NULL;
	Oid			indexOid = InvalidOid;
	MemoryContext cluster_context;
	List	   *rtcs;

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
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized %s option \"%s\"",
							"CLUSTER", opt->defname),
					 parser_errposition(pstate, opt->location)));
	}

	params.options = (verbose ? CLUOPT_VERBOSE : 0);

	if (stmt->relation != NULL)
	{
		/* This is the single-relation case. */
		/*
		 *
		 * 这是单关系的情况。
		 */
		Oid			tableOid;

		/*
		 * Find, lock, and check permissions on the table.  We obtain
		 * AccessExclusiveLock right away to avoid lock-upgrade hazard in the
		 * single-transaction case.
		 *
		 * 查找表、加锁并检查权限。立刻取得 AccessExclusiveLock，
		 * 避免单事务路径上的锁升级风险。
		 */
		tableOid = RangeVarGetRelidExtended(stmt->relation,
											AccessExclusiveLock,
											0,
											RangeVarCallbackMaintainsTable,
											NULL);
		rel = table_open(tableOid, NoLock);

		/*
		 * Reject clustering a remote temp table ... their local buffer
		 * manager is not going to cope.
		 *
		 * 拒绝聚类其他会话的临时表……它们的本地缓冲区管理器无法处理。
		 */
		if (RELATION_IS_OTHER_TEMP(rel))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot cluster temporary tables of other sessions")));

		if (stmt->indexname == NULL)
		{
			ListCell   *index;

			/* We need to find the index that has indisclustered set. */
			/*
			 *
			 * 需要找到设置了 indisclustered 的索引。
			 */
			foreach(index, RelationGetIndexList(rel))
			{
				indexOid = lfirst_oid(index);
				if (get_index_isclustered(indexOid))
					break;
				indexOid = InvalidOid;
			}

			if (!OidIsValid(indexOid))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("there is no previously clustered index for table \"%s\"",
								stmt->relation->relname)));
		}
		else
		{
			/*
			 * The index is expected to be in the same namespace as the
			 * relation.
			 *
			 * 该索引应与关系位于同一命名空间。
			 */
			indexOid = get_relname_relid(stmt->indexname,
										 rel->rd_rel->relnamespace);
			if (!OidIsValid(indexOid))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("index \"%s\" for table \"%s\" does not exist",
								stmt->indexname, stmt->relation->relname)));
		}

		/* For non-partitioned tables, do what we came here to do. */
		/*
		 *
		 * 对非分区表，执行此次要做的聚类。
		 */
		if (rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		{
			cluster_rel(rel, indexOid, &params);
			/* cluster_rel closes the relation, but keeps lock */
			/*
			 *
			 * cluster_rel 会关闭关系，但保留锁。
			 */

			return;
		}
	}

	/*
	 * By here, we know we are in a multi-table situation.  In order to avoid
	 * holding locks for too long, we want to process each table in its own
	 * transaction.  This forces us to disallow running inside a user
	 * transaction block.
	 *
	 * 到这里已确定是多表场景。为避免长时间持锁，每张表用自己的事务处理。
	 * 因此不允许在用户事务块中运行。
	 */
	PreventInTransactionBlock(isTopLevel, "CLUSTER");

	/* Also, we need a memory context to hold our list of relations */
	/*
	 *
	 * 另外需要一个内存上下文来保存关系列表。
	 */
	cluster_context = AllocSetContextCreate(PortalContext,
											"Cluster",
											ALLOCSET_DEFAULT_SIZES);

	/*
	 * Either we're processing a partitioned table, or we were not given any
	 * table name at all.  In either case, obtain a list of relations to
	 * process.
	 *
	 * 要么正在处理分区表，要么根本没有给出表名。两种情况都要取得待处理关系列表。
	 *
	 * In the former case, an index name must have been given, so we don't
	 * need to recheck its "indisclustered" bit, but we have to check that it
	 * is an index that we can cluster on.  In the latter case, we set the
	 * option bit to have indisclustered verified.
	 *
	 * 前一种情况必须已给出索引名，因此不必再查 indisclustered，但要确认该索引可用于聚类。
	 * 后一种情况则设置选项，要求核对 indisclustered。
	 *
	 * Rechecking the relation itself is necessary here in all cases.
	 *
	 * 无论哪种情况，这里都必须重新检查关系本身。
	 */
	params.options |= CLUOPT_RECHECK;
	if (rel != NULL)
	{
		Assert(rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE);
		check_index_is_clusterable(rel, indexOid, AccessShareLock);
		rtcs = get_tables_to_cluster_partitioned(cluster_context, indexOid);

		/* close relation, releasing lock on parent table */
		/*
		 *
		 * 关闭关系，释放父表上的锁。
		 */
		table_close(rel, AccessExclusiveLock);
	}
	else
	{
		rtcs = get_tables_to_cluster(cluster_context);
		params.options |= CLUOPT_RECHECK_ISCLUSTERED;
	}

	/* Do the job. */
	/*
	 *
	 * 执行实际工作。
	 */
	cluster_multiple_rels(rtcs, &params);

	/* Start a new transaction for the cleanup work. */
	/*
	 *
	 * 为清理工作开启一个新事务。
	 */
	StartTransactionCommand();

	/* Clean up working storage */
	/*
	 *
	 * 清理工作存储。
	 */
	MemoryContextDelete(cluster_context);
}

/*
 * Given a list of relations to cluster, process each of them in a separate
 * transaction.
 *
 * 给定待聚类关系列表，每个关系用单独的事务处理。
 *
 * We expect to be in a transaction at start, but there isn't one when we
 * return.
 *
 * 进入时应当处于事务中，返回时则不再处于事务中。
 */
static void
cluster_multiple_rels(List *rtcs, ClusterParams *params)
{
	ListCell   *lc;

	/* Commit to get out of starting transaction */
	/*
	 *
	 * 提交以离开起始事务。
	 */
	PopActiveSnapshot();
	CommitTransactionCommand();

	/* Cluster the tables, each in a separate transaction */
	/*
	 *
	 * 逐表聚类，每张表一个独立事务。
	 */
	foreach(lc, rtcs)
	{
		RelToCluster *rtc = (RelToCluster *) lfirst(lc);
		Relation	rel;

		/* Start a new transaction for each relation. */
		/*
		 *
		 * 为每个关系开启新事务。
		 */
		StartTransactionCommand();

		/* functions in indexes may want a snapshot set */
		/*
		 *
		 * 索引中的函数可能需要已设置的快照。
		 */
		PushActiveSnapshot(GetTransactionSnapshot());

		rel = table_open(rtc->tableOid, AccessExclusiveLock);

		/* Process this table */
		/*
		 *
		 * 处理这张表。
		 */
		cluster_rel(rel, rtc->indexOid, params);
		/* cluster_rel closes the relation, but keeps lock */
		/*
		 *
		 * cluster_rel 会关闭关系，但保留锁。
		 */

		PopActiveSnapshot();
		CommitTransactionCommand();
	}
}

/*
 * cluster_rel
 *
 * cluster_rel。
 *
 * This clusters the table by creating a new, clustered table and
 * swapping the relfilenumbers of the new table and the old table, so
 * the OID of the original table is preserved.  Thus we do not lose
 * GRANT, inheritance nor references to this table.
 *
 * 通过创建一张已聚类的新表，并交换新旧表的 relfilenumber 来完成聚类，
 * 从而保留原表 OID。因此 GRANT、继承以及对该表的引用都不会丢失。
 *
 * Indexes are rebuilt too, via REINDEX. Since we are effectively bulk-loading
 * the new table, it's better to create the indexes afterwards than to fill
 * them incrementally while we load the table.
 *
 * 索引也通过 REINDEX 重建。相当于在批量装载新表，
 * 所以应在装载之后再建索引，而不是边装载边增量填充。
 *
 * If indexOid is InvalidOid, the table will be rewritten in physical order
 * instead of index order.  This is the new implementation of VACUUM FULL,
 * and error messages should refer to the operation as VACUUM not CLUSTER.
 *
 * 若 indexOid 为 InvalidOid，则按物理顺序而不是索引顺序重写表。
 * 这是 VACUUM FULL 的新实现，错误信息应称为 VACUUM 而不是 CLUSTER。
 */
void
cluster_rel(Relation OldHeap, Oid indexOid, ClusterParams *params)
{
	Oid			tableOid = RelationGetRelid(OldHeap);
	Oid			save_userid;
	int			save_sec_context;
	int			save_nestlevel;
	bool		verbose = ((params->options & CLUOPT_VERBOSE) != 0);
	bool		recheck = ((params->options & CLUOPT_RECHECK) != 0);
	Relation	index;

	Assert(CheckRelationLockedByMe(OldHeap, AccessExclusiveLock, false));

	/* Check for user-requested abort. */
	/*
	 *
	 * 检查用户是否请求中止。
	 */
	CHECK_FOR_INTERRUPTS();

	pgstat_progress_start_command(PROGRESS_COMMAND_CLUSTER, tableOid);
	if (OidIsValid(indexOid))
		pgstat_progress_update_param(PROGRESS_CLUSTER_COMMAND,
									 PROGRESS_CLUSTER_COMMAND_CLUSTER);
	else
		pgstat_progress_update_param(PROGRESS_CLUSTER_COMMAND,
									 PROGRESS_CLUSTER_COMMAND_VACUUM_FULL);

	/*
	 * Switch to the table owner's userid, so that any index functions are run
	 * as that user.  Also lock down security-restricted operations and
	 * arrange to make GUC variable changes local to this command.
	 *
	 * 切换到表属主的用户 ID，使索引函数以该用户身份运行。
	 * 同时限制安全敏感操作，并让 GUC 变更仅在本命令内有效。
	 */
	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(OldHeap->rd_rel->relowner,
						   save_sec_context | SECURITY_RESTRICTED_OPERATION);
	save_nestlevel = NewGUCNestLevel();
	RestrictSearchPath();

	/*
	 * Since we may open a new transaction for each relation, we have to check
	 * that the relation still is what we think it is.
	 *
	 * 因为可能为每个关系开启新事务，必须确认该关系仍是我们所认为的那个。
	 *
	 * If this is a single-transaction CLUSTER, we can skip these tests. We
	 * *must* skip the one on indisclustered since it would reject an attempt
	 * to cluster a not-previously-clustered index.
	 *
	 * 若是单事务 CLUSTER，可以跳过这些检查。
	 * 对 indisclustered 的检查必须跳过，否则会拒绝聚类一个此前未标记聚类的索引。
	 */
	if (recheck)
	{
		/* Check that the user still has privileges for the relation */
		/*
		 *
		 * 检查用户对该关系是否仍有权限。
		 */
		if (!cluster_is_permitted_for_relation(tableOid, save_userid))
		{
			relation_close(OldHeap, AccessExclusiveLock);
			goto out;
		}

		/*
		 * Silently skip a temp table for a remote session.  Only doing this
		 * check in the "recheck" case is appropriate (which currently means
		 * somebody is executing a database-wide CLUSTER or on a partitioned
		 * table), because there is another check in cluster() which will stop
		 * any attempt to cluster remote temp tables by name.  There is
		 * another check in cluster_rel which is redundant, but we leave it
		 * for extra safety.
		 *
		 * 静默跳过其他会话的临时表。只在 recheck 情况下做此检查才合适
		 * （目前即全库 CLUSTER 或针对分区表），因为 cluster() 里另有检查会阻止按名聚类远程临时表。
		 * cluster_rel 中还有一处冗余检查，为更安全而保留。
		 */
		if (RELATION_IS_OTHER_TEMP(OldHeap))
		{
			relation_close(OldHeap, AccessExclusiveLock);
			goto out;
		}

		if (OidIsValid(indexOid))
		{
			/*
			 * Check that the index still exists
			 *
			 * 检查索引是否仍然存在。
			 */
			if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(indexOid)))
			{
				relation_close(OldHeap, AccessExclusiveLock);
				goto out;
			}

			/*
			 * Check that the index is still the one with indisclustered set,
			 * if needed.
			 *
			 * 如有需要，检查该索引是否仍是设置了 indisclustered 的那个。
			 */
			if ((params->options & CLUOPT_RECHECK_ISCLUSTERED) != 0 &&
				!get_index_isclustered(indexOid))
			{
				relation_close(OldHeap, AccessExclusiveLock);
				goto out;
			}
		}
	}

	/*
	 * We allow VACUUM FULL, but not CLUSTER, on shared catalogs.  CLUSTER
	 * would work in most respects, but the index would only get marked as
	 * indisclustered in the current database, leading to unexpected behavior
	 * if CLUSTER were later invoked in another database.
	 *
	 * 共享目录允许 VACUUM FULL，但不允许 CLUSTER。CLUSTER 在多数方面能工作，
	 * 但索引只会在当前数据库中被标为 indisclustered，以后在其他数据库中再 CLUSTER 时行为会出乎意料。
	 */
	if (OidIsValid(indexOid) && OldHeap->rd_rel->relisshared)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot cluster a shared catalog")));

	/*
	 * Don't process temp tables of other backends ... their local buffer
	 * manager is not going to cope.
	 *
	 * 不要处理其他后端的临时表……它们的本地缓冲区管理器无法处理。
	 */
	if (RELATION_IS_OTHER_TEMP(OldHeap))
	{
		if (OidIsValid(indexOid))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot cluster temporary tables of other sessions")));
		else
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot vacuum temporary tables of other sessions")));
	}

	/*
	 * Also check for active uses of the relation in the current transaction,
	 * including open scans and pending AFTER trigger events.
	 *
	 * 还要检查当前事务中是否仍在使用该关系，包括打开的扫描和尚未完成的 AFTER 触发器事件。
	 */
	CheckTableNotInUse(OldHeap, OidIsValid(indexOid) ? "CLUSTER" : "VACUUM");

	/* Check heap and index are valid to cluster on */
	/*
	 *
	 * 检查堆和索引是否可以用来聚类。
	 */
	if (OidIsValid(indexOid))
	{
		/* verify the index is good and lock it */
		/*
		 *
		 * 确认索引有效并对其加锁。
		 */
		check_index_is_clusterable(OldHeap, indexOid, AccessExclusiveLock);
		/* also open it */
		/*
		 *
		 * 同时打开它。
		 */
		index = index_open(indexOid, NoLock);
	}
	else
		index = NULL;

	/*
	 * Quietly ignore the request if this is a materialized view which has not
	 * been populated from its query. No harm is done because there is no data
	 * to deal with, and we don't want to throw an error if this is part of a
	 * multi-relation request -- for example, CLUSTER was run on the entire
	 * database.
	 *
	 * 若物化视图尚未按其查询填充，则悄悄忽略该请求。没有数据可处理，不会造成损害；
	 * 若这是多关系请求的一部分（例如对整个数据库执行 CLUSTER），也不应报错。
	 */
	if (OldHeap->rd_rel->relkind == RELKIND_MATVIEW &&
		!RelationIsPopulated(OldHeap))
	{
		relation_close(OldHeap, AccessExclusiveLock);
		goto out;
	}

	Assert(OldHeap->rd_rel->relkind == RELKIND_RELATION ||
		   OldHeap->rd_rel->relkind == RELKIND_MATVIEW ||
		   OldHeap->rd_rel->relkind == RELKIND_TOASTVALUE);

	/*
	 * All predicate locks on the tuples or pages are about to be made
	 * invalid, because we move tuples around.  Promote them to relation
	 * locks.  Predicate locks on indexes will be promoted when they are
	 * reindexed.
	 *
	 * 元组或页面上的谓词锁即将失效，因为元组会被搬走。把它们提升为关系锁。
	 * 索引上的谓词锁在重建索引时再提升。
	 */
	TransferPredicateLocksToHeapRelation(OldHeap);

	/* rebuild_relation does all the dirty work */
	/*
	 *
	 * rebuild_relation 完成全部重活。
	 */
	rebuild_relation(OldHeap, index, verbose);
	/* rebuild_relation closes OldHeap, and index if valid */
	/*
	 *
	 * rebuild_relation 关闭 OldHeap，若索引有效也一并关闭。
	 */

out:
	/* Roll back any GUC changes executed by index functions */
	/*
	 *
	 * 回滚索引函数所做的任何 GUC 变更。
	 */
	AtEOXact_GUC(false, save_nestlevel);

	/* Restore userid and security context */
	/*
	 *
	 * 恢复用户 ID 和安全上下文。
	 */
	SetUserIdAndSecContext(save_userid, save_sec_context);

	pgstat_progress_end_command();
}

/*
 * Verify that the specified heap and index are valid to cluster on
 *
 * 确认指定的堆和索引可以用来聚类。
 *
 * Side effect: obtains lock on the index.  The caller may
 * in some cases already have AccessExclusiveLock on the table, but
 * not in all cases so we can't rely on the table-level lock for
 * protection here.
 *
 * 副作用：取得索引上的锁。调用方有时已对表持有 AccessExclusiveLock，
 * 但并非总是如此，因此这里不能依赖表级锁来保护。
 */
void
check_index_is_clusterable(Relation OldHeap, Oid indexOid, LOCKMODE lockmode)
{
	Relation	OldIndex;

	OldIndex = index_open(indexOid, lockmode);

	/*
	 * Check that index is in fact an index on the given relation
	 *
	 * 检查该索引确实是给定关系上的索引。
	 */
	if (OldIndex->rd_index == NULL ||
		OldIndex->rd_index->indrelid != RelationGetRelid(OldHeap))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not an index for table \"%s\"",
						RelationGetRelationName(OldIndex),
						RelationGetRelationName(OldHeap))));

	/* Index AM must allow clustering */
	/*
	 *
	 * 索引访问方法必须允许聚类。
	 */
	if (!OldIndex->rd_indam->amclusterable)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot cluster on index \"%s\" because access method does not support clustering",
						RelationGetRelationName(OldIndex))));

	/*
	 * Disallow clustering on incomplete indexes (those that might not index
	 * every row of the relation).  We could relax this by making a separate
	 * seqscan pass over the table to copy the missing rows, but that seems
	 * expensive and tedious.
	 *
	 * 不允许在不完整索引上聚类（可能没有索引到关系的每一行）。
	 * 可以再做一遍顺序扫描来复制缺失行，从而放宽限制，但那样既贵又麻烦。
	 */
	if (!heap_attisnull(OldIndex->rd_indextuple, Anum_pg_index_indpred, NULL))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot cluster on partial index \"%s\"",
						RelationGetRelationName(OldIndex))));

	/*
	 * Disallow if index is left over from a failed CREATE INDEX CONCURRENTLY;
	 * it might well not contain entries for every heap row, or might not even
	 * be internally consistent.  (But note that we don't check indcheckxmin;
	 * the worst consequence of following broken HOT chains would be that we
	 * might put recently-dead tuples out-of-order in the new table, and there
	 * is little harm in that.)
	 *
	 * 若索引是失败的 CREATE INDEX CONCURRENTLY 留下的，则禁止使用；
	 * 它可能没有覆盖每一行，甚至内部不一致。（但不检查 indcheckxmin；
	 * 顺着损坏的 HOT 链最坏也只是把 recently-dead 元组以乱序放入新表，危害不大。）
	 */
	if (!OldIndex->rd_index->indisvalid)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot cluster on invalid index \"%s\"",
						RelationGetRelationName(OldIndex))));

	/* Drop relcache refcnt on OldIndex, but keep lock */
	/*
	 *
	 * 放下 OldIndex 的 relcache 引用计数，但保留锁。
	 */
	index_close(OldIndex, NoLock);
}

/*
 * mark_index_clustered: mark the specified index as the one clustered on
 *
 * mark_index_clustered：把指定索引标为当前聚类索引。
 *
 * With indexOid == InvalidOid, will mark all indexes of rel not-clustered.
 *
 * 若 indexOid 为 InvalidOid，则把该关系的所有索引都标为未聚类。
 */
void
mark_index_clustered(Relation rel, Oid indexOid, bool is_internal)
{
	HeapTuple	indexTuple;
	Form_pg_index indexForm;
	Relation	pg_index;
	ListCell   *index;

	/* Disallow applying to a partitioned table */
	/*
	 *
	 * 不允许作用于分区表。
	 */
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot mark index clustered in partitioned table")));

	/*
	 * If the index is already marked clustered, no need to do anything.
	 *
	 * 若索引已被标为聚类索引，则无需再做。
	 */
	if (OidIsValid(indexOid))
	{
		if (get_index_isclustered(indexOid))
			return;
	}

	/*
	 * Check each index of the relation and set/clear the bit as needed.
	 *
	 * 检查该关系的每个索引，按需设置或清除该位。
	 */
	pg_index = table_open(IndexRelationId, RowExclusiveLock);

	foreach(index, RelationGetIndexList(rel))
	{
		Oid			thisIndexOid = lfirst_oid(index);

		indexTuple = SearchSysCacheCopy1(INDEXRELID,
										 ObjectIdGetDatum(thisIndexOid));
		if (!HeapTupleIsValid(indexTuple))
			elog(ERROR, "cache lookup failed for index %u", thisIndexOid);
		indexForm = (Form_pg_index) GETSTRUCT(indexTuple);

		/*
		 * Unset the bit if set.  We know it's wrong because we checked this
		 * earlier.
		 *
		 * 若该位已设置则清除。先前的检查已表明它是错的。
		 */
		if (indexForm->indisclustered)
		{
			indexForm->indisclustered = false;
			CatalogTupleUpdate(pg_index, &indexTuple->t_self, indexTuple);
		}
		else if (thisIndexOid == indexOid)
		{
			/* this was checked earlier, but let's be real sure */
			/*
			 *
			 * 先前已经检查过，这里再确认一次。
			 */
			if (!indexForm->indisvalid)
				elog(ERROR, "cannot cluster on invalid index %u", indexOid);
			indexForm->indisclustered = true;
			CatalogTupleUpdate(pg_index, &indexTuple->t_self, indexTuple);
		}

		InvokeObjectPostAlterHookArg(IndexRelationId, thisIndexOid, 0,
									 InvalidOid, is_internal);

		heap_freetuple(indexTuple);
	}

	table_close(pg_index, RowExclusiveLock);
}

/*
 * rebuild_relation: rebuild an existing relation in index or physical order
 *
 * rebuild_relation：按索引顺序或物理顺序重建已有关系。
 *
 * OldHeap: table to rebuild.
 * index: index to cluster by, or NULL to rewrite in physical order.
 *
 * OldHeap：要重建的表。
 * index：用于聚类的索引；为 NULL 时按物理顺序重写。
 *
 * On entry, heap and index (if one is given) must be open, and
 * AccessExclusiveLock held on them.
 * On exit, they are closed, but locks on them are not released.
 *
 * 进入时堆和索引（若给出）必须已打开，并持有 AccessExclusiveLock。
 * 退出时它们被关闭，但锁不释放。
 */
static void
rebuild_relation(Relation OldHeap, Relation index, bool verbose)
{
	Oid			tableOid = RelationGetRelid(OldHeap);
	Oid			accessMethod = OldHeap->rd_rel->relam;
	Oid			tableSpace = OldHeap->rd_rel->reltablespace;
	Oid			OIDNewHeap;
	Relation	NewHeap;
	char		relpersistence;
	bool		is_system_catalog;
	bool		swap_toast_by_content;
	TransactionId frozenXid;
	MultiXactId cutoffMulti;

	Assert(CheckRelationLockedByMe(OldHeap, AccessExclusiveLock, false) &&
		   (index == NULL || CheckRelationLockedByMe(index, AccessExclusiveLock, false)));

	if (index)
		/* Mark the correct index as clustered */
		/*
		 *
		 * 把正确的索引标为聚类索引。
		 */
		mark_index_clustered(OldHeap, RelationGetRelid(index), true);

	/* Remember info about rel before closing OldHeap */
	/*
	 *
	 * 关闭 OldHeap 之前记住关系的信息。
	 */
	relpersistence = OldHeap->rd_rel->relpersistence;
	is_system_catalog = IsSystemRelation(OldHeap);

	/*
	 * Create the transient table that will receive the re-ordered data.
	 *
	 * 创建用于接收重排数据的临时表。
	 *
	 * OldHeap is already locked, so no need to lock it again.  make_new_heap
	 * obtains AccessExclusiveLock on the new heap and its toast table.
	 *
	 * OldHeap 已经加锁，不必再锁。make_new_heap 会对新堆及其 TOAST 表取得 AccessExclusiveLock。
	 */
	OIDNewHeap = make_new_heap(tableOid, tableSpace,
							   accessMethod,
							   relpersistence,
							   NoLock);
	Assert(CheckRelationOidLockedByMe(OIDNewHeap, AccessExclusiveLock, false));
	NewHeap = table_open(OIDNewHeap, NoLock);

	/* Copy the heap data into the new table in the desired order */
	/*
	 *
	 * 按所需顺序把堆数据复制到新表。
	 */
	copy_table_data(NewHeap, OldHeap, index, verbose,
					&swap_toast_by_content, &frozenXid, &cutoffMulti);


	/* Close relcache entries, but keep lock until transaction commit */
	/*
	 *
	 * 关闭 relcache 项，但把锁保持到事务提交。
	 */
	table_close(OldHeap, NoLock);
	if (index)
		index_close(index, NoLock);

	/*
	 * Close the new relation so it can be dropped as soon as the storage is
	 * swapped. The relation is not visible to others, so no need to unlock it
	 * explicitly.
	 *
	 * 关闭新关系，以便存储交换后立刻删除它。该关系对其他会话不可见，因此不必显式解锁。
	 */
	table_close(NewHeap, NoLock);

	/*
	 * Swap the physical files of the target and transient tables, then
	 * rebuild the target's indexes and throw away the transient table.
	 *
	 * 交换目标表与临时表的物理文件，然后重建目标表索引并丢弃临时表。
	 */
	finish_heap_swap(tableOid, OIDNewHeap, is_system_catalog,
					 swap_toast_by_content, false, true,
					 frozenXid, cutoffMulti,
					 relpersistence);
}


/*
 * Create the transient table that will be filled with new data during
 * CLUSTER, ALTER TABLE, and similar operations.  The transient table
 * duplicates the logical structure of the OldHeap; but will have the
 * specified physical storage properties NewTableSpace, NewAccessMethod, and
 * relpersistence.
 *
 * 创建在 CLUSTER、ALTER TABLE 及类似操作中承接新数据的临时表。
 * 它复制 OldHeap 的逻辑结构，但使用指定的 NewTableSpace、NewAccessMethod 和 relpersistence。
 *
 * After this, the caller should load the new heap with transferred/modified
 * data, then call finish_heap_swap to complete the operation.
 *
 * 此后调用方应把转移或修改后的数据装入新堆，再调用 finish_heap_swap 完成操作。
 */
Oid
make_new_heap(Oid OIDOldHeap, Oid NewTableSpace, Oid NewAccessMethod,
			  char relpersistence, LOCKMODE lockmode)
{
	TupleDesc	OldHeapDesc;
	char		NewHeapName[NAMEDATALEN];
	Oid			OIDNewHeap;
	Oid			toastid;
	Relation	OldHeap;
	HeapTuple	tuple;
	Datum		reloptions;
	bool		isNull;
	Oid			namespaceid;

	OldHeap = table_open(OIDOldHeap, lockmode);
	OldHeapDesc = RelationGetDescr(OldHeap);

	/*
	 * Note that the NewHeap will not receive any of the defaults or
	 * constraints associated with the OldHeap; we don't need 'em, and there's
	 * no reason to spend cycles inserting them into the catalogs only to
	 * delete them.
	 *
	 * NewHeap 不会继承 OldHeap 的默认值或约束；这里不需要它们，
	 * 没必要先写入目录再删掉。
	 */

	/*
	 * But we do want to use reloptions of the old heap for new heap.
	 *
	 * 但新堆要沿用旧堆的 reloptions。
	 */
	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(OIDOldHeap));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", OIDOldHeap);
	reloptions = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions,
								 &isNull);
	if (isNull)
		reloptions = (Datum) 0;

	if (relpersistence == RELPERSISTENCE_TEMP)
		namespaceid = LookupCreationNamespace("pg_temp");
	else
		namespaceid = RelationGetNamespace(OldHeap);

	/*
	 * Create the new heap, using a temporary name in the same namespace as
	 * the existing table.  NOTE: there is some risk of collision with user
	 * relnames.  Working around this seems more trouble than it's worth; in
	 * particular, we can't create the new heap in a different namespace from
	 * the old, or we will have problems with the TEMP status of temp tables.
	 *
	 * 在与现有表相同的命名空间中用临时名称创建新堆。注意：可能与用户关系名冲突。
	 * 规避这个问题比它的价值更麻烦；尤其不能把新堆建在不同命名空间，否则临时表的 TEMP 状态会出问题。
	 *
	 * Note: the new heap is not a shared relation, even if we are rebuilding
	 * a shared rel.  However, we do make the new heap mapped if the source is
	 * mapped.  This simplifies swap_relation_files, and is absolutely
	 * necessary for rebuilding pg_class, for reasons explained there.
	 *
	 * 即使正在重建共享关系，新堆也不是共享关系。但若源是 mapped 关系，新堆也做成 mapped。
	 * 这简化了 swap_relation_files，并且对重建 pg_class 是必需的，原因见该处说明。
	 */
	snprintf(NewHeapName, sizeof(NewHeapName), "pg_temp_%u", OIDOldHeap);

	OIDNewHeap = heap_create_with_catalog(NewHeapName,
										  namespaceid,
										  NewTableSpace,
										  InvalidOid,
										  InvalidOid,
										  InvalidOid,
										  OldHeap->rd_rel->relowner,
										  NewAccessMethod,
										  OldHeapDesc,
										  NIL,
										  RELKIND_RELATION,
										  relpersistence,
										  false,
										  RelationIsMapped(OldHeap),
										  ONCOMMIT_NOOP,
										  reloptions,
										  false,
										  true,
										  true,
										  OIDOldHeap,
										  NULL);
	Assert(OIDNewHeap != InvalidOid);

	ReleaseSysCache(tuple);

	/*
	 * Advance command counter so that the newly-created relation's catalog
	 * tuples will be visible to table_open.
	 *
	 * 推进命令计数器，使新建关系的目录元组对 table_open 可见。
	 */
	CommandCounterIncrement();

	/*
	 * If necessary, create a TOAST table for the new relation.
	 *
	 * 如有必要，为新关系创建 TOAST 表。
	 *
	 * If the relation doesn't have a TOAST table already, we can't need one
	 * for the new relation.  The other way around is possible though: if some
	 * wide columns have been dropped, NewHeapCreateToastTable can decide that
	 * no TOAST table is needed for the new table.
	 *
	 * 若关系本来就没有 TOAST 表，新关系也不需要。反过来则可能：若删过宽列，
	 * NewHeapCreateToastTable 可能判定新表不需要 TOAST 表。
	 *
	 * Note that NewHeapCreateToastTable ends with CommandCounterIncrement, so
	 * that the TOAST table will be visible for insertion.
	 *
	 * NewHeapCreateToastTable 结束时会 CommandCounterIncrement，使 TOAST 表对插入可见。
	 */
	toastid = OldHeap->rd_rel->reltoastrelid;
	if (OidIsValid(toastid))
	{
		/* keep the existing toast table's reloptions, if any */
		/*
		 *
		 * 若有，则保留现有 TOAST 表的 reloptions。
		 */
		tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(toastid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for relation %u", toastid);
		reloptions = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions,
									 &isNull);
		if (isNull)
			reloptions = (Datum) 0;

		NewHeapCreateToastTable(OIDNewHeap, reloptions, lockmode, toastid);

		ReleaseSysCache(tuple);
	}

	table_close(OldHeap, NoLock);

	return OIDNewHeap;
}

/*
 * Do the physical copying of table data.
 *
 * 执行表数据的物理复制。
 *
 * There are three output parameters:
 * *pSwapToastByContent is set true if toast tables must be swapped by content.
 * *pFreezeXid receives the TransactionId used as freeze cutoff point.
 * *pCutoffMulti receives the MultiXactId used as a cutoff point.
 *
 * 有三个输出参数：
 * *pSwapToastByContent 在必须按内容交换 TOAST 表时置为 true。
 * *pFreezeXid 接收用作冻结截止点的 TransactionId。
 * *pCutoffMulti 接收用作截止点的 MultiXactId。
 */
static void
copy_table_data(Relation NewHeap, Relation OldHeap, Relation OldIndex, bool verbose,
				bool *pSwapToastByContent, TransactionId *pFreezeXid,
				MultiXactId *pCutoffMulti)
{
	Relation	relRelation;
	HeapTuple	reltup;
	Form_pg_class relform;
	TupleDesc	oldTupDesc PG_USED_FOR_ASSERTS_ONLY;
	TupleDesc	newTupDesc PG_USED_FOR_ASSERTS_ONLY;
	VacuumParams params;
	struct VacuumCutoffs cutoffs;
	bool		use_sort;
	double		num_tuples = 0,
				tups_vacuumed = 0,
				tups_recently_dead = 0;
	BlockNumber num_pages;
	int			elevel = verbose ? INFO : DEBUG2;
	PGRUsage	ru0;
	char	   *nspname;

	pg_rusage_init(&ru0);

	/* Store a copy of the namespace name for logging purposes */
	/*
	 *
	 * 保存一份命名空间名，供记日志使用。
	 */
	nspname = get_namespace_name(RelationGetNamespace(OldHeap));

	/*
	 * Their tuple descriptors should be exactly alike, but here we only need
	 * assume that they have the same number of columns.
	 *
	 * 它们的元组描述符应当完全一致，但这里只需假定列数相同。
	 */
	oldTupDesc = RelationGetDescr(OldHeap);
	newTupDesc = RelationGetDescr(NewHeap);
	Assert(newTupDesc->natts == oldTupDesc->natts);

	/*
	 * If the OldHeap has a toast table, get lock on the toast table to keep
	 * it from being vacuumed.  This is needed because autovacuum processes
	 * toast tables independently of their main tables, with no lock on the
	 * latter.  If an autovacuum were to start on the toast table after we
	 * compute our OldestXmin below, it would use a later OldestXmin, and then
	 * possibly remove as DEAD toast tuples belonging to main tuples we think
	 * are only RECENTLY_DEAD.  Then we'd fail while trying to copy those
	 * tuples.
	 *
	 * 若 OldHeap 有 TOAST 表，则锁住它以免被 vacuum。autovacuum 会独立处理 TOAST 表且不锁主表。
	 * 若在下面算出 OldestXmin 之后 autovacuum 才开始处理 TOAST 表，它会用更晚的 OldestXmin，
	 * 可能把我们认为只是 RECENTLY_DEAD 的主元组所对应的 TOAST 元组当成 DEAD 删掉，随后复制这些元组就会失败。
	 *
	 * We don't need to open the toast relation here, just lock it.  The lock
	 * will be held till end of transaction.
	 *
	 * 这里不必打开 TOAST 关系，只需加锁。锁会保持到事务结束。
	 */
	if (OldHeap->rd_rel->reltoastrelid)
		LockRelationOid(OldHeap->rd_rel->reltoastrelid, AccessExclusiveLock);

	/*
	 * If both tables have TOAST tables, perform toast swap by content.  It is
	 * possible that the old table has a toast table but the new one doesn't,
	 * if toastable columns have been dropped.  In that case we have to do
	 * swap by links.  This is okay because swap by content is only essential
	 * for system catalogs, and we don't support schema changes for them.
	 *
	 * 若两张表都有 TOAST 表，则按内容交换 TOAST。若可 TOAST 的列已被删除，旧表有 TOAST 表而新表没有，
	 * 此时必须按链接交换。这是可以接受的，因为按内容交换只对系统目录必不可少，而系统目录不支持这种模式变更。
	 */
	if (OldHeap->rd_rel->reltoastrelid && NewHeap->rd_rel->reltoastrelid)
	{
		*pSwapToastByContent = true;

		/*
		 * When doing swap by content, any toast pointers written into NewHeap
		 * must use the old toast table's OID, because that's where the toast
		 * data will eventually be found.  Set this up by setting rd_toastoid.
		 * This also tells toast_save_datum() to preserve the toast value
		 * OIDs, which we want so as not to invalidate toast pointers in
		 * system catalog caches, and to avoid making multiple copies of a
		 * single toast value.
		 *
		 * 按内容交换时，写入 NewHeap 的 TOAST 指针必须使用旧 TOAST 表的 OID，因为数据最终在那里。
		 * 通过设置 rd_toastoid 完成这一点。这也会让 toast_save_datum() 保留 TOAST 值的 OID，
		 * 以免系统目录缓存中的 TOAST 指针失效，并避免把同一 TOAST 值复制多份。
		 *
		 * Note that we must hold NewHeap open until we are done writing data,
		 * since the relcache will not guarantee to remember this setting once
		 * the relation is closed.  Also, this technique depends on the fact
		 * that no one will try to read from the NewHeap until after we've
		 * finished writing it and swapping the rels --- otherwise they could
		 * follow the toast pointers to the wrong place.  (It would actually
		 * work for values copied over from the old toast table, but not for
		 * any values that we toast which were previously not toasted.)
		 *
		 * 写完数据之前必须一直打开 NewHeap，因为关系关闭后 relcache 不保证记住该设置。
		 * 此做法还依赖：在写完并交换关系之前不会有人读 NewHeap，否则 TOAST 指针会指向错误位置。
		 * （从旧 TOAST 表复制过来的值其实没问题，但新产生的 TOAST 值则不行。）
		 */
		NewHeap->rd_toastoid = OldHeap->rd_rel->reltoastrelid;
	}
	else
		*pSwapToastByContent = false;

	/*
	 * Compute xids used to freeze and weed out dead tuples and multixacts.
	 * Since we're going to rewrite the whole table anyway, there's no reason
	 * not to be aggressive about this.
	 *
	 * 计算用于冻结并剔除死元组和 multixact 的 xid。反正要重写整张表，没有理由不做得激进一些。
	 */
	memset(&params, 0, sizeof(VacuumParams));
	vacuum_get_cutoffs(OldHeap, &params, &cutoffs);

	/*
	 * FreezeXid will become the table's new relfrozenxid, and that mustn't go
	 * backwards, so take the max.
	 *
	 * FreezeXid 将成为表的新 relfrozenxid，不能回退，因此取最大值。
	 */
	{
		TransactionId relfrozenxid = OldHeap->rd_rel->relfrozenxid;

		if (TransactionIdIsValid(relfrozenxid) &&
			TransactionIdPrecedes(cutoffs.FreezeLimit, relfrozenxid))
			cutoffs.FreezeLimit = relfrozenxid;
	}

	/*
	 * MultiXactCutoff, similarly, shouldn't go backwards either.
	 *
	 * MultiXactCutoff 同样不能回退。
	 */
	{
		MultiXactId relminmxid = OldHeap->rd_rel->relminmxid;

		if (MultiXactIdIsValid(relminmxid) &&
			MultiXactIdPrecedes(cutoffs.MultiXactCutoff, relminmxid))
			cutoffs.MultiXactCutoff = relminmxid;
	}

	/*
	 * Decide whether to use an indexscan or seqscan-and-optional-sort to scan
	 * the OldHeap.  We know how to use a sort to duplicate the ordering of a
	 * btree index, and will use seqscan-and-sort for that case if the planner
	 * tells us it's cheaper.  Otherwise, always indexscan if an index is
	 * provided, else plain seqscan.
	 *
	 * 决定用索引扫描还是顺序扫描加可选排序来扫描 OldHeap。
	 * 排序可以复现 btree 索引的顺序；若规划器认为更便宜，就用顺序扫描加排序。
	 * 否则，有索引就用索引扫描，没有则用普通顺序扫描。
	 */
	if (OldIndex != NULL && OldIndex->rd_rel->relam == BTREE_AM_OID)
		use_sort = plan_cluster_use_sort(RelationGetRelid(OldHeap),
										 RelationGetRelid(OldIndex));
	else
		use_sort = false;

	/* Log what we're doing */
	/*
	 *
	 * 记录正在做的事情。
	 */
	if (OldIndex != NULL && !use_sort)
		ereport(elevel,
				(errmsg("clustering \"%s.%s\" using index scan on \"%s\"",
						nspname,
						RelationGetRelationName(OldHeap),
						RelationGetRelationName(OldIndex))));
	else if (use_sort)
		ereport(elevel,
				(errmsg("clustering \"%s.%s\" using sequential scan and sort",
						nspname,
						RelationGetRelationName(OldHeap))));
	else
		ereport(elevel,
				(errmsg("vacuuming \"%s.%s\"",
						nspname,
						RelationGetRelationName(OldHeap))));

	/*
	 * Hand off the actual copying to AM specific function, the generic code
	 * cannot know how to deal with visibility across AMs. Note that this
	 * routine is allowed to set FreezeXid / MultiXactCutoff to different
	 * values (e.g. because the AM doesn't use freezing).
	 *
	 * 把实际复制交给访问方法的专用函数，通用代码无法处理不同 AM 的可见性。
	 * 该例程可以改写 FreezeXid / MultiXactCutoff（例如该 AM 不做冻结）。
	 */
	table_relation_copy_for_cluster(OldHeap, NewHeap, OldIndex, use_sort,
									cutoffs.OldestXmin, &cutoffs.FreezeLimit,
									&cutoffs.MultiXactCutoff,
									&num_tuples, &tups_vacuumed,
									&tups_recently_dead);

	/* return selected values to caller, get set as relfrozenxid/minmxid */
	/*
	 *
	 * 把选定的值返回给调用方，用作 relfrozenxid/minmxid。
	 */
	*pFreezeXid = cutoffs.FreezeLimit;
	*pCutoffMulti = cutoffs.MultiXactCutoff;

	/* Reset rd_toastoid just to be tidy --- it shouldn't be looked at again */
	/*
	 *
	 * 把 rd_toastoid 复位以保持整洁——之后不应再被查看。
	 */
	NewHeap->rd_toastoid = InvalidOid;

	num_pages = RelationGetNumberOfBlocks(NewHeap);

	/* Log what we did */
	/*
	 *
	 * 记录已完成的工作。
	 */
	ereport(elevel,
			(errmsg("\"%s.%s\": found %.0f removable, %.0f nonremovable row versions in %u pages",
					nspname,
					RelationGetRelationName(OldHeap),
					tups_vacuumed, num_tuples,
					RelationGetNumberOfBlocks(OldHeap)),
			 errdetail("%.0f dead row versions cannot be removed yet.\n"
					   "%s.",
					   tups_recently_dead,
					   pg_rusage_show(&ru0))));

	/* Update pg_class to reflect the correct values of pages and tuples. */
	/*
	 *
	 * 更新 pg_class，使页数和元组数正确。
	 */
	relRelation = table_open(RelationRelationId, RowExclusiveLock);

	reltup = SearchSysCacheCopy1(RELOID,
								 ObjectIdGetDatum(RelationGetRelid(NewHeap)));
	if (!HeapTupleIsValid(reltup))
		elog(ERROR, "cache lookup failed for relation %u",
			 RelationGetRelid(NewHeap));
	relform = (Form_pg_class) GETSTRUCT(reltup);

	relform->relpages = num_pages;
	relform->reltuples = num_tuples;

	/* Don't update the stats for pg_class.  See swap_relation_files. */
	/*
	 *
	 * 不要更新 pg_class 自身的统计。见 swap_relation_files。
	 */
	if (RelationGetRelid(OldHeap) != RelationRelationId)
		CatalogTupleUpdate(relRelation, &reltup->t_self, reltup);
	else
		CacheInvalidateRelcacheByTuple(reltup);

	/* Clean up. */
	/*
	 *
	 * 清理。
	 */
	heap_freetuple(reltup);
	table_close(relRelation, RowExclusiveLock);

	/* Make the update visible */
	/*
	 *
	 * 使更新可见。
	 */
	CommandCounterIncrement();
}

/*
 * Swap the physical files of two given relations.
 *
 * 交换两个给定关系的物理文件。
 *
 * We swap the physical identity (reltablespace, relfilenumber) while keeping
 * the same logical identities of the two relations.  relpersistence is also
 * swapped, which is critical since it determines where buffers live for each
 * relation.
 *
 * 交换物理身份（reltablespace、relfilenumber），同时保持两个关系的逻辑身份不变。
 * relpersistence 也要交换，这一点很关键，因为它决定每个关系的缓冲区放在哪里。
 *
 * We can swap associated TOAST data in either of two ways: recursively swap
 * the physical content of the toast tables (and their indexes), or swap the
 * TOAST links in the given relations' pg_class entries.  The former is needed
 * to manage rewrites of shared catalogs (where we cannot change the pg_class
 * links) while the latter is the only way to handle cases in which a toast
 * table is added or removed altogether.
 *
 * 关联的 TOAST 数据有两种交换方式：递归交换 TOAST 表（及其索引）的物理内容，
 * 或交换给定关系 pg_class 项中的 TOAST 链接。前者用于重写共享目录（不能改 pg_class 链接），
 * 后者是处理 TOAST 表被整体增加或删除的唯一办法。
 *
 * Additionally, the first relation is marked with relfrozenxid set to
 * frozenXid.  It seems a bit ugly to have this here, but the caller would
 * have to do it anyway, so having it here saves a heap_update.  Note: in
 * the swap-toast-links case, we assume we don't need to change the toast
 * table's relfrozenxid: the new version of the toast table should already
 * have relfrozenxid set to RecentXmin, which is good enough.
 *
 * 此外把第一个关系的 relfrozenxid 设为 frozenXid。放在这里有点别扭，但调用方反正也要做，
 * 放在这里可以省一次 heap_update。按链接交换 TOAST 时，假定不必改 TOAST 表的 relfrozenxid：
 * 新版本的 TOAST 表应已把 relfrozenxid 设为 RecentXmin，这就够了。
 *
 * Lastly, if r2 and its toast table and toast index (if any) are mapped,
 * their OIDs are emitted into mapped_tables[].  This is hacky but beats
 * having to look the information up again later in finish_heap_swap.
 *
 * 最后，若 r2 及其 TOAST 表和 TOAST 索引（如有）是 mapped 的，
 * 把它们的 OID 写入 mapped_tables[]。这样做有些取巧，但免得 finish_heap_swap 再查一次。
 */
static void
swap_relation_files(Oid r1, Oid r2, bool target_is_pg_class,
					bool swap_toast_by_content,
					bool is_internal,
					TransactionId frozenXid,
					MultiXactId cutoffMulti,
					Oid *mapped_tables)
{
	Relation	relRelation;
	HeapTuple	reltup1,
				reltup2;
	Form_pg_class relform1,
				relform2;
	RelFileNumber relfilenumber1,
				relfilenumber2;
	RelFileNumber swaptemp;
	char		swptmpchr;
	Oid			relam1,
				relam2;

	/* We need writable copies of both pg_class tuples. */
	/*
	 *
	 * 需要两份可写的 pg_class 元组副本。
	 */
	relRelation = table_open(RelationRelationId, RowExclusiveLock);

	reltup1 = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(r1));
	if (!HeapTupleIsValid(reltup1))
		elog(ERROR, "cache lookup failed for relation %u", r1);
	relform1 = (Form_pg_class) GETSTRUCT(reltup1);

	reltup2 = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(r2));
	if (!HeapTupleIsValid(reltup2))
		elog(ERROR, "cache lookup failed for relation %u", r2);
	relform2 = (Form_pg_class) GETSTRUCT(reltup2);

	relfilenumber1 = relform1->relfilenode;
	relfilenumber2 = relform2->relfilenode;
	relam1 = relform1->relam;
	relam2 = relform2->relam;

	if (RelFileNumberIsValid(relfilenumber1) &&
		RelFileNumberIsValid(relfilenumber2))
	{
		/*
		 * Normal non-mapped relations: swap relfilenumbers, reltablespaces,
		 * relpersistence
		 *
		 * 普通的非 mapped 关系：交换 relfilenumber、reltablespace 和 relpersistence。
		 */
		Assert(!target_is_pg_class);

		swaptemp = relform1->relfilenode;
		relform1->relfilenode = relform2->relfilenode;
		relform2->relfilenode = swaptemp;

		swaptemp = relform1->reltablespace;
		relform1->reltablespace = relform2->reltablespace;
		relform2->reltablespace = swaptemp;

		swaptemp = relform1->relam;
		relform1->relam = relform2->relam;
		relform2->relam = swaptemp;

		swptmpchr = relform1->relpersistence;
		relform1->relpersistence = relform2->relpersistence;
		relform2->relpersistence = swptmpchr;

		/* Also swap toast links, if we're swapping by links */
		/*
		 *
		 * 若按链接交换，则同时交换 TOAST 链接。
		 */
		if (!swap_toast_by_content)
		{
			swaptemp = relform1->reltoastrelid;
			relform1->reltoastrelid = relform2->reltoastrelid;
			relform2->reltoastrelid = swaptemp;
		}
	}
	else
	{
		/*
		 * Mapped-relation case.  Here we have to swap the relation mappings
		 * instead of modifying the pg_class columns.  Both must be mapped.
		 *
		 * mapped 关系的情况。这里要交换关系映射，而不是修改 pg_class 列。双方都必须是 mapped 的。
		 */
		if (RelFileNumberIsValid(relfilenumber1) ||
			RelFileNumberIsValid(relfilenumber2))
			elog(ERROR, "cannot swap mapped relation \"%s\" with non-mapped relation",
				 NameStr(relform1->relname));

		/*
		 * We can't change the tablespace nor persistence of a mapped rel, and
		 * we can't handle toast link swapping for one either, because we must
		 * not apply any critical changes to its pg_class row.  These cases
		 * should be prevented by upstream permissions tests, so these checks
		 * are non-user-facing emergency backstop.
		 *
		 * 不能改变 mapped 关系的表空间或持久性，也不能为它交换 TOAST 链接，
		 * 因为不能对它的 pg_class 行做任何关键修改。这些情况应由上游权限检查拦住，这里的检查是不对用户暴露的兜底。
		 */
		if (relform1->reltablespace != relform2->reltablespace)
			elog(ERROR, "cannot change tablespace of mapped relation \"%s\"",
				 NameStr(relform1->relname));
		if (relform1->relpersistence != relform2->relpersistence)
			elog(ERROR, "cannot change persistence of mapped relation \"%s\"",
				 NameStr(relform1->relname));
		if (relform1->relam != relform2->relam)
			elog(ERROR, "cannot change access method of mapped relation \"%s\"",
				 NameStr(relform1->relname));
		if (!swap_toast_by_content &&
			(relform1->reltoastrelid || relform2->reltoastrelid))
			elog(ERROR, "cannot swap toast by links for mapped relation \"%s\"",
				 NameStr(relform1->relname));

		/*
		 * Fetch the mappings --- shouldn't fail, but be paranoid
		 *
		 * 取出映射——不应失败，但仍然谨慎处理。
		 */
		relfilenumber1 = RelationMapOidToFilenumber(r1, relform1->relisshared);
		if (!RelFileNumberIsValid(relfilenumber1))
			elog(ERROR, "could not find relation mapping for relation \"%s\", OID %u",
				 NameStr(relform1->relname), r1);
		relfilenumber2 = RelationMapOidToFilenumber(r2, relform2->relisshared);
		if (!RelFileNumberIsValid(relfilenumber2))
			elog(ERROR, "could not find relation mapping for relation \"%s\", OID %u",
				 NameStr(relform2->relname), r2);

		/*
		 * Send replacement mappings to relmapper.  Note these won't actually
		 * take effect until CommandCounterIncrement.
		 *
		 * 把替换后的映射交给 relmapper。注意要到 CommandCounterIncrement 才会真正生效。
		 */
		RelationMapUpdateMap(r1, relfilenumber2, relform1->relisshared, false);
		RelationMapUpdateMap(r2, relfilenumber1, relform2->relisshared, false);

		/* Pass OIDs of mapped r2 tables back to caller */
		/*
		 *
		 * 把 mapped 的 r2 表的 OID 回传给调用方。
		 */
		*mapped_tables++ = r2;
	}

	/*
	 * Recognize that rel1's relfilenumber (swapped from rel2) is new in this
	 * subtransaction. The rel2 storage (swapped from rel1) may or may not be
	 * new.
	 *
	 * 认定 rel1 的 relfilenumber（从 rel2 换来）在本子事务中是新的。
	 * rel2 的存储（从 rel1 换来）则未必是新的。
	 */
	{
		Relation	rel1,
					rel2;

		rel1 = relation_open(r1, NoLock);
		rel2 = relation_open(r2, NoLock);
		rel2->rd_createSubid = rel1->rd_createSubid;
		rel2->rd_newRelfilelocatorSubid = rel1->rd_newRelfilelocatorSubid;
		rel2->rd_firstRelfilelocatorSubid = rel1->rd_firstRelfilelocatorSubid;
		RelationAssumeNewRelfilelocator(rel1);
		relation_close(rel1, NoLock);
		relation_close(rel2, NoLock);
	}

	/*
	 * In the case of a shared catalog, these next few steps will only affect
	 * our own database's pg_class row; but that's okay, because they are all
	 * noncritical updates.  That's also an important fact for the case of a
	 * mapped catalog, because it's possible that we'll commit the map change
	 * and then fail to commit the pg_class update.
	 *
	 * 对共享目录，下面几步只影响本数据库的 pg_class 行；这没问题，因为都是非关键更新。
	 * 对 mapped 目录这也很重要：有可能提交了映射变更，随后却没能提交 pg_class 更新。
	 */

	/* set rel1's frozen Xid and minimum MultiXid */
	/*
	 *
	 * 设置 rel1 的冻结 Xid 和最小 MultiXid。
	 */
	if (relform1->relkind != RELKIND_INDEX)
	{
		Assert(!TransactionIdIsValid(frozenXid) ||
			   TransactionIdIsNormal(frozenXid));
		relform1->relfrozenxid = frozenXid;
		relform1->relminmxid = cutoffMulti;
	}

	/* swap size statistics too, since new rel has freshly-updated stats */
	/*
	 *
	 * 同时交换大小统计，因为新关系的统计刚刚更新过。
	 */
	{
		int32		swap_pages;
		float4		swap_tuples;
		int32		swap_allvisible;
		int32		swap_allfrozen;

		swap_pages = relform1->relpages;
		relform1->relpages = relform2->relpages;
		relform2->relpages = swap_pages;

		swap_tuples = relform1->reltuples;
		relform1->reltuples = relform2->reltuples;
		relform2->reltuples = swap_tuples;

		swap_allvisible = relform1->relallvisible;
		relform1->relallvisible = relform2->relallvisible;
		relform2->relallvisible = swap_allvisible;

		swap_allfrozen = relform1->relallfrozen;
		relform1->relallfrozen = relform2->relallfrozen;
		relform2->relallfrozen = swap_allfrozen;
	}

	/*
	 * Update the tuples in pg_class --- unless the target relation of the
	 * swap is pg_class itself.  In that case, there is zero point in making
	 * changes because we'd be updating the old data that we're about to throw
	 * away.  Because the real work being done here for a mapped relation is
	 * just to change the relation map settings, it's all right to not update
	 * the pg_class rows in this case. The most important changes will instead
	 * performed later, in finish_heap_swap() itself.
	 *
	 * 更新 pg_class 中的元组——除非交换的目标就是 pg_class 本身。那种情况下修改没有意义，
	 * 因为改的是即将丢弃的旧数据。对 mapped 关系，这里真正要做的只是改关系映射，
	 * 因此可以不更新 pg_class 行。最重要的变更稍后在 finish_heap_swap() 中完成。
	 */
	if (!target_is_pg_class)
	{
		CatalogIndexState indstate;

		indstate = CatalogOpenIndexes(relRelation);
		CatalogTupleUpdateWithInfo(relRelation, &reltup1->t_self, reltup1,
								   indstate);
		CatalogTupleUpdateWithInfo(relRelation, &reltup2->t_self, reltup2,
								   indstate);
		CatalogCloseIndexes(indstate);
	}
	else
	{
		/* no update ... but we do still need relcache inval */
		/*
		 *
		 * 不更新……但仍然需要 relcache 失效。
		 */
		CacheInvalidateRelcacheByTuple(reltup1);
		CacheInvalidateRelcacheByTuple(reltup2);
	}

	/*
	 * Now that pg_class has been updated with its relevant information for
	 * the swap, update the dependency of the relations to point to their new
	 * table AM, if it has changed.
	 *
	 * pg_class 已写入交换所需的信息后，若表访问方法变了，则更新关系的依赖，使其指向新的 AM。
	 */
	if (relam1 != relam2)
	{
		if (changeDependencyFor(RelationRelationId,
								r1,
								AccessMethodRelationId,
								relam1,
								relam2) != 1)
			elog(ERROR, "could not change access method dependency for relation \"%s.%s\"",
				 get_namespace_name(get_rel_namespace(r1)),
				 get_rel_name(r1));
		if (changeDependencyFor(RelationRelationId,
								r2,
								AccessMethodRelationId,
								relam2,
								relam1) != 1)
			elog(ERROR, "could not change access method dependency for relation \"%s.%s\"",
				 get_namespace_name(get_rel_namespace(r2)),
				 get_rel_name(r2));
	}

	/*
	 * Post alter hook for modified relations. The change to r2 is always
	 * internal, but r1 depends on the invocation context.
	 *
	 * 对已修改关系调用 alter 后钩子。对 r2 的变更始终是内部的，r1 则取决于调用上下文。
	 */
	InvokeObjectPostAlterHookArg(RelationRelationId, r1, 0,
								 InvalidOid, is_internal);
	InvokeObjectPostAlterHookArg(RelationRelationId, r2, 0,
								 InvalidOid, true);

	/*
	 * If we have toast tables associated with the relations being swapped,
	 * deal with them too.
	 *
	 * 若被交换的关系有关联的 TOAST 表，也一并处理。
	 */
	if (relform1->reltoastrelid || relform2->reltoastrelid)
	{
		if (swap_toast_by_content)
		{
			if (relform1->reltoastrelid && relform2->reltoastrelid)
			{
				/* Recursively swap the contents of the toast tables */
				/*
				 *
				 * 递归交换 TOAST 表的内容。
				 */
				swap_relation_files(relform1->reltoastrelid,
									relform2->reltoastrelid,
									target_is_pg_class,
									swap_toast_by_content,
									is_internal,
									frozenXid,
									cutoffMulti,
									mapped_tables);
			}
			else
			{
				/* caller messed up */
				/*
				 *
				 * 调用方用错了。
				 */
				elog(ERROR, "cannot swap toast files by content when there's only one");
			}
		}
		else
		{
			/*
			 * We swapped the ownership links, so we need to change dependency
			 * data to match.
			 *
			 * 所有权链接已经交换，因此依赖数据也要改成一致。
			 *
			 * NOTE: it is possible that only one table has a toast table.
			 *
			 * 注意：可能只有一张表拥有 TOAST 表。
			 *
			 * NOTE: at present, a TOAST table's only dependency is the one on
			 * its owning table.  If more are ever created, we'd need to use
			 * something more selective than deleteDependencyRecordsFor() to
			 * get rid of just the link we want.
			 *
			 * 注意：目前 TOAST 表唯一的依赖就是对其所属表的依赖。若将来还有别的依赖，
			 * 就需要比 deleteDependencyRecordsFor() 更有选择性的办法，只删掉想去掉的那条链接。
			 */
			ObjectAddress baseobject,
						toastobject;
			long		count;

			/*
			 * We disallow this case for system catalogs, to avoid the
			 * possibility that the catalog we're rebuilding is one of the
			 * ones the dependency changes would change.  It's too late to be
			 * making any data changes to the target catalog.
			 *
			 * 系统目录不允许这种情况，以免正在重建的目录恰好是依赖变更会修改的那些目录之一。
			 * 此时再对目标目录做数据修改已经太晚。
			 */
			if (IsSystemClass(r1, relform1))
				elog(ERROR, "cannot swap toast files by links for system catalogs");

			/* Delete old dependencies */
			/*
			 *
			 * 删除旧依赖。
			 */
			if (relform1->reltoastrelid)
			{
				count = deleteDependencyRecordsFor(RelationRelationId,
												   relform1->reltoastrelid,
												   false);
				if (count != 1)
					elog(ERROR, "expected one dependency record for TOAST table, found %ld",
						 count);
			}
			if (relform2->reltoastrelid)
			{
				count = deleteDependencyRecordsFor(RelationRelationId,
												   relform2->reltoastrelid,
												   false);
				if (count != 1)
					elog(ERROR, "expected one dependency record for TOAST table, found %ld",
						 count);
			}

			/* Register new dependencies */
			/*
			 *
			 * 登记新依赖。
			 */
			baseobject.classId = RelationRelationId;
			baseobject.objectSubId = 0;
			toastobject.classId = RelationRelationId;
			toastobject.objectSubId = 0;

			if (relform1->reltoastrelid)
			{
				baseobject.objectId = r1;
				toastobject.objectId = relform1->reltoastrelid;
				recordDependencyOn(&toastobject, &baseobject,
								   DEPENDENCY_INTERNAL);
			}

			if (relform2->reltoastrelid)
			{
				baseobject.objectId = r2;
				toastobject.objectId = relform2->reltoastrelid;
				recordDependencyOn(&toastobject, &baseobject,
								   DEPENDENCY_INTERNAL);
			}
		}
	}

	/*
	 * If we're swapping two toast tables by content, do the same for their
	 * valid index. The swap can actually be safely done only if the relations
	 * have indexes.
	 *
	 * 若按内容交换两张 TOAST 表，对其有效索引也做同样的事。只有这些关系都有索引时，交换才是安全的。
	 */
	if (swap_toast_by_content &&
		relform1->relkind == RELKIND_TOASTVALUE &&
		relform2->relkind == RELKIND_TOASTVALUE)
	{
		Oid			toastIndex1,
					toastIndex2;

		/* Get valid index for each relation */
		/*
		 *
		 * 取得每个关系的有效索引。
		 */
		toastIndex1 = toast_get_valid_index(r1,
											AccessExclusiveLock);
		toastIndex2 = toast_get_valid_index(r2,
											AccessExclusiveLock);

		swap_relation_files(toastIndex1,
							toastIndex2,
							target_is_pg_class,
							swap_toast_by_content,
							is_internal,
							InvalidTransactionId,
							InvalidMultiXactId,
							mapped_tables);
	}

	/* Clean up. */
	/*
	 *
	 * 清理。
	 */
	heap_freetuple(reltup1);
	heap_freetuple(reltup2);

	table_close(relRelation, RowExclusiveLock);
}

/*
 * Remove the transient table that was built by make_new_heap, and finish
 * cleaning up (including rebuilding all indexes on the old heap).
 *
 * 删除 make_new_heap 建出的临时表，并完成清理（包括重建旧堆上的全部索引）。
 */
void
finish_heap_swap(Oid OIDOldHeap, Oid OIDNewHeap,
				 bool is_system_catalog,
				 bool swap_toast_by_content,
				 bool check_constraints,
				 bool is_internal,
				 TransactionId frozenXid,
				 MultiXactId cutoffMulti,
				 char newrelpersistence)
{
	ObjectAddress object;
	Oid			mapped_tables[4];
	int			reindex_flags;
	ReindexParams reindex_params = {0};
	int			i;

	/* Report that we are now swapping relation files */
	/*
	 *
	 * 报告当前正在交换关系文件。
	 */
	pgstat_progress_update_param(PROGRESS_CLUSTER_PHASE,
								 PROGRESS_CLUSTER_PHASE_SWAP_REL_FILES);

	/* Zero out possible results from swapped_relation_files */
	/*
	 *
	 * 把 swapped_relation_files 可能留下的结果清零。
	 */
	memset(mapped_tables, 0, sizeof(mapped_tables));

	/*
	 * Swap the contents of the heap relations (including any toast tables).
	 * Also set old heap's relfrozenxid to frozenXid.
	 *
	 * 交换堆关系的内容（包括任何 TOAST 表）。并把旧堆的 relfrozenxid 设为 frozenXid。
	 */
	swap_relation_files(OIDOldHeap, OIDNewHeap,
						(OIDOldHeap == RelationRelationId),
						swap_toast_by_content, is_internal,
						frozenXid, cutoffMulti, mapped_tables);

	/*
	 * If it's a system catalog, queue a sinval message to flush all catcaches
	 * on the catalog when we reach CommandCounterIncrement.
	 *
	 * 若是系统目录，则排队一条 sinval 消息，以便在 CommandCounterIncrement 时刷新该目录的全部 catcache。
	 */
	if (is_system_catalog)
		CacheInvalidateCatalog(OIDOldHeap);

	/*
	 * Rebuild each index on the relation (but not the toast table, which is
	 * all-new at this point).  It is important to do this before the DROP
	 * step because if we are processing a system catalog that will be used
	 * during DROP, we want to have its indexes available.  There is no
	 * advantage to the other order anyway because this is all transactional,
	 * so no chance to reclaim disk space before commit.  We do not need a
	 * final CommandCounterIncrement() because reindex_relation does it.
	 *
	 * 重建该关系上的每个索引（不包括 TOAST 表，它此时已是全新的）。必须在 DROP 之前做，
	 * 因为若处理的系统目录会在 DROP 期间被用到，需要它的索引可用。反过来也没有好处：
	 * 这一切都在事务中，提交前无法回收磁盘空间。不必再做最后一次 CommandCounterIncrement()，
	 * 因为 reindex_relation 会做。
	 *
	 * Note: because index_build is called via reindex_relation, it will never
	 * set indcheckxmin true for the indexes.  This is OK even though in some
	 * sense we are building new indexes rather than rebuilding existing ones,
	 * because the new heap won't contain any HOT chains at all, let alone
	 * broken ones, so it can't be necessary to set indcheckxmin.
	 *
	 * 注意：index_build 经由 reindex_relation 调用，因此不会把索引的 indcheckxmin 设为 true。
	 * 这是可以的：虽然某种意义上是在建新索引而不是重建旧索引，但新堆没有任何 HOT 链，更没有损坏的链，不必设置 indcheckxmin。
	 */
	reindex_flags = REINDEX_REL_SUPPRESS_INDEX_USE;
	if (check_constraints)
		reindex_flags |= REINDEX_REL_CHECK_CONSTRAINTS;

	/*
	 * Ensure that the indexes have the same persistence as the parent
	 * relation.
	 *
	 * 确保索引与父关系具有相同的持久性。
	 */
	if (newrelpersistence == RELPERSISTENCE_UNLOGGED)
		reindex_flags |= REINDEX_REL_FORCE_INDEXES_UNLOGGED;
	else if (newrelpersistence == RELPERSISTENCE_PERMANENT)
		reindex_flags |= REINDEX_REL_FORCE_INDEXES_PERMANENT;

	/* Report that we are now reindexing relations */
	/*
	 *
	 * 报告当前正在重建关系的索引。
	 */
	pgstat_progress_update_param(PROGRESS_CLUSTER_PHASE,
								 PROGRESS_CLUSTER_PHASE_REBUILD_INDEX);

	reindex_relation(NULL, OIDOldHeap, reindex_flags, &reindex_params);

	/* Report that we are now doing clean up */
	/*
	 *
	 * 报告当前正在做清理。
	 */
	pgstat_progress_update_param(PROGRESS_CLUSTER_PHASE,
								 PROGRESS_CLUSTER_PHASE_FINAL_CLEANUP);

	/*
	 * If the relation being rebuilt is pg_class, swap_relation_files()
	 * couldn't update pg_class's own pg_class entry (check comments in
	 * swap_relation_files()), thus relfrozenxid was not updated. That's
	 * annoying because a potential reason for doing a VACUUM FULL is a
	 * imminent or actual anti-wraparound shutdown.  So, now that we can
	 * access the new relation using its indices, update relfrozenxid.
	 * pg_class doesn't have a toast relation, so we don't need to update the
	 * corresponding toast relation. Not that there's little point moving all
	 * relfrozenxid updates here since swap_relation_files() needs to write to
	 * pg_class for non-mapped relations anyway.
	 *
	 * 若重建的是 pg_class，swap_relation_files() 无法更新 pg_class 自己的 pg_class 项
	 * （见该函数注释），因此 relfrozenxid 没有更新。这很麻烦，因为做 VACUUM FULL 的一个原因
	 * 可能就是即将或已经发生防回卷停机。现在可以通过索引访问新关系，于是更新 relfrozenxid。
	 * pg_class 没有 TOAST 关系，因此不必更新对应的 TOAST 关系。
	 * 也不必把所有 relfrozenxid 更新都挪到这里，因为对非 mapped 关系，swap_relation_files() 反正要写 pg_class。
	 */
	if (OIDOldHeap == RelationRelationId)
	{
		Relation	relRelation;
		HeapTuple	reltup;
		Form_pg_class relform;

		relRelation = table_open(RelationRelationId, RowExclusiveLock);

		reltup = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(OIDOldHeap));
		if (!HeapTupleIsValid(reltup))
			elog(ERROR, "cache lookup failed for relation %u", OIDOldHeap);
		relform = (Form_pg_class) GETSTRUCT(reltup);

		relform->relfrozenxid = frozenXid;
		relform->relminmxid = cutoffMulti;

		CatalogTupleUpdate(relRelation, &reltup->t_self, reltup);

		table_close(relRelation, RowExclusiveLock);
	}

	/* Destroy new heap with old filenumber */
	/*
	 *
	 * 销毁仍使用旧文件号的新堆。
	 */
	object.classId = RelationRelationId;
	object.objectId = OIDNewHeap;
	object.objectSubId = 0;

	/*
	 * The new relation is local to our transaction and we know nothing
	 * depends on it, so DROP_RESTRICT should be OK.
	 *
	 * 新关系只在本事务内可见，且已知没有东西依赖它，因此 DROP_RESTRICT 应当可以。
	 */
	performDeletion(&object, DROP_RESTRICT, PERFORM_DELETION_INTERNAL);

	/* performDeletion does CommandCounterIncrement at end */
	/*
	 *
	 * performDeletion 在结束时会做 CommandCounterIncrement。
	 */

	/*
	 * Now we must remove any relation mapping entries that we set up for the
	 * transient table, as well as its toast table and toast index if any. If
	 * we fail to do this before commit, the relmapper will complain about new
	 * permanent map entries being added post-bootstrap.
	 *
	 * 现在必须去掉为临时表及其 TOAST 表、TOAST 索引（如有）建立的关系映射项。
	 * 若提交前没做，relmapper 会抱怨在 bootstrap 之后又加入了新的永久映射项。
	 */
	for (i = 0; OidIsValid(mapped_tables[i]); i++)
		RelationMapRemoveMapping(mapped_tables[i]);

	/*
	 * At this point, everything is kosher except that, if we did toast swap
	 * by links, the toast table's name corresponds to the transient table.
	 * The name is irrelevant to the backend because it's referenced by OID,
	 * but users looking at the catalogs could be confused.  Rename it to
	 * prevent this problem.
	 *
	 * 至此一切正常，只有一处例外：若按链接交换了 TOAST，TOAST 表的名字仍对应临时表。
	 * 后端按 OID 引用，名字无关紧要，但查看目录的用户会困惑。将其改名以避免这个问题。
	 *
	 * Note no lock required on the relation, because we already hold an
	 * exclusive lock on it.
	 *
	 * 不必再对关系加锁，因为已经持有排他锁。
	 */
	if (!swap_toast_by_content)
	{
		Relation	newrel;

		newrel = table_open(OIDOldHeap, NoLock);
		if (OidIsValid(newrel->rd_rel->reltoastrelid))
		{
			Oid			toastidx;
			char		NewToastName[NAMEDATALEN];

			/* Get the associated valid index to be renamed */
			/*
			 *
			 * 取得需要改名的关联有效索引。
			 */
			toastidx = toast_get_valid_index(newrel->rd_rel->reltoastrelid,
											 NoLock);

			/* rename the toast table ... */
			/*
			 *
			 * 重命名 TOAST 表……
			 */
			snprintf(NewToastName, NAMEDATALEN, "pg_toast_%u",
					 OIDOldHeap);
			RenameRelationInternal(newrel->rd_rel->reltoastrelid,
								   NewToastName, true, false);

			/* ... and its valid index too. */
			/*
			 *
			 * ……以及它的有效索引。
			 */
			snprintf(NewToastName, NAMEDATALEN, "pg_toast_%u_index",
					 OIDOldHeap);

			RenameRelationInternal(toastidx,
								   NewToastName, true, true);

			/*
			 * Reset the relrewrite for the toast. The command-counter
			 * increment is required here as we are about to update the tuple
			 * that is updated as part of RenameRelationInternal.
			 *
			 * 重置 TOAST 的 relrewrite。这里必须推进命令计数器，
			 * 因为即将更新的元组在 RenameRelationInternal 中已经被更新过。
			 */
			CommandCounterIncrement();
			ResetRelRewrite(newrel->rd_rel->reltoastrelid);
		}
		relation_close(newrel, NoLock);
	}

	/* if it's not a catalog table, clear any missing attribute settings */
	/*
	 *
	 * 若不是目录表，则清除所有缺失属性设置。
	 */
	if (!is_system_catalog)
	{
		Relation	newrel;

		newrel = table_open(OIDOldHeap, NoLock);
		RelationClearMissing(newrel);
		relation_close(newrel, NoLock);
	}
}


/*
 * Get a list of tables that the current user has privileges on and
 * have indisclustered set.  Return the list in a List * of RelToCluster
 * (stored in the specified memory context), each one giving the tableOid
 * and the indexOid on which the table is already clustered.
 *
 * 取得当前用户有权限且设置了 indisclustered 的表列表。
 * 在指定内存上下文中返回 RelToCluster 的 List，每项给出 tableOid 以及该表已聚类所用的 indexOid。
 */
static List *
get_tables_to_cluster(MemoryContext cluster_context)
{
	Relation	indRelation;
	TableScanDesc scan;
	ScanKeyData entry;
	HeapTuple	indexTuple;
	Form_pg_index index;
	MemoryContext old_context;
	List	   *rtcs = NIL;

	/*
	 * Get all indexes that have indisclustered set and that the current user
	 * has the appropriate privileges for.
	 *
	 * 取得所有设置了 indisclustered 且当前用户有相应权限的索引。
	 */
	indRelation = table_open(IndexRelationId, AccessShareLock);
	ScanKeyInit(&entry,
				Anum_pg_index_indisclustered,
				BTEqualStrategyNumber, F_BOOLEQ,
				BoolGetDatum(true));
	scan = table_beginscan_catalog(indRelation, 1, &entry);
	while ((indexTuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		RelToCluster *rtc;

		index = (Form_pg_index) GETSTRUCT(indexTuple);

		if (!cluster_is_permitted_for_relation(index->indrelid, GetUserId()))
			continue;

		/* Use a permanent memory context for the result list */
		/*
		 *
		 * 结果列表使用持久的内存上下文。
		 */
		old_context = MemoryContextSwitchTo(cluster_context);

		rtc = (RelToCluster *) palloc(sizeof(RelToCluster));
		rtc->tableOid = index->indrelid;
		rtc->indexOid = index->indexrelid;
		rtcs = lappend(rtcs, rtc);

		MemoryContextSwitchTo(old_context);
	}
	table_endscan(scan);

	relation_close(indRelation, AccessShareLock);

	return rtcs;
}

/*
 * Given an index on a partitioned table, return a list of RelToCluster for
 * all the children leaves tables/indexes.
 *
 * 给定分区表上的索引，返回所有叶子子表/索引的 RelToCluster 列表。
 *
 * Like expand_vacuum_rel, but here caller must hold AccessExclusiveLock
 * on the table containing the index.
 *
 * 与 expand_vacuum_rel 类似，但这里调用方必须对包含该索引的表持有 AccessExclusiveLock。
 */
static List *
get_tables_to_cluster_partitioned(MemoryContext cluster_context, Oid indexOid)
{
	List	   *inhoids;
	ListCell   *lc;
	List	   *rtcs = NIL;
	MemoryContext old_context;

	/* Do not lock the children until they're processed */
	/*
	 *
	 * 在处理子表之前不要锁它们。
	 */
	inhoids = find_all_inheritors(indexOid, NoLock, NULL);

	foreach(lc, inhoids)
	{
		Oid			indexrelid = lfirst_oid(lc);
		Oid			relid = IndexGetRelation(indexrelid, false);
		RelToCluster *rtc;

		/* consider only leaf indexes */
		/*
		 *
		 * 只考虑叶子索引。
		 */
		if (get_rel_relkind(indexrelid) != RELKIND_INDEX)
			continue;

		/*
		 * It's possible that the user does not have privileges to CLUSTER the
		 * leaf partition despite having such privileges on the partitioned
		 * table.  We skip any partitions which the user is not permitted to
		 * CLUSTER.
		 *
		 * 用户即使对分区表有 CLUSTER 权限，也可能没有叶子分区的权限。跳过用户无权 CLUSTER 的分区。
		 */
		if (!cluster_is_permitted_for_relation(relid, GetUserId()))
			continue;

		/* Use a permanent memory context for the result list */
		/*
		 *
		 * 结果列表使用持久的内存上下文。
		 */
		old_context = MemoryContextSwitchTo(cluster_context);

		rtc = (RelToCluster *) palloc(sizeof(RelToCluster));
		rtc->tableOid = relid;
		rtc->indexOid = indexrelid;
		rtcs = lappend(rtcs, rtc);

		MemoryContextSwitchTo(old_context);
	}

	return rtcs;
}

/*
 * Return whether userid has privileges to CLUSTER relid.  If not, this
 * function emits a WARNING.
 *
 * 返回 userid 是否有权对 relid 执行 CLUSTER。若没有，本函数发出 WARNING。
 */
static bool
cluster_is_permitted_for_relation(Oid relid, Oid userid)
{
	if (pg_class_aclcheck(relid, userid, ACL_MAINTAIN) == ACLCHECK_OK)
		return true;

	ereport(WARNING,
			(errmsg("permission denied to cluster \"%s\", skipping it",
					get_rel_name(relid))));
	return false;
}

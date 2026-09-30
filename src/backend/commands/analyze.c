/*-------------------------------------------------------------------------
 *
 * analyze.c
 *	  the Postgres statistics generator
 *
 *	  PostgreSQL 的统计信息生成器。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/analyze.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/detoast.h"
#include "access/genam.h"
#include "access/multixact.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/tupconvert.h"
#include "access/visibilitymap.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/pg_inherits.h"
#include "commands/dbcommands.h"
#include "commands/progress.h"
#include "commands/tablecmds.h"
#include "commands/vacuum.h"
#include "common/pg_prng.h"
#include "executor/executor.h"
#include "foreign/fdwapi.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "pgstat.h"
#include "statistics/extended_stats_internal.h"
#include "statistics/statistics.h"
#include "storage/bufmgr.h"
#include "storage/procarray.h"
#include "utils/attoptcache.h"
#include "utils/datum.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_rusage.h"
#include "utils/sampling.h"
#include "utils/sortsupport.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"


/* Per-index data for ANALYZE */
/*
 *
 * ANALYZE 使用的逐索引数据。
 */
typedef struct AnlIndexData
{
	IndexInfo  *indexInfo;		/* BuildIndexInfo result */
	/*
	 *
	 * BuildIndexInfo 的结果。
	 */
	double		tupleFract;		/* fraction of rows for partial index */
	/*
	 *
	 * 部分索引所覆盖的行比例。
	 */
	VacAttrStats **vacattrstats;	/* index attrs to analyze */
	/*
	 *
	 * 要分析的索引属性。
	 */
	int			attr_cnt;
} AnlIndexData;


/* Default statistics target (GUC parameter) */
/*
 *
 * 默认统计目标（GUC 参数）。
 */
int			default_statistics_target = 100;

/* A few variables that don't seem worth passing around as parameters */
/*
 *
 * 几个不值得作为参数传来传去的变量。
 */
static MemoryContext anl_context = NULL;
static BufferAccessStrategy vac_strategy;


static void do_analyze_rel(Relation onerel,
						   VacuumParams *params, List *va_cols,
						   AcquireSampleRowsFunc acquirefunc, BlockNumber relpages,
						   bool inh, bool in_outer_xact, int elevel);
static void compute_index_stats(Relation onerel, double totalrows,
								AnlIndexData *indexdata, int nindexes,
								HeapTuple *rows, int numrows,
								MemoryContext col_context);
static VacAttrStats *examine_attribute(Relation onerel, int attnum,
									   Node *index_expr);
static int	acquire_sample_rows(Relation onerel, int elevel,
								HeapTuple *rows, int targrows,
								double *totalrows, double *totaldeadrows);
static int	compare_rows(const void *a, const void *b, void *arg);
static int	acquire_inherited_sample_rows(Relation onerel, int elevel,
										  HeapTuple *rows, int targrows,
										  double *totalrows, double *totaldeadrows);
static void update_attstats(Oid relid, bool inh,
							int natts, VacAttrStats **vacattrstats);
static Datum std_fetch_func(VacAttrStatsP stats, int rownum, bool *isNull);
static Datum ind_fetch_func(VacAttrStatsP stats, int rownum, bool *isNull);

/*
 * 核心流程：
 * analyze_rel() 是单表 ANALYZE 入口：加 ShareUpdateExclusiveLock、做权限与
 * relkind 检查，普通表走 do_analyze_rel()，分区表再递归子表。
 * do_analyze_rel() 决定要分析的列与索引表达式，按最坏情况确定采样行数，
 * 调用 acquire_sample_rows() 或 acquire_inherited_sample_rows() 采样，
 * 计算统计后经 update_attstats() 写入 pg_statistic，并更新 pg_class。
 * 默认类型分析在 std_typanalyze() 中选择 compute_trivial_stats()、
 * compute_distinct_stats() 或 compute_scalar_stats()。
 */


/*
 *	analyze_rel() -- analyze one relation
 *
 *	analyze_rel()：分析一个关系。
 *
 * relid identifies the relation to analyze.  If relation is supplied, use
 * the name therein for reporting any failure to open/lock the rel; do not
 * use it once we've successfully opened the rel, since it might be stale.
 *
 * relid 标识要分析的关系。若提供了 relation，则用其中的名字报告打开或加锁失败；
 * 一旦成功打开该关系就不要再使用它，因为它可能已经过时。
 */
void
analyze_rel(Oid relid, RangeVar *relation,
			VacuumParams *params, List *va_cols, bool in_outer_xact,
			BufferAccessStrategy bstrategy)
{
	Relation	onerel;
	int			elevel;
	AcquireSampleRowsFunc acquirefunc = NULL;
	BlockNumber relpages = 0;

	/* Select logging level */
	/*
	 *
	 * 选择日志级别。
	 */
	if (params->options & VACOPT_VERBOSE)
		elevel = INFO;
	else
		elevel = DEBUG2;

	/* Set up static variables */
	/*
	 *
	 * 设置静态变量。
	 */
	vac_strategy = bstrategy;

	/*
	 * Check for user-requested abort.
	 *
	 * 检查用户是否请求中止。
	 */
	CHECK_FOR_INTERRUPTS();

	/*
	 * Open the relation, getting ShareUpdateExclusiveLock to ensure that two
	 * ANALYZEs don't run on it concurrently.  (This also locks out a
	 * concurrent VACUUM, which doesn't matter much at the moment but might
	 * matter if we ever try to accumulate stats on dead tuples.) If the rel
	 * has been dropped since we last saw it, we don't need to process it.
	 *
	 * 打开关系，并取得 ShareUpdateExclusiveLock，以确保不会有两个 ANALYZE 同
	 * 时在其上运行。（这也会挡住并发的 VACUUM；目前关系不大，但若将来要累计死
	 * 元组统计就可能有关。）若自上次看到该关系以来它已被删除，则不必处理。
	 *
	 * Make sure to generate only logs for ANALYZE in this case.
	 *
	 * 这种情况下只为 ANALYZE 生成日志。
	 */
	onerel = vacuum_open_relation(relid, relation, params->options & ~(VACOPT_VACUUM),
								  params->log_min_duration >= 0,
								  ShareUpdateExclusiveLock);

	/* leave if relation could not be opened or locked */
	/*
	 *
	 * 若关系无法打开或加锁则离开。
	 */
	if (!onerel)
		return;

	/*
	 * Check if relation needs to be skipped based on privileges.  This check
	 * happens also when building the relation list to analyze for a manual
	 * operation, and needs to be done additionally here as ANALYZE could
	 * happen across multiple transactions where privileges could have changed
	 * in-between.  Make sure to generate only logs for ANALYZE in this case.
	 *
	 * 根据权限检查该关系是否应跳过。手动操作在建立待分析关系列表时也会做这项
	 * 检查，这里必须再做一次，因为 ANALYZE 可能跨越多个事务，其间权限可能已经
	 * 改变。这种情况下只为 ANALYZE 生成日志。
	 */
	if (!vacuum_is_permitted_for_relation(RelationGetRelid(onerel),
										  onerel->rd_rel,
										  params->options & ~VACOPT_VACUUM))
	{
		relation_close(onerel, ShareUpdateExclusiveLock);
		return;
	}

	/*
	 * Silently ignore tables that are temp tables of other backends ---
	 * trying to analyze these is rather pointless, since their contents are
	 * probably not up-to-date on disk.  (We don't throw a warning here; it
	 * would just lead to chatter during a database-wide ANALYZE.)
	 *
	 * 静默忽略其他后端的临时表——分析它们没什么意义，因为磁盘上的内容多半不是
	 * 最新的。（这里不发警告，否则数据库范围的 ANALYZE 会吵个不停。）
	 */
	if (RELATION_IS_OTHER_TEMP(onerel))
	{
		relation_close(onerel, ShareUpdateExclusiveLock);
		return;
	}

	/*
	 * We can ANALYZE any table except pg_statistic. See update_attstats
	 *
	 * 除 pg_statistic 外，任何表都可以 ANALYZE。见 update_attstats。
	 */
	if (RelationGetRelid(onerel) == StatisticRelationId)
	{
		relation_close(onerel, ShareUpdateExclusiveLock);
		return;
	}

	/*
	 * Check that it's of an analyzable relkind, and set up appropriately.
	 *
	 * 检查它是否属于可分析的 relkind，并做相应设置。
	 */
	if (onerel->rd_rel->relkind == RELKIND_RELATION ||
		onerel->rd_rel->relkind == RELKIND_MATVIEW)
	{
		/* Regular table, so we'll use the regular row acquisition function */
		/*
		 *
		 * 普通表，因此使用常规的行获取函数。
		 */
		acquirefunc = acquire_sample_rows;
		/* Also get regular table's size */
		/*
		 *
		 * 同时取得普通表的大小。
		 */
		relpages = RelationGetNumberOfBlocks(onerel);
	}
	else if (onerel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
	{
		/*
		 * For a foreign table, call the FDW's hook function to see whether it
		 * supports analysis.
		 *
		 * 对外部表，调用 FDW 的钩子函数，看它是否支持分析。
		 */
		FdwRoutine *fdwroutine;
		bool		ok = false;

		fdwroutine = GetFdwRoutineForRelation(onerel, false);

		if (fdwroutine->AnalyzeForeignTable != NULL)
			ok = fdwroutine->AnalyzeForeignTable(onerel,
												 &acquirefunc,
												 &relpages);

		if (!ok)
		{
			ereport(WARNING,
					(errmsg("skipping \"%s\" --- cannot analyze this foreign table",
							RelationGetRelationName(onerel))));
			relation_close(onerel, ShareUpdateExclusiveLock);
			return;
		}
	}
	else if (onerel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		/*
		 * For partitioned tables, we want to do the recursive ANALYZE below.
		 *
		 * 对分区表，下面要做递归 ANALYZE。
		 */
	}
	else
	{
		/* No need for a WARNING if we already complained during VACUUM */
		/*
		 *
		 * 若在 VACUUM 期间已经抱怨过，则不必再发 WARNING。
		 */
		if (!(params->options & VACOPT_VACUUM))
			ereport(WARNING,
					(errmsg("skipping \"%s\" --- cannot analyze non-tables or special system tables",
							RelationGetRelationName(onerel))));
		relation_close(onerel, ShareUpdateExclusiveLock);
		return;
	}

	/*
	 * OK, let's do it.  First, initialize progress reporting.
	 *
	 * 可以，开始执行。首先初始化进度报告。
	 */
	pgstat_progress_start_command(PROGRESS_COMMAND_ANALYZE,
								  RelationGetRelid(onerel));

	/*
	 * Do the normal non-recursive ANALYZE.  We can skip this for partitioned
	 * tables, which don't contain any rows.
	 *
	 * 做普通的非递归 ANALYZE。对分区表可以跳过这一步。
	 */
	if (onerel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		do_analyze_rel(onerel, params, va_cols, acquirefunc,
					   relpages, false, in_outer_xact, elevel);

	/*
	 * If there are child tables, do recursive ANALYZE.
	 *
	 * 若有子表，则做递归 ANALYZE。
	 */
	if (onerel->rd_rel->relhassubclass)
		do_analyze_rel(onerel, params, va_cols, acquirefunc, relpages,
					   true, in_outer_xact, elevel);

	/*
	 * Close source relation now, but keep lock so that no one deletes it
	 * before we commit.  (If someone did, they'd fail to clean up the entries
	 * we made in pg_statistic.  Also, releasing the lock before commit would
	 * expose us to concurrent-update failures in update_attstats.)
	 *
	 * 现在关闭源关系，但保持锁，以免在我们提交之前有人删除它。（若有人删除，
	 * 他们将无法清理我们在 pg_statistic 中写入的项。另外，在提交前释放锁会使
	 * 我们在 update_attstats 中遭遇并发更新失败。）
	 */
	relation_close(onerel, NoLock);

	pgstat_progress_end_command();
}

/*
 *	do_analyze_rel() -- analyze one relation, recursively or not
 *
 *	do_analyze_rel()：分析一个关系，可以递归也可以不递归。
 *
 * Note that "acquirefunc" is only relevant for the non-inherited case.
 * For the inherited case, acquire_inherited_sample_rows() determines the
 * appropriate acquirefunc for each child table.
 *
 * 注意 acquirefunc 只与非继承情况有关。继承情况下，
 * acquire_inherited_sample_rows() 为每个子表决定合适的 acquirefunc。
 */
static void
do_analyze_rel(Relation onerel, VacuumParams *params,
			   List *va_cols, AcquireSampleRowsFunc acquirefunc,
			   BlockNumber relpages, bool inh, bool in_outer_xact,
			   int elevel)
{
	int			attr_cnt,
				tcnt,
				i,
				ind;
	Relation   *Irel;
	int			nindexes;
	bool		verbose,
				instrument,
				hasindex;
	VacAttrStats **vacattrstats;
	AnlIndexData *indexdata;
	int			targrows,
				numrows,
				minrows;
	double		totalrows,
				totaldeadrows;
	HeapTuple  *rows;
	PGRUsage	ru0;
	TimestampTz starttime = 0;
	MemoryContext caller_context;
	Oid			save_userid;
	int			save_sec_context;
	int			save_nestlevel;
	WalUsage	startwalusage = pgWalUsage;
	BufferUsage startbufferusage = pgBufferUsage;
	BufferUsage bufferusage;
	PgStat_Counter startreadtime = 0;
	PgStat_Counter startwritetime = 0;

	verbose = (params->options & VACOPT_VERBOSE) != 0;
	instrument = (verbose || (AmAutoVacuumWorkerProcess() &&
							  params->log_min_duration >= 0));
	if (inh)
		ereport(elevel,
				(errmsg("analyzing \"%s.%s\" inheritance tree",
						get_namespace_name(RelationGetNamespace(onerel)),
						RelationGetRelationName(onerel))));
	else
		ereport(elevel,
				(errmsg("analyzing \"%s.%s\"",
						get_namespace_name(RelationGetNamespace(onerel)),
						RelationGetRelationName(onerel))));

	/*
	 * Set up a working context so that we can easily free whatever junk gets
	 * created.
	 *
	 * 建立一个工作上下文，以便轻松释放计算过程中产生的杂物。
	 */
	anl_context = AllocSetContextCreate(CurrentMemoryContext,
										"Analyze",
										ALLOCSET_DEFAULT_SIZES);
	caller_context = MemoryContextSwitchTo(anl_context);

	/*
	 * Switch to the table owner's userid, so that any index functions are run
	 * as that user.  Also lock down security-restricted operations and
	 * arrange to make GUC variable changes local to this command.
	 *
	 * 切换到表所有者的 userid，使索引函数以该用户身份运行。同时收紧安全受限操
	 * 作，并使本命令的 GUC 变更局部生效。
	 */
	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(onerel->rd_rel->relowner,
						   save_sec_context | SECURITY_RESTRICTED_OPERATION);
	save_nestlevel = NewGUCNestLevel();
	RestrictSearchPath();

	/*
	 * When verbose or autovacuum logging is used, initialize a resource usage
	 * snapshot and optionally track I/O timing.
	 *
	 * 使用 verbose 或 autovacuum 日志时，初始化资源使用对象。
	 */
	if (instrument)
	{
		if (track_io_timing)
		{
			startreadtime = pgStatBlockReadTime;
			startwritetime = pgStatBlockWriteTime;
		}

		pg_rusage_init(&ru0);
	}

	/* Used for instrumentation and stats report */
	/*
	 *
	 * 用于插桩和统计报告。
	 */
	starttime = GetCurrentTimestamp();

	/*
	 * Determine which columns to analyze
	 *
	 * 决定要分析哪些列。
	 *
	 * Note that system attributes are never analyzed, so we just reject them
	 * at the lookup stage.  We also reject duplicate column mentions.  (We
	 * could alternatively ignore duplicates, but analyzing a column twice
	 * won't work; we'd end up making a conflicting update in pg_statistic.)
	 *
	 * 注意系统属性从不分析，因此在查找阶段就拒绝它们。也拒绝重复提到的列。（
	 * 也可以改为忽略重复，但把一列分析两次行不通，最终会在 pg_statistic 中产
	 * 生冲突更新。）
	 */
	if (va_cols != NIL)
	{
		Bitmapset  *unique_cols = NULL;
		ListCell   *le;

		vacattrstats = (VacAttrStats **) palloc(list_length(va_cols) *
												sizeof(VacAttrStats *));
		tcnt = 0;
		foreach(le, va_cols)
		{
			char	   *col = strVal(lfirst(le));

			i = attnameAttNum(onerel, col, false);
			if (i == InvalidAttrNumber)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("column \"%s\" of relation \"%s\" does not exist",
								col, RelationGetRelationName(onerel))));
			if (bms_is_member(i, unique_cols))
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_COLUMN),
						 errmsg("column \"%s\" of relation \"%s\" appears more than once",
								col, RelationGetRelationName(onerel))));
			unique_cols = bms_add_member(unique_cols, i);

			vacattrstats[tcnt] = examine_attribute(onerel, i, NULL);
			if (vacattrstats[tcnt] != NULL)
				tcnt++;
		}
		attr_cnt = tcnt;
	}
	else
	{
		attr_cnt = onerel->rd_att->natts;
		vacattrstats = (VacAttrStats **)
			palloc(attr_cnt * sizeof(VacAttrStats *));
		tcnt = 0;
		for (i = 1; i <= attr_cnt; i++)
		{
			vacattrstats[tcnt] = examine_attribute(onerel, i, NULL);
			if (vacattrstats[tcnt] != NULL)
				tcnt++;
		}
		attr_cnt = tcnt;
	}

	/*
	 * Open all indexes of the relation, and see if there are any analyzable
	 * columns in the indexes.  We do not analyze index columns if there was
	 * an explicit column list in the ANALYZE command, however.
	 *
	 * 打开该关系的全部索引，查看索引中是否有可分析的列。不过若 ANALYZE 命令给
	 * 出了显式列清单，则不分析索引列。
	 *
	 * If we are doing a recursive scan, we don't want to touch the parent's
	 * indexes at all.  If we're processing a partitioned table, we need to
	 * know if there are any indexes, but we don't want to process them.
	 *
	 * 若正在做递归扫描，则完全不要碰父表的索引。若正在处理分区表，需要知道是
	 * 否存在索引，但不处理它们。
	 */
	if (onerel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		List	   *idxs = RelationGetIndexList(onerel);

		Irel = NULL;
		nindexes = 0;
		hasindex = idxs != NIL;
		list_free(idxs);
	}
	else if (!inh)
	{
		vac_open_indexes(onerel, AccessShareLock, &nindexes, &Irel);
		hasindex = nindexes > 0;
	}
	else
	{
		Irel = NULL;
		nindexes = 0;
		hasindex = false;
	}
	indexdata = NULL;
	if (nindexes > 0)
	{
		indexdata = (AnlIndexData *) palloc0(nindexes * sizeof(AnlIndexData));
		for (ind = 0; ind < nindexes; ind++)
		{
			AnlIndexData *thisdata = &indexdata[ind];
			IndexInfo  *indexInfo;

			thisdata->indexInfo = indexInfo = BuildIndexInfo(Irel[ind]);
			thisdata->tupleFract = 1.0; /* fix later if partial */
			/*
			 *
			 * 若是部分索引，稍后修正。
			 */
			if (indexInfo->ii_Expressions != NIL && va_cols == NIL)
			{
				ListCell   *indexpr_item = list_head(indexInfo->ii_Expressions);

				thisdata->vacattrstats = (VacAttrStats **)
					palloc(indexInfo->ii_NumIndexAttrs * sizeof(VacAttrStats *));
				tcnt = 0;
				for (i = 0; i < indexInfo->ii_NumIndexAttrs; i++)
				{
					int			keycol = indexInfo->ii_IndexAttrNumbers[i];

					if (keycol == 0)
					{
						/* Found an index expression */
						/*
						 *
						 * 找到一个索引表达式。
						 */
						Node	   *indexkey;

						if (indexpr_item == NULL)	/* shouldn't happen */
						/*
						 *
						 * 不应发生。
						 */
							elog(ERROR, "too few entries in indexprs list");
						indexkey = (Node *) lfirst(indexpr_item);
						indexpr_item = lnext(indexInfo->ii_Expressions,
											 indexpr_item);
						thisdata->vacattrstats[tcnt] =
							examine_attribute(Irel[ind], i + 1, indexkey);
						if (thisdata->vacattrstats[tcnt] != NULL)
							tcnt++;
					}
				}
				thisdata->attr_cnt = tcnt;
			}
		}
	}

	/*
	 * Determine how many rows we need to sample, using the worst case from
	 * all analyzable columns.  We use a lower bound of 100 rows to avoid
	 * possible overflow in Vitter's algorithm.  (Note: that will also be the
	 * target in the corner case where there are no analyzable columns.)
	 *
	 * 根据所有可分析列中的最坏情况，决定需要采样多少行。使用 100 行的下限，以
	 * 避免 Vitter 算法可能溢出。（注意：在没有任何可分析列的极端情况下，目标
	 * 也是这个值。）
	 */
	targrows = 100;
	for (i = 0; i < attr_cnt; i++)
	{
		if (targrows < vacattrstats[i]->minrows)
			targrows = vacattrstats[i]->minrows;
	}
	for (ind = 0; ind < nindexes; ind++)
	{
		AnlIndexData *thisdata = &indexdata[ind];

		for (i = 0; i < thisdata->attr_cnt; i++)
		{
			if (targrows < thisdata->vacattrstats[i]->minrows)
				targrows = thisdata->vacattrstats[i]->minrows;
		}
	}

	/*
	 * Look at extended statistics objects too, as those may define custom
	 * statistics target. So we may need to sample more rows and then build
	 * the statistics with enough detail.
	 *
	 * 也查看扩展统计对象，因为它们可能定义自定义统计目标。因此可能需要采样更
	 * 多行，再以足够的细节构建统计信息。
	 */
	minrows = ComputeExtStatisticsRows(onerel, attr_cnt, vacattrstats);

	if (targrows < minrows)
		targrows = minrows;

	/*
	 * Acquire the sample rows
	 *
	 * 获取采样行。
	 */
	rows = (HeapTuple *) palloc(targrows * sizeof(HeapTuple));
	pgstat_progress_update_param(PROGRESS_ANALYZE_PHASE,
								 inh ? PROGRESS_ANALYZE_PHASE_ACQUIRE_SAMPLE_ROWS_INH :
								 PROGRESS_ANALYZE_PHASE_ACQUIRE_SAMPLE_ROWS);
	if (inh)
		numrows = acquire_inherited_sample_rows(onerel, elevel,
												rows, targrows,
												&totalrows, &totaldeadrows);
	else
		numrows = (*acquirefunc) (onerel, elevel,
								  rows, targrows,
								  &totalrows, &totaldeadrows);

	/*
	 * Compute the statistics.  Temporary results during the calculations for
	 * each column are stored in a child context.  The calc routines are
	 * responsible to make sure that whatever they store into the VacAttrStats
	 * structure is allocated in anl_context.
	 *
	 * 计算统计信息。每一列计算过程中的临时结果存放在子上下文中。计算例程必须
	 * 保证写入 VacAttrStats 结构的内容分配在 anl_context 中。
	 */
	if (numrows > 0)
	{
		MemoryContext col_context,
					old_context;

		pgstat_progress_update_param(PROGRESS_ANALYZE_PHASE,
									 PROGRESS_ANALYZE_PHASE_COMPUTE_STATS);

		col_context = AllocSetContextCreate(anl_context,
											"Analyze Column",
											ALLOCSET_DEFAULT_SIZES);
		old_context = MemoryContextSwitchTo(col_context);

		for (i = 0; i < attr_cnt; i++)
		{
			VacAttrStats *stats = vacattrstats[i];
			AttributeOpts *aopt;

			stats->rows = rows;
			stats->tupDesc = onerel->rd_att;
			stats->compute_stats(stats,
								 std_fetch_func,
								 numrows,
								 totalrows);

			/*
			 * If the appropriate flavor of the n_distinct option is
			 * specified, override with the corresponding value.
			 *
			 * 若指定了相应风格的 n_distinct 选项，则用对应的值覆盖。
			 */
			aopt = get_attribute_options(onerel->rd_id, stats->tupattnum);
			if (aopt != NULL)
			{
				float8		n_distinct;

				n_distinct = inh ? aopt->n_distinct_inherited : aopt->n_distinct;
				if (n_distinct != 0.0)
					stats->stadistinct = n_distinct;
			}

			MemoryContextReset(col_context);
		}

		if (nindexes > 0)
			compute_index_stats(onerel, totalrows,
								indexdata, nindexes,
								rows, numrows,
								col_context);

		MemoryContextSwitchTo(old_context);
		MemoryContextDelete(col_context);

		/*
		 * Emit the completed stats rows into pg_statistic, replacing any
		 * previous statistics for the target columns.  (If there are stats in
		 * pg_statistic for columns we didn't process, we leave them alone.)
		 *
		 * 把完成的统计行写入 pg_statistic，替换目标列先前的统计。（若
		 * pg_statistic 中有我们没有处理的列的统计，则保持不动。）
		 */
		update_attstats(RelationGetRelid(onerel), inh,
						attr_cnt, vacattrstats);

		for (ind = 0; ind < nindexes; ind++)
		{
			AnlIndexData *thisdata = &indexdata[ind];

			update_attstats(RelationGetRelid(Irel[ind]), false,
							thisdata->attr_cnt, thisdata->vacattrstats);
		}

		/* Build extended statistics (if there are any). */
		/*
		 *
		 * 构建扩展统计（若有）。
		 */
		BuildRelationExtStatistics(onerel, inh, totalrows, numrows, rows,
								   attr_cnt, vacattrstats);
	}

	pgstat_progress_update_param(PROGRESS_ANALYZE_PHASE,
								 PROGRESS_ANALYZE_PHASE_FINALIZE_ANALYZE);

	/*
	 * Update pages/tuples stats in pg_class ... but not if we're doing
	 * inherited stats.
	 *
	 * 更新 pg_class 中的页数和元组数统计……但若正在做继承统计则不要更新。
	 *
	 * We assume that VACUUM hasn't set pg_class.reltuples already, even
	 * during a VACUUM ANALYZE.  Although VACUUM often updates pg_class,
	 * exceptions exist.  A "VACUUM (ANALYZE, INDEX_CLEANUP OFF)" command will
	 * never update pg_class entries for index relations.  It's also possible
	 * that an individual index's pg_class entry won't be updated during
	 * VACUUM if the index AM returns NULL from its amvacuumcleanup() routine.
	 *
	 * 假定即使在 VACUUM ANALYZE 期间，VACUUM 也还没有设置 pg_class.reltuples。
	 * 虽然 VACUUM 经常更新 pg_class，但也有例外。VACUUM (ANALYZE,
	 * INDEX_CLEANUP OFF) 永远不会更新索引关系的 pg_class 项。若索引访问方法的
	 * amvacuumcleanup() 返回 NULL，单个索引的 pg_class 项在 VACUUM 期间也可能
	 * 不更新。
	 */
	if (!inh)
	{
		BlockNumber relallvisible = 0;
		BlockNumber relallfrozen = 0;

		if (RELKIND_HAS_STORAGE(onerel->rd_rel->relkind))
			visibilitymap_count(onerel, &relallvisible, &relallfrozen);

		/*
		 * Update pg_class for table relation.  CCI first, in case acquirefunc
		 * updated pg_class.
		 *
		 * 更新表关系的 pg_class。先做 CommandCounterIncrement，以防
		 * acquirefunc 已经修改过目录。
		 */
		CommandCounterIncrement();
		vac_update_relstats(onerel,
							relpages,
							totalrows,
							relallvisible,
							relallfrozen,
							hasindex,
							InvalidTransactionId,
							InvalidMultiXactId,
							NULL, NULL,
							in_outer_xact);

		/* Same for indexes */
		/*
		 *
		 * 索引同样处理。
		 */
		for (ind = 0; ind < nindexes; ind++)
		{
			AnlIndexData *thisdata = &indexdata[ind];
			double		totalindexrows;

			totalindexrows = ceil(thisdata->tupleFract * totalrows);
			vac_update_relstats(Irel[ind],
								RelationGetNumberOfBlocks(Irel[ind]),
								totalindexrows,
								0, 0,
								false,
								InvalidTransactionId,
								InvalidMultiXactId,
								NULL, NULL,
								in_outer_xact);
		}
	}
	else if (onerel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		/*
		 * Partitioned tables don't have storage, so we don't set any fields
		 * in their pg_class entries except for reltuples and relhasindex.
		 *
		 * 分区表没有存储，因此不设置这些字段。
		 */
		CommandCounterIncrement();
		vac_update_relstats(onerel, -1, totalrows,
							0, 0, hasindex, InvalidTransactionId,
							InvalidMultiXactId,
							NULL, NULL,
							in_outer_xact);
	}

	/*
	 * Now report ANALYZE to the cumulative stats system.  For regular tables,
	 * we do it only if not doing inherited stats.  For partitioned tables, we
	 * only do it for inherited stats. (We're never called for not-inherited
	 * stats on partitioned tables anyway.)
	 *
	 * 现在向累计统计系统报告 ANALYZE。对普通表，只在不做继承统计时报告。对分
	 * 区表，只对继承统计报告。（反正我们从不会对分区表的非继承统计被调用。）
	 *
	 * Reset the changes_since_analyze counter only if we analyzed all
	 * columns; otherwise, there is still work for auto-analyze to do.
	 *
	 * 只有在分析了全部列时，才重置 changes_since_analyze 计数器。
	 */
	if (!inh)
		pgstat_report_analyze(onerel, totalrows, totaldeadrows,
							  (va_cols == NIL), starttime);
	else if (onerel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		pgstat_report_analyze(onerel, 0, 0, (va_cols == NIL), starttime);

	/*
	 * If this isn't part of VACUUM ANALYZE, let index AMs do cleanup.
	 *
	 * 若这不是 VACUUM ANALYZE 的一部分，则让索引访问方法做清理。
	 *
	 * Note that most index AMs perform a no-op as a matter of policy for
	 * amvacuumcleanup() when called in ANALYZE-only mode.  The only exception
	 * among core index AMs is GIN/ginvacuumcleanup().
	 *
	 * 注意大多数索引访问方法在仅 ANALYZE 模式下对 amvacuumcleanup() 按策略执
	 * 行空操作。核心索引访问方法中唯一的例外是 GIN 的 ginvacuumcleanup()。
	 */
	if (!(params->options & VACOPT_VACUUM))
	{
		for (ind = 0; ind < nindexes; ind++)
		{
			IndexBulkDeleteResult *stats;
			IndexVacuumInfo ivinfo;

			ivinfo.index = Irel[ind];
			ivinfo.heaprel = onerel;
			ivinfo.analyze_only = true;
			ivinfo.estimated_count = true;
			ivinfo.message_level = elevel;
			ivinfo.num_heap_tuples = onerel->rd_rel->reltuples;
			ivinfo.strategy = vac_strategy;

			stats = index_vacuum_cleanup(&ivinfo, NULL);

			if (stats)
				pfree(stats);
		}
	}

	/* Done with indexes */
	/*
	 *
	 * 索引处理完毕。
	 */
	vac_close_indexes(nindexes, Irel, NoLock);

	/* Log the action if appropriate */
	/*
	 *
	 * 在适当时记录该操作。
	 */
	if (instrument)
	{
		TimestampTz endtime = GetCurrentTimestamp();

		if (verbose || params->log_min_duration == 0 ||
			TimestampDifferenceExceeds(starttime, endtime,
									   params->log_min_duration))
		{
			long		delay_in_ms;
			WalUsage	walusage;
			double		read_rate = 0;
			double		write_rate = 0;
			char	   *msgfmt;
			StringInfoData buf;
			int64		total_blks_hit;
			int64		total_blks_read;
			int64		total_blks_dirtied;

			memset(&bufferusage, 0, sizeof(BufferUsage));
			BufferUsageAccumDiff(&bufferusage, &pgBufferUsage, &startbufferusage);
			memset(&walusage, 0, sizeof(WalUsage));
			WalUsageAccumDiff(&walusage, &pgWalUsage, &startwalusage);

			total_blks_hit = bufferusage.shared_blks_hit +
				bufferusage.local_blks_hit;
			total_blks_read = bufferusage.shared_blks_read +
				bufferusage.local_blks_read;
			total_blks_dirtied = bufferusage.shared_blks_dirtied +
				bufferusage.local_blks_dirtied;

			/*
			 * We do not expect an analyze to take > 25 days and it simplifies
			 * things a bit to use TimestampDifferenceMilliseconds.
			 *
			 * 我们不期望一次分析超过 25 天，这样简化了经过时间的计算。
			 */
			delay_in_ms = TimestampDifferenceMilliseconds(starttime, endtime);

			/*
			 * Note that we are reporting these read/write rates in the same
			 * manner as VACUUM does, which means that while the 'average read
			 * rate' here actually corresponds to page misses and resulting
			 * reads which are also picked up by track_io_timing, if enabled,
			 * the 'average write rate' is actually talking about the rate of
			 * pages being dirtied, not being written out, so it's typical to
			 * have a non-zero 'avg write rate' while I/O timings only reports
			 * reads.
			 *
			 * 注意这里报告读写速率的方式与 VACUUM 相同：这里的 average read
			 * rate 实际对应页面未命中及由此产生的读，若启用了 track_io_timing
			 * 也会被它捕捉；而 average write rate 说的是页面被弄脏的速率，不
			 * 是被写出的速率，因此常见的情况是 avg write rate 非零，而 I/O 计
			 * 时只报告读。
			 *
			 * It's not clear that an ANALYZE will ever result in
			 * FlushBuffer() being called, but we track and support reporting
			 * on I/O write time in case that changes as it's practically free
			 * to do so anyway.
			 *
			 * 目前不清楚 ANALYZE 是否会调用 FlushBuffer()，但我们仍然跟踪并支
			 * 持报告 I/O 写时间，以防将来改变，反正这样做几乎没有代价。
			 */

			if (delay_in_ms > 0)
			{
				read_rate = (double) BLCKSZ * total_blks_read /
					(1024 * 1024) / (delay_in_ms / 1000.0);
				write_rate = (double) BLCKSZ * total_blks_dirtied /
					(1024 * 1024) / (delay_in_ms / 1000.0);
			}

			/*
			 * We split this up so we don't emit empty I/O timing values when
			 * track_io_timing isn't enabled.
			 *
			 * 拆开报告，这样在没有写计时时不会输出空的 I/O 计时值。
			 */

			initStringInfo(&buf);

			if (AmAutoVacuumWorkerProcess())
				msgfmt = _("automatic analyze of table \"%s.%s.%s\"\n");
			else
				msgfmt = _("finished analyzing table \"%s.%s.%s\"\n");

			appendStringInfo(&buf, msgfmt,
							 get_database_name(MyDatabaseId),
							 get_namespace_name(RelationGetNamespace(onerel)),
							 RelationGetRelationName(onerel));
			if (track_cost_delay_timing)
			{
				/*
				 * We bypass the changecount mechanism because this value is
				 * only updated by the calling process.
				 *
				 * 绕过 changecount 机制，因为这个值只在本地使用。
				 */
				appendStringInfo(&buf, _("delay time: %.3f ms\n"),
								 (double) MyBEEntry->st_progress_param[PROGRESS_ANALYZE_DELAY_TIME] / 1000000.0);
			}
			if (track_io_timing)
			{
				double		read_ms = (double) (pgStatBlockReadTime - startreadtime) / 1000;
				double		write_ms = (double) (pgStatBlockWriteTime - startwritetime) / 1000;

				appendStringInfo(&buf, _("I/O timings: read: %.3f ms, write: %.3f ms\n"),
								 read_ms, write_ms);
			}
			appendStringInfo(&buf, _("avg read rate: %.3f MB/s, avg write rate: %.3f MB/s\n"),
							 read_rate, write_rate);
			appendStringInfo(&buf, _("buffer usage: %" PRId64 " hits, %" PRId64 " reads, %" PRId64 " dirtied\n"),
							 total_blks_hit,
							 total_blks_read,
							 total_blks_dirtied);
			appendStringInfo(&buf,
							 _("WAL usage: %" PRId64 " records, %" PRId64 " full page images, %" PRIu64 " bytes, %" PRId64 " buffers full\n"),
							 walusage.wal_records,
							 walusage.wal_fpi,
							 walusage.wal_bytes,
							 walusage.wal_buffers_full);
			appendStringInfo(&buf, _("system usage: %s"), pg_rusage_show(&ru0));

			ereport(verbose ? INFO : LOG,
					(errmsg_internal("%s", buf.data)));

			pfree(buf.data);
		}
	}

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

	/* Restore current context and release memory */
	/*
	 *
	 * 恢复当前上下文并释放内存。
	 */
	MemoryContextSwitchTo(caller_context);
	MemoryContextDelete(anl_context);
	anl_context = NULL;
}

/*
 * Compute statistics about indexes of a relation
 *
 * 计算一个关系的索引统计信息。
 */
static void
compute_index_stats(Relation onerel, double totalrows,
					AnlIndexData *indexdata, int nindexes,
					HeapTuple *rows, int numrows,
					MemoryContext col_context)
{
	MemoryContext ind_context,
				old_context;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	int			ind,
				i;

	ind_context = AllocSetContextCreate(anl_context,
										"Analyze Index",
										ALLOCSET_DEFAULT_SIZES);
	old_context = MemoryContextSwitchTo(ind_context);

	for (ind = 0; ind < nindexes; ind++)
	{
		AnlIndexData *thisdata = &indexdata[ind];
		IndexInfo  *indexInfo = thisdata->indexInfo;
		int			attr_cnt = thisdata->attr_cnt;
		TupleTableSlot *slot;
		EState	   *estate;
		ExprContext *econtext;
		ExprState  *predicate;
		Datum	   *exprvals;
		bool	   *exprnulls;
		int			numindexrows,
					tcnt,
					rowno;
		double		totalindexrows;

		/* Ignore index if no columns to analyze and not partial */
		/*
		 *
		 * 若没有要分析的列且不是部分索引，则忽略该索引。
		 */
		if (attr_cnt == 0 && indexInfo->ii_Predicate == NIL)
			continue;

		/*
		 * Need an EState for evaluation of index expressions and
		 * partial-index predicates.  Create it in the per-index context to be
		 * sure it gets cleaned up at the bottom of the loop.
		 *
		 * 需要一个 EState 来求值索引表达式和部分索引谓词。把它创建在逐索引上
		 * 下文中，以确保在循环底部被清理。
		 */
		estate = CreateExecutorState();
		econtext = GetPerTupleExprContext(estate);
		/* Need a slot to hold the current heap tuple, too */
		/*
		 *
		 * 还需要一个槽来保存当前堆元组。
		 */
		slot = MakeSingleTupleTableSlot(RelationGetDescr(onerel),
										&TTSOpsHeapTuple);

		/* Arrange for econtext's scan tuple to be the tuple under test */
		/*
		 *
		 * 让 econtext 的扫描元组成为正在测试的元组。
		 */
		econtext->ecxt_scantuple = slot;

		/* Set up execution state for predicate. */
		/*
		 *
		 * 为谓词设置执行状态。
		 */
		predicate = ExecPrepareQual(indexInfo->ii_Predicate, estate);

		/* Compute and save index expression values */
		/*
		 *
		 * 计算并保存索引表达式的值。
		 */
		exprvals = (Datum *) palloc(numrows * attr_cnt * sizeof(Datum));
		exprnulls = (bool *) palloc(numrows * attr_cnt * sizeof(bool));
		numindexrows = 0;
		tcnt = 0;
		for (rowno = 0; rowno < numrows; rowno++)
		{
			HeapTuple	heapTuple = rows[rowno];

			vacuum_delay_point(true);

			/*
			 * Reset the per-tuple context each time, to reclaim any cruft
			 * left behind by evaluating the predicate or index expressions.
			 *
			 * 每次重置逐元组上下文，以回收杂物。
			 */
			ResetExprContext(econtext);

			/* Set up for predicate or expression evaluation */
			/*
			 *
			 * 为谓词或表达式求值做准备。
			 */
			ExecStoreHeapTuple(heapTuple, slot, false);

			/* If index is partial, check predicate */
			/*
			 *
			 * 若索引是部分索引，则检查谓词。
			 */
			if (predicate != NULL)
			{
				if (!ExecQual(predicate, econtext))
					continue;
			}
			numindexrows++;

			if (attr_cnt > 0)
			{
				/*
				 * Evaluate the index row to compute expression values. We
				 * could do this by hand, but FormIndexDatum is convenient.
				 *
				 * 求值索引行以计算表达式的值。可以手工做，但 FormIndexDatum
				 * 更方便。
				 */
				FormIndexDatum(indexInfo,
							   slot,
							   estate,
							   values,
							   isnull);

				/*
				 * Save just the columns we care about.  We copy the values
				 * into ind_context from the estate's per-tuple context.
				 *
				 * 只保存我们关心的列。把值从 estate 的逐元组上下文复制到
				 * ind_context。
				 */
				for (i = 0; i < attr_cnt; i++)
				{
					VacAttrStats *stats = thisdata->vacattrstats[i];
					int			attnum = stats->tupattnum;

					if (isnull[attnum - 1])
					{
						exprvals[tcnt] = (Datum) 0;
						exprnulls[tcnt] = true;
					}
					else
					{
						exprvals[tcnt] = datumCopy(values[attnum - 1],
												   stats->attrtype->typbyval,
												   stats->attrtype->typlen);
						exprnulls[tcnt] = false;
					}
					tcnt++;
				}
			}
		}

		/*
		 * Having counted the number of rows that pass the predicate in the
		 * sample, we can estimate the total number of rows in the index.
		 *
		 * 在样本中数过通过谓词的行数之后，就可以估算索引中的总行数。
		 */
		thisdata->tupleFract = (double) numindexrows / (double) numrows;
		totalindexrows = ceil(thisdata->tupleFract * totalrows);

		/*
		 * Now we can compute the statistics for the expression columns.
		 *
		 * 现在可以计算表达式列的统计信息。
		 */
		if (numindexrows > 0)
		{
			MemoryContextSwitchTo(col_context);
			for (i = 0; i < attr_cnt; i++)
			{
				VacAttrStats *stats = thisdata->vacattrstats[i];

				stats->exprvals = exprvals + i;
				stats->exprnulls = exprnulls + i;
				stats->rowstride = attr_cnt;
				stats->compute_stats(stats,
									 ind_fetch_func,
									 numindexrows,
									 totalindexrows);

				MemoryContextReset(col_context);
			}
		}

		/* And clean up */
		/*
		 *
		 * 并清理。
		 */
		MemoryContextSwitchTo(ind_context);

		ExecDropSingleTupleTableSlot(slot);
		FreeExecutorState(estate);
		MemoryContextReset(ind_context);
	}

	MemoryContextSwitchTo(old_context);
	MemoryContextDelete(ind_context);
}

/*
 * examine_attribute -- pre-analysis of a single column
 *
 * examine_attribute：对单个列做预分析。
 *
 * Determine whether the column is analyzable; if so, create and initialize
 * a VacAttrStats struct for it.  If not, return NULL.
 *
 * 判断该列是否可分析；若是，则创建并初始化 VacAttrStats。
 *
 * If index_expr isn't NULL, then we're trying to analyze an expression index,
 * and index_expr is the expression tree representing the column's data.
 *
 * 若 index_expr 不为 NULL，则正在分析表达式索引，而不是普通列。
 */
static VacAttrStats *
examine_attribute(Relation onerel, int attnum, Node *index_expr)
{
	Form_pg_attribute attr = TupleDescAttr(onerel->rd_att, attnum - 1);
	int			attstattarget;
	HeapTuple	atttuple;
	Datum		dat;
	bool		isnull;
	HeapTuple	typtuple;
	VacAttrStats *stats;
	int			i;
	bool		ok;

	/* Never analyze dropped columns */
	/*
	 *
	 * 从不分析已删除的列。
	 */
	if (attr->attisdropped)
		return NULL;

	/* Don't analyze virtual generated columns */
	/*
	 *
	 * 不分析虚拟生成列。
	 */
	if (attr->attgenerated == ATTRIBUTE_GENERATED_VIRTUAL)
		return NULL;

	/*
	 * Get attstattarget value.  Set to -1 if null.  (Analyze functions expect
	 * -1 to mean use default_statistics_target; see for example
	 * std_typanalyze.)
	 *
	 * 取得 attstattarget 的值。若为 null 则设为 -1。（分析函数期望用 -1 表示
	 * 使用默认值。）
	 */
	atttuple = SearchSysCache2(ATTNUM, ObjectIdGetDatum(RelationGetRelid(onerel)), Int16GetDatum(attnum));
	if (!HeapTupleIsValid(atttuple))
		elog(ERROR, "cache lookup failed for attribute %d of relation %u",
			 attnum, RelationGetRelid(onerel));
	dat = SysCacheGetAttr(ATTNUM, atttuple, Anum_pg_attribute_attstattarget, &isnull);
	attstattarget = isnull ? -1 : DatumGetInt16(dat);
	ReleaseSysCache(atttuple);

	/* Don't analyze column if user has specified not to */
	/*
	 *
	 * 若用户指定不要分析该列，则不分析。
	 */
	if (attstattarget == 0)
		return NULL;

	/*
	 * Create the VacAttrStats struct.
	 *
	 * 创建 VacAttrStats 结构。
	 */
	stats = (VacAttrStats *) palloc0(sizeof(VacAttrStats));
	stats->attstattarget = attstattarget;

	/*
	 * When analyzing an expression index, believe the expression tree's type
	 * not the column datatype --- the latter might be the opckeytype storage
	 * type of the opclass, which is not interesting for our purposes.  (Note:
	 * if we did anything with non-expression index columns, we'd need to
	 * figure out where to get the correct type info from, but for now that's
	 * not a problem.)	It's not clear whether anyone will care about the
	 * typmod, but we store that too just in case.
	 *
	 * 分析表达式索引时，相信表达式树的类型，而不是列的数据类型——后者可能是操
	 * 作符类的 opckeytype 存储类型，对我们没有意义。（注意：若还要处理非表达
	 * 式索引列，就得弄清从哪里取得正确的类型信息，但目前这不是问题。）不清楚
	 * 是否有人在意 typmod，但为保险起见也把它存下来。
	 */
	if (index_expr)
	{
		stats->attrtypid = exprType(index_expr);
		stats->attrtypmod = exprTypmod(index_expr);

		/*
		 * If a collation has been specified for the index column, use that in
		 * preference to anything else; but if not, fall back to whatever we
		 * can get from the expression.
		 *
		 * 若为索引列指定了排序规则，优先使用它；否则退回到能从表达式得到的任
		 * 何排序规则。
		 */
		if (OidIsValid(onerel->rd_indcollation[attnum - 1]))
			stats->attrcollid = onerel->rd_indcollation[attnum - 1];
		else
			stats->attrcollid = exprCollation(index_expr);
	}
	else
	{
		stats->attrtypid = attr->atttypid;
		stats->attrtypmod = attr->atttypmod;
		stats->attrcollid = attr->attcollation;
	}

	typtuple = SearchSysCacheCopy1(TYPEOID,
								   ObjectIdGetDatum(stats->attrtypid));
	if (!HeapTupleIsValid(typtuple))
		elog(ERROR, "cache lookup failed for type %u", stats->attrtypid);
	stats->attrtype = (Form_pg_type) GETSTRUCT(typtuple);
	stats->anl_context = anl_context;
	stats->tupattnum = attnum;

	/*
	 * The fields describing the stats->stavalues[n] element types default to
	 * the type of the data being analyzed, but the type-specific typanalyze
	 * function can change them if it wants to store something else.
	 *
	 * 描述 stats->stavalues[n] 元素类型的字段默认是被分析数据的类型，但类型专
	 * 用的 typanalyze 函数可以按需要改成存储别的东西。
	 */
	for (i = 0; i < STATISTIC_NUM_SLOTS; i++)
	{
		stats->statypid[i] = stats->attrtypid;
		stats->statyplen[i] = stats->attrtype->typlen;
		stats->statypbyval[i] = stats->attrtype->typbyval;
		stats->statypalign[i] = stats->attrtype->typalign;
	}

	/*
	 * Call the type-specific typanalyze function.  If none is specified, use
	 * std_typanalyze().
	 *
	 * 调用类型专用的 typanalyze 函数。若未指定，则使用标准函数。
	 */
	if (OidIsValid(stats->attrtype->typanalyze))
		ok = DatumGetBool(OidFunctionCall1(stats->attrtype->typanalyze,
										   PointerGetDatum(stats)));
	else
		ok = std_typanalyze(stats);

	if (!ok || stats->compute_stats == NULL || stats->minrows <= 0)
	{
		heap_freetuple(typtuple);
		pfree(stats);
		return NULL;
	}

	return stats;
}

/*
 * Read stream callback returning the next BlockNumber as chosen by the
 * BlockSampling algorithm.
 *
 * 读流回调，返回块采样器选定的下一个 BlockNumber。
 */
static BlockNumber
block_sampling_read_stream_next(ReadStream *stream,
								void *callback_private_data,
								void *per_buffer_data)
{
	BlockSamplerData *bs = callback_private_data;

	return BlockSampler_HasMore(bs) ? BlockSampler_Next(bs) : InvalidBlockNumber;
}

/*
 * acquire_sample_rows -- acquire a random sample of rows from the table
 *
 * acquire_sample_rows：从表中获取随机采样行。
 *
 * Selected rows are returned in the caller-allocated array rows[], which
 * must have at least targrows entries.
 * The actual number of rows selected is returned as the function result.
 * We also estimate the total numbers of live and dead rows in the table,
 * and return them into *totalrows and *totaldeadrows, respectively.
 *
 * 选中的行返回到调用方分配的 rows[] 数组中，该数组至少要有 targrows 个项。实
 * 际选中的行数作为函数结果返回。同时估算表中活行和死行的总数，分别写入 *
 * totalrows 和 *totaldeadrows。
 *
 * The returned list of tuples is in order by physical position in the table.
 * (We will rely on this later to derive correlation estimates.)
 *
 * 返回的元组列表按表中的物理位置排序。
 *
 * As of May 2004 we use a new two-stage method:  Stage one selects up
 * to targrows random blocks (or all blocks, if there aren't so many).
 * Stage two scans these blocks and uses the Vitter algorithm to create
 * a random sample of targrows rows (or less, if there are less in the
 * sample of blocks).  The two stages are executed simultaneously: each
 * block is processed as soon as stage one returns its number and while
 * the rows are read stage two controls which ones are to be inserted
 * into the sample.
 *
 * 自 2004 年 5 月起使用新的两阶段方法：第一阶段选择最多 targrows 个随机块（若
 * 块没那么多则选择全部块）。第二阶段扫描这些块，并用 Vitter 算法建立 targrows
 * 行的随机样本（若采样块中的行更少则更少）。两个阶段同时执行：第一阶段一返回
 * 块号就处理该块，读行时由第二阶段决定哪些行插入样本。
 *
 * Although every row has an equal chance of ending up in the final
 * sample, this sampling method is not perfect: not every possible
 * sample has an equal chance of being selected.  For large relations
 * the number of different blocks represented by the sample tends to be
 * too small.  We can live with that for now.  Improvements are welcome.
 *
 * 虽然每一行进入最终样本的机会相等，但这种采样方法并不完美：并非每个可能的样
 * 本被选中的机会都相等。对大关系，样本所代表的不同块数往往偏少。目前可以接受。
 * 欢迎改进。
 *
 * An important property of this sampling method is that because we do
 * look at a statistically unbiased set of blocks, we should get
 * unbiased estimates of the average numbers of live and dead rows per
 * block.  The previous sampling method put too much credence in the row
 * density near the start of the table.
 *
 * 这种采样方法的一个重要性质是：因为我们查看的是统计上无偏的一组块，所以应当
 * 得到每块平均活行数和死行数的无偏估计。以前的采样方法过分相信表开头附近的行
 * 密度。
 */
static int
acquire_sample_rows(Relation onerel, int elevel,
					HeapTuple *rows, int targrows,
					double *totalrows, double *totaldeadrows)
{
	int			numrows = 0;	/* # rows now in reservoir */
	/*
	 *
	 * 蓄水池中现有的行数。
	 */
	double		samplerows = 0; /* total # rows collected */
	/*
	 *
	 * 已收集的总行数。
	 */
	double		liverows = 0;	/* # live rows seen */
	/*
	 *
	 * 见到的活行数。
	 */
	double		deadrows = 0;	/* # dead rows seen */
	/*
	 *
	 * 见到的死行数。
	 */
	double		rowstoskip = -1;	/* -1 means not set yet */
	/*
	 *
	 * -1 表示尚未设置。
	 */
	uint32		randseed;		/* Seed for block sampler(s) */
	/*
	 *
	 * 块采样器的种子。
	 */
	BlockNumber totalblocks;
	TransactionId OldestXmin;
	BlockSamplerData bs;
	ReservoirStateData rstate;
	TupleTableSlot *slot;
	TableScanDesc scan;
	BlockNumber nblocks;
	BlockNumber blksdone = 0;
	ReadStream *stream;

	Assert(targrows > 0);

	totalblocks = RelationGetNumberOfBlocks(onerel);

	/* Need a cutoff xmin for HeapTupleSatisfiesVacuum */
	/*
	 *
	 * HeapTupleSatisfiesVacuum 需要一个截止 xmin。
	 */
	OldestXmin = GetOldestNonRemovableTransactionId(onerel);

	/* Prepare for sampling block numbers */
	/*
	 *
	 * 准备采样块号。
	 */
	randseed = pg_prng_uint32(&pg_global_prng_state);
	nblocks = BlockSampler_Init(&bs, totalblocks, targrows, randseed);

	/* Report sampling block numbers */
	/*
	 *
	 * 报告正在采样的块号。
	 */
	pgstat_progress_update_param(PROGRESS_ANALYZE_BLOCKS_TOTAL,
								 nblocks);

	/* Prepare for sampling rows */
	/*
	 *
	 * 准备采样行。
	 */
	reservoir_init_selection_state(&rstate, targrows);

	scan = table_beginscan_analyze(onerel);
	slot = table_slot_create(onerel, NULL);

	/*
	 * It is safe to use batching, as block_sampling_read_stream_next never
	 * blocks.
	 *
	 * 可以安全地使用批处理，因为 block_sampling_read_stream_next 从不阻塞。
	 */
	stream = read_stream_begin_relation(READ_STREAM_MAINTENANCE |
										READ_STREAM_USE_BATCHING,
										vac_strategy,
										scan->rs_rd,
										MAIN_FORKNUM,
										block_sampling_read_stream_next,
										&bs,
										0);

	/* Outer loop over blocks to sample */
	/*
	 *
	 * 对要采样的块做外层循环。
	 */
	while (table_scan_analyze_next_block(scan, stream))
	{
		vacuum_delay_point(true);

		while (table_scan_analyze_next_tuple(scan, OldestXmin, &liverows, &deadrows, slot))
		{
			/*
			 * The first targrows sample rows are simply copied into the
			 * reservoir. Then we start replacing tuples in the sample until
			 * we reach the end of the relation.  This algorithm is from Jeff
			 * Vitter's paper (see full citation in utils/misc/sampling.c). It
			 * works by repeatedly computing the number of tuples to skip
			 * before selecting a tuple, which replaces a randomly chosen
			 * element of the reservoir (current set of tuples).  At all times
			 * the reservoir is a true random sample of the tuples we've
			 * passed over so far, so when we fall off the end of the relation
			 * we're done.
			 *
			 * 前 targrows 个采样行直接复制进蓄水池。然后开始替换样本中的元组，
			 * 直到关系结束。该算法来自 Jeff Vitter 的论文（完整引用见 utils/
			 * misc/sampling.c）。它反复计算在选中一个元组之前要跳过的元组数，
			 * 该元组替换蓄水池（当前元组集合）中随机选出的一个元素。任何时刻
			 * 蓄水池都是迄今经过的元组的真正随机样本，因此走到关系末尾就完成
			 * 了。
			 */
			if (numrows < targrows)
				rows[numrows++] = ExecCopySlotHeapTuple(slot);
			else
			{
				/*
				 * t in Vitter's paper is the number of records already
				 * processed.  If we need to compute a new S value, we must
				 * use the not-yet-incremented value of samplerows as t.
				 *
				 * Vitter 论文中的 t 是已经处理的记录数。若需要计算新的 S 值，
				 * 必须使用尚未递增的 samplerows 作为 t。
				 */
				if (rowstoskip < 0)
					rowstoskip = reservoir_get_next_S(&rstate, samplerows, targrows);

				if (rowstoskip <= 0)
				{
					/*
					 * Found a suitable tuple, so save it, replacing one old
					 * tuple at random
					 *
					 * 找到合适的元组，因此保存它，替换一个旧样本。
					 */
					int			k = (int) (targrows * sampler_random_fract(&rstate.randstate));

					Assert(k >= 0 && k < targrows);
					heap_freetuple(rows[k]);
					rows[k] = ExecCopySlotHeapTuple(slot);
				}

				rowstoskip -= 1;
			}

			samplerows += 1;
		}

		pgstat_progress_update_param(PROGRESS_ANALYZE_BLOCKS_DONE,
									 ++blksdone);
	}

	read_stream_end(stream);

	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);

	/*
	 * If we didn't find as many tuples as we wanted then we're done. No sort
	 * is needed, since they're already in order.
	 *
	 * 若找到的元组没有达到想要的数量，则到此结束。不必排序。
	 *
	 * Otherwise we need to sort the collected tuples by position
	 * (itempointer). It's not worth worrying about corner cases where the
	 * tuples are already sorted.
	 *
	 * 否则需要按位置对收集到的元组排序。
	 */
	if (numrows == targrows)
		qsort_interruptible(rows, numrows, sizeof(HeapTuple),
							compare_rows, NULL);

	/*
	 * Estimate total numbers of live and dead rows in relation, extrapolating
	 * on the assumption that the average tuple density in pages we didn't
	 * scan is the same as in the pages we did scan.  Since what we scanned is
	 * a random sample of the pages in the relation, this should be a good
	 * assumption.
	 *
	 * 估算关系中活行和死行的总数，外推时假定未扫描页的平均元组密度与已扫描页
	 * 相同。由于扫描的是关系中页的随机样本，这个假设应当是合理的。
	 */
	if (bs.m > 0)
	{
		*totalrows = floor((liverows / bs.m) * totalblocks + 0.5);
		*totaldeadrows = floor((deadrows / bs.m) * totalblocks + 0.5);
	}
	else
	{
		*totalrows = 0.0;
		*totaldeadrows = 0.0;
	}

	/*
	 * Emit some interesting relation info
	 *
	 * 输出一些有用的关系信息。
	 */
	ereport(elevel,
			(errmsg("\"%s\": scanned %d of %u pages, "
					"containing %.0f live rows and %.0f dead rows; "
					"%d rows in sample, %.0f estimated total rows",
					RelationGetRelationName(onerel),
					bs.m, totalblocks,
					liverows, deadrows,
					numrows, *totalrows)));

	return numrows;
}

/*
 * Comparator for sorting rows[] array
 *
 * 用于对 rows[] 数组排序的比较函数。
 */
static int
compare_rows(const void *a, const void *b, void *arg)
{
	HeapTuple	ha = *(const HeapTuple *) a;
	HeapTuple	hb = *(const HeapTuple *) b;
	BlockNumber ba = ItemPointerGetBlockNumber(&ha->t_self);
	OffsetNumber oa = ItemPointerGetOffsetNumber(&ha->t_self);
	BlockNumber bb = ItemPointerGetBlockNumber(&hb->t_self);
	OffsetNumber ob = ItemPointerGetOffsetNumber(&hb->t_self);

	if (ba < bb)
		return -1;
	if (ba > bb)
		return 1;
	if (oa < ob)
		return -1;
	if (oa > ob)
		return 1;
	return 0;
}


/*
 * acquire_inherited_sample_rows -- acquire sample rows from inheritance tree
 *
 * acquire_inherited_sample_rows：从继承树获取采样行。
 *
 * This has the same API as acquire_sample_rows, except that rows are
 * collected from all inheritance children as well as the specified table.
 * We fail and return zero if there are no inheritance children, or if all
 * children are foreign tables that don't support ANALYZE.
 *
 * API 与 acquire_sample_rows 相同，只是行来自所有继承子表以及指定的表。若没有
 * 继承子表，或所有子表都是不支持 ANALYZE 的外部表，则失败并返回零。
 */
static int
acquire_inherited_sample_rows(Relation onerel, int elevel,
							  HeapTuple *rows, int targrows,
							  double *totalrows, double *totaldeadrows)
{
	List	   *tableOIDs;
	Relation   *rels;
	AcquireSampleRowsFunc *acquirefuncs;
	double	   *relblocks;
	double		totalblocks;
	int			numrows,
				nrels,
				i;
	ListCell   *lc;
	bool		has_child;

	/* Initialize output parameters to zero now, in case we exit early */
	/*
	 *
	 * 现在就把输出参数初始化为零，以防提前退出。
	 */
	*totalrows = 0;
	*totaldeadrows = 0;

	/*
	 * Find all members of inheritance set.  We only need AccessShareLock on
	 * the children.
	 *
	 * 找出继承集合的全部成员。其他成员上只需要 AccessShareLock。
	 */
	tableOIDs =
		find_all_inheritors(RelationGetRelid(onerel), AccessShareLock, NULL);

	/*
	 * Check that there's at least one descendant, else fail.  This could
	 * happen despite analyze_rel's relhassubclass check, if table once had a
	 * child but no longer does.  In that case, we can clear the
	 * relhassubclass field so as not to make the same mistake again later.
	 * (This is safe because we hold ShareUpdateExclusiveLock.)
	 *
	 * 检查至少有一个后代，否则失败。即使 analyze_rel 检查过 relhassubclass，
	 * 仍可能发生这种情况：表曾经有子表但现在没有了。那种情况下可以清除
	 * relhassubclass 字段，以免以后再犯同样的错误。（这是安全的，因为我们持有
	 * ShareUpdateExclusiveLock。）
	 */
	if (list_length(tableOIDs) < 2)
	{
		/* CCI because we already updated the pg_class row in this command */
		/*
		 *
		 * 做 CommandCounterIncrement，因为本命令中已经更新过 pg_class 行。
		 */
		CommandCounterIncrement();
		SetRelationHasSubclass(RelationGetRelid(onerel), false);
		ereport(elevel,
				(errmsg("skipping analyze of \"%s.%s\" inheritance tree --- this inheritance tree contains no child tables",
						get_namespace_name(RelationGetNamespace(onerel)),
						RelationGetRelationName(onerel))));
		return 0;
	}

	/*
	 * Identify acquirefuncs to use, and count blocks in all the relations.
	 * The result could overflow BlockNumber, so we use double arithmetic.
	 *
	 * 确定要使用的 acquirefunc，并统计所有关系中的块数。
	 */
	rels = (Relation *) palloc(list_length(tableOIDs) * sizeof(Relation));
	acquirefuncs = (AcquireSampleRowsFunc *)
		palloc(list_length(tableOIDs) * sizeof(AcquireSampleRowsFunc));
	relblocks = (double *) palloc(list_length(tableOIDs) * sizeof(double));
	totalblocks = 0;
	nrels = 0;
	has_child = false;
	foreach(lc, tableOIDs)
	{
		Oid			childOID = lfirst_oid(lc);
		Relation	childrel;
		AcquireSampleRowsFunc acquirefunc = NULL;
		BlockNumber relpages = 0;

		/* We already got the needed lock */
		/*
		 *
		 * 已经取得所需的锁。
		 */
		childrel = table_open(childOID, NoLock);

		/* Ignore if temp table of another backend */
		/*
		 *
		 * 若是其他后端的临时表则忽略。
		 */
		if (RELATION_IS_OTHER_TEMP(childrel))
		{
			/* ... but release the lock on it */
			/*
			 *
			 * ……但释放它上面的锁。
			 */
			Assert(childrel != onerel);
			table_close(childrel, AccessShareLock);
			continue;
		}

		/* Check table type (MATVIEW can't happen, but might as well allow) */
		/*
		 *
		 * 检查表类型（MATVIEW 不会出现，但不妨允许）。
		 */
		if (childrel->rd_rel->relkind == RELKIND_RELATION ||
			childrel->rd_rel->relkind == RELKIND_MATVIEW)
		{
			/* Regular table, so use the regular row acquisition function */
			/*
			 *
			 * 普通表，因此使用常规的行获取函数。
			 */
			acquirefunc = acquire_sample_rows;
			relpages = RelationGetNumberOfBlocks(childrel);
		}
		else if (childrel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
		{
			/*
			 * For a foreign table, call the FDW's hook function to see
			 * whether it supports analysis.
			 *
			 * 对外部表，调用 FDW 的钩子函数，看它是否支持分析。
			 */
			FdwRoutine *fdwroutine;
			bool		ok = false;

			fdwroutine = GetFdwRoutineForRelation(childrel, false);

			if (fdwroutine->AnalyzeForeignTable != NULL)
				ok = fdwroutine->AnalyzeForeignTable(childrel,
													 &acquirefunc,
													 &relpages);

			if (!ok)
			{
				/* ignore, but release the lock on it */
				/*
				 *
				 * 忽略，但释放它上面的锁。
				 */
				Assert(childrel != onerel);
				table_close(childrel, AccessShareLock);
				continue;
			}
		}
		else
		{
			/*
			 * ignore, but release the lock on it.  don't try to unlock the
			 * passed-in relation
			 *
			 * 忽略，但释放它上面的锁。不要试图解锁传入的那个关系。
			 */
			Assert(childrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE);
			if (childrel != onerel)
				table_close(childrel, AccessShareLock);
			else
				table_close(childrel, NoLock);
			continue;
		}

		/* OK, we'll process this child */
		/*
		 *
		 * 可以，将处理这个子表。
		 */
		has_child = true;
		rels[nrels] = childrel;
		acquirefuncs[nrels] = acquirefunc;
		relblocks[nrels] = (double) relpages;
		totalblocks += (double) relpages;
		nrels++;
	}

	/*
	 * If we don't have at least one child table to consider, fail.  If the
	 * relation is a partitioned table, it's not counted as a child table.
	 *
	 * 若至少没有一个可考虑的子表，则失败。若调用方要求只分析继承树，则报告这
	 * 一点。
	 */
	if (!has_child)
	{
		ereport(elevel,
				(errmsg("skipping analyze of \"%s.%s\" inheritance tree --- this inheritance tree contains no analyzable child tables",
						get_namespace_name(RelationGetNamespace(onerel)),
						RelationGetRelationName(onerel))));
		return 0;
	}

	/*
	 * Now sample rows from each relation, proportionally to its fraction of
	 * the total block count.  (This might be less than desirable if the child
	 * rels have radically different free-space percentages, but it's not
	 * clear that it's worth working harder.)
	 *
	 * 现在按每个关系占总块数的比例，从各关系采样行。（若子关系的空闲空间比例
	 * 相差极大，这可能不那么理想，但不清楚是否值得更费力。）
	 */
	pgstat_progress_update_param(PROGRESS_ANALYZE_CHILD_TABLES_TOTAL,
								 nrels);
	numrows = 0;
	for (i = 0; i < nrels; i++)
	{
		Relation	childrel = rels[i];
		AcquireSampleRowsFunc acquirefunc = acquirefuncs[i];
		double		childblocks = relblocks[i];

		/*
		 * Report progress.  The sampling function will normally report blocks
		 * done/total, but we need to reset them to 0 here, so that they don't
		 * show an old value until that.
		 *
		 * 报告进度。采样函数通常会报告已完成块数和总块数，但这里需要把它们重
		 * 置为 0，以免在那之前显示旧值。
		 */
		{
			const int	progress_index[] = {
				PROGRESS_ANALYZE_CURRENT_CHILD_TABLE_RELID,
				PROGRESS_ANALYZE_BLOCKS_DONE,
				PROGRESS_ANALYZE_BLOCKS_TOTAL
			};
			const int64 progress_vals[] = {
				RelationGetRelid(childrel),
				0,
				0,
			};

			pgstat_progress_update_multi_param(3, progress_index, progress_vals);
		}

		if (childblocks > 0)
		{
			int			childtargrows;

			childtargrows = (int) rint(targrows * childblocks / totalblocks);
			/* Make sure we don't overrun due to roundoff error */
			/*
			 *
			 * 确保不会因舍入误差而越界。
			 */
			childtargrows = Min(childtargrows, targrows - numrows);
			if (childtargrows > 0)
			{
				int			childrows;
				double		trows,
							tdrows;

				/* Fetch a random sample of the child's rows */
				/*
				 *
				 * 获取该子表行的随机样本。
				 */
				childrows = (*acquirefunc) (childrel, elevel,
											rows + numrows, childtargrows,
											&trows, &tdrows);

				/* We may need to convert from child's rowtype to parent's */
				/*
				 *
				 * 可能需要把子表的行类型转换成父表的行类型。
				 */
				if (childrows > 0 &&
					!equalRowTypes(RelationGetDescr(childrel),
								   RelationGetDescr(onerel)))
				{
					TupleConversionMap *map;

					map = convert_tuples_by_name(RelationGetDescr(childrel),
												 RelationGetDescr(onerel));
					if (map != NULL)
					{
						int			j;

						for (j = 0; j < childrows; j++)
						{
							HeapTuple	newtup;

							newtup = execute_attr_map_tuple(rows[numrows + j], map);
							heap_freetuple(rows[numrows + j]);
							rows[numrows + j] = newtup;
						}
						free_conversion_map(map);
					}
				}

				/* And add to counts */
				/*
				 *
				 * 并加入计数。
				 */
				numrows += childrows;
				*totalrows += trows;
				*totaldeadrows += tdrows;
			}
		}

		/*
		 * Note: we cannot release the child-table locks, since we may have
		 * pointers to their TOAST tables in the sampled rows.
		 *
		 * 注意：不能释放子表锁，因为可能还持有指向这些行的指针。
		 */
		table_close(childrel, NoLock);
		pgstat_progress_update_param(PROGRESS_ANALYZE_CHILD_TABLES_DONE,
									 i + 1);
	}

	return numrows;
}


/*
 *	update_attstats() -- update attribute statistics for one relation
 *
 *	update_attstats()：更新一个关系的属性统计信息。
 *
 *		Statistics are stored in several places: the pg_class row for the
 *		relation has stats about the whole relation, and there is a
 *		pg_statistic row for each (non-system) attribute that has ever
 *		been analyzed.  The pg_class values are updated by VACUUM, not here.
 *
 *		统计信息存放在多处：关系的 pg_class 行保存整个关系的统计，每个曾经被分
 *		析过的（非系统）属性各有一行 pg_statistic。pg_class 的值由 VACUUM 更新，
 *		不是在这里。
 *
 *		pg_statistic rows are just added or updated normally.  This means
 *		that pg_statistic will probably contain some deleted rows at the
 *		completion of a vacuum cycle, unless it happens to get vacuumed last.
 *
 *		pg_statistic 行只是正常地添加或更新。这意味着除非 pg_statistic 碰巧最
 *		后被清理，否则一次 vacuum 周期结束时它里面可能含有一些已删除的行。
 *
 *		To keep things simple, we punt for pg_statistic, and don't try
 *		to compute or store rows for pg_statistic itself in pg_statistic.
 *		This could possibly be made to work, but it's not worth the trouble.
 *		Note analyze_rel() has seen to it that we won't come here when
 *		vacuuming pg_statistic itself.
 *
 *		为了简单，对 pg_statistic 本身放弃处理，不尝试在 pg_statistic 中计算或
 *		存储它自己的行。这也许能做成，但不值得费这个劲。注意 analyze_rel() 已
 *		经保证在清理 pg_statistic 自身时不会走到这里。
 *
 *		Note: there would be a race condition here if two backends could
 *		ANALYZE the same table concurrently.  Presently, we lock that out
 *		by taking a self-exclusive lock on the relation in analyze_rel().
 *
 *		注意：若两个后端能并发 ANALYZE 同一张表，这里会有竞争。目前我们在
 *		analyze_rel() 中对该关系取得自排他锁，从而排除这种情况。
 */
static void
update_attstats(Oid relid, bool inh, int natts, VacAttrStats **vacattrstats)
{
	Relation	sd;
	int			attno;
	CatalogIndexState indstate = NULL;

	if (natts <= 0)
		return;					/* nothing to do */
		/*
		 *
		 * 无事可做。
		 */

	sd = table_open(StatisticRelationId, RowExclusiveLock);

	for (attno = 0; attno < natts; attno++)
	{
		VacAttrStats *stats = vacattrstats[attno];
		HeapTuple	stup,
					oldtup;
		int			i,
					k,
					n;
		Datum		values[Natts_pg_statistic];
		bool		nulls[Natts_pg_statistic];
		bool		replaces[Natts_pg_statistic];

		/* Ignore attr if we weren't able to collect stats */
		/*
		 *
		 * 若未能收集到统计，则忽略该属性。
		 */
		if (!stats->stats_valid)
			continue;

		/*
		 * Construct a new pg_statistic tuple
		 *
		 * 构造一个新的 pg_statistic 元组。
		 */
		for (i = 0; i < Natts_pg_statistic; ++i)
		{
			nulls[i] = false;
			replaces[i] = true;
		}

		values[Anum_pg_statistic_starelid - 1] = ObjectIdGetDatum(relid);
		values[Anum_pg_statistic_staattnum - 1] = Int16GetDatum(stats->tupattnum);
		values[Anum_pg_statistic_stainherit - 1] = BoolGetDatum(inh);
		values[Anum_pg_statistic_stanullfrac - 1] = Float4GetDatum(stats->stanullfrac);
		values[Anum_pg_statistic_stawidth - 1] = Int32GetDatum(stats->stawidth);
		values[Anum_pg_statistic_stadistinct - 1] = Float4GetDatum(stats->stadistinct);
		i = Anum_pg_statistic_stakind1 - 1;
		for (k = 0; k < STATISTIC_NUM_SLOTS; k++)
		{
			values[i++] = Int16GetDatum(stats->stakind[k]); /* stakindN */
			/*
			 *
			 * stakindN 槽。
			 */
		}
		i = Anum_pg_statistic_staop1 - 1;
		for (k = 0; k < STATISTIC_NUM_SLOTS; k++)
		{
			values[i++] = ObjectIdGetDatum(stats->staop[k]);	/* staopN */
			/*
			 *
			 * staopN 槽。
			 */
		}
		i = Anum_pg_statistic_stacoll1 - 1;
		for (k = 0; k < STATISTIC_NUM_SLOTS; k++)
		{
			values[i++] = ObjectIdGetDatum(stats->stacoll[k]);	/* stacollN */
			/*
			 *
			 * stacollN 槽。
			 */
		}
		i = Anum_pg_statistic_stanumbers1 - 1;
		for (k = 0; k < STATISTIC_NUM_SLOTS; k++)
		{
			int			nnum = stats->numnumbers[k];

			if (nnum > 0)
			{
				Datum	   *numdatums = (Datum *) palloc(nnum * sizeof(Datum));
				ArrayType  *arry;

				for (n = 0; n < nnum; n++)
					numdatums[n] = Float4GetDatum(stats->stanumbers[k][n]);
				arry = construct_array_builtin(numdatums, nnum, FLOAT4OID);
				values[i++] = PointerGetDatum(arry);	/* stanumbersN */
				/*
				 *
				 * stanumbersN 槽。
				 */
			}
			else
			{
				nulls[i] = true;
				values[i++] = (Datum) 0;
			}
		}
		i = Anum_pg_statistic_stavalues1 - 1;
		for (k = 0; k < STATISTIC_NUM_SLOTS; k++)
		{
			if (stats->numvalues[k] > 0)
			{
				ArrayType  *arry;

				arry = construct_array(stats->stavalues[k],
									   stats->numvalues[k],
									   stats->statypid[k],
									   stats->statyplen[k],
									   stats->statypbyval[k],
									   stats->statypalign[k]);
				values[i++] = PointerGetDatum(arry);	/* stavaluesN */
				/*
				 *
				 * stavaluesN 槽。
				 */
			}
			else
			{
				nulls[i] = true;
				values[i++] = (Datum) 0;
			}
		}

		/* Is there already a pg_statistic tuple for this attribute? */
		/*
		 *
		 * 该属性是否已有 pg_statistic 元组？
		 */
		oldtup = SearchSysCache3(STATRELATTINH,
								 ObjectIdGetDatum(relid),
								 Int16GetDatum(stats->tupattnum),
								 BoolGetDatum(inh));

		/* Open index information when we know we need it */
		/*
		 *
		 * 确定需要时再打开索引信息。
		 */
		if (indstate == NULL)
			indstate = CatalogOpenIndexes(sd);

		if (HeapTupleIsValid(oldtup))
		{
			/* Yes, replace it */
			/*
			 *
			 * 有，则替换它。
			 */
			stup = heap_modify_tuple(oldtup,
									 RelationGetDescr(sd),
									 values,
									 nulls,
									 replaces);
			ReleaseSysCache(oldtup);
			CatalogTupleUpdateWithInfo(sd, &stup->t_self, stup, indstate);
		}
		else
		{
			/* No, insert new tuple */
			/*
			 *
			 * 没有，则插入新元组。
			 */
			stup = heap_form_tuple(RelationGetDescr(sd), values, nulls);
			CatalogTupleInsertWithInfo(sd, stup, indstate);
		}

		heap_freetuple(stup);
	}

	if (indstate != NULL)
		CatalogCloseIndexes(indstate);
	table_close(sd, RowExclusiveLock);
}

/*
 * Standard fetch function for use by compute_stats subroutines.
 *
 * 供 compute_stats 子程序使用的标准获取函数。
 *
 * This exists to provide some insulation between compute_stats routines
 * and the actual storage of the sample data.
 *
 * 它的存在是为了在 compute_stats 例程和采样行的存储方式之间提供一层隔离。
 */
static Datum
std_fetch_func(VacAttrStatsP stats, int rownum, bool *isNull)
{
	int			attnum = stats->tupattnum;
	HeapTuple	tuple = stats->rows[rownum];
	TupleDesc	tupDesc = stats->tupDesc;

	return heap_getattr(tuple, attnum, tupDesc, isNull);
}

/*
 * Fetch function for analyzing index expressions.
 *
 * 分析索引表达式时使用的获取函数。
 *
 * We have not bothered to construct index tuples, instead the data is
 * just in Datum arrays.
 *
 * 我们没有费心构造索引元组，数据直接放在列数组里。
 */
static Datum
ind_fetch_func(VacAttrStatsP stats, int rownum, bool *isNull)
{
	int			i;

	/* exprvals and exprnulls are already offset for proper column */
	/*
	 *
	 * exprvals 和 exprnulls 已经按正确的列做了偏移。
	 */
	i = rownum * stats->rowstride;
	*isNull = stats->exprnulls[i];
	return stats->exprvals[i];
}


/*==========================================================================
 *
 * Code below this point represents the "standard" type-specific statistics
 * analysis algorithms.  This code can be replaced on a per-data-type basis
 * by setting a nonzero value in pg_type.typanalyze.
 *
 * 此线以下是标准的类型专用统计分析算法。可以为每个数据类型替换这段代码，办法
 * 是在 pg_type.typanalyze 中设置非零值。
 *
 *==========================================================================
 */


/*
 * To avoid consuming too much memory during analysis and/or too much space
 * in the resulting pg_statistic rows, we ignore varlena datums that are wider
 * than WIDTH_THRESHOLD (after detoasting!).  This is legitimate for MCV
 * and distinct-value calculations since a wide value is unlikely to be
 * duplicated at all, much less be a most-common value.  For the same reason,
 * ignoring wide values will not affect our estimates of histogram bin
 * boundaries very much.
 *
 * 为避免分析期间消耗过多内存，以及/或者在结果 pg_statistic 行中占用过多空间，
 * 我们忽略宽于 WIDTH_THRESHOLD 的 varlena datum（在 detoast 之后）。这对 MCV
 * 和不同值计算是合理的，因为很宽的值几乎不可能重复，更不可能成为最常见值。出
 * 于同样的原因，忽略宽值也不会太影响直方图桶边界的估计。
 */
#define WIDTH_THRESHOLD  1024

#define swapInt(a,b)	do {int _tmp; _tmp=a; a=b; b=_tmp;} while(0)
#define swapDatum(a,b)	do {Datum _tmp; _tmp=a; a=b; b=_tmp;} while(0)

/*
 * Extra information used by the default analysis routines
 *
 * 默认分析例程使用的额外信息。
 */
typedef struct
{
	int			count;			/* # of duplicates */
	/*
	 *
	 * 重复次数。
	 */
	int			first;			/* values[] index of first occurrence */
	/*
	 *
	 * 第一次出现处的 values[] 下标。
	 */
} ScalarMCVItem;

typedef struct
{
	SortSupport ssup;
	int		   *tupnoLink;
} CompareScalarsContext;


static void compute_trivial_stats(VacAttrStatsP stats,
								  AnalyzeAttrFetchFunc fetchfunc,
								  int samplerows,
								  double totalrows);
static void compute_distinct_stats(VacAttrStatsP stats,
								   AnalyzeAttrFetchFunc fetchfunc,
								   int samplerows,
								   double totalrows);
static void compute_scalar_stats(VacAttrStatsP stats,
								 AnalyzeAttrFetchFunc fetchfunc,
								 int samplerows,
								 double totalrows);
static int	compare_scalars(const void *a, const void *b, void *arg);
static int	compare_mcvs(const void *a, const void *b, void *arg);
static int	analyze_mcv_list(int *mcv_counts,
							 int num_mcv,
							 double stadistinct,
							 double stanullfrac,
							 int samplerows,
							 double totalrows);


/*
 * std_typanalyze -- the default type-specific typanalyze function
 *
 * std_typanalyze：默认的类型专用 typanalyze 函数。
 */
bool
std_typanalyze(VacAttrStats *stats)
{
	Oid			ltopr;
	Oid			eqopr;
	StdAnalyzeData *mystats;

	/* If the attstattarget column is negative, use the default value */
	/*
	 *
	 * 若 attstattarget 列为负，则使用默认值。
	 */
	if (stats->attstattarget < 0)
		stats->attstattarget = default_statistics_target;

	/* Look for default "<" and "=" operators for column's type */
	/*
	 *
	 * 查找该列类型的默认小于操作符和等于操作符。
	 */
	get_sort_group_operators(stats->attrtypid,
							 false, false, false,
							 &ltopr, &eqopr, NULL,
							 NULL);

	/* Save the operator info for compute_stats routines */
	/*
	 *
	 * 为 compute_stats 例程保存操作符信息。
	 */
	mystats = (StdAnalyzeData *) palloc(sizeof(StdAnalyzeData));
	mystats->eqopr = eqopr;
	mystats->eqfunc = OidIsValid(eqopr) ? get_opcode(eqopr) : InvalidOid;
	mystats->ltopr = ltopr;
	stats->extra_data = mystats;

	/*
	 * Determine which standard statistics algorithm to use
	 *
	 * 决定使用哪种标准统计算法。
	 */
	if (OidIsValid(eqopr) && OidIsValid(ltopr))
	{
		/* Seems to be a scalar datatype */
		/*
		 *
		 * 看起来是标量数据类型。
		 */
		stats->compute_stats = compute_scalar_stats;
		/*--------------------
		 * The following choice of minrows is based on the paper
		 * "Random sampling for histogram construction: how much is enough?"
		 * by Surajit Chaudhuri, Rajeev Motwani and Vivek Narasayya, in
		 * Proceedings of ACM SIGMOD International Conference on Management
		 * of Data, 1998, Pages 436-447.  Their Corollary 1 to Theorem 5
		 * says that for table size n, histogram size k, maximum relative
		 * error in bin size f, and error probability gamma, the minimum
		 * random sample size is
		 *		r = 4 * k * ln(2*n/gamma) / f^2
		 * Taking f = 0.5, gamma = 0.01, n = 10^6 rows, we obtain
		 *		r = 305.82 * k
		 * Note that because of the log function, the dependence on n is
		 * quite weak; even at n = 10^12, a 300*k sample gives <= 0.66
		 * bin size error with probability 0.99.  So there's no real need to
		 * scale for n, which is a good thing because we don't necessarily
		 * know it at this point.
		 *
		 * 下面这个 minrows 的选择基于论文中的分析。
		 *--------------------
		 */
		stats->minrows = 300 * stats->attstattarget;
	}
	else if (OidIsValid(eqopr))
	{
		/* We can still recognize distinct values */
		/*
		 *
		 * 仍然可以识别不同的值。
		 */
		stats->compute_stats = compute_distinct_stats;
		/* Might as well use the same minrows as above */
		/*
		 *
		 * 不妨使用与上面相同的 minrows。
		 */
		stats->minrows = 300 * stats->attstattarget;
	}
	else
	{
		/* Can't do much but the trivial stuff */
		/*
		 *
		 * 除了最基本的内容，做不了太多。
		 */
		stats->compute_stats = compute_trivial_stats;
		/* Might as well use the same minrows as above */
		/*
		 *
		 * 不妨使用与上面相同的 minrows。
		 */
		stats->minrows = 300 * stats->attstattarget;
	}

	return true;
}


/*
 *	compute_trivial_stats() -- compute very basic column statistics
 *
 *	compute_trivial_stats()：计算非常基本的列统计。
 *
 *	We use this when we cannot find a hash "=" operator for the datatype.
 *
 *	当找不到该数据类型的哈希等于操作符时使用它。
 *
 *	We determine the fraction of non-null rows and the average datum width.
 *
 *	我们确定非空行的比例以及 datum 的平均宽度。
 */
static void
compute_trivial_stats(VacAttrStatsP stats,
					  AnalyzeAttrFetchFunc fetchfunc,
					  int samplerows,
					  double totalrows)
{
	int			i;
	int			null_cnt = 0;
	int			nonnull_cnt = 0;
	double		total_width = 0;
	bool		is_varlena = (!stats->attrtype->typbyval &&
							  stats->attrtype->typlen == -1);
	bool		is_varwidth = (!stats->attrtype->typbyval &&
							   stats->attrtype->typlen < 0);

	for (i = 0; i < samplerows; i++)
	{
		Datum		value;
		bool		isnull;

		vacuum_delay_point(true);

		value = fetchfunc(stats, i, &isnull);

		/* Check for null/nonnull */
		/*
		 *
		 * 检查空与非空。
		 */
		if (isnull)
		{
			null_cnt++;
			continue;
		}
		nonnull_cnt++;

		/*
		 * If it's a variable-width field, add up widths for average width
		 * calculation.  Note that if the value is toasted, we use the toasted
		 * width.  We don't bother with this calculation if it's a fixed-width
		 * type.
		 *
		 * 若是变宽字段，则累加宽度以计算平均宽度。
		 */
		if (is_varlena)
		{
			total_width += VARSIZE_ANY(DatumGetPointer(value));
		}
		else if (is_varwidth)
		{
			/* must be cstring */
			/*
			 *
			 * 必须是 cstring。
			 */
			total_width += strlen(DatumGetCString(value)) + 1;
		}
	}

	/* We can only compute average width if we found some non-null values. */
	/*
	 *
	 * 只有找到一些非空值时才能计算平均宽度。
	 */
	if (nonnull_cnt > 0)
	{
		stats->stats_valid = true;
		/* Do the simple null-frac and width stats */
		/*
		 *
		 * 做简单的空值比例和宽度统计。
		 */
		stats->stanullfrac = (double) null_cnt / (double) samplerows;
		if (is_varwidth)
			stats->stawidth = total_width / (double) nonnull_cnt;
		else
			stats->stawidth = stats->attrtype->typlen;
		stats->stadistinct = 0.0;	/* "unknown" */
		/*
		 *
		 * 未知。
		 */
	}
	else if (null_cnt > 0)
	{
		/* We found only nulls; assume the column is entirely null */
		/*
		 *
		 * 只找到了空值；假定该列完全为空。
		 */
		stats->stats_valid = true;
		stats->stanullfrac = 1.0;
		if (is_varwidth)
			stats->stawidth = 0;	/* "unknown" */
			/*
			 *
			 * 未知。
			 */
		else
			stats->stawidth = stats->attrtype->typlen;
		stats->stadistinct = 0.0;	/* "unknown" */
		/*
		 *
		 * 未知。
		 */
	}
}


/*
 *	compute_distinct_stats() -- compute column statistics including ndistinct
 *
 *	compute_distinct_stats()：计算包含 ndistinct 的列统计。
 *
 *	We use this when we can find only an "=" operator for the datatype.
 *
 *	当只能找到该数据类型的等于操作符时使用它。
 *
 *	We determine the fraction of non-null rows, the average width, the
 *	most common values, and the (estimated) number of distinct values.
 *
 *	我们确定非空行的比例、平均宽度、最常见值，以及（估算的）不同值个数。
 *
 *	The most common values are determined by brute force: we keep a list
 *	of previously seen values, ordered by number of times seen, as we scan
 *	the samples.  A newly seen value is inserted just after the last
 *	multiply-seen value, causing the bottommost (oldest) singly-seen value
 *	to drop off the list.  The accuracy of this method, and also its cost,
 *	depend mainly on the length of the list we are willing to keep.
 *
 *	最常见值用蛮力确定：我们保留一个已见值及其计数的列表。
 */
static void
compute_distinct_stats(VacAttrStatsP stats,
					   AnalyzeAttrFetchFunc fetchfunc,
					   int samplerows,
					   double totalrows)
{
	int			i;
	int			null_cnt = 0;
	int			nonnull_cnt = 0;
	int			toowide_cnt = 0;
	double		total_width = 0;
	bool		is_varlena = (!stats->attrtype->typbyval &&
							  stats->attrtype->typlen == -1);
	bool		is_varwidth = (!stats->attrtype->typbyval &&
							   stats->attrtype->typlen < 0);
	FmgrInfo	f_cmpeq;
	typedef struct
	{
		Datum		value;
		int			count;
	} TrackItem;
	TrackItem  *track;
	int			track_cnt,
				track_max;
	int			num_mcv = stats->attstattarget;
	StdAnalyzeData *mystats = (StdAnalyzeData *) stats->extra_data;

	/*
	 * We track up to 2*n values for an n-element MCV list; but at least 10
	 *
	 * 对 n 个元素的 MCV 列表，最多跟踪 2*n 个值；但至少跟踪 10 个。
	 */
	track_max = 2 * num_mcv;
	if (track_max < 10)
		track_max = 10;
	track = (TrackItem *) palloc(track_max * sizeof(TrackItem));
	track_cnt = 0;

	fmgr_info(mystats->eqfunc, &f_cmpeq);

	for (i = 0; i < samplerows; i++)
	{
		Datum		value;
		bool		isnull;
		bool		match;
		int			firstcount1,
					j;

		vacuum_delay_point(true);

		value = fetchfunc(stats, i, &isnull);

		/* Check for null/nonnull */
		/*
		 *
		 * 检查空与非空。
		 */
		if (isnull)
		{
			null_cnt++;
			continue;
		}
		nonnull_cnt++;

		/*
		 * If it's a variable-width field, add up widths for average width
		 * calculation.  Note that if the value is toasted, we use the toasted
		 * width.  We don't bother with this calculation if it's a fixed-width
		 * type.
		 *
		 * 若是变宽字段，则累加宽度以计算平均宽度。
		 */
		if (is_varlena)
		{
			total_width += VARSIZE_ANY(DatumGetPointer(value));

			/*
			 * If the value is toasted, we want to detoast it just once to
			 * avoid repeated detoastings and resultant excess memory usage
			 * during the comparisons.  Also, check to see if the value is
			 * excessively wide, and if so don't detoast at all --- just
			 * ignore the value.
			 *
			 * 若值被 toasted，希望只 detoast 一次，以避免比较期间反复 detoast
			 * 并因此过度使用内存。
			 */
			if (toast_raw_datum_size(value) > WIDTH_THRESHOLD)
			{
				toowide_cnt++;
				continue;
			}
			value = PointerGetDatum(PG_DETOAST_DATUM(value));
		}
		else if (is_varwidth)
		{
			/* must be cstring */
			/*
			 *
			 * 必须是 cstring。
			 */
			total_width += strlen(DatumGetCString(value)) + 1;
		}

		/*
		 * See if the value matches anything we're already tracking.
		 *
		 * 查看该值是否匹配我们已经在跟踪的某个值。
		 */
		match = false;
		firstcount1 = track_cnt;
		for (j = 0; j < track_cnt; j++)
		{
			if (DatumGetBool(FunctionCall2Coll(&f_cmpeq,
											   stats->attrcollid,
											   value, track[j].value)))
			{
				match = true;
				break;
			}
			if (j < firstcount1 && track[j].count == 1)
				firstcount1 = j;
		}

		if (match)
		{
			/* Found a match */
			/*
			 *
			 * 找到匹配。
			 */
			track[j].count++;
			/* This value may now need to "bubble up" in the track list */
			/*
			 *
			 * 该值现在可能需要在跟踪列表中向上冒泡。
			 */
			while (j > 0 && track[j].count > track[j - 1].count)
			{
				swapDatum(track[j].value, track[j - 1].value);
				swapInt(track[j].count, track[j - 1].count);
				j--;
			}
		}
		else
		{
			/* No match.  Insert at head of count-1 list */
			/*
			 *
			 * 没有匹配。插入到计数为 1 的列表头部。
			 */
			if (track_cnt < track_max)
				track_cnt++;
			for (j = track_cnt - 1; j > firstcount1; j--)
			{
				track[j].value = track[j - 1].value;
				track[j].count = track[j - 1].count;
			}
			if (firstcount1 < track_cnt)
			{
				track[firstcount1].value = value;
				track[firstcount1].count = 1;
			}
		}
	}

	/* We can only compute real stats if we found some non-null values. */
	/*
	 *
	 * 只有找到一些非空值时才能计算真正的统计。
	 */
	if (nonnull_cnt > 0)
	{
		int			nmultiple,
					summultiple;

		stats->stats_valid = true;
		/* Do the simple null-frac and width stats */
		/*
		 *
		 * 做简单的空值比例和宽度统计。
		 */
		stats->stanullfrac = (double) null_cnt / (double) samplerows;
		if (is_varwidth)
			stats->stawidth = total_width / (double) nonnull_cnt;
		else
			stats->stawidth = stats->attrtype->typlen;

		/* Count the number of values we found multiple times */
		/*
		 *
		 * 统计我们发现多次的值的个数。
		 */
		summultiple = 0;
		for (nmultiple = 0; nmultiple < track_cnt; nmultiple++)
		{
			if (track[nmultiple].count == 1)
				break;
			summultiple += track[nmultiple].count;
		}

		if (nmultiple == 0)
		{
			/*
			 * If we found no repeated non-null values, assume it's a unique
			 * column; but be sure to discount for any nulls we found.
			 *
			 * 若没有发现重复的非空值，则假定它是唯一列；但一定要扣除发现的空
			 * 值。
			 */
			stats->stadistinct = -1.0 * (1.0 - stats->stanullfrac);
		}
		else if (track_cnt < track_max && toowide_cnt == 0 &&
				 nmultiple == track_cnt)
		{
			/*
			 * Our track list includes every value in the sample, and every
			 * value appeared more than once.  Assume the column has just
			 * these values.  (This case is meant to address columns with
			 * small, fixed sets of possible values, such as boolean or enum
			 * columns.  If there are any values that appear just once in the
			 * sample, including too-wide values, we should assume that that's
			 * not what we're dealing with.)
			 *
			 * 跟踪列表包含样本中的每一个值，而且每一个都只出现一次。
			 */
			stats->stadistinct = track_cnt;
		}
		else
		{
			/*----------
			 * Estimate the number of distinct values using the estimator
			 * proposed by Haas and Stokes in IBM Research Report RJ 10025:
			 *		n*d / (n - f1 + f1*n/N)
			 * where f1 is the number of distinct values that occurred
			 * exactly once in our sample of n rows (from a total of N),
			 * and d is the total number of distinct values in the sample.
			 * This is their Duj1 estimator; the other estimators they
			 * recommend are considerably more complex, and are numerically
			 * very unstable when n is much smaller than N.
			 *
			 * 用 Haas 和 Stokes 在 IBM Research Report RJ 10025 中提出的估计
			 * 器估算不同值的个数：n*d / (n - f1 + f1*n/N)。其中 f1 是在来自总
			 * 共 N 行的 n 行样本中恰好出现一次的不同值个数，d 是样本中不同值
			 * 的总数。这是他们的 Duj1 估计器；他们推荐的其他估计器复杂得多，
			 * 而且当 n 远小于 N 时数值非常不稳定。
			 *
			 * In this calculation, we consider only non-nulls.  We used to
			 * include rows with null values in the n and N counts, but that
			 * leads to inaccurate answers in columns with many nulls, and
			 * it's intuitively bogus anyway considering the desired result is
			 * the number of distinct non-null values.
			 *
			 * 在这个计算中只考虑非空值。以前把含空值的行计入 n 和 N，但那会在
			 * 空值很多的列上给出不准确的答案，而且考虑到期望结果是不同非空值
			 * 的个数，那样做在直觉上也不成立。
			 *
			 * We assume (not very reliably!) that all the multiply-occurring
			 * values are reflected in the final track[] list, and the other
			 * nonnull values all appeared but once.  (XXX this usually
			 * results in a drastic overestimate of ndistinct.  Can we do
			 * any better?)
			 *
			 * 我们（不很可靠地）假定所有多次出现的值都反映在最终的 track[] 列
			 * 表中，其他非空值都只出现一次。（XXX 这通常会严重高估 ndistinct。
			 * 还能做得更好吗？）
			 *----------
			 */
			int			f1 = nonnull_cnt - summultiple;
			int			d = f1 + nmultiple;
			double		n = samplerows - null_cnt;
			double		N = totalrows * (1.0 - stats->stanullfrac);
			double		stadistinct;

			/* N == 0 shouldn't happen, but just in case ... */
			/*
			 *
			 * N 等于 0 不应发生，但以防万一……
			 */
			if (N > 0)
				stadistinct = (n * d) / ((n - f1) + f1 * n / N);
			else
				stadistinct = 0;

			/* Clamp to sane range in case of roundoff error */
			/*
			 *
			 * 为防止舍入误差，钳制到合理范围。
			 */
			if (stadistinct < d)
				stadistinct = d;
			if (stadistinct > N)
				stadistinct = N;
			/* And round to integer */
			/*
			 *
			 * 并四舍五入为整数。
			 */
			stats->stadistinct = floor(stadistinct + 0.5);
		}

		/*
		 * If we estimated the number of distinct values at more than 10% of
		 * the total row count (a very arbitrary limit), then assume that
		 * stadistinct should scale with the row count rather than be a fixed
		 * value.
		 *
		 * 若估算的不同值个数超过总行数的 10%（一个非常任意的界限），则假定
		 * stadistinct 应随行数缩放，而不是一个固定值。
		 */
		if (stats->stadistinct > 0.1 * totalrows)
			stats->stadistinct = -(stats->stadistinct / totalrows);

		/*
		 * Decide how many values are worth storing as most-common values. If
		 * we are able to generate a complete MCV list (all the values in the
		 * sample will fit, and we think these are all the ones in the table),
		 * then do so.  Otherwise, store only those values that are
		 * significantly more common than the values not in the list.
		 *
		 * 决定有多少值值得作为最常见值存储。若能生成完整的 MCV 列表（样本中的
		 * 全部值都放得下，并且我们认为它们就是表中的全部值），则这样做。否则
		 * 只存储那些明显比不在列表中的值更常见的值。
		 *
		 * Note: the first of these cases is meant to address columns with
		 * small, fixed sets of possible values, such as boolean or enum
		 * columns.  If we can *completely* represent the column population by
		 * an MCV list that will fit into the stats target, then we should do
		 * so and thus provide the planner with complete information.  But if
		 * the MCV list is not complete, it's generally worth being more
		 * selective, and not just filling it all the way up to the stats
		 * target.
		 *
		 * 注意：第一种情况针对可能取值集合小而固定的列，例如布尔或枚举列。若
		 * 能用放进统计目标的 MCV 列表完整表示列的总体，就应该这样做，从而给规
		 * 划器完整信息。但若 MCV 列表不完整，通常更值得挑剔，而不是一直填到统
		 * 计目标。
		 */
		if (track_cnt < track_max && toowide_cnt == 0 &&
			stats->stadistinct > 0 &&
			track_cnt <= num_mcv)
		{
			/* Track list includes all values seen, and all will fit */
			/*
			 *
			 * 跟踪列表包含见过的全部值，而且全部放得下。
			 */
			num_mcv = track_cnt;
		}
		else
		{
			int		   *mcv_counts;

			/* Incomplete list; decide how many values are worth keeping */
			/*
			 *
			 * 列表不完整；决定有多少值值得保留。
			 */
			if (num_mcv > track_cnt)
				num_mcv = track_cnt;

			if (num_mcv > 0)
			{
				mcv_counts = (int *) palloc(num_mcv * sizeof(int));
				for (i = 0; i < num_mcv; i++)
					mcv_counts[i] = track[i].count;

				num_mcv = analyze_mcv_list(mcv_counts, num_mcv,
										   stats->stadistinct,
										   stats->stanullfrac,
										   samplerows, totalrows);
			}
		}

		/* Generate MCV slot entry */
		/*
		 *
		 * 生成 MCV 槽项。
		 */
		if (num_mcv > 0)
		{
			MemoryContext old_context;
			Datum	   *mcv_values;
			float4	   *mcv_freqs;

			/* Must copy the target values into anl_context */
			/*
			 *
			 * 必须把目标值复制到 anl_context。
			 */
			old_context = MemoryContextSwitchTo(stats->anl_context);
			mcv_values = (Datum *) palloc(num_mcv * sizeof(Datum));
			mcv_freqs = (float4 *) palloc(num_mcv * sizeof(float4));
			for (i = 0; i < num_mcv; i++)
			{
				mcv_values[i] = datumCopy(track[i].value,
										  stats->attrtype->typbyval,
										  stats->attrtype->typlen);
				mcv_freqs[i] = (double) track[i].count / (double) samplerows;
			}
			MemoryContextSwitchTo(old_context);

			stats->stakind[0] = STATISTIC_KIND_MCV;
			stats->staop[0] = mystats->eqopr;
			stats->stacoll[0] = stats->attrcollid;
			stats->stanumbers[0] = mcv_freqs;
			stats->numnumbers[0] = num_mcv;
			stats->stavalues[0] = mcv_values;
			stats->numvalues[0] = num_mcv;

			/*
			 * Accept the defaults for stats->statypid and others. They have
			 * been set before we were called (see vacuum.h)
			 *
			 * 接受 stats->statypid 等的默认值。它们已经被设成合适的内容。
			 */
		}
	}
	else if (null_cnt > 0)
	{
		/* We found only nulls; assume the column is entirely null */
		/*
		 *
		 * 只找到了空值；假定该列完全为空。
		 */
		stats->stats_valid = true;
		stats->stanullfrac = 1.0;
		if (is_varwidth)
			stats->stawidth = 0;	/* "unknown" */
			/*
			 *
			 * 未知。
			 */
		else
			stats->stawidth = stats->attrtype->typlen;
		stats->stadistinct = 0.0;	/* "unknown" */
		/*
		 *
		 * 未知。
		 */
	}

	/* We don't need to bother cleaning up any of our temporary palloc's */
	/*
	 *
	 * 不必费心清理任何临时的 palloc。
	 */
}


/*
 *	compute_scalar_stats() -- compute column statistics
 *
 *	compute_scalar_stats()：计算列统计。
 *
 *	We use this when we can find "=" and "<" operators for the datatype.
 *
 *	当能为该数据类型找到等于和小于操作符时使用它。
 *
 *	We determine the fraction of non-null rows, the average width, the
 *	most common values, the (estimated) number of distinct values, the
 *	distribution histogram, and the correlation of physical to logical order.
 *
 *	我们确定非空行的比例、平均宽度、最常见值、（估算的）不同值个数、分布直方图，
 *	以及物理顺序与逻辑顺序的相关性。
 *
 *	The desired stats can be determined fairly easily after sorting the
 *	data values into order.
 *
 *	对收集到的值排序后，所需统计可以相当容易地确定。
 */
static void
compute_scalar_stats(VacAttrStatsP stats,
					 AnalyzeAttrFetchFunc fetchfunc,
					 int samplerows,
					 double totalrows)
{
	int			i;
	int			null_cnt = 0;
	int			nonnull_cnt = 0;
	int			toowide_cnt = 0;
	double		total_width = 0;
	bool		is_varlena = (!stats->attrtype->typbyval &&
							  stats->attrtype->typlen == -1);
	bool		is_varwidth = (!stats->attrtype->typbyval &&
							   stats->attrtype->typlen < 0);
	double		corr_xysum;
	SortSupportData ssup;
	ScalarItem *values;
	int			values_cnt = 0;
	int		   *tupnoLink;
	ScalarMCVItem *track;
	int			track_cnt = 0;
	int			num_mcv = stats->attstattarget;
	int			num_bins = stats->attstattarget;
	StdAnalyzeData *mystats = (StdAnalyzeData *) stats->extra_data;

	values = (ScalarItem *) palloc(samplerows * sizeof(ScalarItem));
	tupnoLink = (int *) palloc(samplerows * sizeof(int));
	track = (ScalarMCVItem *) palloc(num_mcv * sizeof(ScalarMCVItem));

	memset(&ssup, 0, sizeof(ssup));
	ssup.ssup_cxt = CurrentMemoryContext;
	ssup.ssup_collation = stats->attrcollid;
	ssup.ssup_nulls_first = false;

	/*
	 * For now, don't perform abbreviated key conversion, because full values
	 * are required for MCV slot generation.  Supporting that optimization
	 * would necessitate teaching compare_scalars() to call a tie-breaker.
	 *
	 * 目前不做缩写键转换，因为生成 MCV 槽需要完整值。要支持该优化，必须让
	 * compare_scalars() 调用决胜比较。
	 */
	ssup.abbreviate = false;

	PrepareSortSupportFromOrderingOp(mystats->ltopr, &ssup);

	/* Initial scan to find sortable values */
	/*
	 *
	 * 初次扫描以找出可排序的值。
	 */
	for (i = 0; i < samplerows; i++)
	{
		Datum		value;
		bool		isnull;

		vacuum_delay_point(true);

		value = fetchfunc(stats, i, &isnull);

		/* Check for null/nonnull */
		/*
		 *
		 * 检查空与非空。
		 */
		if (isnull)
		{
			null_cnt++;
			continue;
		}
		nonnull_cnt++;

		/*
		 * If it's a variable-width field, add up widths for average width
		 * calculation.  Note that if the value is toasted, we use the toasted
		 * width.  We don't bother with this calculation if it's a fixed-width
		 * type.
		 *
		 * 若是变宽字段，则累加宽度以计算平均宽度。注意若值被 toasted，使用
		 * toasted 宽度。若是定宽类型则不做这个计算。
		 */
		if (is_varlena)
		{
			total_width += VARSIZE_ANY(DatumGetPointer(value));

			/*
			 * If the value is toasted, we want to detoast it just once to
			 * avoid repeated detoastings and resultant excess memory usage
			 * during the comparisons.  Also, check to see if the value is
			 * excessively wide, and if so don't detoast at all --- just
			 * ignore the value.
			 *
			 * 若值被 toasted，希望只 detoast 一次，以避免反复 detoast 并在比
			 * 较期间过度使用内存。同时检查值是否过宽，若是则完全不 detoast——
			 * 直接忽略该值。
			 */
			if (toast_raw_datum_size(value) > WIDTH_THRESHOLD)
			{
				toowide_cnt++;
				continue;
			}
			value = PointerGetDatum(PG_DETOAST_DATUM(value));
		}
		else if (is_varwidth)
		{
			/* must be cstring */
			/*
			 *
			 * 必须是 cstring。
			 */
			total_width += strlen(DatumGetCString(value)) + 1;
		}

		/* Add it to the list to be sorted */
		/*
		 *
		 * 把它加入待排序列表。
		 */
		values[values_cnt].value = value;
		values[values_cnt].tupno = values_cnt;
		tupnoLink[values_cnt] = values_cnt;
		values_cnt++;
	}

	/* We can only compute real stats if we found some sortable values. */
	/*
	 *
	 * 只有找到一些可排序的值时才能计算真正的统计。
	 */
	if (values_cnt > 0)
	{
		int			ndistinct,	/* # distinct values in sample */
		/*
		 *
		 * 样本中的不同值个数。
		 */
					nmultiple,	/* # that appear multiple times */
					/*
					 *
					 * 出现多次的个数。
					 */
					num_hist,
					dups_cnt;
		int			slot_idx = 0;
		CompareScalarsContext cxt;

		/* Sort the collected values */
		/*
		 *
		 * 对收集到的值排序。
		 */
		cxt.ssup = &ssup;
		cxt.tupnoLink = tupnoLink;
		qsort_interruptible(values, values_cnt, sizeof(ScalarItem),
							compare_scalars, &cxt);

		/*
		 * Now scan the values in order, find the most common ones, and also
		 * accumulate ordering-correlation statistics.
		 *
		 * 现在按顺序扫描这些值，找出最常见的，并累计相关性。
		 *
		 * To determine which are most common, we first have to count the
		 * number of duplicates of each value.  The duplicates are adjacent in
		 * the sorted list, so a brute-force approach is to compare successive
		 * datum values until we find two that are not equal. However, that
		 * requires N-1 invocations of the datum comparison routine, which are
		 * completely redundant with work that was done during the sort.  (The
		 * sort algorithm must at some point have compared each pair of items
		 * that are adjacent in the sorted order; otherwise it could not know
		 * that it's ordered the pair correctly.) We exploit this by having
		 * compare_scalars remember the highest tupno index that each
		 * ScalarItem has been found equal to.  At the end of the sort, a
		 * ScalarItem's tupnoLink will still point to itself if and only if it
		 * is the last item of its group of duplicates (since the group will
		 * be ordered by tupno).
		 *
		 * 要确定哪些最常见，必须先数每个值的重复次数。重复项在已排序列表中相
		 * 邻，因此蛮力做法是比较相继的 datum，直到发现两个不相等。但那需要 N-
		 * 1 次 datum 比较，而这些比较与排序期间已经做过的工作完全重复。（排序
		 * 算法在某个时刻必然比较过排序结果中每一对相邻项，否则它无法知道这对
		 * 的顺序是对的。）我们利用这一点，让 compare_scalars 记住每个
		 * ScalarItem 被发现相等的最高 tupno 下标。排序结束时，ScalarItem 的
		 * tupnoLink 仍指向自身，当且仅当它是其重复组的最后一项（因为该组会按
		 * tupno 排序）。
		 */
		corr_xysum = 0;
		ndistinct = 0;
		nmultiple = 0;
		dups_cnt = 0;
		for (i = 0; i < values_cnt; i++)
		{
			int			tupno = values[i].tupno;

			corr_xysum += ((double) i) * ((double) tupno);
			dups_cnt++;
			if (tupnoLink[tupno] == tupno)
			{
				/* Reached end of duplicates of this value */
				/*
				 *
				 * 到达该值的重复项末尾。
				 */
				ndistinct++;
				if (dups_cnt > 1)
				{
					nmultiple++;
					if (track_cnt < num_mcv ||
						dups_cnt > track[track_cnt - 1].count)
					{
						/*
						 * Found a new item for the mcv list; find its
						 * position, bubbling down old items if needed. Loop
						 * invariant is that j points at an empty/ replaceable
						 * slot.
						 *
						 * 为 MCV 列表找到一个新项；找出它的位置。
						 */
						int			j;

						if (track_cnt < num_mcv)
							track_cnt++;
						for (j = track_cnt - 1; j > 0; j--)
						{
							if (dups_cnt <= track[j - 1].count)
								break;
							track[j].count = track[j - 1].count;
							track[j].first = track[j - 1].first;
						}
						track[j].count = dups_cnt;
						track[j].first = i + 1 - dups_cnt;
					}
				}
				dups_cnt = 0;
			}
		}

		stats->stats_valid = true;
		/* Do the simple null-frac and width stats */
		/*
		 *
		 * 做简单的空值比例和宽度统计。
		 */
		stats->stanullfrac = (double) null_cnt / (double) samplerows;
		if (is_varwidth)
			stats->stawidth = total_width / (double) nonnull_cnt;
		else
			stats->stawidth = stats->attrtype->typlen;

		if (nmultiple == 0)
		{
			/*
			 * If we found no repeated non-null values, assume it's a unique
			 * column; but be sure to discount for any nulls we found.
			 *
			 * 若没有发现重复的非空值，则假定它是唯一列；但一定要扣除发现的空
			 * 值。
			 */
			stats->stadistinct = -1.0 * (1.0 - stats->stanullfrac);
		}
		else if (toowide_cnt == 0 && nmultiple == ndistinct)
		{
			/*
			 * Every value in the sample appeared more than once.  Assume the
			 * column has just these values.  (This case is meant to address
			 * columns with small, fixed sets of possible values, such as
			 * boolean or enum columns.  If there are any values that appear
			 * just once in the sample, including too-wide values, we should
			 * assume that that's not what we're dealing with.)
			 *
			 * 样本中的每个值都出现了不止一次。假定该列只有这些值。（这种情况
			 * 针对可能取值集合小而固定的列，例如布尔或枚举列。若样本中有任何
			 * 只出现一次的值，包括过宽的值，就应假定我们面对的不是这种情况。）
			 */
			stats->stadistinct = ndistinct;
		}
		else
		{
			/*----------
			 * Estimate the number of distinct values using the estimator
			 * proposed by Haas and Stokes in IBM Research Report RJ 10025:
			 *		n*d / (n - f1 + f1*n/N)
			 * where f1 is the number of distinct values that occurred
			 * exactly once in our sample of n rows (from a total of N),
			 * and d is the total number of distinct values in the sample.
			 * This is their Duj1 estimator; the other estimators they
			 * recommend are considerably more complex, and are numerically
			 * very unstable when n is much smaller than N.
			 *
			 * 用 Haas 和 Stokes 在 IBM Research Report RJ 10025 中提出的估计
			 * 器估算不同值的个数：n*d / (n - f1 + f1*n/N)。其中 f1 是在来自总
			 * 共 N 行的 n 行样本中恰好出现一次的不同值个数，d 是样本中不同值
			 * 的总数。这是他们的 Duj1 估计器；他们推荐的其他估计器复杂得多，
			 * 而且当 n 远小于 N 时数值非常不稳定。
			 *
			 * In this calculation, we consider only non-nulls.  We used to
			 * include rows with null values in the n and N counts, but that
			 * leads to inaccurate answers in columns with many nulls, and
			 * it's intuitively bogus anyway considering the desired result is
			 * the number of distinct non-null values.
			 *
			 * 在这个计算中只考虑非空值。以前把含空值的行计入 n 和 N，但那会在
			 * 空值很多的列上给出不准确的答案，而且考虑到期望结果是不同非空值
			 * 的个数，那样做在直觉上也不成立。
			 *
			 * Overwidth values are assumed to have been distinct.
			 *
			 * 过宽的值假定各不相同。
			 *----------
			 */
			int			f1 = ndistinct - nmultiple + toowide_cnt;
			int			d = f1 + nmultiple;
			double		n = samplerows - null_cnt;
			double		N = totalrows * (1.0 - stats->stanullfrac);
			double		stadistinct;

			/* N == 0 shouldn't happen, but just in case ... */
			/*
			 *
			 * N 等于 0 不应发生，但以防万一……
			 */
			if (N > 0)
				stadistinct = (n * d) / ((n - f1) + f1 * n / N);
			else
				stadistinct = 0;

			/* Clamp to sane range in case of roundoff error */
			/*
			 *
			 * 为防止舍入误差，钳制到合理范围。
			 */
			if (stadistinct < d)
				stadistinct = d;
			if (stadistinct > N)
				stadistinct = N;
			/* And round to integer */
			/*
			 *
			 * 并四舍五入为整数。
			 */
			stats->stadistinct = floor(stadistinct + 0.5);
		}

		/*
		 * If we estimated the number of distinct values at more than 10% of
		 * the total row count (a very arbitrary limit), then assume that
		 * stadistinct should scale with the row count rather than be a fixed
		 * value.
		 *
		 * 若估算的不同值个数超过总行数的 10%（一个非常任意的界限），则假定
		 * stadistinct 应随行数缩放，而不是一个固定值。
		 */
		if (stats->stadistinct > 0.1 * totalrows)
			stats->stadistinct = -(stats->stadistinct / totalrows);

		/*
		 * Decide how many values are worth storing as most-common values. If
		 * we are able to generate a complete MCV list (all the values in the
		 * sample will fit, and we think these are all the ones in the table),
		 * then do so.  Otherwise, store only those values that are
		 * significantly more common than the values not in the list.
		 *
		 * 决定有多少值值得作为最常见值存储。若能生成完整的 MCV 列表（样本中的
		 * 全部值都放得下，并且我们认为它们就是表中的全部值），则这样做。否则
		 * 只存储那些明显比不在列表中的值更常见的值。
		 *
		 * Note: the first of these cases is meant to address columns with
		 * small, fixed sets of possible values, such as boolean or enum
		 * columns.  If we can *completely* represent the column population by
		 * an MCV list that will fit into the stats target, then we should do
		 * so and thus provide the planner with complete information.  But if
		 * the MCV list is not complete, it's generally worth being more
		 * selective, and not just filling it all the way up to the stats
		 * target.
		 *
		 * 注意：第一种情况针对可能取值集合小而固定的列，例如布尔或枚举列。若
		 * 能用放进统计目标的 MCV 列表完整表示列的总体，就应该这样做，从而给规
		 * 划器完整信息。但若 MCV 列表不完整，通常更值得挑剔，而不是一直填到统
		 * 计目标。
		 */
		if (track_cnt == ndistinct && toowide_cnt == 0 &&
			stats->stadistinct > 0 &&
			track_cnt <= num_mcv)
		{
			/* Track list includes all values seen, and all will fit */
			/*
			 *
			 * 跟踪列表包含见过的全部值，而且全部放得下。
			 */
			num_mcv = track_cnt;
		}
		else
		{
			int		   *mcv_counts;

			/* Incomplete list; decide how many values are worth keeping */
			/*
			 *
			 * 列表不完整；决定有多少值值得保留。
			 */
			if (num_mcv > track_cnt)
				num_mcv = track_cnt;

			if (num_mcv > 0)
			{
				mcv_counts = (int *) palloc(num_mcv * sizeof(int));
				for (i = 0; i < num_mcv; i++)
					mcv_counts[i] = track[i].count;

				num_mcv = analyze_mcv_list(mcv_counts, num_mcv,
										   stats->stadistinct,
										   stats->stanullfrac,
										   samplerows, totalrows);
			}
		}

		/* Generate MCV slot entry */
		/*
		 *
		 * 生成 MCV 槽项。
		 */
		if (num_mcv > 0)
		{
			MemoryContext old_context;
			Datum	   *mcv_values;
			float4	   *mcv_freqs;

			/* Must copy the target values into anl_context */
			/*
			 *
			 * 必须把目标值复制到 anl_context。
			 */
			old_context = MemoryContextSwitchTo(stats->anl_context);
			mcv_values = (Datum *) palloc(num_mcv * sizeof(Datum));
			mcv_freqs = (float4 *) palloc(num_mcv * sizeof(float4));
			for (i = 0; i < num_mcv; i++)
			{
				mcv_values[i] = datumCopy(values[track[i].first].value,
										  stats->attrtype->typbyval,
										  stats->attrtype->typlen);
				mcv_freqs[i] = (double) track[i].count / (double) samplerows;
			}
			MemoryContextSwitchTo(old_context);

			stats->stakind[slot_idx] = STATISTIC_KIND_MCV;
			stats->staop[slot_idx] = mystats->eqopr;
			stats->stacoll[slot_idx] = stats->attrcollid;
			stats->stanumbers[slot_idx] = mcv_freqs;
			stats->numnumbers[slot_idx] = num_mcv;
			stats->stavalues[slot_idx] = mcv_values;
			stats->numvalues[slot_idx] = num_mcv;

			/*
			 * Accept the defaults for stats->statypid and others. They have
			 * been set before we were called (see vacuum.h)
			 *
			 * 接受 stats->statypid 等的默认值。它们已经被设成合适的内容。
			 */
			slot_idx++;
		}

		/*
		 * Generate a histogram slot entry if there are at least two distinct
		 * values not accounted for in the MCV list.  (This ensures the
		 * histogram won't collapse to empty or a singleton.)
		 *
		 * 若至少有两个不同值没有被 MCV 列表计入，则生成直方图槽项。（这保证直
		 * 方图不会塌成空或单值。）
		 */
		num_hist = ndistinct - num_mcv;
		if (num_hist > num_bins)
			num_hist = num_bins + 1;
		if (num_hist >= 2)
		{
			MemoryContext old_context;
			Datum	   *hist_values;
			int			nvals;
			int			pos,
						posfrac,
						delta,
						deltafrac;

			/* Sort the MCV items into position order to speed next loop */
			/*
			 *
			 * 把 MCV 项按位置顺序排序，以加速下一个循环。
			 */
			qsort_interruptible(track, num_mcv, sizeof(ScalarMCVItem),
								compare_mcvs, NULL);

			/*
			 * Collapse out the MCV items from the values[] array.
			 *
			 * 从 values[] 数组中去掉 MCV 项。
			 *
			 * Note we destroy the values[] array here... but we don't need it
			 * for anything more.  We do, however, still need values_cnt.
			 * nvals will be the number of remaining entries in values[].
			 *
			 * 注意这里会破坏 values[] 数组……但后面不再需要它。不过仍然需要
			 * values_cnt。nvals 将是 values[] 中剩余项的个数。
			 */
			if (num_mcv > 0)
			{
				int			src,
							dest;
				int			j;

				src = dest = 0;
				j = 0;			/* index of next interesting MCV item */
				/*
				 *
				 * 下一个感兴趣的 MCV 项的下标。
				 */
				while (src < values_cnt)
				{
					int			ncopy;

					if (j < num_mcv)
					{
						int			first = track[j].first;

						if (src >= first)
						{
							/* advance past this MCV item */
							/*
							 *
							 * 跳过这个 MCV 项。
							 */
							src = first + track[j].count;
							j++;
							continue;
						}
						ncopy = first - src;
					}
					else
						ncopy = values_cnt - src;
					memmove(&values[dest], &values[src],
							ncopy * sizeof(ScalarItem));
					src += ncopy;
					dest += ncopy;
				}
				nvals = dest;
			}
			else
				nvals = values_cnt;
			Assert(nvals >= num_hist);

			/* Must copy the target values into anl_context */
			/*
			 *
			 * 必须把目标值复制到 anl_context。
			 */
			old_context = MemoryContextSwitchTo(stats->anl_context);
			hist_values = (Datum *) palloc(num_hist * sizeof(Datum));

			/*
			 * The object of this loop is to copy the first and last values[]
			 * entries along with evenly-spaced values in between.  So the
			 * i'th value is values[(i * (nvals - 1)) / (num_hist - 1)].  But
			 * computing that subscript directly risks integer overflow when
			 * the stats target is more than a couple thousand.  Instead we
			 * add (nvals - 1) / (num_hist - 1) to pos at each step, tracking
			 * the integral and fractional parts of the sum separately.
			 *
			 * 这个循环的目的是复制 values[] 的第一项和最后一项，以及中间均匀
			 * 间隔的值。因此第 i 个值是 values[(i * (nvals - 1)) / (num_hist
			 * - 1)]。但直接计算该下标在统计目标超过几千时有整数溢出风险。因此
			 * 每一步把 (nvals - 1) / (num_hist - 1) 加到 pos 上，并分别跟踪和
			 * 的整数部分与小数部分。
			 */
			delta = (nvals - 1) / (num_hist - 1);
			deltafrac = (nvals - 1) % (num_hist - 1);
			pos = posfrac = 0;

			for (i = 0; i < num_hist; i++)
			{
				hist_values[i] = datumCopy(values[pos].value,
										   stats->attrtype->typbyval,
										   stats->attrtype->typlen);
				pos += delta;
				posfrac += deltafrac;
				if (posfrac >= (num_hist - 1))
				{
					/* fractional part exceeds 1, carry to integer part */
					/*
					 *
					 * 小数部分超过 1 时，进位到整数部分。
					 */
					pos++;
					posfrac -= (num_hist - 1);
				}
			}

			MemoryContextSwitchTo(old_context);

			stats->stakind[slot_idx] = STATISTIC_KIND_HISTOGRAM;
			stats->staop[slot_idx] = mystats->ltopr;
			stats->stacoll[slot_idx] = stats->attrcollid;
			stats->stavalues[slot_idx] = hist_values;
			stats->numvalues[slot_idx] = num_hist;

			/*
			 * Accept the defaults for stats->statypid and others. They have
			 * been set before we were called (see vacuum.h)
			 *
			 * 接受 stats->statypid 等的默认值。它们已经被设成合适的内容。
			 */
			slot_idx++;
		}

		/* Generate a correlation entry if there are multiple values */
		/*
		 *
		 * 若有多个值，则生成相关性项。
		 */
		if (values_cnt > 1)
		{
			MemoryContext old_context;
			float4	   *corrs;
			double		corr_xsum,
						corr_x2sum;

			/* Must copy the target values into anl_context */
			/*
			 *
			 * 必须把目标值复制到 anl_context。
			 */
			old_context = MemoryContextSwitchTo(stats->anl_context);
			corrs = (float4 *) palloc(sizeof(float4));
			MemoryContextSwitchTo(old_context);

			/*----------
			 * Since we know the x and y value sets are both
			 *		0, 1, ..., values_cnt-1
			 * we have sum(x) = sum(y) =
			 *		(values_cnt-1)*values_cnt / 2
			 * and sum(x^2) = sum(y^2) =
			 *		(values_cnt-1)*values_cnt*(2*values_cnt-1) / 6.
			 *
			 *		由于已知 x 和 y 的取值集合都是 0, 1, ..., values_cnt-1，因
			 *		此 sum(x) = sum(y) = (values_cnt-1)*values_cnt / 2，且 sum
			 *		(x^2) = sum(y^2) = (values_cnt-1)*values_cnt*(2*values_cnt
			 *		-1) / 6。
			 *----------
			 */
			corr_xsum = ((double) (values_cnt - 1)) *
				((double) values_cnt) / 2.0;
			corr_x2sum = ((double) (values_cnt - 1)) *
				((double) values_cnt) * (double) (2 * values_cnt - 1) / 6.0;

			/* And the correlation coefficient reduces to */
			/*
			 *
			 * 于是相关系数简化为这个公式。
			 */
			corrs[0] = (values_cnt * corr_xysum - corr_xsum * corr_xsum) /
				(values_cnt * corr_x2sum - corr_xsum * corr_xsum);

			stats->stakind[slot_idx] = STATISTIC_KIND_CORRELATION;
			stats->staop[slot_idx] = mystats->ltopr;
			stats->stacoll[slot_idx] = stats->attrcollid;
			stats->stanumbers[slot_idx] = corrs;
			stats->numnumbers[slot_idx] = 1;
			slot_idx++;
		}
	}
	else if (nonnull_cnt > 0)
	{
		/* We found some non-null values, but they were all too wide */
		/*
		 *
		 * 找到了一些非空值，但它们都太宽。
		 */
		Assert(nonnull_cnt == toowide_cnt);
		stats->stats_valid = true;
		/* Do the simple null-frac and width stats */
		/*
		 *
		 * 做简单的空值比例和宽度统计。
		 */
		stats->stanullfrac = (double) null_cnt / (double) samplerows;
		if (is_varwidth)
			stats->stawidth = total_width / (double) nonnull_cnt;
		else
			stats->stawidth = stats->attrtype->typlen;
		/* Assume all too-wide values are distinct, so it's a unique column */
		/*
		 *
		 * 假定所有过宽的值都互不相同，因此它是唯一列。
		 */
		stats->stadistinct = -1.0 * (1.0 - stats->stanullfrac);
	}
	else if (null_cnt > 0)
	{
		/* We found only nulls; assume the column is entirely null */
		/*
		 *
		 * 只找到了空值；假定该列完全为空。
		 */
		stats->stats_valid = true;
		stats->stanullfrac = 1.0;
		if (is_varwidth)
			stats->stawidth = 0;	/* "unknown" */
			/*
			 *
			 * 未知。
			 */
		else
			stats->stawidth = stats->attrtype->typlen;
		stats->stadistinct = 0.0;	/* "unknown" */
		/*
		 *
		 * 未知。
		 */
	}

	/* We don't need to bother cleaning up any of our temporary palloc's */
	/*
	 *
	 * 不必费心清理任何临时的 palloc。
	 */
}

/*
 * Comparator for sorting ScalarItems
 *
 * 用于对 ScalarItem 排序的比较函数。
 *
 * Aside from sorting the items, we update the tupnoLink[] array
 * whenever two ScalarItems are found to contain equal datums.  The array
 * is indexed by tupno; for each ScalarItem, it contains the highest
 * tupno that that item's datum has been found to be equal to.  This allows
 * us to avoid additional comparisons in compute_scalar_stats().
 *
 * 除了排序这些项，每当发现两个 ScalarItem 含有相等的 datum 时，还更新
 * tupnoLink[] 数组。该数组按下标 tupno 索引；对每个 ScalarItem，它包含该项的
 * datum 被发现相等的最高 tupno。这样 compute_scalar_stats() 就可以避免额外的
 * 比较。
 */
static int
compare_scalars(const void *a, const void *b, void *arg)
{
	Datum		da = ((const ScalarItem *) a)->value;
	int			ta = ((const ScalarItem *) a)->tupno;
	Datum		db = ((const ScalarItem *) b)->value;
	int			tb = ((const ScalarItem *) b)->tupno;
	CompareScalarsContext *cxt = (CompareScalarsContext *) arg;
	int			compare;

	compare = ApplySortComparator(da, false, db, false, cxt->ssup);
	if (compare != 0)
		return compare;

	/*
	 * The two datums are equal, so update cxt->tupnoLink[].
	 *
	 * 两个 datum 相等，因此更新 cxt->tupnoLink[]。
	 */
	if (cxt->tupnoLink[ta] < tb)
		cxt->tupnoLink[ta] = tb;
	if (cxt->tupnoLink[tb] < ta)
		cxt->tupnoLink[tb] = ta;

	/*
	 * For equal datums, sort by tupno
	 *
	 * datum 相等时，按 tupno 排序。
	 */
	return ta - tb;
}

/*
 * Comparator for sorting ScalarMCVItems by position
 *
 * 按位置对 ScalarMCVItem 排序的比较函数。
 */
static int
compare_mcvs(const void *a, const void *b, void *arg)
{
	int			da = ((const ScalarMCVItem *) a)->first;
	int			db = ((const ScalarMCVItem *) b)->first;

	return da - db;
}

/*
 * Analyze the list of common values in the sample and decide how many are
 * worth storing in the table's MCV list.
 *
 * 分析样本中常见值的列表，决定有多少个值得存入表的 MCV 列表。
 *
 * mcv_counts is assumed to be a list of the counts of the most common values
 * seen in the sample, starting with the most common.  The return value is the
 * number that are significantly more common than the values not in the list,
 * and which are therefore deemed worth storing in the table's MCV list.
 *
 * 假定 mcv_counts 是样本中最常见值的计数列表，从最常见的开始。返回值是明显比
 * 不在列表中的值更常见、因而值得存入表的 MCV 列表的个数。
 */
static int
analyze_mcv_list(int *mcv_counts,
				 int num_mcv,
				 double stadistinct,
				 double stanullfrac,
				 int samplerows,
				 double totalrows)
{
	double		ndistinct_table;
	double		sumcount;
	int			i;

	/*
	 * If the entire table was sampled, keep the whole list.  This also
	 * protects us against division by zero in the code below.
	 *
	 * 若整张表都被采样，则保留整个列表。这也能避免下面的代码除以零。
	 */
	if (samplerows == totalrows || totalrows <= 1.0)
		return num_mcv;

	/* Re-extract the estimated number of distinct nonnull values in table */
	/*
	 *
	 * 重新取出表中估算的不同非空值个数。
	 */
	ndistinct_table = stadistinct;
	if (ndistinct_table < 0)
		ndistinct_table = -ndistinct_table * totalrows;

	/*
	 * Exclude the least common values from the MCV list, if they are not
	 * significantly more common than the estimated selectivity they would
	 * have if they weren't in the list.  All non-MCV values are assumed to be
	 * equally common, after taking into account the frequencies of all the
	 * values in the MCV list and the number of nulls (c.f. eqsel()).
	 *
	 * 若 MCV 列表中最不常见的值并不明显比它们不在列表中时的估算选择率更常见，
	 * 则把它们排除。所有非 MCV 值在计入 MCV 列表中全部值的频率以及空值个数之
	 * 后，假定同样常见（参见 eqsel()）。
	 *
	 * Here sumcount tracks the total count of all but the last (least common)
	 * value in the MCV list, allowing us to determine the effect of excluding
	 * that value from the list.
	 *
	 * 这里 sumcount 跟踪 MCV 列表中除最后（最不常见）一个值以外的总计数，以便
	 * 判断把该值排除出列表的影响。
	 *
	 * Note that we deliberately do this by removing values from the full
	 * list, rather than starting with an empty list and adding values,
	 * because the latter approach can fail to add any values if all the most
	 * common values have around the same frequency and make up the majority
	 * of the table, so that the overall average frequency of all values is
	 * roughly the same as that of the common values.  This would lead to any
	 * uncommon values being significantly overestimated.
	 *
	 * 注意我们有意通过从完整列表中移除值来做这件事，而不是从空列表开始添加值。
	 * 后一种做法在所有最常见值频率相近且构成表的大部分时会失败，以至于全部值
	 * 的总体平均频率与常见值的频率大致相同。那样会导致任何不常见的值被严重高
	 * 估。
	 */
	sumcount = 0.0;
	for (i = 0; i < num_mcv - 1; i++)
		sumcount += mcv_counts[i];

	while (num_mcv > 0)
	{
		double		selec,
					otherdistinct,
					N,
					n,
					K,
					variance,
					stddev;

		/*
		 * Estimated selectivity the least common value would have if it
		 * wasn't in the MCV list (c.f. eqsel()).
		 *
		 * 最不常见的值若不在 MCV 列表中会具有的估算选择率（参见 eqsel()）。
		 */
		selec = 1.0 - sumcount / samplerows - stanullfrac;
		if (selec < 0.0)
			selec = 0.0;
		if (selec > 1.0)
			selec = 1.0;
		otherdistinct = ndistinct_table - (num_mcv - 1);
		if (otherdistinct > 1)
			selec /= otherdistinct;

		/*
		 * If the value is kept in the MCV list, its population frequency is
		 * assumed to equal its sample frequency.  We use the lower end of a
		 * textbook continuity-corrected Wald-type confidence interval to
		 * determine if that is significantly more common than the non-MCV
		 * frequency --- specifically we assume the population frequency is
		 * highly likely to be within around 2 standard errors of the sample
		 * frequency, which equates to an interval of 2 standard deviations
		 * either side of the sample count, plus an additional 0.5 for the
		 * continuity correction.  Since we are sampling without replacement,
		 * this is a hypergeometric distribution.
		 *
		 * 若该值留在 MCV 列表中，其总体频率假定等于其样本频率。我们用教科书式、
		 * 经连续性校正的 Wald 型置信区间的下端，判断它是否明显比非 MCV 频率更
		 * 常见——具体说，假定总体频率极可能落在样本频率大约 2 个标准误差之内，
		 * 这相当于样本计数两侧各 2 个标准差，再加上 0.5 的连续性校正。由于是
		 * 不放回采样，这是超几何分布。
		 *
		 * XXX: Empirically, this approach seems to work quite well, but it
		 * may be worth considering more advanced techniques for estimating
		 * the confidence interval of the hypergeometric distribution.
		 *
		 * XXX：经验上这种方法效果相当好，但也许值得考虑更高级的技术来估计超几
		 * 何分布的置信区间。
		 */
		N = totalrows;
		n = samplerows;
		K = N * mcv_counts[num_mcv - 1] / n;
		variance = n * K * (N - K) * (N - n) / (N * N * (N - 1));
		stddev = sqrt(variance);

		if (mcv_counts[num_mcv - 1] > selec * samplerows + 2 * stddev + 0.5)
		{
			/*
			 * The value is significantly more common than the non-MCV
			 * selectivity would suggest.  Keep it, and all the other more
			 * common values in the list.
			 *
			 * 该值明显比非 MCV 选择率所暗示的更常见。保留它，以及列表中所有更
			 * 常见的值。
			 */
			break;
		}
		else
		{
			/* Discard this value and consider the next least common value */
			/*
			 *
			 * 丢弃该值，并考虑下一个最不常见的值。
			 */
			num_mcv--;
			if (num_mcv == 0)
				break;
			sumcount -= mcv_counts[num_mcv - 1];
		}
	}
	return num_mcv;
}

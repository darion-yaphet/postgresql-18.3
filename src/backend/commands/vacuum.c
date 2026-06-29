/*-------------------------------------------------------------------------
 *
 * vacuum.c
 *	  The postgres vacuum cleaner.
 *	  PostgreSQL 的 VACUUM 清理器入口与公共逻辑。
 *
 * This file includes (a) control and dispatch code for VACUUM and ANALYZE
 * commands, (b) code to compute various vacuum thresholds, and (c) index
 * vacuum code.
 * 本文件包含：(a) VACUUM/ANALYZE 的调度与控制；
 			 (b) 各类 vacuum 阈值计算；
 			 (c) 索引 vacuum 相关代码。
 *
 * VACUUM for heap AM is implemented in vacuumlazy.c, parallel vacuum in
 * vacuumparallel.c, ANALYZE in analyze.c, and VACUUM FULL is a variant of
 * CLUSTER, handled in cluster.c.
 * 堆访问方法的 VACUUM 在 vacuumlazy.c；并行 vacuum 在 vacuumparallel.c；ANALYZE 在 analyze.c；
 * VACUUM FULL 是 CLUSTER 的变体，在 cluster.c 处理。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/vacuum.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/clog.h"
#include "access/commit_ts.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_database.h"
#include "catalog/pg_inherits.h"
#include "commands/async.h"
#include "commands/cluster.h"
#include "commands/defrem.h"
#include "commands/progress.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "pgstat.h"
#include "postmaster/autovacuum.h"
#include "postmaster/bgworker_internals.h"
#include "postmaster/interrupt.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/acl.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/guc_hooks.h"
#include "utils/injection_point.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/*
 * 核心流程概览：
 * ExecVacuum：解析选项、校验互斥、创建跨事务 vac_context 与 BufferAccessStrategy，调用 vacuum()。
 * vacuum()：展开/收集待处理表、决定是否每表独立事务、循环 vacuum_rel() 与 analyze_rel()，
 *           非 SKIP_DATABASE_STATS 时在末尾 vac_update_datfrozenxid()。
 * vacuum_rel()：单表事务、lazy 时设 PROC_IN_VACUUM、打开表并加锁、VACUUM FULL 走 cluster_rel，
 *               否则 table_relation_vacuum()（vacuumlazy.c），再递归 TOAST。
 * vacuum_get_cutoffs()：计算 OldestXmin / 冻结与 MultiXact 截断线，返回是否 aggressive VACUUM。
 * vac_update_relstats / vac_update_datfrozenxid / vac_truncate_clog：更新 pg_class/pg_database 统计与
 *           冻结线，并可能截断 pg_xact、pg_multixact 等 SLRU。
 * vac_open_indexes / vac_bulkdel_one_index / vac_cleanup_one_index：索引批量删与收尾。
 * vacuum_delay_point()：按 vacuum_cost_* 与并行代价做节流。
 */

/*
 * Minimum interval for cost-based vacuum delay reports from a parallel worker.
 * This aims to avoid sending too many messages and waking up the leader too
 * frequently.
 * 并行 worker 上报基于代价的 vacuum 延迟的最小间隔，避免消息过频、leader 被频繁唤醒。
 */
#define PARALLEL_VACUUM_DELAY_REPORT_INTERVAL_NS	(NS_PER_S)

/*
 * GUC parameters
 * GUC 参数（与 vacuum 冻结年龄、代价、截断等相关）
 */
int			vacuum_freeze_min_age;
int			vacuum_freeze_table_age;
int			vacuum_multixact_freeze_min_age;
int			vacuum_multixact_freeze_table_age;
int			vacuum_failsafe_age;
int			vacuum_multixact_failsafe_age;
double		vacuum_max_eager_freeze_failure_rate;
bool		track_cost_delay_timing;
bool		vacuum_truncate;

/*
 * Variables for cost-based vacuum delay. The defaults differ between
 * autovacuum and vacuum. They should be set with the appropriate GUC value in
 * vacuum code. They are initialized here to the defaults for client backends
 * executing VACUUM or ANALYZE.
 
 * 基于代价的 vacuum 延迟所用变量；autovacuum 与手动 VACUUM 默认值不同，应在 vacuum 路径
 * 中设为对应 GUC。此处初始化为客户端执行 VACUUM/ANALYZE 时的默认。
 */
double		vacuum_cost_delay = 0;
int			vacuum_cost_limit = 200;

/* Variable for reporting cost-based vacuum delay from parallel workers. */
/* 并行 worker 上报的基于代价的 vacuum 延迟累计（纳秒） */
int64		parallel_vacuum_worker_delay_ns = 0;

/*
 * VacuumFailsafeActive is a defined as a global so that we can determine
 * whether or not to re-enable cost-based vacuum delay when vacuuming a table.
 * If failsafe mode has been engaged, we will not re-enable cost-based delay
 * for the table until after vacuuming has completed, regardless of other
 * settings.
 *
 * Only VACUUM code should inspect this variable and only table access methods
 * should set it to true. In Table AM-agnostic VACUUM code, this variable is
 * inspected to determine whether or not to allow cost-based delays. Table AMs
 * are free to set it if they desire this behavior, but it is false by default
 * and reset to false in between vacuuming each relation.
 * 全局标志：是否处于 wraparound failsafe；用于在单表 vacuum 完成前是否重新启用代价延迟。
 * 仅 VACUUM 代码应读；仅表 AM 可置 true；AM 无关路径据此决定是否允许代价延迟；默认 false，
 * 每处理完一表重置。
 */
bool		VacuumFailsafeActive = false;

/*
 * Variables for cost-based parallel vacuum.  See comments atop
 * compute_parallel_delay to understand how it works.
 * 并行 vacuum 的共享代价计数；机制见 compute_parallel_delay 顶部注释。
 */
pg_atomic_uint32 *VacuumSharedCostBalance = NULL;
pg_atomic_uint32 *VacuumActiveNWorkers = NULL;
int			VacuumCostBalanceLocal = 0;

/* non-export function prototypes */
/* 本文件内部静态函数声明 */
static List *expand_vacuum_rel(VacuumRelation *vrel,
							   MemoryContext vac_context, int options);
static List *get_all_vacuum_rels(MemoryContext vac_context, int options);
static void vac_truncate_clog(TransactionId frozenXID,
							  MultiXactId minMulti,
							  TransactionId lastSaneFrozenXid,
							  MultiXactId lastSaneMinMulti);
static bool vacuum_rel(Oid relid, RangeVar *relation, VacuumParams *params,
					   BufferAccessStrategy bstrategy);
static double compute_parallel_delay(void);
static VacOptValue get_vacoptval_from_boolean(DefElem *def);
static bool vac_tid_reaped(ItemPointer itemptr, void *state);

/*
 * GUC check function to ensure GUC value specified is within the allowable
 * range.
 * 校验 vacuum_buffer_usage_limit 等是否在允许范围内。
 */
bool
check_vacuum_buffer_usage_limit(int *newval, void **extra,
								GucSource source)
{
	/* Value upper and lower hard limits are inclusive */
	/* 上下硬限均为闭区间 */
	if (*newval == 0 || (*newval >= MIN_BAS_VAC_RING_SIZE_KB &&
						 *newval <= MAX_BAS_VAC_RING_SIZE_KB))
		return true;

	/* Value does not fall within any allowable range */
	/* 取值不在任何允许区间 */
	GUC_check_errdetail("\"%s\" must be 0 or between %d kB and %d kB.",
						"vacuum_buffer_usage_limit",
						MIN_BAS_VAC_RING_SIZE_KB, MAX_BAS_VAC_RING_SIZE_KB);

	return false;
}

/*
 * Primary entry point for manual VACUUM and ANALYZE commands
 *
 * This is mainly a preparation wrapper for the real operations that will
 * happen in vacuum().
 * 手动 VACUUM/ANALYZE 的主入口；解析选项并准备参数后调用 vacuum() 执行实际工作。
 */
void
ExecVacuum(ParseState *pstate, VacuumStmt *vacstmt, bool isTopLevel)
{
	VacuumParams params;
	BufferAccessStrategy bstrategy = NULL;
	bool		verbose = false;
	bool		skip_locked = false;
	bool		analyze = false;
	bool		freeze = false;
	bool		full = false;
	bool		disable_page_skipping = false;
	bool		process_main = true;
	bool		process_toast = true;
	int			ring_size;
	bool		skip_database_stats = false;
	bool		only_database_stats = false;
	MemoryContext vac_context;
	ListCell   *lc;

	/* index_cleanup and truncate values unspecified for now */
	/* index_cleanup、truncate 暂为未指定，后续可由 reloption 或命令填充 */
	params.index_cleanup = VACOPTVALUE_UNSPECIFIED;
	params.truncate = VACOPTVALUE_UNSPECIFIED;

	/* By default parallel vacuum is enabled */
	/* 默认启用并行 vacuum（0 表示由规划器决定；-1 表示用户显式关闭，见 parallel 选项解析） */
	params.nworkers = 0;

	/* Will be set later if we recurse to a TOAST table. */
	/* 若递归处理 TOAST 表，稍后设置 toast_parent */
	params.toast_parent = InvalidOid;

	/*
	 * Set this to an invalid value so it is clear whether or not a
	 * BUFFER_USAGE_LIMIT was specified when making the access strategy.
	 * 置为无效值，以便区分命令是否显式指定 BUFFER_USAGE_LIMIT。
	 */
	ring_size = -1;

	/* Parse options list */
	/* 遍历并解析 VACUUM/ANALYZE 选项子句 */
	foreach(lc, vacstmt->options)
	{
		DefElem    *opt = (DefElem *) lfirst(lc);

		/* Parse common options for VACUUM and ANALYZE */
		/* VACUUM 与 ANALYZE 共有的选项 */
		if (strcmp(opt->defname, "verbose") == 0)
			verbose = defGetBoolean(opt);
		else if (strcmp(opt->defname, "skip_locked") == 0)
			skip_locked = defGetBoolean(opt);
		else if (strcmp(opt->defname, "buffer_usage_limit") == 0)
		{
			const char *hintmsg;
			int			result;
			char	   *vac_buffer_size;

			vac_buffer_size = defGetString(opt);

			/*
			 * Check that the specified value is valid and the size falls
			 * within the hard upper and lower limits if it is not 0.
			 * 校验 BUFFER_USAGE_LIMIT 合法且非 0 时在硬限范围内。
			 */
			if (!parse_int(vac_buffer_size, &result, GUC_UNIT_KB, &hintmsg) ||
				(result != 0 &&
				 (result < MIN_BAS_VAC_RING_SIZE_KB || result > MAX_BAS_VAC_RING_SIZE_KB)))
			{
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("BUFFER_USAGE_LIMIT option must be 0 or between %d kB and %d kB",
								MIN_BAS_VAC_RING_SIZE_KB, MAX_BAS_VAC_RING_SIZE_KB),
						 hintmsg ? errhint("%s", _(hintmsg)) : 0));
			}

			ring_size = result;
		}
		else if (!vacstmt->is_vacuumcmd)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized %s option \"%s\"",
							"ANALYZE", opt->defname),
					 parser_errposition(pstate, opt->location)));

		/* Parse options available on VACUUM */
		/* 仅 VACUUM 支持的选项 */
		else if (strcmp(opt->defname, "analyze") == 0)
			analyze = defGetBoolean(opt);
		else if (strcmp(opt->defname, "freeze") == 0)
			freeze = defGetBoolean(opt);
		else if (strcmp(opt->defname, "full") == 0)
			full = defGetBoolean(opt);
		else if (strcmp(opt->defname, "disable_page_skipping") == 0)
			disable_page_skipping = defGetBoolean(opt);
		else if (strcmp(opt->defname, "index_cleanup") == 0)
		{
			/* Interpret no string as the default, which is 'auto' */
			/* 无参时等价于 'auto' */
			if (!opt->arg)
				params.index_cleanup = VACOPTVALUE_AUTO;
			else
			{
				char	   *sval = defGetString(opt);

				/* Try matching on 'auto' string, or fall back on boolean */
				/* 匹配字符串 'auto'，否则按布尔解析 */
				if (pg_strcasecmp(sval, "auto") == 0)
					params.index_cleanup = VACOPTVALUE_AUTO;
				else
					params.index_cleanup = get_vacoptval_from_boolean(opt);
			}
		}
		else if (strcmp(opt->defname, "process_main") == 0)
			process_main = defGetBoolean(opt);
		else if (strcmp(opt->defname, "process_toast") == 0)
			process_toast = defGetBoolean(opt);
		else if (strcmp(opt->defname, "truncate") == 0)
			params.truncate = get_vacoptval_from_boolean(opt);
		else if (strcmp(opt->defname, "parallel") == 0)
		{
			if (opt->arg == NULL)
			{
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("parallel option requires a value between 0 and %d",
								MAX_PARALLEL_WORKER_LIMIT),
						 parser_errposition(pstate, opt->location)));
			}
			else
			{
				int			nworkers;

				nworkers = defGetInt32(opt);
				if (nworkers < 0 || nworkers > MAX_PARALLEL_WORKER_LIMIT)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("parallel workers for vacuum must be between 0 and %d",
									MAX_PARALLEL_WORKER_LIMIT),
							 parser_errposition(pstate, opt->location)));

				/*
				 * Disable parallel vacuum, if user has specified parallel
				 * degree as zero.
				 * 用户指定 parallel 0 时显式关闭并行 vacuum（nworkers = -1）。
				 */
				if (nworkers == 0)
					params.nworkers = -1;
				else
					params.nworkers = nworkers;
			}
		}
		else if (strcmp(opt->defname, "skip_database_stats") == 0)
			skip_database_stats = defGetBoolean(opt);
		else if (strcmp(opt->defname, "only_database_stats") == 0)
			only_database_stats = defGetBoolean(opt);
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized %s option \"%s\"",
							"VACUUM", opt->defname),
					 parser_errposition(pstate, opt->location)));
	}

	/* Set vacuum options */
	/* 将解析结果折叠为 VACOPT_* 位掩码 */
	params.options =
		(vacstmt->is_vacuumcmd ? VACOPT_VACUUM : VACOPT_ANALYZE) |
		(verbose ? VACOPT_VERBOSE : 0) |
		(skip_locked ? VACOPT_SKIP_LOCKED : 0) |
		(analyze ? VACOPT_ANALYZE : 0) |
		(freeze ? VACOPT_FREEZE : 0) |
		(full ? VACOPT_FULL : 0) |
		(disable_page_skipping ? VACOPT_DISABLE_PAGE_SKIPPING : 0) |
		(process_main ? VACOPT_PROCESS_MAIN : 0) |
		(process_toast ? VACOPT_PROCESS_TOAST : 0) |
		(skip_database_stats ? VACOPT_SKIP_DATABASE_STATS : 0) |
		(only_database_stats ? VACOPT_ONLY_DATABASE_STATS : 0);

	/* sanity checks on options */
	/* 选项一致性断言 */
	Assert(params.options & (VACOPT_VACUUM | VACOPT_ANALYZE));
	Assert((params.options & VACOPT_VACUUM) ||
		   !(params.options & (VACOPT_FULL | VACOPT_FREEZE)));

	if ((params.options & VACOPT_FULL) && params.nworkers > 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("VACUUM FULL cannot be performed in parallel")));

	/*
	 * BUFFER_USAGE_LIMIT does nothing for VACUUM (FULL) so just raise an
	 * ERROR for that case.  VACUUM (FULL, ANALYZE) does make use of it, so
	 * we'll permit that.
	 * 纯 VACUUM FULL 不使用 BUFFER_USAGE_LIMIT；带 ANALYZE 时 ANALYZE 仍可用，故允许组合。
	 */
	if (ring_size != -1 && (params.options & VACOPT_FULL) &&
		!(params.options & VACOPT_ANALYZE))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("BUFFER_USAGE_LIMIT cannot be specified for VACUUM FULL")));

	/*
	 * Make sure VACOPT_ANALYZE is specified if any column lists are present.
	 * 若指定了列列表则必须带 ANALYZE。
	 */
	if (!(params.options & VACOPT_ANALYZE))
	{
		foreach(lc, vacstmt->rels)
		{
			VacuumRelation *vrel = lfirst_node(VacuumRelation, lc);

			if (vrel->va_cols != NIL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("ANALYZE option must be specified when a column list is provided")));
		}
	}


	/*
	 * Sanity check DISABLE_PAGE_SKIPPING option.
	 * DISABLE_PAGE_SKIPPING 不能与 FULL 同用。
	 */
	if ((params.options & VACOPT_FULL) != 0 &&
		(params.options & VACOPT_DISABLE_PAGE_SKIPPING) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("VACUUM option DISABLE_PAGE_SKIPPING cannot be used with FULL")));

	/* sanity check for PROCESS_TOAST */
	/* VACUUM FULL 必须处理 TOAST */
	if ((params.options & VACOPT_FULL) != 0 &&
		(params.options & VACOPT_PROCESS_TOAST) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("PROCESS_TOAST required with VACUUM FULL")));

	/* sanity check for ONLY_DATABASE_STATS */
	if (params.options & VACOPT_ONLY_DATABASE_STATS)
	{
		Assert(params.options & VACOPT_VACUUM);
		if (vacstmt->rels != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("ONLY_DATABASE_STATS cannot be specified with a list of tables")));
		/* don't require people to turn off PROCESS_TOAST/MAIN explicitly */
		/* 仅数据库级统计时不要求用户显式关 PROCESS_TOAST/MAIN */
		if (params.options & ~(VACOPT_VACUUM |
							   VACOPT_VERBOSE |
							   VACOPT_PROCESS_MAIN |
							   VACOPT_PROCESS_TOAST |
							   VACOPT_ONLY_DATABASE_STATS))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("ONLY_DATABASE_STATS cannot be specified with other VACUUM options")));
	}

	/*
	 * All freeze ages are zero if the FREEZE option is given; otherwise pass
	 * them as -1 which means to use the default values.
	 * FREEZE 时各冻结年龄为 0；否则传 -1 表示后续用 GUC 默认。
	 */
	if (params.options & VACOPT_FREEZE)
	{
		params.freeze_min_age = 0;
		params.freeze_table_age = 0;
		params.multixact_freeze_min_age = 0;
		params.multixact_freeze_table_age = 0;
	}
	else
	{
		params.freeze_min_age = -1;
		params.freeze_table_age = -1;
		params.multixact_freeze_min_age = -1;
		params.multixact_freeze_table_age = -1;
	}

	/* user-invoked vacuum is never "for wraparound" */
	/* 用户发起的 VACUUM 不设 wraparound 紧急标志 */
	params.is_wraparound = false;

	/* user-invoked vacuum uses VACOPT_VERBOSE instead of log_min_duration */
	/* 手动 VACUUM 用 VERBOSE 选项而非 log_min_duration */
	params.log_min_duration = -1;

	/*
	 * Later, in vacuum_rel(), we check if a reloption override was specified.
	 * eager 冻结失败率上限可在 vacuum_rel 中由 reloption 覆盖。
	 */
	params.max_eager_freeze_failure_rate = vacuum_max_eager_freeze_failure_rate;

	/*
	 * Create special memory context for cross-transaction storage.
	 *
	 * Since it is a child of PortalContext, it will go away eventually even
	 * if we suffer an error; there's no need for special abort cleanup logic.
	 * 跨事务存活的工作内存上下文；挂在 PortalContext 下，出错也会随门户释放，无需专门 abort 清理。
	 */
	vac_context = AllocSetContextCreate(PortalContext,
										"Vacuum",
										ALLOCSET_DEFAULT_SIZES);

	/*
	 * Make a buffer strategy object in the cross-transaction memory context.
	 * We needn't bother making this for VACUUM (FULL) or VACUUM
	 * (ONLY_DATABASE_STATS) as they'll not make use of it.  VACUUM (FULL,
	 * ANALYZE) is possible, so we'd better ensure that we make a strategy
	 * when we see ANALYZE.
	 * 在跨事务上下文中创建缓冲访问策略；FULL 与 ONLY_DATABASE_STATS 不需要；
	 * FULL+ANALYZE 时 ANALYZE 仍需要，故见 ANALYZE 则创建。
	 */
	if ((params.options & (VACOPT_ONLY_DATABASE_STATS |
						   VACOPT_FULL)) == 0 ||
		(params.options & VACOPT_ANALYZE) != 0)
	{

		MemoryContext old_context = MemoryContextSwitchTo(vac_context);

		Assert(ring_size >= -1);

		/*
		 * If BUFFER_USAGE_LIMIT was specified by the VACUUM or ANALYZE
		 * command, it overrides the value of VacuumBufferUsageLimit.  Either
		 * value may be 0, in which case GetAccessStrategyWithSize() will
		 * return NULL, effectively allowing full use of shared buffers.
		 * 命令指定的 BUFFER_USAGE_LIMIT 覆盖 GUC；0 表示不限制 ring，策略可为 NULL。
		 */
		if (ring_size == -1)
			ring_size = VacuumBufferUsageLimit;

		bstrategy = GetAccessStrategyWithSize(BAS_VACUUM, ring_size);

		MemoryContextSwitchTo(old_context);
	}

	/* Now go through the common routine */
	/* 进入与 autovacuum 共用的 vacuum() 分发逻辑 */
	vacuum(vacstmt->rels, &params, bstrategy, vac_context, isTopLevel);

	/* Finally, clean up the vacuum memory context */
	/* 删除跨事务 vac 上下文 */
	MemoryContextDelete(vac_context);
}

/*
 * Internal entry point for autovacuum and the VACUUM / ANALYZE commands.
 *
 * relations, if not NIL, is a list of VacuumRelation to process; otherwise,
 * we process all relevant tables in the database.  For each VacuumRelation,
 * if a valid OID is supplied, the table with that OID is what to process;
 * otherwise, the VacuumRelation's RangeVar indicates what to process.
 *
 * params contains a set of parameters that can be used to customize the
 * behavior.
 *
 * bstrategy may be passed in as NULL when the caller does not want to
 * restrict the number of shared_buffers that VACUUM / ANALYZE can use,
 * otherwise, the caller must build a BufferAccessStrategy with the number of
 * shared_buffers that VACUUM / ANALYZE should try to limit themselves to
 * using.
 *
 * isTopLevel should be passed down from ProcessUtility.
 *
 * It is the caller's responsibility that all parameters are allocated in a
 * memory context that will not disappear at transaction commit.
 * autovacuum 与 SQL VACUUM/ANALYZE 的内部入口。relations 非 NIL 时处理列表中的 VacuumRelation，
 * 否则处理库内所有可 vacuum 的表。params 定制行为；bstrategy 限制使用的 shared_buffers（NULL 不限制）。
 * isTopLevel 来自 ProcessUtility。调用方须保证参数所在内存上下文在提交后仍存在。
 */
void
vacuum(List *relations, VacuumParams *params, BufferAccessStrategy bstrategy,
	   MemoryContext vac_context, bool isTopLevel)
{
	static bool in_vacuum = false;

	const char *stmttype;
	volatile bool in_outer_xact,
				use_own_xacts;

	Assert(params != NULL);

	stmttype = (params->options & VACOPT_VACUUM) ? "VACUUM" : "ANALYZE";

	/*
	 * We cannot run VACUUM inside a user transaction block; if we were inside
	 * a transaction, then our commit- and start-transaction-command calls
	 * would not have the intended effect!	There are numerous other subtle
	 * dependencies on this, too.
	 *
	 * ANALYZE (without VACUUM) can run either way.
	 * VACUUM 不能在用户事务块内执行，否则内部的提交/启事务语义错乱。纯 ANALYZE 两种都可。
	 */
	if (params->options & VACOPT_VACUUM)
	{
		PreventInTransactionBlock(isTopLevel, stmttype);
		in_outer_xact = false;
	}
	else
		in_outer_xact = IsInTransactionBlock(isTopLevel);

	/*
	 * Check for and disallow recursive calls.  This could happen when VACUUM
	 * FULL or ANALYZE calls a hostile index expression that itself calls
	 * ANALYZE.
	 * 禁止递归（例如索引表达式里再调 ANALYZE）。
	 */
	if (in_vacuum)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("%s cannot be executed from VACUUM or ANALYZE",
						stmttype)));

	/*
	 * Build list of relation(s) to process, putting any new data in
	 * vac_context for safekeeping.
	 * 构建待处理表列表，新分配的数据放入 vac_context。
	 */
	if (params->options & VACOPT_ONLY_DATABASE_STATS)
	{
		/* We don't process any tables in this case */
		/* ONLY_DATABASE_STATS：不处理任何表 */
		Assert(relations == NIL);
	}
	else if (relations != NIL)
	{
		List	   *newrels = NIL;
		ListCell   *lc;

		foreach(lc, relations)
		{
			VacuumRelation *vrel = lfirst_node(VacuumRelation, lc);
			List	   *sublist;
			MemoryContext old_context;

			sublist = expand_vacuum_rel(vrel, vac_context, params->options);
			old_context = MemoryContextSwitchTo(vac_context);
			newrels = list_concat(newrels, sublist);
			MemoryContextSwitchTo(old_context);
		}
		relations = newrels;
	}
	else
		relations = get_all_vacuum_rels(vac_context, params->options);

	/*
	 * Decide whether we need to start/commit our own transactions.
	 *
	 * For VACUUM (with or without ANALYZE): always do so, so that we can
	 * release locks as soon as possible.  (We could possibly use the outer
	 * transaction for a one-table VACUUM, but handling TOAST tables would be
	 * problematic.)
	 *
	 * For ANALYZE (no VACUUM): if inside a transaction block, we cannot
	 * start/commit our own transactions.  Also, there's no need to do so if
	 * only processing one relation.  For multiple relations when not within a
	 * transaction block, and also in an autovacuum worker, use own
	 * transactions so we can release locks sooner.
	 * 是否每步自管事务：VACUUM 始终自管以便尽快放锁。纯 ANALYZE 在事务块内或单表时可用外层事务；
	 * 多表且不在块内或 autovacuum worker 则自管事务。
	 */
	if (params->options & VACOPT_VACUUM)
		use_own_xacts = true;
	else
	{
		Assert(params->options & VACOPT_ANALYZE);
		if (AmAutoVacuumWorkerProcess())
			use_own_xacts = true;
		else if (in_outer_xact)
			use_own_xacts = false;
		else if (list_length(relations) > 1)
			use_own_xacts = true;
		else
			use_own_xacts = false;
	}

	/*
	 * vacuum_rel expects to be entered with no transaction active; it will
	 * start and commit its own transaction.  But we are called by an SQL
	 * command, and so we are executing inside a transaction already. We
	 * commit the transaction started in PostgresMain() here, and start
	 * another one before exiting to match the commit waiting for us back in
	 * PostgresMain().
	 * vacuum_rel 要求进入时无活跃事务；SQL 调用时外层已有事务，故先 Commit 再在各表内启停事务，
	 * 结束前再 StartTransaction 与 PostgresMain 配对。
	 */
	if (use_own_xacts)
	{
		Assert(!in_outer_xact);

		/* ActiveSnapshot is not set by autovacuum */
		/* autovacuum 不设 ActiveSnapshot；手动路径先弹出 */
		if (ActiveSnapshotSet())
			PopActiveSnapshot();

		/* matches the StartTransaction in PostgresMain() */
		/* 与 PostgresMain 里 StartTransaction 成对 */
		CommitTransactionCommand();
	}

	/* Turn vacuum cost accounting on or off, and set/clear in_vacuum */
	/* 打开 vacuum 代价统计并置 in_vacuum 标志 */
	PG_TRY();
	{
		ListCell   *cur;

		in_vacuum = true;
		VacuumFailsafeActive = false;
		VacuumUpdateCosts();
		VacuumCostBalance = 0;
		VacuumCostBalanceLocal = 0;
		VacuumSharedCostBalance = NULL;
		VacuumActiveNWorkers = NULL;

		/*
		 * Loop to process each selected relation.
		 * 逐表：先 VACUUM（若请求）再 ANALYZE（若请求）。
		 */
		foreach(cur, relations)
		{
			VacuumRelation *vrel = lfirst_node(VacuumRelation, cur);

			if (params->options & VACOPT_VACUUM)
			{
				VacuumParams params_copy;

				/*
				 * vacuum_rel() scribbles on the parameters, so give it a copy
				 * to avoid affecting other relations.
				 * vacuum_rel 会改写 params，故每表用副本。
				 */
				memcpy(&params_copy, params, sizeof(VacuumParams));

				if (!vacuum_rel(vrel->oid, vrel->relation, &params_copy, bstrategy))
					continue;
			}

			if (params->options & VACOPT_ANALYZE)
			{
				/*
				 * If using separate xacts, start one for analyze. Otherwise,
				 * we can use the outer transaction.
				 * 独立事务时 ANALYZE 单独启事务并 PushSnapshot。
				 */
				if (use_own_xacts)
				{
					StartTransactionCommand();
					/* functions in indexes may want a snapshot set */
					/* 索引表达式可能需要快照 */
					PushActiveSnapshot(GetTransactionSnapshot());
				}

				analyze_rel(vrel->oid, vrel->relation, params,
							vrel->va_cols, in_outer_xact, bstrategy);

				if (use_own_xacts)
				{
					PopActiveSnapshot();
					/* standard_ProcessUtility() does CCI if !use_own_xacts */
					/* 自管事务路径在此 CCI */
					CommandCounterIncrement();
					CommitTransactionCommand();
				}
				else
				{
					/*
					 * If we're not using separate xacts, better separate the
					 * ANALYZE actions with CCIs.  This avoids trouble if user
					 * says "ANALYZE t, t".
					 * 外层事务时用 CCI 分隔多次 ANALYZE，避免同表重复分析问题。
					 */
					CommandCounterIncrement();
				}
			}

			/*
			 * Ensure VacuumFailsafeActive has been reset before vacuuming the
			 * next relation.
			 * 每表结束后重置 failsafe 标志。
			 */
			VacuumFailsafeActive = false;
		}
	}
	PG_FINALLY();
	{
		in_vacuum = false;
		VacuumCostActive = false;
		VacuumFailsafeActive = false;
		VacuumCostBalance = 0;
	}
	PG_END_TRY();

	/*
	 * Finish up processing.
	 * 收尾：若曾退出外层事务，此处重新 StartTransaction。
	 */
	if (use_own_xacts)
	{
		/* here, we are not in a transaction */
		/* 当前不在事务中 */

		/*
		 * This matches the CommitTransaction waiting for us in
		 * PostgresMain().
		 * 与 PostgresMain 末尾等待的 Commit 配对。
		 */
		StartTransactionCommand();
	}

	if ((params->options & VACOPT_VACUUM) &&
		!(params->options & VACOPT_SKIP_DATABASE_STATS))
	{
		/*
		 * Update pg_database.datfrozenxid, and truncate pg_xact if possible.
		 * 更新库级 datfrozenxid/datminmxid 并尝试截断 CLOG 等。
		 */
		vac_update_datfrozenxid();
	}

}

/*
 * Check if the current user has privileges to vacuum or analyze the relation.
 * If not, issue a WARNING log message and return false to let the caller
 * decide what to do with this relation.  This routine is used to decide if a
 * relation can be processed for VACUUM or ANALYZE.
 * 检查当前用户是否有 MAINTAIN 等权限对关系做 VACUUM/ANALYZE；无则 WARNING 并返回 false。
 */
bool
vacuum_is_permitted_for_relation(Oid relid, Form_pg_class reltuple,
								 bits32 options)
{
	char	   *relname;

	Assert((options & (VACOPT_VACUUM | VACOPT_ANALYZE)) != 0);

	/*----------
	 * A role has privileges to vacuum or analyze the relation if any of the
	 * following are true:
	 *   - the role owns the current database and the relation is not shared
	 *   - the role has the MAINTAIN privilege on the relation
	 *----------
	 * 满足任一即可：拥有当前库且关系非共享；或对关系有 MAINTAIN。
	 */
	if ((object_ownercheck(DatabaseRelationId, MyDatabaseId, GetUserId()) &&
		 !reltuple->relisshared) ||
		pg_class_aclcheck(relid, GetUserId(), ACL_MAINTAIN) == ACLCHECK_OK)
		return true;

	relname = NameStr(reltuple->relname);

	if ((options & VACOPT_VACUUM) != 0)
	{
		ereport(WARNING,
				(errmsg("permission denied to vacuum \"%s\", skipping it",
						relname)));

		/*
		 * For VACUUM ANALYZE, both logs could show up, but just generate
		 * information for VACUUM as that would be the first one to be
		 * processed.
		 * VACUUM ANALYZE 组合时先报 VACUUM 侧权限问题即可。
		 */
		return false;
	}

	if ((options & VACOPT_ANALYZE) != 0)
		ereport(WARNING,
				(errmsg("permission denied to analyze \"%s\", skipping it",
						relname)));

	return false;
}


/*
 * vacuum_open_relation
 *
 * This routine is used for attempting to open and lock a relation which
 * is going to be vacuumed or analyzed.  If the relation cannot be opened
 * or locked, a log is emitted if possible.
 * 尝试打开并加锁待 vacuum/analyze 的关系；失败时在合适级别记日志。
 */
Relation
vacuum_open_relation(Oid relid, RangeVar *relation, bits32 options,
					 bool verbose, LOCKMODE lmode)
{
	Relation	rel;
	bool		rel_lock = true;
	int			elevel;

	Assert((options & (VACOPT_VACUUM | VACOPT_ANALYZE)) != 0);

	/*
	 * Open the relation and get the appropriate lock on it.
	 *
	 * There's a race condition here: the relation may have gone away since
	 * the last time we saw it.  If so, we don't need to vacuum or analyze it.
	 *
	 * If we've been asked not to wait for the relation lock, acquire it first
	 * in non-blocking mode, before calling try_relation_open().
	 * 打开关系并加锁；关系可能已删除。SKIP_LOCKED 时先尝试非阻塞锁再打开。
	 */
	if (!(options & VACOPT_SKIP_LOCKED))
		rel = try_relation_open(relid, lmode);
	else if (ConditionalLockRelationOid(relid, lmode))
		rel = try_relation_open(relid, NoLock);
	else
	{
		rel = NULL;
		rel_lock = false;
	}

	/* if relation is opened, leave */
	/* 成功打开则直接返回 */
	if (rel)
		return rel;

	/*
	 * Relation could not be opened, hence generate if possible a log
	 * informing on the situation.
	 *
	 * If the RangeVar is not defined, we do not have enough information to
	 * provide a meaningful log statement.  Chances are that the caller has
	 * intentionally not provided this information so that this logging is
	 * skipped, anyway.
	 * 打开失败则尽量打日志；无 RangeVar 时跳过（多为 OID 列表路径）。
	 */
	if (relation == NULL)
		return NULL;

	/*
	 * Determine the log level.
	 *
	 * For manual VACUUM or ANALYZE, we emit a WARNING to match the log
	 * statements in the permission checks; otherwise, only log if the caller
	 * so requested.
	 * 手动执行用 WARNING；autovacuum 仅 verbose 时 LOG。
	 */
	if (!AmAutoVacuumWorkerProcess())
		elevel = WARNING;
	else if (verbose)
		elevel = LOG;
	else
		return NULL;

	if ((options & VACOPT_VACUUM) != 0)
	{
		if (!rel_lock)
			ereport(elevel,
					(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
					 errmsg("skipping vacuum of \"%s\" --- lock not available",
							relation->relname)));
		else
			ereport(elevel,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("skipping vacuum of \"%s\" --- relation no longer exists",
							relation->relname)));

		/*
		 * For VACUUM ANALYZE, both logs could show up, but just generate
		 * information for VACUUM as that would be the first one to be
		 * processed.
		 * 同权限检查：VACUUM ANALYZE 先只报 vacuum 侧。
		 */
		return NULL;
	}

	if ((options & VACOPT_ANALYZE) != 0)
	{
		if (!rel_lock)
			ereport(elevel,
					(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
					 errmsg("skipping analyze of \"%s\" --- lock not available",
							relation->relname)));
		else
			ereport(elevel,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("skipping analyze of \"%s\" --- relation no longer exists",
							relation->relname)));
	}

	return NULL;
}


/*
 * Given a VacuumRelation, fill in the table OID if it wasn't specified,
 * and optionally add VacuumRelations for partitions or inheritance children.
 *
 * If a VacuumRelation does not have an OID supplied and is a partitioned
 * table, an extra entry will be added to the output for each partition.
 * Presently, only autovacuum supplies OIDs when calling vacuum(), and
 * it does not want us to expand partitioned tables.
 *
 * We take care not to modify the input data structure, but instead build
 * new VacuumRelation(s) to return.  (But note that they will reference
 * unmodified parts of the input, eg column lists.)  New data structures
 * are made in vac_context.
 * 解析 RangeVar 得 OID；对分区/继承可展开子表。autovacuum 传 OID 故不展开分区。
 * 不修改输入，新节点在 vac_context 分配。
 */
static List *
expand_vacuum_rel(VacuumRelation *vrel, MemoryContext vac_context,
				  int options)
{
	List	   *vacrels = NIL;
	MemoryContext oldcontext;

	/* If caller supplied OID, there's nothing we need do here. */
	/* 调用方已给 OID 则原样加入列表 */
	if (OidIsValid(vrel->oid))
	{
		oldcontext = MemoryContextSwitchTo(vac_context);
		vacrels = lappend(vacrels, vrel);
		MemoryContextSwitchTo(oldcontext);
	}
	else
	{
		/*
		 * Process a specific relation, and possibly partitions or child
		 * tables thereof.
		 * 按名称解析关系，并可展开分区或继承子表。
		 */
		Oid			relid;
		HeapTuple	tuple;
		Form_pg_class classForm;
		bool		include_children;
		bool		is_partitioned_table;
		int			rvr_opts;

		/*
		 * Since autovacuum workers supply OIDs when calling vacuum(), no
		 * autovacuum worker should reach this code.
		 * autovacuum 不应走到无 OID 分支。
		 */
		Assert(!AmAutoVacuumWorkerProcess());

		/*
		 * We transiently take AccessShareLock to protect the syscache lookup
		 * below, as well as find_all_inheritors's expectation that the caller
		 * holds some lock on the starting relation.
		 * 短暂 AccessShareLock 保护 syscache 与 find_all_inheritors 前提。
		 */
		rvr_opts = (options & VACOPT_SKIP_LOCKED) ? RVR_SKIP_LOCKED : 0;
		relid = RangeVarGetRelidExtended(vrel->relation,
										 AccessShareLock,
										 rvr_opts,
										 NULL, NULL);

		/*
		 * If the lock is unavailable, emit the same log statement that
		 * vacuum_rel() and analyze_rel() would.
		 * 拿不到锁时与 vacuum_rel 一致的告警文案。
		 */
		if (!OidIsValid(relid))
		{
			if (options & VACOPT_VACUUM)
				ereport(WARNING,
						(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
						 errmsg("skipping vacuum of \"%s\" --- lock not available",
								vrel->relation->relname)));
			else
				ereport(WARNING,
						(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
						 errmsg("skipping analyze of \"%s\" --- lock not available",
								vrel->relation->relname)));
			return vacrels;
		}

		/*
		 * To check whether the relation is a partitioned table and its
		 * ownership, fetch its syscache entry.
		 * 取 syscache 判断分区表与权限。
		 */
		tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for relation %u", relid);
		classForm = (Form_pg_class) GETSTRUCT(tuple);

		/*
		 * Make a returnable VacuumRelation for this rel if the user has the
		 * required privileges.
		 * 有权限则把本表加入返回列表。
		 */
		if (vacuum_is_permitted_for_relation(relid, classForm, options))
		{
			oldcontext = MemoryContextSwitchTo(vac_context);
			vacrels = lappend(vacrels, makeVacuumRelation(vrel->relation,
														  relid,
														  vrel->va_cols));
			MemoryContextSwitchTo(oldcontext);
		}

		/*
		 * Vacuuming a partitioned table with ONLY will not do anything since
		 * the partitioned table itself is empty.  Issue a warning if the user
		 * requests this.
		 * 分区父表无堆数据，ONLY 无效果，发 WARNING。
		 */
		include_children = vrel->relation->inh;
		is_partitioned_table = (classForm->relkind == RELKIND_PARTITIONED_TABLE);
		if ((options & VACOPT_VACUUM) && is_partitioned_table && !include_children)
			ereport(WARNING,
					(errmsg("VACUUM ONLY of partitioned table \"%s\" has no effect",
							vrel->relation->relname)));

		ReleaseSysCache(tuple);

		/*
		 * Unless the user has specified ONLY, make relation list entries for
		 * its partitions or inheritance child tables.  Note that the list
		 * returned by find_all_inheritors() includes the passed-in OID, so we
		 * have to skip that.  There's no point in taking locks on the
		 * individual partitions or child tables yet, and doing so would just
		 * add unnecessary deadlock risk.  For this last reason, we do not yet
		 * check the ownership of the partitions/tables, which get added to
		 * the list to process.  Ownership will be checked later on anyway.
		 * 非 ONLY 时列出分区/子表；find_all_inheritors 含根 OID 需跳过。暂不锁子表以免死锁，权限稍后验。
		 */
		if (include_children)
		{
			List	   *part_oids = find_all_inheritors(relid, NoLock, NULL);
			ListCell   *part_lc;

			foreach(part_lc, part_oids)
			{
				Oid			part_oid = lfirst_oid(part_lc);

				if (part_oid == relid)
					continue;	/* ignore original table */

				/*
				 * We omit a RangeVar since it wouldn't be appropriate to
				 * complain about failure to open one of these relations
				 * later.
				 * 子表项不设 RangeVar，避免后续打开失败时名称不当。
				 */
				oldcontext = MemoryContextSwitchTo(vac_context);
				vacrels = lappend(vacrels, makeVacuumRelation(NULL,
															  part_oid,
															  vrel->va_cols));
				MemoryContextSwitchTo(oldcontext);
			}
		}

		/*
		 * Release lock again.  This means that by the time we actually try to
		 * process the table, it might be gone or renamed.  In the former case
		 * we'll silently ignore it; in the latter case we'll process it
		 * anyway, but we must beware that the RangeVar doesn't necessarily
		 * identify it anymore.  This isn't ideal, perhaps, but there's little
		 * practical alternative, since we're typically going to commit this
		 * transaction and begin a new one between now and then.  Moreover,
		 * holding locks on multiple relations would create significant risk
		 * of deadlock.
		 * 释放锁：真正 vacuum 时表可能已删或改名；多表长期持锁易死锁，故接受此竞态。
		 */
		UnlockRelationOid(relid, AccessShareLock);
	}

	return vacrels;
}

/*
 * Construct a list of VacuumRelations for all vacuumable rels in
 * the current database.  The list is built in vac_context.
 * 扫描 pg_class，构造当前库所有可 vacuum 的普通表/物化视图/分区父表列表。
 */
static List *
get_all_vacuum_rels(MemoryContext vac_context, int options)
{
	List	   *vacrels = NIL;
	Relation	pgclass;
	TableScanDesc scan;
	HeapTuple	tuple;

	pgclass = table_open(RelationRelationId, AccessShareLock);

	scan = table_beginscan_catalog(pgclass, 0, NULL);

	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_class classForm = (Form_pg_class) GETSTRUCT(tuple);
		MemoryContext oldcontext;
		Oid			relid = classForm->oid;

		/*
		 * We include partitioned tables here; depending on which operation is
		 * to be performed, caller will decide whether to process or ignore
		 * them.
		 * 分区父表也列入；具体是否干活由后续 VACUUM/ANALYZE 路径决定。
		 */
		if (classForm->relkind != RELKIND_RELATION &&
			classForm->relkind != RELKIND_MATVIEW &&
			classForm->relkind != RELKIND_PARTITIONED_TABLE)
			continue;

		/* check permissions of relation */
		/* 权限检查 */
		if (!vacuum_is_permitted_for_relation(relid, classForm, options))
			continue;

		/*
		 * Build VacuumRelation(s) specifying the table OIDs to be processed.
		 * We omit a RangeVar since it wouldn't be appropriate to complain
		 * about failure to open one of these relations later.
		 * 仅 OID、无 RangeVar，与全库扫描路径一致。
		 */
		oldcontext = MemoryContextSwitchTo(vac_context);
		vacrels = lappend(vacrels, makeVacuumRelation(NULL,
													  relid,
													  NIL));
		MemoryContextSwitchTo(oldcontext);
	}

	table_endscan(scan);
	table_close(pgclass, AccessShareLock);

	return vacrels;
}

/*
 * vacuum_get_cutoffs() -- compute OldestXmin and freeze cutoff points
 *
 * The target relation and VACUUM parameters are our inputs.
 *
 * Output parameters are the cutoffs that VACUUM caller should use.
 *
 * Return value indicates if vacuumlazy.c caller should make its VACUUM
 * operation aggressive.  An aggressive VACUUM must advance relfrozenxid up to
 * FreezeLimit (at a minimum), and relminmxid up to MultiXactCutoff (at a
 * minimum).
 * 计算 OldestXmin、MultiXact 视界与 FreezeLimit/MultiXactCutoff 等截断线，写入 cutoffs。
 * 返回 true 表示应做 aggressive VACUUM（至少把 relfrozenxid/relminmxid 推进到上述界限）。
 */
bool
vacuum_get_cutoffs(Relation rel, const VacuumParams *params,
				   struct VacuumCutoffs *cutoffs)
{
	int			freeze_min_age,
				multixact_freeze_min_age,
				freeze_table_age,
				multixact_freeze_table_age,
				effective_multixact_freeze_max_age;
	TransactionId nextXID,
				safeOldestXmin,
				aggressiveXIDCutoff;
	MultiXactId nextMXID,
				safeOldestMxact,
				aggressiveMXIDCutoff;

	/* Use mutable copies of freeze age parameters */
	/* 冻结年龄参数的可变副本 */
	freeze_min_age = params->freeze_min_age;
	multixact_freeze_min_age = params->multixact_freeze_min_age;
	freeze_table_age = params->freeze_table_age;
	multixact_freeze_table_age = params->multixact_freeze_table_age;

	/* Set pg_class fields in cutoffs */
	/* 从 pg_class 拷贝当前 relfrozenxid/relminmxid */
	cutoffs->relfrozenxid = rel->rd_rel->relfrozenxid;
	cutoffs->relminmxid = rel->rd_rel->relminmxid;

	/*
	 * Acquire OldestXmin.
	 *
	 * We can always ignore processes running lazy vacuum.  This is because we
	 * use these values only for deciding which tuples we must keep in the
	 * tables.  Since lazy vacuum doesn't write its XID anywhere (usually no
	 * XID assigned), it's safe to ignore it.  In theory it could be
	 * problematic to ignore lazy vacuums in a full vacuum, but keep in mind
	 * that only one vacuum process can be working on a particular table at
	 * any time, and that each vacuum is always an independent transaction.
	 * 取 OldestXmin；可忽略 lazy vacuum 进程（通常无写入 XID），用于判断行版本可见性。
	 */
	cutoffs->OldestXmin = GetOldestNonRemovableTransactionId(rel);

	Assert(TransactionIdIsNormal(cutoffs->OldestXmin));

	/* Acquire OldestMxact */
	/* 最老仍须保留的 MultiXact */
	cutoffs->OldestMxact = GetOldestMultiXactId();
	Assert(MultiXactIdIsValid(cutoffs->OldestMxact));

	/* Acquire next XID/next MXID values used to apply age-based settings */
	/* 下一 XID/MXID，用于按“年龄”计算冻结阈值 */
	nextXID = ReadNextTransactionId();
	nextMXID = ReadNextMultiXactId();

	/*
	 * Also compute the multixact age for which freezing is urgent.  This is
	 * normally autovacuum_multixact_freeze_max_age, but may be less if we are
	 * short of multixact member space.
	 * 紧急冻结 MultiXact 的年龄上限（成员槽紧张时会更小）。
	 */
	effective_multixact_freeze_max_age = MultiXactMemberFreezeThreshold();

	/*
	 * Almost ready to set freeze output parameters; check if OldestXmin or
	 * OldestMxact are held back to an unsafe degree before we start on that
	 * 若 OldestXmin/Mxact 过旧则 WARNING，提示长事务/复制槽等风险。
	 */
	safeOldestXmin = nextXID - autovacuum_freeze_max_age;
	if (!TransactionIdIsNormal(safeOldestXmin))
		safeOldestXmin = FirstNormalTransactionId;
	safeOldestMxact = nextMXID - effective_multixact_freeze_max_age;
	if (safeOldestMxact < FirstMultiXactId)
		safeOldestMxact = FirstMultiXactId;
	if (TransactionIdPrecedes(cutoffs->OldestXmin, safeOldestXmin))
		ereport(WARNING,
				(errmsg("cutoff for removing and freezing tuples is far in the past"),
				 errhint("Close open transactions soon to avoid wraparound problems.\n"
						 "You might also need to commit or roll back old prepared transactions, or drop stale replication slots.")));
	if (MultiXactIdPrecedes(cutoffs->OldestMxact, safeOldestMxact))
		ereport(WARNING,
				(errmsg("cutoff for freezing multixacts is far in the past"),
				 errhint("Close open transactions soon to avoid wraparound problems.\n"
						 "You might also need to commit or roll back old prepared transactions, or drop stale replication slots.")));

	/*
	 * Determine the minimum freeze age to use: as specified by the caller, or
	 * vacuum_freeze_min_age, but in any case not more than half
	 * autovacuum_freeze_max_age, so that autovacuums to prevent XID
	 * wraparound won't occur too frequently.
	 * 最小冻结年龄：调用方或 GUC，且不超过 autovacuum_freeze_max_age 的一半。
	 */
	if (freeze_min_age < 0)
		freeze_min_age = vacuum_freeze_min_age;
	freeze_min_age = Min(freeze_min_age, autovacuum_freeze_max_age / 2);
	Assert(freeze_min_age >= 0);

	/* Compute FreezeLimit, being careful to generate a normal XID */
	/* FreezeLimit：早于此的 XID 可考虑冻结，且须 <= OldestXmin */
	cutoffs->FreezeLimit = nextXID - freeze_min_age;
	if (!TransactionIdIsNormal(cutoffs->FreezeLimit))
		cutoffs->FreezeLimit = FirstNormalTransactionId;
	/* FreezeLimit must always be <= OldestXmin */
	/* 与 OldestXmin 对齐 */
	if (TransactionIdPrecedes(cutoffs->OldestXmin, cutoffs->FreezeLimit))
		cutoffs->FreezeLimit = cutoffs->OldestXmin;

	/*
	 * Determine the minimum multixact freeze age to use: as specified by
	 * caller, or vacuum_multixact_freeze_min_age, but in any case not more
	 * than half effective_multixact_freeze_max_age, so that autovacuums to
	 * prevent MultiXact wraparound won't occur too frequently.
	 * MultiXact 最小冻结年龄，同样 capped 为有效 max_age 的一半。
	 */
	if (multixact_freeze_min_age < 0)
		multixact_freeze_min_age = vacuum_multixact_freeze_min_age;
	multixact_freeze_min_age = Min(multixact_freeze_min_age,
								   effective_multixact_freeze_max_age / 2);
	Assert(multixact_freeze_min_age >= 0);

	/* Compute MultiXactCutoff, being careful to generate a valid value */
	/* MultiXact 冻结截断，须 <= OldestMxact */
	cutoffs->MultiXactCutoff = nextMXID - multixact_freeze_min_age;
	if (cutoffs->MultiXactCutoff < FirstMultiXactId)
		cutoffs->MultiXactCutoff = FirstMultiXactId;
	/* MultiXactCutoff must always be <= OldestMxact */
	if (MultiXactIdPrecedes(cutoffs->OldestMxact, cutoffs->MultiXactCutoff))
		cutoffs->MultiXactCutoff = cutoffs->OldestMxact;

	/*
	 * Finally, figure out if caller needs to do an aggressive VACUUM or not.
	 *
	 * Determine the table freeze age to use: as specified by the caller, or
	 * the value of the vacuum_freeze_table_age GUC, but in any case not more
	 * than autovacuum_freeze_max_age * 0.95, so that if you have e.g nightly
	 * VACUUM schedule, the nightly VACUUM gets a chance to freeze XIDs before
	 * anti-wraparound autovacuum is launched.
	 * 表级“冻结年龄”与 relfrozenxid 比较，过旧则 aggressive；上限为 max_age*0.95 给例行 VACUUM 留余地。
	 */
	if (freeze_table_age < 0)
		freeze_table_age = vacuum_freeze_table_age;
	freeze_table_age = Min(freeze_table_age, autovacuum_freeze_max_age * 0.95);
	Assert(freeze_table_age >= 0);
	aggressiveXIDCutoff = nextXID - freeze_table_age;
	if (!TransactionIdIsNormal(aggressiveXIDCutoff))
		aggressiveXIDCutoff = FirstNormalTransactionId;
	if (TransactionIdPrecedesOrEquals(cutoffs->relfrozenxid,
									  aggressiveXIDCutoff))
		return true;

	/*
	 * Similar to the above, determine the table freeze age to use for
	 * multixacts: as specified by the caller, or the value of the
	 * vacuum_multixact_freeze_table_age GUC, but in any case not more than
	 * effective_multixact_freeze_max_age * 0.95, so that if you have e.g.
	 * nightly VACUUM schedule, the nightly VACUUM gets a chance to freeze
	 * multixacts before anti-wraparound autovacuum is launched.
	 * MultiXact 表级年龄同理，与 relminmxid 比较决定是否 aggressive。
	 */
	if (multixact_freeze_table_age < 0)
		multixact_freeze_table_age = vacuum_multixact_freeze_table_age;
	multixact_freeze_table_age =
		Min(multixact_freeze_table_age,
			effective_multixact_freeze_max_age * 0.95);
	Assert(multixact_freeze_table_age >= 0);
	aggressiveMXIDCutoff = nextMXID - multixact_freeze_table_age;
	if (aggressiveMXIDCutoff < FirstMultiXactId)
		aggressiveMXIDCutoff = FirstMultiXactId;
	if (MultiXactIdPrecedesOrEquals(cutoffs->relminmxid,
									aggressiveMXIDCutoff))
		return true;

	/* Non-aggressive VACUUM */
	/* 非 aggressive：仅通常清理，不强制推进冻结线到上述界限 */
	return false;
}

/*
 * vacuum_xid_failsafe_check() -- Used by VACUUM's wraparound failsafe
 * mechanism to determine if its table's relfrozenxid and relminmxid are now
 * dangerously far in the past.
 *
 * When we return true, VACUUM caller triggers the failsafe.
 * 判断表冻结线是否危险滞后；true 时启用 failsafe（如可跳过索引 vacuum 以抢进度）。
 */
bool
vacuum_xid_failsafe_check(const struct VacuumCutoffs *cutoffs)
{
	TransactionId relfrozenxid = cutoffs->relfrozenxid;
	MultiXactId relminmxid = cutoffs->relminmxid;
	TransactionId xid_skip_limit;
	MultiXactId multi_skip_limit;
	int			skip_index_vacuum;

	Assert(TransactionIdIsNormal(relfrozenxid));
	Assert(MultiXactIdIsValid(relminmxid));

	/*
	 * Determine the index skipping age to use. In any case no less than
	 * autovacuum_freeze_max_age * 1.05.
	 * XID 侧“跳过索引”年龄阈值，不低于 max_age*1.05。
	 */
	skip_index_vacuum = Max(vacuum_failsafe_age, autovacuum_freeze_max_age * 1.05);

	xid_skip_limit = ReadNextTransactionId() - skip_index_vacuum;
	if (!TransactionIdIsNormal(xid_skip_limit))
		xid_skip_limit = FirstNormalTransactionId;

	if (TransactionIdPrecedes(relfrozenxid, xid_skip_limit))
	{
		/* The table's relfrozenxid is too old */
		/* relfrozenxid 过旧 */
		return true;
	}

	/*
	 * Similar to above, determine the index skipping age to use for
	 * multixact. In any case no less than autovacuum_multixact_freeze_max_age *
	 * 1.05.
	 * MultiXact 侧同理。
	 */
	skip_index_vacuum = Max(vacuum_multixact_failsafe_age,
							autovacuum_multixact_freeze_max_age * 1.05);

	multi_skip_limit = ReadNextMultiXactId() - skip_index_vacuum;
	if (multi_skip_limit < FirstMultiXactId)
		multi_skip_limit = FirstMultiXactId;

	if (MultiXactIdPrecedes(relminmxid, multi_skip_limit))
	{
		/* The table's relminmxid is too old */
		/* relminmxid 过旧 */
		return true;
	}

	return false;
}

/*
 * vac_estimate_reltuples() -- estimate the new value for pg_class.reltuples
 *
 *		If we scanned the whole relation then we should just use the count of
 *		live tuples seen; but if we did not, we should not blindly extrapolate
 *		from that number, since VACUUM may have scanned a quite nonrandom
 *		subset of the table.  When we have only partial information, we take
 *		the old value of pg_class.reltuples/pg_class.relpages as a measurement
 *		of the tuple density in the unscanned pages.
 *
 *		Note: scanned_tuples should count only *live* tuples, since
 *		pg_class.reltuples is defined that way.
 *		根据扫描页数与活元组数估算 pg_class.reltuples；全表扫描直接用计数，
 *		部分扫描则用旧密度估计未扫页。scanned_tuples 须只计活元组。
 */
double
vac_estimate_reltuples(Relation relation,
					   BlockNumber total_pages,
					   BlockNumber scanned_pages,
					   double scanned_tuples)
{
	BlockNumber old_rel_pages = relation->rd_rel->relpages;
	double		old_rel_tuples = relation->rd_rel->reltuples;
	double		old_density;
	double		unscanned_pages;
	double		total_tuples;

	/* If we did scan the whole table, just use the count as-is */
	/* 全表扫描则直接采用扫描到的活元组数 */
	if (scanned_pages >= total_pages)
		return scanned_tuples;

	/*
	 * When successive VACUUM commands scan the same few pages again and
	 * again, without anything from the table really changing, there is a risk
	 * that our beliefs about tuple density will gradually become distorted.
	 * This might be caused by vacuumlazy.c implementation details, such as
	 * its tendency to always scan the last heap page.  Handle that here.
	 * 连续 VACUUM 若反复只扫相同少数页，元组密度估计可能扭曲（lazy 实现常扫堆末页）。
	 *
	 * If the relation is _exactly_ the same size according to the existing
	 * pg_class entry, and only a few of its pages (less than 2%) were
	 * scanned, keep the existing value of reltuples.  Also keep the existing
	 * value when only a subset of rel's pages <= a single page were scanned.
	 *
	 * (Note: we might be returning -1 here.)
	 * 表大小未变且扫描页极少时保留旧 reltuples，避免密度估计漂移（可能仍为 -1）。
	 */
	if (old_rel_pages == total_pages &&
		scanned_pages < (double) total_pages * 0.02)
		return old_rel_tuples;
	if (scanned_pages <= 1)
		return old_rel_tuples;

	/*
	 * If old density is unknown, we can't do much except scale up
	 * scanned_tuples to match total_pages.
	 * 旧统计未知则按扫描比例外推到全表。
	 */
	if (old_rel_tuples < 0 || old_rel_pages == 0)
		return floor((scanned_tuples / scanned_pages) * total_pages + 0.5);

	/*
	 * Okay, we've covered the corner cases.  The normal calculation is to
	 * convert the old measurement to a density (tuples per page), then
	 * estimate the number of tuples in the unscanned pages using that figure,
	 * and finally add on the number of tuples in the scanned pages.
	 * 常规：旧元组/页得密度，未扫页按密度估计，加上已扫页实计数。
	 */
	old_density = old_rel_tuples / old_rel_pages;
	unscanned_pages = (double) total_pages - (double) scanned_pages;
	total_tuples = old_density * unscanned_pages + scanned_tuples;
	return floor(total_tuples + 0.5);
}


/*
 *	vac_update_relstats() -- update statistics for one relation
 *
 *		Update the whole-relation statistics that are kept in its pg_class
 *		row.  There are additional stats that will be updated if we are
 *		doing ANALYZE, but we always update these stats.  This routine works
 *		for both index and heap relation entries in pg_class.
 *
 *		We violate transaction semantics here by overwriting the rel's
 *		existing pg_class tuple with the new values.  This is reasonably
 *		safe as long as we're sure that the new values are correct whether or
 *		not this transaction commits.  The reason for doing this is that if
 *		we updated these tuples in the usual way, vacuuming pg_class itself
 *		wouldn't work very well --- by the time we got done with a vacuum
 *		cycle, most of the tuples in pg_class would've been obsoleted.  Of
 *		course, this only works for fixed-size not-null columns, but these are.
 *
 *		Another reason for doing it this way is that when we are in a lazy
 *		VACUUM and have PROC_IN_VACUUM set, we mustn't do any regular updates.
 *		Somebody vacuuming pg_class might think they could delete a tuple
 *		marked with xmin = our xid.
 *
 *		In addition to fundamentally nontransactional statistics such as
 *		relpages and relallvisible, we try to maintain certain lazily-updated
 *		DDL flags such as relhasindex, by clearing them if no longer correct.
 *		It's safe to do this in VACUUM, which can't run in parallel with
 *		CREATE INDEX/RULE/TRIGGER and can't be part of a transaction block.
 *		However, it's *not* safe to do it in an ANALYZE that's within an
 *		outer transaction, because for example the current transaction might
 *		have dropped the last index; then we'd think relhasindex should be
 *		cleared, but if the transaction later rolls back this would be wrong.
 *		So we refrain from updating the DDL flags if we're inside an outer
 *		transaction.  This is OK since postponing the flag maintenance is
 *		always allowable.
 *
 *		Note: num_tuples should count only *live* tuples, since
 *		pg_class.reltuples is defined that way.
 *
 *		This routine is shared by VACUUM and ANALYZE.
 *		原地更新 pg_class 中 relpages/reltuples/可见冻结页数及冻结线等；非规范事务更新，
 *		避免 vacuum pg_class 时产生大量死元组；lazy VACUUM 且 PROC_IN_VACUUM 时不能普通过更新。
 *		外层事务中的 ANALYZE 不维护 relhasindex 等 DDL 缓存标志。VACUUM 与 ANALYZE 共用。
 */
void
vac_update_relstats(Relation relation,
					BlockNumber num_pages, double num_tuples,
					BlockNumber num_all_visible_pages,
					BlockNumber num_all_frozen_pages,
					bool hasindex, TransactionId frozenxid,
					MultiXactId minmulti,
					bool *frozenxid_updated, bool *minmulti_updated,
					bool in_outer_xact)
{
	Oid			relid = RelationGetRelid(relation);
	Relation	rd;
	ScanKeyData key[1];
	HeapTuple	ctup;
	void	   *inplace_state;
	Form_pg_class pgcform;
	bool		dirty,
				futurexid,
				futuremxid;
	TransactionId oldfrozenxid;
	MultiXactId oldminmulti;

	rd = table_open(RelationRelationId, RowExclusiveLock);

	/* Fetch a copy of the tuple to scribble on */
	/* 取 pg_class 行做原地修改 */
	ScanKeyInit(&key[0],
				Anum_pg_class_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	systable_inplace_update_begin(rd, ClassOidIndexId, true,
								  NULL, 1, key, &ctup, &inplace_state);
	if (!HeapTupleIsValid(ctup))
		elog(ERROR, "pg_class entry for relid %u vanished during vacuuming",
			 relid);
	pgcform = (Form_pg_class) GETSTRUCT(ctup);

	/* Apply statistical updates, if any, to copied tuple */
	/* 更新页数、元组估计、全可见/全冻结页数 */

	dirty = false;
	if (pgcform->relpages != (int32) num_pages)
	{
		pgcform->relpages = (int32) num_pages;
		dirty = true;
	}
	if (pgcform->reltuples != (float4) num_tuples)
	{
		pgcform->reltuples = (float4) num_tuples;
		dirty = true;
	}
	if (pgcform->relallvisible != (int32) num_all_visible_pages)
	{
		pgcform->relallvisible = (int32) num_all_visible_pages;
		dirty = true;
	}
	if (pgcform->relallfrozen != (int32) num_all_frozen_pages)
	{
		pgcform->relallfrozen = (int32) num_all_frozen_pages;
		dirty = true;
	}

	/* Apply DDL updates, but not inside an outer transaction (see above) */

	if (!in_outer_xact)
	{
		/*
		 * If we didn't find any indexes, reset relhasindex.
		 * 无索引则清 relhasindex 等延迟维护标志。
		 */
		if (pgcform->relhasindex && !hasindex)
		{
			pgcform->relhasindex = false;
			dirty = true;
		}

		/* We also clear relhasrules and relhastriggers if needed */
		/* 规则/触发器已无时清对应标志 */
		if (pgcform->relhasrules && relation->rd_rules == NULL)
		{
			pgcform->relhasrules = false;
			dirty = true;
		}
		if (pgcform->relhastriggers && relation->trigdesc == NULL)
		{
			pgcform->relhastriggers = false;
			dirty = true;
		}
	}

	/*
	 * Update relfrozenxid, unless caller passed InvalidTransactionId
	 * indicating it has no new data.
	 *
	 * Ordinarily, we don't let relfrozenxid go backwards.  However, if the
	 * stored relfrozenxid is "in the future" then it seems best to assume
	 * it's corrupt, and overwrite with the oldest remaining XID in the table.
	 * This should match vac_update_datfrozenxid() concerning what we consider
	 * to be "in the future".
	 * 推进 relfrozenxid 一般不后退；“未来”值视为损坏则覆盖。InvalidTransactionId 表示本趟无新冻结信息。
	 */
	oldfrozenxid = pgcform->relfrozenxid;
	futurexid = false;
	if (frozenxid_updated)
		*frozenxid_updated = false;
	if (TransactionIdIsNormal(frozenxid) && oldfrozenxid != frozenxid)
	{
		bool		update = false;

		if (TransactionIdPrecedes(oldfrozenxid, frozenxid))
			update = true;
		else if (TransactionIdPrecedes(ReadNextTransactionId(), oldfrozenxid))
			futurexid = update = true;

		if (update)
		{
			pgcform->relfrozenxid = frozenxid;
			dirty = true;
			if (frozenxid_updated)
				*frozenxid_updated = true;
		}
	}

	/* Similarly for relminmxid */
	/* relminmxid 同理 */
	oldminmulti = pgcform->relminmxid;
	futuremxid = false;
	if (minmulti_updated)
		*minmulti_updated = false;
	if (MultiXactIdIsValid(minmulti) && oldminmulti != minmulti)
	{
		bool		update = false;

		if (MultiXactIdPrecedes(oldminmulti, minmulti))
			update = true;
		else if (MultiXactIdPrecedes(ReadNextMultiXactId(), oldminmulti))
			futuremxid = update = true;

		if (update)
		{
			pgcform->relminmxid = minmulti;
			dirty = true;
			if (minmulti_updated)
				*minmulti_updated = true;
		}
	}

	/* If anything changed, write out the tuple. */
	/* 有改动则完成原地更新 */
	if (dirty)
		systable_inplace_update_finish(inplace_state, ctup);
	else
		systable_inplace_update_cancel(inplace_state);

	table_close(rd, RowExclusiveLock);

	if (futurexid)
		ereport(WARNING,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("overwrote invalid relfrozenxid value %u with new value %u for table \"%s\"",
								 oldfrozenxid, frozenxid,
								 RelationGetRelationName(relation))));
	if (futuremxid)
		ereport(WARNING,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("overwrote invalid relminmxid value %u with new value %u for table \"%s\"",
								 oldminmulti, minmulti,
								 RelationGetRelationName(relation))));
}


/*
 *	vac_update_datfrozenxid() -- update pg_database.datfrozenxid for our DB
 *
 *		Update pg_database's datfrozenxid entry for our database to be the
 *		minimum of the pg_class.relfrozenxid values.
 *
 *		Similarly, update our datminmxid to be the minimum of the
 *		pg_class.relminmxid values.
 *
 *		If we are able to advance either pg_database value, also try to
 *		truncate pg_xact and pg_multixact.
 *
 *		We violate transaction semantics here by overwriting the database's
 *		existing pg_database tuple with the new values.  This is reasonably
 *		safe since the new values are correct whether or not this transaction
 *		commits.  As with vac_update_relstats, this avoids leaving dead tuples
 *		behind after a VACUUM.
 *		扫描全库 relfrozenxid/relminmxid 取最小值更新本库 pg_database.datfrozenxid/datminmxid，
 *		并可能调用 vac_truncate_clog 截断 SLRU；原地更新语义同 vac_update_relstats。
 */
void
vac_update_datfrozenxid(void)
{
	HeapTuple	tuple;
	Form_pg_database dbform;
	Relation	relation;
	SysScanDesc scan;
	HeapTuple	classTup;
	TransactionId newFrozenXid;
	MultiXactId newMinMulti;
	TransactionId lastSaneFrozenXid;
	MultiXactId lastSaneMinMulti;
	bool		bogus = false;
	bool		dirty = false;
	ScanKeyData key[1];
	void	   *inplace_state;

	/*
	 * Restrict this task to one backend per database.  This avoids race
	 * conditions that would move datfrozenxid or datminmxid backward.  It
	 * avoids calling vac_truncate_clog() with a datfrozenxid preceding a
	 * datfrozenxid passed to an earlier vac_truncate_clog() call.
	 * 每库串行，防止冻结线回退或截断顺序错乱。
	 */
	LockDatabaseFrozenIds(ExclusiveLock);

	/*
	 * Initialize the "min" calculation with
	 * GetOldestNonRemovableTransactionId(), which is a reasonable
	 * approximation to the minimum relfrozenxid for not-yet-committed
	 * pg_class entries for new tables; see AddNewRelationTuple().  So we
	 * cannot produce a wrong minimum by starting with this.
	 * 最小 XID 初值用 OldestNonRemovable，覆盖尚未提交的建新表元组情况。
	 */
	newFrozenXid = GetOldestNonRemovableTransactionId(NULL);

	/*
	 * Similarly, initialize the MultiXact "min" with the value that would be
	 * used on pg_class for new tables.  See AddNewRelationTuple().
	 * MultiXact 最小值初始化同理。
	 */
	newMinMulti = GetOldestMultiXactId();

	/*
	 * Identify the latest relfrozenxid and relminmxid values that we could
	 * validly see during the scan.  These are conservative values, but it's
	 * not really worth trying to be more exact.
	 * 扫描期间可能出现的“最晚仍合理”XID/MXID，用于检测未来值。
	 */
	lastSaneFrozenXid = ReadNextTransactionId();
	lastSaneMinMulti = ReadNextMultiXactId();

	/*
	 * We must seqscan pg_class to find the minimum Xid, because there is no
	 * index that can help us here.
	 *
	 * See vac_truncate_clog() for the race condition to prevent.
	 * 须顺序扫 pg_class 求最小冻结线；与 vac_truncate_clog 配合避免竞态。
	 */
	relation = table_open(RelationRelationId, AccessShareLock);

	scan = systable_beginscan(relation, InvalidOid, false,
							  NULL, 0, NULL);

	while ((classTup = systable_getnext(scan)) != NULL)
	{
		volatile FormData_pg_class *classForm = (Form_pg_class) GETSTRUCT(classTup);
		TransactionId relfrozenxid = classForm->relfrozenxid;
		TransactionId relminmxid = classForm->relminmxid;

		/*
		 * Only consider relations able to hold unfrozen XIDs (anything else
		 * should have InvalidTransactionId in relfrozenxid anyway).
		 * 仅堆/物化视图/TOAST 等可存未冻结 XID 的关系。
		 */
		if (classForm->relkind != RELKIND_RELATION &&
			classForm->relkind != RELKIND_MATVIEW &&
			classForm->relkind != RELKIND_TOASTVALUE)
		{
			Assert(!TransactionIdIsValid(relfrozenxid));
			Assert(!MultiXactIdIsValid(relminmxid));
			continue;
		}

		/*
		 * Some table AMs might not need per-relation xid / multixid horizons.
		 * It therefore seems reasonable to allow relfrozenxid and relminmxid
		 * to not be set (i.e. set to their respective Invalid*Id)
		 * independently. Thus validate and compute horizon for each only if
		 * set.
		 *
		 * If things are working properly, no relation should have a
		 * relfrozenxid or relminmxid that is "in the future".  However, such
		 * cases have been known to arise due to bugs in pg_upgrade.  If we
		 * see any entries that are "in the future", chicken out and don't do
		 * anything.  This ensures we won't truncate clog & multixact SLRUs
		 * before those relations have been scanned and cleaned up.
		 * 独立校验 XID 与 MXID 视界；若见“未来”值则放弃更新以免误截断 SLRU。
		 */

		if (TransactionIdIsValid(relfrozenxid))
		{
			Assert(TransactionIdIsNormal(relfrozenxid));

			/* check for values in the future */
			/* 检测异常的未来 XID */
			if (TransactionIdPrecedes(lastSaneFrozenXid, relfrozenxid))
			{
				bogus = true;
				break;
			}

			/* determine new horizon */
			/* 维护最小 relfrozenxid */
			if (TransactionIdPrecedes(relfrozenxid, newFrozenXid))
				newFrozenXid = relfrozenxid;
		}

		if (MultiXactIdIsValid(relminmxid))
		{
			/* check for values in the future */
			if (MultiXactIdPrecedes(lastSaneMinMulti, relminmxid))
			{
				bogus = true;
				break;
			}

			/* determine new horizon */
			if (MultiXactIdPrecedes(relminmxid, newMinMulti))
				newMinMulti = relminmxid;
		}
	}

	/* we're done with pg_class */
	/* pg_class 扫描结束 */
	systable_endscan(scan);
	table_close(relation, AccessShareLock);

	/* chicken out if bogus data found */
	/* 发现异常数据则放弃 */
	if (bogus)
		return;

	Assert(TransactionIdIsNormal(newFrozenXid));
	Assert(MultiXactIdIsValid(newMinMulti));

	/* Now fetch the pg_database tuple we need to update. */
	/* 打开 pg_database 更新本库元组 */
	relation = table_open(DatabaseRelationId, RowExclusiveLock);

	/*
	 * Fetch a copy of the tuple to scribble on.  We could check the syscache
	 * tuple first.  If that concluded !dirty, we'd avoid waiting on
	 * concurrent heap_update() and would avoid exclusive-locking the buffer.
	 * For now, don't optimize that.
	 * 取元组原地修改；暂未做 syscache 短路优化。
	 */
	ScanKeyInit(&key[0],
				Anum_pg_database_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(MyDatabaseId));

	systable_inplace_update_begin(relation, DatabaseOidIndexId, true,
								  NULL, 1, key, &tuple, &inplace_state);

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "could not find tuple for database %u", MyDatabaseId);

	dbform = (Form_pg_database) GETSTRUCT(tuple);

	/*
	 * As in vac_update_relstats(), we ordinarily don't want to let
	 * datfrozenxid go backward; but if it's "in the future" then it must be
	 * corrupt and it seems best to overwrite it.
	 * 一般不后退 datfrozenxid；“未来”值视为损坏并覆盖。
	 */
	if (dbform->datfrozenxid != newFrozenXid &&
		(TransactionIdPrecedes(dbform->datfrozenxid, newFrozenXid) ||
		 TransactionIdPrecedes(lastSaneFrozenXid, dbform->datfrozenxid)))
	{
		dbform->datfrozenxid = newFrozenXid;
		dirty = true;
	}
	else
		newFrozenXid = dbform->datfrozenxid;

	/* Ditto for datminmxid */
	/* datminmxid 更新规则同 datfrozenxid */
	if (dbform->datminmxid != newMinMulti &&
		(MultiXactIdPrecedes(dbform->datminmxid, newMinMulti) ||
		 MultiXactIdPrecedes(lastSaneMinMulti, dbform->datminmxid)))
	{
		dbform->datminmxid = newMinMulti;
		dirty = true;
	}
	else
		newMinMulti = dbform->datminmxid;

	if (dirty)
		systable_inplace_update_finish(inplace_state, tuple);
	else
		systable_inplace_update_cancel(inplace_state);

	heap_freetuple(tuple);
	table_close(relation, RowExclusiveLock);

	/*
	 * If we were able to advance datfrozenxid or datminmxid, see if we can
	 * truncate pg_xact and/or pg_multixact.  Also do it if the shared
	 * XID-wrap-limit info is stale, since this action will update that too.
	 * 推进成功或全局 wrap 信息过期时尝试截断并更新限制。
	 */
	if (dirty || ForceTransactionIdLimitUpdate())
		vac_truncate_clog(newFrozenXid, newMinMulti,
						  lastSaneFrozenXid, lastSaneMinMulti);
}


/*
 *	vac_truncate_clog() -- attempt to truncate the commit log
 *
 *		Scan pg_database to determine the system-wide oldest datfrozenxid,
 *		and use it to truncate the transaction commit log (pg_xact).
 *		Also update the XID wrap limit info maintained by varsup.c.
 *		Likewise for datminmxid.
 *
 *		The passed frozenXID and minMulti are the updated values for my own
 *		pg_database entry. They're used to initialize the "min" calculations.
 *		The caller also passes the "last sane" XID and MXID, since it has
 *		those at hand already.
 *
 *		This routine is only invoked when we've managed to change our
 *		DB's datfrozenxid/datminmxid values, or we found that the shared
 *		XID-wrap-limit info is stale.
 *		集群级扫描 pg_database 求最老 datfrozenxid/datminmxid，截断 CLOG/MultiXact/CommitTs，
 *		并刷新 varsup 中的环绕限制；持 WrapLimitsVacuumLock。
 */
static void
vac_truncate_clog(TransactionId frozenXID,
				  MultiXactId minMulti,
				  TransactionId lastSaneFrozenXid,
				  MultiXactId lastSaneMinMulti)
{
	TransactionId nextXID = ReadNextTransactionId();
	Relation	relation;
	TableScanDesc scan;
	HeapTuple	tuple;
	Oid			oldestxid_datoid;
	Oid			minmulti_datoid;
	bool		bogus = false;
	bool		frozenAlreadyWrapped = false;

	/* Restrict task to one backend per cluster; see SimpleLruTruncate(). */
	/* 全集群串行截断，见 SimpleLruTruncate */
	LWLockAcquire(WrapLimitsVacuumLock, LW_EXCLUSIVE);

	/* init oldest datoids to sync with my frozenXID/minMulti values */
	/* 最小值初值用本库刚算出的冻结线 */
	oldestxid_datoid = MyDatabaseId;
	minmulti_datoid = MyDatabaseId;

	/*
	 * Scan pg_database to compute the minimum datfrozenxid/datminmxid
	 *
	 * Since vac_update_datfrozenxid updates datfrozenxid/datminmxid in-place,
	 * the values could change while we look at them.  Fetch each one just
	 * once to ensure sane behavior of the comparison logic.  (Here, as in
	 * many other places, we assume that fetching or updating an XID in shared
	 * storage is atomic.)
	 *
	 * Note: we need not worry about a race condition with new entries being
	 * inserted by CREATE DATABASE.  Any such entry will have a copy of some
	 * existing DB's datfrozenxid, and that source DB cannot be ours because
	 * of the interlock against copying a DB containing an active backend.
	 * Hence the new entry will not reduce the minimum.  Also, if two VACUUMs
	 * concurrently modify the datfrozenxid's of different databases, the
	 * worst possible outcome is that pg_xact is not truncated as aggressively
	 * as it could be.
	 * 扫 pg_database 求全局最小冻结线；每行只读一次 XID 原子性假设；CREATE DATABASE 竞态可接受。
	 */
	relation = table_open(DatabaseRelationId, AccessShareLock);

	scan = table_beginscan_catalog(relation, 0, NULL);

	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		volatile FormData_pg_database *dbform = (Form_pg_database) GETSTRUCT(tuple);
		TransactionId datfrozenxid = dbform->datfrozenxid;
		TransactionId datminmxid = dbform->datminmxid;

		Assert(TransactionIdIsNormal(datfrozenxid));
		Assert(MultiXactIdIsValid(datminmxid));

		/*
		 * If database is in the process of getting dropped, or has been
		 * interrupted while doing so, no connections to it are possible
		 * anymore. Therefore we don't need to take it into account here.
		 * Which is good, because it can't be processed by autovacuum either.
		 * 正在删除的无效库不参与最小冻结线计算。
		 */
		if (database_is_invalid_form((Form_pg_database) dbform))
		{
			elog(DEBUG2,
				 "skipping invalid database \"%s\" while computing relfrozenxid",
				 NameStr(dbform->datname));
			continue;
		}

		/*
		 * If things are working properly, no database should have a
		 * datfrozenxid or datminmxid that is "in the future".  However, such
		 * cases have been known to arise due to bugs in pg_upgrade.  If we
		 * see any entries that are "in the future", chicken out and don't do
		 * anything.  This ensures we won't truncate clog before those
		 * databases have been scanned and cleaned up.  (We will issue the
		 * "already wrapped" warning if appropriate, though.)
		 * 发现未来 datfrozenxid/datminmxid 则放弃截断（可能 pg_upgrade 残留）。
		 */
		if (TransactionIdPrecedes(lastSaneFrozenXid, datfrozenxid) ||
			MultiXactIdPrecedes(lastSaneMinMulti, datminmxid))
			bogus = true;

		if (TransactionIdPrecedes(nextXID, datfrozenxid))
			frozenAlreadyWrapped = true;
		else if (TransactionIdPrecedes(datfrozenxid, frozenXID))
		{
			frozenXID = datfrozenxid;
			oldestxid_datoid = dbform->oid;
		}

		if (MultiXactIdPrecedes(datminmxid, minMulti))
		{
			minMulti = datminmxid;
			minmulti_datoid = dbform->oid;
		}
	}

	table_endscan(scan);

	table_close(relation, AccessShareLock);

	/*
	 * Do not truncate CLOG if we seem to have suffered wraparound already;
	 * the computed minimum XID might be bogus.  This case should now be
	 * impossible due to the defenses in GetNewTransactionId, but we keep the
	 * test anyway.
	 * 若已发生环绕则最小 XID 不可信，不截断 CLOG。
	 */
	if (frozenAlreadyWrapped)
	{
		ereport(WARNING,
				(errmsg("some databases have not been vacuumed in over 2 billion transactions"),
				 errdetail("You might have already suffered transaction-wraparound data loss.")));
		LWLockRelease(WrapLimitsVacuumLock);
		return;
	}

	/* chicken out if data is bogus in any other way */
	/* 其它异常数据同样放弃 */
	if (bogus)
	{
		LWLockRelease(WrapLimitsVacuumLock);
		return;
	}

	/*
	 * Freeze any old transaction IDs in the async notification queue before
	 * CLOG truncation.
	 * 截断 CLOG 前冻结 LISTEN/NOTIFY 队列中的旧 XID。
	 */
	AsyncNotifyFreezeXids(frozenXID);

	/*
	 * Advance the oldest value for commit timestamps before truncating, so
	 * that if a user requests a timestamp for a transaction we're truncating
	 * away right after this point, they get NULL instead of an ugly "file not
	 * found" error from slru.c.  This doesn't matter for xact/multixact
	 * because they are not subject to arbitrary lookups from users.
	 * 先推进 commit_ts 最老 XID，避免截断后用户查时间戳得到 SLRU 错误。
	 */
	AdvanceOldestCommitTsXid(frozenXID);

	/*
	 * Truncate CLOG, multixact and CommitTs to the oldest computed value.
	 * 按全局最小冻结线截断 CLOG、CommitTs、MultiXact。
	 */
	TruncateCLOG(frozenXID, oldestxid_datoid);
	TruncateCommitTs(frozenXID);
	TruncateMultiXact(minMulti, minmulti_datoid);

	/*
	 * Update the wrap limit for GetNewTransactionId and creation of new
	 * MultiXactIds.  Note: these functions will also signal the postmaster
	 * for an(other) autovac cycle if needed.   XXX should we avoid possibly
	 * signaling twice?
	 * 更新全局事务/MultiXact 生成边界，必要时通知 postmaster 再跑 autovacuum。
	 */
	SetTransactionIdLimit(frozenXID, oldestxid_datoid);
	SetMultiXactIdLimit(minMulti, minmulti_datoid, false);

	LWLockRelease(WrapLimitsVacuumLock);
}


/*
 *	vacuum_rel() -- vacuum one heap relation
 *
 *		relid identifies the relation to vacuum.  If relation is supplied,
 *		use the name therein for reporting any failure to open/lock the rel;
 *		do not use it once we've successfully opened the rel, since it might
 *		be stale.
 *
 *		Returns true if it's okay to proceed with a requested ANALYZE
 *		operation on this table.
 *
 *		Doing one heap at a time incurs extra overhead, since we need to
 *		check that the heap exists again just before we vacuum it.  The
 *		reason that we do this is so that vacuuming can be spread across
 *		many small transactions.  Otherwise, two-phase locking would require
 *		us to lock the entire database during one pass of the vacuum cleaner.
 *
 *		At entry and exit, we are not inside a transaction.
 *		对单表执行 VACUUM（lazy 或 FULL）或仅打开校验；返回 true 表示可继续对本表 ANALYZE。
 *		每表独立事务以缩小持锁范围。调用前后均不在事务内。
 */
static bool
vacuum_rel(Oid relid, RangeVar *relation, VacuumParams *params,
		   BufferAccessStrategy bstrategy)
{
	LOCKMODE	lmode;
	Relation	rel;
	LockRelId	lockrelid;
	Oid			priv_relid;
	Oid			toast_relid;
	Oid			save_userid;
	int			save_sec_context;
	int			save_nestlevel;
	VacuumParams toast_vacuum_params;

	Assert(params != NULL);

	/*
	 * This function scribbles on the parameters, so make a copy early to
	 * avoid affecting the TOAST table (if we do end up recursing to it).
	 * 本函数会改写 params，提前复制一份供递归 TOAST 使用。
	 */
	memcpy(&toast_vacuum_params, params, sizeof(VacuumParams));

	/* Begin a transaction for vacuuming this relation */
	/* 本表 vacuum 使用独立事务开始 */
	StartTransactionCommand();

	if (!(params->options & VACOPT_FULL))
	{
		/*
		 * In lazy vacuum, we can set the PROC_IN_VACUUM flag, which lets
		 * other concurrent VACUUMs know that they can ignore this one while
		 * determining their OldestXmin.  (The reason we don't set it during a
		 * full VACUUM is exactly that we may have to run user-defined
		 * functions for functional indexes, and we want to make sure that if
		 * they use the snapshot set above, any tuples it requires can't get
		 * removed from other tables.  An index function that depends on the
		 * contents of other tables is arguably broken, but we won't break it
		 * here by violating transaction semantics.)
		 *
		 * We also set the VACUUM_FOR_WRAPAROUND flag, which is passed down by
		 * autovacuum; it's used to avoid canceling a vacuum that was invoked
		 * in an emergency.
		 *
		 * Note: these flags remain set until CommitTransaction or
		 * AbortTransaction.  We don't want to clear them until we reset
		 * MyProc->xid/xmin, otherwise GetOldestNonRemovableTransactionId()
		 * might appear to go backwards, which is probably Not Good.  (We also
		 * set PROC_IN_VACUUM *before* taking our own snapshot, so that our
		 * xmin doesn't become visible ahead of setting the flag.)
		 * lazy 设 PROC_IN_VACUUM 供其它 VACUUM 计算 OldestXmin 时忽略本进程；FULL 因可能
		 * 执行索引表达式不设。WRAPAROUND 标志防紧急 vacuum 被取消。须在拿快照前设标志。
		 */
		LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
		MyProc->statusFlags |= PROC_IN_VACUUM;
		if (params->is_wraparound)
			MyProc->statusFlags |= PROC_VACUUM_FOR_WRAPAROUND;
		ProcGlobal->statusFlags[MyProc->pgxactoff] = MyProc->statusFlags;
		LWLockRelease(ProcArrayLock);
	}

	/*
	 * Need to acquire a snapshot to prevent pg_subtrans from being truncated,
	 * cutoff xids in local memory wrapping around, and to have updated xmin
	 * horizons.
	 * 需要快照以防 pg_subtrans 截断与本地 xmin 视界异常。
	 */
	PushActiveSnapshot(GetTransactionSnapshot());

	/*
	 * Check for user-requested abort.  Note we want this to be inside a
	 * transaction, so xact.c doesn't issue useless WARNING.
	 * 在事务内检查取消，避免 xact.c 多余 WARNING。
	 */
	CHECK_FOR_INTERRUPTS();

	/*
	 * Determine the type of lock we want --- hard exclusive lock for a FULL
	 * vacuum, but just ShareUpdateExclusiveLock for concurrent vacuum. Either
	 * way, we can be sure that no other backend is vacuuming the same table.
	 * FULL 用 AccessExclusiveLock；lazy 用 ShareUpdateExclusiveLock，可与读写并发（除 DDL）。
	 */
	lmode = (params->options & VACOPT_FULL) ?
		AccessExclusiveLock : ShareUpdateExclusiveLock;

	/* open the relation and get the appropriate lock on it */
	/* 打开关系并加锁 */
	rel = vacuum_open_relation(relid, relation, params->options,
							   params->log_min_duration >= 0, lmode);

	/* leave if relation could not be opened or locked */
	/* 打不开则结束事务并返回 false */
	if (!rel)
	{
		PopActiveSnapshot();
		CommitTransactionCommand();
		return false;
	}

	/*
	 * When recursing to a TOAST table, check privileges on the parent.  NB:
	 * This is only safe to do because we hold a session lock on the main
	 * relation that prevents concurrent deletion.
	 * TOAST 递归时用父表 OID 做权限检查；依赖主表会话锁防并发删表。
	 */
	if (OidIsValid(params->toast_parent))
		priv_relid = params->toast_parent;
	else
		priv_relid = RelationGetRelid(rel);

	/*
	 * Check if relation needs to be skipped based on privileges.  This check
	 * happens also when building the relation list to vacuum for a manual
	 * operation, and needs to be done additionally here as VACUUM could
	 * happen across multiple transactions where privileges could have changed
	 * in-between.  Make sure to only generate logs for VACUUM in this case.
	 * 跨事务 vacuum 时权限可能变化，此处再次检查；日志仅针对 VACUUM 侧选项。
	 */
	if (!vacuum_is_permitted_for_relation(priv_relid,
										  rel->rd_rel,
										  params->options & ~VACOPT_ANALYZE))
	{
		relation_close(rel, lmode);
		PopActiveSnapshot();
		CommitTransactionCommand();
		return false;
	}

	/*
	 * Check that it's of a vacuumable relkind.
	 * 仅普通表、物化视图、TOAST、分区父表可进入后续逻辑（父表稍后单独跳过）。
	 */
	if (rel->rd_rel->relkind != RELKIND_RELATION &&
		rel->rd_rel->relkind != RELKIND_MATVIEW &&
		rel->rd_rel->relkind != RELKIND_TOASTVALUE &&
		rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
	{
		ereport(WARNING,
				(errmsg("skipping \"%s\" --- cannot vacuum non-tables or special system tables",
						RelationGetRelationName(rel))));
		relation_close(rel, lmode);
		PopActiveSnapshot();
		CommitTransactionCommand();
		return false;
	}

	/*
	 * Silently ignore tables that are temp tables of other backends ---
	 * trying to vacuum these will lead to great unhappiness, since their
	 * contents are probably not up-to-date on disk.  (We don't throw a
	 * warning here; it would just lead to chatter during a database-wide
	 * VACUUM.)
	 * 静默跳过其它后端的临时表；全库 VACUUM 不打 WARNING 避免刷屏。
	 */
	if (RELATION_IS_OTHER_TEMP(rel))
	{
		relation_close(rel, lmode);
		PopActiveSnapshot();
		CommitTransactionCommand();
		return false;
	}

	/*
	 * Silently ignore partitioned tables as there is no work to be done.  The
	 * useful work is on their child partitions, which have been queued up for
	 * us separately.
	 * 分区父表无堆数据，静默跳过；实际工作在子分区上。
	 */
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		relation_close(rel, lmode);
		PopActiveSnapshot();
		CommitTransactionCommand();
		/* It's OK to proceed with ANALYZE on this table */
		/* 父表仍可 ANALYZE（统计分区合并等由 analyze 路径处理） */
		return true;
	}

	/*
	 * Get a session-level lock too. This will protect our access to the
	 * relation across multiple transactions, so that we can vacuum the
	 * relation's TOAST table (if any) secure in the knowledge that no one is
	 * deleting the parent relation.
	 *
	 * NOTE: this cannot block, even if someone else is waiting for access,
	 * because the lock manager knows that both lock requests are from the
	 * same process.
	 * 会话级锁跨事务持有，保证递归 vacuum TOAST 时父表不被删；同进程不自我阻塞。
	 */
	lockrelid = rel->rd_lockInfo.lockRelId;
	LockRelationIdForSession(&lockrelid, lmode);

	/*
	 * Set index_cleanup option based on index_cleanup reloption if it wasn't
	 * specified in VACUUM command, or when running in an autovacuum worker
	 * 未在命令指定时从表级 reloption 取 index_cleanup（auto/on/off）。
	 */
	if (params->index_cleanup == VACOPTVALUE_UNSPECIFIED)
	{
		StdRdOptIndexCleanup vacuum_index_cleanup;

		if (rel->rd_options == NULL)
			vacuum_index_cleanup = STDRD_OPTION_VACUUM_INDEX_CLEANUP_AUTO;
		else
			vacuum_index_cleanup =
				((StdRdOptions *) rel->rd_options)->vacuum_index_cleanup;

		if (vacuum_index_cleanup == STDRD_OPTION_VACUUM_INDEX_CLEANUP_AUTO)
			params->index_cleanup = VACOPTVALUE_AUTO;
		else if (vacuum_index_cleanup == STDRD_OPTION_VACUUM_INDEX_CLEANUP_ON)
			params->index_cleanup = VACOPTVALUE_ENABLED;
		else
		{
			Assert(vacuum_index_cleanup ==
				   STDRD_OPTION_VACUUM_INDEX_CLEANUP_OFF);
			params->index_cleanup = VACOPTVALUE_DISABLED;
		}
	}

#ifdef USE_INJECTION_POINTS
	if (params->index_cleanup == VACOPTVALUE_AUTO)
		INJECTION_POINT("vacuum-index-cleanup-auto", NULL);
	else if (params->index_cleanup == VACOPTVALUE_DISABLED)
		INJECTION_POINT("vacuum-index-cleanup-disabled", NULL);
	else if (params->index_cleanup == VACOPTVALUE_ENABLED)
		INJECTION_POINT("vacuum-index-cleanup-enabled", NULL);
#endif

	/*
	 * Check if the vacuum_max_eager_freeze_failure_rate table storage
	 * parameter was specified. This overrides the GUC value.
	 * 表级 storage 参数可覆盖 eager 冻结失败率 GUC。
	 */
	if (rel->rd_options != NULL &&
		((StdRdOptions *) rel->rd_options)->vacuum_max_eager_freeze_failure_rate >= 0)
		params->max_eager_freeze_failure_rate =
			((StdRdOptions *) rel->rd_options)->vacuum_max_eager_freeze_failure_rate;

	/*
	 * Set truncate option based on truncate reloption or GUC if it wasn't
	 * specified in VACUUM command, or when running in an autovacuum worker
	 * truncate 未指定时来自 reloption 或 vacuum_truncate GUC。
	 */
	if (params->truncate == VACOPTVALUE_UNSPECIFIED)
	{
		StdRdOptions *opts = (StdRdOptions *) rel->rd_options;

		if (opts && opts->vacuum_truncate_set)
		{
			if (opts->vacuum_truncate)
				params->truncate = VACOPTVALUE_ENABLED;
			else
				params->truncate = VACOPTVALUE_DISABLED;
		}
		else if (vacuum_truncate)
			params->truncate = VACOPTVALUE_ENABLED;
		else
			params->truncate = VACOPTVALUE_DISABLED;
	}

#ifdef USE_INJECTION_POINTS
	if (params->truncate == VACOPTVALUE_AUTO)
		INJECTION_POINT("vacuum-truncate-auto", NULL);
	else if (params->truncate == VACOPTVALUE_DISABLED)
		INJECTION_POINT("vacuum-truncate-disabled", NULL);
	else if (params->truncate == VACOPTVALUE_ENABLED)
		INJECTION_POINT("vacuum-truncate-enabled", NULL);
#endif

	/*
	 * Remember the relation's TOAST relation for later, if the caller asked
	 * us to process it.  In VACUUM FULL, though, the toast table is
	 * automatically rebuilt by cluster_rel so we shouldn't recurse to it,
	 * unless PROCESS_MAIN is disabled.
	 * 记录 TOAST OID 供主表事务提交后递归；FULL 且处理主表时 cluster 已重建 TOAST，一般不递归。
	 */
	if ((params->options & VACOPT_PROCESS_TOAST) != 0 &&
		((params->options & VACOPT_FULL) == 0 ||
		 (params->options & VACOPT_PROCESS_MAIN) == 0))
		toast_relid = rel->rd_rel->reltoastrelid;
	else
		toast_relid = InvalidOid;

	/*
	 * Switch to the table owner's userid, so that any index functions are run
	 * as that user.  Also lock down security-restricted operations and
	 * arrange to make GUC variable changes local to this command. (This is
	 * unnecessary, but harmless, for lazy VACUUM.)
	 * 切换为表所有者执行索引表达式；限制 search_path 等；GUC 嵌套级便于回滚。
	 */
	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(rel->rd_rel->relowner,
						   save_sec_context | SECURITY_RESTRICTED_OPERATION);
	save_nestlevel = NewGUCNestLevel();
	RestrictSearchPath();

	/*
	 * If PROCESS_MAIN is set (the default), it's time to vacuum the main
	 * relation.  Otherwise, we can skip this part.  If processing the TOAST
	 * table is required (e.g., PROCESS_TOAST is set), we force PROCESS_MAIN
	 * to be set when we recurse to the TOAST table.
	 * PROCESS_MAIN 时对主表执行 FULL→cluster_rel 或 lazy→table_relation_vacuum。
	 */
	if (params->options & VACOPT_PROCESS_MAIN)
	{
		/*
		 * Do the actual work --- either FULL or "lazy" vacuum
		 * 实际清理：FULL 走 cluster.c；否则表 AM 的 vacuum（堆为 lazy vacuum）。
		 */
		if (params->options & VACOPT_FULL)
		{
			ClusterParams cluster_params = {0};

			if ((params->options & VACOPT_VERBOSE) != 0)
				cluster_params.options |= CLUOPT_VERBOSE;

			/* VACUUM FULL is now a variant of CLUSTER; see cluster.c */
			/* VACUUM FULL 即 CLUSTER 变体 */
			cluster_rel(rel, InvalidOid, &cluster_params);
			/* cluster_rel closes the relation, but keeps lock */

			rel = NULL;
		}
		else
			table_relation_vacuum(rel, params, bstrategy);
	}

	/* Roll back any GUC changes executed by index functions */
	/* 回滚索引表达式内对 GUC 的修改 */
	AtEOXact_GUC(false, save_nestlevel);

	/* Restore userid and security context */
	/* 恢复调用者身份与安全上下文 */
	SetUserIdAndSecContext(save_userid, save_sec_context);

	/* all done with this class, but hold lock until commit */
	/* 关闭 rel 但锁保留到 Commit */
	if (rel)
		relation_close(rel, NoLock);

	/*
	 * Complete the transaction and free all temporary memory used.
	 * 提交本表事务，释放临时内存。
	 */
	PopActiveSnapshot();
	CommitTransactionCommand();

	/*
	 * If the relation has a secondary toast rel, vacuum that too while we
	 * still hold the session lock on the main table.  Note however that
	 * "analyze" will not get done on the toast table.  This is good, because
	 * the toaster always uses hardcoded index access and statistics are
	 * totally unimportant for toast relations.
	 * 在仍持主表会话锁时递归 vacuum TOAST；不对 TOAST 做 ANALYZE。
	 */
	if (toast_relid != InvalidOid)
	{
		/*
		 * Force VACOPT_PROCESS_MAIN so vacuum_rel() processes it.  Likewise,
		 * set toast_parent so that the privilege checks are done on the main
		 * relation.  NB: This is only safe to do because we hold a session
		 * lock on the main relation that prevents concurrent deletion.
		 * 强制 PROCESS_MAIN 并设 toast_parent 做权限检查；依赖主表会话锁。
		 */
		toast_vacuum_params.options |= VACOPT_PROCESS_MAIN;
		toast_vacuum_params.toast_parent = relid;

		vacuum_rel(toast_relid, NULL, &toast_vacuum_params, bstrategy);
	}

	/*
	 * Now release the session-level lock on the main table.
	 * TOAST 处理完后释放主表会话锁。
	 */
	UnlockRelationIdForSession(&lockrelid, lmode);

	/* Report that we really did it. */
	/* 成功完成本表（及 TOAST）处理 */
	return true;
}


/*
 * Open all the vacuumable indexes of the given relation, obtaining the
 * specified kind of lock on each.  Return an array of Relation pointers for
 * the indexes into *Irel, and the number of indexes into *nindexes.
 *
 * We consider an index vacuumable if it is marked insertable (indisready).
 * If it isn't, probably a CREATE INDEX CONCURRENTLY command failed early in
 * execution, and what we have is too corrupt to be processable.  We will
 * vacuum even if the index isn't indisvalid; this is important because in a
 * unique index, uniqueness checks will be performed anyway and had better not
 * hit dangling index pointers.
 * 打开关系上所有 indisready 的索引并加锁；结果放入 *Irel，个数 *nindexes。
 * 未 ready 的索引（并发建索引失败）跳过；invalid 仍可能 vacuum 以清理悬挂指针。
 */
void
vac_open_indexes(Relation relation, LOCKMODE lockmode,
				 int *nindexes, Relation **Irel)
{
	List	   *indexoidlist;
	ListCell   *indexoidscan;
	int			i;

	Assert(lockmode != NoLock);

	indexoidlist = RelationGetIndexList(relation);

	/* allocate enough memory for all indexes */
	/* 按索引个数分配 Relation 数组 */
	i = list_length(indexoidlist);

	if (i > 0)
		*Irel = (Relation *) palloc(i * sizeof(Relation));
	else
		*Irel = NULL;

	/* collect just the ready indexes */
	/* 只收集 indisready 的索引 */
	i = 0;
	foreach(indexoidscan, indexoidlist)
	{
		Oid			indexoid = lfirst_oid(indexoidscan);
		Relation	indrel;

		indrel = index_open(indexoid, lockmode);
		if (indrel->rd_index->indisready)
			(*Irel)[i++] = indrel;
		else
			index_close(indrel, lockmode);
	}

	*nindexes = i;

	list_free(indexoidlist);
}

/*
 * Release the resources acquired by vac_open_indexes.  Optionally release
 * the locks (say NoLock to keep 'em).
 * 释放 vac_open_indexes 打开的索引；lockmode 可为 NoLock 保留锁。
 */
void
vac_close_indexes(int nindexes, Relation *Irel, LOCKMODE lockmode)
{
	if (Irel == NULL)
		return;

	while (nindexes--)
	{
		Relation	ind = Irel[nindexes];

		index_close(ind, lockmode);
	}
	pfree(Irel);
}

/*
 * vacuum_delay_point --- check for interrupts and cost-based delay.
 *
 * This should be called in each major loop of VACUUM processing,
 * typically once per page processed.
 * VACUUM/ANALYZE 主循环中调用：检查中断并按 vacuum_cost_* 或并行代价休眠节流。
 */
void
vacuum_delay_point(bool is_analyze)
{
	double		msec = 0;

	/* Always check for interrupts */
	/* 始终检查取消请求 */
	CHECK_FOR_INTERRUPTS();

	if (InterruptPending ||
		(!VacuumCostActive && !ConfigReloadPending))
		return;

	/*
	 * Autovacuum workers should reload the configuration file if requested.
	 * This allows changes to [autovacuum_]vacuum_cost_limit and
	 * [autovacuum_]vacuum_cost_delay to take effect while a table is being
	 * vacuumed or analyzed.
	 * autovacuum worker 在 SIGHUP 时重载配置使代价参数即时生效。
	 */
	if (ConfigReloadPending && AmAutoVacuumWorkerProcess())
	{
		ConfigReloadPending = false;
		ProcessConfigFile(PGC_SIGHUP);
		VacuumUpdateCosts();
	}

	/*
	 * If we disabled cost-based delays after reloading the config file,
	 * return.
	 * 重载后若关闭代价延迟则返回。
	 */
	if (!VacuumCostActive)
		return;

	/*
	 * For parallel vacuum, the delay is computed based on the shared cost
	 * balance.  See compute_parallel_delay.
	 * 并行 vacuum 的休眠时间由共享代价余额决定。
	 */
	if (VacuumSharedCostBalance != NULL)
		msec = compute_parallel_delay();
	else if (VacuumCostBalance >= vacuum_cost_limit)
		msec = vacuum_cost_delay * VacuumCostBalance / vacuum_cost_limit;

	/* Nap if appropriate */
	/* 需要则睡眠 */
	if (msec > 0)
	{
		instr_time	delay_start;

		if (msec > vacuum_cost_delay * 4)
			msec = vacuum_cost_delay * 4;

		if (track_cost_delay_timing)
			INSTR_TIME_SET_CURRENT(delay_start);

		pgstat_report_wait_start(WAIT_EVENT_VACUUM_DELAY);
		pg_usleep(msec * 1000);
		pgstat_report_wait_end();

		if (track_cost_delay_timing)
		{
			instr_time	delay_end;
			instr_time	delay;

			INSTR_TIME_SET_CURRENT(delay_end);
			INSTR_TIME_SET_ZERO(delay);
			INSTR_TIME_ACCUM_DIFF(delay, delay_end, delay_start);

			/*
			 * For parallel workers, we only report the delay time every once
			 * in a while to avoid overloading the leader with messages and
			 * interrupts.
			 * 并行 worker 节流上报延迟统计，减轻 leader 负担。
			 */
			if (IsParallelWorker())
			{
				static instr_time last_report_time;
				instr_time	time_since_last_report;

				Assert(!is_analyze);

				/* Accumulate the delay time */
				/* 累积本 worker 延迟时间 */
				parallel_vacuum_worker_delay_ns += INSTR_TIME_GET_NANOSEC(delay);

				/* Calculate interval since last report */
				/* 距上次上报间隔 */
				INSTR_TIME_SET_ZERO(time_since_last_report);
				INSTR_TIME_ACCUM_DIFF(time_since_last_report, delay_end, last_report_time);

				/* If we haven't reported in a while, do so now */
				/* 超过间隔则上报进度并清零累积 */
				if (INSTR_TIME_GET_NANOSEC(time_since_last_report) >=
					PARALLEL_VACUUM_DELAY_REPORT_INTERVAL_NS)
				{
					pgstat_progress_parallel_incr_param(PROGRESS_VACUUM_DELAY_TIME,
														parallel_vacuum_worker_delay_ns);

					/* Reset variables */
					/* 重置上报基准与累积 */
					last_report_time = delay_end;
					parallel_vacuum_worker_delay_ns = 0;
				}
			}
			else if (is_analyze)
				pgstat_progress_incr_param(PROGRESS_ANALYZE_DELAY_TIME,
										   INSTR_TIME_GET_NANOSEC(delay));
			else
				pgstat_progress_incr_param(PROGRESS_VACUUM_DELAY_TIME,
										   INSTR_TIME_GET_NANOSEC(delay));
		}

		/*
		 * We don't want to ignore postmaster death during very long vacuums
		 * with vacuum_cost_delay configured.  We can't use the usual
		 * WaitLatch() approach here because we want microsecond-based sleep
		 * durations above.
		 * 长 sleep 期间仍检测 postmaster 是否存活（无法用 WaitLatch 微秒睡眠）。
		 */
		if (IsUnderPostmaster && !PostmasterIsAlive())
			exit(1);

		VacuumCostBalance = 0;

		/*
		 * Balance and update limit values for autovacuum workers. We must do
		 * this periodically, as the number of workers across which we are
		 * balancing the limit may have changed.
		 *
		 * TODO: There may be better criteria for determining when to do this
		 * besides "check after napping".
		 * 每次睡醒后可能重算 autovacuum worker 间代价限额分配。
		 */
		AutoVacuumUpdateCostLimit();

		/* Might have gotten an interrupt while sleeping */
		/* 睡眠中可能收到中断 */
		CHECK_FOR_INTERRUPTS();
	}
}

/*
 * Computes the vacuum delay for parallel workers.
 *
 * The basic idea of a cost-based delay for parallel vacuum is to allow each
 * worker to sleep in proportion to the share of work it's done.  We achieve this
 * by allowing all parallel vacuum workers including the leader process to
 * have a shared view of cost related parameters (mainly VacuumCostBalance).
 * We allow each worker to update it as and when it has incurred any cost and
 * then based on that decide whether it needs to sleep.  We compute the time
 * to sleep for a worker based on the cost it has incurred
 * (VacuumCostBalanceLocal) and then reduce the VacuumSharedCostBalance by
 * that amount.  This avoids putting to sleep those workers which have done less
 * I/O than other workers and therefore ensure that workers
 * which are doing more I/O got throttled more.
 *
 * We allow a worker to sleep only if it has performed I/O above a certain
 * threshold, which is calculated based on the number of active workers
 * (VacuumActiveNWorkers), and the overall cost balance is more than
 * VacuumCostLimit set by the system.  Testing reveals that we achieve
 * the required throttling if we force a worker that has done more than 50%
 * of its share of work to sleep.
 * 并行 vacuum 的共享代价余额：本地累积代价，全局超限时让“干活多”的 worker 多睡；
 * 从共享余额扣减本地份额，避免慢 worker 被不必要休眠。
 */
static double
compute_parallel_delay(void)
{
	double		msec = 0;
	uint32		shared_balance;
	int			nworkers;

	/* Parallel vacuum must be active */
	/* 仅在并行 vacuum 激活时调用 */
	Assert(VacuumSharedCostBalance);

	nworkers = pg_atomic_read_u32(VacuumActiveNWorkers);

	/* At least count itself */
	/* 至少包含当前 worker */
	Assert(nworkers >= 1);

	/* Update the shared cost balance value atomically */
	/* 原子累加本周期代价到共享余额 */
	shared_balance = pg_atomic_add_fetch_u32(VacuumSharedCostBalance, VacuumCostBalance);

	/* Compute the total local balance for the current worker */
	/* 累加本 worker 本地代价 */
	VacuumCostBalanceLocal += VacuumCostBalance;

	if ((shared_balance >= vacuum_cost_limit) &&
		(VacuumCostBalanceLocal > 0.5 * ((double) vacuum_cost_limit / nworkers)))
	{
		/* Compute sleep time based on the local cost balance */
		/* 按本地代价占比计算睡眠时间 */
		msec = vacuum_cost_delay * VacuumCostBalanceLocal / vacuum_cost_limit;
		pg_atomic_sub_fetch_u32(VacuumSharedCostBalance, VacuumCostBalanceLocal);
		VacuumCostBalanceLocal = 0;
	}

	/*
	 * Reset the local balance as we accumulated it into the shared value.
	 * 本周期代价已并入共享或用于睡眠计算，清零 VacuumCostBalance。
	 */
	VacuumCostBalance = 0;

	return msec;
}

/*
 * A wrapper function of defGetBoolean().
 *
 * This function returns VACOPTVALUE_ENABLED and VACOPTVALUE_DISABLED instead
 * of true and false.
 * defGetBoolean 的包装，返回 VACOPTVALUE_ENABLED/DISABLED。
 */
static VacOptValue
get_vacoptval_from_boolean(DefElem *def)
{
	return defGetBoolean(def) ? VACOPTVALUE_ENABLED : VACOPTVALUE_DISABLED;
}

/*
 *	vac_bulkdel_one_index() -- bulk-deletion for index relation.
 *
 * Returns bulk delete stats derived from input stats
 * 对单索引执行 index_bulk_delete（批量删死元组 TID），返回更新后的统计。
 */
IndexBulkDeleteResult *
vac_bulkdel_one_index(IndexVacuumInfo *ivinfo, IndexBulkDeleteResult *istat,
					  TidStore *dead_items, VacDeadItemsInfo *dead_items_info)
{
	/* Do bulk deletion */
	/* 调用访问方法批量删除 */
	istat = index_bulk_delete(ivinfo, istat, vac_tid_reaped,
							  dead_items);

	ereport(ivinfo->message_level,
			(errmsg("scanned index \"%s\" to remove %" PRId64 " row versions",
					RelationGetRelationName(ivinfo->index),
					dead_items_info->num_items)));

	return istat;
}

/*
 *	vac_cleanup_one_index() -- do post-vacuum cleanup for index relation.
 *
 * Returns bulk delete stats derived from input stats
 * 索引 vacuum 收尾：index_vacuum_cleanup（如回收空页、更新统计）。
 */
IndexBulkDeleteResult *
vac_cleanup_one_index(IndexVacuumInfo *ivinfo, IndexBulkDeleteResult *istat)
{
	istat = index_vacuum_cleanup(ivinfo, istat);

	if (istat)
		ereport(ivinfo->message_level,
				(errmsg("index \"%s\" now contains %.0f row versions in %u pages",
						RelationGetRelationName(ivinfo->index),
						istat->num_index_tuples,
						istat->num_pages),
				 errdetail("%.0f index row versions were removed.\n"
						   "%u index pages were newly deleted.\n"
						   "%u index pages are currently deleted, of which %u are currently reusable.",
						   istat->tuples_removed,
						   istat->pages_newly_deleted,
						   istat->pages_deleted, istat->pages_free)));

	return istat;
}

/*
 *	vac_tid_reaped() -- is a particular tid deletable?
 *
 *		This has the right signature to be an IndexBulkDeleteCallback.
 *		IndexBulkDeleteCallback：判断 TID 是否在 dead_items（TidStore）中。
 */
static bool
vac_tid_reaped(ItemPointer itemptr, void *state)
{
	TidStore   *dead_items = (TidStore *) state;

	return TidStoreIsMember(dead_items, itemptr);
}

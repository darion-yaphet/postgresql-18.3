/*-------------------------------------------------------------------------
 *
 * pg_stat_statements.c
 *		Track statement planning and execution times as well as resource
 *		usage across a whole database cluster.
 *
 * Execution costs are totaled for each distinct source query, and kept in
 * a shared hashtable.  (We track only as many distinct queries as will fit
 * in the designated amount of shared memory.)
 *
 * Starting in Postgres 9.2, this module normalized query entries.  As of
 * Postgres 14, the normalization is done by the core if compute_query_id is
 * enabled, or optionally by third-party modules.
 *
 * To facilitate presenting entries to users, we create "representative" query
 * strings in which constants are replaced with parameter symbols ($n), to
 * make it clearer what a normalized entry can represent.  To save on shared
 * memory, and to avoid having to truncate oversized query strings, we store
 * these strings in a temporary external query-texts file.  Offsets into this
 * file are kept in shared memory.
 *
 * Note about locking issues: to create or delete an entry in the shared
 * hashtable, one must hold pgss->lock exclusively.  Modifying any field
 * in an entry except the counters requires the same.  To look up an entry,
 * one must hold the lock shared.  To read or update the counters within
 * an entry, one must hold the lock shared or exclusive (so the entry doesn't
 * disappear!) and also take the entry's mutex spinlock.
 * The shared state variable pgss->extent (the next free spot in the external
 * query-text file) should be accessed only while holding either the
 * pgss->mutex spinlock, or exclusive lock on pgss->lock.  We use the mutex to
 * allow reserving file space while holding only shared lock on pgss->lock.
 * Rewriting the entire external query-text file, eg for garbage collection,
 * requires holding pgss->lock exclusively; this allows individual entries
 * in the file to be read or written while holding only shared lock.
 *
 *
 * Copyright (c) 2008-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/pg_stat_statements/pg_stat_statements.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/parallel.h"
#include "catalog/pg_authid.h"
#include "common/int.h"
#include "executor/instrument.h"
#include "funcapi.h"
#include "jit/jit.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/queryjumble.h"
#include "optimizer/planner.h"
#include "parser/analyze.h"
#include "parser/scanner.h"
#include "pgstat.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

PG_MODULE_MAGIC_EXT(
					.name = "pg_stat_statements",
					.version = PG_VERSION
);

/* Location of permanent stats file (valid when database is shut down)
 *
 * 永久统计文件的位置（数据库关闭时有效）
 */
#define PGSS_DUMP_FILE	PGSTAT_STAT_PERMANENT_DIRECTORY "/pg_stat_statements.stat"

/*
 * Location of external query text file.
 *
 * 外部查询文本文件的位置。
 */
#define PGSS_TEXT_FILE	PG_STAT_TMP_DIR "/pgss_query_texts.stat"

/* Magic number identifying the stats file format
 *
 * 标识统计文件格式的幻数
 */
static const uint32 PGSS_FILE_HEADER = 0x20220408;

/* PostgreSQL major version number, changes in which invalidate all entries
 *
 * PostgreSQL 主要版本号，其中所有条目无效的更改
 */
static const uint32 PGSS_PG_MAJOR_VERSION = PG_VERSION_NUM / 100;

/* XXX: Should USAGE_EXEC reflect execution time and/or buffer usage?
 *
 * XXX：USAGE_EXEC 是否应该反映执行时间和/或缓冲区使用情况？
 */
#define USAGE_EXEC(duration)	(1.0)
#define USAGE_INIT				(1.0)	/* including initial planning */
#define ASSUMED_MEDIAN_INIT		(10.0)	/* initial assumed median usage */
#define ASSUMED_LENGTH_INIT		1024	/* initial assumed mean query length */
#define USAGE_DECREASE_FACTOR	(0.99)	/* decreased every entry_dealloc */
#define STICKY_DECREASE_FACTOR	(0.50)	/* factor for sticky entries */
#define USAGE_DEALLOC_PERCENT	5	/* free this % of entries at once */
#define IS_STICKY(c)	((c.calls[PGSS_PLAN] + c.calls[PGSS_EXEC]) == 0)

/*
 * Extension version number, for supporting older extension versions' objects
 *
 * 扩展版本号，用于支持旧扩展版本的对象
 */
typedef enum pgssVersion
{
	PGSS_V1_0 = 0,
	PGSS_V1_1,
	PGSS_V1_2,
	PGSS_V1_3,
	PGSS_V1_8,
	PGSS_V1_9,
	PGSS_V1_10,
	PGSS_V1_11,
	PGSS_V1_12,
} pgssVersion;

typedef enum pgssStoreKind
{
	PGSS_INVALID = -1,

	/*
	 * PGSS_PLAN and PGSS_EXEC must be respectively 0 and 1 as they're used to
	 * reference the underlying values in the arrays in the Counters struct,
	 * and this order is required in pg_stat_statements_internal().
	 *
	 * PGSS_PLAN 和 PGSS_EXEC 必须分别为 0 和 1，因为它们用于引用 Counters 结构中数组中的基础值，并且 pg_stat_statements_internal() 中需要此顺序。
	 */
	PGSS_PLAN = 0,
	PGSS_EXEC,
} pgssStoreKind;

#define PGSS_NUMKIND (PGSS_EXEC + 1)

/*
 * Hashtable key that defines the identity of a hashtable entry.  We separate
 * queries by user and by database even if they are otherwise identical.
 *
 * 定义哈希表条目标识的哈希表键。  我们按用户和数据库分开查询，即使它们在其他方面是相同的。
 *
 * If you add a new key to this struct, make sure to teach pgss_store() to
 * zero the padding bytes.  Otherwise, things will break, because pgss_hash is
 * created using HASH_BLOBS, and thus tag_hash is used to hash this.
 *
 * 如果向该结构添加新键，请确保教导 pgss_store() 将填充字节清零。  否则，事情就会崩溃，因为 pgss_hash 是使用 HASH_BLOBS 创建的，因此 tag_hash 用于对其进行哈希处理。

 */
typedef struct pgssHashKey
{
	Oid			userid;			/* user OID */
	Oid			dbid;			/* database OID */
	int64		queryid;		/* query identifier */
	bool		toplevel;		/* query executed at top level */
} pgssHashKey;

/*
 * The actual stats counters kept within pgssEntry.
 *
 * 实际的统计计数器保存在 pgssEntry 中。
 */
typedef struct Counters
{
	int64		calls[PGSS_NUMKIND];	/* # of times planned/executed */
	double		total_time[PGSS_NUMKIND];	/* total planning/execution time,
											 * in msec */
	double		min_time[PGSS_NUMKIND]; /* minimum planning/execution time in
										 * msec since min/max reset */
	double		max_time[PGSS_NUMKIND]; /* maximum planning/execution time in
										 * msec since min/max reset */
	double		mean_time[PGSS_NUMKIND];	/* mean planning/execution time in
											 * msec */
	double		sum_var_time[PGSS_NUMKIND]; /* sum of variances in
											 * planning/execution time in msec */
	int64		rows;			/* total # of retrieved or affected rows */
	int64		shared_blks_hit;	/* # of shared buffer hits */
	int64		shared_blks_read;	/* # of shared disk blocks read */
	int64		shared_blks_dirtied;	/* # of shared disk blocks dirtied */
	int64		shared_blks_written;	/* # of shared disk blocks written */
	int64		local_blks_hit; /* # of local buffer hits */
	int64		local_blks_read;	/* # of local disk blocks read */
	int64		local_blks_dirtied; /* # of local disk blocks dirtied */
	int64		local_blks_written; /* # of local disk blocks written */
	int64		temp_blks_read; /* # of temp blocks read */
	int64		temp_blks_written;	/* # of temp blocks written */
	double		shared_blk_read_time;	/* time spent reading shared blocks,
										 * in msec */
	double		shared_blk_write_time;	/* time spent writing shared blocks,
										 * in msec */
	double		local_blk_read_time;	/* time spent reading local blocks, in
										 * msec */
	double		local_blk_write_time;	/* time spent writing local blocks, in
										 * msec */
	double		temp_blk_read_time; /* time spent reading temp blocks, in msec */
	double		temp_blk_write_time;	/* time spent writing temp blocks, in
										 * msec */
	double		usage;			/* usage factor */
	int64		wal_records;	/* # of WAL records generated */
	int64		wal_fpi;		/* # of WAL full page images generated */
	uint64		wal_bytes;		/* total amount of WAL generated in bytes */
	int64		wal_buffers_full;	/* # of times the WAL buffers became full */
	int64		jit_functions;	/* total number of JIT functions emitted */
	double		jit_generation_time;	/* total time to generate jit code */
	int64		jit_inlining_count; /* number of times inlining time has been
									 * > 0 */
	double		jit_deform_time;	/* total time to deform tuples in jit code */
	int64		jit_deform_count;	/* number of times deform time has been >
									 * 0 */

	double		jit_inlining_time;	/* total time to inline jit code */
	int64		jit_optimization_count; /* number of times optimization time
										 * has been > 0 */
	double		jit_optimization_time;	/* total time to optimize jit code */
	int64		jit_emission_count; /* number of times emission time has been
									 * > 0 */
	double		jit_emission_time;	/* total time to emit jit code */
	int64		parallel_workers_to_launch; /* # of parallel workers planned
											 * to be launched */
	int64		parallel_workers_launched;	/* # of parallel workers actually
											 * launched */
} Counters;

/*
 * Global statistics for pg_stat_statements
 *
 * pg_stat_statements 的全局统计信息
 */
typedef struct pgssGlobalStats
{
	int64		dealloc;		/* # of times entries were deallocated */
	TimestampTz stats_reset;	/* timestamp with all stats reset */
} pgssGlobalStats;

/*
 * Statistics per statement
 *
 * 每个语句的统计数据
 *
 * Note: in event of a failure in garbage collection of the query text file,
 * we reset query_offset to zero and query_len to -1.  This will be seen as
 * an invalid state by qtext_fetch().
 *
 * 注意：如果查询文本文件的垃圾收集失败，我们将 query_offset 重置为零，将 query_len 重置为 -1。  这将被 qtext_fetch() 视为无效状态。
 */
typedef struct pgssEntry
{
	pgssHashKey key;			/* hash key of entry - MUST BE FIRST */
	Counters	counters;		/* the statistics for this query */
	Size		query_offset;	/* query text offset in external file */
	int			query_len;		/* # of valid bytes in query string, or -1 */
	int			encoding;		/* query text encoding */
	TimestampTz stats_since;	/* timestamp of entry allocation */
	TimestampTz minmax_stats_since; /* timestamp of last min/max values reset */
	slock_t		mutex;			/* protects the counters only */
} pgssEntry;

/*
 * Global shared state
 *
 * 全局共享状态
 */
typedef struct pgssSharedState
{
	LWLock	   *lock;			/* protects hashtable search/modification */
	double		cur_median_usage;	/* current median usage in hashtable */
	Size		mean_query_len; /* current mean entry text length */
	slock_t		mutex;			/* protects following fields only: */
	Size		extent;			/* current extent of query file */
	int			n_writers;		/* number of active writers to query file */
	int			gc_count;		/* query file garbage collection cycle count */
	pgssGlobalStats stats;		/* global statistics for pgss */
} pgssSharedState;

/* ---- Local variables ----
 *
 * ---- 局部变量 ----
 */

/* Current nesting depth of planner/ExecutorRun/ProcessUtility calls
 *
 * Planner/ExecutorRun/ProcessUtility 调用的当前嵌套深度
 */
static int	nesting_level = 0;

/* Saved hook values
 *
 * 保存的挂钩值
 */
static shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static post_parse_analyze_hook_type prev_post_parse_analyze_hook = NULL;
static planner_hook_type prev_planner_hook = NULL;
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;
static ProcessUtility_hook_type prev_ProcessUtility = NULL;

/* Links to shared memory state
 *
 * 共享内存状态的链接
 */
static pgssSharedState *pgss = NULL;
static HTAB *pgss_hash = NULL;

/* ---- GUC variables ----
 *
 * ---- GUC 变量 ----
 */

typedef enum
{
	PGSS_TRACK_NONE,			/* track no statements */
	PGSS_TRACK_TOP,				/* only top level statements */
	PGSS_TRACK_ALL,				/* all statements, including nested ones */
}			PGSSTrackLevel;

static const struct config_enum_entry track_options[] =
{
	{"none", PGSS_TRACK_NONE, false},
	{"top", PGSS_TRACK_TOP, false},
	{"all", PGSS_TRACK_ALL, false},
	{NULL, 0, false}
};

static int	pgss_max = 5000;	/* max # statements to track */
static int	pgss_track = PGSS_TRACK_TOP;	/* tracking level */
static bool pgss_track_utility = true;	/* whether to track utility commands */
static bool pgss_track_planning = false;	/* whether to track planning
											 * duration */
static bool pgss_save = true;	/* whether to save stats across shutdown */

#define pgss_enabled(level) \
	(!IsParallelWorker() && \
	(pgss_track == PGSS_TRACK_ALL || \
	(pgss_track == PGSS_TRACK_TOP && (level) == 0)))

#define record_gc_qtexts() \
	do { \
		SpinLockAcquire(&pgss->mutex); \
		pgss->gc_count++; \
		SpinLockRelease(&pgss->mutex); \
	} while(0)

/* ---- Function declarations ----
 *
 * ---- 函数声明 ----
 */

PG_FUNCTION_INFO_V1(pg_stat_statements_reset);
PG_FUNCTION_INFO_V1(pg_stat_statements_reset_1_7);
PG_FUNCTION_INFO_V1(pg_stat_statements_reset_1_11);
PG_FUNCTION_INFO_V1(pg_stat_statements_1_2);
PG_FUNCTION_INFO_V1(pg_stat_statements_1_3);
PG_FUNCTION_INFO_V1(pg_stat_statements_1_8);
PG_FUNCTION_INFO_V1(pg_stat_statements_1_9);
PG_FUNCTION_INFO_V1(pg_stat_statements_1_10);
PG_FUNCTION_INFO_V1(pg_stat_statements_1_11);
PG_FUNCTION_INFO_V1(pg_stat_statements_1_12);
PG_FUNCTION_INFO_V1(pg_stat_statements);
PG_FUNCTION_INFO_V1(pg_stat_statements_info);

static void pgss_shmem_request(void);
static void pgss_shmem_startup(void);
static void pgss_shmem_shutdown(int code, Datum arg);
static void pgss_post_parse_analyze(ParseState *pstate, Query *query,
									JumbleState *jstate);
static PlannedStmt *pgss_planner(Query *parse,
								 const char *query_string,
								 int cursorOptions,
								 ParamListInfo boundParams);
static void pgss_ExecutorStart(QueryDesc *queryDesc, int eflags);
static void pgss_ExecutorRun(QueryDesc *queryDesc,
							 ScanDirection direction,
							 uint64 count);
static void pgss_ExecutorFinish(QueryDesc *queryDesc);
static void pgss_ExecutorEnd(QueryDesc *queryDesc);
static void pgss_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
								bool readOnlyTree,
								ProcessUtilityContext context, ParamListInfo params,
								QueryEnvironment *queryEnv,
								DestReceiver *dest, QueryCompletion *qc);
static void pgss_store(const char *query, int64 queryId,
					   int query_location, int query_len,
					   pgssStoreKind kind,
					   double total_time, uint64 rows,
					   const BufferUsage *bufusage,
					   const WalUsage *walusage,
					   const struct JitInstrumentation *jitusage,
					   JumbleState *jstate,
					   int parallel_workers_to_launch,
					   int parallel_workers_launched);
static void pg_stat_statements_internal(FunctionCallInfo fcinfo,
										pgssVersion api_version,
										bool showtext);
static Size pgss_memsize(void);
static pgssEntry *entry_alloc(pgssHashKey *key, Size query_offset, int query_len,
							  int encoding, bool sticky);
static void entry_dealloc(void);
static bool qtext_store(const char *query, int query_len,
						Size *query_offset, int *gc_count);
static char *qtext_load_file(Size *buffer_size);
static char *qtext_fetch(Size query_offset, int query_len,
						 char *buffer, Size buffer_size);
static bool need_gc_qtexts(void);
static void gc_qtexts(void);
static TimestampTz entry_reset(Oid userid, Oid dbid, int64 queryid, bool minmax_only);
static char *generate_normalized_query(JumbleState *jstate, const char *query,
									   int query_loc, int *query_len_p);
static void fill_in_constant_lengths(JumbleState *jstate, const char *query,
									 int query_loc);
static int	comp_location(const void *a, const void *b);


/*
 * Module load callback
 *
 * 模块加载回调
 */
void
_PG_init(void)
{
	/*
	 * In order to create our shared memory area, we have to be loaded via
	 * shared_preload_libraries.  If not, fall out without hooking into any of
	 * the main system.  (We don't throw error here because it seems useful to
	 * allow the pg_stat_statements functions to be created even when the
	 * module isn't active.  The functions must protect themselves against
	 * being called then, however.)
	 *
	 * 为了创建我们的共享内存区域，我们必须通过shared_preload_libraries进行加载。  如果没有，请在不连接到任何主系统的情况下退出。  （我们不会在这里抛出错误，因为即使模块未处于活动状态，允许创建 pg_stat_statements 函数似乎很有用。但是，这些函数必须保护自己不被调用。）
	 */
	if (!process_shared_preload_libraries_in_progress)
		return;

	/*
	 * Inform the postmaster that we want to enable query_id calculation if
	 * compute_query_id is set to auto.
	 *
	 * 通知邮局管理员，如果compute_query_id 设置为auto，我们要启用query_id 计算。
	 */
	EnableQueryId();

	/*
	 * Define (or redefine) custom GUC variables.
	 *
	 * 定义（或重新定义）自定义 GUC 变量。
	 */
	DefineCustomIntVariable("pg_stat_statements.max",
							"Sets the maximum number of statements tracked by pg_stat_statements.",
							NULL,
							&pgss_max,
							5000,
							100,
							INT_MAX / 2,
							PGC_POSTMASTER,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomEnumVariable("pg_stat_statements.track",
							 "Selects which statements are tracked by pg_stat_statements.",
							 NULL,
							 &pgss_track,
							 PGSS_TRACK_TOP,
							 track_options,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("pg_stat_statements.track_utility",
							 "Selects whether utility commands are tracked by pg_stat_statements.",
							 NULL,
							 &pgss_track_utility,
							 true,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("pg_stat_statements.track_planning",
							 "Selects whether planning duration is tracked by pg_stat_statements.",
							 NULL,
							 &pgss_track_planning,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("pg_stat_statements.save",
							 "Save pg_stat_statements statistics across server shutdowns.",
							 NULL,
							 &pgss_save,
							 true,
							 PGC_SIGHUP,
							 0,
							 NULL,
							 NULL,
							 NULL);

	MarkGUCPrefixReserved("pg_stat_statements");

	/*
	 * Install hooks.
	 *
	 * 安装挂钩。
	 */
	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = pgss_shmem_request;
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = pgss_shmem_startup;
	prev_post_parse_analyze_hook = post_parse_analyze_hook;
	post_parse_analyze_hook = pgss_post_parse_analyze;
	prev_planner_hook = planner_hook;
	planner_hook = pgss_planner;
	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = pgss_ExecutorStart;
	prev_ExecutorRun = ExecutorRun_hook;
	ExecutorRun_hook = pgss_ExecutorRun;
	prev_ExecutorFinish = ExecutorFinish_hook;
	ExecutorFinish_hook = pgss_ExecutorFinish;
	prev_ExecutorEnd = ExecutorEnd_hook;
	ExecutorEnd_hook = pgss_ExecutorEnd;
	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = pgss_ProcessUtility;
}

/*
 * shmem_request hook: request additional shared resources.  We'll allocate or
 * attach to the shared resources in pgss_shmem_startup().
 *
 * shmem_request 钩子：请求额外的共享资源。  我们将在 pgss_shmem_startup() 中分配或附加到共享资源。
 */
static void
pgss_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	RequestAddinShmemSpace(pgss_memsize());
	RequestNamedLWLockTranche("pg_stat_statements", 1);
}

/*
 * shmem_startup hook: allocate or attach to shared memory,
 * then load any pre-existing statistics from file.
 * Also create and load the query-texts file, which is expected to exist
 * (even if empty) while the module is enabled.
 *
 * shmem_startup 挂钩：分配或附加到共享内存，然后从文件加载任何预先存在的统计信息。还创建并加载查询文本文件，该文件在启用模块时预计存在（即使为空）。
 */
static void
pgss_shmem_startup(void)
{
	bool		found;
	HASHCTL		info;
	FILE	   *file = NULL;
	FILE	   *qfile = NULL;
	uint32		header;
	int32		num;
	int32		pgver;
	int32		i;
	int			buffer_size;
	char	   *buffer = NULL;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	/* reset in case this is a restart within the postmaster
	 *
	 * 重置以防邮局管理员内重新启动
	 */
	pgss = NULL;
	pgss_hash = NULL;

	/*
	 * Create or attach to the shared memory state, including hash table
	 *
	 * 创建或附加到共享内存状态，包括哈希表
	 */
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	pgss = ShmemInitStruct("pg_stat_statements",
						   sizeof(pgssSharedState),
						   &found);

	if (!found)
	{
		/* First time through ...
		 *
		 * 第一次通过...
		 */
		pgss->lock = &(GetNamedLWLockTranche("pg_stat_statements"))->lock;
		pgss->cur_median_usage = ASSUMED_MEDIAN_INIT;
		pgss->mean_query_len = ASSUMED_LENGTH_INIT;
		SpinLockInit(&pgss->mutex);
		pgss->extent = 0;
		pgss->n_writers = 0;
		pgss->gc_count = 0;
		pgss->stats.dealloc = 0;
		pgss->stats.stats_reset = GetCurrentTimestamp();
	}

	info.keysize = sizeof(pgssHashKey);
	info.entrysize = sizeof(pgssEntry);
	pgss_hash = ShmemInitHash("pg_stat_statements hash",
							  pgss_max, pgss_max,
							  &info,
							  HASH_ELEM | HASH_BLOBS);

	LWLockRelease(AddinShmemInitLock);

	/*
	 * If we're in the postmaster (or a standalone backend...), set up a shmem
	 * exit hook to dump the statistics to disk.
	 *
	 * 如果我们位于 postmaster（或独立后端...），请设置 shmem 退出挂钩以将统计信息转储到磁盘。
	 */
	if (!IsUnderPostmaster)
		on_shmem_exit(pgss_shmem_shutdown, (Datum) 0);

	/*
	 * Done if some other process already completed our initialization.
	 *
	 * 如果其他进程已经完成了我们的初始化，则完成。
	 */
	if (found)
		return;

	/*
	 * Note: we don't bother with locks here, because there should be no other
	 * processes running when this code is reached.
	 *
	 * 注意：我们在这里不关心锁，因为到达此代码时不应该有其他进程在运行。
	 */

	/* Unlink query text file possibly left over from crash
	 *
	 * 取消链接可能因崩溃而遗留下来的查询文本文件
	 */
	unlink(PGSS_TEXT_FILE);

	/* Allocate new query text temp file
	 *
	 * 分配新的查询文本临时文件
	 */
	qfile = AllocateFile(PGSS_TEXT_FILE, PG_BINARY_W);
	if (qfile == NULL)
		goto write_error;

	/*
	 * If we were told not to load old statistics, we're done.  (Note we do
	 * not try to unlink any old dump file in this case.  This seems a bit
	 * questionable but it's the historical behavior.)
	 *
	 * 如果我们被告知不要加载旧的统计数据，我们就完成了。  （请注意，在这种情况下，我们不会尝试取消任何旧转储文件的链接。这似乎有点可疑，但这是历史行为。）
	 */
	if (!pgss_save)
	{
		FreeFile(qfile);
		return;
	}

	/*
	 * Attempt to load old statistics from the dump file.
	 *
	 * 尝试从转储文件加载旧的统计信息。
	 */
	file = AllocateFile(PGSS_DUMP_FILE, PG_BINARY_R);
	if (file == NULL)
	{
		if (errno != ENOENT)
			goto read_error;
		/* No existing persisted stats file, so we're done
		 *
		 * 没有现有的持久统计文件，所以我们完成了
		 */
		FreeFile(qfile);
		return;
	}

	buffer_size = 2048;
	buffer = (char *) palloc(buffer_size);

	if (fread(&header, sizeof(uint32), 1, file) != 1 ||
		fread(&pgver, sizeof(uint32), 1, file) != 1 ||
		fread(&num, sizeof(int32), 1, file) != 1)
		goto read_error;

	if (header != PGSS_FILE_HEADER ||
		pgver != PGSS_PG_MAJOR_VERSION)
		goto data_error;

	for (i = 0; i < num; i++)
	{
		pgssEntry	temp;
		pgssEntry  *entry;
		Size		query_offset;

		if (fread(&temp, sizeof(pgssEntry), 1, file) != 1)
			goto read_error;

		/* Encoding is the only field we can easily sanity-check
		 *
		 * 编码是我们可以轻松进行健全性检查的唯一字段
		 */
		if (!PG_VALID_BE_ENCODING(temp.encoding))
			goto data_error;

		/* Resize buffer as needed
		 *
		 * 根据需要调整缓冲区大小
		 */
		if (temp.query_len >= buffer_size)
		{
			buffer_size = Max(buffer_size * 2, temp.query_len + 1);
			buffer = repalloc(buffer, buffer_size);
		}

		if (fread(buffer, 1, temp.query_len + 1, file) != temp.query_len + 1)
			goto read_error;

		/* Should have a trailing null, but let's make sure
		 *
		 * 应该有一个尾随 null，但让我们确保
		 */
		buffer[temp.query_len] = '\0';

		/* Skip loading "sticky" entries
		 *
		 * 跳过加载“粘性”条目
		 */
		if (IS_STICKY(temp.counters))
			continue;

		/* Store the query text
		 *
		 * 存储查询文本
		 */
		query_offset = pgss->extent;
		if (fwrite(buffer, 1, temp.query_len + 1, qfile) != temp.query_len + 1)
			goto write_error;
		pgss->extent += temp.query_len + 1;

		/* make the hashtable entry (discards old entries if too many)
		 *
		 * 创建哈希表条目（如果太多则丢弃旧条目）
		 */
		entry = entry_alloc(&temp.key, query_offset, temp.query_len,
							temp.encoding,
							false);

		/* copy in the actual stats
		 *
		 * 复制实际统计数据
		 */
		entry->counters = temp.counters;
		entry->stats_since = temp.stats_since;
		entry->minmax_stats_since = temp.minmax_stats_since;
	}

	/* Read global statistics for pg_stat_statements
	 *
	 * 读取 pg_stat_statements 的全局统计信息
	 */
	if (fread(&pgss->stats, sizeof(pgssGlobalStats), 1, file) != 1)
		goto read_error;

	pfree(buffer);
	FreeFile(file);
	FreeFile(qfile);

	/*
	 * Remove the persisted stats file so it's not included in
	 * backups/replication standbys, etc.  A new file will be written on next
	 * shutdown.
	 *
	 * 删除持久的统计文件，使其不包含在备份/复制备用文件等中。下次关闭时将写入新文件。
	 *
	 * Note: it's okay if the PGSS_TEXT_FILE is included in a basebackup,
	 * because we remove that file on startup; it acts inversely to
	 * PGSS_DUMP_FILE, in that it is only supposed to be around when the
	 * server is running, whereas PGSS_DUMP_FILE is only supposed to be around
	 * when the server is not running.  Leaving the file creates no danger of
	 * a newly restored database having a spurious record of execution costs,
	 * which is what we're really concerned about here.
	 *
	 * 注意：如果 PGSS_TEXT_FILE 包含在基础备份中也没关系，因为我们会在启动时删除该文件；它的作用与 PGSS_DUMP_FILE 相反，因为它只应该在服务器运行时存在，而 PGSS_DUMP_FILE 仅应该在服务器不运行时存在。  保留该文件不会产生新恢复的数据库具有执行成本的虚假记录的危险，这才是我们真正关心的。
	 */
	unlink(PGSS_DUMP_FILE);

	return;

read_error:
	ereport(LOG,
			(errcode_for_file_access(),
			 errmsg("could not read file \"%s\": %m",
					PGSS_DUMP_FILE)));
	goto fail;
data_error:
	ereport(LOG,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("ignoring invalid data in file \"%s\"",
					PGSS_DUMP_FILE)));
	goto fail;
write_error:
	ereport(LOG,
			(errcode_for_file_access(),
			 errmsg("could not write file \"%s\": %m",
					PGSS_TEXT_FILE)));
fail:
	if (buffer)
		pfree(buffer);
	if (file)
		FreeFile(file);
	if (qfile)
		FreeFile(qfile);
	/* If possible, throw away the bogus file; ignore any error
	 *
	 * 如果可能的话，扔掉伪造的文件；忽略任何错误
	 */
	unlink(PGSS_DUMP_FILE);

	/*
	 * Don't unlink PGSS_TEXT_FILE here; it should always be around while the
	 * server is running with pg_stat_statements enabled
	 *
	 * 不要在此处取消链接 PGSS_TEXT_FILE；当服务器在启用 pg_stat_statements 的情况下运行时，它应该始终存在
	 */
}

/*
 * shmem_shutdown hook: Dump statistics into file.
 *
 * shmem_shutdown 挂钩：将统计信息转储到文件中。
 *
 * Note: we don't bother with acquiring lock, because there should be no
 * other processes running when this is called.
 *
 * 注意：我们不关心获取锁，因为调用它时不应该有其他进程在运行。
 */
static void
pgss_shmem_shutdown(int code, Datum arg)
{
	FILE	   *file;
	char	   *qbuffer = NULL;
	Size		qbuffer_size = 0;
	HASH_SEQ_STATUS hash_seq;
	int32		num_entries;
	pgssEntry  *entry;

	/* Don't try to dump during a crash.
	 *
	 * 发生碰撞时请勿尝试倾倒。
	 */
	if (code)
		return;

	/* Safety check ... shouldn't get here unless shmem is set up.
	 *
	 * 安全检查...除非设置了 shmem，否则不应到达此处。
	 */
	if (!pgss || !pgss_hash)
		return;

	/* Don't dump if told not to.
	 *
	 * 如果被告知不要倾倒，请勿倾倒。
	 */
	if (!pgss_save)
		return;

	file = AllocateFile(PGSS_DUMP_FILE ".tmp", PG_BINARY_W);
	if (file == NULL)
		goto error;

	if (fwrite(&PGSS_FILE_HEADER, sizeof(uint32), 1, file) != 1)
		goto error;
	if (fwrite(&PGSS_PG_MAJOR_VERSION, sizeof(uint32), 1, file) != 1)
		goto error;
	num_entries = hash_get_num_entries(pgss_hash);
	if (fwrite(&num_entries, sizeof(int32), 1, file) != 1)
		goto error;

	qbuffer = qtext_load_file(&qbuffer_size);
	if (qbuffer == NULL)
		goto error;

	/*
	 * When serializing to disk, we store query texts immediately after their
	 * entry data.  Any orphaned query texts are thereby excluded.
	 *
	 * 当序列化到磁盘时，我们将查询文本存储在其输入数据之后。  从而排除任何孤立的查询文本。
	 */
	hash_seq_init(&hash_seq, pgss_hash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		int			len = entry->query_len;
		char	   *qstr = qtext_fetch(entry->query_offset, len,
									   qbuffer, qbuffer_size);

		if (qstr == NULL)
			continue;			/* Ignore any entries with bogus texts */

		if (fwrite(entry, sizeof(pgssEntry), 1, file) != 1 ||
			fwrite(qstr, 1, len + 1, file) != len + 1)
		{
			/* note: we assume hash_seq_term won't change errno
			 *
			 * 注意：我们假设 hash_seq_term 不会改变 errno
			 */
			hash_seq_term(&hash_seq);
			goto error;
		}
	}

	/* Dump global statistics for pg_stat_statements
	 *
	 * 转储 pg_stat_statements 的全局统计信息
	 */
	if (fwrite(&pgss->stats, sizeof(pgssGlobalStats), 1, file) != 1)
		goto error;

	free(qbuffer);
	qbuffer = NULL;

	if (FreeFile(file))
	{
		file = NULL;
		goto error;
	}

	/*
	 * Rename file into place, so we atomically replace any old one.
	 *
	 * 将文件重命名到位，以便我们自动替换任何旧文件。
	 */
	(void) durable_rename(PGSS_DUMP_FILE ".tmp", PGSS_DUMP_FILE, LOG);

	/* Unlink query-texts file; it's not needed while shutdown
	 *
	 * 取消链接查询文本文件；关机时不需要
	 */
	unlink(PGSS_TEXT_FILE);

	return;

error:
	ereport(LOG,
			(errcode_for_file_access(),
			 errmsg("could not write file \"%s\": %m",
					PGSS_DUMP_FILE ".tmp")));
	free(qbuffer);
	if (file)
		FreeFile(file);
	unlink(PGSS_DUMP_FILE ".tmp");
	unlink(PGSS_TEXT_FILE);
}

/*
 * Post-parse-analysis hook: mark query with a queryId
 *
 * 解析后分析钩子：用 queryId 标记查询
 */
static void
pgss_post_parse_analyze(ParseState *pstate, Query *query, JumbleState *jstate)
{
	if (prev_post_parse_analyze_hook)
		prev_post_parse_analyze_hook(pstate, query, jstate);

	/* Safety check...
	 *
	 * 安全检查...
	 */
	if (!pgss || !pgss_hash || !pgss_enabled(nesting_level))
		return;

	/*
	 * If it's EXECUTE, clear the queryId so that stats will accumulate for
	 * the underlying PREPARE.  But don't do this if we're not tracking
	 * utility statements, to avoid messing up another extension that might be
	 * tracking them.
	 *
	 * 如果是 EXECUTE，则清除 queryId，以便为基础 PREPARE 累积统计信息。  但是，如果我们不跟踪实用程序语句，请不要这样做，以避免弄乱可能正在跟踪它们的另一个扩展。
	 */
	if (query->utilityStmt)
	{
		if (pgss_track_utility && IsA(query->utilityStmt, ExecuteStmt))
		{
			query->queryId = INT64CONST(0);
			return;
		}
	}

	/*
	 * If query jumbling were able to identify any ignorable constants, we
	 * immediately create a hash table entry for the query, so that we can
	 * record the normalized form of the query string.  If there were no such
	 * constants, the normalized string would be the same as the query text
	 * anyway, so there's no need for an early entry.
	 *
	 * 如果查询混乱能够识别任何可忽略的常量，我们立即为查询创建一个哈希表条目，以便我们可以记录查询字符串的规范化形式。  如果没有这样的常量，则规范化字符串无论如何都将与查询文本相同，因此无需提前输入。
	 */
	if (jstate && jstate->clocations_count > 0)
		pgss_store(pstate->p_sourcetext,
				   query->queryId,
				   query->stmt_location,
				   query->stmt_len,
				   PGSS_INVALID,
				   0,
				   0,
				   NULL,
				   NULL,
				   NULL,
				   jstate,
				   0,
				   0);
}

/*
 * Planner hook: forward to regular planner, but measure planning time
 * if needed.
 *
 * 计划者钩子：转发给常规计划者，但如果需要的话测量计划时间。
 */
static PlannedStmt *
pgss_planner(Query *parse,
			 const char *query_string,
			 int cursorOptions,
			 ParamListInfo boundParams)
{
	PlannedStmt *result;

	/*
	 * We can't process the query if no query_string is provided, as
	 * pgss_store needs it.  We also ignore query without queryid, as it would
	 * be treated as a utility statement, which may not be the case.
	 *
	 * 如果没有提供 query_string，我们将无法处理查询，因为 pgss_store 需要它。  我们还忽略没有 queryid 的查询，因为它会被视为实用程序语句，但情况可能并非如此。
	 */
	if (pgss_enabled(nesting_level)
		&& pgss_track_planning && query_string
		&& parse->queryId != INT64CONST(0))
	{
		instr_time	start;
		instr_time	duration;
		BufferUsage bufusage_start,
					bufusage;
		WalUsage	walusage_start,
					walusage;

		/* We need to track buffer usage as the planner can access them.
		 *
		 * 我们需要跟踪缓冲区的使用情况，因为规划器可以访问它们。
		 */
		bufusage_start = pgBufferUsage;

		/*
		 * Similarly the planner could write some WAL records in some cases
		 * (e.g. setting a hint bit with those being WAL-logged)
		 *
		 * 类似地，规划器在某些情况下可以写入一些 WAL 记录（例如，为 WAL 记录的记录设置提示位）
		 */
		walusage_start = pgWalUsage;
		INSTR_TIME_SET_CURRENT(start);

		nesting_level++;
		PG_TRY();
		{
			if (prev_planner_hook)
				result = prev_planner_hook(parse, query_string, cursorOptions,
										   boundParams);
			else
				result = standard_planner(parse, query_string, cursorOptions,
										  boundParams);
		}
		PG_FINALLY();
		{
			nesting_level--;
		}
		PG_END_TRY();

		INSTR_TIME_SET_CURRENT(duration);
		INSTR_TIME_SUBTRACT(duration, start);

		/* calc differences of buffer counters.
		 *
		 * 计算缓冲区计数器的差异。
		 */
		memset(&bufusage, 0, sizeof(BufferUsage));
		BufferUsageAccumDiff(&bufusage, &pgBufferUsage, &bufusage_start);

		/* calc differences of WAL counters.
		 *
		 * WAL 计数器的计算差异。
		 */
		memset(&walusage, 0, sizeof(WalUsage));
		WalUsageAccumDiff(&walusage, &pgWalUsage, &walusage_start);

		pgss_store(query_string,
				   parse->queryId,
				   parse->stmt_location,
				   parse->stmt_len,
				   PGSS_PLAN,
				   INSTR_TIME_GET_MILLISEC(duration),
				   0,
				   &bufusage,
				   &walusage,
				   NULL,
				   NULL,
				   0,
				   0);
	}
	else
	{
		/*
		 * Even though we're not tracking plan time for this statement, we
		 * must still increment the nesting level, to ensure that functions
		 * evaluated during planning are not seen as top-level calls.
		 *
		 * 即使我们没有跟踪此语句的计划时间，我们仍然必须增加嵌套级别，以确保在计划期间评估的函数不会被视为顶级调用。
		 */
		nesting_level++;
		PG_TRY();
		{
			if (prev_planner_hook)
				result = prev_planner_hook(parse, query_string, cursorOptions,
										   boundParams);
			else
				result = standard_planner(parse, query_string, cursorOptions,
										  boundParams);
		}
		PG_FINALLY();
		{
			nesting_level--;
		}
		PG_END_TRY();
	}

	return result;
}

/*
 * ExecutorStart hook: start up tracking if needed
 *
 * ExecutorStart hook：如果需要则启动跟踪
 */
static void
pgss_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);

	/*
	 * If query has queryId zero, don't track it.  This prevents double
	 * counting of optimizable statements that are directly contained in
	 * utility statements.
	 *
	 * 如果查询的 queryId 为零，则不跟踪它。  这可以防止对直接包含在实用程序语句中的可优化语句进行重复计算。
	 */
	if (pgss_enabled(nesting_level) && queryDesc->plannedstmt->queryId != INT64CONST(0))
	{
		/*
		 * Set up to track total elapsed time in ExecutorRun.  Make sure the
		 * space is allocated in the per-query context so it will go away at
		 * ExecutorEnd.
		 *
		 * 设置为跟踪 ExecutorRun 中的总运行时间。  确保在每个查询上下文中分配空间，以便它在 ExecutorEnd 时消失。
		 */
		if (queryDesc->totaltime == NULL)
		{
			MemoryContext oldcxt;

			oldcxt = MemoryContextSwitchTo(queryDesc->estate->es_query_cxt);
			queryDesc->totaltime = InstrAlloc(1, INSTRUMENT_ALL, false);
			MemoryContextSwitchTo(oldcxt);
		}
	}
}

/*
 * ExecutorRun hook: all we need do is track nesting depth
 *
 * ExecutorRun 钩子：我们需要做的就是跟踪嵌套深度
 */
static void
pgss_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction, uint64 count)
{
	nesting_level++;
	PG_TRY();
	{
		if (prev_ExecutorRun)
			prev_ExecutorRun(queryDesc, direction, count);
		else
			standard_ExecutorRun(queryDesc, direction, count);
	}
	PG_FINALLY();
	{
		nesting_level--;
	}
	PG_END_TRY();
}

/*
 * ExecutorFinish hook: all we need do is track nesting depth
 *
 * ExecutorFinish 钩子：我们需要做的就是跟踪嵌套深度
 */
static void
pgss_ExecutorFinish(QueryDesc *queryDesc)
{
	nesting_level++;
	PG_TRY();
	{
		if (prev_ExecutorFinish)
			prev_ExecutorFinish(queryDesc);
		else
			standard_ExecutorFinish(queryDesc);
	}
	PG_FINALLY();
	{
		nesting_level--;
	}
	PG_END_TRY();
}

/*
 * ExecutorEnd hook: store results if needed
 *
 * ExecutorEnd 钩子：如果需要则存储结果
 */
static void
pgss_ExecutorEnd(QueryDesc *queryDesc)
{
	int64		queryId = queryDesc->plannedstmt->queryId;

	if (queryId != INT64CONST(0) && queryDesc->totaltime &&
		pgss_enabled(nesting_level))
	{
		/*
		 * Make sure stats accumulation is done.  (Note: it's okay if several
		 * levels of hook all do this.)
		 *
		 * 确保统计数据积累已完成。  （注意：如果几个级别的钩子都这样做也没关系。）
		 */
		InstrEndLoop(queryDesc->totaltime);

		pgss_store(queryDesc->sourceText,
				   queryId,
				   queryDesc->plannedstmt->stmt_location,
				   queryDesc->plannedstmt->stmt_len,
				   PGSS_EXEC,
				   queryDesc->totaltime->total * 1000.0,	/* convert to msec */
				   queryDesc->estate->es_total_processed,
				   &queryDesc->totaltime->bufusage,
				   &queryDesc->totaltime->walusage,
				   queryDesc->estate->es_jit ? &queryDesc->estate->es_jit->instr : NULL,
				   NULL,
				   queryDesc->estate->es_parallel_workers_to_launch,
				   queryDesc->estate->es_parallel_workers_launched);
	}

	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}

/*
 * ProcessUtility hook
 *
 * ProcessUtility 钩子
 */
static void
pgss_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
					bool readOnlyTree,
					ProcessUtilityContext context,
					ParamListInfo params, QueryEnvironment *queryEnv,
					DestReceiver *dest, QueryCompletion *qc)
{
	Node	   *parsetree = pstmt->utilityStmt;
	int64		saved_queryId = pstmt->queryId;
	int			saved_stmt_location = pstmt->stmt_location;
	int			saved_stmt_len = pstmt->stmt_len;
	bool		enabled = pgss_track_utility && pgss_enabled(nesting_level);

	/*
	 * Force utility statements to get queryId zero.  We do this even in cases
	 * where the statement contains an optimizable statement for which a
	 * queryId could be derived (such as EXPLAIN or DECLARE CURSOR).  For such
	 * cases, runtime control will first go through ProcessUtility and then
	 * the executor, and we don't want the executor hooks to do anything,
	 * since we are already measuring the statement's costs at the utility
	 * level.
	 *
	 * 强制实用程序语句将 queryId 设为零。  即使语句包含可为其派生 queryId 的可优化语句（例如 EXPLAIN 或 DECLARE CURSOR），我们也会这样做。  对于这种情况，运行时控制将首先通过 ProcessUtility，然后通过执行器，并且我们不希望执行器挂钩执行任何操作，因为我们已经在实用程序级别测量语句的成本。
	 *
	 * Note that this is only done if pg_stat_statements is enabled and
	 * configured to track utility statements, in the unlikely possibility
	 * that user configured another extension to handle utility statements
	 * only.
	 *
	 * 请注意，只有在启用 pg_stat_statements 并配置为跟踪实用程序语句时才会执行此操作，用户配置另一个扩展以仅处理实用程序语句的可能性不大。
	 */
	if (enabled)
		pstmt->queryId = INT64CONST(0);

	/*
	 * If it's an EXECUTE statement, we don't track it and don't increment the
	 * nesting level.  This allows the cycles to be charged to the underlying
	 * PREPARE instead (by the Executor hooks), which is much more useful.
	 *
	 * 如果它是 EXECUTE 语句，我们不会跟踪它，也不会增加嵌套级别。  这允许将周期转而由底层 PREPARE 负责（通过 Executor 挂钩），这更加有用。
	 *
	 * We also don't track execution of PREPARE.  If we did, we would get one
	 * hash table entry for the PREPARE (with hash calculated from the query
	 * string), and then a different one with the same query string (but hash
	 * calculated from the query tree) would be used to accumulate costs of
	 * ensuing EXECUTEs.  This would be confusing.  Since PREPARE doesn't
	 * actually run the planner (only parse+rewrite), its costs are generally
	 * pretty negligible and it seems okay to just ignore it.
	 *
	 * 我们也不跟踪 PREPARE 的执行情况。  如果这样做，我们将获得 PREPARE 的一个哈希表条目（具有根据查询字符串计算的哈希值），然后使用具有相同查询字符串的不同哈希表条目（但根据查询树计算的哈希值）来累积后续 EXECUTE 的成本。  这会令人困惑。  由于 PREPARE 实际上并不运行规划器（仅解析+重写），因此它的成本通常可以忽略不计，并且似乎可以忽略它。
	 */
	if (enabled &&
		!IsA(parsetree, ExecuteStmt) &&
		!IsA(parsetree, PrepareStmt))
	{
		instr_time	start;
		instr_time	duration;
		uint64		rows;
		BufferUsage bufusage_start,
					bufusage;
		WalUsage	walusage_start,
					walusage;

		bufusage_start = pgBufferUsage;
		walusage_start = pgWalUsage;
		INSTR_TIME_SET_CURRENT(start);

		nesting_level++;
		PG_TRY();
		{
			if (prev_ProcessUtility)
				prev_ProcessUtility(pstmt, queryString, readOnlyTree,
									context, params, queryEnv,
									dest, qc);
			else
				standard_ProcessUtility(pstmt, queryString, readOnlyTree,
										context, params, queryEnv,
										dest, qc);
		}
		PG_FINALLY();
		{
			nesting_level--;
		}
		PG_END_TRY();

		/*
		 * CAUTION: do not access the *pstmt data structure again below here.
		 * If it was a ROLLBACK or similar, that data structure may have been
		 * freed.  We must copy everything we still need into local variables,
		 * which we did above.
		 *
		 * 注意：请勿在此处再次访问 *pstmt 数据结构。如果是 ROLLBACK 或类似的操作，则该数据结构可能已被释放。  我们必须将仍然需要的所有内容复制到局部变量中，正如我们上面所做的那样。
		 *
		 * For the same reason, we can't risk restoring pstmt->queryId to its
		 * former value, which'd otherwise be a good idea.
		 *
		 * 出于同样的原因，我们不能冒险将 pstmt->queryId 恢复为其以前的值，否则这是一个好主意。
		 */

		INSTR_TIME_SET_CURRENT(duration);
		INSTR_TIME_SUBTRACT(duration, start);

		/*
		 * Track the total number of rows retrieved or affected by the utility
		 * statements of COPY, FETCH, CREATE TABLE AS, CREATE MATERIALIZED
		 * VIEW, REFRESH MATERIALIZED VIEW and SELECT INTO.
		 *
		 * 跟踪 COPY、FETCH、CREATE TABLE AS、CREATE MATERIALIZED VIEW、REFRESH MATERIALIZED VIEW 和 SELECT INTO 实用程序语句检索或影响的总行数。
		 */
		rows = (qc && (qc->commandTag == CMDTAG_COPY ||
					   qc->commandTag == CMDTAG_FETCH ||
					   qc->commandTag == CMDTAG_SELECT ||
					   qc->commandTag == CMDTAG_REFRESH_MATERIALIZED_VIEW)) ?
			qc->nprocessed : 0;

		/* calc differences of buffer counters.
		 *
		 * 计算缓冲区计数器的差异。
		 */
		memset(&bufusage, 0, sizeof(BufferUsage));
		BufferUsageAccumDiff(&bufusage, &pgBufferUsage, &bufusage_start);

		/* calc differences of WAL counters.
		 *
		 * WAL 计数器的计算差异。
		 */
		memset(&walusage, 0, sizeof(WalUsage));
		WalUsageAccumDiff(&walusage, &pgWalUsage, &walusage_start);

		pgss_store(queryString,
				   saved_queryId,
				   saved_stmt_location,
				   saved_stmt_len,
				   PGSS_EXEC,
				   INSTR_TIME_GET_MILLISEC(duration),
				   rows,
				   &bufusage,
				   &walusage,
				   NULL,
				   NULL,
				   0,
				   0);
	}
	else
	{
		/*
		 * Even though we're not tracking execution time for this statement,
		 * we must still increment the nesting level, to ensure that functions
		 * evaluated within it are not seen as top-level calls.  But don't do
		 * so for EXECUTE; that way, when control reaches pgss_planner or
		 * pgss_ExecutorStart, we will treat the costs as top-level if
		 * appropriate.  Likewise, don't bump for PREPARE, so that parse
		 * analysis will treat the statement as top-level if appropriate.
		 *
		 * 即使我们没有跟踪此语句的执行时间，我们仍然必须增加嵌套级别，以确保在其中计算的函数不会被视为顶级调用。  但不要为 EXECUTE 这样做；这样，当控制到达 pgss_planner 或 pgss_ExecutorStart 时，如果合适，我们将把成本视为顶级。  同样，不要碰撞 PREPARE，以便解析分析会在适当的情况下将该语句视为顶级语句。
		 *
		 * To be absolutely certain we don't mess up the nesting level,
		 * evaluate the bump_level condition just once.
		 *
		 * 为了绝对确定我们不会弄乱嵌套级别，请仅评估一次凹凸级别条件。
		 */
		bool		bump_level =
			!IsA(parsetree, ExecuteStmt) &&
			!IsA(parsetree, PrepareStmt);

		if (bump_level)
			nesting_level++;
		PG_TRY();
		{
			if (prev_ProcessUtility)
				prev_ProcessUtility(pstmt, queryString, readOnlyTree,
									context, params, queryEnv,
									dest, qc);
			else
				standard_ProcessUtility(pstmt, queryString, readOnlyTree,
										context, params, queryEnv,
										dest, qc);
		}
		PG_FINALLY();
		{
			if (bump_level)
				nesting_level--;
		}
		PG_END_TRY();
	}
}

/*
 * Store some statistics for a statement.
 *
 * 存储语句的一些统计信息。
 *
 * If jstate is not NULL then we're trying to create an entry for which
 * we have no statistics as yet; we just want to record the normalized
 * query string.  total_time, rows, bufusage and walusage are ignored in this
 * case.
 *
 * 如果 jstate 不为 NULL，那么我们将尝试创建一个尚无统计信息的条目；我们只想记录规范化的查询字符串。  在这种情况下，total_time、rows、bufusage 和 walusage 将被忽略。
 *
 * If kind is PGSS_PLAN or PGSS_EXEC, its value is used as the array position
 * for the arrays in the Counters field.
 *
 * 如果 kind 是 PGSS_PLAN 或 PGSS_EXEC，则其值用作 Counters 字段中数组的数组位置。
 */
static void
pgss_store(const char *query, int64 queryId,
		   int query_location, int query_len,
		   pgssStoreKind kind,
		   double total_time, uint64 rows,
		   const BufferUsage *bufusage,
		   const WalUsage *walusage,
		   const struct JitInstrumentation *jitusage,
		   JumbleState *jstate,
		   int parallel_workers_to_launch,
		   int parallel_workers_launched)
{
	pgssHashKey key;
	pgssEntry  *entry;
	char	   *norm_query = NULL;
	int			encoding = GetDatabaseEncoding();

	Assert(query != NULL);

	/* Safety check...
	 *
	 * 安全检查...
	 */
	if (!pgss || !pgss_hash)
		return;

	/*
	 * Nothing to do if compute_query_id isn't enabled and no other module
	 * computed a query identifier.
	 *
	 * 如果未启用compute_query_id并且没有其他模块计算查询标识符，则无需执行任何操作。
	 */
	if (queryId == INT64CONST(0))
		return;

	/*
	 * Confine our attention to the relevant part of the string, if the query
	 * is a portion of a multi-statement source string, and update query
	 * location and length if needed.
	 *
	 * 如果查询是多语句源字符串的一部分，请将我们的注意力限制在字符串的相关部分，并根据需要更新查询位置和长度。
	 */
	query = CleanQuerytext(query, &query_location, &query_len);

	/* Set up key for hashtable search
	 *
	 * 设置哈希表搜索的键
	 */

	/* clear padding
	 *
	 * 清除填充
	 */
	memset(&key, 0, sizeof(pgssHashKey));

	key.userid = GetUserId();
	key.dbid = MyDatabaseId;
	key.queryid = queryId;
	key.toplevel = (nesting_level == 0);

	/* Lookup the hash table entry with shared lock.
	 *
	 * 查找具有共享锁的哈希表条目。
	 */
	LWLockAcquire(pgss->lock, LW_SHARED);

	entry = (pgssEntry *) hash_search(pgss_hash, &key, HASH_FIND, NULL);

	/* Create new entry, if not present
	 *
	 * 创建新条目（如果不存在）
	 */
	if (!entry)
	{
		Size		query_offset;
		int			gc_count;
		bool		stored;
		bool		do_gc;

		/*
		 * Create a new, normalized query string if caller asked.  We don't
		 * need to hold the lock while doing this work.  (Note: in any case,
		 * it's possible that someone else creates a duplicate hashtable entry
		 * in the interval where we don't hold the lock below.  That case is
		 * handled by entry_alloc.)
		 *
		 * 如果调用者询问，则创建一个新的规范化查询字符串。  我们在做这项工作时不需要持有锁。  （注意：在任何情况下，其他人都可能在我们不持有下面锁的区间内创建重复的哈希表条目。这种情况由entry_alloc处理。）
		 */
		if (jstate)
		{
			LWLockRelease(pgss->lock);
			norm_query = generate_normalized_query(jstate, query,
												   query_location,
												   &query_len);
			LWLockAcquire(pgss->lock, LW_SHARED);
		}

		/* Append new query text to file with only shared lock held
		 *
		 * 将新查询文本附加到仅持有共享锁的文件
		 */
		stored = qtext_store(norm_query ? norm_query : query, query_len,
							 &query_offset, &gc_count);

		/*
		 * Determine whether we need to garbage collect external query texts
		 * while the shared lock is still held.  This micro-optimization
		 * avoids taking the time to decide this while holding exclusive lock.
		 *
		 * 确定我们是否需要在仍持有共享锁的情况下对外部查询文本进行垃圾收集。  这种微观优化避免了在持有独占锁时花时间来决定这一点。
		 */
		do_gc = need_gc_qtexts();

		/* Need exclusive lock to make a new hashtable entry - promote
		 *
		 * 需要独占锁来创建新的哈希表条目 - 提升
		 */
		LWLockRelease(pgss->lock);
		LWLockAcquire(pgss->lock, LW_EXCLUSIVE);

		/*
		 * A garbage collection may have occurred while we weren't holding the
		 * lock.  In the unlikely event that this happens, the query text we
		 * stored above will have been garbage collected, so write it again.
		 * This should be infrequent enough that doing it while holding
		 * exclusive lock isn't a performance problem.
		 *
		 * 当我们没有持有锁时，可能会发生垃圾收集。  万一发生这种情况，我们上面存储的查询文本将被垃圾收集，因此请重新编写。这种情况应该很少发生，因此在持有独占锁的情况下执行此操作不会造成性能问题。
		 */
		if (!stored || pgss->gc_count != gc_count)
			stored = qtext_store(norm_query ? norm_query : query, query_len,
								 &query_offset, NULL);

		/* If we failed to write to the text file, give up
		 *
		 * 如果我们无法写入文本文件，则放弃
		 */
		if (!stored)
			goto done;

		/* OK to create a new hashtable entry
		 *
		 * 确定创建一个新的哈希表条目
		 */
		entry = entry_alloc(&key, query_offset, query_len, encoding,
							jstate != NULL);

		/* If needed, perform garbage collection while exclusive lock held
		 *
		 * 如果需要，在持有独占锁时执行垃圾收集
		 */
		if (do_gc)
			gc_qtexts();
	}

	/* Increment the counts, except when jstate is not NULL
	 *
	 * 增加计数，除非 jstate 不为 NULL
	 */
	if (!jstate)
	{
		Assert(kind == PGSS_PLAN || kind == PGSS_EXEC);

		/*
		 * Grab the spinlock while updating the counters (see comment about
		 * locking rules at the head of the file)
		 *
		 * 更新计数器时抓住自旋锁（请参阅文件头部有关锁定规则的注释）
		 */
		SpinLockAcquire(&entry->mutex);

		/* "Unstick" entry if it was previously sticky
		 *
		 * 如果条目之前是粘着的，则“取消粘着”条目
		 */
		if (IS_STICKY(entry->counters))
			entry->counters.usage = USAGE_INIT;

		entry->counters.calls[kind] += 1;
		entry->counters.total_time[kind] += total_time;

		if (entry->counters.calls[kind] == 1)
		{
			entry->counters.min_time[kind] = total_time;
			entry->counters.max_time[kind] = total_time;
			entry->counters.mean_time[kind] = total_time;
		}
		else
		{
			/*
			 * Welford's method for accurately computing variance. See
			 * <http://www.johndcook.com/blog/standard_deviation/>
			 *
			 * 准确计算方差的韦尔福德方法。请参阅<http://www.johndcook.com/blog/standard_deviation/>
			 */
			double		old_mean = entry->counters.mean_time[kind];

			entry->counters.mean_time[kind] +=
				(total_time - old_mean) / entry->counters.calls[kind];
			entry->counters.sum_var_time[kind] +=
				(total_time - old_mean) * (total_time - entry->counters.mean_time[kind]);

			/*
			 * Calculate min and max time. min = 0 and max = 0 means that the
			 * min/max statistics were reset
			 *
			 * 计算最短和最长时间。 min = 0 和 max = 0 表示最小/最大统计数据已重置
			 */
			if (entry->counters.min_time[kind] == 0
				&& entry->counters.max_time[kind] == 0)
			{
				entry->counters.min_time[kind] = total_time;
				entry->counters.max_time[kind] = total_time;
			}
			else
			{
				if (entry->counters.min_time[kind] > total_time)
					entry->counters.min_time[kind] = total_time;
				if (entry->counters.max_time[kind] < total_time)
					entry->counters.max_time[kind] = total_time;
			}
		}
		entry->counters.rows += rows;
		entry->counters.shared_blks_hit += bufusage->shared_blks_hit;
		entry->counters.shared_blks_read += bufusage->shared_blks_read;
		entry->counters.shared_blks_dirtied += bufusage->shared_blks_dirtied;
		entry->counters.shared_blks_written += bufusage->shared_blks_written;
		entry->counters.local_blks_hit += bufusage->local_blks_hit;
		entry->counters.local_blks_read += bufusage->local_blks_read;
		entry->counters.local_blks_dirtied += bufusage->local_blks_dirtied;
		entry->counters.local_blks_written += bufusage->local_blks_written;
		entry->counters.temp_blks_read += bufusage->temp_blks_read;
		entry->counters.temp_blks_written += bufusage->temp_blks_written;
		entry->counters.shared_blk_read_time += INSTR_TIME_GET_MILLISEC(bufusage->shared_blk_read_time);
		entry->counters.shared_blk_write_time += INSTR_TIME_GET_MILLISEC(bufusage->shared_blk_write_time);
		entry->counters.local_blk_read_time += INSTR_TIME_GET_MILLISEC(bufusage->local_blk_read_time);
		entry->counters.local_blk_write_time += INSTR_TIME_GET_MILLISEC(bufusage->local_blk_write_time);
		entry->counters.temp_blk_read_time += INSTR_TIME_GET_MILLISEC(bufusage->temp_blk_read_time);
		entry->counters.temp_blk_write_time += INSTR_TIME_GET_MILLISEC(bufusage->temp_blk_write_time);
		entry->counters.usage += USAGE_EXEC(total_time);
		entry->counters.wal_records += walusage->wal_records;
		entry->counters.wal_fpi += walusage->wal_fpi;
		entry->counters.wal_bytes += walusage->wal_bytes;
		entry->counters.wal_buffers_full += walusage->wal_buffers_full;
		if (jitusage)
		{
			entry->counters.jit_functions += jitusage->created_functions;
			entry->counters.jit_generation_time += INSTR_TIME_GET_MILLISEC(jitusage->generation_counter);

			if (INSTR_TIME_GET_MILLISEC(jitusage->deform_counter))
				entry->counters.jit_deform_count++;
			entry->counters.jit_deform_time += INSTR_TIME_GET_MILLISEC(jitusage->deform_counter);

			if (INSTR_TIME_GET_MILLISEC(jitusage->inlining_counter))
				entry->counters.jit_inlining_count++;
			entry->counters.jit_inlining_time += INSTR_TIME_GET_MILLISEC(jitusage->inlining_counter);

			if (INSTR_TIME_GET_MILLISEC(jitusage->optimization_counter))
				entry->counters.jit_optimization_count++;
			entry->counters.jit_optimization_time += INSTR_TIME_GET_MILLISEC(jitusage->optimization_counter);

			if (INSTR_TIME_GET_MILLISEC(jitusage->emission_counter))
				entry->counters.jit_emission_count++;
			entry->counters.jit_emission_time += INSTR_TIME_GET_MILLISEC(jitusage->emission_counter);
		}

		/* parallel worker counters
		 *
		 * 并行工作计数器
		 */
		entry->counters.parallel_workers_to_launch += parallel_workers_to_launch;
		entry->counters.parallel_workers_launched += parallel_workers_launched;

		SpinLockRelease(&entry->mutex);
	}

done:
	LWLockRelease(pgss->lock);

	/* We postpone this clean-up until we're out of the lock
	 *
	 * 我们推迟这次清理工作，直到我们解除锁定为止
	 */
	if (norm_query)
		pfree(norm_query);
}

/*
 * Reset statement statistics corresponding to userid, dbid, and queryid.
 *
 * 重置userid、dbid、queryid对应的语句统计信息。
 */
Datum
pg_stat_statements_reset_1_7(PG_FUNCTION_ARGS)
{
	Oid			userid;
	Oid			dbid;
	int64		queryid;

	userid = PG_GETARG_OID(0);
	dbid = PG_GETARG_OID(1);
	queryid = PG_GETARG_INT64(2);

	entry_reset(userid, dbid, queryid, false);

	PG_RETURN_VOID();
}

Datum
pg_stat_statements_reset_1_11(PG_FUNCTION_ARGS)
{
	Oid			userid;
	Oid			dbid;
	int64		queryid;
	bool		minmax_only;

	userid = PG_GETARG_OID(0);
	dbid = PG_GETARG_OID(1);
	queryid = PG_GETARG_INT64(2);
	minmax_only = PG_GETARG_BOOL(3);

	PG_RETURN_TIMESTAMPTZ(entry_reset(userid, dbid, queryid, minmax_only));
}

/*
 * Reset statement statistics.
 *
 * 重置语句统计。
 */
Datum
pg_stat_statements_reset(PG_FUNCTION_ARGS)
{
	entry_reset(0, 0, 0, false);

	PG_RETURN_VOID();
}

/* Number of output arguments (columns) for various API versions
 *
 * 各种 API 版本的输出参数（列）数量
 */
#define PG_STAT_STATEMENTS_COLS_V1_0	14
#define PG_STAT_STATEMENTS_COLS_V1_1	18
#define PG_STAT_STATEMENTS_COLS_V1_2	19
#define PG_STAT_STATEMENTS_COLS_V1_3	23
#define PG_STAT_STATEMENTS_COLS_V1_8	32
#define PG_STAT_STATEMENTS_COLS_V1_9	33
#define PG_STAT_STATEMENTS_COLS_V1_10	43
#define PG_STAT_STATEMENTS_COLS_V1_11	49
#define PG_STAT_STATEMENTS_COLS_V1_12	52
#define PG_STAT_STATEMENTS_COLS			52	/* maximum of above */

/*
 * Retrieve statement statistics.
 *
 * 检索语句统计信息。
 *
 * The SQL API of this function has changed multiple times, and will likely
 * do so again in future.  To support the case where a newer version of this
 * loadable module is being used with an old SQL declaration of the function,
 * we continue to support the older API versions.  For 1.2 and later, the
 * expected API version is identified by embedding it in the C name of the
 * function.  Unfortunately we weren't bright enough to do that for 1.1.
 *
 * 此函数的 SQL API 已更改多次，并且将来可能会再次更改。  为了支持此可加载模块的较新版本与旧的 SQL 函数声明一起使用的情况，我们继续支持旧的 API 版本。  对于 1.2 及更高版本，预期的 API 版本通过将其嵌入到函数的 C 名称中来标识。  不幸的是我们不够聪明，无法在 1.1 中做到这一点。
 */
Datum
pg_stat_statements_1_12(PG_FUNCTION_ARGS)
{
	bool		showtext = PG_GETARG_BOOL(0);

	pg_stat_statements_internal(fcinfo, PGSS_V1_12, showtext);

	return (Datum) 0;
}

Datum
pg_stat_statements_1_11(PG_FUNCTION_ARGS)
{
	bool		showtext = PG_GETARG_BOOL(0);

	pg_stat_statements_internal(fcinfo, PGSS_V1_11, showtext);

	return (Datum) 0;
}

Datum
pg_stat_statements_1_10(PG_FUNCTION_ARGS)
{
	bool		showtext = PG_GETARG_BOOL(0);

	pg_stat_statements_internal(fcinfo, PGSS_V1_10, showtext);

	return (Datum) 0;
}

Datum
pg_stat_statements_1_9(PG_FUNCTION_ARGS)
{
	bool		showtext = PG_GETARG_BOOL(0);

	pg_stat_statements_internal(fcinfo, PGSS_V1_9, showtext);

	return (Datum) 0;
}

Datum
pg_stat_statements_1_8(PG_FUNCTION_ARGS)
{
	bool		showtext = PG_GETARG_BOOL(0);

	pg_stat_statements_internal(fcinfo, PGSS_V1_8, showtext);

	return (Datum) 0;
}

Datum
pg_stat_statements_1_3(PG_FUNCTION_ARGS)
{
	bool		showtext = PG_GETARG_BOOL(0);

	pg_stat_statements_internal(fcinfo, PGSS_V1_3, showtext);

	return (Datum) 0;
}

Datum
pg_stat_statements_1_2(PG_FUNCTION_ARGS)
{
	bool		showtext = PG_GETARG_BOOL(0);

	pg_stat_statements_internal(fcinfo, PGSS_V1_2, showtext);

	return (Datum) 0;
}

/*
 * Legacy entry point for pg_stat_statements() API versions 1.0 and 1.1.
 * This can be removed someday, perhaps.
 *
 * pg_stat_statements() API 版本 1.0 和 1.1 的旧入口点。也许有一天这个可以被删除。
 */
Datum
pg_stat_statements(PG_FUNCTION_ARGS)
{
	/* If it's really API 1.1, we'll figure that out below
	 *
	 * 如果真的是 API 1.1，我们会在下面弄清楚
	 */
	pg_stat_statements_internal(fcinfo, PGSS_V1_0, true);

	return (Datum) 0;
}

/* Common code for all versions of pg_stat_statements()
 *
 * 所有版本 pg_stat_statements() 的通用代码
 */
static void
pg_stat_statements_internal(FunctionCallInfo fcinfo,
							pgssVersion api_version,
							bool showtext)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Oid			userid = GetUserId();
	bool		is_allowed_role = false;
	char	   *qbuffer = NULL;
	Size		qbuffer_size = 0;
	Size		extent = 0;
	int			gc_count = 0;
	HASH_SEQ_STATUS hash_seq;
	pgssEntry  *entry;

	/*
	 * Superusers or roles with the privileges of pg_read_all_stats members
	 * are allowed
	 *
	 * 允许具有 pg_read_all_stats 成员权限的超级用户或角色
	 */
	is_allowed_role = has_privs_of_role(userid, ROLE_PG_READ_ALL_STATS);

	/* hash table must exist already
	 *
	 * 哈希表必须已经存在
	 */
	if (!pgss || !pgss_hash)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_stat_statements must be loaded via \"shared_preload_libraries\"")));

	InitMaterializedSRF(fcinfo, 0);

	/*
	 * Check we have the expected number of output arguments.  Aside from
	 * being a good safety check, we need a kluge here to detect API version
	 * 1.1, which was wedged into the code in an ill-considered way.
	 *
	 * 检查我们是否有预期数量的输出参数。  除了良好的安全检查之外，我们还需要一个工具来检测 API 版本 1.1，该版本以一种考虑不周的方式嵌入到代码中。
	 */
	switch (rsinfo->setDesc->natts)
	{
		case PG_STAT_STATEMENTS_COLS_V1_0:
			if (api_version != PGSS_V1_0)
				elog(ERROR, "incorrect number of output arguments");
			break;
		case PG_STAT_STATEMENTS_COLS_V1_1:
			/* pg_stat_statements() should have told us 1.0
			 *
			 * pg_stat_statements() 应该告诉我们 1.0
			 */
			if (api_version != PGSS_V1_0)
				elog(ERROR, "incorrect number of output arguments");
			api_version = PGSS_V1_1;
			break;
		case PG_STAT_STATEMENTS_COLS_V1_2:
			if (api_version != PGSS_V1_2)
				elog(ERROR, "incorrect number of output arguments");
			break;
		case PG_STAT_STATEMENTS_COLS_V1_3:
			if (api_version != PGSS_V1_3)
				elog(ERROR, "incorrect number of output arguments");
			break;
		case PG_STAT_STATEMENTS_COLS_V1_8:
			if (api_version != PGSS_V1_8)
				elog(ERROR, "incorrect number of output arguments");
			break;
		case PG_STAT_STATEMENTS_COLS_V1_9:
			if (api_version != PGSS_V1_9)
				elog(ERROR, "incorrect number of output arguments");
			break;
		case PG_STAT_STATEMENTS_COLS_V1_10:
			if (api_version != PGSS_V1_10)
				elog(ERROR, "incorrect number of output arguments");
			break;
		case PG_STAT_STATEMENTS_COLS_V1_11:
			if (api_version != PGSS_V1_11)
				elog(ERROR, "incorrect number of output arguments");
			break;
		case PG_STAT_STATEMENTS_COLS_V1_12:
			if (api_version != PGSS_V1_12)
				elog(ERROR, "incorrect number of output arguments");
			break;
		default:
			elog(ERROR, "incorrect number of output arguments");
	}

	/*
	 * We'd like to load the query text file (if needed) while not holding any
	 * lock on pgss->lock.  In the worst case we'll have to do this again
	 * after we have the lock, but it's unlikely enough to make this a win
	 * despite occasional duplicated work.  We need to reload if anybody
	 * writes to the file (either a retail qtext_store(), or a garbage
	 * collection) between this point and where we've gotten shared lock.  If
	 * a qtext_store is actually in progress when we look, we might as well
	 * skip the speculative load entirely.
	 *
	 * 我们希望加载查询文本文件（如果需要），同时不对 pgss->lock 持有任何锁。  在最坏的情况下，我们在获得锁后将不得不再次执行此操作，但尽管偶尔会出现重复工作，但它不太可能取得胜利。  如果在此时和我们获得共享锁之间有人写入文件（零售 qtext_store() 或垃圾收集），我们需要重新加载。  如果当我们查看时 qtext_store 实际上正在进行中，我们不妨完全跳过推测加载。
	 */
	if (showtext)
	{
		int			n_writers;

		/* Take the mutex so we can examine variables
		 *
		 * 获取互斥锁，以便我们可以检查变量
		 */
		SpinLockAcquire(&pgss->mutex);
		extent = pgss->extent;
		n_writers = pgss->n_writers;
		gc_count = pgss->gc_count;
		SpinLockRelease(&pgss->mutex);

		/* No point in loading file now if there are active writers
		 *
		 * 如果有活跃的写入者，现在加载文件就没有意义了
		 */
		if (n_writers == 0)
			qbuffer = qtext_load_file(&qbuffer_size);
	}

	/*
	 * Get shared lock, load or reload the query text file if we must, and
	 * iterate over the hashtable entries.
	 *
	 * 获取共享锁，加载或重新加载查询文本文件（如果需要），并迭代哈希表条目。
	 *
	 * With a large hash table, we might be holding the lock rather longer
	 * than one could wish.  However, this only blocks creation of new hash
	 * table entries, and the larger the hash table the less likely that is to
	 * be needed.  So we can hope this is okay.  Perhaps someday we'll decide
	 * we need to partition the hash table to limit the time spent holding any
	 * one lock.
	 *
	 * 对于一个大的哈希表，我们持有锁的时间可能会比人们希望的要长。  然而，这只会阻止创建新的哈希表条目，并且哈希表越大，需要的可能性就越小。  所以我们希望这没问题。  也许有一天我们会决定需要对哈希表进行分区以限制持有任何一个锁所花费的时间。
	 */
	LWLockAcquire(pgss->lock, LW_SHARED);

	if (showtext)
	{
		/*
		 * Here it is safe to examine extent and gc_count without taking the
		 * mutex.  Note that although other processes might change
		 * pgss->extent just after we look at it, the strings they then write
		 * into the file cannot yet be referenced in the hashtable, so we
		 * don't care whether we see them or not.
		 *
		 * 在这里，可以安全地检查范围和 gc_count 而不使用互斥锁。  请注意，尽管其他进程可能会在我们查看后立即更改 pgss->extent，但它们随后写入文件的字符串尚无法在哈希表中引用，因此我们并不关心是否看到它们。
		 *
		 * If qtext_load_file fails, we just press on; we'll return NULL for
		 * every query text.
		 *
		 * 如果 qtext_load_file 失败，我们就继续；我们将为每个查询文本返回 NULL。
		 */
		if (qbuffer == NULL ||
			pgss->extent != extent ||
			pgss->gc_count != gc_count)
		{
			free(qbuffer);
			qbuffer = qtext_load_file(&qbuffer_size);
		}
	}

	hash_seq_init(&hash_seq, pgss_hash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		Datum		values[PG_STAT_STATEMENTS_COLS];
		bool		nulls[PG_STAT_STATEMENTS_COLS];
		int			i = 0;
		Counters	tmp;
		double		stddev;
		int64		queryid = entry->key.queryid;
		TimestampTz stats_since;
		TimestampTz minmax_stats_since;

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		values[i++] = ObjectIdGetDatum(entry->key.userid);
		values[i++] = ObjectIdGetDatum(entry->key.dbid);
		if (api_version >= PGSS_V1_9)
			values[i++] = BoolGetDatum(entry->key.toplevel);

		if (is_allowed_role || entry->key.userid == userid)
		{
			if (api_version >= PGSS_V1_2)
				values[i++] = Int64GetDatumFast(queryid);

			if (showtext)
			{
				char	   *qstr = qtext_fetch(entry->query_offset,
											   entry->query_len,
											   qbuffer,
											   qbuffer_size);

				if (qstr)
				{
					char	   *enc;

					enc = pg_any_to_server(qstr,
										   entry->query_len,
										   entry->encoding);

					values[i++] = CStringGetTextDatum(enc);

					if (enc != qstr)
						pfree(enc);
				}
				else
				{
					/* Just return a null if we fail to find the text
					 *
					 * 如果我们找不到文本，则返回 null
					 */
					nulls[i++] = true;
				}
			}
			else
			{
				/* Query text not requested
				 *
				 * 未请求查询文本
				 */
				nulls[i++] = true;
			}
		}
		else
		{
			/* Don't show queryid
			 *
			 * 不显示查询ID
			 */
			if (api_version >= PGSS_V1_2)
				nulls[i++] = true;

			/*
			 * Don't show query text, but hint as to the reason for not doing
			 * so if it was requested
			 *
			 * 不显示查询文本，但如果有要求，则提示不这样做的原因
			 */
			if (showtext)
				values[i++] = CStringGetTextDatum("<insufficient privilege>");
			else
				nulls[i++] = true;
		}

		/* copy counters to a local variable to keep locking time short
		 *
		 * 将计数器复制到局部变量以保持较短的锁定时间
		 */
		SpinLockAcquire(&entry->mutex);
		tmp = entry->counters;
		SpinLockRelease(&entry->mutex);

		/*
		 * The spinlock is not required when reading these two as they are
		 * always updated when holding pgss->lock exclusively.
		 *
		 * 读取这两个值时不需要自旋锁，因为它们在独占 pgss->lock 时总是会更新。
		 */
		stats_since = entry->stats_since;
		minmax_stats_since = entry->minmax_stats_since;

		/* Skip entry if unexecuted (ie, it's a pending "sticky" entry)
		 *
		 * 如果未执行则跳过条目（即，它是待处理的“粘性”条目）
		 */
		if (IS_STICKY(tmp))
			continue;

		/* Note that we rely on PGSS_PLAN being 0 and PGSS_EXEC being 1.
		 *
		 * 请注意，我们依赖 PGSS_PLAN 为 0 且 PGSS_EXEC 为 1。
		 */
		for (int kind = 0; kind < PGSS_NUMKIND; kind++)
		{
			if (kind == PGSS_EXEC || api_version >= PGSS_V1_8)
			{
				values[i++] = Int64GetDatumFast(tmp.calls[kind]);
				values[i++] = Float8GetDatumFast(tmp.total_time[kind]);
			}

			if ((kind == PGSS_EXEC && api_version >= PGSS_V1_3) ||
				api_version >= PGSS_V1_8)
			{
				values[i++] = Float8GetDatumFast(tmp.min_time[kind]);
				values[i++] = Float8GetDatumFast(tmp.max_time[kind]);
				values[i++] = Float8GetDatumFast(tmp.mean_time[kind]);

				/*
				 * Note we are calculating the population variance here, not
				 * the sample variance, as we have data for the whole
				 * population, so Bessel's correction is not used, and we
				 * don't divide by tmp.calls - 1.
				 *
				 * 请注意，我们在这里计算总体方差，而不是样本方差，因为我们有整个总体的数据，因此不使用贝塞尔校正，并且我们不除以 tmp.calls - 1。
				 */
				if (tmp.calls[kind] > 1)
					stddev = sqrt(tmp.sum_var_time[kind] / tmp.calls[kind]);
				else
					stddev = 0.0;
				values[i++] = Float8GetDatumFast(stddev);
			}
		}
		values[i++] = Int64GetDatumFast(tmp.rows);
		values[i++] = Int64GetDatumFast(tmp.shared_blks_hit);
		values[i++] = Int64GetDatumFast(tmp.shared_blks_read);
		if (api_version >= PGSS_V1_1)
			values[i++] = Int64GetDatumFast(tmp.shared_blks_dirtied);
		values[i++] = Int64GetDatumFast(tmp.shared_blks_written);
		values[i++] = Int64GetDatumFast(tmp.local_blks_hit);
		values[i++] = Int64GetDatumFast(tmp.local_blks_read);
		if (api_version >= PGSS_V1_1)
			values[i++] = Int64GetDatumFast(tmp.local_blks_dirtied);
		values[i++] = Int64GetDatumFast(tmp.local_blks_written);
		values[i++] = Int64GetDatumFast(tmp.temp_blks_read);
		values[i++] = Int64GetDatumFast(tmp.temp_blks_written);
		if (api_version >= PGSS_V1_1)
		{
			values[i++] = Float8GetDatumFast(tmp.shared_blk_read_time);
			values[i++] = Float8GetDatumFast(tmp.shared_blk_write_time);
		}
		if (api_version >= PGSS_V1_11)
		{
			values[i++] = Float8GetDatumFast(tmp.local_blk_read_time);
			values[i++] = Float8GetDatumFast(tmp.local_blk_write_time);
		}
		if (api_version >= PGSS_V1_10)
		{
			values[i++] = Float8GetDatumFast(tmp.temp_blk_read_time);
			values[i++] = Float8GetDatumFast(tmp.temp_blk_write_time);
		}
		if (api_version >= PGSS_V1_8)
		{
			char		buf[256];
			Datum		wal_bytes;

			values[i++] = Int64GetDatumFast(tmp.wal_records);
			values[i++] = Int64GetDatumFast(tmp.wal_fpi);

			snprintf(buf, sizeof buf, UINT64_FORMAT, tmp.wal_bytes);

			/* Convert to numeric.
			 *
			 * 转换为数字。
			 */
			wal_bytes = DirectFunctionCall3(numeric_in,
											CStringGetDatum(buf),
											ObjectIdGetDatum(0),
											Int32GetDatum(-1));
			values[i++] = wal_bytes;
		}
		if (api_version >= PGSS_V1_12)
		{
			values[i++] = Int64GetDatumFast(tmp.wal_buffers_full);
		}
		if (api_version >= PGSS_V1_10)
		{
			values[i++] = Int64GetDatumFast(tmp.jit_functions);
			values[i++] = Float8GetDatumFast(tmp.jit_generation_time);
			values[i++] = Int64GetDatumFast(tmp.jit_inlining_count);
			values[i++] = Float8GetDatumFast(tmp.jit_inlining_time);
			values[i++] = Int64GetDatumFast(tmp.jit_optimization_count);
			values[i++] = Float8GetDatumFast(tmp.jit_optimization_time);
			values[i++] = Int64GetDatumFast(tmp.jit_emission_count);
			values[i++] = Float8GetDatumFast(tmp.jit_emission_time);
		}
		if (api_version >= PGSS_V1_11)
		{
			values[i++] = Int64GetDatumFast(tmp.jit_deform_count);
			values[i++] = Float8GetDatumFast(tmp.jit_deform_time);
		}
		if (api_version >= PGSS_V1_12)
		{
			values[i++] = Int64GetDatumFast(tmp.parallel_workers_to_launch);
			values[i++] = Int64GetDatumFast(tmp.parallel_workers_launched);
		}
		if (api_version >= PGSS_V1_11)
		{
			values[i++] = TimestampTzGetDatum(stats_since);
			values[i++] = TimestampTzGetDatum(minmax_stats_since);
		}

		Assert(i == (api_version == PGSS_V1_0 ? PG_STAT_STATEMENTS_COLS_V1_0 :
					 api_version == PGSS_V1_1 ? PG_STAT_STATEMENTS_COLS_V1_1 :
					 api_version == PGSS_V1_2 ? PG_STAT_STATEMENTS_COLS_V1_2 :
					 api_version == PGSS_V1_3 ? PG_STAT_STATEMENTS_COLS_V1_3 :
					 api_version == PGSS_V1_8 ? PG_STAT_STATEMENTS_COLS_V1_8 :
					 api_version == PGSS_V1_9 ? PG_STAT_STATEMENTS_COLS_V1_9 :
					 api_version == PGSS_V1_10 ? PG_STAT_STATEMENTS_COLS_V1_10 :
					 api_version == PGSS_V1_11 ? PG_STAT_STATEMENTS_COLS_V1_11 :
					 api_version == PGSS_V1_12 ? PG_STAT_STATEMENTS_COLS_V1_12 :
					 -1 /* fail if you forget to update this assert */ ));

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	LWLockRelease(pgss->lock);

	free(qbuffer);
}

/* Number of output arguments (columns) for pg_stat_statements_info
 *
 * pg_stat_statements_info 的输出参数（列）数
 */
#define PG_STAT_STATEMENTS_INFO_COLS	2

/*
 * Return statistics of pg_stat_statements.
 *
 * 返回 pg_stat_statements 的统计信息。
 */
Datum
pg_stat_statements_info(PG_FUNCTION_ARGS)
{
	pgssGlobalStats stats;
	TupleDesc	tupdesc;
	Datum		values[PG_STAT_STATEMENTS_INFO_COLS] = {0};
	bool		nulls[PG_STAT_STATEMENTS_INFO_COLS] = {0};

	if (!pgss || !pgss_hash)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_stat_statements must be loaded via \"shared_preload_libraries\"")));

	/* Build a tuple descriptor for our result type
	 *
	 * 为我们的结果类型构建一个元组描述符
	 */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	/* Read global statistics for pg_stat_statements
	 *
	 * 读取 pg_stat_statements 的全局统计信息
	 */
	SpinLockAcquire(&pgss->mutex);
	stats = pgss->stats;
	SpinLockRelease(&pgss->mutex);

	values[0] = Int64GetDatum(stats.dealloc);
	values[1] = TimestampTzGetDatum(stats.stats_reset);

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/*
 * Estimate shared memory space needed.
 *
 * 估计所需的共享内存空间。
 */
static Size
pgss_memsize(void)
{
	Size		size;

	size = MAXALIGN(sizeof(pgssSharedState));
	size = add_size(size, hash_estimate_size(pgss_max, sizeof(pgssEntry)));

	return size;
}

/*
 * Allocate a new hashtable entry.
 * caller must hold an exclusive lock on pgss->lock
 *
 * 分配一个新的哈希表条目。调用者必须持有 pgss->lock 上的独占锁
 *
 * "query" need not be null-terminated; we rely on query_len instead
 *
 * “query”不需要以 null 结尾；我们依赖于 query_len
 *
 * If "sticky" is true, make the new entry artificially sticky so that it will
 * probably still be there when the query finishes execution.  We do this by
 * giving it a median usage value rather than the normal value.  (Strictly
 * speaking, query strings are normalized on a best effort basis, though it
 * would be difficult to demonstrate this even under artificial conditions.)
 *
 * 如果“sticky”为 true，则人为地使新条目具有粘性，以便在查询完成执行时它可能仍然存在。  我们通过给它一个中位使用值而不是正常值来做到这一点。  （严格来说，查询字符串是在尽最大努力的基础上进行规范化的，尽管即使在人为条件下也很难证明这一点。）
 *
 * Note: despite needing exclusive lock, it's not an error for the target
 * entry to already exist.  This is because pgss_store releases and
 * reacquires lock after failing to find a match; so someone else could
 * have made the entry while we waited to get exclusive lock.
 *
 * 注意：尽管需要独占锁，但目标条目已经存在并不是错误。  这是因为pgss_store在找不到匹配项后释放并重新获取锁；所以当我们等待获得独占锁时，其他人可能已经进入了。
 */
static pgssEntry *
entry_alloc(pgssHashKey *key, Size query_offset, int query_len, int encoding,
			bool sticky)
{
	pgssEntry  *entry;
	bool		found;

	/* Make space if needed
	 *
	 * 如果需要，请腾出空间
	 */
	while (hash_get_num_entries(pgss_hash) >= pgss_max)
		entry_dealloc();

	/* Find or create an entry with desired hash code
	 *
	 * 查找或创建具有所需哈希码的条目
	 */
	entry = (pgssEntry *) hash_search(pgss_hash, key, HASH_ENTER, &found);

	if (!found)
	{
		/* New entry, initialize it
		 *
		 * 新条目，初始化它
		 */

		/* reset the statistics
		 *
		 * 重置统计数据
		 */
		memset(&entry->counters, 0, sizeof(Counters));
		/* set the appropriate initial usage count
		 *
		 * 设置适当的初始使用计数
		 */
		entry->counters.usage = sticky ? pgss->cur_median_usage : USAGE_INIT;
		/* re-initialize the mutex each time ... we assume no one using it
		 *
		 * 每次都重新初始化互斥锁...我们假设没有人使用它
		 */
		SpinLockInit(&entry->mutex);
		/* ... and don't forget the query text metadata
		 *
		 * ...并且不要忘记查询文本元数据
		 */
		Assert(query_len >= 0);
		entry->query_offset = query_offset;
		entry->query_len = query_len;
		entry->encoding = encoding;
		entry->stats_since = GetCurrentTimestamp();
		entry->minmax_stats_since = entry->stats_since;
	}

	return entry;
}

/*
 * qsort comparator for sorting into increasing usage order
 *
 * qsort 比较器，用于按递增的使用顺序进行排序
 */
static int
entry_cmp(const void *lhs, const void *rhs)
{
	double		l_usage = (*(pgssEntry *const *) lhs)->counters.usage;
	double		r_usage = (*(pgssEntry *const *) rhs)->counters.usage;

	if (l_usage < r_usage)
		return -1;
	else if (l_usage > r_usage)
		return +1;
	else
		return 0;
}

/*
 * Deallocate least-used entries.
 *
 * 释放最少使用的条目。
 *
 * Caller must hold an exclusive lock on pgss->lock.
 *
 * 调用者必须持有 pgss->lock 上的独占锁。
 */
static void
entry_dealloc(void)
{
	HASH_SEQ_STATUS hash_seq;
	pgssEntry **entries;
	pgssEntry  *entry;
	int			nvictims;
	int			i;
	Size		tottextlen;
	int			nvalidtexts;

	/*
	 * Sort entries by usage and deallocate USAGE_DEALLOC_PERCENT of them.
	 * While we're scanning the table, apply the decay factor to the usage
	 * values, and update the mean query length.
	 *
	 * 按使用情况对条目进行排序，并释放其中的 USAGE_DEALLOC_PERCENT。当我们扫描表时，将衰减因子应用于使用值，并更新平均查询长度。
	 *
	 * Note that the mean query length is almost immediately obsolete, since
	 * we compute it before not after discarding the least-used entries.
	 * Hopefully, that doesn't affect the mean too much; it doesn't seem worth
	 * making two passes to get a more current result.  Likewise, the new
	 * cur_median_usage includes the entries we're about to zap.
	 *
	 * 请注意，平均查询长度几乎立即过时，因为我们是在丢弃最少使用的条目之前而不是之后计算它的。希望这不会对平均值产生太大影响；似乎不值得进行两次传递以获得更新的结果。  同样，新的 cur_median_usage 包括我们要删除的条目。
	 */

	entries = palloc(hash_get_num_entries(pgss_hash) * sizeof(pgssEntry *));

	i = 0;
	tottextlen = 0;
	nvalidtexts = 0;

	hash_seq_init(&hash_seq, pgss_hash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		entries[i++] = entry;
		/* "Sticky" entries get a different usage decay rate.
		 *
		 * “粘性”条目具有不同的使用衰减率。
		 */
		if (IS_STICKY(entry->counters))
			entry->counters.usage *= STICKY_DECREASE_FACTOR;
		else
			entry->counters.usage *= USAGE_DECREASE_FACTOR;
		/* In the mean length computation, ignore dropped texts.
		 *
		 * 在平均长度计算中，忽略丢弃的文本。
		 */
		if (entry->query_len >= 0)
		{
			tottextlen += entry->query_len + 1;
			nvalidtexts++;
		}
	}

	/* Sort into increasing order by usage
	 *
	 * 按用途升序排序
	 */
	qsort(entries, i, sizeof(pgssEntry *), entry_cmp);

	/* Record the (approximate) median usage
	 *
	 * 记录（大约）使用中位数
	 */
	if (i > 0)
		pgss->cur_median_usage = entries[i / 2]->counters.usage;
	/* Record the mean query length
	 *
	 * 记录平均查询长度
	 */
	if (nvalidtexts > 0)
		pgss->mean_query_len = tottextlen / nvalidtexts;
	else
		pgss->mean_query_len = ASSUMED_LENGTH_INIT;

	/* Now zap an appropriate fraction of lowest-usage entries
	 *
	 * 现在删除最低使用率条目的适当部分
	 */
	nvictims = Max(10, i * USAGE_DEALLOC_PERCENT / 100);
	nvictims = Min(nvictims, i);

	for (i = 0; i < nvictims; i++)
	{
		hash_search(pgss_hash, &entries[i]->key, HASH_REMOVE, NULL);
	}

	pfree(entries);

	/* Increment the number of times entries are deallocated
	 *
	 * 增加条目被释放的次数
	 */
	SpinLockAcquire(&pgss->mutex);
	pgss->stats.dealloc += 1;
	SpinLockRelease(&pgss->mutex);
}

/*
 * Given a query string (not necessarily null-terminated), allocate a new
 * entry in the external query text file and store the string there.
 *
 * 给定一个查询字符串（不一定以 null 结尾），在外部查询文本文件中分配一个新条目并将该字符串存储在那里。
 *
 * If successful, returns true, and stores the new entry's offset in the file
 * into *query_offset.  Also, if gc_count isn't NULL, *gc_count is set to the
 * number of garbage collections that have occurred so far.
 *
 * 如果成功，则返回 true，并将新条目在文件中的偏移量存储到 *query_offset 中。  另外，如果 gc_count 不为 NULL，则 *gc_count 将设置为迄今为止已发生的垃圾收集的数量。
 *
 * On failure, returns false.
 *
 * 失败时返回 false。
 *
 * At least a shared lock on pgss->lock must be held by the caller, so as
 * to prevent a concurrent garbage collection.  Share-lock-holding callers
 * should pass a gc_count pointer to obtain the number of garbage collections,
 * so that they can recheck the count after obtaining exclusive lock to
 * detect whether a garbage collection occurred (and removed this entry).
 *
 * 调用者至少必须持有 pgss->lock 上的共享锁，以防止并发垃圾收集。  持有共享锁的调用者应该传递一个 gc_count 指针来获取垃圾收集的次数，以便在获得排他锁后重新检查计数以检测是否发生了垃圾收集（并删除了该条目）。
 */
static bool
qtext_store(const char *query, int query_len,
			Size *query_offset, int *gc_count)
{
	Size		off;
	int			fd;

	/*
	 * We use a spinlock to protect extent/n_writers/gc_count, so that
	 * multiple processes may execute this function concurrently.
	 *
	 * 我们使用自旋锁来保护extent/n_writers/gc_count，以便多个进程可以并发执行该函数。
	 */
	SpinLockAcquire(&pgss->mutex);
	off = pgss->extent;
	pgss->extent += query_len + 1;
	pgss->n_writers++;
	if (gc_count)
		*gc_count = pgss->gc_count;
	SpinLockRelease(&pgss->mutex);

	*query_offset = off;

	/*
	 * Don't allow the file to grow larger than what qtext_load_file can
	 * (theoretically) handle.  This has been seen to be reachable on 32-bit
	 * platforms.
	 *
	 * 不允许文件增长到超过 qtext_load_file 可以（理论上）处理的大小。  这在 32 位平台上是可以实现的。
	 */
	if (unlikely(query_len >= MaxAllocHugeSize - off))
	{
		errno = EFBIG;			/* not quite right, but it'll do */
		fd = -1;
		goto error;
	}

	/* Now write the data into the successfully-reserved part of the file
	 *
	 * 现在将数据写入文件成功保留的部分
	 */
	fd = OpenTransientFile(PGSS_TEXT_FILE, O_RDWR | O_CREAT | PG_BINARY);
	if (fd < 0)
		goto error;

	if (pg_pwrite(fd, query, query_len, off) != query_len)
		goto error;
	if (pg_pwrite(fd, "\0", 1, off + query_len) != 1)
		goto error;

	CloseTransientFile(fd);

	/* Mark our write complete
	 *
	 * 将我们的写入标记为完成
	 */
	SpinLockAcquire(&pgss->mutex);
	pgss->n_writers--;
	SpinLockRelease(&pgss->mutex);

	return true;

error:
	ereport(LOG,
			(errcode_for_file_access(),
			 errmsg("could not write file \"%s\": %m",
					PGSS_TEXT_FILE)));

	if (fd >= 0)
		CloseTransientFile(fd);

	/* Mark our write complete
	 *
	 * 将我们的写入标记为完成
	 */
	SpinLockAcquire(&pgss->mutex);
	pgss->n_writers--;
	SpinLockRelease(&pgss->mutex);

	return false;
}

/*
 * Read the external query text file into a malloc'd buffer.
 *
 * 将外部查询文本文件读入 malloc 缓冲区。
 *
 * Returns NULL (without throwing an error) if unable to read, eg
 * file not there or insufficient memory.
 *
 * 如果无法读取，例如文件不存在或内存不足，则返回 NULL（不抛出错误）。
 *
 * On success, the buffer size is also returned into *buffer_size.
 *
 * 成功时，缓冲区大小也会返回到*buffer_size。
 *
 * This can be called without any lock on pgss->lock, but in that case
 * the caller is responsible for verifying that the result is sane.
 *
 * 可以在没有 pgss->lock 上任何锁定的情况下调用此函数，但在这种情况下，调用者负责验证结果是否正常。
 */
static char *
qtext_load_file(Size *buffer_size)
{
	char	   *buf;
	int			fd;
	struct stat stat;
	Size		nread;

	fd = OpenTransientFile(PGSS_TEXT_FILE, O_RDONLY | PG_BINARY);
	if (fd < 0)
	{
		if (errno != ENOENT)
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m",
							PGSS_TEXT_FILE)));
		return NULL;
	}

	/* Get file length
	 *
	 * 获取文件长度
	 */
	if (fstat(fd, &stat))
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m",
						PGSS_TEXT_FILE)));
		CloseTransientFile(fd);
		return NULL;
	}

	/* Allocate buffer; beware that off_t might be wider than size_t
	 *
	 * 分配缓冲区；请注意 off_t 可能比 size_t 宽
	 */
	if (stat.st_size <= MaxAllocHugeSize)
		buf = (char *) malloc(stat.st_size);
	else
		buf = NULL;
	if (buf == NULL)
	{
		ereport(LOG,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Could not allocate enough memory to read file \"%s\".",
						   PGSS_TEXT_FILE)));
		CloseTransientFile(fd);
		return NULL;
	}

	/*
	 * OK, slurp in the file.  Windows fails if we try to read more than
	 * INT_MAX bytes at once, and other platforms might not like that either,
	 * so read a very large file in 1GB segments.
	 *
	 * 好的，把文件放进去。  如果我们尝试一次读取超过 INT_MAX 字节，Windows 就会失败，其他平台也可能不喜欢这样，因此以 1GB 段读取非常大的文件。
	 */
	nread = 0;
	while (nread < stat.st_size)
	{
		int			toread = Min(1024 * 1024 * 1024, stat.st_size - nread);

		/*
		 * If we get a short read and errno doesn't get set, the reason is
		 * probably that garbage collection truncated the file since we did
		 * the fstat(), so we don't log a complaint --- but we don't return
		 * the data, either, since it's most likely corrupt due to concurrent
		 * writes from garbage collection.
		 *
		 * 如果我们得到一个简短的读取并且 errno 没有设置，原因可能是垃圾收集在我们执行 fstat() 后截断了文件，所以我们不记录投诉 --- 但我们也不返回数据，因为它很可能由于垃圾收集的并发写入而损坏。
		 */
		errno = 0;
		if (read(fd, buf + nread, toread) != toread)
		{
			if (errno)
				ereport(LOG,
						(errcode_for_file_access(),
						 errmsg("could not read file \"%s\": %m",
								PGSS_TEXT_FILE)));
			free(buf);
			CloseTransientFile(fd);
			return NULL;
		}
		nread += toread;
	}

	if (CloseTransientFile(fd) != 0)
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", PGSS_TEXT_FILE)));

	*buffer_size = nread;
	return buf;
}

/*
 * Locate a query text in the file image previously read by qtext_load_file().
 *
 * 在先前由 qtext_load_file() 读取的文件映像中找到查询文本。
 *
 * We validate the given offset/length, and return NULL if bogus.  Otherwise,
 * the result points to a null-terminated string within the buffer.
 *
 * 我们验证给定的偏移量/长度，如果是假的，则返回 NULL。  否则，结果指向缓冲区内的空终止字符串。
 */
static char *
qtext_fetch(Size query_offset, int query_len,
			char *buffer, Size buffer_size)
{
	/* File read failed?
	 *
	 * 文件读取失败？
	 */
	if (buffer == NULL)
		return NULL;
	/* Bogus offset/length?
	 *
	 * 伪造的偏移量/长度？
	 */
	if (query_len < 0 ||
		query_offset + query_len >= buffer_size)
		return NULL;
	/* As a further sanity check, make sure there's a trailing null
	 *
	 * 作为进一步的健全性检查，请确保有一个尾随空值
	 */
	if (buffer[query_offset + query_len] != '\0')
		return NULL;
	/* Looks OK
	 *
	 * 看起来不错
	 */
	return buffer + query_offset;
}

/*
 * Do we need to garbage-collect the external query text file?
 *
 * 我们是否需要对外部查询文本文件进行垃圾收集？
 *
 * Caller should hold at least a shared lock on pgss->lock.
 *
 * 调用者应该至少在 pgss->lock 上持有一个共享锁。
 */
static bool
need_gc_qtexts(void)
{
	Size		extent;

	/* Read shared extent pointer
	 *
	 * 读取共享盘区指针
	 */
	SpinLockAcquire(&pgss->mutex);
	extent = pgss->extent;
	SpinLockRelease(&pgss->mutex);

	/*
	 * Don't proceed if file does not exceed 512 bytes per possible entry.
	 *
	 * 如果文件每个可能条目不超过 512 字节，则不要继续。
	 *
	 * Here and in the next test, 32-bit machines have overflow hazards if
	 * pgss_max and/or mean_query_len are large.  Force the multiplications
	 * and comparisons to be done in uint64 arithmetic to forestall trouble.
	 *
	 * 在这里和下一个测试中，如果 pgss_max 和/或 Mean_query_len 很大，32 位机器就会存在溢出危险。  强制在 uint64 算术中完成乘法和比较，以防止出现问题。
	 */
	if ((uint64) extent < (uint64) 512 * pgss_max)
		return false;

	/*
	 * Don't proceed if file is less than about 50% bloat.  Nothing can or
	 * should be done in the event of unusually large query texts accounting
	 * for file's large size.  We go to the trouble of maintaining the mean
	 * query length in order to prevent garbage collection from thrashing
	 * uselessly.
	 *
	 * 如果文件膨胀程度低于 50%，请不要继续。  如果文件的大小异常大，则不能也不应该采取任何措施。  我们不厌其烦地维护平均查询长度，以防止垃圾收集无用地崩溃。
	 */
	if ((uint64) extent < (uint64) pgss->mean_query_len * pgss_max * 2)
		return false;

	return true;
}

/*
 * Garbage-collect orphaned query texts in external file.
 *
 * 垃圾收集外部文件中的孤立查询文本。
 *
 * This won't be called often in the typical case, since it's likely that
 * there won't be too much churn, and besides, a similar compaction process
 * occurs when serializing to disk at shutdown or as part of resetting.
 * Despite this, it seems prudent to plan for the edge case where the file
 * becomes unreasonably large, with no other method of compaction likely to
 * occur in the foreseeable future.
 *
 * 在典型情况下，这不会经常被调用，因为很可能不会有太多的改动，此外，在关闭时序列化到磁盘或作为重置的一部分时，会发生类似的压缩过程。尽管如此，为文件变得不合理的大的边缘情况做好计划似乎是谨慎的，在可预见的将来不可能出现其他压缩方法。
 *
 * The caller must hold an exclusive lock on pgss->lock.
 *
 * 调用者必须持有 pgss->lock 上的独占锁。
 *
 * At the first sign of trouble we unlink the query text file to get a clean
 * slate (although existing statistics are retained), rather than risk
 * thrashing by allowing the same problem case to recur indefinitely.
 *
 * 一旦出现问题，我们就会取消查询文本文件的链接以重新开始（尽管现有的统计信息将被保留），而不是冒着让相同问题案例无限期重复出现而导致崩溃的风险。
 */
static void
gc_qtexts(void)
{
	char	   *qbuffer;
	Size		qbuffer_size;
	FILE	   *qfile = NULL;
	HASH_SEQ_STATUS hash_seq;
	pgssEntry  *entry;
	Size		extent;
	int			nentries;

	/*
	 * When called from pgss_store, some other session might have proceeded
	 * with garbage collection in the no-lock-held interim of lock strength
	 * escalation.  Check once more that this is actually necessary.
	 *
	 * 当从 pgss_store 调用时，其他一些会话可能在锁强度升级的无锁持有期间继续进行垃圾收集。  再次检查这是否确实必要。
	 */
	if (!need_gc_qtexts())
		return;

	/*
	 * Load the old texts file.  If we fail (out of memory, for instance),
	 * invalidate query texts.  Hopefully this is rare.  It might seem better
	 * to leave things alone on an OOM failure, but the problem is that the
	 * file is only going to get bigger; hoping for a future non-OOM result is
	 * risky and can easily lead to complete denial of service.
	 *
	 * 加载旧文本文件。  如果我们失败（例如内存不足），则查询文本无效。  希望这种情况很少见。  在发生 OOM 故障时，最好不要理会事情，但问题是文件只会变得更大；希望未来出现非 OOM 结果是有风险的，并且很容易导致完全拒绝服务。
	 */
	qbuffer = qtext_load_file(&qbuffer_size);
	if (qbuffer == NULL)
		goto gc_fail;

	/*
	 * We overwrite the query texts file in place, so as to reduce the risk of
	 * an out-of-disk-space failure.  Since the file is guaranteed not to get
	 * larger, this should always work on traditional filesystems; though we
	 * could still lose on copy-on-write filesystems.
	 *
	 * 我们就地覆盖查询文本文件，以降低磁盘空间不足故障的风险。  由于保证文件不会变大，因此这应该始终适用于传统文件系统；尽管我们仍然可能在写时复制文件系统上失败。
	 */
	qfile = AllocateFile(PGSS_TEXT_FILE, PG_BINARY_W);
	if (qfile == NULL)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m",
						PGSS_TEXT_FILE)));
		goto gc_fail;
	}

	extent = 0;
	nentries = 0;

	hash_seq_init(&hash_seq, pgss_hash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		int			query_len = entry->query_len;
		char	   *qry = qtext_fetch(entry->query_offset,
									  query_len,
									  qbuffer,
									  qbuffer_size);

		if (qry == NULL)
		{
			/* Trouble ... drop the text
			 *
			 * 麻烦...丢掉文字
			 */
			entry->query_offset = 0;
			entry->query_len = -1;
			/* entry will not be counted in mean query length computation
			 *
			 * 条目将不计入平均查询长度计算中
			 */
			continue;
		}

		if (fwrite(qry, 1, query_len + 1, qfile) != query_len + 1)
		{
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not write file \"%s\": %m",
							PGSS_TEXT_FILE)));
			hash_seq_term(&hash_seq);
			goto gc_fail;
		}

		entry->query_offset = extent;
		extent += query_len + 1;
		nentries++;
	}

	/*
	 * Truncate away any now-unused space.  If this fails for some odd reason,
	 * we log it, but there's no need to fail.
	 *
	 * 截断任何现在未使用的空间。  如果由于某种奇怪的原因失败，我们会记录它，但没有必要失败。
	 */
	if (ftruncate(fileno(qfile), extent) != 0)
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not truncate file \"%s\": %m",
						PGSS_TEXT_FILE)));

	if (FreeFile(qfile))
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m",
						PGSS_TEXT_FILE)));
		qfile = NULL;
		goto gc_fail;
	}

	elog(DEBUG1, "pgss gc of queries file shrunk size from %zu to %zu",
		 pgss->extent, extent);

	/* Reset the shared extent pointer
	 *
	 * 重置共享盘区指针
	 */
	pgss->extent = extent;

	/*
	 * Also update the mean query length, to be sure that need_gc_qtexts()
	 * won't still think we have a problem.
	 *
	 * 还要更新平均查询长度，以确保 need_gc_qtexts() 不会仍然认为我们有问题。
	 */
	if (nentries > 0)
		pgss->mean_query_len = extent / nentries;
	else
		pgss->mean_query_len = ASSUMED_LENGTH_INIT;

	free(qbuffer);

	/*
	 * OK, count a garbage collection cycle.  (Note: even though we have
	 * exclusive lock on pgss->lock, we must take pgss->mutex for this, since
	 * other processes may examine gc_count while holding only the mutex.
	 * Also, we have to advance the count *after* we've rewritten the file,
	 * else other processes might not realize they read a stale file.)
	 *
	 * OK，统计一个垃圾回收周期。  （注意：即使我们在 pgss->lock 上有独占锁，我们也必须为此使用 pgss->mutex，因为其他进程可能会在仅持有互斥锁的情况下检查 gc_count。此外，我们必须在重写文件后*提前*计数，否则其他进程可能不会意识到它们读取了过时的文件。）
	 */
	record_gc_qtexts();

	return;

gc_fail:
	/* clean up resources
	 *
	 * 清理资源
	 */
	if (qfile)
		FreeFile(qfile);
	free(qbuffer);

	/*
	 * Since the contents of the external file are now uncertain, mark all
	 * hashtable entries as having invalid texts.
	 *
	 * 由于外部文件的内容现在不确定，因此将所有哈希表条目标记为具有无效文本。
	 */
	hash_seq_init(&hash_seq, pgss_hash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		entry->query_offset = 0;
		entry->query_len = -1;
	}

	/*
	 * Destroy the query text file and create a new, empty one
	 *
	 * 销毁查询文本文件并创建一个新的空文件
	 */
	(void) unlink(PGSS_TEXT_FILE);
	qfile = AllocateFile(PGSS_TEXT_FILE, PG_BINARY_W);
	if (qfile == NULL)
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not recreate file \"%s\": %m",
						PGSS_TEXT_FILE)));
	else
		FreeFile(qfile);

	/* Reset the shared extent pointer
	 *
	 * 重置共享盘区指针
	 */
	pgss->extent = 0;

	/* Reset mean_query_len to match the new state
	 *
	 * 重置mean_query_len以匹配新状态
	 */
	pgss->mean_query_len = ASSUMED_LENGTH_INIT;

	/*
	 * Bump the GC count even though we failed.
	 *
	 * 即使我们失败了，GC 计数也会增加。
	 *
	 * This is needed to make concurrent readers of file without any lock on
	 * pgss->lock notice existence of new version of file.  Once readers
	 * subsequently observe a change in GC count with pgss->lock held, that
	 * forces a safe reopen of file.  Writers also require that we bump here,
	 * of course.  (As required by locking protocol, readers and writers don't
	 * trust earlier file contents until gc_count is found unchanged after
	 * pgss->lock acquired in shared or exclusive mode respectively.)
	 *
	 * 这是为了使文件的并发读取器在 pgss->lock 上没有任何锁定的情况下注意到新版本文件的存在。  一旦读者随后观察到 pgss->lock 持有的 GC 计数发生变化，就会强制安全地重新打开文件。  当然，作家也要求我们在这里碰撞。  （根据锁定协议的要求，读取器和写入器不信任较早的文件内容，直到分别以共享或独占模式获取 pgss->lock 后发现 gc_count 未更改。）
	 */
	record_gc_qtexts();
}

#define SINGLE_ENTRY_RESET(e) \
if (e) { \
	if (minmax_only) { \
		/* When requested reset only min/max statistics of an entry */ \
		for (int kind = 0; kind < PGSS_NUMKIND; kind++) \
		{ \
			e->counters.max_time[kind] = 0; \
			e->counters.min_time[kind] = 0; \
		} \
		e->minmax_stats_since = stats_reset; \
	} \
	else \
	{ \
		/* Remove the key otherwise  */ \
		hash_search(pgss_hash, &e->key, HASH_REMOVE, NULL); \
		num_remove++; \
	} \
}

/*
 * Reset entries corresponding to parameters passed.
 *
 * 重置与传递的参数相对应的条目。
 */
static TimestampTz
entry_reset(Oid userid, Oid dbid, int64 queryid, bool minmax_only)
{
	HASH_SEQ_STATUS hash_seq;
	pgssEntry  *entry;
	FILE	   *qfile;
	long		num_entries;
	long		num_remove = 0;
	pgssHashKey key;
	TimestampTz stats_reset;

	if (!pgss || !pgss_hash)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_stat_statements must be loaded via \"shared_preload_libraries\"")));

	LWLockAcquire(pgss->lock, LW_EXCLUSIVE);
	num_entries = hash_get_num_entries(pgss_hash);

	stats_reset = GetCurrentTimestamp();

	if (userid != 0 && dbid != 0 && queryid != INT64CONST(0))
	{
		/* If all the parameters are available, use the fast path.
		 *
		 * 如果所有参数都可用，则使用快速路径。
		 */
		memset(&key, 0, sizeof(pgssHashKey));
		key.userid = userid;
		key.dbid = dbid;
		key.queryid = queryid;

		/*
		 * Reset the entry if it exists, starting with the non-top-level
		 * entry.
		 *
		 * 重置该条目（如果存在），从非顶级条目开始。
		 */
		key.toplevel = false;
		entry = (pgssEntry *) hash_search(pgss_hash, &key, HASH_FIND, NULL);

		SINGLE_ENTRY_RESET(entry);

		/* Also reset the top-level entry if it exists.
		 *
		 * 如果存在的话，还要重置顶级条目。
		 */
		key.toplevel = true;
		entry = (pgssEntry *) hash_search(pgss_hash, &key, HASH_FIND, NULL);

		SINGLE_ENTRY_RESET(entry);
	}
	else if (userid != 0 || dbid != 0 || queryid != INT64CONST(0))
	{
		/* Reset entries corresponding to valid parameters.
		 *
		 * 重置与有效参数对应的条目。
		 */
		hash_seq_init(&hash_seq, pgss_hash);
		while ((entry = hash_seq_search(&hash_seq)) != NULL)
		{
			if ((!userid || entry->key.userid == userid) &&
				(!dbid || entry->key.dbid == dbid) &&
				(!queryid || entry->key.queryid == queryid))
			{
				SINGLE_ENTRY_RESET(entry);
			}
		}
	}
	else
	{
		/* Reset all entries.
		 *
		 * 重置所有条目。
		 */
		hash_seq_init(&hash_seq, pgss_hash);
		while ((entry = hash_seq_search(&hash_seq)) != NULL)
		{
			SINGLE_ENTRY_RESET(entry);
		}
	}

	/* All entries are removed?
	 *
	 * 所有条目都被删除？
	 */
	if (num_entries != num_remove)
		goto release_lock;

	/*
	 * Reset global statistics for pg_stat_statements since all entries are
	 * removed.
	 *
	 * 由于所有条目都已删除，因此重置 pg_stat_statements 的全局统计信息。
	 */
	SpinLockAcquire(&pgss->mutex);
	pgss->stats.dealloc = 0;
	pgss->stats.stats_reset = stats_reset;
	SpinLockRelease(&pgss->mutex);

	/*
	 * Write new empty query file, perhaps even creating a new one to recover
	 * if the file was missing.
	 *
	 * 写入新的空查询文件，甚至可能创建一个新文件以在文件丢失时进行恢复。
	 */
	qfile = AllocateFile(PGSS_TEXT_FILE, PG_BINARY_W);
	if (qfile == NULL)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m",
						PGSS_TEXT_FILE)));
		goto done;
	}

	/* If ftruncate fails, log it, but it's not a fatal problem
	 *
	 * 如果 ftruncate 失败，记录它，但这不是致命问题
	 */
	if (ftruncate(fileno(qfile), 0) != 0)
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not truncate file \"%s\": %m",
						PGSS_TEXT_FILE)));

	FreeFile(qfile);

done:
	pgss->extent = 0;
	/* This counts as a query text garbage collection for our purposes
	 *
	 * 出于我们的目的，这算作查询文本垃圾收集
	 */
	record_gc_qtexts();

release_lock:
	LWLockRelease(pgss->lock);

	return stats_reset;
}

/*
 * Generate a normalized version of the query string that will be used to
 * represent all similar queries.
 *
 * 生成查询字符串的规范化版本，用于表示所有类似的查询。
 *
 * Note that the normalized representation may well vary depending on
 * just which "equivalent" query is used to create the hashtable entry.
 * We assume this is OK.
 *
 * 请注意，规范化表示可能会根据用于创建哈希表条目的“等效”查询而有所不同。我们假设这没问题。
 *
 * If query_loc > 0, then "query" has been advanced by that much compared to
 * the original string start, so we need to translate the provided locations
 * to compensate.  (This lets us avoid re-scanning statements before the one
 * of interest, so it's worth doing.)
 *
 * 如果 query_loc > 0，则“query”与原始字符串开头相比已提前了那么多，因此我们需要翻译提供的位置以进行补偿。  （这可以让我们避免在感兴趣的语句之前重新扫描语句，因此值得这样做。）
 *
 * *query_len_p contains the input string length, and is updated with
 * the result string length on exit.  The resulting string might be longer
 * or shorter depending on what happens with replacement of constants.
 *
 * *query_len_p 包含输入字符串长度，并在退出时使用结果字符串长度进行更新。  生成的字符串可能更长或更短，具体取决于替换常量时发生的情况。
 *
 * Returns a palloc'd string.
 *
 * 返回一个 palloc 的字符串。
 */
static char *
generate_normalized_query(JumbleState *jstate, const char *query,
						  int query_loc, int *query_len_p)
{
	char	   *norm_query;
	int			query_len = *query_len_p;
	int			norm_query_buflen,	/* Space allowed for norm_query */
				len_to_wrt,		/* Length (in bytes) to write */
				quer_loc = 0,	/* Source query byte location */
				n_quer_loc = 0, /* Normalized query byte location */
				last_off = 0,	/* Offset from start for previous tok */
				last_tok_len = 0;	/* Length (in bytes) of that tok */
	int			num_constants_replaced = 0;

	/*
	 * Get constants' lengths (core system only gives us locations).  Note
	 * this also ensures the items are sorted by location.
	 *
	 * 获取常量的长度（核心系统只给我们位置）。  请注意，这还可以确保项目按位置排序。
	 */
	fill_in_constant_lengths(jstate, query, query_loc);

	/*
	 * Allow for $n symbols to be longer than the constants they replace.
	 * Constants must take at least one byte in text form, while a $n symbol
	 * certainly isn't more than 11 bytes, even if n reaches INT_MAX.  We
	 * could refine that limit based on the max value of n for the current
	 * query, but it hardly seems worth any extra effort to do so.
	 *
	 * 允许 $n 符号比它们替换的常量长。常量必须至少占用文本形式的 1 个字节，而 $n 符号当然不会超过 11 个字节，即使 n 达到 INT_MAX。  我们可以根据当前查询的 n 的最大值来细化该限制，但似乎不值得为此付出任何额外的努力。
	 */
	norm_query_buflen = query_len + jstate->clocations_count * 10;

	/* Allocate result buffer
	 *
	 * 分配结果缓冲区
	 */
	norm_query = palloc(norm_query_buflen + 1);

	for (int i = 0; i < jstate->clocations_count; i++)
	{
		int			off,		/* Offset from start for cur tok */
					tok_len;	/* Length (in bytes) of that tok */

		/*
		 * If we have an external param at this location, but no lists are
		 * being squashed across the query, then we skip here; this will make
		 * us print the characters found in the original query that represent
		 * the parameter in the next iteration (or after the loop is done),
		 * which is a bit odd but seems to work okay in most cases.
		 *
		 * 如果我们在这个位置有一个外部参数，但没有列表在查询中被压缩，那么我们跳过这里；这将使我们打印在原始查询中找到的代表下一次迭代（或循环完成后）参数的字符，这有点奇怪，但在大多数情况下似乎工作正常。
		 */
		if (jstate->clocations[i].extern_param && !jstate->has_squashed_lists)
			continue;

		off = jstate->clocations[i].location;

		/* Adjust recorded location if we're dealing with partial string
		 *
		 * 如果我们处理部分字符串，请调整记录位置
		 */
		off -= query_loc;

		tok_len = jstate->clocations[i].length;

		if (tok_len < 0)
			continue;			/* ignore any duplicates */

		/* Copy next chunk (what precedes the next constant)
		 *
		 * 复制下一个块（下一个常量之前的内容）
		 */
		len_to_wrt = off - last_off;
		len_to_wrt -= last_tok_len;
		Assert(len_to_wrt >= 0);
		memcpy(norm_query + n_quer_loc, query + quer_loc, len_to_wrt);
		n_quer_loc += len_to_wrt;

		/*
		 * And insert a param symbol in place of the constant token; and, if
		 * we have a squashable list, insert a placeholder comment starting
		 * from the list's second value.
		 *
		 * 并插入一个 param 符号来代替常量标记；并且，如果我们有一个可压缩列表，请从列表的第二个值开始插入占位符注释。
		 */
		n_quer_loc += sprintf(norm_query + n_quer_loc, "$%d%s",
							  num_constants_replaced + 1 + jstate->highest_extern_param_id,
							  jstate->clocations[i].squashed ? " /*, ... */" : "");
		num_constants_replaced++;

		/* move forward
		 *
		 * 前进
		 */
		quer_loc = off + tok_len;
		last_off = off;
		last_tok_len = tok_len;
	}

	/*
	 * We've copied up until the last ignorable constant.  Copy over the
	 * remaining bytes of the original query string.
	 *
	 * 我们已经复制到最后一个可忽略的常量。  复制原始查询字符串的剩余字节。
	 */
	len_to_wrt = query_len - quer_loc;

	Assert(len_to_wrt >= 0);
	memcpy(norm_query + n_quer_loc, query + quer_loc, len_to_wrt);
	n_quer_loc += len_to_wrt;

	Assert(n_quer_loc <= norm_query_buflen);
	norm_query[n_quer_loc] = '\0';

	*query_len_p = n_quer_loc;
	return norm_query;
}

/*
 * Given a valid SQL string and an array of constant-location records,
 * fill in the textual lengths of those constants.
 *
 * 给定一个有效的 SQL 字符串和一个常量位置记录数组，填写这些常量的文本长度。
 *
 * The constants may use any allowed constant syntax, such as float literals,
 * bit-strings, single-quoted strings and dollar-quoted strings.  This is
 * accomplished by using the public API for the core scanner.
 *
 * 常量可以使用任何允许的常量语法，例如浮点文字、位字符串、单引号字符串和美元引号字符串。  这是通过使用核心扫描器的公共 API 来完成的。
 *
 * It is the caller's job to ensure that the string is a valid SQL statement
 * with constants at the indicated locations.  Since in practice the string
 * has already been parsed, and the locations that the caller provides will
 * have originated from within the authoritative parser, this should not be
 * a problem.
 *
 * 调用者的工作是确保该字符串是有效的 SQL 语句，并且在指定位置具有常量。  由于实际上字符串已经被解析，并且调用者提供的位置将源自权威解析器内，因此这应该不是问题。
 *
 * Multiple constants can have the same location.  We reset lengths of those
 * past the first to -1 so that they can later be ignored.
 *
 * 多个常量可以具有相同的位置。  我们将第一个之后的长度重置为 -1，以便以后可以忽略它们。
 *
 * If query_loc > 0, then "query" has been advanced by that much compared to
 * the original string start, so we need to translate the provided locations
 * to compensate.  (This lets us avoid re-scanning statements before the one
 * of interest, so it's worth doing.)
 *
 * 如果 query_loc > 0，则“query”与原始字符串开头相比已提前了那么多，因此我们需要翻译提供的位置以进行补偿。  （这可以让我们避免在感兴趣的语句之前重新扫描语句，因此值得这样做。）
 *
 * N.B. There is an assumption that a '-' character at a Const location begins
 * a negative numeric constant.  This precludes there ever being another
 * reason for a constant to start with a '-'.
 *
 * 注意：假设 Const 位置处的“-”字符开始一个负数值常量。  这排除了常量以“-”开头的另一个原因。
 */
static void
fill_in_constant_lengths(JumbleState *jstate, const char *query,
						 int query_loc)
{
	LocationLen *locs;
	core_yyscan_t yyscanner;
	core_yy_extra_type yyextra;
	core_YYSTYPE yylval;
	YYLTYPE		yylloc;

	/*
	 * Sort the records by location so that we can process them in order while
	 * scanning the query text.
	 *
	 * 按位置对记录进行排序，以便我们在扫描查询文本时可以按顺序处理它们。
	 */
	if (jstate->clocations_count > 1)
		qsort(jstate->clocations, jstate->clocations_count,
			  sizeof(LocationLen), comp_location);
	locs = jstate->clocations;

	/* initialize the flex scanner --- should match raw_parser()
	 *
	 * 初始化 Flex 扫描仪 --- 应该匹配 raw_parser()
	 */
	yyscanner = scanner_init(query,
							 &yyextra,
							 &ScanKeywords,
							 ScanKeywordTokens);

	/* we don't want to re-emit any escape string warnings
	 *
	 * 我们不想重新发出任何转义字符串警告
	 */
	yyextra.escape_string_warning = false;

	/* Search for each constant, in sequence
	 *
	 * 按顺序搜索每个常量
	 */
	for (int i = 0; i < jstate->clocations_count; i++)
	{
		int			loc;
		int			tok;

		/* Ignore constants after the first one in the same location
		 *
		 * 忽略同一位置第一个之后的常量
		 */
		if (i > 0 && locs[i].location == locs[i - 1].location)
		{
			locs[i].length = -1;
			continue;
		}

		if (locs[i].squashed)
			continue;			/* squashable list, ignore */

		/* Adjust recorded location if we're dealing with partial string
		 *
		 * 如果我们处理部分字符串，请调整记录位置
		 */
		loc = locs[i].location - query_loc;
		Assert(loc >= 0);

		/*
		 * We have a valid location for a constant that's not a dupe. Lex
		 * tokens until we find the desired constant.
		 *
		 * 我们有一个有效的常量位置，该常量不是骗人的。 Lex 标记直到我们找到所需的常量。
		 */
		for (;;)
		{
			tok = core_yylex(&yylval, &yylloc, yyscanner);

			/* We should not hit end-of-string, but if we do, behave sanely
			 *
			 * 我们不应该击中字符串结尾，但如果这样做，请表现得理智
			 */
			if (tok == 0)
				break;			/* out of inner for-loop */

			/*
			 * We should find the token position exactly, but if we somehow
			 * run past it, work with that.
			 *
			 * 我们应该准确地找到令牌位置，但如果我们以某种方式跑过了它，请处理它。
			 */
			if (yylloc >= loc)
			{
				if (query[loc] == '-')
				{
					/*
					 * It's a negative value - this is the one and only case
					 * where we replace more than a single token.
					 *
					 * 这是一个负值 - 这是我们替换多个令牌的唯一情况。
					 *
					 * Do not compensate for the core system's special-case
					 * adjustment of location to that of the leading '-'
					 * operator in the event of a negative constant.  It is
					 * also useful for our purposes to start from the minus
					 * symbol.  In this way, queries like "select * from foo
					 * where bar = 1" and "select * from foo where bar = -2"
					 * will have identical normalized query strings.
					 *
					 * 如果出现负常数，请勿将核心系统的位置特殊情况调整补偿为前导“-”运算符的位置。  从减号开始对于我们的目的也很有用。  这样，像“select * from foo where bar = 1”和“select * from foo where bar = -2”这样的查询将具有相同的规范化查询字符串。
					 */
					tok = core_yylex(&yylval, &yylloc, yyscanner);
					if (tok == 0)
						break;	/* out of inner for-loop */
				}

				/*
				 * We now rely on the assumption that flex has placed a zero
				 * byte after the text of the current token in scanbuf.
				 *
				 * 我们现在依赖于这样的假设：flex 在 scanbuf 中当前标记的文本后面放置了一个零字节。
				 */
				locs[i].length = strlen(yyextra.scanbuf + loc);
				break;			/* out of inner for-loop */
			}
		}

		/* If we hit end-of-string, give up, leaving remaining lengths -1
		 *
		 * 如果我们到达字符串末尾，则放弃，剩余长度为 -1
		 */
		if (tok == 0)
			break;
	}

	scanner_finish(yyscanner);
}

/*
 * comp_location: comparator for qsorting LocationLen structs by location
 *
 * comp_location：按位置对 LocationLen 结构进行排序的比较器
 */
static int
comp_location(const void *a, const void *b)
{
	int			l = ((const LocationLen *) a)->location;
	int			r = ((const LocationLen *) b)->location;

	return pg_cmp_s32(l, r);
}

/*-------------------------------------------------------------------------
 *
 * autovacuum.c
 *
 * PostgreSQL Integrated Autovacuum Daemon
 *
 * PostgreSQL 集成自动清理守护进程
 *
 * The autovacuum system is structured in two different kinds of processes: the
 * autovacuum launcher and the autovacuum worker.  The launcher is an
 * always-running process, started by the postmaster when the autovacuum GUC
 * parameter is set.  The launcher schedules autovacuum workers to be started
 * when appropriate.  The workers are the processes which execute the actual
 * vacuuming; they connect to a database as determined in the launcher, and
 * once connected they examine the catalogs to select the tables to vacuum.
 *
 * 自动清理系统由两类不同的进程组成：自动清理启动器 (launcher) 和自动清理工作者 (worker)。
 * 启动器是一个常驻进程，当设置了 autovacuum GUC 参数时由 postmaster 启动。
 * 启动器负责调度并在适当时机启动自动清理工作者。
 * 工作者是执行实际清理工作的进程；它们连接到由启动器确定的数据库，
 * 连接后会检查目录以选择需要清理的表。
 *
 * The autovacuum launcher cannot start the worker processes by itself,
 * because doing so would cause robustness issues (namely, failure to shut
 * them down on exceptional conditions, and also, since the launcher is
 * connected to shared memory and is thus subject to corruption there, it is
 * not as robust as the postmaster).  So it leaves that task to the postmaster.
 *
 * 自动清理启动器不能自行启动工作者进程，因为这样做会导致健壮性问题
 *（即在异常情况下无法关闭它们，此外，由于启动器连接到共享内存，
 * 容易受到内存损坏的影响，因此其健壮性不如 postmaster）。
 * 所以它将该任务留给 postmaster。
 *
 * There is an autovacuum shared memory area, where the launcher stores
 * information about the database it wants vacuumed.  When it wants a new
 * worker to start, it sets a flag in shared memory and sends a signal to the
 * postmaster.  Then postmaster knows nothing more than it must start a worker;
 * so it forks a new child, which turns into a worker.  This new process
 * connects to shared memory, and there it can inspect the information that the
 * launcher has set up.
 *
 * 有一个自动清理共享内存区域，启动器在此存储它想要清理的数据库信息。
 * 当它想要启动一个新的工作者时，它会在共享内存中设置一个标志并向 postmaster 发送信号。
 * 然后 postmaster 只知道它必须启动一个工作者；所以它 fork 一个新的子进程，
 * 该子进程变身为工作者。这个新进程连接到共享内存，并可以检查启动器设置的信息。
 *
 * If the fork() call fails in the postmaster, it sets a flag in the shared
 * memory area, and sends a signal to the launcher.  The launcher, upon
 * noticing the flag, can try starting the worker again by resending the
 * signal.  Note that the failure can only be transient (fork failure due to
 * high load, memory pressure, too many processes, etc); more permanent
 * problems, like failure to connect to a database, are detected later in the
 * worker and dealt with just by having the worker exit normally.  The launcher
 * will launch a new worker again later, per schedule.
 *
 * 如果 postmaster 中的 fork() 调用失败，它会在共享内存区域设置一个标志，并向启动器发送信号。
 * 启动器在注意到该标志后，可以尝试通过重新发送信号来再次启动工作者。
 * 请注意，这种失败只能是瞬态的（由于高负载、内存压力、进程过多等导致的 fork 失败）；
 * 更持久的问题（如无法连接到数据库）稍后会在工作者中检测到，并仅通过让工作者正常退出来处理。
 * 启动器稍后会根据进度表再次启动一个新的工作者。
 *
 * When the worker is done vacuuming it sends SIGUSR2 to the launcher.  The
 * launcher then wakes up and is able to launch another worker, if the schedule
 * is so tight that a new worker is needed immediately.  At this time the
 * launcher can also balance the settings for the various remaining workers'
 * cost-based vacuum delay feature.
 *
 * 当工作者完成清理后，它会向启动器发送 SIGUSR2。启动器随后被唤醒，
 * 如果进度非常紧凑，需要立即启动另一个工作者，则它可以再次启动。
 * 此时，启动器还可以平衡各个剩余工作者的基于代价的清理延迟 (cost-based vacuum delay) 设置。
 *
 * Note that there can be more than one worker in a database concurrently.
 * They will store the table they are currently vacuuming in shared memory, so
 * that other workers avoid being blocked waiting for the vacuum lock for that
 * table.  They will also fetch the last time the table was vacuumed from
 * pgstats just before vacuuming each table, to avoid vacuuming a table that
 * was just finished being vacuumed by another worker and thus is no longer
 * noted in shared memory.  However, there is a small window (due to not yet
 * holding the relation lock) during which a worker may choose a table that was
 * already vacuumed; this is a bug in the current design.
 *
 * 请注意，一个数据库中可以同时有多个工作者。它们会将当前正在清理的表存储在共享内存中，
 * 以便其他工作者避免因等待该表的清理锁而被阻塞。它们还会在清理每个表之前从
 * pgstats 获取该表上次清理的时间，以避免清理一个刚刚由另一个工作者完成清理、
 * 且因此不再记于共享内存中的表。然而，存在一个小的时间窗口（由于尚未持有
 * 关系锁），在此期间工作者可能会选择一个已经被清理过的表；这是当前设计中的一个 bug。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/autovacuum.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <sys/time.h>
#include <unistd.h>

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/reloptions.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/namespace.h"
#include "catalog/pg_database.h"
#include "catalog/pg_namespace.h"
#include "commands/dbcommands.h"
#include "commands/vacuum.h"
#include "common/int.h"
#include "lib/ilist.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "pgstat.h"
#include "postmaster/autovacuum.h"
#include "postmaster/interrupt.h"
#include "postmaster/postmaster.h"
#include "storage/aio_subsys.h"
#include "storage/bufmgr.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lmgr.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/smgr.h"
#include "tcop/tcopprot.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/guc_hooks.h"
#include "utils/injection_point.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timeout.h"
#include "utils/timestamp.h"


/*
 * GUC parameters
 *
 * GUC 参数
 */
bool		autovacuum_start_daemon = false;
int			autovacuum_worker_slots;
int			autovacuum_max_workers;
int			autovacuum_work_mem = -1;
int			autovacuum_naptime;
int			autovacuum_vac_thresh;
int			autovacuum_vac_max_thresh;
double		autovacuum_vac_scale;
int			autovacuum_vac_ins_thresh;
double		autovacuum_vac_ins_scale;
int			autovacuum_anl_thresh;
double		autovacuum_anl_scale;
int			autovacuum_freeze_max_age;
int			autovacuum_multixact_freeze_max_age;

double		autovacuum_vac_cost_delay;
int			autovacuum_vac_cost_limit;

int			Log_autovacuum_min_duration = 600000;

/* the minimum allowed time between two awakenings of the launcher */
/* 两次启动器唤醒之间的最小允许时间 */
#define MIN_AUTOVAC_SLEEPTIME 100.0 /* milliseconds */
#define MAX_AUTOVAC_SLEEPTIME 300	/* seconds */

/*
 * Variables to save the cost-related storage parameters for the current
 * relation being vacuumed by this autovacuum worker. Using these, we can
 * ensure we don't overwrite the values of vacuum_cost_delay and
 * vacuum_cost_limit after reloading the configuration file. They are
 * initialized to "invalid" values to indicate that no cost-related storage
 * parameters were specified and will be set in do_autovacuum() after checking
 * the storage parameters in table_recheck_autovac().
 *
 * 用于保存当前正在由该自动清理工作者清理的关系的代价相关存储参数的变量。
 * 使用这些变量，我们可以确保在重新加载配置文件后不会覆盖 vacuum_cost_delay
 * 和 vacuum_cost_limit 的值。它们被初始化为“无效”值，以表示未指定代价相关的存储参数，
 * 并且将在检查 table_recheck_autovac() 中的存储参数后在 do_autovacuum() 中设置。
 */
static double av_storage_param_cost_delay = -1;
static int	av_storage_param_cost_limit = -1;

/* Flags set by signal handlers */
/* 由信号处理程序设置的标志 */
static volatile sig_atomic_t got_SIGUSR2 = false;

/* Comparison points for determining whether freeze_max_age is exceeded */
/* 用于确定是否超过 freeze_max_age 的比较点 */
static TransactionId recentXid;
static MultiXactId recentMulti;

/* Default freeze ages to use for autovacuum (varies by database) */
/* 自动清理使用的默认冻结年龄（随数据库而异） */
static int	default_freeze_min_age;
static int	default_freeze_table_age;
static int	default_multixact_freeze_min_age;
static int	default_multixact_freeze_table_age;

/* Memory context for long-lived data */
/* 用于长生命周期数据的内存上下文 */
static MemoryContext AutovacMemCxt;

/* struct to keep track of databases in launcher */
/* 用于在启动器中跟踪数据库的结构体 */
typedef struct avl_dbase
{
	Oid			adl_datid;		/* hash key -- must be first */
	TimestampTz adl_next_worker;
	int			adl_score;
	dlist_node	adl_node;
} avl_dbase;

/* struct to keep track of databases in worker */
/* 用于在工作者中跟踪数据库的结构体 */
typedef struct avw_dbase
{
	Oid			adw_datid;
	char	   *adw_name;
	TransactionId adw_frozenxid;
	MultiXactId adw_minmulti;
	PgStat_StatDBEntry *adw_entry;
} avw_dbase;

/* struct to keep track of tables to vacuum and/or analyze, in 1st pass */
/* 用于在第一轮处理中跟踪待清理和/或分析的表的结构体 */
typedef struct av_relation
{
	Oid			ar_toastrelid;	/* hash key - must be first */
	Oid			ar_relid;
	bool		ar_hasrelopts;
	AutoVacOpts ar_reloptions;	/* copy of AutoVacOpts from the main table's
								 * reloptions, or NULL if none */
} av_relation;

/* struct to keep track of tables to vacuum and/or analyze, after rechecking */
/* 用于在重新检查后跟踪待清理和/或分析的表的结构体 */
typedef struct autovac_table
{
	Oid			at_relid;
	VacuumParams at_params;
	double		at_storage_param_vac_cost_delay;
	int			at_storage_param_vac_cost_limit;
	bool		at_dobalance;
	bool		at_sharedrel;
	char	   *at_relname;
	char	   *at_nspname;
	char	   *at_datname;
} autovac_table;

/*-------------
 * This struct holds information about a single worker's whereabouts.  We keep
 * an array of these in shared memory, sized according to
 * autovacuum_worker_slots.
 *
 * wi_links		entry into free list or running list
 * wi_dboid		OID of the database this worker is supposed to work on
 * wi_tableoid	OID of the table currently being vacuumed, if any
 * wi_sharedrel flag indicating whether table is marked relisshared
 * wi_proc		pointer to PGPROC of the running worker, NULL if not started
 * wi_launchtime Time at which this worker was launched
 * wi_dobalance Whether this worker should be included in balance calculations
 *
 * All fields are protected by AutovacuumLock, except for wi_tableoid and
 * wi_sharedrel which are protected by AutovacuumScheduleLock (note these
 * two fields are read-only for everyone except that worker itself).
 *
 * 该结构保存有关单个工作者行踪的信息。我们在共享内存中保留一个此类结构的数组，
 * 其大小根据 autovacuum_worker_slots 确定。
 *
 * wi_links		进入空闲列表或运行列表的项
 * wi_dboid		该工作者应该处理的数据库 OID
 * wi_tableoid	当前正在清理的表的 OID（如果有）
 * wi_sharedrel 指示表是否被标记为 relisshared 的标志
 * wi_proc		指向正在运行的工作者的 PGPROC 指针，如果未启动则为 NULL
 * wi_launchtime 启动该工作者的时间
 * wi_dobalance 该工作者是否应包含在平衡计算中
 *
 * 除 wi_tableoid 和 wi_sharedrel 由 AutovacuumScheduleLock 保护外，
 * 所有字段均由 AutovacuumLock 保护（注意这两个字段对除该工作者本身以外的所有人都是只读的）。
 *-------------
 */
typedef struct WorkerInfoData
{
	dlist_node	wi_links;
	Oid			wi_dboid;
	Oid			wi_tableoid;
	PGPROC	   *wi_proc;
	TimestampTz wi_launchtime;
	pg_atomic_flag wi_dobalance;
	bool		wi_sharedrel;
} WorkerInfoData;

typedef struct WorkerInfoData *WorkerInfo;

/*
 * Possible signals received by the launcher from remote processes.  These are
 * stored atomically in shared memory so that other processes can set them
 * without locking.
 *
 * 启动器从远程进程接收到的可能信号。这些信号以原子方式存储在共享内存中，
 * 以便其他进程可以在不加锁的情况下设置它们。
 */
typedef enum
{
	AutoVacForkFailed,			/* failed trying to start a worker */
	AutoVacRebalance,			/* rebalance the cost limits */
}			AutoVacuumSignal;

#define AutoVacNumSignals (AutoVacRebalance + 1)

/*
 * Autovacuum workitem array, stored in AutoVacuumShmem->av_workItems.  This
 * list is mostly protected by AutovacuumLock, except that if an item is
 * marked 'active' other processes must not modify the work-identifying
 * members.
 *
 * 自动清理工作项数组，存储在 AutoVacuumShmem->av_workItems 中。
 * 该列表主要受 AutovacuumLock 保护，但如果某个项目被标记为“active”，
 * 则其他进程不得修改标识该工作的成员。
 */
typedef struct AutoVacuumWorkItem
{
	AutoVacuumWorkItemType avw_type;
	bool		avw_used;		/* below data is valid */
	bool		avw_active;		/* being processed */
	Oid			avw_database;
	Oid			avw_relation;
	BlockNumber avw_blockNumber;
} AutoVacuumWorkItem;

#define NUM_WORKITEMS	256

/*-------------
 * The main autovacuum shmem struct.  On shared memory we store this main
 * struct and the array of WorkerInfo structs.  This struct keeps:
 *
 * av_signal		set by other processes to indicate various conditions
 * av_launcherpid	the PID of the autovacuum launcher
 * av_freeWorkers	the WorkerInfo freelist
 * av_runningWorkers the WorkerInfo non-free queue
 * av_startingWorker pointer to WorkerInfo currently being started (cleared by
 *					the worker itself as soon as it's up and running)
 * av_workItems		work item array
 * av_nworkersForBalance the number of autovacuum workers to use when
 * 					calculating the per worker cost limit
 *
 * This struct is protected by AutovacuumLock, except for av_signal and parts
 * of the worker list (see above).
 *
 * 主自动清理共享内存结构。我们在共享内存中存储此主结构和 WorkerInfo 结构数组。
 * 该结构保存：
 *
 * av_signal		由其他进程设置以指示各种情况
 * av_launcherpid	自动清理启动器的 PID
 * av_freeWorkers	WorkerInfo 空闲列表
 * av_runningWorkers WorkerInfo 非空闲队列
 * av_startingWorker 指向当前正在启动的工作者的指针（一旦工作者启动并运行，就由工作者本身清除）
 * av_workItems		工作项数组
 * av_nworkersForBalance 计算每个工作者的代价限制时使用的自动清理工作者数量
 *
 * 该结构主要由 AutovacuumLock 保护，但 av_signal 和工作者列表的部分除外（见上文）。
 *-------------
 */
typedef struct
{
	sig_atomic_t av_signal[AutoVacNumSignals];
	pid_t		av_launcherpid;
	dclist_head av_freeWorkers;
	dlist_head	av_runningWorkers;
	WorkerInfo	av_startingWorker;
	AutoVacuumWorkItem av_workItems[NUM_WORKITEMS];
	pg_atomic_uint32 av_nworkersForBalance;
} AutoVacuumShmemStruct;

static AutoVacuumShmemStruct *AutoVacuumShmem;

/*
 * the database list (of avl_dbase elements) in the launcher, and the context
 * that contains it
 *
 * 启动器中的数据库列表（由 avl_dbase 元素组成）以及包含它的上下文
 */
static dlist_head DatabaseList = DLIST_STATIC_INIT(DatabaseList);
static MemoryContext DatabaseListCxt = NULL;

/* Pointer to my own WorkerInfo, valid on each worker */
/* 指向我自己的 WorkerInfo 的指针，对每个工作者有效 */
static WorkerInfo MyWorkerInfo = NULL;

/* PID of launcher, valid only in worker while shutting down */
/* 启动器的 PID，仅在工作者关闭期间有效 */
int			AutovacuumLauncherPid = 0;

static Oid	do_start_worker(void);
static void ProcessAutoVacLauncherInterrupts(void);
pg_noreturn static void AutoVacLauncherShutdown(void);
static void launcher_determine_sleep(bool canlaunch, bool recursing,
									 struct timeval *nap);
static void launch_worker(TimestampTz now);
static List *get_database_list(void);
static void rebuild_database_list(Oid newdb);
static int	db_comparator(const void *a, const void *b);
static void autovac_recalculate_workers_for_balance(void);

static void do_autovacuum(void);
static void FreeWorkerInfo(int code, Datum arg);

static autovac_table *table_recheck_autovac(Oid relid, HTAB *table_toast_map,
											TupleDesc pg_class_desc,
											int effective_multixact_freeze_max_age);
static void recheck_relation_needs_vacanalyze(Oid relid, AutoVacOpts *avopts,
											  Form_pg_class classForm,
											  int effective_multixact_freeze_max_age,
											  bool *dovacuum, bool *doanalyze, bool *wraparound);
static void relation_needs_vacanalyze(Oid relid, AutoVacOpts *relopts,
									  Form_pg_class classForm,
									  PgStat_StatTabEntry *tabentry,
									  int effective_multixact_freeze_max_age,
									  bool *dovacuum, bool *doanalyze, bool *wraparound);

static void autovacuum_do_vac_analyze(autovac_table *tab,
									  BufferAccessStrategy bstrategy);
static AutoVacOpts *extract_autovac_opts(HeapTuple tup,
										 TupleDesc pg_class_desc);
static void perform_work_item(AutoVacuumWorkItem *workitem);
static void autovac_report_activity(autovac_table *tab);
static void autovac_report_workitem(AutoVacuumWorkItem *workitem,
									const char *nspname, const char *relname);
static void avl_sigusr2_handler(SIGNAL_ARGS);
static bool av_worker_available(void);
static void check_av_worker_gucs(void);



/********************************************************************
 *					  AUTOVACUUM LAUNCHER CODE
 ********************************************************************/

/*
 * Main entry point for the autovacuum launcher process.
 *
 * 自动清理启动器进程的主入口点。
 */
void
AutoVacLauncherMain(const void *startup_data, size_t startup_data_len)
{
	sigjmp_buf	local_sigjmp_buf;

	Assert(startup_data_len == 0);

	/* Release postmaster's working memory context */
	if (PostmasterContext)
	{
		MemoryContextDelete(PostmasterContext);
		PostmasterContext = NULL;
	}

	MyBackendType = B_AUTOVAC_LAUNCHER;
	init_ps_display(NULL);

	ereport(DEBUG1,
			(errmsg_internal("autovacuum launcher started")));

	if (PostAuthDelay)
		pg_usleep(PostAuthDelay * 1000000L);

	Assert(GetProcessingMode() == InitProcessing);

	/*
	 * Set up signal handlers.  We operate on databases much like a regular
	 * backend, so we use the same signal handling.  See equivalent code in
	 * tcop/postgres.c.
	 */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGINT, StatementCancelHandler);
	pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
	/* SIGQUIT handler was already set up by InitPostmasterChild */

	InitializeTimeouts();		/* establishes SIGALRM handler */

	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, avl_sigusr2_handler);
	pqsignal(SIGFPE, FloatExceptionHandler);
	pqsignal(SIGCHLD, SIG_DFL);

	/*
	 * Create a per-backend PGPROC struct in shared memory.  We must do this
	 * before we can use LWLocks or access any shared memory.
	 */
	InitProcess();

	/* Early initialization */
	BaseInit();

	InitPostgres(NULL, InvalidOid, NULL, InvalidOid, 0, NULL);

	SetProcessingMode(NormalProcessing);

	/*
	 * Create a memory context that we will do all our work in.  We do this so
	 * that we can reset the context during error recovery and thereby avoid
	 * possible memory leaks.
	 *
	 * 创建我们将执行所有工作的内存上下文。这样做是为了让我们可以在错误恢复期间
	 * 重置上下文，从而避免可能的内存泄漏。
	 */
	AutovacMemCxt = AllocSetContextCreate(TopMemoryContext,
										  "Autovacuum Launcher",
										  ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(AutovacMemCxt);

	/*
	 * If an exception is encountered, processing resumes here.
	 *
	 * This code is a stripped down version of PostgresMain error recovery.
	 *
	 * Note that we use sigsetjmp(..., 1), so that the prevailing signal mask
	 * (to wit, BlockSig) will be restored when longjmp'ing to here.  Thus,
	 * signals other than SIGQUIT will be blocked until we complete error
	 * recovery.  It might seem that this policy makes the HOLD_INTERRUPTS()
	 * call redundant, but it is not since InterruptPending might be set
	 * already.
	 *
	 * 如果遇到异常，处理将从这里恢复。
	 *
	 * 此代码是 PostgresMain 错误恢复的精简版。
	 *
	 * 请注意，我们使用 sigsetjmp(..., 1)，以便在 longjmp 到此处时恢复
	 * 当时的信号掩码（即 BlockSig）。因此，在完成错误恢复之前，
	 * 除 SIGQUIT 以外的信号将被阻塞。这种策略看起来似乎使
	 * HOLD_INTERRUPTS() 调用变得多余，但事实并非如此，因为
	 * InterruptPending 可能已经被设置了。
	 */
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/* since not using PG_TRY, must reset error stack by hand */
		error_context_stack = NULL;

		/* Prevents interrupts while cleaning up */
		HOLD_INTERRUPTS();

		/* Forget any pending QueryCancel or timeout request */
		/* 忘记任何挂起的查询取消或超时请求 */
		disable_all_timeouts(false);
		QueryCancelPending = false; /* second to avoid race condition */

		/* Report the error to the server log */
		EmitErrorReport();

		/* Abort the current transaction in order to recover */
		/* 中止当前事务以便恢复 */
		AbortCurrentTransaction();

		/*
		 * Release any other resources, for the case where we were not in a
		 * transaction.
		 *
		 * 释放任何其他资源，以防我们不在事务中。
		 */
		LWLockReleaseAll();
		pgstat_report_wait_end();
		pgaio_error_cleanup();
		UnlockBuffers();
		/* this is probably dead code, but let's be safe: */
		if (AuxProcessResourceOwner)
			ReleaseAuxProcessResources(false);
		AtEOXact_Buffers(false);
		AtEOXact_SMgr();
		AtEOXact_Files(false);
		AtEOXact_HashTables(false);

		/*
		 * Now return to normal top-level context and clear ErrorContext for
		 * next time.
		 *
		 * 现在返回到正常的顶层上下文，并为下次清除 ErrorContext。
		 */
		MemoryContextSwitchTo(AutovacMemCxt);
		FlushErrorState();

		/* Flush any leaked data in the top-level context */
		/* 清除顶层上下文中泄漏的任何数据 */
		MemoryContextReset(AutovacMemCxt);

		/* don't leave dangling pointers to freed memory */
		/* 不要留下指向已释放内存的悬空指针 */
		DatabaseListCxt = NULL;
		dlist_init(&DatabaseList);

		/* Now we can allow interrupts again */
		/* 现在我们可以再次允许中断了 */
		RESUME_INTERRUPTS();

		/* if in shutdown mode, no need for anything further; just go away */
		/* 如果处于关闭模式，则无需进一步操作；直接退出 */
		if (ShutdownRequestPending)
			AutoVacLauncherShutdown();

		/*
		 * Sleep at least 1 second after any error.  We don't want to be
		 * filling the error logs as fast as we can.
		 *
		 * 发生任何错误后至少休眠 1 秒。我们不想让错误日志被尽快填满。
		 */
		pg_usleep(1000000L);
	}

	/* We can now handle ereport(ERROR) */
	PG_exception_stack = &local_sigjmp_buf;

	/* must unblock signals before calling rebuild_database_list */
	/* 在调用 rebuild_database_list 之前必须解除对信号的阻塞 */
	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

	/*
	 * Set always-secure search path.  Launcher doesn't connect to a database,
	 * so this has no effect.
	 *
	 * 设置始终安全的 search_path。启动器不连接到数据库，所以这没有影响。
	 */
	SetConfigOption("search_path", "", PGC_SUSET, PGC_S_OVERRIDE);

	/*
	 * Force zero_damaged_pages OFF in the autovac process, even if it is set
	 * in postgresql.conf.  We don't really want such a dangerous option being
	 * applied non-interactively.
	 */
	SetConfigOption("zero_damaged_pages", "false", PGC_SUSET, PGC_S_OVERRIDE);

	/*
	 * Force settable timeouts off to avoid letting these settings prevent
	 * regular maintenance from being executed.
	 */
	SetConfigOption("statement_timeout", "0", PGC_SUSET, PGC_S_OVERRIDE);
	SetConfigOption("transaction_timeout", "0", PGC_SUSET, PGC_S_OVERRIDE);
	SetConfigOption("lock_timeout", "0", PGC_SUSET, PGC_S_OVERRIDE);
	SetConfigOption("idle_in_transaction_session_timeout", "0",
					PGC_SUSET, PGC_S_OVERRIDE);

	/*
	 * Force default_transaction_isolation to READ COMMITTED.  We don't want
	 * to pay the overhead of serializable mode, nor add any risk of causing
	 * deadlocks or delaying other transactions.
	 */
	SetConfigOption("default_transaction_isolation", "read committed",
					PGC_SUSET, PGC_S_OVERRIDE);

	/*
	 * Even when system is configured to use a different fetch consistency,
	 * for autovac we always want fresh stats.
	 */
	SetConfigOption("stats_fetch_consistency", "none", PGC_SUSET, PGC_S_OVERRIDE);

	/*
	 * In emergency mode, just start a worker (unless shutdown was requested)
	 * and go away.
	 *
	 * 在紧急模式下，只需启动一个工作者（除非请求了关闭）然后退出。
	 */
	if (!AutoVacuumingActive())
	{
		if (!ShutdownRequestPending)
			do_start_worker();
		proc_exit(0);			/* done */
	}

	AutoVacuumShmem->av_launcherpid = MyProcPid;

	/*
	 * Create the initial database list.  The invariant we want this list to
	 * keep is that it's ordered by decreasing next_time.  As soon as an entry
	 * is updated to a higher time, it will be moved to the front (which is
	 * correct because the only operation is to add autovacuum_naptime to the
	 * entry, and time always increases).
	 *
	 * 创建初始数据库列表。我们希望该列表维持的变量是按 next_time 递减排序。
	 * 一旦某个条目更新为更晚的时间，它将被移到最前端（这是正确的，因为唯一
	 * 的操作就是给条目增加 autovacuum_naptime，而时间总是递增的）。
	 */
	rebuild_database_list(InvalidOid);

	/* loop until shutdown request */
	/* 循环直到收到关闭请求 */
	while (!ShutdownRequestPending)
	{
		struct timeval nap;
		TimestampTz current_time = 0;
		bool		can_launch;

		/*
		 * This loop is a bit different from the normal use of WaitLatch,
		 * because we'd like to sleep before the first launch of a child
		 * process.  So it's WaitLatch, then ResetLatch, then check for
		 * wakening conditions.
		 *
		 * 此循环与 WaitLatch 的常规用法稍有不同，因为我们希望在第一次启动
		 * 子进程之前进行休眠。所以是先 WaitLatch，然后 ResetLatch，
		 * 接着检查唤醒条件。
		 */

		launcher_determine_sleep(av_worker_available(), false, &nap);

		/*
		 * Wait until naptime expires or we get some type of signal (all the
		 * signal handlers will wake us by calling SetLatch).
		 *
		 * 等待直到休眠时间到期或我们收到某种类型的信号
		 *（所有信号处理程序都会通过调用 SetLatch 来唤醒我们）。
		 */
		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 (nap.tv_sec * 1000L) + (nap.tv_usec / 1000L),
						 WAIT_EVENT_AUTOVACUUM_MAIN);

		ResetLatch(MyLatch);

		ProcessAutoVacLauncherInterrupts();

		/*
		 * a worker finished, or postmaster signaled failure to start a worker
		 *
		 * 一个工作者完成工作，或者 postmaster 发信号表示启动工作者失败
		 */
		if (got_SIGUSR2)
		{
			got_SIGUSR2 = false;

			/* rebalance cost limits, if needed */
			/* 如果需要，重新平衡代价限制 */
			if (AutoVacuumShmem->av_signal[AutoVacRebalance])
			{
				LWLockAcquire(AutovacuumLock, LW_EXCLUSIVE);
				AutoVacuumShmem->av_signal[AutoVacRebalance] = false;
				autovac_recalculate_workers_for_balance();
				LWLockRelease(AutovacuumLock);
			}

			if (AutoVacuumShmem->av_signal[AutoVacForkFailed])
			{
				/*
				 * If the postmaster failed to start a new worker, we sleep
				 * for a little while and resend the signal.  The new worker's
				 * state is still in memory, so this is sufficient.  After
				 * that, we restart the main loop.
				 *
				 * XXX should we put a limit to the number of times we retry?
				 * I don't think it makes much sense, because a future start
				 * of a worker will continue to fail in the same way.
				 *
				 * 如果 postmaster 启动新工作者失败，我们会休眠一小会儿并重新发送信号。
				 * 新工作者的状态仍保留在内存中，所以这样做就足够了。
				 * 之后，我们重新开始主循环。
				 *
				 * XXX 我们是否应该限制重试次数？我认为这没有太大意义，
				 * 因为未来启动的工作者仍会以同样的方式失败。
				 */
				AutoVacuumShmem->av_signal[AutoVacForkFailed] = false;
				pg_usleep(1000000L);	/* 1s */
				SendPostmasterSignal(PMSIGNAL_START_AUTOVAC_WORKER);
				continue;
			}
		}

		/*
		 * There are some conditions that we need to check before trying to
		 * start a worker.  First, we need to make sure that there is a worker
		 * slot available.  Second, we need to make sure that no other worker
		 * failed while starting up.
		 *
		 * 在尝试启动工作者之前，我们需要检查一些条件。首先，我们需要确保
		 * 有可用的工作者插槽。其次，我们需要确保没有其他工作者在启动时失败。
		 */

		current_time = GetCurrentTimestamp();
		LWLockAcquire(AutovacuumLock, LW_SHARED);

		can_launch = av_worker_available();

		if (AutoVacuumShmem->av_startingWorker != NULL)
		{
			int			waittime;
			WorkerInfo	worker = AutoVacuumShmem->av_startingWorker;

			/*
			 * We can't launch another worker when another one is still
			 * starting up (or failed while doing so), so just sleep for a bit
			 * more; that worker will wake us up again as soon as it's ready.
			 * We will only wait autovacuum_naptime seconds (up to a maximum
			 * of 60 seconds) for this to happen however.  Note that failure
			 * to connect to a particular database is not a problem here,
			 * because the worker removes itself from the startingWorker
			 * pointer before trying to connect.  Problems detected by the
			 * postmaster (like fork() failure) are also reported and handled
			 * differently.  The only problems that may cause this code to
			 * fire are errors in the earlier sections of AutoVacWorkerMain,
			 * before the worker removes the WorkerInfo from the
			 * startingWorker pointer.
			 *
			 * 当另一个工作者仍在启动中（或在启动时失败）时，我们不能启动另一个工作者，
			 * 所以只需再睡一会儿；那个工作者准备好后会再次唤醒我们。
			 * 然而，我们最多只会等待 autovacuum_naptime 秒（最大 60 秒）让这种情况发生。
			 * 请注意，此处无法连接到特定数据库并不是问题，因为工作者在尝试连接
			 * 之前会将其自身从 startingWorker 指针中移除。由 postmaster 检测到的问题
			 *（如 fork() 失败）也会以不同的方式报告和处理。可能导致此段代码生效的唯一
			 * 问题是 AutoVacWorkerMain 早期阶段的错误，即在工作者将 WorkerInfo
			 * 从 startingWorker 指针中移除之前发生的错误。
			 */
			waittime = Min(autovacuum_naptime, 60) * 1000;
			if (TimestampDifferenceExceeds(worker->wi_launchtime, current_time,
										   waittime))
			{
				LWLockRelease(AutovacuumLock);
				LWLockAcquire(AutovacuumLock, LW_EXCLUSIVE);

				/*
				 * No other process can put a worker in starting mode, so if
				 * startingWorker is still INVALID after exchanging our lock,
				 * we assume it's the same one we saw above (so we don't
				 * recheck the launch time).
				 *
				 * 没有其他进程可以将工作者置于启动模式，所以如果在交换锁后
				 * startingWorker 仍为无效 (INVALID)，我们假设它就是我们上面看到的
				 * 同一个工作者（所以我们不再重新检查启动时间）。
				 */
				if (AutoVacuumShmem->av_startingWorker != NULL)
				{
					worker = AutoVacuumShmem->av_startingWorker;
					worker->wi_dboid = InvalidOid;
					worker->wi_tableoid = InvalidOid;
					worker->wi_sharedrel = false;
					worker->wi_proc = NULL;
					worker->wi_launchtime = 0;
					dclist_push_head(&AutoVacuumShmem->av_freeWorkers,
									 &worker->wi_links);
					AutoVacuumShmem->av_startingWorker = NULL;
					ereport(WARNING,
							errmsg("autovacuum worker took too long to start; canceled"));
				}
			}
			else
				can_launch = false;
		}
		LWLockRelease(AutovacuumLock);	/* either shared or exclusive */

		/* if we can't do anything, just go back to sleep */
		/* 如果我们无能为力，就继续休眠 */
		if (!can_launch)
			continue;

		/* We're OK to start a new worker */
		/* 我们现在可以启动一个新的工作者了 */

		if (dlist_is_empty(&DatabaseList))
		{
			/*
			 * Special case when the list is empty: start a worker right away.
			 * This covers the initial case, when no database is in pgstats
			 * (thus the list is empty).  Note that the constraints in
			 * launcher_determine_sleep keep us from starting workers too
			 * quickly (at most once every autovacuum_naptime when the list is
			 * empty).
			 *
			 * 列表为空的特殊情况：立即启动一个工作者。
			 * 这涵盖了初始情况，即 pgstats 中没有数据库（因此列表为空）。
			 * 请注意，launcher_determine_sleep 中的限制防止我们启动工作者
			 * 过快（当列表为空时，最多每隔 autovacuum_naptime 启动一次）。
			 */
			launch_worker(current_time);
		}
		else
		{
			/*
			 * because rebuild_database_list constructs a list with most
			 * distant adl_next_worker first, we obtain our database from the
			 * tail of the list.
			 *
			 * 因为 rebuild_database_list 会构造一个将 adl_next_worker 最远的数据库
			 * 排在最前面的列表，所以我们从列表的末尾获取数据库。
			 */
			avl_dbase  *avdb;

			avdb = dlist_tail_element(avl_dbase, adl_node, &DatabaseList);

			/*
			 * launch a worker if next_worker is right now or it is in the
			 * past
			 *
			 * 如果 next_worker 是现在或过去的时间，则启动工作者
			 */
			if (TimestampDifferenceExceeds(avdb->adl_next_worker,
										   current_time, 0))
				launch_worker(current_time);
		}
	}

	AutoVacLauncherShutdown();
}

/*
 * Process any new interrupts.
 *
 * 处理任何新的中断。
 */
static void
ProcessAutoVacLauncherInterrupts(void)
{
	/* the normal shutdown case */
	/* 正常关闭的情况 */
	if (ShutdownRequestPending)
		AutoVacLauncherShutdown();

	if (ConfigReloadPending)
	{
		int			autovacuum_max_workers_prev = autovacuum_max_workers;

		ConfigReloadPending = false;
		ProcessConfigFile(PGC_SIGHUP);

		/* shutdown requested in config file? */
		/* 配置文件中是否请求了关闭？ */
		if (!AutoVacuumingActive())
			AutoVacLauncherShutdown();

		/*
		 * If autovacuum_max_workers changed, emit a WARNING if
		 * autovacuum_worker_slots < autovacuum_max_workers.  If it didn't
		 * change, skip this to avoid too many repeated log messages.
		 *
		 * 如果 autovacuum_max_workers 发生变化，且 autovacuum_worker_slots < 
		 * autovacuum_max_workers，则发出警告。如果它没有变化，则跳过此操作
		 * 以避免产生过多的重复日志消息。
		 */
		if (autovacuum_max_workers_prev != autovacuum_max_workers)
			check_av_worker_gucs();

		/* rebuild the list in case the naptime changed */
		/* 以防休眠时间发生变化，重新构建列表 */
		rebuild_database_list(InvalidOid);
	}

	/* Process barrier events */
	/* 处理屏障 (barrier) 事件 */
	if (ProcSignalBarrierPending)
		ProcessProcSignalBarrier();

	/* Perform logging of memory contexts of this process */
	/* 对该进程的内存上下文执行日志记录 */
	if (LogMemoryContextPending)
		ProcessLogMemoryContextInterrupt();

	/* Process sinval catchup interrupts that happened while sleeping */
	/* 处理休眠期间发生的 sinval catchup 中断 */
	ProcessCatchupInterrupt();
}

/*
 * Perform a normal exit from the autovac launcher.
 *
 * 执行自动清理启动器的正常退出。
 */
static void
AutoVacLauncherShutdown(void)
{
	ereport(DEBUG1,
			(errmsg_internal("autovacuum launcher shutting down")));
	AutoVacuumShmem->av_launcherpid = 0;

	proc_exit(0);				/* done */
}

/*
 * Determine the time to sleep, based on the database list.
 *
 * The "canlaunch" parameter indicates whether we can start a worker right now,
 * for example due to the workers being all busy.  If this is false, we will
 * cause a long sleep, which will be interrupted when a worker exits.
 *
 * 基于数据库列表确定休眠时间。
 *
 * “canlaunch”参数指示我们现在是否可以启动工作者，例如由于工作者都处于繁忙状态。
 * 如果是 false，我们将导致长时间休眠，该休眠将在工作者退出时被中断。
 */
static void
launcher_determine_sleep(bool canlaunch, bool recursing, struct timeval *nap)
{
	/*
	 * We sleep until the next scheduled vacuum.  We trust that when the
	 * database list was built, care was taken so that no entries have times
	 * in the past; if the first entry has too close a next_worker value, or a
	 * time in the past, we will sleep a small nominal time.
	 *
	 * 我们休眠直到下一次计划的清理。我们相信在建立数据库列表时已经注意确保没有条目
	 * 具有过去的时间；如果第一个条目的 next_worker 值太接近，或者是过去的时间，
	 * 我们将休眠一个较小的标称时间。
	 */
	if (!canlaunch)
	{
		nap->tv_sec = autovacuum_naptime;
		nap->tv_usec = 0;
	}
	else if (!dlist_is_empty(&DatabaseList))
	{
		TimestampTz current_time = GetCurrentTimestamp();
		TimestampTz next_wakeup;
		avl_dbase  *avdb;
		long		secs;
		int			usecs;

		avdb = dlist_tail_element(avl_dbase, adl_node, &DatabaseList);

		next_wakeup = avdb->adl_next_worker;
		TimestampDifference(current_time, next_wakeup, &secs, &usecs);

		nap->tv_sec = secs;
		nap->tv_usec = usecs;
	}
	else
	{
		/* list is empty, sleep for whole autovacuum_naptime seconds  */
		/* 列表为空，休眠整个 autovacuum_naptime 秒 */
		nap->tv_sec = autovacuum_naptime;
		nap->tv_usec = 0;
	}

	/*
	 * If the result is exactly zero, it means a database had an entry with
	 * time in the past.  Rebuild the list so that the databases are evenly
	 * distributed again, and recalculate the time to sleep.  This can happen
	 * if there are more tables needing vacuum than workers, and they all take
	 * longer to vacuum than autovacuum_naptime.
	 *
	 * We only recurse once.  rebuild_database_list should always return times
	 * in the future, but it seems best not to trust too much on that.
	 *
	 * 如果结果恰好为零，这意味着某个数据库有一个时间在过去的条目。
	 * 重新构建列表，以便数据库再次均匀分布，并重新计算休眠时间。
	 * 如果有比工作者更多的表需要清理，并且它们清理的时间都长于 autovacuum_naptime，
	 * 就会发生这种情况。
	 *
	 * 我们只递归一次。rebuild_database_list 应该总是返回未来的时间，
	 * 但最好不要太相信这一点。
	 */
	if (nap->tv_sec == 0 && nap->tv_usec == 0 && !recursing)
	{
		rebuild_database_list(InvalidOid);
		launcher_determine_sleep(canlaunch, true, nap);
		return;
	}

	/* The smallest time we'll allow the launcher to sleep. */
	/* 我们允许启动器休眠的最小时间。 */
	if (nap->tv_sec <= 0 && nap->tv_usec <= MIN_AUTOVAC_SLEEPTIME * 1000)
	{
		nap->tv_sec = 0;
		nap->tv_usec = MIN_AUTOVAC_SLEEPTIME * 1000;
	}

	/*
	 * If the sleep time is too large, clamp it to an arbitrary maximum (plus
	 * any fractional seconds, for simplicity).  This avoids an essentially
	 * infinite sleep in strange cases like the system clock going backwards a
	 * few years.
	 *
	 * 如果休眠时间太大，将其限制在一个任意的最大值（为了简单起见，加上任何
	 * 秒的小数部分）。这可以避免在奇怪的情况下（如系统时钟后退几年）
	 * 产生本质上的无限休眠。
	 */
	if (nap->tv_sec > MAX_AUTOVAC_SLEEPTIME)
		nap->tv_sec = MAX_AUTOVAC_SLEEPTIME;
}

/*
 * Build an updated DatabaseList.  It must only contain databases that appear
 * in pgstats, and must be sorted by next_worker from highest to lowest,
 * distributed regularly across the next autovacuum_naptime interval.
 *
 * Receives the Oid of the database that made this list be generated (we call
 * this the "new" database, because when the database was already present on
 * the list, we expect that this function is not called at all).  The
 * preexisting list, if any, will be used to preserve the order of the
 * databases in the autovacuum_naptime period.  The new database is put at the
 * end of the interval.  The actual values are not saved, which should not be
 * much of a problem.
 */
static void
rebuild_database_list(Oid newdb)
{
	List	   *dblist;
	ListCell   *cell;
	MemoryContext newcxt;
	MemoryContext oldcxt;
	MemoryContext tmpcxt;
	HASHCTL		hctl;
	int			score;
	int			nelems;
	HTAB	   *dbhash;
	dlist_iter	iter;

	newcxt = AllocSetContextCreate(AutovacMemCxt,
								   "Autovacuum database list",
								   ALLOCSET_DEFAULT_SIZES);
	tmpcxt = AllocSetContextCreate(newcxt,
								   "Autovacuum database list (tmp)",
								   ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(tmpcxt);

	/*
	 * Implementing this is not as simple as it sounds, because we need to put
	 * the new database at the end of the list; next the databases that were
	 * already on the list, and finally (at the tail of the list) all the
	 * other databases that are not on the existing list.
	 *
	 * To do this, we build an empty hash table of scored databases.  We will
	 * start with the lowest score (zero) for the new database, then
	 * increasing scores for the databases in the existing list, in order, and
	 * lastly increasing scores for all databases gotten via
	 * get_database_list() that are not already on the hash.
	 *
	 * Then we will put all the hash elements into an array, sort the array by
	 * score, and finally put the array elements into the new doubly linked
	 * list.
	 */
	hctl.keysize = sizeof(Oid);
	hctl.entrysize = sizeof(avl_dbase);
	hctl.hcxt = tmpcxt;
	dbhash = hash_create("autovacuum db hash", 20, &hctl,	/* magic number here
															 * FIXME */
						 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	/* start by inserting the new database */
	/* 从插入新数据库开始 */
	score = 0;
	if (OidIsValid(newdb))
	{
		avl_dbase  *db;
		PgStat_StatDBEntry *entry;

		/* only consider this database if it has a pgstat entry */
		/* 仅当该数据库有 pgstat 条目时才考虑它 */
		entry = pgstat_fetch_stat_dbentry(newdb);
		if (entry != NULL)
		{
			/* we assume it isn't found because the hash was just created */
			/* 我们假设找不到是因为哈希表刚刚创建 */
			db = hash_search(dbhash, &newdb, HASH_ENTER, NULL);

			/* hash_search already filled in the key */
			db->adl_score = score++;
			/* next_worker is filled in later */
		}
	}

	/* Now insert the databases from the existing list */
	/* 现在插入来自现有列表的数据库 */
	dlist_foreach(iter, &DatabaseList)
	{
		avl_dbase  *avdb = dlist_container(avl_dbase, adl_node, iter.cur);
		avl_dbase  *db;
		bool		found;
		PgStat_StatDBEntry *entry;

		/*
		 * skip databases with no stat entries -- in particular, this gets rid
		 * of dropped databases
		 *
		 * 跳过没有统计条目的数据库——特别是这会清除已删除的数据库
		 */
		entry = pgstat_fetch_stat_dbentry(avdb->adl_datid);
		if (entry == NULL)
			continue;

		db = hash_search(dbhash, &(avdb->adl_datid), HASH_ENTER, &found);

		if (!found)
		{
			/* hash_search already filled in the key */
			db->adl_score = score++;
			/* next_worker is filled in later */
		}
	}

	/* finally, insert all qualifying databases not previously inserted */
	/* 最后，插入所有之前未插入的合格数据库 */
	dblist = get_database_list();
	foreach(cell, dblist)
	{
		avw_dbase  *avdb = lfirst(cell);
		avl_dbase  *db;
		bool		found;
		PgStat_StatDBEntry *entry;

		/* only consider databases with a pgstat entry */
		/* 仅考虑有 pgstat 条目的数据库 */
		entry = pgstat_fetch_stat_dbentry(avdb->adw_datid);
		if (entry == NULL)
			continue;

		db = hash_search(dbhash, &(avdb->adw_datid), HASH_ENTER, &found);
		/* only update the score if the database was not already on the hash */
		/* 仅当数据库不在哈希表中时才更新分数 */
		if (!found)
		{
			/* hash_search already filled in the key */
			db->adl_score = score++;
			/* next_worker is filled in later */
		}
	}
	nelems = score;

	/* from here on, the allocated memory belongs to the new list */
	/* 从这里开始，分配的内存属于新列表 */
	MemoryContextSwitchTo(newcxt);
	dlist_init(&DatabaseList);

	if (nelems > 0)
	{
		TimestampTz current_time;
		int			millis_increment;
		avl_dbase  *dbary;
		avl_dbase  *db;
		HASH_SEQ_STATUS seq;
		int			i;

		/* put all the hash elements into an array */
		/* 将所有哈希元素放入一个数组中 */
		dbary = palloc(nelems * sizeof(avl_dbase));

		i = 0;
		hash_seq_init(&seq, dbhash);
		while ((db = hash_seq_search(&seq)) != NULL)
			memcpy(&(dbary[i++]), db, sizeof(avl_dbase));

		/* sort the array */
		/* 对数组进行排序 */
		qsort(dbary, nelems, sizeof(avl_dbase), db_comparator);

		/*
		 * Determine the time interval between databases in the schedule. If
		 * we see that the configured naptime would take us to sleep times
		 * lower than our min sleep time (which launcher_determine_sleep is
		 * coded not to allow), silently use a larger naptime (but don't touch
		 * the GUC variable).
		 *
		 * 确定计划中数据库之间的时间间隔。如果我们看到配置的休眠时间会导致休眠时间
		 * 低于我们的最小休眠时间（launcher_determine_sleep 被编码为不允许这种情况），
		 * 则默默地使用较大的 naptime（但不要触碰 GUC 变量）。
		 */
		millis_increment = 1000.0 * autovacuum_naptime / nelems;
		if (millis_increment <= MIN_AUTOVAC_SLEEPTIME)
			millis_increment = MIN_AUTOVAC_SLEEPTIME * 1.1;

		current_time = GetCurrentTimestamp();

		/*
		 * move the elements from the array into the dlist, setting the
		 * next_worker while walking the array
		 *
		 * 将元素从数组移动到 dlist 中，在遍历数组时设置 next_worker
		 */
		for (i = 0; i < nelems; i++)
		{
			db = &(dbary[i]);

			current_time = TimestampTzPlusMilliseconds(current_time,
													   millis_increment);
			db->adl_next_worker = current_time;

			/* later elements should go closer to the head of the list */
			/* 较晚的元素应该更接近列表头部 */
			dlist_push_head(&DatabaseList, &db->adl_node);
		}
	}

	/* all done, clean up memory */
	/* 全部完成，清理内存 */
	if (DatabaseListCxt != NULL)
		MemoryContextDelete(DatabaseListCxt);
	MemoryContextDelete(tmpcxt);
	DatabaseListCxt = newcxt;
	MemoryContextSwitchTo(oldcxt);
}

/* qsort comparator for avl_dbase, using adl_score */
/* avl_dbase 的 qsort 比较器，使用 adl_score */
static int
db_comparator(const void *a, const void *b)
{
	return pg_cmp_s32(((const avl_dbase *) a)->adl_score,
					  ((const avl_dbase *) b)->adl_score);
}

/*
 * do_start_worker
 *
 * Bare-bones procedure for starting an autovacuum worker from the launcher.
 * It determines what database to work on, sets up shared memory stuff and
 * signals postmaster to start the worker.  It fails gracefully if invoked when
 * autovacuum_workers are already active.
 *
 * 从启动器开始自动清理工作者的基本程序。它确定要处理哪个数据库，设置共享内存
 * 内容并通知 postmaster 启动工作者。如果调用时 autovacuum_workers 
 * 已经处于活动状态，它会优雅地失败。
 *
 * Return value is the OID of the database that the worker is going to process,
 * or InvalidOid if no worker was actually started.
 *
 * 返回值是工作者将要处理的数据库的 OID，如果实际上没有启动任何工作者，则为 InvalidOid。
 */
static Oid
do_start_worker(void)
{
	List	   *dblist;
	ListCell   *cell;
	TransactionId xidForceLimit;
	MultiXactId multiForceLimit;
	bool		for_xid_wrap;
	bool		for_multi_wrap;
	avw_dbase  *avdb;
	TimestampTz current_time;
	bool		skipit = false;
	Oid			retval = InvalidOid;
	MemoryContext tmpcxt,
				oldcxt;

	/* return quickly when there are no free workers */
	/* 如果没有空闲的工作者，快速返回 */
	LWLockAcquire(AutovacuumLock, LW_SHARED);
	if (!av_worker_available())
	{
		LWLockRelease(AutovacuumLock);
		return InvalidOid;
	}
	LWLockRelease(AutovacuumLock);

	/*
	 * Create and switch to a temporary context to avoid leaking the memory
	 * allocated for the database list.
	 *
	 * 创建并切换到一个临时上下文，以避免泄漏为数据库列表分配的内存。
	 */
	tmpcxt = AllocSetContextCreate(CurrentMemoryContext,
								   "Autovacuum start worker (tmp)",
								   ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(tmpcxt);

	/* Get a list of databases */
	/* 获取数据库列表 */
	dblist = get_database_list();

	/*
	 * Determine the oldest datfrozenxid/relfrozenxid that we will allow to
	 * pass without forcing a vacuum.  (This limit can be tightened for
	 * particular tables, but not loosened.)
	 *
	 * 确定在不强制执行清理的情况下我们允许通过的最久远的 datfrozenxid/relfrozenxid。
	 *（该限制可以针对特定表收紧，但不能放宽。）
	 */
	recentXid = ReadNextTransactionId();
	xidForceLimit = recentXid - autovacuum_freeze_max_age;
	/* ensure it's a "normal" XID, else TransactionIdPrecedes misbehaves */
	/* 确保它是“普通”XID，否则 TransactionIdPrecedes 行为会异常 */
	/* this can cause the limit to go backwards by 3, but that's OK */
	/* 这可能会导致限制倒退 3，但没关系 */
	if (xidForceLimit < FirstNormalTransactionId)
		xidForceLimit -= FirstNormalTransactionId;

	/* Also determine the oldest datminmxid we will consider. */
	/* 同时确定我们将考虑的最久远的 datminmxid。 */
	recentMulti = ReadNextMultiXactId();
	multiForceLimit = recentMulti - MultiXactMemberFreezeThreshold();
	if (multiForceLimit < FirstMultiXactId)
		multiForceLimit -= FirstMultiXactId;

	/*
	 * Choose a database to connect to.  We pick the database that was least
	 * recently auto-vacuumed, or one that needs vacuuming to prevent Xid
	 * wraparound-related data loss.  If any db at risk of Xid wraparound is
	 * found, we pick the one with oldest datfrozenxid, independently of
	 * autovacuum times; similarly we pick the one with the oldest datminmxid
	 * if any is in MultiXactId wraparound.  Note that those in Xid wraparound
	 * danger are given more priority than those in multi wraparound danger.
	 *
	 * Note that a database with no stats entry is not considered, except for
	 * Xid wraparound purposes.  The theory is that if no one has ever
	 * connected to it since the stats were last initialized, it doesn't need
	 * vacuuming.
	 *
	 * XXX This could be improved if we had more info about whether it needs
	 * vacuuming before connecting to it.  Perhaps look through the pgstats
	 * data for the database's tables?  One idea is to keep track of the
	 * number of new and dead tuples per database in pgstats.  However it
	 * isn't clear how to construct a metric that measures that and not cause
	 * starvation for less busy databases.
	 */
	avdb = NULL;
	for_xid_wrap = false;
	for_multi_wrap = false;
	current_time = GetCurrentTimestamp();
	foreach(cell, dblist)
	{
		avw_dbase  *tmp = lfirst(cell);
		dlist_iter	iter;

		/* Check to see if this one is at risk of wraparound */
		/* 检查此数据库是否有回绕风险 */
		if (TransactionIdPrecedes(tmp->adw_frozenxid, xidForceLimit))
		{
			if (avdb == NULL ||
				TransactionIdPrecedes(tmp->adw_frozenxid,
									  avdb->adw_frozenxid))
				avdb = tmp;
			for_xid_wrap = true;
			continue;
		}
		else if (for_xid_wrap)
			continue;			/* ignore not-at-risk DBs */
			/* 忽略没有风险的数据库 */
		else if (MultiXactIdPrecedes(tmp->adw_minmulti, multiForceLimit))
		{
			if (avdb == NULL ||
				MultiXactIdPrecedes(tmp->adw_minmulti, avdb->adw_minmulti))
				avdb = tmp;
			for_multi_wrap = true;
			continue;
		}
		else if (for_multi_wrap)
			continue;			/* ignore not-at-risk DBs */
			/* 忽略没有风险的数据库 */

		/* Find pgstat entry if any */
		/* 查找 pgstat 条目（如果有） */
		tmp->adw_entry = pgstat_fetch_stat_dbentry(tmp->adw_datid);

		/*
		 * Skip a database with no pgstat entry; it means it hasn't seen any
		 * activity.
		 *
		 * 跳过没有 pgstat 条目的数据库；这意味着它没有任何活动。
		 */
		if (!tmp->adw_entry)
			continue;

		/*
		 * Also, skip a database that appears on the database list as having
		 * been processed recently (less than autovacuum_naptime seconds ago).
		 * We do this so that we don't select a database which we just
		 * selected, but that pgstat hasn't gotten around to updating the last
		 * autovacuum time yet.
		 *
		 * 此外，跳过出现在数据库列表中且最近（不到 autovacuum_naptime 秒前）
		 * 被处理过的数据库。我们这样做是为了不选择一个我们刚刚选择过的，
		 * 但 pgstat 还没来得及更新上次自动清理时间的数据库。
		 */
		skipit = false;

		dlist_reverse_foreach(iter, &DatabaseList)
		{
			avl_dbase  *dbp = dlist_container(avl_dbase, adl_node, iter.cur);

			if (dbp->adl_datid == tmp->adw_datid)
			{
				/*
				 * Skip this database if its next_worker value falls between
				 * the current time and the current time plus naptime.
				 *
				 * 如果数据库的 next_worker 值落在当前时间与当前时间加 naptime 之间，
				 * 则跳过此数据库。
				 */
				if (!TimestampDifferenceExceeds(dbp->adl_next_worker,
												current_time, 0) &&
					!TimestampDifferenceExceeds(current_time,
												dbp->adl_next_worker,
												autovacuum_naptime * 1000))
					skipit = true;

				break;
			}
		}
		if (skipit)
			continue;

		/*
		 * Remember the db with oldest autovac time.  (If we are here, both
		 * tmp->entry and db->entry must be non-null.)
		 *
		 * 记住具有最旧自动清理时间的数据库。（如果运行到这里，
		 * tmp->entry 和 db->entry 都必须非空。）
		 */
		if (avdb == NULL ||
			tmp->adw_entry->last_autovac_time < avdb->adw_entry->last_autovac_time)
			avdb = tmp;
	}

	/* Found a database -- process it */
	/* 找到了一个数据库——处理它 */
	if (avdb != NULL)
	{
		WorkerInfo	worker;
		dlist_node *wptr;

		LWLockAcquire(AutovacuumLock, LW_EXCLUSIVE);

		/*
		 * Get a worker entry from the freelist.  We checked above, so there
		 * really should be a free slot.
		 *
		 * 从空闲列表中获取一个工作者条目。我们上面检查过，所以真的应该有一个空槽位。
		 */
		wptr = dclist_pop_head_node(&AutoVacuumShmem->av_freeWorkers);

		worker = dlist_container(WorkerInfoData, wi_links, wptr);
		worker->wi_dboid = avdb->adw_datid;
		worker->wi_proc = NULL;
		worker->wi_launchtime = GetCurrentTimestamp();

		AutoVacuumShmem->av_startingWorker = worker;

		LWLockRelease(AutovacuumLock);

		SendPostmasterSignal(PMSIGNAL_START_AUTOVAC_WORKER);

		retval = avdb->adw_datid;
	}
	else if (skipit)
	{
		/*
		 * If we skipped all databases on the list, rebuild it, because it
		 * probably contains a dropped database.
		 *
		 * 如果我们跳过了列表上的所有数据库，重新构建列表，因为它可能包含一个已删除的数据库。
		 */
		rebuild_database_list(InvalidOid);
	}

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(tmpcxt);

	return retval;
}

/*
 * launch_worker
 *
 * Wrapper for starting a worker from the launcher.  Besides actually starting
 * it, update the database list to reflect the next time that another one will
 * need to be started on the selected database.  The actual database choice is
 * left to do_start_worker.
 *
 * 从启动器启动工作者的包装器。除了实际启动它，还要更新数据库列表，以反映所选数据库
 * 下一次需要启动另一个工作者的时间。实际的数据库选择留给 do_start_worker 处理。
 *
 * This routine is also expected to insert an entry into the database list if
 * the selected database was previously absent from the list.
 *
 * 如果所选数据库之前不在列表中，该例程还应向数据库列表中插入一个条目。
 */
static void
launch_worker(TimestampTz now)
{
	Oid			dbid;
	dlist_iter	iter;

	dbid = do_start_worker();
	if (OidIsValid(dbid))
	{
		bool		found = false;

		/*
		 * Walk the database list and update the corresponding entry.  If the
		 * database is not on the list, we'll recreate the list.
		 *
		 * 遍历数据库列表并更新相应的条目。如果数据库不在列表中，我们将重新创建列表。
		 */
		dlist_foreach(iter, &DatabaseList)
		{
			avl_dbase  *avdb = dlist_container(avl_dbase, adl_node, iter.cur);

			if (avdb->adl_datid == dbid)
			{
				found = true;

				/*
				 * add autovacuum_naptime seconds to the current time, and use
				 * that as the new "next_worker" field for this database.
				 *
				 * 将 autovacuum_naptime 秒加到当前时间，并将其作为该数据库的新“next_worker”字段。
				 */
				avdb->adl_next_worker =
					TimestampTzPlusMilliseconds(now, autovacuum_naptime * 1000);

				dlist_move_head(&DatabaseList, iter.cur);
				break;
			}
		}

		/*
		 * If the database was not present in the database list, we rebuild
		 * the list.  It's possible that the database does not get into the
		 * list anyway, for example if it's a database that doesn't have a
		 * pgstat entry, but this is not a problem because we don't want to
		 * schedule workers regularly into those in any case.
		 *
		 * 如果数据库不在数据库列表中，我们重新构建列表。无论如何，数据库可能无法
		 * 进入列表，例如它是一个没有 pgstat 条目的数据库，但这没有问题，
		 * 因为在任何情况下我们都不想定期向这些数据库调度工作者。
		 */
		if (!found)
			rebuild_database_list(dbid);
	}
}

/*
 * Called from postmaster to signal a failure to fork a process to become
 * worker.  The postmaster should kill(SIGUSR2) the launcher shortly
 * after calling this function.
 *
 * 由 postmaster 调用，以发信号表示 fork 进程变身为工作者失败。
 * 调用此函数后不久，postmaster 应向启动器发送 kill(SIGUSR2) 信号。
 */
void
AutoVacWorkerFailed(void)
{
	AutoVacuumShmem->av_signal[AutoVacForkFailed] = true;
}

/* SIGUSR2: a worker is up and running, or just finished, or failed to fork */
/* SIGUSR2: 一个工作者已启动并运行，或刚完成，或 fork 失败 */
static void
avl_sigusr2_handler(SIGNAL_ARGS)
{
	got_SIGUSR2 = true;
	SetLatch(MyLatch);
}


/********************************************************************
 *					  AUTOVACUUM WORKER CODE
 *
 *					  自动清理工作者代码
 ********************************************************************/

/*
 * Main entry point for autovacuum worker processes.
 *
 * 自动清理工作者进程的主入口点。
 */
void
AutoVacWorkerMain(const void *startup_data, size_t startup_data_len)
{
	sigjmp_buf	local_sigjmp_buf;
	Oid			dbid;

	Assert(startup_data_len == 0);

	/* Release postmaster's working memory context */
	if (PostmasterContext)
	{
		MemoryContextDelete(PostmasterContext);
		PostmasterContext = NULL;
	}

	MyBackendType = B_AUTOVAC_WORKER;
	init_ps_display(NULL);

	Assert(GetProcessingMode() == InitProcessing);

	/*
	 * Set up signal handlers.  We operate on databases much like a regular
	 * backend, so we use the same signal handling.  See equivalent code in
	 * tcop/postgres.c.
	 */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);

	/*
	 * SIGINT is used to signal canceling the current table's vacuum; SIGTERM
	 * means abort and exit cleanly, and SIGQUIT means abandon ship.
	 *
	 * SIGINT 用于发信号取消当前表的清理；SIGTERM 表示中止并正常退出，SIGQUIT 表示弃船。
	 */
	pqsignal(SIGINT, StatementCancelHandler);
	pqsignal(SIGTERM, die);
	/* SIGQUIT handler was already set up by InitPostmasterChild */

	InitializeTimeouts();		/* establishes SIGALRM handler */

	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, SIG_IGN);
	pqsignal(SIGFPE, FloatExceptionHandler);
	pqsignal(SIGCHLD, SIG_DFL);

	/*
	 * Create a per-backend PGPROC struct in shared memory.  We must do this
	 * before we can use LWLocks or access any shared memory.
	 */
	InitProcess();

	/* Early initialization */
	/* 早期初始化 */
	BaseInit();

	/*
	 * If an exception is encountered, processing resumes here.
	 *
	 * Unlike most auxiliary processes, we don't attempt to continue
	 * processing after an error; we just clean up and exit.  The autovac
	 * launcher is responsible for spawning another worker later.
	 *
	 * Note that we use sigsetjmp(..., 1), so that the prevailing signal mask
	 * (to wit, BlockSig) will be restored when longjmp'ing to here.  Thus,
	 * signals other than SIGQUIT will be blocked until we exit.  It might
	 * seem that this policy makes the HOLD_INTERRUPTS() call redundant, but
	 * it is not since InterruptPending might be set already.
	 *
	 * 如果遇到异常，处理将从这里恢复。
	 *
	 * 与大多数辅助进程不同，我们不会在发生错误后尝试继续处理；我们只是清理并退出。
	 * 自动清理启动器负责稍后启动另一个工作者。
	 *
	 * 请注意，我们使用 sigsetjmp(..., 1)，以便在 longjmp 到此处时恢复
	 * 当时的信号掩码（即 BlockSig）。因此，在完成错误退出之前，
	 * 除 SIGQUIT 以外的信号将被阻塞。这种策略看起来似乎使
	 * HOLD_INTERRUPTS() 调用变得多余，但事实并非如此，因为
	 * InterruptPending 可能已经被设置了。
	 */
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/* since not using PG_TRY, must reset error stack by hand */
		error_context_stack = NULL;

		/* Prevents interrupts while cleaning up */
		/* 在清理时防止中断 */
		HOLD_INTERRUPTS();

		/* Report the error to the server log */
		/* 将错误报告给服务器日志 */
		EmitErrorReport();

		/*
		 * We can now go away.  Note that because we called InitProcess, a
		 * callback was registered to do ProcKill, which will clean up
		 * necessary state.
		 *
		 * 我们现在可以离开了。请注意，因为我们调用了 InitProcess，
		 * 一个回调函数已经被注册用来执行 ProcKill，它将清理必要的状态。
		 */
		proc_exit(0);
	}

	/* We can now handle ereport(ERROR) */
	PG_exception_stack = &local_sigjmp_buf;

	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

	/*
	 * Set always-secure search path, so malicious users can't redirect user
	 * code (e.g. pg_index.indexprs).  (That code runs in a
	 * SECURITY_RESTRICTED_OPERATION sandbox, so malicious users could not
	 * take control of the entire autovacuum worker in any case.)
	 *
	 * 设置始终安全的 search_path，以便恶意用户无法重定向用户代码
	 *（例如 pg_index.indexprs）。（该代码在 SECURITY_RESTRICTED_OPERATION 
	 * 沙箱中运行，因此在任何情况下恶意用户都无法控制整个自动清理工作者。）
	 */
	SetConfigOption("search_path", "", PGC_SUSET, PGC_S_OVERRIDE);

	/*
	 * Force zero_damaged_pages OFF in the autovac process, even if it is set
	 * in postgresql.conf.  We don't really want such a dangerous option being
	 * applied non-interactively.
	 *
	 * 在自动清理进程中强制关闭 zero_damaged_pages，即使它在 postgresql.conf 
	 * 中已设置。我们真的不想在非交互环境下应用这样一个危险的选项。
	 */
	SetConfigOption("zero_damaged_pages", "false", PGC_SUSET, PGC_S_OVERRIDE);

	/*
	 * Force settable timeouts off to avoid letting these settings prevent
	 * regular maintenance from being executed.
	 */
	SetConfigOption("statement_timeout", "0", PGC_SUSET, PGC_S_OVERRIDE);
	SetConfigOption("transaction_timeout", "0", PGC_SUSET, PGC_S_OVERRIDE);
	SetConfigOption("lock_timeout", "0", PGC_SUSET, PGC_S_OVERRIDE);
	SetConfigOption("idle_in_transaction_session_timeout", "0",
					PGC_SUSET, PGC_S_OVERRIDE);

	/*
	 * Force default_transaction_isolation to READ COMMITTED.  We don't want
	 * to pay the overhead of serializable mode, nor add any risk of causing
	 * deadlocks or delaying other transactions.
	 *
	 * 强制将 default_transaction_isolation 设置为 READ COMMITTED。
	 * 我们不想承担序列化模式的开销，也不想增加导致死锁或延迟其他事务的风险。
	 */
	SetConfigOption("default_transaction_isolation", "read committed",
					PGC_SUSET, PGC_S_OVERRIDE);

	/*
	 * Force synchronous replication off to allow regular maintenance even if
	 * we are waiting for standbys to connect. This is important to ensure we
	 * aren't blocked from performing anti-wraparound tasks.
	 *
	 * 强制关闭同步复制，以便即使在等待备库连接时也能允许进行常规维护。
	 * 这对于确保我们不被阻止执行反回转任务至关重要。
	 */
	if (synchronous_commit > SYNCHRONOUS_COMMIT_LOCAL_FLUSH)
		SetConfigOption("synchronous_commit", "local",
						PGC_SUSET, PGC_S_OVERRIDE);

	/*
	 * Even when system is configured to use a different fetch consistency,
	 * for autovac we always want fresh stats.
	 */
	SetConfigOption("stats_fetch_consistency", "none", PGC_SUSET, PGC_S_OVERRIDE);

	/*
	 * Get the info about the database we're going to work on.
	 *
	 * 获取我们将要处理的数据库的信息。
	 */
	LWLockAcquire(AutovacuumLock, LW_EXCLUSIVE);

	/*
	 * beware of startingWorker being INVALID; this should normally not
	 * happen, but if a worker fails after forking and before this, the
	 * launcher might have decided to remove it from the queue and start
	 * again.
	 *
	 * 注意 startingWorker 可能是无效的 (INVALID)；这通常不应该发生，
	 * 但如果工作者在 fork 之后和此操作之前失败，启动器可能已经决定将其
	 * 从队列中移除并再次启动。
	 */
	if (AutoVacuumShmem->av_startingWorker != NULL)
	{
		MyWorkerInfo = AutoVacuumShmem->av_startingWorker;
		dbid = MyWorkerInfo->wi_dboid;
		MyWorkerInfo->wi_proc = MyProc;

		/* insert into the running list */
		/* 插入到运行列表中 */
		dlist_push_head(&AutoVacuumShmem->av_runningWorkers,
						&MyWorkerInfo->wi_links);

		/*
		 * remove from the "starting" pointer, so that the launcher can start
		 * a new worker if required
		 *
		 * 从“starting”指针中移除，以便启动器可以在需要时启动一个新的工作者
		 */
		AutoVacuumShmem->av_startingWorker = NULL;
		LWLockRelease(AutovacuumLock);

		on_shmem_exit(FreeWorkerInfo, 0);

		/* wake up the launcher */
		/* 唤醒启动器 */
		if (AutoVacuumShmem->av_launcherpid != 0)
			kill(AutoVacuumShmem->av_launcherpid, SIGUSR2);
	}
	else
	{
		/* no worker entry for me, go away */
		/* 没有我的工作者条目，退出 */
		elog(WARNING, "autovacuum worker started without a worker entry");
		dbid = InvalidOid;
		LWLockRelease(AutovacuumLock);
	}

	if (OidIsValid(dbid))
	{
		char		dbname[NAMEDATALEN];

		/*
		 * Report autovac startup to the cumulative stats system.  We
		 * deliberately do this before InitPostgres, so that the
		 * last_autovac_time will get updated even if the connection attempt
		 * fails.  This is to prevent autovac from getting "stuck" repeatedly
		 * selecting an unopenable database, rather than making any progress
		 * on stuff it can connect to.
		 *
		 * 向累积统计系统报告自动清理启动。我们刻意在 InitPostgres 之前执行此操作，
		 * 以便即使连接尝试失败，last_autovac_time 也会得到更新。
		 * 这是为了防止自动清理因重复选择无法打开的数据库而“卡住”，
		 * 而不是在它可以连接的内容上取得任何进展。
		 */
		pgstat_report_autovac(dbid);

		/*
		 * Connect to the selected database, specifying no particular user,
		 * and ignoring datallowconn.  Collect the database's name for
		 * display.
		 *
		 * Note: if we have selected a just-deleted database (due to using
		 * stale stats info), we'll fail and exit here.
		 *
		 * 连接到所选数据库，不指定特定用户，并忽略 datallowconn。收集数据库名称以供显示。
		 *
		 * 注意：如果我们选择了一个刚删除的数据库（由于使用了过时的统计信息），
		 * 我们将在这里失败并退出。
		 */
		InitPostgres(NULL, dbid, NULL, InvalidOid,
					 INIT_PG_OVERRIDE_ALLOW_CONNS,
					 dbname);
		SetProcessingMode(NormalProcessing);
		set_ps_display(dbname);
		ereport(DEBUG1,
				(errmsg_internal("autovacuum: processing database \"%s\"", dbname)));

		if (PostAuthDelay)
			pg_usleep(PostAuthDelay * 1000000L);

		/* And do an appropriate amount of work */
		/* 并执行适当数量的工作 */
		recentXid = ReadNextTransactionId();
		recentMulti = ReadNextMultiXactId();
		do_autovacuum();
	}

	/*
	 * The launcher will be notified of my death in ProcKill, *if* we managed
	 * to get a worker slot at all
	 *
	 * 如果我们确实获得了一个工作者插槽，那么启动器将在 ProcKill 中得知我的死亡。
	 */

	/* All done, go away */
	/* 全部完成，退出 */
	proc_exit(0);
}

/*
 * Return a WorkerInfo to the free list
 *
 * 将一个 WorkerInfo 返回到空闲列表
 */
static void
FreeWorkerInfo(int code, Datum arg)
{
	if (MyWorkerInfo != NULL)
	{
		LWLockAcquire(AutovacuumLock, LW_EXCLUSIVE);

		/*
		 * Wake the launcher up so that he can launch a new worker immediately
		 * if required.  We only save the launcher's PID in local memory here;
		 * the actual signal will be sent when the PGPROC is recycled.  Note
		 * that we always do this, so that the launcher can rebalance the cost
		 * limit setting of the remaining workers.
		 *
		 * We somewhat ignore the risk that the launcher changes its PID
		 * between us reading it and the actual kill; we expect ProcKill to be
		 * called shortly after us, and we assume that PIDs are not reused too
		 * quickly after a process exits.
		 *
		 * 唤醒启动器，以便他在需要时可以立即启动一个新的工作者。我们这里只在本地内存中
		 * 保存启动器的 PID；实际信号将在回收 PGPROC 时发送。请注意，我们总是执行此操作，
		 * 以便启动器可以重新平衡剩余工作者的代价限制设置。
		 *
		 * 我们在某种程度上忽略了在读取 PID 到实际执行 kill 之间启动器更改其 PID 的风险；
		 * 我们预期在其后不久调用 ProcKill，并且我们假设 PID 在进程退出后不会太快被重用。
		 */
		AutovacuumLauncherPid = AutoVacuumShmem->av_launcherpid;

		dlist_delete(&MyWorkerInfo->wi_links);
		MyWorkerInfo->wi_dboid = InvalidOid;
		MyWorkerInfo->wi_tableoid = InvalidOid;
		MyWorkerInfo->wi_sharedrel = false;
		MyWorkerInfo->wi_proc = NULL;
		MyWorkerInfo->wi_launchtime = 0;
		pg_atomic_clear_flag(&MyWorkerInfo->wi_dobalance);
		dclist_push_head(&AutoVacuumShmem->av_freeWorkers,
						 &MyWorkerInfo->wi_links);
		/* not mine anymore */
		MyWorkerInfo = NULL;

		/*
		 * now that we're inactive, cause a rebalancing of the surviving
		 * workers
		 *
		 * 既然我们已经不处于活动状态，触发对幸存工作者的重新平衡
		 */
		AutoVacuumShmem->av_signal[AutoVacRebalance] = true;
		LWLockRelease(AutovacuumLock);
	}
}

/*
 * Update vacuum cost-based delay-related parameters for autovacuum workers and
 * backends executing VACUUM or ANALYZE using the value of relevant GUCs and
 * global state. This must be called during setup for vacuum and after every
 * config reload to ensure up-to-date values.
 *
 * 为自动清理工作者和使用相关 GUC 值和全局状态执行 VACUUM 或 ANALYZE 的后端
 * 更新基于清理代价延迟相关的参数。必须在清理设置期间以及每次重新加载配置后调用此函数，
 * 以确保使用最新的值。
 */
void
VacuumUpdateCosts(void)
{
	if (MyWorkerInfo)
	{
		if (av_storage_param_cost_delay >= 0)
			vacuum_cost_delay = av_storage_param_cost_delay;
		else if (autovacuum_vac_cost_delay >= 0)
			vacuum_cost_delay = autovacuum_vac_cost_delay;
		else
			/* fall back to VacuumCostDelay */
			vacuum_cost_delay = VacuumCostDelay;

		AutoVacuumUpdateCostLimit();
	}
	else
	{
		/* Must be explicit VACUUM or ANALYZE */
		/* 必须是显式的 VACUUM 或 ANALYZE */
		vacuum_cost_delay = VacuumCostDelay;
		vacuum_cost_limit = VacuumCostLimit;
	}

	/*
	 * If configuration changes are allowed to impact VacuumCostActive, make
	 * sure it is updated.
	 *
	 * 如果允许配置更改影响 VacuumCostActive，请确保其得到更新。
	 */
	if (VacuumFailsafeActive)
		Assert(!VacuumCostActive);
	else if (vacuum_cost_delay > 0)
		VacuumCostActive = true;
	else
	{
		VacuumCostActive = false;
		VacuumCostBalance = 0;
	}

	/*
	 * Since the cost logging requires a lock, avoid rendering the log message
	 * in case we are using a message level where the log wouldn't be emitted.
	 *
	 * 由于代价日志记录需要锁，因此如果我们使用的消息级别不会发出日志，请避免渲染日志消息。
	 */
	if (MyWorkerInfo && message_level_is_interesting(DEBUG2))
	{
		Oid			dboid,
					tableoid;

		Assert(!LWLockHeldByMe(AutovacuumLock));

		LWLockAcquire(AutovacuumLock, LW_SHARED);
		dboid = MyWorkerInfo->wi_dboid;
		tableoid = MyWorkerInfo->wi_tableoid;
		LWLockRelease(AutovacuumLock);

		elog(DEBUG2,
			 "Autovacuum VacuumUpdateCosts(db=%u, rel=%u, dobalance=%s, cost_limit=%d, cost_delay=%g active=%s failsafe=%s)",
			 dboid, tableoid, pg_atomic_unlocked_test_flag(&MyWorkerInfo->wi_dobalance) ? "no" : "yes",
			 vacuum_cost_limit, vacuum_cost_delay,
			 vacuum_cost_delay > 0 ? "yes" : "no",
			 VacuumFailsafeActive ? "yes" : "no");
	}
}

/*
 * Update vacuum_cost_limit with the correct value for an autovacuum worker,
 * given the value of other relevant cost limit parameters and the number of
 * workers across which the limit must be balanced. Autovacuum workers must
 * call this regularly in case av_nworkersForBalance has been updated by
 * another worker or by the autovacuum launcher. They must also call it after a
 * config reload.
 *
 * 根据其他相关的代价限制参数值以及必须平衡限制的工作者数量，为自动清理工作者更新 
 * vacuum_cost_limit 为正确的值。自动清理工作者必须在 av_nworkersForBalance 
 * 被其他工作者或自动清理启动器更新的情况下定期调用此函数。它们还必须在重新加载配置后调用它。
 */
void
AutoVacuumUpdateCostLimit(void)
{
	if (!MyWorkerInfo)
		return;

	/*
	 * note: in cost_limit, zero also means use value from elsewhere, because
	 * zero is not a valid value.
	 *
	 * 注意：在 cost_limit 中，零也表示使用来自其他地方的值，因为零不是有效值。
	 */

	if (av_storage_param_cost_limit > 0)
		vacuum_cost_limit = av_storage_param_cost_limit;
	else
	{
		int			nworkers_for_balance;

		if (autovacuum_vac_cost_limit > 0)
			vacuum_cost_limit = autovacuum_vac_cost_limit;
		else
			vacuum_cost_limit = VacuumCostLimit;

		/* Only balance limit if no cost-related storage parameters specified */
		/* 仅在未指定代价相关存储参数时才平衡限制 */
		if (pg_atomic_unlocked_test_flag(&MyWorkerInfo->wi_dobalance))
			return;

		Assert(vacuum_cost_limit > 0);

		nworkers_for_balance = pg_atomic_read_u32(&AutoVacuumShmem->av_nworkersForBalance);

		/* There is at least 1 autovac worker (this worker) */
		/* 至少有 1 个自动清理工作者（即该工作者） */
		if (nworkers_for_balance <= 0)
			elog(ERROR, "nworkers_for_balance must be > 0");

		vacuum_cost_limit = Max(vacuum_cost_limit / nworkers_for_balance, 1);
	}
}

/*
 * autovac_recalculate_workers_for_balance
 *		Recalculate the number of workers to consider, given cost-related
 *		storage parameters and the current number of active workers.
 *
 * Caller must hold the AutovacuumLock in at least shared mode to access
 * worker->wi_proc.
 *
 * 根据代价相关的存储参数和当前活动工作者的数量，重新计算要考虑的工作者数量。
 *
 * 调用者必须持有至少共享模式的 AutovacuumLock 才能访问 worker->wi_proc。
 */
static void
autovac_recalculate_workers_for_balance(void)
{
	dlist_iter	iter;
	int			orig_nworkers_for_balance;
	int			nworkers_for_balance = 0;

	Assert(LWLockHeldByMe(AutovacuumLock));

	orig_nworkers_for_balance =
		pg_atomic_read_u32(&AutoVacuumShmem->av_nworkersForBalance);

	dlist_foreach(iter, &AutoVacuumShmem->av_runningWorkers)
	{
		WorkerInfo	worker = dlist_container(WorkerInfoData, wi_links, iter.cur);

		if (worker->wi_proc == NULL ||
			pg_atomic_unlocked_test_flag(&worker->wi_dobalance))
			continue;

		nworkers_for_balance++;
	}

	if (nworkers_for_balance != orig_nworkers_for_balance)
		pg_atomic_write_u32(&AutoVacuumShmem->av_nworkersForBalance,
							nworkers_for_balance);
}

/*
 * get_database_list
 *		Return a list of all databases found in pg_database.
 *
 * The list and associated data is allocated in the caller's memory context,
 * which is in charge of ensuring that it's properly cleaned up afterwards.
 *
 * Note: this is the only function in which the autovacuum launcher uses a
 * transaction.  Although we aren't attached to any particular database and
 * therefore can't access most catalogs, we do have enough infrastructure
 * to do a seqscan on pg_database.
 *
 * 返回在 pg_database 中找到的所有数据库的列表。
 *
 * 该列表及其关联数据分配在调用者的内存上下文中，调用者负责确保之后正确清理。
 *
 * 注意：这是自动清理启动器使用事务的唯一函数。虽然我们没有附加到任何特定的数据库，
 * 因此无法访问大多数目录，但我们确实有足够的基础设施对 pg_database 
 * 进行顺序扫描 (seqscan)。
 */
static List *
get_database_list(void)
{
	List	   *dblist = NIL;
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tup;
	MemoryContext resultcxt;

	/* This is the context that we will allocate our output data in */
	/* 这是我们将分配输出数据的上下文 */
	resultcxt = CurrentMemoryContext;

	/*
	 * Start a transaction so we can access pg_database.
	 *
	 * 启动事务以便访问 pg_database。
	 */
	StartTransactionCommand();

	rel = table_open(DatabaseRelationId, AccessShareLock);
	scan = table_beginscan_catalog(rel, 0, NULL);

	while (HeapTupleIsValid(tup = heap_getnext(scan, ForwardScanDirection)))
	{
		Form_pg_database pgdatabase = (Form_pg_database) GETSTRUCT(tup);
		avw_dbase  *avdb;
		MemoryContext oldcxt;

		/*
		 * If database has partially been dropped, we can't, nor need to,
		 * vacuum it.
		 *
		 * 如果数据库已部分删除，我们既不能也不需要对其进行清理。
		 */
		if (database_is_invalid_form(pgdatabase))
		{
			elog(DEBUG2,
				 "autovacuum: skipping invalid database \"%s\"",
				 NameStr(pgdatabase->datname));
			continue;
		}

		/*
		 * Allocate our results in the caller's context, not the
		 * transaction's. We do this inside the loop, and restore the original
		 * context at the end, so that leaky things like heap_getnext() are
		 * not called in a potentially long-lived context.
		 *
		 * 在调用者的上下文中分配结果，而不是事务的上下文。我们在循环内部执行此操作，
		 * 并在结束时恢复原始上下文，以便像 heap_getnext() 这样可能有泄漏
		 * 的操作不会在可能长期存在的上下文中调用。
		 */
		oldcxt = MemoryContextSwitchTo(resultcxt);

		avdb = (avw_dbase *) palloc(sizeof(avw_dbase));

		avdb->adw_datid = pgdatabase->oid;
		avdb->adw_name = pstrdup(NameStr(pgdatabase->datname));
		avdb->adw_frozenxid = pgdatabase->datfrozenxid;
		avdb->adw_minmulti = pgdatabase->datminmxid;
		/* this gets set later: */
		/* 这将在稍后设置： */
		avdb->adw_entry = NULL;

		dblist = lappend(dblist, avdb);
		MemoryContextSwitchTo(oldcxt);
	}

	table_endscan(scan);
	table_close(rel, AccessShareLock);

	CommitTransactionCommand();

	/* Be sure to restore caller's memory context */
	/* 务必恢复调用者的内存上下文 */
	MemoryContextSwitchTo(resultcxt);

	return dblist;
}

/*
 * Process a database table-by-table
 *
 * Note that CHECK_FOR_INTERRUPTS is supposed to be used in certain spots in
 * order not to ignore shutdown commands for too long.
 *
 * 逐表处理数据库。
 *
 * 请注意，CHECK_FOR_INTERRUPTS 应该在某些位置使用，以免忽略关闭命令太久。
 */
static void
do_autovacuum(void)
{
	Relation	classRel;
	HeapTuple	tuple;
	TableScanDesc relScan;
	Form_pg_database dbForm;
	List	   *table_oids = NIL;
	List	   *orphan_oids = NIL;
	HASHCTL		ctl;
	HTAB	   *table_toast_map;
	ListCell   *volatile cell;
	BufferAccessStrategy bstrategy;
	ScanKeyData key;
	TupleDesc	pg_class_desc;
	int			effective_multixact_freeze_max_age;
	bool		did_vacuum = false;
	bool		found_concurrent_worker = false;
	int			i;

	/*
	 * StartTransactionCommand and CommitTransactionCommand will automatically
	 * switch to other contexts.  We need this one to keep the list of
	 * relations to vacuum/analyze across transactions.
	 *
	 * StartTransactionCommand 和 CommitTransactionCommand 会自动切换到其他上下文。
	 * 我们需要这个上下文来跨事务保存待清理/分析的关系列表。
	 */
	AutovacMemCxt = AllocSetContextCreate(TopMemoryContext,
										  "Autovacuum worker",
										  ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(AutovacMemCxt);

	/* Start a transaction so our commands have one to play into. */
	/* 启动事务以便我们的命令可以在其中执行。 */
	StartTransactionCommand();

	/*
	 * This injection point is put in a transaction block to work with a wait
	 * that uses a condition variable.
	 *
	 * 此注入点被放置在事务块中，以便与使用条件变量的等待配合工作。
	 */
	INJECTION_POINT("autovacuum-worker-start", NULL);

	/*
	 * Compute the multixact age for which freezing is urgent.  This is
	 * normally autovacuum_multixact_freeze_max_age, but may be less if we are
	 * short of multixact member space.
	 *
	 * 计算冻结变得紧急的 multixact 年龄。通常是 autovacuum_multixact_freeze_max_age，
	 * 但如果我们 multixact 成员空间不足，可能会更小。
	 */
	effective_multixact_freeze_max_age = MultiXactMemberFreezeThreshold();

	/*
	 * Find the pg_database entry and select the default freeze ages. We use
	 * zero in template and nonconnectable databases, else the system-wide
	 * default.
	 *
	 * 查找 pg_database 条目并选择默认冻结年龄。在模板数据库和不可连接的
	 * 数据库中我们使用零，否则使用系统全局默认值。
	 */
	tuple = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(MyDatabaseId));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for database %u", MyDatabaseId);
	dbForm = (Form_pg_database) GETSTRUCT(tuple);

	if (dbForm->datistemplate || !dbForm->datallowconn)
	{
		default_freeze_min_age = 0;
		default_freeze_table_age = 0;
		default_multixact_freeze_min_age = 0;
		default_multixact_freeze_table_age = 0;
	}
	else
	{
		default_freeze_min_age = vacuum_freeze_min_age;
		default_freeze_table_age = vacuum_freeze_table_age;
		default_multixact_freeze_min_age = vacuum_multixact_freeze_min_age;
		default_multixact_freeze_table_age = vacuum_multixact_freeze_table_age;
	}

	ReleaseSysCache(tuple);

	/* StartTransactionCommand changed elsewhere */
	/* StartTransactionCommand 在其他地方更改了 */
	MemoryContextSwitchTo(AutovacMemCxt);

	classRel = table_open(RelationRelationId, AccessShareLock);

	/* create a copy so we can use it after closing pg_class */
	/* 创建一个副本以便在关闭 pg_class 后使用 */
	pg_class_desc = CreateTupleDescCopy(RelationGetDescr(classRel));

	/* create hash table for toast <-> main relid mapping */
	/* 为 toast <-> 主表 relid 映射创建哈希表 */
	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(av_relation);

	table_toast_map = hash_create("TOAST to main relid map",
								  100,
								  &ctl,
								  HASH_ELEM | HASH_BLOBS);

	/*
	 * Scan pg_class to determine which tables to vacuum.
	 *
	 * We do this in two passes: on the first one we collect the list of plain
	 * relations and materialized views, and on the second one we collect
	 * TOAST tables. The reason for doing the second pass is that during it we
	 * want to use the main relation's pg_class.reloptions entry if the TOAST
	 * table does not have any, and we cannot obtain it unless we know
	 * beforehand what's the main table OID.
	 *
	 * We need to check TOAST tables separately because in cases with short,
	 * wide tables there might be proportionally much more activity in the
	 * TOAST table than in its parent.
	 *
	 * 扫描 pg_class 以确定要清理哪些表。
	 *
	 * 我们分两轮进行：第一轮收集普通关系和物化视图的列表，第二轮收集 TOAST 表。
	 * 进行第二轮的原因是，在此期间如果 TOAST 表没有任何条目，我们想要使用
	 * 主关系的 pg_class.reloptions 条目，除非我们预先知道主表的 OID，
	 * 否则我们无法获得该条目。
	 *
	 * 我们需要单独检查 TOAST 表，因为在表短而宽的情况下，TOAST 表的活动可能比其父表按比例多得多。
	 */
	relScan = table_beginscan_catalog(classRel, 0, NULL);

	/*
	 * On the first pass, we collect main tables to vacuum, and also the main
	 * table relid to TOAST relid mapping.
	 *
	 * 在第一轮处理中，我们收集要清理的主表，以及主表 relid 到 TOAST relid 的映射。
	 */
	while ((tuple = heap_getnext(relScan, ForwardScanDirection)) != NULL)
	{
		Form_pg_class classForm = (Form_pg_class) GETSTRUCT(tuple);
		PgStat_StatTabEntry *tabentry;
		AutoVacOpts *relopts;
		Oid			relid;
		bool		dovacuum;
		bool		doanalyze;
		bool		wraparound;

		if (classForm->relkind != RELKIND_RELATION &&
			classForm->relkind != RELKIND_MATVIEW)
			continue;

		relid = classForm->oid;

		/*
		 * Check if it is a temp table (presumably, of some other backend's).
		 * We cannot safely process other backends' temp tables.
		 *
		 * 检查是否为临时表（推测是由于其他某个后端进程产生的）。
		 * 我们无法安全地处理其他后端的临时表。
		 */
		if (classForm->relpersistence == RELPERSISTENCE_TEMP)
		{
			/*
			 * We just ignore it if the owning backend is still active and
			 * using the temporary schema.  Also, for safety, ignore it if the
			 * namespace doesn't exist or isn't a temp namespace after all.
			 *
			 * 如果所属后端仍处于活动状态并使用临时模式，我们直接忽略它。
			 * 此外，为了安全起见，如果命名空间不存在或终究不是临时命名空间，也忽略它。
			 */
			if (checkTempNamespaceStatus(classForm->relnamespace) == TEMP_NAMESPACE_IDLE)
			{
				/*
				 * The table seems to be orphaned -- although it might be that
				 * the owning backend has already deleted it and exited; our
				 * pg_class scan snapshot is not necessarily up-to-date
				 * anymore, so we could be looking at a committed-dead entry.
				 * Remember it so we can try to delete it later.
				 *
				 * 该表似乎已孤立——尽管可能是所属后端已经删除了它并退出；
				 * 我们的 pg_class 扫描快照不一定还是最新的，所以我们可能看到的
				 * 是一个已提交的死条目。记住它，以便之后尝试删除。
				 */
				orphan_oids = lappend_oid(orphan_oids, relid);
			}
			continue;
		}

		/* Fetch reloptions and the pgstat entry for this table */
		/* 获取该表的 reloptions 和 pgstat 条目 */
		relopts = extract_autovac_opts(tuple, pg_class_desc);
		tabentry = pgstat_fetch_stat_tabentry_ext(classForm->relisshared,
												  relid);

		/* Check if it needs vacuum or analyze */
		/* 检查是否需要执行 vacuum 或 analyze */
		relation_needs_vacanalyze(relid, relopts, classForm, tabentry,
								  effective_multixact_freeze_max_age,
								  &dovacuum, &doanalyze, &wraparound);

		/* Relations that need work are added to table_oids */
		/* 需要处理的关系被添加到 table_oids 中 */
		if (dovacuum || doanalyze)
			table_oids = lappend_oid(table_oids, relid);

		/*
		 * Remember TOAST associations for the second pass.  Note: we must do
		 * this whether or not the table is going to be vacuumed, because we
		 * don't automatically vacuum toast tables along the parent table.
		 *
		 * 记住 TOAST 关联以便在第二轮处理中使用。
		 * 注意：无论表是否要进行清理，我们都必须这样做，因为我们不会自动清理主表的 toast 表。
		 */
		if (OidIsValid(classForm->reltoastrelid))
		{
			av_relation *hentry;
			bool		found;

			hentry = hash_search(table_toast_map,
								 &classForm->reltoastrelid,
								 HASH_ENTER, &found);

			if (!found)
			{
				/* hash_search already filled in the key */
				hentry->ar_relid = relid;
				hentry->ar_hasrelopts = false;
				if (relopts != NULL)
				{
					hentry->ar_hasrelopts = true;
					memcpy(&hentry->ar_reloptions, relopts,
						   sizeof(AutoVacOpts));
				}
			}
		}

		/* Release stuff to avoid per-relation leakage */
		/* 释放资源以避免每个关系产生的泄漏 */
		if (relopts)
			pfree(relopts);
		if (tabentry)
			pfree(tabentry);
	}

	table_endscan(relScan);

	/* second pass: check TOAST tables */
	/* 第二轮：检查 TOAST 表 */
	ScanKeyInit(&key,
				Anum_pg_class_relkind,
				BTEqualStrategyNumber, F_CHAREQ,
				CharGetDatum(RELKIND_TOASTVALUE));

	relScan = table_beginscan_catalog(classRel, 1, &key);
	while ((tuple = heap_getnext(relScan, ForwardScanDirection)) != NULL)
	{
		Form_pg_class classForm = (Form_pg_class) GETSTRUCT(tuple);
		PgStat_StatTabEntry *tabentry;
		Oid			relid;
		AutoVacOpts *relopts;
		bool		free_relopts = false;
		bool		dovacuum;
		bool		doanalyze;
		bool		wraparound;

		/*
		 * We cannot safely process other backends' temp tables, so skip 'em.
		 *
		 * 我们无法安全地处理其他后端的临时表，因此跳过它们。
		 */
		if (classForm->relpersistence == RELPERSISTENCE_TEMP)
			continue;

		relid = classForm->oid;

		/*
		 * fetch reloptions -- if this toast table does not have them, try the
		 * main rel
		 *
		 * 获取 reloptions —— 如果此 toast 表没有，尝试使用主表
		 */
		relopts = extract_autovac_opts(tuple, pg_class_desc);
		if (relopts)
			free_relopts = true;
		else
		{
			av_relation *hentry;
			bool		found;

			hentry = hash_search(table_toast_map, &relid, HASH_FIND, &found);
			if (found && hentry->ar_hasrelopts)
				relopts = &hentry->ar_reloptions;
		}

		/* Fetch the pgstat entry for this table */
		/* 获取该表的 pgstat 条目 */
		tabentry = pgstat_fetch_stat_tabentry_ext(classForm->relisshared,
												  relid);

		relation_needs_vacanalyze(relid, relopts, classForm, tabentry,
								  effective_multixact_freeze_max_age,
								  &dovacuum, &doanalyze, &wraparound);

		/* ignore analyze for toast tables */
		/* 为 toast 表忽略分析 (analyze) */
		if (dovacuum)
			table_oids = lappend_oid(table_oids, relid);

		/* Release stuff to avoid leakage */
		/* 释放资源以避免泄漏 */
		if (free_relopts)
			pfree(relopts);
		if (tabentry)
			pfree(tabentry);
	}

	table_endscan(relScan);
	table_close(classRel, AccessShareLock);

	/*
	 * Recheck orphan temporary tables, and if they still seem orphaned, drop
	 * them.  We'll eat a transaction per dropped table, which might seem
	 * excessive, but we should only need to do anything as a result of a
	 * previous backend crash, so this should not happen often enough to
	 * justify "optimizing".  Using separate transactions ensures that we
	 * don't bloat the lock table if there are many temp tables to be dropped,
	 * and it ensures that we don't lose work if a deletion attempt fails.
	 *
	 * 重新检查孤立的临时表，如果它们仍然看起来是孤立的，则将其删除。我们将为每个
	 * 删除的表消耗一个事务，这看起来可能有点多，但我们应该只需要在之前的后端崩溃
	 * 后才执行此操作，所以这不应该经常发生以至于需要“优化”。使用单独的事务可以
	 * 确保如果有很多临时表要删除，我们不会使锁表膨胀，并确保如果删除尝试失败，
	 * 我们不会丢失工作。
	 */
	foreach(cell, orphan_oids)
	{
		Oid			relid = lfirst_oid(cell);
		Form_pg_class classForm;
		ObjectAddress object;

		/*
		 * Check for user-requested abort.
		 *
		 * 检查用户请求的中止。
		 */
		CHECK_FOR_INTERRUPTS();

		/*
		 * Try to lock the table.  If we can't get the lock immediately,
		 * somebody else is using (or dropping) the table, so it's not our
		 * concern anymore.  Having the lock prevents race conditions below.
		 *
		 * 尝试锁定表。如果无法立即获得锁，则说明有其他人在使用（或删除）该表，
		 * 因此我们不再关心它。拥有该锁可以防止下面的竞态条件。
		 */
		if (!ConditionalLockRelationOid(relid, AccessExclusiveLock))
			continue;

		/*
		 * Re-fetch the pg_class tuple and re-check whether it still seems to
		 * be an orphaned temp table.  If it's not there or no longer the same
		 * relation, ignore it.
		 *
		 * 重新获取 pg_class 元组并重新检查它是否仍然看起来是一个孤立的临时表。
		 * 如果它不存在或不再是相同的关系，则忽略它。
		 */
		tuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relid));
		if (!HeapTupleIsValid(tuple))
		{
			/* be sure to drop useless lock so we don't bloat lock table */
			/* 务必释放无用的锁，以免使锁表膨胀 */
			UnlockRelationOid(relid, AccessExclusiveLock);
			continue;
		}
		classForm = (Form_pg_class) GETSTRUCT(tuple);

		/*
		 * Make all the same tests made in the loop above.  In event of OID
		 * counter wraparound, the pg_class entry we have now might be
		 * completely unrelated to the one we saw before.
		 *
		 * 执行与上述循环中完全相同的测试。在 OID 计数器回绕的情况下，
		 * 我们现在拥有的 pg_class 条目可能与我们之前看到的完全无关。
		 */
		if (!((classForm->relkind == RELKIND_RELATION ||
			   classForm->relkind == RELKIND_MATVIEW) &&
			  classForm->relpersistence == RELPERSISTENCE_TEMP))
		{
			UnlockRelationOid(relid, AccessExclusiveLock);
			continue;
		}

		if (checkTempNamespaceStatus(classForm->relnamespace) != TEMP_NAMESPACE_IDLE)
		{
			UnlockRelationOid(relid, AccessExclusiveLock);
			continue;
		}

		/*
		 * Try to lock the temp namespace, too.  Even though we have lock on
		 * the table itself, there's a risk of deadlock against an incoming
		 * backend trying to clean out the temp namespace, in case this table
		 * has dependencies (such as sequences) that the backend's
		 * performDeletion call might visit in a different order.  If we can
		 * get AccessShareLock on the namespace, that's sufficient to ensure
		 * we're not running concurrently with RemoveTempRelations.  If we
		 * can't, back off and let RemoveTempRelations do its thing.
		 *
		 * 也尝试锁定临时命名空间。即使我们已经锁定了表本身，但在所属后端尝试清理
		 * 临时命名空间的情况下，仍然存在死锁风险，因为该表可能具有 backend 调用的
		 * performDeletion 可能会以不同顺序访问的依赖项（如序列）。如果我们可以
		 * 获得命名空间上的 AccessShareLock，这足以确保我们不会与 
		 * RemoveTempRelations 同时运行。如果不能，则退出并让 RemoveTempRelations 执行其操作。
		 */
		if (!ConditionalLockDatabaseObject(NamespaceRelationId,
										   classForm->relnamespace, 0,
										   AccessShareLock))
		{
			UnlockRelationOid(relid, AccessExclusiveLock);
			continue;
		}

		/* OK, let's delete it */
		/* 好了，删除它 */
		ereport(LOG,
				(errmsg("autovacuum: dropping orphan temp table \"%s.%s.%s\"",
						get_database_name(MyDatabaseId),
						get_namespace_name(classForm->relnamespace),
						NameStr(classForm->relname))));

		/*
		 * Deletion might involve TOAST table access, so ensure we have a
		 * valid snapshot.
		 *
		 * 删除可能涉及 TOAST 表访问，因此请确保我们有一个有效的快照。
		 */
		PushActiveSnapshot(GetTransactionSnapshot());

		object.classId = RelationRelationId;
		object.objectId = relid;
		object.objectSubId = 0;
		performDeletion(&object, DROP_CASCADE,
						PERFORM_DELETION_INTERNAL |
						PERFORM_DELETION_QUIETLY |
						PERFORM_DELETION_SKIP_EXTENSIONS);

		/*
		 * To commit the deletion, end current transaction and start a new
		 * one.  Note this also releases the locks we took.
		 */
		PopActiveSnapshot();
		CommitTransactionCommand();
		StartTransactionCommand();

		/* StartTransactionCommand changed current memory context */
		/* StartTransactionCommand 更改了当前内存上下文 */
		MemoryContextSwitchTo(AutovacMemCxt);
	}

	/*
	 * Optionally, create a buffer access strategy object for VACUUM to use.
	 * We use the same BufferAccessStrategy object for all tables VACUUMed by
	 * this worker to prevent autovacuum from blowing out shared buffers.
	 *
	 * VacuumBufferUsageLimit being set to 0 results in
	 * GetAccessStrategyWithSize returning NULL, effectively meaning we can
	 * use up to all of shared buffers.
	 *
	 * If we later enter failsafe mode on any of the tables being vacuumed, we
	 * will cease use of the BufferAccessStrategy only for that table.
	 *
	 * XXX should we consider adding code to adjust the size of this if
	 * VacuumBufferUsageLimit changes?
	 *
	 * 可选地，创建一个供 VACUUM 使用的缓冲访问策略对象。对于该工作者清理的所有表，
	 * 我们使用相同的 BufferAccessStrategy 对象，以防止自动清理耗尽共享缓冲区。
	 *
	 * VacuumBufferUsageLimit 设置为 0 会导致 GetAccessStrategyWithSize 返回 NULL，
	 * 实际上意味着我们最多可以使用所有贡献的共享缓冲区。
	 *
	 * 如果我们稍后在任何正在清理的表上进入安全模式，我们将仅为该表停止使用 BufferAccessStrategy。
	 *
	 * XXX 我们是否应该考虑增加代码，在 VacuumBufferUsageLimit 更改时调整此项的大小？
	 */
	bstrategy = GetAccessStrategyWithSize(BAS_VACUUM, VacuumBufferUsageLimit);

	/*
	 * create a memory context to act as fake PortalContext, so that the
	 * contexts created in the vacuum code are cleaned up for each table.
	 *
	 * 创建一个内存上下文作为伪 PortalContext，以便为清理代码中创建的每个表清理上下文。
	 */
	PortalContext = AllocSetContextCreate(AutovacMemCxt,
										  "Autovacuum Portal",
										  ALLOCSET_DEFAULT_SIZES);

	/*
	 * Perform operations on collected tables.
	 *
	 * 对收集到的表执行操作。
	 */
	foreach(cell, table_oids)
	{
		Oid			relid = lfirst_oid(cell);
		HeapTuple	classTup;
		autovac_table *tab;
		bool		isshared;
		bool		skipit;
		dlist_iter	iter;

		CHECK_FOR_INTERRUPTS();

		/*
		 * Check for config changes before processing each collected table.
		 *
		 * 在处理每个收集到的表之前检查配置更改。
		 */
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);

			/*
			 * You might be tempted to bail out if we see autovacuum is now
			 * disabled.  Must resist that temptation -- this might be a
			 * for-wraparound emergency worker, in which case that would be
			 * entirely inappropriate.
			 *
			 * 如果你看到自动清理现在已被禁用，你可能会想要退出。必须抵制这种诱惑——
			 * 这可能是一个为了应对回绕 (wraparound) 的紧急工作者，
			 * 那种情况下（退出）将是完全不合适的。
			 */
		}

		/*
		 * Find out whether the table is shared or not.  (It's slightly
		 * annoying to fetch the syscache entry just for this, but in typical
		 * cases it adds little cost because table_recheck_autovac would
		 * refetch the entry anyway.  We could buy that back by copying the
		 * tuple here and passing it to table_recheck_autovac, but that
		 * increases the odds of that function working with stale data.)
		 *
		 * 找出表是否为共享表。（仅为此目的而获取 syscache 条目有点烦人，但在典型情况下，
		 * 它几乎不会增加成本，因为 table_recheck_autovac 无论如何都会重新获取该条目。
		 * 我们可以通过在此处复制元组并将其传递给 table_recheck_autovac 来换回这一点，
		 * 但这会增加该函数使用过时数据的可能性。）
		 */
		classTup = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
		if (!HeapTupleIsValid(classTup))
			continue;			/* somebody deleted the rel, forget it */
		isshared = ((Form_pg_class) GETSTRUCT(classTup))->relisshared;
		ReleaseSysCache(classTup);

		/*
		 * Hold schedule lock from here until we've claimed the table.  We
		 * also need the AutovacuumLock to walk the worker array, but that one
		 * can just be a shared lock.
		 *
		 * 从这里开始持有调度锁，直到我们认领了该表。我们还需要 AutovacuumLock 
		 * 来遍历工作者数组，但那个锁只需是共享锁即可。
		 */
		LWLockAcquire(AutovacuumScheduleLock, LW_EXCLUSIVE);
		LWLockAcquire(AutovacuumLock, LW_SHARED);

		/*
		 * Check whether the table is being vacuumed concurrently by another
		 * worker.
		 *
		 * 检查该表是否正由另一个工作者并发清理。
		 */
		skipit = false;
		dlist_foreach(iter, &AutoVacuumShmem->av_runningWorkers)
		{
			WorkerInfo	worker = dlist_container(WorkerInfoData, wi_links, iter.cur);

			/* ignore myself */
			/* 忽略我自己 */
			if (worker == MyWorkerInfo)
				continue;

			/* ignore workers in other databases (unless table is shared) */
			/* 忽略其他数据库中的工作者（除非表是共享的） */
			if (!worker->wi_sharedrel && worker->wi_dboid != MyDatabaseId)
				continue;

			if (worker->wi_tableoid == relid)
			{
				skipit = true;
				found_concurrent_worker = true;
				break;
			}
		}
		LWLockRelease(AutovacuumLock);
		if (skipit)
		{
			LWLockRelease(AutovacuumScheduleLock);
			continue;
		}

		/*
		 * Store the table's OID in shared memory before releasing the
		 * schedule lock, so that other workers don't try to vacuum it
		 * concurrently.  (We claim it here so as not to hold
		 * AutovacuumScheduleLock while rechecking the stats.)
		 *
		 * 在释放调度锁之前将表的 OID 存储在共享内存中，以便其他工作者不会尝试并发清理它。
		 *（我们在这里认领它，以免在重新检查统计信息时持有 AutovacuumScheduleLock。）
		 */
		MyWorkerInfo->wi_tableoid = relid;
		MyWorkerInfo->wi_sharedrel = isshared;
		LWLockRelease(AutovacuumScheduleLock);

		/*
		 * Check whether pgstat data still says we need to vacuum this table.
		 * It could have changed if something else processed the table while
		 * we weren't looking. This doesn't entirely close the race condition,
		 * but it is very small.
		 *
		 * 检查 pgstat 数据是否仍然显示我们需要清理这张表。如果当我们没看时有其他进程
		 * 处理了该表，情况可能会发生变化。这并不能完全消除竞态条件，但它非常小。
		 */
		MemoryContextSwitchTo(AutovacMemCxt);
		tab = table_recheck_autovac(relid, table_toast_map, pg_class_desc,
									effective_multixact_freeze_max_age);
		if (tab == NULL)
		{
			/* someone else vacuumed the table, or it went away */
			/* 其他人清理了该表，或者表已经消失了 */
			LWLockAcquire(AutovacuumScheduleLock, LW_EXCLUSIVE);
			MyWorkerInfo->wi_tableoid = InvalidOid;
			MyWorkerInfo->wi_sharedrel = false;
			LWLockRelease(AutovacuumScheduleLock);
			continue;
		}

		/*
		 * Save the cost-related storage parameter values in global variables
		 * for reference when updating vacuum_cost_delay and vacuum_cost_limit
		 * during vacuuming this table.
		 *
		 * 将代价相关的存储参数值保存在全局变量中，以便在清理此表期间更新 
		 * vacuum_cost_delay 和 vacuum_cost_limit 时参考。
		 */
		av_storage_param_cost_delay = tab->at_storage_param_vac_cost_delay;
		av_storage_param_cost_limit = tab->at_storage_param_vac_cost_limit;

		/*
		 * We only expect this worker to ever set the flag, so don't bother
		 * checking the return value. We shouldn't have to retry.
		 *
		 * 我们只预期该工作者会设置该标志，因此不麻烦去检查返回值。我们不应该需要重试。
		 */
		if (tab->at_dobalance)
			pg_atomic_test_set_flag(&MyWorkerInfo->wi_dobalance);
		else
			pg_atomic_clear_flag(&MyWorkerInfo->wi_dobalance);

		LWLockAcquire(AutovacuumLock, LW_SHARED);
		autovac_recalculate_workers_for_balance();
		LWLockRelease(AutovacuumLock);

		/*
		 * We wait until this point to update cost delay and cost limit
		 * values, even though we reloaded the configuration file above, so
		 * that we can take into account the cost-related storage parameters.
		 *
		 * 即使我们在上面重新加载了配置文件，我们也要等到这一点才更新代价延迟和代价限制值，
		 * 以便我们可以考虑到与代价相关的存储参数。
		 */
		VacuumUpdateCosts();


		/* clean up memory before each iteration */
		/* 在每次迭代之前清理内存 */
		MemoryContextReset(PortalContext);

		/*
		 * Save the relation name for a possible error message, to avoid a
		 * catalog lookup in case of an error.  If any of these return NULL,
		 * then the relation has been dropped since last we checked; skip it.
		 * Note: they must live in a long-lived memory context because we call
		 * vacuum and analyze in different transactions.
		 *
		 * 保存关系名称以便可能出现的错误消息，从而避免在发生错误时进行目录查找。
		 * 如果其中任何一个返回 NULL，则说明自上次检查以来该关系已被删除；请跳过。
		 * 注意：它们必须存在于长期存活的内存上下文中，因为我们在不同的事务中调用 
		 * vacuum 和 analyze。
		 */

		tab->at_relname = get_rel_name(tab->at_relid);
		tab->at_nspname = get_namespace_name(get_rel_namespace(tab->at_relid));
		tab->at_datname = get_database_name(MyDatabaseId);
		if (!tab->at_relname || !tab->at_nspname || !tab->at_datname)
			goto deleted;

		/*
		 * We will abort vacuuming the current table if something errors out,
		 * and continue with the next one in schedule; in particular, this
		 * happens if we are interrupted with SIGINT.
		 *
		 * 如果某些操作出错，我们将中止当前表的清理，并继续调度中的下一个；
		 * 特别是，如果我们被 SIGINT 中断，就会发生这种情况。
		 */
		PG_TRY();
		{
			/* Use PortalContext for any per-table allocations */
			/* 为任何每表分配使用 PortalContext */
			MemoryContextSwitchTo(PortalContext);

			/* have at it */
			/* 开始执行 */
			autovacuum_do_vac_analyze(tab, bstrategy);

			/*
			 * Clear a possible query-cancel signal, to avoid a late reaction
			 * to an automatically-sent signal because of vacuuming the
			 * current table (we're done with it, so it would make no sense to
			 * cancel at this point.)
			 */
			QueryCancelPending = false;
		}
		PG_CATCH();
		{
			/*
			 * Abort the transaction, start a new one, and proceed with the
			 * next table in our list.
			 */
			HOLD_INTERRUPTS();
			if (tab->at_params.options & VACOPT_VACUUM)
				errcontext("automatic vacuum of table \"%s.%s.%s\"",
						   tab->at_datname, tab->at_nspname, tab->at_relname);
			else
				errcontext("automatic analyze of table \"%s.%s.%s\"",
						   tab->at_datname, tab->at_nspname, tab->at_relname);
			EmitErrorReport();

			/* this resets ProcGlobal->statusFlags[i] too */
			/* 这也会重置 ProcGlobal->statusFlags[i] */
			AbortOutOfAnyTransaction();
			FlushErrorState();
			MemoryContextReset(PortalContext);

			/* restart our transaction for the following operations */
			/* 为后续操作重新启动我们的事务 */
			StartTransactionCommand();
			RESUME_INTERRUPTS();
		}
		PG_END_TRY();

		/* Make sure we're back in AutovacMemCxt */
		/* 确保我们回到了 AutovacMemCxt */
		MemoryContextSwitchTo(AutovacMemCxt);

		did_vacuum = true;

		/* ProcGlobal->statusFlags[i] are reset at the next end of xact */
		/* ProcGlobal->statusFlags[i] 在下次事务结束时重置 */

		/* be tidy */
		/* 保持整洁 */
deleted:
		if (tab->at_datname != NULL)
			pfree(tab->at_datname);
		if (tab->at_nspname != NULL)
			pfree(tab->at_nspname);
		if (tab->at_relname != NULL)
			pfree(tab->at_relname);
		pfree(tab);

		/*
		 * Remove my info from shared memory.  We set wi_dobalance on the
		 * assumption that we are more likely than not to vacuum a table with
		 * no cost-related storage parameters next, so we want to claim our
		 * share of I/O as soon as possible to avoid thrashing the global
		 * balance.
		 */
		LWLockAcquire(AutovacuumScheduleLock, LW_EXCLUSIVE);
		MyWorkerInfo->wi_tableoid = InvalidOid;
		MyWorkerInfo->wi_sharedrel = false;
		LWLockRelease(AutovacuumScheduleLock);
		pg_atomic_test_set_flag(&MyWorkerInfo->wi_dobalance);
	}

	list_free(table_oids);

	/*
	 * Perform additional work items, as requested by backends.
	 */
	LWLockAcquire(AutovacuumLock, LW_EXCLUSIVE);
	for (i = 0; i < NUM_WORKITEMS; i++)
	{
		AutoVacuumWorkItem *workitem = &AutoVacuumShmem->av_workItems[i];

		if (!workitem->avw_used)
			continue;
		if (workitem->avw_active)
			continue;
		if (workitem->avw_database != MyDatabaseId)
			continue;

		/* claim this one, and release lock while performing it */
		/* 认领此项，并在执行期间释放锁 */
		workitem->avw_active = true;
		LWLockRelease(AutovacuumLock);

		PushActiveSnapshot(GetTransactionSnapshot());
		perform_work_item(workitem);
		if (ActiveSnapshotSet())	/* transaction could have aborted */
			/* 事务可能已经中止 */
			PopActiveSnapshot();

		/*
		 * Check for config changes before acquiring lock for further jobs.
		 */
		CHECK_FOR_INTERRUPTS();
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
			VacuumUpdateCosts();
		}

		LWLockAcquire(AutovacuumLock, LW_EXCLUSIVE);

		/* and mark it done */
		/* 并将其标记为已完成 */
		workitem->avw_active = false;
		workitem->avw_used = false;
	}
	LWLockRelease(AutovacuumLock);

	/*
	 * We leak table_toast_map here (among other things), but since we're
	 * going away soon, it's not a problem.
	 */

	/*
	 * Update pg_database.datfrozenxid, and truncate pg_xact if possible. We
	 * only need to do this once, not after each table.
	 *
	 * Even if we didn't vacuum anything, it may still be important to do
	 * this, because one indirect effect of vac_update_datfrozenxid() is to
	 * update TransamVariables->xidVacLimit.  That might need to be done even
	 * if we haven't vacuumed anything, because relations with older
	 * relfrozenxid values or other databases with older datfrozenxid values
	 * might have been dropped, allowing xidVacLimit to advance.
	 *
	 * However, it's also important not to do this blindly in all cases,
	 * because when autovacuum=off this will restart the autovacuum launcher.
	 * If we're not careful, an infinite loop can result, where workers find
	 * no work to do and restart the launcher, which starts another worker in
	 * the same database that finds no work to do.  To prevent that, we skip
	 * this if (1) we found no work to do and (2) we skipped at least one
	 * table due to concurrent autovacuum activity.  In that case, the other
	 * worker has already done it, or will do so when it finishes.
	 *
	 * 更新 pg_database.datfrozenxid，并尽可能截断 pg_xact。
	 * 我们只需要在所有表处理完后执行一次。
	 *
	 * 即使我们没有清理任何内容，执行此操作可能仍然很重要，因为 vac_update_datfrozenxid() 
	 * 的一个间接影响是更新 TransamVariables->xidVacLimit。即使我们没有清理任何内容，
	 * 这也可能需要执行，因为具有较旧 relfrozenxid 值的关系或其他具有
	 * 较旧 datfrozenxid 值的数据库可能已被删除，从而允许 xidVacLimit 推进。
	 *
	 * 然而，在所有情况下都不盲目执行此操作也同样重要，因为当 autovacuum=off 时，
	 * 这将重新启动自动清理启动器。如果我们不小心，可能会产生无限循环：
	 * 工作者发现没有工作要做，从而重新启动启动器，而启动器在同一个数据库中启动
	 * 另一个工作者，结果该工作者也发现没有工作要做。
	 * 为了防止这种情况，如果 (1) 我们发现没有工作要做，并且 (2) 由于并发自动清理活动，
	 * 我们跳过了至少一张表，我们就跳过此操作。在那种情况下，
	 * 另一个工作者已经完成了，或者在它结束时会执行此操作。
	 */
	if (did_vacuum || !found_concurrent_worker)
		vac_update_datfrozenxid();

	/* Finally close out the last transaction. */
	/* 最后结束最后一个事务。 */
	CommitTransactionCommand();
}

/*
 * Execute a previously registered work item.
 *
 * 执行之前注册的工作项。
 */
static void
perform_work_item(AutoVacuumWorkItem *workitem)
{
	char	   *cur_datname = NULL;
	char	   *cur_nspname = NULL;
	char	   *cur_relname = NULL;

	/*
	 * Note we do not store table info in MyWorkerInfo, since this is not
	 * vacuuming proper.
	 */

	/*
	 * Save the relation name for a possible error message, to avoid a catalog
	 * lookup in case of an error.  If any of these return NULL, then the
	 * relation has been dropped since last we checked; skip it.
	 */
	Assert(CurrentMemoryContext == AutovacMemCxt);

	cur_relname = get_rel_name(workitem->avw_relation);
	cur_nspname = get_namespace_name(get_rel_namespace(workitem->avw_relation));
	cur_datname = get_database_name(MyDatabaseId);
	if (!cur_relname || !cur_nspname || !cur_datname)
		goto deleted2;

	autovac_report_workitem(workitem, cur_nspname, cur_relname);

	/* clean up memory before each work item */
	/* 在每个工作项之前清理内存 */
	MemoryContextReset(PortalContext);

	/*
	 * We will abort the current work item if something errors out, and
	 * continue with the next one; in particular, this happens if we are
	 * interrupted with SIGINT.  Note that this means that the work item list
	 * can be lossy.
	 */
	PG_TRY();
	{
		/* Use PortalContext for any per-work-item allocations */
		/* 为任何每工作项分配使用 PortalContext */
		MemoryContextSwitchTo(PortalContext);

		/*
		 * Have at it.  Functions called here are responsible for any required
		 * user switch and sandbox.
		 */
		switch (workitem->avw_type)
		{
			case AVW_BRINSummarizeRange:
				DirectFunctionCall2(brin_summarize_range,
									ObjectIdGetDatum(workitem->avw_relation),
									Int64GetDatum((int64) workitem->avw_blockNumber));
				break;
			default:
				elog(WARNING, "unrecognized work item found: type %d",
					 workitem->avw_type);
				break;
		}

		/*
		 * Clear a possible query-cancel signal, to avoid a late reaction to
		 * an automatically-sent signal because of vacuuming the current table
		 * (we're done with it, so it would make no sense to cancel at this
		 * point.)
		 */
		QueryCancelPending = false;
	}
	PG_CATCH();
	{
		/*
		 * Abort the transaction, start a new one, and proceed with the next
		 * table in our list.
		 */
		HOLD_INTERRUPTS();
		errcontext("processing work entry for relation \"%s.%s.%s\"",
				   cur_datname, cur_nspname, cur_relname);
		EmitErrorReport();

		/* this resets ProcGlobal->statusFlags[i] too */
		AbortOutOfAnyTransaction();
		FlushErrorState();
		MemoryContextReset(PortalContext);

		/* restart our transaction for the following operations */
		StartTransactionCommand();
		RESUME_INTERRUPTS();
	}
	PG_END_TRY();

	/* Make sure we're back in AutovacMemCxt */
	MemoryContextSwitchTo(AutovacMemCxt);

	/* We intentionally do not set did_vacuum here */
	/* 我们故意不在这里设置 did_vacuum */

	/* be tidy */
deleted2:
	if (cur_datname)
		pfree(cur_datname);
	if (cur_nspname)
		pfree(cur_nspname);
	if (cur_relname)
		pfree(cur_relname);
}

/*
 * extract_autovac_opts
 *
 * Given a relation's pg_class tuple, return a palloc'd copy of the
 * AutoVacOpts portion of reloptions, if set; otherwise, return NULL.
 *
 * Note: callers do not have a relation lock on the table at this point,
 * so the table could have been dropped, and its catalog rows gone, after
 * we acquired the pg_class row.  If pg_class had a TOAST table, this would
 * be a risk; fortunately, it doesn't.
 *
 * 给定关系的 pg_class 元组，如果设置了 reloptions 的 AutoVacOpts 部分，
 * 则返回其分配好的副本；否则返回 NULL。
 *
 * 注意：此时调用者没有表上的关系锁，因此在我们获取 pg_class 行后，
 * 表可能已被删除，其目录行也已消失。如果 pg_class 有 TOAST 表，
 * 这将是一个风险；幸运的是，它没有。
 */
static AutoVacOpts *
extract_autovac_opts(HeapTuple tup, TupleDesc pg_class_desc)
{
	bytea	   *relopts;
	AutoVacOpts *av;

	Assert(((Form_pg_class) GETSTRUCT(tup))->relkind == RELKIND_RELATION ||
		   ((Form_pg_class) GETSTRUCT(tup))->relkind == RELKIND_MATVIEW ||
		   ((Form_pg_class) GETSTRUCT(tup))->relkind == RELKIND_TOASTVALUE);

	relopts = extractRelOptions(tup, pg_class_desc, NULL);
	if (relopts == NULL)
		return NULL;

	av = palloc(sizeof(AutoVacOpts));
	memcpy(av, &(((StdRdOptions *) relopts)->autovacuum), sizeof(AutoVacOpts));
	pfree(relopts);

	return av;
}


/*
 * table_recheck_autovac
 *
 * Recheck whether a table still needs vacuum or analyze.  Return value is a
 * valid autovac_table pointer if it does, NULL otherwise.
 *
 * Note that the returned autovac_table does not have the name fields set.
 *
 * 重新检查表是否仍然需要 vacuum 或 analyze。如果需要，返回值为有效的 
 * autovac_table 指针，否则返回 NULL。请注意，返回的 autovac_table 
 * 未设置名称字段。
 */
static autovac_table *
table_recheck_autovac(Oid relid, HTAB *table_toast_map,
					  TupleDesc pg_class_desc,
					  int effective_multixact_freeze_max_age)
{
	Form_pg_class classForm;
	HeapTuple	classTup;
	bool		dovacuum;
	bool		doanalyze;
	autovac_table *tab = NULL;
	bool		wraparound;
	AutoVacOpts *avopts;
	bool		free_avopts = false;

	/* fetch the relation's relcache entry */
	/* 获取关系的 relcache 条目 */
	classTup = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(classTup))
		return NULL;
	classForm = (Form_pg_class) GETSTRUCT(classTup);

	/*
	 * Get the applicable reloptions.  If it is a TOAST table, try to get the
	 * main table reloptions if the toast table itself doesn't have.
	 */
	avopts = extract_autovac_opts(classTup, pg_class_desc);
	if (avopts)
		free_avopts = true;
	else if (classForm->relkind == RELKIND_TOASTVALUE &&
			 table_toast_map != NULL)
	{
		av_relation *hentry;
		bool		found;

		hentry = hash_search(table_toast_map, &relid, HASH_FIND, &found);
		if (found && hentry->ar_hasrelopts)
			avopts = &hentry->ar_reloptions;
	}

	recheck_relation_needs_vacanalyze(relid, avopts, classForm,
									  effective_multixact_freeze_max_age,
									  &dovacuum, &doanalyze, &wraparound);

	/* OK, it needs something done */
	/* 好了，需要做点什么 */
	if (doanalyze || dovacuum)
	{
		int			freeze_min_age;
		int			freeze_table_age;
		int			multixact_freeze_min_age;
		int			multixact_freeze_table_age;
		int			log_min_duration;

		/*
		 * Calculate the vacuum cost parameters and the freeze ages.  If there
		 * are options set in pg_class.reloptions, use them; in the case of a
		 * toast table, try the main table too.  Otherwise use the GUC
		 * defaults, autovacuum's own first and plain vacuum second.
		 */

		/* -1 in autovac setting means use log_autovacuum_min_duration */
		/* 自动清理设置中的 -1 表示使用 log_autovacuum_min_duration */
		log_min_duration = (avopts && avopts->log_min_duration >= 0)
			? avopts->log_min_duration
			: Log_autovacuum_min_duration;

		/* these do not have autovacuum-specific settings */
		/* 这些没有自动清理特定的设置 */
		freeze_min_age = (avopts && avopts->freeze_min_age >= 0)
			? avopts->freeze_min_age
			: default_freeze_min_age;

		freeze_table_age = (avopts && avopts->freeze_table_age >= 0)
			? avopts->freeze_table_age
			: default_freeze_table_age;

		multixact_freeze_min_age = (avopts &&
									avopts->multixact_freeze_min_age >= 0)
			? avopts->multixact_freeze_min_age
			: default_multixact_freeze_min_age;

		multixact_freeze_table_age = (avopts &&
									  avopts->multixact_freeze_table_age >= 0)
			? avopts->multixact_freeze_table_age
			: default_multixact_freeze_table_age;

		tab = palloc(sizeof(autovac_table));
		tab->at_relid = relid;
		tab->at_sharedrel = classForm->relisshared;

		/*
		 * Select VACUUM options.  Note we don't say VACOPT_PROCESS_TOAST, so
		 * that vacuum() skips toast relations.  Also note we tell vacuum() to
		 * skip vac_update_datfrozenxid(); we'll do that separately.
		 */
		tab->at_params.options =
			(dovacuum ? (VACOPT_VACUUM |
						 VACOPT_PROCESS_MAIN |
						 VACOPT_SKIP_DATABASE_STATS) : 0) |
			(doanalyze ? VACOPT_ANALYZE : 0) |
			(!wraparound ? VACOPT_SKIP_LOCKED : 0);

		/*
		 * index_cleanup and truncate are unspecified at first in autovacuum.
		 * They will be filled in with usable values using their reloptions
		 * (or reloption defaults) later.
		 */
		tab->at_params.index_cleanup = VACOPTVALUE_UNSPECIFIED;
		tab->at_params.truncate = VACOPTVALUE_UNSPECIFIED;
		/* As of now, we don't support parallel vacuum for autovacuum */
		/* 截至目前，我们不支持自动清理的并行清理 */
		tab->at_params.nworkers = -1;
		tab->at_params.freeze_min_age = freeze_min_age;
		tab->at_params.freeze_table_age = freeze_table_age;
		tab->at_params.multixact_freeze_min_age = multixact_freeze_min_age;
		tab->at_params.multixact_freeze_table_age = multixact_freeze_table_age;
		tab->at_params.is_wraparound = wraparound;
		tab->at_params.log_min_duration = log_min_duration;
		tab->at_params.toast_parent = InvalidOid;

		/*
		 * Later, in vacuum_rel(), we check reloptions for any
		 * vacuum_max_eager_freeze_failure_rate override.
		 */
		tab->at_params.max_eager_freeze_failure_rate = vacuum_max_eager_freeze_failure_rate;
		tab->at_storage_param_vac_cost_limit = avopts ?
			avopts->vacuum_cost_limit : 0;
		tab->at_storage_param_vac_cost_delay = avopts ?
			avopts->vacuum_cost_delay : -1;
		tab->at_relname = NULL;
		tab->at_nspname = NULL;
		tab->at_datname = NULL;

		/*
		 * If any of the cost delay parameters has been set individually for
		 * this table, disable the balancing algorithm.
		 */
		tab->at_dobalance =
			!(avopts && (avopts->vacuum_cost_limit > 0 ||
						 avopts->vacuum_cost_delay >= 0));
	}

	if (free_avopts)
		pfree(avopts);
	heap_freetuple(classTup);
	return tab;
}

/*
 * recheck_relation_needs_vacanalyze
 *
 * Subroutine for table_recheck_autovac.
 *
 * Fetch the pgstat of a relation and recheck whether a relation
 * needs to be vacuumed or analyzed.
 *
 * table_recheck_autovac 的子程序。
 *
 * 获取关系的 pgstat 并重新检查关系是否需要 vacuum 或 analyze。
 */
static void
recheck_relation_needs_vacanalyze(Oid relid,
								  AutoVacOpts *avopts,
								  Form_pg_class classForm,
								  int effective_multixact_freeze_max_age,
								  bool *dovacuum,
								  bool *doanalyze,
								  bool *wraparound)
{
	PgStat_StatTabEntry *tabentry;

	/* fetch the pgstat table entry */
	/* 获取 pgstat 表条目 */
	tabentry = pgstat_fetch_stat_tabentry_ext(classForm->relisshared,
											  relid);

	relation_needs_vacanalyze(relid, avopts, classForm, tabentry,
							  effective_multixact_freeze_max_age,
							  dovacuum, doanalyze, wraparound);

	/* Release tabentry to avoid leakage */
	/* 释放 tabentry 以避免泄漏 */
	if (tabentry)
		pfree(tabentry);

	/* ignore ANALYZE for toast tables */
	/* 为 toast 表忽略 ANALYZE */
	if (classForm->relkind == RELKIND_TOASTVALUE)
		*doanalyze = false;
}

/*
 * relation_needs_vacanalyze
 *
 * Check whether a relation needs to be vacuumed or analyzed; return each into
 * "dovacuum" and "doanalyze", respectively.  Also return whether the vacuum is
 * being forced because of Xid or multixact wraparound.
 *
 * relopts is a pointer to the AutoVacOpts options (either for itself in the
 * case of a plain table, or for either itself or its parent table in the case
 * of a TOAST table), NULL if none; tabentry is the pgstats entry, which can be
 * NULL.
 *
 * A table needs to be vacuumed if the number of dead tuples exceeds a
 * threshold.  This threshold is calculated as
 *
 * threshold = vac_base_thresh + vac_scale_factor * reltuples
 * if (threshold > vac_max_thresh)
 *     threshold = vac_max_thresh;
 *
 * For analyze, the analysis done is that the number of tuples inserted,
 * deleted and updated since the last analyze exceeds a threshold calculated
 * in the same fashion as above.  Note that the cumulative stats system stores
 * the number of tuples (both live and dead) that there were as of the last
 * analyze.  This is asymmetric to the VACUUM case.
 *
 * We also force vacuum if the table's relfrozenxid is more than freeze_max_age
 * transactions back, and if its relminmxid is more than
 * multixact_freeze_max_age multixacts back.
 *
 * A table whose autovacuum_enabled option is false is
 * automatically skipped (unless we have to vacuum it due to freeze_max_age).
 * Thus autovacuum can be disabled for specific tables. Also, when the cumulative
 * stats system does not have data about a table, it will be skipped.
 *
 * A table whose vac_base_thresh value is < 0 takes the base value from the
 * autovacuum_vacuum_threshold GUC variable.  Similarly, a vac_scale_factor
 * value < 0 is substituted with the value of
 * autovacuum_vacuum_scale_factor GUC variable.  Ditto for analyze.
 *
 * 检查关系是否需要 vacuum 或 analyze；分别将结果返回到“dovacuum”和“doanalyze”中。
 * 此外，还返回是否由于 Xid 或 multixact 回绕而强制执行清理。
 *
 * relopts 是指向 AutoVacOpts 选项的指针（在普通表的情况下是其本身，
 * 或者在 TOAST 表的情况下是其本身或其父表），如果没有则为 NULL；
 * tabentry 是 pgstats 条目，可以为 NULL。
 *
 * 如果死元组的数量超过阈值，则需要对表进行清理。此阈值计算如下：
 *
 * threshold = vac_base_thresh + vac_scale_factor * reltuples
 * if (threshold > vac_max_thresh)
 *     threshold = vac_max_thresh;
 *
 * 对于分析 (analyze)，所做的分析是自上次分析以来插入、删除和更新的元组数量超过了
 * 以同样方式计算的阈值。请注意，累积统计系统存储了截至上次分析时的元组数量
 *（包括活动和死亡）。这与 VACUUM 的情况是不对称的。
 *
 * 如果表的 relfrozenxid 超过了 freeze_max_age 个事务，以及如果其 relminmxid 
 * 超过了 multixact_freeze_max_age 个 multixacts，我们也强制执行清理。
 *
 * 自动跳过 autovacuum_enabled 选项为 false 的表（除非由于 freeze_max_age 而必须清理它）。
 * 因此，可以针对特定表禁用自动清理。此外，当累积统计系统没有关于表的数据时，它将被跳过。
 *
 * vac_base_thresh 值 < 0 的表其基础值取自 autovacuum_vacuum_threshold GUC 变量。
 * 类似地，vac_scale_factor 值 < 0 会被替换为 autovacuum_vacuum_scale_factor 
 * GUC 变量的值。分析 (analyze) 情况同理。
 */
static void
relation_needs_vacanalyze(Oid relid,
						  AutoVacOpts *relopts,
						  Form_pg_class classForm,
						  PgStat_StatTabEntry *tabentry,
						  int effective_multixact_freeze_max_age,
 /* output params below */
						  bool *dovacuum,
						  bool *doanalyze,
						  bool *wraparound)
{
	bool		force_vacuum;
	bool		av_enabled;

	/* constants from reloptions or GUC variables */
	int			vac_base_thresh,
				vac_max_thresh,
				vac_ins_base_thresh,
				anl_base_thresh;
	float4		vac_scale_factor,
				vac_ins_scale_factor,
				anl_scale_factor;

	/* thresholds calculated from above constants */
	float4		vacthresh,
				vacinsthresh,
				anlthresh;

	/* number of vacuum (resp. analyze) tuples at this time */
	float4		vactuples,
				instuples,
				anltuples;

	/* freeze parameters */
	int			freeze_max_age;
	int			multixact_freeze_max_age;
	TransactionId xidForceLimit;
	TransactionId relfrozenxid;
	MultiXactId multiForceLimit;

	Assert(classForm != NULL);
	Assert(OidIsValid(relid));

	/*
	 * Determine vacuum/analyze equation parameters.  We have two possible
	 * sources: the passed reloptions (which could be a main table or a toast
	 * table), or the autovacuum GUC variables.
	 */

	/* -1 in autovac setting means use plain vacuum_scale_factor */
	/* 自动清理设置中的 -1 表示使用普通 vacuum_scale_factor */
	vac_scale_factor = (relopts && relopts->vacuum_scale_factor >= 0)
		? relopts->vacuum_scale_factor
		: autovacuum_vac_scale;

	vac_base_thresh = (relopts && relopts->vacuum_threshold >= 0)
		? relopts->vacuum_threshold
		: autovacuum_vac_thresh;

	/* -1 is used to disable max threshold */
	/* -1 用于禁用最大阈值 */
	vac_max_thresh = (relopts && relopts->vacuum_max_threshold >= -1)
		? relopts->vacuum_max_threshold
		: autovacuum_vac_max_thresh;

	vac_ins_scale_factor = (relopts && relopts->vacuum_ins_scale_factor >= 0)
		? relopts->vacuum_ins_scale_factor
		: autovacuum_vac_ins_scale;

	/* -1 is used to disable insert vacuums */
	/* -1 用于禁用插入清理 (insert vacuums) */
	vac_ins_base_thresh = (relopts && relopts->vacuum_ins_threshold >= -1)
		? relopts->vacuum_ins_threshold
		: autovacuum_vac_ins_thresh;

	anl_scale_factor = (relopts && relopts->analyze_scale_factor >= 0)
		? relopts->analyze_scale_factor
		: autovacuum_anl_scale;

	anl_base_thresh = (relopts && relopts->analyze_threshold >= 0)
		? relopts->analyze_threshold
		: autovacuum_anl_thresh;

	freeze_max_age = (relopts && relopts->freeze_max_age >= 0)
		? Min(relopts->freeze_max_age, autovacuum_freeze_max_age)
		: autovacuum_freeze_max_age;

	multixact_freeze_max_age = (relopts && relopts->multixact_freeze_max_age >= 0)
		? Min(relopts->multixact_freeze_max_age, effective_multixact_freeze_max_age)
		: effective_multixact_freeze_max_age;

	av_enabled = (relopts ? relopts->enabled : true);

	/* Force vacuum if table is at risk of wraparound */
	/* 如果表有回绕风险，则强制执行清理 */
	xidForceLimit = recentXid - freeze_max_age;
	if (xidForceLimit < FirstNormalTransactionId)
		xidForceLimit -= FirstNormalTransactionId;
	relfrozenxid = classForm->relfrozenxid;
	force_vacuum = (TransactionIdIsNormal(relfrozenxid) &&
					TransactionIdPrecedes(relfrozenxid, xidForceLimit));
	if (!force_vacuum)
	{
		MultiXactId relminmxid = classForm->relminmxid;

		multiForceLimit = recentMulti - multixact_freeze_max_age;
		if (multiForceLimit < FirstMultiXactId)
			multiForceLimit -= FirstMultiXactId;
		force_vacuum = MultiXactIdIsValid(relminmxid) &&
			MultiXactIdPrecedes(relminmxid, multiForceLimit);
	}
	*wraparound = force_vacuum;

	/* User disabled it in pg_class.reloptions?  (But ignore if at risk) */
	/* 用户在 pg_class.reloptions 中禁用了它？（但在有风险时忽略） */
	if (!av_enabled && !force_vacuum)
	{
		*doanalyze = false;
		*dovacuum = false;
		return;
	}

	/*
	 * If we found stats for the table, and autovacuum is currently enabled,
	 * make a threshold-based decision whether to vacuum and/or analyze.  If
	 * autovacuum is currently disabled, we must be here for anti-wraparound
	 * vacuuming only, so don't vacuum (or analyze) anything that's not being
	 * forced.
	 */
	if (PointerIsValid(tabentry) && AutoVacuumingActive())
	{
		float4		pcnt_unfrozen = 1;
		float4		reltuples = classForm->reltuples;
		int32		relpages = classForm->relpages;
		int32		relallfrozen = classForm->relallfrozen;

		vactuples = tabentry->dead_tuples;
		instuples = tabentry->ins_since_vacuum;
		anltuples = tabentry->mod_since_analyze;

		/* If the table hasn't yet been vacuumed, take reltuples as zero */
		/* 如果表尚未执行过清理，则将 reltuples 视为零 */
		if (reltuples < 0)
			reltuples = 0;

		/*
		 * If we have data for relallfrozen, calculate the unfrozen percentage
		 * of the table to modify insert scale factor. This helps us decide
		 * whether or not to vacuum an insert-heavy table based on the number
		 * of inserts to the more "active" part of the table.
		 */
		if (relpages > 0 && relallfrozen > 0)
		{
			/*
			 * It could be the stats were updated manually and relallfrozen >
			 * relpages. Clamp relallfrozen to relpages to avoid nonsensical
			 * calculations.
			 */
			relallfrozen = Min(relallfrozen, relpages);
			pcnt_unfrozen = 1 - ((float4) relallfrozen / relpages);
		}

		vacthresh = (float4) vac_base_thresh + vac_scale_factor * reltuples;
		if (vac_max_thresh >= 0 && vacthresh > (float4) vac_max_thresh)
			vacthresh = (float4) vac_max_thresh;

		vacinsthresh = (float4) vac_ins_base_thresh +
			vac_ins_scale_factor * reltuples * pcnt_unfrozen;
		anlthresh = (float4) anl_base_thresh + anl_scale_factor * reltuples;

		/*
		 * Note that we don't need to take special consideration for stat
		 * reset, because if that happens, the last vacuum and analyze counts
		 * will be reset too.
		 */
		if (vac_ins_base_thresh >= 0)
			elog(DEBUG3, "%s: vac: %.0f (threshold %.0f), ins: %.0f (threshold %.0f), anl: %.0f (threshold %.0f)",
				 NameStr(classForm->relname),
				 vactuples, vacthresh, instuples, vacinsthresh, anltuples, anlthresh);
		else
			elog(DEBUG3, "%s: vac: %.0f (threshold %.0f), ins: (disabled), anl: %.0f (threshold %.0f)",
				 NameStr(classForm->relname),
				 vactuples, vacthresh, anltuples, anlthresh);

		/* Determine if this table needs vacuum or analyze. */
		/* 确定此表是否需要 vacuum 或 analyze。 */
		*dovacuum = force_vacuum || (vactuples > vacthresh) ||
			(vac_ins_base_thresh >= 0 && instuples > vacinsthresh);
		*doanalyze = (anltuples > anlthresh);
	}
	else
	{
		/*
		 * Skip a table not found in stat hash, unless we have to force vacuum
		 * for anti-wrap purposes.  If it's not acted upon, there's no need to
		 * vacuum it.
		 */
		*dovacuum = force_vacuum;
		*doanalyze = false;
	}

	/* ANALYZE refuses to work with pg_statistic */
	/* ANALYZE 拒绝处理 pg_statistic */
	if (relid == StatisticRelationId)
		*doanalyze = false;
}

/*
 * autovacuum_do_vac_analyze
 *		Vacuum and/or analyze the specified table
 *
 * We expect the caller to have switched into a memory context that won't
 * disappear at transaction commit.
 *
 * 我们预期调用者已经切换到了一个不会在事务提交时消失的内存上下文中。
 */
static void
autovacuum_do_vac_analyze(autovac_table *tab, BufferAccessStrategy bstrategy)
{
	RangeVar   *rangevar;
	VacuumRelation *rel;
	List	   *rel_list;
	MemoryContext vac_context;
	MemoryContext old_context;

	/* Let pgstat know what we're doing */
	/* 让 pgstat 知道我们在做什么 */
	autovac_report_activity(tab);

	/* Create a context that vacuum() can use as cross-transaction storage */
	/* 创建一个 vacuum() 可以用作跨事务存储的上下文 */
	vac_context = AllocSetContextCreate(CurrentMemoryContext,
										"Vacuum",
										ALLOCSET_DEFAULT_SIZES);

	/* Set up one VacuumRelation target, identified by OID, for vacuum() */
	/* 为 vacuum() 设置一个由 OID 标识的 VacuumRelation 目标 */
	old_context = MemoryContextSwitchTo(vac_context);
	rangevar = makeRangeVar(tab->at_nspname, tab->at_relname, -1);
	rel = makeVacuumRelation(rangevar, tab->at_relid, NIL);
	rel_list = list_make1(rel);
	MemoryContextSwitchTo(old_context);

	vacuum(rel_list, &tab->at_params, bstrategy, vac_context, true);

	MemoryContextDelete(vac_context);
}

/*
 * autovac_report_activity
 *		Report to pgstat what autovacuum is doing
 *
 * We send a SQL string corresponding to what the user would see if the
 * equivalent command was to be issued manually.
 *
 * Note we assume that we are going to report the next command as soon as we're
 * done with the current one, and exit right after the last one, so we don't
 * bother to report "<IDLE>" or some such.
 *
 * 注意我们假设当前命令执行完毕后立即报告下一个命令，并在最后一个命令执行后立即退出，
 * 所以我们不麻烦去报告 "<IDLE>" 或类似内容。
 */
static void
autovac_report_activity(autovac_table *tab)
{
#define MAX_AUTOVAC_ACTIV_LEN (NAMEDATALEN * 2 + 56)
	char		activity[MAX_AUTOVAC_ACTIV_LEN];
	int			len;

	/* Report the command and possible options */
	/* 报告命令和可能的选项 */
	if (tab->at_params.options & VACOPT_VACUUM)
		snprintf(activity, MAX_AUTOVAC_ACTIV_LEN,
				 "autovacuum: VACUUM%s",
				 tab->at_params.options & VACOPT_ANALYZE ? " ANALYZE" : "");
	else
		snprintf(activity, MAX_AUTOVAC_ACTIV_LEN,
				 "autovacuum: ANALYZE");

	/*
	 * Report the qualified name of the relation.
	 *
	 * 报告关系的限定名称。
	 */
	len = strlen(activity);

	snprintf(activity + len, MAX_AUTOVAC_ACTIV_LEN - len,
			 " %s.%s%s", tab->at_nspname, tab->at_relname,
			 tab->at_params.is_wraparound ? " (to prevent wraparound)" : "");

	/* Set statement_timestamp() to current time for pg_stat_activity */
	SetCurrentStatementStartTimestamp();

	pgstat_report_activity(STATE_RUNNING, activity);
}

/*
 * autovac_report_workitem
 *		Report to pgstat that autovacuum is processing a work item
 *
 * 向 pgstat 报告自动清理正在处理一个工作项
 */
static void
autovac_report_workitem(AutoVacuumWorkItem *workitem,
						const char *nspname, const char *relname)
{
	char		activity[MAX_AUTOVAC_ACTIV_LEN + 12 + 2];
	char		blk[12 + 2];
	int			len;

	switch (workitem->avw_type)
	{
		case AVW_BRINSummarizeRange:
			snprintf(activity, MAX_AUTOVAC_ACTIV_LEN,
					 "autovacuum: BRIN summarize");
			break;
	}

	/*
	 * Report the qualified name of the relation, and the block number if any
	 */
	len = strlen(activity);

	if (BlockNumberIsValid(workitem->avw_blockNumber))
		snprintf(blk, sizeof(blk), " %u", workitem->avw_blockNumber);
	else
		blk[0] = '\0';

	snprintf(activity + len, MAX_AUTOVAC_ACTIV_LEN - len,
			 " %s.%s%s", nspname, relname, blk);

	/* Set statement_timestamp() to current time for pg_stat_activity */
	SetCurrentStatementStartTimestamp();

	pgstat_report_activity(STATE_RUNNING, activity);
}

/*
 * AutoVacuumingActive
 *		Check GUC vars and report whether the autovacuum process should be
 *		running.
 *
 * 检查 GUC 变量并报告自动清理进程是否应该运行。
 */
bool
AutoVacuumingActive(void)
{
	if (!autovacuum_start_daemon || !pgstat_track_counts)
		return false;
	return true;
}

/*
 * Request one work item to the next autovacuum run processing our database.
 * Return false if the request can't be recorded.
 *
 * 请求一项工作到下一次处理我们数据库的自动清理运行中。如果无法记录请求，则返回 false。
 */
bool
AutoVacuumRequestWork(AutoVacuumWorkItemType type, Oid relationId,
					  BlockNumber blkno)
{
	int			i;
	bool		result = false;

	LWLockAcquire(AutovacuumLock, LW_EXCLUSIVE);

	/*
	 * Locate an unused work item and fill it with the given data.
	 */
	for (i = 0; i < NUM_WORKITEMS; i++)
	{
		AutoVacuumWorkItem *workitem = &AutoVacuumShmem->av_workItems[i];

		if (workitem->avw_used)
			continue;

		workitem->avw_used = true;
		workitem->avw_active = false;
		workitem->avw_type = type;
		workitem->avw_database = MyDatabaseId;
		workitem->avw_relation = relationId;
		workitem->avw_blockNumber = blkno;
		result = true;

		/* done */
		break;
	}

	LWLockRelease(AutovacuumLock);

	return result;
}

/*
 * autovac_init
 *		This is called at postmaster initialization.
 *
 * All we do here is annoy the user if he got it wrong.
 *
 * 这是在 postmaster 初始化时调用的。
 * 我们在这里所做的只是在用户弄错时“打扰”一下他们。
 */
void
autovac_init(void)
{
	if (!autovacuum_start_daemon)
		return;
	else if (!pgstat_track_counts)
		ereport(WARNING,
				(errmsg("autovacuum not started because of misconfiguration"),
				 errhint("Enable the \"track_counts\" option.")));
	else
		check_av_worker_gucs();
}

/*
 * AutoVacuumShmemSize
 *		Compute space needed for autovacuum-related shared memory
 *
 * 计算自动清理相关的共享内存所需的空间
 */
Size
AutoVacuumShmemSize(void)
{
	Size		size;

	/*
	 * Need the fixed struct and the array of WorkerInfoData.
	 */
	size = sizeof(AutoVacuumShmemStruct);
	size = MAXALIGN(size);
	size = add_size(size, mul_size(autovacuum_worker_slots,
								   sizeof(WorkerInfoData)));
	return size;
}

/*
 * AutoVacuumShmemInit
 *		Allocate and initialize autovacuum-related shared memory
 *
 * 分配并初始化自动清理相关的共享内存
 */
void
AutoVacuumShmemInit(void)
{
	bool		found;

	AutoVacuumShmem = (AutoVacuumShmemStruct *)
		ShmemInitStruct("AutoVacuum Data",
						AutoVacuumShmemSize(),
						&found);

	if (!IsUnderPostmaster)
	{
		WorkerInfo	worker;
		int			i;

		Assert(!found);

		AutoVacuumShmem->av_launcherpid = 0;
		dclist_init(&AutoVacuumShmem->av_freeWorkers);
		dlist_init(&AutoVacuumShmem->av_runningWorkers);
		AutoVacuumShmem->av_startingWorker = NULL;
		memset(AutoVacuumShmem->av_workItems, 0,
			   sizeof(AutoVacuumWorkItem) * NUM_WORKITEMS);

		worker = (WorkerInfo) ((char *) AutoVacuumShmem +
							   MAXALIGN(sizeof(AutoVacuumShmemStruct)));

		/* initialize the WorkerInfo free list */
		/* 初始化 WorkerInfo 空闲列表 */
		for (i = 0; i < autovacuum_worker_slots; i++)
		{
			dclist_push_head(&AutoVacuumShmem->av_freeWorkers,
							 &worker[i].wi_links);
			pg_atomic_init_flag(&worker[i].wi_dobalance);
		}

		pg_atomic_init_u32(&AutoVacuumShmem->av_nworkersForBalance, 0);

	}
	else
		Assert(found);
}

/*
 * GUC check_hook for autovacuum_work_mem
 *
 * autovacuum_work_mem 的 GUC check_hook
 */
bool
check_autovacuum_work_mem(int *newval, void **extra, GucSource source)
{
	/*
	 * -1 indicates fallback.
	 *
	 * If we haven't yet changed the boot_val default of -1, just let it be.
	 * Autovacuum will look to maintenance_work_mem instead.
	 */
	if (*newval == -1)
		return true;

	/*
	 * We clamp manually-set values to at least 64kB.  Since
	 * maintenance_work_mem is always set to at least this value, do the same
	 * here.
	 */
	if (*newval < 64)
		*newval = 64;

	return true;
}

/*
 * Returns whether there is a free autovacuum worker slot available.
 *
 * 返回是否有可用的自动清理工作者插槽。
 */
static bool
av_worker_available(void)
{
	int			free_slots;
	int			reserved_slots;

	free_slots = dclist_count(&AutoVacuumShmem->av_freeWorkers);

	reserved_slots = autovacuum_worker_slots - autovacuum_max_workers;
	reserved_slots = Max(0, reserved_slots);

	return free_slots > reserved_slots;
}

/*
 * Emits a WARNING if autovacuum_worker_slots < autovacuum_max_workers.
 *
 * 如果 autovacuum_worker_slots < autovacuum_max_workers，则发出警告 (WARNING)。
 */
static void
check_av_worker_gucs(void)
{
	if (autovacuum_worker_slots < autovacuum_max_workers)
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"autovacuum_max_workers\" (%d) should be less than or equal to \"autovacuum_worker_slots\" (%d)",
						autovacuum_max_workers, autovacuum_worker_slots),
				 errdetail("The server will only start up to \"autovacuum_worker_slots\" (%d) autovacuum workers at a given time.",
						   autovacuum_worker_slots)));
}

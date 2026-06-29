/*-------------------------------------------------------------------------
 *
 * startup.c
 *
 * The Startup process initialises the server and performs any recovery
 * actions that have been specified. Notice that there is no "main loop"
 * since the Startup process ends as soon as initialisation is complete.
 * (in standby mode, one can think of the replay loop as a main loop,
 * though.)
 * 启动（Startup）进程初始化服务器并执行指定的任何恢复操作。
 * 请注意，由于启动进程在初始化完成后立即结束，因此没有“主循环”。
 * （不过，在备机模式下，人们可以将重放循环视为一个主循环。）
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/startup.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xlog.h"
#include "access/xlogrecovery.h"
#include "access/xlogutils.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/auxprocess.h"
#include "postmaster/startup.h"
#include "storage/ipc.h"
#include "storage/pmsignal.h"
#include "storage/procsignal.h"
#include "storage/standby.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/timeout.h"


#ifndef USE_POSTMASTER_DEATH_SIGNAL
/*
 * On systems that need to make a system call to find out if the postmaster has
 * gone away, we'll do so only every Nth call to ProcessStartupProcInterrupts().
 * This only affects how long it takes us to detect the condition while we're
 * busy replaying WAL.  Latch waits and similar which should react immediately
 * through the usual techniques.
 */
/*
 * 在需要调用系统调用来发现 postmaster 是否已退出的系统上，我们每隔 N 次调用
 * ProcessStartupProcInterrupts() 才执行一次该系统调用。
 * 这只影响我们在忙于重放 WAL 时检测到该状态所需的时间。
 * 锁存器等待等情况应当通过常规技术立即做出反应。
 */
#define POSTMASTER_POLL_RATE_LIMIT 1024
#endif

/*
 * Flags set by interrupt handlers for later service in the redo loop.
 */
/*
 * 中断处理程序设置的标志，以便稍后在 redo 循环中进行处理。
 */
static volatile sig_atomic_t got_SIGHUP = false;
static volatile sig_atomic_t shutdown_requested = false;
static volatile sig_atomic_t promote_signaled = false;

/*
 * Flag set when executing a restore command, to tell SIGTERM signal handler
 * that it's safe to just proc_exit.
 */
/*
 * 在执行 restore_command 时设置的标志，以告诉 SIGTERM 信号处理程序
 * 可以安全地直接调用 proc_exit。
 */
static volatile sig_atomic_t in_restore_command = false;

/*
 * Time at which the most recent startup operation started.
 */
/*
 * 最近一次启动操作开始的时间。
 */
static TimestampTz startup_progress_phase_start_time;

/*
 * Indicates whether the startup progress interval mentioned by the user is
 * elapsed or not. TRUE if timeout occurred, FALSE otherwise.
 */
/*
 * 指示用户指定的启动进度时间间隔是否已过去。如果超时则为 TRUE，否则为 FALSE。
 */
static volatile sig_atomic_t startup_progress_timer_expired = false;

/*
 * Time between progress updates for long-running startup operations.
 */
/*
 * 长时间运行的启动操作两次进度更新之间的时间间隔。
 */
int			log_startup_progress_interval = 10000;	/* 10 sec */

/* Signal handlers */
/* 信号处理函数 */
static void StartupProcTriggerHandler(SIGNAL_ARGS);
static void StartupProcSigHupHandler(SIGNAL_ARGS);

/* Callbacks */
/* 回调函数 */
static void StartupProcExit(int code, Datum arg);


/* --------------------------------
 *		signal handler routines
 *		信号处理程序例程
 * --------------------------------
 */

/* SIGUSR2: set flag to finish recovery */
/* SIGUSR2：设置结束恢复的标志 */
static void
StartupProcTriggerHandler(SIGNAL_ARGS)
{
	promote_signaled = true;
	WakeupRecovery();
}

/* SIGHUP: set flag to re-read config file at next convenient time */
/* SIGHUP：设置在下一次方便时重新读取配置文件的标志 */
static void
StartupProcSigHupHandler(SIGNAL_ARGS)
{
	got_SIGHUP = true;
	WakeupRecovery();
}

/* SIGTERM: set flag to abort redo and exit */
/* SIGTERM：设置中止 redo 并退出的标志 */
static void
StartupProcShutdownHandler(SIGNAL_ARGS)
{
	if (in_restore_command)
		proc_exit(1);
	else
		shutdown_requested = true;
	WakeupRecovery();
}

/*
 * Re-read the config file.
 *
 * If one of the critical walreceiver options has changed, flag xlog.c
 * to restart it.
 */
/*
 * 重新读取配置文件。
 *
 * 如果关键的 walreceiver 选项之一发生变化，通知 xlog.c 重启它。
 *
 * Function purpose: Re-read the configuration and request walreceiver restart if relevant options changed.
 * 函数作用：重新读取配置，并在关键选项更改时请求重启 walreceiver。
 */
static void
StartupRereadConfig(void)
{
	char	   *conninfo = pstrdup(PrimaryConnInfo);
	char	   *slotname = pstrdup(PrimarySlotName);
	bool		tempSlot = wal_receiver_create_temp_slot;
	bool		conninfoChanged;
	bool		slotnameChanged;
	bool		tempSlotChanged = false;

	ProcessConfigFile(PGC_SIGHUP);

	conninfoChanged = strcmp(conninfo, PrimaryConnInfo) != 0;
	slotnameChanged = strcmp(slotname, PrimarySlotName) != 0;

	/*
	 * wal_receiver_create_temp_slot is used only when we have no slot
	 * configured.  We do not need to track this change if it has no effect.
	 */
	/*
	 * wal_receiver_create_temp_slot 仅在未配置复制槽时使用。
	 * 如果该更改没有效果，我们不需要追踪它。
	 */
	if (!slotnameChanged && strcmp(PrimarySlotName, "") == 0)
		tempSlotChanged = tempSlot != wal_receiver_create_temp_slot;
	pfree(conninfo);
	pfree(slotname);

	if (conninfoChanged || slotnameChanged || tempSlotChanged)
		StartupRequestWalReceiverRestart();
}

/* Process various signals that might be sent to the startup process */
/*
 * 处理可能发送给启动进程的各种信号。
 *
 * Function purpose: Check and process pending events (SIGHUP, SIGTERM, postmaster death, proc signals, memory logs).
 * 函数作用：检查并处理待处理事件（如 GUC 重载、退出请求、主进程退出检测、内存统计）。
 */
void
ProcessStartupProcInterrupts(void)
{
#ifdef POSTMASTER_POLL_RATE_LIMIT
	static uint32 postmaster_poll_count = 0;
#endif

	/*
	 * Process any requests or signals received recently.
	 */
	/*
	 * 处理最近收到的任何请求或信号。
	 */
	if (got_SIGHUP)
	{
		got_SIGHUP = false;
		StartupRereadConfig();
	}

	/*
	 * Check if we were requested to exit without finishing recovery.
	 */
	/*
	 * 检查我们是否被请求在未完成恢复的情况下退出。
	 */
	if (shutdown_requested)
		proc_exit(1);

	/*
	 * Emergency bailout if postmaster has died.  This is to avoid the
	 * necessity for manual cleanup of all postmaster children.  Do this less
	 * frequently on systems for which we don't have signals to make that
	 * cheap.
	 */
	/*
	 * 如果 postmaster 已经死亡，则进行紧急退出。这是为了避免
	 * 需要手动清理所有 postmaster 子进程。
	 * 在我们没有低成本信号的系统上，降低该检查的频率。
	 */
	if (IsUnderPostmaster &&
#ifdef POSTMASTER_POLL_RATE_LIMIT
		postmaster_poll_count++ % POSTMASTER_POLL_RATE_LIMIT == 0 &&
#endif
		!PostmasterIsAlive())
		exit(1);

	/* Process barrier events */
	/* 处理屏障事件 */
	if (ProcSignalBarrierPending)
		ProcessProcSignalBarrier();

	/* Perform logging of memory contexts of this process */
	/* 记录该进程的内存上下文日志 */
	if (LogMemoryContextPending)
		ProcessLogMemoryContextInterrupt();
}


/* --------------------------------
 *		signal handler routines
 *		信号处理程序例程
 * --------------------------------
 */
/*
 * Function purpose: Callback on startup process exit to clean up recovery transaction environment.
 * 函数作用：启动进程退出时的回调函数，用于清理恢复事务环境。
 */
static void
StartupProcExit(int code, Datum arg)
{
	/* Shutdown the recovery environment */
	/* 关闭恢复环境 */
	if (standbyState != STANDBY_DISABLED)
		ShutdownRecoveryTransactionEnvironment();
}


/* ----------------------------------
 *	Startup Process main entry point
 *	启动进程主入口点
 * ----------------------------------
 */
/*
 * Function purpose: Main function of the Startup Process.
 * 函数作用：启动进程的主函数。
 *
 * Core workflow:
 * 核心流程：
 * 1. Initialize process type as B_STARTUP and call AuxiliaryProcessMainCommon().
 *    初始化进程类型为 B_STARTUP，并调用 AuxiliaryProcessMainCommon()。
 * 2. Register Exit callback StartupProcExit.
 *    注册退出回调函数 StartupProcExit。
 * 3. Setup signal handlers for SIGHUP, SIGTERM, SIGUSR1, SIGUSR2, etc.
 *    设置 SIGHUP, SIGTERM, SIGUSR1, SIGUSR2 等信号的处理程序。
 * 4. Register timeouts for Standby mode (deadlock, lock, query timeout).
 *    注册备机模式所需的超时（死锁超时、锁超时、备机超时）。
 * 5. Unblock signals and call StartupXLOG() to perform the actual recovery.
 *    解除信号阻塞，并调用 StartupXLOG() 进行实际的日志恢复。
 * 6. Exit with status 0 indicating success.
 *    退出并返回状态码 0，表示成功。
 */
void
StartupProcessMain(const void *startup_data, size_t startup_data_len)
{
	Assert(startup_data_len == 0);

	MyBackendType = B_STARTUP;
	AuxiliaryProcessMainCommon();

	/* Arrange to clean up at startup process exit */
	/* 安排在启动进程退出时进行清理 */
	on_shmem_exit(StartupProcExit, 0);

	/*
	 * Properly accept or ignore signals the postmaster might send us.
	 */
	/*
	 * 正确接受或忽略 postmaster 可能发送给我们的信号。
	 */
	pqsignal(SIGHUP, StartupProcSigHupHandler); /* reload config file */ /* 重新加载配置文件 */
	pqsignal(SIGINT, SIG_IGN);	/* ignore query cancel */ /* 忽略查询取消 */
	pqsignal(SIGTERM, StartupProcShutdownHandler);	/* request shutdown */ /* 请求关机 */
	/* SIGQUIT handler was already set up by InitPostmasterChild */
	/* SIGQUIT 处理程序已由 InitPostmasterChild 设置 */
	InitializeTimeouts();		/* establishes SIGALRM handler */ /* 建立 SIGALRM 处理程序 */
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, StartupProcTriggerHandler);

	/*
	 * Reset some signals that are accepted by postmaster but not here
	 */
	/*
	 * 重置一些由 postmaster 接受但在此处不接受的信号
	 */
	pqsignal(SIGCHLD, SIG_DFL);

	/*
	 * Register timeouts needed for standby mode
	 */
	/*
	 * 注册备机模式所需的超时
	 */
	RegisterTimeout(STANDBY_DEADLOCK_TIMEOUT, StandbyDeadLockHandler);
	RegisterTimeout(STANDBY_TIMEOUT, StandbyTimeoutHandler);
	RegisterTimeout(STANDBY_LOCK_TIMEOUT, StandbyLockTimeoutHandler);

	/*
	 * Unblock signals (they were blocked when the postmaster forked us)
	 */
	/*
	 * 解除信号阻塞（在 postmaster fork 我们时，它们是被阻塞的）
	 */
	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

	/*
	 * Do what we came for.
	 */
	/*
	 * 执行我们的主要任务。
	 */
	StartupXLOG();

	/*
	 * Exit normally. Exit code 0 tells postmaster that we completed recovery
	 * successfully.
	 */
	/*
	 * 正常退出。退出码 0 告诉 postmaster 我们已成功完成恢复。
	 */
	proc_exit(0);
}

/*
 * Function purpose: Set in_restore_command flag before running restore_command.
 * 函数作用：在运行恢复命令之前，设置 in_restore_command 标志。
 */
void
PreRestoreCommand(void)
{
	/*
	 * Set in_restore_command to tell the signal handler that we should exit
	 * right away on SIGTERM. We know that we're at a safe point to do that.
	 * Check if we had already received the signal, so that we don't miss a
	 * shutdown request received just before this.
	 */
	/*
	 * 设置 in_restore_command 以告诉信号处理程序我们应该在收到 SIGTERM 时
	 * 立即退出。我们知道那时处于安全点。
	 * 检查我们是否已经收到该信号，以便我们不会错过在此之前收到的关机请求。
	 */
	in_restore_command = true;
	if (shutdown_requested)
		proc_exit(1);
}

/*
 * Function purpose: Reset in_restore_command flag after running restore_command.
 * 函数作用：在运行恢复命令之后，重置 in_restore_command 标志。
 */
void
PostRestoreCommand(void)
{
	in_restore_command = false;
}

/*
 * Function purpose: Check if promote (failover) signal was received.
 * 函数作用：检查是否收到了提升（故障转移）信号。
 */
bool
IsPromoteSignaled(void)
{
	return promote_signaled;
}

/*
 * Function purpose: Reset promote signaled flag.
 * 函数作用：重置提升信号标志。
 */
void
ResetPromoteSignaled(void)
{
	promote_signaled = false;
}

/*
 * Set a flag indicating that it's time to log a progress report.
 */
/*
 * 设置一个标志，指示现在是记录进度报告的时候了。
 *
 * Function purpose: Timeout handler for startup progress logs.
 * 函数作用：启动进度日志的超时处理程序。
 */
void
startup_progress_timeout_handler(void)
{
	startup_progress_timer_expired = true;
}

/*
 * Function purpose: Disable the startup progress timeout.
 * 函数作用：禁用启动进度超时。
 */
void
disable_startup_progress_timeout(void)
{
	/* Feature is disabled. */
	/* 功能已禁用。 */
	if (log_startup_progress_interval == 0)
		return;

	disable_timeout(STARTUP_PROGRESS_TIMEOUT, false);
	startup_progress_timer_expired = false;
}

/*
 * Set the start timestamp of the current operation and enable the timeout.
 */
/*
 * 设置当前操作的开始时间戳并启用超时。
 *
 * Function purpose: Enable the startup progress timeout with a start timestamp.
 * 函数作用：记录开始时间戳并启用启动进度超时。
 */
void
enable_startup_progress_timeout(void)
{
	TimestampTz fin_time;

	/* Feature is disabled. */
	/* 功能已禁用。 */
	if (log_startup_progress_interval == 0)
		return;

	startup_progress_phase_start_time = GetCurrentTimestamp();
	fin_time = TimestampTzPlusMilliseconds(startup_progress_phase_start_time,
										   log_startup_progress_interval);
	enable_timeout_every(STARTUP_PROGRESS_TIMEOUT, fin_time,
						 log_startup_progress_interval);
}

/*
 * A thin wrapper to first disable and then enable the startup progress
 * timeout.
 */
/*
 * 一个轻量包装函数，首先禁用然后启用启动进度超时。
 *
 * Function purpose: Restart the startup progress phase timeout.
 * 函数作用：重新开始启动进度阶段的超时计时。
 */
void
begin_startup_progress_phase(void)
{
	/* Feature is disabled. */
	/* 功能已禁用。 */
	if (log_startup_progress_interval == 0)
		return;

	disable_startup_progress_timeout();
	enable_startup_progress_timeout();
}

/*
 * Report whether startup progress timeout has occurred. Reset the timer flag
 * if it did, set the elapsed time to the out parameters and return true,
 * otherwise return false.
 */
/*
 * 报告是否已发生启动进度超时。如果发生超时，重置定时器标志，
 * 将流逝的时间设置到输出参数中并返回 true，否则返回 false。
 *
 * Function purpose: Check if the startup progress timer has expired and calculate elapsed time.
 * 函数作用：检查启动进度定时器是否过期并计算已过去的时间。
 */
bool
has_startup_progress_timeout_expired(long *secs, int *usecs)
{
	long		seconds;
	int			useconds;
	TimestampTz now;

	/* No timeout has occurred. */
	/* 未发生超时。 */
	if (!startup_progress_timer_expired)
		return false;

	/* Calculate the elapsed time. */
	/* 计算流逝的时间。 */
	now = GetCurrentTimestamp();
	TimestampDifference(startup_progress_phase_start_time, now, &seconds, &useconds);

	*secs = seconds;
	*usecs = useconds;
	startup_progress_timer_expired = false;

	return true;
}


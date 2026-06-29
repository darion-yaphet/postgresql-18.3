/*-------------------------------------------------------------------------
 *
 * interrupt.c
 *	  Interrupt handling routines.
 *	  中断处理例程。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/interrupt.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>

#include "miscadmin.h"
#include "postmaster/interrupt.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/procsignal.h"
#include "utils/guc.h"
#include "utils/memutils.h"

volatile sig_atomic_t ConfigReloadPending = false;
volatile sig_atomic_t ShutdownRequestPending = false;

/*
 * Simple interrupt handler for main loops of background processes.
 */
/*
 * 后台进程主循环的简单中断处理程序。
 *
 * Function purpose: Process pending interrupts for background processes (e.g. reload config, shutdown, stats logging).
 * 函数作用：处理后台进程的待处理中断（例如：重新加载配置、关闭进程、内存上下文日志记录）。
 *
 * Core workflow:
 * 核心流程：
 * 1. If ProcSignalBarrierPending is true, process the proc signal barrier.
 *    如果 ProcSignalBarrierPending 为真，则处理进程信号屏障。
 * 2. If ConfigReloadPending is true, reset the flag and reload GUC configuration.
 *    如果 ConfigReloadPending 为真，则重置标志并重新加载 GUC 配置。
 * 3. If ShutdownRequestPending is true, perform a normal exit (proc_exit(0)).
 *    如果 ShutdownRequestPending 为真，则进行正常退出（proc_exit(0)）。
 * 4. If LogMemoryContextPending is true, dump memory contexts.
 *    如果 LogMemoryContextPending 为真，则转储内存上下文。
 */
void
ProcessMainLoopInterrupts(void)
{
	if (ProcSignalBarrierPending)
		ProcessProcSignalBarrier();

	if (ConfigReloadPending)
	{
		ConfigReloadPending = false;
		ProcessConfigFile(PGC_SIGHUP);
	}

	if (ShutdownRequestPending)
		proc_exit(0);

	/* Perform logging of memory contexts of this process */
	/* 记录该进程的内存上下文日志 */
	if (LogMemoryContextPending)
		ProcessLogMemoryContextInterrupt();
}

/*
 * Simple signal handler for triggering a configuration reload.
 *
 * Normally, this handler would be used for SIGHUP. The idea is that code
 * which uses it would arrange to check the ConfigReloadPending flag at
 * convenient places inside main loops, or else call ProcessMainLoopInterrupts.
 */
/*
 * 用于触发配置重新加载的简单信号处理函数。
 *
 * 通常，该处理函数会被用于 SIGHUP。其设计思路是，使用它的代码会安排在
 * 主循环内的合适位置检查 ConfigReloadPending 标志，或者调用 ProcessMainLoopInterrupts。
 *
 * Function purpose: Handle config reload signal (SIGHUP) by setting pending flag and waking up the process.
 * 函数作用：通过设置待处理标志并唤醒进程来处理配置重载信号（SIGHUP）。
 */
void
SignalHandlerForConfigReload(SIGNAL_ARGS)
{
	ConfigReloadPending = true;
	SetLatch(MyLatch);
}

/*
 * Simple signal handler for exiting quickly as if due to a crash.
 *
 * Normally, this would be used for handling SIGQUIT.
 */
/*
 * 用于像崩溃一样快速退出的简单信号处理函数。
 *
 * 通常，这被用于处理 SIGQUIT。
 *
 * Function purpose: Handle crash/quick exit signal (SIGQUIT) by immediately exiting with status 2.
 * 函数作用：通过立即以状态码 2 退出，处理崩溃/快速退出信号（SIGQUIT）。
 */
void
SignalHandlerForCrashExit(SIGNAL_ARGS)
{
	/*
	 * We DO NOT want to run proc_exit() or atexit() callbacks -- we're here
	 * because shared memory may be corrupted, so we don't want to try to
	 * clean up our transaction.  Just nail the windows shut and get out of
	 * town.  The callbacks wouldn't be safe to run from a signal handler,
	 * anyway.
	 *
	 * Note we do _exit(2) not _exit(0).  This is to force the postmaster into
	 * a system reset cycle if someone sends a manual SIGQUIT to a random
	 * backend.  This is necessary precisely because we don't clean up our
	 * shared memory state.  (The "dead man switch" mechanism in pmsignal.c
	 * should ensure the postmaster sees this as a crash, too, but no harm in
	 * being doubly sure.)
	 */
	/*
	 * 我们不想运行 proc_exit() 或 atexit() 回调函数——我们在此处是因为
	 * 共享内存可能已经损坏，所以我们不想尝试清理我们的事务。
	 * 关紧窗户直接离开即可。况且这些回调在信号处理函数中运行本身也不安全。
	 *
	 * 注意我们调用的是 _exit(2) 而不是 _exit(0)。这是为了在有人向随机后台进程
	 * 发送手动的 SIGQUIT 信号时，强制 postmaster 进入系统重置周期。
	 * 这之所以必要，正是因为我们没有清理共享内存状态。
	 * （pmsignal.c 中的“死人开关”机制应当也能确保 postmaster 将其视为崩溃，
	 * 但双重保险总没有坏处。）
	 */
	_exit(2);
}

/*
 * Simple signal handler for triggering a long-running background process to
 * shut down and exit.
 *
 * Typically, this handler would be used for SIGTERM, but some processes use
 * other signals. In particular, the checkpointer and parallel apply worker
 * exit on SIGUSR2, and the WAL writer exits on either SIGINT or SIGTERM.
 *
 * ShutdownRequestPending should be checked at a convenient place within the
 * main loop, or else the main loop should call ProcessMainLoopInterrupts.
 */
/*
 * 用于触发长时间运行的后台进程关闭并退出的简单信号处理函数。
 *
 * 通常，该处理函数会被用于 SIGTERM，但某些进程使用其他信号。
 * 特别地，检查点进程（checkpointer）和并行应用工作进程（parallel apply worker）在收到 SIGUSR2 时退出，
 * 而 WAL 写入进程（WAL writer）在收到 SIGINT 或 SIGTERM 时都会退出。
 *
 * 应当在主循环的合适位置检查 ShutdownRequestPending，或者主循环应当调用 ProcessMainLoopInterrupts。
 *
 * Function purpose: Handle shutdown signals (SIGTERM/SIGINT/SIGUSR2) by setting shutdown pending flag and setting latch.
 * 函数作用：通过设置关闭待处理标志和设置锁存器（latch）来处理退出信号（SIGTERM/SIGINT/SIGUSR2）。
 */
void
SignalHandlerForShutdownRequest(SIGNAL_ARGS)
{
	ShutdownRequestPending = true;
	SetLatch(MyLatch);
}


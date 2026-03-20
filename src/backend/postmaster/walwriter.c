/*-------------------------------------------------------------------------
 *
 * walwriter.c
 *
 * The WAL writer background process is new as of Postgres 8.3.  It attempts
 * to keep regular backends from having to write out (and fsync) WAL pages.
 * Also, it guarantees that transaction commit records that weren't synced
 * to disk immediately upon commit (ie, were "asynchronously committed")
 * will reach disk within a knowable time --- which, as it happens, is at
 * most three times the wal_writer_delay cycle time.
 *
 * WAL 写入后台进程是自 Postgres 8.3 起引入的新特性。它尝试
 * 避免普通后端进程必须亲自写入（并 fsync）WAL 页面。
 * 此外，它保证了在提交时未立即同步到磁盘的事务提交记录
 * （即“异步提交”的记录）将在一个可预见的时间内到达磁盘
 * —— 实际上，这个时间最多是 wal_writer_delay 周期时间的三倍。
 *
 * Note that as with the bgwriter for shared buffers, regular backends are
 * still empowered to issue WAL writes and fsyncs when the walwriter doesn't
 * keep up. This means that the WALWriter is not an essential process and
 * can shutdown quickly when requested.
 *
 * 请注意，与用于共享缓冲区的 bgwriter（后台写入进程）一样，当 
 * walwriter（WAL 写入进程）跟不上进度时，普通后端进程仍有权
 * 进行 WAL 写入和 fsync。这意味着 WALWriter 并非必不可少的进程，
 * 并且可以在收到请求时快速关闭。
 *
 * Because the walwriter's cycle is directly linked to the maximum delay
 * before async-commit transactions are guaranteed committed, it's probably
 * unwise to load additional functionality onto it.  For instance, if you've
 * got a yen to create xlog segments further in advance, that'd be better done
 * in bgwriter than in walwriter.
 *
 * 由于 walwriter 的周期直接关系到异步提交事务被保证提交前的最大延迟，
 * 因此将额外的功能加载到它身上可能是不明智的。例如，如果你想更提前地
 * 创建 xlog 段，那么在 bgwriter 中执行会比在 walwriter 中更好。
 *
 * The walwriter is started by the postmaster as soon as the startup subprocess
 * finishes.  It remains alive until the postmaster commands it to terminate.
 * Normal termination is by SIGTERM, which instructs the walwriter to exit(0).
 * Emergency termination is by SIGQUIT; like any backend, the walwriter will
 * simply abort and exit on SIGQUIT.
 *
 * walwriter 在启动子进程结束后由 postmaster 立即启动。
 * 它一直保持运行，直到 postmaster 命令其终止。
 * 正常终止通过 SIGTERM 进行，这会指示 walwriter 调用 exit(0) 退出。
 * 紧急终止通过 SIGQUIT 进行；与任何后端一样，walwriter 在收到 
 * SIGQUIT 后会直接中断并退出。
 *
 * If the walwriter exits unexpectedly, the postmaster treats that the same
 * as a backend crash: shared memory may be corrupted, so remaining backends
 * should be killed by SIGQUIT and then a recovery cycle started.
 *
 * 如果 walwriter 意外退出，postmaster 会将其视为后端崩溃：
 * 共享内存可能已损坏，因此其余后端应通过 SIGQUIT 杀掉，
 * 然后启动恢复周期。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/walwriter.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <unistd.h>

#include "access/xlog.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/auxprocess.h"
#include "postmaster/interrupt.h"
#include "postmaster/walwriter.h"
#include "storage/aio_subsys.h"
#include "storage/bufmgr.h"
#include "storage/condition_variable.h"
#include "storage/fd.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/smgr.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/resowner.h"


/*
 * GUC parameters
 *
 * GUC（全局统一配置）参数
 */
int			WalWriterDelay = 200;
int			WalWriterFlushAfter = DEFAULT_WAL_WRITER_FLUSH_AFTER;

/*
 * Number of do-nothing loops before lengthening the delay time, and the
 * multiplier to apply to WalWriterDelay when we do decide to hibernate.
 * (Perhaps these need to be configurable?)
 *
 * 在延长延迟时间之前的“无操作”循环次数，以及当我们决定 
 * 进入休眠（hibernate）时要应用到 WalWriterDelay 的乘数。
 * （也许这些需要设为可配置？）
 */
#define LOOPS_UNTIL_HIBERNATE		50
#define HIBERNATE_FACTOR			25

/*
 * Main entry point for walwriter process
 *
 * This is invoked from AuxiliaryProcessMain, which has already created the
 * basic execution environment, but not enabled signals yet.
 *
 * walwriter 进程的主入口点
 *
 * 此函数从 AuxiliaryProcessMain（辅助进程主程序）调用，
 * 后者已经创建了基本执行环境，但尚未启用信号。
 */
void
WalWriterMain(const void *startup_data, size_t startup_data_len)
{
	sigjmp_buf	local_sigjmp_buf;
	MemoryContext walwriter_context;
	int			left_till_hibernate;
	bool		hibernating;

	Assert(startup_data_len == 0);

	MyBackendType = B_WAL_WRITER;
	AuxiliaryProcessMainCommon();

	/*
	 * Properly accept or ignore signals the postmaster might send us
	 *
	 * We have no particular use for SIGINT at the moment, but seems
	 * reasonable to treat like SIGTERM.
	 *
	 * 正确接受或忽略 postmaster 可能发送给我们的信号
	 *
	 * 目前我们对 SIGINT 没有特别的用途，但将其视为 
	 * 与 SIGTERM 相同的处理似乎是合理的。
	 */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGINT, SignalHandlerForShutdownRequest);
	pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
	/* SIGQUIT handler was already set up by InitPostmasterChild */
	pqsignal(SIGALRM, SIG_IGN);
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, SIG_IGN); /* not used */

	/*
	 * Reset some signals that are accepted by postmaster but not here
	 *
	 * 重置一些 postmaster 接受但在本进程中不接受的信号
	 */
	pqsignal(SIGCHLD, SIG_DFL);

	/*
	 * Create a memory context that we will do all our work in.  We do this so
	 * that we can reset the context during error recovery and thereby avoid
	 * possible memory leaks.  Formerly this code just ran in
	 * TopMemoryContext, but resetting that would be a really bad idea.
	 *
	 * 创建一个供我们执行所有工作的内存上下文。这样做是为了让我们可以在
	 * 错误恢复期间重置该上下文，从而避免可能的内存泄漏。
	 * 以前这段代码只是在 TopMemoryContext 中运行，但重置它会是一个非常糟糕的主意。
	 */
	walwriter_context = AllocSetContextCreate(TopMemoryContext,
											  "Wal Writer",
											  ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(walwriter_context);

	/*
	 * If an exception is encountered, processing resumes here.
	 *
	 * You might wonder why this isn't coded as an infinite loop around a
	 * PG_TRY construct.  The reason is that this is the bottom of the
	 * exception stack, and so with PG_TRY there would be no exception handler
	 * in force at all during the CATCH part.  By leaving the outermost setjmp
	 * always active, we have at least some chance of recovering from an error
	 * during error recovery.  (If we get into an infinite loop thereby, it
	 * will soon be stopped by overflow of elog.c's internal state stack.)
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
	 * 你可能会纳闷为什么不把这部分代码写成围绕 PG_TRY 的无限循环。
	 * 原因是这里处于异常栈的最底层，如果使用 PG_TRY，在 CATCH 部分
	 * 期间将根本没有任何有效的异常处理程序。通过让最外层的 setjmp 
	 * 始终保持激活，我们至少有一定机会从错误恢复过程中的错误里恢复过来。
	 * （如果因此陷入无限循环，它很快就会因 elog.c 内部状态栈溢出而停止。）
	 *
	 * 注意我们使用了 sigsetjmp(..., 1)，以便在 longjmp 到此处时恢复
	 * 当时的信号掩码（即 BlockSig）。这样，除了 SIGQUIT 以外的信号
	 * 都将被阻塞，直到我们完成错误恢复。虽然这项政策看起来像是让 
	 * HOLD_INTERRUPTS() 的调用变得多余，但事实并非如此，因为 
	 * InterruptPending 可能已经被设置过了。
	 */
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/* Since not using PG_TRY, must reset error stack by hand */
		error_context_stack = NULL;

		/* Prevent interrupts while cleaning up */
		HOLD_INTERRUPTS();

		/* Report the error to the server log */
		EmitErrorReport();

		/*
		 * These operations are really just a minimal subset of
		 * AbortTransaction().  We don't have very many resources to worry
		 * about in walwriter, but we do have LWLocks, and perhaps buffers?
		 *
		 * 这些操作实际上只是 AbortTransaction() 的一个极小真子集。
		 * 我们在 walwriter 中不需要担心太多资源，但我们确实有 
		 * LWLock（轻量级锁），也许还有缓冲区？
		 */
		LWLockReleaseAll();
		ConditionVariableCancelSleep();
		pgstat_report_wait_end();
		pgaio_error_cleanup();
		UnlockBuffers();
		ReleaseAuxProcessResources(false);
		AtEOXact_Buffers(false);
		AtEOXact_SMgr();
		AtEOXact_Files(false);
		AtEOXact_HashTables(false);

		/*
		 * Now return to normal top-level context and clear ErrorContext for
		 * next time.
		 *
		 * 现在返回正常的顶级上下文，并清除 ErrorContext 以备下次使用。
		 */
		MemoryContextSwitchTo(walwriter_context);
		FlushErrorState();

		/* Flush any leaked data in the top-level context */
		MemoryContextReset(walwriter_context);

		/* Now we can allow interrupts again */
		RESUME_INTERRUPTS();

		/*
		 * Sleep at least 1 second after any error.  A write error is likely
		 * to be repeated, and we don't want to be filling the error logs as
		 * fast as we can.
		 *
		 * 在发生任何错误后，至少休眠 1 秒。写入错误很可能会重复发生，
		 * 我们不希望以最快速度填满错误日志。
		 */
		pg_usleep(1000000L);
	}

	/* We can now handle ereport(ERROR) */
	PG_exception_stack = &local_sigjmp_buf;

	/*
	 * Unblock signals (they were blocked when the postmaster forked us)
	 *
	 * 解除非阻塞信号（这些信号在 postmaster fork 我们时被阻塞了）
	 */
	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

	/*
	 * Reset hibernation state after any error.
	 *
	 * 在发生任何错误后重置休眠状态。
	 */
	left_till_hibernate = LOOPS_UNTIL_HIBERNATE;
	hibernating = false;
	SetWalWriterSleeping(false);

	/*
	 * Advertise our proc number that backends can use to wake us up while
	 * we're sleeping.
	 *
	 * 公布我们的进程编号，以便后端在我们睡眠时可以用它来唤醒我们。
	 */
	ProcGlobal->walwriterProc = MyProcNumber;

	/*
	 * Loop forever
	 *
	 * 无限循环
	 */
	for (;;)
	{
		long		cur_timeout;

		/*
		 * Advertise whether we might hibernate in this cycle.  We do this
		 * before resetting the latch to ensure that any async commits will
		 * see the flag set if they might possibly need to wake us up, and
		 * that we won't miss any signal they send us.  (If we discover work
		 * to do in the last cycle before we would hibernate, the global flag
		 * will be set unnecessarily, but little harm is done.)  But avoid
		 * touching the global flag if it doesn't need to change.
		 *
		 * 公布我们在本周期是否可能进入休眠。我们在重置 latch（门闩）之前 
		 * 执行此操作，以确保任何异步提交在可能需要唤醒我们时能看到该 
		 * 标志已设置，并且我们不会错过它们发送给我们的任何信号。
		 * （如果我们要在休眠前的最后一个周期发现有工作要做，全局标志
		 * 将被不必要地设置，但并无大碍。）不过，如果全局标志不需要 
		 * 更改，就避免去触动它。
		 */
		if (hibernating != (left_till_hibernate <= 1))
		{
			hibernating = (left_till_hibernate <= 1);
			SetWalWriterSleeping(hibernating);
		}

		/* Clear any already-pending wakeups */
		/* 清除任何已挂起的唤醒信号 */
		ResetLatch(MyLatch);

		/* Process any signals received recently */
		/* 处理最近收到的任何信号 */
		ProcessMainLoopInterrupts();

		/*
		 * Do what we're here for; then, if XLogBackgroundFlush() found useful
		 * work to do, reset hibernation counter.
		 *
		 * 执行我们的本职工作；然后，如果 XLogBackgroundFlush() 发现了 
		 * 有意义的工作，则重置休眠计数器。
		 */
		if (XLogBackgroundFlush())
			left_till_hibernate = LOOPS_UNTIL_HIBERNATE;
		else if (left_till_hibernate > 0)
			left_till_hibernate--;

		/* report pending statistics to the cumulative stats system */
		/* 向累计统计系统报告挂起的统计信息 */
		pgstat_report_wal(false);

		/*
		 * Sleep until we are signaled or WalWriterDelay has elapsed.  If we
		 * haven't done anything useful for quite some time, lengthen the
		 * sleep time so as to reduce the server's idle power consumption.
		 *
		 * 休眠直到收到信号或 WalWriterDelay 到期。如果我们已经有相当 
		 * 长时间没做任何有意义的工作了，就延长休眠时间，以减少服务
		 * 器的空闲功耗。
		 */
		if (left_till_hibernate > 0)
			cur_timeout = WalWriterDelay;	/* in ms */
		else
			cur_timeout = WalWriterDelay * HIBERNATE_FACTOR;

		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 cur_timeout,
						 WAIT_EVENT_WAL_WRITER_MAIN);
	}
}

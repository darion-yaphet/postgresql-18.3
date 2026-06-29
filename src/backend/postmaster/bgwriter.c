/*-------------------------------------------------------------------------
 *
 * bgwriter.c
 *
 * The background writer (bgwriter) is new as of Postgres 8.0.  It attempts
 * to keep regular backends from having to write out dirty shared buffers
 * (which they would only do when needing to free a shared buffer to read in
 * another page).  In the best scenario all writes from shared buffers will
 * be issued by the background writer process.  However, regular backends are
 * still empowered to issue writes if the bgwriter fails to maintain enough
 * clean shared buffers.
 * 后台写进程（bgwriter）自 Postgres 8.0 引入。它尽量让普通后端不必把脏共享缓冲
 * 写回磁盘（否则后端只有在需要腾出缓冲以读入另一页时才会写）。理想情况下，共享
 * 缓冲上的写都由 bgwriter 发起；若其无法维持足够干净缓冲，普通后端仍可自行写盘。
 *
 * As of Postgres 9.2 the bgwriter no longer handles checkpoints.
 * 自 Postgres 9.2 起，bgwriter 不再负责 checkpoint。
 *
 * Normal termination is by SIGTERM, which instructs the bgwriter to exit(0).
 * Emergency termination is by SIGQUIT; like any backend, the bgwriter will
 * simply abort and exit on SIGQUIT.
 * 正常退出：SIGTERM，bgwriter 执行 exit(0)。紧急退出：SIGQUIT；与其它后端一样，
 * 收到 SIGQUIT 会直接中止并退出。
 *
 * If the bgwriter exits unexpectedly, the postmaster treats that the same
 * as a backend crash: shared memory may be corrupted, so remaining backends
 * should be killed by SIGQUIT and then a recovery cycle started.
 * 若 bgwriter 意外退出，postmaster 按后端崩溃处理：共享内存可能已损坏，其余后端
 * 应以 SIGQUIT 终止并进入恢复流程。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/bgwriter.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xlog.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/auxprocess.h"
#include "postmaster/bgwriter.h"
#include "postmaster/interrupt.h"
#include "storage/aio_subsys.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "storage/condition_variable.h"
#include "storage/fd.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/smgr.h"
#include "storage/standby.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/timestamp.h"

/*
 * 核心流程（BackgroundWriterMain 主循环，周而复始）：
 * 1) 清 latch、处理主循环中断；
 * 2) BgBufferSync()：按策略刷出一批脏页（返回是否可进入“冬眠”）；
 * 3) 上报 bgwriter / WAL 统计；若距上次 checkpoint 后首次回到此处则 smgrdestroyall()；
 * 4) 非恢复且开启物理复制信息时，定期 LogStandbySnapshot() 写入 xl_running_xacts；
 * 5) WaitLatch 等待 BgWriterDelay；若连续两周期判定空闲则注册 StrategyNotifyBgWriter 并长睡。
 */

/*
 * GUC parameters
 * GUC 参数
 */
int			BgWriterDelay = 200;

/*
 * Multiplier to apply to BgWriterDelay when we decide to hibernate.
 * (Perhaps this needs to be configurable?)
 * 进入“冬眠”时，将 BgWriterDelay 乘以此系数作为额外睡眠时间。（或许应做成可配置？）
 */
#define HIBERNATE_FACTOR			50

/*
 * Interval in which standby snapshots are logged into the WAL stream, in
 * milliseconds.
 * 向 WAL 流写入 standby 快照（running xacts）的时间间隔，单位毫秒。
 */
#define LOG_SNAPSHOT_INTERVAL_MS 15000

/*
 * LSN and timestamp at which we last issued a LogStandbySnapshot(), to avoid
 * doing so too often or repeatedly if there has been no other write activity
 * in the system.
 * 上次调用 LogStandbySnapshot() 的 LSN 与时间戳，用于在无其它写活动时避免过于频繁重复记录。
 */
static TimestampTz last_snapshot_ts;
static XLogRecPtr last_snapshot_lsn = InvalidXLogRecPtr;


/*
 * Main entry point for bgwriter process
 *
 * This is invoked from AuxiliaryProcessMain, which has already created the
 * basic execution environment, but not enabled signals yet.
 * bgwriter 进程主入口；由 AuxiliaryProcessMain 调用，此时已建立基本运行环境但尚未启用信号。
 */
void
BackgroundWriterMain(const void *startup_data, size_t startup_data_len)
{
	sigjmp_buf	local_sigjmp_buf;
	MemoryContext bgwriter_context;
	bool		prev_hibernate;
	WritebackContext wb_context;

	Assert(startup_data_len == 0);

	MyBackendType = B_BG_WRITER;
	AuxiliaryProcessMainCommon();

	/*
	 * Properly accept or ignore signals that might be sent to us.
	 * 正确接受或忽略可能发向本进程的信号。
	 */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGINT, SIG_IGN);
	pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
	/* SIGQUIT handler was already set up by InitPostmasterChild */
	/* SIGQUIT 已由 InitPostmasterChild 注册 */
	pqsignal(SIGALRM, SIG_IGN);
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, SIG_IGN);

	/*
	 * Reset some signals that are accepted by postmaster but not here
	 * 重置 postmaster 会处理、但本进程不处理的若干信号为默认行为。
	 */
	pqsignal(SIGCHLD, SIG_DFL);

	/*
	 * We just started, assume there has been either a shutdown or
	 * end-of-recovery snapshot.
	 * 刚启动，假定已发生过关闭或恢复结束时的快照，避免立即再记一条。
	 */
	last_snapshot_ts = GetCurrentTimestamp();

	/*
	 * Create a memory context that we will do all our work in.  We do this so
	 * that we can reset the context during error recovery and thereby avoid
	 * possible memory leaks.  Formerly this code just ran in
	 * TopMemoryContext, but resetting that would be a really bad idea.
	 * 创建专用内存上下文；错误恢复时可整体重置，避免泄漏。旧代码曾跑在
	 * TopMemoryContext，重置后者后果严重。
	 */
	bgwriter_context = AllocSetContextCreate(TopMemoryContext,
											 "Background Writer",
											 ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(bgwriter_context);

	/*
	 * WritebackContextInit sets up batching of kernel writeback (see storage/fd.c).
	 * wb_context is passed to BgBufferSync so page writes can be coalesced per bgwriter_flush_after.
	 * WritebackContextInit 初始化内核写回批处理上下文（见 storage/fd.c）；wb_context 传给
	 * BgBufferSync，按 bgwriter_flush_after 合并下发写，减少系统调用与磁盘抖动。
	 */
	WritebackContextInit(&wb_context, &bgwriter_flush_after);

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
	 * 若发生异常，从此处恢复执行。此处不用 PG_TRY 包一层无限循环，是因为处于
	 * 异常栈底，CATCH 阶段将没有有效处理器；保持最外层 setjmp 常驻，才有机会在
	 * “恢复中的错误”里再次恢复。（若因此死循环，elog.c 状态栈溢出会终止之。）
	 * 使用 sigsetjmp(...,1) 以便 longjmp 回来时恢复当时信号掩码（如 BlockSig），
	 * 除 SIGQUIT 外信号在恢复完成前被屏蔽。HOLD_INTERRUPTS() 仍必要，因
	 * InterruptPending 可能已置位。
	 */
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/* Since not using PG_TRY, must reset error stack by hand */
		/* 未用 PG_TRY，须手动清空错误上下文栈 */
		error_context_stack = NULL;

		/* Prevent interrupts while cleaning up */
		/* 清理期间禁止中断 */
		HOLD_INTERRUPTS();

		/* Report the error to the server log */
		/* 将错误写入服务器日志 */
		EmitErrorReport();

		/*
		 * These operations are really just a minimal subset of
		 * AbortTransaction().  We don't have very many resources to worry
		 * about in bgwriter, but we do have LWLocks, buffers, and temp files.
		 * 这些操作相当于 AbortTransaction() 的最小子集。bgwriter 资源不多，
		 * 但须释放 LWLock、缓冲与临时文件等。
		 */
		LWLockReleaseAll();
		ConditionVariableCancelSleep();
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
		 * 回到顶层上下文并清空 ErrorContext，供下次使用。
		 */
		MemoryContextSwitchTo(bgwriter_context);
		FlushErrorState();

		/* Flush any leaked data in the top-level context */
		/* 丢弃顶层上下文中可能泄漏的分配 */
		MemoryContextReset(bgwriter_context);

		/* re-initialize to avoid repeated errors causing problems */
		/* 重新初始化写回上下文，避免连续错误放大问题 */
		WritebackContextInit(&wb_context, &bgwriter_flush_after);

		/* Now we can allow interrupts again */
		/* 恢复允许中断 */
		RESUME_INTERRUPTS();

		/*
		 * Sleep at least 1 second after any error.  A write error is likely
		 * to be repeated, and we don't want to be filling the error logs as
		 * fast as we can.
		 * 任一类错误后至少睡眠 1 秒；写错误易反复，避免日志被瞬间刷满。
		 */
		pg_usleep(1000000L);

		/* Report wait end here, when there is no further possibility of wait */
		/* 此处已无再等待可能，结束等待事件统计上报 */
		pgstat_report_wait_end();
	}

	/* We can now handle ereport(ERROR) */
	/* 此后 ereport(ERROR) 可经 longjmp 回到上面恢复点 */
	PG_exception_stack = &local_sigjmp_buf;

	/*
	 * Unblock signals (they were blocked when the postmaster forked us)
	 * 解除信号屏蔽（postmaster fork 本进程时曾屏蔽）。
	 */
	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

	/*
	 * Reset hibernation state after any error.
	 * 错误恢复后重置“冬眠”状态，避免错误路径上沿用旧的 prev_hibernate。
	 */
	prev_hibernate = false;

	/*
	 * Loop forever
	 * 无限主循环（见文件头“核心流程”）。
	 */
	for (;;)
	{
		bool		can_hibernate;
		int			rc;

		/* Clear any already-pending wakeups */
		/* 清除 latch 上已挂起的唤醒，避免误用旧事件 */
		ResetLatch(MyLatch);

		/*
		 * Process config reload and other main-loop level interrupts (see interrupt.c).
		 * 处理配置重载及 interrupt.c 中定义的其它主循环级中断。
		 */
		ProcessMainLoopInterrupts();

		/*
		 * Do one cycle of dirty-buffer writing.
		 * 执行一轮脏缓冲写盘（具体策略与数量由 bufmgr 中 BgBufferSync 决定）。
		 */
		can_hibernate = BgBufferSync(&wb_context);

		/* Report pending statistics to the cumulative stats system */
		/* 将本周期累积的 bgwriter 统计提交给全局统计子系统 */
		pgstat_report_bgwriter();
		pgstat_report_wal(true);

		/*
		 * FirstCallSinceLastCheckpoint (checkpointer.c): true once per checkpoint cycle.
		 * FirstCallSinceLastCheckpoint（checkpointer.c）：每个 checkpoint 周期内仅第一次为 true。
		 */
		if (FirstCallSinceLastCheckpoint())
		{
			/*
			 * After any checkpoint, free all smgr objects.  Otherwise we
			 * would never do so for dropped relations, as the bgwriter does
			 * not process shared invalidation messages or call
			 * AtEOXact_SMgr().
			 * 每次 checkpoint 完成后释放全部 smgr 对象；否则已删表等关系
			 * 的 smgr 句柄无法回收，因 bgwriter 不处理失效消息也不调 AtEOXact_SMgr()。
			 */
			smgrdestroyall();
		}

		/*
		 * Log a new xl_running_xacts every now and then so replication can
		 * get into a consistent state faster (think of suboverflowed
		 * snapshots) and clean up resources (locks, KnownXids*) more
		 * frequently. The costs of this are relatively low, so doing it 4
		 * times (LOG_SNAPSHOT_INTERVAL_MS) a minute seems fine.
		 *
		 * We assume the interval for writing xl_running_xacts is
		 * significantly bigger than BgWriterDelay, so we don't complicate the
		 * overall timeout handling but just assume we're going to get called
		 * often enough even if hibernation mode is active. It's not that
		 * important that LOG_SNAPSHOT_INTERVAL_MS is met strictly. To make
		 * sure we're not waking the disk up unnecessarily on an idle system
		 * we check whether there has been any WAL inserted since the last
		 * time we've logged a running xacts.
		 *
		 * We do this logging in the bgwriter as it is the only process that
		 * is run regularly and returns to its mainloop all the time. E.g.
		 * Checkpointer, when active, is barely ever in its mainloop and thus
		 * makes it hard to log regularly.
		 * 周期性写入 xl_running_xacts，使物理复制更快达到一致可读（如子溢出快照场景），
		 * 并更频繁回收锁与 KnownXids* 等资源；开销较低，约每分钟 4 次（LOG_SNAPSHOT_INTERVAL_MS）。
		 * 假定该间隔远大于 BgWriterDelay，故不单独搞复杂超时，即便冬眠也认为会被足够频繁调用；
		 * 间隔不必严格。空闲系统上若上次快照后无新 WAL，则不写，避免无谓唤醒磁盘。
		 * 放在 bgwriter 是因为只有它持续规律回到主循环；checkpointer 忙时几乎不在主循环，难定时写。
		 */
		if (XLogStandbyInfoActive() && !RecoveryInProgress())
		{
			TimestampTz timeout = 0;
			TimestampTz now = GetCurrentTimestamp();

			timeout = TimestampTzPlusMilliseconds(last_snapshot_ts,
												  LOG_SNAPSHOT_INTERVAL_MS);

			/*
			 * Only log if enough time has passed and interesting records have
			 * been inserted since the last snapshot.  Have to compare with <=
			 * instead of < because GetLastImportantRecPtr() points at the
			 * start of a record, whereas last_snapshot_lsn points just past
			 * the end of the record.
			 * 仅当时间到且自上次快照后有“重要”WAL。须用 <= 而非 <：GetLastImportantRecPtr()
			 * 指向记录起点，last_snapshot_lsn 指向上次记录结束之后。
			 */
			if (now >= timeout &&
				last_snapshot_lsn <= GetLastImportantRecPtr())
			{
				last_snapshot_lsn = LogStandbySnapshot();
				last_snapshot_ts = now;
			}
		}

		/*
		 * Sleep until we are signaled or BgWriterDelay has elapsed.
		 *
		 * Note: the feedback control loop in BgBufferSync() expects that we
		 * will call it every BgWriterDelay msec.  While it's not critical for
		 * correctness that that be exact, the feedback loop might misbehave
		 * if we stray too far from that.  Hence, avoid loading this process
		 * down with latch events that are likely to happen frequently during
		 * normal operation.
		 * 睡眠直至被唤醒或经过 BgWriterDelay 毫秒。注意：BgBufferSync() 内反馈环
		 * 假定约每 BgWriterDelay 调用一次；不必毫秒级精确，但偏离过大会失调。
		 * 因此勿让本进程被过于频繁的 latch 事件淹没。
		 */
		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
					   BgWriterDelay /* ms，毫秒 */ , WAIT_EVENT_BGWRITER_MAIN);

		/*
		 * If no latch event and BgBufferSync says nothing's happening, extend
		 * the sleep in "hibernation" mode, where we sleep for much longer
		 * than bgwriter_delay says.  Fewer wakeups save electricity.  When a
		 * backend starts using buffers again, it will wake us up by setting
		 * our latch.  Because the extra sleep will persist only as long as no
		 * buffer allocations happen, this should not distort the behavior of
		 * BgBufferSync's control loop too badly; essentially, it will think
		 * that the system-wide idle interval didn't exist.
		 *
		 * There is a race condition here, in that a backend might allocate a
		 * buffer between the time BgBufferSync saw the alloc count as zero
		 * and the time we call StrategyNotifyBgWriter.  While it's not
		 * critical that we not hibernate anyway, we try to reduce the odds of
		 * that by only hibernating when BgBufferSync says nothing's happening
		 * for two consecutive cycles.  Also, we mitigate any possible
		 * consequences of a missed wakeup by not hibernating forever.
		 * 若本次 WaitLatch 仅因超时返回、且 BgBufferSync 认为系统空闲，则进入“冬眠”：
		 * 再睡 BgWriterDelay * HIBERNATE_FACTOR，减少唤醒省电；有后端再分配缓冲时会
		 * 置 latch 唤醒。额外睡眠仅在无新分配期间有效，对 BgBufferSync 反馈环影响
		 * 有限，相当于把全库空闲时段“折叠”掉。
		 * 存在竞态：BgBufferSync 见分配计数为 0 到调用 StrategyNotifyBgWriter 之间
		 * 可能有分配。故要求连续两周期均判定空闲才冬眠，且长睡有上限，降低漏唤醒后果。
		 */
		if (rc == WL_TIMEOUT && can_hibernate && prev_hibernate)
		{
			/* Ask for notification at next buffer allocation */
			/* 登记：下次有缓冲分配时唤醒本 bgwriter */
			StrategyNotifyBgWriter(MyProcNumber);
			/* Sleep ... */
			/* 冬眠长睡 */
			(void) WaitLatch(MyLatch,
							 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
							 BgWriterDelay * HIBERNATE_FACTOR,
							 WAIT_EVENT_BGWRITER_HIBERNATE);
			/* Reset the notification request in case we timed out */
			/* 若因超时醒来则取消登记，避免长期挂着无效通知 */
			StrategyNotifyBgWriter(-1);
		}

		prev_hibernate = can_hibernate;
	}
}

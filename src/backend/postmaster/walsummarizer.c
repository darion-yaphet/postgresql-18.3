/*-------------------------------------------------------------------------
 *
 * walsummarizer.c
 *
 * Background process to perform WAL summarization, if it is enabled.
 * It continuously scans the write-ahead log and periodically emits a
 * summary file which indicates which blocks in which relation forks
 * were modified by WAL records in the LSN range covered by the summary
 * file. See walsummary.c and blkreftable.c for more details on the
 * naming and contents of WAL summary files.
 * WAL 归纳（WAL summarization）的后台进程（如果已启用）。
 * 它持续扫描预写日志，并定期输出一个汇总文件，该文件指示在汇总文件所覆盖的
 * LSN 范围内，哪些关系分支（relation forks）中的哪些数据块被 WAL 记录所修改。
 * 有关 WAL 汇总文件的命名和内容的更多详细信息，请参见 walsummary.c 和 blkreftable.c。
 *
 * If configured to do, this background process will also remove WAL
 * summary files when the file timestamp is older than a configurable
 * threshold (but only if the WAL has been removed first).
 * 如果配置了相应选项，当汇总文件的修改时间早于可配置的阈值时，该后台进程
 * 还会删除旧 of WAL 汇总文件（但前提是对应的 WAL 已经被删除）。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/walsummarizer.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/timeline.h"
#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xlogrecovery.h"
#include "access/xlogutils.h"
#include "backup/walsummary.h"
#include "catalog/storage_xlog.h"
#include "commands/dbcommands_xlog.h"
#include "common/blkreftable.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/auxprocess.h"
#include "postmaster/interrupt.h"
#include "postmaster/walsummarizer.h"
#include "replication/walreceiver.h"
#include "storage/aio_subsys.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/shmem.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/wait_event.h"

/*
 * Data in shared memory related to WAL summarization.
 * 共享内存中与 WAL 归纳相关的数据。
 */
typedef struct
{
	/*
	 * These fields are protected by WALSummarizerLock.
	 * 这些字段受 WALSummarizerLock 保护。
	 *
	 * Until we've discovered what summary files already exist on disk and
	 * stored that information in shared memory, initialized is false and the
	 * other fields here contain no meaningful information. After that has
	 * been done, initialized is true.
	 * 在我们检测到磁盘上已存在哪些汇总文件并将该信息存储在共享内存中之前，
	 * initialized 为 false，此处的其他字段不包含任何有意义的信息。
	 * 完成该操作后，initialized 为 true。
	 *
	 * summarized_tli and summarized_lsn indicate the last LSN and TLI at
	 * which the next summary file will start. Normally, these are the LSN and
	 * TLI at which the last file ended; in such case, lsn_is_exact is true.
	 * If, however, the LSN is just an approximation, then lsn_is_exact is
	 * false. This can happen if, for example, there are no existing WAL
	 * summary files at startup. In that case, we have to derive the position
	 * at which to start summarizing from the WAL files that exist on disk,
	 * and so the LSN might point to the start of the next file even though
	 * that might happen to be in the middle of a WAL record.
	 * summarized_tli 和 summarized_lsn 表示下一个汇总文件开始的最后一个 LSN 和 TLI。
	 * 通常，这些是上一个文件结束时的 LSN 和 TLI；在这种情况下，lsn_is_exact 为 true。
	 * 然而，如果 LSN 只是一个近似值，则 lsn_is_exact 为 false。
	 * 例如，如果启动时不存在现有的 WAL 汇总文件，就会发生这种情况。
	 * 在这种情况下，我们必须从磁盘上存在的 WAL 文件推导开始归纳的位置，
	 * 因此 LSN 可能会指向下一个文件的开始，即使这碰巧位于 WAL 记录的中间。
	 *
	 * summarizer_pgprocno is the proc number of the summarizer process, if
	 * one is running, or else INVALID_PROC_NUMBER.
	 * summarizer_pgprocno 是正在运行的归纳器进程的进程号，如果未运行则为 INVALID_PROC_NUMBER。
	 *
	 * pending_lsn is used by the summarizer to advertise the ending LSN of a
	 * record it has recently read. It shouldn't ever be less than
	 * summarized_lsn, but might be greater, because the summarizer buffers
	 * data for a range of LSNs in memory before writing out a new file.
	 * pending_lsn 由归纳器使用，用于公布它最近读取的记录的结束 LSN。
	 * 它永远不应该小于 summarized_lsn，但可能会大于，因为归纳器在内存中缓存了
	 * 一个 LSN 范围的数据，然后再写入新的文件。
	 */
	bool		initialized;
	TimeLineID	summarized_tli;
	XLogRecPtr	summarized_lsn;
	bool		lsn_is_exact;
	ProcNumber	summarizer_pgprocno;
	XLogRecPtr	pending_lsn;

	/*
	 * This field handles its own synchronization.
	 * 该字段处理自身的同步。
	 */
	ConditionVariable summary_file_cv;
} WalSummarizerData;

/*
 * Private data for our xlogreader's page read callback.
 * 我们的 xlogreader 页面读取回调的私有数据。
 */
typedef struct
{
	TimeLineID	tli;
	bool		historic;
	XLogRecPtr	read_upto;
	bool		end_of_wal;
} SummarizerReadLocalXLogPrivate;

/* Pointer to shared memory state. */
/* 指向共享内存状态的指针。 */
static WalSummarizerData *WalSummarizerCtl;

/*
 * When we reach end of WAL and need to read more, we sleep for a number of
 * milliseconds that is an integer multiple of MS_PER_SLEEP_QUANTUM. This is
 * the multiplier. It should vary between 1 and MAX_SLEEP_QUANTA, depending
 * on system activity. See summarizer_wait_for_wal() for how we adjust this.
 * 当我们到达 WAL 末尾并需要读取更多内容时，我们会休眠若干毫秒，该毫秒数是
 * MS_PER_SLEEP_QUANTUM 的整数倍。这就是乘数。它应该在 1 和 MAX_SLEEP_QUANTA
 * 之间变化，具体取决于系统活动。有关我们如何调整此值，请参见 summarizer_wait_for_wal()。
 */
static long sleep_quanta = 1;

/*
 * The sleep time will always be a multiple of 200ms and will not exceed
 * thirty seconds (150 * 200 = 30 * 1000). Note that the timeout here needs
 * to be substantially less than the maximum amount of time for which an
 * incremental backup will wait for this process to catch up. Otherwise, an
 * incremental backup might time out on an idle system just because we sleep
 * for too long.
 * 休眠时间始终是 200ms 的倍数，且不会超过三十秒（150 * 200 = 30 * 1000）。
 * 注意，这里的超时时间需要明显小于增量备份等待此进程赶上的最大时间。
 * 否则，增量备份可能会因为我们休眠时间太长而在空闲系统上超时。
 */
#define MAX_SLEEP_QUANTA		150
#define MS_PER_SLEEP_QUANTUM	200

/*
 * This is a count of the number of pages of WAL that we've read since the
 * last time we waited for more WAL to appear.
 * 这是自我们上次等待更多 WAL 出现以来所读取的 WAL 页面数量的计数。
 */
static long pages_read_since_last_sleep = 0;

/*
 * Most recent RedoRecPtr value observed by MaybeRemoveOldWalSummaries.
 * MaybeRemoveOldWalSummaries 观察到的最新 RedoRecPtr 值。
 */
static XLogRecPtr redo_pointer_at_last_summary_removal = InvalidXLogRecPtr;

/*
 * GUC parameters
 * GUC 参数
 */
bool		summarize_wal = false;
int			wal_summary_keep_time = 10 * HOURS_PER_DAY * MINS_PER_HOUR;

static void WalSummarizerShutdown(int code, Datum arg);
static XLogRecPtr GetLatestLSN(TimeLineID *tli);
static void ProcessWalSummarizerInterrupts(void);
static XLogRecPtr SummarizeWAL(TimeLineID tli, XLogRecPtr start_lsn,
							   bool exact, XLogRecPtr switch_lsn,
							   XLogRecPtr maximum_lsn);
static void SummarizeDbaseRecord(XLogReaderState *xlogreader,
								 BlockRefTable *brtab);
static void SummarizeSmgrRecord(XLogReaderState *xlogreader,
								BlockRefTable *brtab);
static void SummarizeXactRecord(XLogReaderState *xlogreader,
								BlockRefTable *brtab);
static bool SummarizeXlogRecord(XLogReaderState *xlogreader,
								bool *new_fast_forward);
static int	summarizer_read_local_xlog_page(XLogReaderState *state,
											XLogRecPtr targetPagePtr,
											int reqLen,
											XLogRecPtr targetRecPtr,
											char *cur_page);
static void summarizer_wait_for_wal(void);
static void MaybeRemoveOldWalSummaries(void);

/*
 * Amount of shared memory required for this module.
 * 获取此模块所需的共享内存大小。
 */
Size
WalSummarizerShmemSize(void)
{
	return sizeof(WalSummarizerData);
}

/*
 * Create or attach to shared memory segment for this module.
 * 创建或连接到此模块的共享内存段。
 */
void
WalSummarizerShmemInit(void)
{
	bool		found;

	WalSummarizerCtl = (WalSummarizerData *)
		ShmemInitStruct("Wal Summarizer Ctl", WalSummarizerShmemSize(),
						&found);

	if (!found)
	{
		/*
		 * First time through, so initialize.
		 * 第一次进入，因此进行初始化。
		 *
		 * We're just filling in dummy values here -- the real initialization
		 * will happen when GetOldestUnsummarizedLSN() is called for the first
		 * time.
		 * 我们在这里只是填充一些虚拟值——真正的初始化将在第一次调用
		 * GetOldestUnsummarizedLSN() 时发生。
		 */
		WalSummarizerCtl->initialized = false;
		WalSummarizerCtl->summarized_tli = 0;
		WalSummarizerCtl->summarized_lsn = InvalidXLogRecPtr;
		WalSummarizerCtl->lsn_is_exact = false;
		WalSummarizerCtl->summarizer_pgprocno = INVALID_PROC_NUMBER;
		WalSummarizerCtl->pending_lsn = InvalidXLogRecPtr;
		ConditionVariableInit(&WalSummarizerCtl->summary_file_cv);
	}
}

/*
 * Entry point for walsummarizer process.
 * walsummarizer 进程的入口点。
 */
void
WalSummarizerMain(const void *startup_data, size_t startup_data_len)
{
	sigjmp_buf	local_sigjmp_buf;
	MemoryContext context;

	/*
	 * Within this function, 'current_lsn' and 'current_tli' refer to the
	 * point from which the next WAL summary file should start. 'exact' is
	 * true if 'current_lsn' is known to be the start of a WAL record or WAL
	 * segment, and false if it might be in the middle of a record someplace.
	 * 在此函数中，'current_lsn' 和 'current_tli' 指的是下一个 WAL 汇总文件应该开始的位置。
	 * 如果已知 'current_lsn' 是 WAL 记录或 WAL 段的起点，则 'exact' 为 true，
	 * 如果它可能位于某处记录的中间，则为 false。
	 *
	 * 'switch_lsn' and 'switch_tli', if set, are the LSN at which we need to
	 * switch to a new timeline and the timeline to which we need to switch.
	 * If not set, we either haven't figured out the answers yet or we're
	 * already on the latest timeline.
	 * 'switch_lsn' 和 'switch_tli'（如果已设置）是我们需要切换到新时间线的 LSN，
	 * 以及我们需要切换到的时间线。如果未设置，说明我们要么还没有计算出结果，
	 * 要么已经处于最新时间线上。
	 */
	XLogRecPtr	current_lsn;
	TimeLineID	current_tli;
	bool		exact;
	XLogRecPtr	switch_lsn = InvalidXLogRecPtr;
	TimeLineID	switch_tli = 0;

	Assert(startup_data_len == 0);

	MyBackendType = B_WAL_SUMMARIZER;
	AuxiliaryProcessMainCommon();

	ereport(DEBUG1,
			(errmsg_internal("WAL summarizer started")));

	/*
	 * Properly accept or ignore signals the postmaster might send us
	 * 正确接受或忽略 postmaster 可能发送给我们的信号
	 *
	 * We have no particular use for SIGINT at the moment, but seems
	 * reasonable to treat like SIGTERM.
	 * 我们目前对 SIGINT 没有特别的用途，但似乎像对待 SIGTERM 一样对待它是合理的。
	 */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGINT, SignalHandlerForShutdownRequest);
	pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
	/* SIGQUIT handler was already set up by InitPostmasterChild */
	pqsignal(SIGALRM, SIG_IGN);
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, SIG_IGN); /* not used */

	/* Advertise ourselves. */
	/* 宣告我们自己的存在。 */
	on_shmem_exit(WalSummarizerShutdown, (Datum) 0);
	LWLockAcquire(WALSummarizerLock, LW_EXCLUSIVE);
	WalSummarizerCtl->summarizer_pgprocno = MyProcNumber;
	LWLockRelease(WALSummarizerLock);

	/* Create and switch to a memory context that we can reset on error. */
	/* 创建并切换到一个在出错时可以重置的内存上下文。 */
	context = AllocSetContextCreate(TopMemoryContext,
									"Wal Summarizer",
									ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(context);

	/*
	 * Reset some signals that are accepted by postmaster but not here
	 * 重置一些 postmaster 接受但这里不接受的信号
	 */
	pqsignal(SIGCHLD, SIG_DFL);

	/*
	 * If an exception is encountered, processing resumes here.
	 * 如果遇到异常，处理将在此处恢复。
	 */
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/* Since not using PG_TRY, must reset error stack by hand */
		/* 由于没有使用 PG_TRY，必须手动重置错误栈 */
		error_context_stack = NULL;

		/* Prevent interrupts while cleaning up */
		/* 清理时防止中断 */
		HOLD_INTERRUPTS();

		/* Report the error to the server log */
		/* 将错误报告给服务器日志 */
		EmitErrorReport();

		/* Release resources we might have acquired. */
		/* 释放我们可能已获取的资源。 */
		LWLockReleaseAll();
		ConditionVariableCancelSleep();
		pgstat_report_wait_end();
		pgaio_error_cleanup();
		ReleaseAuxProcessResources(false);
		AtEOXact_Files(false);
		AtEOXact_HashTables(false);

		/*
		 * Now return to normal top-level context and clear ErrorContext for
		 * next time.
		 * 现在返回到正常的顶级上下文并清除 ErrorContext 以备下次使用。
		 */
		MemoryContextSwitchTo(context);
		FlushErrorState();

		/* Flush any leaked data in the top-level context */
		/* 刷新顶级上下文中泄漏的任何数据 */
		MemoryContextReset(context);

		/* Now we can allow interrupts again */
		/* 现在我们可以再次允许中断 */
		RESUME_INTERRUPTS();

		/*
		 * Sleep for 10 seconds before attempting to resume operations in
		 * order to avoid excessive logging.
		 * 休眠 10 秒，然后尝试恢复操作，以避免过度记录日志。
		 *
		 * Many of the likely error conditions are things that will repeat
		 * every time. For example, if the WAL can't be read or the summary
		 * can't be written, only administrator action will cure the problem.
		 * So a really fast retry time doesn't seem to be especially
		 * beneficial, and it will clutter the logs.
		 * 许多可能的错误条件都是每次都会重复发生的事情。例如，如果无法读取 WAL 
		 * 或无法写入汇总，则只有管理员操作才能解决问题。因此，非常快的重试时间
		 * 似乎并不特别有益，而且它会使日志杂乱无章。
		 */
		(void) WaitLatch(NULL,
						 WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 10000,
						 WAIT_EVENT_WAL_SUMMARIZER_ERROR);
	}

	/* We can now handle ereport(ERROR) */
	/* 我们现在可以处理 ereport(ERROR) 了 */
	PG_exception_stack = &local_sigjmp_buf;

	/*
	 * Unblock signals (they were blocked when the postmaster forked us)
	 * 解除信号阻塞（它们在 postmaster fork 我们时被阻塞了）
	 */
	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

	/*
	 * Fetch information about previous progress from shared memory, and ask
	 * GetOldestUnsummarizedLSN to reset pending_lsn to summarized_lsn. We
	 * might be recovering from an error, and if so, pending_lsn might have
	 * advanced past summarized_lsn, but any WAL we read previously has been
	 * lost and will need to be reread.
	 * 从共享内存中获取先前进度的信息，并要求 GetOldestUnsummarizedLSN 将 
	 * pending_lsn 重置为 summarized_lsn。我们可能正在从错误中恢复，如果是这样，
	 * pending_lsn 可能已经超越了 summarized_lsn，但是我们之前读取的任何 WAL 
	 * 都已丢失，需要重新读取。
	 *
	 * If we discover that WAL summarization is not enabled, just exit.
	 * 如果我们发现未启用 WAL 归纳，直接退出。
	 */
	current_lsn = GetOldestUnsummarizedLSN(&current_tli, &exact);
	if (XLogRecPtrIsInvalid(current_lsn))
		proc_exit(0);

	/*
	 * Loop forever
	 * 无限循环
	 */
	for (;;)
	{
		XLogRecPtr	latest_lsn;
		TimeLineID	latest_tli;
		XLogRecPtr	end_of_summary_lsn;

		/* Flush any leaked data in the top-level context */
		/* 刷新顶级上下文中泄漏的任何数据 */
		MemoryContextReset(context);

		/* Process any signals received recently. */
		/* 处理最近收到的任何信号。 */
		ProcessWalSummarizerInterrupts();

		/* If it's time to remove any old WAL summaries, do that now. */
		/* 如果是时候删除旧的 WAL 汇总文件了，现在就执行。 */
		MaybeRemoveOldWalSummaries();

		/* Find the LSN and TLI up to which we can safely summarize. */
		/* 找到我们可以安全归纳到的 LSN 和 TLI。 */
		latest_lsn = GetLatestLSN(&latest_tli);

		/*
		 * If we're summarizing a historic timeline and we haven't yet
		 * computed the point at which to switch to the next timeline, do that
		 * now.
		 * 如果我们正在归纳一个历史时间线，并且尚未计算出切换到下一个时间线的点，
		 * 则现在进行计算。
		 *
		 * Note that if this is a standby, what was previously the current
		 * timeline could become historic at any time.
		 * 注意，如果这是备用数据库（standby），先前当前的时间线可能随时变为历史时间线。
		 *
		 * We could try to make this more efficient by caching the results of
		 * readTimeLineHistory when latest_tli has not changed, but since we
		 * only have to do this once per timeline switch, we probably wouldn't
		 * save any significant amount of work in practice.
		 * 我们可以尝试通过在 latest_tli 未改变时缓存 readTimeLineHistory 的结果来提高效率，
		 * 但由于每个时间线切换只需要执行一次，在实际中我们可能不会节省任何可观的工作量。
		 */
		if (current_tli != latest_tli && XLogRecPtrIsInvalid(switch_lsn))
		{
			List	   *tles = readTimeLineHistory(latest_tli);

			switch_lsn = tliSwitchPoint(current_tli, tles, &switch_tli);
			ereport(DEBUG1,
					errmsg_internal("switch point from TLI %u to TLI %u is at %X/%X",
									current_tli, switch_tli, LSN_FORMAT_ARGS(switch_lsn)));
		}

		/*
		 * If we've reached the switch LSN, we can't summarize anything else
		 * on this timeline. Switch to the next timeline and go around again,
		 * backing up to the exact switch point if we passed it.
		 * 如果我们达到了切换 LSN，我们就不能在此时间线上归纳任何其他内容。
		 * 切换到下一个时间线并重新开始，如果我们超越了确切的切换点，则回退到该点。
		 */
		if (!XLogRecPtrIsInvalid(switch_lsn) && current_lsn >= switch_lsn)
		{
			/* Restart summarization from switch point. */
			/* 从切换点重新启动归纳。 */
			current_tli = switch_tli;
			current_lsn = switch_lsn;

			/* Next timeline and switch point, if any, not yet known. */
			/* 下一个时间线和切换点（如果有）尚不可知。 */
			switch_lsn = InvalidXLogRecPtr;
			switch_tli = 0;

			/* Update (really, rewind, if needed) state in shared memory. */
			/* 更新（如果需要，实际上是倒回）共享内存中的状态。 */
			LWLockAcquire(WALSummarizerLock, LW_EXCLUSIVE);
			WalSummarizerCtl->summarized_lsn = current_lsn;
			WalSummarizerCtl->summarized_tli = current_tli;
			WalSummarizerCtl->lsn_is_exact = true;
			WalSummarizerCtl->pending_lsn = current_lsn;
			LWLockRelease(WALSummarizerLock);

			continue;
		}

		/* Summarize WAL. */
		/* 归纳 WAL。 */
		end_of_summary_lsn = SummarizeWAL(current_tli,
										  current_lsn, exact,
										  switch_lsn, latest_lsn);
		Assert(!XLogRecPtrIsInvalid(end_of_summary_lsn));
		Assert(end_of_summary_lsn >= current_lsn);

		/*
		 * Update state for next loop iteration.
		 * 更新下一次循环迭代的状态。
		 *
		 * Next summary file should start from exactly where this one ended.
		 * 下一个汇总文件应该正好从当前文件结束的位置开始。
		 */
		current_lsn = end_of_summary_lsn;
		exact = true;

		/* Update state in shared memory. */
		/* 更新共享内存中的状态。 */
		LWLockAcquire(WALSummarizerLock, LW_EXCLUSIVE);
		WalSummarizerCtl->summarized_lsn = end_of_summary_lsn;
		WalSummarizerCtl->summarized_tli = current_tli;
		WalSummarizerCtl->lsn_is_exact = true;
		WalSummarizerCtl->pending_lsn = end_of_summary_lsn;
		LWLockRelease(WALSummarizerLock);

		/* Wake up anyone waiting for more summary files to be written. */
		/* 唤醒所有正在等待写入更多汇总文件的人。 */
		ConditionVariableBroadcast(&WalSummarizerCtl->summary_file_cv);
	}
}

/*
 * Get information about the state of the WAL summarizer.
 * 获取关于 WAL 归纳器状态的信息。
 */
void
GetWalSummarizerState(TimeLineID *summarized_tli, XLogRecPtr *summarized_lsn,
					  XLogRecPtr *pending_lsn, int *summarizer_pid)
{
	LWLockAcquire(WALSummarizerLock, LW_SHARED);
	if (!WalSummarizerCtl->initialized)
	{
		/*
		 * If initialized is false, the rest of the structure contents are
		 * undefined.
		 * 如果 initialized 为 false，则结构体内容的其余部分是未定义的。
		 */
		*summarized_tli = 0;
		*summarized_lsn = InvalidXLogRecPtr;
		*pending_lsn = InvalidXLogRecPtr;
		*summarizer_pid = -1;
	}
	else
	{
		int			summarizer_pgprocno = WalSummarizerCtl->summarizer_pgprocno;

		*summarized_tli = WalSummarizerCtl->summarized_tli;
		*summarized_lsn = WalSummarizerCtl->summarized_lsn;
		if (summarizer_pgprocno == INVALID_PROC_NUMBER)
		{
			/*
			 * If the summarizer has exited, the fact that it had processed
			 * beyond summarized_lsn is irrelevant now.
			 * 如果归纳器已退出，则它已处理超出 summarized_lsn 的事实现在无关紧要。
			 */
			*pending_lsn = WalSummarizerCtl->summarized_lsn;
			*summarizer_pid = -1;
		}
		else
		{
			*pending_lsn = WalSummarizerCtl->pending_lsn;

			/*
			 * We're not fussed about inexact answers here, since they could
			 * become stale instantly, so we don't bother taking the lock, but
			 * make sure that invalid PID values are normalized to -1.
			 * 我们在这里并不介意不精确的答案，因为它们可能会立即过时，所以我们不麻烦获取锁，
			 * 但要确保无效的 PID 值被规范化为 -1。
			 */
			*summarizer_pid = GetPGProcByNumber(summarizer_pgprocno)->pid;
			if (*summarizer_pid <= 0)
				*summarizer_pid = -1;
		}
	}
	LWLockRelease(WALSummarizerLock);
}

/*
 * Get the oldest LSN in this server's timeline history that has not yet been
 * summarized, and update shared memory state as appropriate.
 * 获取此服务器时间线历史中尚未归纳的最旧 LSN，并酌情更新共享内存状态。
 *
 * If *tli != NULL, it will be set to the TLI for the LSN that is returned.
 * 如果 *tli != NULL，它将被设置为返回的 LSN 的 TLI。
 *
 * If *lsn_is_exact != NULL, it will be set to true if the returned LSN is
 * necessarily the start of a WAL record and false if it's just the beginning
 * of a WAL segment.
 * 如果 *lsn_is_exact != NULL，当返回的 LSN 必然是 WAL 记录的起点时它将被设置为 true，
 * 如果只是 WAL 段的起点则为 false。
 */
XLogRecPtr
GetOldestUnsummarizedLSN(TimeLineID *tli, bool *lsn_is_exact)
{
	TimeLineID	latest_tli;
	int			n;
	List	   *tles;
	XLogRecPtr	unsummarized_lsn = InvalidXLogRecPtr;
	TimeLineID	unsummarized_tli = 0;
	bool		should_make_exact = false;
	List	   *existing_summaries;
	ListCell   *lc;
	bool		am_wal_summarizer = AmWalSummarizerProcess();

	/* If not summarizing WAL, do nothing. */
	/* 如果不归纳 WAL，则什么都不做。 */
	if (!summarize_wal)
		return InvalidXLogRecPtr;

	/*
	 * If we are not the WAL summarizer process, then we normally just want to
	 * read the values from shared memory. However, as an exception, if shared
	 * memory hasn't been initialized yet, then we need to do that so that we
	 * can read legal values and not remove any WAL too early.
	 * 如果我们不是 WAL 归纳器进程，那么通常我们只想从共享内存中读取值。
	 * 但是，作为一个例外，如果共享内存尚未初始化，那么我们需要这样做，
	 * 以便我们可以读取合法值，并且不会过早地删除任何 WAL。
	 */
	if (!am_wal_summarizer)
	{
		LWLockAcquire(WALSummarizerLock, LW_SHARED);

		if (WalSummarizerCtl->initialized)
		{
			unsummarized_lsn = WalSummarizerCtl->summarized_lsn;
			if (tli != NULL)
				*tli = WalSummarizerCtl->summarized_tli;
			if (lsn_is_exact != NULL)
				*lsn_is_exact = WalSummarizerCtl->lsn_is_exact;
			LWLockRelease(WALSummarizerLock);
			return unsummarized_lsn;
		}

		LWLockRelease(WALSummarizerLock);
	}

	/*
	 * Find the oldest timeline on which WAL still exists, and the earliest
	 * segment for which it exists.
	 * 找到仍存在 WAL 的最旧时间线，以及它所存在的最早分段。
	 *
	 * Note that we do this every time the WAL summarizer process restarts or
	 * recovers from an error, in case the contents of pg_wal have changed
	 * under us e.g. if some files were removed, either manually - which
	 * shouldn't really happen, but might - or by postgres itself, if
	 * summarize_wal was turned off and then back on again.
	 * 注意，我们每次在 WAL 归纳器进程重启或从错误中恢复时都会这样做，
	 * 以防 pg_wal 的内容发生变化，例如，如果某些文件被删除（无论是手动删除——
	 * 这不应该真正发生，但可能会发生——还是由 postgres 本身删除，如果
	 * summarize_wal 被关闭然后又再次打开）。
	 */
	(void) GetLatestLSN(&latest_tli);
	tles = readTimeLineHistory(latest_tli);
	for (n = list_length(tles) - 1; n >= 0; --n)
	{
		TimeLineHistoryEntry *tle = list_nth(tles, n);
		XLogSegNo	oldest_segno;

		oldest_segno = XLogGetOldestSegno(tle->tli);
		if (oldest_segno != 0)
		{
			/* Compute oldest LSN that still exists on disk. */
			/* 计算仍在磁盘上存在的最旧 LSN。 */
			XLogSegNoOffsetToRecPtr(oldest_segno, 0, wal_segment_size,
									unsummarized_lsn);

			unsummarized_tli = tle->tli;
			break;
		}
	}

	/*
	 * Don't try to summarize anything older than the end LSN of the newest
	 * summary file that exists for this timeline.
	 * 不要尝试归纳任何早于为此时间线存在的最新汇总文件的结束 LSN 的内容。
	 */
	existing_summaries =
		GetWalSummaries(unsummarized_tli,
						InvalidXLogRecPtr, InvalidXLogRecPtr);
	foreach(lc, existing_summaries)
	{
		WalSummaryFile *ws = lfirst(lc);

		if (ws->end_lsn > unsummarized_lsn)
		{
			unsummarized_lsn = ws->end_lsn;
			should_make_exact = true;
		}
	}

	/* It really should not be possible for us to find no WAL. */
	/* 我们找不到 WAL 实在是不太可能。 */
	if (unsummarized_tli == 0)
		ereport(ERROR,
				errcode(ERRCODE_INTERNAL_ERROR),
				errmsg_internal("no WAL found on timeline %u", latest_tli));

	/*
	 * If we're the WAL summarizer, we always want to store the values we just
	 * computed into shared memory, because those are the values we're going
	 * to use to drive our operation, and so they are the authoritative
	 * values. Otherwise, we only store values into shared memory if shared
	 * memory is uninitialized. Our values are not canonical in such a case,
	 * but it's better to have something than nothing, to guide WAL retention.
	 * 如果我们是 WAL 归纳器，我们总是希望将刚刚计算出的值存储到共享内存中，
	 * 因为这些是我们将要用来驱动操作的值，因此它们是权威值。
	 * 否则，我们只在共享内存未初始化时才将值存储到共享内存中。在这种情况下，
	 * 我们的值并不是规范的，但有总比没有好，可以用以指导 WAL 的保留。
	 */
	LWLockAcquire(WALSummarizerLock, LW_EXCLUSIVE);
	if (am_wal_summarizer || !WalSummarizerCtl->initialized)
	{
		WalSummarizerCtl->initialized = true;
		WalSummarizerCtl->summarized_lsn = unsummarized_lsn;
		WalSummarizerCtl->summarized_tli = unsummarized_tli;
		WalSummarizerCtl->lsn_is_exact = should_make_exact;
		WalSummarizerCtl->pending_lsn = unsummarized_lsn;
	}
	else
		unsummarized_lsn = WalSummarizerCtl->summarized_lsn;

	/* Also return the to the caller as required. */
	/* 同时也按要求将内容返回给调用者。 */
	if (tli != NULL)
		*tli = WalSummarizerCtl->summarized_tli;
	if (lsn_is_exact != NULL)
		*lsn_is_exact = WalSummarizerCtl->lsn_is_exact;
	LWLockRelease(WALSummarizerLock);

	return unsummarized_lsn;
}

/*
 * Wake up the WAL summarizer process.
 * 唤醒 WAL 归纳器进程。
 *
 * This might not work, because there's no guarantee that the WAL summarizer
 * process was successfully started, and it also might have started but
 * subsequently terminated. So, under normal circumstances, this will get the
 * latch set, but there's no guarantee.
 * 这可能不起作用，因为不能保证 WAL 归纳器进程已成功启动，而且它也可能启动了
 * 但随后终止了。因此，在正常情况下，这会设置 latch，但不能保证百分之百成功。
 */
void
WakeupWalSummarizer(void)
{
	ProcNumber	pgprocno;

	if (WalSummarizerCtl == NULL)
		return;

	LWLockAcquire(WALSummarizerLock, LW_SHARED);
	pgprocno = WalSummarizerCtl->summarizer_pgprocno;
	LWLockRelease(WALSummarizerLock);

	if (pgprocno != INVALID_PROC_NUMBER)
		SetLatch(&ProcGlobal->allProcs[pgprocno].procLatch);
}

/*
 * Wait until WAL summarization reaches the given LSN, but time out with an
 * error if the summarizer seems to be stick.
 * 等待直至 WAL 归纳达到给定的 LSN，但如果归纳器似乎卡住，则因超时报错。
 *
 * Returns immediately if summarize_wal is turned off while we wait. Caller
 * is expected to handle this case, if necessary.
 * 如果在我们等待期间 summarize_wal 被关闭，则立即返回。调用者应在必要时处理此情况。
 */
void
WaitForWalSummarization(XLogRecPtr lsn)
{
	TimestampTz initial_time,
				cycle_time,
				current_time;
	XLogRecPtr	prior_pending_lsn = InvalidXLogRecPtr;
	int			deadcycles = 0;

	initial_time = cycle_time = GetCurrentTimestamp();

	while (1)
	{
		long		timeout_in_ms = 10000;
		XLogRecPtr	summarized_lsn;
		XLogRecPtr	pending_lsn;

		CHECK_FOR_INTERRUPTS();

		/* If WAL summarization is disabled while we're waiting, give up. */
		/* 如果在我们等待期间 WAL 归纳被禁用，则放弃。 */
		if (!summarize_wal)
			return;

		/*
		 * If the LSN summarized on disk has reached the target value, stop.
		 * 如果磁盘上已归纳的 LSN 已达到目标值，则停止。
		 */
		LWLockAcquire(WALSummarizerLock, LW_SHARED);
		summarized_lsn = WalSummarizerCtl->summarized_lsn;
		pending_lsn = WalSummarizerCtl->pending_lsn;
		LWLockRelease(WALSummarizerLock);

		/* If WAL summarization has progressed sufficiently, stop waiting. */
		/* 如果 WAL 归纳已经有了足够的进展，则停止等待。 */
		if (summarized_lsn >= lsn)
			break;

		/* Recheck current time. */
		/* 重新检查当前时间。 */
		current_time = GetCurrentTimestamp();

		/* Have we finished the current cycle of waiting? */
		/* 我们是否完成了当前的等待周期？ */
		if (TimestampDifferenceMilliseconds(cycle_time,
											current_time) >= timeout_in_ms)
		{
			long		elapsed_seconds;

			/* Begin new wait cycle. */
			/* 开始新的等待周期。 */
			cycle_time = TimestampTzPlusMilliseconds(cycle_time,
													 timeout_in_ms);

			/*
			 * Keep track of the number of cycles during which there has been
			 * no progression of pending_lsn. If pending_lsn is not advancing,
			 * that means that not only are no new files appearing on disk,
			 * but we're not even incorporating new records into the in-memory
			 * state.
			 * 跟踪 pending_lsn 没有进展的周期数。如果 pending_lsn 没有推进，
			 * 那意味着不仅磁盘上没有出现新文件，而且我们甚至没有将新记录并入内存状态中。
			 */
			if (pending_lsn > prior_pending_lsn)
			{
				prior_pending_lsn = pending_lsn;
				deadcycles = 0;
			}
			else
				++deadcycles;

			/*
			 * If we've managed to wait for an entire minute without the WAL
			 * summarizer absorbing a single WAL record, error out; probably
			 * something is wrong.
			 * 如果我们设法等待了整整一分钟而没有 WAL 归纳器吸收哪怕一条 WAL 记录，
			 * 则报错；可能出问题了。
			 *
			 * We could consider also erroring out if the summarizer is taking
			 * too long to catch up, but it's not clear what rate of progress
			 * would be acceptable and what would be too slow. So instead, we
			 * just try to error out in the case where there's no progress at
			 * all. That seems likely to catch a reasonable number of the
			 * things that can go wrong in practice (e.g. the summarizer
			 * process is completely hung, say because somebody hooked up a
			 * debugger to it or something) without giving up too quickly when
			 * the system is just slow.
			 * 我们也可以考虑在归纳器花费太长时间去赶上时报错，但目前尚不清楚什么样的进度速率
			 * 是可以接受的，什么样是太慢了。因此，我们只是尝试在完全没有进展的情况下报错。
			 * 这在实际中似乎能够捕获相当一部分可能出错的情况（例如，归纳器进程完全挂起，
			 * 比如说因为有人把调试器挂到了上面），而又不会在系统仅仅是慢时过快放弃。
			 */
			if (deadcycles >= 6)
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("WAL summarization is not progressing"),
						 errdetail("Summarization is needed through %X/%X, but is stuck at %X/%X on disk and %X/%X in memory.",
								   LSN_FORMAT_ARGS(lsn),
								   LSN_FORMAT_ARGS(summarized_lsn),
								   LSN_FORMAT_ARGS(pending_lsn))));


			/*
			 * Otherwise, just let the user know what's happening.
			 * 否则，只需让用户知道正在发生什么。
			 */
			elapsed_seconds =
				TimestampDifferenceMilliseconds(initial_time,
												current_time) / 1000;
			ereport(WARNING,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg_plural("still waiting for WAL summarization through %X/%X after %ld second",
								   "still waiting for WAL summarization through %X/%X after %ld seconds",
								   elapsed_seconds,
								   LSN_FORMAT_ARGS(lsn),
								   elapsed_seconds),
					 errdetail("Summarization has reached %X/%X on disk and %X/%X in memory.",
							   LSN_FORMAT_ARGS(summarized_lsn),
							   LSN_FORMAT_ARGS(pending_lsn))));
		}

		/*
		 * Align the wait time to prevent drift. This doesn't really matter,
		 * but we'd like the warnings about how long we've been waiting to say
		 * 10 seconds, 20 seconds, 30 seconds, 40 seconds ... without ever
		 * drifting to something that is not a multiple of ten.
		 * 对齐等待时间以防止漂移。这其实并不重要，但我们希望关于我们等待了多久的警告能够说
		 * 10 秒、20 秒、30 秒、40 秒……而不会漂移到非 10 的倍数。
		 */
		timeout_in_ms -=
			TimestampDifferenceMilliseconds(cycle_time, current_time);

		/* Wait and see. */
		/* 等待并观察。 */
		ConditionVariableTimedSleep(&WalSummarizerCtl->summary_file_cv,
									timeout_in_ms,
									WAIT_EVENT_WAL_SUMMARY_READY);
	}

	ConditionVariableCancelSleep();
}

/*
 * On exit, update shared memory to make it clear that we're no longer
 * running.
 * 退出时，更新共享内存以明确表示我们不再运行。
 */
static void
WalSummarizerShutdown(int code, Datum arg)
{
	LWLockAcquire(WALSummarizerLock, LW_EXCLUSIVE);
	WalSummarizerCtl->summarizer_pgprocno = INVALID_PROC_NUMBER;
	LWLockRelease(WALSummarizerLock);
}

/*
 * Get the latest LSN that is eligible to be summarized, and set *tli to the
 * corresponding timeline.
 * 获取符合归纳条件的最新的 LSN，并将 *tli 设置为相应的时间线。
 */
static XLogRecPtr
GetLatestLSN(TimeLineID *tli)
{
	if (!RecoveryInProgress())
	{
		/* Don't summarize WAL before it's flushed. */
		/* 在 WAL 被刷写（flushed）之前，不要对其进行归纳。 */
		return GetFlushRecPtr(tli);
	}
	else
	{
		XLogRecPtr	flush_lsn;
		TimeLineID	flush_tli;
		XLogRecPtr	replay_lsn;
		TimeLineID	replay_tli;
		TimeLineID	insert_tli;

		/*
		 * After the insert TLI has been set and before the control file has
		 * been updated to show the DB in production, RecoveryInProgress()
		 * will return true, because it's not yet safe for all backends to
		 * begin writing WAL. However, replay has already ceased, so from our
		 * point of view, recovery is already over. We should summarize up to
		 * where replay stopped and then prepare to resume at the start of the
		 * insert timeline.
		 * 在设置了插入 TLI 之后以及更新控制文件以显示数据库处于生产状态之前，
		 * RecoveryInProgress() 将返回 true，因为对于所有后台进程来说，开始写入 WAL 
		 * 还不够安全。但是，重放已经停止，所以从我们的角度来看，恢复已经结束。
		 * 我们应该归纳到重放停止的位置，然后准备在插入时间线的起点恢复。
		 */
		if ((insert_tli = GetWALInsertionTimeLineIfSet()) != 0)
		{
			*tli = insert_tli;
			return GetXLogReplayRecPtr(NULL);
		}

		/*
		 * What we really want to know is how much WAL has been flushed to
		 * disk, but the only flush position available is the one provided by
		 * the walreceiver, which may not be running, because this could be
		 * crash recovery or recovery via restore_command. So use either the
		 * WAL receiver's flush position or the replay position, whichever is
		 * further ahead, on the theory that if the WAL has been replayed then
		 * it must also have been flushed to disk.
		 * 我们真正想知道的是有多少 WAL 已经刷写到了磁盘，但唯一可用的刷写位置是
		 * walreceiver 提供的那个，而它可能没有运行，因为这可能是崩溃恢复或通过
		 * restore_command 进行的恢复。因此，使用 WAL 接收器的刷写位置或重放位置中
		 * 较前的那个，理论依据是，如果 WAL 已经被重放，那么它肯定也已经被刷写到了磁盘。
		 */
		flush_lsn = GetWalRcvFlushRecPtr(NULL, &flush_tli);
		replay_lsn = GetXLogReplayRecPtr(&replay_tli);
		if (flush_lsn > replay_lsn)
		{
			*tli = flush_tli;
			return flush_lsn;
		}
		else
		{
			*tli = replay_tli;
			return replay_lsn;
		}
	}
}

/*
 * Interrupt handler for main loop of WAL summarizer process.
 * WAL 归纳器进程主循环的中断处理程序。
 */
static void
ProcessWalSummarizerInterrupts(void)
{
	if (ProcSignalBarrierPending)
		ProcessProcSignalBarrier();

	if (ConfigReloadPending)
	{
		ConfigReloadPending = false;
		ProcessConfigFile(PGC_SIGHUP);
	}

	if (ShutdownRequestPending || !summarize_wal)
	{
		ereport(DEBUG1,
				errmsg_internal("WAL summarizer shutting down"));
		proc_exit(0);
	}

	/* Perform logging of memory contexts of this process */
	/* 记录该进程的内存上下文日志 */
	if (LogMemoryContextPending)
		ProcessLogMemoryContextInterrupt();
}

/*
 * Summarize a range of WAL records on a single timeline.
 * 归纳单个时间线上的一定范围的 WAL 记录。
 *
 * 'tli' is the timeline to be summarized.
 * 'tli' 是要归纳的时间线。
 *
 * 'start_lsn' is the point at which we should start summarizing. If this
 * value comes from the end LSN of the previous record as returned by the
 * xlogreader machinery, 'exact' should be true; otherwise, 'exact' should
 * be false, and this function will search forward for the start of a valid
 * WAL record.
 * 'start_lsn' 是我们应该开始归纳的点。如果该值来自 xlogreader 机制返回的上一条记录的
 * 结束 LSN，则 'exact' 应该为 true；否则，'exact' 应该为 false，此函数将向前搜索
 * 另一条有效的 WAL 记录的起点。
 *
 * 'switch_lsn' is the point at which we should switch to a later timeline,
 * if we're summarizing a historic timeline.
 * 如果我们正在归纳历史时间线，'switch_lsn' 是我们应该切换到较晚时间线的点。
 *
 * 'maximum_lsn' identifies the point beyond which we can't count on being
 * able to read any more WAL. It should be the switch point when reading a
 * historic timeline, or the most-recently-measured end of WAL when reading
 * the current timeline.
 * 'maximum_lsn' 标识了一个点，超过这个点我们不能指望能够读取更多的 WAL。
 * 读取历史时间线时它应该是切换点，读取当前时间线时应该是最近测量的 WAL 终点。
 *
 * The return value is the LSN at which the WAL summary actually ends. Most
 * often, a summary file ends because we notice that a checkpoint has
 * occurred and reach the redo pointer of that checkpoint, but sometimes
 * we stop for other reasons, such as a timeline switch.
 * 返回值是 WAL 汇总实际结束处的 LSN。最常见的情况是，汇总文件之所以结束，
 * 是因为我们注意到发生了一个检查点并且达到了该检查点的重做指针（redo pointer），
 * 但有时我们也会出于其他原因而停止，比如时间线切换。
 */
static XLogRecPtr
SummarizeWAL(TimeLineID tli, XLogRecPtr start_lsn, bool exact,
			 XLogRecPtr switch_lsn, XLogRecPtr maximum_lsn)
{
	SummarizerReadLocalXLogPrivate *private_data;
	XLogReaderState *xlogreader;
	XLogRecPtr	summary_start_lsn;
	XLogRecPtr	summary_end_lsn = switch_lsn;
	char		temp_path[MAXPGPATH];
	char		final_path[MAXPGPATH];
	WalSummaryIO io;
	BlockRefTable *brtab = CreateEmptyBlockRefTable();
	bool		fast_forward = true;

	/* Initialize private data for xlogreader. */
	/* 初始化 xlogreader 的私有数据。 */
	private_data = (SummarizerReadLocalXLogPrivate *)
		palloc0(sizeof(SummarizerReadLocalXLogPrivate));
	private_data->tli = tli;
	private_data->historic = !XLogRecPtrIsInvalid(switch_lsn);
	private_data->read_upto = maximum_lsn;

	/* Create xlogreader. */
	/* 创建 xlogreader。 */
	xlogreader = XLogReaderAllocate(wal_segment_size, NULL,
									XL_ROUTINE(.page_read = &summarizer_read_local_xlog_page,
											   .segment_open = &wal_segment_open,
											   .segment_close = &wal_segment_close),
									private_data);
	if (xlogreader == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed while allocating a WAL reading processor.")));

	/*
	 * When exact = false, we're starting from an arbitrary point in the WAL
	 * and must search forward for the start of the next record.
	 * 当 exact = false 时，我们是从 WAL 中的任意点开始，必须向前搜索下一条记录的起点。
	 *
	 * When exact = true, start_lsn should be either the LSN where a record
	 * begins, or the LSN of a page where the page header is immediately
	 * followed by the start of a new record. XLogBeginRead should tolerate
	 * either case.
	 * 当 exact = true 时，start_lsn 应该要么是记录开始的 LSN，要么是页面头部紧接着新记录
	 * 起始位置的页面 LSN。XLogBeginRead 应该容忍这两种情况。
	 *
	 * We need to allow for both cases because the behavior of xlogreader
	 * varies. When a record spans two or more xlog pages, the ending LSN
	 * reported by xlogreader will be the starting LSN of the following
	 * record, but when an xlog page boundary falls between two records, the
	 * end LSN for the first will be reported as the first byte of the
	 * following page. We can't know until we read that page how large the
	 * header will be, but we'll have to skip over it to find the next record.
	 * 我们需要允许这两种情况，因为 xlogreader 的行为各不相同。
	 * 当一条记录跨越两个或更多 xlog 页面时，xlogreader 报告的结束 LSN 将是下一条记录的
	 * 起始 LSN，但是当 xlog 页面边界落在两条记录之间时，第一条记录的结束 LSN 将报告为
	 * 下一页的首个字节。在我们读取该页之前，我们无法知道头部会有多大，但是我们必须跳过它
	 * 才能找到下一条记录。
	 */
	if (exact)
	{
		/*
		 * Even if start_lsn is the beginning of a page rather than the
		 * beginning of the first record on that page, we should still use it
		 * as the start LSN for the summary file. That's because we detect
		 * missing summary files by looking for cases where the end LSN of one
		 * file is less than the start LSN of the next file. When only a page
		 * header is skipped, nothing has been missed.
		 * 即使 start_lsn 是页面的起点，而不是该页面上第一条记录的起点，我们仍应将其用作
		 * 汇总文件的开始 LSN。这是因为我们通过寻找一个文件的结束 LSN 小于下一个文件的
		 * 开始 LSN 的情况来检测丢失的汇总文件。当仅仅跳过一个页面头部时，并没有丢失任何内容。
		 */
		XLogBeginRead(xlogreader, start_lsn);
		summary_start_lsn = start_lsn;
	}
	else
	{
		summary_start_lsn = XLogFindNextRecord(xlogreader, start_lsn);
		if (XLogRecPtrIsInvalid(summary_start_lsn))
		{
			/*
			 * If we hit end-of-WAL while trying to find the next valid
			 * record, we must be on a historic timeline that has no valid
			 * records that begin after start_lsn and before end of WAL.
			 * 如果我们在尝试寻找下一个有效记录时遇到了 WAL 末尾，我们必须是在一条历史时间线上，
			 * 该时间线没有任何从 start_lsn 之后、WAL 结束之前开始的有效记录。
			 */
			if (private_data->end_of_wal)
			{
				ereport(DEBUG1,
						errmsg_internal("could not read WAL from timeline %u at %X/%X: end of WAL at %X/%X",
										tli,
										LSN_FORMAT_ARGS(start_lsn),
										LSN_FORMAT_ARGS(private_data->read_upto)));

				/*
				 * The timeline ends at or after start_lsn, without containing
				 * any records. Thus, we must make sure the main loop does not
				 * iterate. If start_lsn is the end of the timeline, then we
				 * won't actually emit an empty summary file, but otherwise,
				 * we must, to capture the fact that the LSN range in question
				 * contains no interesting WAL records.
				 * 时间线在 start_lsn 或之后结束，不包含任何记录。因此，我们必须确保主循环不进行迭代。
				 * 如果 start_lsn 是时间线的末尾，那么我们实际上不会输出空的汇总文件，
				 * 但否则，我们必须输出以捕获所涉及的 LSN 范围不包含有意义的 WAL 记录的事实。
				 */
				summary_start_lsn = start_lsn;
				summary_end_lsn = private_data->read_upto;
				switch_lsn = xlogreader->EndRecPtr;
			}
			else
				ereport(ERROR,
						(errmsg("could not find a valid record after %X/%X",
								LSN_FORMAT_ARGS(start_lsn))));
		}

		/* We shouldn't go backward. */
		/* 我们不应该倒退。 */
		Assert(summary_start_lsn >= start_lsn);
	}

	/*
	 * Main loop: read xlog records one by one.
	 * 主循环：逐个读取 xlog 记录。
	 */
	while (1)
	{
		int			block_id;
		char	   *errormsg;
		XLogRecord *record;
		uint8		rmid;

		ProcessWalSummarizerInterrupts();

		/* We shouldn't go backward. */
		/* 我们不应该倒退。 */
		Assert(summary_start_lsn <= xlogreader->EndRecPtr);

		/* Now read the next record. */
		/* 现在读取下一条记录。 */
		record = XLogReadRecord(xlogreader, &errormsg);
		if (record == NULL)
		{
			if (private_data->end_of_wal)
			{
				/*
				 * This timeline must be historic and must end before we were
				 * able to read a complete record.
				 * 此时间线必定是历史时间线，并且在我们能够读取完整记录之前就已经结束。
				 */
				ereport(DEBUG1,
						errmsg_internal("could not read WAL from timeline %u at %X/%X: end of WAL at %X/%X",
										tli,
										LSN_FORMAT_ARGS(xlogreader->EndRecPtr),
										LSN_FORMAT_ARGS(private_data->read_upto)));
				/* Summary ends at end of WAL. */
				/* 汇总结束于 WAL 的末尾。 */
				summary_end_lsn = private_data->read_upto;
				break;
			}
			if (errormsg)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not read WAL from timeline %u at %X/%X: %s",
								tli, LSN_FORMAT_ARGS(xlogreader->EndRecPtr),
								errormsg)));
			else
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not read WAL from timeline %u at %X/%X",
								tli, LSN_FORMAT_ARGS(xlogreader->EndRecPtr))));
		}

		/* We shouldn't go backward. */
		/* 我们不应该倒退。 */
		Assert(summary_start_lsn <= xlogreader->EndRecPtr);

		if (!XLogRecPtrIsInvalid(switch_lsn) &&
			xlogreader->ReadRecPtr >= switch_lsn)
		{
			/*
			 * Whoops! We've read a record that *starts* after the switch LSN,
			 * contrary to our goal of reading only until we hit the first
			 * record that ends at or after the switch LSN. Pretend we didn't
			 * read it after all by bailing out of this loop right here,
			 * before we do anything with this record.
			 * 哎呀！我们读取了一条“开始”于切换 LSN 之后的记录，这与我们的目标相反
			 * （我们的目标是仅读取到碰到第一条在切换 LSN 或之后结束的记录为止）。
			 * 我们在这里退出此循环，在对该记录执行任何操作之前，假装我们根本没有读取它。
			 *
			 * This can happen because the last record before the switch LSN
			 * might be continued across multiple pages, and then we might
			 * come to a page with XLP_FIRST_IS_OVERWRITE_CONTRECORD set. In
			 * that case, the record that was continued across multiple pages
			 * is incomplete and will be disregarded, and the read will
			 * restart from the beginning of the page that is flagged
			 * XLP_FIRST_IS_OVERWRITE_CONTRECORD.
			 * 发生这种情况的原因是，切换 LSN 之前的最后一条记录可能会跨多个页面继续，
			 * 然后我们可能会遇到一个设置了 XLP_FIRST_IS_OVERWRITE_CONTRECORD 的页面。
			 * 在这种情况下，跨多个页面继续的记录是不完整的，将被忽略，
			 * 并且读取将从标记为 XLP_FIRST_IS_OVERWRITE_CONTRECORD 的页面起点重新开始。
			 *
			 * If this case occurs, we can fairly say that the current summary
			 * file ends at the switch LSN exactly. The first record on the
			 * page marked XLP_FIRST_IS_OVERWRITE_CONTRECORD will be
			 * discovered when generating the next summary file.
			 * 如果发生这种情况，我们完全可以说当前的汇总文件正好结束于切换 LSN。
			 * 在生成下一个汇总文件时，将会发现标记为 XLP_FIRST_IS_OVERWRITE_CONTRECORD 
			 * 的页面上的第一条记录。
			 */
			summary_end_lsn = switch_lsn;
			break;
		}

		/*
		 * Certain types of records require special handling. Redo points and
		 * shutdown checkpoints trigger creation of new summary files and can
		 * also cause us to enter or exit "fast forward" mode. Other types of
		 * records can require special updates to the block reference table.
		 * 某些类型的记录需要特殊处理。重做点和关机检查点会触发新汇总文件的创建，
		 * 并且还可以导致我们进入或退出“快速向前（fast forward）”模式。
		 * 其他类型的记录可能需要对数据块引用表进行特殊的更新。
		 */
		rmid = XLogRecGetRmid(xlogreader);
		if (rmid == RM_XLOG_ID)
		{
			bool		new_fast_forward;

			/*
			 * If we've already processed some WAL records when we hit a redo
			 * point or shutdown checkpoint, then we stop summarization before
			 * including this record in the current file, so that it will be
			 * the first record in the next file.
			 * 如果我们在遇到重做点或关机检查点时已经处理了一些 WAL 记录，
			 * 那么我们在将此记录包含在当前文件中之前停止归纳，以便它成为下一个文件中的第一条记录。
			 *
			 * When we hit one of those record types as the first record in a
			 * file, we adjust our notion of whether we're fast-forwarding.
			 * Any WAL generated with wal_level=minimal must be skipped
			 * without actually generating any summary file, because an
			 * incremental backup that crosses such WAL would be unsafe.
			 * 当我们碰到那些记录类型作为文件中的第一条记录时，我们会调整对是否快速前向的看法。
			 * 使用 wal_level=minimal 生成的任何 WAL 都必须跳过，而不实际生成任何汇总文件，
			 * 因为跨越此类 WAL 的增量备份将是不安全的。
			 */
			if (SummarizeXlogRecord(xlogreader, &new_fast_forward))
			{
				if (xlogreader->ReadRecPtr > summary_start_lsn)
				{
					summary_end_lsn = xlogreader->ReadRecPtr;
					break;
				}
				else
					fast_forward = new_fast_forward;
			}
		}
		else if (!fast_forward)
		{
			/*
			 * This switch handles record types that require extra updates to
			 * the contents of the block reference table.
			 * 此开关分支处理需要对数据块引用表内容进行额外更新的记录类型。
			 */
			switch (rmid)
			{
				case RM_DBASE_ID:
					SummarizeDbaseRecord(xlogreader, brtab);
					break;
				case RM_SMGR_ID:
					SummarizeSmgrRecord(xlogreader, brtab);
					break;
				case RM_XACT_ID:
					SummarizeXactRecord(xlogreader, brtab);
					break;
			}
		}

		/*
		 * If we're in fast-forward mode, we don't really need to do anything.
		 * Otherwise, feed block references from xlog record to block
		 * reference table.
		 * 如果我们处于快速前向模式，我们实际上不需要做任何事情。
		 * 否则，将 xlog 记录中的块引用送入数据块引用表中。
		 */
		if (!fast_forward)
		{
			for (block_id = 0; block_id <= XLogRecMaxBlockId(xlogreader);
				 block_id++)
			{
				RelFileLocator rlocator;
				ForkNumber	forknum;
				BlockNumber blocknum;

				if (!XLogRecGetBlockTagExtended(xlogreader, block_id, &rlocator,
												&forknum, &blocknum, NULL))
					continue;

				/*
				 * As we do elsewhere, ignore the FSM fork, because it's not
				 * fully WAL-logged.
				 * 正如我们在其他地方所做的那样，忽略 FSM 分支，因为它没有被完整地记录在 WAL 中。
				 */
				if (forknum != FSM_FORKNUM)
					BlockRefTableMarkBlockModified(brtab, &rlocator, forknum,
												   blocknum);
			}
		}

		/* Update our notion of where this summary file ends. */
		/* 更新我们对当前汇总文件结束位置的认识。 */
		summary_end_lsn = xlogreader->EndRecPtr;

		/* Also update shared memory. */
		/* 同时也更新共享内存。 */
		LWLockAcquire(WALSummarizerLock, LW_EXCLUSIVE);
		Assert(summary_end_lsn >= WalSummarizerCtl->summarized_lsn);
		WalSummarizerCtl->pending_lsn = summary_end_lsn;
		LWLockRelease(WALSummarizerLock);

		/*
		 * If we have a switch LSN and have reached it, stop before reading
		 * the next record.
		 * 如果我们有切换 LSN 并且已经达到了它，请在读取下一条记录之前停止。
		 */
		if (!XLogRecPtrIsInvalid(switch_lsn) &&
			xlogreader->EndRecPtr >= switch_lsn)
			break;
	}

	/* Destroy xlogreader. */
	/* 销毁 xlogreader。 */
	pfree(xlogreader->private_data);
	XLogReaderFree(xlogreader);

	/*
	 * If a timeline switch occurs, we may fail to make any progress at all
	 * before exiting the loop above. If that happens, we don't write a WAL
	 * summary file at all. We can also skip writing a file if we're in
	 * fast-forward mode.
	 * 如果发生时间线切换，我们可能会在退出上述循环之前根本无法取得任何进展。
	 * 如果发生这种情况，我们根本不会写入 WAL 汇总文件。如果我们处于快速前向模式，
	 * 也可以跳过写入文件。
	 */
	if (summary_end_lsn > summary_start_lsn && !fast_forward)
	{
		/* Generate temporary and final path name. */
		/* 生成临时路径名和最终路径名。 */
		snprintf(temp_path, MAXPGPATH,
				 XLOGDIR "/summaries/temp.summary");
		snprintf(final_path, MAXPGPATH,
				 XLOGDIR "/summaries/%08X%08X%08X%08X%08X.summary",
				 tli,
				 LSN_FORMAT_ARGS(summary_start_lsn),
				 LSN_FORMAT_ARGS(summary_end_lsn));

		/* Open the temporary file for writing. */
		/* 打开临时文件以进行写入。 */
		io.filepos = 0;
		io.file = PathNameOpenFile(temp_path, O_WRONLY | O_CREAT | O_TRUNC);
		if (io.file < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not create file \"%s\": %m", temp_path)));

		/* Write the data. */
		/* 写入数据。 */
		WriteBlockRefTable(brtab, WriteWalSummary, &io);

		/* Close temporary file and shut down xlogreader. */
		/* 关闭临时文件并关闭 xlogreader。 */
		FileClose(io.file);

		/* Tell the user what we did. */
		/* 告诉用户我们做了什么。 */
		ereport(DEBUG1,
				errmsg_internal("summarized WAL on TLI %u from %X/%X to %X/%X",
								tli,
								LSN_FORMAT_ARGS(summary_start_lsn),
								LSN_FORMAT_ARGS(summary_end_lsn)));

		/* Durably rename the new summary into place. */
		/* 持久化地将新汇总文件重命名到位。 */
		durable_rename(temp_path, final_path, ERROR);
	}

	/* If we skipped a non-zero amount of WAL, log a debug message. */
	/* 如果我们跳过了非零数量的 WAL，记录一条调试消息。 */
	if (summary_end_lsn > summary_start_lsn && fast_forward)
		ereport(DEBUG1,
				errmsg_internal("skipped summarizing WAL on TLI %u from %X/%X to %X/%X",
								tli,
								LSN_FORMAT_ARGS(summary_start_lsn),
								LSN_FORMAT_ARGS(summary_end_lsn)));

	return summary_end_lsn;
}

/*
 * Special handling for WAL records with RM_DBASE_ID.
 * 针对含有 RM_DBASE_ID 的 WAL 记录的特殊处理。
 */
static void
SummarizeDbaseRecord(XLogReaderState *xlogreader, BlockRefTable *brtab)
{
	uint8		info = XLogRecGetInfo(xlogreader) & ~XLR_INFO_MASK;

	/*
	 * We use relfilenode zero for a given database OID and tablespace OID to
	 * indicate that all relations with that pair of IDs have been recreated
	 * if they exist at all. Effectively, we're setting a limit block of 0 for
	 * all such relfilenodes.
	 * 我们对给定的数据库 OID 和表空间 OID 使用 relfilenode 零，以指示具有该对 ID 的
	 * 所有关系（如果存在的话）都已重新创建。实际上，我们正在为所有这些 relfilenode 
	 * 设置一个限制块 0。
	 *
	 * Technically, this special handling is only needed in the case of
	 * XLOG_DBASE_CREATE_FILE_COPY, because that can create a whole bunch of
	 * relation files in a directory without logging anything specific to each
	 * one. If we didn't mark the whole DB OID/TS OID combination in some way,
	 * then a tablespace that was dropped after the reference backup and
	 * recreated using the FILE_COPY method prior to the incremental backup
	 * would look just like one that was never touched at all, which would be
	 * catastrophic.
	 * 从技术上讲，这种特殊处理仅在 XLOG_DBASE_CREATE_FILE_COPY 的情况下需要，
	 * 因为这可以在一个目录中创建一大堆关系文件，而不会记录针对每一个关系的任何具体内容。
	 * 如果我们没有以某种方式标记整个 DB OID/TS OID 组合，那么在参考备份之后被删除
	 * 并在增量备份之前使用 FILE_COPY 方法重新创建的表空间，看起来就和完全没有被触碰过
	 * 一模一样，这将是灾难性的。
	 *
	 * But it seems best to adopt this treatment for all records that drop or
	 * create a DB OID/TS OID combination. That's similar to how we treat the
	 * limit block for individual relations, and it's an extra layer of safety
	 * here. We can never lose data by marking more stuff as needing to be
	 * backed up in full.
	 * 但似乎最好对删除或创建 DB OID/TS OID 组合的所有记录都采用这种处理方式。
	 * 这类似于我们处理单个关系的限制块的方式，并且在这里是一层额外的安全防护。
	 * 通过将更多内容标记为需要完全备份，我们永远不会丢失数据。
	 */
	if (info == XLOG_DBASE_CREATE_FILE_COPY)
	{
		xl_dbase_create_file_copy_rec *xlrec;
		RelFileLocator rlocator;

		xlrec =
			(xl_dbase_create_file_copy_rec *) XLogRecGetData(xlogreader);
		rlocator.spcOid = xlrec->tablespace_id;
		rlocator.dbOid = xlrec->db_id;
		rlocator.relNumber = 0;
		BlockRefTableSetLimitBlock(brtab, &rlocator, MAIN_FORKNUM, 0);
	}
	else if (info == XLOG_DBASE_CREATE_WAL_LOG)
	{
		xl_dbase_create_wal_log_rec *xlrec;
		RelFileLocator rlocator;

		xlrec = (xl_dbase_create_wal_log_rec *) XLogRecGetData(xlogreader);
		rlocator.spcOid = xlrec->tablespace_id;
		rlocator.dbOid = xlrec->db_id;
		rlocator.relNumber = 0;
		BlockRefTableSetLimitBlock(brtab, &rlocator, MAIN_FORKNUM, 0);
	}
	else if (info == XLOG_DBASE_DROP)
	{
		xl_dbase_drop_rec *xlrec;
		RelFileLocator rlocator;
		int			i;

		xlrec = (xl_dbase_drop_rec *) XLogRecGetData(xlogreader);
		rlocator.dbOid = xlrec->db_id;
		rlocator.relNumber = 0;
		for (i = 0; i < xlrec->ntablespaces; ++i)
		{
			rlocator.spcOid = xlrec->tablespace_ids[i];
			BlockRefTableSetLimitBlock(brtab, &rlocator, MAIN_FORKNUM, 0);
		}
	}
}

/*
 * Special handling for WAL records with RM_SMGR_ID.
 * 针对含有 RM_SMGR_ID 的 WAL 记录的特殊处理。
 */
static void
SummarizeSmgrRecord(XLogReaderState *xlogreader, BlockRefTable *brtab)
{
	uint8		info = XLogRecGetInfo(xlogreader) & ~XLR_INFO_MASK;

	if (info == XLOG_SMGR_CREATE)
	{
		xl_smgr_create *xlrec;

		/*
		 * If a new relation fork is created on disk, there is no point
		 * tracking anything about which blocks have been modified, because
		 * the whole thing will be new. Hence, set the limit block for this
		 * fork to 0.
		 * 如果在磁盘上创建了新的关系分支（fork），则跟踪修改了哪些数据块没有任何意义，
		 * 因为整个东西都是新的。因此，将此分支的限制块（limit block）设置为 0。
		 *
		 * Ignore the FSM fork, which is not fully WAL-logged.
		 * 忽略 FSM 分支，因为它没有被完整地记录在 WAL 中。
		 */
		xlrec = (xl_smgr_create *) XLogRecGetData(xlogreader);

		if (xlrec->forkNum != FSM_FORKNUM)
			BlockRefTableSetLimitBlock(brtab, &xlrec->rlocator,
									   xlrec->forkNum, 0);
	}
	else if (info == XLOG_SMGR_TRUNCATE)
	{
		xl_smgr_truncate *xlrec;

		xlrec = (xl_smgr_truncate *) XLogRecGetData(xlogreader);

		/*
		 * If a relation fork is truncated on disk, there is no point in
		 * tracking anything about block modifications beyond the truncation
		 * point.
		 * 如果磁盘上的关系分支被截断（truncated），则在截断点之外跟踪任何关于块修改的内容
		 * 都是没有意义的。
		 *
		 * We ignore SMGR_TRUNCATE_FSM here because the FSM isn't fully
		 * WAL-logged and thus we can't track modified blocks for it anyway.
		 * 我们在这里忽略 SMGR_TRUNCATE_FSM，因为 FSM 没有完整地记入 WAL，
		 * 因而无论如何我们都无法为其跟踪修改的块。
		 */
		if ((xlrec->flags & SMGR_TRUNCATE_HEAP) != 0)
			BlockRefTableSetLimitBlock(brtab, &xlrec->rlocator,
									   MAIN_FORKNUM, xlrec->blkno);
		if ((xlrec->flags & SMGR_TRUNCATE_VM) != 0)
			BlockRefTableSetLimitBlock(brtab, &xlrec->rlocator,
									   VISIBILITYMAP_FORKNUM, xlrec->blkno);
	}
}

/*
 * Special handling for WAL records with RM_XACT_ID.
 * 针对含有 RM_XACT_ID 的 WAL 记录的特殊处理。
 */
static void
SummarizeXactRecord(XLogReaderState *xlogreader, BlockRefTable *brtab)
{
	uint8		info = XLogRecGetInfo(xlogreader) & ~XLR_INFO_MASK;
	uint8		xact_info = info & XLOG_XACT_OPMASK;

	if (xact_info == XLOG_XACT_COMMIT ||
		xact_info == XLOG_XACT_COMMIT_PREPARED)
	{
		xl_xact_commit *xlrec = (xl_xact_commit *) XLogRecGetData(xlogreader);
		xl_xact_parsed_commit parsed;
		int			i;

		/*
		 * Don't track modified blocks for any relations that were removed on
		 * commit.
		 * 不要为在提交（commit）时被删除的任何关系跟踪已修改的块。
		 */
		ParseCommitRecord(XLogRecGetInfo(xlogreader), xlrec, &parsed);
		for (i = 0; i < parsed.nrels; ++i)
		{
			ForkNumber	forknum;

			for (forknum = 0; forknum <= MAX_FORKNUM; ++forknum)
				if (forknum != FSM_FORKNUM)
					BlockRefTableSetLimitBlock(brtab, &parsed.xlocators[i],
											   forknum, 0);
		}
	}
	else if (xact_info == XLOG_XACT_ABORT ||
			 xact_info == XLOG_XACT_ABORT_PREPARED)
	{
		xl_xact_abort *xlrec = (xl_xact_abort *) XLogRecGetData(xlogreader);
		xl_xact_parsed_abort parsed;
		int			i;

		/*
		 * Don't track modified blocks for any relations that were removed on
		 * abort.
		 * 不要为在中止（abort）时被删除的任何关系跟踪已修改的块。
		 */
		ParseAbortRecord(XLogRecGetInfo(xlogreader), xlrec, &parsed);
		for (i = 0; i < parsed.nrels; ++i)
		{
			ForkNumber	forknum;

			for (forknum = 0; forknum <= MAX_FORKNUM; ++forknum)
				if (forknum != FSM_FORKNUM)
					BlockRefTableSetLimitBlock(brtab, &parsed.xlocators[i],
											   forknum, 0);
		}
	}
}

/*
 * Special handling for WAL records with RM_XLOG_ID.
 * 针对含有 RM_XLOG_ID 的 WAL 记录的特殊处理。
 *
 * The return value is true if WAL summarization should stop before this
 * record and false otherwise. When the return value is true,
 * *new_fast_forward indicates whether future processing should be done
 * in fast forward mode (i.e. read WAL without emitting summaries) or not.
 * 如果 WAL 归纳应该在此记录之前停止，则返回值为 true，否则为 false。
 * 当返回值为 true 时，*new_fast_forward 指示未来的处理是否应该在
 * 快速前向模式下进行（即读取 WAL 而不输出汇总）。
 */
static bool
SummarizeXlogRecord(XLogReaderState *xlogreader, bool *new_fast_forward)
{
	uint8		info = XLogRecGetInfo(xlogreader) & ~XLR_INFO_MASK;
	int			record_wal_level;

	if (info == XLOG_CHECKPOINT_REDO)
	{
		/* Payload is wal_level at the time record was written. */
		/* 载荷是写入记录时的 wal_level。 */
		memcpy(&record_wal_level, XLogRecGetData(xlogreader), sizeof(int));
	}
	else if (info == XLOG_CHECKPOINT_SHUTDOWN)
	{
		CheckPoint	rec_ckpt;

		/* Extract wal_level at time record was written from payload. */
		/* 从载荷中提取写入记录时的 wal_level。 */
		memcpy(&rec_ckpt, XLogRecGetData(xlogreader), sizeof(CheckPoint));
		record_wal_level = rec_ckpt.wal_level;
	}
	else if (info == XLOG_PARAMETER_CHANGE)
	{
		xl_parameter_change xlrec;

		/* Extract wal_level at time record was written from payload. */
		/* 从载荷中提取写入记录时的 wal_level。 */
		memcpy(&xlrec, XLogRecGetData(xlogreader),
			   sizeof(xl_parameter_change));
		record_wal_level = xlrec.wal_level;
	}
	else if (info == XLOG_END_OF_RECOVERY)
	{
		xl_end_of_recovery xlrec;

		/* Extract wal_level at time record was written from payload. */
		/* 从载荷中提取写入记录时的 wal_level。 */
		memcpy(&xlrec, XLogRecGetData(xlogreader), sizeof(xl_end_of_recovery));
		record_wal_level = xlrec.wal_level;
	}
	else
	{
		/* No special handling required. Return false. */
		/* 不需要特殊处理。返回 false。 */
		return false;
	}

	/*
	 * Redo can only begin at an XLOG_CHECKPOINT_REDO or
	 * XLOG_CHECKPOINT_SHUTDOWN record, so we want WAL summarization to begin
	 * at those points. Hence, when those records are encountered, return
	 * true, so that we stop just before summarizing either of those records.
	 * 重做（Redo）只能从 XLOG_CHECKPOINT_REDO 或 XLOG_CHECKPOINT_SHUTDOWN 记录开始，
	 * 所以我们希望 WAL 归纳从这些点开始。因此，当遇到这些记录时，返回 true，
	 * 以便我们在归纳这两类记录之前恰好停止。
	 *
	 * We also reach here if we just saw XLOG_END_OF_RECOVERY or
	 * XLOG_PARAMETER_CHANGE. These are not places where recovery can start,
	 * but they're still relevant here. A new timeline can begin with
	 * XLOG_END_OF_RECOVERY, so we need to confirm the WAL level at that
	 * point; and a restart can provoke XLOG_PARAMETER_CHANGE after an
	 * intervening change to postgresql.conf, which might force us to stop
	 * summarizing.
	 * 如果我们刚看到 XLOG_END_OF_RECOVERY 或 XLOG_PARAMETER_CHANGE，我们也会到达这里。
	 * 这些不是恢复可以开始的地方，但它们在这里仍然相关。一个新的时间线可以从
	 * XLOG_END_OF_RECOVERY 开始，所以我们需要在那个点确认 WAL 级别；
	 * 并且重启可以在介于其间的 postgresql.conf 修改之后引发 XLOG_PARAMETER_CHANGE，
	 * 这可能会强迫我们停止归纳。
	 */
	*new_fast_forward = (record_wal_level == WAL_LEVEL_MINIMAL);
	return true;
}

/*
 * Similar to read_local_xlog_page, but limited to read from one particular
 * timeline. If the end of WAL is reached, it will wait for more if reading
 * from the current timeline, or give up if reading from a historic timeline.
 * In the latter case, it will also set private_data->end_of_wal = true.
 * 类似于 read_local_xlog_page，但限制为仅从一个特定时间线读取。
 * 如果达到了 WAL 的末尾，如果是从当前时间线读取，它将等待更多内容，
 * 或者如果是从历史时间线读取，则放弃。在后一种情况下，它还将设置
 * private_data->end_of_wal = true。
 *
 * Caller must set private_data->tli to the TLI of interest,
 * private_data->read_upto to the lowest LSN that is not known to be safe
 * to read on that timeline, and private_data->historic to true if and only
 * if the timeline is not the current timeline. This function will update
 * private_data->read_upto and private_data->historic if more WAL appears
 * on the current timeline or if the current timeline becomes historic.
 * 调用者必须将 private_data->tli 设置为感兴趣的 TLI，
 * 将 private_data->read_upto 设置为在该时间线上已知不安全以供读取的最低 LSN，
 * 并且当且仅当该时间线不是当前时间线时，将 private_data->historic 设置为 true。
 * 如果在当前时间线上出现了更多的 WAL，或者如果当前时间线变成了历史时间线，
 * 此函数将更新 private_data->read_upto 和 private_data->historic。
 */
static int
summarizer_read_local_xlog_page(XLogReaderState *state,
								XLogRecPtr targetPagePtr, int reqLen,
								XLogRecPtr targetRecPtr, char *cur_page)
{
	int			count;
	WALReadError errinfo;
	SummarizerReadLocalXLogPrivate *private_data;

	ProcessWalSummarizerInterrupts();

	private_data = (SummarizerReadLocalXLogPrivate *)
		state->private_data;

	while (1)
	{
		if (targetPagePtr + XLOG_BLCKSZ <= private_data->read_upto)
		{
			/*
			 * more than one block available; read only that block, have
			 * caller come back if they need more.
			 * 存在多于一个数据块可用；仅读取该块，如果调用者需要更多，让他们再回来。
			 */
			count = XLOG_BLCKSZ;
			break;
		}
		else if (targetPagePtr + reqLen > private_data->read_upto)
		{
			/* We don't seem to have enough data. */
			/* 我们似乎没有足够的数据。 */
			if (private_data->historic)
			{
				/*
				 * This is a historic timeline, so there will never be any
				 * more data than we have currently.
				 * 这是一个历史时间线，因此绝不会有比我们当前拥有的更多的数据。
				 */
				private_data->end_of_wal = true;
				return -1;
			}
			else
			{
				XLogRecPtr	latest_lsn;
				TimeLineID	latest_tli;

				/*
				 * This is - or at least was up until very recently - the
				 * current timeline, so more data might show up.  Delay here
				 * so we don't tight-loop.
				 * 这是——或者至少在最近极短时间之前是——当前的时间线，因此可能会有更多的数据显示出来。
				 * 在这里进行延迟，以免我们陷入紧密循环。
				 */
				ProcessWalSummarizerInterrupts();
				summarizer_wait_for_wal();

				/* Recheck end-of-WAL. */
				/* 重新检查 WAL 的末尾。 */
				latest_lsn = GetLatestLSN(&latest_tli);
				if (private_data->tli == latest_tli)
				{
					/* Still the current timeline, update max LSN. */
					/* 仍然是当前时间线，更新最大 LSN。 */
					Assert(latest_lsn >= private_data->read_upto);
					private_data->read_upto = latest_lsn;
				}
				else
				{
					List	   *tles = readTimeLineHistory(latest_tli);
					XLogRecPtr	switchpoint;

					/*
					 * The timeline we're scanning is no longer the latest
					 * one. Figure out when it ended.
					 * 我们正在扫描的时间线不再是最新的一条。找出它在何时结束。
					 */
					private_data->historic = true;
					switchpoint = tliSwitchPoint(private_data->tli, tles,
												 NULL);

					/*
					 * Allow reads up to exactly the switch point.
					 * 允许精确读取到切换点为止。
					 *
					 * It's possible that this will cause read_upto to move
					 * backwards, because we might have been promoted before
					 * reaching the end of the previous timeline. In that
					 * case, the next loop iteration will likely conclude that
					 * we've reached end of WAL.
					 * 这有可能会导致 read_upto 向后移动，因为我们可能在达到上一个时间线的末端
					 * 之前就已经被晋升（promoted）了。在这种情况下，下一次循环迭代很可能会得出
					 * 我们已经达到 WAL 末尾的结论。
					 */
					private_data->read_upto = switchpoint;

					/* Debugging output. */
					/* 调试输出。 */
					ereport(DEBUG1,
							errmsg_internal("timeline %u became historic, can read up to %X/%X",
											private_data->tli, LSN_FORMAT_ARGS(private_data->read_upto)));
				}

				/* Go around and try again. */
				/* 绕回来重新尝试。 */
			}
		}
		else
		{
			/* enough bytes available to satisfy the request */
			/* 存在足够的字节可用以满足请求 */
			count = private_data->read_upto - targetPagePtr;
			break;
		}
	}

	if (!WALRead(state, cur_page, targetPagePtr, count,
				 private_data->tli, &errinfo))
		WALReadRaiseError(&errinfo);

	/* Track that we read a page, for sleep time calculation. */
	/* 跟踪我们读取了一个页面，用以计算休眠时间。 */
	++pages_read_since_last_sleep;

	/* number of valid bytes in the buffer */
	/* 缓冲区中有效字节的数量 */
	return count;
}

/*
 * Sleep for long enough that we believe it's likely that more WAL will
 * be available afterwards.
 * 休眠足够长的时间，以至于我们相信之后很可能会有更多的 WAL 可用。
 */
static void
summarizer_wait_for_wal(void)
{
	if (pages_read_since_last_sleep == 0)
	{
		/*
		 * No pages were read since the last sleep, so double the sleep time,
		 * but not beyond the maximum allowable value.
		 * 自上次休眠以来未读取任何页面，因此将休眠时间翻倍，但不超过允许的最大值。
		 */
		sleep_quanta = Min(sleep_quanta * 2, MAX_SLEEP_QUANTA);
	}
	else if (pages_read_since_last_sleep > 1)
	{
		/*
		 * Multiple pages were read since the last sleep, so reduce the sleep
		 * time.
		 * 自上次休眠以来读取了多个页面，因此减少休眠时间。
		 *
		 * A large burst of activity should be able to quickly reduce the
		 * sleep time to the minimum, but we don't want a handful of extra WAL
		 * records to provoke a strong reaction. We choose to reduce the sleep
		 * time by 1 quantum for each page read beyond the first, which is a
		 * fairly arbitrary way of trying to be reactive without overreacting.
		 * 爆发性的大量活动应该能够快速地将休眠时间降至最低，但我们不希望少数几条额外的 
		 * WAL 记录就引发强烈的反应。我们选择对于在第一页之外每读取一页就将休眠时间减少
		 * 1 个量子，这是一种相当任意的尝试做到了既有反应而又不过度反应的方法。
		 */
		if (pages_read_since_last_sleep > sleep_quanta - 1)
			sleep_quanta = 1;
		else
			sleep_quanta -= pages_read_since_last_sleep;
	}

	/* Report pending statistics to the cumulative stats system. */
	/* 将待处理的统计信息报告给累积统计系统。 */
	pgstat_report_wal(false);

	/* OK, now sleep. */
	/* 好的，现在休眠。 */
	(void) WaitLatch(MyLatch,
					 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
					 sleep_quanta * MS_PER_SLEEP_QUANTUM,
					 WAIT_EVENT_WAL_SUMMARIZER_WAL);
	ResetLatch(MyLatch);

	/* Reset count of pages read. */
	/* 重置读取页数的计数。 */
	pages_read_since_last_sleep = 0;
}

/*
 * Remove WAL summaries whose mtimes are older than wal_summary_keep_time.
 * 删除其修改时间（mtime）早于 wal_summary_keep_time 的 WAL 汇总文件。
 */
static void
MaybeRemoveOldWalSummaries(void)
{
	XLogRecPtr	redo_pointer = GetRedoRecPtr();
	List	   *wslist;
	time_t		cutoff_time;

	/* If WAL summary removal is disabled, don't do anything. */
	/* 如果禁用了 WAL 汇总文件删除，则什么都不做。 */
	if (wal_summary_keep_time == 0)
		return;

	/*
	 * If the redo pointer has not advanced, don't do anything.
	 * 如果重做指针没有推进，则什么都不做。
	 *
	 * This has the effect that we only try to remove old WAL summary files
	 * once per checkpoint cycle.
	 * 这会起到如下作用：我们每个检查点周期只尝试一次删除旧的 WAL 汇总文件。
	 */
	if (redo_pointer == redo_pointer_at_last_summary_removal)
		return;
	redo_pointer_at_last_summary_removal = redo_pointer;

	/*
	 * Files should only be removed if the last modification time precedes the
	 * cutoff time we compute here.
	 * 仅当最后修改时间早于我们在此计算的截止时间时，才应该删除文件。
	 */
	cutoff_time = time(NULL) - wal_summary_keep_time * SECS_PER_MINUTE;

	/* Get all the summaries that currently exist. */
	/* 获取当前存在的所有汇总。 */
	wslist = GetWalSummaries(0, InvalidXLogRecPtr, InvalidXLogRecPtr);

	/* Loop until all summaries have been considered for removal. */
	/* 循环处理，直至所有汇总文件都已被考虑删除。 */
	while (wslist != NIL)
	{
		ListCell   *lc;
		XLogSegNo	oldest_segno;
		XLogRecPtr	oldest_lsn = InvalidXLogRecPtr;
		TimeLineID	selected_tli;

		ProcessWalSummarizerInterrupts();

		/*
		 * Pick a timeline for which some summary files still exist on disk,
		 * and find the oldest LSN that still exists on disk for that
		 * timeline.
		 * 选择磁盘上仍存在一些汇总文件的时间线，并找到该时间线上仍存在于磁盘上的最旧 LSN。
		 */
		selected_tli = ((WalSummaryFile *) linitial(wslist))->tli;
		oldest_segno = XLogGetOldestSegno(selected_tli);
		if (oldest_segno != 0)
			XLogSegNoOffsetToRecPtr(oldest_segno, 0, wal_segment_size,
									oldest_lsn);


		/* Consider each WAL file on the selected timeline in turn. */
		/* 依次考虑所选时间线上的每个 WAL 文件。 */
		foreach(lc, wslist)
		{
			WalSummaryFile *ws = lfirst(lc);

			ProcessWalSummarizerInterrupts();

			/* If it's not on this timeline, it's not time to consider it. */
			/* 如果它不在此时间线上，现在就不该考虑它。 */
			if (selected_tli != ws->tli)
				continue;

			/*
			 * If the WAL doesn't exist any more, we can remove it if the file
			 * modification time is old enough.
			 * 如果 WAL 不再存在，若文件修改时间足够久，我们可以将其删除。
			 */
			if (XLogRecPtrIsInvalid(oldest_lsn) || ws->end_lsn <= oldest_lsn)
				RemoveWalSummaryIfOlderThan(ws, cutoff_time);

			/*
			 * Whether we removed the file or not, we need not consider it
			 * again.
			 * 无论我们是否删除了该文件，我们都无需再次考虑它。
			 */
			wslist = foreach_delete_current(wslist, lc);
			pfree(ws);
		}
	}
}

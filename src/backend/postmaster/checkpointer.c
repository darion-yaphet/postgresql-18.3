/*-------------------------------------------------------------------------
 *
 * checkpointer.c
 *
 * The checkpointer is new as of Postgres 9.2.  It handles all checkpoints.
 * Checkpoints are automatically dispatched after a certain amount of time has
 * elapsed since the last one, and it can be signaled to perform requested
 * checkpoints as well.  (The GUC parameter that mandates a checkpoint every
 * so many WAL segments is implemented by having backends signal when they
 * fill WAL segments; the checkpointer itself doesn't watch for the
 * condition.)
 *
 * checkpointer 是 Postgres 9.2 引入的新功能。它处理所有检查点。
 * 检查点会在距离上一次检查点流逝一定时间后自动派发，
 * 并且它也能被发信号以执行请求的检查点。（要求每隔多少个
 * WAL 段进行一次检查点的 GUC 参数，其实现方式是让后端在
 * 填满 WAL 段时发送信号；checkpointer 本身并不监视这一状况。）
 *
 * The normal termination sequence is that checkpointer is instructed to
 * execute the shutdown checkpoint by SIGINT.  After that checkpointer waits
 * to be terminated via SIGUSR2, which instructs the checkpointer to exit(0).
 * All backends must be stopped before SIGINT or SIGUSR2 is issued!
 *
 * 正常的终止序列是，通过 SIGINT 指示 checkpointer 
 * 执行关闭检查点（shutdown checkpoint）。之后 checkpointer
 * 会等待通过 SIGUSR2 来被终止，这会指示 checkpointer 执行 exit(0)。
 * 在发出 SIGINT 或 SIGUSR2 之前，所有后端都必须已停止！
 *
 * Emergency termination is by SIGQUIT; like any backend, the checkpointer
 * will simply abort and exit on SIGQUIT.
 *
 * 紧急终止通过 SIGQUIT 进行；与任何后端一样，checkpointer
 * 在收到 SIGQUIT 时将简单地中止并退出。
 *
 * If the checkpointer exits unexpectedly, the postmaster treats that the same
 * as a backend crash: shared memory may be corrupted, so remaining backends
 * should be killed by SIGQUIT and then a recovery cycle started.  (Even if
 * shared memory isn't corrupted, we have lost information about which
 * files need to be fsync'd for the next checkpoint, and so a system
 * restart needs to be forced.)
 *
 * 如果 checkpointer 意外退出，postmaster 将其视同
 * 后端崩溃处理：共享内存可能已损坏，因此其余后端
 * 应该被 SIGQUIT 杀死，然后启动恢复周期。（即使
 * 共享内存未损坏，我们也已经丢失了关于下个检查点
 * 哪些文件需要被 fsync 的信息，因此需要强制进行系统
 * 重启。）
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/checkpointer.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/time.h>
#include <time.h>

#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xlogrecovery.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/auxprocess.h"
#include "postmaster/bgwriter.h"
#include "postmaster/interrupt.h"
#include "replication/syncrep.h"
#include "storage/aio_subsys.h"
#include "storage/bufmgr.h"
#include "storage/condition_variable.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/shmem.h"
#include "storage/smgr.h"
#include "storage/spin.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/resowner.h"


/*----------
 * Shared memory area for communication between checkpointer and backends
 *
 * The ckpt counters allow backends to watch for completion of a checkpoint
 * request they send.  Here's how it works:
 *	* At start of a checkpoint, checkpointer reads (and clears) the request
 *	  flags and increments ckpt_started, while holding ckpt_lck.
 *	* On completion of a checkpoint, checkpointer sets ckpt_done to
 *	  equal ckpt_started.
 *	* On failure of a checkpoint, checkpointer increments ckpt_failed
 *	  and sets ckpt_done to equal ckpt_started.
 *
 * The algorithm for backends is:
 *	1. Record current values of ckpt_failed and ckpt_started, and
 *	   set request flags, while holding ckpt_lck.
 *	2. Send signal to request checkpoint.
 *	3. Sleep until ckpt_started changes.  Now you know a checkpoint has
 *	   begun since you started this algorithm (although *not* that it was
 *	   specifically initiated by your signal), and that it is using your flags.
 *	4. Record new value of ckpt_started.
 *	5. Sleep until ckpt_done >= saved value of ckpt_started.  (Use modulo
 *	   arithmetic here in case counters wrap around.)  Now you know a
 *	   checkpoint has started and completed, but not whether it was
 *	   successful.
 *	6. If ckpt_failed is different from the originally saved value,
 *	   assume request failed; otherwise it was definitely successful.
 *
 * ckpt_flags holds the OR of the checkpoint request flags sent by all
 * requesting backends since the last checkpoint start.  The flags are
 * chosen so that OR'ing is the correct way to combine multiple requests.
 *
 * The requests array holds fsync requests sent by backends and not yet
 * absorbed by the checkpointer.
 *
 * Unlike the checkpoint fields, requests related fields are protected by
 * CheckpointerCommLock.
 *
 * checkpointer 和后端之间通信的共享内存区域
 *
 * ckpt 计数器允许后端监视它们发送的检查点请求的完成情况。
 * 其工作原理如下：
 *	* 在检查点开始时，checkpointer 在持有 ckpt_lck 的情况下，
 *	  读取（并清除）请求标志，并递增 ckpt_started。
 *	* 在检查点完成时，checkpointer 将 ckpt_done 设置为
 *	  等于 ckpt_started。
 *	* 在检查点失败时，checkpointer 递增 ckpt_failed
 *	  并将 ckpt_done 设置为等于 ckpt_started。
 *
 * 后端的算法是：
 *	1. 在持有 ckpt_lck 的情况下，记录 ckpt_failed 和
 *	   ckpt_started 的当前值，并设置请求标志。
 *	2. 发送信号以请求检查点。
 *	3. 休眠直到 ckpt_started 发生变化。现在你知道自你开始此算法以来
 *	   已经开始了一个检查点（尽管*不一定*是由你的信号专门启动的），
 *	   并且它正在使用你的标志。
 *	4. 记录 ckpt_started 的新值。
 *	5. 休眠直到 ckpt_done >= 保存的 ckpt_started 值。
 *	   （此处使用模运算，以防计数器回绕。）现在你知道
 *	   检查点已经开始并完成，但不知道它是否成功。
 *	6. 如果 ckpt_failed 与最初保存的值不同，
 *	   则假定请求失败；否则必定是成功的。
 *
 * ckpt_flags 保存自上一个检查点开始以来，所有请求后端发送的
 * 检查点请求标志的 OR。选择这些标志是为了使 OR 运算成为
 * 组合多个请求的正确方法。
 *
 * requests 数组保存后端发送且尚未被 checkpointer 吸收的
 * fsync 请求。
 *
 * 与检查点字段不同，请求相关的字段受 CheckpointerCommLock 保护。
 *----------
 */
typedef struct
{
	SyncRequestType type;		/* request type */
	/* 请求类型 */
	FileTag		ftag;			/* file identifier */
	/* 文件标识符 */
} CheckpointerRequest;

typedef struct
{
	pid_t		checkpointer_pid;	/* PID (0 if not started) */
	/* PID（如果未启动则为 0） */

	slock_t		ckpt_lck;		/* protects all the ckpt_* fields */
	/* 保护所有的 ckpt_* 字段 */

	int			ckpt_started;	/* advances when checkpoint starts */
	/* 检查点开始时推进 */
	int			ckpt_done;		/* advances when checkpoint done */
	/* 检查点完成时推进 */
	int			ckpt_failed;	/* advances when checkpoint fails */
	/* 检查点失败时推进 */

	int			ckpt_flags;		/* checkpoint flags, as defined in xlog.h */
	/* 检查点标志，定义在 xlog.h 中 */

	ConditionVariable start_cv; /* signaled when ckpt_started advances */
	/* 当 ckpt_started 推进时发送信号 */
	ConditionVariable done_cv;	/* signaled when ckpt_done advances */
	/* 当 ckpt_done 推进时发送信号 */

	int			num_requests;	/* current # of requests */
	/* 当前请求数 */
	int			max_requests;	/* allocated array size */
	/* 分配的数组大小 */
	CheckpointerRequest requests[FLEXIBLE_ARRAY_MEMBER];
} CheckpointerShmemStruct;

static CheckpointerShmemStruct *CheckpointerShmem;

/* interval for calling AbsorbSyncRequests in CheckpointWriteDelay */
/* 在 CheckpointWriteDelay 中调用 AbsorbSyncRequests 的间隔 */
#define WRITES_PER_ABSORB		1000

/* Max number of requests the checkpointer request queue can hold */
/* checkpointer 请求队列可容纳的最大请求数 */
#define MAX_CHECKPOINT_REQUESTS 10000000

/*
 * GUC parameters
 *
 * GUC 参数
 */
int			CheckPointTimeout = 300;
int			CheckPointWarning = 30;
double		CheckPointCompletionTarget = 0.9;

/*
 * Private state
 *
 * 私有状态
 */
static bool ckpt_active = false;
static volatile sig_atomic_t ShutdownXLOGPending = false;

/* these values are valid when ckpt_active is true: */
/* 这些值在 ckpt_active 为 true 时有效： */
static pg_time_t ckpt_start_time;
static XLogRecPtr ckpt_start_recptr;
static double ckpt_cached_elapsed;

static pg_time_t last_checkpoint_time;
static pg_time_t last_xlog_switch_time;

/* Prototypes for private functions */
/* 私有函数原型 */

static void ProcessCheckpointerInterrupts(void);
static void CheckArchiveTimeout(void);
static bool IsCheckpointOnSchedule(double progress);
static bool ImmediateCheckpointRequested(void);
static bool CompactCheckpointerRequestQueue(void);
static void UpdateSharedMemoryConfig(void);

/* Signal handlers */
/* 信号处理程序 */
static void ReqShutdownXLOG(SIGNAL_ARGS);


/*
 * Main entry point for checkpointer process
 *
 * This is invoked from AuxiliaryProcessMain, which has already created the
 * basic execution environment, but not enabled signals yet.
 *
 * checkpointer 进程的主入口点
 *
 * 该函数由 AuxiliaryProcessMain 调用，后者已经创建了
 * 基本执行环境，但尚未启用信号。
 */
void
CheckpointerMain(const void *startup_data, size_t startup_data_len)
{
	sigjmp_buf	local_sigjmp_buf;
	MemoryContext checkpointer_context;

	Assert(startup_data_len == 0);

	MyBackendType = B_CHECKPOINTER;
	AuxiliaryProcessMainCommon();

	CheckpointerShmem->checkpointer_pid = MyProcPid;

	/*
	 * Properly accept or ignore signals the postmaster might send us
	 *
	 * Note: we deliberately ignore SIGTERM, because during a standard Unix
	 * system shutdown cycle, init will SIGTERM all processes at once.  We
	 * want to wait for the backends to exit, whereupon the postmaster will
	 * tell us it's okay to shut down (via SIGUSR2).
	 *
	 * 正确接受或忽略 postmaster 可能发送给我们的信号
	 *
	 * 注意：由于在标准 Unix 系统关闭周期内，init 将立即
	 * 向所有进程发送 SIGTERM，因此这里故意忽略 SIGTERM。
	 * 我们想要等待所有后端退出，之后 postmaster 会告诉我们
	 * 可以关机了（通过 SIGUSR2）。
	 */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGINT, ReqShutdownXLOG);
	pqsignal(SIGTERM, SIG_IGN); /* ignore SIGTERM */
	/* 忽略 SIGTERM */
	/* SIGQUIT handler was already set up by InitPostmasterChild */
	pqsignal(SIGALRM, SIG_IGN);
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, SignalHandlerForShutdownRequest);

	/*
	 * Reset some signals that are accepted by postmaster but not here
	 *
	 * 重置一些被 postmaster 接受但不在这里接受的信号
	 */
	pqsignal(SIGCHLD, SIG_DFL);

	/*
	 * Initialize so that first time-driven event happens at the correct time.
	 *
	 * 初始化以使第一个时间驱动事件在正确的时间发生。
	 */
	last_checkpoint_time = last_xlog_switch_time = (pg_time_t) time(NULL);

	/*
	 * Write out stats after shutdown. This needs to be called by exactly one
	 * process during a normal shutdown, and since checkpointer is shut down
	 * very late...
	 *
	 * While e.g. walsenders are active after the shutdown checkpoint has been
	 * written (and thus could produce more stats), checkpointer stays around
	 * after the shutdown checkpoint has been written. postmaster will only
	 * signal checkpointer to exit after all processes that could emit stats
	 * have been shut down.
	 *
	 * 在关闭后写出统计信息。在正常关闭期间，这个需要被确切的一个进程调用，
	 * 而因为 checkpointer 关得很晚...
	 *
	 * 虽然（例如）walsenders 在写完关闭检查点后仍活跃（所以可能生成更多
	 * 统计信息），但 checkpointer 在写完关闭检查点后依然驻留等待。只有所有可能
	 * 生成统计信息的进程都已关闭之后，postmaster 才会向 checkpointer 发信号
	 * 使得退出。
	 */
	before_shmem_exit(pgstat_before_server_shutdown, 0);

	/*
	 * Create a memory context that we will do all our work in.  We do this so
	 * that we can reset the context during error recovery and thereby avoid
	 * possible memory leaks.  Formerly this code just ran in
	 * TopMemoryContext, but resetting that would be a really bad idea.
	 *
	 * 创建所有工作都将执行在其中的内存上下文。这样做是为了通过
	 * 在错误恢复期间重置上下文，避免可能的内存泄露。以前这段代码
	 * 只是在 TopMemoryContext 运行，但是重置它将是个极差的主意。
	 */
	checkpointer_context = AllocSetContextCreate(TopMemoryContext,
												 "Checkpointer",
												 ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(checkpointer_context);

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
	 * 如果遇到异常情况，处理在这里恢复。
	 *
	 * 你可能想要了解为什么这里没编码为包含 PG_TRY 构造的无限循环。
	 * 其原因是：这里处于异常堆栈的最底层，所以使用 PG_TRY，在
	 * CATCH 部分压根不会有有效的异常处理器。通过始终激活最外层
	 * 的 setjmp，我们起码还有极小的机会在异常恢复发生错误时，将
	 * 系统恢复。（假如这样导致无穷循环，elog.c 的内置状态集溢出
	 * 时就会使其立即停止。）
	 *
	 * 注：之所以用 sigsetjmp(..., 1)，是因为在随同 BlockSig 一起跳
	 * 至这里时可以重建当时的信号掩码。另外除 SIGQUIT 外，所有将要
	 * 被拦截的信号直至完成错误恢复后才能恢复。这就好像其规定了
	 * 冗余调用 HOLD_INTERRUPTS() 为不必要的规则，不过这没用因为
	 * InterruptPending 可能被提早创建。
	 */
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/* Since not using PG_TRY, must reset error stack by hand */
		/* 因为没用 PG_TRY，必须要人工把错误集清空 */
		error_context_stack = NULL;

		/* Prevent interrupts while cleaning up */
		/* 当清除时禁止打断 */
		HOLD_INTERRUPTS();

		/* Report the error to the server log */
		/* 呈报系统日志文件中的异常 */
		EmitErrorReport();

		/*
		 * These operations are really just a minimal subset of
		 * AbortTransaction().  We don't have very many resources to worry
		 * about in checkpointer, but we do have LWLocks, buffers, and temp
		 * files.
		 *
		 * 这些操作仅仅是极小版 AbortTransaction() 。
		 * 在 checkpointer 里不需关注太多资料的可用性，
		 * 但是需要关注 LWLocks、 buffers 和临时文件等资源的安全。
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

		/* Warn any waiting backends that the checkpoint failed. */
		/* 提出给等待中所有的后端的报警，指出 checkpoint 返回不成功。 */
		if (ckpt_active)
		{
			SpinLockAcquire(&CheckpointerShmem->ckpt_lck);
			CheckpointerShmem->ckpt_failed++;
			CheckpointerShmem->ckpt_done = CheckpointerShmem->ckpt_started;
			SpinLockRelease(&CheckpointerShmem->ckpt_lck);

			ConditionVariableBroadcast(&CheckpointerShmem->done_cv);

			ckpt_active = false;
		}

		/*
		 * Now return to normal top-level context and clear ErrorContext for
		 * next time.
		 *
		 * 此时还原到顶部默认层，且移除本 ErrorContext 内错误资料准备下次使用。
		 */
		MemoryContextSwitchTo(checkpointer_context);
		FlushErrorState();

		/* Flush any leaked data in the top-level context */
		/* 把顶层遗漏部分抹去 */
		MemoryContextReset(checkpointer_context);

		/* Now we can allow interrupts again */
		/* 能够再度让所有任务返回可干预权限区段之内了 */
		RESUME_INTERRUPTS();

		/*
		 * Sleep at least 1 second after any error.  A write error is likely
		 * to be repeated, and we don't want to be filling the error logs as
		 * fast as we can.
		 *
		 * 当存在一些错误报告下，保证具有 1 秒休眠周期。一个存储报错有可能
		 * 会再次存在且持续输出，而没有理由让各种出错汇报马上冲顶出错目录。
		 */
		pg_usleep(1000000L);
	}

	/* We can now handle ereport(ERROR) */
	/* 我们可以解决 ereport(ERROR) */
	PG_exception_stack = &local_sigjmp_buf;

	/*
	 * Unblock signals (they were blocked when the postmaster forked us)
	 *
	 * 解除非阻塞信号（这些信号在 postmaster fork 我们时被阻塞了）
	 */
	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

	/*
	 * Ensure all shared memory values are set correctly for the config. Doing
	 * this here ensures no race conditions from other concurrent updaters.
	 *
	 * 确保本结构里用于共同存放信息的相关设置都被设定完好，这样
	 * 能保证避开任何和并行化同时的更变时存在的竞值等现象。
	 */
	UpdateSharedMemoryConfig();

	/*
	 * Advertise our proc number that backends can use to wake us up while
	 * we're sleeping.
	 *
	 * 公布我们的进程编号，以便后端在我们睡眠时可以用它来唤醒我们。
	 */
	ProcGlobal->checkpointerProc = MyProcNumber;

	/*
	 * Loop until we've been asked to write the shutdown checkpoint or
	 * terminate.
	 *
	 * 循环直到我们被要求转储关闭检查点或者被中断。
	 */
	for (;;)
	{
		bool		do_checkpoint = false;
		int			flags = 0;
		pg_time_t	now;
		int			elapsed_secs;
		int			cur_timeout;
		bool		chkpt_or_rstpt_requested = false;
		bool		chkpt_or_rstpt_timed = false;

		/* Clear any already-pending wakeups */
		ResetLatch(MyLatch);

		/*
		 * Process any requests or signals received recently.
		 *
		 * 实施运作最近接收过的各项指令任务还有种种不同的触发类型和标记。
		 */
		AbsorbSyncRequests();

		ProcessCheckpointerInterrupts();
		if (ShutdownXLOGPending || ShutdownRequestPending)
			break;

		/*
		 * Detect a pending checkpoint request by checking whether the flags
		 * word in shared memory is nonzero.  We shouldn't need to acquire the
		 * ckpt_lck for this.
		 *
		 * 通过检查共享内存中的标志字是否非零来检测是否有挂起的
		 * 检查点请求。我们应该不需要为此获取 ckpt_lck。
		 */
		if (((volatile CheckpointerShmemStruct *) CheckpointerShmem)->ckpt_flags)
		{
			do_checkpoint = true;
			chkpt_or_rstpt_requested = true;
		}

		/*
		 * Force a checkpoint if too much time has elapsed since the last one.
		 * Note that we count a timed checkpoint in stats only when this
		 * occurs without an external request, but we set the CAUSE_TIME flag
		 * bit even if there is also an external request.
		 *
		 * 如果自上一次检查点以来经过了太多时间，则强制执行检查点。
		 * 请注意，只有当发生此情况且没有外部请求时，我们才会在统计
		 * 中计入一个定时检查点，但即使有外部请求，我们也会设置 
		 * CAUSE_TIME 标志位。
		 */
		now = (pg_time_t) time(NULL);
		elapsed_secs = now - last_checkpoint_time;
		if (elapsed_secs >= CheckPointTimeout)
		{
			if (!do_checkpoint)
				chkpt_or_rstpt_timed = true;
			do_checkpoint = true;
			flags |= CHECKPOINT_CAUSE_TIME;
		}

		/*
		 * Do a checkpoint if requested.
		 *
		 * 如果被请求，执行一个检查点操作。
		 */
		if (do_checkpoint)
		{
			bool		ckpt_performed = false;
			bool		do_restartpoint;

			/* Check if we should perform a checkpoint or a restartpoint. */
			/* 检查我们是要执行 checkpoint 还是 restartpoint。 */
			do_restartpoint = RecoveryInProgress();

			/*
			 * Atomically fetch the request flags to figure out what kind of a
			 * checkpoint we should perform, and increase the started-counter
			 * to acknowledge that we've started a new checkpoint.
			 *
			 * 原子获取所请求的触发标识来明确需要实施的一个怎样的存盘点操作
			 * ，并进行对起始位记录仪加以进位累加从而宣示确立新一个存盘写入已处于进行中。
			 */
			SpinLockAcquire(&CheckpointerShmem->ckpt_lck);
			flags |= CheckpointerShmem->ckpt_flags;
			CheckpointerShmem->ckpt_flags = 0;
			CheckpointerShmem->ckpt_started++;
			SpinLockRelease(&CheckpointerShmem->ckpt_lck);

			ConditionVariableBroadcast(&CheckpointerShmem->start_cv);

			/*
			 * The end-of-recovery checkpoint is a real checkpoint that's
			 * performed while we're still in recovery.
			 *
			 * 恢复结束的检查点（end-of-recovery checkpoint）是一个真正的
			 * 检查点，它是在我们仍处于恢复阶段时执行的。
			 */
			if (flags & CHECKPOINT_END_OF_RECOVERY)
				do_restartpoint = false;

			if (chkpt_or_rstpt_timed)
			{
				chkpt_or_rstpt_timed = false;
				if (do_restartpoint)
					PendingCheckpointerStats.restartpoints_timed++;
				else
					PendingCheckpointerStats.num_timed++;
			}

			if (chkpt_or_rstpt_requested)
			{
				chkpt_or_rstpt_requested = false;
				if (do_restartpoint)
					PendingCheckpointerStats.restartpoints_requested++;
				else
					PendingCheckpointerStats.num_requested++;
			}

			/*
			 * We will warn if (a) too soon since last checkpoint (whatever
			 * caused it) and (b) somebody set the CHECKPOINT_CAUSE_XLOG flag
			 * since the last checkpoint start.  Note in particular that this
			 * implementation will not generate warnings caused by
			 * CheckPointTimeout < CheckPointWarning.
			 *
			 * 如果满足以下条件，我们将发出警告：(a) 距离上一个检查点的时间
			 * 太短（无论是什么原因引起的）；(b) 自上一个检查点开始以来，
			 * 有人设置了 CHECKPOINT_CAUSE_XLOG 标志。特别要注意的是，
			 * 这种实现方式不会产生因为 CheckPointTimeout < CheckPointWarning
			 * 而被引起的警告讯息。
			 */
			if (!do_restartpoint &&
				(flags & CHECKPOINT_CAUSE_XLOG) &&
				elapsed_secs < CheckPointWarning)
				ereport(LOG,
						(errmsg_plural("checkpoints are occurring too frequently (%d second apart)",
									   "checkpoints are occurring too frequently (%d seconds apart)",
									   elapsed_secs,
									   elapsed_secs),
						 errhint("Consider increasing the configuration parameter \"%s\".", "max_wal_size")));

			/*
			 * Initialize checkpointer-private variables used during
			 * checkpoint.
			 *
			 * 初始化检查点执行期间使用的 checkpointer 私有变量。
			 */
			ckpt_active = true;
			if (do_restartpoint)
				ckpt_start_recptr = GetXLogReplayRecPtr(NULL);
			else
				ckpt_start_recptr = GetInsertRecPtr();
			ckpt_start_time = now;
			ckpt_cached_elapsed = 0;

			/*
			 * Do the checkpoint.
			 *
			 * 执行检查点过程。
			 */
			if (!do_restartpoint)
				ckpt_performed = CreateCheckPoint(flags);
			else
				ckpt_performed = CreateRestartPoint(flags);

			/*
			 * After any checkpoint, free all smgr objects.  Otherwise we
			 * would never do so for dropped relations, as the checkpointer
			 * does not process shared invalidation messages or call
			 * AtEOXact_SMgr().
			 *
			 * 在任何 checkpoint 结束后都施放各类 smgr 对象。如果我们
			 * 没有处理过对于不再可用对象的清理释放机制，对于在运行阶段不
			 * 处理各种对分享型信息废除以及不对外引发或不唤作执行的
			 * AtEOXact_SMgr 的 checkpointer 就会在之后出现相关堆积的无主废对象。
			 */
			smgrdestroyall();

			/*
			 * Indicate checkpoint completion to any waiting backends.
			 * 
			 * 对任何等待的后台通知检查点完成。
			 */
			SpinLockAcquire(&CheckpointerShmem->ckpt_lck);
			CheckpointerShmem->ckpt_done = CheckpointerShmem->ckpt_started;
			SpinLockRelease(&CheckpointerShmem->ckpt_lck);

			ConditionVariableBroadcast(&CheckpointerShmem->done_cv);

			if (!do_restartpoint)
			{
				/*
				 * Note we record the checkpoint start time not end time as
				 * last_checkpoint_time.  This is so that time-driven
				 * checkpoints happen at a predictable spacing.
				 *
				 * 请注意，我们将检查点开始时间而不是结束时间记录为
				 * last_checkpoint_time。这样是为了让基于时间的检查点
				 * 能在可预测的时间间隔内发生。
				 */
				last_checkpoint_time = now;

				if (ckpt_performed)
					PendingCheckpointerStats.num_performed++;
			}
			else
			{
				if (ckpt_performed)
				{
					/*
					 * The same as for checkpoint. Please see the
					 * corresponding comment.
					 *
					 * 和检查点一样。请参阅对应的注释。
					 */
					last_checkpoint_time = now;

					PendingCheckpointerStats.restartpoints_performed++;
				}
				else
				{
					/*
					 * We were not able to perform the restartpoint
					 * (checkpoints throw an ERROR in case of error).  Most
					 * likely because we have not received any new checkpoint
					 * WAL records since the last restartpoint. Try again in
					 * 15 s.
					 *
					 * 我们没能成功实现关于建立起重启记录位置的行动（如果出错，执行 checkpoint 将发送一场异常报错）
					 * 。基本上最有概率造成此事的最大起原就是我们直到现在的这之前都没有提取收到不论一条有关在早前
					 * 那回记录确立过后新的包含新检查触发要求记录 WAL 。先待后于随近的一刻钟过后再予重新实施处理试验吧。
					 */
					last_checkpoint_time = now - CheckPointTimeout + 15;
				}
			}

			ckpt_active = false;

			/*
			 * We may have received an interrupt during the checkpoint and the
			 * latch might have been reset (e.g. in CheckpointWriteDelay).
			 *
			 * 我们确有可能在这个检查点的时期，遇到拦截的触发要求甚至同时把有关用在触发判断当做防阻门的判断信号
			 * Latch 都因此得到强制重配还原的情况存在。（比方说像发生了在要求延后触发刷入（CheckpointWriteDelay）中时）。
			 */
			ProcessCheckpointerInterrupts();
			if (ShutdownXLOGPending || ShutdownRequestPending)
				break;
		}

		/* Check for archive_timeout and switch xlog files if necessary. */
		/* 如有相关 archive_timeout 要求的情况下做定期判断切换记录文件。 */
		CheckArchiveTimeout();

		/* Report pending statistics to the cumulative stats system */
		/* 对全集式的报表系统中输入最新暂缓在积存区域的数据状态改变和进度回报。 */
		pgstat_report_checkpointer();
		pgstat_report_wal(true);

		/*
		 * If any checkpoint flags have been set, redo the loop to handle the
		 * checkpoint without sleeping.
		 *
		 * 如果已经设定好了有关被指令过的检查确认触发判断标，直接不经过沉睡暂停环节再做一次回圈执行操作。
		 */
		if (((volatile CheckpointerShmemStruct *) CheckpointerShmem)->ckpt_flags)
			continue;

		/*
		 * Sleep until we are signaled or it's time for another checkpoint or
		 * xlog file switch.
		 *
		 * 睡眠并直至能够受到通知去进行新动作；或是它到期时去发起下一处记录存储或是该执行记录切换的时候。
		 */
		now = (pg_time_t) time(NULL);
		elapsed_secs = now - last_checkpoint_time;
		if (elapsed_secs >= CheckPointTimeout)
			continue;			/* no sleep for us ... */
		cur_timeout = CheckPointTimeout - elapsed_secs;
		if (XLogArchiveTimeout > 0 && !RecoveryInProgress())
		{
			elapsed_secs = now - last_xlog_switch_time;
			if (elapsed_secs >= XLogArchiveTimeout)
				continue;		/* no sleep for us ... */
			cur_timeout = Min(cur_timeout, XLogArchiveTimeout - elapsed_secs);
		}

		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 cur_timeout * 1000L /* convert to ms */ ,
						 WAIT_EVENT_CHECKPOINTER_MAIN);
	}

	/*
	 * From here on, elog(ERROR) should end with exit(1), not send control
	 * back to the sigsetjmp block above.
	 *
	 * 在此之后，elog(ERROR) 都必须最终以 exit(1) 来结束退出，而不是跳返控制
	 * 到上半头处的 sigsetjmp 指向的代码区块之内。
	 */
	ExitOnAnyError = true;

	if (ShutdownXLOGPending)
	{
		/*
		 * Close down the database.
		 *
		 * Since ShutdownXLOG() creates restartpoint or checkpoint, and
		 * updates the statistics, increment the checkpoint request and flush
		 * out pending statistic.
		 *
		 * 把数据库关闭中止运作。
		 *
		 * 在因为 ShutdownXLOG() 运作而去建立了 restartpoint 或 checkpoint 等同时
		 * 做了变更汇总数值的操作，我们需要追加更新累加上记录点所耗和清除并写入被遗漏暂存部分的反馈信息数值。
		 */
		PendingCheckpointerStats.num_requested++;
		ShutdownXLOG(0, 0);
		pgstat_report_checkpointer();
		pgstat_report_wal(true);

		/*
		 * Tell postmaster that we're done.
		 *
		 * 这里我们给回告给 postmaster 去表态这端已把善后的事情妥善完成。
		 */
		SendPostmasterSignal(PMSIGNAL_XLOG_IS_SHUTDOWN);
		ShutdownXLOGPending = false;
	}

	/*
	 * Wait until we're asked to shut down. By separating the writing of the
	 * shutdown checkpoint from checkpointer exiting, checkpointer can perform
	 * some should-be-as-late-as-possible work like writing out stats.
	 *
	 * 长停保持直到我们得到有指示须中止该执行体才去运作退出。凭借将其在执行
	 * 记录中止时的储存存盘的撰文分离移除了跟直接促使该检测工作端被完全关闭并断绝这二者；checkpointer 此方
	 * 能够更进一步做出一些如最后进行统计报告的填写这类理直气壮须压到最极限延迟的时候所施办的活动。
	 */
	for (;;)
	{
		/* Clear any already-pending wakeups */
		/* 把先前已被指发却尚并未排上处理之全数苏醒启动给擦消掉 */
		ResetLatch(MyLatch);

		ProcessCheckpointerInterrupts();

		if (ShutdownRequestPending)
			break;

		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_EXIT_ON_PM_DEATH,
						 0,
						 WAIT_EVENT_CHECKPOINTER_SHUTDOWN);
	}

	/* Normal exit from the checkpointer is here */
	/* 检查服务进程通过这一关时为代表其以和平且平常方式进行了终了。 */
	proc_exit(0);				/* done */
}

/*
 * Process any new interrupts.
 *
 * 实施一切新生发生的各种新出现的内部强插信号及相关命令动作。
 */
static void
ProcessCheckpointerInterrupts(void)
{
	if (ProcSignalBarrierPending)
		ProcessProcSignalBarrier();

	if (ConfigReloadPending)
	{
		ConfigReloadPending = false;
		ProcessConfigFile(PGC_SIGHUP);

		/*
		 * Checkpointer is the last process to shut down, so we ask it to hold
		 * the keys for a range of other tasks required most of which have
		 * nothing to do with checkpointing at all.
		 *
		 * For various reasons, some config values can change dynamically so
		 * the primary copy of them is held in shared memory to make sure all
		 * backends see the same value.  We make Checkpointer responsible for
		 * updating the shared memory copy if the parameter setting changes
		 * because of SIGHUP.
		 *
		 * Checkpointer 是最后关闭的进程，故让其负责一系列其他任务，
		 * 其中多数与 checkpoint 无关。
		 *
		 * 因多种原因，部分配置可动态变更，其主副本保存在共享内存中，
		 * 保证所有后端看到一致值。参数因 SIGHUP 改变时由 Checkpointer
		 * 负责更新共享内存副本。
		 */
		UpdateSharedMemoryConfig();
	}

		/* Perform logging of memory contexts of this process */
	/* 对本进程的内存上下文执行日志记录 */
	if (LogMemoryContextPending)
		ProcessLogMemoryContextInterrupt();
}

/*
 * CheckArchiveTimeout -- check for archive_timeout and switch xlog files
 *
 * This will switch to a new WAL file and force an archive file write if
 * meaningful activity is recorded in the current WAL file. This includes most
 * writes, including just a single checkpoint record, but excludes WAL records
 * that were inserted with the XLOG_MARK_UNIMPORTANT flag being set (like
 * snapshots of running transactions).  Such records, depending on
 * configuration, occur on regular intervals and don't contain important
 * information.  This avoids generating archives with a few unimportant
 * records.
 *
 * 检查 archive_timeout 并在需要时切换 xlog 文件
 *
 * 若当前 WAL 文件中记录了有意义的活动，将切换到新的 WAL 文件并强制
 * 归档写入。这包括绝大多数写入（包括单个 checkpoint 记录），但排除
 * 以 XLOG_MARK_UNIMPORTANT 标志插入的 WAL 记录（如运行事务的快照）。
 * 这类记录按配置定期产生，且不含重要信息，从而避免生成只含少量
 * 不重要记录的归档。
 */
static void
CheckArchiveTimeout(void)
{
	pg_time_t	now;
	pg_time_t	last_time;
	XLogRecPtr	last_switch_lsn;

	if (XLogArchiveTimeout <= 0 || RecoveryInProgress())
		return;

	now = (pg_time_t) time(NULL);

	/* First we do a quick check using possibly-stale local state. */
	/* 先用可能过期的本地状态做快速检查 */
	if ((int) (now - last_xlog_switch_time) < XLogArchiveTimeout)
		return;

	/*
	 * Update local state ... note that last_xlog_switch_time is the last time
	 * a switch was performed *or requested*.
	 *
	 * 更新本地状态 ... 注意 last_xlog_switch_time 是最近一次
	 * 执行*或请求*切换的时间。
	 */
	last_time = GetLastSegSwitchData(&last_switch_lsn);

	last_xlog_switch_time = Max(last_xlog_switch_time, last_time);

	/* Now we can do the real checks */
	/* 现在可以进行实际检查 */
	if ((int) (now - last_xlog_switch_time) >= XLogArchiveTimeout)
	{
		/*
		 * Switch segment only when "important" WAL has been logged since the
		 * last segment switch (last_switch_lsn points to end of segment
		 * switch occurred in).
		 *
		 * 仅当自上次段切换以来记录了“重要”的 WAL 时才切换段
		 * （last_switch_lsn 指向发生切换的段尾）。
		 */
		if (GetLastImportantRecPtr() > last_switch_lsn)
		{
			XLogRecPtr	switchpoint;

			/* mark switch as unimportant, avoids triggering checkpoints */
			/* 将切换标记为不重要，避免触发 checkpoint */
			switchpoint = RequestXLogSwitch(true);

			/*
			 * If the returned pointer points exactly to a segment boundary,
			 * assume nothing happened.
			 *
			 * 若返回的指针恰好指向段边界，认为未发生任何切换。
			 */
			if (XLogSegmentOffset(switchpoint, wal_segment_size) != 0)
				elog(DEBUG1, "write-ahead log switch forced (\"archive_timeout\"=%d)",
					 XLogArchiveTimeout);
		}

		/*
		 * Update state in any case, so we don't retry constantly when the
		 * system is idle.
		 *
		 * 无论如何更新状态，避免系统空闲时不断重试。
		 */
		last_xlog_switch_time = now;
	}
}

/*
 * Returns true if an immediate checkpoint request is pending.  (Note that
 * this does not check the *current* checkpoint's IMMEDIATE flag, but whether
 * there is one pending behind it.)
 *
 * 若存在挂起的立即检查点请求则返回 true。（注意：此函数不检查*当前*
 * 检查点的 IMMEDIATE 标志，而是检查其后是否还有挂起的立即请求。）
 */
static bool
ImmediateCheckpointRequested(void)
{
	volatile CheckpointerShmemStruct *cps = CheckpointerShmem;

	/*
	 * We don't need to acquire the ckpt_lck in this case because we're only
	 * looking at a single flag bit.
	 */
	if (cps->ckpt_flags & CHECKPOINT_IMMEDIATE)
		return true;
	return false;
}

/*
 * CheckpointWriteDelay -- control rate of checkpoint
 *
 * This function is called after each page write performed by BufferSync().
 * It is responsible for throttling BufferSync()'s write rate to hit
 * checkpoint_completion_target.
 *
 * The checkpoint request flags should be passed in; currently the only one
 * examined is CHECKPOINT_IMMEDIATE, which disables delays between writes.
 *
 * 'progress' is an estimate of how much of the work has been done, as a
 * fraction between 0.0 meaning none, and 1.0 meaning all done.
 *
 * CheckpointWriteDelay -- 控制 checkpoint 的写入速率
 *
 * 该函数在 BufferSync() 完成每次页写入后调用，负责对 BufferSync() 的
 * 写入速率进行节流，以达成 checkpoint_completion_target。
 *
 * 应传入 checkpoint 请求标志；当前仅检查 CHECKPOINT_IMMEDIATE，该标志
 * 会禁用写入之间的延迟。
 *
 * 'progress' 为已完成工作量的估计，0.0 表示未开始，1.0 表示全部完成。
 */
void
CheckpointWriteDelay(int flags, double progress)
{
	static int	absorb_counter = WRITES_PER_ABSORB;

	/* Do nothing if checkpoint is being executed by non-checkpointer process */
	/* 若 checkpoint 由非 checkpointer 进程执行，则不做任何事 */
	if (!AmCheckpointerProcess())
		return;

	/*
	 * Perform the usual duties and take a nap, unless we're behind schedule,
	 * in which case we just try to catch up as quickly as possible.
	 *
	 * 执行常规任务并短暂休眠，除非进度落后，此时则尽快追赶。
	 */
	if (!(flags & CHECKPOINT_IMMEDIATE) &&
		!ShutdownXLOGPending &&
		!ShutdownRequestPending &&
		!ImmediateCheckpointRequested() &&
		IsCheckpointOnSchedule(progress))
	{
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
			/* update shmem copies of config variables */
			UpdateSharedMemoryConfig();
		}

		AbsorbSyncRequests();
		absorb_counter = WRITES_PER_ABSORB;

		CheckArchiveTimeout();

		/* Report interim statistics to the cumulative stats system */
		pgstat_report_checkpointer();

		/*
		 * This sleep used to be connected to bgwriter_delay, typically 200ms.
		 * That resulted in more frequent wakeups if not much work to do.
		 * Checkpointer and bgwriter are no longer related so take the Big
		 * Sleep.
		 *
		 * 此睡眠曾与 bgwriter_delay 关联，通常为 200ms。在待办工作不多时
		 * 会造成更频繁的唤醒。checkpointer 与 bgwriter 现已解耦，故采用
		 * 较长的睡眠时间。
		 */
		WaitLatch(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH | WL_TIMEOUT,
				  100,
				  WAIT_EVENT_CHECKPOINT_WRITE_DELAY);
		ResetLatch(MyLatch);
	}
	else if (--absorb_counter <= 0)
	{
		/*
		 * Absorb pending fsync requests after each WRITES_PER_ABSORB write
		 * operations even when we don't sleep, to prevent overflow of the
		 * fsync request queue.
		 *
		 * 即使未休眠，每 WRITES_PER_ABSORB 次写入后也吸收挂起的 fsync 请求，
		 * 防止 fsync 请求队列溢出。
		 */
		AbsorbSyncRequests();
		absorb_counter = WRITES_PER_ABSORB;
	}

	/* Check for barrier events. */
	/* 检查 barrier 事件 */
	if (ProcSignalBarrierPending)
		ProcessProcSignalBarrier();
}

/*
 * IsCheckpointOnSchedule -- are we on schedule to finish this checkpoint
 *		 (or restartpoint) in time?
 *
 * Compares the current progress against the time/segments elapsed since last
 * checkpoint, and returns true if the progress we've made this far is greater
 * than the elapsed time/segments.
 *
 * IsCheckpointOnSchedule -- 能否按计划及时完成本 checkpoint（或 restartpoint）？
 *
 * 将当前进度与自上次 checkpoint 以来经过的时间/段数比较，若已完成的
 * 进度大于已过的时间/段数则返回 true。
 */
static bool
IsCheckpointOnSchedule(double progress)
{
	XLogRecPtr	recptr;
	struct timeval now;
	double		elapsed_xlogs,
				elapsed_time;

	Assert(ckpt_active);

	/* Scale progress according to checkpoint_completion_target. */
	/* 按 checkpoint_completion_target 缩放进度 */
	progress *= CheckPointCompletionTarget;

	/*
	 * Check against the cached value first. Only do the more expensive
	 * calculations once we reach the target previously calculated. Since
	 * neither time or WAL insert pointer moves backwards, a freshly
	 * calculated value can only be greater than or equal to the cached value.
	 *
	 * 先与缓存值比较。只有达到先前计算的目标时才做更耗时的计算。
	 * 因时间与 WAL 插入指针都不会回退，新计算值只可能大于等于缓存值。
	 */
	if (progress < ckpt_cached_elapsed)
		return false;

	/*
	 * Check progress against WAL segments written and CheckPointSegments.
	 *
	 * We compare the current WAL insert location against the location
	 * computed before calling CreateCheckPoint. The code in XLogInsert that
	 * actually triggers a checkpoint when CheckPointSegments is exceeded
	 * compares against RedoRecPtr, so this is not completely accurate.
	 * However, it's good enough for our purposes, we're only calculating an
	 * estimate anyway.
	 *
	 * During recovery, we compare last replayed WAL record's location with
	 * the location computed before calling CreateRestartPoint. That maintains
	 * the same pacing as we have during checkpoints in normal operation, but
	 * we might exceed max_wal_size by a fair amount. That's because there can
	 * be a large gap between a checkpoint's redo-pointer and the checkpoint
	 * record itself, and we only start the restartpoint after we've seen the
	 * checkpoint record. (The gap is typically up to CheckPointSegments *
	 * checkpoint_completion_target where checkpoint_completion_target is the
	 * value that was in effect when the WAL was generated).
	 *
	 * 将进度与已写入的 WAL 段数和 CheckPointSegments 比较。
	 *
	 * 我们将当前 WAL 插入位置与调用 CreateCheckPoint 前计算的位置比较。
	 * XLogInsert 中在超过 CheckPointSegments 时实际触发 checkpoint 的代码
	 * 是与 RedoRecPtr 比较，故此处并非完全准确，但足以满足需求，只是估算。
	 *
	 * 恢复期间，我们将最后重放的 WAL 记录位置与调用 CreateRestartPoint 前
	 * 计算的位置比较，以保持与正常运行 checkpoint 时相同的步调。但可能
	 * 明显超过 max_wal_size，因 checkpoint 的 redo 指针与 checkpoint 记录
	 * 本身之间可能有较大间隙，且只有在看到 checkpoint 记录后才启动
	 * restartpoint。（间隙通常可达 CheckPointSegments * checkpoint_completion_target，
	 * 其中 checkpoint_completion_target 为生成 WAL 时生效的值。）
	 */
	if (RecoveryInProgress())
		recptr = GetXLogReplayRecPtr(NULL);
	else
		recptr = GetInsertRecPtr();
	elapsed_xlogs = (((double) (recptr - ckpt_start_recptr)) /
					 wal_segment_size) / CheckPointSegments;

	if (progress < elapsed_xlogs)
	{
		ckpt_cached_elapsed = elapsed_xlogs;
		return false;
	}

	/*
	 * Check progress against time elapsed and checkpoint_timeout.
	 *
	 * 将进度与已用时间和 checkpoint_timeout 比较。
	 */
	gettimeofday(&now, NULL);
	elapsed_time = ((double) ((pg_time_t) now.tv_sec - ckpt_start_time) +
					now.tv_usec / 1000000.0) / CheckPointTimeout;

	if (progress < elapsed_time)
	{
		ckpt_cached_elapsed = elapsed_time;
		return false;
	}

	/* It looks like we're on schedule. */
	/* 看起来进度在计划内 */
	return true;
}


/* --------------------------------
 *		signal handler routines
 * --------------------------------
 */

/* SIGINT: set flag to trigger writing of shutdown checkpoint */
static void
ReqShutdownXLOG(SIGNAL_ARGS)
{
	ShutdownXLOGPending = true;
	SetLatch(MyLatch);
}


/* --------------------------------
 *		communication with backends
 * --------------------------------
 */

/*
 * CheckpointerShmemSize
 *		Compute space needed for checkpointer-related shared memory
 *
 * 计算 checkpointer 相关共享内存所需的空间
 */
Size
CheckpointerShmemSize(void)
{
	Size		size;

	/*
	 * The size of the requests[] array is arbitrarily set equal to NBuffers.
	 * But there is a cap of MAX_CHECKPOINT_REQUESTS to prevent accumulating
	 * too many checkpoint requests in the ring buffer.
	 *
	 * requests[] 数组大小约定为 NBuffers。但有 MAX_CHECKPOINT_REQUESTS
	 * 上限，防止环形缓冲中堆积过多 checkpoint 请求。
	 */
	size = offsetof(CheckpointerShmemStruct, requests);
	size = add_size(size, mul_size(Min(NBuffers,
									   MAX_CHECKPOINT_REQUESTS),
								   sizeof(CheckpointerRequest)));

	return size;
}

/*
 * CheckpointerShmemInit
 *		Allocate and initialize checkpointer-related shared memory
 *
 * 分配并初始化 checkpointer 相关共享内存
 */
void
CheckpointerShmemInit(void)
{
	Size		size = CheckpointerShmemSize();
	bool		found;

	CheckpointerShmem = (CheckpointerShmemStruct *)
		ShmemInitStruct("Checkpointer Data",
						size,
						&found);

	if (!found)
	{
		/*
		 * First time through, so initialize.  Note that we zero the whole
		 * requests array; this is so that CompactCheckpointerRequestQueue can
		 * assume that any pad bytes in the request structs are zeroes.
		 *
		 * 首次执行，进行初始化。注意我们将整个 requests 数组置零；
		 * 这样 CompactCheckpointerRequestQueue 可以假定请求结构中的
		 * 填充字节全为 0。
		 */
		MemSet(CheckpointerShmem, 0, size);
		SpinLockInit(&CheckpointerShmem->ckpt_lck);
		CheckpointerShmem->max_requests = Min(NBuffers, MAX_CHECKPOINT_REQUESTS);
		ConditionVariableInit(&CheckpointerShmem->start_cv);
		ConditionVariableInit(&CheckpointerShmem->done_cv);
	}
}

/*
 * RequestCheckpoint
 *		Called in backend processes to request a checkpoint
 *
 * flags is a bitwise OR of the following:
 *	CHECKPOINT_IS_SHUTDOWN: checkpoint is for database shutdown.
 *	CHECKPOINT_END_OF_RECOVERY: checkpoint is for end of WAL recovery.
 *	CHECKPOINT_IMMEDIATE: finish the checkpoint ASAP,
 *		ignoring checkpoint_completion_target parameter.
 *	CHECKPOINT_FORCE: force a checkpoint even if no XLOG activity has occurred
 *		since the last one (implied by CHECKPOINT_IS_SHUTDOWN or
 *		CHECKPOINT_END_OF_RECOVERY).
 *	CHECKPOINT_WAIT: wait for completion before returning (otherwise,
 *		just signal checkpointer to do it, and return).
 *	CHECKPOINT_CAUSE_XLOG: checkpoint is requested due to xlog filling.
 *		(This affects logging, and in particular enables CheckPointWarning.)
 *
 * 在后端进程中调用以请求 checkpoint。
 *
 * flags 为下列标志的按位或：
 *	CHECKPOINT_IS_SHUTDOWN: checkpoint 用于数据库关闭
 *	CHECKPOINT_END_OF_RECOVERY: checkpoint 用于 WAL 恢复结束
 *	CHECKPOINT_IMMEDIATE: 尽快完成 checkpoint，忽略 checkpoint_completion_target
 *	CHECKPOINT_FORCE: 即使自上次以来无 XLOG 活动也强制 checkpoint
 *		（CHECKPOINT_IS_SHUTDOWN 或 CHECKPOINT_END_OF_RECOVERY 隐含此项）
 *	CHECKPOINT_WAIT: 等待完成后再返回（否则仅发信号给 checkpointer 并返回）
 *	CHECKPOINT_CAUSE_XLOG: 因 xlog 填满而请求 checkpoint（影响日志，尤其是 CheckPointWarning）
 */
void
RequestCheckpoint(int flags)
{
	int			ntries;
	int			old_failed,
				old_started;

	/*
	 * If in a standalone backend, just do it ourselves.
	 */
	if (!IsPostmasterEnvironment)
	{
		/*
		 * There's no point in doing slow checkpoints in a standalone backend,
		 * because there's no other backends the checkpoint could disrupt.
		 */
		CreateCheckPoint(flags | CHECKPOINT_IMMEDIATE);

		/* Free all smgr objects, as CheckpointerMain() normally would. */
		smgrdestroyall();

		return;
	}

	/*
	 * Atomically set the request flags, and take a snapshot of the counters.
	 * When we see ckpt_started > old_started, we know the flags we set here
	 * have been seen by checkpointer.
	 *
	 * Note that we OR the flags with any existing flags, to avoid overriding
	 * a "stronger" request by another backend.  The flag senses must be
	 * chosen to make this work!
	 */
	SpinLockAcquire(&CheckpointerShmem->ckpt_lck);

	old_failed = CheckpointerShmem->ckpt_failed;
	old_started = CheckpointerShmem->ckpt_started;
	CheckpointerShmem->ckpt_flags |= (flags | CHECKPOINT_REQUESTED);

	SpinLockRelease(&CheckpointerShmem->ckpt_lck);

	/*
	 * Set checkpointer's latch to request checkpoint.  It's possible that the
	 * checkpointer hasn't started yet, so we will retry a few times if
	 * needed.  (Actually, more than a few times, since on slow or overloaded
	 * buildfarm machines, it's been observed that the checkpointer can take
	 * several seconds to start.)  However, if not told to wait for the
	 * checkpoint to occur, we consider failure to set the latch to be
	 * nonfatal and merely LOG it.  The checkpointer should see the request
	 * when it does start, with or without the SetLatch().
	 */
#define MAX_SIGNAL_TRIES 600	/* max wait 60.0 sec */
	for (ntries = 0;; ntries++)
	{
		volatile PROC_HDR *procglobal = ProcGlobal;
		ProcNumber	checkpointerProc = procglobal->checkpointerProc;

		if (checkpointerProc == INVALID_PROC_NUMBER)
		{
			if (ntries >= MAX_SIGNAL_TRIES || !(flags & CHECKPOINT_WAIT))
			{
				elog((flags & CHECKPOINT_WAIT) ? ERROR : LOG,
					 "could not notify checkpoint: checkpointer is not running");
				break;
			}
		}
		else
		{
			SetLatch(&GetPGProcByNumber(checkpointerProc)->procLatch);
			/* notified successfully */
			break;
		}

		CHECK_FOR_INTERRUPTS();
		pg_usleep(100000L);		/* wait 0.1 sec, then retry */
	}

	/*
	 * If requested, wait for completion.  We detect completion according to
	 * the algorithm given above.
	 */
	if (flags & CHECKPOINT_WAIT)
	{
		int			new_started,
					new_failed;

		/* Wait for a new checkpoint to start. */
		ConditionVariablePrepareToSleep(&CheckpointerShmem->start_cv);
		for (;;)
		{
			SpinLockAcquire(&CheckpointerShmem->ckpt_lck);
			new_started = CheckpointerShmem->ckpt_started;
			SpinLockRelease(&CheckpointerShmem->ckpt_lck);

			if (new_started != old_started)
				break;

			ConditionVariableSleep(&CheckpointerShmem->start_cv,
								   WAIT_EVENT_CHECKPOINT_START);
		}
		ConditionVariableCancelSleep();

		/*
		 * We are waiting for ckpt_done >= new_started, in a modulo sense.
		 */
		ConditionVariablePrepareToSleep(&CheckpointerShmem->done_cv);
		for (;;)
		{
			int			new_done;

			SpinLockAcquire(&CheckpointerShmem->ckpt_lck);
			new_done = CheckpointerShmem->ckpt_done;
			new_failed = CheckpointerShmem->ckpt_failed;
			SpinLockRelease(&CheckpointerShmem->ckpt_lck);

			if (new_done - new_started >= 0)
				break;

			ConditionVariableSleep(&CheckpointerShmem->done_cv,
								   WAIT_EVENT_CHECKPOINT_DONE);
		}
		ConditionVariableCancelSleep();

		if (new_failed != old_failed)
			ereport(ERROR,
					(errmsg("checkpoint request failed"),
					 errhint("Consult recent messages in the server log for details.")));
	}
}

/*
 * ForwardSyncRequest
 *		Forward a file-fsync request from a backend to the checkpointer
 *
 * Whenever a backend is compelled to write directly to a relation
 * (which should be seldom, if the background writer is getting its job done),
 * the backend calls this routine to pass over knowledge that the relation
 * is dirty and must be fsync'd before next checkpoint.  We also use this
 * opportunity to count such writes for statistical purposes.
 *
 * To avoid holding the lock for longer than necessary, we normally write
 * to the requests[] queue without checking for duplicates.  The checkpointer
 * will have to eliminate dups internally anyway.  However, if we discover
 * that the queue is full, we make a pass over the entire queue to compact
 * it.  This is somewhat expensive, but the alternative is for the backend
 * to perform its own fsync, which is far more expensive in practice.  It
 * is theoretically possible a backend fsync might still be necessary, if
 * the queue is full and contains no duplicate entries.  In that case, we
 * let the backend know by returning false.
 *
 * ForwardSyncRequest -- 将后端的文件 fsync 请求转发给 checkpointer
 *
 * 当后端不得不直接写 relation 时（在 bgwriter 正常工作时应很少发生），
 * 后端调用本例程告知该 relation 已被修改，必须在下次 checkpoint 前 fsync。
 * 我们同时利用此机会统计此类写入。
 *
 * 为避免持锁过久，通常向 requests[] 队列写入时不检查重复，checkpointer
 * 内部会去重。但若队列已满，会对整个队列做一次压缩。这有一定开销，但
 * 让后端自行 fsync 在实践中代价更大。理论上若队列已满且无重复项，后端
 * 仍需自行 fsync；此时通过返回 false 告知后端。
 */
bool
ForwardSyncRequest(const FileTag *ftag, SyncRequestType type)
{
	CheckpointerRequest *request;
	bool		too_full;

	if (!IsUnderPostmaster)
		return false;			/* probably shouldn't even get here */

	if (AmCheckpointerProcess())
		elog(ERROR, "ForwardSyncRequest must not be called in checkpointer");

	LWLockAcquire(CheckpointerCommLock, LW_EXCLUSIVE);

	/*
	 * If the checkpointer isn't running or the request queue is full, the
	 * backend will have to perform its own fsync request.  But before forcing
	 * that to happen, we can try to compact the request queue.
	 *
	 * 若 checkpointer 未运行或请求队列已满，后端将不得不自行 fsync。
	 * 但在强制如此之前，可先尝试压缩请求队列。
	 */
	if (CheckpointerShmem->checkpointer_pid == 0 ||
		(CheckpointerShmem->num_requests >= CheckpointerShmem->max_requests &&
		 !CompactCheckpointerRequestQueue()))
	{
		LWLockRelease(CheckpointerCommLock);
		return false;
	}

	/* OK, insert request */
	/* 插入请求 */
	request = &CheckpointerShmem->requests[CheckpointerShmem->num_requests++];
	request->ftag = *ftag;
	request->type = type;

	/* If queue is more than half full, nudge the checkpointer to empty it */
	/* 若队列超过半满，提醒 checkpointer 清空 */
	too_full = (CheckpointerShmem->num_requests >=
				CheckpointerShmem->max_requests / 2);

	LWLockRelease(CheckpointerCommLock);

	/* ... but not till after we release the lock */
	/* ... 但要在释放锁之后 */
	if (too_full)
	{
		volatile PROC_HDR *procglobal = ProcGlobal;
		ProcNumber	checkpointerProc = procglobal->checkpointerProc;

		if (checkpointerProc != INVALID_PROC_NUMBER)
			SetLatch(&GetPGProcByNumber(checkpointerProc)->procLatch);
	}

	return true;
}

/*
 * CompactCheckpointerRequestQueue
 *		Remove duplicates from the request queue to avoid backend fsyncs.
 *		Returns "true" if any entries were removed.
 *
 * Although a full fsync request queue is not common, it can lead to severe
 * performance problems when it does happen.  So far, this situation has
 * only been observed to occur when the system is under heavy write load,
 * and especially during the "sync" phase of a checkpoint.  Without this
 * logic, each backend begins doing an fsync for every block written, which
 * gets very expensive and can slow down the whole system.
 *
 * Trying to do this every time the queue is full could lose if there
 * aren't any removable entries.  But that should be vanishingly rare in
 * practice: there's one queue entry per shared buffer.
 *
 * CompactCheckpointerRequestQueue -- 从请求队列移除重复项，避免后端自行 fsync
 *
 * 若有条目被移除则返回 "true"。fsync 请求队列满虽不常见，一旦发生会导致严重
 * 性能问题。目前仅在高写负载下观测到，尤其是 checkpoint 的“sync”阶段。
 * 无此逻辑时，每个后端会对每次写入的块执行 fsync，开销很大，拖慢整个系统。
 *
 * 队列每次满都做压缩，若无可删条目会浪费。但实践中极少：每共享缓冲对应一队列项。
 */
static bool
CompactCheckpointerRequestQueue(void)
{
	struct CheckpointerSlotMapping
	{
		CheckpointerRequest request;
		int			slot;
	};

	int			n,
				preserve_count;
	int			num_skipped = 0;
	HASHCTL		ctl;
	HTAB	   *htab;
	bool	   *skip_slot;

	/* must hold CheckpointerCommLock in exclusive mode */
	/* 必须以排他模式持有 CheckpointerCommLock */
	Assert(LWLockHeldByMe(CheckpointerCommLock));

	/* Avoid memory allocations in a critical section. */
	/* 避免在临界区内分配内存 */
	if (CritSectionCount > 0)
		return false;

	/* Initialize skip_slot array */
	skip_slot = palloc0(sizeof(bool) * CheckpointerShmem->num_requests);

	/* Initialize temporary hash table */
	ctl.keysize = sizeof(CheckpointerRequest);
	ctl.entrysize = sizeof(struct CheckpointerSlotMapping);
	ctl.hcxt = CurrentMemoryContext;

	htab = hash_create("CompactCheckpointerRequestQueue",
					   CheckpointerShmem->num_requests,
					   &ctl,
					   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	/*
	 * The basic idea here is that a request can be skipped if it's followed
	 * by a later, identical request.  It might seem more sensible to work
	 * backwards from the end of the queue and check whether a request is
	 * *preceded* by an earlier, identical request, in the hopes of doing less
	 * copying.  But that might change the semantics, if there's an
	 * intervening SYNC_FORGET_REQUEST or SYNC_FILTER_REQUEST, so we do it
	 * this way.  It would be possible to be even smarter if we made the code
	 * below understand the specific semantics of such requests (it could blow
	 * away preceding entries that would end up being canceled anyhow), but
	 * it's not clear that the extra complexity would buy us anything.
	 *
	 * 基本思路：若某请求后有更晚出现的同一请求，则前者可跳过。
	 * 从队列尾反向检查、看请求是否被更早出现的同一请求*前置*，看似
	 * 更省复制，但若有 SYNC_FORGET_REQUEST 或 SYNC_FILTER_REQUEST 穿插
	 * 会改变语义，故采用现方式。若让下方代码理解这些请求的语义（可删除
	 * 终将被取消的前置项），可以更聪明，但额外复杂度未必划算。
	 */
	for (n = 0; n < CheckpointerShmem->num_requests; n++)
	{
		CheckpointerRequest *request;
		struct CheckpointerSlotMapping *slotmap;
		bool		found;

		/*
		 * We use the request struct directly as a hashtable key.  This
		 * assumes that any padding bytes in the structs are consistently the
		 * same, which should be okay because we zeroed them in
		 * CheckpointerShmemInit.  Note also that RelFileLocator had better
		 * contain no pad bytes.
		 *
		 * 直接以请求结构体作为哈希表键。假定结构体中填充字节一致，因在
		 * CheckpointerShmemInit 中已置零应无问题。另 RelFileLocator 不应
		 * 含填充字节。
		 */
		request = &CheckpointerShmem->requests[n];
		slotmap = hash_search(htab, request, HASH_ENTER, &found);
		if (found)
		{
			/* Duplicate, so mark the previous occurrence as skippable */
			/* 重复，将前一次出现标记为可跳过 */
			skip_slot[slotmap->slot] = true;
			num_skipped++;
		}
		/* Remember slot containing latest occurrence of this request value */
		/* 记住包含该请求值最新出现的槽位 */
		slotmap->slot = n;
	}

	/* Done with the hash table. */
	/* 哈希表用毕 */
	hash_destroy(htab);

	/* If no duplicates, we're out of luck. */
	/* 若无重复项，压缩无收益 */
	if (!num_skipped)
	{
		pfree(skip_slot);
		return false;
	}

	/* We found some duplicates; remove them. */
	/* 发现重复项，移除之 */
	preserve_count = 0;
	for (n = 0; n < CheckpointerShmem->num_requests; n++)
	{
		if (skip_slot[n])
			continue;
		CheckpointerShmem->requests[preserve_count++] = CheckpointerShmem->requests[n];
	}
	ereport(DEBUG1,
			(errmsg_internal("compacted fsync request queue from %d entries to %d entries",
							 CheckpointerShmem->num_requests, preserve_count)));
	CheckpointerShmem->num_requests = preserve_count;

	/* Cleanup. */
	pfree(skip_slot);
	return true;
}

/*
 * AbsorbSyncRequests
 *		Retrieve queued sync requests and pass them to sync mechanism.
 *
 * This is exported because it must be called during CreateCheckPoint;
 * we have to be sure we have accepted all pending requests just before
 * we start fsync'ing.  Since CreateCheckPoint sometimes runs in
 * non-checkpointer processes, do nothing if not checkpointer.
 *
 * 取出排队的 sync 请求并交给 sync 机制处理。
 *
 * 之所以导出：必须在 CreateCheckPoint 期间调用；必须在开始 fsync 前
 * 确保已接受所有挂起请求。CreateCheckPoint 有时在非 checkpointer 进程中
 * 运行，若非 checkpointer 则直接返回。
 */
void
AbsorbSyncRequests(void)
{
	CheckpointerRequest *requests = NULL;
	CheckpointerRequest *request;
	int			n;

	if (!AmCheckpointerProcess())
		return;

	LWLockAcquire(CheckpointerCommLock, LW_EXCLUSIVE);

	/*
	 * We try to avoid holding the lock for a long time by copying the request
	 * array, and processing the requests after releasing the lock.
	 *
	 * Once we have cleared the requests from shared memory, we have to PANIC
	 * if we then fail to absorb them (eg, because our hashtable runs out of
	 * memory).  This is because the system cannot run safely if we are unable
	 * to fsync what we have been told to fsync.  Fortunately, the hashtable
	 * is so small that the problem is quite unlikely to arise in practice.
	 *
	 * 通过复制请求数组并在释放锁后处理，尽量避免长时间持锁。
	 *
	 * 一旦从共享内存清空请求，若后续吸收失败（例如哈希表内存不足）必须
	 * PANIC，否则系统无法 fsync 被要求 fsync 的内容，不能安全运行。
	 * 所幸哈希表很小，实践中几乎不会出现该问题。
	 */
	n = CheckpointerShmem->num_requests;
	if (n > 0)
	{
		requests = (CheckpointerRequest *) palloc(n * sizeof(CheckpointerRequest));
		memcpy(requests, CheckpointerShmem->requests, n * sizeof(CheckpointerRequest));
	}

	START_CRIT_SECTION();

	CheckpointerShmem->num_requests = 0;

	LWLockRelease(CheckpointerCommLock);

	for (request = requests; n > 0; request++, n--)
		RememberSyncRequest(&request->ftag, request->type);

	END_CRIT_SECTION();

	if (requests)
		pfree(requests);
}

/*
 * Update any shared memory configurations based on config parameters
 *
 * 根据配置参数更新共享内存中的相关配置
 */
static void
UpdateSharedMemoryConfig(void)
{
	/* update global shmem state for sync rep */
	/* 更新同步复制的全局 shmem 状态 */
	SyncRepUpdateSyncStandbysDefined();

	/*
	 * If full_page_writes has been changed by SIGHUP, we update it in shared
	 * memory and write an XLOG_FPW_CHANGE record.
	 *
	 * 若 full_page_writes 被 SIGHUP 修改，在共享内存中更新并写入
	 * XLOG_FPW_CHANGE 记录。
	 */
	UpdateFullPageWrites();

	elog(DEBUG2, "checkpointer updated shared memory configuration values");
}

/*
 * FirstCallSinceLastCheckpoint allows a process to take an action once
 * per checkpoint cycle by asynchronously checking for checkpoint completion.
 *
 * 允许进程在每次 checkpoint 周期内执行一次动作，通过异步检查
 * checkpoint 是否完成。
 */
bool
FirstCallSinceLastCheckpoint(void)
{
	static int	ckpt_done = 0;
	int			new_done;
	bool		FirstCall = false;

	SpinLockAcquire(&CheckpointerShmem->ckpt_lck);
	new_done = CheckpointerShmem->ckpt_done;
	SpinLockRelease(&CheckpointerShmem->ckpt_lck);

	if (new_done != ckpt_done)
		FirstCall = true;

	ckpt_done = new_done;

	return FirstCall;
}

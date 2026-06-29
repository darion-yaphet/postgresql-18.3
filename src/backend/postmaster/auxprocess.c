/*-------------------------------------------------------------------------
 * auxprocess.c
 *	  functions related to auxiliary processes.
 *	  与辅助进程（auxiliary processes）相关的函数。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/auxprocess.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <signal.h>

#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/auxprocess.h"
#include "storage/condition_variable.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"


static void ShutdownAuxiliaryProcess(int code, Datum arg);


/*
 *	 AuxiliaryProcessMainCommon
 *
 *	 Common initialization code for auxiliary processes, such as the bgwriter,
 *	 walwriter, walreceiver, and the startup process.
 *	 辅助进程的通用初始化代码，例如 bgwriter（后台写入器）、
 *	 walwriter（WAL写入器）、walreceiver（WAL接收器）和 startup（启动）进程。
 *
 *	 Function purpose: Initialize execution environment for auxiliary processes.
 *	 函数作用：初始化辅助进程的运行环境。
 *
 *	 Core workflow:
 *	 核心流程：
 *	 1. Delete postmaster context to release its memory.
 *	    删除 postmaster 上下文以释放其占用的内存。
 *	 2. Initialize process title display.
 *	    初始化进程标题（ps）显示。
 *	 3. Initialize auxiliary process shared memory structures (PGPROC).
 *	    初始化辅助进程的共享内存结构（PGPROC）。
 *	 4. Initialize backend stats, resource owner, and register shutdown callback.
 *	    初始化后台统计信息、资源所有者，并注册关机退出回调。
 */
void
AuxiliaryProcessMainCommon(void)
{
	Assert(IsUnderPostmaster);

	/* Release postmaster's working memory context */
	/* 释放 postmaster 的工作内存上下文 */
	if (PostmasterContext)
	{
		MemoryContextDelete(PostmasterContext);
		PostmasterContext = NULL;
	}

	init_ps_display(NULL);

	Assert(GetProcessingMode() == InitProcessing);

	IgnoreSystemIndexes = true;

	/*
	 * As an auxiliary process, we aren't going to do the full InitPostgres
	 * pushups, but there are a couple of things that need to get lit up even
	 * in an auxiliary process.
	 */
	/*
	 * 作为一个辅助进程，我们不会进行完整的 InitPostgres 初始化步骤，
	 * 但即使在辅助进程中，也有几件事情需要启动。
	 */

	/*
	 * Create a PGPROC so we can use LWLocks and access shared memory.
	 */
	/*
	 * 创建一个 PGPROC 结构，以便我们可以使用轻量级锁（LWLocks）并访问共享内存。
	 */
	InitAuxiliaryProcess();

	BaseInit();

	ProcSignalInit(NULL, 0);

	/*
	 * Auxiliary processes don't run transactions, but they may need a
	 * resource owner anyway to manage buffer pins acquired outside
	 * transactions (and, perhaps, other things in future).
	 */
	/*
	 * 辅助进程不运行事务，但它们仍然可能需要一个资源所有者（ResourceOwner）
	 * 来管理在事务外部获取的缓冲区钉（buffer pins）（未来可能还有其他用途）。
	 */
	CreateAuxProcessResourceOwner();


	/* Initialize backend status information */
	/* 初始化后台状态信息 */
	pgstat_beinit();
	pgstat_bestart_initial();
	pgstat_bestart_final();

	/* register a before-shutdown callback for LWLock cleanup */
	/* 注册关机前的回调函数以进行轻量级锁（LWLock）的清理 */
	before_shmem_exit(ShutdownAuxiliaryProcess, 0);

	SetProcessingMode(NormalProcessing);
}

/*
 * Begin shutdown of an auxiliary process.  This is approximately the equivalent
 * of ShutdownPostgres() in postinit.c.  We can't run transactions in an
 * auxiliary process, so most of the work of AbortTransaction() is not needed,
 * but we do need to make sure we've released any LWLocks we are holding.
 * (This is only critical during an error exit.)
 */
/*
 * 开始关闭辅助进程。这大约相当于 postinit.c 中的 ShutdownPostgres()。
 * 我们不能在辅助进程中运行事务，所以不需要 AbortTransaction() 的大部分工作，
 * 但我们确实需要确保已经释放了所持有的任何轻量级锁（LWLocks）。
 * （这只在发生错误退出时是关键的。）
 *
 * Function purpose: Release locks and cleanup stats when shutting down an auxiliary process.
 * 函数作用：在辅助进程关闭时释放锁并清理统计状态。
 */
static void
ShutdownAuxiliaryProcess(int code, Datum arg)
{
	LWLockReleaseAll();
	ConditionVariableCancelSleep();
	pgstat_report_wait_end();
}


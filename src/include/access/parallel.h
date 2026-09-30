/*-------------------------------------------------------------------------
 *
 * parallel.h
 *	  Infrastructure for launching parallel workers
 *
 * 说明并行执行上下文、工作进程及共享状态的组织方式。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/parallel.h
 *
 * 说明并行执行上下文、工作进程及共享状态的组织方式。
 *
 *-------------------------------------------------------------------------
 */

#ifndef PARALLEL_H
#define PARALLEL_H

#include "access/xlogdefs.h"
#include "lib/ilist.h"
#include "postmaster/bgworker.h"
#include "storage/shm_mq.h"
#include "storage/shm_toc.h"

typedef void (*parallel_worker_main_type) (dsm_segment *seg, shm_toc *toc);

typedef struct ParallelWorkerInfo
{
	BackgroundWorkerHandle *bgwhandle;
	shm_mq_handle *error_mqh;
} ParallelWorkerInfo;

typedef struct ParallelContext
{
	dlist_node	node;
	SubTransactionId subid;
	int			nworkers;		/* Maximum number of workers to launch */

	/* int nworkers；		 启动的最大工作人员数量 */
	int			nworkers_to_launch; /* Actual number of workers to launch */

	/* int nworkers_to_launch；  实际启动的工人数量 */
	int			nworkers_launched;
	char	   *library_name;
	char	   *function_name;
	ErrorContextCallback *error_context_stack;
	shm_toc_estimator estimator;
	dsm_segment *seg;
	void	   *private_memory;
	shm_toc    *toc;
	ParallelWorkerInfo *worker;
	int			nknown_attached_workers;
	bool	   *known_attached_workers;
} ParallelContext;

typedef struct ParallelWorkerContext
{
	dsm_segment *seg;
	shm_toc    *toc;
} ParallelWorkerContext;

extern PGDLLIMPORT volatile sig_atomic_t ParallelMessagePending;
extern PGDLLIMPORT int ParallelWorkerNumber;
extern PGDLLIMPORT bool InitializingParallelWorker;

#define		IsParallelWorker()		(ParallelWorkerNumber >= 0)

/*
 * Function: CreateParallelContext.
 * Purpose: Creates, starts, or registers the resource represented by create parallel context.
 * Core flow: It prepares the required context, installs it in the owning subsystem, and returns or exposes the resulting resource.
 *
 * 函数：CreateParallelContext。
 * 作用：创建、启动或注册 create parallel context 所表示的资源。
 * 核心流程：它准备所需上下文，将其安装到所属子系统，并返回或公开生成的资源。
 */
extern ParallelContext *CreateParallelContext(const char *library_name,
											  const char *function_name, int nworkers);
/*
 * Function: InitializeParallelDSM.
 * Purpose: Initializes the state required for initialize parallel dsm.
 * Core flow: It establishes the relevant shared, local, or on-disk state so later operations can use the subsystem safely.
 *
 * 函数：InitializeParallelDSM。
 * 作用：初始化 initialize parallel dsm 所需的状态。
 * 核心流程：它建立相关的共享、本地或磁盘状态，使后续操作能够安全使用该子系统。
 */
extern void InitializeParallelDSM(ParallelContext *pcxt);
/*
 * Function: ReinitializeParallelDSM.
 * Purpose: Updates the state represented by reinitialize parallel dsm.
 * Core flow: It validates the supplied inputs, changes the relevant transaction or WAL state, and keeps the associated metadata consistent.
 *
 * 函数：ReinitializeParallelDSM。
 * 作用：更新 reinitialize parallel dsm 所表示的状态。
 * 核心流程：它校验输入，变更相关事务或 WAL 状态，并保持关联元数据一致。
 */
extern void ReinitializeParallelDSM(ParallelContext *pcxt);
/*
 * Function: ReinitializeParallelWorkers.
 * Purpose: Updates the state represented by reinitialize parallel workers.
 * Core flow: It validates the supplied inputs, changes the relevant transaction or WAL state, and keeps the associated metadata consistent.
 *
 * 函数：ReinitializeParallelWorkers。
 * 作用：更新 reinitialize parallel workers 所表示的状态。
 * 核心流程：它校验输入，变更相关事务或 WAL 状态，并保持关联元数据一致。
 */
extern void ReinitializeParallelWorkers(ParallelContext *pcxt, int nworkers_to_launch);
/*
 * Function: LaunchParallelWorkers.
 * Purpose: Creates, starts, or registers the resource represented by launch parallel workers.
 * Core flow: It prepares the required context, installs it in the owning subsystem, and returns or exposes the resulting resource.
 *
 * 函数：LaunchParallelWorkers。
 * 作用：创建、启动或注册 launch parallel workers 所表示的资源。
 * 核心流程：它准备所需上下文，将其安装到所属子系统，并返回或公开生成的资源。
 */
extern void LaunchParallelWorkers(ParallelContext *pcxt);
/*
 * Function: WaitForParallelWorkersToAttach.
 * Purpose: Performs the operation represented by wait for parallel workers to attach.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：WaitForParallelWorkersToAttach。
 * 作用：执行 wait for parallel workers to attach 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
extern void WaitForParallelWorkersToAttach(ParallelContext *pcxt);
/*
 * Function: WaitForParallelWorkersToFinish.
 * Purpose: Performs the operation represented by wait for parallel workers to finish.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：WaitForParallelWorkersToFinish。
 * 作用：执行 wait for parallel workers to finish 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
extern void WaitForParallelWorkersToFinish(ParallelContext *pcxt);
/*
 * Function: DestroyParallelContext.
 * Purpose: Completes or releases the work represented by destroy parallel context.
 * Core flow: It applies the required completion or cleanup actions and leaves the related transaction or WAL state ready for subsequent work.
 *
 * 函数：DestroyParallelContext。
 * 作用：完成或释放 destroy parallel context 所表示的工作。
 * 核心流程：它执行所需的完成或清理操作，并使相关事务或 WAL 状态可用于后续工作。
 */
extern void DestroyParallelContext(ParallelContext *pcxt);
/*
 * Function: ParallelContextActive.
 * Purpose: Performs the operation represented by parallel context active.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：ParallelContextActive。
 * 作用：执行 parallel context active 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
extern bool ParallelContextActive(void);

/*
 * Function: HandleParallelMessageInterrupt.
 * Purpose: Performs the WAL-related operation represented by handle parallel message interrupt.
 * Core flow: It interprets the supplied record or context, performs the requested processing, and reports or records the resulting state.
 *
 * 函数：HandleParallelMessageInterrupt。
 * 作用：执行 handle parallel message interrupt 所表示的 WAL 相关操作。
 * 核心流程：它解释给定记录或上下文，完成请求的处理，并报告或记录生成的状态。
 */
extern void HandleParallelMessageInterrupt(void);
/*
 * Function: ProcessParallelMessages.
 * Purpose: Performs the WAL-related operation represented by process parallel messages.
 * Core flow: It interprets the supplied record or context, performs the requested processing, and reports or records the resulting state.
 *
 * 函数：ProcessParallelMessages。
 * 作用：执行 process parallel messages 所表示的 WAL 相关操作。
 * 核心流程：它解释给定记录或上下文，完成请求的处理，并报告或记录生成的状态。
 */
extern void ProcessParallelMessages(void);
/*
 * Function: AtEOXact_Parallel.
 * Purpose: Performs the operation represented by at eoxact parallel.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：AtEOXact_Parallel。
 * 作用：执行 at eoxact parallel 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
extern void AtEOXact_Parallel(bool isCommit);
/*
 * Function: AtEOSubXact_Parallel.
 * Purpose: Performs the operation represented by at eosub xact parallel.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：AtEOSubXact_Parallel。
 * 作用：执行 at eosub xact parallel 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
extern void AtEOSubXact_Parallel(bool isCommit, SubTransactionId mySubId);
/*
 * Function: ParallelWorkerReportLastRecEnd.
 * Purpose: Performs the operation represented by parallel worker report last rec end.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：ParallelWorkerReportLastRecEnd。
 * 作用：执行 parallel worker report last rec end 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
extern void ParallelWorkerReportLastRecEnd(XLogRecPtr last_xlog_end);

/*
 * Function: ParallelWorkerMain.
 * Purpose: Performs the operation represented by parallel worker main.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：ParallelWorkerMain。
 * 作用：执行 parallel worker main 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
extern void ParallelWorkerMain(Datum main_arg);

#endif							/* PARALLEL_H */

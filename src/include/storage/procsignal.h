/*-------------------------------------------------------------------------
 *
 * procsignal.h
 *	  Routines for interprocess signaling
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/procsignal.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROCSIGNAL_H
#define PROCSIGNAL_H

#include "storage/procnumber.h"


/*
 * Reasons for signaling a Postgres child process (a backend or an auxiliary
 * process, like checkpointer).  We can cope with concurrent signals for different
 * reasons.  However, if the same reason is signaled multiple times in quick
 * succession, the process is likely to observe only one notification of it.
 * This is okay for the present uses.
 *
 * Also, because of race conditions, it's important that all the signals be
 * defined so that no harm is done if a process mistakenly receives one.
 */

/*
 * 向 Postgres 子进程（后端或检查点进程等辅助进程）发送信号的原因。不同原因的并发
 * 信号可以处理；但同一原因在短时间内多次发信号时，进程可能只观察到一次通知。这对当前
 * 用途可以接受。
 *
 * 此外，由于存在竞争条件，所有信号均须定义为即使进程误收也不会造成损害。
 */
typedef enum
{
	PROCSIG_CATCHUP_INTERRUPT,	/* sinval catchup interrupt */

	/* sinval 追赶中断。 */
	PROCSIG_NOTIFY_INTERRUPT,	/* listen/notify interrupt */

	/* listen/notify 中断。 */
	PROCSIG_PARALLEL_MESSAGE,	/* message from cooperating parallel backend */

	/* 来自协作并行后端的消息。 */
	PROCSIG_WALSND_INIT_STOPPING,	/* ask walsenders to prepare for shutdown  */

	/* 要求 WAL 发送器为关闭做准备。 */
	PROCSIG_BARRIER,			/* global barrier interrupt  */

	/* 全局屏障中断。 */
	PROCSIG_LOG_MEMORY_CONTEXT, /* ask backend to log the memory contexts */

	/* 要求后端记录内存上下文。 */
	PROCSIG_PARALLEL_APPLY_MESSAGE, /* Message from parallel apply workers */

	/* 来自并行应用工作进程的消息。 */

	/* Recovery conflict reasons */

	/* 恢复冲突原因。 */
	PROCSIG_RECOVERY_CONFLICT_FIRST,
	PROCSIG_RECOVERY_CONFLICT_DATABASE = PROCSIG_RECOVERY_CONFLICT_FIRST,
	PROCSIG_RECOVERY_CONFLICT_TABLESPACE,
	PROCSIG_RECOVERY_CONFLICT_LOCK,
	PROCSIG_RECOVERY_CONFLICT_SNAPSHOT,
	PROCSIG_RECOVERY_CONFLICT_LOGICALSLOT,
	PROCSIG_RECOVERY_CONFLICT_BUFFERPIN,
	PROCSIG_RECOVERY_CONFLICT_STARTUP_DEADLOCK,
	PROCSIG_RECOVERY_CONFLICT_LAST = PROCSIG_RECOVERY_CONFLICT_STARTUP_DEADLOCK,
} ProcSignalReason;

#define NUM_PROCSIGNALS (PROCSIG_RECOVERY_CONFLICT_LAST + 1)

typedef enum
{
	PROCSIGNAL_BARRIER_SMGRRELEASE, /* ask smgr to close files */

	/* 要求 smgr 关闭文件。 */
} ProcSignalBarrierType;

/*
 * Length of query cancel keys generated.
 *
 * Note that the protocol allows for longer keys, or shorter, but this is the
 * length we actually generate.  Client code, and the server code that handles
 * incoming cancellation packets from clients, mustn't use this hardcoded
 * length.
 */

/*
 * 生成的查询取消密钥长度。
 *
 * 协议允许更长或更短的密钥，但这就是实际生成的长度。客户端代码以及处理客户端传入取消
 * 数据包的服务器代码不得使用这个硬编码长度。
 */
#define MAX_CANCEL_KEY_LENGTH  32

/*
 * prototypes for functions in procsignal.c
 */

/*
 * procsignal.c 中函数的原型。
 */
/*
 * Returns the shared-memory space required for process signaling.
 */

/*
 * 返回进程信号机制所需的共享内存空间。
 */
extern Size ProcSignalShmemSize(void);
/*
 * Initializes shared process-signal state.
 */

/*
 * 初始化共享的进程信号状态。
 */
extern void ProcSignalShmemInit(void);

/*
 * Initializes per-process signal state using a cancellation key.
 */

/*
 * 使用取消密钥初始化每进程的信号状态。
 */
extern void ProcSignalInit(const uint8 *cancel_key, int cancel_key_len);
/*
 * Locates a target process and sends it a reason-specific signal.
 */

/*
 * 定位目标进程并向其发送原因专属的信号。
 */
extern int	SendProcSignal(pid_t pid, ProcSignalReason reason,
						   ProcNumber procNumber);
/*
 * Sends a client cancellation request after validating its key.
 */

/*
 * 验证密钥后发送客户端取消请求。
 */
extern void SendCancelRequest(int backendPID, const uint8 *cancel_key, int cancel_key_len);

/*
 * Emits a process-signal barrier and returns its generation number.
 */

/*
 * 发出进程信号屏障并返回其代次编号。
 */
extern uint64 EmitProcSignalBarrier(ProcSignalBarrierType type);
/*
 * Waits until all relevant processes reach a barrier generation.
 */

/*
 * 等待所有相关进程到达某个屏障代次。
 */
extern void WaitForProcSignalBarrier(uint64 generation);
/*
 * Processes pending process-signal barrier work in the current backend.
 */

/*
 * 在当前后端处理待处理的进程信号屏障工作。
 */
extern void ProcessProcSignalBarrier(void);

/*
 * Handles SIGUSR1 by recording and dispatching process-signal work.
 */

/*
 * 通过记录和分派进程信号工作来处理 SIGUSR1。
 */
extern void procsignal_sigusr1_handler(SIGNAL_ARGS);

/* ProcSignalHeader is an opaque struct, details known only within procsignal.c */

/* ProcSignalHeader 是不透明结构体，细节仅在 procsignal.c 中可见。 */
typedef struct ProcSignalHeader ProcSignalHeader;

#ifdef EXEC_BACKEND
extern PGDLLIMPORT ProcSignalHeader *ProcSignal;
#endif

#endif							/* PROCSIGNAL_H */

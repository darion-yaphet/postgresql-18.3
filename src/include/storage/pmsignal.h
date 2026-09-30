/*-------------------------------------------------------------------------
 *
 * pmsignal.h
 *	  routines for signaling between the postmaster and its child processes
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/pmsignal.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PMSIGNAL_H
#define PMSIGNAL_H

#include <signal.h>

#ifdef HAVE_SYS_PRCTL_H
#include <sys/prctl.h>
#endif

#ifdef HAVE_SYS_PROCCTL_H
#include <sys/procctl.h>
#endif

/*
 * Reasons for signaling the postmaster.  We can cope with simultaneous
 * signals for different reasons.  If the same reason is signaled multiple
 * times in quick succession, however, the postmaster is likely to observe
 * only one notification of it.  This is okay for the present uses.
 */

/*
 * 向 postmaster 发送信号的原因。不同原因的信号可以同时处理；但同一原因在短时
 * 间隔内多次发信号时，postmaster 很可能只观察到一次通知。这对当前用途是可接受的。
 */
typedef enum
{
	PMSIGNAL_RECOVERY_STARTED,	/* recovery has started */

	/* 恢复已经开始。 */
	PMSIGNAL_RECOVERY_CONSISTENT,	/* recovery has reached consistent state */

	/* 恢复已达到一致状态。 */
	PMSIGNAL_BEGIN_HOT_STANDBY, /* begin Hot Standby */

	/* 开始热备。 */
	PMSIGNAL_ROTATE_LOGFILE,	/* send SIGUSR1 to syslogger to rotate logfile */

	/* 向 syslogger 发送 SIGUSR1 以轮转日志文件。 */
	PMSIGNAL_START_AUTOVAC_LAUNCHER,	/* start an autovacuum launcher */

	/* 启动自动清理启动器。 */
	PMSIGNAL_START_AUTOVAC_WORKER,	/* start an autovacuum worker */

	/* 启动自动清理工作进程。 */
	PMSIGNAL_BACKGROUND_WORKER_CHANGE,	/* background worker state change */

	/* 后台工作进程状态发生变化。 */
	PMSIGNAL_START_WALRECEIVER, /* start a walreceiver */

	/* 启动 WAL 接收器。 */
	PMSIGNAL_ADVANCE_STATE_MACHINE, /* advance postmaster's state machine */

	/* 推进 postmaster 的状态机。 */
	PMSIGNAL_XLOG_IS_SHUTDOWN,	/* ShutdownXLOG() completed */

	/* ShutdownXLOG() 已完成。 */
} PMSignalReason;

#define NUM_PMSIGNALS (PMSIGNAL_XLOG_IS_SHUTDOWN+1)

/*
 * Reasons why the postmaster would send SIGQUIT to its children.
 */

/*
 * postmaster 向其子进程发送 SIGQUIT 的原因。
 */
typedef enum
{
	PMQUIT_NOT_SENT = 0,		/* postmaster hasn't sent SIGQUIT */

	/* postmaster 尚未发送 SIGQUIT。 */
	PMQUIT_FOR_CRASH,			/* some other backend bought the farm */

	/* 另一个后端进程已异常退出。 */
	PMQUIT_FOR_STOP,			/* immediate stop was commanded */

	/* 已下达立即停止命令。 */
} QuitSignalReason;

/* PMSignalData is an opaque struct, details known only within pmsignal.c */

/* PMSignalData 是不透明结构体，细节仅在 pmsignal.c 中可见。 */
typedef struct PMSignalData PMSignalData;

#ifdef EXEC_BACKEND
extern PGDLLIMPORT volatile PMSignalData *PMSignalState;
#endif

/*
 * prototypes for functions in pmsignal.c
 */

/*
 * pmsignal.c 中函数的原型。
 */
/*
 * Returns the shared-memory space required by postmaster signaling.
 */

/*
 * 返回 postmaster 信号机制所需的共享内存空间。
 */
extern Size PMSignalShmemSize(void);
/*
 * Initializes shared state used for postmaster signaling.
 */

/*
 * 初始化供 postmaster 信号机制使用的共享状态。
 */
extern void PMSignalShmemInit(void);
/*
 * Records and sends a reason-specific signal to the postmaster.
 */

/*
 * 记录原因专属的信号并将其发送给 postmaster。
 */
extern void SendPostmasterSignal(PMSignalReason reason);
/*
 * Tests and consumes a pending postmaster signal reason.
 */

/*
 * 检查并消费一个待处理的 postmaster 信号原因。
 */
extern bool CheckPostmasterSignal(PMSignalReason reason);
/*
 * Stores why the postmaster should send SIGQUIT to children.
 */

/*
 * 存储 postmaster 应向子进程发送 SIGQUIT 的原因。
 */
extern void SetQuitSignalReason(QuitSignalReason reason);
/*
 * Returns the stored SIGQUIT reason for postmaster children.
 */

/*
 * 返回为 postmaster 子进程存储的 SIGQUIT 原因。
 */
extern QuitSignalReason GetQuitSignalReason(void);
/*
 * Marks a postmaster child slot as assigned.
 */

/*
 * 将一个 postmaster 子进程槽位标记为已分配。
 */
extern void MarkPostmasterChildSlotAssigned(int slot);
/*
 * Marks a postmaster child slot as unassigned and reports prior assignment.
 */

/*
 * 将一个 postmaster 子进程槽位标记为未分配，并报告其先前的分配状态。
 */
extern bool MarkPostmasterChildSlotUnassigned(int slot);
/*
 * Reports whether a postmaster child slot belongs to a WAL sender.
 */

/*
 * 报告 postmaster 子进程槽位是否属于 WAL 发送器。
 */
extern bool IsPostmasterChildWalSender(int slot);
/*
 * Registers the current postmaster child as active.
 */

/*
 * 将当前 postmaster 子进程登记为活动状态。
 */
extern void RegisterPostmasterChildActive(void);
/*
 * Marks the current postmaster child as a WAL sender.
 */

/*
 * 将当前 postmaster 子进程标记为 WAL 发送器。
 */
extern void MarkPostmasterChildWalSender(void);
/*
 * Performs the authoritative liveness check for the postmaster.
 */

/*
 * 对 postmaster 执行权威的存活检查。
 */
extern bool PostmasterIsAliveInternal(void);
/*
 * Installs parent-death signal handling when the platform supports it.
 */

/*
 * 在平台支持时安装父进程死亡信号处理。
 */
extern void PostmasterDeathSignalInit(void);


/*
 * Do we have a way to ask for a signal on parent death?
 *
 * If we do, pmsignal.c will set up a signal handler, that sets a flag when
 * the parent dies.  Checking the flag first makes PostmasterIsAlive() a lot
 * cheaper in usual case that the postmaster is alive.
 */

/*
 * 是否可以请求在父进程死亡时收到信号？
 *
 * 如果可以，pmsignal.c 会设置一个信号处理器，在父进程死亡时设置标志。先检查
 * 该标志可使 PostmasterIsAlive() 在 postmaster 正常存活这一常见情形下更廉价。
 */
#if (defined(HAVE_SYS_PRCTL_H) && defined(PR_SET_PDEATHSIG)) || \
	(defined(HAVE_SYS_PROCCTL_H) && defined(PROC_PDEATHSIG_CTL))
#define USE_POSTMASTER_DEATH_SIGNAL
#endif

#ifdef USE_POSTMASTER_DEATH_SIGNAL
extern PGDLLIMPORT volatile sig_atomic_t postmaster_possibly_dead;

/*
 * Checks the cached parent-death flag before performing the full liveness check.
 */

/*
 * 先检查缓存的父进程死亡标志，再执行完整的存活检查。
 */
static inline bool
PostmasterIsAlive(void)
{
	if (likely(!postmaster_possibly_dead))
		return true;
	return PostmasterIsAliveInternal();
}
#else
#define PostmasterIsAlive() PostmasterIsAliveInternal()
#endif

#endif							/* PMSIGNAL_H */

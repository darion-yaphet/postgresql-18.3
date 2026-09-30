/*-------------------------------------------------------------------------
 *
 * latch.h
 *	  Routines for interprocess latches
 *
 * A latch is a boolean variable, with operations that let processes sleep
 * until it is set. A latch can be set from another process, or a signal
 * handler within the same process.
 *
 * The latch interface is a reliable replacement for the common pattern of
 * using pg_usleep() or select() to wait until a signal arrives, where the
 * signal handler sets a flag variable. Because on some platforms an
 * incoming signal doesn't interrupt sleep, and even on platforms where it
 * does there is a race condition if the signal arrives just before
 * entering the sleep, the common pattern must periodically wake up and
 * poll the flag variable. The pselect() system call was invented to solve
 * this problem, but it is not portable enough. Latches are designed to
 * overcome these limitations, allowing you to sleep without polling and
 * ensuring quick response to signals from other processes.
 *
 * There are two kinds of latches: local and shared. A local latch is
 * initialized by InitLatch, and can only be set from the same process.
 * A local latch can be used to wait for a signal to arrive, by calling
 * SetLatch in the signal handler. A shared latch resides in shared memory,
 * and must be initialized at postmaster startup by InitSharedLatch. Before
 * a shared latch can be waited on, it must be associated with a process
 * with OwnLatch. Only the process owning the latch can wait on it, but any
 * process can set it.
 *
 * There are three basic operations on a latch:
 *
 * SetLatch		- Sets the latch
 * ResetLatch	- Clears the latch, allowing it to be set again
 * WaitLatch	- Waits for the latch to become set
 *
 * WaitLatch includes a provision for timeouts (which should be avoided
 * when possible, as they incur extra overhead) and a provision for
 * postmaster child processes to wake up immediately on postmaster death.
 * See latch.c for detailed specifications for the exported functions.
 *
 * The correct pattern to wait for event(s) is:
 *
 * for (;;)
 * {
 *	   ResetLatch();
 *	   if (work to do)
 *		   Do Stuff();
 *	   WaitLatch();
 * }
 *
 * It's important to reset the latch *before* checking if there's work to
 * do. Otherwise, if someone sets the latch between the check and the
 * ResetLatch call, you will miss it and Wait will incorrectly block.
 *
 * Another valid coding pattern looks like:
 *
 * for (;;)
 * {
 *	   if (work to do)
 *		   Do Stuff(); // in particular, exit loop if some condition satisfied
 *	   WaitLatch();
 *	   ResetLatch();
 * }
 *
 * This is useful to reduce latch traffic if it's expected that the loop's
 * termination condition will often be satisfied in the first iteration;
 * the cost is an extra loop iteration before blocking when it is not.
 * What must be avoided is placing any checks for asynchronous events after
 * WaitLatch and before ResetLatch, as that creates a race condition.
 *
 * To wake up the waiter, you must first set a global flag or something
 * else that the wait loop tests in the "if (work to do)" part, and call
 * SetLatch *after* that. SetLatch is designed to return quickly if the
 * latch is already set.
 *
 * On some platforms, signals will not interrupt the latch wait primitive
 * by themselves.  Therefore, it is critical that any signal handler that
 * is meant to terminate a WaitLatch wait calls SetLatch.
 *
 * Note that use of the process latch (PGPROC.procLatch) is generally better
 * than an ad-hoc shared latch for signaling auxiliary processes.  This is
 * because generic signal handlers will call SetLatch on the process latch
 * only, so using any latch other than the process latch effectively precludes
 * use of any generic handler.
 *
 *
 * See also WaitEventSets in waiteventset.h. They allow to wait for latches
 * being set and additional events - postmaster dying and socket readiness of
 * several sockets currently - at the same time.  On many platforms using a
 * long lived event set is more efficient than using WaitLatch or
 * WaitLatchOrSocket.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/latch.h
 *
 *-------------------------------------------------------------------------
 */

/*
 * 闩锁是布尔变量，其操作允许进程睡眠直到它被设置。它可由其他进程设置，也可由同一进程内的
 * 信号处理程序设置。
 * 闩锁接口可靠地替代了等待信号到达时使用 pg_usleep() 或 select() 的常见模式；在该模式中，
 * 信号处理程序会设置一个标志变量。某些平台上，传入信号不会中断睡眠；即使能中断，若信号恰好在
 * 进入睡眠前到达也会产生竞态。因此该常见模式必须定期唤醒并轮询标志变量。pselect() 系统调用
 * 专为解决此问题而设计，但其可移植性不足。闩锁旨在克服这些限制，使进程无需轮询即可睡眠，并能
 * 快速响应其他进程的信号。
 * 闩锁分为本地和共享两种。本地闩锁由 InitLatch 初始化，只能由同一进程设置；可在信号处理程序
 * 中调用 SetLatch 以等待信号到达。共享闩锁驻留在共享内存中，必须由 postmaster 在启动时通过
 * InitSharedLatch 初始化。在等待共享闩锁前，必须用 OwnLatch 将其关联到一个进程。只有拥有闩锁的
 * 进程可等待它，但任意进程都可设置它。
 * 三个基本操作是：SetLatch 设置闩锁；ResetLatch 清除闩锁以允许再次设置；WaitLatch 等待闩锁被设置。
 * WaitLatch 支持超时（应尽量避免，因为会带来额外开销），并支持 postmaster 子进程在 postmaster
 * 死亡时立即唤醒。导出函数的详细规范见 latch.c。
 * 等待事件的正确模式如下：先 ResetLatch()，检查是否有工作；如有则执行工作，然后调用 WaitLatch()。
 * 必须在检查是否有工作之前重置闩锁。否则，若有人在检查和调用 ResetLatch 之间设置闩锁，事件会丢失，
 * Wait 将错误地阻塞。
 * 另一种有效模式是：检查工作、必要时执行工作，再 WaitLatch()，最后 ResetLatch()。
 * 当预期循环常在第一次迭代即满足退出条件时，该模式可减少闩锁流量；代价是在不满足时阻塞前多一次
 * 循环迭代。必须避免在 WaitLatch 和 ResetLatch 之间检查异步事件，因为这会造成竞态。
 * 要唤醒等待者，必须先设置等待循环在“if (work to do)”部分测试的全局标志或其他状态，然后调用
 * SetLatch。若闩锁已设置，SetLatch 会快速返回。
 * 某些平台上的信号本身不会中断闩锁等待原语。因此，任何用于终止 WaitLatch 等待的信号处理程序都必须
 * 调用 SetLatch。
 * 对于辅助进程的信号通知，通常应使用进程闩锁（PGPROC.procLatch），而不是临时共享闩锁。通用信号
 * 处理程序只会对进程闩锁调用 SetLatch；使用其他闩锁实际上会阻止使用任何通用处理程序。
 * 另见 waiteventset.h 中的 WaitEventSets。它们可同时等待闩锁被设置和其他事件（当前包括
 * postmaster 死亡与多个套接字就绪）。在许多平台上，长期存在的事件集比 WaitLatch 或
 * WaitLatchOrSocket 更高效。
 */
#ifndef LATCH_H
#define LATCH_H

#include <signal.h>

#include "storage/waiteventset.h"	/* for WL_* arguments to WaitLatch */

/*
 * 用于 WaitLatch 的 WL_* 参数。
 */

/*
 * Latch structure should be treated as opaque and only accessed through
 * the public functions. It is defined here to allow embedding Latches as
 * part of bigger structs.
 */

/*
 * 应将 Latch 结构视为不透明对象，并且只能通过公共函数访问。之所以在此定义，是为了允许将 Latch
 * 嵌入更大的结构中。
 */
typedef struct Latch
{
	sig_atomic_t is_set;
	sig_atomic_t maybe_sleeping;
	bool		is_shared;
	int			owner_pid;
#ifdef WIN32
	HANDLE		event;
#endif
} Latch;

/*
 * prototypes for functions in latch.c
 */

/*
 * latch.c 中函数的原型。
 */
/*
 * Initializes a process-local latch for use by its creating process.
 */

/*
 * 初始化供创建进程使用的进程本地闩锁。
 */
extern void InitLatch(Latch *latch);
/*
 * Initializes a shared-memory latch during postmaster startup.
 */

/*
 * 在 postmaster 启动期间初始化共享内存闩锁。
 */
extern void InitSharedLatch(Latch *latch);
/*
 * Associates a shared latch with the current process so it can wait on it.
 */

/*
 * 将共享闩锁关联到当前进程，使该进程可以等待它。
 */
extern void OwnLatch(Latch *latch);
/*
 * Removes the current process's ownership of a shared latch.
 */

/*
 * 移除当前进程对共享闩锁的所有权。
 */
extern void DisownLatch(Latch *latch);
/*
 * Sets a latch and wakes a waiting owner when necessary.
 */

/*
 * 设置闩锁，并在需要时唤醒正在等待的所有者。
 */
extern void SetLatch(Latch *latch);
/*
 * Clears a latch after the caller has processed pending work.
 */

/*
 * 在调用方处理完待处理工作后清除闩锁。
 */
extern void ResetLatch(Latch *latch);

/*
 * Waits for latch, socket, timeout, or requested wake events and returns them.
 */

/*
 * 等待闩锁、套接字、超时或请求的唤醒事件，并返回发生的事件。
 */
extern int	WaitLatch(Latch *latch, int wakeEvents, long timeout,
					  uint32 wait_event_info);
/*
 * Waits for a latch or socket while also honoring wake events and timeout.
 */

/*
 * 等待闩锁或套接字，同时处理唤醒事件和超时。
 */
extern int	WaitLatchOrSocket(Latch *latch, int wakeEvents,
							  pgsocket sock, long timeout, uint32 wait_event_info);
/*
 * Initializes the shared wait set used by latch wait operations.
 */

/*
 * 初始化闩锁等待操作使用的共享等待集。
 */
extern void InitializeLatchWaitSet(void);

#endif							/* LATCH_H */

/*-------------------------------------------------------------------------
 *
 * waiteventset.h
 *		ppoll() / pselect() like interface for waiting for events
 *
 *
 *		用于等待事件的、类似 ppoll() / pselect() 的接口。
 *
 * WaitEventSets allow to wait for latches being set and additional events -
 * postmaster dying and socket readiness of several sockets currently - at the
 * same time.  On many platforms using a long lived event set is more
 * efficient than using WaitLatch or WaitLatchOrSocket.
 *
 * WaitEventSetWait includes a provision for timeouts (which should be avoided
 * when possible, as they incur extra overhead) and a provision for postmaster
 * child processes to wake up immediately on postmaster death.  See
 * storage/ipc/waiteventset.c for detailed specifications for the exported
 * functions.
 *
 * WaitEventSet 允许同时等待锁存器被设置以及其他事件——目前包括 postmaster
 * 退出和多个套接字的就绪状态。在许多平台上，使用长期存在的事件集比使用
 * WaitLatch 或 WaitLatchOrSocket 更高效。
 *
 * WaitEventSetWait 支持超时（应尽量避免，因为会带来额外开销），还支持在
 * postmaster 死亡时让 postmaster 子进程立即唤醒。有关导出函数的详细规范，
 * 请参见 storage/ipc/waiteventset.c。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/waiteventset.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WAITEVENTSET_H
#define WAITEVENTSET_H

#include "utils/resowner.h"

/*
 * Bitmasks for events that may wake-up WaitLatch(), WaitLatchOrSocket(), or
 * WaitEventSetWait().
 */

/*
 * 可唤醒 WaitLatch()、WaitLatchOrSocket() 或 WaitEventSetWait() 的事件位掩码。
 */
#define WL_LATCH_SET		 (1 << 0)
#define WL_SOCKET_READABLE	 (1 << 1)
#define WL_SOCKET_WRITEABLE  (1 << 2)
#define WL_TIMEOUT			 (1 << 3)	/* not for WaitEventSetWait() */

										/* 不用于 WaitEventSetWait()。 */
#define WL_POSTMASTER_DEATH  (1 << 4)
#define WL_EXIT_ON_PM_DEATH	 (1 << 5)
#ifdef WIN32
#define WL_SOCKET_CONNECTED  (1 << 6)
#else
/* avoid having to deal with case on platforms not requiring it */

/* 避免在不需要此情况的平台上处理该分支。 */
#define WL_SOCKET_CONNECTED  WL_SOCKET_WRITEABLE
#endif
#define WL_SOCKET_CLOSED 	 (1 << 7)
#ifdef WIN32
#define WL_SOCKET_ACCEPT	 (1 << 8)
#else
/* avoid having to deal with case on platforms not requiring it */

/* 避免在不需要此情况的平台上处理该分支。 */
#define WL_SOCKET_ACCEPT	WL_SOCKET_READABLE
#endif
#define WL_SOCKET_MASK		(WL_SOCKET_READABLE | \
							 WL_SOCKET_WRITEABLE | \
							 WL_SOCKET_CONNECTED | \
							 WL_SOCKET_ACCEPT | \
							 WL_SOCKET_CLOSED)

typedef struct WaitEvent
{
	int			pos;			/* position in the event data structure */

									/* 事件数据结构中的位置。 */
	uint32		events;			/* triggered events */

									/* 已触发的事件。 */
	pgsocket	fd;				/* socket fd associated with event */

									/* 与事件关联的套接字 fd。 */
	void	   *user_data;		/* pointer provided in AddWaitEventToSet */

									/* AddWaitEventToSet 提供的指针。 */
#ifdef WIN32
	bool		reset;			/* Is reset of the event required? */

									/* 是否需要重置该事件？ */
#endif
} WaitEvent;

/* forward declarations to avoid exposing waiteventset.c implementation details */

/* 前向声明，以避免暴露 waiteventset.c 的实现细节。 */
typedef struct WaitEventSet WaitEventSet;

struct Latch;

/*
 * prototypes for functions in waiteventset.c
 */

/*
 * waiteventset.c 中函数的原型。
 */

/* Initialize platform support for wait event sets.
 * Startup prepares the operating-system mechanisms that later event sets use
 * for latch and socket waits.
 *
 * 初始化等待事件集的平台支持。
 * 启动过程准备随后事件集用于锁存器和套接字等待的操作系统机制。
 */
extern void InitializeWaitEventSupport(void);

/* Allocate a wait event set with room for nevents registrations.
 * The set is associated with the resource owner so it can be reclaimed with
 * its owning resource scope.
 *
 * 分配可容纳 nevents 个注册项的等待事件集。
 * 该集合关联资源所有者，因此可随其拥有的资源作用域一同回收。
 */
extern WaitEventSet *CreateWaitEventSet(ResourceOwner resowner, int nevents);

/* Free a wait event set and its registered event resources.
 * The routine tears down platform state and detaches it from its owner.
 *
 * 释放等待事件集及其注册的事件资源。
 * 此例程拆除平台状态并将其与资源所有者分离。
 */
extern void FreeWaitEventSet(WaitEventSet *set);

/* Free a wait event set safely after a process fork.
 * The cleanup path avoids using state inherited unsafely across fork.
 *
 * 在进程 fork 后安全释放等待事件集。
 * 清理路径避免使用跨 fork 不安全继承的状态。
 */
extern void FreeWaitEventSetAfterFork(WaitEventSet *set);

/* Add one latch or socket event to a wait set.
 * The function validates the event mask, records the supplied descriptors,
 * and returns the registration position used by later modifications.
 *
 * 向等待集中添加一个锁存器或套接字事件。
 * 此函数验证事件掩码、记录给定描述符，并返回供后续修改使用的注册位置。
 */
extern int	AddWaitEventToSet(WaitEventSet *set, uint32 events, pgsocket fd,
							  struct Latch *latch, void *user_data);

/* Change an existing wait event registration.
 * The position selects the stored entry, whose mask and latch are updated for
 * subsequent waits.
 *
 * 修改已有的等待事件注册项。
 * 位置选择已存储条目，并更新其掩码和锁存器以供后续等待。
 */
extern void ModifyWaitEvent(WaitEventSet *set, int pos, uint32 events,
							struct Latch *latch);

/* Wait for registered events or timeout.
 * The routine blocks through the platform event mechanism, then fills the
 * caller's occurrence array and returns its event count.
 *
 * 等待已注册事件或超时。
 * 此例程通过平台事件机制阻塞，然后填充调用方的发生事件数组并返回事件数量。
 */
extern int	WaitEventSetWait(WaitEventSet *set, long timeout,
							 WaitEvent *occurred_events, int nevents,
							 uint32 wait_event_info);
/* Return the number of currently registered events.
 * The routine reads the set's registration count without waiting.
 *
 * 返回当前已注册事件的数量。
 * 此例程无需等待即可读取集合的注册计数。
 */
extern int	GetNumRegisteredWaitEvents(WaitEventSet *set);

/* Report whether the platform can signal socket-closed events.
 * Callers use the capability result to choose compatible event handling.
 *
 * 报告平台是否能通知套接字关闭事件。
 * 调用方使用该能力结果选择兼容的事件处理方式。
 */
extern bool WaitEventSetCanReportClosed(void);

#ifndef WIN32
/* Wake the current process from its wait event set.
 * The routine signals the process-local wakeup mechanism used by latch waits.
 *
 * 从等待事件集中唤醒当前进程。
 * 此例程向锁存器等待使用的进程本地唤醒机制发信号。
 */
extern void WakeupMyProc(void);

/* Wake another process from a wait event set.
 * The target process ID selects the external wakeup mechanism to signal.
 *
 * 从等待事件集中唤醒另一个进程。
 * 目标进程 ID 选择要发信号的外部唤醒机制。
 */
extern void WakeupOtherProc(int pid);
#endif

#endif							/* WAITEVENTSET_H */

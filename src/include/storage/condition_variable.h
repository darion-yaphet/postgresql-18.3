/*-------------------------------------------------------------------------
 *
 * condition_variable.h
 *	  Condition variables
 *
 *	  条件变量。
 *
 * A condition variable is a method of waiting until a certain condition
 * becomes true.  Conventionally, a condition variable supports three
 * operations: (1) sleep; (2) signal, which wakes up one process sleeping
 * on the condition variable; and (3) broadcast, which wakes up every
 * process sleeping on the condition variable.  In our implementation,
 * condition variables put a process into an interruptible sleep (so it
 * can be canceled prior to the fulfillment of the condition) and do not
 * use pointers internally (so that they are safe to use within DSMs).
 *
 * 条件变量是一种等待某个条件变为真的方法。按照惯例，条件变量支持三种操作：
 * （1）休眠；（2）信号，唤醒一个在条件变量上休眠的进程；（3）广播，唤醒每个
 * 在条件变量上休眠的进程。在我们的实现中，条件变量让进程进入可中断的休眠状态
 * （因而可以在条件满足前取消），且内部不使用指针（因而可以安全地用于 DSM）。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/condition_variable.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CONDITION_VARIABLE_H
#define CONDITION_VARIABLE_H

#include "storage/proclist_types.h"
#include "storage/spin.h"

typedef struct
{
	slock_t		mutex;			/* spinlock protecting the wakeup list */

								/* 保护唤醒列表的自旋锁 */
	proclist_head wakeup;		/* list of wake-able processes */

								/* 可被唤醒的进程列表 */
} ConditionVariable;

/*
 * Pad a condition variable to a power-of-two size so that an array of
 * condition variables does not cross a cache line boundary.
 *
 * 将条件变量填充到 2 的幂大小，以便条件变量数组不会跨越缓存行边界。
 */
#define CV_MINIMAL_SIZE		(sizeof(ConditionVariable) <= 16 ? 16 : 32)
typedef union ConditionVariableMinimallyPadded
{
	ConditionVariable cv;
	char		pad[CV_MINIMAL_SIZE];
} ConditionVariableMinimallyPadded;

/* Initialize a condition variable. */

/* 初始化条件变量。 */
/*
 * The function initializes the spinlock and empty wakeup list before use.
 *
 * 该函数在使用前初始化自旋锁和空的唤醒列表。
 */
extern void ConditionVariableInit(ConditionVariable *cv);

/*
 * To sleep on a condition variable, a process should use a loop which first
 * checks the condition, exiting the loop if it is met, and then calls
 * ConditionVariableSleep.  Spurious wakeups are possible, but should be
 * infrequent.  After exiting the loop, ConditionVariableCancelSleep must
 * be called to ensure that the process is no longer in the wait list for
 * the condition variable.
 *
 * 要在条件变量上休眠，进程应当使用一个循环：先检查条件，若条件已满足则退出循环，
 * 否则调用 ConditionVariableSleep。可能发生伪唤醒，但应当很少。退出循环后必须调用
 * ConditionVariableCancelSleep，以确保该进程不再位于条件变量的等待列表中。
 */

/*
 * Sleep on a condition variable until a signal, interrupt, or wakeup occurs.
 * The function registers the caller in the wait list and performs an interruptible wait.
 *
 * 在条件变量上休眠，直到发生信号、中断或唤醒。
 * 该函数将调用者注册到等待列表中并执行可中断的等待。
 */
extern void ConditionVariableSleep(ConditionVariable *cv, uint32 wait_event_info);

/*
 * Sleep on a condition variable with a timeout.
 * The function follows the normal wait-list protocol and reports whether the wait completed.
 *
 * 带超时地在条件变量上休眠。
 * 该函数遵循常规等待列表协议，并报告等待是否完成。
 */
extern bool ConditionVariableTimedSleep(ConditionVariable *cv, long timeout,
										uint32 wait_event_info);

/*
 * Remove the current process from a condition-variable wait list.
 * The function finalizes a prior sleep attempt so the caller no longer receives wakeups.
 *
 * 将当前进程从条件变量等待列表中移除。
 * 该函数结束先前的休眠尝试，使调用者不再接收唤醒通知。
 */
extern bool ConditionVariableCancelSleep(void);

/*
 * Optionally, ConditionVariablePrepareToSleep can be called before entering
 * the test-and-sleep loop described above.  Doing so is more efficient if
 * at least one sleep is needed, whereas not doing so is more efficient when
 * no sleep is needed because the test condition is true the first time.
 *
 * 进入上述测试并休眠循环前，可以选择调用 ConditionVariablePrepareToSleep。
 * 如果至少需要一次休眠，这样做更高效；若测试条件第一次即为真而无需休眠，
 * 则不这样做更高效。
 */

/*
 * Prepare the current process to sleep on a condition variable.
 * The function performs early wait-list setup to optimize the expected sleep path.
 *
 * 准备当前进程在条件变量上休眠。
 * 该函数提前设置等待列表，以优化预期的休眠路径。
 */
extern void ConditionVariablePrepareToSleep(ConditionVariable *cv);

/* Wake up a single waiter (via signal) or all waiters (via broadcast). */

/* 通过信号唤醒单个等待者，或通过广播唤醒所有等待者。 */

/*
 * Wake one process waiting on a condition variable.
 * The function selects one registered waiter and signals it to resume.
 *
 * 唤醒一个等待条件变量的进程。
 * 该函数选择一个已注册的等待者并向其发送恢复执行的信号。
 */
extern void ConditionVariableSignal(ConditionVariable *cv);

/*
 * Wake all processes waiting on a condition variable.
 * The function broadcasts a wakeup to every registered waiter.
 *
 * 唤醒所有等待条件变量的进程。
 * 该函数向每个已注册的等待者广播唤醒通知。
 */
extern void ConditionVariableBroadcast(ConditionVariable *cv);

#endif							/* CONDITION_VARIABLE_H */

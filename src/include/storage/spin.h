/*-------------------------------------------------------------------------
 *
 * spin.h
 *	   API for spinlocks.
 *
 *
 *	   自旋锁 API。
 *
 *
 *	The interface to spinlocks is defined by the typedef "slock_t" and
 *	these macros:
 *
 *	void SpinLockInit(volatile slock_t *lock)
 *		Initialize a spinlock (to the unlocked state).
 *
 *	void SpinLockAcquire(volatile slock_t *lock)
 *		Acquire a spinlock, waiting if necessary.
 *		Time out and abort() if unable to acquire the lock in a
 *		"reasonable" amount of time --- typically ~ 1 minute.
 *
 *	void SpinLockRelease(volatile slock_t *lock)
 *		Unlock a previously acquired lock.
 *
 *	bool SpinLockFree(slock_t *lock)
 *		Tests if the lock is free. Returns true if free, false if locked.
 *		This does *not* change the state of the lock.
 *
 *	Callers must beware that the macro argument may be evaluated multiple
 *	times!
 *
 *	Load and store operations in calling code are guaranteed not to be
 *	reordered with respect to these operations, because they include a
 *	compiler barrier.  (Before PostgreSQL 9.5, callers needed to use a
 *	volatile qualifier to access data protected by spinlocks.)
 *
 *	Keep in mind the coding rule that spinlocks must not be held for more
 *	than a few instructions.  In particular, we assume it is not possible
 *	for a CHECK_FOR_INTERRUPTS() to occur while holding a spinlock, and so
 *	it is not necessary to do HOLD/RESUME_INTERRUPTS() in these macros.
 *
 *	These macros are implemented in terms of hardware-dependent macros
 *	supplied by s_lock.h.  There is not currently any extra functionality
 *	added by this header, but there has been in the past and may someday
 *	be again.
 *
 *	自旋锁的接口由 typedef “slock_t” 和以下宏定义：
 *
 *	void SpinLockInit(volatile slock_t *lock)
 *		初始化自旋锁（为未锁定状态）。
 *
 *	void SpinLockAcquire(volatile slock_t *lock)
 *		获取自旋锁；必要时等待。
 *		若无法在“合理”的时间内获取锁（通常约 1 分钟），则超时并 abort()。
 *
 *	void SpinLockRelease(volatile slock_t *lock)
 *		解锁此前获取的锁。
 *
 *	bool SpinLockFree(slock_t *lock)
 *		测试锁是否空闲。空闲时返回 true，锁定时返回 false。
 *		此操作不会改变锁的状态。
 *
 *	调用方必须注意，宏参数可能会被求值多次！
 *
 *	调用代码中的加载和存储操作保证不会相对于这些操作重排，因为它们包含
 *	编译器屏障。（PostgreSQL 9.5 之前，调用方需要使用 volatile 限定符访问
 *	受自旋锁保护的数据。）
 *
 *	请牢记编码规则：持有自旋锁的时间不得超过几条指令。尤其是，我们假定
 *	持有自旋锁时不可能发生 CHECK_FOR_INTERRUPTS()，因此这些宏无需执行
 *	HOLD/RESUME_INTERRUPTS()。
 *
 *	这些宏基于 s_lock.h 提供的硬件相关宏实现。此头文件当前没有增加额外功能，
 *	但过去曾增加过，将来也可能再次增加。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/spin.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SPIN_H
#define SPIN_H

#include "storage/s_lock.h"


#define SpinLockInit(lock)	S_INIT_LOCK(lock)

#define SpinLockAcquire(lock) S_LOCK(lock)

#define SpinLockRelease(lock) S_UNLOCK(lock)

#define SpinLockFree(lock)	S_LOCK_FREE(lock)

#endif							/* SPIN_H */

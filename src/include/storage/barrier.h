/*-------------------------------------------------------------------------
 *
 * barrier.h
 *	  Barriers for synchronizing cooperating processes.
 *
 *	  用于同步协作进程的屏障。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/barrier.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BARRIER_H
#define BARRIER_H

/*
 * For the header previously known as "barrier.h", please include
 * "port/atomics.h", which deals with atomics, compiler barriers and memory
 * barriers.
 *
 * 对于以前称为 "barrier.h" 的头文件，请包含处理原子操作、编译器屏障和
 * 内存屏障的 "port/atomics.h"。
 */

#include "storage/condition_variable.h"
#include "storage/spin.h"

typedef struct Barrier
{
	slock_t		mutex;
	int			phase;			/* phase counter */

								/* 阶段计数器 */
	int			participants;	/* the number of participants attached */

								/* 已附加参与者的数量 */
	int			arrived;		/* the number of participants that have
								 * arrived */

								/* 已到达的参与者数量 */
	int			elected;		/* highest phase elected */

								/* 已选出的最高阶段 */
	bool		static_party;	/* used only for assertions */

								/* 仅用于断言 */
	ConditionVariable condition_variable;
} Barrier;

/*
 * Initialize a barrier with its initial participant count.
 * The function initializes synchronization state before any participant arrives.
 *
 * 使用初始参与者数量初始化屏障。
 * 该函数会在任何参与者到达前初始化同步状态。
 */
extern void BarrierInit(Barrier *barrier, int participants);

/*
 * Record arrival and wait until all participants complete the phase.
 * The final arrival advances the phase and wakes the remaining participants.
 *
 * 记录到达并等待所有参与者完成当前阶段。
 * 最后一个到达者推进阶段并唤醒其余参与者。
 */
extern bool BarrierArriveAndWait(Barrier *barrier, uint32 wait_event_info);

/*
 * Record arrival, detach this participant, and complete the phase if possible.
 * The function removes the caller from future phases while updating the barrier.
 *
 * 记录到达、分离当前参与者，并在可能时完成该阶段。
 * 该函数在更新屏障状态的同时将调用者移出后续阶段。
 */
extern bool BarrierArriveAndDetach(Barrier *barrier);

/*
 * Record arrival and detach unless this is the final participant.
 * The function preserves the final participant so it can complete the phase.
 *
 * 记录到达，并在调用者不是最后一个参与者时将其分离。
 * 该函数保留最后一个参与者，使其能够完成当前阶段。
 */
extern bool BarrierArriveAndDetachExceptLast(Barrier *barrier);

/*
 * Attach a participant to the barrier and return its current phase.
 * The function increments participation while synchronizing with phase changes.
 *
 * 将一个参与者附加到屏障并返回当前阶段。
 * 该函数在与阶段变更同步的同时增加参与者数量。
 */
extern int	BarrierAttach(Barrier *barrier);

/*
 * Detach a participant from the barrier.
 * The function reduces participation and reports whether its departure completed a phase.
 *
 * 从屏障中分离一个参与者。
 * 该函数减少参与者数量，并报告其离开是否完成了一个阶段。
 */
extern bool BarrierDetach(Barrier *barrier);

/*
 * Return the barrier's current phase.
 * The function reads the phase counter used to distinguish completed rounds.
 *
 * 返回屏障的当前阶段。
 * 该函数读取用于区分已完成轮次的阶段计数器。
 */
extern int	BarrierPhase(Barrier *barrier);

/*
 * Return the number of currently attached participants.
 * The function exposes the participation count maintained by the barrier.
 *
 * 返回当前已附加参与者的数量。
 * 该函数公开屏障维护的参与者计数。
 */
extern int	BarrierParticipants(Barrier *barrier);

#endif							/* BARRIER_H */

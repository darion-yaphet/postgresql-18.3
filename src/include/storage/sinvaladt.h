/*-------------------------------------------------------------------------
 *
 * sinvaladt.h
 *	  POSTGRES shared cache invalidation data manager.
 *
 *
 *	  POSTGRES 共享缓存失效数据管理器。
 *
 * The shared cache invalidation manager is responsible for transmitting
 * invalidation messages between backends.  Any message sent by any backend
 * must be delivered to all already-running backends before it can be
 * forgotten.  (If we run out of space, we instead deliver a "RESET"
 * message to backends that have fallen too far behind.)
 *
 * The struct type SharedInvalidationMessage, defining the contents of
 * a single message, is defined in sinval.h.
 *
 * 共享缓存失效管理器负责在后端之间传输失效消息。任一后端发送的消息
 * 必须在可被遗忘前交付给所有已运行的后端。（若空间耗尽，则向落后过多的
 * 后端交付一条“RESET”消息。）
 *
 * 定义单条消息内容的结构类型 SharedInvalidationMessage 定义在 sinval.h 中。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/sinvaladt.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SINVALADT_H
#define SINVALADT_H

#include "storage/lock.h"
#include "storage/sinval.h"

/*
 * prototypes for functions in sinvaladt.c
 */

/*
 * sinvaladt.c 中函数的原型。
 */

/* Return the shared-memory size required by the invalidation manager.
 * The sizing pass accounts for queues and control data before shared memory
 * is allocated.
 *
 * 返回失效管理器所需的共享内存大小。
 * 大小计算阶段会在分配共享内存前计入队列和控制数据。
 */
extern Size SharedInvalShmemSize(void);

/* Initialize shared invalidation queues and control state.
 * Startup creates the shared structures that producers and consumers use to
 * exchange invalidation messages.
 *
 * 初始化共享失效队列和控制状态。
 * 启动过程创建供生产者和消费者交换失效消息的共享结构。
 */
extern void SharedInvalShmemInit(void);

/* Register the current backend with the invalidation manager.
 * The sendOnly mode determines whether this backend also consumes queued
 * messages or only publishes them.
 *
 * 向失效管理器注册当前后端。
 * sendOnly 模式决定此后端是否也消费队列消息，还是仅发布消息。
 */
extern void SharedInvalBackendInit(bool sendOnly);

/* Append invalidation messages to the shared queue.
 * The entries are inserted in order so active backends can later consume the
 * same sequence.
 *
 * 将失效消息追加到共享队列。
 * 条目按顺序插入，使活动后端稍后能消费相同的序列。
 */
extern void SIInsertDataEntries(const SharedInvalidationMessage *data, int n);

/* Copy pending invalidation messages for the current backend.
 * The routine drains up to datasize entries and returns the number copied.
 *
 * 为当前后端复制待处理的失效消息。
 * 此例程最多取出 datasize 个条目并返回复制的数量。
 */
extern int	SIGetDataEntries(SharedInvalidationMessage *data, int datasize);

/* Reclaim shared-queue space after consumers advance.
 * The caller's locking state and requested free space guide queue cleanup or
 * lagging-backend reset handling.
 *
 * 在消费者前进后回收共享队列空间。
 * 调用方的锁状态和所需空闲空间会指导队列清理或落后后端的重置处理。
 */
extern void SICleanupQueue(bool callerHasWriteLock, int minFree);

/* Allocate the next local transaction identifier.
 * The manager advances per-backend transaction state to produce an ID used
 * for ordering local invalidation work.
 *
 * 分配下一个本地事务标识符。
 * 管理器推进每后端事务状态，生成用于排序本地失效工作的 ID。
 */
extern LocalTransactionId GetNextLocalTransactionId(void);

#endif							/* SINVALADT_H */

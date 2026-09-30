/*-------------------------------------------------------------------------
 *
 * shm_mq.h
 *	  single-reader, single-writer shared memory message queue
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/shm_mq.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SHM_MQ_H
#define SHM_MQ_H

#include "postmaster/bgworker.h"
#include "storage/dsm.h"
#include "storage/proc.h"

/* The queue itself, in shared memory. */

/* 位于共享内存中的队列本体。 */
struct shm_mq;
typedef struct shm_mq shm_mq;

/* Backend-private state. */

/* 后端私有状态。 */
struct shm_mq_handle;
typedef struct shm_mq_handle shm_mq_handle;

/* Descriptors for a single write spanning multiple locations. */

/* 描述跨越多个位置的一次写入。 */
typedef struct
{
	const char *data;
	Size		len;
} shm_mq_iovec;

/* Possible results of a send or receive operation. */

/* 发送或接收操作的可能结果。 */
typedef enum
{
	SHM_MQ_SUCCESS,				/* Sent or received a message. */

	/* 已发送或接收一条消息。 */
	SHM_MQ_WOULD_BLOCK,			/* Not completed; retry later. */

	/* 尚未完成；稍后重试。 */
	SHM_MQ_DETACHED,			/* Other process has detached queue. */

	/* 另一个进程已分离队列。 */
} shm_mq_result;

/*
 * Primitives to create a queue and set the sender and receiver.
 *
 * Both the sender and the receiver must be set before any messages are read
 * or written, but they need not be set by the same process.  Each must be
 * set exactly once.
 */

/*
 * 用于创建队列并设置发送者和接收者的原语。
 *
 * 读取或写入任何消息前必须同时设置发送者和接收者，但无需由同一进程设置。二者均只能
 * 设置一次。
 */
/*
 * Creates a shared-memory queue in the supplied memory region.
 */

/*
 * 在提供的内存区域中创建共享内存队列。
 */
extern shm_mq *shm_mq_create(void *address, Size size);
/*
 * Registers the process that receives messages from a queue.
 */

/*
 * 登记从队列接收消息的进程。
 */
extern void shm_mq_set_receiver(shm_mq *mq, PGPROC *);
/*
 * Registers the process that sends messages to a queue.
 */

/*
 * 登记向队列发送消息的进程。
 */
extern void shm_mq_set_sender(shm_mq *mq, PGPROC *);

/* Accessor methods for sender and receiver. */

/* 发送者和接收者的访问器方法。 */
/*
 * Returns the process registered as a queue receiver.
 */

/*
 * 返回登记为队列接收者的进程。
 */
extern PGPROC *shm_mq_get_receiver(shm_mq *);
/*
 * Returns the process registered as a queue sender.
 */

/*
 * 返回登记为队列发送者的进程。
 */
extern PGPROC *shm_mq_get_sender(shm_mq *);

/* Set up backend-local queue state. */

/* 设置后端本地队列状态。 */
/*
 * Attaches a backend to a queue and creates its local handle.
 */

/*
 * 将后端附加到队列并创建其本地句柄。
 */
extern shm_mq_handle *shm_mq_attach(shm_mq *mq, dsm_segment *seg,
									BackgroundWorkerHandle *handle);

/* Associate worker handle with shm_mq. */

/* 将工作进程句柄与 shm_mq 关联。 */
/*
 * Associates a background-worker handle with a queue handle.
 */

/*
 * 将后台工作进程句柄与队列句柄关联。
 */
extern void shm_mq_set_handle(shm_mq_handle *, BackgroundWorkerHandle *);

/* Break connection, release handle resources. */

/* 断开连接并释放句柄资源。 */
/*
 * Detaches a queue handle and releases its backend-local resources.
 */

/*
 * 分离队列句柄并释放其后端本地资源。
 */
extern void shm_mq_detach(shm_mq_handle *mqh);

/* Get the shm_mq from handle. */

/* 从句柄获取 shm_mq。 */
/*
 * Returns the shared-memory queue referenced by a local handle.
 */

/*
 * 返回本地句柄引用的共享内存队列。
 */
extern shm_mq *shm_mq_get_queue(shm_mq_handle *mqh);

/* Send or receive messages. */

/* 发送或接收消息。 */
/*
 * Sends one contiguous message, handling attachment and flushing rules.
 */

/*
 * 发送一条连续消息，并处理附加和刷新规则。
 */
extern shm_mq_result shm_mq_send(shm_mq_handle *mqh,
								 Size nbytes, const void *data, bool nowait,
								 bool force_flush);
/*
 * Sends a vectored message assembled from multiple memory regions.
 */

/*
 * 发送由多个内存区域组装的向量消息。
 */
extern shm_mq_result shm_mq_sendv(shm_mq_handle *mqh, shm_mq_iovec *iov,
								  int iovcnt, bool nowait, bool force_flush);
/*
 * Receives the next queued message and returns its data and size.
 */

/*
 * 接收下一条排队消息，并返回其数据和大小。
 */
extern shm_mq_result shm_mq_receive(shm_mq_handle *mqh,
									Size *nbytesp, void **datap, bool nowait);

/* Wait for our counterparty to attach to the queue. */

/* 等待对端附加到队列。 */
/*
 * Waits until the queue's sender or receiver counterpart is attached.
 */

/*
 * 等待队列的发送者或接收者对端完成附加。
 */
extern shm_mq_result shm_mq_wait_for_attach(shm_mq_handle *mqh);

/* Smallest possible queue. */

/* 最小可用队列。 */
extern PGDLLIMPORT const Size shm_mq_minimum_size;

#endif							/* SHM_MQ_H */

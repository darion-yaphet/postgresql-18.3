/*-------------------------------------------------------------------------
 *
 * walreceiverfuncs.c
 *
 * This file contains functions used by the startup process to communicate
 * with the walreceiver process. Functions implementing walreceiver itself
 * are in walreceiver.c.
 *
 * 本文件是 startup 进程用来与 walreceiver 进程通信的函数。
 * walreceiver 自身的实现在 walreceiver.c。
 *
 * Portions Copyright (c) 2010-2025, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/replication/walreceiverfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>

#include "access/xlog_internal.h"
#include "access/xlogrecovery.h"
#include "pgstat.h"
#include "replication/walreceiver.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "utils/timestamp.h"

WalRcvData *WalRcv = NULL;

/*
 * How long to wait for walreceiver to start up after requesting
 * postmaster to launch it. In seconds.
 *
 * 请求 postmaster 启动 walreceiver 之后，等待它起来的最长时间，单位为秒。
 */
#define WALRCV_STARTUP_TIMEOUT 10

/*
 * 核心流程：startup 通过共享内存请求 postmaster 启动 walreceiver，并读取其写入与刷盘位置。
 */

/* Report shared memory space needed by WalRcvShmemInit
 *
 * 报告 WalRcvShmemInit 所需的共享内存大小。
 */
Size
WalRcvShmemSize(void)
{
	Size		size = 0;

	size = add_size(size, sizeof(WalRcvData));

	return size;
}

/* Allocate and initialize walreceiver-related shared memory
 *
 * 分配并初始化 walreceiver 相关的共享内存。
 */
void
WalRcvShmemInit(void)
{
	bool		found;

	WalRcv = (WalRcvData *)
		ShmemInitStruct("Wal Receiver Ctl", WalRcvShmemSize(), &found);

	if (!found)
	{
		/* First time through, so initialize
		 *
		 * 第一次创建，因此做初始化。
		 */
		MemSet(WalRcv, 0, WalRcvShmemSize());
		WalRcv->walRcvState = WALRCV_STOPPED;
		ConditionVariableInit(&WalRcv->walRcvStoppedCV);
		SpinLockInit(&WalRcv->mutex);
		pg_atomic_init_u64(&WalRcv->writtenUpto, 0);
		WalRcv->procno = INVALID_PROC_NUMBER;
	}
}

/* Is walreceiver running (or starting up)?
 *
 * walreceiver 是否正在运行，或正在启动？
 */
bool
WalRcvRunning(void)
{
	WalRcvData *walrcv = WalRcv;
	WalRcvState state;
	pg_time_t	startTime;

	SpinLockAcquire(&walrcv->mutex);

	state = walrcv->walRcvState;
	startTime = walrcv->startTime;

	SpinLockRelease(&walrcv->mutex);

	/*
	 * If it has taken too long for walreceiver to start up, give up. Setting
	 * the state to STOPPED ensures that if walreceiver later does start up
	 * after all, it will see that it's not supposed to be running and die
	 * without doing anything.
	 *
	 * 若 walreceiver 启动耗时过长就放弃。把状态设为 STOPPED 后，
	 * 即便它后来真的起来，也会发现自己不该运行，于是什么都不做并退出。
	 */
	if (state == WALRCV_STARTING)
	{
		pg_time_t	now = (pg_time_t) time(NULL);

		if ((now - startTime) > WALRCV_STARTUP_TIMEOUT)
		{
			bool		stopped = false;

			SpinLockAcquire(&walrcv->mutex);
			if (walrcv->walRcvState == WALRCV_STARTING)
			{
				state = walrcv->walRcvState = WALRCV_STOPPED;
				stopped = true;
			}
			SpinLockRelease(&walrcv->mutex);

			if (stopped)
				ConditionVariableBroadcast(&walrcv->walRcvStoppedCV);
		}
	}

	if (state != WALRCV_STOPPED)
		return true;
	else
		return false;
}

/*
 * Is walreceiver running and streaming (or at least attempting to connect,
 * or starting up)?
 *
 * walreceiver 是否正在运行并流式接收，或至少正在尝试连接、正在启动？
 */
bool
WalRcvStreaming(void)
{
	WalRcvData *walrcv = WalRcv;
	WalRcvState state;
	pg_time_t	startTime;

	SpinLockAcquire(&walrcv->mutex);

	state = walrcv->walRcvState;
	startTime = walrcv->startTime;

	SpinLockRelease(&walrcv->mutex);

	/*
	 * If it has taken too long for walreceiver to start up, give up. Setting
	 * the state to STOPPED ensures that if walreceiver later does start up
	 * after all, it will see that it's not supposed to be running and die
	 * without doing anything.
	 *
	 * 若 walreceiver 启动耗时过长就放弃。把状态设为 STOPPED 后，
	 * 即便它后来真的起来，也会发现自己不该运行，于是什么都不做并退出。
	 */
	if (state == WALRCV_STARTING)
	{
		pg_time_t	now = (pg_time_t) time(NULL);

		if ((now - startTime) > WALRCV_STARTUP_TIMEOUT)
		{
			bool		stopped = false;

			SpinLockAcquire(&walrcv->mutex);
			if (walrcv->walRcvState == WALRCV_STARTING)
			{
				state = walrcv->walRcvState = WALRCV_STOPPED;
				stopped = true;
			}
			SpinLockRelease(&walrcv->mutex);

			if (stopped)
				ConditionVariableBroadcast(&walrcv->walRcvStoppedCV);
		}
	}

	if (state == WALRCV_STREAMING || state == WALRCV_STARTING ||
		state == WALRCV_RESTARTING)
		return true;
	else
		return false;
}

/*
 * Stop walreceiver (if running) and wait for it to die.
 * Executed by the Startup process.
 *
 * 若 walreceiver 在运行则让它停止，并等待它退出。由 startup 进程执行。
 */
void
ShutdownWalRcv(void)
{
	WalRcvData *walrcv = WalRcv;
	pid_t		walrcvpid = 0;
	bool		stopped = false;

	/*
	 * Request walreceiver to stop. Walreceiver will switch to WALRCV_STOPPED
	 * mode once it's finished, and will also request postmaster to not
	 * restart itself.
	 *
	 * 请求 walreceiver 停止。结束后它会切到 WALRCV_STOPPED，并请 postmaster 不要再重启它。
	 */
	SpinLockAcquire(&walrcv->mutex);
	switch (walrcv->walRcvState)
	{
		case WALRCV_STOPPED:
			break;
		case WALRCV_STARTING:
			walrcv->walRcvState = WALRCV_STOPPED;
			stopped = true;
			break;

		case WALRCV_STREAMING:
		case WALRCV_WAITING:
		case WALRCV_RESTARTING:
			walrcv->walRcvState = WALRCV_STOPPING;
			/* fall through
			 *
			 * 贯穿到下一分支。
			 */
		case WALRCV_STOPPING:
			walrcvpid = walrcv->pid;
			break;
	}
	SpinLockRelease(&walrcv->mutex);

	/* Unnecessary but consistent.
	 *
	 * 并非必需，但与其他分支保持一致。
	 */
	if (stopped)
		ConditionVariableBroadcast(&walrcv->walRcvStoppedCV);

	/*
	 * Signal walreceiver process if it was still running.
	 *
	 * 若 walreceiver 进程仍在运行，则向它发信号。
	 */
	if (walrcvpid != 0)
		kill(walrcvpid, SIGTERM);

	/*
	 * Wait for walreceiver to acknowledge its death by setting state to
	 * WALRCV_STOPPED.
	 *
	 * 等待 walreceiver 把状态设为 WALRCV_STOPPED，以此确认自己已退出。
	 */
	ConditionVariablePrepareToSleep(&walrcv->walRcvStoppedCV);
	while (WalRcvRunning())
		ConditionVariableSleep(&walrcv->walRcvStoppedCV,
							   WAIT_EVENT_WAL_RECEIVER_EXIT);
	ConditionVariableCancelSleep();
}

/*
 * Request postmaster to start walreceiver.
 *
 * 请求 postmaster 启动 walreceiver。
 *
 * "recptr" indicates the position where streaming should begin.  "conninfo"
 * is a libpq connection string to use.  "slotname" is, optionally, the name
 * of a replication slot to acquire.  "create_temp_slot" indicates to create
 * a temporary slot when no "slotname" is given.
 *
 * recptr 是开始流式接收的位置。conninfo 是要使用的 libpq 连接串。
 * slotname 可选，是要获取的复制槽名。未给出 slotname 时，create_temp_slot 表示是否创建临时槽。
 *
 * WAL receivers do not directly load GUC parameters used for the connection
 * to the primary, and rely on the values passed down by the caller of this
 * routine instead.  Hence, the addition of any new parameters should happen
 * through this code path.
 *
 * walreceiver 不直接加载连接主库所用的 GUC，而是使用本函数调用者传下来的值。
 * 因此新增参数都应走这条路径。
 */
void
RequestXLogStreaming(TimeLineID tli, XLogRecPtr recptr, const char *conninfo,
					 const char *slotname, bool create_temp_slot)
{
	WalRcvData *walrcv = WalRcv;
	bool		launch = false;
	pg_time_t	now = (pg_time_t) time(NULL);
	ProcNumber	walrcv_proc;

	/*
	 * We always start at the beginning of the segment. That prevents a broken
	 * segment (i.e., with no records in the first half of a segment) from
	 * being created by XLOG streaming, which might cause trouble later on if
	 * the segment is e.g archived.
	 *
	 * 总是从段的开头开始。这样可避免 XLOG 流式复制造出前半段没有记录的坏段，
	 * 日后归档这类段时会出问题。
	 */
	if (XLogSegmentOffset(recptr, wal_segment_size) != 0)
		recptr -= XLogSegmentOffset(recptr, wal_segment_size);

	SpinLockAcquire(&walrcv->mutex);

	/* It better be stopped if we try to restart it
	 *
	 * 准备重启时，它应当已经处于停止状态。
	 */
	Assert(walrcv->walRcvState == WALRCV_STOPPED ||
		   walrcv->walRcvState == WALRCV_WAITING);

	if (conninfo != NULL)
		strlcpy(walrcv->conninfo, conninfo, MAXCONNINFO);
	else
		walrcv->conninfo[0] = '\0';

	/*
	 * Use configured replication slot if present, and ignore the value of
	 * create_temp_slot as the slot name should be persistent.  Otherwise, use
	 * create_temp_slot to determine whether this WAL receiver should create a
	 * temporary slot by itself and use it, or not.
	 *
	 * 若已配置复制槽就使用它，并忽略 create_temp_slot，因为槽名应当是持久的。
	 * 否则用 create_temp_slot 决定这个 walreceiver 是否自己创建并使用临时槽。
	 */
	if (slotname != NULL && slotname[0] != '\0')
	{
		strlcpy(walrcv->slotname, slotname, NAMEDATALEN);
		walrcv->is_temp_slot = false;
	}
	else
	{
		walrcv->slotname[0] = '\0';
		walrcv->is_temp_slot = create_temp_slot;
	}

	if (walrcv->walRcvState == WALRCV_STOPPED)
	{
		launch = true;
		walrcv->walRcvState = WALRCV_STARTING;
	}
	else
		walrcv->walRcvState = WALRCV_RESTARTING;
	walrcv->startTime = now;

	/*
	 * If this is the first startup of walreceiver (on this timeline),
	 * initialize flushedUpto and latestChunkStart to the starting point.
	 *
	 * 若这是本时间线上 walreceiver 的第一次启动，把 flushedUpto 和 latestChunkStart 初始化为起点。
	 */
	if (walrcv->receiveStart == 0 || walrcv->receivedTLI != tli)
	{
		walrcv->flushedUpto = recptr;
		walrcv->receivedTLI = tli;
		walrcv->latestChunkStart = recptr;
	}
	walrcv->receiveStart = recptr;
	walrcv->receiveStartTLI = tli;

	walrcv_proc = walrcv->procno;

	SpinLockRelease(&walrcv->mutex);

	if (launch)
		SendPostmasterSignal(PMSIGNAL_START_WALRECEIVER);
	else if (walrcv_proc != INVALID_PROC_NUMBER)
		SetLatch(&GetPGProcByNumber(walrcv_proc)->procLatch);
}

/*
 * Returns the last+1 byte position that walreceiver has flushed.
 *
 * 返回 walreceiver 已刷盘位置的下一字节。
 *
 * Optionally, returns the previous chunk start, that is the first byte
 * written in the most recent walreceiver flush cycle.  Callers not
 * interested in that value may pass NULL for latestChunkStart. Same for
 * receiveTLI.
 *
 * 可选地返回上一块的起点，即最近一轮 walreceiver 刷盘时写入的第一个字节。
 * 不需要该值时，latestChunkStart 可传 NULL。receiveTLI 同样如此。
 */
XLogRecPtr
GetWalRcvFlushRecPtr(XLogRecPtr *latestChunkStart, TimeLineID *receiveTLI)
{
	WalRcvData *walrcv = WalRcv;
	XLogRecPtr	recptr;

	SpinLockAcquire(&walrcv->mutex);
	recptr = walrcv->flushedUpto;
	if (latestChunkStart)
		*latestChunkStart = walrcv->latestChunkStart;
	if (receiveTLI)
		*receiveTLI = walrcv->receivedTLI;
	SpinLockRelease(&walrcv->mutex);

	return recptr;
}

/*
 * Returns the last+1 byte position that walreceiver has written.
 * This returns a recently written value without taking a lock.
 *
 * 返回 walreceiver 已写入位置的下一字节。这里不加锁，返回的是最近写入的值。
 */
XLogRecPtr
GetWalRcvWriteRecPtr(void)
{
	WalRcvData *walrcv = WalRcv;

	return pg_atomic_read_u64(&walrcv->writtenUpto);
}

/*
 * Returns the replication apply delay in ms or -1
 * if the apply delay info is not available
 *
 * 返回复制应用延迟，单位为毫秒；若没有该信息则返回 -1。
 */
int
GetReplicationApplyDelay(void)
{
	WalRcvData *walrcv = WalRcv;
	XLogRecPtr	receivePtr;
	XLogRecPtr	replayPtr;
	TimestampTz chunkReplayStartTime;

	SpinLockAcquire(&walrcv->mutex);
	receivePtr = walrcv->flushedUpto;
	SpinLockRelease(&walrcv->mutex);

	replayPtr = GetXLogReplayRecPtr(NULL);

	if (receivePtr == replayPtr)
		return 0;

	chunkReplayStartTime = GetCurrentChunkReplayStartTime();

	if (chunkReplayStartTime == 0)
		return -1;

	return TimestampDifferenceMilliseconds(chunkReplayStartTime,
										   GetCurrentTimestamp());
}

/*
 * Returns the network latency in ms, note that this includes any
 * difference in clock settings between the servers, as well as timezone.
 *
 * 返回网络延迟，单位为毫秒。其中包含两台服务器的时钟设定差和时区差。
 */
int
GetReplicationTransferLatency(void)
{
	WalRcvData *walrcv = WalRcv;
	TimestampTz lastMsgSendTime;
	TimestampTz lastMsgReceiptTime;

	SpinLockAcquire(&walrcv->mutex);
	lastMsgSendTime = walrcv->lastMsgSendTime;
	lastMsgReceiptTime = walrcv->lastMsgReceiptTime;
	SpinLockRelease(&walrcv->mutex);

	return TimestampDifferenceMilliseconds(lastMsgSendTime,
										   lastMsgReceiptTime);
}

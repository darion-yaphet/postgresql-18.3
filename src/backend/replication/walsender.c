/*-------------------------------------------------------------------------
 *
 * walsender.c
 *
 * The WAL sender process (walsender) is new as of Postgres 9.0. It takes
 * care of sending XLOG from the primary server to a single recipient.
 * (Note that there can be more than one walsender process concurrently.)
 * It is started by the postmaster when the walreceiver of a standby server
 * connects to the primary server and requests XLOG streaming replication.
 *
 * A walsender is similar to a regular backend, ie. there is a one-to-one
 * relationship between a connection and a walsender process, but instead
 * of processing SQL queries, it understands a small set of special
 * replication-mode commands. The START_REPLICATION command begins streaming
 * WAL to the client. While streaming, the walsender keeps reading XLOG
 * records from the disk and sends them to the standby server over the
 * COPY protocol, until either side ends the replication by exiting COPY
 * mode (or until the connection is closed).
 *
 * Normal termination is by SIGTERM, which instructs the walsender to
 * close the connection and exit(0) at the next convenient moment. Emergency
 * termination is by SIGQUIT; like any backend, the walsender will simply
 * abort and exit on SIGQUIT. A close of the connection and a FATAL error
 * are treated as not a crash but approximately normal termination;
 * the walsender will exit quickly without sending any more XLOG records.
 *
 * If the server is shut down, checkpointer sends us
 * PROCSIG_WALSND_INIT_STOPPING after all regular backends have exited.  If
 * the backend is idle or runs an SQL query this causes the backend to
 * shutdown, if logical replication is in progress all existing WAL records
 * are processed followed by a shutdown.  Otherwise this causes the walsender
 * to switch to the "stopping" state. In this state, the walsender will reject
 * any further replication commands. The checkpointer begins the shutdown
 * checkpoint once all walsenders are confirmed as stopping. When the shutdown
 * checkpoint finishes, the postmaster sends us SIGUSR2. This instructs
 * walsender to send any outstanding WAL, including the shutdown checkpoint
 * record, wait for it to be replicated to the standby, and then exit.
 *
 * WAL 发送进程（walsender）自 Postgres 9.0 起新增。它负责将 XLOG 从主服务器
 * 发送给单个接收方（可以同时存在多个 walsender 进程）。当备服务器的 walreceiver
 * 连接到主服务器并请求 XLOG 流式复制时，由 postmaster 启动 walsender。
 *
 * walsender 类似于普通后端进程，即连接与 walsender 进程一一对应，但它不处理
 * SQL 查询，而是理解一组特殊的复制模式命令。START_REPLICATION 命令开始向客户端
 * 流式传输 WAL。在流式传输期间，walsender 持续从磁盘读取 XLOG 记录，并通过
 * COPY 协议发送给备服务器，直到任意一方退出 COPY 模式（或连接关闭）为止。
 *
 * 正常终止通过 SIGTERM 信号触发，指示 walsender 在下一个合适时机关闭连接并
 * exit(0)。紧急终止通过 SIGQUIT 信号触发，与普通后端一样，walsender 会直接
 * 中止并退出。连接关闭和 FATAL 错误被视为近似正常终止而非崩溃，walsender
 * 会快速退出，不再发送更多 XLOG 记录。
 *
 * 当服务器关闭时，checkpointer 在所有普通后端退出后向我们发送
 * PROCSIG_WALSND_INIT_STOPPING。若后端处于空闲状态或正在执行 SQL 查询，
 * 则导致后端关闭；若逻辑复制正在进行，则处理所有现有 WAL 记录后再关闭。
 * 否则，walsender 切换到"stopping"状态，此后拒绝接受新的复制命令。
 * 所有 walsender 确认进入 stopping 状态后，checkpointer 开始执行关闭
 * 检查点。检查点完成后，postmaster 向我们发送 SIGUSR2，指示 walsender
 * 发送所有剩余 WAL（包括关闭检查点记录），等待其被复制到备服务器后退出。
 *
 *
 * Portions Copyright (c) 2010-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/walsender.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <unistd.h>

#include "access/timeline.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog_internal.h"
#include "access/xlogreader.h"
#include "access/xlogrecovery.h"
#include "access/xlogutils.h"
#include "backup/basebackup.h"
#include "backup/basebackup_incremental.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "funcapi.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "nodes/replnodes.h"
#include "pgstat.h"
#include "postmaster/interrupt.h"
#include "replication/decode.h"
#include "replication/logical.h"
#include "replication/slotsync.h"
#include "replication/slot.h"
#include "replication/snapbuild.h"
#include "replication/syncrep.h"
#include "replication/walreceiver.h"
#include "replication/walsender.h"
#include "replication/walsender_private.h"
#include "storage/condition_variable.h"
#include "storage/aio_subsys.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "tcop/dest.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/pg_lsn.h"
#include "utils/pgstat_internal.h"
#include "utils/ps_status.h"
#include "utils/timeout.h"
#include "utils/timestamp.h"

/* Minimum interval used by walsender for stats flushes, in ms */
/* walsender 刷新统计信息的最小时间间隔，单位为毫秒 */
#define WALSENDER_STATS_FLUSH_INTERVAL         1000

/*
 * Maximum data payload in a WAL data message.  Must be >= XLOG_BLCKSZ.
 *
 * We don't have a good idea of what a good value would be; there's some
 * overhead per message in both walsender and walreceiver, but on the other
 * hand sending large batches makes walsender less responsive to signals
 * because signals are checked only between messages.  128kB (with
 * default 8k blocks) seems like a reasonable guess for now.
 */
/*
 * WAL 数据消息中的最大数据负载，必须 >= XLOG_BLCKSZ。
 *
 * 我们尚不清楚最佳值是多少；walsender 和 walreceiver 每条消息都有一定开销，
 * 但另一方面，发送大批量数据会降低 walsender 对信号的响应速度，
 * 因为信号只在消息之间检查。128kB（默认 8k 块）目前看来是合理的估算值。
 */
#define MAX_SEND_SIZE (XLOG_BLCKSZ * 16)

/* Array of WalSnds in shared memory */
/* 共享内存中的 WalSnd 数组 */
WalSndCtlData *WalSndCtl = NULL;

/* My slot in the shared memory array */
/* 当前进程在共享内存数组中的槽位 */
WalSnd	   *MyWalSnd = NULL;

/* Global state */
bool		am_walsender = false;	/* Am I a walsender process? */
								/* 当前进程是否为 walsender 进程？ */
bool		am_cascading_walsender = false; /* Am I cascading WAL to another
										 * standby? */
										/* 是否正在将 WAL 级联转发给另一个备服务器？ */
bool		am_db_walsender = false;	/* Connected to a database? */
								/* 是否已连接到某个数据库？ */

/* GUC variables */
/* GUC 配置变量 */
int			max_wal_senders = 10;	/* the maximum number of concurrent
									 * walsenders */
								/* 最大并发 walsender 进程数 */
int			wal_sender_timeout = 60 * 1000; /* maximum time to send one WAL
										 * data message */
								/* 发送一条 WAL 数据消息的最大时间 */
bool		log_replication_commands = false;

/*
 * State for WalSndWakeupRequest
 */
/* WalSndWakeupRequest 的状态标志 */
bool		wake_wal_senders = false;

/*
 * xlogreader used for replication.  Note that a WAL sender doing physical
 * replication does not need xlogreader to read WAL, but it needs one to
 * keep a state of its work.
 */
/*
 * 用于复制的 xlogreader。注意，执行物理复制的 WAL 发送进程不需要 xlogreader
 * 来读取 WAL，但需要它来保存工作状态。
 */
static XLogReaderState *xlogreader = NULL;

/*
 * If the UPLOAD_MANIFEST command is used to provide a backup manifest in
 * preparation for an incremental backup, uploaded_manifest will be point
 * to an object containing information about its contexts, and
 * uploaded_manifest_mcxt will point to the memory context that contains
 * that object and all of its subordinate data. Otherwise, both values will
 * be NULL.
 */
/*
 * 若使用 UPLOAD_MANIFEST 命令为增量备份提供备份清单，uploaded_manifest
 * 将指向包含其上下文信息的对象，uploaded_manifest_mcxt 将指向包含该对象
 * 及其所有子数据的内存上下文。否则两者均为 NULL。
 */
static IncrementalBackupInfo *uploaded_manifest = NULL;
static MemoryContext uploaded_manifest_mcxt = NULL;

/*
 * These variables keep track of the state of the timeline we're currently
 * sending. sendTimeLine identifies the timeline. If sendTimeLineIsHistoric,
 * the timeline is not the latest timeline on this server, and the server's
 * history forked off from that timeline at sendTimeLineValidUpto.
 */
/*
 * 这些变量跟踪当前正在发送的时间线状态。sendTimeLine 标识时间线。
 * 若 sendTimeLineIsHistoric 为真，则该时间线不是本服务器的最新时间线，
 * 服务器历史在 sendTimeLineValidUpto 处从该时间线分叉。
 */
static TimeLineID sendTimeLine = 0;
static TimeLineID sendTimeLineNextTLI = 0;
static bool sendTimeLineIsHistoric = false;
static XLogRecPtr sendTimeLineValidUpto = InvalidXLogRecPtr;

/*
 * How far have we sent WAL already? This is also advertised in
 * MyWalSnd->sentPtr.  (Actually, this is the next WAL location to send.)
 */
/*
 * 已经发送了多少 WAL？这也在 MyWalSnd->sentPtr 中公示。
 * （实际上，这是下一个要发送的 WAL 位置。）
 */
static XLogRecPtr sentPtr = InvalidXLogRecPtr;

/* Buffers for constructing outgoing messages and processing reply messages. */
/* 用于构造发出消息和处理回复消息的缓冲区。 */
static StringInfoData output_message;
static StringInfoData reply_message;
static StringInfoData tmpbuf;

/* Timestamp of last ProcessRepliesIfAny(). */
/* 上次调用 ProcessRepliesIfAny() 的时间戳。 */
static TimestampTz last_processing = 0;

/*
 * Timestamp of last ProcessRepliesIfAny() that saw a reply from the
 * standby. Set to 0 if wal_sender_timeout doesn't need to be active.
 */
/*
 * 上次收到备服务器回复时 ProcessRepliesIfAny() 的时间戳。
 * 若不需要激活 wal_sender_timeout，则设为 0。
 */
static TimestampTz last_reply_timestamp = 0;

/* Have we sent a heartbeat message asking for reply, since last reply? */
/* 自上次收到回复后，是否已发送过请求回复的心跳消息？ */
static bool waiting_for_ping_response = false;

/*
 * While streaming WAL in Copy mode, streamingDoneSending is set to true
 * after we have sent CopyDone. We should not send any more CopyData messages
 * after that. streamingDoneReceiving is set to true when we receive CopyDone
 * from the other end. When both become true, it's time to exit Copy mode.
 */
/*
 * 在 Copy 模式下流式传输 WAL 时，发送 CopyDone 后 streamingDoneSending 置为 true，
 * 此后不应再发送任何 CopyData 消息。收到对端的 CopyDone 后
 * streamingDoneReceiving 置为 true。两者均为 true 时，退出 Copy 模式。
 */
static bool streamingDoneSending;
static bool streamingDoneReceiving;

/* Are we there yet? */
/* 是否已追上（追平主库位置）？ */
static bool WalSndCaughtUp = false;

/* Flags set by signal handlers for later service in main loop */
/* 由信号处理函数设置的标志，在主循环中延迟处理 */
static volatile sig_atomic_t got_SIGUSR2 = false;
static volatile sig_atomic_t got_STOPPING = false;

/*
 * This is set while we are streaming. When not set
 * PROCSIG_WALSND_INIT_STOPPING signal will be handled like SIGTERM. When set,
 * the main loop is responsible for checking got_STOPPING and terminating when
 * it's set (after streaming any remaining WAL).
 */
/*
 * 在流式传输期间此标志被设置。未设置时，PROCSIG_WALSND_INIT_STOPPING 信号
 * 的处理方式与 SIGTERM 相同。设置后，主循环负责检查 got_STOPPING，
 * 并在其被设置时（发送完所有剩余 WAL 后）终止进程。
 */
static volatile sig_atomic_t replication_active = false;

static LogicalDecodingContext *logical_decoding_ctx = NULL;

/* A sample associating a WAL location with the time it was written. */
/* 将 WAL 位置与其写入时间关联的采样数据结构。 */
typedef struct
{
	XLogRecPtr	lsn;
	TimestampTz time;
} WalTimeSample;

/* The size of our buffer of time samples. */
/* 时间采样缓冲区的大小。 */
#define LAG_TRACKER_BUFFER_SIZE 8192

/* A mechanism for tracking replication lag. */
/* 用于跟踪复制延迟的机制。 */
typedef struct
{
	XLogRecPtr	last_lsn;
	WalTimeSample buffer[LAG_TRACKER_BUFFER_SIZE];
	int			write_head;
	int			read_heads[NUM_SYNC_REP_WAIT_MODE];
	WalTimeSample last_read[NUM_SYNC_REP_WAIT_MODE];

	/*
	 * Overflow entries for read heads that collide with the write head.
	 *
	 * When the cyclic buffer fills (write head is about to collide with a
	 * read head), we save that read head's current sample here and mark it as
	 * using overflow (read_heads[i] = -1). This allows the write head to
	 * continue advancing while the overflowed mode continues lag computation
	 * using the saved sample.
	 *
	 * Once the standby's reported LSN advances past the overflow entry's LSN,
	 * we transition back to normal buffer-based tracking.
	 */
	/*
	 * 与写头发生碰撞的读头的溢出条目。
	 *
	 * 当循环缓冲区已满（写头即将与某个读头碰撞）时，将该读头当前的采样保存到此处，
	 * 并将其标记为使用溢出模式（read_heads[i] = -1）。这样写头可以继续前进，
	 * 而溢出的模式继续使用保存的采样进行延迟计算。
	 *
	 * 一旦备服务器上报的 LSN 超过溢出条目的 LSN，则切回基于缓冲区的正常跟踪方式。
	 */
	WalTimeSample overflowed[NUM_SYNC_REP_WAIT_MODE];
} LagTracker;

static LagTracker *lag_tracker;

/* Signal handlers */
static void WalSndLastCycleHandler(SIGNAL_ARGS);

/* Prototypes for private functions */
typedef void (*WalSndSendDataCallback) (void);
static void WalSndLoop(WalSndSendDataCallback send_data);
static void InitWalSenderSlot(void);
static void WalSndKill(int code, Datum arg);
pg_noreturn static void WalSndShutdown(void);
static void XLogSendPhysical(void);
static void XLogSendLogical(void);
static void WalSndDone(WalSndSendDataCallback send_data);
static void IdentifySystem(void);
static void UploadManifest(void);
static bool HandleUploadManifestPacket(StringInfo buf, off_t *offset,
									   IncrementalBackupInfo *ib);
static void ReadReplicationSlot(ReadReplicationSlotCmd *cmd);
static void CreateReplicationSlot(CreateReplicationSlotCmd *cmd);
static void DropReplicationSlot(DropReplicationSlotCmd *cmd);
static void StartReplication(StartReplicationCmd *cmd);
static void StartLogicalReplication(StartReplicationCmd *cmd);
static void ProcessStandbyMessage(void);
static void ProcessStandbyReplyMessage(void);
static void ProcessStandbyHSFeedbackMessage(void);
static void ProcessRepliesIfAny(void);
static void ProcessPendingWrites(void);
static void WalSndKeepalive(bool requestReply, XLogRecPtr writePtr);
static void WalSndKeepaliveIfNecessary(void);
static void WalSndCheckTimeOut(void);
static long WalSndComputeSleeptime(TimestampTz now);
static void WalSndWait(uint32 socket_events, long timeout, uint32 wait_event);
static void WalSndPrepareWrite(LogicalDecodingContext *ctx, XLogRecPtr lsn, TransactionId xid, bool last_write);
static void WalSndWriteData(LogicalDecodingContext *ctx, XLogRecPtr lsn, TransactionId xid, bool last_write);
static void WalSndUpdateProgress(LogicalDecodingContext *ctx, XLogRecPtr lsn, TransactionId xid,
								 bool skipped_xact);
static XLogRecPtr WalSndWaitForWal(XLogRecPtr loc);
static void LagTrackerWrite(XLogRecPtr lsn, TimestampTz local_flush_time);
static TimeOffset LagTrackerRead(int head, XLogRecPtr lsn, TimestampTz now);
static bool TransactionIdInRecentPast(TransactionId xid, uint32 epoch);

static void WalSndSegmentOpen(XLogReaderState *state, XLogSegNo nextSegNo,
							  TimeLineID *tli_p);


/* Initialize walsender process before entering the main command loop */
/* 在进入主命令循环之前初始化 walsender 进程 */
void
InitWalSender(void)
{
	am_cascading_walsender = RecoveryInProgress();

	/* Create a per-walsender data structure in shared memory */
	/* 在共享内存中创建每个 walsender 专属的数据结构 */
	InitWalSenderSlot();

	/* need resource owner for e.g. basebackups */
	/* 需要资源所有者，例如用于基础备份 */
	CreateAuxProcessResourceOwner();

	/*
	 * Let postmaster know that we're a WAL sender. Once we've declared us as
	 * a WAL sender process, postmaster will let us outlive the bgwriter and
	 * kill us last in the shutdown sequence, so we get a chance to stream all
	 * remaining WAL at shutdown, including the shutdown checkpoint. Note that
	 * there's no going back, and we mustn't write any WAL records after this.
	 */
	/*
	 * 通知 postmaster 我们是 WAL 发送进程。一旦声明为 WAL 发送进程，
	 * postmaster 会让我们比 bgwriter 存活更久，并在关闭序列中最后杀死我们，
	 * 从而有机会在关闭时发送所有剩余 WAL，包括关闭检查点。
	 * 注意此操作不可撤销，此后不得再写入任何 WAL 记录。
	 */
	MarkPostmasterChildWalSender();
	SendPostmasterSignal(PMSIGNAL_ADVANCE_STATE_MACHINE);

	/*
	 * If the client didn't specify a database to connect to, show in PGPROC
	 * that our advertised xmin should affect vacuum horizons in all
	 * databases.  This allows physical replication clients to send hot
	 * standby feedback that will delay vacuum cleanup in all databases.
	 */
	/*
	 * 若客户端未指定连接的数据库，则在 PGPROC 中标记我们公示的 xmin
	 * 应影响所有数据库的 vacuum 水位线。这允许物理复制客户端发送
	 * hot standby 反馈，以延迟所有数据库的 vacuum 清理。
	 */
	if (MyDatabaseId == InvalidOid)
	{
		Assert(MyProc->xmin == InvalidTransactionId);
		LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
		MyProc->statusFlags |= PROC_AFFECTS_ALL_HORIZONS;
		ProcGlobal->statusFlags[MyProc->pgxactoff] = MyProc->statusFlags;
		LWLockRelease(ProcArrayLock);
	}

	/* Initialize empty timestamp buffer for lag tracking. */
	/* 初始化用于延迟跟踪的空时间戳缓冲区。 */
	lag_tracker = MemoryContextAllocZero(TopMemoryContext, sizeof(LagTracker));
}

/*
 * Clean up after an error.
 *
 * WAL sender processes don't use transactions like regular backends do.
 * This function does any cleanup required after an error in a WAL sender
 * process, similar to what transaction abort does in a regular backend.
 */
/*
 * 错误发生后的清理工作。
 *
 * WAL 发送进程不像普通后端那样使用事务。此函数执行 WAL 发送进程在错误后
 * 所需的所有清理，类似于普通后端中事务回滚所做的工作。
 */
void
WalSndErrorCleanup(void)
{
	LWLockReleaseAll();
	ConditionVariableCancelSleep();
	pgstat_report_wait_end();
	pgaio_error_cleanup();

	if (xlogreader != NULL && xlogreader->seg.ws_file >= 0)
		wal_segment_close(xlogreader);

	if (MyReplicationSlot != NULL)
		ReplicationSlotRelease();

	ReplicationSlotCleanup(false);

	replication_active = false;

	/*
	 * If there is a transaction in progress, it will clean up our
	 * ResourceOwner, but if a replication command set up a resource owner
	 * without a transaction, we've got to clean that up now.
	 */
	/*
	 * 若当前有事务正在进行，事务会清理我们的 ResourceOwner；但如果某个
	 * 复制命令在没有事务的情况下创建了资源所有者，则需要在此处立即清理。
	 */
	if (!IsTransactionOrTransactionBlock())
		ReleaseAuxProcessResources(false);

	if (got_STOPPING || got_SIGUSR2)
		proc_exit(0);

	/* Revert back to startup state */
	/* 恢复到启动状态 */
	WalSndSetState(WALSNDSTATE_STARTUP);
}

/*
 * Handle a client's connection abort in an orderly manner.
 */
/*
 * 以有序方式处理客户端连接的中止。
 */
static void
WalSndShutdown(void)
{
	/*
	 * Reset whereToSendOutput to prevent ereport from attempting to send any
	 * more messages to the standby.
	 */
	/*
	 * 重置 whereToSendOutput，防止 ereport 尝试向备服务器发送更多消息。
	 */
	if (whereToSendOutput == DestRemote)
		whereToSendOutput = DestNone;

	proc_exit(0);
	abort();					/* keep the compiler quiet */
}

/*
 * Handle the IDENTIFY_SYSTEM command.
 */
/*
 * 处理 IDENTIFY_SYSTEM 命令。
 */
static void
IdentifySystem(void)
{
	char		sysid[32];
	char		xloc[MAXFNAMELEN];
	XLogRecPtr	logptr;
	char	   *dbname = NULL;
	DestReceiver *dest;
	TupOutputState *tstate;
	TupleDesc	tupdesc;
	Datum		values[4];
	bool		nulls[4] = {0};
	TimeLineID	currTLI;

	/*
	 * Reply with a result set with one row, four columns. First col is system
	 * ID, second is timeline ID, third is current xlog location and the
	 * fourth contains the database name if we are connected to one.
	 */
	/*
	 * 以包含一行四列的结果集作为回复。第一列为系统 ID，第二列为时间线 ID，
	 * 第三列为当前 xlog 位置，第四列为已连接的数据库名称（若有）。
	 */

	snprintf(sysid, sizeof(sysid), UINT64_FORMAT,
			 GetSystemIdentifier());

	am_cascading_walsender = RecoveryInProgress();
	if (am_cascading_walsender)
		logptr = GetStandbyFlushRecPtr(&currTLI);
	else
		logptr = GetFlushRecPtr(&currTLI);

	snprintf(xloc, sizeof(xloc), "%X/%X", LSN_FORMAT_ARGS(logptr));

	if (MyDatabaseId != InvalidOid)
	{
		MemoryContext cur = CurrentMemoryContext;

		/* syscache access needs a transaction env. */
		StartTransactionCommand();
		dbname = get_database_name(MyDatabaseId);
		/* copy dbname out of TX context */
		dbname = MemoryContextStrdup(cur, dbname);
		CommitTransactionCommand();
	}

	dest = CreateDestReceiver(DestRemoteSimple);

	/* need a tuple descriptor representing four columns */
	tupdesc = CreateTemplateTupleDesc(4);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 1, "systemid",
							  TEXTOID, -1, 0);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 2, "timeline",
							  INT8OID, -1, 0);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 3, "xlogpos",
							  TEXTOID, -1, 0);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 4, "dbname",
							  TEXTOID, -1, 0);

	/* prepare for projection of tuples */
	tstate = begin_tup_output_tupdesc(dest, tupdesc, &TTSOpsVirtual);

	/* column 1: system identifier */
	values[0] = CStringGetTextDatum(sysid);

	/* column 2: timeline */
	values[1] = Int64GetDatum(currTLI);

	/* column 3: wal location */
	values[2] = CStringGetTextDatum(xloc);

	/* column 4: database name, or NULL if none */
	if (dbname)
		values[3] = CStringGetTextDatum(dbname);
	else
		nulls[3] = true;

	/* send it to dest */
	do_tup_output(tstate, values, nulls);

	end_tup_output(tstate);
}

/* Handle READ_REPLICATION_SLOT command */
/* 处理 READ_REPLICATION_SLOT 命令 */
static void
ReadReplicationSlot(ReadReplicationSlotCmd *cmd)
{
#define READ_REPLICATION_SLOT_COLS 3
	ReplicationSlot *slot;
	DestReceiver *dest;
	TupOutputState *tstate;
	TupleDesc	tupdesc;
	Datum		values[READ_REPLICATION_SLOT_COLS] = {0};
	bool		nulls[READ_REPLICATION_SLOT_COLS];

	tupdesc = CreateTemplateTupleDesc(READ_REPLICATION_SLOT_COLS);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 1, "slot_type",
							  TEXTOID, -1, 0);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 2, "restart_lsn",
							  TEXTOID, -1, 0);
	/* TimeLineID is unsigned, so int4 is not wide enough. */
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 3, "restart_tli",
							  INT8OID, -1, 0);

	memset(nulls, true, READ_REPLICATION_SLOT_COLS * sizeof(bool));

	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	slot = SearchNamedReplicationSlot(cmd->slotname, false);
	if (slot == NULL || !slot->in_use)
	{
		LWLockRelease(ReplicationSlotControlLock);
	}
	else
	{
		ReplicationSlot slot_contents;
		int			i = 0;

		/* Copy slot contents while holding spinlock */
		SpinLockAcquire(&slot->mutex);
		slot_contents = *slot;
		SpinLockRelease(&slot->mutex);
		LWLockRelease(ReplicationSlotControlLock);

		if (OidIsValid(slot_contents.data.database))
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("cannot use %s with a logical replication slot",
						   "READ_REPLICATION_SLOT"));

		/* slot type */
		values[i] = CStringGetTextDatum("physical");
		nulls[i] = false;
		i++;

		/* start LSN */
		if (!XLogRecPtrIsInvalid(slot_contents.data.restart_lsn))
		{
			char		xloc[64];

			snprintf(xloc, sizeof(xloc), "%X/%X",
					 LSN_FORMAT_ARGS(slot_contents.data.restart_lsn));
			values[i] = CStringGetTextDatum(xloc);
			nulls[i] = false;
		}
		i++;

		/* timeline this WAL was produced on */
		if (!XLogRecPtrIsInvalid(slot_contents.data.restart_lsn))
		{
			TimeLineID	slots_position_timeline;
			TimeLineID	current_timeline;
			List	   *timeline_history = NIL;

		/*
		 * While in recovery, use as timeline the currently-replaying one
		 * to get the LSN position's history.
		 */
		/*
		 * 在恢复期间，使用当前正在重放的时间线来获取 LSN 位置的历史记录。
		 */
			if (RecoveryInProgress())
				(void) GetXLogReplayRecPtr(&current_timeline);
			else
				current_timeline = GetWALInsertionTimeLine();

			timeline_history = readTimeLineHistory(current_timeline);
			slots_position_timeline = tliOfPointInHistory(slot_contents.data.restart_lsn,
														  timeline_history);
			values[i] = Int64GetDatum((int64) slots_position_timeline);
			nulls[i] = false;
		}
		i++;

		Assert(i == READ_REPLICATION_SLOT_COLS);
	}

	dest = CreateDestReceiver(DestRemoteSimple);
	tstate = begin_tup_output_tupdesc(dest, tupdesc, &TTSOpsVirtual);
	do_tup_output(tstate, values, nulls);
	end_tup_output(tstate);
}


/*
 * Handle TIMELINE_HISTORY command.
 */
/*
 * 处理 TIMELINE_HISTORY 命令。
 */
static void
SendTimeLineHistory(TimeLineHistoryCmd *cmd)
{
	DestReceiver *dest;
	TupleDesc	tupdesc;
	StringInfoData buf;
	char		histfname[MAXFNAMELEN];
	char		path[MAXPGPATH];
	int			fd;
	off_t		histfilelen;
	off_t		bytesleft;
	Size		len;

	dest = CreateDestReceiver(DestRemoteSimple);

	/*
	 * Reply with a result set with one row, and two columns. The first col is
	 * the name of the history file, 2nd is the contents.
	 */
	/*
	 * 以包含一行两列的结果集作为回复。第一列为历史文件名，第二列为文件内容。
	 */
	tupdesc = CreateTemplateTupleDesc(2);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 1, "filename", TEXTOID, -1, 0);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 2, "content", TEXTOID, -1, 0);

	TLHistoryFileName(histfname, cmd->timeline);
	TLHistoryFilePath(path, cmd->timeline);

	/* Send a RowDescription message */
	dest->rStartup(dest, CMD_SELECT, tupdesc);

	/* Send a DataRow message */
	pq_beginmessage(&buf, PqMsg_DataRow);
	pq_sendint16(&buf, 2);		/* # of columns */
	len = strlen(histfname);
	pq_sendint32(&buf, len);	/* col1 len */
	pq_sendbytes(&buf, histfname, len);

	fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));

	/* Determine file length and send it to client */
	histfilelen = lseek(fd, 0, SEEK_END);
	if (histfilelen < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not seek to end of file \"%s\": %m", path)));
	if (lseek(fd, 0, SEEK_SET) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not seek to beginning of file \"%s\": %m", path)));

	pq_sendint32(&buf, histfilelen);	/* col2 len */

	bytesleft = histfilelen;
	while (bytesleft > 0)
	{
		PGAlignedBlock rbuf;
		int			nread;

		pgstat_report_wait_start(WAIT_EVENT_WALSENDER_TIMELINE_HISTORY_READ);
		nread = read(fd, rbuf.data, sizeof(rbuf));
		pgstat_report_wait_end();
		if (nread < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m",
							path)));
		else if (nread == 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("could not read file \"%s\": read %d of %zu",
							path, nread, (Size) bytesleft)));

		pq_sendbytes(&buf, rbuf.data, nread);
		bytesleft -= nread;
	}

	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", path)));

	pq_endmessage(&buf);
}

/*
 * Handle UPLOAD_MANIFEST command.
 */
/*
 * 处理 UPLOAD_MANIFEST 命令。
 */
static void
UploadManifest(void)
{
	MemoryContext mcxt;
	IncrementalBackupInfo *ib;
	off_t		offset = 0;
	StringInfoData buf;

	/*
	 * parsing the manifest will use the cryptohash stuff, which requires a
	 * resource owner
	 */
	/*
	 * 解析清单时会使用加密哈希功能，这需要一个资源所有者。
	 */
	Assert(AuxProcessResourceOwner != NULL);
	Assert(CurrentResourceOwner == AuxProcessResourceOwner ||
		   CurrentResourceOwner == NULL);
	CurrentResourceOwner = AuxProcessResourceOwner;

	/* Prepare to read manifest data into a temporary context. */
	/* 准备将清单数据读入临时内存上下文。 */
	mcxt = AllocSetContextCreate(CurrentMemoryContext,
								 "incremental backup information",
								 ALLOCSET_DEFAULT_SIZES);
	ib = CreateIncrementalBackupInfo(mcxt);

	/* Send a CopyInResponse message */
	/* 发送 CopyInResponse 消息 */
	pq_beginmessage(&buf, PqMsg_CopyInResponse);
	pq_sendbyte(&buf, 0);
	pq_sendint16(&buf, 0);
	pq_endmessage_reuse(&buf);
	pq_flush();

	/* Receive packets from client until done. */
	/* 持续从客户端接收数据包，直到完成。 */
	while (HandleUploadManifestPacket(&buf, &offset, ib))
		;

	/* Finish up manifest processing. */
	/* 完成清单处理。 */
	FinalizeIncrementalManifest(ib);

	/*
	 * Discard any old manifest information and arrange to preserve the new
	 * information we just got.
	 *
	 * We assume that MemoryContextDelete and MemoryContextSetParent won't
	 * fail, and thus we shouldn't end up bailing out of here in such a way as
	 * to leave dangling pointers.
	 */
	/*
	 * 丢弃所有旧的清单信息，并安排保留刚刚获取的新信息。
	 *
	 * 我们假设 MemoryContextDelete 和 MemoryContextSetParent 不会失败，
	 * 因此不应在此处以会留下悬空指针的方式退出。
	 */
	if (uploaded_manifest_mcxt != NULL)
		MemoryContextDelete(uploaded_manifest_mcxt);
	MemoryContextSetParent(mcxt, CacheMemoryContext);
	uploaded_manifest = ib;
	uploaded_manifest_mcxt = mcxt;

	/* clean up the resource owner we created */
	/* 清理我们创建的资源所有者 */
	ReleaseAuxProcessResources(true);
}

/*
 * Process one packet received during the handling of an UPLOAD_MANIFEST
 * operation.
 *
 * 'buf' is scratch space. This function expects it to be initialized, doesn't
 * care what the current contents are, and may override them with completely
 * new contents.
 *
 * The return value is true if the caller should continue processing
 * additional packets and false if the UPLOAD_MANIFEST operation is complete.
 */
/*
 * 处理 UPLOAD_MANIFEST 操作期间接收到的一个数据包。
 *
 * 'buf' 是暂存空间。本函数期望它已初始化，不关心当前内容，
 * 并可能用全新内容覆盖它。
 *
 * 若调用方应继续处理更多数据包则返回 true，若 UPLOAD_MANIFEST 操作已完成则返回 false。
 */
static bool
HandleUploadManifestPacket(StringInfo buf, off_t *offset,
						   IncrementalBackupInfo *ib)
{
	int			mtype;
	int			maxmsglen;

	HOLD_CANCEL_INTERRUPTS();

	pq_startmsgread();
	mtype = pq_getbyte();
	if (mtype == EOF)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("unexpected EOF on client connection with an open transaction")));

	switch (mtype)
	{
		case 'd':				/* CopyData */
			maxmsglen = PQ_LARGE_MESSAGE_LIMIT;
			break;
		case 'c':				/* CopyDone */
		case 'f':				/* CopyFail */
		case 'H':				/* Flush */
		case 'S':				/* Sync */
			maxmsglen = PQ_SMALL_MESSAGE_LIMIT;
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("unexpected message type 0x%02X during COPY from stdin",
							mtype)));
			maxmsglen = 0;		/* keep compiler quiet */
			break;
	}

	/* Now collect the message body */
	/* 现在收集消息体 */
	if (pq_getmessage(buf, maxmsglen))
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("unexpected EOF on client connection with an open transaction")));
	RESUME_CANCEL_INTERRUPTS();

	/* Process the message */
	/* 处理消息 */
	switch (mtype)
	{
		case 'd':				/* CopyData */
			AppendIncrementalManifestData(ib, buf->data, buf->len);
			return true;

		case 'c':				/* CopyDone */
			return false;

		case 'H':				/* Sync */
		case 'S':				/* Flush */
			/* Ignore these while in CopyOut mode as we do elsewhere. */
			/* 与其他地方一样，在 CopyOut 模式下忽略这些消息。 */
			return true;

		case 'f':
			ereport(ERROR,
					(errcode(ERRCODE_QUERY_CANCELED),
					 errmsg("COPY from stdin failed: %s",
							pq_getmsgstring(buf))));
	}

	/* Not reached. */
	/* 不应到达此处。 */
	Assert(false);
	return false;
}

/*
 * Handle START_REPLICATION command.
 *
 * At the moment, this never returns, but an ereport(ERROR) will take us back
 * to the main loop.
 */
/*
 * 处理 START_REPLICATION 命令。
 *
 * 目前此函数永不返回，但 ereport(ERROR) 会将我们带回主循环。
 */
static void
StartReplication(StartReplicationCmd *cmd)
{
	StringInfoData buf;
	XLogRecPtr	FlushPtr;
	TimeLineID	FlushTLI;

	/* create xlogreader for physical replication */
	/* 为物理复制创建 xlogreader */
	xlogreader =
		XLogReaderAllocate(wal_segment_size, NULL,
						   XL_ROUTINE(.segment_open = WalSndSegmentOpen,
									  .segment_close = wal_segment_close),
						   NULL);

	if (!xlogreader)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed while allocating a WAL reading processor.")));

	/*
	 * We assume here that we're logging enough information in the WAL for
	 * log-shipping, since this is checked in PostmasterMain().
	 *
	 * NOTE: wal_level can only change at shutdown, so in most cases it is
	 * difficult for there to be WAL data that we can still see that was
	 * written at wal_level='minimal'.
	 */
	/*
	 * 此处我们假设 WAL 中记录了足够的日志传送信息，这在 PostmasterMain() 中
	 * 已经检查过。
	 *
	 * 注意：wal_level 只能在关闭时更改，因此在大多数情况下，我们能看到的
	 * WAL 数据不太可能是在 wal_level='minimal' 下写入的。
	 */

	if (cmd->slotname)
	{
		ReplicationSlotAcquire(cmd->slotname, true, true);
		if (SlotIsLogical(MyReplicationSlot))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot use a logical replication slot for physical replication")));

		/*
		 * We don't need to verify the slot's restart_lsn here; instead we
		 * rely on the caller requesting the starting point to use.  If the
		 * WAL segment doesn't exist, we'll fail later.
		 */
		/*
		 * 此处无需验证槽位的 restart_lsn；我们依赖调用方请求的起始点。
		 * 若 WAL 段文件不存在，稍后会报错。
		 */
	}

	/*
	 * Select the timeline. If it was given explicitly by the client, use
	 * that. Otherwise use the timeline of the last replayed record.
	 */
	/*
	 * 选择时间线。若客户端明确指定，则使用指定的时间线；
	 * 否则使用最后回放记录所在的时间线。
	 */
	am_cascading_walsender = RecoveryInProgress();
	if (am_cascading_walsender)
		FlushPtr = GetStandbyFlushRecPtr(&FlushTLI);
	else
		FlushPtr = GetFlushRecPtr(&FlushTLI);

	if (cmd->timeline != 0)
	{
		XLogRecPtr	switchpoint;

		sendTimeLine = cmd->timeline;
		if (sendTimeLine == FlushTLI)
		{
			sendTimeLineIsHistoric = false;
			sendTimeLineValidUpto = InvalidXLogRecPtr;
		}
		else
		{
			List	   *timeLineHistory;

			sendTimeLineIsHistoric = true;

		/*
		 * Check that the timeline the client requested exists, and the
		 * requested start location is on that timeline.
		 */
		/*
		 * 检查客户端请求的时间线是否存在，以及请求的起始位置是否在该时间线上。
		 */
			timeLineHistory = readTimeLineHistory(FlushTLI);
			switchpoint = tliSwitchPoint(cmd->timeline, timeLineHistory,
										 &sendTimeLineNextTLI);
			list_free_deep(timeLineHistory);

		/*
		 * Found the requested timeline in the history. Check that
		 * requested startpoint is on that timeline in our history.
		 *
		 * This is quite loose on purpose. We only check that we didn't
		 * fork off the requested timeline before the switchpoint. We
		 * don't check that we switched *to* it before the requested
		 * starting point. This is because the client can legitimately
		 * request to start replication from the beginning of the WAL
		 * segment that contains switchpoint, but on the new timeline, so
		 * that it doesn't end up with a partial segment. If you ask for
		 * too old a starting point, you'll get an error later when we
		 * fail to find the requested WAL segment in pg_wal.
		 *
		 * XXX: we could be more strict here and only allow a startpoint
		 * that's older than the switchpoint, if it's still in the same
		 * WAL segment.
		 */
		/*
		 * 在历史记录中找到了请求的时间线。检查请求的起始点是否位于我们
		 * 历史记录中的该时间线上。
		 *
		 * 此检查故意较宽松。我们只检查我们是否在切换点之前从请求的时间线分叉，
		 * 不检查我们是否在请求起始点之前切换*到*该时间线。这是因为客户端可以
		 * 合法地请求从包含切换点的 WAL 段开头（但在新时间线上）开始复制，
		 * 以避免得到残缺段。若起始点请求过旧，稍后在 pg_wal 中找不到所请求的
		 * WAL 段时会报错。
		 *
		 * XXX：若切换点仍在同一 WAL 段中，我们可以在此处更严格，只允许
		 * 早于切换点的起始点。
		 */
			if (!XLogRecPtrIsInvalid(switchpoint) &&
				switchpoint < cmd->startpoint)
			{
				ereport(ERROR,
						(errmsg("requested starting point %X/%X on timeline %u is not in this server's history",
								LSN_FORMAT_ARGS(cmd->startpoint),
								cmd->timeline),
						 errdetail("This server's history forked from timeline %u at %X/%X.",
								   cmd->timeline,
								   LSN_FORMAT_ARGS(switchpoint))));
			}
			sendTimeLineValidUpto = switchpoint;
		}
	}
	else
	{
		sendTimeLine = FlushTLI;
		sendTimeLineValidUpto = InvalidXLogRecPtr;
		sendTimeLineIsHistoric = false;
	}

	streamingDoneSending = streamingDoneReceiving = false;

	/* If there is nothing to stream, don't even enter COPY mode */
	/* 若没有需要流式传输的内容，甚至不需要进入 COPY 模式 */
	if (!sendTimeLineIsHistoric || cmd->startpoint < sendTimeLineValidUpto)
	{
		/*
		 * When we first start replication the standby will be behind the
		 * primary. For some applications, for example synchronous
		 * replication, it is important to have a clear state for this initial
		 * catchup mode, so we can trigger actions when we change streaming
		 * state later. We may stay in this state for a long time, which is
		 * exactly why we want to be able to monitor whether or not we are
		 * still here.
		 */
		/*
		 * 刚开始复制时，备服务器会落后于主服务器。对于某些应用（如同步复制），
		 * 初始追赶模式需要有清晰的状态，以便后续切换流式状态时可以触发相应动作。
		 * 我们可能在此状态停留较长时间，这正是我们需要能够监控是否仍处于此处的原因。
		 */
		WalSndSetState(WALSNDSTATE_CATCHUP);

		/* Send a CopyBothResponse message, and start streaming */
		/* 发送 CopyBothResponse 消息，并开始流式传输 */
		pq_beginmessage(&buf, PqMsg_CopyBothResponse);
		pq_sendbyte(&buf, 0);
		pq_sendint16(&buf, 0);
		pq_endmessage(&buf);
		pq_flush();

		/*
		 * Don't allow a request to stream from a future point in WAL that
		 * hasn't been flushed to disk in this server yet.
		 */
		/*
		 * 不允许从尚未刷新到本服务器磁盘的 WAL 未来位置开始流式传输。
		 */
		if (FlushPtr < cmd->startpoint)
		{
			ereport(ERROR,
					(errmsg("requested starting point %X/%X is ahead of the WAL flush position of this server %X/%X",
							LSN_FORMAT_ARGS(cmd->startpoint),
							LSN_FORMAT_ARGS(FlushPtr))));
		}

		/* Start streaming from the requested point */
		/* 从请求的位置开始流式传输 */
		sentPtr = cmd->startpoint;

		/* Initialize shared memory status, too */
		/* 同时初始化共享内存中的状态 */
		SpinLockAcquire(&MyWalSnd->mutex);
		MyWalSnd->sentPtr = sentPtr;
		SpinLockRelease(&MyWalSnd->mutex);

		SyncRepInitConfig();

		/* Main loop of walsender */
		/* walsender 主循环 */
		replication_active = true;

		WalSndLoop(XLogSendPhysical);

		replication_active = false;
		if (got_STOPPING)
			proc_exit(0);
		WalSndSetState(WALSNDSTATE_STARTUP);

		Assert(streamingDoneSending && streamingDoneReceiving);
	}

	if (cmd->slotname)
		ReplicationSlotRelease();

	/*
	 * Copy is finished now. Send a single-row result set indicating the next
	 * timeline.
	 */
	/*
	 * Copy 已完成。发送包含单行的结果集，指示下一个时间线。
	 */
	if (sendTimeLineIsHistoric)
	{
		char		startpos_str[8 + 1 + 8 + 1];
		DestReceiver *dest;
		TupOutputState *tstate;
		TupleDesc	tupdesc;
		Datum		values[2];
		bool		nulls[2] = {0};

		snprintf(startpos_str, sizeof(startpos_str), "%X/%X",
				 LSN_FORMAT_ARGS(sendTimeLineValidUpto));

		dest = CreateDestReceiver(DestRemoteSimple);

		/*
		 * Need a tuple descriptor representing two columns. int8 may seem
		 * like a surprising data type for this, but in theory int4 would not
		 * be wide enough for this, as TimeLineID is unsigned.
		 */
		/*
		 * 需要一个表示两列的元组描述符。int8 作为此处的数据类型可能出乎意料，
		 * 但理论上 int4 不够宽，因为 TimeLineID 是无符号类型。
		 */
		tupdesc = CreateTemplateTupleDesc(2);
		TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 1, "next_tli",
								  INT8OID, -1, 0);
		TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 2, "next_tli_startpos",
								  TEXTOID, -1, 0);

		/* prepare for projection of tuple */
		tstate = begin_tup_output_tupdesc(dest, tupdesc, &TTSOpsVirtual);

		values[0] = Int64GetDatum((int64) sendTimeLineNextTLI);
		values[1] = CStringGetTextDatum(startpos_str);

		/* send it to dest */
		do_tup_output(tstate, values, nulls);

		end_tup_output(tstate);
	}

	/* Send CommandComplete message */
	/* 发送 CommandComplete 消息 */
	EndReplicationCommand("START_STREAMING");
}

/*
 * XLogReaderRoutine->page_read callback for logical decoding contexts, as a
 * walsender process.
 *
 * Inside the walsender we can do better than read_local_xlog_page,
 * which has to do a plain sleep/busy loop, because the walsender's latch gets
 * set every time WAL is flushed.
 */
/*
 * 作为 walsender 进程的逻辑解码上下文的 XLogReaderRoutine->page_read 回调。
 *
 * 在 walsender 内部，我们可以比 read_local_xlog_page 做得更好——后者必须做
 * 简单的 sleep/忙循环，而 walsender 的 latch 在每次 WAL 刷新时都会被设置。
 */
static int
logical_read_xlog_page(XLogReaderState *state, XLogRecPtr targetPagePtr, int reqLen,
					   XLogRecPtr targetRecPtr, char *cur_page)
{
	XLogRecPtr	flushptr;
	int			count;
	WALReadError errinfo;
	XLogSegNo	segno;
	TimeLineID	currTLI;

	/*
	 * Make sure we have enough WAL available before retrieving the current
	 * timeline.
	 */
	/*
	 * 在获取当前时间线之前，确保有足够的 WAL 可用。
	 */
	flushptr = WalSndWaitForWal(targetPagePtr + reqLen);

	/* Fail if not enough (implies we are going to shut down) */
	/* 若不足则失败（意味着我们即将关闭） */
	if (flushptr < targetPagePtr + reqLen)
		return -1;

	/*
	 * Since logical decoding is also permitted on a standby server, we need
	 * to check if the server is in recovery to decide how to get the current
	 * timeline ID (so that it also covers the promotion or timeline change
	 * cases). We must determine am_cascading_walsender after waiting for the
	 * required WAL so that it is correct when the walsender wakes up after a
	 * promotion.
	 */
	/*
	 * 由于备服务器也允许逻辑解码，我们需要检查服务器是否处于恢复状态，
	 * 以决定如何获取当前时间线 ID（从而涵盖提升或时间线变更的情况）。
	 * 必须在等待所需 WAL 之后确定 am_cascading_walsender，以确保 walsender
	 * 在提升后唤醒时该值是正确的。
	 */
	am_cascading_walsender = RecoveryInProgress();

	if (am_cascading_walsender)
		GetXLogReplayRecPtr(&currTLI);
	else
		currTLI = GetWALInsertionTimeLine();

	XLogReadDetermineTimeline(state, targetPagePtr, reqLen, currTLI);
	sendTimeLineIsHistoric = (state->currTLI != currTLI);
	sendTimeLine = state->currTLI;
	sendTimeLineValidUpto = state->currTLIValidUntil;
	sendTimeLineNextTLI = state->nextTLI;

	if (targetPagePtr + XLOG_BLCKSZ <= flushptr)
		count = XLOG_BLCKSZ;	/* more than one block available */
	else
		count = flushptr - targetPagePtr;	/* part of the page available */

	/* now actually read the data, we know it's there */
	if (!WALRead(state,
				 cur_page,
				 targetPagePtr,
				 count,
				 currTLI,		/* Pass the current TLI because only
								 * WalSndSegmentOpen controls whether new TLI
								 * is needed. */
				 &errinfo))
		WALReadRaiseError(&errinfo);

	/*
	 * After reading into the buffer, check that what we read was valid. We do
	 * this after reading, because even though the segment was present when we
	 * opened it, it might get recycled or removed while we read it. The
	 * read() succeeds in that case, but the data we tried to read might
	 * already have been overwritten with new WAL records.
	 */
	/*
	 * 读入缓冲区后，检查读取的内容是否有效。之所以在读取后才检查，
	 * 是因为即使我们打开时段文件存在，读取期间也可能被回收或删除。
	 * 在这种情况下 read() 会成功，但我们尝试读取的数据可能已被新的
	 * WAL 记录覆盖。
	 */
	XLByteToSeg(targetPagePtr, segno, state->segcxt.ws_segsize);
	CheckXLogRemoved(segno, state->seg.ws_tli);

	return count;
}

/*
 * Process extra options given to CREATE_REPLICATION_SLOT.
 */
/*
 * 处理 CREATE_REPLICATION_SLOT 命令的额外选项。
 */
static void
parseCreateReplSlotOptions(CreateReplicationSlotCmd *cmd,
						   bool *reserve_wal,
						   CRSSnapshotAction *snapshot_action,
						   bool *two_phase, bool *failover)
{
	ListCell   *lc;
	bool		snapshot_action_given = false;
	bool		reserve_wal_given = false;
	bool		two_phase_given = false;
	bool		failover_given = false;

	/* Parse options */
	foreach(lc, cmd->options)
	{
		DefElem    *defel = (DefElem *) lfirst(lc);

		if (strcmp(defel->defname, "snapshot") == 0)
		{
			char	   *action;

			if (snapshot_action_given || cmd->kind != REPLICATION_KIND_LOGICAL)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));

			action = defGetString(defel);
			snapshot_action_given = true;

			if (strcmp(action, "export") == 0)
				*snapshot_action = CRS_EXPORT_SNAPSHOT;
			else if (strcmp(action, "nothing") == 0)
				*snapshot_action = CRS_NOEXPORT_SNAPSHOT;
			else if (strcmp(action, "use") == 0)
				*snapshot_action = CRS_USE_SNAPSHOT;
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("unrecognized value for %s option \"%s\": \"%s\"",
								"CREATE_REPLICATION_SLOT", defel->defname, action)));
		}
		else if (strcmp(defel->defname, "reserve_wal") == 0)
		{
			if (reserve_wal_given || cmd->kind != REPLICATION_KIND_PHYSICAL)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));

			reserve_wal_given = true;
			*reserve_wal = defGetBoolean(defel);
		}
		else if (strcmp(defel->defname, "two_phase") == 0)
		{
			if (two_phase_given || cmd->kind != REPLICATION_KIND_LOGICAL)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			two_phase_given = true;
			*two_phase = defGetBoolean(defel);
		}
		else if (strcmp(defel->defname, "failover") == 0)
		{
			if (failover_given || cmd->kind != REPLICATION_KIND_LOGICAL)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			failover_given = true;
			*failover = defGetBoolean(defel);
		}
		else
			elog(ERROR, "unrecognized option: %s", defel->defname);
	}
}

/*
 * Create a new replication slot.
 */
/*
 * 创建新的复制槽。
 */
static void
CreateReplicationSlot(CreateReplicationSlotCmd *cmd)
{
	const char *snapshot_name = NULL;
	char		xloc[MAXFNAMELEN];
	char	   *slot_name;
	bool		reserve_wal = false;
	bool		two_phase = false;
	bool		failover = false;
	CRSSnapshotAction snapshot_action = CRS_EXPORT_SNAPSHOT;
	DestReceiver *dest;
	TupOutputState *tstate;
	TupleDesc	tupdesc;
	Datum		values[4];
	bool		nulls[4] = {0};

	Assert(!MyReplicationSlot);

	parseCreateReplSlotOptions(cmd, &reserve_wal, &snapshot_action, &two_phase,
							   &failover);

	if (cmd->kind == REPLICATION_KIND_PHYSICAL)
	{
		ReplicationSlotCreate(cmd->slotname, false,
							  cmd->temporary ? RS_TEMPORARY : RS_PERSISTENT,
							  false, false, false);

		if (reserve_wal)
		{
			ReplicationSlotReserveWal();

			ReplicationSlotMarkDirty();

			/* Write this slot to disk if it's a permanent one. */
			if (!cmd->temporary)
				ReplicationSlotSave();
		}
	}
	else
	{
		LogicalDecodingContext *ctx;
		bool		need_full_snapshot = false;

		Assert(cmd->kind == REPLICATION_KIND_LOGICAL);

		CheckLogicalDecodingRequirements();

		/*
		 * Initially create persistent slot as ephemeral - that allows us to
		 * nicely handle errors during initialization because it'll get
		 * dropped if this transaction fails. We'll make it persistent at the
		 * end. Temporary slots can be created as temporary from beginning as
		 * they get dropped on error as well.
		 */
		/*
		 * 最初将持久槽创建为临时槽——这样在初始化期间可以优雅地处理错误，
		 * 因为如果事务失败，槽会被自动删除。最后再将其转为持久槽。
		 * 临时槽从一开始就以临时方式创建，出错时同样会被删除。
		 */
		ReplicationSlotCreate(cmd->slotname, true,
							  cmd->temporary ? RS_TEMPORARY : RS_EPHEMERAL,
							  two_phase, failover, false);

		/*
		 * Do options check early so that we can bail before calling the
		 * DecodingContextFindStartpoint which can take long time.
		 */
		/*
		 * 提前进行选项检查，以便在调用可能耗时较长的
		 * DecodingContextFindStartpoint 之前就能提前退出。
		 */
		if (snapshot_action == CRS_EXPORT_SNAPSHOT)
		{
			if (IsTransactionBlock())
				ereport(ERROR,
				/*- translator: %s is a CREATE_REPLICATION_SLOT statement */
						(errmsg("%s must not be called inside a transaction",
								"CREATE_REPLICATION_SLOT ... (SNAPSHOT 'export')")));

			need_full_snapshot = true;
		}
		else if (snapshot_action == CRS_USE_SNAPSHOT)
		{
			if (!IsTransactionBlock())
				ereport(ERROR,
				/*- translator: %s is a CREATE_REPLICATION_SLOT statement */
						(errmsg("%s must be called inside a transaction",
								"CREATE_REPLICATION_SLOT ... (SNAPSHOT 'use')")));

			if (XactIsoLevel != XACT_REPEATABLE_READ)
				ereport(ERROR,
				/*- translator: %s is a CREATE_REPLICATION_SLOT statement */
						(errmsg("%s must be called in REPEATABLE READ isolation mode transaction",
								"CREATE_REPLICATION_SLOT ... (SNAPSHOT 'use')")));
			if (!XactReadOnly)
				ereport(ERROR,
				/*- translator: %s is a CREATE_REPLICATION_SLOT statement */
						(errmsg("%s must be called in a read-only transaction",
								"CREATE_REPLICATION_SLOT ... (SNAPSHOT 'use')")));

			if (FirstSnapshotSet)
				ereport(ERROR,
				/*- translator: %s is a CREATE_REPLICATION_SLOT statement */
						(errmsg("%s must be called before any query",
								"CREATE_REPLICATION_SLOT ... (SNAPSHOT 'use')")));

			if (IsSubTransaction())
				ereport(ERROR,
				/*- translator: %s is a CREATE_REPLICATION_SLOT statement */
						(errmsg("%s must not be called in a subtransaction",
								"CREATE_REPLICATION_SLOT ... (SNAPSHOT 'use')")));

			need_full_snapshot = true;
		}

		ctx = CreateInitDecodingContext(cmd->plugin, NIL, need_full_snapshot,
										InvalidXLogRecPtr,
										XL_ROUTINE(.page_read = logical_read_xlog_page,
												   .segment_open = WalSndSegmentOpen,
												   .segment_close = wal_segment_close),
										WalSndPrepareWrite, WalSndWriteData,
										WalSndUpdateProgress);

		/*
		 * Signal that we don't need the timeout mechanism. We're just
		 * creating the replication slot and don't yet accept feedback
		 * messages or send keepalives. As we possibly need to wait for
		 * further WAL the walsender would otherwise possibly be killed too
		 * soon.
		 */
		/*
		 * 表明我们不需要超时机制。此时只是在创建复制槽，尚未接受反馈消息
		 * 或发送 keepalive。由于可能需要等待更多 WAL，否则 walsender
		 * 可能会过早被终止。
		 */
		last_reply_timestamp = 0;

		/* build initial snapshot, might take a while */
		/* 构建初始快照，可能需要一些时间 */
		DecodingContextFindStartpoint(ctx);

		/*
		 * Export or use the snapshot if we've been asked to do so.
		 *
		 * NB. We will convert the snapbuild.c kind of snapshot to normal
		 * snapshot when doing this.
		 */
		/*
		 * 如果被要求，导出或使用快照。
		 *
		 * 注意：执行此操作时，我们将把 snapbuild.c 类型的快照转换为普通快照。
		 */
		if (snapshot_action == CRS_EXPORT_SNAPSHOT)
		{
			snapshot_name = SnapBuildExportSnapshot(ctx->snapshot_builder);
		}
		else if (snapshot_action == CRS_USE_SNAPSHOT)
		{
			Snapshot	snap;

			snap = SnapBuildInitialSnapshot(ctx->snapshot_builder);
			RestoreTransactionSnapshot(snap, MyProc);
		}

		/* don't need the decoding context anymore */
		/* 不再需要解码上下文 */
		FreeDecodingContext(ctx);

		if (!cmd->temporary)
			ReplicationSlotPersist();
	}

	snprintf(xloc, sizeof(xloc), "%X/%X",
			 LSN_FORMAT_ARGS(MyReplicationSlot->data.confirmed_flush));

	dest = CreateDestReceiver(DestRemoteSimple);

	/*----------
	 * Need a tuple descriptor representing four columns:
	 * - first field: the slot name
	 * - second field: LSN at which we became consistent
	 * - third field: exported snapshot's name
	 * - fourth field: output plugin
	 */
	/*----------
	 * 需要一个表示四列的元组描述符：
	 * - 第一列：槽名称
	 * - 第二列：达到一致性时的 LSN
	 * - 第三列：导出的快照名称
	 * - 第四列：输出插件
	 */
	tupdesc = CreateTemplateTupleDesc(4);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 1, "slot_name",
							  TEXTOID, -1, 0);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 2, "consistent_point",
							  TEXTOID, -1, 0);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 3, "snapshot_name",
							  TEXTOID, -1, 0);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 4, "output_plugin",
							  TEXTOID, -1, 0);

	/* prepare for projection of tuples */
	tstate = begin_tup_output_tupdesc(dest, tupdesc, &TTSOpsVirtual);

	/* slot_name */
	slot_name = NameStr(MyReplicationSlot->data.name);
	values[0] = CStringGetTextDatum(slot_name);

	/* consistent wal location */
	values[1] = CStringGetTextDatum(xloc);

	/* snapshot name, or NULL if none */
	if (snapshot_name != NULL)
		values[2] = CStringGetTextDatum(snapshot_name);
	else
		nulls[2] = true;

	/* plugin, or NULL if none */
	if (cmd->plugin != NULL)
		values[3] = CStringGetTextDatum(cmd->plugin);
	else
		nulls[3] = true;

	/* send it to dest */
	do_tup_output(tstate, values, nulls);
	end_tup_output(tstate);

	ReplicationSlotRelease();
}

/*
 * Get rid of a replication slot that is no longer wanted.
 */
/*
 * 删除不再需要的复制槽。
 */
static void
DropReplicationSlot(DropReplicationSlotCmd *cmd)
{
	ReplicationSlotDrop(cmd->slotname, !cmd->wait);
}

/*
 * Change the definition of a replication slot.
 */
/*
 * 修改复制槽的定义。
 */
static void
AlterReplicationSlot(AlterReplicationSlotCmd *cmd)
{
	bool		failover_given = false;
	bool		two_phase_given = false;
	bool		failover;
	bool		two_phase;

	/* Parse options */
	foreach_ptr(DefElem, defel, cmd->options)
	{
		if (strcmp(defel->defname, "failover") == 0)
		{
			if (failover_given)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			failover_given = true;
			failover = defGetBoolean(defel);
		}
		else if (strcmp(defel->defname, "two_phase") == 0)
		{
			if (two_phase_given)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			two_phase_given = true;
			two_phase = defGetBoolean(defel);
		}
		else
			elog(ERROR, "unrecognized option: %s", defel->defname);
	}

	ReplicationSlotAlter(cmd->slotname,
						 failover_given ? &failover : NULL,
						 two_phase_given ? &two_phase : NULL);
}

/*
 * Load previously initiated logical slot and prepare for sending data (via
 * WalSndLoop).
 */
/*
 * 加载之前初始化的逻辑槽，并准备通过 WalSndLoop 发送数据。
 */
static void
StartLogicalReplication(StartReplicationCmd *cmd)
{
	StringInfoData buf;
	QueryCompletion qc;

	/* make sure that our requirements are still fulfilled */
	CheckLogicalDecodingRequirements();

	Assert(!MyReplicationSlot);

	ReplicationSlotAcquire(cmd->slotname, true, true);

	/*
	 * Force a disconnect, so that the decoding code doesn't need to care
	 * about an eventual switch from running in recovery, to running in a
	 * normal environment. Client code is expected to handle reconnects.
	 */
	/*
	 * 强制断开连接，使解码代码无需关心从恢复模式切换到正常环境的过程。
	 * 客户端代码应能处理重连。
	 */
	if (am_cascading_walsender && !RecoveryInProgress())
	{
		ereport(LOG,
				(errmsg("terminating walsender process after promotion")));
		got_STOPPING = true;
	}

	/*
	 * Create our decoding context, making it start at the previously ack'ed
	 * position.
	 *
	 * Do this before sending a CopyBothResponse message, so that any errors
	 * are reported early.
	 */
	/*
	 * 创建解码上下文，从之前已确认的位置开始。
	 *
	 * 在发送 CopyBothResponse 消息之前执行此操作，以便尽早报告任何错误。
	 */
	logical_decoding_ctx =
		CreateDecodingContext(cmd->startpoint, cmd->options, false,
							  XL_ROUTINE(.page_read = logical_read_xlog_page,
										 .segment_open = WalSndSegmentOpen,
										 .segment_close = wal_segment_close),
							  WalSndPrepareWrite, WalSndWriteData,
							  WalSndUpdateProgress);
	xlogreader = logical_decoding_ctx->reader;

	WalSndSetState(WALSNDSTATE_CATCHUP);

	/* Send a CopyBothResponse message, and start streaming */
	/* 发送 CopyBothResponse 消息，并开始流式传输 */
	pq_beginmessage(&buf, PqMsg_CopyBothResponse);
	pq_sendbyte(&buf, 0);
	pq_sendint16(&buf, 0);
	pq_endmessage(&buf);
	pq_flush();

	/* Start reading WAL from the oldest required WAL. */
	/* 从最旧的所需 WAL 位置开始读取。 */
	XLogBeginRead(logical_decoding_ctx->reader,
				  MyReplicationSlot->data.restart_lsn);

	/*
	 * Report the location after which we'll send out further commits as the
	 * current sentPtr.
	 */
	/*
	 * 将我们将继续发送提交的位置报告为当前的 sentPtr。
	 */
	sentPtr = MyReplicationSlot->data.confirmed_flush;

	/* Also update the sent position status in shared memory */
	/* 同时更新共享内存中的已发送位置状态 */
	SpinLockAcquire(&MyWalSnd->mutex);
	MyWalSnd->sentPtr = MyReplicationSlot->data.restart_lsn;
	SpinLockRelease(&MyWalSnd->mutex);

	replication_active = true;

	SyncRepInitConfig();

	/* Main loop of walsender */
	/* walsender 主循环 */
	WalSndLoop(XLogSendLogical);

	FreeDecodingContext(logical_decoding_ctx);
	ReplicationSlotRelease();

	replication_active = false;
	if (got_STOPPING)
		proc_exit(0);
	WalSndSetState(WALSNDSTATE_STARTUP);

	/* Get out of COPY mode (CommandComplete). */
	/* 退出 COPY 模式（发送 CommandComplete）。 */
	SetQueryCompletion(&qc, CMDTAG_COPY, 0);
	EndCommand(&qc, DestRemote, false);
}

/*
 * LogicalDecodingContext 'prepare_write' callback.
 *
 * Prepare a write into a StringInfo.
 *
 * Don't do anything lasting in here, it's quite possible that nothing will be done
 * with the data.
 */
/*
 * LogicalDecodingContext 的 'prepare_write' 回调。
 *
 * 准备将数据写入 StringInfo。
 *
 * 此处不要做任何持久性操作，因为很可能不会对数据进行任何实际处理。
 */
static void
WalSndPrepareWrite(LogicalDecodingContext *ctx, XLogRecPtr lsn, TransactionId xid, bool last_write)
{
	/* can't have sync rep confused by sending the same LSN several times */
	/* 不能因多次发送相同 LSN 而使同步复制产生混乱 */
	if (!last_write)
		lsn = InvalidXLogRecPtr;

	resetStringInfo(ctx->out);

	pq_sendbyte(ctx->out, 'w');
	pq_sendint64(ctx->out, lsn);	/* dataStart */
	pq_sendint64(ctx->out, lsn);	/* walEnd */

	/*
	 * Fill out the sendtime later, just as it's done in XLogSendPhysical, but
	 * reserve space here.
	 */
	/*
	 * 稍后填写发送时间戳（与 XLogSendPhysical 中的做法相同），但此处先预留空间。
	 */
	pq_sendint64(ctx->out, 0);	/* sendtime */
}

/*
 * LogicalDecodingContext 'write' callback.
 *
 * Actually write out data previously prepared by WalSndPrepareWrite out to
 * the network. Take as long as needed, but process replies from the other
 * side and check timeouts during that.
 */
/*
 * LogicalDecodingContext 的 'write' 回调。
 *
 * 将 WalSndPrepareWrite 之前准备好的数据实际写入网络。需要多长时间就花多长时间，
 * 但在此期间要处理对端的回复并检查超时。
 */
static void
WalSndWriteData(LogicalDecodingContext *ctx, XLogRecPtr lsn, TransactionId xid,
				bool last_write)
{
	TimestampTz now;

	/*
	 * Fill the send timestamp last, so that it is taken as late as possible.
	 * This is somewhat ugly, but the protocol is set as it's already used for
	 * several releases by streaming physical replication.
	 */
	/*
	 * 最后填写发送时间戳，以便尽可能晚地获取时间。这有点丑陋，但协议是固定的，
	 * 流式物理复制已经使用了好几个版本。
	 */
	resetStringInfo(&tmpbuf);
	now = GetCurrentTimestamp();
	pq_sendint64(&tmpbuf, now);
	memcpy(&ctx->out->data[1 + sizeof(int64) + sizeof(int64)],
		   tmpbuf.data, sizeof(int64));

	/* output previously gathered data in a CopyData packet */
	/* 将之前收集的数据以 CopyData 数据包的形式输出 */
	pq_putmessage_noblock('d', ctx->out->data, ctx->out->len);

	CHECK_FOR_INTERRUPTS();

	/* Try to flush pending output to the client */
	/* 尝试将待发送数据刷新给客户端 */
	if (pq_flush_if_writable() != 0)
		WalSndShutdown();

	/* Try taking fast path unless we get too close to walsender timeout. */
	/* 尝试走快速路径，除非我们已经太接近 walsender 超时。 */
	if (now < TimestampTzPlusMilliseconds(last_reply_timestamp,
										  wal_sender_timeout / 2) &&
		!pq_is_send_pending())
	{
		return;
	}

	/* If we have pending write here, go to slow path */
	/* 若此处有待发送数据，则走慢速路径 */
	ProcessPendingWrites();
}

/*
 * Wait until there is no pending write. Also process replies from the other
 * side and check timeouts during that.
 */
/*
 * 等待直到没有待发送的写操作。在等待期间也处理对端的回复并检查超时。
 */
static void
ProcessPendingWrites(void)
{
	for (;;)
	{
		long		sleeptime;

		/* Check for input from the client */
		/* 检查来自客户端的输入 */
		ProcessRepliesIfAny();

		/* die if timeout was reached */
		/* 若已达超时则退出 */
		WalSndCheckTimeOut();

		/* Send keepalive if the time has come */
		/* 若时机合适则发送 keepalive */
		WalSndKeepaliveIfNecessary();

		if (!pq_is_send_pending())
			break;

		sleeptime = WalSndComputeSleeptime(GetCurrentTimestamp());

		/* Sleep until something happens or we time out */
		/* 休眠直到发生某事或超时 */
		WalSndWait(WL_SOCKET_WRITEABLE | WL_SOCKET_READABLE, sleeptime,
				   WAIT_EVENT_WAL_SENDER_WRITE_DATA);

		/* Clear any already-pending wakeups */
		/* 清除所有已挂起的唤醒 */
		ResetLatch(MyLatch);

		CHECK_FOR_INTERRUPTS();

		/* Process any requests or signals received recently */
		/* 处理最近收到的任何请求或信号 */
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
			SyncRepInitConfig();
		}

		/* Try to flush pending output to the client */
		/* 尝试将待发送数据刷新给客户端 */
		if (pq_flush_if_writable() != 0)
			WalSndShutdown();
	}

	/* reactivate latch so WalSndLoop knows to continue */
	/* 重新激活 latch，让 WalSndLoop 知道继续执行 */
	SetLatch(MyLatch);
}

/*
 * LogicalDecodingContext 'update_progress' callback.
 *
 * Write the current position to the lag tracker (see XLogSendPhysical).
 *
 * When skipping empty transactions, send a keepalive message if necessary.
 */
/*
 * LogicalDecodingContext 的 'update_progress' 回调。
 *
 * 将当前位置写入延迟跟踪器（参见 XLogSendPhysical）。
 *
 * 跳过空事务时，必要时发送 keepalive 消息。
 */
static void
WalSndUpdateProgress(LogicalDecodingContext *ctx, XLogRecPtr lsn, TransactionId xid,
					 bool skipped_xact)
{
	static TimestampTz sendTime = 0;
	TimestampTz now = GetCurrentTimestamp();
	bool		pending_writes = false;
	bool		end_xact = ctx->end_xact;

	/*
	 * Track lag no more than once per WALSND_LOGICAL_LAG_TRACK_INTERVAL_MS to
	 * avoid flooding the lag tracker when we commit frequently.
	 *
	 * We don't have a mechanism to get the ack for any LSN other than end
	 * xact LSN from the downstream. So, we track lag only for end of
	 * transaction LSN.
	 */
	/*
	 * 每隔 WALSND_LOGICAL_LAG_TRACK_INTERVAL_MS 最多跟踪一次延迟，
	 * 避免频繁提交时延迟跟踪器被大量数据淹没。
	 *
	 * 我们没有办法从下游获取除事务结束 LSN 以外的任何 LSN 的 ack。
	 * 因此，我们只跟踪事务结束 LSN 的延迟。
	 */
#define WALSND_LOGICAL_LAG_TRACK_INTERVAL_MS	1000
	if (end_xact && TimestampDifferenceExceeds(sendTime, now,
											   WALSND_LOGICAL_LAG_TRACK_INTERVAL_MS))
	{
		LagTrackerWrite(lsn, now);
		sendTime = now;
	}

	/*
	 * When skipping empty transactions in synchronous replication, we send a
	 * keepalive message to avoid delaying such transactions.
	 *
	 * It is okay to check sync_standbys_status without lock here as in the
	 * worst case we will just send an extra keepalive message when it is
	 * really not required.
	 */
	/*
	 * 在同步复制中跳过空事务时，我们发送一条 keepalive 消息，以避免延迟这些事务。
	 *
	 * 此处无需加锁即可检查 sync_standbys_status，最坏情况下只是在不必要时
	 * 多发一条 keepalive 消息。
	 */
	if (skipped_xact &&
		SyncRepRequested() &&
		(((volatile WalSndCtlData *) WalSndCtl)->sync_standbys_status & SYNC_STANDBY_DEFINED))
	{
		WalSndKeepalive(false, lsn);

		/* Try to flush pending output to the client */
		if (pq_flush_if_writable() != 0)
			WalSndShutdown();

		/* If we have pending write here, make sure it's actually flushed */
		if (pq_is_send_pending())
			pending_writes = true;
	}

	/*
	 * Process pending writes if any or try to send a keepalive if required.
	 * We don't need to try sending keep alive messages at the transaction end
	 * as that will be done at a later point in time. This is required only
	 * for large transactions where we don't send any changes to the
	 * downstream and the receiver can timeout due to that.
	 */
	/*
	 * 若有待发送写操作则处理，或在需要时尝试发送 keepalive。
	 * 事务结束时无需尝试发送 keepalive，因为稍后会处理。这仅在大事务场景下需要，
	 * 因为我们没有向下游发送任何变更，接收方可能因此超时。
	 */
	if (pending_writes || (!end_xact &&
						   now >= TimestampTzPlusMilliseconds(last_reply_timestamp,
															  wal_sender_timeout / 2)))
		ProcessPendingWrites();
}

/*
 * Wake up the logical walsender processes with logical failover slots if the
 * currently acquired physical slot is specified in synchronized_standby_slots GUC.
 */
/*
 * 若当前持有的物理槽在 synchronized_standby_slots GUC 中指定，
 * 则唤醒拥有逻辑故障切换槽的逻辑 walsender 进程。
 */
void
PhysicalWakeupLogicalWalSnd(void)
{
	Assert(MyReplicationSlot && SlotIsPhysical(MyReplicationSlot));

	/*
	 * If we are running in a standby, there is no need to wake up walsenders.
	 * This is because we do not support syncing slots to cascading standbys,
	 * so, there are no walsenders waiting for standbys to catch up.
	 */
	/*
	 * 若我们在备服务器上运行，则无需唤醒 walsender。
	 * 因为我们不支持将槽同步到级联备服务器，所以没有 walsender 在等待备服务器追赶。
	 */
	if (RecoveryInProgress())
		return;

	if (SlotExistsInSyncStandbySlots(NameStr(MyReplicationSlot->data.name)))
		ConditionVariableBroadcast(&WalSndCtl->wal_confirm_rcv_cv);
}

/*
 * Returns true if not all standbys have caught up to the flushed position
 * (flushed_lsn) when the current acquired slot is a logical failover
 * slot and we are streaming; otherwise, returns false.
 *
 * If returning true, the function sets the appropriate wait event in
 * wait_event; otherwise, wait_event is set to 0.
 */
/*
 * 当当前持有的槽是逻辑故障切换槽且我们正在流式传输时，若并非所有备服务器都已
 * 追赶到已刷新位置（flushed_lsn），则返回 true；否则返回 false。
 *
 * 返回 true 时，函数在 wait_event 中设置适当的等待事件；否则 wait_event 设为 0。
 */
static bool
NeedToWaitForStandbys(XLogRecPtr flushed_lsn, uint32 *wait_event)
{
	int			elevel = got_STOPPING ? ERROR : WARNING;
	bool		failover_slot;

	failover_slot = (replication_active && MyReplicationSlot->data.failover);

	/*
	 * Note that after receiving the shutdown signal, an ERROR is reported if
	 * any slots are dropped, invalidated, or inactive. This measure is taken
	 * to prevent the walsender from waiting indefinitely.
	 */
	/*
	 * 注意：收到关闭信号后，若有槽被删除、失效或处于非活动状态，将报告 ERROR。
	 * 此措施是为了防止 walsender 无限期等待。
	 */
	if (failover_slot && !StandbySlotsHaveCaughtup(flushed_lsn, elevel))
	{
		*wait_event = WAIT_EVENT_WAIT_FOR_STANDBY_CONFIRMATION;
		return true;
	}

	*wait_event = 0;
	return false;
}

/*
 * Returns true if we need to wait for WALs to be flushed to disk, or if not
 * all standbys have caught up to the flushed position (flushed_lsn) when the
 * current acquired slot is a logical failover slot and we are
 * streaming; otherwise, returns false.
 *
 * If returning true, the function sets the appropriate wait event in
 * wait_event; otherwise, wait_event is set to 0.
 */
/*
 * 若需要等待 WAL 刷新到磁盘，或者当前持有槽是逻辑故障切换槽且处于流式传输状态时
 * 并非所有备服务器都追赶到已刷新位置（flushed_lsn），则返回 true；否则返回 false。
 *
 * 返回 true 时，函数在 wait_event 中设置适当的等待事件；否则 wait_event 设为 0。
 */
static bool
NeedToWaitForWal(XLogRecPtr target_lsn, XLogRecPtr flushed_lsn,
				 uint32 *wait_event)
{
	/* Check if we need to wait for WALs to be flushed to disk */
	if (target_lsn > flushed_lsn)
	{
		*wait_event = WAIT_EVENT_WAL_SENDER_WAIT_FOR_WAL;
		return true;
	}

	/* Check if the standby slots have caught up to the flushed position */
	return NeedToWaitForStandbys(flushed_lsn, wait_event);
}

/*
 * Wait till WAL < loc is flushed to disk so it can be safely sent to client.
 *
 * If the walsender holds a logical failover slot, we also wait for all the
 * specified streaming replication standby servers to confirm receipt of WAL
 * up to RecentFlushPtr. It is beneficial to wait here for the confirmation
 * up to RecentFlushPtr rather than waiting before transmitting each change
 * to logical subscribers, which is already covered by RecentFlushPtr.
 *
 * Returns end LSN of flushed WAL.  Normally this will be >= loc, but if we
 * detect a shutdown request (either from postmaster or client) we will return
 * early, so caller must always check.
 */
/*
 * 等待 WAL < loc 被刷新到磁盘，以便安全地发送给客户端。
 *
 * 若 walsender 持有逻辑故障切换槽，还需等待所有指定的流式复制备服务器确认
 * 已收到直到 RecentFlushPtr 的 WAL。在此等待确认直到 RecentFlushPtr 比在
 * 向每个逻辑订阅者传输变更前等待更有益，因为 RecentFlushPtr 已经涵盖了这一点。
 *
 * 返回已刷新 WAL 的末尾 LSN。通常 >= loc，但如果检测到关闭请求（来自
 * postmaster 或客户端），则提前返回，调用方必须始终检查返回值。
 */
static XLogRecPtr
WalSndWaitForWal(XLogRecPtr loc)
{
	int			wakeEvents;
	uint32		wait_event = 0;
	static XLogRecPtr RecentFlushPtr = InvalidXLogRecPtr;
	TimestampTz last_flush = 0;

	/*
	 * Fast path to avoid acquiring the spinlock in case we already know we
	 * have enough WAL available and all the standby servers have confirmed
	 * receipt of WAL up to RecentFlushPtr. This is particularly interesting
	 * if we're far behind.
	 */
	/*
	 * 快速路径：若已知有足够的 WAL 可用且所有备服务器都已确认收到直到
	 * RecentFlushPtr 的 WAL，则跳过获取自旋锁。在我们落后很多时尤为有用。
	 */
	if (!XLogRecPtrIsInvalid(RecentFlushPtr) &&
		!NeedToWaitForWal(loc, RecentFlushPtr, &wait_event))
		return RecentFlushPtr;

	/*
	 * Within the loop, we wait for the necessary WALs to be flushed to disk
	 * first, followed by waiting for standbys to catch up if there are enough
	 * WALs (see NeedToWaitForWal()) or upon receiving the shutdown signal.
	 */
	/*
	 * 在循环中，先等待必要的 WAL 刷新到磁盘，然后若有足够的 WAL
	 * （见 NeedToWaitForWal()）或收到关闭信号，则等待备服务器追赶上来。
	 */
	for (;;)
	{
		bool		wait_for_standby_at_stop = false;
		long		sleeptime;
		TimestampTz now;

		/* Clear any already-pending wakeups */
		/* 清除所有已挂起的唤醒 */
		ResetLatch(MyLatch);

		CHECK_FOR_INTERRUPTS();

		/* Process any requests or signals received recently */
		/* 处理最近收到的任何请求或信号 */
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
			SyncRepInitConfig();
		}

		/* Check for input from the client */
		/* 检查来自客户端的输入 */
		ProcessRepliesIfAny();

		/*
		 * If we're shutting down, trigger pending WAL to be written out,
		 * otherwise we'd possibly end up waiting for WAL that never gets
		 * written, because walwriter has shut down already.
		 */
		/*
		 * 若正在关闭，触发待写 WAL 的写出，否则可能因 walwriter 已关闭而
		 * 陷入等待永远不会被写出的 WAL 的境地。
		 */
		if (got_STOPPING)
			XLogBackgroundFlush();

		/*
		 * To avoid the scenario where standbys need to catch up to a newer
		 * WAL location in each iteration, we update our idea of the currently
		 * flushed position only if we are not waiting for standbys to catch
		 * up.
		 */
		/*
		 * 为了避免备服务器在每次迭代中都需要追赶更新的 WAL 位置，
		 * 仅在我们不在等待备服务器追赶时才更新当前已刷新位置的记录。
		 */
		if (wait_event != WAIT_EVENT_WAIT_FOR_STANDBY_CONFIRMATION)
		{
			if (!RecoveryInProgress())
				RecentFlushPtr = GetFlushRecPtr(NULL);
			else
				RecentFlushPtr = GetXLogReplayRecPtr(NULL);
		}

		/*
		 * If postmaster asked us to stop and the standby slots have caught up
		 * to the flushed position, don't wait anymore.
		 *
		 * It's important to do this check after the recomputation of
		 * RecentFlushPtr, so we can send all remaining data before shutting
		 * down.
		 */
		/*
		 * 若 postmaster 要求我们停止，且备服务器槽已追赶到已刷新位置，
		 * 则不再等待。
		 *
		 * 必须在重新计算 RecentFlushPtr 之后进行此检查，以便在关闭前
		 * 发送所有剩余数据。
		 */
		if (got_STOPPING)
		{
			if (NeedToWaitForStandbys(RecentFlushPtr, &wait_event))
				wait_for_standby_at_stop = true;
			else
				break;
		}

		/*
		 * We only send regular messages to the client for full decoded
		 * transactions, but a synchronous replication and walsender shutdown
		 * possibly are waiting for a later location. So, before sleeping, we
		 * send a ping containing the flush location. If the receiver is
		 * otherwise idle, this keepalive will trigger a reply. Processing the
		 * reply will update these MyWalSnd locations.
		 */
		/*
		 * 我们只对完整解码事务向客户端发送常规消息，但同步复制和 walsender 关闭
		 * 可能正在等待更晚的位置。因此在休眠前，我们发送一个包含刷新位置的 ping。
		 * 若接收方处于空闲状态，此 keepalive 会触发回复。处理回复将更新
		 * MyWalSnd 中的位置信息。
		 */
		if (MyWalSnd->flush < sentPtr &&
			MyWalSnd->write < sentPtr &&
			!waiting_for_ping_response)
			WalSndKeepalive(false, InvalidXLogRecPtr);

		/*
		 * Exit the loop if already caught up and doesn't need to wait for
		 * standby slots.
		 */
		/*
		 * 若已追赶上且不需要等待备服务器槽，则退出循环。
		 */
		if (!wait_for_standby_at_stop &&
			!NeedToWaitForWal(loc, RecentFlushPtr, &wait_event))
			break;

		/*
		 * Waiting for new WAL or waiting for standbys to catch up. Since we
		 * need to wait, we're now caught up.
		 */
		/*
		 * 正在等待新 WAL 或等待备服务器追赶。既然需要等待，说明我们已经追上了。
		 */
		WalSndCaughtUp = true;

		/*
		 * Try to flush any pending output to the client.
		 */
		/*
		 * 尝试将所有待发送输出刷新给客户端。
		 */
		if (pq_flush_if_writable() != 0)
			WalSndShutdown();

		/*
		 * If we have received CopyDone from the client, sent CopyDone
		 * ourselves, and the output buffer is empty, it's time to exit
		 * streaming, so fail the current WAL fetch request.
		 */
		/*
		 * 若已收到客户端的 CopyDone、我们自己也已发送 CopyDone 且输出缓冲区为空，
		 * 则应退出流式传输，使当前 WAL 获取请求失败。
		 */
		if (streamingDoneReceiving && streamingDoneSending &&
			!pq_is_send_pending())
			break;

		/* die if timeout was reached */
		/* 若已达超时则退出 */
		WalSndCheckTimeOut();

		/* Send keepalive if the time has come */
		/* 若时机合适则发送 keepalive */
		WalSndKeepaliveIfNecessary();

		/*
		 * Sleep until something happens or we time out.  Also wait for the
		 * socket becoming writable, if there's still pending output.
		 * Otherwise we might sit on sendable output data while waiting for
		 * new WAL to be generated.  (But if we have nothing to send, we don't
		 * want to wake on socket-writable.)
		 */
		/*
		 * 休眠直到发生某事或超时。若仍有待发送输出，也等待套接字变为可写。
		 * 否则我们可能在等待新 WAL 生成时滞留在可发送的输出数据上。
		 * （但若没有需要发送的内容，则不需要在套接字可写时唤醒。）
		 */
		now = GetCurrentTimestamp();
		sleeptime = WalSndComputeSleeptime(now);

		wakeEvents = WL_SOCKET_READABLE;

		if (pq_is_send_pending())
			wakeEvents |= WL_SOCKET_WRITEABLE;

		Assert(wait_event != 0);

		/* Report IO statistics, if needed */
		if (TimestampDifferenceExceeds(last_flush, now,
									   WALSENDER_STATS_FLUSH_INTERVAL))
		{
			pgstat_flush_io(false);
			(void) pgstat_flush_backend(false, PGSTAT_BACKEND_FLUSH_IO);
			last_flush = now;
		}

		WalSndWait(wakeEvents, sleeptime, wait_event);
	}

	/* reactivate latch so WalSndLoop knows to continue */
	/* 重新激活 latch，让 WalSndLoop 知道继续执行 */
	SetLatch(MyLatch);
	return RecentFlushPtr;
}

/*
 * Execute an incoming replication command.
 *
 * Returns true if the cmd_string was recognized as WalSender command, false
 * if not.
 */
/*
 * 执行传入的复制命令。
 *
 * 若 cmd_string 被识别为 WalSender 命令则返回 true，否则返回 false。
 */
bool
exec_replication_command(const char *cmd_string)
{
	yyscan_t	scanner;
	int			parse_rc;
	Node	   *cmd_node;
	const char *cmdtag;
	MemoryContext old_context = CurrentMemoryContext;

	/* We save and re-use the cmd_context across calls */
	static MemoryContext cmd_context = NULL;

	/*
	 * If WAL sender has been told that shutdown is getting close, switch its
	 * status accordingly to handle the next replication commands correctly.
	 */
	/*
	 * 若 WAL 发送进程被告知即将关闭，则相应切换其状态，以正确处理后续复制命令。
	 */
	if (got_STOPPING)
		WalSndSetState(WALSNDSTATE_STOPPING);

	/*
	 * Throw error if in stopping mode.  We need prevent commands that could
	 * generate WAL while the shutdown checkpoint is being written.  To be
	 * safe, we just prohibit all new commands.
	 */
	/*
	 * 若处于 stopping 模式则报错。需要防止在写入关闭检查点期间执行
	 * 可能生成 WAL 的命令。为安全起见，我们禁止所有新命令。
	 */
	if (MyWalSnd->state == WALSNDSTATE_STOPPING)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot execute new commands while WAL sender is in stopping mode")));

	/*
	 * CREATE_REPLICATION_SLOT ... LOGICAL exports a snapshot until the next
	 * command arrives. Clean up the old stuff if there's anything.
	 */
	/*
	 * CREATE_REPLICATION_SLOT ... LOGICAL 导出快照直到下一条命令到达。
	 * 若有旧的快照，则清理它。
	 */
	SnapBuildClearExportedSnapshot();

	CHECK_FOR_INTERRUPTS();

	/*
	 * Prepare to parse and execute the command.
	 *
	 * Because replication command execution can involve beginning or ending
	 * transactions, we need a working context that will survive that, so we
	 * make it a child of TopMemoryContext.  That in turn creates a hazard of
	 * long-lived memory leaks if we lose track of the working context.  We
	 * deal with that by creating it only once per walsender, and resetting it
	 * for each new command.  (Normally this reset is a no-op, but if the
	 * prior exec_replication_command call failed with an error, it won't be.)
	 *
	 * This is subtler than it looks.  The transactions we manage can extend
	 * across replication commands, indeed SnapBuildClearExportedSnapshot
	 * might have just ended one.  Because transaction exit will revert to the
	 * memory context that was current at transaction start, we need to be
	 * sure that that context is still valid.  That motivates re-using the
	 * same cmd_context rather than making a new one each time.
	 */
	/*
	 * 准备解析并执行命令。
	 *
	 * 由于复制命令执行可能涉及开启或结束事务，我们需要一个能在此过程中存活的
	 * 工作上下文，因此将其设为 TopMemoryContext 的子上下文。若我们丢失对工作
	 * 上下文的跟踪，这会带来长期内存泄漏的风险。解决方法是每个 walsender 只
	 * 创建一次，并在每条新命令时重置它。（通常重置是空操作，但若上次
	 * exec_replication_command 调用以错误失败，则不然。）
	 *
	 * 这比看起来更微妙。我们管理的事务可以跨越多条复制命令，实际上
	 * SnapBuildClearExportedSnapshot 可能刚刚结束了一个事务。由于事务退出时
	 * 会回滚到事务开始时的内存上下文，我们需要确保该上下文仍然有效。这就是
	 * 重用同一个 cmd_context 而不是每次新建一个的原因。
	 */
	if (cmd_context == NULL)
		cmd_context = AllocSetContextCreate(TopMemoryContext,
											"Replication command context",
											ALLOCSET_DEFAULT_SIZES);
	else
		MemoryContextReset(cmd_context);

	MemoryContextSwitchTo(cmd_context);

	replication_scanner_init(cmd_string, &scanner);

	/*
	 * Is it a WalSender command?
	 */
	/*
	 * 这是 WalSender 命令吗？
	 */
	if (!replication_scanner_is_replication_command(scanner))
	{
		/* Nope; clean up and get out. */
		replication_scanner_finish(scanner);

		MemoryContextSwitchTo(old_context);
		MemoryContextReset(cmd_context);

		/* XXX this is a pretty random place to make this check */
		if (MyDatabaseId == InvalidOid)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot execute SQL commands in WAL sender for physical replication")));

		/* Tell the caller that this wasn't a WalSender command. */
		return false;
	}

	/*
	 * Looks like a WalSender command, so parse it.
	 */
	/*
	 * 看起来是 WalSender 命令，对其进行解析。
	 */
	parse_rc = replication_yyparse(&cmd_node, scanner);
	if (parse_rc != 0)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg_internal("replication command parser returned %d",
								 parse_rc)));
	replication_scanner_finish(scanner);

	/*
	 * Report query to various monitoring facilities.  For this purpose, we
	 * report replication commands just like SQL commands.
	 */
	/*
	 * 向各种监控设施报告查询。为此，我们像报告 SQL 命令一样报告复制命令。
	 */
	debug_query_string = cmd_string;

	pgstat_report_activity(STATE_RUNNING, cmd_string);

	/*
	 * Log replication command if log_replication_commands is enabled. Even
	 * when it's disabled, log the command with DEBUG1 level for backward
	 * compatibility.
	 */
	/*
	 * 若 log_replication_commands 已启用，则记录复制命令日志。
	 * 即使未启用，也以 DEBUG1 级别记录命令日志，以保持向后兼容性。
	 */
	ereport(log_replication_commands ? LOG : DEBUG1,
			(errmsg("received replication command: %s", cmd_string)));

	/*
	 * Disallow replication commands in aborted transaction blocks.
	 */
	/*
	 * 不允许在已中止的事务块中执行复制命令。
	 */
	if (IsAbortedTransactionBlockState())
		ereport(ERROR,
				(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
				 errmsg("current transaction is aborted, "
						"commands ignored until end of transaction block")));

	CHECK_FOR_INTERRUPTS();

	/*
	 * Allocate buffers that will be used for each outgoing and incoming
	 * message.  We do this just once per command to reduce palloc overhead.
	 */
	/*
	 * 为每条发出和接收的消息分配缓冲区。每条命令只分配一次，以减少 palloc 开销。
	 */
	initStringInfo(&output_message);
	initStringInfo(&reply_message);
	initStringInfo(&tmpbuf);

	switch (cmd_node->type)
	{
		case T_IdentifySystemCmd:
			cmdtag = "IDENTIFY_SYSTEM";
			set_ps_display(cmdtag);
			IdentifySystem();
			EndReplicationCommand(cmdtag);
			break;

		case T_ReadReplicationSlotCmd:
			cmdtag = "READ_REPLICATION_SLOT";
			set_ps_display(cmdtag);
			ReadReplicationSlot((ReadReplicationSlotCmd *) cmd_node);
			EndReplicationCommand(cmdtag);
			break;

		case T_BaseBackupCmd:
			cmdtag = "BASE_BACKUP";
			set_ps_display(cmdtag);
			PreventInTransactionBlock(true, cmdtag);
			SendBaseBackup((BaseBackupCmd *) cmd_node, uploaded_manifest);
			EndReplicationCommand(cmdtag);
			break;

		case T_CreateReplicationSlotCmd:
			cmdtag = "CREATE_REPLICATION_SLOT";
			set_ps_display(cmdtag);
			CreateReplicationSlot((CreateReplicationSlotCmd *) cmd_node);
			EndReplicationCommand(cmdtag);
			break;

		case T_DropReplicationSlotCmd:
			cmdtag = "DROP_REPLICATION_SLOT";
			set_ps_display(cmdtag);
			DropReplicationSlot((DropReplicationSlotCmd *) cmd_node);
			EndReplicationCommand(cmdtag);
			break;

		case T_AlterReplicationSlotCmd:
			cmdtag = "ALTER_REPLICATION_SLOT";
			set_ps_display(cmdtag);
			AlterReplicationSlot((AlterReplicationSlotCmd *) cmd_node);
			EndReplicationCommand(cmdtag);
			break;

		case T_StartReplicationCmd:
			{
				StartReplicationCmd *cmd = (StartReplicationCmd *) cmd_node;

				cmdtag = "START_REPLICATION";
				set_ps_display(cmdtag);
				PreventInTransactionBlock(true, cmdtag);

				if (cmd->kind == REPLICATION_KIND_PHYSICAL)
					StartReplication(cmd);
				else
					StartLogicalReplication(cmd);

				/* dupe, but necessary per libpqrcv_endstreaming */
				EndReplicationCommand(cmdtag);

				Assert(xlogreader != NULL);
				break;
			}

		case T_TimeLineHistoryCmd:
			cmdtag = "TIMELINE_HISTORY";
			set_ps_display(cmdtag);
			PreventInTransactionBlock(true, cmdtag);
			SendTimeLineHistory((TimeLineHistoryCmd *) cmd_node);
			EndReplicationCommand(cmdtag);
			break;

		case T_VariableShowStmt:
			{
				DestReceiver *dest = CreateDestReceiver(DestRemoteSimple);
				VariableShowStmt *n = (VariableShowStmt *) cmd_node;

				cmdtag = "SHOW";
				set_ps_display(cmdtag);

				/* syscache access needs a transaction environment */
				StartTransactionCommand();
				GetPGVariable(n->name, dest);
				CommitTransactionCommand();
				EndReplicationCommand(cmdtag);
			}
			break;

		case T_UploadManifestCmd:
			cmdtag = "UPLOAD_MANIFEST";
			set_ps_display(cmdtag);
			PreventInTransactionBlock(true, cmdtag);
			UploadManifest();
			EndReplicationCommand(cmdtag);
			break;

		default:
			elog(ERROR, "unrecognized replication command node tag: %u",
				 cmd_node->type);
	}

	/*
	 * Done.  Revert to caller's memory context, and clean out the cmd_context
	 * to recover memory right away.
	 */
	/*
	 * 完成。恢复到调用方的内存上下文，并立即清空 cmd_context 以回收内存。
	 */
	MemoryContextSwitchTo(old_context);
	MemoryContextReset(cmd_context);

	/*
	 * We need not update ps display or pg_stat_activity, because PostgresMain
	 * will reset those to "idle".  But we must reset debug_query_string to
	 * ensure it doesn't become a dangling pointer.
	 */
	/*
	 * 无需更新 ps 显示或 pg_stat_activity，因为 PostgresMain 会将其重置为 "idle"。
	 * 但必须重置 debug_query_string，以确保它不会成为悬空指针。
	 */
	debug_query_string = NULL;

	return true;
}

/*
 * Process any incoming messages while streaming. Also checks if the remote
 * end has closed the connection.
 */
/*
 * 在流式传输期间处理所有传入消息，同时检查远端是否已关闭连接。
 */
static void
ProcessRepliesIfAny(void)
{
	unsigned char firstchar;
	int			maxmsglen;
	int			r;
	bool		received = false;

	last_processing = GetCurrentTimestamp();

	/*
	 * If we already received a CopyDone from the frontend, any subsequent
	 * message is the beginning of a new command, and should be processed in
	 * the main processing loop.
	 */
	/*
	 * 若已从前端收到 CopyDone，任何后续消息都是新命令的开始，
	 * 应在主处理循环中处理。
	 */
	while (!streamingDoneReceiving)
	{
		pq_startmsgread();
		r = pq_getbyte_if_available(&firstchar);
		if (r < 0)
		{
			/* unexpected error or EOF */
			ereport(COMMERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("unexpected EOF on standby connection")));
			proc_exit(0);
		}
		if (r == 0)
		{
			/* no data available without blocking */
			pq_endmsgread();
			break;
		}

		/* Validate message type and set packet size limit */
		switch (firstchar)
		{
			case PqMsg_CopyData:
				maxmsglen = PQ_LARGE_MESSAGE_LIMIT;
				break;
			case PqMsg_CopyDone:
			case PqMsg_Terminate:
				maxmsglen = PQ_SMALL_MESSAGE_LIMIT;
				break;
			default:
				ereport(FATAL,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("invalid standby message type \"%c\"",
								firstchar)));
				maxmsglen = 0;	/* keep compiler quiet */
				break;
		}

		/* Read the message contents */
		resetStringInfo(&reply_message);
		if (pq_getmessage(&reply_message, maxmsglen))
		{
			ereport(COMMERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("unexpected EOF on standby connection")));
			proc_exit(0);
		}

		/* ... and process it */
		switch (firstchar)
		{
			/*
			 * 'd' means a standby reply wrapped in a CopyData packet.
			 */
			/*
			 * 'd' 表示封装在 CopyData 数据包中的备服务器回复。
			 */
		case PqMsg_CopyData:
			ProcessStandbyMessage();
			received = true;
			break;

			/*
			 * CopyDone means the standby requested to finish streaming.
			 * Reply with CopyDone, if we had not sent that already.
			 */
			/*
			 * CopyDone 表示备服务器请求结束流式传输。
			 * 若我们尚未发送 CopyDone，则回复 CopyDone。
			 */
		case PqMsg_CopyDone:
			if (!streamingDoneSending)
			{
				pq_putmessage_noblock('c', NULL, 0);
				streamingDoneSending = true;
			}

			streamingDoneReceiving = true;
			received = true;
			break;

			/*
			 * 'X' means that the standby is closing down the socket.
			 */
			/*
			 * 'X' 表示备服务器正在关闭套接字。
			 */
		case PqMsg_Terminate:
			proc_exit(0);

			default:
				Assert(false);	/* NOT REACHED */
		}
	}

	/*
	 * Save the last reply timestamp if we've received at least one reply.
	 */
	/*
	 * 若已收到至少一条回复，则保存最后一次回复的时间戳。
	 */
	if (received)
	{
		last_reply_timestamp = last_processing;
		waiting_for_ping_response = false;
	}
}

/*
 * Process a status update message received from standby.
 */
/*
 * 处理从备服务器收到的状态更新消息。
 */
static void
ProcessStandbyMessage(void)
{
	char		msgtype;

	/*
	 * Check message type from the first byte.
	 */
	/*
	 * 从第一个字节检查消息类型。
	 */
	msgtype = pq_getmsgbyte(&reply_message);

	switch (msgtype)
	{
		case 'r':
			ProcessStandbyReplyMessage();
			break;

		case 'h':
			ProcessStandbyHSFeedbackMessage();
			break;

		default:
			ereport(COMMERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("unexpected message type \"%c\"", msgtype)));
			proc_exit(0);
	}
}

/*
 * Remember that a walreceiver just confirmed receipt of lsn `lsn`.
 */
/*
 * 记录 walreceiver 刚刚确认收到了 LSN `lsn`。
 */
static void
PhysicalConfirmReceivedLocation(XLogRecPtr lsn)
{
	bool		changed = false;
	ReplicationSlot *slot = MyReplicationSlot;

	Assert(lsn != InvalidXLogRecPtr);
	SpinLockAcquire(&slot->mutex);
	if (slot->data.restart_lsn != lsn)
	{
		changed = true;
		slot->data.restart_lsn = lsn;
	}
	SpinLockRelease(&slot->mutex);

	if (changed)
	{
		ReplicationSlotMarkDirty();
		ReplicationSlotsComputeRequiredLSN();
		PhysicalWakeupLogicalWalSnd();
	}

	/*
	 * One could argue that the slot should be saved to disk now, but that'd
	 * be energy wasted - the worst thing lost information could cause here is
	 * to give wrong information in a statistics view - we'll just potentially
	 * be more conservative in removing files.
	 */
	/*
	 * 有人可能认为此时应将槽保存到磁盘，但那样会浪费资源——丢失信息在这里
	 * 最坏的影响是在统计视图中显示错误信息——我们只是在删除文件时可能更保守。
	 */
}

/*
 * Regular reply from standby advising of WAL locations on standby server.
 */
/*
 * 备服务器发来的常规回复，告知备服务器上的 WAL 位置信息。
 */
static void
ProcessStandbyReplyMessage(void)
{
	XLogRecPtr	writePtr,
				flushPtr,
				applyPtr;
	bool		replyRequested;
	TimeOffset	writeLag,
				flushLag,
				applyLag;
	bool		clearLagTimes;
	TimestampTz now;
	TimestampTz replyTime;

	static bool fullyAppliedLastTime = false;

	/* the caller already consumed the msgtype byte */
	/* 调用方已经消费了消息类型字节 */
	writePtr = pq_getmsgint64(&reply_message);
	flushPtr = pq_getmsgint64(&reply_message);
	applyPtr = pq_getmsgint64(&reply_message);
	replyTime = pq_getmsgint64(&reply_message);
	replyRequested = pq_getmsgbyte(&reply_message);

	if (message_level_is_interesting(DEBUG2))
	{
		char	   *replyTimeStr;

		/* Copy because timestamptz_to_str returns a static buffer */
		replyTimeStr = pstrdup(timestamptz_to_str(replyTime));

		elog(DEBUG2, "write %X/%X flush %X/%X apply %X/%X%s reply_time %s",
			 LSN_FORMAT_ARGS(writePtr),
			 LSN_FORMAT_ARGS(flushPtr),
			 LSN_FORMAT_ARGS(applyPtr),
			 replyRequested ? " (reply requested)" : "",
			 replyTimeStr);

		pfree(replyTimeStr);
	}

	/* See if we can compute the round-trip lag for these positions. */
	/* 检查是否能为这些位置计算往返延迟。 */
	now = GetCurrentTimestamp();
	writeLag = LagTrackerRead(SYNC_REP_WAIT_WRITE, writePtr, now);
	flushLag = LagTrackerRead(SYNC_REP_WAIT_FLUSH, flushPtr, now);
	applyLag = LagTrackerRead(SYNC_REP_WAIT_APPLY, applyPtr, now);

	/*
	 * If the standby reports that it has fully replayed the WAL in two
	 * consecutive reply messages, then the second such message must result
	 * from wal_receiver_status_interval expiring on the standby.  This is a
	 * convenient time to forget the lag times measured when it last
	 * wrote/flushed/applied a WAL record, to avoid displaying stale lag data
	 * until more WAL traffic arrives.
	 */
	/*
	 * 若备服务器在连续两条回复消息中都报告已完全重放 WAL，则第二条此类消息
	 * 必然是因备服务器上的 wal_receiver_status_interval 到期所致。此时是
	 * 清除上次写入/刷新/应用 WAL 记录时测量的延迟时间的好时机，以避免在
	 * 更多 WAL 流量到来之前显示过时的延迟数据。
	 */
	clearLagTimes = false;
	if (applyPtr == sentPtr)
	{
		if (fullyAppliedLastTime)
			clearLagTimes = true;
		fullyAppliedLastTime = true;
	}
	else
		fullyAppliedLastTime = false;

	/* Send a reply if the standby requested one. */
	/* 若备服务器请求回复，则发送回复。 */
	if (replyRequested)
		WalSndKeepalive(false, InvalidXLogRecPtr);

	/*
	 * Update shared state for this WalSender process based on reply data from
	 * standby.
	 */
	/*
	 * 根据来自备服务器的回复数据，更新当前 WalSender 进程的共享状态。
	 */
	{
		WalSnd	   *walsnd = MyWalSnd;

		SpinLockAcquire(&walsnd->mutex);
		walsnd->write = writePtr;
		walsnd->flush = flushPtr;
		walsnd->apply = applyPtr;
		if (writeLag != -1 || clearLagTimes)
			walsnd->writeLag = writeLag;
		if (flushLag != -1 || clearLagTimes)
			walsnd->flushLag = flushLag;
		if (applyLag != -1 || clearLagTimes)
			walsnd->applyLag = applyLag;
		walsnd->replyTime = replyTime;
		SpinLockRelease(&walsnd->mutex);
	}

	if (!am_cascading_walsender)
		SyncRepReleaseWaiters();

	/*
	 * Advance our local xmin horizon when the client confirmed a flush.
	 */
	/*
	 * 当客户端确认刷新时，推进我们本地的 xmin 水位线。
	 */
	if (MyReplicationSlot && flushPtr != InvalidXLogRecPtr)
	{
		if (SlotIsLogical(MyReplicationSlot))
			LogicalConfirmReceivedLocation(flushPtr);
		else
			PhysicalConfirmReceivedLocation(flushPtr);
	}
}

/* compute new replication slot xmin horizon if needed */
/* 若需要，计算新的复制槽 xmin 水位线 */
static void
PhysicalReplicationSlotNewXmin(TransactionId feedbackXmin, TransactionId feedbackCatalogXmin)
{
	bool		changed = false;
	ReplicationSlot *slot = MyReplicationSlot;

	SpinLockAcquire(&slot->mutex);
	MyProc->xmin = InvalidTransactionId;

	/*
	 * For physical replication we don't need the interlock provided by xmin
	 * and effective_xmin since the consequences of a missed increase are
	 * limited to query cancellations, so set both at once.
	 */
	/*
	 * 对于物理复制，我们不需要 xmin 和 effective_xmin 提供的互锁，
	 * 因为遗漏一次增加的后果仅限于查询取消，所以一次性同时设置两者。
	 */
	if (!TransactionIdIsNormal(slot->data.xmin) ||
		!TransactionIdIsNormal(feedbackXmin) ||
		TransactionIdPrecedes(slot->data.xmin, feedbackXmin))
	{
		changed = true;
		slot->data.xmin = feedbackXmin;
		slot->effective_xmin = feedbackXmin;
	}
	if (!TransactionIdIsNormal(slot->data.catalog_xmin) ||
		!TransactionIdIsNormal(feedbackCatalogXmin) ||
		TransactionIdPrecedes(slot->data.catalog_xmin, feedbackCatalogXmin))
	{
		changed = true;
		slot->data.catalog_xmin = feedbackCatalogXmin;
		slot->effective_catalog_xmin = feedbackCatalogXmin;
	}
	SpinLockRelease(&slot->mutex);

	if (changed)
	{
		ReplicationSlotMarkDirty();
		ReplicationSlotsComputeRequiredXmin(false);
	}
}

/*
 * Check that the provided xmin/epoch are sane, that is, not in the future
 * and not so far back as to be already wrapped around.
 *
 * Epoch of nextXid should be same as standby, or if the counter has
 * wrapped, then one greater than standby.
 *
 * This check doesn't care about whether clog exists for these xids
 * at all.
 */
/*
 * 检查提供的 xmin/epoch 是否合理，即不在未来，也没有早到已经绕回。
 *
 * nextXid 的 epoch 应与备服务器相同，或若计数器已绕回，则比备服务器大一。
 *
 * 此检查完全不关心这些 xid 是否存在 clog。
 */
static bool
TransactionIdInRecentPast(TransactionId xid, uint32 epoch)
{
	FullTransactionId nextFullXid;
	TransactionId nextXid;
	uint32		nextEpoch;

	nextFullXid = ReadNextFullTransactionId();
	nextXid = XidFromFullTransactionId(nextFullXid);
	nextEpoch = EpochFromFullTransactionId(nextFullXid);

	if (xid <= nextXid)
	{
		if (epoch != nextEpoch)
			return false;
	}
	else
	{
		if (epoch + 1 != nextEpoch)
			return false;
	}

	if (!TransactionIdPrecedesOrEquals(xid, nextXid))
		return false;			/* epoch OK, but it's wrapped around */

	return true;
}

/*
 * Hot Standby feedback
 */
/*
 * Hot Standby 反馈处理
 */
static void
ProcessStandbyHSFeedbackMessage(void)
{
	TransactionId feedbackXmin;
	uint32		feedbackEpoch;
	TransactionId feedbackCatalogXmin;
	uint32		feedbackCatalogEpoch;
	TimestampTz replyTime;

	/*
	 * Decipher the reply message. The caller already consumed the msgtype
	 * byte. See XLogWalRcvSendHSFeedback() in walreceiver.c for the creation
	 * of this message.
	 */
	/*
	 * 解读回复消息。调用方已经消费了消息类型字节。
	 * 参见 walreceiver.c 中的 XLogWalRcvSendHSFeedback() 了解此消息的创建方式。
	 */
	replyTime = pq_getmsgint64(&reply_message);
	feedbackXmin = pq_getmsgint(&reply_message, 4);
	feedbackEpoch = pq_getmsgint(&reply_message, 4);
	feedbackCatalogXmin = pq_getmsgint(&reply_message, 4);
	feedbackCatalogEpoch = pq_getmsgint(&reply_message, 4);

	if (message_level_is_interesting(DEBUG2))
	{
		char	   *replyTimeStr;

		/* Copy because timestamptz_to_str returns a static buffer */
		replyTimeStr = pstrdup(timestamptz_to_str(replyTime));

		elog(DEBUG2, "hot standby feedback xmin %u epoch %u, catalog_xmin %u epoch %u reply_time %s",
			 feedbackXmin,
			 feedbackEpoch,
			 feedbackCatalogXmin,
			 feedbackCatalogEpoch,
			 replyTimeStr);

		pfree(replyTimeStr);
	}

	/*
	 * Update shared state for this WalSender process based on reply data from
	 * standby.
	 */
	/*
	 * 根据来自备服务器的回复数据，更新当前 WalSender 进程的共享状态。
	 */
	{
		WalSnd	   *walsnd = MyWalSnd;

		SpinLockAcquire(&walsnd->mutex);
		walsnd->replyTime = replyTime;
		SpinLockRelease(&walsnd->mutex);
	}

	/*
	 * Unset WalSender's xmins if the feedback message values are invalid.
	 * This happens when the downstream turned hot_standby_feedback off.
	 */
	/*
	 * 若反馈消息中的值无效，则清除 WalSender 的 xmins。
	 * 这发生在下游关闭了 hot_standby_feedback 的情况下。
	 */
	if (!TransactionIdIsNormal(feedbackXmin)
		&& !TransactionIdIsNormal(feedbackCatalogXmin))
	{
		MyProc->xmin = InvalidTransactionId;
		if (MyReplicationSlot != NULL)
			PhysicalReplicationSlotNewXmin(feedbackXmin, feedbackCatalogXmin);
		return;
	}

	/*
	 * Check that the provided xmin/epoch are sane, that is, not in the future
	 * and not so far back as to be already wrapped around.  Ignore if not.
	 */
	/*
	 * 检查提供的 xmin/epoch 是否合理，即不在未来，也没有早到已经绕回。若不合理则忽略。
	 */
	if (TransactionIdIsNormal(feedbackXmin) &&
		!TransactionIdInRecentPast(feedbackXmin, feedbackEpoch))
		return;

	if (TransactionIdIsNormal(feedbackCatalogXmin) &&
		!TransactionIdInRecentPast(feedbackCatalogXmin, feedbackCatalogEpoch))
		return;

	/*
	 * Set the WalSender's xmin equal to the standby's requested xmin, so that
	 * the xmin will be taken into account by GetSnapshotData() /
	 * ComputeXidHorizons().  This will hold back the removal of dead rows and
	 * thereby prevent the generation of cleanup conflicts on the standby
	 * server.
	 *
	 * There is a small window for a race condition here: although we just
	 * checked that feedbackXmin precedes nextXid, the nextXid could have
	 * gotten advanced between our fetching it and applying the xmin below,
	 * perhaps far enough to make feedbackXmin wrap around.  In that case the
	 * xmin we set here would be "in the future" and have no effect.  No point
	 * in worrying about this since it's too late to save the desired data
	 * anyway.  Assuming that the standby sends us an increasing sequence of
	 * xmins, this could only happen during the first reply cycle, else our
	 * own xmin would prevent nextXid from advancing so far.
	 *
	 * We don't bother taking the ProcArrayLock here.  Setting the xmin field
	 * is assumed atomic, and there's no real need to prevent concurrent
	 * horizon determinations.  (If we're moving our xmin forward, this is
	 * obviously safe, and if we're moving it backwards, well, the data is at
	 * risk already since a VACUUM could already have determined the horizon.)
	 *
	 * If we're using a replication slot we reserve the xmin via that,
	 * otherwise via the walsender's PGPROC entry. We can only track the
	 * catalog xmin separately when using a slot, so we store the least of the
	 * two provided when not using a slot.
	 *
	 * XXX: It might make sense to generalize the ephemeral slot concept and
	 * always use the slot mechanism to handle the feedback xmin.
	 */
	/*
	 * 将 WalSender 的 xmin 设置为备服务器请求的 xmin，以便 GetSnapshotData() /
	 * ComputeXidHorizons() 将其纳入考虑。这将阻止死行的删除，从而防止在备服务器上
	 * 产生清理冲突。
	 *
	 * 此处存在一个小的竞态条件窗口：尽管我们刚刚检查了 feedbackXmin 先于 nextXid，
	 * 但在我们获取 nextXid 和在下方应用 xmin 之间，nextXid 可能已经推进，甚至推进
	 * 得足够远使 feedbackXmin 绕回。在这种情况下，我们在此设置的 xmin 将处于"未来"
	 * 而无效。不必担心这个问题，因为无论如何已经来不及保存所需数据了。假设备服务器
	 * 向我们发送递增的 xmin 序列，这只可能发生在第一个回复周期，否则我们自己的 xmin
	 * 会阻止 nextXid 推进那么远。
	 *
	 * 我们不在此处获取 ProcArrayLock。设置 xmin 字段被假定为原子操作，且没有真正
	 * 需要阻止并发的水位线确定。（若我们在推进 xmin，这显然是安全的；若我们在回退它，
	 * 数据已经有风险，因为 VACUUM 可能已经确定了水位线。）
	 *
	 * 若我们使用复制槽，则通过复制槽保留 xmin；否则通过 walsender 的 PGPROC 条目。
	 * 只有使用槽时才能单独跟踪 catalog xmin，因此在不使用槽时存储两者中较小的值。
	 *
	 * XXX：推广临时槽的概念，始终使用槽机制来处理反馈 xmin 可能是有意义的。
	 */
	if (MyReplicationSlot != NULL)	/* XXX: persistency configurable? */
		PhysicalReplicationSlotNewXmin(feedbackXmin, feedbackCatalogXmin);
	else
	{
		if (TransactionIdIsNormal(feedbackCatalogXmin)
			&& TransactionIdPrecedes(feedbackCatalogXmin, feedbackXmin))
			MyProc->xmin = feedbackCatalogXmin;
		else
			MyProc->xmin = feedbackXmin;
	}
}

/*
 * Compute how long send/receive loops should sleep.
 *
 * If wal_sender_timeout is enabled we want to wake up in time to send
 * keepalives and to abort the connection if wal_sender_timeout has been
 * reached.
 */
/*
 * 计算发送/接收循环应休眠多长时间。
 *
 * 若 wal_sender_timeout 已启用，我们希望在适当时机唤醒以发送 keepalive，
 * 并在达到 wal_sender_timeout 时中止连接。
 */
static long
WalSndComputeSleeptime(TimestampTz now)
{
	long		sleeptime = 10000;	/* 10 s */

	if (wal_sender_timeout > 0 && last_reply_timestamp > 0)
	{
		TimestampTz wakeup_time;

		/*
		 * At the latest stop sleeping once wal_sender_timeout has been
		 * reached.
		 */
		/*
		 * 最迟在达到 wal_sender_timeout 时停止休眠。
		 */
		wakeup_time = TimestampTzPlusMilliseconds(last_reply_timestamp,
												  wal_sender_timeout);

		/*
		 * If no ping has been sent yet, wakeup when it's time to do so.
		 * WalSndKeepaliveIfNecessary() wants to send a keepalive once half of
		 * the timeout passed without a response.
		 */
		/*
		 * 若尚未发送 ping，则在应发送时唤醒。
		 * WalSndKeepaliveIfNecessary() 希望在超时一半时间内没有收到响应后发送 keepalive。
		 */
		if (!waiting_for_ping_response)
			wakeup_time = TimestampTzPlusMilliseconds(last_reply_timestamp,
													  wal_sender_timeout / 2);

		/* Compute relative time until wakeup. */
		/* 计算距唤醒的相对时间。 */
		sleeptime = TimestampDifferenceMilliseconds(now, wakeup_time);
	}

	return sleeptime;
}

/*
 * Check whether there have been responses by the client within
 * wal_sender_timeout and shutdown if not.  Using last_processing as the
 * reference point avoids counting server-side stalls against the client.
 * However, a long server-side stall can make WalSndKeepaliveIfNecessary()
 * postdate last_processing by more than wal_sender_timeout.  If that happens,
 * the client must reply almost immediately to avoid a timeout.  This rarely
 * affects the default configuration, under which clients spontaneously send a
 * message every standby_message_timeout = wal_sender_timeout/6 = 10s.  We
 * could eliminate that problem by recognizing timeout expiration at
 * wal_sender_timeout/2 after the keepalive.
 */
/*
 * 检查客户端是否在 wal_sender_timeout 内有过响应，若没有则关闭连接。
 * 使用 last_processing 作为参考点，避免将服务器端的停滞计入客户端超时。
 * 然而，较长的服务器端停滞可能使 WalSndKeepaliveIfNecessary() 的时间比
 * last_processing 晚超过 wal_sender_timeout。若发生这种情况，客户端必须
 * 几乎立即回复才能避免超时。这在默认配置下很少发生，默认配置下客户端每隔
 * standby_message_timeout = wal_sender_timeout/6 = 10s 自发地发送消息。
 * 可以通过在 keepalive 后 wal_sender_timeout/2 时识别超时到期来消除该问题。
 */
static void
WalSndCheckTimeOut(void)
{
	TimestampTz timeout;

	/* don't bail out if we're doing something that doesn't require timeouts */
	/* 若正在执行不需要超时的操作，则不退出 */
	if (last_reply_timestamp <= 0)
		return;

	timeout = TimestampTzPlusMilliseconds(last_reply_timestamp,
										  wal_sender_timeout);

	if (wal_sender_timeout > 0 && last_processing >= timeout)
	{
		/*
		 * Since typically expiration of replication timeout means
		 * communication problem, we don't send the error message to the
		 * standby.
		 */
		/*
		 * 由于复制超时通常意味着通信问题，我们不向备服务器发送错误消息。
		 */
		ereport(COMMERROR,
				(errmsg("terminating walsender process due to replication timeout")));

		WalSndShutdown();
	}
}

/* Main loop of walsender process that streams the WAL over Copy messages. */
/* walsender 进程的主循环，通过 Copy 消息流式传输 WAL。 */
static void
WalSndLoop(WalSndSendDataCallback send_data)
{
	TimestampTz last_flush = 0;

	/*
	 * Initialize the last reply timestamp. That enables timeout processing
	 * from hereon.
	 */
	/*
	 * 初始化最后一次回复的时间戳，从此开始启用超时处理。
	 */
	last_reply_timestamp = GetCurrentTimestamp();
	waiting_for_ping_response = false;

	/*
	 * Loop until we reach the end of this timeline or the client requests to
	 * stop streaming.
	 */
	/*
	 * 循环直到到达当前时间线末尾或客户端请求停止流式传输。
	 */
	for (;;)
	{
		/* Clear any already-pending wakeups */
		/* 清除所有已挂起的唤醒 */
		ResetLatch(MyLatch);

		CHECK_FOR_INTERRUPTS();

		/* Process any requests or signals received recently */
		/* 处理最近收到的任何请求或信号 */
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
			SyncRepInitConfig();
		}

		/* Check for input from the client */
		/* 检查来自客户端的输入 */
		ProcessRepliesIfAny();

		/*
		 * If we have received CopyDone from the client, sent CopyDone
		 * ourselves, and the output buffer is empty, it's time to exit
		 * streaming.
		 */
		/*
		 * 若已收到客户端的 CopyDone、我们自己也已发送 CopyDone 且输出缓冲区为空，
		 * 则是时候退出流式传输了。
		 */
		if (streamingDoneReceiving && streamingDoneSending &&
			!pq_is_send_pending())
			break;

		/*
		 * If we don't have any pending data in the output buffer, try to send
		 * some more.  If there is some, we don't bother to call send_data
		 * again until we've flushed it ... but we'd better assume we are not
		 * caught up.
		 */
		/*
		 * 若输出缓冲区中没有待发送数据，尝试发送更多。若有，则在刷新完成前不再
		 * 调用 send_data……但最好假设我们尚未追上。
		 */
		if (!pq_is_send_pending())
			send_data();
		else
			WalSndCaughtUp = false;

		/* Try to flush pending output to the client */
		/* 尝试将待发送输出刷新给客户端 */
		if (pq_flush_if_writable() != 0)
			WalSndShutdown();

		/* If nothing remains to be sent right now ... */
		/* 若当前没有剩余需要发送的内容…… */
		if (WalSndCaughtUp && !pq_is_send_pending())
		{
			/*
			 * If we're in catchup state, move to streaming.  This is an
			 * important state change for users to know about, since before
			 * this point data loss might occur if the primary dies and we
			 * need to failover to the standby. The state change is also
			 * important for synchronous replication, since commits that
			 * started to wait at that point might wait for some time.
			 */
			/*
			 * 若处于追赶状态，切换到流式传输状态。这是用户需要了解的重要状态变化，
			 * 因为在此之前若主服务器宕机而需要故障切换到备服务器，可能会发生数据丢失。
			 * 此状态变化对同步复制也很重要，因为在此时开始等待的提交可能需要等待一段时间。
			 */
			if (MyWalSnd->state == WALSNDSTATE_CATCHUP)
			{
				ereport(DEBUG1,
						(errmsg_internal("\"%s\" has now caught up with upstream server",
										 application_name)));
				WalSndSetState(WALSNDSTATE_STREAMING);
			}

			/*
			 * When SIGUSR2 arrives, we send any outstanding logs up to the
			 * shutdown checkpoint record (i.e., the latest record), wait for
			 * them to be replicated to the standby, and exit. This may be a
			 * normal termination at shutdown, or a promotion, the walsender
			 * is not sure which.
			 */
			/*
			 * 收到 SIGUSR2 时，发送所有未发送的日志直到关闭检查点记录（即最新记录），
			 * 等待其被复制到备服务器后退出。这可能是关闭时的正常终止，也可能是提升，
			 * walsender 无法确定是哪种情况。
			 */
			if (got_SIGUSR2)
				WalSndDone(send_data);
		}

		/* Check for replication timeout. */
		/* 检查复制超时。 */
		WalSndCheckTimeOut();

		/* Send keepalive if the time has come */
		/* 若时机合适则发送 keepalive */
		WalSndKeepaliveIfNecessary();

		/*
		 * Block if we have unsent data.  XXX For logical replication, let
		 * WalSndWaitForWal() handle any other blocking; idle receivers need
		 * its additional actions.  For physical replication, also block if
		 * caught up; its send_data does not block.
		 *
		 * The IO statistics are reported in WalSndWaitForWal() for the
		 * logical WAL senders.
		 */
		/*
		 * 若有未发送数据则阻塞。XXX 对于逻辑复制，让 WalSndWaitForWal() 处理任何其他
		 * 阻塞；空闲接收方需要其额外操作。对于物理复制，在追上时也阻塞；其 send_data
		 * 不会阻塞。
		 *
		 * 逻辑 WAL 发送进程的 IO 统计在 WalSndWaitForWal() 中报告。
		 */
		if ((WalSndCaughtUp && send_data != XLogSendLogical &&
			 !streamingDoneSending) ||
			pq_is_send_pending())
		{
			long		sleeptime;
			int			wakeEvents;
			TimestampTz now;

			if (!streamingDoneReceiving)
				wakeEvents = WL_SOCKET_READABLE;
			else
				wakeEvents = 0;

			/*
			 * Use fresh timestamp, not last_processing, to reduce the chance
			 * of reaching wal_sender_timeout before sending a keepalive.
			 */
			/*
			 * 使用最新时间戳而非 last_processing，以减少在发送 keepalive 前
			 * 达到 wal_sender_timeout 的可能性。
			 */
			now = GetCurrentTimestamp();
			sleeptime = WalSndComputeSleeptime(now);

			if (pq_is_send_pending())
				wakeEvents |= WL_SOCKET_WRITEABLE;

			/* Report IO statistics, if needed */
			if (TimestampDifferenceExceeds(last_flush, now,
										   WALSENDER_STATS_FLUSH_INTERVAL))
			{
				pgstat_flush_io(false);
				(void) pgstat_flush_backend(false, PGSTAT_BACKEND_FLUSH_IO);
				last_flush = now;
			}

			/* Sleep until something happens or we time out */
			WalSndWait(wakeEvents, sleeptime, WAIT_EVENT_WAL_SENDER_MAIN);
		}
	}
}

/* Initialize a per-walsender data structure for this walsender process */
/* 为当前 walsender 进程初始化每个 walsender 的数据结构 */
static void
InitWalSenderSlot(void)
{
	int			i;

	/*
	 * WalSndCtl should be set up already (we inherit this by fork() or
	 * EXEC_BACKEND mechanism from the postmaster).
	 */
	/*
	 * WalSndCtl 应已设置好（通过 fork() 或 EXEC_BACKEND 机制从 postmaster 继承）。
	 */
	Assert(WalSndCtl != NULL);
	Assert(MyWalSnd == NULL);

	/*
	 * Find a free walsender slot and reserve it. This must not fail due to
	 * the prior check for free WAL senders in InitProcess().
	 */
	/*
	 * 找一个空闲的 walsender 槽并保留它。由于 InitProcess() 中已进行了空闲
	 * WAL 发送进程的检查，此操作不应失败。
	 */
	for (i = 0; i < max_wal_senders; i++)
	{
		WalSnd	   *walsnd = &WalSndCtl->walsnds[i];

		SpinLockAcquire(&walsnd->mutex);

		if (walsnd->pid != 0)
		{
			SpinLockRelease(&walsnd->mutex);
			continue;
		}
		else
		{
			/*
			 * Found a free slot. Reserve it for us.
			 */
			/* 找到了一个空闲槽，为我们保留它。 */
			walsnd->pid = MyProcPid;
			walsnd->state = WALSNDSTATE_STARTUP;
			walsnd->sentPtr = InvalidXLogRecPtr;
			walsnd->needreload = false;
			walsnd->write = InvalidXLogRecPtr;
			walsnd->flush = InvalidXLogRecPtr;
			walsnd->apply = InvalidXLogRecPtr;
			walsnd->writeLag = -1;
			walsnd->flushLag = -1;
			walsnd->applyLag = -1;
			walsnd->sync_standby_priority = 0;
			walsnd->replyTime = 0;

			/*
			 * The kind assignment is done here and not in StartReplication()
			 * and StartLogicalReplication(). Indeed, the logical walsender
			 * needs to read WAL records (like snapshot of running
			 * transactions) during the slot creation. So it needs to be woken
			 * up based on its kind.
			 *
			 * The kind assignment could also be done in StartReplication(),
			 * StartLogicalReplication() and CREATE_REPLICATION_SLOT but it
			 * seems better to set it on one place.
			 */
			/*
			 * kind 的赋值在此处完成，而不是在 StartReplication() 和
			 * StartLogicalReplication() 中。确实，逻辑 walsender 在槽创建期间
			 * 需要读取 WAL 记录（例如正在运行的事务快照），所以需要根据其 kind
			 * 被唤醒。
			 *
			 * kind 的赋值也可以在 StartReplication()、StartLogicalReplication()
			 * 和 CREATE_REPLICATION_SLOT 中完成，但在一处统一设置更合适。
			 */
			if (MyDatabaseId == InvalidOid)
				walsnd->kind = REPLICATION_KIND_PHYSICAL;
			else
				walsnd->kind = REPLICATION_KIND_LOGICAL;

			SpinLockRelease(&walsnd->mutex);
			/* don't need the lock anymore */
			MyWalSnd = (WalSnd *) walsnd;

			break;
		}
	}

	Assert(MyWalSnd != NULL);

	/* Arrange to clean up at walsender exit */
	/* 安排在 walsender 退出时进行清理 */
	on_shmem_exit(WalSndKill, 0);
}

/* Destroy the per-walsender data structure for this walsender process */
/* 销毁当前 walsender 进程的每个 walsender 数据结构 */
static void
WalSndKill(int code, Datum arg)
{
	WalSnd	   *walsnd = MyWalSnd;

	Assert(walsnd != NULL);

	MyWalSnd = NULL;

	SpinLockAcquire(&walsnd->mutex);
	/* Mark WalSnd struct as no longer being in use. */
	/* 将 WalSnd 结构体标记为不再使用。 */
	walsnd->pid = 0;
	SpinLockRelease(&walsnd->mutex);
}

/* XLogReaderRoutine->segment_open callback */
/* XLogReaderRoutine->segment_open 回调 */
static void
WalSndSegmentOpen(XLogReaderState *state, XLogSegNo nextSegNo,
				  TimeLineID *tli_p)
{
	char		path[MAXPGPATH];

	/*-------
	 * When reading from a historic timeline, and there is a timeline switch
	 * within this segment, read from the WAL segment belonging to the new
	 * timeline.
	 *
	 * For example, imagine that this server is currently on timeline 5, and
	 * we're streaming timeline 4. The switch from timeline 4 to 5 happened at
	 * 0/13002088. In pg_wal, we have these files:
	 *
	 * ...
	 * 000000040000000000000012
	 * 000000040000000000000013
	 * 000000050000000000000013
	 * 000000050000000000000014
	 * ...
	 *
	 * In this situation, when requested to send the WAL from segment 0x13, on
	 * timeline 4, we read the WAL from file 000000050000000000000013. Archive
	 * recovery prefers files from newer timelines, so if the segment was
	 * restored from the archive on this server, the file belonging to the old
	 * timeline, 000000040000000000000013, might not exist. Their contents are
	 * equal up to the switchpoint, because at a timeline switch, the used
	 * portion of the old segment is copied to the new file.
	 */
	/*-------
	 * 从历史时间线读取时，若该段内存在时间线切换，则从属于新时间线的 WAL 段读取。
	 *
	 * 例如，假设服务器当前处于时间线 5，而我们正在流式传输时间线 4。
	 * 从时间线 4 到 5 的切换发生在 0/13002088。在 pg_wal 中，有以下文件：
	 *
	 * ...
	 * 000000040000000000000012
	 * 000000040000000000000013
	 * 000000050000000000000013
	 * 000000050000000000000014
	 * ...
	 *
	 * 在这种情况下，当被请求从时间线 4 的段 0x13 发送 WAL 时，
	 * 我们从文件 000000050000000000000013 读取 WAL。归档恢复优先使用新时间线的文件，
	 * 因此若该段是从服务器上的归档恢复的，属于旧时间线的文件
	 * 000000040000000000000013 可能不存在。它们的内容在切换点之前是相同的，
	 * 因为在时间线切换时，旧段的已使用部分会被复制到新文件中。
	 */
	*tli_p = sendTimeLine;
	if (sendTimeLineIsHistoric)
	{
		XLogSegNo	endSegNo;

		XLByteToSeg(sendTimeLineValidUpto, endSegNo, state->segcxt.ws_segsize);
		if (nextSegNo == endSegNo)
			*tli_p = sendTimeLineNextTLI;
	}

	XLogFilePath(path, *tli_p, nextSegNo, state->segcxt.ws_segsize);
	state->seg.ws_file = BasicOpenFile(path, O_RDONLY | PG_BINARY);
	if (state->seg.ws_file >= 0)
		return;

	/*
	 * If the file is not found, assume it's because the standby asked for a
	 * too old WAL segment that has already been removed or recycled.
	 */
	/*
	 * 若文件未找到，假设是因为备服务器请求了一个已被删除或回收的过旧 WAL 段。
	 */
	if (errno == ENOENT)
	{
		char		xlogfname[MAXFNAMELEN];
		int			save_errno = errno;

		XLogFileName(xlogfname, *tli_p, nextSegNo, wal_segment_size);
		errno = save_errno;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("requested WAL segment %s has already been removed",
						xlogfname)));
	}
	else
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m",
						path)));
}

/*
 * Send out the WAL in its normal physical/stored form.
 *
 * Read up to MAX_SEND_SIZE bytes of WAL that's been flushed to disk,
 * but not yet sent to the client, and buffer it in the libpq output
 * buffer.
 *
 * If there is no unsent WAL remaining, WalSndCaughtUp is set to true,
 * otherwise WalSndCaughtUp is set to false.
 */
/*
 * 以正常物理/存储形式发送 WAL。
 *
 * 读取最多 MAX_SEND_SIZE 字节的已刷新到磁盘但尚未发送给客户端的 WAL，
 * 并将其缓冲到 libpq 输出缓冲区中。
 *
 * 若没有剩余未发送的 WAL，WalSndCaughtUp 设为 true；否则设为 false。
 */
static void
XLogSendPhysical(void)
{
	XLogRecPtr	SendRqstPtr;
	XLogRecPtr	startptr;
	XLogRecPtr	endptr;
	Size		nbytes;
	XLogSegNo	segno;
	WALReadError errinfo;
	Size		rbytes;

	/* If requested switch the WAL sender to the stopping state. */
	/* 若收到请求，将 WAL 发送进程切换到 stopping 状态。 */
	if (got_STOPPING)
		WalSndSetState(WALSNDSTATE_STOPPING);

	if (streamingDoneSending)
	{
		WalSndCaughtUp = true;
		return;
	}

	/* Figure out how far we can safely send the WAL. */
	/* 确定我们可以安全发送 WAL 的范围。 */
	if (sendTimeLineIsHistoric)
	{
		/*
		 * Streaming an old timeline that's in this server's history, but is
		 * not the one we're currently inserting or replaying. It can be
		 * streamed up to the point where we switched off that timeline.
		 */
		/*
		 * 正在流式传输服务器历史中的一条旧时间线，但不是当前正在插入或重放的时间线。
		 * 可以流式传输到切换离开该时间线的那个点。
		 */
		SendRqstPtr = sendTimeLineValidUpto;
	}
	else if (am_cascading_walsender)
	{
		TimeLineID	SendRqstTLI;

		/*
		 * Streaming the latest timeline on a standby.
		 *
		 * Attempt to send all WAL that has already been replayed, so that we
		 * know it's valid. If we're receiving WAL through streaming
		 * replication, it's also OK to send any WAL that has been received
		 * but not replayed.
		 *
		 * The timeline we're recovering from can change, or we can be
		 * promoted. In either case, the current timeline becomes historic. We
		 * need to detect that so that we don't try to stream past the point
		 * where we switched to another timeline. We check for promotion or
		 * timeline switch after calculating FlushPtr, to avoid a race
		 * condition: if the timeline becomes historic just after we checked
		 * that it was still current, it's still be OK to stream it up to the
		 * FlushPtr that was calculated before it became historic.
		 */
		/*
		 * 在备服务器上流式传输最新时间线。
		 *
		 * 尝试发送所有已重放的 WAL，以确保其有效性。若通过流式复制接收 WAL，
		 * 也可以发送已接收但尚未重放的 WAL。
		 *
		 * 我们正在恢复的时间线可能会改变，或者我们可能被提升。
		 * 在这两种情况下，当前时间线都会变为历史时间线。我们需要检测到这一点，
		 * 以避免尝试流式传输到切换到另一时间线的点之后。我们在计算 FlushPtr 之后
		 * 检查提升或时间线切换，以避免竞态条件：如果时间线在我们检查到它仍然是
		 * 当前时间线之后才变为历史时间线，仍然可以将其流式传输到在其变为历史时间线
		 * 之前计算的 FlushPtr。
		 */
		bool		becameHistoric = false;

		SendRqstPtr = GetStandbyFlushRecPtr(&SendRqstTLI);

		if (!RecoveryInProgress())
		{
			/* We have been promoted. */
			/* 我们已被提升。 */
			SendRqstTLI = GetWALInsertionTimeLine();
			am_cascading_walsender = false;
			becameHistoric = true;
		}
		else
		{
			/*
			 * Still a cascading standby. But is the timeline we're sending
			 * still the one recovery is recovering from?
			 */
			/*
			 * 仍然是级联备服务器。但我们正在发送的时间线是否仍然是恢复正在
			 * 恢复自的那个时间线？
			 */
			if (sendTimeLine != SendRqstTLI)
				becameHistoric = true;
		}

		if (becameHistoric)
		{
			/*
			 * The timeline we were sending has become historic. Read the
			 * timeline history file of the new timeline to see where exactly
			 * we forked off from the timeline we were sending.
			 */
			/*
			 * 我们正在发送的时间线已变为历史时间线。读取新时间线的时间线历史文件，
			 * 查看我们从正在发送的时间线中精确分叉的位置。
			 */
			List	   *history;

			history = readTimeLineHistory(SendRqstTLI);
			sendTimeLineValidUpto = tliSwitchPoint(sendTimeLine, history, &sendTimeLineNextTLI);

			Assert(sendTimeLine < sendTimeLineNextTLI);
			list_free_deep(history);

			sendTimeLineIsHistoric = true;

			SendRqstPtr = sendTimeLineValidUpto;
		}
	}
	else
	{
		/*
		 * Streaming the current timeline on a primary.
		 *
		 * Attempt to send all data that's already been written out and
		 * fsync'd to disk.  We cannot go further than what's been written out
		 * given the current implementation of WALRead().  And in any case
		 * it's unsafe to send WAL that is not securely down to disk on the
		 * primary: if the primary subsequently crashes and restarts, standbys
		 * must not have applied any WAL that got lost on the primary.
		 */
		/*
		 * 在主服务器上流式传输当前时间线。
		 *
		 * 尝试发送所有已写出并 fsync 到磁盘的数据。鉴于 WALRead() 的当前实现，
		 * 我们不能超过已写出的范围。而且无论如何，发送未安全落盘到主服务器的 WAL
		 * 是不安全的：若主服务器随后崩溃并重启，备服务器不应已应用了在主服务器上
		 * 丢失的 WAL。
		 */
		SendRqstPtr = GetFlushRecPtr(NULL);
	}

	/*
	 * Record the current system time as an approximation of the time at which
	 * this WAL location was written for the purposes of lag tracking.
	 *
	 * In theory we could make XLogFlush() record a time in shmem whenever WAL
	 * is flushed and we could get that time as well as the LSN when we call
	 * GetFlushRecPtr() above (and likewise for the cascading standby
	 * equivalent), but rather than putting any new code into the hot WAL path
	 * it seems good enough to capture the time here.  We should reach this
	 * after XLogFlush() runs WalSndWakeupProcessRequests(), and although that
	 * may take some time, we read the WAL flush pointer and take the time
	 * very close to together here so that we'll get a later position if it is
	 * still moving.
	 *
	 * Because LagTrackerWrite ignores samples when the LSN hasn't advanced,
	 * this gives us a cheap approximation for the WAL flush time for this
	 * LSN.
	 *
	 * Note that the LSN is not necessarily the LSN for the data contained in
	 * the present message; it's the end of the WAL, which might be further
	 * ahead.  All the lag tracking machinery cares about is finding out when
	 * that arbitrary LSN is eventually reported as written, flushed and
	 * applied, so that it can measure the elapsed time.
	 */
	/*
	 * 将当前系统时间记录为该 WAL 位置被写入时间的近似值，用于延迟跟踪。
	 *
	 * 理论上我们可以让 XLogFlush() 在每次 WAL 被刷新时在共享内存中记录时间，
	 * 这样在调用上面的 GetFlushRecPtr() 时可以同时获取时间和 LSN（级联备服务器同理），
	 * 但与其在热 WAL 路径中添加新代码，不如在此处捕获时间更合适。我们应该在
	 * XLogFlush() 运行 WalSndWakeupProcessRequests() 之后到达此处，尽管可能需要
	 * 一些时间，但我们读取 WAL 刷新指针和获取时间几乎是同时进行的，因此若 WAL
	 * 仍在移动，我们会获取到更靠后的位置。
	 *
	 * 由于 LagTrackerWrite 在 LSN 未推进时会忽略采样，这为我们提供了该 LSN 的
	 * WAL 刷新时间的廉价近似值。
	 *
	 * 注意，LSN 不一定是当前消息中数据对应的 LSN；它是 WAL 的末尾，
	 * 可能更靠前。延迟跟踪机制关心的是找出该任意 LSN 最终何时被报告为已写入、
	 * 已刷新和已应用，以便测量经过的时间。
	 */
	LagTrackerWrite(SendRqstPtr, GetCurrentTimestamp());

	/*
	 * If this is a historic timeline and we've reached the point where we
	 * forked to the next timeline, stop streaming.
	 *
	 * Note: We might already have sent WAL > sendTimeLineValidUpto. The
	 * startup process will normally replay all WAL that has been received
	 * from the primary, before promoting, but if the WAL streaming is
	 * terminated at a WAL page boundary, the valid portion of the timeline
	 * might end in the middle of a WAL record. We might've already sent the
	 * first half of that partial WAL record to the cascading standby, so that
	 * sentPtr > sendTimeLineValidUpto. That's OK; the cascading standby can't
	 * replay the partial WAL record either, so it can still follow our
	 * timeline switch.
	 */
	/*
	 * 若这是历史时间线且我们已到达分叉到下一个时间线的点，则停止流式传输。
	 *
	 * 注意：我们可能已发送了 WAL > sendTimeLineValidUpto。启动进程通常会在提升前
	 * 重放从主服务器收到的所有 WAL，但若 WAL 流式传输在 WAL 页边界处终止，
	 * 时间线的有效部分可能在 WAL 记录的中间结束。我们可能已将该部分 WAL 记录的
	 * 前半部分发送给级联备服务器，导致 sentPtr > sendTimeLineValidUpto。
	 * 这没关系；级联备服务器也无法重放不完整的 WAL 记录，因此它仍然可以跟随我们
	 * 的时间线切换。
	 */
	if (sendTimeLineIsHistoric && sendTimeLineValidUpto <= sentPtr)
	{
		/* close the current file. */
		/* 关闭当前文件。 */
		if (xlogreader->seg.ws_file >= 0)
			wal_segment_close(xlogreader);

		/* Send CopyDone */
		/* 发送 CopyDone */
		pq_putmessage_noblock('c', NULL, 0);
		streamingDoneSending = true;

		WalSndCaughtUp = true;

		elog(DEBUG1, "walsender reached end of timeline at %X/%X (sent up to %X/%X)",
			 LSN_FORMAT_ARGS(sendTimeLineValidUpto),
			 LSN_FORMAT_ARGS(sentPtr));
		return;
	}

	/* Do we have any work to do? */
	/* 我们有任何工作要做吗？ */
	Assert(sentPtr <= SendRqstPtr);
	if (SendRqstPtr <= sentPtr)
	{
		WalSndCaughtUp = true;
		return;
	}

	/*
	 * Figure out how much to send in one message. If there's no more than
	 * MAX_SEND_SIZE bytes to send, send everything. Otherwise send
	 * MAX_SEND_SIZE bytes, but round back to logfile or page boundary.
	 *
	 * The rounding is not only for performance reasons. Walreceiver relies on
	 * the fact that we never split a WAL record across two messages. Since a
	 * long WAL record is split at page boundary into continuation records,
	 * page boundary is always a safe cut-off point. We also assume that
	 * SendRqstPtr never points to the middle of a WAL record.
	 */
	/*
	 * 确定每条消息发送多少内容。若剩余内容不超过 MAX_SEND_SIZE 字节，则全部发送；
	 * 否则发送 MAX_SEND_SIZE 字节，但向后对齐到日志文件或页边界。
	 *
	 * 取整不仅出于性能原因。Walreceiver 依赖这样一个事实：我们从不跨两条消息分割
	 * WAL 记录。由于长 WAL 记录在页边界处被分割成连续记录，页边界始终是安全的
	 * 截断点。我们还假设 SendRqstPtr 永远不会指向 WAL 记录的中间。
	 */
	startptr = sentPtr;
	endptr = startptr;
	endptr += MAX_SEND_SIZE;

	/* if we went beyond SendRqstPtr, back off */
	/* 若超过了 SendRqstPtr，则回退 */
	if (SendRqstPtr <= endptr)
	{
		endptr = SendRqstPtr;
		if (sendTimeLineIsHistoric)
			WalSndCaughtUp = false;
		else
			WalSndCaughtUp = true;
	}
	else
	{
		/* round down to page boundary. */
		/* 向下对齐到页边界。 */
		endptr -= (endptr % XLOG_BLCKSZ);
		WalSndCaughtUp = false;
	}

	nbytes = endptr - startptr;
	Assert(nbytes <= MAX_SEND_SIZE);

	/*
	 * OK to read and send the slice.
	 */
	/*
	 * 可以读取并发送这个片段了。
	 */
	resetStringInfo(&output_message);
	pq_sendbyte(&output_message, 'w');

	pq_sendint64(&output_message, startptr);	/* dataStart */
	pq_sendint64(&output_message, SendRqstPtr); /* walEnd */
	pq_sendint64(&output_message, 0);	/* sendtime, filled in last */

	/*
	 * Read the log directly into the output buffer to avoid extra memcpy
	 * calls.
	 */
	/*
	 * 直接将日志读入输出缓冲区，以避免额外的 memcpy 调用。
	 */
	enlargeStringInfo(&output_message, nbytes);

retry:
	/* attempt to read WAL from WAL buffers first */
	/* 首先尝试从 WAL 缓冲区读取 WAL */
	rbytes = WALReadFromBuffers(&output_message.data[output_message.len],
								startptr, nbytes, xlogreader->seg.ws_tli);
	output_message.len += rbytes;
	startptr += rbytes;
	nbytes -= rbytes;

	/* now read the remaining WAL from WAL file */
	/* 现在从 WAL 文件读取剩余的 WAL */
	if (nbytes > 0 &&
		!WALRead(xlogreader,
				 &output_message.data[output_message.len],
				 startptr,
				 nbytes,
				 xlogreader->seg.ws_tli,	/* Pass the current TLI because
											 * only WalSndSegmentOpen controls
											 * whether new TLI is needed. */
				 &errinfo))
		WALReadRaiseError(&errinfo);

	/* See logical_read_xlog_page(). */
	/* 参见 logical_read_xlog_page()。 */
	XLByteToSeg(startptr, segno, xlogreader->segcxt.ws_segsize);
	CheckXLogRemoved(segno, xlogreader->seg.ws_tli);

	/*
	 * During recovery, the currently-open WAL file might be replaced with the
	 * file of the same name retrieved from archive. So we always need to
	 * check what we read was valid after reading into the buffer. If it's
	 * invalid, we try to open and read the file again.
	 */
	/*
	 * 在恢复期间，当前打开的 WAL 文件可能会被从归档中检索到的同名文件替换。
	 * 因此我们在读入缓冲区后，始终需要检查读取的内容是否有效。若无效，则尝试
	 * 重新打开并读取该文件。
	 */
	if (am_cascading_walsender)
	{
		WalSnd	   *walsnd = MyWalSnd;
		bool		reload;

		SpinLockAcquire(&walsnd->mutex);
		reload = walsnd->needreload;
		walsnd->needreload = false;
		SpinLockRelease(&walsnd->mutex);

		if (reload && xlogreader->seg.ws_file >= 0)
		{
			wal_segment_close(xlogreader);

			goto retry;
		}
	}

	output_message.len += nbytes;
	output_message.data[output_message.len] = '\0';

	/*
	 * Fill the send timestamp last, so that it is taken as late as possible.
	 */
	/*
	 * 最后填写发送时间戳，以便尽可能晚地获取时间。
	 */
	resetStringInfo(&tmpbuf);
	pq_sendint64(&tmpbuf, GetCurrentTimestamp());
	memcpy(&output_message.data[1 + sizeof(int64) + sizeof(int64)],
		   tmpbuf.data, sizeof(int64));

	pq_putmessage_noblock('d', output_message.data, output_message.len);

	sentPtr = endptr;

	/* Update shared memory status */
	/* 更新共享内存状态 */
	{
		WalSnd	   *walsnd = MyWalSnd;

		SpinLockAcquire(&walsnd->mutex);
		walsnd->sentPtr = sentPtr;
		SpinLockRelease(&walsnd->mutex);
	}

	/* Report progress of XLOG streaming in PS display */
	/* 在 PS 显示中报告 XLOG 流式传输的进度 */
	if (update_process_title)
	{
		char		activitymsg[50];

		snprintf(activitymsg, sizeof(activitymsg), "streaming %X/%X",
				 LSN_FORMAT_ARGS(sentPtr));
		set_ps_display(activitymsg);
	}
}

/*
 * Stream out logically decoded data.
 */
/*
 * 流式输出逻辑解码的数据。
 */
static void
XLogSendLogical(void)
{
	XLogRecord *record;
	char	   *errm;

	/*
	 * We'll use the current flush point to determine whether we've caught up.
	 * This variable is static in order to cache it across calls.  Caching is
	 * helpful because GetFlushRecPtr() needs to acquire a heavily-contended
	 * spinlock.
	 */
	/*
	 * 我们将使用当前刷新点来判断是否已追赶上。此变量为静态的，以便跨调用缓存。
	 * 缓存是有益的，因为 GetFlushRecPtr() 需要获取一个竞争激烈的自旋锁。
	 */
	static XLogRecPtr flushPtr = InvalidXLogRecPtr;

	/*
	 * Don't know whether we've caught up yet. We'll set WalSndCaughtUp to
	 * true in WalSndWaitForWal, if we're actually waiting. We also set to
	 * true if XLogReadRecord() had to stop reading but WalSndWaitForWal
	 * didn't wait - i.e. when we're shutting down.
	 */
	/*
	 * 尚不知道是否已追赶上。若我们实际在等待，则在 WalSndWaitForWal 中将
	 * WalSndCaughtUp 设为 true。若 XLogReadRecord() 不得不停止读取但
	 * WalSndWaitForWal 未等待（即正在关闭时），也会设为 true。
	 */
	WalSndCaughtUp = false;

	record = XLogReadRecord(logical_decoding_ctx->reader, &errm);

	/* xlog record was invalid */
	/* xlog 记录无效 */
	if (errm != NULL)
		elog(ERROR, "could not find record while sending logically-decoded data: %s",
			 errm);

	if (record != NULL)
	{
		/*
		 * Note the lack of any call to LagTrackerWrite() which is handled by
		 * WalSndUpdateProgress which is called by output plugin through
		 * logical decoding write api.
		 */
		/*
		 * 注意此处没有调用 LagTrackerWrite()，这由 WalSndUpdateProgress 处理，
		 * 而 WalSndUpdateProgress 由输出插件通过逻辑解码写入 API 调用。
		 */
		LogicalDecodingProcessRecord(logical_decoding_ctx, logical_decoding_ctx->reader);

		sentPtr = logical_decoding_ctx->reader->EndRecPtr;
	}

	/*
	 * If first time through in this session, initialize flushPtr.  Otherwise,
	 * we only need to update flushPtr if EndRecPtr is past it.
	 */
	/*
	 * 若本会话中首次通过，则初始化 flushPtr。否则，只有当 EndRecPtr 超过 flushPtr 时
	 * 才需要更新 flushPtr。
	 */
	if (flushPtr == InvalidXLogRecPtr ||
		logical_decoding_ctx->reader->EndRecPtr >= flushPtr)
	{
		/*
		 * For cascading logical WAL senders, we use the replay LSN instead of
		 * the flush LSN, since logical decoding on a standby only processes
		 * WAL that has been replayed.  This distinction becomes particularly
		 * important during shutdown, as new WAL is no longer replayed and the
		 * last replayed LSN marks the furthest point up to which decoding can
		 * proceed.
		 */
		/*
		 * 对于级联逻辑 WAL 发送进程，我们使用重放 LSN 而不是刷新 LSN，
		 * 因为备服务器上的逻辑解码只处理已重放的 WAL。在关闭期间这一区别尤为重要，
		 * 因为新 WAL 不再被重放，最后重放的 LSN 标志着解码可以进行到的最远点。
		 */
		if (am_cascading_walsender)
			flushPtr = GetXLogReplayRecPtr(NULL);
		else
			flushPtr = GetFlushRecPtr(NULL);
	}

	/* If EndRecPtr is still past our flushPtr, it means we caught up. */
	/* 若 EndRecPtr 仍超过我们的 flushPtr，则意味着我们已追赶上。 */
	if (logical_decoding_ctx->reader->EndRecPtr >= flushPtr)
		WalSndCaughtUp = true;

	/*
	 * If we're caught up and have been requested to stop, have WalSndLoop()
	 * terminate the connection in an orderly manner, after writing out all
	 * the pending data.
	 */
	/*
	 * 若我们已追赶上且收到停止请求，让 WalSndLoop() 在写出所有待发送数据后
	 * 有序地终止连接。
	 */
	if (WalSndCaughtUp && got_STOPPING)
		got_SIGUSR2 = true;

	/* Update shared memory status */
	/* 更新共享内存状态 */
	{
		WalSnd	   *walsnd = MyWalSnd;

		SpinLockAcquire(&walsnd->mutex);
		walsnd->sentPtr = sentPtr;
		SpinLockRelease(&walsnd->mutex);
	}
}

/*
 * Shutdown if the sender is caught up.
 *
 * NB: This should only be called when the shutdown signal has been received
 * from postmaster.
 *
 * Note that if we determine that there's still more data to send, this
 * function will return control to the caller.
 */
/*
 * 若发送进程已追赶上，则关闭。
 *
 * 注意：只有在从 postmaster 收到关闭信号后才应调用此函数。
 *
 * 注意，若我们确定还有更多数据需要发送，此函数将把控制权返回给调用方。
 */
static void
WalSndDone(WalSndSendDataCallback send_data)
{
	XLogRecPtr	replicatedPtr;

	/* ... let's just be real sure we're caught up ... */
	/* ... 确保我们真的已经追赶上了 ... */
	send_data();

	/*
	 * To figure out whether all WAL has successfully been replicated, check
	 * flush location if valid, write otherwise. Tools like pg_receivewal will
	 * usually (unless in synchronous mode) return an invalid flush location.
	 */
	/*
	 * 要判断所有 WAL 是否已成功复制，若刷新位置有效则检查刷新位置，否则检查写入位置。
	 * 像 pg_receivewal 这样的工具通常（除非在同步模式下）会返回无效的刷新位置。
	 */
	replicatedPtr = XLogRecPtrIsInvalid(MyWalSnd->flush) ?
		MyWalSnd->write : MyWalSnd->flush;

	if (WalSndCaughtUp && sentPtr == replicatedPtr &&
		!pq_is_send_pending())
	{
		QueryCompletion qc;

		/* Inform the standby that XLOG streaming is done */
		/* 通知备服务器 XLOG 流式传输已完成 */
		SetQueryCompletion(&qc, CMDTAG_COPY, 0);
		EndCommand(&qc, DestRemote, false);
		pq_flush();

		proc_exit(0);
	}
	if (!waiting_for_ping_response)
		WalSndKeepalive(true, InvalidXLogRecPtr);
}

/*
 * Returns the latest point in WAL that has been safely flushed to disk.
 * This should only be called when in recovery.
 *
 * This is called either by cascading walsender to find WAL position to be sent
 * to a cascaded standby or by slot synchronization operation to validate remote
 * slot's lsn before syncing it locally.
 *
 * As a side-effect, *tli is updated to the TLI of the last
 * replayed WAL record.
 */
/*
 * 返回已安全刷新到磁盘的 WAL 中最新的点。此函数只应在恢复期间调用。
 *
 * 此函数由级联 walsender 调用以查找要发送给级联备服务器的 WAL 位置，
 * 或由槽同步操作调用以在本地同步之前验证远程槽的 LSN。
 *
 * 作为副作用，*tli 会更新为最后重放的 WAL 记录的 TLI。
 */
XLogRecPtr
GetStandbyFlushRecPtr(TimeLineID *tli)
{
	XLogRecPtr	replayPtr;
	TimeLineID	replayTLI;
	XLogRecPtr	receivePtr;
	TimeLineID	receiveTLI;
	XLogRecPtr	result;

	Assert(am_cascading_walsender || IsSyncingReplicationSlots());

	/*
	 * We can safely send what's already been replayed. Also, if walreceiver
	 * is streaming WAL from the same timeline, we can send anything that it
	 * has streamed, but hasn't been replayed yet.
	 */
	/*
	 * 我们可以安全地发送已重放的内容。此外，若 walreceiver 正在从同一时间线
	 * 流式传输 WAL，我们也可以发送它已流式传输但尚未重放的内容。
	 */

	receivePtr = GetWalRcvFlushRecPtr(NULL, &receiveTLI);
	replayPtr = GetXLogReplayRecPtr(&replayTLI);

	if (tli)
		*tli = replayTLI;

	result = replayPtr;
	if (receiveTLI == replayTLI && receivePtr > replayPtr)
		result = receivePtr;

	return result;
}

/*
 * Request walsenders to reload the currently-open WAL file
 */
/*
 * 请求 walsender 重新加载当前打开的 WAL 文件
 */
void
WalSndRqstFileReload(void)
{
	int			i;

	for (i = 0; i < max_wal_senders; i++)
	{
		WalSnd	   *walsnd = &WalSndCtl->walsnds[i];

		SpinLockAcquire(&walsnd->mutex);
		if (walsnd->pid == 0)
		{
			SpinLockRelease(&walsnd->mutex);
			continue;
		}
		walsnd->needreload = true;
		SpinLockRelease(&walsnd->mutex);
	}
}

/*
 * Handle PROCSIG_WALSND_INIT_STOPPING signal.
 */
/*
 * 处理 PROCSIG_WALSND_INIT_STOPPING 信号。
 */
void
HandleWalSndInitStopping(void)
{
	Assert(am_walsender);

	/*
	 * If replication has not yet started, die like with SIGTERM. If
	 * replication is active, only set a flag and wake up the main loop. It
	 * will send any outstanding WAL, wait for it to be replicated to the
	 * standby, and then exit gracefully.
	 */
	/*
	 * 若复制尚未开始，则像响应 SIGTERM 那样终止。若复制处于活跃状态，只需设置
	 * 标志并唤醒主循环。主循环将发送所有未发送的 WAL，等待其被复制到备服务器，
	 * 然后优雅退出。
	 */
	if (!replication_active)
		kill(MyProcPid, SIGTERM);
	else
		got_STOPPING = true;
}

/*
 * SIGUSR2: set flag to do a last cycle and shut down afterwards. The WAL
 * sender should already have been switched to WALSNDSTATE_STOPPING at
 * this point.
 */
/*
 * SIGUSR2：设置标志，执行最后一个周期后关闭。此时 WAL 发送进程应已切换到
 * WALSNDSTATE_STOPPING 状态。
 */
static void
WalSndLastCycleHandler(SIGNAL_ARGS)
{
	got_SIGUSR2 = true;
	SetLatch(MyLatch);
}

/* Set up signal handlers */
/* 设置信号处理程序 */
void
WalSndSignals(void)
{
	/* Set up signal handlers */
	/* 设置信号处理程序 */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGINT, StatementCancelHandler);	/* query cancel */
	pqsignal(SIGTERM, die);		/* request shutdown */
	/* SIGQUIT handler was already set up by InitPostmasterChild */
	/* SIGQUIT 处理程序已由 InitPostmasterChild 设置 */
	InitializeTimeouts();		/* establishes SIGALRM handler */
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, WalSndLastCycleHandler);	/* request a last cycle and
												 * shutdown */

	/* Reset some signals that are accepted by postmaster but not here */
	/* 重置一些 postmaster 接受但此处不接受的信号 */
	pqsignal(SIGCHLD, SIG_DFL);
}

/* Report shared-memory space needed by WalSndShmemInit */
/* 报告 WalSndShmemInit 所需的共享内存空间 */
Size
WalSndShmemSize(void)
{
	Size		size = 0;

	size = offsetof(WalSndCtlData, walsnds);
	size = add_size(size, mul_size(max_wal_senders, sizeof(WalSnd)));

	return size;
}

/* Allocate and initialize walsender-related shared memory */
/* 分配并初始化与 walsender 相关的共享内存 */
void
WalSndShmemInit(void)
{
	bool		found;
	int			i;

	WalSndCtl = (WalSndCtlData *)
		ShmemInitStruct("Wal Sender Ctl", WalSndShmemSize(), &found);

	if (!found)
	{
		/* First time through, so initialize */
		/* 第一次进入，执行初始化 */
		MemSet(WalSndCtl, 0, WalSndShmemSize());

		for (i = 0; i < NUM_SYNC_REP_WAIT_MODE; i++)
			dlist_init(&(WalSndCtl->SyncRepQueue[i]));

		for (i = 0; i < max_wal_senders; i++)
		{
			WalSnd	   *walsnd = &WalSndCtl->walsnds[i];

			SpinLockInit(&walsnd->mutex);
		}

		ConditionVariableInit(&WalSndCtl->wal_flush_cv);
		ConditionVariableInit(&WalSndCtl->wal_replay_cv);
		ConditionVariableInit(&WalSndCtl->wal_confirm_rcv_cv);
	}
}

/*
 * Wake up physical, logical or both kinds of walsenders
 *
 * The distinction between physical and logical walsenders is done, because:
 * - physical walsenders can't send data until it's been flushed
 * - logical walsenders on standby can't decode and send data until it's been
 *   applied
 *
 * For cascading replication we need to wake up physical walsenders separately
 * from logical walsenders (see the comment before calling WalSndWakeup() in
 * ApplyWalRecord() for more details).
 *
 * This will be called inside critical sections, so throwing an error is not
 * advisable.
 */
/*
 * 唤醒物理、逻辑或两种 walsender
 *
 * 区分物理和逻辑 walsender 的原因是：
 * - 物理 walsender 在数据被刷新之前不能发送
 * - 备服务器上的逻辑 walsender 在数据被应用之前不能解码和发送
 *
 * 对于级联复制，我们需要将物理 walsender 与逻辑 walsender 分开唤醒
 * （更多详情参见 ApplyWalRecord() 中调用 WalSndWakeup() 之前的注释）。
 *
 * 此函数将在关键区内被调用，因此不建议抛出错误。
 */
void
WalSndWakeup(bool physical, bool logical)
{
	/*
	 * Wake up all the walsenders waiting on WAL being flushed or replayed
	 * respectively.  Note that waiting walsender would have prepared to sleep
	 * on the CV (i.e., added itself to the CV's waitlist) in WalSndWait()
	 * before actually waiting.
	 */
	/*
	 * 分别唤醒所有等待 WAL 被刷新或重放的 walsender。注意，等待中的 walsender
	 * 在实际等待之前，已在 WalSndWait() 中准备好在条件变量上休眠
	 * （即已将自己添加到条件变量的等待列表中）。
	 */
	if (physical)
		ConditionVariableBroadcast(&WalSndCtl->wal_flush_cv);

	if (logical)
		ConditionVariableBroadcast(&WalSndCtl->wal_replay_cv);
}

/*
 * Wait for readiness on the FeBe socket, or a timeout.  The mask should be
 * composed of optional WL_SOCKET_WRITEABLE and WL_SOCKET_READABLE flags.  Exit
 * on postmaster death.
 */
/*
 * 等待 FeBe 套接字就绪或超时。掩码应由可选的 WL_SOCKET_WRITEABLE 和
 * WL_SOCKET_READABLE 标志组成。postmaster 终止时退出。
 */
static void
WalSndWait(uint32 socket_events, long timeout, uint32 wait_event)
{
	WaitEvent	event;

	ModifyWaitEvent(FeBeWaitSet, FeBeWaitSetSocketPos, socket_events, NULL);

	/*
	 * We use a condition variable to efficiently wake up walsenders in
	 * WalSndWakeup().
	 *
	 * Every walsender prepares to sleep on a shared memory CV. Note that it
	 * just prepares to sleep on the CV (i.e., adds itself to the CV's
	 * waitlist), but does not actually wait on the CV (IOW, it never calls
	 * ConditionVariableSleep()). It still uses WaitEventSetWait() for
	 * waiting, because we also need to wait for socket events. The processes
	 * (startup process, walreceiver etc.) wanting to wake up walsenders use
	 * ConditionVariableBroadcast(), which in turn calls SetLatch(), helping
	 * walsenders come out of WaitEventSetWait().
	 *
	 * This approach is simple and efficient because, one doesn't have to loop
	 * through all the walsenders slots, with a spinlock acquisition and
	 * release for every iteration, just to wake up only the waiting
	 * walsenders. It makes WalSndWakeup() callers' life easy.
	 *
	 * XXX: A desirable future improvement would be to add support for CVs
	 * into WaitEventSetWait().
	 *
	 * And, we use separate shared memory CVs for physical and logical
	 * walsenders for selective wake ups, see WalSndWakeup() for more details.
	 *
	 * If the wait event is WAIT_FOR_STANDBY_CONFIRMATION, wait on another CV
	 * until awakened by physical walsenders after the walreceiver confirms
	 * the receipt of the LSN.
	 */
	/*
	 * 我们使用条件变量在 WalSndWakeup() 中高效地唤醒 walsender。
	 *
	 * 每个 walsender 准备在共享内存条件变量上休眠。注意它只是准备在条件变量上
	 * 休眠（即将自己添加到条件变量的等待列表），而不实际在条件变量上等待
	 * （IOW，它从不调用 ConditionVariableSleep()）。它仍然使用 WaitEventSetWait()
	 * 来等待，因为我们也需要等待套接字事件。想要唤醒 walsender 的进程
	 * （启动进程、walreceiver 等）使用 ConditionVariableBroadcast()，
	 * 它反过来调用 SetLatch()，帮助 walsender 从 WaitEventSetWait() 中退出。
	 *
	 * 这种方法简单高效，因为不需要在每次迭代都获取和释放自旋锁来遍历所有
	 * walsender 槽，只需唤醒正在等待的 walsender 即可。这让 WalSndWakeup()
	 * 的调用方轻松许多。
	 *
	 * XXX：未来希望改进的方向是在 WaitEventSetWait() 中添加对条件变量的支持。
	 *
	 * 我们对物理和逻辑 walsender 使用单独的共享内存条件变量以实现选择性唤醒，
	 * 详情参见 WalSndWakeup()。
	 *
	 * 若等待事件是 WAIT_FOR_STANDBY_CONFIRMATION，则在另一个条件变量上等待，
	 * 直到 walreceiver 确认收到 LSN 后被物理 walsender 唤醒。
	 */
	if (wait_event == WAIT_EVENT_WAIT_FOR_STANDBY_CONFIRMATION)
		ConditionVariablePrepareToSleep(&WalSndCtl->wal_confirm_rcv_cv);
	else if (MyWalSnd->kind == REPLICATION_KIND_PHYSICAL)
		ConditionVariablePrepareToSleep(&WalSndCtl->wal_flush_cv);
	else if (MyWalSnd->kind == REPLICATION_KIND_LOGICAL)
		ConditionVariablePrepareToSleep(&WalSndCtl->wal_replay_cv);

	if (WaitEventSetWait(FeBeWaitSet, timeout, &event, 1, wait_event) == 1 &&
		(event.events & WL_POSTMASTER_DEATH))
	{
		ConditionVariableCancelSleep();
		proc_exit(1);
	}

	ConditionVariableCancelSleep();
}

/*
 * Signal all walsenders to move to stopping state.
 *
 * This will trigger walsenders to move to a state where no further WAL can be
 * generated. See this file's header for details.
 */
/*
 * 向所有 walsender 发出信号，使其进入 stopping 状态。
 *
 * 这将触发 walsender 进入一个不能再生成 WAL 的状态。详情参见本文件头部。
 */
void
WalSndInitStopping(void)
{
	int			i;

	for (i = 0; i < max_wal_senders; i++)
	{
		WalSnd	   *walsnd = &WalSndCtl->walsnds[i];
		pid_t		pid;

		SpinLockAcquire(&walsnd->mutex);
		pid = walsnd->pid;
		SpinLockRelease(&walsnd->mutex);

		if (pid == 0)
			continue;

		SendProcSignal(pid, PROCSIG_WALSND_INIT_STOPPING, INVALID_PROC_NUMBER);
	}
}

/*
 * Wait that all the WAL senders have quit or reached the stopping state. This
 * is used by the checkpointer to control when the shutdown checkpoint can
 * safely be performed.
 */
/*
 * 等待所有 WAL 发送进程退出或到达 stopping 状态。此函数由 checkpointer 使用，
 * 以控制何时可以安全地执行关闭检查点。
 */
void
WalSndWaitStopping(void)
{
	for (;;)
	{
		int			i;
		bool		all_stopped = true;

		for (i = 0; i < max_wal_senders; i++)
		{
			WalSnd	   *walsnd = &WalSndCtl->walsnds[i];

			SpinLockAcquire(&walsnd->mutex);

			if (walsnd->pid == 0)
			{
				SpinLockRelease(&walsnd->mutex);
				continue;
			}

			if (walsnd->state != WALSNDSTATE_STOPPING)
			{
				all_stopped = false;
				SpinLockRelease(&walsnd->mutex);
				break;
			}
			SpinLockRelease(&walsnd->mutex);
		}

		/* safe to leave if confirmation is done for all WAL senders */
		/* 若所有 WAL 发送进程的确认都已完成，则可以安全退出 */
		if (all_stopped)
			return;

		pg_usleep(10000L);		/* wait for 10 msec */
	}
}

/* Set state for current walsender (only called in walsender) */
/* 设置当前 walsender 的状态（仅在 walsender 中调用） */
void
WalSndSetState(WalSndState state)
{
	WalSnd	   *walsnd = MyWalSnd;

	Assert(am_walsender);

	if (walsnd->state == state)
		return;

	SpinLockAcquire(&walsnd->mutex);
	walsnd->state = state;
	SpinLockRelease(&walsnd->mutex);
}

/*
 * Return a string constant representing the state. This is used
 * in system views, and should *not* be translated.
 */
/*
 * 返回表示状态的字符串常量。此函数用于系统视图，不应被翻译。
 */
static const char *
WalSndGetStateString(WalSndState state)
{
	switch (state)
	{
		case WALSNDSTATE_STARTUP:
			return "startup";
		case WALSNDSTATE_BACKUP:
			return "backup";
		case WALSNDSTATE_CATCHUP:
			return "catchup";
		case WALSNDSTATE_STREAMING:
			return "streaming";
		case WALSNDSTATE_STOPPING:
			return "stopping";
	}
	return "UNKNOWN";
}

static Interval *
offset_to_interval(TimeOffset offset)
{
	Interval   *result = palloc(sizeof(Interval));

	result->month = 0;
	result->day = 0;
	result->time = offset;

	return result;
}

/*
 * Returns activity of walsenders, including pids and xlog locations sent to
 * standby servers.
 */
/*
 * 返回 walsender 的活动信息，包括 pid 和发送到备服务器的 xlog 位置。
 */
Datum
pg_stat_get_wal_senders(PG_FUNCTION_ARGS)
{
#define PG_STAT_GET_WAL_SENDERS_COLS	12
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	SyncRepStandbyData *sync_standbys;
	int			num_standbys;
	int			i;

	InitMaterializedSRF(fcinfo, 0);

	/*
	 * Get the currently active synchronous standbys.  This could be out of
	 * date before we're done, but we'll use the data anyway.
	 */
	/*
	 * 获取当前活跃的同步备服务器。在我们完成之前这可能已过时，但我们仍然使用这些数据。
	 */
	num_standbys = SyncRepGetCandidateStandbys(&sync_standbys);

	for (i = 0; i < max_wal_senders; i++)
	{
		WalSnd	   *walsnd = &WalSndCtl->walsnds[i];
		XLogRecPtr	sent_ptr;
		XLogRecPtr	write;
		XLogRecPtr	flush;
		XLogRecPtr	apply;
		TimeOffset	writeLag;
		TimeOffset	flushLag;
		TimeOffset	applyLag;
		int			priority;
		int			pid;
		WalSndState state;
		TimestampTz replyTime;
		bool		is_sync_standby;
		Datum		values[PG_STAT_GET_WAL_SENDERS_COLS];
		bool		nulls[PG_STAT_GET_WAL_SENDERS_COLS] = {0};
		int			j;

		/* Collect data from shared memory */
		/* 从共享内存收集数据 */
		SpinLockAcquire(&walsnd->mutex);
		if (walsnd->pid == 0)
		{
			SpinLockRelease(&walsnd->mutex);
			continue;
		}
		pid = walsnd->pid;
		sent_ptr = walsnd->sentPtr;
		state = walsnd->state;
		write = walsnd->write;
		flush = walsnd->flush;
		apply = walsnd->apply;
		writeLag = walsnd->writeLag;
		flushLag = walsnd->flushLag;
		applyLag = walsnd->applyLag;
		priority = walsnd->sync_standby_priority;
		replyTime = walsnd->replyTime;
		SpinLockRelease(&walsnd->mutex);

		/*
		 * Detect whether walsender is/was considered synchronous.  We can
		 * provide some protection against stale data by checking the PID
		 * along with walsnd_index.
		 */
		/*
		 * 检测 walsender 是否/曾经被视为同步的。通过同时检查 PID 和 walsnd_index，
		 * 可以提供一定程度的过时数据保护。
		 */
		is_sync_standby = false;
		for (j = 0; j < num_standbys; j++)
		{
			if (sync_standbys[j].walsnd_index == i &&
				sync_standbys[j].pid == pid)
			{
				is_sync_standby = true;
				break;
			}
		}

		values[0] = Int32GetDatum(pid);

		if (!has_privs_of_role(GetUserId(), ROLE_PG_READ_ALL_STATS))
		{
			/*
			 * Only superusers and roles with privileges of pg_read_all_stats
			 * can see details. Other users only get the pid value to know
			 * it's a walsender, but no details.
			 */
			/*
			 * 只有超级用户和具有 pg_read_all_stats 权限的角色才能查看详情。
			 * 其他用户只能获取 pid 值以知道这是一个 walsender，但无法获取详情。
			 */
			MemSet(&nulls[1], true, PG_STAT_GET_WAL_SENDERS_COLS - 1);
		}
		else
		{
			values[1] = CStringGetTextDatum(WalSndGetStateString(state));

			if (XLogRecPtrIsInvalid(sent_ptr))
				nulls[2] = true;
			values[2] = LSNGetDatum(sent_ptr);

			if (XLogRecPtrIsInvalid(write))
				nulls[3] = true;
			values[3] = LSNGetDatum(write);

			if (XLogRecPtrIsInvalid(flush))
				nulls[4] = true;
			values[4] = LSNGetDatum(flush);

			if (XLogRecPtrIsInvalid(apply))
				nulls[5] = true;
			values[5] = LSNGetDatum(apply);

			/*
			 * Treat a standby such as a pg_basebackup background process
			 * which always returns an invalid flush location, as an
			 * asynchronous standby.
			 */
			priority = XLogRecPtrIsInvalid(flush) ? 0 : priority;

			if (writeLag < 0)
				nulls[6] = true;
			else
				values[6] = IntervalPGetDatum(offset_to_interval(writeLag));

			if (flushLag < 0)
				nulls[7] = true;
			else
				values[7] = IntervalPGetDatum(offset_to_interval(flushLag));

			if (applyLag < 0)
				nulls[8] = true;
			else
				values[8] = IntervalPGetDatum(offset_to_interval(applyLag));

			values[9] = Int32GetDatum(priority);

			/*
			 * More easily understood version of standby state. This is purely
			 * informational.
			 *
			 * In quorum-based sync replication, the role of each standby
			 * listed in synchronous_standby_names can be changing very
			 * frequently. Any standbys considered as "sync" at one moment can
			 * be switched to "potential" ones at the next moment. So, it's
			 * basically useless to report "sync" or "potential" as their sync
			 * states. We report just "quorum" for them.
			 */
			/*
			 * 更易理解的备服务器状态版本，纯粹用于信息展示。
			 *
			 * 在基于法定人数的同步复制中，synchronous_standby_names 中列出的
			 * 每个备服务器的角色可能非常频繁地变化。某一时刻被视为 "sync" 的备服务器
			 * 可能在下一时刻切换为 "potential"。因此，报告 "sync" 或 "potential" 作为
			 * 其同步状态基本上是没有意义的。我们只报告 "quorum"。
			 */
			if (priority == 0)
				values[10] = CStringGetTextDatum("async");
			else if (is_sync_standby)
				values[10] = SyncRepConfig->syncrep_method == SYNC_REP_PRIORITY ?
					CStringGetTextDatum("sync") : CStringGetTextDatum("quorum");
			else
				values[10] = CStringGetTextDatum("potential");

			if (replyTime == 0)
				nulls[11] = true;
			else
				values[11] = TimestampTzGetDatum(replyTime);
		}

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
							 values, nulls);
	}

	return (Datum) 0;
}

/*
 * Send a keepalive message to standby.
 *
 * If requestReply is set, the message requests the other party to send
 * a message back to us, for heartbeat purposes.  We also set a flag to
 * let nearby code know that we're waiting for that response, to avoid
 * repeated requests.
 *
 * writePtr is the location up to which the WAL is sent. It is essentially
 * the same as sentPtr but in some cases, we need to send keep alive before
 * sentPtr is updated like when skipping empty transactions.
 */
/*
 * 向备服务器发送 keepalive 消息。
 *
 * 若 requestReply 设置，则消息请求对方回发一条消息，用于心跳检测。
 * 我们也设置一个标志，让附近的代码知道我们在等待该响应，以避免重复请求。
 *
 * writePtr 是 WAL 已发送到的位置。它本质上与 sentPtr 相同，但在某些情况下，
 * 例如跳过空事务时，我们需要在 sentPtr 更新之前发送 keepalive。
 */
static void
WalSndKeepalive(bool requestReply, XLogRecPtr writePtr)
{
	elog(DEBUG2, "sending replication keepalive");

	/* construct the message... */
	/* 构造消息…… */
	resetStringInfo(&output_message);
	pq_sendbyte(&output_message, 'k');
	pq_sendint64(&output_message, XLogRecPtrIsInvalid(writePtr) ? sentPtr : writePtr);
	pq_sendint64(&output_message, GetCurrentTimestamp());
	pq_sendbyte(&output_message, requestReply ? 1 : 0);

	/* ... and send it wrapped in CopyData */
	/* …… 并封装在 CopyData 中发送 */
	pq_putmessage_noblock('d', output_message.data, output_message.len);

	/* Set local flag */
	/* 设置本地标志 */
	if (requestReply)
		waiting_for_ping_response = true;
}

/*
 * Send keepalive message if too much time has elapsed.
 */
/*
 * 若已经过了太长时间，则发送 keepalive 消息。
 */
static void
WalSndKeepaliveIfNecessary(void)
{
	TimestampTz ping_time;

	/*
	 * Don't send keepalive messages if timeouts are globally disabled or
	 * we're doing something not partaking in timeouts.
	 */
	/*
	 * 若超时在全局禁用，或我们正在执行不参与超时的操作，则不发送 keepalive 消息。
	 */
	if (wal_sender_timeout <= 0 || last_reply_timestamp <= 0)
		return;

	if (waiting_for_ping_response)
		return;

	/*
	 * If half of wal_sender_timeout has lapsed without receiving any reply
	 * from the standby, send a keep-alive message to the standby requesting
	 * an immediate reply.
	 */
	/*
	 * 若在没有收到备服务器任何回复的情况下 wal_sender_timeout 的一半时间已过，
	 * 则向备服务器发送 keep-alive 消息，请求立即回复。
	 */
	ping_time = TimestampTzPlusMilliseconds(last_reply_timestamp,
											wal_sender_timeout / 2);
	if (last_processing >= ping_time)
	{
		WalSndKeepalive(true, InvalidXLogRecPtr);

		/* Try to flush pending output to the client */
		/* 尝试将待发送输出刷新给客户端 */
		if (pq_flush_if_writable() != 0)
			WalSndShutdown();
	}
}

/*
 * Record the end of the WAL and the time it was flushed locally, so that
 * LagTrackerRead can compute the elapsed time (lag) when this WAL location is
 * eventually reported to have been written, flushed and applied by the
 * standby in a reply message.
 */
/*
 * 记录 WAL 的末尾及其在本地被刷新的时间，以便 LagTrackerRead 在该 WAL 位置最终
 * 被备服务器在回复消息中报告为已写入、已刷新和已应用时，能够计算经过的时间（延迟）。
 */
static void
LagTrackerWrite(XLogRecPtr lsn, TimestampTz local_flush_time)
{
	int			new_write_head;
	int			i;

	if (!am_walsender)
		return;

	/*
	 * If the lsn hasn't advanced since last time, then do nothing.  This way
	 * we only record a new sample when new WAL has been written.
	 */
	/*
	 * 若 LSN 自上次以来没有推进，则不执行任何操作。这样我们只在有新 WAL 写入时
	 * 才记录新的采样。
	 */
	if (lag_tracker->last_lsn == lsn)
		return;
	lag_tracker->last_lsn = lsn;

	/*
	 * If advancing the write head of the circular buffer would crash into any
	 * of the read heads, then the buffer is full.  In other words, the
	 * slowest reader (presumably apply) is the one that controls the release
	 * of space.
	 */
	/*
	 * 若推进循环缓冲区的写头会与任何读头碰撞，则缓冲区已满。换句话说，
	 * 最慢的读者（可能是应用）控制着空间的释放。
	 */
	new_write_head = (lag_tracker->write_head + 1) % LAG_TRACKER_BUFFER_SIZE;
	for (i = 0; i < NUM_SYNC_REP_WAIT_MODE; ++i)
	{
		/*
		 * If the buffer is full, move the slowest reader to a separate
		 * overflow entry and free its space in the buffer so the write head
		 * can advance.
		 */
		/*
		 * 若缓冲区已满，将最慢的读者移到单独的溢出条目中，并释放其在缓冲区中
		 * 的空间，以便写头可以继续前进。
		 */
		if (new_write_head == lag_tracker->read_heads[i])
		{
			lag_tracker->overflowed[i] =
				lag_tracker->buffer[lag_tracker->read_heads[i]];
			lag_tracker->read_heads[i] = -1;
		}
	}

	/* Store a sample at the current write head position. */
	/* 在当前写头位置存储一个采样。 */
	lag_tracker->buffer[lag_tracker->write_head].lsn = lsn;
	lag_tracker->buffer[lag_tracker->write_head].time = local_flush_time;
	lag_tracker->write_head = new_write_head;
}

/*
 * Find out how much time has elapsed between the moment WAL location 'lsn'
 * (or the highest known earlier LSN) was flushed locally and the time 'now'.
 * We have a separate read head for each of the reported LSN locations we
 * receive in replies from standby; 'head' controls which read head is
 * used.  Whenever a read head crosses an LSN which was written into the
 * lag buffer with LagTrackerWrite, we can use the associated timestamp to
 * find out the time this LSN (or an earlier one) was flushed locally, and
 * therefore compute the lag.
 *
 * Return -1 if no new sample data is available, and otherwise the elapsed
 * time in microseconds.
 */
/*
 * 找出 WAL 位置 'lsn'（或已知的最高早期 LSN）在本地被刷新的那一刻到 'now' 之间
 * 经过了多少时间。我们对从备服务器回复中收到的每个报告的 LSN 位置都有单独的读头；
 * 'head' 控制使用哪个读头。每当读头越过通过 LagTrackerWrite 写入延迟缓冲区的 LSN 时，
 * 我们可以使用关联的时间戳来找出该 LSN（或更早的 LSN）在本地被刷新的时间，
 * 从而计算延迟。
 *
 * 若没有新的采样数据可用，则返回 -1；否则返回经过的时间（以微秒为单位）。
 */
static TimeOffset
LagTrackerRead(int head, XLogRecPtr lsn, TimestampTz now)
{
	TimestampTz time = 0;

	/*
	 * If 'lsn' has not passed the WAL position stored in the overflow entry,
	 * return the elapsed time (in microseconds) since the saved local flush
	 * time. If the flush time is in the future (due to clock drift), return
	 * -1 to treat as no valid sample.
	 *
	 * Otherwise, switch back to using the buffer to control the read head and
	 * compute the elapsed time.  The read head is then reset to point to the
	 * oldest entry in the buffer.
	 */
	/*
	 * 若 'lsn' 尚未超过溢出条目中存储的 WAL 位置，则返回自保存的本地刷新时间以来
	 * 经过的时间（以微秒为单位）。若刷新时间在未来（由于时钟漂移），则返回 -1
	 * 表示没有有效采样。
	 *
	 * 否则，切回使用缓冲区来控制读头并计算经过的时间。读头随后被重置为指向缓冲区
	 * 中最旧的条目。
	 */
	if (lag_tracker->read_heads[head] == -1)
	{
		if (lag_tracker->overflowed[head].lsn > lsn)
			return (now >= lag_tracker->overflowed[head].time) ?
				now - lag_tracker->overflowed[head].time : -1;

		time = lag_tracker->overflowed[head].time;
		lag_tracker->last_read[head] = lag_tracker->overflowed[head];
		lag_tracker->read_heads[head] =
			(lag_tracker->write_head + 1) % LAG_TRACKER_BUFFER_SIZE;
	}

	/* Read all unread samples up to this LSN or end of buffer. */
	/* 读取所有未读采样，直到此 LSN 或缓冲区末尾。 */
	while (lag_tracker->read_heads[head] != lag_tracker->write_head &&
		   lag_tracker->buffer[lag_tracker->read_heads[head]].lsn <= lsn)
	{
		time = lag_tracker->buffer[lag_tracker->read_heads[head]].time;
		lag_tracker->last_read[head] =
			lag_tracker->buffer[lag_tracker->read_heads[head]];
		lag_tracker->read_heads[head] =
			(lag_tracker->read_heads[head] + 1) % LAG_TRACKER_BUFFER_SIZE;
	}

	/*
	 * If the lag tracker is empty, that means the standby has processed
	 * everything we've ever sent so we should now clear 'last_read'.  If we
	 * didn't do that, we'd risk using a stale and irrelevant sample for
	 * interpolation at the beginning of the next burst of WAL after a period
	 * of idleness.
	 */
	/*
	 * 若延迟跟踪器为空，则意味着备服务器已处理了我们发送的所有内容，
	 * 因此我们现在应清除 'last_read'。若不这样做，在一段空闲期后，
	 * 我们可能会在下一批 WAL 开始时使用陈旧且无关的采样进行插值。
	 */
	if (lag_tracker->read_heads[head] == lag_tracker->write_head)
		lag_tracker->last_read[head].time = 0;

	if (time > now)
	{
		/* If the clock somehow went backwards, treat as not found. */
		/* 若时钟以某种方式向后走了，则视为未找到。 */
		return -1;
	}
	else if (time == 0)
	{
		/*
		 * We didn't cross a time.  If there is a future sample that we
		 * haven't reached yet, and we've already reached at least one sample,
		 * let's interpolate the local flushed time.  This is mainly useful
		 * for reporting a completely stuck apply position as having
		 * increasing lag, since otherwise we'd have to wait for it to
		 * eventually start moving again and cross one of our samples before
		 * we can show the lag increasing.
		 */
		/*
		 * 我们没有越过任何时间点。若有我们尚未到达的未来采样，且我们已至少到达一个采样，
		 * 我们来插值本地刷新时间。这主要用于将完全卡住的应用位置报告为延迟增加，
		 * 否则我们必须等待它最终重新开始移动并越过我们的某个采样，才能显示延迟增加。
		 */
		if (lag_tracker->read_heads[head] == lag_tracker->write_head)
		{
			/* There are no future samples, so we can't interpolate. */
			/* 没有未来的采样，所以我们无法插值。 */
			return -1;
		}
		else if (lag_tracker->last_read[head].time != 0)
		{
			/* We can interpolate between last_read and the next sample. */
			/* 我们可以在 last_read 和下一个采样之间进行插值。 */
			double		fraction;
			WalTimeSample prev = lag_tracker->last_read[head];
			WalTimeSample next = lag_tracker->buffer[lag_tracker->read_heads[head]];

			if (lsn < prev.lsn)
			{
				/*
				 * Reported LSNs shouldn't normally go backwards, but it's
				 * possible when there is a timeline change.  Treat as not
				 * found.
				 */
				/*
				 * 报告的 LSN 通常不应向后走，但在时间线变化时是可能的。
				 * 视为未找到。
				 */
				return -1;
			}

			Assert(prev.lsn < next.lsn);

			if (prev.time > next.time)
			{
				/* If the clock somehow went backwards, treat as not found. */
				/* 若时钟以某种方式向后走了，则视为未找到。 */
				return -1;
			}

			/* See how far we are between the previous and next samples. */
			/* 查看我们在前一个和后一个采样之间的位置。 */
			fraction =
				(double) (lsn - prev.lsn) / (double) (next.lsn - prev.lsn);

			/* Scale the local flush time proportionally. */
			/* 按比例缩放本地刷新时间。 */
			time = (TimestampTz)
				((double) prev.time + (next.time - prev.time) * fraction);
		}
		else
		{
			/*
			 * We have only a future sample, implying that we were entirely
			 * caught up but and now there is a new burst of WAL and the
			 * standby hasn't processed the first sample yet.  Until the
			 * standby reaches the future sample the best we can do is report
			 * the hypothetical lag if that sample were to be replayed now.
			 */
			/*
			 * 我们只有一个未来采样，这意味着我们之前已完全追赶上，但现在有新的一批 WAL，
			 * 备服务器尚未处理第一个采样。在备服务器到达未来采样之前，我们能做的最好的
			 * 事情是报告如果该采样现在被重放的假设延迟。
			 */
			time = lag_tracker->buffer[lag_tracker->read_heads[head]].time;
		}
	}

	/* Return the elapsed time since local flush time in microseconds. */
	/* 返回自本地刷新时间以来经过的时间（以微秒为单位）。 */
	Assert(time != 0);
	return now - time;
}

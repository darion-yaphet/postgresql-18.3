/*-------------------------------------------------------------------------
 *
 * walreceiver.c
 *
 * The WAL receiver process (walreceiver) is new as of Postgres 9.0. It
 * is the process in the standby server that takes charge of receiving
 * XLOG records from a primary server during streaming replication.
 *
 * WAL 接收进程（walreceiver）自 Postgres 9.0 起新增。
 * 它运行在备库上，负责在流式复制期间从主库接收 XLOG 记录。
 *
 * When the startup process determines that it's time to start streaming,
 * it instructs postmaster to start walreceiver. Walreceiver first connects
 * to the primary server (it will be served by a walsender process
 * in the primary server), and then keeps receiving XLOG records and
 * writing them to the disk as long as the connection is alive. As XLOG
 * records are received and flushed to disk, it updates the
 * WalRcv->flushedUpto variable in shared memory, to inform the startup
 * process of how far it can proceed with XLOG replay.
 *
 * 当 startup 进程确定应当开始流式复制时，它指示 postmaster 启动
 * walreceiver。walreceiver 先连接主库（由主库上的
 * walsender 进程提供服务），并在连接存活期间持续接收 XLOG 记录、写入磁盘。
 * 记录被接收并刷盘后，更新共享内存变量 WalRcv->flushedUpto，告知
 * startup 进程 XLOG 回放可以推进到何处。
 *
 * A WAL receiver cannot directly load GUC parameters used when establishing
 * its connection to the primary. Instead it relies on parameter values
 * that are passed down by the startup process when streaming is requested.
 * This applies, for example, to the replication slot and the connection
 * string to be used for the connection with the primary.
 *
 * walreceiver 不能直接加载建立到主库连接时使用的 GUC 参数，而是依赖
 * startup 进程在请求流式复制时传下来的参数值。例如复制槽，
 * 以及与主库连接所用的连接串。
 *
 * If the primary server ends streaming, but doesn't disconnect, walreceiver
 * goes into "waiting" mode, and waits for the startup process to give new
 * instructions. The startup process will treat that the same as
 * disconnection, and will rescan the archive/pg_wal directory. But when the
 * startup process wants to try streaming replication again, it will just
 * nudge the existing walreceiver process that's waiting, instead of launching
 * a new one.
 *
 * 若主库结束流式传输但未断开连接，walreceiver 进入 "waiting" 模式，
 * 等待 startup 进程给出新指示。startup 进程会把它当作断线处理，并重新扫描
 * archive/pg_wal 目录。当 startup 进程准备再次尝试流式复制时，
 * 只会唤醒正在等待的现有 walreceiver，而不会另起一个新进程。
 *
 * Normal termination is by SIGTERM, which instructs the walreceiver to
 * exit(0). Emergency termination is by SIGQUIT; like any postmaster child
 * process, the walreceiver will simply abort and exit on SIGQUIT. A close
 * of the connection and a FATAL error are treated not as a crash but as
 * normal operation.
 *
 * 正常终止由 SIGTERM 触发，指示 walreceiver 执行 exit(0)。
 * 紧急终止由 SIGQUIT 触发；与任何 postmaster 子进程一样，
 * walreceiver 收到 SIGQUIT 会直接中止并退出。连接关闭和 FATAL
 * 错误不视为崩溃，而视为正常操作。
 *
 * This file contains the server-facing parts of walreceiver. The libpq-
 * specific parts are in the libpqwalreceiver module. It's loaded
 * dynamically to avoid linking the server with libpq.
 *
 * 本文件包含 walreceiver 面向服务端的部分。与 libpq 相关的部分位于
 * libpqwalreceiver 模块中，动态加载以避免服务器链接 libpq。
 *
 * Portions Copyright (c) 2010-2025, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/replication/walreceiver.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "access/htup_details.h"
#include "access/timeline.h"
#include "access/transam.h"
#include "access/xlog_internal.h"
#include "access/xlogarchive.h"
#include "access/xlogrecovery.h"
#include "catalog/pg_authid.h"
#include "funcapi.h"
#include "libpq/pqformat.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/auxprocess.h"
#include "postmaster/interrupt.h"
#include "replication/walreceiver.h"
#include "replication/walsender.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/procsignal.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/pg_lsn.h"
#include "utils/ps_status.h"
#include "utils/timestamp.h"


/*
 * GUC variables.  (Other variables that affect walreceiver are in xlog.c
 * because they're passed down from the startup process, for better
 * synchronization.)
 *
 * GUC 变量。（其他影响 walreceiver 的变量在 xlog.c 中，因为它们由
 * startup 进程下发，以便更好地同步。）
 */
int			wal_receiver_status_interval;
int			wal_receiver_timeout;
bool		hot_standby_feedback;

/* libpqwalreceiver connection
 *
 * libpqwalreceiver 连接。
 */
static WalReceiverConn *wrconn = NULL;
WalReceiverFunctionsType *WalReceiverFunctions = NULL;

/*
 * These variables are used similarly to openLogFile/SegNo,
 * but for walreceiver to write the XLOG. recvFileTLI is the TimeLineID
 * corresponding the filename of recvFile.
 *
 * 这些变量的用法类似于 openLogFile/SegNo，但用于 walreceiver
 * 写入 XLOG。recvFileTLI 是 recvFile 文件名对应的
 * TimeLineID。
 */
static int	recvFile = -1;
static TimeLineID recvFileTLI = 0;
static XLogSegNo recvSegNo = 0;

/*
 * LogstreamResult indicates the byte positions that we have already
 * written/fsynced.
 *
 * LogstreamResult 表示已经写入或 fsync 过的字节位置。
 */
static struct
{
	XLogRecPtr	Write;			/* last byte + 1 written out in the standby
								 *
								 * 备库上已写出的最后字节加 1。
								 */
	XLogRecPtr	Flush;			/* last byte + 1 flushed in the standby
								 *
								 * 备库上已刷盘的最后字节加 1。
								 */
}			LogstreamResult;

/*
 * Reasons to wake up and perform periodic tasks.
 *
 * 唤醒并执行周期性任务的原因。
 */
typedef enum WalRcvWakeupReason
{
	WALRCV_WAKEUP_TERMINATE,
	WALRCV_WAKEUP_PING,
	WALRCV_WAKEUP_REPLY,
	WALRCV_WAKEUP_HSFEEDBACK,
#define NUM_WALRCV_WAKEUPS (WALRCV_WAKEUP_HSFEEDBACK + 1)
} WalRcvWakeupReason;

/*
 * Wake up times for periodic tasks.
 *
 * 周期性任务的唤醒时间。
 */
static TimestampTz wakeup[NUM_WALRCV_WAKEUPS];

static StringInfoData reply_message;

/* Prototypes for private functions
 *
 * 私有函数原型。
 */
static void WalRcvFetchTimeLineHistoryFiles(TimeLineID first, TimeLineID last);
static void WalRcvWaitForStartPosition(XLogRecPtr *startpoint, TimeLineID *startpointTLI);
static void WalRcvDie(int code, Datum arg);
static void XLogWalRcvProcessMsg(unsigned char type, char *buf, Size len,
								 TimeLineID tli);
static void XLogWalRcvWrite(char *buf, Size nbytes, XLogRecPtr recptr,
							TimeLineID tli);
static void XLogWalRcvFlush(bool dying, TimeLineID tli);
static void XLogWalRcvClose(XLogRecPtr recptr, TimeLineID tli);
static void XLogWalRcvSendReply(bool force, bool requestReply);
static void XLogWalRcvSendHSFeedback(bool immed);
static void ProcessWalSndrMessage(XLogRecPtr walEnd, TimestampTz sendTime);
static void WalRcvComputeNextWakeup(WalRcvWakeupReason reason, TimestampTz now);


/*
 *
 * 核心流程：startup 进程判定可以开始流式复制后，通知 postmaster 启动
 * walreceiver。本进程连接主库上的 walsender，在连接存活期间持续接收 XLOG
 * 并写入磁盘，刷盘后更新共享内存中的 WalRcv->flushedUpto，告知 startup
 * 进程回放可以推进到何处。建连用的 GUC 不由本进程直接加载，而由 startup
 * 进程在请求流式复制时传入，例如复制槽和连接串。主库结束发送但未断开时
 * 进入 waiting，等待新的接收起点；再次流式复制时只唤醒同一进程。
 * SIGTERM 指示 exit(0)，SIGQUIT 则立即中止。本文件是服务端逻辑，
 * libpq 相关代码由 libpqwalreceiver 动态加载。
 */
/* Main entry point for walreceiver process
 *
 * walreceiver 进程的主入口。
 */
void
WalReceiverMain(const void *startup_data, size_t startup_data_len)
{
	char		conninfo[MAXCONNINFO];
	char	   *tmp_conninfo;
	char		slotname[NAMEDATALEN];
	bool		is_temp_slot;
	XLogRecPtr	startpoint;
	TimeLineID	startpointTLI;
	TimeLineID	primaryTLI;
	bool		first_stream;
	WalRcvData *walrcv;
	TimestampTz now;
	char	   *err;
	char	   *sender_host = NULL;
	int			sender_port = 0;
	char	   *appname;

	Assert(startup_data_len == 0);

	MyBackendType = B_WAL_RECEIVER;
	AuxiliaryProcessMainCommon();

	/*
	 * WalRcv should be set up already (if we are a backend, we inherit this
	 * by fork() or EXEC_BACKEND mechanism from the postmaster).
	 *
	 * WalRcv 此时应已就绪（若我们是后端进程，则通过 fork() 或
	 * EXEC_BACKEND 机制从 postmaster 继承）。
	 */
	walrcv = WalRcv;
	Assert(walrcv != NULL);

	/*
	 * Mark walreceiver as running in shared memory.
	 *
	 * 在共享内存中把 walreceiver 标记为正在运行。
	 *
	 * Do this as early as possible, so that if we fail later on, we'll set
	 * state to STOPPED. If we die before this, the startup process will keep
	 * waiting for us to start up, until it times out.
	 *
	 * 尽可能早地做这件事，这样若稍后失败，我们会把状态设为 STOPPED。若在此之前就退出，
	 * startup 进程会一直等待我们启动，直到超时。
	 */
	SpinLockAcquire(&walrcv->mutex);
	Assert(walrcv->pid == 0);
	switch (walrcv->walRcvState)
	{
		case WALRCV_STOPPING:
			/* If we've already been requested to stop, don't start up.
			 *
			 * 若已经收到停止请求，则不要启动。
			 */
			walrcv->walRcvState = WALRCV_STOPPED;
			/* fall through
			 *
			 * 贯穿执行下一分支。
			 */

		case WALRCV_STOPPED:
			SpinLockRelease(&walrcv->mutex);
			ConditionVariableBroadcast(&walrcv->walRcvStoppedCV);
			proc_exit(1);
			break;

		case WALRCV_STARTING:
			/* The usual case
			 *
			 * 通常情况。
			 */
			break;

		case WALRCV_WAITING:
		case WALRCV_STREAMING:
		case WALRCV_RESTARTING:
		default:
			/* Shouldn't happen
			 *
			 * 不应发生。
			 */
			SpinLockRelease(&walrcv->mutex);
			elog(PANIC, "walreceiver still running according to shared memory state");
	}
	/* Advertise our PID so that the startup process can kill us
	 *
	 * 公布自己的 PID，以便 startup 进程可以终止我们。
	 */
	walrcv->pid = MyProcPid;
	walrcv->walRcvState = WALRCV_STREAMING;

	/* Fetch information required to start streaming
	 *
	 * 获取开始流式传输所需的信息。
	 */
	walrcv->ready_to_display = false;
	strlcpy(conninfo, walrcv->conninfo, MAXCONNINFO);
	strlcpy(slotname, walrcv->slotname, NAMEDATALEN);
	is_temp_slot = walrcv->is_temp_slot;
	startpoint = walrcv->receiveStart;
	startpointTLI = walrcv->receiveStartTLI;

	/*
	 * At most one of is_temp_slot and slotname can be set; otherwise,
	 * RequestXLogStreaming messed up.
	 *
	 * is_temp_slot 和 slotname 最多只能设置其中一个；否则就是
	 * RequestXLogStreaming 出错了。
	 */
	Assert(!is_temp_slot || (slotname[0] == '\0'));

	/* Initialise to a sanish value
	 *
	 * 初始化为一个合理的值。
	 */
	now = GetCurrentTimestamp();
	walrcv->lastMsgSendTime =
		walrcv->lastMsgReceiptTime = walrcv->latestWalEndTime = now;

	/* Report our proc number so that others can wake us up
	 *
	 * 报告自己的进程编号，以便其他进程唤醒我们。
	 */
	walrcv->procno = MyProcNumber;

	SpinLockRelease(&walrcv->mutex);

	pg_atomic_write_u64(&WalRcv->writtenUpto, 0);

	/* Arrange to clean up at walreceiver exit
	 *
	 * 安排在 walreceiver 退出时做清理。
	 */
	on_shmem_exit(WalRcvDie, PointerGetDatum(&startpointTLI));

	/* Properly accept or ignore signals the postmaster might send us
	 *
	 * 正确接受或忽略 postmaster 可能发给我们的信号。
	 */
	pqsignal(SIGHUP, SignalHandlerForConfigReload); /* set flag to read config
													 * file
													 *
													 * 设置标志以便读取配置文件。
													 */
	pqsignal(SIGINT, SIG_IGN);
	pqsignal(SIGTERM, die);		/* request shutdown
								 *
								 * 请求关闭。
								 */
	/* SIGQUIT handler was already set up by InitPostmasterChild
	 *
	 * SIGQUIT 处理函数已由 InitPostmasterChild 设置。
	 */
	pqsignal(SIGALRM, SIG_IGN);
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, SIG_IGN);

	/* Reset some signals that are accepted by postmaster but not here
	 *
	 * 重置一些 postmaster 会接受但此处不接受的信号。
	 */
	pqsignal(SIGCHLD, SIG_DFL);

	/* Load the libpq-specific functions
	 *
	 * 加载 libpq 专用函数。
	 */
	load_file("libpqwalreceiver", false);
	if (WalReceiverFunctions == NULL)
		elog(ERROR, "libpqwalreceiver didn't initialize correctly");

	/* Unblock signals (they were blocked when the postmaster forked us)
	 *
	 * 解除信号阻塞（postmaster 派生我们时这些信号处于阻塞状态）。
	 */
	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

	/* Establish the connection to the primary for XLOG streaming
	 *
	 * 建立到主库的连接，用于 XLOG 流式传输。
	 */
	appname = cluster_name[0] ? cluster_name : "walreceiver";
	wrconn = walrcv_connect(conninfo, true, false, false, appname, &err);
	if (!wrconn)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("streaming replication receiver \"%s\" could not connect to the primary server: %s",
						appname, err)));

	/*
	 * Save user-visible connection string.  This clobbers the original
	 * conninfo, for security. Also save host and port of the sender server
	 * this walreceiver is connected to.
	 *
	 * 保存用户可见的连接串。出于安全考虑，这会覆盖原来的 conninfo。同时保存本
	 * walreceiver 所连接发送端服务器的主机和端口。
	 */
	tmp_conninfo = walrcv_get_conninfo(wrconn);
	walrcv_get_senderinfo(wrconn, &sender_host, &sender_port);
	SpinLockAcquire(&walrcv->mutex);
	memset(walrcv->conninfo, 0, MAXCONNINFO);
	if (tmp_conninfo)
		strlcpy(walrcv->conninfo, tmp_conninfo, MAXCONNINFO);

	memset(walrcv->sender_host, 0, NI_MAXHOST);
	if (sender_host)
		strlcpy(walrcv->sender_host, sender_host, NI_MAXHOST);

	walrcv->sender_port = sender_port;
	walrcv->ready_to_display = true;
	SpinLockRelease(&walrcv->mutex);

	if (tmp_conninfo)
		pfree(tmp_conninfo);

	if (sender_host)
		pfree(sender_host);

	first_stream = true;
	for (;;)
	{
		char	   *primary_sysid;
		char		standby_sysid[32];
		WalRcvStreamOptions options;

		/*
		 * Check that we're connected to a valid server using the
		 * IDENTIFY_SYSTEM replication command.
		 *
		 * 用 IDENTIFY_SYSTEM 复制命令检查我们连接的是一台有效服务器。
		 */
		primary_sysid = walrcv_identify_system(wrconn, &primaryTLI);

		snprintf(standby_sysid, sizeof(standby_sysid), UINT64_FORMAT,
				 GetSystemIdentifier());
		if (strcmp(primary_sysid, standby_sysid) != 0)
		{
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("database system identifier differs between the primary and standby"),
					 errdetail("The primary's identifier is %s, the standby's identifier is %s.",
							   primary_sysid, standby_sysid)));
		}

		/*
		 * Confirm that the current timeline of the primary is the same or
		 * ahead of ours.
		 *
		 * 确认主库当前时间线与我们相同，或比我们更靠前。
		 */
		if (primaryTLI < startpointTLI)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("highest timeline %u of the primary is behind recovery timeline %u",
							primaryTLI, startpointTLI)));

		/*
		 * Get any missing history files. We do this always, even when we're
		 * not interested in that timeline, so that if we're promoted to
		 * become the primary later on, we don't select the same timeline that
		 * was already used in the current primary. This isn't bullet-proof -
		 * you'll need some external software to manage your cluster if you
		 * need to ensure that a unique timeline id is chosen in every case,
		 * but let's avoid the confusion of timeline id collisions where we
		 * can.
		 *
		 * 获取所有缺失的历史文件。即使当前并不关心那条时间线也总是这样做，以便以后被提升为主库时，
		 * 不会选用当前主库已经用过的同一条时间线。这并非万无一失：若必须保证每种情况下都选出唯一的
		 * 时间线 ID，还需要外部软件管理集群。但能避免的时间线 ID 冲突，就尽量避免。
		 */
		WalRcvFetchTimeLineHistoryFiles(startpointTLI, primaryTLI);

		/*
		 * Create temporary replication slot if requested, and update slot
		 * name in shared memory.  (Note the slot name cannot already be set
		 * in this case.)
		 *
		 * 若有请求则创建临时复制槽，并更新共享内存中的复制槽名。（注意这种情况下复制槽名原先不能已
		 * 经设置。）
		 */
		if (is_temp_slot)
		{
			snprintf(slotname, sizeof(slotname),
					 "pg_walreceiver_%lld",
					 (long long int) walrcv_get_backend_pid(wrconn));

			walrcv_create_slot(wrconn, slotname, true, false, false, 0, NULL);

			SpinLockAcquire(&walrcv->mutex);
			strlcpy(walrcv->slotname, slotname, NAMEDATALEN);
			SpinLockRelease(&walrcv->mutex);
		}

		/*
		 * Start streaming.
		 *
		 * 开始流式传输。
		 *
		 * We'll try to start at the requested starting point and timeline,
		 * even if it's different from the server's latest timeline. In case
		 * we've already reached the end of the old timeline, the server will
		 * finish the streaming immediately, and we will go back to await
		 * orders from the startup process. If recovery_target_timeline is
		 * 'latest', the startup process will scan pg_wal and find the new
		 * history file, bump recovery target timeline, and ask us to restart
		 * on the new timeline.
		 *
		 * 即使请求的起点和时间线与服务器最新时间线不同，也会尝试从该起点和时间线开始。
		 * 若已经到达旧时间线的末尾，服务器会立刻结束流式传输，我们再回去等待 startup
		 * 进程的指令。若 recovery_target_timeline 为 'latest'，
		 * startup 进程会扫描 pg_wal，找到新的历史文件，提升恢复目标时间线，
		 * 并要求我们在新时间线上重新开始。
		 */
		options.logical = false;
		options.startpoint = startpoint;
		options.slotname = slotname[0] != '\0' ? slotname : NULL;
		options.proto.physical.startpointTLI = startpointTLI;
		if (walrcv_startstreaming(wrconn, &options))
		{
			if (first_stream)
				ereport(LOG,
						(errmsg("started streaming WAL from primary at %X/%X on timeline %u",
								LSN_FORMAT_ARGS(startpoint), startpointTLI)));
			else
				ereport(LOG,
						(errmsg("restarted WAL streaming at %X/%X on timeline %u",
								LSN_FORMAT_ARGS(startpoint), startpointTLI)));
			first_stream = false;

			/* Initialize LogstreamResult and buffers for processing messages
			 *
			 * 初始化 LogstreamResult 以及处理消息用的缓冲区。
			 */
			LogstreamResult.Write = LogstreamResult.Flush = GetXLogReplayRecPtr(NULL);
			initStringInfo(&reply_message);

			/* Initialize nap wakeup times.
			 *
			 * 初始化休眠唤醒时间。
			 */
			now = GetCurrentTimestamp();
			for (int i = 0; i < NUM_WALRCV_WAKEUPS; ++i)
				WalRcvComputeNextWakeup(i, now);

			/* Send initial reply/feedback messages.
			 *
			 * 发送初始回复和反馈消息。
			 */
			XLogWalRcvSendReply(true, false);
			XLogWalRcvSendHSFeedback(true);

			/* Loop until end-of-streaming or error
			 *
			 * 循环直到流式传输结束或出错。
			 */
			for (;;)
			{
				char	   *buf;
				int			len;
				bool		endofwal = false;
				pgsocket	wait_fd = PGINVALID_SOCKET;
				int			rc;
				TimestampTz nextWakeup;
				long		nap;

				/*
				 * Exit walreceiver if we're not in recovery. This should not
				 * happen, but cross-check the status here.
				 *
				 * 若不在恢复中则退出 walreceiver。这本不应发生，但在这里交叉检查状态。
				 */
				if (!RecoveryInProgress())
					ereport(FATAL,
							(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
							 errmsg("cannot continue WAL streaming, recovery has already ended")));

				/* Process any requests or signals received recently
				 *
				 * 处理最近收到的请求或信号。
				 */
				CHECK_FOR_INTERRUPTS();

				if (ConfigReloadPending)
				{
					ConfigReloadPending = false;
					ProcessConfigFile(PGC_SIGHUP);
					/* recompute wakeup times
					 *
					 * 重新计算唤醒时间。
					 */
					now = GetCurrentTimestamp();
					for (int i = 0; i < NUM_WALRCV_WAKEUPS; ++i)
						WalRcvComputeNextWakeup(i, now);
					XLogWalRcvSendHSFeedback(true);
				}

				/* See if we can read data immediately
				 *
				 * 看看能否立刻读到数据。
				 */
				len = walrcv_receive(wrconn, &buf, &wait_fd);
				if (len != 0)
				{
					/*
					 * Process the received data, and any subsequent data we
					 * can read without blocking.
					 *
					 * 处理已收到的数据，以及随后可以不阻塞读到的数据。
					 */
					for (;;)
					{
						if (len > 0)
						{
							/*
							 * Something was received from primary, so adjust
							 * the ping and terminate wakeup times.
							 *
							 * 从主库收到了数据，因此调整 ping 和终止的唤醒时间。
							 */
							now = GetCurrentTimestamp();
							WalRcvComputeNextWakeup(WALRCV_WAKEUP_TERMINATE,
													now);
							WalRcvComputeNextWakeup(WALRCV_WAKEUP_PING, now);
							XLogWalRcvProcessMsg(buf[0], &buf[1], len - 1,
												 startpointTLI);
						}
						else if (len == 0)
							break;
						else if (len < 0)
						{
							ereport(LOG,
									(errmsg("replication terminated by primary server"),
									 errdetail("End of WAL reached on timeline %u at %X/%X.",
											   startpointTLI,
											   LSN_FORMAT_ARGS(LogstreamResult.Write))));
							endofwal = true;
							break;
						}
						len = walrcv_receive(wrconn, &buf, &wait_fd);
					}

					/* Let the primary know that we received some data.
					 *
					 * 告知主库我们已经收到一些数据。
					 */
					XLogWalRcvSendReply(false, false);

					/*
					 * If we've written some records, flush them to disk and
					 * let the startup process and primary server know about
					 * them.
					 *
					 * 若已经写下一些记录，就把它们刷到磁盘，并告知 startup 进程和主库。
					 */
					XLogWalRcvFlush(false, startpointTLI);
				}

				/* Check if we need to exit the streaming loop.
				 *
				 * 检查是否需要退出流式传输循环。
				 */
				if (endofwal)
					break;

				/* Find the soonest wakeup time, to limit our nap.
				 *
				 * 找出最近的唤醒时间，用来限制本次休眠。
				 */
				nextWakeup = TIMESTAMP_INFINITY;
				for (int i = 0; i < NUM_WALRCV_WAKEUPS; ++i)
					nextWakeup = Min(wakeup[i], nextWakeup);

				/* Calculate the nap time, clamping as necessary.
				 *
				 * 计算休眠时长，必要时做截断。
				 */
				now = GetCurrentTimestamp();
				nap = TimestampDifferenceMilliseconds(now, nextWakeup);

				/*
				 * Ideally we would reuse a WaitEventSet object repeatedly
				 * here to avoid the overheads of WaitLatchOrSocket on epoll
				 * systems, but we can't be sure that libpq (or any other
				 * walreceiver implementation) has the same socket (even if
				 * the fd is the same number, it may have been closed and
				 * reopened since the last time).  In future, if there is a
				 * function for removing sockets from WaitEventSet, then we
				 * could add and remove just the socket each time, potentially
				 * avoiding some system calls.
				 *
				 * 理想情况下会在这里反复复用同一个 WaitEventSet，以避免在 epoll
				 * 系统上反复调用 WaitLatchOrSocket 的开销。但无法确定
				 * libpq（或其他 walreceiver 实现）用的是同一个套接字（即使文件描述符编号
				 * 相同，也可能在上次之后被关闭并重新打开）。将来若有从 WaitEventSet
				 * 移除套接字的函数，就可以每次只增删该套接字，从而可能省去一些系统调用。
				 */
				Assert(wait_fd != PGINVALID_SOCKET);
				rc = WaitLatchOrSocket(MyLatch,
									   WL_EXIT_ON_PM_DEATH | WL_SOCKET_READABLE |
									   WL_TIMEOUT | WL_LATCH_SET,
									   wait_fd,
									   nap,
									   WAIT_EVENT_WAL_RECEIVER_MAIN);
				if (rc & WL_LATCH_SET)
				{
					ResetLatch(MyLatch);
					CHECK_FOR_INTERRUPTS();

					if (walrcv->force_reply)
					{
						/*
						 * The recovery process has asked us to send apply
						 * feedback now.  Make sure the flag is really set to
						 * false in shared memory before sending the reply, so
						 * we don't miss a new request for a reply.
						 *
						 * 恢复进程要求我们现在发送 apply 反馈。发送回复之前，先确认共享内存中的标志确实被设
						 * 为 false，以免错过新的回复请求。
						 */
						walrcv->force_reply = false;
						pg_memory_barrier();
						XLogWalRcvSendReply(true, false);
					}
				}
				if (rc & WL_TIMEOUT)
				{
					/*
					 * We didn't receive anything new. If we haven't heard
					 * anything from the server for more than
					 * wal_receiver_timeout / 2, ping the server. Also, if
					 * it's been longer than wal_receiver_status_interval
					 * since the last update we sent, send a status update to
					 * the primary anyway, to report any progress in applying
					 * WAL.
					 *
					 * 没有收到新数据。若超过 wal_receiver_timeout / 2
					 * 没听到服务器消息，就 ping 服务器。另外，若距离上次发送状态更新已超过
					 * wal_receiver_status_interval，也向主库发送状态更新，以报告
					 * WAL 应用进度。
					 */
					bool		requestReply = false;

					/*
					 * Report pending statistics to the cumulative stats
					 * system.  This location is useful for the report as it
					 * is not within a tight loop in the WAL receiver, to
					 * avoid bloating pgstats with requests, while also making
					 * sure that the reports happen each time a status update
					 * is sent.
					 *
					 * 把待上报的统计信息交给累积统计系统。这里适合上报：它不在 WAL receiver
					 * 的紧循环里，避免用请求撑大 pgstats，同时保证每次发送状态更新时都会上报。
					 */
					pgstat_report_wal(false);

					/*
					 * Check if time since last receive from primary has
					 * reached the configured limit.
					 *
					 * 检查距离上次从主库收到数据是否已达到配置的时限。
					 */
					now = GetCurrentTimestamp();
					if (now >= wakeup[WALRCV_WAKEUP_TERMINATE])
						ereport(ERROR,
								(errcode(ERRCODE_CONNECTION_FAILURE),
								 errmsg("terminating walreceiver due to timeout")));

					/*
					 * If we didn't receive anything new for half of receiver
					 * replication timeout, then ping the server.
					 *
					 * 若在接收端复制超时的一半时间内没有收到新数据，就 ping 服务器。
					 */
					if (now >= wakeup[WALRCV_WAKEUP_PING])
					{
						requestReply = true;
						wakeup[WALRCV_WAKEUP_PING] = TIMESTAMP_INFINITY;
					}

					XLogWalRcvSendReply(requestReply, requestReply);
					XLogWalRcvSendHSFeedback(false);
				}
			}

			/*
			 * The backend finished streaming. Exit streaming COPY-mode from
			 * our side, too.
			 *
			 * 后端已结束流式传输。我们也退出流式 COPY 模式。
			 */
			walrcv_endstreaming(wrconn, &primaryTLI);

			/*
			 * If the server had switched to a new timeline that we didn't
			 * know about when we began streaming, fetch its timeline history
			 * file now.
			 *
			 * 若服务器已切换到我们开始流式传输时还不知道的新时间线，现在获取它的时间线历史文件。
			 */
			WalRcvFetchTimeLineHistoryFiles(startpointTLI, primaryTLI);
		}
		else
			ereport(LOG,
					(errmsg("primary server contains no more WAL on requested timeline %u",
							startpointTLI)));

		/*
		 * End of WAL reached on the requested timeline. Close the last
		 * segment, and await for new orders from the startup process.
		 *
		 * 在请求的时间线上已到达 WAL 末尾。关闭最后一个段，并等待 startup
		 * 进程的新指令。
		 */
		if (recvFile >= 0)
		{
			char		xlogfname[MAXFNAMELEN];

			XLogWalRcvFlush(false, startpointTLI);
			XLogFileName(xlogfname, recvFileTLI, recvSegNo, wal_segment_size);
			if (close(recvFile) != 0)
				ereport(PANIC,
						(errcode_for_file_access(),
						 errmsg("could not close WAL segment %s: %m",
								xlogfname)));

			/*
			 * Create .done file forcibly to prevent the streamed segment from
			 * being archived later.
			 *
			 * 强制创建 .done 文件，防止已流式接收的段稍后被归档。
			 */
			if (XLogArchiveMode != ARCHIVE_MODE_ALWAYS)
				XLogArchiveForceDone(xlogfname);
			else
				XLogArchiveNotify(xlogfname);
		}
		recvFile = -1;

		elog(DEBUG1, "walreceiver ended streaming and awaits new instructions");
		WalRcvWaitForStartPosition(&startpoint, &startpointTLI);
	}
	/* not reached
	 *
	 * 不会执行到此处。
	 */
}

/*
 * Wait for startup process to set receiveStart and receiveStartTLI.
 *
 * 等待 startup 进程设置 receiveStart 和
 * receiveStartTLI。
 */
static void
WalRcvWaitForStartPosition(XLogRecPtr *startpoint, TimeLineID *startpointTLI)
{
	WalRcvData *walrcv = WalRcv;
	int			state;

	SpinLockAcquire(&walrcv->mutex);
	state = walrcv->walRcvState;
	if (state != WALRCV_STREAMING)
	{
		SpinLockRelease(&walrcv->mutex);
		if (state == WALRCV_STOPPING)
			proc_exit(0);
		else
			elog(FATAL, "unexpected walreceiver state");
	}
	walrcv->walRcvState = WALRCV_WAITING;
	walrcv->receiveStart = InvalidXLogRecPtr;
	walrcv->receiveStartTLI = 0;
	SpinLockRelease(&walrcv->mutex);

	set_ps_display("idle");

	/*
	 * nudge startup process to notice that we've stopped streaming and are
	 * now waiting for instructions.
	 *
	 * 提醒 startup 进程：我们已经停止流式传输，正在等待指令。
	 */
	WakeupRecovery();
	for (;;)
	{
		ResetLatch(MyLatch);

		CHECK_FOR_INTERRUPTS();

		SpinLockAcquire(&walrcv->mutex);
		Assert(walrcv->walRcvState == WALRCV_RESTARTING ||
			   walrcv->walRcvState == WALRCV_WAITING ||
			   walrcv->walRcvState == WALRCV_STOPPING);
		if (walrcv->walRcvState == WALRCV_RESTARTING)
		{
			/*
			 * No need to handle changes in primary_conninfo or
			 * primary_slot_name here. Startup process will signal us to
			 * terminate in case those change.
			 *
			 * 这里不必处理 primary_conninfo 或 primary_slot_name
			 * 的变化。若它们改变，startup 进程会发信号让我们终止。
			 */
			*startpoint = walrcv->receiveStart;
			*startpointTLI = walrcv->receiveStartTLI;
			walrcv->walRcvState = WALRCV_STREAMING;
			SpinLockRelease(&walrcv->mutex);
			break;
		}
		if (walrcv->walRcvState == WALRCV_STOPPING)
		{
			/*
			 * We should've received SIGTERM if the startup process wants us
			 * to die, but might as well check it here too.
			 *
			 * 若 startup 进程希望我们退出，本应已经收到 SIGTERM，但这里也顺便检查一下。
			 */
			SpinLockRelease(&walrcv->mutex);
			exit(1);
		}
		SpinLockRelease(&walrcv->mutex);

		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH, 0,
						 WAIT_EVENT_WAL_RECEIVER_WAIT_START);
	}

	if (update_process_title)
	{
		char		activitymsg[50];

		snprintf(activitymsg, sizeof(activitymsg), "restarting at %X/%X",
				 LSN_FORMAT_ARGS(*startpoint));
		set_ps_display(activitymsg);
	}
}

/*
 * Fetch any missing timeline history files between 'first' and 'last'
 * (inclusive) from the server.
 *
 * 从服务器获取 'first' 与 'last' 之间（含两端）所有缺失的时间线历史文件。
 */
static void
WalRcvFetchTimeLineHistoryFiles(TimeLineID first, TimeLineID last)
{
	TimeLineID	tli;

	for (tli = first; tli <= last; tli++)
	{
		/* there's no history file for timeline 1
		 *
		 * 时间线 1 没有历史文件。
		 */
		if (tli != 1 && !existsTimeLineHistory(tli))
		{
			char	   *fname;
			char	   *content;
			int			len;
			char		expectedfname[MAXFNAMELEN];

			ereport(LOG,
					(errmsg("fetching timeline history file for timeline %u from primary server",
							tli)));

			walrcv_readtimelinehistoryfile(wrconn, tli, &fname, &content, &len);

			/*
			 * Check that the filename on the primary matches what we
			 * calculated ourselves. This is just a sanity check, it should
			 * always match.
			 *
			 * 检查主库上的文件名是否与我们自己算出的一致。这只是健全性检查，应当总是一致。
			 */
			TLHistoryFileName(expectedfname, tli);
			if (strcmp(fname, expectedfname) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg_internal("primary reported unexpected file name for timeline history file of timeline %u",
										 tli)));

			/*
			 * Write the file to pg_wal.
			 *
			 * 把文件写入 pg_wal。
			 */
			writeTimeLineHistoryFile(tli, content, len);

			/*
			 * Mark the streamed history file as ready for archiving if
			 * archive_mode is always.
			 *
			 * 若 archive_mode 为 always，把流式接收的历史文件标记为可以归档。
			 */
			if (XLogArchiveMode != ARCHIVE_MODE_ALWAYS)
				XLogArchiveForceDone(fname);
			else
				XLogArchiveNotify(fname);

			pfree(fname);
			pfree(content);
		}
	}
}

/*
 * Mark us as STOPPED in shared memory at exit.
 *
 * 退出时在共享内存中把我们标记为 STOPPED。
 */
static void
WalRcvDie(int code, Datum arg)
{
	WalRcvData *walrcv = WalRcv;
	TimeLineID *startpointTLI_p = (TimeLineID *) DatumGetPointer(arg);

	Assert(*startpointTLI_p != 0);

	/* Ensure that all WAL records received are flushed to disk
	 *
	 * 确保收到的全部 WAL 记录都已刷到磁盘。
	 */
	XLogWalRcvFlush(true, *startpointTLI_p);

	/* Mark ourselves inactive in shared memory
	 *
	 * 在共享内存中把我们标记为非活动。
	 */
	SpinLockAcquire(&walrcv->mutex);
	Assert(walrcv->walRcvState == WALRCV_STREAMING ||
		   walrcv->walRcvState == WALRCV_RESTARTING ||
		   walrcv->walRcvState == WALRCV_STARTING ||
		   walrcv->walRcvState == WALRCV_WAITING ||
		   walrcv->walRcvState == WALRCV_STOPPING);
	Assert(walrcv->pid == MyProcPid);
	walrcv->walRcvState = WALRCV_STOPPED;
	walrcv->pid = 0;
	walrcv->procno = INVALID_PROC_NUMBER;
	walrcv->ready_to_display = false;
	SpinLockRelease(&walrcv->mutex);

	ConditionVariableBroadcast(&walrcv->walRcvStoppedCV);

	/* Terminate the connection gracefully.
	 *
	 * 优雅地终止连接。
	 */
	if (wrconn != NULL)
		walrcv_disconnect(wrconn);

	/* Wake up the startup process to notice promptly that we're gone
	 *
	 * 唤醒 startup 进程，让它尽快发现我们已经退出。
	 */
	WakeupRecovery();
}

/*
 * Accept the message from XLOG stream, and process it.
 *
 * 接收 XLOG 流中的消息并处理。
 */
static void
XLogWalRcvProcessMsg(unsigned char type, char *buf, Size len, TimeLineID tli)
{
	int			hdrlen;
	XLogRecPtr	dataStart;
	XLogRecPtr	walEnd;
	TimestampTz sendTime;
	bool		replyRequested;

	switch (type)
	{
		case 'w':				/* WAL records
								 *
								 * WAL 记录。
								 */
			{
				StringInfoData incoming_message;

				hdrlen = sizeof(int64) + sizeof(int64) + sizeof(int64);
				if (len < hdrlen)
					ereport(ERROR,
							(errcode(ERRCODE_PROTOCOL_VIOLATION),
							 errmsg_internal("invalid WAL message received from primary")));

				/* initialize a StringInfo with the given buffer
				 *
				 * 用给定缓冲区初始化一个 StringInfo。
				 */
				initReadOnlyStringInfo(&incoming_message, buf, hdrlen);

				/* read the fields
				 *
				 * 读取各个字段。
				 */
				dataStart = pq_getmsgint64(&incoming_message);
				walEnd = pq_getmsgint64(&incoming_message);
				sendTime = pq_getmsgint64(&incoming_message);
				ProcessWalSndrMessage(walEnd, sendTime);

				buf += hdrlen;
				len -= hdrlen;
				XLogWalRcvWrite(buf, len, dataStart, tli);
				break;
			}
		case 'k':				/* Keepalive
								 *
								 * Keepalive 保活消息。
								 */
			{
				StringInfoData incoming_message;

				hdrlen = sizeof(int64) + sizeof(int64) + sizeof(char);
				if (len != hdrlen)
					ereport(ERROR,
							(errcode(ERRCODE_PROTOCOL_VIOLATION),
							 errmsg_internal("invalid keepalive message received from primary")));

				/* initialize a StringInfo with the given buffer
				 *
				 * 用给定缓冲区初始化一个 StringInfo。
				 */
				initReadOnlyStringInfo(&incoming_message, buf, hdrlen);

				/* read the fields
				 *
				 * 读取各个字段。
				 */
				walEnd = pq_getmsgint64(&incoming_message);
				sendTime = pq_getmsgint64(&incoming_message);
				replyRequested = pq_getmsgbyte(&incoming_message);

				ProcessWalSndrMessage(walEnd, sendTime);

				/* If the primary requested a reply, send one immediately
				 *
				 * 若主库要求回复，立即发送。
				 */
				if (replyRequested)
					XLogWalRcvSendReply(true, false);
				break;
			}
		default:
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg_internal("invalid replication message type %d",
									 type)));
	}
}

/*
 * Write XLOG data to disk.
 *
 * 把 XLOG 数据写入磁盘。
 */
static void
XLogWalRcvWrite(char *buf, Size nbytes, XLogRecPtr recptr, TimeLineID tli)
{
	int			startoff;
	int			byteswritten;
	instr_time	start;

	Assert(tli != 0);

	while (nbytes > 0)
	{
		int			segbytes;

		/* Close the current segment if it's completed
		 *
		 * 若当前段已写完则关闭它。
		 */
		if (recvFile >= 0 && !XLByteInSeg(recptr, recvSegNo, wal_segment_size))
			XLogWalRcvClose(recptr, tli);

		if (recvFile < 0)
		{
			/* Create/use new log file
			 *
			 * 创建或使用新的日志文件。
			 */
			XLByteToSeg(recptr, recvSegNo, wal_segment_size);
			recvFile = XLogFileInit(recvSegNo, tli);
			recvFileTLI = tli;
		}

		/* Calculate the start offset of the received logs
		 *
		 * 计算收到的日志的起始偏移。
		 */
		startoff = XLogSegmentOffset(recptr, wal_segment_size);

		if (startoff + nbytes > wal_segment_size)
			segbytes = wal_segment_size - startoff;
		else
			segbytes = nbytes;

		/* OK to write the logs
		 *
		 * 可以写这些日志了。
		 */
		errno = 0;

		/*
		 * Measure I/O timing to write WAL data, for pg_stat_io.
		 *
		 * 测量写 WAL 数据的 I/O 耗时，供 pg_stat_io 使用。
		 */
		start = pgstat_prepare_io_time(track_wal_io_timing);

		pgstat_report_wait_start(WAIT_EVENT_WAL_WRITE);
		byteswritten = pg_pwrite(recvFile, buf, segbytes, (off_t) startoff);
		pgstat_report_wait_end();

		pgstat_count_io_op_time(IOOBJECT_WAL, IOCONTEXT_NORMAL,
								IOOP_WRITE, start, 1, byteswritten);

		if (byteswritten <= 0)
		{
			char		xlogfname[MAXFNAMELEN];
			int			save_errno;

			/* if write didn't set errno, assume no disk space
			 *
			 * 若写入没有设置 errno，就假定磁盘空间不足。
			 */
			if (errno == 0)
				errno = ENOSPC;

			save_errno = errno;
			XLogFileName(xlogfname, recvFileTLI, recvSegNo, wal_segment_size);
			errno = save_errno;
			ereport(PANIC,
					(errcode_for_file_access(),
					 errmsg("could not write to WAL segment %s "
							"at offset %d, length %lu: %m",
							xlogfname, startoff, (unsigned long) segbytes)));
		}

		/* Update state for write
		 *
		 * 更新写入状态。
		 */
		recptr += byteswritten;

		nbytes -= byteswritten;
		buf += byteswritten;

		LogstreamResult.Write = recptr;
	}

	/* Update shared-memory status
	 *
	 * 更新共享内存状态。
	 */
	pg_atomic_write_u64(&WalRcv->writtenUpto, LogstreamResult.Write);

	/*
	 * Close the current segment if it's fully written up in the last cycle of
	 * the loop, to create its archive notification file soon. Otherwise WAL
	 * archiving of the segment will be delayed until any data in the next
	 * segment is received and written.
	 *
	 * 若循环最后一轮已经把当前段写满，就关闭它，以便尽快创建归档通知文件。否则该段的 WAL
	 * 归档会推迟到下一段有数据被接收并写入之后。
	 */
	if (recvFile >= 0 && !XLByteInSeg(recptr, recvSegNo, wal_segment_size))
		XLogWalRcvClose(recptr, tli);
}

/*
 * Flush the log to disk.
 *
 * 把日志刷到磁盘。
 *
 * If we're in the midst of dying, it's unwise to do anything that might throw
 * an error, so we skip sending a reply in that case.
 *
 * 若正处于退出过程中，做任何可能抛错的事情都不明智，因此这种情况下跳过发送回复。
 */
static void
XLogWalRcvFlush(bool dying, TimeLineID tli)
{
	Assert(tli != 0);

	if (LogstreamResult.Flush < LogstreamResult.Write)
	{
		WalRcvData *walrcv = WalRcv;

		issue_xlog_fsync(recvFile, recvSegNo, tli);

		LogstreamResult.Flush = LogstreamResult.Write;

		/* Update shared-memory status
		 *
		 * 更新共享内存状态。
		 */
		SpinLockAcquire(&walrcv->mutex);
		if (walrcv->flushedUpto < LogstreamResult.Flush)
		{
			walrcv->latestChunkStart = walrcv->flushedUpto;
			walrcv->flushedUpto = LogstreamResult.Flush;
			walrcv->receivedTLI = tli;
		}
		SpinLockRelease(&walrcv->mutex);

		/* Signal the startup process and walsender that new WAL has arrived
		 *
		 * 通知 startup 进程和 walsender：有新的 WAL 到达。
		 */
		WakeupRecovery();
		if (AllowCascadeReplication())
			WalSndWakeup(true, false);

		/* Report XLOG streaming progress in PS display
		 *
		 * 在进程状态显示中报告 XLOG 流式传输进度。
		 */
		if (update_process_title)
		{
			char		activitymsg[50];

			snprintf(activitymsg, sizeof(activitymsg), "streaming %X/%X",
					 LSN_FORMAT_ARGS(LogstreamResult.Write));
			set_ps_display(activitymsg);
		}

		/* Also let the primary know that we made some progress
		 *
		 * 同时告知主库我们已经有所进展。
		 */
		if (!dying)
		{
			XLogWalRcvSendReply(false, false);
			XLogWalRcvSendHSFeedback(false);
		}
	}
}

/*
 * Close the current segment.
 *
 * 关闭当前段。
 *
 * Flush the segment to disk before closing it. Otherwise we have to
 * reopen and fsync it later.
 *
 * 关闭前先把该段刷到磁盘。否则以后还得重新打开并 fsync。
 *
 * Create an archive notification file since the segment is known completed.
 *
 * 该段已知写完，因此创建归档通知文件。
 */
static void
XLogWalRcvClose(XLogRecPtr recptr, TimeLineID tli)
{
	char		xlogfname[MAXFNAMELEN];

	Assert(recvFile >= 0 && !XLByteInSeg(recptr, recvSegNo, wal_segment_size));
	Assert(tli != 0);

	/*
	 * fsync() and close current file before we switch to next one. We would
	 * otherwise have to reopen this file to fsync it later
	 *
	 * 切换到下一段之前先 fsync() 并关闭当前文件。否则以后还得重新打开这个文件才能
	 * fsync。
	 */
	XLogWalRcvFlush(false, tli);

	XLogFileName(xlogfname, recvFileTLI, recvSegNo, wal_segment_size);

	/*
	 * XLOG segment files will be re-read by recovery in startup process soon,
	 * so we don't advise the OS to release cache pages associated with the
	 * file like XLogFileClose() does.
	 *
	 * startup 进程中的恢复很快会重新读取这些 XLOG 段文件，因此不像
	 * XLogFileClose() 那样建议操作系统释放该文件关联的缓存页。
	 */
	if (close(recvFile) != 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not close WAL segment %s: %m",
						xlogfname)));

	/*
	 * Create .done file forcibly to prevent the streamed segment from being
	 * archived later.
	 *
	 * 强制创建 .done 文件，防止已流式接收的段稍后被归档。
	 */
	if (XLogArchiveMode != ARCHIVE_MODE_ALWAYS)
		XLogArchiveForceDone(xlogfname);
	else
		XLogArchiveNotify(xlogfname);

	recvFile = -1;
}

/*
 * Send reply message to primary, indicating our current WAL locations, oldest
 * xmin and the current time.
 *
 * 向主库发送回复消息，说明当前 WAL 位置、最老的 xmin 以及当前时间。
 *
 * If 'force' is not set, the message is only sent if enough time has
 * passed since last status update to reach wal_receiver_status_interval.
 * If wal_receiver_status_interval is disabled altogether and 'force' is
 * false, this is a no-op.
 *
 * 若未设置 'force'，则只有距离上次状态更新已达到
 * wal_receiver_status_interval 时才发送。若
 * wal_receiver_status_interval 被完全禁用且 'force'
 * 为 false，则什么也不做。
 *
 * If 'requestReply' is true, requests the server to reply immediately upon
 * receiving this message. This is used for heartbeats, when approaching
 * wal_receiver_timeout.
 *
 * 若 'requestReply' 为 true，则请求服务器收到此消息后立即回复。接近
 * wal_receiver_timeout 时，心跳会用到这一点。
 */
static void
XLogWalRcvSendReply(bool force, bool requestReply)
{
	static XLogRecPtr writePtr = 0;
	static XLogRecPtr flushPtr = 0;
	XLogRecPtr	applyPtr;
	TimestampTz now;

	/*
	 * If the user doesn't want status to be reported to the primary, be sure
	 * to exit before doing anything at all.
	 *
	 * 若用户不希望向主库报告状态，务必在做任何事情之前就返回。
	 */
	if (!force && wal_receiver_status_interval <= 0)
		return;

	/* Get current timestamp.
	 *
	 * 取得当前时间戳。
	 */
	now = GetCurrentTimestamp();

	/*
	 * We can compare the write and flush positions to the last message we
	 * sent without taking any lock, but the apply position requires a spin
	 * lock, so we don't check that unless something else has changed or 10
	 * seconds have passed.  This means that the apply WAL location will
	 * appear, from the primary's point of view, to lag slightly, but since
	 * this is only for reporting purposes and only on idle systems, that's
	 * probably OK.
	 *
	 * 可以不加锁就把 write 和 flush 位置与上次发送的消息比较，但 apply
	 * 位置需要自旋锁。因此除非其他位置有变化，或已经过去 10 秒，否则不检查 apply
	 * 位置。从主库看来，apply 的 WAL 位置会略有滞后；这只用于报告，
	 * 且只发生在空闲系统上，通常可以接受。
	 */
	if (!force
		&& writePtr == LogstreamResult.Write
		&& flushPtr == LogstreamResult.Flush
		&& now < wakeup[WALRCV_WAKEUP_REPLY])
		return;

	/* Make sure we wake up when it's time to send another reply.
	 *
	 * 确保到了该再发一条回复的时候会被唤醒。
	 */
	WalRcvComputeNextWakeup(WALRCV_WAKEUP_REPLY, now);

	/* Construct a new message
	 *
	 * 构造一条新消息。
	 */
	writePtr = LogstreamResult.Write;
	flushPtr = LogstreamResult.Flush;
	applyPtr = GetXLogReplayRecPtr(NULL);

	resetStringInfo(&reply_message);
	pq_sendbyte(&reply_message, 'r');
	pq_sendint64(&reply_message, writePtr);
	pq_sendint64(&reply_message, flushPtr);
	pq_sendint64(&reply_message, applyPtr);
	pq_sendint64(&reply_message, GetCurrentTimestamp());
	pq_sendbyte(&reply_message, requestReply ? 1 : 0);

	/* Send it
	 *
	 * 发送它。
	 */
	elog(DEBUG2, "sending write %X/%X flush %X/%X apply %X/%X%s",
		 LSN_FORMAT_ARGS(writePtr),
		 LSN_FORMAT_ARGS(flushPtr),
		 LSN_FORMAT_ARGS(applyPtr),
		 requestReply ? " (reply requested)" : "");

	walrcv_send(wrconn, reply_message.data, reply_message.len);
}

/*
 * Send hot standby feedback message to primary, plus the current time,
 * in case they don't have a watch.
 *
 * 向主库发送热备反馈消息，并附上当前时间，以防对方没有时钟。
 *
 * If the user disables feedback, send one final message to tell sender
 * to forget about the xmin on this standby. We also send this message
 * on first connect because a previous connection might have set xmin
 * on a replication slot. (If we're not using a slot it's harmless to
 * send a feedback message explicitly setting InvalidTransactionId).
 *
 * 若用户关闭反馈，仍发送最后一条消息，让发送端忘掉这个备库上的 xmin。
 * 首次连接时也会发送该消息，因为先前的连接可能已在复制槽上设置了 xmin。
 * （若未使用复制槽，显式把 InvalidTransactionId 写入反馈消息也无害。）
 */
static void
XLogWalRcvSendHSFeedback(bool immed)
{
	TimestampTz now;
	FullTransactionId nextFullXid;
	TransactionId nextXid;
	uint32		xmin_epoch,
				catalog_xmin_epoch;
	TransactionId xmin,
				catalog_xmin;

	/* initially true so we always send at least one feedback message
	 *
	 * 初始为 true，这样至少总会发送一条反馈消息。
	 */
	static bool primary_has_standby_xmin = true;

	/*
	 * If the user doesn't want status to be reported to the primary, be sure
	 * to exit before doing anything at all.
	 *
	 * 若用户不希望向主库报告状态，务必在做任何事情之前就返回。
	 */
	if ((wal_receiver_status_interval <= 0 || !hot_standby_feedback) &&
		!primary_has_standby_xmin)
		return;

	/* Get current timestamp.
	 *
	 * 取得当前时间戳。
	 */
	now = GetCurrentTimestamp();

	/* Send feedback at most once per wal_receiver_status_interval.
	 *
	 * 每个 wal_receiver_status_interval 最多发送一次反馈。
	 */
	if (!immed && now < wakeup[WALRCV_WAKEUP_HSFEEDBACK])
		return;

	/* Make sure we wake up when it's time to send feedback again.
	 *
	 * 确保到了该再次发送反馈的时候会被唤醒。
	 */
	WalRcvComputeNextWakeup(WALRCV_WAKEUP_HSFEEDBACK, now);

	/*
	 * If Hot Standby is not yet accepting connections there is nothing to
	 * send. Check this after the interval has expired to reduce number of
	 * calls.
	 *
	 * 若 Hot Standby 尚未接受连接，就没有什么可发送的。等间隔过期后再检查，
	 * 以减少调用次数。
	 *
	 * Bailing out here also ensures that we don't send feedback until we've
	 * read our own replication slot state, so we don't tell the primary to
	 * discard needed xmin or catalog_xmin from any slots that may exist on
	 * this replica.
	 *
	 * 在这里返回还能保证：在读到本机复制槽状态之前不发送反馈，以免让主库丢弃本副本上任何复制槽
	 * 仍需要的 xmin 或 catalog_xmin。
	 */
	if (!HotStandbyActive())
		return;

	/*
	 * Make the expensive call to get the oldest xmin once we are certain
	 * everything else has been checked.
	 *
	 * 等确认其他条件都已检查过，再去做获取最老 xmin 这个开销较大的调用。
	 */
	if (hot_standby_feedback)
	{
		GetReplicationHorizons(&xmin, &catalog_xmin);
	}
	else
	{
		xmin = InvalidTransactionId;
		catalog_xmin = InvalidTransactionId;
	}

	/*
	 * Get epoch and adjust if nextXid and oldestXmin are different sides of
	 * the epoch boundary.
	 *
	 * 取得 epoch；若 nextXid 与 oldestXmin 落在 epoch
	 * 边界的两侧，则加以调整。
	 */
	nextFullXid = ReadNextFullTransactionId();
	nextXid = XidFromFullTransactionId(nextFullXid);
	xmin_epoch = EpochFromFullTransactionId(nextFullXid);
	catalog_xmin_epoch = xmin_epoch;
	if (nextXid < xmin)
		xmin_epoch--;
	if (nextXid < catalog_xmin)
		catalog_xmin_epoch--;

	elog(DEBUG2, "sending hot standby feedback xmin %u epoch %u catalog_xmin %u catalog_xmin_epoch %u",
		 xmin, xmin_epoch, catalog_xmin, catalog_xmin_epoch);

	/* Construct the message and send it.
	 *
	 * 构造消息并发送。
	 */
	resetStringInfo(&reply_message);
	pq_sendbyte(&reply_message, 'h');
	pq_sendint64(&reply_message, GetCurrentTimestamp());
	pq_sendint32(&reply_message, xmin);
	pq_sendint32(&reply_message, xmin_epoch);
	pq_sendint32(&reply_message, catalog_xmin);
	pq_sendint32(&reply_message, catalog_xmin_epoch);
	walrcv_send(wrconn, reply_message.data, reply_message.len);
	if (TransactionIdIsValid(xmin) || TransactionIdIsValid(catalog_xmin))
		primary_has_standby_xmin = true;
	else
		primary_has_standby_xmin = false;
}

/*
 * Update shared memory status upon receiving a message from primary.
 *
 * 收到主库消息后更新共享内存状态。
 *
 * 'walEnd' and 'sendTime' are the end-of-WAL and timestamp of the latest
 * message, reported by primary.
 *
 * 'walEnd' 和 'sendTime' 是主库报告的最新消息的 WAL
 * 结尾位置和时间戳。
 */
static void
ProcessWalSndrMessage(XLogRecPtr walEnd, TimestampTz sendTime)
{
	WalRcvData *walrcv = WalRcv;
	TimestampTz lastMsgReceiptTime = GetCurrentTimestamp();

	/* Update shared-memory status
	 *
	 * 更新共享内存状态。
	 */
	SpinLockAcquire(&walrcv->mutex);
	if (walrcv->latestWalEnd < walEnd)
		walrcv->latestWalEndTime = sendTime;
	walrcv->latestWalEnd = walEnd;
	walrcv->lastMsgSendTime = sendTime;
	walrcv->lastMsgReceiptTime = lastMsgReceiptTime;
	SpinLockRelease(&walrcv->mutex);

	if (message_level_is_interesting(DEBUG2))
	{
		char	   *sendtime;
		char	   *receipttime;
		int			applyDelay;

		/* Copy because timestamptz_to_str returns a static buffer
		 *
		 * 必须复制，因为 timestamptz_to_str 返回的是静态缓冲区。
		 */
		sendtime = pstrdup(timestamptz_to_str(sendTime));
		receipttime = pstrdup(timestamptz_to_str(lastMsgReceiptTime));
		applyDelay = GetReplicationApplyDelay();

		/* apply delay is not available
		 *
		 * apply 延迟不可用。
		 */
		if (applyDelay == -1)
			elog(DEBUG2, "sendtime %s receipttime %s replication apply delay (N/A) transfer latency %d ms",
				 sendtime,
				 receipttime,
				 GetReplicationTransferLatency());
		else
			elog(DEBUG2, "sendtime %s receipttime %s replication apply delay %d ms transfer latency %d ms",
				 sendtime,
				 receipttime,
				 applyDelay,
				 GetReplicationTransferLatency());

		pfree(sendtime);
		pfree(receipttime);
	}
}

/*
 * Compute the next wakeup time for a given wakeup reason.  Can be called to
 * initialize a wakeup time, to adjust it for the next wakeup, or to
 * reinitialize it when GUCs have changed.  We ask the caller to pass in the
 * value of "now" because this frequently avoids multiple calls of
 * GetCurrentTimestamp().  It had better be a reasonably up-to-date value
 * though.
 *
 * 按给定的唤醒原因计算下一次唤醒时间。可用于初始化唤醒时间、把它调整到下一次，或在 GUC
 * 变化后重新初始化。要求调用方传入 "now"，是为了经常避免多次调用
 * GetCurrentTimestamp()。不过这个值最好是相当新的。
 */
static void
WalRcvComputeNextWakeup(WalRcvWakeupReason reason, TimestampTz now)
{
	switch (reason)
	{
		case WALRCV_WAKEUP_TERMINATE:
			if (wal_receiver_timeout <= 0)
				wakeup[reason] = TIMESTAMP_INFINITY;
			else
				wakeup[reason] = TimestampTzPlusMilliseconds(now, wal_receiver_timeout);
			break;
		case WALRCV_WAKEUP_PING:
			if (wal_receiver_timeout <= 0)
				wakeup[reason] = TIMESTAMP_INFINITY;
			else
				wakeup[reason] = TimestampTzPlusMilliseconds(now, wal_receiver_timeout / 2);
			break;
		case WALRCV_WAKEUP_HSFEEDBACK:
			if (!hot_standby_feedback || wal_receiver_status_interval <= 0)
				wakeup[reason] = TIMESTAMP_INFINITY;
			else
				wakeup[reason] = TimestampTzPlusSeconds(now, wal_receiver_status_interval);
			break;
		case WALRCV_WAKEUP_REPLY:
			if (wal_receiver_status_interval <= 0)
				wakeup[reason] = TIMESTAMP_INFINITY;
			else
				wakeup[reason] = TimestampTzPlusSeconds(now, wal_receiver_status_interval);
			break;
			/* there's intentionally no default: here
			 *
			 * 这里有意不写 default 分支。
			 */
	}
}

/*
 * Wake up the walreceiver main loop.
 *
 * 唤醒 walreceiver 主循环。
 *
 * This is called by the startup process whenever interesting xlog records
 * are applied, so that walreceiver can check if it needs to send an apply
 * notification back to the primary which may be waiting in a COMMIT with
 * synchronous_commit = remote_apply.
 *
 * startup 进程每当应用了值得关注的 xlog 记录就会调用它，以便
 * walreceiver 检查是否需要向主库回送 apply 通知。主库可能正停在
 * synchronous_commit = remote_apply 的 COMMIT
 * 上等待。
 */
void
WalRcvForceReply(void)
{
	ProcNumber	procno;

	WalRcv->force_reply = true;
	/* fetching the proc number is probably atomic, but don't rely on it
	 *
	 * 读取进程编号大概是原子的，但不要依赖这一点。
	 */
	SpinLockAcquire(&WalRcv->mutex);
	procno = WalRcv->procno;
	SpinLockRelease(&WalRcv->mutex);
	if (procno != INVALID_PROC_NUMBER)
		SetLatch(&GetPGProcByNumber(procno)->procLatch);
}

/*
 * Return a string constant representing the state. This is used
 * in system functions and views, and should *not* be translated.
 *
 * 返回表示状态的字符串常量。用于系统函数和视图，不应被翻译。
 */
static const char *
WalRcvGetStateString(WalRcvState state)
{
	switch (state)
	{
		case WALRCV_STOPPED:
			return "stopped";
		case WALRCV_STARTING:
			return "starting";
		case WALRCV_STREAMING:
			return "streaming";
		case WALRCV_WAITING:
			return "waiting";
		case WALRCV_RESTARTING:
			return "restarting";
		case WALRCV_STOPPING:
			return "stopping";
	}
	return "UNKNOWN";
}

/*
 * Returns activity of WAL receiver, including pid, state and xlog locations
 * received from the WAL sender of another server.
 *
 * 返回 WAL receiver 的活动信息，包括 pid、状态，以及从另一台服务器的
 * WAL sender 收到的 xlog 位置。
 */
Datum
pg_stat_get_wal_receiver(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum	   *values;
	bool	   *nulls;
	int			pid;
	bool		ready_to_display;
	WalRcvState state;
	XLogRecPtr	receive_start_lsn;
	TimeLineID	receive_start_tli;
	XLogRecPtr	written_lsn;
	XLogRecPtr	flushed_lsn;
	TimeLineID	received_tli;
	TimestampTz last_send_time;
	TimestampTz last_receipt_time;
	XLogRecPtr	latest_end_lsn;
	TimestampTz latest_end_time;
	char		sender_host[NI_MAXHOST];
	int			sender_port = 0;
	char		slotname[NAMEDATALEN];
	char		conninfo[MAXCONNINFO];

	/* Take a lock to ensure value consistency
	 *
	 * 加锁以保证读到的值一致。
	 */
	SpinLockAcquire(&WalRcv->mutex);
	pid = (int) WalRcv->pid;
	ready_to_display = WalRcv->ready_to_display;
	state = WalRcv->walRcvState;
	receive_start_lsn = WalRcv->receiveStart;
	receive_start_tli = WalRcv->receiveStartTLI;
	flushed_lsn = WalRcv->flushedUpto;
	received_tli = WalRcv->receivedTLI;
	last_send_time = WalRcv->lastMsgSendTime;
	last_receipt_time = WalRcv->lastMsgReceiptTime;
	latest_end_lsn = WalRcv->latestWalEnd;
	latest_end_time = WalRcv->latestWalEndTime;
	strlcpy(slotname, WalRcv->slotname, sizeof(slotname));
	strlcpy(sender_host, WalRcv->sender_host, sizeof(sender_host));
	sender_port = WalRcv->sender_port;
	strlcpy(conninfo, WalRcv->conninfo, sizeof(conninfo));
	SpinLockRelease(&WalRcv->mutex);

	/*
	 * No WAL receiver (or not ready yet), just return a tuple with NULL
	 * values
	 *
	 * 没有 WAL receiver（或尚未就绪），直接返回各列为 NULL 的元组。
	 */
	if (pid == 0 || !ready_to_display)
		PG_RETURN_NULL();

	/*
	 * Read "writtenUpto" without holding a spinlock.  Note that it may not be
	 * consistent with the other shared variables of the WAL receiver
	 * protected by a spinlock, but this should not be used for data integrity
	 * checks.
	 *
	 * 不加自旋锁读取 "writtenUpto"。它可能与 walreceiver
	 * 中由自旋锁保护的其他共享变量不一致，但不应把它用于数据完整性检查。
	 */
	written_lsn = pg_atomic_read_u64(&WalRcv->writtenUpto);

	/* determine result type
	 *
	 * 确定结果类型。
	 */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	values = palloc0(sizeof(Datum) * tupdesc->natts);
	nulls = palloc0(sizeof(bool) * tupdesc->natts);

	/* Fetch values
	 *
	 * 取出各个值。
	 */
	values[0] = Int32GetDatum(pid);

	if (!has_privs_of_role(GetUserId(), ROLE_PG_READ_ALL_STATS))
	{
		/*
		 * Only superusers and roles with privileges of pg_read_all_stats can
		 * see details. Other users only get the pid value to know whether it
		 * is a WAL receiver, but no details.
		 *
		 * 只有超级用户以及拥有 pg_read_all_stats 权限的角色才能看到详情。
		 * 其他用户只能拿到 pid，用来判断是否存在 WAL receiver，但看不到详情。
		 */
		memset(&nulls[1], true, sizeof(bool) * (tupdesc->natts - 1));
	}
	else
	{
		values[1] = CStringGetTextDatum(WalRcvGetStateString(state));

		if (XLogRecPtrIsInvalid(receive_start_lsn))
			nulls[2] = true;
		else
			values[2] = LSNGetDatum(receive_start_lsn);
		values[3] = Int32GetDatum(receive_start_tli);
		if (XLogRecPtrIsInvalid(written_lsn))
			nulls[4] = true;
		else
			values[4] = LSNGetDatum(written_lsn);
		if (XLogRecPtrIsInvalid(flushed_lsn))
			nulls[5] = true;
		else
			values[5] = LSNGetDatum(flushed_lsn);
		values[6] = Int32GetDatum(received_tli);
		if (last_send_time == 0)
			nulls[7] = true;
		else
			values[7] = TimestampTzGetDatum(last_send_time);
		if (last_receipt_time == 0)
			nulls[8] = true;
		else
			values[8] = TimestampTzGetDatum(last_receipt_time);
		if (XLogRecPtrIsInvalid(latest_end_lsn))
			nulls[9] = true;
		else
			values[9] = LSNGetDatum(latest_end_lsn);
		if (latest_end_time == 0)
			nulls[10] = true;
		else
			values[10] = TimestampTzGetDatum(latest_end_time);
		if (*slotname == '\0')
			nulls[11] = true;
		else
			values[11] = CStringGetTextDatum(slotname);
		if (*sender_host == '\0')
			nulls[12] = true;
		else
			values[12] = CStringGetTextDatum(sender_host);
		if (sender_port == 0)
			nulls[13] = true;
		else
			values[13] = Int32GetDatum(sender_port);
		if (*conninfo == '\0')
			nulls[14] = true;
		else
			values[14] = CStringGetTextDatum(conninfo);
	}

	/* Returns the record as Datum
	 *
	 * 把记录作为 Datum 返回。
	 */
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

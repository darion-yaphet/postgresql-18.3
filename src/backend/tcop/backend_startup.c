/*-------------------------------------------------------------------------
 *
 * backend_startup.c
 *	  Backend startup code
 *
 *	  后端启动代码
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/tcop/backend_startup.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>

#include "access/xlog.h"
#include "access/xlogrecovery.h"
#include "common/ip.h"
#include "common/string.h"
#include "libpq/libpq.h"
#include "libpq/libpq-be.h"
#include "libpq/pqformat.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "replication/walsender.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/procsignal.h"
#include "storage/proc.h"
#include "tcop/backend_startup.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/guc_hooks.h"
#include "utils/injection_point.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/timeout.h"
#include "utils/varlena.h"

/*
 * GUCs
 *
 * GUC 参数
 */
bool		Trace_connection_negotiation = false;
uint32		log_connections = 0;
char	   *log_connections_string = NULL;

/*
 * Other globals
 *
 * 其他全局变量
 */

/*
 * ConnectionTiming stores timestamps of various points in connection
 * establishment and setup.
 * ready_for_use is initialized to a special value here so we can check if
 * we've already set it before doing so in PostgresMain().
 *
 * ConnectionTiming 存储连接建立和设置过程中各个时间点的时间戳。
 * ready_for_use 在这里初始化为一个特殊值，这样在 PostgresMain() 中设置
 * 它之前，可以检查它是否已经被设置过。
 */
ConnectionTiming conn_timing = {.ready_for_use = TIMESTAMP_MINUS_INFINITY};

static void BackendInitialize(ClientSocket *client_sock, CAC_state cac);
static int	ProcessSSLStartup(Port *port);
static int	ProcessStartupPacket(Port *port, bool ssl_done, bool gss_done);
static void ProcessCancelRequestPacket(Port *port, void *pkt, int pktlen);
static void SendNegotiateProtocolVersion(List *unrecognized_protocol_options);
static void process_startup_packet_die(SIGNAL_ARGS);
static void StartupPacketTimeoutHandler(void);
static bool validate_log_connections_options(List *elemlist, uint32 *flags);

/*
 * Entry point for a new backend process.
 *
 * 新后端进程的入口点。
 *
 * Initialize the connection, read the startup packet, authenticate the
 * client, and start the main processing loop.
 *
 * 初始化连接，读取启动包，对客户端进行认证，并启动主处理循环。
 */
void
BackendMain(const void *startup_data, size_t startup_data_len)
{
	const BackendStartupData *bsdata = startup_data;

	Assert(startup_data_len == sizeof(BackendStartupData));
	Assert(MyClientSocket != NULL);

#ifdef EXEC_BACKEND

	/*
	 * Need to reinitialize the SSL library in the backend, since the context
	 * structures contain function pointers and cannot be passed through the
	 * parameter file.
	 *
	 * 需要在后端中重新初始化 SSL 库，因为上下文结构包含函数指针，无法通过
	 * 参数文件传递。
	 *
	 * If for some reason reload fails (maybe the user installed broken key
	 * files), soldier on without SSL; that's better than all connections
	 * becoming impossible.
	 *
	 * 如果由于某种原因重新加载失败（也许用户安装了损坏的密钥文件），就不使用
	 * SSL 继续运行；这比让所有连接都无法建立要好。
	 *
	 * XXX should we do this in all child processes?  For the moment it's
	 * enough to do it in backend children.
	 *
	 * XXX 我们是否应该在所有子进程中都这样做？目前只在后端子进程中这样做就
	 * 足够了。
	 */
#ifdef USE_SSL
	if (EnableSSL)
	{
		if (secure_initialize(false) == 0)
			LoadedSSL = true;
		else
			ereport(LOG,
					(errmsg("SSL configuration could not be loaded in child process")));
	}
#endif
#endif

	/*
	 * Perform additional initialization and collect startup packet
	 *
	 * 执行额外初始化并收集启动包。
	 */
	BackendInitialize(MyClientSocket, bsdata->canAcceptConnections);

	/*
	 * Create a per-backend PGPROC struct in shared memory.  We must do this
	 * before we can use LWLocks or access any shared memory.
	 *
	 * 在共享内存中为每个后端创建 PGPROC 结构。必须在使用 LWLock 或访问任何
	 * 共享内存之前完成这一步。
	 */
	InitProcess();

	/*
	 * Make sure we aren't in PostmasterContext anymore.  (We can't delete it
	 * just yet, though, because InitPostgres will need the HBA data.)
	 *
	 * 确保我们已经不在 PostmasterContext 中。（不过还不能删除它，因为
	 * InitPostgres 将需要 HBA 数据。）
	 */
	MemoryContextSwitchTo(TopMemoryContext);

	PostgresMain(MyProcPort->database_name, MyProcPort->user_name);
}


/*
 * BackendInitialize -- initialize an interactive (postmaster-child)
 *				backend process, and collect the client's startup packet.
 *
 * returns: nothing.  Will not return at all if there's any failure.
 *
 * 返回：无。如果出现任何故障，将完全不会返回。
 *
 * Note: this code does not depend on having any access to shared memory.
 * Indeed, our approach to SIGTERM/timeout handling *requires* that
 * shared memory not have been touched yet; see comments within.
 * In the EXEC_BACKEND case, we are physically attached to shared memory
 * but have not yet set up most of our local pointers to shmem structures.
 *
 * 注意：此代码不依赖于对共享内存的任何访问。实际上，我们处理 SIGTERM/超时
 * 的方法要求尚未接触共享内存；参见下方注释。在 EXEC_BACKEND 情况下，我们
 * 在物理上已经附加到共享内存，但尚未设置大多数指向共享内存结构的本地指针。
 */
static void
BackendInitialize(ClientSocket *client_sock, CAC_state cac)
{
	int			status;
	int			ret;
	Port	   *port;
	char		remote_host[NI_MAXHOST];
	char		remote_port[NI_MAXSERV];
	StringInfoData ps_data;
	MemoryContext oldcontext;

	/*
	 * Tell fd.c about the long-lived FD associated with the client_sock
	 *
	 * 告知 fd.c 与 client_sock 关联的长生命周期文件描述符。
	 */
	ReserveExternalFD();

	/*
	 * PreAuthDelay is a debugging aid for investigating problems in the
	 * authentication cycle: it can be set in postgresql.conf to allow time to
	 * attach to the newly-forked backend with a debugger.  (See also
	 * PostAuthDelay, which we allow clients to pass through PGOPTIONS, but it
	 * is not honored until after authentication.)
	 *
	 * PreAuthDelay 是用于调查认证周期中问题的调试辅助项：可以在
	 * postgresql.conf 中设置它，以便有时间用调试器附加到新 fork 的后端。
	 * （另见 PostAuthDelay，我们允许客户端通过 PGOPTIONS 传入它，但只有在
	 * 认证之后才会生效。）
	 */
	if (PreAuthDelay > 0)
		pg_usleep(PreAuthDelay * 1000000L);

	/*
	 * This flag will remain set until InitPostgres finishes authentication
	 *
	 * 在 InitPostgres 完成认证之前，此标志将一直保持设置状态。
	 */
	/*
	 * limit visibility of log messages
	 *
	 * 限制日志消息的可见性。
	 */
	ClientAuthInProgress = true;

	/*
	 * Initialize libpq and enable reporting of ereport errors to the client.
	 * Must do this now because authentication uses libpq to send messages.
	 *
	 * 初始化 libpq，并允许将 ereport 错误报告给客户端。必须现在执行此操作，
	 * 因为认证会使用 libpq 发送消息。
	 *
	 * The Port structure and all data structures attached to it are allocated
	 * in TopMemoryContext, so that they survive into PostgresMain execution.
	 * We need not worry about leaking this storage on failure, since we
	 * aren't in the postmaster process anymore.
	 *
	 * Port 结构及其附带的所有数据结构都分配在 TopMemoryContext 中，以便它们
	 * 能在进入 PostgresMain 执行后继续存在。失败时无需担心泄漏这些存储，因为
	 * 我们已经不在 postmaster 进程中了。
	 */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);
	port = MyProcPort = pq_init(client_sock);
	MemoryContextSwitchTo(oldcontext);

	/*
	 * now safe to ereport to client
	 *
	 * 现在可以安全地向客户端 ereport。
	 */
	whereToSendOutput = DestRemote;

	/*
	 * set these to empty in case they are needed before we set them up
	 *
	 * 将这些值设为空，以防在设置它们之前就需要使用。
	 */
	port->remote_host = "";
	port->remote_port = "";

	/*
	 * We arrange to do _exit(1) if we receive SIGTERM or timeout while trying
	 * to collect the startup packet; while SIGQUIT results in _exit(2).
	 * Otherwise the postmaster cannot shutdown the database FAST or IMMED
	 * cleanly if a buggy client fails to send the packet promptly.
	 *
	 * 我们安排在尝试收集启动包时，如果收到 SIGTERM 或发生超时，就执行
	 * _exit(1)；而 SIGQUIT 会导致 _exit(2)。否则，如果有缺陷的客户端未能及时
	 * 发送数据包，postmaster 就无法干净地执行 FAST 或 IMMED 数据库关闭。
	 *
	 * Exiting with _exit(1) is only possible because we have not yet touched
	 * shared memory; therefore no outside-the-process state needs to get
	 * cleaned up.
	 *
	 * 能够用 _exit(1) 退出只是因为我们还没有接触共享内存；因此不需要清理任何
	 * 进程外部状态。
	 */
	pqsignal(SIGTERM, process_startup_packet_die);
	/*
	 * SIGQUIT handler was already set up by InitPostmasterChild
	 *
	 * SIGQUIT 处理器已经由 InitPostmasterChild 设置。
	 */
	/*
	 * establishes SIGALRM handler
	 *
	 * 建立 SIGALRM 处理器。
	 */
	InitializeTimeouts();
	sigprocmask(SIG_SETMASK, &StartupBlockSig, NULL);

	/*
	 * Get the remote host name and port for logging and status display.
	 *
	 * 获取远程主机名和端口，用于日志记录和状态显示。
	 */
	remote_host[0] = '\0';
	remote_port[0] = '\0';
	if ((ret = pg_getnameinfo_all(&port->raddr.addr, port->raddr.salen,
								  remote_host, sizeof(remote_host),
								  remote_port, sizeof(remote_port),
								  (log_hostname ? 0 : NI_NUMERICHOST) | NI_NUMERICSERV)) != 0)
		ereport(WARNING,
				(errmsg_internal("pg_getnameinfo_all() failed: %s",
								 gai_strerror(ret))));

	/*
	 * Save remote_host and remote_port in port structure (after this, they
	 * will appear in log_line_prefix data for log messages).
	 *
	 * 将 remote_host 和 remote_port 保存在 port 结构中（之后它们会出现在日志消息的
	 * log_line_prefix 数据中）。
	 */
	port->remote_host = MemoryContextStrdup(TopMemoryContext, remote_host);
	port->remote_port = MemoryContextStrdup(TopMemoryContext, remote_port);

	/*
	 * And now we can log that the connection was received, if enabled
	 *
	 * 现在如果启用了相应选项，就可以记录已收到连接。
	 */
	if (log_connections & LOG_CONNECTION_RECEIPT)
	{
		if (remote_port[0])
			ereport(LOG,
					(errmsg("connection received: host=%s port=%s",
							remote_host,
							remote_port)));
		else
			ereport(LOG,
					(errmsg("connection received: host=%s",
							remote_host)));
	}

	/*
	 * For testing client error handling
	 *
	 * 用于测试客户端错误处理。
	 */
#ifdef USE_INJECTION_POINTS
	INJECTION_POINT("backend-initialize", NULL);
	if (IS_INJECTION_POINT_ATTACHED("backend-initialize-v2-error"))
	{
		/*
		 * This simulates an early error from a pre-v14 server, which used the
		 * version 2 protocol for any errors that occurred before processing
		 * the startup packet.
		 *
		 * 这会模拟 v14 之前服务器的早期错误；这种服务器会对处理启动包之前发生的
		 * 任何错误使用版本 2 协议。
		 */
		FrontendProtocol = PG_PROTOCOL(2, 0);
		elog(FATAL, "protocol version 2 error triggered");
	}
#endif

	/*
	 * If we did a reverse lookup to name, we might as well save the results
	 * rather than possibly repeating the lookup during authentication.
	 *
	 * 如果我们反向查找得到了名称，就不妨保存结果，而不是可能在认证期间重复查找。
	 *
	 * Note that we don't want to specify NI_NAMEREQD above, because then we'd
	 * get nothing useful for a client without an rDNS entry.  Therefore, we
	 * must check whether we got a numeric IPv4 or IPv6 address, and not save
	 * it into remote_hostname if so.  (This test is conservative and might
	 * sometimes classify a hostname as numeric, but an error in that
	 * direction is safe; it only results in a possible extra lookup.)
	 *
	 * 注意我们不想在上面指定 NI_NAMEREQD，因为那样对于没有 rDNS 记录的客户端就
	 * 得不到有用信息。因此，我们必须检查得到的是否为数字形式的 IPv4 或 IPv6
	 * 地址；如果是，就不要把它保存到 remote_hostname 中。（此测试较保守，有时
	 * 可能会把主机名归类为数字形式，但这种方向的错误是安全的；它只会导致一次
	 * 可能的额外查找。）
	 */
	if (log_hostname &&
		ret == 0 &&
		strspn(remote_host, "0123456789.") < strlen(remote_host) &&
		strspn(remote_host, "0123456789ABCDEFabcdef:") < strlen(remote_host))
	{
		port->remote_hostname = MemoryContextStrdup(TopMemoryContext, remote_host);
	}

	/*
	 * Ready to begin client interaction.  We will give up and _exit(1) after
	 * a time delay, so that a broken client can't hog a connection
	 * indefinitely.  PreAuthDelay and any DNS interactions above don't count
	 * against the time limit.
	 *
	 * 准备开始客户端交互。我们会在一段延迟后放弃并执行 _exit(1)，这样损坏的
	 * 客户端就不能无限期占用连接。PreAuthDelay 以及上面的任何 DNS 交互都不计入
	 * 此时间限制。
	 *
	 * Note: AuthenticationTimeout is applied here while waiting for the
	 * startup packet, and then again in InitPostgres for the duration of any
	 * authentication operations.  So a hostile client could tie up the
	 * process for nearly twice AuthenticationTimeout before we kick him off.
	 *
	 * 注意：AuthenticationTimeout 在这里用于等待启动包，之后在 InitPostgres 中
	 * 又会用于任何认证操作的持续时间。因此，恶意客户端可能在被断开之前占用进程
	 * 接近两倍的 AuthenticationTimeout 时间。
	 *
	 * Note: because PostgresMain will call InitializeTimeouts again, the
	 * registration of STARTUP_PACKET_TIMEOUT will be lost.  This is okay
	 * since we never use it again after this function.
	 *
	 * 注意：由于 PostgresMain 会再次调用 InitializeTimeouts，
	 * STARTUP_PACKET_TIMEOUT 的注册将会丢失。这没有问题，因为在此函数之后我们
	 * 不会再使用它。
	 */
	RegisterTimeout(STARTUP_PACKET_TIMEOUT, StartupPacketTimeoutHandler);
	enable_timeout_after(STARTUP_PACKET_TIMEOUT, AuthenticationTimeout * 1000);

	/*
	 * Handle direct SSL handshake
	 *
	 * 处理直接 SSL 握手。
	 */
	status = ProcessSSLStartup(port);

	/*
	 * Receive the startup packet (which might turn out to be a cancel request
	 * packet).
	 *
	 * 接收启动包（它可能实际是取消请求包）。
	 */
	if (status == STATUS_OK)
		status = ProcessStartupPacket(port, false, false);

	/*
	 * If we're going to reject the connection due to database state, say so
	 * now instead of wasting cycles on an authentication exchange. (This also
	 * allows a pg_ping utility to be written.)
	 *
	 * 如果要因数据库状态拒绝连接，就现在说明，而不是在认证交换上浪费周期。
	 * （这也允许编写 pg_ping 工具。）
	 */
	if (status == STATUS_OK)
	{
		switch (cac)
		{
			case CAC_STARTUP:
				ereport(FATAL,
						(errcode(ERRCODE_CANNOT_CONNECT_NOW),
						 errmsg("the database system is starting up")));
				break;
			case CAC_NOTHOTSTANDBY:
				if (!EnableHotStandby)
					ereport(FATAL,
							(errcode(ERRCODE_CANNOT_CONNECT_NOW),
							 errmsg("the database system is not accepting connections"),
							 errdetail("Hot standby mode is disabled.")));
				else if (reachedConsistency)
					ereport(FATAL,
							(errcode(ERRCODE_CANNOT_CONNECT_NOW),
							 errmsg("the database system is not yet accepting connections"),
							 errdetail("Recovery snapshot is not yet ready for hot standby."),
							 errhint("To enable hot standby, close write transactions with more than %d subtransactions on the primary server.",
									 PGPROC_MAX_CACHED_SUBXIDS)));
				else
					ereport(FATAL,
							(errcode(ERRCODE_CANNOT_CONNECT_NOW),
							 errmsg("the database system is not yet accepting connections"),
							 errdetail("Consistent recovery state has not been yet reached.")));
				break;
			case CAC_SHUTDOWN:
				ereport(FATAL,
						(errcode(ERRCODE_CANNOT_CONNECT_NOW),
						 errmsg("the database system is shutting down")));
				break;
			case CAC_RECOVERY:
				ereport(FATAL,
						(errcode(ERRCODE_CANNOT_CONNECT_NOW),
						 errmsg("the database system is in recovery mode")));
				break;
			case CAC_TOOMANY:
				ereport(FATAL,
						(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
						 errmsg("sorry, too many clients already")));
				break;
			case CAC_OK:
				break;
		}
	}

	/*
	 * Disable the timeout, and prevent SIGTERM again.
	 *
	 * 禁用超时，并再次阻止 SIGTERM。
	 */
	disable_timeout(STARTUP_PACKET_TIMEOUT, false);
	sigprocmask(SIG_SETMASK, &BlockSig, NULL);

	/*
	 * As a safety check that nothing in startup has yet performed
	 * shared-memory modifications that would need to be undone if we had
	 * exited through SIGTERM or timeout above, check that no on_shmem_exit
	 * handlers have been registered yet.  (This isn't terribly bulletproof,
	 * since someone might misuse an on_proc_exit handler for shmem cleanup,
	 * but it's a cheap and helpful check.  We cannot disallow on_proc_exit
	 * handlers unfortunately, since pq_init() already registered one.)
	 *
	 * 作为安全检查，确认启动期间尚未执行任何共享内存修改，否则如果上面通过
	 * SIGTERM 或超时退出，就需要撤销这些修改；因此检查尚未注册任何
	 * on_shmem_exit 处理器。（这并非万无一失，因为有人可能误用 on_proc_exit
	 * 处理器来清理共享内存，但这是一个代价低且有帮助的检查。遗憾的是，我们不能
	 * 禁止 on_proc_exit 处理器，因为 pq_init() 已经注册了一个。）
	 */
	check_on_shmem_exit_lists_are_empty();

	/*
	 * Stop here if it was bad or a cancel packet.  ProcessStartupPacket
	 * already did any appropriate error reporting.
	 *
	 * 如果它是坏包或取消包，就在这里停止。ProcessStartupPacket 已经完成任何适当的
	 * 错误报告。
	 */
	if (status != STATUS_OK)
		proc_exit(0);

	/*
	 * Now that we have the user and database name, we can set the process
	 * title for ps.  It's good to do this as early as possible in startup.
	 *
	 * 既然已经有了用户名和数据库名，就可以为 ps 设置进程标题。在启动过程中越早
	 * 执行此操作越好。
	 */
	initStringInfo(&ps_data);
	if (am_walsender)
		appendStringInfo(&ps_data, "%s ", GetBackendTypeDesc(B_WAL_SENDER));
	appendStringInfo(&ps_data, "%s ", port->user_name);
	if (port->database_name[0] != '\0')
		appendStringInfo(&ps_data, "%s ", port->database_name);
	appendStringInfoString(&ps_data, port->remote_host);
	if (port->remote_port[0] != '\0')
		appendStringInfo(&ps_data, "(%s)", port->remote_port);

	init_ps_display(ps_data.data);
	pfree(ps_data.data);

	set_ps_display("initializing");
}

/*
 * Check for a direct SSL connection.
 *
 * 检查是否为直接 SSL 连接。
 *
 * This happens before the startup packet so we are careful not to actually
 * read any bytes from the stream if it's not a direct SSL connection.
 *
 * 这发生在启动包之前，因此如果它不是直接 SSL 连接，我们会小心避免真正从流中
 * 读取任何字节。
 */
static int
ProcessSSLStartup(Port *port)
{
	int			firstbyte;

	Assert(!port->ssl_in_use);

	pq_startmsgread();
	firstbyte = pq_peekbyte();
	pq_endmsgread();
	if (firstbyte == EOF)
	{
		/*
		 * Like in ProcessStartupPacket, if we get no data at all, don't
		 * clutter the log with a complaint.
		 *
		 * 与 ProcessStartupPacket 中一样，如果完全没有收到数据，就不要用抱怨信息
		 * 扰乱日志。
		 */
		return STATUS_ERROR;
	}

	if (firstbyte != 0x16)
	{
		/*
		 * Not an SSL handshake message
		 *
		 * 不是 SSL 握手消息。
		 */
		return STATUS_OK;
	}

	/*
	 * First byte indicates standard SSL handshake message
	 *
	 * 第一个字节表示标准 SSL 握手消息。
	 *
	 * (It can't be a Postgres startup length because in network byte order
	 * that would be a startup packet hundreds of megabytes long)
	 *
	 * （它不可能是 Postgres 启动长度，因为按网络字节序解释时，那将是一个长达
	 * 数百 MB 的启动包。）
	 */

#ifdef USE_SSL
	if (!LoadedSSL || port->laddr.addr.ss_family == AF_UNIX)
	{
		/*
		 * SSL not supported
		 *
		 * 不支持 SSL。
		 */
		goto reject;
	}

	if (secure_open_server(port) == -1)
	{
		/*
		 * we assume secure_open_server() sent an appropriate TLS alert
		 * already
		 *
		 * 我们假定 secure_open_server() 已经发送了适当的 TLS 警报。
		 */
		goto reject;
	}
	Assert(port->ssl_in_use);

	if (!port->alpn_used)
	{
		ereport(COMMERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("received direct SSL connection request without ALPN protocol negotiation extension")));
		goto reject;
	}

	if (Trace_connection_negotiation)
		ereport(LOG,
				(errmsg("direct SSL connection accepted")));
	return STATUS_OK;
#else
	/*
	 * SSL not supported by this build
	 *
	 * 此构建不支持 SSL。
	 */
	goto reject;
#endif

reject:
	if (Trace_connection_negotiation)
		ereport(LOG,
				(errmsg("direct SSL connection rejected")));
	return STATUS_ERROR;
}

/*
 * Read a client's startup packet and do something according to it.
 *
 * 读取客户端的启动包，并根据其内容采取相应操作。
 *
 * Returns STATUS_OK or STATUS_ERROR, or might call ereport(FATAL) and
 * not return at all.
 *
 * 返回 STATUS_OK 或 STATUS_ERROR，或者可能调用 ereport(FATAL) 而完全不返回。
 *
 * (Note that ereport(FATAL) stuff is sent to the client, so only use it
 * if that's what you want.  Return STATUS_ERROR if you don't want to
 * send anything to the client, which would typically be appropriate
 * if we detect a communications failure.)
 *
 * （注意 ereport(FATAL) 内容会发送给客户端，因此只有在确实需要这样做时才使用它。
 * 如果不想向客户端发送任何内容，就返回 STATUS_ERROR；当检测到通信失败时，这通常是
 * 合适的做法。）
 *
 * Set ssl_done and/or gss_done when negotiation of an encrypted layer
 * (currently, TLS or GSSAPI) is completed. A successful negotiation of either
 * encryption layer sets both flags, but a rejected negotiation sets only the
 * flag for that layer, since the client may wish to try the other one. We
 * should make no assumption here about the order in which the client may make
 * requests.
 *
 * 当加密层（目前为 TLS 或 GSSAPI）协商完成时，设置 ssl_done 和/或 gss_done。
 * 任一加密层协商成功都会设置两个标志，但协商被拒绝时只设置该层对应的标志，
 * 因为客户端可能希望尝试另一种加密层。这里不应假设客户端发起请求的顺序。
 */
static int
ProcessStartupPacket(Port *port, bool ssl_done, bool gss_done)
{
	int32		len;
	char	   *buf;
	ProtocolVersion proto;
	MemoryContext oldcontext;

	pq_startmsgread();

	/*
	 * Grab the first byte of the length word separately, so that we can tell
	 * whether we have no data at all or an incomplete packet.  (This might
	 * sound inefficient, but it's not really, because of buffering in
	 * pqcomm.c.)
	 *
	 * 单独抓取长度字的第一个字节，这样就能区分完全没有数据和数据包不完整。
	 * （这听起来可能效率低，但实际上不是，因为 pqcomm.c 中有缓冲。）
	 */
	if (pq_getbytes(&len, 1) == EOF)
	{
		/*
		 * If we get no data at all, don't clutter the log with a complaint;
		 * such cases often occur for legitimate reasons.  An example is that
		 * we might be here after responding to NEGOTIATE_SSL_CODE, and if the
		 * client didn't like our response, it'll probably just drop the
		 * connection.  Service-monitoring software also often just opens and
		 * closes a connection without sending anything.  (So do port
		 * scanners, which may be less benign, but it's not really our job to
		 * notice those.)
		 *
		 * 如果完全没有收到数据，就不要用抱怨信息扰乱日志；这种情况通常出于合法原因
		 * 发生。例如，我们可能是在响应 NEGOTIATE_SSL_CODE 之后到达这里，如果客户端
		 * 不喜欢我们的响应，它很可能会直接断开连接。服务监控软件也经常只是打开并关闭
		 * 连接而不发送任何内容。（端口扫描器也是如此，它们可能不那么友善，但注意它们
		 * 并不真正是我们的职责。）
		 */
		return STATUS_ERROR;
	}

	if (pq_getbytes(((char *) &len) + 1, 3) == EOF)
	{
		/*
		 * Got a partial length word, so bleat about that
		 *
		 * 收到了部分长度字，因此对此发出抱怨。
		 */
		if (!ssl_done && !gss_done)
			ereport(COMMERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("incomplete startup packet")));
		return STATUS_ERROR;
	}

	len = pg_ntoh32(len);
	len -= 4;

	if (len < (int32) sizeof(ProtocolVersion) ||
		len > MAX_STARTUP_PACKET_LENGTH)
	{
		ereport(COMMERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("invalid length of startup packet")));
		return STATUS_ERROR;
	}

	/*
	 * Allocate space to hold the startup packet, plus one extra byte that's
	 * initialized to be zero.  This ensures we will have null termination of
	 * all strings inside the packet.
	 *
	 * 分配空间来保存启动包，并额外分配一个初始化为零的字节。这确保包内所有字符串都
	 * 以空字符结尾。
	 */
	buf = palloc(len + 1);
	buf[len] = '\0';

	if (pq_getbytes(buf, len) == EOF)
	{
		ereport(COMMERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("incomplete startup packet")));
		return STATUS_ERROR;
	}
	pq_endmsgread();

	/*
	 * The first field is either a protocol version number or a special
	 * request code.
	 *
	 * 第一个字段要么是协议版本号，要么是特殊请求码。
	 */
	port->proto = proto = pg_ntoh32(*((ProtocolVersion *) buf));

	if (proto == CANCEL_REQUEST_CODE)
	{
		ProcessCancelRequestPacket(port, buf, len);
		/*
		 * Not really an error, but we don't want to proceed further
		 *
		 * 这并不是真正的错误，但我们不想继续处理。
		 */
		return STATUS_ERROR;
	}

	if (proto == NEGOTIATE_SSL_CODE && !ssl_done)
	{
		char		SSLok;

#ifdef USE_SSL

		/*
		 * No SSL when disabled or on Unix sockets.
		 *
		 * 禁用 SSL 或使用 Unix 套接字时不使用 SSL。
		 *
		 * Also no SSL negotiation if we already have a direct SSL connection
		 *
		 * 如果我们已经有直接 SSL 连接，也不再进行 SSL 协商。
		 */
		if (!LoadedSSL || port->laddr.addr.ss_family == AF_UNIX || port->ssl_in_use)
			SSLok = 'N';
		else
			/*
			 * Support for SSL
			 *
			 * 支持 SSL。
			 */
			SSLok = 'S';
#else
		/*
		 * No support for SSL
		 *
		 * 不支持 SSL。
		 */
		SSLok = 'N';
#endif

		if (Trace_connection_negotiation)
		{
			if (SSLok == 'S')
				ereport(LOG,
						(errmsg("SSLRequest accepted")));
			else
				ereport(LOG,
						(errmsg("SSLRequest rejected")));
		}

		while (secure_write(port, &SSLok, 1) != 1)
		{
			if (errno == EINTR)
				/*
				 * if interrupted, just retry
				 *
				 * 如果被中断，就直接重试。
				 */
				continue;
			ereport(COMMERROR,
					(errcode_for_socket_access(),
					 errmsg("failed to send SSL negotiation response: %m")));
			/*
			 * close the connection
			 *
			 * 关闭连接。
			 */
			return STATUS_ERROR;
		}

#ifdef USE_SSL
		if (SSLok == 'S' && secure_open_server(port) == -1)
			return STATUS_ERROR;
#endif

		/*
		 * At this point we should have no data already buffered.  If we do,
		 * it was received before we performed the SSL handshake, so it wasn't
		 * encrypted and indeed may have been injected by a man-in-the-middle.
		 * We report this case to the client.
		 *
		 * 此时不应已有缓冲数据。如果有，这些数据是在执行 SSL 握手之前收到的，因此
		 * 没有加密，实际上可能是由中间人注入的。我们会向客户端报告这种情况。
		 */
		if (pq_buffer_remaining_data() > 0)
			ereport(FATAL,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("received unencrypted data after SSL request"),
					 errdetail("This could be either a client-software bug or evidence of an attempted man-in-the-middle attack.")));

		/*
		 * regular startup packet, cancel, etc packet should follow, but not
		 * another SSL negotiation request, and a GSS request should only
		 * follow if SSL was rejected (client may negotiate in either order)
		 *
		 * 接下来应该是普通启动包、取消包等，而不是另一个 SSL 协商请求；只有在 SSL
		 * 被拒绝时，才应该跟随 GSS 请求（客户端可以按任一顺序协商）。
		 */
		return ProcessStartupPacket(port, true, SSLok == 'S');
	}
	else if (proto == NEGOTIATE_GSS_CODE && !gss_done)
	{
		char		GSSok = 'N';

#ifdef ENABLE_GSS
		/*
		 * No GSSAPI encryption when on Unix socket
		 *
		 * 使用 Unix 套接字时不使用 GSSAPI 加密。
		 */
		if (port->laddr.addr.ss_family != AF_UNIX)
			GSSok = 'G';
#endif

		if (Trace_connection_negotiation)
		{
			if (GSSok == 'G')
				ereport(LOG,
						(errmsg("GSSENCRequest accepted")));
			else
				ereport(LOG,
						(errmsg("GSSENCRequest rejected")));
		}

		while (secure_write(port, &GSSok, 1) != 1)
		{
			if (errno == EINTR)
				continue;
			ereport(COMMERROR,
					(errcode_for_socket_access(),
					 errmsg("failed to send GSSAPI negotiation response: %m")));
			/*
			 * close the connection
			 *
			 * 关闭连接。
			 */
			return STATUS_ERROR;
		}

#ifdef ENABLE_GSS
		if (GSSok == 'G' && secure_open_gssapi(port) == -1)
			return STATUS_ERROR;
#endif

		/*
		 * At this point we should have no data already buffered.  If we do,
		 * it was received before we performed the GSS handshake, so it wasn't
		 * encrypted and indeed may have been injected by a man-in-the-middle.
		 * We report this case to the client.
		 *
		 * 此时不应已有缓冲数据。如果有，这些数据是在执行 GSS 握手之前收到的，因此
		 * 没有加密，实际上可能是由中间人注入的。我们会向客户端报告这种情况。
		 */
		if (pq_buffer_remaining_data() > 0)
			ereport(FATAL,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("received unencrypted data after GSSAPI encryption request"),
					 errdetail("This could be either a client-software bug or evidence of an attempted man-in-the-middle attack.")));

		/*
		 * regular startup packet, cancel, etc packet should follow, but not
		 * another GSS negotiation request, and an SSL request should only
		 * follow if GSS was rejected (client may negotiate in either order)
		 *
		 * 接下来应该是普通启动包、取消包等，而不是另一个 GSS 协商请求；只有在 GSS
		 * 被拒绝时，才应该跟随 SSL 请求（客户端可以按任一顺序协商）。
		 */
		return ProcessStartupPacket(port, GSSok == 'G', true);
	}

	/*
	 * Could add additional special packet types here
	 *
	 * 可以在这里添加其他特殊数据包类型。
	 */

	/*
	 * Set FrontendProtocol now so that ereport() knows what format to send if
	 * we fail during startup. We use the protocol version requested by the
	 * client unless it's higher than the latest version we support. It's
	 * possible that error message fields might look different in newer
	 * protocol versions, but that's something those new clients should be
	 * able to deal with.
	 *
	 * 现在设置 FrontendProtocol，这样如果启动期间失败，ereport() 就知道要发送什么
	 * 格式。除非客户端请求的协议版本高于我们支持的最新版本，否则使用客户端请求的
	 * 协议版本。较新的协议版本中的错误消息字段可能看起来不同，但这是那些新客户端
	 * 应该能够处理的事情。
	 */
	FrontendProtocol = Min(proto, PG_PROTOCOL_LATEST);

	/*
	 * Check that the major protocol version is in range.
	 *
	 * 检查主协议版本是否在范围内。
	 */
	if (PG_PROTOCOL_MAJOR(proto) < PG_PROTOCOL_MAJOR(PG_PROTOCOL_EARLIEST) ||
		PG_PROTOCOL_MAJOR(proto) > PG_PROTOCOL_MAJOR(PG_PROTOCOL_LATEST))
		ereport(FATAL,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("unsupported frontend protocol %u.%u: server supports %u.0 to %u.%u",
						PG_PROTOCOL_MAJOR(proto), PG_PROTOCOL_MINOR(proto),
						PG_PROTOCOL_MAJOR(PG_PROTOCOL_EARLIEST),
						PG_PROTOCOL_MAJOR(PG_PROTOCOL_LATEST),
						PG_PROTOCOL_MINOR(PG_PROTOCOL_LATEST))));

	/*
	 * Now fetch parameters out of startup packet and save them into the Port
	 * structure.
	 *
	 * 现在从启动包中取出参数，并将其保存到 Port 结构中。
	 */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	/*
	 * Handle protocol version 3 startup packet
	 *
	 * 处理协议版本 3 的启动包。
	 */
	{
		int32		offset = sizeof(ProtocolVersion);
		List	   *unrecognized_protocol_options = NIL;

		/*
		 * Scan packet body for name/option pairs.  We can assume any string
		 * beginning within the packet body is null-terminated, thanks to
		 * zeroing extra byte above.
		 *
		 * 扫描包体中的名称/选项对。由于上面将额外字节清零，可以假设包体内开始的
		 * 任何字符串都以空字符结尾。
		 */
		port->guc_options = NIL;

		while (offset < len)
		{
			char	   *nameptr = buf + offset;
			int32		valoffset;
			char	   *valptr;

			if (*nameptr == '\0')
				/*
				 * found packet terminator
				 *
				 * 找到包终止符。
				 */
				break;
			valoffset = offset + strlen(nameptr) + 1;
			if (valoffset >= len)
				/*
				 * missing value, will complain below
				 *
				 * 缺少值，稍后会报错。
				 */
				break;
			valptr = buf + valoffset;

			if (strcmp(nameptr, "database") == 0)
				port->database_name = pstrdup(valptr);
			else if (strcmp(nameptr, "user") == 0)
				port->user_name = pstrdup(valptr);
			else if (strcmp(nameptr, "options") == 0)
				port->cmdline_options = pstrdup(valptr);
			else if (strcmp(nameptr, "replication") == 0)
			{
				/*
				 * Due to backward compatibility concerns the replication
				 * parameter is a hybrid beast which allows the value to be
				 * either boolean or the string 'database'. The latter
				 * connects to a specific database which is e.g. required for
				 * logical decoding while.
				 *
				 * 出于向后兼容考虑，replication 参数是一个混合体，允许其值为布尔值或
				 * 字符串 'database'。后者会连接到特定数据库，例如逻辑解码需要这样做。
				 */
				if (strcmp(valptr, "database") == 0)
				{
					am_walsender = true;
					am_db_walsender = true;
				}
				else if (!parse_bool(valptr, &am_walsender))
					ereport(FATAL,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("invalid value for parameter \"%s\": \"%s\"",
									"replication",
									valptr),
							 errhint("Valid values are: \"false\", 0, \"true\", 1, \"database\".")));
			}
			else if (strncmp(nameptr, "_pq_.", 5) == 0)
			{
				/*
				 * Any option beginning with _pq_. is reserved for use as a
				 * protocol-level option, but at present no such options are
				 * defined.
				 *
				 * 任何以 _pq_. 开头的选项都保留用作协议级选项，但目前尚未定义此类选项。
				 */
				unrecognized_protocol_options =
					lappend(unrecognized_protocol_options, pstrdup(nameptr));
			}
			else
			{
				/*
				 * Assume it's a generic GUC option
				 *
				 * 假定它是通用 GUC 选项。
				 */
				port->guc_options = lappend(port->guc_options,
											pstrdup(nameptr));
				port->guc_options = lappend(port->guc_options,
											pstrdup(valptr));

				/*
				 * Copy application_name to port if we come across it.  This
				 * is done so we can log the application_name in the
				 * connection authorization message.  Note that the GUC would
				 * be used but we haven't gone through GUC setup yet.
				 *
				 * 如果遇到 application_name，就将其复制到 port 中。这样做是为了能在连接
				 * 授权消息中记录 application_name。注意本应使用 GUC，但我们还没有经过
				 * GUC 设置。
				 */
				if (strcmp(nameptr, "application_name") == 0)
				{
					port->application_name = pg_clean_ascii(valptr, 0);
				}
			}
			offset = valoffset + strlen(valptr) + 1;
		}

		/*
		 * If we didn't find a packet terminator exactly at the end of the
		 * given packet length, complain.
		 *
		 * 如果没有在给定包长度的正好末尾找到包终止符，就报错。
		 */
		if (offset != len - 1)
			ereport(FATAL,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("invalid startup packet layout: expected terminator as last byte")));

		/*
		 * If the client requested a newer protocol version or if the client
		 * requested any protocol options we didn't recognize, let them know
		 * the newest minor protocol version we do support and the names of
		 * any unrecognized options.
		 *
		 * 如果客户端请求了较新的协议版本，或者请求了任何我们无法识别的协议选项，就告知
		 * 它们我们支持的最新次协议版本以及所有无法识别选项的名称。
		 */
		if (PG_PROTOCOL_MINOR(proto) > PG_PROTOCOL_MINOR(PG_PROTOCOL_LATEST) ||
			unrecognized_protocol_options != NIL)
			SendNegotiateProtocolVersion(unrecognized_protocol_options);
	}

	/*
	 * Check a user name was given.
	 *
	 * 检查是否给出了用户名。
	 */
	if (port->user_name == NULL || port->user_name[0] == '\0')
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
				 errmsg("no PostgreSQL user name specified in startup packet")));

	/*
	 * The database defaults to the user name.
	 *
	 * 数据库默认使用用户名。
	 */
	if (port->database_name == NULL || port->database_name[0] == '\0')
		port->database_name = pstrdup(port->user_name);

	/*
	 * Truncate given database and user names to length of a Postgres name.
	 * This avoids lookup failures when overlength names are given.
	 *
	 * 将给定的数据库名和用户名截断到 Postgres 名称的长度。这可以避免给出超长名称时
	 * 查找失败。
	 */
	if (strlen(port->database_name) >= NAMEDATALEN)
		port->database_name[NAMEDATALEN - 1] = '\0';
	if (strlen(port->user_name) >= NAMEDATALEN)
		port->user_name[NAMEDATALEN - 1] = '\0';

	if (am_walsender)
		MyBackendType = B_WAL_SENDER;
	else
		MyBackendType = B_BACKEND;

	/*
	 * Normal walsender backends, e.g. for streaming replication, are not
	 * connected to a particular database. But walsenders used for logical
	 * replication need to connect to a specific database. We allow streaming
	 * replication commands to be issued even if connected to a database as it
	 * can make sense to first make a basebackup and then stream changes
	 * starting from that.
	 *
	 * 普通 walsender 后端（例如用于流复制的后端）不会连接到特定数据库。但用于
	 * 逻辑复制的 walsender 需要连接到特定数据库。即使连接到数据库，我们也允许发出
	 * 流复制命令，因为先进行基础备份再从该位置开始流式传输变更可能是合理的。
	 */
	if (am_walsender && !am_db_walsender)
		port->database_name[0] = '\0';

	/*
	 * Done filling the Port structure
	 *
	 * Port 结构填充完毕。
	 */
	MemoryContextSwitchTo(oldcontext);

	return STATUS_OK;
}

/*
 * The client has sent a cancel request packet, not a normal
 * start-a-new-connection packet.  Perform the necessary processing.  Nothing
 * is sent back to the client.
 *
 * 客户端发送的是取消请求包，而不是普通的新建连接启动包。执行必要处理。不向客户端
 * 返回任何内容。
 */
static void
ProcessCancelRequestPacket(Port *port, void *pkt, int pktlen)
{
	CancelRequestPacket *canc;
	int			len;

	if (pktlen < offsetof(CancelRequestPacket, cancelAuthCode))
	{
		ereport(COMMERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("invalid length of cancel request packet")));
		return;
	}
	len = pktlen - offsetof(CancelRequestPacket, cancelAuthCode);
	if (len == 0 || len > 256)
	{
		ereport(COMMERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("invalid length of cancel key in cancel request packet")));
		return;
	}

	canc = (CancelRequestPacket *) pkt;
	SendCancelRequest(pg_ntoh32(canc->backendPID), canc->cancelAuthCode, len);
}

/*
 * Send a NegotiateProtocolVersion to the client.  This lets the client know
 * that they have either requested a newer minor protocol version than we are
 * able to speak, or at least one protocol option that we don't understand, or
 * possibly both. FrontendProtocol has already been set to the version
 * requested by the client or the highest version we know how to speak,
 * whichever is older. If the highest version that we know how to speak is too
 * old for the client, it can abandon the connection.
 *
 * 向客户端发送 NegotiateProtocolVersion。这会让客户端知道，它请求的次协议版本比
 * 我们能使用的版本更新，或者它至少请求了一个我们不理解的协议选项，或者两者都有。
 * FrontendProtocol 已经被设置为客户端请求的版本，或我们知道如何使用的最高版本，
 * 取二者中较旧的一个。如果我们知道如何使用的最高版本对客户端而言太旧，客户端可以
 * 放弃连接。
 *
 * We also include in the response a list of protocol options we didn't
 * understand.  This allows clients to include optional parameters that might
 * be present either in newer protocol versions or third-party protocol
 * extensions without fear of having to reconnect if those options are not
 * understood, while at the same time making certain that the client is aware
 * of which options were actually accepted.
 *
 * 我们还会在响应中包含一组我们不理解的协议选项。这允许客户端包含可能存在于较新
 * 协议版本或第三方协议扩展中的可选参数，而不必担心这些选项不被理解时还要重新连接；
 * 同时也能确保客户端知道实际接受了哪些选项。
 */
static void
SendNegotiateProtocolVersion(List *unrecognized_protocol_options)
{
	StringInfoData buf;
	ListCell   *lc;

	pq_beginmessage(&buf, PqMsg_NegotiateProtocolVersion);
	pq_sendint32(&buf, FrontendProtocol);
	pq_sendint32(&buf, list_length(unrecognized_protocol_options));
	foreach(lc, unrecognized_protocol_options)
		pq_sendstring(&buf, lfirst(lc));
	pq_endmessage(&buf);

	/*
	 * no need to flush, some other message will follow
	 *
	 * 无需刷新，后面还会有其他消息。
	 */
}


/*
 * SIGTERM while processing startup packet.
 *
 * 处理启动包期间收到 SIGTERM。
 *
 * Running proc_exit() from a signal handler would be quite unsafe.
 * However, since we have not yet touched shared memory, we can just
 * pull the plug and exit without running any atexit handlers.
 *
 * 从信号处理器中运行 proc_exit() 会很不安全。不过，因为我们还没有接触共享内存，
 * 可以直接断开并退出，而不运行任何 atexit 处理器。
 *
 * One might be tempted to try to send a message, or log one, indicating
 * why we are disconnecting.  However, that would be quite unsafe in itself.
 * Also, it seems undesirable to provide clues about the database's state
 * to a client that has not yet completed authentication, or even sent us
 * a startup packet.
 *
 * 人们可能会想尝试发送一条消息或记录一条日志，说明我们为什么断开连接。然而，这本身
 * 就相当不安全。此外，向尚未完成认证、甚至尚未发送启动包的客户端提供数据库状态线索
 * 似乎也不可取。
 */
static void
process_startup_packet_die(SIGNAL_ARGS)
{
	_exit(1);
}

/*
 * Timeout while processing startup packet.
 * As for process_startup_packet_die(), we exit via _exit(1).
 *
 * 处理启动包期间发生超时。
 * 与 process_startup_packet_die() 一样，我们通过 _exit(1) 退出。
 */
static void
StartupPacketTimeoutHandler(void)
{
	_exit(1);
}

/*
 * Helper for the log_connections GUC check hook.
 *
 * log_connections GUC 检查钩子的辅助函数。
 *
 * `elemlist` is a listified version of the string input passed to the
 * log_connections GUC check hook, check_log_connections().
 * check_log_connections() is responsible for cleaning up `elemlist`.
 *
 * `elemlist` 是传递给 log_connections GUC 检查钩子 check_log_connections() 的
 * 字符串输入的列表化版本。check_log_connections() 负责清理 `elemlist`。
 *
 * validate_log_connections_options() returns false if an error was
 * encountered and the GUC input could not be validated and true otherwise.
 *
 * 如果遇到错误且无法验证 GUC 输入，validate_log_connections_options() 返回 false；
 * 否则返回 true。
 *
 * `flags` returns the flags that should be stored in the log_connections GUC
 * by its assign hook.
 *
 * `flags` 返回应由赋值钩子存储到 log_connections GUC 中的标志。
 */
static bool
validate_log_connections_options(List *elemlist, uint32 *flags)
{
	ListCell   *l;
	char	   *item;

	/*
	 * For backwards compatibility, we accept these tokens by themselves.
	 *
	 * 为了向后兼容，我们接受这些单独出现的标记。
	 *
	 * Prior to PostgreSQL 18, log_connections was a boolean GUC that accepted
	 * any unambiguous substring of 'true', 'false', 'yes', 'no', 'on', and
	 * 'off'. Since log_connections became a list of strings in 18, we only
	 * accept complete option strings.
	 *
	 * 在 PostgreSQL 18 之前，log_connections 是布尔 GUC，接受 'true'、'false'、
	 * 'yes'、'no'、'on' 和 'off' 的任何无歧义子串。由于 log_connections 在 18 中
	 * 变成了字符串列表，我们只接受完整的选项字符串。
	 */
	static const struct config_enum_entry compat_options[] = {
		{"off", 0},
		{"false", 0},
		{"no", 0},
		{"0", 0},
		{"on", LOG_CONNECTION_ON},
		{"true", LOG_CONNECTION_ON},
		{"yes", LOG_CONNECTION_ON},
		{"1", LOG_CONNECTION_ON},
	};

	*flags = 0;

	/*
	 * If an empty string was passed, we're done
	 *
	 * 如果传入的是空字符串，就完成了。
	 */
	if (list_length(elemlist) == 0)
		return true;

	/*
	 * Now check for the backwards compatibility options. They must always be
	 * specified on their own, so we error out if the first option is a
	 * backwards compatibility option and other options are also specified.
	 *
	 * 现在检查向后兼容选项。它们必须始终单独指定，因此如果第一个选项是向后兼容选项，
	 * 而且还指定了其他选项，我们就报错。
	 */
	item = linitial(elemlist);

	for (size_t i = 0; i < lengthof(compat_options); i++)
	{
		struct config_enum_entry option = compat_options[i];

		if (pg_strcasecmp(item, option.name) != 0)
			continue;

		if (list_length(elemlist) > 1)
		{
			GUC_check_errdetail("Cannot specify log_connections option \"%s\" in a list with other options.",
								item);
			return false;
		}

		*flags = option.val;
		return true;
	}

	/*
	 * Now check the aspect options. The empty string was already handled
	 *
	 * 现在检查方面选项。空字符串已经处理过了。
	 */
	foreach(l, elemlist)
	{
		static const struct config_enum_entry options[] = {
			{"receipt", LOG_CONNECTION_RECEIPT},
			{"authentication", LOG_CONNECTION_AUTHENTICATION},
			{"authorization", LOG_CONNECTION_AUTHORIZATION},
			{"setup_durations", LOG_CONNECTION_SETUP_DURATIONS},
			{"all", LOG_CONNECTION_ALL},
		};

		item = lfirst(l);
		for (size_t i = 0; i < lengthof(options); i++)
		{
			struct config_enum_entry option = options[i];

			if (pg_strcasecmp(item, option.name) == 0)
			{
				*flags |= option.val;
				goto next;
			}
		}

		GUC_check_errdetail("Invalid option \"%s\".", item);
		return false;

next:	;
	}

	return true;
}


/*
 * GUC check hook for log_connections.
 *
 * Validate the comma-separated option list, translate it into bit flags, and
 * stash those flags in `extra` for the assign hook.
 *
 * log_connections 的 GUC 检查钩子。
 *
 * 验证逗号分隔的选项列表，将其转换为位标志，并把这些标志保存到 `extra` 中供赋值
 * 钩子使用。
 */
bool
check_log_connections(char **newval, void **extra, GucSource source)
{
	uint32		flags;
	char	   *rawstring;
	List	   *elemlist;
	bool		success;

	/*
	 * Need a modifiable copy of string
	 *
	 * 需要一个可修改的字符串副本。
	 */
	rawstring = pstrdup(*newval);

	if (!SplitIdentifierString(rawstring, ',', &elemlist))
	{
		GUC_check_errdetail("Invalid list syntax in parameter \"%s\".", "log_connections");
		pfree(rawstring);
		list_free(elemlist);
		return false;
	}

	/*
	 * Validation logic is all in the helper
	 *
	 * 验证逻辑全部位于辅助函数中。
	 */
	success = validate_log_connections_options(elemlist, &flags);

	/*
	 * Time for cleanup
	 *
	 * 开始清理。
	 */
	pfree(rawstring);
	list_free(elemlist);

	if (!success)
		return false;

	/*
	 * We succeeded, so allocate `extra` and save the flags there for use by
	 * assign_log_connections().
	 *
	 * 我们成功了，因此分配 `extra`，并将标志保存在其中，供 assign_log_connections()
	 * 使用。
	 */
	*extra = guc_malloc(LOG, sizeof(int));
	if (!*extra)
		return false;
	*((int *) *extra) = flags;

	return true;
}

/*
 * GUC assign hook for log_connections.
 *
 * Install the already-validated flags produced by check_log_connections().
 *
 * log_connections 的 GUC 赋值钩子。
 *
 * 安装 check_log_connections() 生成的、已经验证过的标志。
 */
void
assign_log_connections(const char *newval, void *extra)
{
	log_connections = *((int *) extra);
}

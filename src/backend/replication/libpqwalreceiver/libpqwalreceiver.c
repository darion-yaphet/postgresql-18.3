/*-------------------------------------------------------------------------
 *
 * libpqwalreceiver.c
 *
 * This file contains the libpq-specific parts of walreceiver. It's
 * loaded as a dynamic module to avoid linking the main server binary with
 * libpq.
 *
 * 本文件是 walreceiver 中依赖 libpq 的部分。它作为动态模块加载，以免主服务器二进制链接 libpq。
 *
 * Apart from walreceiver, the libpq-specific routines are now being used by
 * logical replication workers and slot synchronization.
 *
 * 除 walreceiver 外，逻辑复制工作进程和槽同步现在也使用这些 libpq 例程。
 *
 * Portions Copyright (c) 2010-2025, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/replication/libpqwalreceiver/libpqwalreceiver.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <sys/time.h>

#include "common/connect.h"
#include "funcapi.h"
#include "libpq-fe.h"
#include "libpq/libpq-be-fe-helpers.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "pqexpbuffer.h"
#include "replication/walreceiver.h"
#include "storage/latch.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/pg_lsn.h"
#include "utils/tuplestore.h"

PG_MODULE_MAGIC_EXT(
					.name = "libpqwalreceiver",
					.version = PG_VERSION
);

struct WalReceiverConn
{
	/* Current connection to the primary, if any
	 *
	 * 当前与主库的连接；没有则为空。
	 */
	PGconn	   *streamConn;
	/* Used to remember if the connection is logical or physical
	 *
	 * 记住这条连接是逻辑复制还是物理复制。
	 */
	bool		logical;
	/* Buffer for currently read records
	 *
	 * 当前读到的记录缓冲区。
	 */
	char	   *recvBuf;
};

/* Prototypes for interface functions
 *
 * 接口函数的原型。
 */
static WalReceiverConn *libpqrcv_connect(const char *conninfo,
										 bool replication, bool logical,
										 bool must_use_password,
										 const char *appname, char **err);
static void libpqrcv_check_conninfo(const char *conninfo,
									bool must_use_password);
static char *libpqrcv_get_conninfo(WalReceiverConn *conn);
static void libpqrcv_get_senderinfo(WalReceiverConn *conn,
									char **sender_host, int *sender_port);
static char *libpqrcv_identify_system(WalReceiverConn *conn,
									  TimeLineID *primary_tli);
static char *libpqrcv_get_dbname_from_conninfo(const char *connInfo);
static char *libpqrcv_get_option_from_conninfo(const char *connInfo,
											   const char *keyword);
static int	libpqrcv_server_version(WalReceiverConn *conn);
static void libpqrcv_readtimelinehistoryfile(WalReceiverConn *conn,
											 TimeLineID tli, char **filename,
											 char **content, int *len);
static bool libpqrcv_startstreaming(WalReceiverConn *conn,
									const WalRcvStreamOptions *options);
static void libpqrcv_endstreaming(WalReceiverConn *conn,
								  TimeLineID *next_tli);
static int	libpqrcv_receive(WalReceiverConn *conn, char **buffer,
							 pgsocket *wait_fd);
static void libpqrcv_send(WalReceiverConn *conn, const char *buffer,
						  int nbytes);
static char *libpqrcv_create_slot(WalReceiverConn *conn,
								  const char *slotname,
								  bool temporary,
								  bool two_phase,
								  bool failover,
								  CRSSnapshotAction snapshot_action,
								  XLogRecPtr *lsn);
static void libpqrcv_alter_slot(WalReceiverConn *conn, const char *slotname,
								const bool *failover, const bool *two_phase);
static pid_t libpqrcv_get_backend_pid(WalReceiverConn *conn);
static WalRcvExecResult *libpqrcv_exec(WalReceiverConn *conn,
									   const char *query,
									   const int nRetTypes,
									   const Oid *retTypes);
static void libpqrcv_disconnect(WalReceiverConn *conn);

static WalReceiverFunctionsType PQWalReceiverFunctions = {
	.walrcv_connect = libpqrcv_connect,
	.walrcv_check_conninfo = libpqrcv_check_conninfo,
	.walrcv_get_conninfo = libpqrcv_get_conninfo,
	.walrcv_get_senderinfo = libpqrcv_get_senderinfo,
	.walrcv_identify_system = libpqrcv_identify_system,
	.walrcv_server_version = libpqrcv_server_version,
	.walrcv_readtimelinehistoryfile = libpqrcv_readtimelinehistoryfile,
	.walrcv_startstreaming = libpqrcv_startstreaming,
	.walrcv_endstreaming = libpqrcv_endstreaming,
	.walrcv_receive = libpqrcv_receive,
	.walrcv_send = libpqrcv_send,
	.walrcv_create_slot = libpqrcv_create_slot,
	.walrcv_alter_slot = libpqrcv_alter_slot,
	.walrcv_get_dbname_from_conninfo = libpqrcv_get_dbname_from_conninfo,
	.walrcv_get_backend_pid = libpqrcv_get_backend_pid,
	.walrcv_exec = libpqrcv_exec,
	.walrcv_disconnect = libpqrcv_disconnect
};

/* Prototypes for private functions
 *
 * 私有函数的原型。
 */
static char *stringlist_to_identifierstr(PGconn *conn, List *strings);

/*
 * 核心流程：_PG_init 注册回调；libpqrcv_connect 连上主库，libpqrcv_startstreaming 进入复制，libpqrcv_receive 取 WAL。
 */

/*
 * Module initialization function
 *
 * 模块初始化函数。
 */
void
_PG_init(void)
{
	if (WalReceiverFunctions != NULL)
		elog(ERROR, "libpqwalreceiver already loaded");
	WalReceiverFunctions = &PQWalReceiverFunctions;
}

/*
 * Establish the connection to the primary server.
 *
 * 建立到主库的连接。
 *
 * This function can be used for both replication and regular connections.
 * If it is a replication connection, it could be either logical or physical
 * based on input argument 'logical'.
 *
 * 本函数既可用于复制连接，也可用于普通连接。若是复制连接，由参数 logical 决定是逻辑还是物理。
 *
 * If an error occurs, this function will normally return NULL and set *err
 * to a palloc'ed error message. However, if must_use_password is true and
 * the connection fails to use the password, this function will ereport(ERROR).
 * We do this because in that case the error includes a detail and a hint for
 * consistency with other parts of the system, and it's not worth adding the
 * machinery to pass all of those back to the caller just to cover this one
 * case.
 *
 * 出错时通常返回 NULL，并把 err 设为 palloc 出来的错误信息。
 * 但若 must_use_password 为真且连接没有使用密码，则 ereport(ERROR)。
 * 这种情况的错误带有 detail 和 hint，与系统其他部分保持一致；不值得为这一种情况把它们全部传回调用者。
 */
static WalReceiverConn *
libpqrcv_connect(const char *conninfo, bool replication, bool logical,
				 bool must_use_password, const char *appname, char **err)
{
	WalReceiverConn *conn;
	const char *keys[6];
	const char *vals[6];
	int			i = 0;
	char	   *options_val = NULL;

	/*
	 * Re-validate connection string. The validation already happened at DDL
	 * time, but the subscription owner may have changed. If we don't recheck
	 * with the correct must_use_password, it's possible that the connection
	 * will obtain the password from a different source, such as PGPASSFILE or
	 * PGPASSWORD.
	 *
	 * 重新校验连接串。DDL 时已经校验过，但订阅所有者可能已变。
	 * 若不按正确的 must_use_password 再查一次，连接可能从 PGPASSFILE 或 PGPASSWORD 等其他来源取得密码。
	 */
	libpqrcv_check_conninfo(conninfo, must_use_password);

	/*
	 * We use the expand_dbname parameter to process the connection string (or
	 * URI), and pass some extra options.
	 *
	 * 用 expand_dbname 参数处理连接串或 URI，并传入一些额外选项。
	 */
	keys[i] = "dbname";
	vals[i] = conninfo;

	/* We can not have logical without replication
	 *
	 * 没有复制连接就不能做逻辑复制。
	 */
	Assert(replication || !logical);

	if (replication)
	{
		keys[++i] = "replication";
		vals[i] = logical ? "database" : "true";

		if (logical)
		{
			char	   *opt = NULL;

			/* Tell the publisher to translate to our encoding
			 *
			 * 让发布端转换到我们的编码。
			 */
			keys[++i] = "client_encoding";
			vals[i] = GetDatabaseEncodingName();

			/*
			 * Force assorted GUC parameters to settings that ensure that the
			 * publisher will output data values in a form that is unambiguous
			 * to the subscriber.  (We don't want to modify the subscriber's
			 * GUC settings, since that might surprise user-defined code
			 * running in the subscriber, such as triggers.)  This should
			 * match what pg_dump does.
			 *
			 * 强制若干 GUC，使发布端输出的数据值对订阅端没有歧义。
			 * 不去改订阅端的 GUC，以免惊动订阅端上的用户代码，例如触发器。这里应与 pg_dump 一致。
			 */
			opt = libpqrcv_get_option_from_conninfo(conninfo, "options");
			options_val = psprintf("%s -c datestyle=ISO -c intervalstyle=postgres -c extra_float_digits=3",
								   (opt == NULL) ? "" : opt);
			keys[++i] = "options";
			vals[i] = options_val;
			if (opt != NULL)
				pfree(opt);
		}
		else
		{
			/*
			 * The database name is ignored by the server in replication mode,
			 * but specify "replication" for .pgpass lookup.
			 *
			 * 复制模式下服务器会忽略数据库名，但为了查找 .pgpass，这里指定为 replication。
			 */
			keys[++i] = "dbname";
			vals[i] = "replication";
		}
	}

	keys[++i] = "fallback_application_name";
	vals[i] = appname;

	keys[++i] = NULL;
	vals[i] = NULL;

	Assert(i < lengthof(keys));

	conn = palloc0(sizeof(WalReceiverConn));
	conn->streamConn =
		libpqsrv_connect_params(keys, vals,
								 /* expand_dbname = */ true,
								WAIT_EVENT_LIBPQWALRECEIVER_CONNECT);

	if (options_val != NULL)
		pfree(options_val);

	if (PQstatus(conn->streamConn) != CONNECTION_OK)
		goto bad_connection_errmsg;

	if (must_use_password && !PQconnectionUsedPassword(conn->streamConn))
	{
		libpqsrv_disconnect(conn->streamConn);
		pfree(conn);

		ereport(ERROR,
				(errcode(ERRCODE_S_R_E_PROHIBITED_SQL_STATEMENT_ATTEMPTED),
				 errmsg("password is required"),
				 errdetail("Non-superuser cannot connect if the server does not request a password."),
				 errhint("Target server's authentication method must be changed, or set password_required=false in the subscription parameters.")));
	}

	/*
	 * Set always-secure search path for the cases where the connection is
	 * used to run SQL queries, so malicious users can't get control.
	 *
	 * 连接用来跑 SQL 时，设置始终安全的 search_path，避免恶意用户取得控制权。
	 */
	if (!replication || logical)
	{
		PGresult   *res;

		res = libpqsrv_exec(conn->streamConn,
							ALWAYS_SECURE_SEARCH_PATH_SQL,
							WAIT_EVENT_LIBPQWALRECEIVER_RECEIVE);
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
		{
			PQclear(res);
			*err = psprintf(_("could not clear search path: %s"),
							pchomp(PQerrorMessage(conn->streamConn)));
			goto bad_connection;
		}
		PQclear(res);
	}

	conn->logical = logical;

	return conn;

	/* error path, using libpq's error message
	 *
	 * 错误路径：使用 libpq 的错误信息。
	 */
bad_connection_errmsg:
	*err = pchomp(PQerrorMessage(conn->streamConn));

	/* error path, error already set
	 *
	 * 错误路径：错误信息已经设好。
	 */
bad_connection:
	libpqsrv_disconnect(conn->streamConn);
	pfree(conn);
	return NULL;
}

/*
 * Validate connection info string.
 *
 * 校验连接信息字符串。
 *
 * If the connection string can't be parsed, this function will raise
 * an error. If must_use_password is true, the function raises an error
 * if no password is provided in the connection string. In any other case
 * it successfully completes.
 *
 * 连接串无法解析时本函数报错。must_use_password 为真且连接串里没有密码时也报错。其他情况成功返回。
 */
static void
libpqrcv_check_conninfo(const char *conninfo, bool must_use_password)
{
	PQconninfoOption *opts = NULL;
	PQconninfoOption *opt;
	char	   *err = NULL;

	opts = PQconninfoParse(conninfo, &err);
	if (opts == NULL)
	{
		/* The error string is malloc'd, so we must free it explicitly
		 *
		 * 错误字符串由 malloc 分配，必须显式释放。
		 */
		char	   *errcopy = err ? pstrdup(err) : "out of memory";

		PQfreemem(err);
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("invalid connection string syntax: %s", errcopy)));
	}

	if (must_use_password)
	{
		bool		uses_password = false;

		for (opt = opts; opt->keyword != NULL; ++opt)
		{
			/* Ignore connection options that are not present.
			 *
			 * 忽略没有出现的连接选项。
			 */
			if (opt->val == NULL)
				continue;

			if (strcmp(opt->keyword, "password") == 0 && opt->val[0] != '\0')
			{
				uses_password = true;
				break;
			}
		}

		if (!uses_password)
		{
			/* malloc'd, so we must free it explicitly
			 *
			 * 由 malloc 分配，必须显式释放。
			 */
			PQconninfoFree(opts);

			ereport(ERROR,
					(errcode(ERRCODE_S_R_E_PROHIBITED_SQL_STATEMENT_ATTEMPTED),
					 errmsg("password is required"),
					 errdetail("Non-superusers must provide a password in the connection string.")));
		}
	}

	PQconninfoFree(opts);
}

/*
 * Return a user-displayable conninfo string.  Any security-sensitive fields
 * are obfuscated.
 *
 * 返回可展示给用户的 conninfo 字符串。敏感字段会被遮盖。
 */
static char *
libpqrcv_get_conninfo(WalReceiverConn *conn)
{
	PQconninfoOption *conn_opts;
	PQconninfoOption *conn_opt;
	PQExpBufferData buf;
	char	   *retval;

	Assert(conn->streamConn != NULL);

	initPQExpBuffer(&buf);
	conn_opts = PQconninfo(conn->streamConn);

	if (conn_opts == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("could not parse connection string: %s",
						_("out of memory"))));

	/* build a clean connection string from pieces
	 *
	 * 用各个片段拼出干净的连接串。
	 */
	for (conn_opt = conn_opts; conn_opt->keyword != NULL; conn_opt++)
	{
		bool		obfuscate;

		/* Skip debug and empty options
		 *
		 * 跳过调试选项和空选项。
		 */
		if (strchr(conn_opt->dispchar, 'D') ||
			conn_opt->val == NULL ||
			conn_opt->val[0] == '\0')
			continue;

		/* Obfuscate security-sensitive options
		 *
		 * 遮盖敏感选项。
		 */
		obfuscate = strchr(conn_opt->dispchar, '*') != NULL;

		appendPQExpBuffer(&buf, "%s%s=%s",
						  buf.len == 0 ? "" : " ",
						  conn_opt->keyword,
						  obfuscate ? "********" : conn_opt->val);
	}

	PQconninfoFree(conn_opts);

	retval = PQExpBufferDataBroken(buf) ? NULL : pstrdup(buf.data);
	termPQExpBuffer(&buf);
	return retval;
}

/*
 * Provides information of sender this WAL receiver is connected to.
 *
 * 提供本 WAL 接收进程所连接发送端的信息。
 */
static void
libpqrcv_get_senderinfo(WalReceiverConn *conn, char **sender_host,
						int *sender_port)
{
	char	   *ret = NULL;

	*sender_host = NULL;
	*sender_port = 0;

	Assert(conn->streamConn != NULL);

	ret = PQhost(conn->streamConn);
	if (ret && strlen(ret) != 0)
		*sender_host = pstrdup(ret);

	ret = PQport(conn->streamConn);
	if (ret && strlen(ret) != 0)
		*sender_port = atoi(ret);
}

/*
 * Check that primary's system identifier matches ours, and fetch the current
 * timeline ID of the primary.
 *
 * 检查主库的系统标识与本机一致，并取主库当前时间线 ID。
 */
static char *
libpqrcv_identify_system(WalReceiverConn *conn, TimeLineID *primary_tli)
{
	PGresult   *res;
	char	   *primary_sysid;

	/*
	 * Get the system identifier and timeline ID as a DataRow message from the
	 * primary server.
	 *
	 * 从主库以 DataRow 消息取得系统标识和时间线 ID。
	 */
	res = libpqsrv_exec(conn->streamConn,
						"IDENTIFY_SYSTEM",
						WAIT_EVENT_LIBPQWALRECEIVER_RECEIVE);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		PQclear(res);
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("could not receive database system identifier and timeline ID from "
						"the primary server: %s",
						pchomp(PQerrorMessage(conn->streamConn)))));
	}

	/*
	 * IDENTIFY_SYSTEM returns 3 columns in 9.3 and earlier, and 4 columns in
	 * 9.4 and onwards.
	 *
	 * IDENTIFY_SYSTEM 在 9.3 及更早返回 3 列，从 9.4 起返回 4 列。
	 */
	if (PQnfields(res) < 3 || PQntuples(res) != 1)
	{
		int			ntuples = PQntuples(res);
		int			nfields = PQnfields(res);

		PQclear(res);
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("invalid response from primary server"),
				 errdetail("Could not identify system: got %d rows and %d fields, expected %d rows and %d or more fields.",
						   ntuples, nfields, 1, 3)));
	}
	primary_sysid = pstrdup(PQgetvalue(res, 0, 0));
	*primary_tli = pg_strtoint32(PQgetvalue(res, 0, 1));
	PQclear(res);

	return primary_sysid;
}

/*
 * Thin wrapper around libpq to obtain server version.
 *
 * 对 libpq 取服务器版本的薄包装。
 */
static int
libpqrcv_server_version(WalReceiverConn *conn)
{
	return PQserverVersion(conn->streamConn);
}

/*
 * Get database name from the primary server's conninfo.
 *
 * 从主库的 conninfo 中取数据库名。
 *
 * If dbname is not found in connInfo, return NULL value.
 *
 * 若 connInfo 中没有 dbname，则返回 NULL。
 */
static char *
libpqrcv_get_dbname_from_conninfo(const char *connInfo)
{
	return libpqrcv_get_option_from_conninfo(connInfo, "dbname");
}

/*
 * Get the value of the option with the given keyword from the primary
 * server's conninfo.
 *
 * 从主库的 conninfo 中按关键字取选项值。
 *
 * If the option is not found in connInfo, return NULL value.
 *
 * 若 connInfo 中没有该选项，则返回 NULL。
 */
static char *
libpqrcv_get_option_from_conninfo(const char *connInfo, const char *keyword)
{
	PQconninfoOption *opts;
	char	   *option = NULL;
	char	   *err = NULL;

	opts = PQconninfoParse(connInfo, &err);
	if (opts == NULL)
	{
		/* The error string is malloc'd, so we must free it explicitly
		 *
		 * 错误字符串由 malloc 分配，必须显式释放。
		 */
		char	   *errcopy = err ? pstrdup(err) : "out of memory";

		PQfreemem(err);
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("invalid connection string syntax: %s", errcopy)));
	}

	for (PQconninfoOption *opt = opts; opt->keyword != NULL; ++opt)
	{
		/*
		 * If the same option appears multiple times, then the last one will
		 * be returned
		 *
		 * 同一选项出现多次时，返回最后一次的值。
		 */
		if (strcmp(opt->keyword, keyword) == 0 && opt->val &&
			*opt->val)
		{
			if (option)
				pfree(option);

			option = pstrdup(opt->val);
		}
	}

	PQconninfoFree(opts);
	return option;
}

/*
 * Start streaming WAL data from given streaming options.
 *
 * 按给定的流式选项开始接收 WAL。
 *
 * Returns true if we switched successfully to copy-both mode. False
 * means the server received the command and executed it successfully, but
 * didn't switch to copy-mode.  That means that there was no WAL on the
 * requested timeline and starting point, because the server switched to
 * another timeline at or before the requested starting point. On failure,
 * throws an ERROR.
 *
 * 成功切到 copy-both 模式则返回 true。返回 false 表示服务器收到并成功执行了命令，但没有进入复制模式。
 * 这表示所请求的时间线和起点上没有 WAL，因为服务器在该起点或更早处切换了时间线。失败时抛出 ERROR。
 */
static bool
libpqrcv_startstreaming(WalReceiverConn *conn,
						const WalRcvStreamOptions *options)
{
	StringInfoData cmd;
	PGresult   *res;

	Assert(options->logical == conn->logical);
	Assert(options->slotname || !options->logical);

	initStringInfo(&cmd);

	/* Build the command.
	 *
	 * 组装命令。
	 */
	appendStringInfoString(&cmd, "START_REPLICATION");
	if (options->slotname != NULL)
		appendStringInfo(&cmd, " SLOT \"%s\"",
						 options->slotname);

	if (options->logical)
		appendStringInfoString(&cmd, " LOGICAL");

	appendStringInfo(&cmd, " %X/%X", LSN_FORMAT_ARGS(options->startpoint));

	/*
	 * Additional options are different depending on if we are doing logical
	 * or physical replication.
	 *
	 * 附加选项因逻辑复制或物理复制而不同。
	 */
	if (options->logical)
	{
		char	   *pubnames_str;
		List	   *pubnames;
		char	   *pubnames_literal;

		appendStringInfoString(&cmd, " (");

		appendStringInfo(&cmd, "proto_version '%u'",
						 options->proto.logical.proto_version);

		if (options->proto.logical.streaming_str)
			appendStringInfo(&cmd, ", streaming '%s'",
							 options->proto.logical.streaming_str);

		if (options->proto.logical.twophase &&
			PQserverVersion(conn->streamConn) >= 150000)
			appendStringInfoString(&cmd, ", two_phase 'on'");

		if (options->proto.logical.origin &&
			PQserverVersion(conn->streamConn) >= 160000)
			appendStringInfo(&cmd, ", origin '%s'",
							 options->proto.logical.origin);

		pubnames = options->proto.logical.publication_names;
		pubnames_str = stringlist_to_identifierstr(conn->streamConn, pubnames);
		if (!pubnames_str)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),	/* likely guess
														 *
														 * 多半是这个原因。
														 */
					 errmsg("could not start WAL streaming: %s",
							pchomp(PQerrorMessage(conn->streamConn)))));
		pubnames_literal = PQescapeLiteral(conn->streamConn, pubnames_str,
										   strlen(pubnames_str));
		if (!pubnames_literal)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),	/* likely guess
														 *
														 * 多半是这个原因。
														 */
					 errmsg("could not start WAL streaming: %s",
							pchomp(PQerrorMessage(conn->streamConn)))));
		appendStringInfo(&cmd, ", publication_names %s", pubnames_literal);
		PQfreemem(pubnames_literal);
		pfree(pubnames_str);

		if (options->proto.logical.binary &&
			PQserverVersion(conn->streamConn) >= 140000)
			appendStringInfoString(&cmd, ", binary 'true'");

		appendStringInfoChar(&cmd, ')');
	}
	else
		appendStringInfo(&cmd, " TIMELINE %u",
						 options->proto.physical.startpointTLI);

	/* Start streaming.
	 *
	 * 开始流式传输。
	 */
	res = libpqsrv_exec(conn->streamConn,
						cmd.data,
						WAIT_EVENT_LIBPQWALRECEIVER_RECEIVE);
	pfree(cmd.data);

	if (PQresultStatus(res) == PGRES_COMMAND_OK)
	{
		PQclear(res);
		return false;
	}
	else if (PQresultStatus(res) != PGRES_COPY_BOTH)
	{
		PQclear(res);
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("could not start WAL streaming: %s",
						pchomp(PQerrorMessage(conn->streamConn)))));
	}
	PQclear(res);
	return true;
}

/*
 * Stop streaming WAL data. Returns the next timeline's ID in *next_tli, as
 * reported by the server, or 0 if it did not report it.
 *
 * 停止流式接收 WAL。把服务器报告的下一条时间线 ID 写入 next_tli；服务器没报告则为 0。
 */
static void
libpqrcv_endstreaming(WalReceiverConn *conn, TimeLineID *next_tli)
{
	PGresult   *res;

	/*
	 * Send copy-end message.  As in libpqsrv_exec, this could theoretically
	 * block, but the risk seems small.
	 *
	 * 发送 copy-end 消息。和 libpqsrv_exec 一样，理论上可能阻塞，但风险很小。
	 */
	if (PQputCopyEnd(conn->streamConn, NULL) <= 0 ||
		PQflush(conn->streamConn))
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not send end-of-streaming message to primary: %s",
						pchomp(PQerrorMessage(conn->streamConn)))));

	*next_tli = 0;

	/*
	 * After COPY is finished, we should receive a result set indicating the
	 * next timeline's ID, or just CommandComplete if the server was shut
	 * down.
	 *
	 * COPY 结束后，应收到表示下一条时间线 ID 的结果集；若服务器已关闭，则只有 CommandComplete。
	 *
	 * If we had not yet received CopyDone from the backend, PGRES_COPY_OUT is
	 * also possible in case we aborted the copy in mid-stream.
	 *
	 * 若还没从后端收到 CopyDone，中途中止复制时也可能得到 PGRES_COPY_OUT。
	 */
	res = libpqsrv_get_result(conn->streamConn,
							  WAIT_EVENT_LIBPQWALRECEIVER_RECEIVE);
	if (PQresultStatus(res) == PGRES_TUPLES_OK)
	{
		/*
		 * Read the next timeline's ID. The server also sends the timeline's
		 * starting point, but it is ignored.
		 *
		 * 读取下一条时间线的 ID。服务器还会发送该时间线的起点，这里忽略。
		 */
		if (PQnfields(res) < 2 || PQntuples(res) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("unexpected result set after end-of-streaming")));
		*next_tli = pg_strtoint32(PQgetvalue(res, 0, 0));
		PQclear(res);

		/* the result set should be followed by CommandComplete
		 *
		 * 结果集之后应是 CommandComplete。
		 */
		res = libpqsrv_get_result(conn->streamConn,
								  WAIT_EVENT_LIBPQWALRECEIVER_RECEIVE);
	}
	else if (PQresultStatus(res) == PGRES_COPY_OUT)
	{
		PQclear(res);

		/* End the copy
		 *
		 * 结束这次 COPY。
		 */
		if (PQendcopy(conn->streamConn))
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("error while shutting down streaming COPY: %s",
							pchomp(PQerrorMessage(conn->streamConn)))));

		/* CommandComplete should follow
		 *
		 * 后面应是 CommandComplete。
		 */
		res = libpqsrv_get_result(conn->streamConn,
								  WAIT_EVENT_LIBPQWALRECEIVER_RECEIVE);
	}

	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("error reading result of streaming command: %s",
						pchomp(PQerrorMessage(conn->streamConn)))));
	PQclear(res);

	/* Verify that there are no more results
	 *
	 * 确认没有更多结果。
	 */
	res = libpqsrv_get_result(conn->streamConn,
							  WAIT_EVENT_LIBPQWALRECEIVER_RECEIVE);
	if (res != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("unexpected result after CommandComplete: %s",
						pchomp(PQerrorMessage(conn->streamConn)))));
}

/*
 * Fetch the timeline history file for 'tli' from primary.
 *
 * 从主库取时间线 tli 的历史文件。
 */
static void
libpqrcv_readtimelinehistoryfile(WalReceiverConn *conn,
								 TimeLineID tli, char **filename,
								 char **content, int *len)
{
	PGresult   *res;
	char		cmd[64];

	Assert(!conn->logical);

	/*
	 * Request the primary to send over the history file for given timeline.
	 *
	 * 请求主库发送给定时间线的历史文件。
	 */
	snprintf(cmd, sizeof(cmd), "TIMELINE_HISTORY %u", tli);
	res = libpqsrv_exec(conn->streamConn,
						cmd,
						WAIT_EVENT_LIBPQWALRECEIVER_RECEIVE);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		PQclear(res);
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("could not receive timeline history file from "
						"the primary server: %s",
						pchomp(PQerrorMessage(conn->streamConn)))));
	}
	if (PQnfields(res) != 2 || PQntuples(res) != 1)
	{
		int			ntuples = PQntuples(res);
		int			nfields = PQnfields(res);

		PQclear(res);
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("invalid response from primary server"),
				 errdetail("Expected 1 tuple with 2 fields, got %d tuples with %d fields.",
						   ntuples, nfields)));
	}
	*filename = pstrdup(PQgetvalue(res, 0, 0));

	*len = PQgetlength(res, 0, 1);
	*content = palloc(*len);
	memcpy(*content, PQgetvalue(res, 0, 1), *len);
	PQclear(res);
}

/*
 * Disconnect connection to primary, if any.
 *
 * 若仍连着主库，则断开。
 */
static void
libpqrcv_disconnect(WalReceiverConn *conn)
{
	libpqsrv_disconnect(conn->streamConn);
	PQfreemem(conn->recvBuf);
	pfree(conn);
}

/*
 * Receive a message available from XLOG stream.
 *
 * 从 XLOG 流中接收一条已到达的消息。
 *
 * Returns:
 *
 * 返回值：
 *
 *	 If data was received, returns the length of the data. *buffer is set to
 *	 point to a buffer holding the received message. The buffer is only valid
 *	 until the next libpqrcv_* call.
 *
 * 若收到数据，返回数据长度。buffer 指向存放该消息的缓冲区。这块缓冲区只保持到下一次 libpqrcv 调用。
 *
 *	 If no data was available immediately, returns 0, and *wait_fd is set to a
 *	 socket descriptor which can be waited on before trying again.
 *
 * 若眼下没有数据，返回 0，并把 wait_fd 设为可等待的套接字，待就绪后再试。
 *
 *	 -1 if the server ended the COPY.
 *
 * 若服务器结束了 COPY，返回 -1。
 *
 * ereports on error.
 *
 * 出错时 ereport。
 */
static int
libpqrcv_receive(WalReceiverConn *conn, char **buffer,
				 pgsocket *wait_fd)
{
	int			rawlen;

	PQfreemem(conn->recvBuf);
	conn->recvBuf = NULL;

	/* Try to receive a CopyData message
	 *
	 * 尝试接收一条 CopyData 消息。
	 */
	rawlen = PQgetCopyData(conn->streamConn, &conn->recvBuf, 1);
	if (rawlen == 0)
	{
		/* Try consuming some data.
		 *
		 * 尝试消费一些数据。
		 */
		if (PQconsumeInput(conn->streamConn) == 0)
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("could not receive data from WAL stream: %s",
							pchomp(PQerrorMessage(conn->streamConn)))));

		/* Now that we've consumed some input, try again
		 *
		 * 已经消费了一些输入，再试一次。
		 */
		rawlen = PQgetCopyData(conn->streamConn, &conn->recvBuf, 1);
		if (rawlen == 0)
		{
			/* Tell caller to try again when our socket is ready.
			 *
			 * 告诉调用者等套接字就绪后再试。
			 */
			*wait_fd = PQsocket(conn->streamConn);
			return 0;
		}
	}
	if (rawlen == -1)			/* end-of-streaming or error
								 *
								 * 流结束或出错。
								 */
	{
		PGresult   *res;

		res = libpqsrv_get_result(conn->streamConn,
								  WAIT_EVENT_LIBPQWALRECEIVER_RECEIVE);
		if (PQresultStatus(res) == PGRES_COMMAND_OK)
		{
			PQclear(res);

			/* Verify that there are no more results.
			 *
			 * 确认没有更多结果。
			 */
			res = libpqsrv_get_result(conn->streamConn,
									  WAIT_EVENT_LIBPQWALRECEIVER_RECEIVE);
			if (res != NULL)
			{
				PQclear(res);

				/*
				 * If the other side closed the connection orderly (otherwise
				 * we'd seen an error, or PGRES_COPY_IN) don't report an error
				 * here, but let callers deal with it.
				 *
				 * 若对端有序关闭了连接（否则我们会看到错误或 PGRES_COPY_IN），这里不报错，交给调用者处理。
				 */
				if (PQstatus(conn->streamConn) == CONNECTION_BAD)
					return -1;

				ereport(ERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("unexpected result after CommandComplete: %s",
								PQerrorMessage(conn->streamConn))));
			}

			return -1;
		}
		else if (PQresultStatus(res) == PGRES_COPY_IN)
		{
			PQclear(res);
			return -1;
		}
		else
		{
			PQclear(res);
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("could not receive data from WAL stream: %s",
							pchomp(PQerrorMessage(conn->streamConn)))));
		}
	}
	if (rawlen < -1)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("could not receive data from WAL stream: %s",
						pchomp(PQerrorMessage(conn->streamConn)))));

	/* Return received messages to caller
	 *
	 * 把收到的消息返回给调用者。
	 */
	*buffer = conn->recvBuf;
	return rawlen;
}

/*
 * Send a message to XLOG stream.
 *
 * 向 XLOG 流发送一条消息。
 *
 * ereports on error.
 *
 * 出错时 ereport。
 */
static void
libpqrcv_send(WalReceiverConn *conn, const char *buffer, int nbytes)
{
	if (PQputCopyData(conn->streamConn, buffer, nbytes) <= 0 ||
		PQflush(conn->streamConn))
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not send data to WAL stream: %s",
						pchomp(PQerrorMessage(conn->streamConn)))));
}

/*
 * Create new replication slot.
 * Returns the name of the exported snapshot for logical slot or NULL for
 * physical slot.
 *
 * 创建新复制槽。逻辑槽返回导出的快照名，物理槽返回 NULL。
 */
static char *
libpqrcv_create_slot(WalReceiverConn *conn, const char *slotname,
					 bool temporary, bool two_phase, bool failover,
					 CRSSnapshotAction snapshot_action, XLogRecPtr *lsn)
{
	PGresult   *res;
	StringInfoData cmd;
	char	   *snapshot;
	int			use_new_options_syntax;

	use_new_options_syntax = (PQserverVersion(conn->streamConn) >= 150000);

	initStringInfo(&cmd);

	appendStringInfo(&cmd, "CREATE_REPLICATION_SLOT \"%s\"", slotname);

	if (temporary)
		appendStringInfoString(&cmd, " TEMPORARY");

	if (conn->logical)
	{
		appendStringInfoString(&cmd, " LOGICAL pgoutput ");
		if (use_new_options_syntax)
			appendStringInfoChar(&cmd, '(');
		if (two_phase)
		{
			appendStringInfoString(&cmd, "TWO_PHASE");
			if (use_new_options_syntax)
				appendStringInfoString(&cmd, ", ");
			else
				appendStringInfoChar(&cmd, ' ');
		}

		if (failover)
		{
			appendStringInfoString(&cmd, "FAILOVER");
			if (use_new_options_syntax)
				appendStringInfoString(&cmd, ", ");
			else
				appendStringInfoChar(&cmd, ' ');
		}

		if (use_new_options_syntax)
		{
			switch (snapshot_action)
			{
				case CRS_EXPORT_SNAPSHOT:
					appendStringInfoString(&cmd, "SNAPSHOT 'export'");
					break;
				case CRS_NOEXPORT_SNAPSHOT:
					appendStringInfoString(&cmd, "SNAPSHOT 'nothing'");
					break;
				case CRS_USE_SNAPSHOT:
					appendStringInfoString(&cmd, "SNAPSHOT 'use'");
					break;
			}
		}
		else
		{
			switch (snapshot_action)
			{
				case CRS_EXPORT_SNAPSHOT:
					appendStringInfoString(&cmd, "EXPORT_SNAPSHOT");
					break;
				case CRS_NOEXPORT_SNAPSHOT:
					appendStringInfoString(&cmd, "NOEXPORT_SNAPSHOT");
					break;
				case CRS_USE_SNAPSHOT:
					appendStringInfoString(&cmd, "USE_SNAPSHOT");
					break;
			}
		}

		if (use_new_options_syntax)
			appendStringInfoChar(&cmd, ')');
	}
	else
	{
		if (use_new_options_syntax)
			appendStringInfoString(&cmd, " PHYSICAL (RESERVE_WAL)");
		else
			appendStringInfoString(&cmd, " PHYSICAL RESERVE_WAL");
	}

	res = libpqsrv_exec(conn->streamConn,
						cmd.data,
						WAIT_EVENT_LIBPQWALRECEIVER_RECEIVE);
	pfree(cmd.data);

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		PQclear(res);
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("could not create replication slot \"%s\": %s",
						slotname, pchomp(PQerrorMessage(conn->streamConn)))));
	}

	if (lsn)
		*lsn = DatumGetLSN(DirectFunctionCall1Coll(pg_lsn_in, InvalidOid,
												   CStringGetDatum(PQgetvalue(res, 0, 1))));

	if (!PQgetisnull(res, 0, 2))
		snapshot = pstrdup(PQgetvalue(res, 0, 2));
	else
		snapshot = NULL;

	PQclear(res);

	return snapshot;
}

/*
 * Change the definition of the replication slot.
 *
 * 修改复制槽的定义。
 */
static void
libpqrcv_alter_slot(WalReceiverConn *conn, const char *slotname,
					const bool *failover, const bool *two_phase)
{
	StringInfoData cmd;
	PGresult   *res;

	initStringInfo(&cmd);
	appendStringInfo(&cmd, "ALTER_REPLICATION_SLOT %s ( ",
					 quote_identifier(slotname));

	if (failover)
		appendStringInfo(&cmd, "FAILOVER %s",
						 *failover ? "true" : "false");

	if (failover && two_phase)
		appendStringInfoString(&cmd, ", ");

	if (two_phase)
		appendStringInfo(&cmd, "TWO_PHASE %s",
						 *two_phase ? "true" : "false");

	appendStringInfoString(&cmd, " );");

	res = libpqsrv_exec(conn->streamConn, cmd.data,
						WAIT_EVENT_LIBPQWALRECEIVER_RECEIVE);
	pfree(cmd.data);

	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("could not alter replication slot \"%s\": %s",
						slotname, pchomp(PQerrorMessage(conn->streamConn)))));

	PQclear(res);
}

/*
 * Return PID of remote backend process.
 *
 * 返回远端后端进程的 PID。
 */
static pid_t
libpqrcv_get_backend_pid(WalReceiverConn *conn)
{
	return PQbackendPID(conn->streamConn);
}

/*
 * Convert tuple query result to tuplestore.
 *
 * 把元组查询结果转换成 tuplestore。
 */
static void
libpqrcv_processTuples(PGresult *pgres, WalRcvExecResult *walres,
					   const int nRetTypes, const Oid *retTypes)
{
	int			tupn;
	int			coln;
	int			nfields = PQnfields(pgres);
	HeapTuple	tuple;
	AttInMetadata *attinmeta;
	MemoryContext rowcontext;
	MemoryContext oldcontext;

	/* Make sure we got expected number of fields.
	 *
	 * 确认字段数符合预期。
	 */
	if (nfields != nRetTypes)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("invalid query response"),
				 errdetail("Expected %d fields, got %d fields.",
						   nRetTypes, nfields)));

	walres->tuplestore = tuplestore_begin_heap(true, false, work_mem);

	/* Create tuple descriptor corresponding to expected result.
	 *
	 * 按预期结果创建元组描述符。
	 */
	walres->tupledesc = CreateTemplateTupleDesc(nRetTypes);
	for (coln = 0; coln < nRetTypes; coln++)
		TupleDescInitEntry(walres->tupledesc, (AttrNumber) coln + 1,
						   PQfname(pgres, coln), retTypes[coln], -1, 0);
	attinmeta = TupleDescGetAttInMetadata(walres->tupledesc);

	/* No point in doing more here if there were no tuples returned.
	 *
	 * 没有返回元组时，不必再继续。
	 */
	if (PQntuples(pgres) == 0)
		return;

	/* Create temporary context for local allocations.
	 *
	 * 为本地分配创建临时内存上下文。
	 */
	rowcontext = AllocSetContextCreate(CurrentMemoryContext,
									   "libpqrcv query result context",
									   ALLOCSET_DEFAULT_SIZES);

	/* Process returned rows.
	 *
	 * 处理返回的行。
	 */
	for (tupn = 0; tupn < PQntuples(pgres); tupn++)
	{
		char	   *cstrs[MaxTupleAttributeNumber];

		CHECK_FOR_INTERRUPTS();

		/* Do the allocations in temporary context.
		 *
		 * 在临时上下文中做分配。
		 */
		oldcontext = MemoryContextSwitchTo(rowcontext);

		/*
		 * Fill cstrs with null-terminated strings of column values.
		 *
		 * 用以 NUL 结尾的列值字符串填 cstrs。
		 */
		for (coln = 0; coln < nfields; coln++)
		{
			if (PQgetisnull(pgres, tupn, coln))
				cstrs[coln] = NULL;
			else
				cstrs[coln] = PQgetvalue(pgres, tupn, coln);
		}

		/* Convert row to a tuple, and add it to the tuplestore
		 *
		 * 把一行转成元组并加入 tuplestore。
		 */
		tuple = BuildTupleFromCStrings(attinmeta, cstrs);
		tuplestore_puttuple(walres->tuplestore, tuple);

		/* Clean up
		 *
		 * 清理。
		 */
		MemoryContextSwitchTo(oldcontext);
		MemoryContextReset(rowcontext);
	}

	MemoryContextDelete(rowcontext);
}

/*
 * Public interface for sending generic queries (and commands).
 *
 * 发送一般查询和命令的公开接口。
 *
 * This can only be called from process connected to database.
 *
 * 只能由已连接到数据库的进程调用。
 */
static WalRcvExecResult *
libpqrcv_exec(WalReceiverConn *conn, const char *query,
			  const int nRetTypes, const Oid *retTypes)
{
	PGresult   *pgres = NULL;
	WalRcvExecResult *walres = palloc0(sizeof(WalRcvExecResult));
	char	   *diag_sqlstate;

	if (MyDatabaseId == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("the query interface requires a database connection")));

	pgres = libpqsrv_exec(conn->streamConn,
						  query,
						  WAIT_EVENT_LIBPQWALRECEIVER_RECEIVE);

	switch (PQresultStatus(pgres))
	{
		case PGRES_TUPLES_OK:
		case PGRES_SINGLE_TUPLE:
		case PGRES_TUPLES_CHUNK:
			walres->status = WALRCV_OK_TUPLES;
			libpqrcv_processTuples(pgres, walres, nRetTypes, retTypes);
			break;

		case PGRES_COPY_IN:
			walres->status = WALRCV_OK_COPY_IN;
			break;

		case PGRES_COPY_OUT:
			walres->status = WALRCV_OK_COPY_OUT;
			break;

		case PGRES_COPY_BOTH:
			walres->status = WALRCV_OK_COPY_BOTH;
			break;

		case PGRES_COMMAND_OK:
			walres->status = WALRCV_OK_COMMAND;
			break;

			/* Empty query is considered error.
			 *
			 * 空查询视为错误。
			 */
		case PGRES_EMPTY_QUERY:
			walres->status = WALRCV_ERROR;
			walres->err = _("empty query");
			break;

		case PGRES_PIPELINE_SYNC:
		case PGRES_PIPELINE_ABORTED:
			walres->status = WALRCV_ERROR;
			walres->err = _("unexpected pipeline mode");
			break;

		case PGRES_NONFATAL_ERROR:
		case PGRES_FATAL_ERROR:
		case PGRES_BAD_RESPONSE:
			walres->status = WALRCV_ERROR;
			walres->err = pchomp(PQerrorMessage(conn->streamConn));
			diag_sqlstate = PQresultErrorField(pgres, PG_DIAG_SQLSTATE);
			if (diag_sqlstate)
				walres->sqlstate = MAKE_SQLSTATE(diag_sqlstate[0],
												 diag_sqlstate[1],
												 diag_sqlstate[2],
												 diag_sqlstate[3],
												 diag_sqlstate[4]);
			break;
	}

	PQclear(pgres);

	return walres;
}

/*
 * Given a List of strings, return it as single comma separated
 * string, quoting identifiers as needed.
 *
 * 把字符串 List 变成一条逗号分隔的字符串，并按需要给标识符加引号。
 *
 * This is essentially the reverse of SplitIdentifierString.
 *
 * 这基本上是 SplitIdentifierString 的逆操作。
 *
 * The caller should free the result.
 *
 * 调用者应释放返回的结果。
 */
static char *
stringlist_to_identifierstr(PGconn *conn, List *strings)
{
	ListCell   *lc;
	StringInfoData res;
	bool		first = true;

	initStringInfo(&res);

	foreach(lc, strings)
	{
		char	   *val = strVal(lfirst(lc));
		char	   *val_escaped;

		if (first)
			first = false;
		else
			appendStringInfoChar(&res, ',');

		val_escaped = PQescapeIdentifier(conn, val, strlen(val));
		if (!val_escaped)
		{
			free(res.data);
			return NULL;
		}
		appendStringInfoString(&res, val_escaped);
		PQfreemem(val_escaped);
	}

	return res.data;
}

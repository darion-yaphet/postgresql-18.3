/*-------------------------------------------------------------------------
 *
 * connection.c
 *		  Connection management functions for postgres_fdw
 *
 * Portions Copyright (c) 2012-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  contrib/postgres_fdw/connection.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#if HAVE_POLL_H
#include <poll.h>
#endif

#include "access/xact.h"
#include "catalog/pg_user_mapping.h"
#include "commands/defrem.h"
#include "common/base64.h"
#include "funcapi.h"
#include "libpq/libpq-be.h"
#include "libpq/libpq-be-fe-helpers.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postgres_fdw.h"
#include "storage/latch.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/syscache.h"

/*
 * Connection cache hash table entry
 *
 * 连接缓存哈希表条目
 *
 * The lookup key in this hash table is the user mapping OID. We use just one
 * connection per user mapping ID, which ensures that all the scans use the
 * same snapshot during a query.  Using the user mapping OID rather than
 * the foreign server OID + user OID avoids creating multiple connections when
 * the public user mapping applies to all user OIDs.
 *
 * 该哈希表中的查找键是用户映射 OID。我们对每个用户映射 ID 仅使用一个连接，这可确保所有扫描在查询期间使用相同的快照。  使用用户映射 OID 而不是外部服务器 OID + 用户 OID 可以避免在公共用户映射应用于所有用户 OID 时创建多个连接。
 *
 * The "conn" pointer can be NULL if we don't currently have a live connection.
 * When we do have a connection, xact_depth tracks the current depth of
 * transactions and subtransactions open on the remote side.  We need to issue
 * commands at the same nesting depth on the remote as we're executing at
 * ourselves, so that rolling back a subtransaction will kill the right
 * queries and not the wrong ones.
 *
 * 如果我们当前没有实时连接，“conn”指针可以为 NULL。当我们确实建立连接时，xact_depth 会跟踪远程端打开的事务和子事务的当前深度。  我们需要在远程上以与我们自己执行相同的嵌套深度发出命令，以便回滚子事务将杀死正确的查询，而不是错误的查询。
 */
typedef Oid ConnCacheKey;

typedef struct ConnCacheEntry
{
	ConnCacheKey key;			/* hash key (must be first) */
	PGconn	   *conn;			/* connection to foreign server, or NULL */
	/* Remaining fields are invalid when conn is NULL:
	 *
	 * 当 conn 为 NULL 时，其余字段无效：
	 */
	int			xact_depth;		/* 0 = no xact open, 1 = main xact open, 2 =
								 * one level of subxact open, etc */
	bool		have_prep_stmt; /* have we prepared any stmts in this xact? */
	bool		have_error;		/* have any subxacts aborted in this xact? */
	bool		changing_xact_state;	/* xact state change in process */
	bool		parallel_commit;	/* do we commit (sub)xacts in parallel? */
	bool		parallel_abort; /* do we abort (sub)xacts in parallel? */
	bool		invalidated;	/* true if reconnect is pending */
	bool		keep_connections;	/* setting value of keep_connections
									 * server option */
	Oid			serverid;		/* foreign server OID used to get server name */
	uint32		server_hashvalue;	/* hash value of foreign server OID */
	uint32		mapping_hashvalue;	/* hash value of user mapping OID */
	PgFdwConnState state;		/* extra per-connection state */
} ConnCacheEntry;

/*
 * Connection cache (initialized on first use)
 *
 * 连接缓存（首次使用时初始化）
 */
static HTAB *ConnectionHash = NULL;

/* for assigning cursor numbers and prepared statement numbers
 *
 * 用于分配游标编号和准备好的语句编号
 */
static unsigned int cursor_number = 0;
static unsigned int prep_stmt_number = 0;

/* tracks whether any work is needed in callback functions
 *
 * 跟踪回调函数中是否需要任何工作
 */
static bool xact_got_connection = false;

/* custom wait event values, retrieved from shared memory
 *
 * 自定义等待事件值，从共享内存中检索
 */
static uint32 pgfdw_we_cleanup_result = 0;
static uint32 pgfdw_we_connect = 0;
static uint32 pgfdw_we_get_result = 0;

/*
 * Milliseconds to wait to cancel an in-progress query or execute a cleanup
 * query; if it takes longer than 30 seconds to do these, we assume the
 * connection is dead.
 *
 * 等待取消正在进行的查询或执行清理查询的毫秒数；如果执行这些操作花费的时间超过 30 秒，我们就假设连接已断开。
 */
#define CONNECTION_CLEANUP_TIMEOUT	30000

/*
 * Milliseconds to wait before issuing another cancel request.  This covers
 * the race condition where the remote session ignored our cancel request
 * because it arrived while idle.
 *
 * 发出另一个取消请求之前等待的毫秒数。  这涵盖了远程会话忽略我们的取消请求的竞争条件，因为它是在空闲时到达的。
 */
#define RETRY_CANCEL_TIMEOUT	1000

/* Macro for constructing abort command to be sent
 *
 * 用于构造要发送的中止命令的宏
 */
#define CONSTRUCT_ABORT_COMMAND(sql, entry, toplevel) \
	do { \
		if (toplevel) \
			snprintf((sql), sizeof(sql), \
					 "ABORT TRANSACTION"); \
		else \
			snprintf((sql), sizeof(sql), \
					 "ROLLBACK TO SAVEPOINT s%d; RELEASE SAVEPOINT s%d", \
					 (entry)->xact_depth, (entry)->xact_depth); \
	} while(0)

/*
 * Extension version number, for supporting older extension versions' objects
 *
 * 扩展版本号，用于支持旧扩展版本的对象
 */
enum pgfdwVersion
{
	PGFDW_V1_1 = 0,
	PGFDW_V1_2,
};

/*
 * SQL functions
 *
 * SQL函数
 */
PG_FUNCTION_INFO_V1(postgres_fdw_get_connections);
PG_FUNCTION_INFO_V1(postgres_fdw_get_connections_1_2);
PG_FUNCTION_INFO_V1(postgres_fdw_disconnect);
PG_FUNCTION_INFO_V1(postgres_fdw_disconnect_all);

/* prototypes of private functions
 *
 * 私有函数的原型
 */
static void make_new_connection(ConnCacheEntry *entry, UserMapping *user);
static PGconn *connect_pg_server(ForeignServer *server, UserMapping *user);
static void disconnect_pg_server(ConnCacheEntry *entry);
static void check_conn_params(const char **keywords, const char **values, UserMapping *user);
static void configure_remote_session(PGconn *conn);
static void do_sql_command_begin(PGconn *conn, const char *sql);
static void do_sql_command_end(PGconn *conn, const char *sql,
							   bool consume_input);
static void begin_remote_xact(ConnCacheEntry *entry);
static void pgfdw_xact_callback(XactEvent event, void *arg);
static void pgfdw_subxact_callback(SubXactEvent event,
								   SubTransactionId mySubid,
								   SubTransactionId parentSubid,
								   void *arg);
static void pgfdw_inval_callback(Datum arg, int cacheid, uint32 hashvalue);
static void pgfdw_reject_incomplete_xact_state_change(ConnCacheEntry *entry);
static void pgfdw_reset_xact_state(ConnCacheEntry *entry, bool toplevel);
static bool pgfdw_cancel_query(PGconn *conn);
static bool pgfdw_cancel_query_begin(PGconn *conn, TimestampTz endtime);
static bool pgfdw_cancel_query_end(PGconn *conn, TimestampTz endtime,
								   TimestampTz retrycanceltime,
								   bool consume_input);
static bool pgfdw_exec_cleanup_query(PGconn *conn, const char *query,
									 bool ignore_errors);
static bool pgfdw_exec_cleanup_query_begin(PGconn *conn, const char *query);
static bool pgfdw_exec_cleanup_query_end(PGconn *conn, const char *query,
										 TimestampTz endtime,
										 bool consume_input,
										 bool ignore_errors);
static bool pgfdw_get_cleanup_result(PGconn *conn, TimestampTz endtime,
									 TimestampTz retrycanceltime,
									 PGresult **result, bool *timed_out);
static void pgfdw_abort_cleanup(ConnCacheEntry *entry, bool toplevel);
static bool pgfdw_abort_cleanup_begin(ConnCacheEntry *entry, bool toplevel,
									  List **pending_entries,
									  List **cancel_requested);
static void pgfdw_finish_pre_commit_cleanup(List *pending_entries);
static void pgfdw_finish_pre_subcommit_cleanup(List *pending_entries,
											   int curlevel);
static void pgfdw_finish_abort_cleanup(List *pending_entries,
									   List *cancel_requested,
									   bool toplevel);
static void pgfdw_security_check(const char **keywords, const char **values,
								 UserMapping *user, PGconn *conn);
static bool UserMappingPasswordRequired(UserMapping *user);
static bool UseScramPassthrough(ForeignServer *server, UserMapping *user);
static bool disconnect_cached_connections(Oid serverid);
static void postgres_fdw_get_connections_internal(FunctionCallInfo fcinfo,
												  enum pgfdwVersion api_version);
static int	pgfdw_conn_check(PGconn *conn);
static bool pgfdw_conn_checkable(void);
static bool pgfdw_has_required_scram_options(const char **keywords, const char **values);

/*
 * Get a PGconn which can be used to execute queries on the remote PostgreSQL
 * server with the user's authorization.  A new connection is established
 * if we don't already have a suitable one, and a transaction is opened at
 * the right subtransaction nesting depth if we didn't do that already.
 *
 * 获取一个 PGconn，可用于在用户授权的情况下在远程 PostgreSQL 服务器上执行查询。  如果我们还没有合适的连接，则会建立一个新连接；如果我们还没有这样做，则会在正确的子事务嵌套深度打开一个事务。
 *
 * will_prep_stmt must be true if caller intends to create any prepared
 * statements.  Since those don't go away automatically at transaction end
 * (not even on error), we need this flag to cue manual cleanup.
 *
 * 如果调用者打算创建任何准备好的语句，则 will_prep_stmt 必须为 true。  由于这些不会在事务结束时自动消失（甚至不会出现错误），因此我们需要此标志来提示手动清理。
 *
 * If state is not NULL, *state receives the per-connection state associated
 * with the PGconn.
 *
 * 如果 state 不为 NULL，则 *state 接收与 PGconn 关联的每个连接状态。
 */
PGconn *
GetConnection(UserMapping *user, bool will_prep_stmt, PgFdwConnState **state)
{
	bool		found;
	bool		retry = false;
	ConnCacheEntry *entry;
	ConnCacheKey key;
	MemoryContext ccxt = CurrentMemoryContext;

	/* First time through, initialize connection cache hashtable
	 *
	 * 第一次通过，初始化连接缓存哈希表
	 */
	if (ConnectionHash == NULL)
	{
		HASHCTL		ctl;

		if (pgfdw_we_get_result == 0)
			pgfdw_we_get_result =
				WaitEventExtensionNew("PostgresFdwGetResult");

		ctl.keysize = sizeof(ConnCacheKey);
		ctl.entrysize = sizeof(ConnCacheEntry);
		ConnectionHash = hash_create("postgres_fdw connections", 8,
									 &ctl,
									 HASH_ELEM | HASH_BLOBS);

		/*
		 * Register some callback functions that manage connection cleanup.
		 * This should be done just once in each backend.
		 *
		 * 注册一些管理连接清理的回调函数。这应该在每个后端只执行一次。
		 */
		RegisterXactCallback(pgfdw_xact_callback, NULL);
		RegisterSubXactCallback(pgfdw_subxact_callback, NULL);
		CacheRegisterSyscacheCallback(FOREIGNSERVEROID,
									  pgfdw_inval_callback, (Datum) 0);
		CacheRegisterSyscacheCallback(USERMAPPINGOID,
									  pgfdw_inval_callback, (Datum) 0);
	}

	/* Set flag that we did GetConnection during the current transaction
	 *
	 * 设置我们在当前事务期间执行 GetConnection 的标志
	 */
	xact_got_connection = true;

	/* Create hash key for the entry.  Assume no pad bytes in key struct
	 *
	 * 为条目创建哈希键。  假设关键结构中没有填充字节
	 */
	key = user->umid;

	/*
	 * Find or create cached entry for requested connection.
	 *
	 * 查找或创建请求连接的缓存条目。
	 */
	entry = hash_search(ConnectionHash, &key, HASH_ENTER, &found);
	if (!found)
	{
		/*
		 * We need only clear "conn" here; remaining fields will be filled
		 * later when "conn" is set.
		 *
		 * 这里我们只需要明确“conn”即可；其余字段将在设置“conn”后填充。
		 */
		entry->conn = NULL;
	}

	/* Reject further use of connections which failed abort cleanup.
	 *
	 * 拒绝进一步使用中止清理失败的连接。
	 */
	pgfdw_reject_incomplete_xact_state_change(entry);

	/*
	 * If the connection needs to be remade due to invalidation, disconnect as
	 * soon as we're out of all transactions.
	 *
	 * 如果由于失效而需要重新建立连接，请在完成所有事务后立即断开连接。
	 */
	if (entry->conn != NULL && entry->invalidated && entry->xact_depth == 0)
	{
		elog(DEBUG3, "closing connection %p for option changes to take effect",
			 entry->conn);
		disconnect_pg_server(entry);
	}

	/*
	 * If cache entry doesn't have a connection, we have to establish a new
	 * connection.  (If connect_pg_server throws an error, the cache entry
	 * will remain in a valid empty state, ie conn == NULL.)
	 *
	 * 如果缓存条目没有连接，我们必须建立一个新连接。  （如果connect_pg_server抛出错误，缓存条目将保持有效的空状态，即conn == NULL。）
	 */
	if (entry->conn == NULL)
		make_new_connection(entry, user);

	/*
	 * We check the health of the cached connection here when using it.  In
	 * cases where we're out of all transactions, if a broken connection is
	 * detected, we try to reestablish a new connection later.
	 *
	 * 我们在使用时检查缓存连接的健康状况。  在我们停止所有事务的情况下，如果检测到连接断开，我们稍后会尝试重新建立新连接。
	 */
	PG_TRY();
	{
		/* Process a pending asynchronous request if any.
		 *
		 * 处理挂起的异步请求（如果有）。
		 */
		if (entry->state.pendingAreq)
			process_pending_request(entry->state.pendingAreq);
		/* Start a new transaction or subtransaction if needed.
		 *
		 * 如果需要，启动新事务或子事务。
		 */
		begin_remote_xact(entry);
	}
	PG_CATCH();
	{
		MemoryContext ecxt = MemoryContextSwitchTo(ccxt);
		ErrorData  *errdata = CopyErrorData();

		/*
		 * Determine whether to try to reestablish the connection.
		 *
		 * 确定是否尝试重新建立连接。
		 *
		 * After a broken connection is detected in libpq, any error other
		 * than connection failure (e.g., out-of-memory) can be thrown
		 * somewhere between return from libpq and the expected ereport() call
		 * in pgfdw_report_error(). In this case, since PQstatus() indicates
		 * CONNECTION_BAD, checking only PQstatus() causes the false detection
		 * of connection failure. To avoid this, we also verify that the
		 * error's sqlstate is ERRCODE_CONNECTION_FAILURE. Note that also
		 * checking only the sqlstate can cause another false detection
		 * because pgfdw_report_error() may report ERRCODE_CONNECTION_FAILURE
		 * for any libpq-originated error condition.
		 *
		 * 在 libpq 中检测到断开的连接后，除了连接失败（例如内存不足）之外的任何错误都可能在 libpq 返回和 pgfdw_report_error() 中预期的 ereport() 调用之间抛出。在这种情况下，由于 PQstatus() 指示 CONNECTION_BAD，因此仅检查 PQstatus() 会导致连接失败的错误检测。为了避免这种情况，我们还验证错误的 sqlstate 是否为 ERRCODE_CONNECTION_FAILURE。请注意，仅检查 sqlstate 可能会导致另一个错误检测，因为 pgfdw_report_error() 可能会针对任何 libpq 引发的错误情况报告 ERRCODE_CONNECTION_FAILURE。
		 */
		if (errdata->sqlerrcode != ERRCODE_CONNECTION_FAILURE ||
			PQstatus(entry->conn) != CONNECTION_BAD ||
			entry->xact_depth > 0)
		{
			MemoryContextSwitchTo(ecxt);
			PG_RE_THROW();
		}

		/* Clean up the error state
		 *
		 * 清理错误状态
		 */
		FlushErrorState();
		FreeErrorData(errdata);
		errdata = NULL;

		retry = true;
	}
	PG_END_TRY();

	/*
	 * If a broken connection is detected, disconnect it, reestablish a new
	 * connection and retry a new remote transaction. If connection failure is
	 * reported again, we give up getting a connection.
	 *
	 * 如果检测到连接断开，请断开连接，重新建立新连接并重试新的远程事务。如果再次报告连接失败，我们放弃获取连接。
	 */
	if (retry)
	{
		Assert(entry->xact_depth == 0);

		ereport(DEBUG3,
				(errmsg_internal("could not start remote transaction on connection %p",
								 entry->conn)),
				errdetail_internal("%s", pchomp(PQerrorMessage(entry->conn))));

		elog(DEBUG3, "closing connection %p to reestablish a new one",
			 entry->conn);
		disconnect_pg_server(entry);

		make_new_connection(entry, user);

		begin_remote_xact(entry);
	}

	/* Remember if caller will prepare statements
	 *
	 * 请记住来电者是否会准备声明
	 */
	entry->have_prep_stmt |= will_prep_stmt;

	/* If caller needs access to the per-connection state, return it.
	 *
	 * 如果调用者需要访问每个连接的状态，则返回它。
	 */
	if (state)
		*state = &entry->state;

	return entry->conn;
}

/*
 * Reset all transient state fields in the cached connection entry and
 * establish new connection to the remote server.
 *
 * 重置缓存连接条目中的所有瞬态字段并建立与远程服务器的新连接。
 */
static void
make_new_connection(ConnCacheEntry *entry, UserMapping *user)
{
	ForeignServer *server = GetForeignServer(user->serverid);
	ListCell   *lc;

	Assert(entry->conn == NULL);

	/* Reset all transient state fields, to be sure all are clean
	 *
	 * 重置所有瞬态字段，以确保所有字段都是干净的
	 */
	entry->xact_depth = 0;
	entry->have_prep_stmt = false;
	entry->have_error = false;
	entry->changing_xact_state = false;
	entry->invalidated = false;
	entry->serverid = server->serverid;
	entry->server_hashvalue =
		GetSysCacheHashValue1(FOREIGNSERVEROID,
							  ObjectIdGetDatum(server->serverid));
	entry->mapping_hashvalue =
		GetSysCacheHashValue1(USERMAPPINGOID,
							  ObjectIdGetDatum(user->umid));
	memset(&entry->state, 0, sizeof(entry->state));

	/*
	 * Determine whether to keep the connection that we're about to make here
	 * open even after the transaction using it ends, so that the subsequent
	 * transactions can re-use it.
	 *
	 * 确定即使在使用该连接的事务结束后是否仍保持我们要在此处打开的连接，以便后续事务可以重新使用它。
	 *
	 * By default, all the connections to any foreign servers are kept open.
	 *
	 * 默认情况下，与任何外部服务器的所有连接都保持打开状态。
	 *
	 * Also determine whether to commit/abort (sub)transactions opened on the
	 * remote server in parallel at (sub)transaction end, which is disabled by
	 * default.
	 *
	 * 还确定是否在（子）事务结束时提交/中止在远程服务器上并行打开的（子）事务，默认情况下禁用。
	 *
	 * Note: it's enough to determine these only when making a new connection
	 * because if these settings for it are changed, it will be closed and
	 * re-made later.
	 *
	 * 注意：仅在建立新连接时确定这些就足够了，因为如果更改了这些设置，它将被关闭并稍后重新建立。
	 */
	entry->keep_connections = true;
	entry->parallel_commit = false;
	entry->parallel_abort = false;
	foreach(lc, server->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "keep_connections") == 0)
			entry->keep_connections = defGetBoolean(def);
		else if (strcmp(def->defname, "parallel_commit") == 0)
			entry->parallel_commit = defGetBoolean(def);
		else if (strcmp(def->defname, "parallel_abort") == 0)
			entry->parallel_abort = defGetBoolean(def);
	}

	/* Now try to make the connection
	 *
	 * 现在尝试建立连接
	 */
	entry->conn = connect_pg_server(server, user);

	elog(DEBUG3, "new postgres_fdw connection %p for server \"%s\" (user mapping oid %u, userid %u)",
		 entry->conn, server->servername, user->umid, user->userid);
}

/*
 * Check that non-superuser has used password or delegated credentials
 * to establish connection; otherwise, he's piggybacking on the
 * postgres server's user identity. See also dblink_security_check()
 * in contrib/dblink and check_conn_params.
 *
 * 检查非超级用户是否已使用密码或委派凭据来建立连接；否则，他就会利用 postgres 服务器的用户身份。另请参阅 contrib/dblink 中的 dblink_security_check() 和 check_conn_params。
 */
static void
pgfdw_security_check(const char **keywords, const char **values, UserMapping *user, PGconn *conn)
{
	/* Superusers bypass the check */
	/*
	 * 超级用户绕过检查
	 */
	if (superuser_arg(user->userid))
		return;

#ifdef ENABLE_GSS
	/* Connected via GSSAPI with delegated credentials- all good.
	 *
	 * 通过 GSSAPI 使用委托凭证进行连接 - 一切都很好。
	 */
	if (PQconnectionUsedGSSAPI(conn) && be_gssapi_get_delegation(MyProcPort))
		return;
#endif

	/* Ok if superuser set PW required false.
	 *
	 * 如果超级用户将 PW required 设置为 false，则可以。
	 */
	if (!UserMappingPasswordRequired(user))
		return;

	/* Connected via PW, with PW required true, and provided non-empty PW.
	 *
	 * 通过 PW 连接，PW 要求为 true，且提供的 PW 非空。
	 */
	if (PQconnectionUsedPassword(conn))
	{
		/* ok if params contain a non-empty password
		 *
		 * 如果 params 包含非空密码，则 ok
		 */
		for (int i = 0; keywords[i] != NULL; i++)
		{
			if (strcmp(keywords[i], "password") == 0 && values[i][0] != '\0')
				return;
		}
	}

	/*
	 * Ok if SCRAM pass-through is being used and all required SCRAM options
	 * are set correctly. If pgfdw_has_required_scram_options returns true we
	 * assume that UseScramPassthrough is also true since SCRAM options are
	 * only set when UseScramPassthrough is enabled.
	 *
	 * 如果正在使用 SCRAM 直通并且所有必需的 SCRAM 选项均已正确设置，则可以。如果 pgfdw_has_required_scram_options 返回 true，我们假设 UseScramPassthrough 也为 true，因为 SCRAM 选项仅在启用 UseScramPassthrough 时设置。
	 */
	if (MyProcPort != NULL && MyProcPort->has_scram_keys && pgfdw_has_required_scram_options(keywords, values))
		return;

	ereport(ERROR,
			(errcode(ERRCODE_S_R_E_PROHIBITED_SQL_STATEMENT_ATTEMPTED),
			 errmsg("password or GSSAPI delegated credentials required"),
			 errdetail("Non-superuser cannot connect if the server does not request a password or use GSSAPI with delegated credentials."),
			 errhint("Target server's authentication method must be changed or password_required=false set in the user mapping attributes.")));
}

/*
 * Connect to remote server using specified server and user mapping properties.
 *
 * 使用指定的服务器和用户映射属性连接到远程服务器。
 */
static PGconn *
connect_pg_server(ForeignServer *server, UserMapping *user)
{
	PGconn	   *volatile conn = NULL;

	/*
	 * Use PG_TRY block to ensure closing connection on error.
	 *
	 * 使用 PG_TRY 块确保在出错时关闭连接。
	 */
	PG_TRY();
	{
		const char **keywords;
		const char **values;
		char	   *appname = NULL;
		int			n;

		/*
		 * Construct connection params from generic options of ForeignServer
		 * and UserMapping.  (Some of them might not be libpq options, in
		 * which case we'll just waste a few array slots.)  Add 4 extra slots
		 * for application_name, fallback_application_name, client_encoding,
		 * end marker, and 3 extra slots for scram keys and required scram
		 * pass-through options.
		 *
		 * 从foreignserver和usermapping的通用选项构造连接参数。  （其中一些可能不是 libpq 选项，在这种情况下，我们只会浪费一些数组槽。）为 application_name、后备应用名称、client_encoding、结束标记添加 4 个额外槽，为 scram 键和所需的 scram 传递选项添加 3 个额外槽。
		 */
		n = list_length(server->options) + list_length(user->options) + 4 + 3;
		keywords = (const char **) palloc(n * sizeof(char *));
		values = (const char **) palloc(n * sizeof(char *));

		n = 0;
		n += ExtractConnectionOptions(server->options,
									  keywords + n, values + n);
		n += ExtractConnectionOptions(user->options,
									  keywords + n, values + n);

		/*
		 * Use pgfdw_application_name as application_name if set.
		 *
		 * 如果设置，则使用 pgfdw_application_name 作为 application_name。
		 *
		 * PQconnectdbParams() processes the parameter arrays from start to
		 * end. If any key word is repeated, the last value is used. Therefore
		 * note that pgfdw_application_name must be added to the arrays after
		 * options of ForeignServer are, so that it can override
		 * application_name set in ForeignServer.
		 *
		 * PQconnectdbParams() 从头到尾处理参数数组。如果任何关键字重复，则使用最后一个值。因此请注意，必须将pgfdw_application_name添加到ForeignServer选项之后的数组中，以便它可以覆盖ForeignServer中设置的application_name。
		 */
		if (pgfdw_application_name && *pgfdw_application_name != '\0')
		{
			keywords[n] = "application_name";
			values[n] = pgfdw_application_name;
			n++;
		}

		/*
		 * Search the parameter arrays to find application_name setting, and
		 * replace escape sequences in it with status information if found.
		 * The arrays are searched backwards because the last value is used if
		 * application_name is repeatedly set.
		 *
		 * 搜索参数数组以查找 application_name 设置，如果找到，则用状态信息替换其中的转义序列。向后搜索数组，因为如果重复设置 application_name，则使用最后一个值。
		 */
		for (int i = n - 1; i >= 0; i--)
		{
			if (strcmp(keywords[i], "application_name") == 0 &&
				*(values[i]) != '\0')
			{
				/*
				 * Use this application_name setting if it's not empty string
				 * even after any escape sequences in it are replaced.
				 *
				 * 如果 application_name 设置不是空字符串，即使其中的任何转义序列被替换，也可以使用此设置。
				 */
				appname = process_pgfdw_appname(values[i]);
				if (appname[0] != '\0')
				{
					values[i] = appname;
					break;
				}

				/*
				 * This empty application_name is not used, so we set
				 * values[i] to NULL and keep searching the array to find the
				 * next one.
				 *
				 * 这个空的 application_name 没有被使用，所以我们将 value[i] 设置为 NULL 并继续搜索数组以找到下一个。
				 */
				values[i] = NULL;
				pfree(appname);
				appname = NULL;
			}
		}

		/* Use "postgres_fdw" as fallback_application_name */
		/*
		 * 使用“postgres_fdw”作为后备应用名称。
		 */
		keywords[n] = "fallback_application_name";
		values[n] = "postgres_fdw";
		n++;

		/* Set client_encoding so that libpq can convert encoding properly.
		 *
		 * 设置client_encoding，以便libpq能够正确转换编码。
		 */
		keywords[n] = "client_encoding";
		values[n] = GetDatabaseEncodingName();
		n++;

		/* Add required SCRAM pass-through connection options if it's enabled.
		 *
		 * 添加所需的 SCRAM 直通连接选项（如果已启用）。
		 */
		if (MyProcPort != NULL && MyProcPort->has_scram_keys && UseScramPassthrough(server, user))
		{
			int			len;
			int			encoded_len;

			keywords[n] = "scram_client_key";
			len = pg_b64_enc_len(sizeof(MyProcPort->scram_ClientKey));
			/* don't forget the zero-terminator
			 *
			 * 不要忘记零终止符
			 */
			values[n] = palloc0(len + 1);
			encoded_len = pg_b64_encode(MyProcPort->scram_ClientKey,
										sizeof(MyProcPort->scram_ClientKey),
										(char *) values[n], len);
			if (encoded_len < 0)
				elog(ERROR, "could not encode SCRAM client key");
			n++;

			keywords[n] = "scram_server_key";
			len = pg_b64_enc_len(sizeof(MyProcPort->scram_ServerKey));
			/* don't forget the zero-terminator
			 *
			 * 不要忘记零终止符
			 */
			values[n] = palloc0(len + 1);
			encoded_len = pg_b64_encode(MyProcPort->scram_ServerKey,
										sizeof(MyProcPort->scram_ServerKey),
										(char *) values[n], len);
			if (encoded_len < 0)
				elog(ERROR, "could not encode SCRAM server key");
			n++;

			/*
			 * Require scram-sha-256 to ensure that no other auth method is
			 * used when connecting with foreign server.
			 *
			 * 需要 scram-sha-256 以确保与外部服务器连接时不使用其他身份验证方法。
			 */
			keywords[n] = "require_auth";
			values[n] = "scram-sha-256";
			n++;
		}

		keywords[n] = values[n] = NULL;

		/* Verify the set of connection parameters.
		 *
		 * 验证连接参数集。
		 */
		check_conn_params(keywords, values, user);

		/* first time, allocate or get the custom wait event
		 *
		 * 第一次，分配或获取自定义等待事件
		 */
		if (pgfdw_we_connect == 0)
			pgfdw_we_connect = WaitEventExtensionNew("PostgresFdwConnect");

		/* OK to make connection
		 *
		 * 确定建立连接
		 */
		conn = libpqsrv_connect_params(keywords, values,
									   false,	/* expand_dbname */
									   pgfdw_we_connect);

		if (!conn || PQstatus(conn) != CONNECTION_OK)
			ereport(ERROR,
					(errcode(ERRCODE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION),
					 errmsg("could not connect to server \"%s\"",
							server->servername),
					 errdetail_internal("%s", pchomp(PQerrorMessage(conn)))));

		/* Perform post-connection security checks.
		 *
		 * 执行连接后安全检查。
		 */
		pgfdw_security_check(keywords, values, user, conn);

		/* Prepare new session for use
		 *
		 * 准备新会话以供使用
		 */
		configure_remote_session(conn);

		if (appname != NULL)
			pfree(appname);
		pfree(keywords);
		pfree(values);
	}
	PG_CATCH();
	{
		libpqsrv_disconnect(conn);
		PG_RE_THROW();
	}
	PG_END_TRY();

	return conn;
}

/*
 * Disconnect any open connection for a connection cache entry.
 *
 * 断开连接缓存条目的任何打开的连接。
 */
static void
disconnect_pg_server(ConnCacheEntry *entry)
{
	if (entry->conn != NULL)
	{
		libpqsrv_disconnect(entry->conn);
		entry->conn = NULL;
	}
}

/*
 * Return true if the password_required is defined and false for this user
 * mapping, otherwise false. The mapping has been pre-validated.
 *
 * 如果定义了该用户映射的password_required，则返回true，否则返回false。该映射已经过预先验证。
 */
static bool
UserMappingPasswordRequired(UserMapping *user)
{
	ListCell   *cell;

	foreach(cell, user->options)
	{
		DefElem    *def = (DefElem *) lfirst(cell);

		if (strcmp(def->defname, "password_required") == 0)
			return defGetBoolean(def);
	}

	return true;
}

static bool
UseScramPassthrough(ForeignServer *server, UserMapping *user)
{
	ListCell   *cell;

	foreach(cell, server->options)
	{
		DefElem    *def = (DefElem *) lfirst(cell);

		if (strcmp(def->defname, "use_scram_passthrough") == 0)
			return defGetBoolean(def);
	}

	foreach(cell, user->options)
	{
		DefElem    *def = (DefElem *) lfirst(cell);

		if (strcmp(def->defname, "use_scram_passthrough") == 0)
			return defGetBoolean(def);
	}

	return false;
}

/*
 * For non-superusers, insist that the connstr specify a password or that the
 * user provided their own GSSAPI delegated credentials.  This
 * prevents a password from being picked up from .pgpass, a service file, the
 * environment, etc.  We don't want the postgres user's passwords,
 * certificates, etc to be accessible to non-superusers.  (See also
 * dblink_connstr_check in contrib/dblink.)
 *
 * 对于非超级用户，坚持要求 connstr 指定密码或用户提供自己的 GSSAPI 委托凭据。  这可以防止从 .pgpass、服务文件、环境等中获取密码。我们不希望非超级用户可以访问 postgres 用户的密码、证书等。  （另请参阅 contrib/dblink 中的 dblink_connstr_check。）
 */
static void
check_conn_params(const char **keywords, const char **values, UserMapping *user)
{
	int			i;

	/* no check required if superuser
	 *
	 * 如果是超级用户则无需检查
	 */
	if (superuser_arg(user->userid))
		return;

#ifdef ENABLE_GSS
	/* ok if the user provided their own delegated credentials
	 *
	 * 如果用户提供了自己的委派凭据，则可以
	 */
	if (be_gssapi_get_delegation(MyProcPort))
		return;
#endif

	/* ok if params contain a non-empty password
	 *
	 * 如果 params 包含非空密码，则 ok
	 */
	for (i = 0; keywords[i] != NULL; i++)
	{
		if (strcmp(keywords[i], "password") == 0 && values[i][0] != '\0')
			return;
	}

	/* ok if the superuser explicitly said so at user mapping creation time
	 *
	 * 好的，如果超级用户在用户映射创建时明确这么说的话
	 */
	if (!UserMappingPasswordRequired(user))
		return;

	/*
	 * Ok if SCRAM pass-through is being used and all required scram options
	 * are set correctly. If pgfdw_has_required_scram_options returns true we
	 * assume that UseScramPassthrough is also true since SCRAM options are
	 * only set when UseScramPassthrough is enabled.
	 *
	 * 如果正在使用 SCRAM 直通并且所有必需的 scram 选项均已正确设置，则可以。如果 pgfdw_has_required_scram_options 返回 true，我们假设 UseScramPassthrough 也为 true，因为 SCRAM 选项仅在启用 UseScramPassthrough 时设置。
	 */
	if (MyProcPort != NULL && MyProcPort->has_scram_keys && pgfdw_has_required_scram_options(keywords, values))
		return;

	ereport(ERROR,
			(errcode(ERRCODE_S_R_E_PROHIBITED_SQL_STATEMENT_ATTEMPTED),
			 errmsg("password or GSSAPI delegated credentials required"),
			 errdetail("Non-superusers must delegate GSSAPI credentials, provide a password, or enable SCRAM pass-through in user mapping.")));
}

/*
 * Issue SET commands to make sure remote session is configured properly.
 *
 * 发出 SET 命令以确保远程会话配置正确。
 *
 * We do this just once at connection, assuming nothing will change the
 * values later.  Since we'll never send volatile function calls to the
 * remote, there shouldn't be any way to break this assumption from our end.
 * It's possible to think of ways to break it at the remote end, eg making
 * a foreign table point to a view that includes a set_config call ---
 * but once you admit the possibility of a malicious view definition,
 * there are any number of ways to break things.
 *
 * 我们在连接时只执行一次此操作，假设稍后不会更改这些值。  由于我们永远不会向远程发送易失性函数调用，因此不应该有任何方法从我们这边打破这个假设。可以考虑在远程端破坏它的方法，例如使外部表指向包含 set_config 调用的视图 --- 但是一旦您承认恶意视图定义的可能性，就有许多方法可以破坏它。
 */
static void
configure_remote_session(PGconn *conn)
{
	int			remoteversion = PQserverVersion(conn);

	/* Force the search path to contain only pg_catalog (see deparse.c)
	 *
	 * 强制搜索路径仅包含 pg_catalog （请参阅 deparse.c）
	 */
	do_sql_command(conn, "SET search_path = pg_catalog");

	/*
	 * Set remote timezone; this is basically just cosmetic, since all
	 * transmitted and returned timestamptzs should specify a zone explicitly
	 * anyway.  However it makes the regression test outputs more predictable.
	 *
	 * 设置远程时区；这基本上只是装饰性的，因为所有传输和返回的时间戳都应该明确指定一个区域。  然而，它使回归测试的输出更加可预测。
	 *
	 * We don't risk setting remote zone equal to ours, since the remote
	 * server might use a different timezone database.  Instead, use GMT
	 * (quoted, because very old servers are picky about case).  That's
	 * guaranteed to work regardless of the remote's timezone database,
	 * because pg_tzset() hard-wires it (at least in PG 9.2 and later).
	 *
	 * 我们不会冒险将远程区域设置为与我们的相同，因为远程服务器可能使用不同的时区数据库。  相反，请使用 GMT（引用，因为非常旧的服务器对大小写很挑剔）。  无论远程的时区数据库如何，这都保证可以工作，因为 pg_tzset() 对其进行了硬连接（至少在 PG 9.2 及更高版本中）。
	 */
	do_sql_command(conn, "SET timezone = 'GMT'");

	/*
	 * Set values needed to ensure unambiguous data output from remote.  (This
	 * logic should match what pg_dump does.  See also set_transmission_modes
	 * in postgres_fdw.c.)
	 *
	 * 设置所需的值以确保从远程输出明确的数据。  （这个逻辑应该与 pg_dump 的做法相匹配。另请参阅 postgres_fdw.c 中的 set_transmission_modes。）
	 */
	do_sql_command(conn, "SET datestyle = ISO");
	if (remoteversion >= 80400)
		do_sql_command(conn, "SET intervalstyle = postgres");
	if (remoteversion >= 90000)
		do_sql_command(conn, "SET extra_float_digits = 3");
	else
		do_sql_command(conn, "SET extra_float_digits = 2");
}

/*
 * Convenience subroutine to issue a non-data-returning SQL command to remote
 *
 * 用于向远程发出非数据返回 SQL 命令的便捷子例程
 */
void
do_sql_command(PGconn *conn, const char *sql)
{
	do_sql_command_begin(conn, sql);
	do_sql_command_end(conn, sql, false);
}

static void
do_sql_command_begin(PGconn *conn, const char *sql)
{
	if (!PQsendQuery(conn, sql))
		pgfdw_report_error(ERROR, NULL, conn, false, sql);
}

static void
do_sql_command_end(PGconn *conn, const char *sql, bool consume_input)
{
	PGresult   *res;

	/*
	 * If requested, consume whatever data is available from the socket. (Note
	 * that if all data is available, this allows pgfdw_get_result to call
	 * PQgetResult without forcing the overhead of WaitLatchOrSocket, which
	 * would be large compared to the overhead of PQconsumeInput.)
	 *
	 * 如果有请求，则使用套接字中可用的任何数据。 （请注意，如果所有数据均可用，则这允许 pgfdw_get_result 调用 PQgetResult，而无需强制使用 WaitLatchOrSocket 的开销，与 PQconsumeInput 的开销相比，该开销会很大。）
	 */
	if (consume_input && !PQconsumeInput(conn))
		pgfdw_report_error(ERROR, NULL, conn, false, sql);
	res = pgfdw_get_result(conn);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pgfdw_report_error(ERROR, res, conn, true, sql);
	PQclear(res);
}

/*
 * Start remote transaction or subtransaction, if needed.
 *
 * 如果需要，启动远程事务或子事务。
 *
 * Note that we always use at least REPEATABLE READ in the remote session.
 * This is so that, if a query initiates multiple scans of the same or
 * different foreign tables, we will get snapshot-consistent results from
 * those scans.  A disadvantage is that we can't provide sane emulation of
 * READ COMMITTED behavior --- it would be nice if we had some other way to
 * control which remote queries share a snapshot.
 *
 * 请注意，我们始终在远程会话中至少使用 REPEATABLE READ。这样，如果查询启动对相同或不同外表的多次扫描，我们将从这些扫描中获得快照一致的结果。  缺点是我们无法提供 READ COMMITTED 行为的合理模拟——如果我们有其他方法来控制哪些远程查询共享快照，那就太好了。
 */
static void
begin_remote_xact(ConnCacheEntry *entry)
{
	int			curlevel = GetCurrentTransactionNestLevel();

	/* Start main transaction if we haven't yet
	 *
	 * 如果我们还没有开始主要交易
	 */
	if (entry->xact_depth <= 0)
	{
		const char *sql;

		elog(DEBUG3, "starting remote transaction on connection %p",
			 entry->conn);

		if (IsolationIsSerializable())
			sql = "START TRANSACTION ISOLATION LEVEL SERIALIZABLE";
		else
			sql = "START TRANSACTION ISOLATION LEVEL REPEATABLE READ";
		entry->changing_xact_state = true;
		do_sql_command(entry->conn, sql);
		entry->xact_depth = 1;
		entry->changing_xact_state = false;
	}

	/*
	 * If we're in a subtransaction, stack up savepoints to match our level.
	 * This ensures we can rollback just the desired effects when a
	 * subtransaction aborts.
	 *
	 * 如果我们处于子事务中，请堆叠保存点以匹配我们的级别。这确保了当子事务中止时我们可以只回滚所需的效果。
	 */
	while (entry->xact_depth < curlevel)
	{
		char		sql[64];

		snprintf(sql, sizeof(sql), "SAVEPOINT s%d", entry->xact_depth + 1);
		entry->changing_xact_state = true;
		do_sql_command(entry->conn, sql);
		entry->xact_depth++;
		entry->changing_xact_state = false;
	}
}

/*
 * Release connection reference count created by calling GetConnection.
 *
 * 释放通过调用 GetConnection 创建的连接引用计数。
 */
void
ReleaseConnection(PGconn *conn)
{
	/*
	 * Currently, we don't actually track connection references because all
	 * cleanup is managed on a transaction or subtransaction basis instead. So
	 * there's nothing to do here.
	 *
	 * 目前，我们实际上并不跟踪连接引用，因为所有清理都是在事务或子事务的基础上进行管理。所以这里没什么可做的。
	 */
}

/*
 * Assign a "unique" number for a cursor.
 *
 * 为光标分配一个“唯一”编号。
 *
 * These really only need to be unique per connection within a transaction.
 * For the moment we ignore the per-connection point and assign them across
 * all connections in the transaction, but we ask for the connection to be
 * supplied in case we want to refine that.
 *
 * 这些实际上只需要在事务中的每个连接都是唯一的。目前，我们忽略每个连接点并将它们分配给事务中的所有连接，但我们要求提供连接，以防我们想要改进它。
 *
 * Note that even if wraparound happens in a very long transaction, actual
 * collisions are highly improbable; just be sure to use %u not %d to print.
 *
 * 请注意，即使在很长的事务中发生回绕，实际发生冲突的可能性也很小；请务必使用 %u 而不是 %d 进行打印。
 */
unsigned int
GetCursorNumber(PGconn *conn)
{
	return ++cursor_number;
}

/*
 * Assign a "unique" number for a prepared statement.
 *
 * 为准备好的语句分配一个“唯一”编号。
 *
 * This works much like GetCursorNumber, except that we never reset the counter
 * within a session.  That's because we can't be 100% sure we've gotten rid
 * of all prepared statements on all connections, and it's not really worth
 * increasing the risk of prepared-statement name collisions by resetting.
 *
 * 这与 GetCursorNumber 的工作原理非常相似，只是我们从不在会话中重置计数器。  这是因为我们不能 100% 确定我们已经删除了所有连接上的所有准备语句，并且通过重置来增加准备语句名称冲突的风险并不值得。
 */
unsigned int
GetPrepStmtNumber(PGconn *conn)
{
	return ++prep_stmt_number;
}

/*
 * Submit a query and wait for the result.
 *
 * 提交查询并等待结果。
 *
 * Since we don't use non-blocking mode, this can't process interrupts while
 * pushing the query text to the server.  That risk is relatively small, so we
 * ignore that for now.
 *
 * 由于我们不使用非阻塞模式，因此在将查询文本推送到服务器时无法处理中断。  这种风险相对较小，所以我们暂时忽略它。
 *
 * Caller is responsible for the error handling on the result.
 *
 * 调用者负责对结果进行错误处理。
 */
PGresult *
pgfdw_exec_query(PGconn *conn, const char *query, PgFdwConnState *state)
{
	/* First, process a pending asynchronous request, if any.
	 *
	 * 首先，处理挂起的异步请求（如果有）。
	 */
	if (state && state->pendingAreq)
		process_pending_request(state->pendingAreq);

	if (!PQsendQuery(conn, query))
		return NULL;
	return pgfdw_get_result(conn);
}

/*
 * Wrap libpqsrv_get_result_last(), adding wait event.
 *
 * 包装 libpqsrv_get_result_last()，添加等待事件。
 *
 * Caller is responsible for the error handling on the result.
 *
 * 调用者负责对结果进行错误处理。
 */
PGresult *
pgfdw_get_result(PGconn *conn)
{
	return libpqsrv_get_result_last(conn, pgfdw_we_get_result);
}

/*
 * Report an error we got from the remote server.
 *
 * 报告我们从远程服务器收到的错误。
 *
 * elevel: error level to use (typically ERROR, but might be less)
 * res: PGresult containing the error
 * conn: connection we did the query on
 * clear: if true, PQclear the result (otherwise caller will handle it)
 * sql: NULL, or text of remote command we tried to execute
 *
 * elevel：要使用的错误级别（通常为 ERROR，但可能更少） res：包含错误的 PGresult conn：我们在clear上执行查询的连接：如果为 true，则 PQclear 结果（否则调用者将处理它） sql：NULL，或我们尝试执行的远程命令的文本
 *
 * Note: callers that choose not to throw ERROR for a remote error are
 * responsible for making sure that the associated ConnCacheEntry gets
 * marked with have_error = true.
 *
 * 注意：选择不为远程错误抛出 ERROR 的调用者有责任确保关联的 ConnCacheEntry 被标记为 have_error = true。
 */
void
pgfdw_report_error(int elevel, PGresult *res, PGconn *conn,
				   bool clear, const char *sql)
{
	/* If requested, PGresult must be released before leaving this function.
	 *
	 * 如果需要，必须在离开此函数之前释放 PGresult。
	 */
	PG_TRY();
	{
		char	   *diag_sqlstate = PQresultErrorField(res, PG_DIAG_SQLSTATE);
		char	   *message_primary = PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY);
		char	   *message_detail = PQresultErrorField(res, PG_DIAG_MESSAGE_DETAIL);
		char	   *message_hint = PQresultErrorField(res, PG_DIAG_MESSAGE_HINT);
		char	   *message_context = PQresultErrorField(res, PG_DIAG_CONTEXT);
		int			sqlstate;

		if (diag_sqlstate)
			sqlstate = MAKE_SQLSTATE(diag_sqlstate[0],
									 diag_sqlstate[1],
									 diag_sqlstate[2],
									 diag_sqlstate[3],
									 diag_sqlstate[4]);
		else
			sqlstate = ERRCODE_CONNECTION_FAILURE;

		/*
		 * If we don't get a message from the PGresult, try the PGconn.  This
		 * is needed because for connection-level failures, PQgetResult may
		 * just return NULL, not a PGresult at all.
		 *
		 * 如果我们没有从 PGresult 收到消息，请尝试 PGconn。  这是必需的，因为对于连接级故障，PQgetResult 可能只返回 NULL，而不是 PGresult。
		 */
		if (message_primary == NULL)
			message_primary = pchomp(PQerrorMessage(conn));

		ereport(elevel,
				(errcode(sqlstate),
				 (message_primary != NULL && message_primary[0] != '\0') ?
				 errmsg_internal("%s", message_primary) :
				 errmsg("could not obtain message string for remote error"),
				 message_detail ? errdetail_internal("%s", message_detail) : 0,
				 message_hint ? errhint("%s", message_hint) : 0,
				 message_context ? errcontext("%s", message_context) : 0,
				 sql ? errcontext("remote SQL command: %s", sql) : 0));
	}
	PG_FINALLY();
	{
		if (clear)
			PQclear(res);
	}
	PG_END_TRY();
}

/*
 * pgfdw_xact_callback --- cleanup at main-transaction end.
 *
 * pgfdw_xact_callback --- 主事务结束时的清理。
 *
 * This runs just late enough that it must not enter user-defined code
 * locally.  (Entering such code on the remote side is fine.  Its remote
 * COMMIT TRANSACTION may run deferred triggers.)
 *
 * 它运行得足够晚，以至于它不能在本地输入用户定义的代码。  （在远程端输入这样的代码是可以的。其远程 COMMIT TRANSACTION 可能会运行延迟触发器。）
 */
static void
pgfdw_xact_callback(XactEvent event, void *arg)
{
	HASH_SEQ_STATUS scan;
	ConnCacheEntry *entry;
	List	   *pending_entries = NIL;
	List	   *cancel_requested = NIL;

	/* Quick exit if no connections were touched in this transaction.
	 *
	 * 如果此事务中没有触及任何连接，则快速退出。
	 */
	if (!xact_got_connection)
		return;

	/*
	 * Scan all connection cache entries to find open remote transactions, and
	 * close them.
	 *
	 * 扫描所有连接缓存条目以查找打开的远程事务，然后关闭它们。
	 */
	hash_seq_init(&scan, ConnectionHash);
	while ((entry = (ConnCacheEntry *) hash_seq_search(&scan)))
	{
		PGresult   *res;

		/* Ignore cache entry if no open connection right now
		 *
		 * 如果现在没有打开的连接，则忽略缓存条目
		 */
		if (entry->conn == NULL)
			continue;

		/* If it has an open remote transaction, try to close it
		 *
		 * 如果有打开的远程事务，请尝试关闭它
		 */
		if (entry->xact_depth > 0)
		{
			elog(DEBUG3, "closing remote transaction on connection %p",
				 entry->conn);

			switch (event)
			{
				case XACT_EVENT_PARALLEL_PRE_COMMIT:
				case XACT_EVENT_PRE_COMMIT:

					/*
					 * If abort cleanup previously failed for this connection,
					 * we can't issue any more commands against it.
					 *
					 * 如果之前对该连接的中止清理失败，我们将无法对其发出任何更多命令。
					 */
					pgfdw_reject_incomplete_xact_state_change(entry);

					/* Commit all remote transactions during pre-commit
					 *
					 * 在预提交期间提交所有远程事务
					 */
					entry->changing_xact_state = true;
					if (entry->parallel_commit)
					{
						do_sql_command_begin(entry->conn, "COMMIT TRANSACTION");
						pending_entries = lappend(pending_entries, entry);
						continue;
					}
					do_sql_command(entry->conn, "COMMIT TRANSACTION");
					entry->changing_xact_state = false;

					/*
					 * If there were any errors in subtransactions, and we
					 * made prepared statements, do a DEALLOCATE ALL to make
					 * sure we get rid of all prepared statements. This is
					 * annoying and not terribly bulletproof, but it's
					 * probably not worth trying harder.
					 *
					 * 如果子事务中有任何错误，并且我们做了准备好的语句，请执行 DEALLOCATE ALL 以确保我们删除所有准备好的语句。这很烦人，而且不是非常防弹，但可能不值得更加努力。
					 *
					 * DEALLOCATE ALL only exists in 8.3 and later, so this
					 * constrains how old a server postgres_fdw can
					 * communicate with.  We intentionally ignore errors in
					 * the DEALLOCATE, so that we can hobble along to some
					 * extent with older servers (leaking prepared statements
					 * as we go; but we don't really support update operations
					 * pre-8.3 anyway).
					 *
					 * DEALLOCATE ALL 仅存在于 8.3 及更高版本中，因此这限制了服务器 postgres_fdw 可以与之通信的年龄。  我们故意忽略 DEALLOCATE 中的错误，这样我们就可以在某种程度上与较旧的服务器打交道（在我们进行的过程中泄漏准备好的语句；但无论如何我们并不真正支持 8.3 之前的更新操作）。
					 */
					if (entry->have_prep_stmt && entry->have_error)
					{
						res = pgfdw_exec_query(entry->conn, "DEALLOCATE ALL",
											   NULL);
						PQclear(res);
					}
					entry->have_prep_stmt = false;
					entry->have_error = false;
					break;
				case XACT_EVENT_PRE_PREPARE:

					/*
					 * We disallow any remote transactions, since it's not
					 * very reasonable to hold them open until the prepared
					 * transaction is committed.  For the moment, throw error
					 * unconditionally; later we might allow read-only cases.
					 * Note that the error will cause us to come right back
					 * here with event == XACT_EVENT_ABORT, so we'll clean up
					 * the connection state at that point.
					 *
					 * 我们不允许任何远程事务，因为在提交准备好的事务之前保持它们打开是不太合理的。  目前，无条件抛出错误；稍后我们可能会允许只读情况。请注意，该错误将导致我们以 event == XACT_EVENT_ABORT 的方式返回此处，因此我们将在此时清理连接状态。
					 */
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot PREPARE a transaction that has operated on postgres_fdw foreign tables")));
					break;
				case XACT_EVENT_PARALLEL_COMMIT:
				case XACT_EVENT_COMMIT:
				case XACT_EVENT_PREPARE:
					/* Pre-commit should have closed the open transaction
					 *
					 * 预提交应该已经关闭了打开的事务
					 */
					elog(ERROR, "missed cleaning up connection during pre-commit");
					break;
				case XACT_EVENT_PARALLEL_ABORT:
				case XACT_EVENT_ABORT:
					/* Rollback all remote transactions during abort
					 *
					 * 中止期间回滚所有远程事务
					 */
					if (entry->parallel_abort)
					{
						if (pgfdw_abort_cleanup_begin(entry, true,
													  &pending_entries,
													  &cancel_requested))
							continue;
					}
					else
						pgfdw_abort_cleanup(entry, true);
					break;
			}
		}

		/* Reset state to show we're out of a transaction
		 *
		 * 重置状态以显示我们已结束交易
		 */
		pgfdw_reset_xact_state(entry, true);
	}

	/* If there are any pending connections, finish cleaning them up
	 *
	 * 如果有任何挂起的连接，请完成清理它们
	 */
	if (pending_entries || cancel_requested)
	{
		if (event == XACT_EVENT_PARALLEL_PRE_COMMIT ||
			event == XACT_EVENT_PRE_COMMIT)
		{
			Assert(cancel_requested == NIL);
			pgfdw_finish_pre_commit_cleanup(pending_entries);
		}
		else
		{
			Assert(event == XACT_EVENT_PARALLEL_ABORT ||
				   event == XACT_EVENT_ABORT);
			pgfdw_finish_abort_cleanup(pending_entries, cancel_requested,
									   true);
		}
	}

	/*
	 * Regardless of the event type, we can now mark ourselves as out of the
	 * transaction.  (Note: if we are here during PRE_COMMIT or PRE_PREPARE,
	 * this saves a useless scan of the hashtable during COMMIT or PREPARE.)
	 *
	 * 无论事件类型如何，我们现在都可以将自己标记为退出事务。  （注意：如果我们在 PRE_COMMIT 或 PRE_PREPARE 期间处于此处，则可以在 COMMIT 或 PREPARE 期间节省对哈希表的无用扫描。）
	 */
	xact_got_connection = false;

	/* Also reset cursor numbering for next transaction
	 *
	 * 还为下一个事务重置光标编号
	 */
	cursor_number = 0;
}

/*
 * pgfdw_subxact_callback --- cleanup at subtransaction end.
 *
 * pgfdw_subxact_callback --- 子事务结束时的清理。
 */
static void
pgfdw_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
					   SubTransactionId parentSubid, void *arg)
{
	HASH_SEQ_STATUS scan;
	ConnCacheEntry *entry;
	int			curlevel;
	List	   *pending_entries = NIL;
	List	   *cancel_requested = NIL;

	/* Nothing to do at subxact start, nor after commit.
	 *
	 * 在 subxact 启动时和提交后都无需执行任何操作。
	 */
	if (!(event == SUBXACT_EVENT_PRE_COMMIT_SUB ||
		  event == SUBXACT_EVENT_ABORT_SUB))
		return;

	/* Quick exit if no connections were touched in this transaction.
	 *
	 * 如果此事务中没有触及任何连接，则快速退出。
	 */
	if (!xact_got_connection)
		return;

	/*
	 * Scan all connection cache entries to find open remote subtransactions
	 * of the current level, and close them.
	 *
	 * 扫描所有连接缓存条目，找到当前级别打开的远程子事务，并将其关闭。
	 */
	curlevel = GetCurrentTransactionNestLevel();
	hash_seq_init(&scan, ConnectionHash);
	while ((entry = (ConnCacheEntry *) hash_seq_search(&scan)))
	{
		char		sql[100];

		/*
		 * We only care about connections with open remote subtransactions of
		 * the current level.
		 *
		 * 我们只关心与当前级别的开放远程子事务的连接。
		 */
		if (entry->conn == NULL || entry->xact_depth < curlevel)
			continue;

		if (entry->xact_depth > curlevel)
			elog(ERROR, "missed cleaning up remote subtransaction at level %d",
				 entry->xact_depth);

		if (event == SUBXACT_EVENT_PRE_COMMIT_SUB)
		{
			/*
			 * If abort cleanup previously failed for this connection, we
			 * can't issue any more commands against it.
			 *
			 * 如果之前对该连接的中止清理失败，我们将无法对其发出任何更多命令。
			 */
			pgfdw_reject_incomplete_xact_state_change(entry);

			/* Commit all remote subtransactions during pre-commit
			 *
			 * 在预提交期间提交所有远程子事务
			 */
			snprintf(sql, sizeof(sql), "RELEASE SAVEPOINT s%d", curlevel);
			entry->changing_xact_state = true;
			if (entry->parallel_commit)
			{
				do_sql_command_begin(entry->conn, sql);
				pending_entries = lappend(pending_entries, entry);
				continue;
			}
			do_sql_command(entry->conn, sql);
			entry->changing_xact_state = false;
		}
		else
		{
			/* Rollback all remote subtransactions during abort
			 *
			 * 中止期间回滚所有远程子事务
			 */
			if (entry->parallel_abort)
			{
				if (pgfdw_abort_cleanup_begin(entry, false,
											  &pending_entries,
											  &cancel_requested))
					continue;
			}
			else
				pgfdw_abort_cleanup(entry, false);
		}

		/* OK, we're outta that level of subtransaction
		 *
		 * 好的，我们已经超出了子事务的级别
		 */
		pgfdw_reset_xact_state(entry, false);
	}

	/* If there are any pending connections, finish cleaning them up
	 *
	 * 如果有任何挂起的连接，请完成清理它们
	 */
	if (pending_entries || cancel_requested)
	{
		if (event == SUBXACT_EVENT_PRE_COMMIT_SUB)
		{
			Assert(cancel_requested == NIL);
			pgfdw_finish_pre_subcommit_cleanup(pending_entries, curlevel);
		}
		else
		{
			Assert(event == SUBXACT_EVENT_ABORT_SUB);
			pgfdw_finish_abort_cleanup(pending_entries, cancel_requested,
									   false);
		}
	}
}

/*
 * Connection invalidation callback function
 *
 * 连接失效回调函数
 *
 * After a change to a pg_foreign_server or pg_user_mapping catalog entry,
 * close connections depending on that entry immediately if current transaction
 * has not used those connections yet. Otherwise, mark those connections as
 * invalid and then make pgfdw_xact_callback() close them at the end of current
 * transaction, since they cannot be closed in the midst of the transaction
 * using them. Closed connections will be remade at the next opportunity if
 * necessary.
 *
 * 更改 pg_foreign_server 或 pg_user_mapping 目录条目后，如果当前事务尚未使用这些连接，则立即关闭取决于该条目的连接。否则，将这些连接标记为无效，然后使 pgfdw_xact_callback() 在当前事务结束时关闭它们，因为它们无法在使用它们的事务中间关闭。如有必要，将在下次机会时重新建立关闭的连接。
 *
 * Although most cache invalidation callbacks blow away all the related stuff
 * regardless of the given hashvalue, connections are expensive enough that
 * it's worth trying to avoid that.
 *
 * 尽管大多数缓存失效回调都会清除所有相关内容，而不管给定的哈希值如何，但连接的成本足够高，值得尝试避免这种情况。
 *
 * NB: We could avoid unnecessary disconnection more strictly by examining
 * individual option values, but it seems too much effort for the gain.
 *
 * 注意：我们可以通过检查单个选项值来更严格地避免不必要的断开连接，但这似乎需要付出太多努力才能获得收益。
 */
static void
pgfdw_inval_callback(Datum arg, int cacheid, uint32 hashvalue)
{
	HASH_SEQ_STATUS scan;
	ConnCacheEntry *entry;

	Assert(cacheid == FOREIGNSERVEROID || cacheid == USERMAPPINGOID);

	/* ConnectionHash must exist already, if we're registered
	 *
	 * 如果我们注册了，ConnectionHash 必须已经存在
	 */
	hash_seq_init(&scan, ConnectionHash);
	while ((entry = (ConnCacheEntry *) hash_seq_search(&scan)))
	{
		/* Ignore invalid entries
		 *
		 * 忽略无效条目
		 */
		if (entry->conn == NULL)
			continue;

		/* hashvalue == 0 means a cache reset, must clear all state
		 *
		 * hashvalue == 0 表示缓存重置，必须清除所有状态
		 */
		if (hashvalue == 0 ||
			(cacheid == FOREIGNSERVEROID &&
			 entry->server_hashvalue == hashvalue) ||
			(cacheid == USERMAPPINGOID &&
			 entry->mapping_hashvalue == hashvalue))
		{
			/*
			 * Close the connection immediately if it's not used yet in this
			 * transaction. Otherwise mark it as invalid so that
			 * pgfdw_xact_callback() can close it at the end of this
			 * transaction.
			 *
			 * 如果此事务中尚未使用连接，请立即关闭连接。否则将其标记为无效，以便 pgfdw_xact_callback() 可以在此事务结束时将其关闭。
			 */
			if (entry->xact_depth == 0)
			{
				elog(DEBUG3, "discarding connection %p", entry->conn);
				disconnect_pg_server(entry);
			}
			else
				entry->invalidated = true;
		}
	}
}

/*
 * Raise an error if the given connection cache entry is marked as being
 * in the middle of an xact state change.  This should be called at which no
 * such change is expected to be in progress; if one is found to be in
 * progress, it means that we aborted in the middle of a previous state change
 * and now don't know what the remote transaction state actually is.
 * Such connections can't safely be further used.  Re-establishing the
 * connection would change the snapshot and roll back any writes already
 * performed, so that's not an option, either. Thus, we must abort.
 *
 * 如果给定的连接缓存条目被标记为正处于 xact 状态更改的中间，则会引发错误。  预计不会发生此类更改时应调用此方法；如果发现一个事务正在进行中，则意味着我们在上一次状态更改期间中止，现在不知道远程事务状态实际上是什么。无法安全地进一步使用此类连接。  重新建立连接将更改快照并回滚任何已执行的写入，因此这也不是一个选项。因此，我们必须中止。
 */
static void
pgfdw_reject_incomplete_xact_state_change(ConnCacheEntry *entry)
{
	ForeignServer *server;

	/* nothing to do for inactive entries and entries of sane state
	 *
	 * 对于不活动的条目和正常状态的条目没有任何作用
	 */
	if (entry->conn == NULL || !entry->changing_xact_state)
		return;

	/* make sure this entry is inactive
	 *
	 * 确保该条目处于非活动状态
	 */
	disconnect_pg_server(entry);

	/* find server name to be shown in the message below
	 *
	 * 找到下面消息中显示的服务器名称
	 */
	server = GetForeignServer(entry->serverid);

	ereport(ERROR,
			(errcode(ERRCODE_CONNECTION_EXCEPTION),
			 errmsg("connection to server \"%s\" was lost",
					server->servername)));
}

/*
 * Reset state to show we're out of a (sub)transaction.
 *
 * 重置状态以表明我们已经完成（子）事务。
 */
static void
pgfdw_reset_xact_state(ConnCacheEntry *entry, bool toplevel)
{
	if (toplevel)
	{
		/* Reset state to show we're out of a transaction
		 *
		 * 重置状态以显示我们已结束交易
		 */
		entry->xact_depth = 0;

		/*
		 * If the connection isn't in a good idle state, it is marked as
		 * invalid or keep_connections option of its server is disabled, then
		 * discard it to recover. Next GetConnection will open a new
		 * connection.
		 *
		 * 如果连接未处于良好的空闲状态，则将其标记为无效或禁用其服务器的 keep_connections 选项，然后将其丢弃以恢复。接下来 GetConnection 将打开一个新连接。
		 */
		if (PQstatus(entry->conn) != CONNECTION_OK ||
			PQtransactionStatus(entry->conn) != PQTRANS_IDLE ||
			entry->changing_xact_state ||
			entry->invalidated ||
			!entry->keep_connections)
		{
			elog(DEBUG3, "discarding connection %p", entry->conn);
			disconnect_pg_server(entry);
		}
	}
	else
	{
		/* Reset state to show we're out of a subtransaction
		 *
		 * 重置状态以显示我们已经完成子事务
		 */
		entry->xact_depth--;
	}
}

/*
 * Cancel the currently-in-progress query (whose query text we do not have)
 * and ignore the result.  Returns true if we successfully cancel the query
 * and discard any pending result, and false if not.
 *
 * 取消当前正在进行的查询（我们没有其查询文本）并忽略结果。  如果我们成功取消查询并丢弃任何挂起的结果，则返回 true，否则返回 false。
 *
 * It's not a huge problem if we throw an ERROR here, but if we get into error
 * recursion trouble, we'll end up slamming the connection shut, which will
 * necessitate failing the entire toplevel transaction even if subtransactions
 * were used.  Try to use WARNING where we can.
 *
 * 如果我们在这里抛出一个错误，这并不是一个大问题，但是如果我们遇到错误递归问题，我们最终将关闭连接，这将导致整个顶级事务失败，即使使用了子事务。  尽可能使用警告。
 *
 * XXX: if the query was one sent by fetch_more_data_begin(), we could get the
 * query text from the pendingAreq saved in the per-connection state, then
 * report the query using it.
 *
 * XXX：如果查询是由 fetch_more_data_begin() 发送的，我们可以从保存在每个连接状态的pendingAreq 中获取查询文本，然后使用它报告查询。
 */
static bool
pgfdw_cancel_query(PGconn *conn)
{
	TimestampTz now = GetCurrentTimestamp();
	TimestampTz endtime;
	TimestampTz retrycanceltime;

	/*
	 * If it takes too long to cancel the query and discard the result, assume
	 * the connection is dead.
	 *
	 * 如果取消查询并丢弃结果花费的时间太长，则假定连接已断开。
	 */
	endtime = TimestampTzPlusMilliseconds(now, CONNECTION_CLEANUP_TIMEOUT);

	/*
	 * Also, lose patience and re-issue the cancel request after a little bit.
	 * (This serves to close some race conditions.)
	 *
	 * 另外，请失去耐心，稍后重新发出取消请求。 （这用于关闭一些竞争条件。）
	 */
	retrycanceltime = TimestampTzPlusMilliseconds(now, RETRY_CANCEL_TIMEOUT);

	if (!pgfdw_cancel_query_begin(conn, endtime))
		return false;
	return pgfdw_cancel_query_end(conn, endtime, retrycanceltime, false);
}

/*
 * Submit a cancel request to the given connection, waiting only until
 * the given time.
 *
 * 向给定连接提交取消请求，仅等待给定时间。
 *
 * We sleep interruptibly until we receive confirmation that the cancel
 * request has been accepted, and if it is, return true; if the timeout
 * lapses without that, or the request fails for whatever reason, return
 * false.
 *
 * 我们会中断睡眠，直到收到取消请求已被接受的确认，如果是，则返回 true；如果没有超时，或者请求由于某种原因失败，则返回 false。
 */
static bool
pgfdw_cancel_query_begin(PGconn *conn, TimestampTz endtime)
{
	const char *errormsg = libpqsrv_cancel(conn, endtime);

	if (errormsg != NULL)
		ereport(WARNING,
				errcode(ERRCODE_CONNECTION_FAILURE),
				errmsg("could not send cancel request: %s", errormsg));

	return errormsg == NULL;
}

static bool
pgfdw_cancel_query_end(PGconn *conn, TimestampTz endtime,
					   TimestampTz retrycanceltime, bool consume_input)
{
	PGresult   *result;
	bool		timed_out;

	/*
	 * If requested, consume whatever data is available from the socket. (Note
	 * that if all data is available, this allows pgfdw_get_cleanup_result to
	 * call PQgetResult without forcing the overhead of WaitLatchOrSocket,
	 * which would be large compared to the overhead of PQconsumeInput.)
	 *
	 * 如果有请求，则使用套接字中可用的任何数据。 （请注意，如果所有数据均可用，则这允许 pgfdw_get_cleanup_result 调用 PQgetResult，而无需强制使用 WaitLatchOrSocket 的开销，与 PQconsumeInput 的开销相比，该开销会很大。）
	 */
	if (consume_input && !PQconsumeInput(conn))
	{
		ereport(WARNING,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not get result of cancel request: %s",
						pchomp(PQerrorMessage(conn)))));
		return false;
	}

	/* Get and discard the result of the query.
	 *
	 * 获取并丢弃查询结果。
	 */
	if (pgfdw_get_cleanup_result(conn, endtime, retrycanceltime,
								 &result, &timed_out))
	{
		if (timed_out)
			ereport(WARNING,
					(errmsg("could not get result of cancel request due to timeout")));
		else
			ereport(WARNING,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("could not get result of cancel request: %s",
							pchomp(PQerrorMessage(conn)))));

		return false;
	}
	PQclear(result);

	return true;
}

/*
 * Submit a query during (sub)abort cleanup and wait up to 30 seconds for the
 * result.  If the query is executed without error, the return value is true.
 * If the query is executed successfully but returns an error, the return
 * value is true if and only if ignore_errors is set.  If the query can't be
 * sent or times out, the return value is false.
 *
 * 在（子）中止清理期间提交查询并等待最多 30 秒以获得结果。  如果查询执行没有错误，则返回值为true。如果查询执行成功但返回错误，当且仅当设置了ignore_errors时，返回值为true。  如果查询无法发送或超时，则返回值为 false。
 *
 * It's not a huge problem if we throw an ERROR here, but if we get into error
 * recursion trouble, we'll end up slamming the connection shut, which will
 * necessitate failing the entire toplevel transaction even if subtransactions
 * were used.  Try to use WARNING where we can.
 *
 * 如果我们在这里抛出一个错误，这并不是一个大问题，但是如果我们遇到错误递归问题，我们最终将关闭连接，这将导致整个顶级事务失败，即使使用了子事务。  尽可能使用警告。
 */
static bool
pgfdw_exec_cleanup_query(PGconn *conn, const char *query, bool ignore_errors)
{
	TimestampTz endtime;

	/*
	 * If it takes too long to execute a cleanup query, assume the connection
	 * is dead.  It's fairly likely that this is why we aborted in the first
	 * place (e.g. statement timeout, user cancel), so the timeout shouldn't
	 * be too long.
	 *
	 * 如果执行清理查询花费的时间太长，则假定连接已失效。  这很可能就是我们首先中止的原因（例如语句超时、用户取消），因此超时不应该太长。
	 */
	endtime = TimestampTzPlusMilliseconds(GetCurrentTimestamp(),
										  CONNECTION_CLEANUP_TIMEOUT);

	if (!pgfdw_exec_cleanup_query_begin(conn, query))
		return false;
	return pgfdw_exec_cleanup_query_end(conn, query, endtime,
										false, ignore_errors);
}

static bool
pgfdw_exec_cleanup_query_begin(PGconn *conn, const char *query)
{
	Assert(query != NULL);

	/*
	 * Submit a query.  Since we don't use non-blocking mode, this also can
	 * block.  But its risk is relatively small, so we ignore that for now.
	 *
	 * 提交查询。  由于我们不使用非阻塞模式，因此这也可能会阻塞。  但它的风险相对较小，所以我们暂时忽略它。
	 */
	if (!PQsendQuery(conn, query))
	{
		pgfdw_report_error(WARNING, NULL, conn, false, query);
		return false;
	}

	return true;
}

static bool
pgfdw_exec_cleanup_query_end(PGconn *conn, const char *query,
							 TimestampTz endtime, bool consume_input,
							 bool ignore_errors)
{
	PGresult   *result;
	bool		timed_out;

	Assert(query != NULL);

	/*
	 * If requested, consume whatever data is available from the socket. (Note
	 * that if all data is available, this allows pgfdw_get_cleanup_result to
	 * call PQgetResult without forcing the overhead of WaitLatchOrSocket,
	 * which would be large compared to the overhead of PQconsumeInput.)
	 *
	 * 如果有请求，则使用套接字中可用的任何数据。 （请注意，如果所有数据均可用，则这允许 pgfdw_get_cleanup_result 调用 PQgetResult，而无需强制使用 WaitLatchOrSocket 的开销，与 PQconsumeInput 的开销相比，该开销会很大。）
	 */
	if (consume_input && !PQconsumeInput(conn))
	{
		pgfdw_report_error(WARNING, NULL, conn, false, query);
		return false;
	}

	/* Get the result of the query.
	 *
	 * 获取查询结果。
	 */
	if (pgfdw_get_cleanup_result(conn, endtime, endtime, &result, &timed_out))
	{
		if (timed_out)
			ereport(WARNING,
					(errmsg("could not get query result due to timeout"),
					 errcontext("remote SQL command: %s", query)));
		else
			pgfdw_report_error(WARNING, NULL, conn, false, query);

		return false;
	}

	/* Issue a warning if not successful.
	 *
	 * 如果不成功则发出警告。
	 */
	if (PQresultStatus(result) != PGRES_COMMAND_OK)
	{
		pgfdw_report_error(WARNING, result, conn, true, query);
		return ignore_errors;
	}
	PQclear(result);

	return true;
}

/*
 * Get, during abort cleanup, the result of a query that is in progress.
 * This might be a query that is being interrupted by a cancel request or by
 * transaction abort, or it might be a query that was initiated as part of
 * transaction abort to get the remote side back to the appropriate state.
 *
 * 在中止清理期间获取正在进行的查询的结果。这可能是被取消请求或事务中止中断的查询，也可能是作为事务中止的一部分而启动的查询，以使远程端返回到适当的状态。
 *
 * endtime is the time at which we should give up and assume the remote side
 * is dead.  retrycanceltime is the time at which we should issue a fresh
 * cancel request (pass the same value as endtime if this is not wanted).
 *
 * endtime 是我们应该放弃并假设远程端已死亡的时间。  retrycanceltime 是我们应该发出新的取消请求的时间（如果不需要，则传递与 endtime 相同的值）。
 *
 * Returns true if the timeout expired or connection trouble occurred,
 * false otherwise.  Sets *result except in case of a true result.
 * Sets *timed_out to true only when the timeout expired.
 *
 * 如果超时或发生连接问题，则返回 true，否则返回 false。  设置*结果，除非结果为真。仅当超时到期时才将 *timed_out 设置为 true。
 */
static bool
pgfdw_get_cleanup_result(PGconn *conn, TimestampTz endtime,
						 TimestampTz retrycanceltime,
						 PGresult **result,
						 bool *timed_out)
{
	volatile bool failed = false;
	PGresult   *volatile last_res = NULL;

	*result = NULL;
	*timed_out = false;

	/* In what follows, do not leak any PGresults on an error.
	 *
	 * 在接下来的内容中，不要因错误而泄漏任何 PGresults。
	 */
	PG_TRY();
	{
		int			canceldelta = RETRY_CANCEL_TIMEOUT * 2;

		for (;;)
		{
			PGresult   *res;

			while (PQisBusy(conn))
			{
				int			wc;
				TimestampTz now = GetCurrentTimestamp();
				long		cur_timeout;

				/* If timeout has expired, give up.
				 *
				 * 如果超时已过，则放弃。
				 */
				if (now >= endtime)
				{
					*timed_out = true;
					failed = true;
					goto exit;
				}

				/* If we need to re-issue the cancel request, do that.
				 *
				 * 如果我们需要重新发出取消请求，请这样做。
				 */
				if (now >= retrycanceltime)
				{
					/* We ignore failure to issue the repeated request.
					 *
					 * 我们忽略未能发出重复请求的情况。
					 */
					(void) libpqsrv_cancel(conn, endtime);

					/* Recompute "now" in case that took measurable time.
					 *
					 * 重新计算“现在”，以防花费了可测量的时间。
					 */
					now = GetCurrentTimestamp();

					/* Adjust re-cancel timeout in increasing steps.
					 *
					 * 以递增的步骤调整重新取消超时。
					 */
					retrycanceltime = TimestampTzPlusMilliseconds(now,
																  canceldelta);
					canceldelta += canceldelta;
				}

				/* If timeout has expired, give up, else get sleep time.
				 *
				 * 如果超时已过，则放弃，否则获得睡眠时间。
				 */
				cur_timeout = TimestampDifferenceMilliseconds(now,
															  Min(endtime,
																  retrycanceltime));
				if (cur_timeout <= 0)
				{
					*timed_out = true;
					failed = true;
					goto exit;
				}

				/* first time, allocate or get the custom wait event
				 *
				 * 第一次，分配或获取自定义等待事件
				 */
				if (pgfdw_we_cleanup_result == 0)
					pgfdw_we_cleanup_result = WaitEventExtensionNew("PostgresFdwCleanupResult");

				/* Sleep until there's something to do
				 *
				 * 睡觉直到有事可做
				 */
				wc = WaitLatchOrSocket(MyLatch,
									   WL_LATCH_SET | WL_SOCKET_READABLE |
									   WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
									   PQsocket(conn),
									   cur_timeout, pgfdw_we_cleanup_result);
				ResetLatch(MyLatch);

				CHECK_FOR_INTERRUPTS();

				/* Data available in socket?
				 *
				 * 套接字中的数据可用吗？
				 */
				if (wc & WL_SOCKET_READABLE)
				{
					if (!PQconsumeInput(conn))
					{
						/* connection trouble
						 *
						 * 连接问题
						 */
						failed = true;
						goto exit;
					}
				}
			}

			res = PQgetResult(conn);
			if (res == NULL)
				break;			/* query is complete */

			PQclear(last_res);
			last_res = res;
		}
exit:	;
	}
	PG_CATCH();
	{
		PQclear(last_res);
		PG_RE_THROW();
	}
	PG_END_TRY();

	if (failed)
		PQclear(last_res);
	else
		*result = last_res;
	return failed;
}

/*
 * Abort remote transaction or subtransaction.
 *
 * 中止远程事务或子事务。
 *
 * "toplevel" should be set to true if toplevel (main) transaction is
 * rollbacked, false otherwise.
 *
 * 如果顶级（主）事务回滚，则“toplevel”应设置为 true，否则应设置为 false。
 *
 * Set entry->changing_xact_state to false on success, true on failure.
 *
 * 成功时将entry->changing_xact_state 设置为 false，失败时设置为 true。
 */
static void
pgfdw_abort_cleanup(ConnCacheEntry *entry, bool toplevel)
{
	char		sql[100];

	/*
	 * Don't try to clean up the connection if we're already in error
	 * recursion trouble.
	 *
	 * 如果我们已经遇到错误递归麻烦，请不要尝试清理连接。
	 */
	if (in_error_recursion_trouble())
		entry->changing_xact_state = true;

	/*
	 * If connection is already unsalvageable, don't touch it further.
	 *
	 * 如果连接已经无法挽救，请不要进一步触摸它。
	 */
	if (entry->changing_xact_state)
		return;

	/*
	 * Mark this connection as in the process of changing transaction state.
	 *
	 * 将此连接标记为正在更改事务状态。
	 */
	entry->changing_xact_state = true;

	/* Assume we might have lost track of prepared statements
	 *
	 * 假设我们可能丢失了准备好的语句
	 */
	entry->have_error = true;

	/*
	 * If a command has been submitted to the remote server by using an
	 * asynchronous execution function, the command might not have yet
	 * completed.  Check to see if a command is still being processed by the
	 * remote server, and if so, request cancellation of the command.
	 *
	 * 如果使用异步执行功能将命令提交到远程服务器，则该命令可能尚未完成。  检查远程服务器是否仍在处理命令，如果是，则请求取消该命令。
	 */
	if (PQtransactionStatus(entry->conn) == PQTRANS_ACTIVE &&
		!pgfdw_cancel_query(entry->conn))
		return;					/* Unable to cancel running query */

	CONSTRUCT_ABORT_COMMAND(sql, entry, toplevel);
	if (!pgfdw_exec_cleanup_query(entry->conn, sql, false))
		return;					/* Unable to abort remote (sub)transaction */

	if (toplevel)
	{
		if (entry->have_prep_stmt && entry->have_error &&
			!pgfdw_exec_cleanup_query(entry->conn,
									  "DEALLOCATE ALL",
									  true))
			return;				/* Trouble clearing prepared statements */

		entry->have_prep_stmt = false;
		entry->have_error = false;
	}

	/*
	 * If pendingAreq of the per-connection state is not NULL, it means that
	 * an asynchronous fetch begun by fetch_more_data_begin() was not done
	 * successfully and thus the per-connection state was not reset in
	 * fetch_more_data(); in that case reset the per-connection state here.
	 *
	 * 如果每个连接状态的pendingAreq不为NULL，则意味着由fetch_more_data_begin()开始的异步获取未成功完成，因此每个连接状态未在fetch_more_data()中重置；在这种情况下，请在此处重置每个连接的状态。
	 */
	if (entry->state.pendingAreq)
		memset(&entry->state, 0, sizeof(entry->state));

	/* Disarm changing_xact_state if it all worked
	 *
	 * 如果一切正常，则解除changing_xact_state
	 */
	entry->changing_xact_state = false;
}

/*
 * Like pgfdw_abort_cleanup, submit an abort command or cancel request, but
 * don't wait for the result.
 *
 * 与pgfdw_abort_cleanup类似，提交中止命令或取消请求，但不等待结果。
 *
 * Returns true if the abort command or cancel request is successfully issued,
 * false otherwise.  If the abort command is successfully issued, the given
 * connection cache entry is appended to *pending_entries.  Otherwise, if the
 * cancel request is successfully issued, it is appended to *cancel_requested.
 *
 * 如果成功发出中止命令或取消请求，则返回 true，否则返回 false。  如果成功发出 abort 命令，则给定的连接缓存条目将附加到 *pending_entries。  否则，如果取消请求成功发出，则会将其附加到*cancel_requested。
 */
static bool
pgfdw_abort_cleanup_begin(ConnCacheEntry *entry, bool toplevel,
						  List **pending_entries, List **cancel_requested)
{
	/*
	 * Don't try to clean up the connection if we're already in error
	 * recursion trouble.
	 *
	 * 如果我们已经遇到错误递归麻烦，请不要尝试清理连接。
	 */
	if (in_error_recursion_trouble())
		entry->changing_xact_state = true;

	/*
	 * If connection is already unsalvageable, don't touch it further.
	 *
	 * 如果连接已经无法挽救，请不要进一步触摸它。
	 */
	if (entry->changing_xact_state)
		return false;

	/*
	 * Mark this connection as in the process of changing transaction state.
	 *
	 * 将此连接标记为正在更改事务状态。
	 */
	entry->changing_xact_state = true;

	/* Assume we might have lost track of prepared statements
	 *
	 * 假设我们可能丢失了准备好的语句
	 */
	entry->have_error = true;

	/*
	 * If a command has been submitted to the remote server by using an
	 * asynchronous execution function, the command might not have yet
	 * completed.  Check to see if a command is still being processed by the
	 * remote server, and if so, request cancellation of the command.
	 *
	 * 如果使用异步执行功能将命令提交到远程服务器，则该命令可能尚未完成。  检查远程服务器是否仍在处理命令，如果是，则请求取消该命令。
	 */
	if (PQtransactionStatus(entry->conn) == PQTRANS_ACTIVE)
	{
		TimestampTz endtime;

		endtime = TimestampTzPlusMilliseconds(GetCurrentTimestamp(),
											  CONNECTION_CLEANUP_TIMEOUT);
		if (!pgfdw_cancel_query_begin(entry->conn, endtime))
			return false;		/* Unable to cancel running query */
		*cancel_requested = lappend(*cancel_requested, entry);
	}
	else
	{
		char		sql[100];

		CONSTRUCT_ABORT_COMMAND(sql, entry, toplevel);
		if (!pgfdw_exec_cleanup_query_begin(entry->conn, sql))
			return false;		/* Unable to abort remote transaction */
		*pending_entries = lappend(*pending_entries, entry);
	}

	return true;
}

/*
 * Finish pre-commit cleanup of connections on each of which we've sent a
 * COMMIT command to the remote server.
 *
 * 完成我们已向远程服务器发送 COMMIT 命令的每个连接的预提交清理。
 */
static void
pgfdw_finish_pre_commit_cleanup(List *pending_entries)
{
	ConnCacheEntry *entry;
	List	   *pending_deallocs = NIL;
	ListCell   *lc;

	Assert(pending_entries);

	/*
	 * Get the result of the COMMIT command for each of the pending entries
	 *
	 * 获取每个待处理条目的 COMMIT 命令的结果
	 */
	foreach(lc, pending_entries)
	{
		entry = (ConnCacheEntry *) lfirst(lc);

		Assert(entry->changing_xact_state);

		/*
		 * We might already have received the result on the socket, so pass
		 * consume_input=true to try to consume it first
		 *
		 * 我们可能已经在套接字上收到了结果，因此传递consume_input=true来尝试首先使用它
		 */
		do_sql_command_end(entry->conn, "COMMIT TRANSACTION", true);
		entry->changing_xact_state = false;

		/* Do a DEALLOCATE ALL in parallel if needed
		 *
		 * 如果需要，并行执行 DEALLOCATE ALL
		 */
		if (entry->have_prep_stmt && entry->have_error)
		{
			/* Ignore errors (see notes in pgfdw_xact_callback)
			 *
			 * 忽略错误（参见 pgfdw_xact_callback 中的注释）
			 */
			if (PQsendQuery(entry->conn, "DEALLOCATE ALL"))
			{
				pending_deallocs = lappend(pending_deallocs, entry);
				continue;
			}
		}
		entry->have_prep_stmt = false;
		entry->have_error = false;

		pgfdw_reset_xact_state(entry, true);
	}

	/* No further work if no pending entries
	 *
	 * 如果没有待处理的条目，则无需进一步工作
	 */
	if (!pending_deallocs)
		return;

	/*
	 * Get the result of the DEALLOCATE command for each of the pending
	 * entries
	 *
	 * 获取每个挂起条目的 DEALLOCATE 命令的结果
	 */
	foreach(lc, pending_deallocs)
	{
		PGresult   *res;

		entry = (ConnCacheEntry *) lfirst(lc);

		/* Ignore errors (see notes in pgfdw_xact_callback)
		 *
		 * 忽略错误（参见 pgfdw_xact_callback 中的注释）
		 */
		while ((res = PQgetResult(entry->conn)) != NULL)
		{
			PQclear(res);
			/* Stop if the connection is lost (else we'll loop infinitely)
			 *
			 * 如果连接丢失则停止（否则我们将无限循环）
			 */
			if (PQstatus(entry->conn) == CONNECTION_BAD)
				break;
		}
		entry->have_prep_stmt = false;
		entry->have_error = false;

		pgfdw_reset_xact_state(entry, true);
	}
}

/*
 * Finish pre-subcommit cleanup of connections on each of which we've sent a
 * RELEASE command to the remote server.
 *
 * 完成对每个连接的预提交清理，我们已向远程服务器发送了 RELEASE 命令。
 */
static void
pgfdw_finish_pre_subcommit_cleanup(List *pending_entries, int curlevel)
{
	ConnCacheEntry *entry;
	char		sql[100];
	ListCell   *lc;

	Assert(pending_entries);

	/*
	 * Get the result of the RELEASE command for each of the pending entries
	 *
	 * 获取每个挂起条目的 RELEASE 命令的结果
	 */
	snprintf(sql, sizeof(sql), "RELEASE SAVEPOINT s%d", curlevel);
	foreach(lc, pending_entries)
	{
		entry = (ConnCacheEntry *) lfirst(lc);

		Assert(entry->changing_xact_state);

		/*
		 * We might already have received the result on the socket, so pass
		 * consume_input=true to try to consume it first
		 *
		 * 我们可能已经在套接字上收到了结果，因此传递consume_input=true来尝试首先使用它
		 */
		do_sql_command_end(entry->conn, sql, true);
		entry->changing_xact_state = false;

		pgfdw_reset_xact_state(entry, false);
	}
}

/*
 * Finish abort cleanup of connections on each of which we've sent an abort
 * command or cancel request to the remote server.
 *
 * 完成对每个已向远程服务器发送中止命令或取消请求的连接的中止清理。
 */
static void
pgfdw_finish_abort_cleanup(List *pending_entries, List *cancel_requested,
						   bool toplevel)
{
	List	   *pending_deallocs = NIL;
	ListCell   *lc;

	/*
	 * For each of the pending cancel requests (if any), get and discard the
	 * result of the query, and submit an abort command to the remote server.
	 *
	 * 对于每个挂起的取消请求（如果有），获取并丢弃查询结果，并向远程服务器提交中止命令。
	 */
	if (cancel_requested)
	{
		foreach(lc, cancel_requested)
		{
			ConnCacheEntry *entry = (ConnCacheEntry *) lfirst(lc);
			TimestampTz now = GetCurrentTimestamp();
			TimestampTz endtime;
			TimestampTz retrycanceltime;
			char		sql[100];

			Assert(entry->changing_xact_state);

			/*
			 * Set end time.  You might think we should do this before issuing
			 * cancel request like in normal mode, but that is problematic,
			 * because if, for example, it took longer than 30 seconds to
			 * process the first few entries in the cancel_requested list, it
			 * would cause a timeout error when processing each of the
			 * remaining entries in the list, leading to slamming that entry's
			 * connection shut.
			 *
			 * 设置结束时间。  您可能认为我们应该像在正常模式下那样在发出取消请求之前执行此操作，但这是有问题的，因为如果处理 cancel_requested 列表中的前几个条目花费的时间超过 30 秒，那么在处理列表中的每个剩余条目时会导致超时错误，从而导致该条目的连接关闭。
			 */
			endtime = TimestampTzPlusMilliseconds(now,
												  CONNECTION_CLEANUP_TIMEOUT);
			retrycanceltime = TimestampTzPlusMilliseconds(now,
														  RETRY_CANCEL_TIMEOUT);

			if (!pgfdw_cancel_query_end(entry->conn, endtime,
										retrycanceltime, true))
			{
				/* Unable to cancel running query
				 *
				 * 无法取消正在运行的查询
				 */
				pgfdw_reset_xact_state(entry, toplevel);
				continue;
			}

			/* Send an abort command in parallel if needed
			 *
			 * 如果需要，并行发送中止命令
			 */
			CONSTRUCT_ABORT_COMMAND(sql, entry, toplevel);
			if (!pgfdw_exec_cleanup_query_begin(entry->conn, sql))
			{
				/* Unable to abort remote (sub)transaction
				 *
				 * 无法中止远程（子）事务
				 */
				pgfdw_reset_xact_state(entry, toplevel);
			}
			else
				pending_entries = lappend(pending_entries, entry);
		}
	}

	/* No further work if no pending entries
	 *
	 * 如果没有待处理的条目，则无需进一步工作
	 */
	if (!pending_entries)
		return;

	/*
	 * Get the result of the abort command for each of the pending entries
	 *
	 * 获取每个待处理条目的中止命令的结果
	 */
	foreach(lc, pending_entries)
	{
		ConnCacheEntry *entry = (ConnCacheEntry *) lfirst(lc);
		TimestampTz endtime;
		char		sql[100];

		Assert(entry->changing_xact_state);

		/*
		 * Set end time.  We do this now, not before issuing the command like
		 * in normal mode, for the same reason as for the cancel_requested
		 * entries.
		 *
		 * 设置结束时间。  我们现在这样做，而不是像在正常模式下那样发出命令之前，出于与 cancel_requested 条目相同的原因。
		 */
		endtime = TimestampTzPlusMilliseconds(GetCurrentTimestamp(),
											  CONNECTION_CLEANUP_TIMEOUT);

		CONSTRUCT_ABORT_COMMAND(sql, entry, toplevel);
		if (!pgfdw_exec_cleanup_query_end(entry->conn, sql, endtime,
										  true, false))
		{
			/* Unable to abort remote (sub)transaction
			 *
			 * 无法中止远程（子）事务
			 */
			pgfdw_reset_xact_state(entry, toplevel);
			continue;
		}

		if (toplevel)
		{
			/* Do a DEALLOCATE ALL in parallel if needed
			 *
			 * 如果需要，并行执行 DEALLOCATE ALL
			 */
			if (entry->have_prep_stmt && entry->have_error)
			{
				if (!pgfdw_exec_cleanup_query_begin(entry->conn,
													"DEALLOCATE ALL"))
				{
					/* Trouble clearing prepared statements
					 *
					 * 清除准备好的语句时遇到问题
					 */
					pgfdw_reset_xact_state(entry, toplevel);
				}
				else
					pending_deallocs = lappend(pending_deallocs, entry);
				continue;
			}
			entry->have_prep_stmt = false;
			entry->have_error = false;
		}

		/* Reset the per-connection state if needed
		 *
		 * 如果需要，重置每个连接的状态
		 */
		if (entry->state.pendingAreq)
			memset(&entry->state, 0, sizeof(entry->state));

		/* We're done with this entry; unset the changing_xact_state flag
		 *
		 * 我们已经完成了这个条目；取消设置 Changeing_xact_state 标志
		 */
		entry->changing_xact_state = false;
		pgfdw_reset_xact_state(entry, toplevel);
	}

	/* No further work if no pending entries
	 *
	 * 如果没有待处理的条目，则无需进一步工作
	 */
	if (!pending_deallocs)
		return;
	Assert(toplevel);

	/*
	 * Get the result of the DEALLOCATE command for each of the pending
	 * entries
	 *
	 * 获取每个挂起条目的 DEALLOCATE 命令的结果
	 */
	foreach(lc, pending_deallocs)
	{
		ConnCacheEntry *entry = (ConnCacheEntry *) lfirst(lc);
		TimestampTz endtime;

		Assert(entry->changing_xact_state);
		Assert(entry->have_prep_stmt);
		Assert(entry->have_error);

		/*
		 * Set end time.  We do this now, not before issuing the command like
		 * in normal mode, for the same reason as for the cancel_requested
		 * entries.
		 *
		 * 设置结束时间。  我们现在这样做，而不是像在正常模式下那样发出命令之前，出于与 cancel_requested 条目相同的原因。
		 */
		endtime = TimestampTzPlusMilliseconds(GetCurrentTimestamp(),
											  CONNECTION_CLEANUP_TIMEOUT);

		if (!pgfdw_exec_cleanup_query_end(entry->conn, "DEALLOCATE ALL",
										  endtime, true, true))
		{
			/* Trouble clearing prepared statements
			 *
			 * 清除准备好的语句时遇到问题
			 */
			pgfdw_reset_xact_state(entry, toplevel);
			continue;
		}
		entry->have_prep_stmt = false;
		entry->have_error = false;

		/* Reset the per-connection state if needed
		 *
		 * 如果需要，重置每个连接的状态
		 */
		if (entry->state.pendingAreq)
			memset(&entry->state, 0, sizeof(entry->state));

		/* We're done with this entry; unset the changing_xact_state flag
		 *
		 * 我们已经完成了这个条目；取消设置 Changeing_xact_state 标志
		 */
		entry->changing_xact_state = false;
		pgfdw_reset_xact_state(entry, toplevel);
	}
}

/* Number of output arguments (columns) for various API versions
 *
 * 各种 API 版本的输出参数（列）数量
 */
#define POSTGRES_FDW_GET_CONNECTIONS_COLS_V1_1	2
#define POSTGRES_FDW_GET_CONNECTIONS_COLS_V1_2	6
#define POSTGRES_FDW_GET_CONNECTIONS_COLS	6	/* maximum of above */

/*
 * Internal function used by postgres_fdw_get_connections variants.
 *
 * postgres_fdw_get_connections 变体使用的内部函数。
 *
 * For API version 1.1, this function takes no input parameter and
 * returns a set of records with the following values:
 *
 * 对于 API 版本 1.1，此函数不带输入参数并返回一组具有以下值的记录：
 *
 * - server_name - server name of active connection. In case the foreign server
 *   is dropped but still the connection is active, then the server name will
 *   be NULL in output.
 * - valid - true/false representing whether the connection is valid or not.
 *   Note that connections can become invalid in pgfdw_inval_callback.
 *
 * - server_name - 活动连接的服务器名称。如果外部服务器已断开但连接仍然处于活动状态，则输出中的服务器名称将为 NULL。 - valid - true/false 表示连接是否有效。请注意，连接可能在 pgfdw_inval_callback 中变得无效。
 *
 * For API version 1.2 and later, this function takes an input parameter
 * to check a connection status and returns the following
 * additional values along with the four values from version 1.1:
 *
 * 对于 API 版本 1.2 及更高版本，此函数采用输入参数来检查连接状态，并返回以下附加值以及版本 1.1 中的四个值：
 *
 * - user_name - the local user name of the active connection. In case the
 *   user mapping is dropped but the connection is still active, then the
 *   user name will be NULL in the output.
 * - used_in_xact - true if the connection is used in the current transaction.
 * - closed - true if the connection is closed.
 * - remote_backend_pid - process ID of the remote backend, on the foreign
 *   server, handling the connection.
 *
 * - user_name - 活动连接的本地用户名。如果用户映射已删除但连接仍处于活动状态，则输出中的用户名将为 NULL。 -used_in_xact - 如果连接在当前事务中使用，则为 true。 - 已关闭 - 如果连接已关闭，则为 true。 -remote_backend_pid - 外部服务器上处理连接的远程后端的进程 ID。
 *
 * No records are returned when there are no cached connections at all.
 *
 * 当根本没有缓存的连接时，不会返回任何记录。
 */
static void
postgres_fdw_get_connections_internal(FunctionCallInfo fcinfo,
									  enum pgfdwVersion api_version)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	HASH_SEQ_STATUS scan;
	ConnCacheEntry *entry;

	InitMaterializedSRF(fcinfo, 0);

	/* If cache doesn't exist, we return no records
	 *
	 * 如果缓存不存在，我们不返回任何记录
	 */
	if (!ConnectionHash)
		return;

	/* Check we have the expected number of output arguments
	 *
	 * 检查我们是否有预期数量的输出参数
	 */
	switch (rsinfo->setDesc->natts)
	{
		case POSTGRES_FDW_GET_CONNECTIONS_COLS_V1_1:
			if (api_version != PGFDW_V1_1)
				elog(ERROR, "incorrect number of output arguments");
			break;
		case POSTGRES_FDW_GET_CONNECTIONS_COLS_V1_2:
			if (api_version != PGFDW_V1_2)
				elog(ERROR, "incorrect number of output arguments");
			break;
		default:
			elog(ERROR, "incorrect number of output arguments");
	}

	hash_seq_init(&scan, ConnectionHash);
	while ((entry = (ConnCacheEntry *) hash_seq_search(&scan)))
	{
		ForeignServer *server;
		Datum		values[POSTGRES_FDW_GET_CONNECTIONS_COLS] = {0};
		bool		nulls[POSTGRES_FDW_GET_CONNECTIONS_COLS] = {0};
		int			i = 0;

		/* We only look for open remote connections
		 *
		 * 我们只寻找开放的远程连接
		 */
		if (!entry->conn)
			continue;

		server = GetForeignServerExtended(entry->serverid, FSV_MISSING_OK);

		/*
		 * The foreign server may have been dropped in current explicit
		 * transaction. It is not possible to drop the server from another
		 * session when the connection associated with it is in use in the
		 * current transaction, if tried so, the drop query in another session
		 * blocks until the current transaction finishes.
		 *
		 * 外部服务器可能已在当前显式事务中被删除。当当前事务正在使用与其关联的连接时，不可能从另一个会话中删除服务器，如果尝试这样做，另一个会话中的删除查询将阻塞，直到当前事务完成。
		 *
		 * Even though the server is dropped in the current transaction, the
		 * cache can still have associated active connection entry, say we
		 * call such connections dangling. Since we can not fetch the server
		 * name from system catalogs for dangling connections, instead we show
		 * NULL value for server name in output.
		 *
		 * 即使服务器在当前事务中被删除，缓存仍然可以具有关联的活动连接条目，假设我们将此类连接称为悬空。由于我们无法从悬空连接的系统目录中获取服务器名称，因此我们在输出中显示服务器名称的 NULL 值。
		 *
		 * We could have done better by storing the server name in the cache
		 * entry instead of server oid so that it could be used in the output.
		 * But the server name in each cache entry requires 64 bytes of
		 * memory, which is huge, when there are many cached connections and
		 * the use case i.e. dropping the foreign server within the explicit
		 * current transaction seems rare. So, we chose to show NULL value for
		 * server name in output.
		 *
		 * 我们可以通过将服务器名称而不是服务器 oid 存储在缓存条目中来做得更好，以便可以在输出中使用它。但是每个缓存条目中的服务器名称需要 64 字节的内存，当存在许多缓存连接并且在显式当前事务中删除外部服务器的用例似乎很少见时，这会是巨大的。因此，我们选择在输出中显示服务器名称的 NULL 值。
		 *
		 * Such dangling connections get closed either in next use or at the
		 * end of current explicit transaction in pgfdw_xact_callback.
		 *
		 * 此类悬空连接会在下次使用时或在 pgfdw_xact_callback 中的当前显式事务结束时关闭。
		 */
		if (!server)
		{
			/*
			 * If the server has been dropped in the current explicit
			 * transaction, then this entry would have been invalidated in
			 * pgfdw_inval_callback at the end of drop server command. Note
			 * that this connection would not have been closed in
			 * pgfdw_inval_callback because it is still being used in the
			 * current explicit transaction. So, assert that here.
			 *
			 * 如果服务器已在当前显式事务中删除，则该条目将在 drop server 命令末尾的 pgfdw_inval_callback 中失效。请注意，此连接不会在 pgfdw_inval_callback 中关闭，因为它仍在当前显式事务中使用。所以，在这里断言。
			 */
			Assert(entry->conn && entry->xact_depth > 0 && entry->invalidated);

			/* Show null, if no server name was found
			 *
			 * 如果未找到服务器名称，则显示 null
			 */
			nulls[i++] = true;
		}
		else
			values[i++] = CStringGetTextDatum(server->servername);

		if (api_version >= PGFDW_V1_2)
		{
			HeapTuple	tp;

			/* Use the system cache to obtain the user mapping
			 *
			 * 使用系统缓存获取用户映射
			 */
			tp = SearchSysCache1(USERMAPPINGOID, ObjectIdGetDatum(entry->key));

			/*
			 * Just like in the foreign server case, user mappings can also be
			 * dropped in the current explicit transaction. Therefore, the
			 * similar check as in the server case is required.
			 *
			 * 就像外部服务器的情况一样，用户映射也可以在当前显式事务中删除。因此，需要进行与服务器情况类似的检查。
			 */
			if (!HeapTupleIsValid(tp))
			{
				/*
				 * If we reach here, this entry must have been invalidated in
				 * pgfdw_inval_callback, same as in the server case.
				 *
				 * 如果我们到达这里，该条目一定已在 pgfdw_inval_callback 中失效，与服务器情况相同。
				 */
				Assert(entry->conn && entry->xact_depth > 0 &&
					   entry->invalidated);

				nulls[i++] = true;
			}
			else
			{
				Oid			userid;

				userid = ((Form_pg_user_mapping) GETSTRUCT(tp))->umuser;
				values[i++] = CStringGetTextDatum(MappingUserName(userid));
				ReleaseSysCache(tp);
			}
		}

		values[i++] = BoolGetDatum(!entry->invalidated);

		if (api_version >= PGFDW_V1_2)
		{
			bool		check_conn = PG_GETARG_BOOL(0);

			/* Is this connection used in the current transaction?
			 *
			 * 当前事务中是否使用此连接？
			 */
			values[i++] = BoolGetDatum(entry->xact_depth > 0);

			/*
			 * If a connection status check is requested and supported, return
			 * whether the connection is closed. Otherwise, return NULL.
			 *
			 * 如果请求并支持连接状态检查，则返回连接是否关闭。否则，返回 NULL。
			 */
			if (check_conn && pgfdw_conn_checkable())
				values[i++] = BoolGetDatum(pgfdw_conn_check(entry->conn) != 0);
			else
				nulls[i++] = true;

			/* Return process ID of remote backend
			 *
			 * 返回远程后端的进程ID
			 */
			values[i++] = Int32GetDatum(PQbackendPID(entry->conn));
		}

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
}

/*
 * List active foreign server connections.
 *
 * 列出活动的外部服务器连接。
 *
 * The SQL API of this function has changed multiple times, and will likely
 * do so again in future.  To support the case where a newer version of this
 * loadable module is being used with an old SQL declaration of the function,
 * we continue to support the older API versions.
 *
 * 此函数的 SQL API 已更改多次，并且将来可能会再次更改。  为了支持此可加载模块的较新版本与旧的 SQL 函数声明一起使用的情况，我们继续支持旧的 API 版本。
 */
Datum
postgres_fdw_get_connections_1_2(PG_FUNCTION_ARGS)
{
	postgres_fdw_get_connections_internal(fcinfo, PGFDW_V1_2);

	PG_RETURN_VOID();
}

Datum
postgres_fdw_get_connections(PG_FUNCTION_ARGS)
{
	postgres_fdw_get_connections_internal(fcinfo, PGFDW_V1_1);

	PG_RETURN_VOID();
}

/*
 * Disconnect the specified cached connections.
 *
 * 断开指定的缓存连接。
 *
 * This function discards the open connections that are established by
 * postgres_fdw from the local session to the foreign server with
 * the given name. Note that there can be multiple connections to
 * the given server using different user mappings. If the connections
 * are used in the current local transaction, they are not disconnected
 * and warning messages are reported. This function returns true
 * if it disconnects at least one connection, otherwise false. If no
 * foreign server with the given name is found, an error is reported.
 *
 * 此函数丢弃由 postgres_fdw 从本地会话到具有给定名称的外部服务器建立的打开连接。请注意，使用不同的用户映射可以有多个到给定服务器的连接。如果当前本地事务中使用了连接，则不会断开连接并报告警告消息。如果该函数断开至少一个连接，则返回 true，否则返回 false。如果没有找到具有给定名称的外部服务器，则会报告错误。
 */
Datum
postgres_fdw_disconnect(PG_FUNCTION_ARGS)
{
	ForeignServer *server;
	char	   *servername;

	servername = text_to_cstring(PG_GETARG_TEXT_PP(0));
	server = GetForeignServerByName(servername, false);

	PG_RETURN_BOOL(disconnect_cached_connections(server->serverid));
}

/*
 * Disconnect all the cached connections.
 *
 * 断开所有缓存的连接。
 *
 * This function discards all the open connections that are established by
 * postgres_fdw from the local session to the foreign servers.
 * If the connections are used in the current local transaction, they are
 * not disconnected and warning messages are reported. This function
 * returns true if it disconnects at least one connection, otherwise false.
 *
 * 此函数丢弃 postgres_fdw 从本地会话到外部服务器建立的所有打开连接。如果当前本地事务中使用了连接，则不会断开连接并报告警告消息。如果该函数断开至少一个连接，则返回 true，否则返回 false。
 */
Datum
postgres_fdw_disconnect_all(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(disconnect_cached_connections(InvalidOid));
}

/*
 * Workhorse to disconnect cached connections.
 *
 * 断开缓存连接的主力。
 *
 * This function scans all the connection cache entries and disconnects
 * the open connections whose foreign server OID matches with
 * the specified one. If InvalidOid is specified, it disconnects all
 * the cached connections.
 *
 * 该函数扫描所有连接缓存条目，并断开与指定外部服务器 OID 匹配的打开连接。如果指定了 InvalidOid，则会断开所有缓存的连接。
 *
 * This function emits a warning for each connection that's used in
 * the current transaction and doesn't close it. It returns true if
 * it disconnects at least one connection, otherwise false.
 *
 * 此函数会对当前事务中使用的每个连接发出警告，并且不会关闭它。如果断开至少一个连接，则返回 true，否则返回 false。
 *
 * Note that this function disconnects even the connections that are
 * established by other users in the same local session using different
 * user mappings. This leads even non-superuser to be able to close
 * the connections established by superusers in the same local session.
 *
 * 请注意，此功能甚至会断开同一本地会话中其他用户使用不同用户映射建立的连接。这甚至导致非超级用户也能够关闭超级用户在同一本地会话中建立的连接。
 *
 * XXX As of now we don't see any security risk doing this. But we should
 * set some restrictions on that, for example, prevent non-superuser
 * from closing the connections established by superusers even
 * in the same session?
 *
 * XXX 截至目前，我们没有发现这样做有任何安全风险。但我们应该对此设置一些限制，例如，防止非超级用户关闭超级用户建立的连接，即使是在同一个会话中？
 */
static bool
disconnect_cached_connections(Oid serverid)
{
	HASH_SEQ_STATUS scan;
	ConnCacheEntry *entry;
	bool		all = !OidIsValid(serverid);
	bool		result = false;

	/*
	 * Connection cache hashtable has not been initialized yet in this
	 * session, so return false.
	 *
	 * 此会话中连接缓存哈希表尚未初始化，因此返回 false。
	 */
	if (!ConnectionHash)
		return false;

	hash_seq_init(&scan, ConnectionHash);
	while ((entry = (ConnCacheEntry *) hash_seq_search(&scan)))
	{
		/* Ignore cache entry if no open connection right now.
		 *
		 * 如果现在没有打开的连接，则忽略缓存条目。
		 */
		if (!entry->conn)
			continue;

		if (all || entry->serverid == serverid)
		{
			/*
			 * Emit a warning because the connection to close is used in the
			 * current transaction and cannot be disconnected right now.
			 *
			 * 发出警告，因为要关闭的连接已在当前事务中使用，现在无法断开连接。
			 */
			if (entry->xact_depth > 0)
			{
				ForeignServer *server;

				server = GetForeignServerExtended(entry->serverid,
												  FSV_MISSING_OK);

				if (!server)
				{
					/*
					 * If the foreign server was dropped while its connection
					 * was used in the current transaction, the connection
					 * must have been marked as invalid by
					 * pgfdw_inval_callback at the end of DROP SERVER command.
					 *
					 * 如果外部服务器在当前事务中使用其连接时被删除，则该连接必须在 DROP SERVER 命令末尾被 pgfdw_inval_callback 标记为无效。
					 */
					Assert(entry->invalidated);

					ereport(WARNING,
							(errmsg("cannot close dropped server connection because it is still in use")));
				}
				else
					ereport(WARNING,
							(errmsg("cannot close connection for server \"%s\" because it is still in use",
									server->servername)));
			}
			else
			{
				elog(DEBUG3, "discarding connection %p", entry->conn);
				disconnect_pg_server(entry);
				result = true;
			}
		}
	}

	return result;
}

/*
 * Check if the remote server closed the connection.
 *
 * 检查远程服务器是否关闭了连接。
 *
 * Returns 1 if the connection is closed, -1 if an error occurred,
 * and 0 if it's not closed or if the connection check is unavailable
 * on this platform.
 *
 * 如果连接已关闭，则返回 1；如果发生错误，则返回 -1；如果未关闭或连接检查在此平台上不可用，则返回 0。
 */
static int
pgfdw_conn_check(PGconn *conn)
{
	int			sock = PQsocket(conn);

	if (PQstatus(conn) != CONNECTION_OK || sock == -1)
		return -1;

#if (defined(HAVE_POLL) && defined(POLLRDHUP))
	{
		struct pollfd input_fd;
		int			result;

		input_fd.fd = sock;
		input_fd.events = POLLRDHUP;
		input_fd.revents = 0;

		do
			result = poll(&input_fd, 1, 0);
		while (result < 0 && errno == EINTR);

		if (result < 0)
			return -1;

		return (input_fd.revents &
				(POLLRDHUP | POLLHUP | POLLERR | POLLNVAL)) ? 1 : 0;
	}
#else
	return 0;
#endif
}

/*
 * Check if connection status checking is available on this platform.
 *
 * 检查该平台是否可以进行连接状态检查。
 *
 * Returns true if available, false otherwise.
 *
 * 如果可用则返回 true，否则返回 false。
 */
static bool
pgfdw_conn_checkable(void)
{
#if (defined(HAVE_POLL) && defined(POLLRDHUP))
	return true;
#else
	return false;
#endif
}

/*
 * Ensure that require_auth and SCRAM keys are correctly set on values. SCRAM
 * keys used to pass-through are coming from the initial connection from the
 * client with the server.
 *
 * 确保 require_auth 和 SCRAM 密钥的值设置正确。用于传递的 SCRAM 密钥来自客户端与服务器的初始连接。
 *
 * All required SCRAM options are set by postgres_fdw, so we just need to
 * ensure that these options are not overwritten by the user.
 *
 * 所有必需的 SCRAM 选项均由 postgres_fdw 设置，因此我们只需确保这些选项不被用户覆盖即可。
 */
static bool
pgfdw_has_required_scram_options(const char **keywords, const char **values)
{
	bool		has_scram_server_key = false;
	bool		has_scram_client_key = false;
	bool		has_require_auth = false;
	bool		has_scram_keys = false;

	/*
	 * Continue iterating even if we found the keys that we need to validate
	 * to make sure that there is no other declaration of these keys that can
	 * overwrite the first.
	 *
	 * 即使我们找到了需要验证的键，也要继续迭代，以确保这些键没有其他声明可以覆盖第一个。
	 */
	for (int i = 0; keywords[i] != NULL; i++)
	{
		if (strcmp(keywords[i], "scram_client_key") == 0)
		{
			if (values[i] != NULL && values[i][0] != '\0')
				has_scram_client_key = true;
			else
				has_scram_client_key = false;
		}

		if (strcmp(keywords[i], "scram_server_key") == 0)
		{
			if (values[i] != NULL && values[i][0] != '\0')
				has_scram_server_key = true;
			else
				has_scram_server_key = false;
		}

		if (strcmp(keywords[i], "require_auth") == 0)
		{
			if (values[i] != NULL && strcmp(values[i], "scram-sha-256") == 0)
				has_require_auth = true;
			else
				has_require_auth = false;
		}
	}

	has_scram_keys = has_scram_client_key && has_scram_server_key && MyProcPort != NULL && MyProcPort->has_scram_keys;

	return (has_scram_keys && has_require_auth);
}

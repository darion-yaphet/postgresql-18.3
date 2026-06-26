/*
 * dblink.c
 *
 * Functions returning results from a remote database
 *
 * Joe Conway <mail@joeconway.com>
 * And contributors:
 * Darko Prenosil <Darko.Prenosil@finteh.hr>
 * Shridhar Daithankar <shridhar_daithankar@persistent.co.in>
 *
 * contrib/dblink/dblink.c
 * Copyright (c) 2001-2025, PostgreSQL Global Development Group
 * ALL RIGHTS RESERVED;
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation for any purpose, without fee, and without a written agreement
 * is hereby granted, provided that the above copyright notice and this
 * paragraph and the following two paragraphs appear in all copies.
 *
 * IN NO EVENT SHALL THE AUTHOR OR DISTRIBUTORS BE LIABLE TO ANY PARTY FOR
 * DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING
 * LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
 * DOCUMENTATION, EVEN IF THE AUTHOR OR DISTRIBUTORS HAVE BEEN ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * THE AUTHOR AND DISTRIBUTORS SPECIFICALLY DISCLAIMS ANY WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
 * ON AN "AS IS" BASIS, AND THE AUTHOR AND DISTRIBUTORS HAS NO OBLIGATIONS TO
 * PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 *
 */
#include "postgres.h"

#include <limits.h>

#include "access/htup_details.h"
#include "access/relation.h"
#include "access/reloptions.h"
#include "access/table.h"
#include "catalog/namespace.h"
#include "catalog/pg_foreign_data_wrapper.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_type.h"
#include "catalog/pg_user_mapping.h"
#include "commands/defrem.h"
#include "common/base64.h"
#include "executor/spi.h"
#include "foreign/foreign.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "libpq-fe.h"
#include "libpq/libpq-be.h"
#include "libpq/libpq-be-fe-helpers.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "parser/scansup.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/varlena.h"
#include "utils/wait_event.h"

PG_MODULE_MAGIC_EXT(
					.name = "dblink",
					.version = PG_VERSION
);

typedef struct remoteConn
{
	PGconn	   *conn;			/* Hold the remote connection */
	int			openCursorCount;	/* The number of open cursors */
	bool		newXactForCursor;	/* Opened a transaction for a cursor */
} remoteConn;

typedef struct storeInfo
{
	FunctionCallInfo fcinfo;
	Tuplestorestate *tuplestore;
	AttInMetadata *attinmeta;
	MemoryContext tmpcontext;
	char	  **cstrs;
	/* temp storage for results to avoid leaks on exception
	 *
	 * 结果的临时存储以避免异常泄漏
	 */
	PGresult   *last_res;
	PGresult   *cur_res;
} storeInfo;

/*
 * Internal declarations
 *
 * 内部声明
 */
static Datum dblink_record_internal(FunctionCallInfo fcinfo, bool is_async);
static void prepTuplestoreResult(FunctionCallInfo fcinfo);
static void materializeResult(FunctionCallInfo fcinfo, PGconn *conn,
							  PGresult *res);
static void materializeQueryResult(FunctionCallInfo fcinfo,
								   PGconn *conn,
								   const char *conname,
								   const char *sql,
								   bool fail);
static PGresult *storeQueryResult(volatile storeInfo *sinfo, PGconn *conn, const char *sql);
static void storeRow(volatile storeInfo *sinfo, PGresult *res, bool first);
static remoteConn *getConnectionByName(const char *name);
static HTAB *createConnHash(void);
static remoteConn *createNewConnection(const char *name);
static void deleteConnection(const char *name);
static char **get_pkey_attnames(Relation rel, int16 *indnkeyatts);
static char **get_text_array_contents(ArrayType *array, int *numitems);
static char *get_sql_insert(Relation rel, int *pkattnums, int pknumatts, char **src_pkattvals, char **tgt_pkattvals);
static char *get_sql_delete(Relation rel, int *pkattnums, int pknumatts, char **tgt_pkattvals);
static char *get_sql_update(Relation rel, int *pkattnums, int pknumatts, char **src_pkattvals, char **tgt_pkattvals);
static char *quote_ident_cstr(char *rawstr);
static int	get_attnum_pk_pos(int *pkattnums, int pknumatts, int key);
static HeapTuple get_tuple_of_interest(Relation rel, int *pkattnums, int pknumatts, char **src_pkattvals);
static Relation get_rel_from_relname(text *relname_text, LOCKMODE lockmode, AclMode aclmode);
static char *generate_relation_name(Relation rel);
static void dblink_connstr_check(const char *connstr);
static bool dblink_connstr_has_pw(const char *connstr);
static void dblink_security_check(PGconn *conn, const char *connname,
								  const char *connstr);
static void dblink_res_error(PGconn *conn, const char *conname, PGresult *res,
							 bool fail, const char *fmt,...) pg_attribute_printf(5, 6);
static char *get_connect_string(const char *servername);
static char *escape_param_str(const char *str);
static void validate_pkattnums(Relation rel,
							   int2vector *pkattnums_arg, int32 pknumatts_arg,
							   int **pkattnums, int *pknumatts);
static bool is_valid_dblink_option(const PQconninfoOption *options,
								   const char *option, Oid context);
static int	applyRemoteGucs(PGconn *conn);
static void restoreLocalGucs(int nestlevel);
static bool UseScramPassthrough(ForeignServer *foreign_server, UserMapping *user);
static void appendSCRAMKeysInfo(StringInfo buf);
static bool is_valid_dblink_fdw_option(const PQconninfoOption *options, const char *option,
									   Oid context);
static bool dblink_connstr_has_required_scram_options(const char *connstr);

/* Global */
static remoteConn *pconn = NULL;
static HTAB *remoteConnHash = NULL;

/* custom wait event values, retrieved from shared memory
 *
 * 自定义等待事件值，从共享内存中检索
 */
static uint32 dblink_we_connect = 0;
static uint32 dblink_we_get_conn = 0;
static uint32 dblink_we_get_result = 0;

/*
 *	Following is hash that holds multiple remote connections.
 *	Calling convention of each dblink function changes to accept
 *	connection name as the first parameter. The connection hash is
 *	much like ecpg e.g. a mapping between a name and a PGconn object.
 *
 * 以下是保存多个远程连接的哈希。每个 dblink 函数的调用约定都更改为接受连接名称作为第一个参数。连接哈希很像 ecpg，例如名称和 PGconn 对象之间的映射。
 *
 *	To avoid potentially leaking a PGconn object in case of out-of-memory
 *	errors, we first create the hash entry, then open the PGconn.
 *	Hence, a hash entry whose rconn.conn pointer is NULL must be
 *	understood as a leftover from a failed create; it should be ignored
 *	by lookup operations, and silently replaced by create operations.
 *
 * 为了避免在内存不足错误的情况下可能泄漏 PGconn 对象，我们首先创建哈希条目，然后打开 PGconn。因此，rconn.conn 指针为 NULL 的哈希条目必须被理解为创建失败的剩余内容；它应该被查找操作忽略，并被创建操作默默地替换。
 */

typedef struct remoteConnHashEnt
{
	char		name[NAMEDATALEN];
	remoteConn	rconn;
} remoteConnHashEnt;

/* initial number of connection hashes
 *
 * 连接哈希的初始数量
 */
#define NUMCONN 16

static char *
xpstrdup(const char *in)
{
	if (in == NULL)
		return NULL;
	return pstrdup(in);
}

pg_noreturn static void
dblink_res_internalerror(PGconn *conn, PGresult *res, const char *p2)
{
	char	   *msg = pchomp(PQerrorMessage(conn));

	PQclear(res);
	elog(ERROR, "%s: %s", p2, msg);
}

pg_noreturn static void
dblink_conn_not_avail(const char *conname)
{
	if (conname)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_DOES_NOT_EXIST),
				 errmsg("connection \"%s\" not available", conname)));
	else
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_DOES_NOT_EXIST),
				 errmsg("connection not available")));
}

static void
dblink_get_conn(char *conname_or_str,
				PGconn *volatile *conn_p, char **conname_p, volatile bool *freeconn_p)
{
	remoteConn *rconn = getConnectionByName(conname_or_str);
	PGconn	   *conn;
	char	   *conname;
	bool		freeconn;

	if (rconn)
	{
		conn = rconn->conn;
		conname = conname_or_str;
		freeconn = false;
	}
	else
	{
		const char *connstr;

		connstr = get_connect_string(conname_or_str);
		if (connstr == NULL)
			connstr = conname_or_str;
		dblink_connstr_check(connstr);

		/* first time, allocate or get the custom wait event
		 *
		 * 第一次，分配或获取自定义等待事件
		 */
		if (dblink_we_get_conn == 0)
			dblink_we_get_conn = WaitEventExtensionNew("DblinkGetConnect");

		/* OK to make connection
		 *
		 * 确定建立连接
		 */
		conn = libpqsrv_connect(connstr, dblink_we_get_conn);

		if (PQstatus(conn) == CONNECTION_BAD)
		{
			char	   *msg = pchomp(PQerrorMessage(conn));

			libpqsrv_disconnect(conn);
			ereport(ERROR,
					(errcode(ERRCODE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION),
					 errmsg("could not establish connection"),
					 errdetail_internal("%s", msg)));
		}
		dblink_security_check(conn, NULL, connstr);
		if (PQclientEncoding(conn) != GetDatabaseEncoding())
			PQsetClientEncoding(conn, GetDatabaseEncodingName());
		freeconn = true;
		conname = NULL;
	}

	*conn_p = conn;
	*conname_p = conname;
	*freeconn_p = freeconn;
}

static PGconn *
dblink_get_named_conn(const char *conname)
{
	remoteConn *rconn = getConnectionByName(conname);

	if (rconn)
		return rconn->conn;

	dblink_conn_not_avail(conname);
	return NULL;				/* keep compiler quiet */
}

static void
dblink_init(void)
{
	if (!pconn)
	{
		if (dblink_we_get_result == 0)
			dblink_we_get_result = WaitEventExtensionNew("DblinkGetResult");

		pconn = (remoteConn *) MemoryContextAlloc(TopMemoryContext, sizeof(remoteConn));
		pconn->conn = NULL;
		pconn->openCursorCount = 0;
		pconn->newXactForCursor = false;
	}
}

/*
 * Create a persistent connection to another database
 *
 * 创建到另一个数据库的持久连接
 */
PG_FUNCTION_INFO_V1(dblink_connect);
Datum
dblink_connect(PG_FUNCTION_ARGS)
{
	char	   *conname_or_str = NULL;
	char	   *connstr = NULL;
	char	   *connname = NULL;
	char	   *msg;
	PGconn	   *conn = NULL;
	remoteConn *rconn = NULL;

	dblink_init();

	if (PG_NARGS() == 2)
	{
		conname_or_str = text_to_cstring(PG_GETARG_TEXT_PP(1));
		connname = text_to_cstring(PG_GETARG_TEXT_PP(0));
	}
	else if (PG_NARGS() == 1)
		conname_or_str = text_to_cstring(PG_GETARG_TEXT_PP(0));

	/* first check for valid foreign data server
	 *
	 * 首先检查有效的外部数据服务器
	 */
	connstr = get_connect_string(conname_or_str);
	if (connstr == NULL)
		connstr = conname_or_str;

	/* check password in connection string if not superuser
	 *
	 * 检查连接字符串中的密码（如果不是超级用户）
	 */
	dblink_connstr_check(connstr);

	/* first time, allocate or get the custom wait event
	 *
	 * 第一次，分配或获取自定义等待事件
	 */
	if (dblink_we_connect == 0)
		dblink_we_connect = WaitEventExtensionNew("DblinkConnect");

	/* if we need a hashtable entry, make that first, since it might fail
	 *
	 * 如果我们需要一个哈希表条目，请先创建它，因为它可能会失败
	 */
	if (connname)
	{
		rconn = createNewConnection(connname);
		Assert(rconn->conn == NULL);
	}

	/* OK to make connection
	 *
	 * 确定建立连接
	 */
	conn = libpqsrv_connect(connstr, dblink_we_connect);

	if (PQstatus(conn) == CONNECTION_BAD)
	{
		msg = pchomp(PQerrorMessage(conn));
		libpqsrv_disconnect(conn);
		if (connname)
			deleteConnection(connname);

		ereport(ERROR,
				(errcode(ERRCODE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION),
				 errmsg("could not establish connection"),
				 errdetail_internal("%s", msg)));
	}

	/* check password actually used if not superuser
	 *
	 * 检查实际使用的密码（如果不是超级用户）
	 */
	dblink_security_check(conn, connname, connstr);

	/* attempt to set client encoding to match server encoding, if needed
	 *
	 * 如果需要，尝试设置客户端编码以匹配服务器编码
	 */
	if (PQclientEncoding(conn) != GetDatabaseEncoding())
		PQsetClientEncoding(conn, GetDatabaseEncodingName());

	/* all OK, save away the conn
	 *
	 * 一切OK，保存掉conn
	 */
	if (connname)
	{
		rconn->conn = conn;
	}
	else
	{
		if (pconn->conn)
			libpqsrv_disconnect(pconn->conn);
		pconn->conn = conn;
	}

	PG_RETURN_TEXT_P(cstring_to_text("OK"));
}

/*
 * Clear a persistent connection to another database
 *
 * 清除与另一个数据库的持久连接
 */
PG_FUNCTION_INFO_V1(dblink_disconnect);
Datum
dblink_disconnect(PG_FUNCTION_ARGS)
{
	char	   *conname = NULL;
	remoteConn *rconn = NULL;
	PGconn	   *conn = NULL;

	dblink_init();

	if (PG_NARGS() == 1)
	{
		conname = text_to_cstring(PG_GETARG_TEXT_PP(0));
		rconn = getConnectionByName(conname);
		if (rconn)
			conn = rconn->conn;
	}
	else
		conn = pconn->conn;

	if (!conn)
		dblink_conn_not_avail(conname);

	libpqsrv_disconnect(conn);
	if (rconn)
		deleteConnection(conname);
	else
		pconn->conn = NULL;

	PG_RETURN_TEXT_P(cstring_to_text("OK"));
}

/*
 * opens a cursor using a persistent connection
 *
 * 使用持久连接打开游标
 */
PG_FUNCTION_INFO_V1(dblink_open);
Datum
dblink_open(PG_FUNCTION_ARGS)
{
	PGresult   *res = NULL;
	PGconn	   *conn;
	char	   *curname = NULL;
	char	   *sql = NULL;
	char	   *conname = NULL;
	StringInfoData buf;
	remoteConn *rconn = NULL;
	bool		fail = true;	/* default to backward compatible behavior */

	dblink_init();
	initStringInfo(&buf);

	if (PG_NARGS() == 2)
	{
		/* text,text
		 *
		 * 文字，文字
		 */
		curname = text_to_cstring(PG_GETARG_TEXT_PP(0));
		sql = text_to_cstring(PG_GETARG_TEXT_PP(1));
		rconn = pconn;
	}
	else if (PG_NARGS() == 3)
	{
		/* might be text,text,text or text,text,bool
		 *
		 * 可能是文本、文本、文本或文本、文本、布尔值
		 */
		if (get_fn_expr_argtype(fcinfo->flinfo, 2) == BOOLOID)
		{
			curname = text_to_cstring(PG_GETARG_TEXT_PP(0));
			sql = text_to_cstring(PG_GETARG_TEXT_PP(1));
			fail = PG_GETARG_BOOL(2);
			rconn = pconn;
		}
		else
		{
			conname = text_to_cstring(PG_GETARG_TEXT_PP(0));
			curname = text_to_cstring(PG_GETARG_TEXT_PP(1));
			sql = text_to_cstring(PG_GETARG_TEXT_PP(2));
			rconn = getConnectionByName(conname);
		}
	}
	else if (PG_NARGS() == 4)
	{
		/* text,text,text,bool
		 *
		 * 文本，文本，文本，布尔
		 */
		conname = text_to_cstring(PG_GETARG_TEXT_PP(0));
		curname = text_to_cstring(PG_GETARG_TEXT_PP(1));
		sql = text_to_cstring(PG_GETARG_TEXT_PP(2));
		fail = PG_GETARG_BOOL(3);
		rconn = getConnectionByName(conname);
	}

	if (!rconn || !rconn->conn)
		dblink_conn_not_avail(conname);

	conn = rconn->conn;

	/* If we are not in a transaction, start one
	 *
	 * 如果我们没有进行交易，则开始一项交易
	 */
	if (PQtransactionStatus(conn) == PQTRANS_IDLE)
	{
		res = libpqsrv_exec(conn, "BEGIN", dblink_we_get_result);
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
			dblink_res_internalerror(conn, res, "begin error");
		PQclear(res);
		rconn->newXactForCursor = true;

		/*
		 * Since transaction state was IDLE, we force cursor count to
		 * initially be 0. This is needed as a previous ABORT might have wiped
		 * out our transaction without maintaining the cursor count for us.
		 *
		 * 由于事务状态为 IDLE，我们强制游标计数最初为 0。这是必需的，因为之前的 ABORT 可能会清除我们的事务，而不会为我们维护游标计数。
		 */
		rconn->openCursorCount = 0;
	}

	/* if we started a transaction, increment cursor count
	 *
	 * 如果我们开始一个事务，则增加游标计数
	 */
	if (rconn->newXactForCursor)
		(rconn->openCursorCount)++;

	appendStringInfo(&buf, "DECLARE %s CURSOR FOR %s", curname, sql);
	res = libpqsrv_exec(conn, buf.data, dblink_we_get_result);
	if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		dblink_res_error(conn, conname, res, fail,
						 "while opening cursor \"%s\"", curname);
		PG_RETURN_TEXT_P(cstring_to_text("ERROR"));
	}

	PQclear(res);
	PG_RETURN_TEXT_P(cstring_to_text("OK"));
}

/*
 * closes a cursor
 *
 * 关闭光标
 */
PG_FUNCTION_INFO_V1(dblink_close);
Datum
dblink_close(PG_FUNCTION_ARGS)
{
	PGconn	   *conn;
	PGresult   *res = NULL;
	char	   *curname = NULL;
	char	   *conname = NULL;
	StringInfoData buf;
	remoteConn *rconn = NULL;
	bool		fail = true;	/* default to backward compatible behavior */

	dblink_init();
	initStringInfo(&buf);

	if (PG_NARGS() == 1)
	{
		/* text */
		curname = text_to_cstring(PG_GETARG_TEXT_PP(0));
		rconn = pconn;
	}
	else if (PG_NARGS() == 2)
	{
		/* might be text,text or text,bool
		 *
		 * 可能是文本、文本或文本、布尔值
		 */
		if (get_fn_expr_argtype(fcinfo->flinfo, 1) == BOOLOID)
		{
			curname = text_to_cstring(PG_GETARG_TEXT_PP(0));
			fail = PG_GETARG_BOOL(1);
			rconn = pconn;
		}
		else
		{
			conname = text_to_cstring(PG_GETARG_TEXT_PP(0));
			curname = text_to_cstring(PG_GETARG_TEXT_PP(1));
			rconn = getConnectionByName(conname);
		}
	}
	if (PG_NARGS() == 3)
	{
		/* text,text,bool
		 *
		 * 文本,文本,布尔值
		 */
		conname = text_to_cstring(PG_GETARG_TEXT_PP(0));
		curname = text_to_cstring(PG_GETARG_TEXT_PP(1));
		fail = PG_GETARG_BOOL(2);
		rconn = getConnectionByName(conname);
	}

	if (!rconn || !rconn->conn)
		dblink_conn_not_avail(conname);

	conn = rconn->conn;

	appendStringInfo(&buf, "CLOSE %s", curname);

	/* close the cursor
	 *
	 * 关闭光标
	 */
	res = libpqsrv_exec(conn, buf.data, dblink_we_get_result);
	if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		dblink_res_error(conn, conname, res, fail,
						 "while closing cursor \"%s\"", curname);
		PG_RETURN_TEXT_P(cstring_to_text("ERROR"));
	}

	PQclear(res);

	/* if we started a transaction, decrement cursor count
	 *
	 * 如果我们开始一个事务，则减少游标计数
	 */
	if (rconn->newXactForCursor)
	{
		(rconn->openCursorCount)--;

		/* if count is zero, commit the transaction
		 *
		 * 如果计数为零，则提交事务
		 */
		if (rconn->openCursorCount == 0)
		{
			rconn->newXactForCursor = false;

			res = libpqsrv_exec(conn, "COMMIT", dblink_we_get_result);
			if (PQresultStatus(res) != PGRES_COMMAND_OK)
				dblink_res_internalerror(conn, res, "commit error");
			PQclear(res);
		}
	}

	PG_RETURN_TEXT_P(cstring_to_text("OK"));
}

/*
 * Fetch results from an open cursor
 *
 * 从打开的游标中获取结果
 */
PG_FUNCTION_INFO_V1(dblink_fetch);
Datum
dblink_fetch(PG_FUNCTION_ARGS)
{
	PGresult   *res = NULL;
	char	   *conname = NULL;
	remoteConn *rconn = NULL;
	PGconn	   *conn = NULL;
	StringInfoData buf;
	char	   *curname = NULL;
	int			howmany = 0;
	bool		fail = true;	/* default to backward compatible */

	prepTuplestoreResult(fcinfo);

	dblink_init();

	if (PG_NARGS() == 4)
	{
		/* text,text,int,bool
		 *
		 * 文本,文本,整数,布尔
		 */
		conname = text_to_cstring(PG_GETARG_TEXT_PP(0));
		curname = text_to_cstring(PG_GETARG_TEXT_PP(1));
		howmany = PG_GETARG_INT32(2);
		fail = PG_GETARG_BOOL(3);

		rconn = getConnectionByName(conname);
		if (rconn)
			conn = rconn->conn;
	}
	else if (PG_NARGS() == 3)
	{
		/* text,text,int or text,int,bool
		 *
		 * 文本、文本、int 或文本、int、bool
		 */
		if (get_fn_expr_argtype(fcinfo->flinfo, 2) == BOOLOID)
		{
			curname = text_to_cstring(PG_GETARG_TEXT_PP(0));
			howmany = PG_GETARG_INT32(1);
			fail = PG_GETARG_BOOL(2);
			conn = pconn->conn;
		}
		else
		{
			conname = text_to_cstring(PG_GETARG_TEXT_PP(0));
			curname = text_to_cstring(PG_GETARG_TEXT_PP(1));
			howmany = PG_GETARG_INT32(2);

			rconn = getConnectionByName(conname);
			if (rconn)
				conn = rconn->conn;
		}
	}
	else if (PG_NARGS() == 2)
	{
		/* text,int
		 *
		 * 文本，整数
		 */
		curname = text_to_cstring(PG_GETARG_TEXT_PP(0));
		howmany = PG_GETARG_INT32(1);
		conn = pconn->conn;
	}

	if (!conn)
		dblink_conn_not_avail(conname);

	initStringInfo(&buf);
	appendStringInfo(&buf, "FETCH %d FROM %s", howmany, curname);

	/*
	 * Try to execute the query.  Note that since libpq uses malloc, the
	 * PGresult will be long-lived even though we are still in a short-lived
	 * memory context.
	 *
	 * 尝试执行查询。  请注意，由于 libpq 使用 malloc，因此即使我们仍处于短期内存上下文中，PGresult 也将长期存在。
	 */
	res = libpqsrv_exec(conn, buf.data, dblink_we_get_result);
	if (!res ||
		(PQresultStatus(res) != PGRES_COMMAND_OK &&
		 PQresultStatus(res) != PGRES_TUPLES_OK))
	{
		dblink_res_error(conn, conname, res, fail,
						 "while fetching from cursor \"%s\"", curname);
		return (Datum) 0;
	}
	else if (PQresultStatus(res) == PGRES_COMMAND_OK)
	{
		/* cursor does not exist - closed already or bad name
		 *
		 * 游标不存在 - 已关闭或名称错误
		 */
		PQclear(res);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_NAME),
				 errmsg("cursor \"%s\" does not exist", curname)));
	}

	materializeResult(fcinfo, conn, res);
	return (Datum) 0;
}

/*
 * Note: this is the new preferred version of dblink
 *
 * 注意：这是 dblink 的新首选版本
 */
PG_FUNCTION_INFO_V1(dblink_record);
Datum
dblink_record(PG_FUNCTION_ARGS)
{
	return dblink_record_internal(fcinfo, false);
}

PG_FUNCTION_INFO_V1(dblink_send_query);
Datum
dblink_send_query(PG_FUNCTION_ARGS)
{
	PGconn	   *conn;
	char	   *sql;
	int			retval;

	if (PG_NARGS() == 2)
	{
		conn = dblink_get_named_conn(text_to_cstring(PG_GETARG_TEXT_PP(0)));
		sql = text_to_cstring(PG_GETARG_TEXT_PP(1));
	}
	else
		/* shouldn't happen
		 *
		 * 不应该发生
		 */
		elog(ERROR, "wrong number of arguments");

	/* async query send
	 *
	 * 异步查询发送
	 */
	retval = PQsendQuery(conn, sql);
	if (retval != 1)
		elog(NOTICE, "could not send query: %s", pchomp(PQerrorMessage(conn)));

	PG_RETURN_INT32(retval);
}

PG_FUNCTION_INFO_V1(dblink_get_result);
Datum
dblink_get_result(PG_FUNCTION_ARGS)
{
	return dblink_record_internal(fcinfo, true);
}

static Datum
dblink_record_internal(FunctionCallInfo fcinfo, bool is_async)
{
	PGconn	   *volatile conn = NULL;
	volatile bool freeconn = false;

	prepTuplestoreResult(fcinfo);

	dblink_init();

	PG_TRY();
	{
		char	   *sql = NULL;
		char	   *conname = NULL;
		bool		fail = true;	/* default to backward compatible */

		if (!is_async)
		{
			if (PG_NARGS() == 3)
			{
				/* text,text,bool
				 *
				 * 文本,文本,布尔值
				 */
				conname = text_to_cstring(PG_GETARG_TEXT_PP(0));
				sql = text_to_cstring(PG_GETARG_TEXT_PP(1));
				fail = PG_GETARG_BOOL(2);
				dblink_get_conn(conname, &conn, &conname, &freeconn);
			}
			else if (PG_NARGS() == 2)
			{
				/* text,text or text,bool
				 *
				 * 文本，文本或文本，布尔
				 */
				if (get_fn_expr_argtype(fcinfo->flinfo, 1) == BOOLOID)
				{
					sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
					fail = PG_GETARG_BOOL(1);
					conn = pconn->conn;
				}
				else
				{
					conname = text_to_cstring(PG_GETARG_TEXT_PP(0));
					sql = text_to_cstring(PG_GETARG_TEXT_PP(1));
					dblink_get_conn(conname, &conn, &conname, &freeconn);
				}
			}
			else if (PG_NARGS() == 1)
			{
				/* text */
				conn = pconn->conn;
				sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
			}
			else
				/* shouldn't happen
				 *
				 * 不应该发生
				 */
				elog(ERROR, "wrong number of arguments");
		}
		else					/* is_async */
		{
			/* get async result
			 *
			 * 获取异步结果
			 */
			conname = text_to_cstring(PG_GETARG_TEXT_PP(0));

			if (PG_NARGS() == 2)
			{
				/* text,bool
				 *
				 * 文本，布尔值
				 */
				fail = PG_GETARG_BOOL(1);
				conn = dblink_get_named_conn(conname);
			}
			else if (PG_NARGS() == 1)
			{
				/* text */
				conn = dblink_get_named_conn(conname);
			}
			else
				/* shouldn't happen
				 *
				 * 不应该发生
				 */
				elog(ERROR, "wrong number of arguments");
		}

		if (!conn)
			dblink_conn_not_avail(conname);

		if (!is_async)
		{
			/* synchronous query, use efficient tuple collection method
			 *
			 * 同步查询，使用高效的元​​组集合方法
			 */
			materializeQueryResult(fcinfo, conn, conname, sql, fail);
		}
		else
		{
			/* async result retrieval, do it the old way
			 *
			 * 异步结果检索，按照旧方法进行
			 */
			PGresult   *res = libpqsrv_get_result(conn, dblink_we_get_result);

			/* NULL means we're all done with the async results
			 *
			 * NULL 意味着我们已经完成了异步结果
			 */
			if (res)
			{
				if (PQresultStatus(res) != PGRES_COMMAND_OK &&
					PQresultStatus(res) != PGRES_TUPLES_OK)
				{
					dblink_res_error(conn, conname, res, fail,
									 "while executing query");
					/* if fail isn't set, we'll return an empty query result
					 *
					 * 如果未设置fail，我们将返回一个空的查询结果
					 */
				}
				else
				{
					materializeResult(fcinfo, conn, res);
				}
			}
		}
	}
	PG_FINALLY();
	{
		/* if needed, close the connection to the database
		 *
		 * 如果需要，关闭与数据库的连接
		 */
		if (freeconn)
			libpqsrv_disconnect(conn);
	}
	PG_END_TRY();

	return (Datum) 0;
}

/*
 * Verify function caller can handle a tuplestore result, and set up for that.
 *
 * 验证函数调用者可以处理元组存储结果，并为此进行设置。
 *
 * Note: if the caller returns without actually creating a tuplestore, the
 * executor will treat the function result as an empty set.
 *
 * 注意：如果调用者返回时没有实际创建元组存储，则执行器会将函数结果视为空集。
 */
static void
prepTuplestoreResult(FunctionCallInfo fcinfo)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	/* check to see if query supports us returning a tuplestore
	 *
	 * 检查查询是否支持我们返回元组存储
	 */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not allowed in this context")));

	/* let the executor know we're sending back a tuplestore
	 *
	 * 让执行者知道我们正在发回一个元组存储
	 */
	rsinfo->returnMode = SFRM_Materialize;

	/* caller must fill these to return a non-empty result
	 *
	 * 调用者必须填写这些以返回非空结果
	 */
	rsinfo->setResult = NULL;
	rsinfo->setDesc = NULL;
}

/*
 * Copy the contents of the PGresult into a tuplestore to be returned
 * as the result of the current function.
 * The PGresult will be released in this function.
 *
 * 将 PGresult 的内容复制到元组存储中，以作为当前函数的结果返回。 PGresult将在此函数中释放。
 */
static void
materializeResult(FunctionCallInfo fcinfo, PGconn *conn, PGresult *res)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	/* prepTuplestoreResult must have been called previously
	 *
	 * prepTuplestoreResult 必须先前已调用
	 */
	Assert(rsinfo->returnMode == SFRM_Materialize);

	PG_TRY();
	{
		TupleDesc	tupdesc;
		bool		is_sql_cmd;
		int			ntuples;
		int			nfields;

		if (PQresultStatus(res) == PGRES_COMMAND_OK)
		{
			is_sql_cmd = true;

			/*
			 * need a tuple descriptor representing one TEXT column to return
			 * the command status string as our result tuple
			 *
			 * 需要一个表示一个 TEXT 列的元组描述符来返回命令状态字符串作为我们的结果元组
			 */
			tupdesc = CreateTemplateTupleDesc(1);
			TupleDescInitEntry(tupdesc, (AttrNumber) 1, "status",
							   TEXTOID, -1, 0);
			ntuples = 1;
			nfields = 1;
		}
		else
		{
			Assert(PQresultStatus(res) == PGRES_TUPLES_OK);

			is_sql_cmd = false;

			/* get a tuple descriptor for our result type
			 *
			 * 获取结果类型的元组描述符
			 */
			switch (get_call_result_type(fcinfo, NULL, &tupdesc))
			{
				case TYPEFUNC_COMPOSITE:
					/* success */
					break;
				case TYPEFUNC_RECORD:
					/* failed to determine actual type of RECORD
					 *
					 * 无法确定 RECORD 的实际类型
					 */
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("function returning record called in context "
									"that cannot accept type record")));
					break;
				default:
					/* result type isn't composite
					 *
					 * 结果类型不是复合类型
					 */
					elog(ERROR, "return type must be a row type");
					break;
			}

			/* make sure we have a persistent copy of the tupdesc
			 *
			 * 确保我们有 tupdesc 的持久副本
			 */
			tupdesc = CreateTupleDescCopy(tupdesc);
			ntuples = PQntuples(res);
			nfields = PQnfields(res);
		}

		/*
		 * check result and tuple descriptor have the same number of columns
		 *
		 * 检查结果和元组描述符具有相同的列数
		 */
		if (nfields != tupdesc->natts)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("remote query result rowtype does not match "
							"the specified FROM clause rowtype")));

		if (ntuples > 0)
		{
			AttInMetadata *attinmeta;
			int			nestlevel = -1;
			Tuplestorestate *tupstore;
			MemoryContext oldcontext;
			int			row;
			char	  **values;

			attinmeta = TupleDescGetAttInMetadata(tupdesc);

			/* Set GUCs to ensure we read GUC-sensitive data types correctly
			 *
			 * 设置GUC以确保我们正确读取GUC敏感的数据类型
			 */
			if (!is_sql_cmd)
				nestlevel = applyRemoteGucs(conn);

			oldcontext = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);
			tupstore = tuplestore_begin_heap(true, false, work_mem);
			rsinfo->setResult = tupstore;
			rsinfo->setDesc = tupdesc;
			MemoryContextSwitchTo(oldcontext);

			values = palloc_array(char *, nfields);

			/* put all tuples into the tuplestore
			 *
			 * 将所有元组放入元组存储中
			 */
			for (row = 0; row < ntuples; row++)
			{
				HeapTuple	tuple;

				if (!is_sql_cmd)
				{
					int			i;

					for (i = 0; i < nfields; i++)
					{
						if (PQgetisnull(res, row, i))
							values[i] = NULL;
						else
							values[i] = PQgetvalue(res, row, i);
					}
				}
				else
				{
					values[0] = PQcmdStatus(res);
				}

				/* build the tuple and put it into the tuplestore.
				 *
				 * 构建元组并将其放入元组存储中。
				 */
				tuple = BuildTupleFromCStrings(attinmeta, values);
				tuplestore_puttuple(tupstore, tuple);
			}

			/* clean up GUC settings, if we changed any
			 *
			 * 清理 GUC 设置（如果我们更改了任何设置）
			 */
			restoreLocalGucs(nestlevel);
		}
	}
	PG_FINALLY();
	{
		/* be sure to release the libpq result
		 *
		 * 请务必发布 libpq 结果
		 */
		PQclear(res);
	}
	PG_END_TRY();
}

/*
 * Execute the given SQL command and store its results into a tuplestore
 * to be returned as the result of the current function.
 *
 * 执行给定的 SQL 命令并将其结果存储到元组存储中，以作为当前函数的结果返回。
 *
 * This is equivalent to PQexec followed by materializeResult, but we make
 * use of libpq's single-row mode to avoid accumulating the whole result
 * inside libpq before it gets transferred to the tuplestore.
 *
 * 这相当于 PQexec 后跟 MaterializeResult，但我们利用 libpq 的单行模式来避免在传输到元组存储之前将整个结果累加到 libpq 中。
 */
static void
materializeQueryResult(FunctionCallInfo fcinfo,
					   PGconn *conn,
					   const char *conname,
					   const char *sql,
					   bool fail)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	PGresult   *volatile res = NULL;
	volatile storeInfo sinfo = {0};

	/* prepTuplestoreResult must have been called previously
	 *
	 * prepTuplestoreResult 必须先前已调用
	 */
	Assert(rsinfo->returnMode == SFRM_Materialize);

	sinfo.fcinfo = fcinfo;

	PG_TRY();
	{
		/* Create short-lived memory context for data conversions
		 *
		 * 为数据转换创建短暂的内存上下文
		 */
		sinfo.tmpcontext = AllocSetContextCreate(CurrentMemoryContext,
												 "dblink temporary context",
												 ALLOCSET_DEFAULT_SIZES);

		/* execute query, collecting any tuples into the tuplestore
		 *
		 * 执行查询，将所有元组收集到元组存储中
		 */
		res = storeQueryResult(&sinfo, conn, sql);

		if (!res ||
			(PQresultStatus(res) != PGRES_COMMAND_OK &&
			 PQresultStatus(res) != PGRES_TUPLES_OK))
		{
			/*
			 * dblink_res_error will clear the passed PGresult, so we need
			 * this ugly dance to avoid doing so twice during error exit
			 *
			 * dblink_res_error 将清除传递的 PGresult，因此我们需要这种丑陋的舞蹈来避免在错误退出期间这样做两次
			 */
			PGresult   *res1 = res;

			res = NULL;
			dblink_res_error(conn, conname, res1, fail,
							 "while executing query");
			/* if fail isn't set, we'll return an empty query result
			 *
			 * 如果未设置fail，我们将返回一个空的查询结果
			 */
		}
		else if (PQresultStatus(res) == PGRES_COMMAND_OK)
		{
			/*
			 * storeRow didn't get called, so we need to convert the command
			 * status string to a tuple manually
			 *
			 * storeRow 没有被调用，所以我们需要手动将命令状态字符串转换为元组
			 */
			TupleDesc	tupdesc;
			AttInMetadata *attinmeta;
			Tuplestorestate *tupstore;
			HeapTuple	tuple;
			char	   *values[1];
			MemoryContext oldcontext;

			/*
			 * need a tuple descriptor representing one TEXT column to return
			 * the command status string as our result tuple
			 *
			 * 需要一个表示一个 TEXT 列的元组描述符来返回命令状态字符串作为我们的结果元组
			 */
			tupdesc = CreateTemplateTupleDesc(1);
			TupleDescInitEntry(tupdesc, (AttrNumber) 1, "status",
							   TEXTOID, -1, 0);
			attinmeta = TupleDescGetAttInMetadata(tupdesc);

			oldcontext = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);
			tupstore = tuplestore_begin_heap(true, false, work_mem);
			rsinfo->setResult = tupstore;
			rsinfo->setDesc = tupdesc;
			MemoryContextSwitchTo(oldcontext);

			values[0] = PQcmdStatus(res);

			/* build the tuple and put it into the tuplestore.
			 *
			 * 构建元组并将其放入元组存储中。
			 */
			tuple = BuildTupleFromCStrings(attinmeta, values);
			tuplestore_puttuple(tupstore, tuple);

			PQclear(res);
			res = NULL;
		}
		else
		{
			Assert(PQresultStatus(res) == PGRES_TUPLES_OK);
			/* storeRow should have created a tuplestore
			 *
			 * storeRow 应该创建一个 tuplestore
			 */
			Assert(rsinfo->setResult != NULL);

			PQclear(res);
			res = NULL;
		}

		/* clean up data conversion short-lived memory context
		 *
		 * 清理数据转换短期内存上下文
		 */
		if (sinfo.tmpcontext != NULL)
			MemoryContextDelete(sinfo.tmpcontext);
		sinfo.tmpcontext = NULL;

		PQclear(sinfo.last_res);
		sinfo.last_res = NULL;
		PQclear(sinfo.cur_res);
		sinfo.cur_res = NULL;
	}
	PG_CATCH();
	{
		/* be sure to release any libpq result we collected
		 *
		 * 请务必发布我们收集的所有 libpq 结果
		 */
		PQclear(res);
		PQclear(sinfo.last_res);
		PQclear(sinfo.cur_res);
		/* and clear out any pending data in libpq
		 *
		 * 并清除 libpq 中所有待处理的数据
		 */
		while ((res = libpqsrv_get_result(conn, dblink_we_get_result)) !=
			   NULL)
			PQclear(res);
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * Execute query, and send any result rows to sinfo->tuplestore.
 *
 * 执行查询，并将任何结果行发送到 sinfo->tuplestore。
 */
static PGresult *
storeQueryResult(volatile storeInfo *sinfo, PGconn *conn, const char *sql)
{
	bool		first = true;
	int			nestlevel = -1;
	PGresult   *res;

	if (!PQsendQuery(conn, sql))
		elog(ERROR, "could not send query: %s", pchomp(PQerrorMessage(conn)));

	if (!PQsetSingleRowMode(conn))	/* shouldn't fail */
		elog(ERROR, "failed to set single-row mode for dblink query");

	for (;;)
	{
		CHECK_FOR_INTERRUPTS();

		sinfo->cur_res = libpqsrv_get_result(conn, dblink_we_get_result);
		if (!sinfo->cur_res)
			break;

		if (PQresultStatus(sinfo->cur_res) == PGRES_SINGLE_TUPLE)
		{
			/* got one row from possibly-bigger resultset
			 *
			 * 从可能更大的结果集中获取一行
			 */

			/*
			 * Set GUCs to ensure we read GUC-sensitive data types correctly.
			 * We shouldn't do this until we have a row in hand, to ensure
			 * libpq has seen any earlier ParameterStatus protocol messages.
			 *
			 * 设置 GUC 以确保我们正确读取 GUC 敏感数据类型。在手头有一行之前我们不应该这样做，以确保 libpq 已经看到任何早期的 ParameterStatus 协议消息。
			 */
			if (first && nestlevel < 0)
				nestlevel = applyRemoteGucs(conn);

			storeRow(sinfo, sinfo->cur_res, first);

			PQclear(sinfo->cur_res);
			sinfo->cur_res = NULL;
			first = false;
		}
		else
		{
			/* if empty resultset, fill tuplestore header
			 *
			 * 如果结果集为空，则填充 tuplestore 标头
			 */
			if (first && PQresultStatus(sinfo->cur_res) == PGRES_TUPLES_OK)
				storeRow(sinfo, sinfo->cur_res, first);

			/* store completed result at last_res
			 *
			 * 将完成的结果存储在last_res中
			 */
			PQclear(sinfo->last_res);
			sinfo->last_res = sinfo->cur_res;
			sinfo->cur_res = NULL;
			first = true;
		}
	}

	/* clean up GUC settings, if we changed any
	 *
	 * 清理 GUC 设置（如果我们更改了任何设置）
	 */
	restoreLocalGucs(nestlevel);

	/* return last_res
	 *
	 * 返回最后的结果
	 */
	res = sinfo->last_res;
	sinfo->last_res = NULL;
	return res;
}

/*
 * Send single row to sinfo->tuplestore.
 *
 * 将单行发送到 sinfo->tuplestore。
 *
 * If "first" is true, create the tuplestore using PGresult's metadata
 * (in this case the PGresult might contain either zero or one row).
 *
 * 如果“first”为 true，则使用 PGresult 的元数据创建元组存储（在这种情况下，PGresult 可能包含零行或一行）。
 */
static void
storeRow(volatile storeInfo *sinfo, PGresult *res, bool first)
{
	int			nfields = PQnfields(res);
	HeapTuple	tuple;
	int			i;
	MemoryContext oldcontext;

	if (first)
	{
		/* Prepare for new result set
		 *
		 * 准备新的结果集
		 */
		ReturnSetInfo *rsinfo = (ReturnSetInfo *) sinfo->fcinfo->resultinfo;
		TupleDesc	tupdesc;

		/*
		 * It's possible to get more than one result set if the query string
		 * contained multiple SQL commands.  In that case, we follow PQexec's
		 * traditional behavior of throwing away all but the last result.
		 *
		 * 如果查询字符串包含多个 SQL 命令，则可能会获得多个结果集。  在这种情况下，我们遵循 PQexec 的传统行为，即丢弃除最后结果之外的所有结果。
		 */
		if (sinfo->tuplestore)
			tuplestore_end(sinfo->tuplestore);
		sinfo->tuplestore = NULL;

		/* get a tuple descriptor for our result type
		 *
		 * 获取结果类型的元组描述符
		 */
		switch (get_call_result_type(sinfo->fcinfo, NULL, &tupdesc))
		{
			case TYPEFUNC_COMPOSITE:
				/* success */
				break;
			case TYPEFUNC_RECORD:
				/* failed to determine actual type of RECORD
				 *
				 * 无法确定 RECORD 的实际类型
				 */
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("function returning record called in context "
								"that cannot accept type record")));
				break;
			default:
				/* result type isn't composite
				 *
				 * 结果类型不是复合类型
				 */
				elog(ERROR, "return type must be a row type");
				break;
		}

		/* make sure we have a persistent copy of the tupdesc
		 *
		 * 确保我们有 tupdesc 的持久副本
		 */
		tupdesc = CreateTupleDescCopy(tupdesc);

		/* check result and tuple descriptor have the same number of columns
		 *
		 * 检查结果和元组描述符具有相同的列数
		 */
		if (nfields != tupdesc->natts)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("remote query result rowtype does not match "
							"the specified FROM clause rowtype")));

		/* Prepare attinmeta for later data conversions
		 *
		 * 为以后的数据转换准备attinmeta
		 */
		sinfo->attinmeta = TupleDescGetAttInMetadata(tupdesc);

		/* Create a new, empty tuplestore
		 *
		 * 创建一个新的空元组存储
		 */
		oldcontext = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);
		sinfo->tuplestore = tuplestore_begin_heap(true, false, work_mem);
		rsinfo->setResult = sinfo->tuplestore;
		rsinfo->setDesc = tupdesc;
		MemoryContextSwitchTo(oldcontext);

		/* Done if empty resultset
		 *
		 * 如果结果集为空则完成
		 */
		if (PQntuples(res) == 0)
			return;

		/*
		 * Set up sufficiently-wide string pointers array; this won't change
		 * in size so it's easy to preallocate.
		 *
		 * 设置足够宽的字符串指针数组；它的大小不会改变，因此很容易预分配。
		 */
		if (sinfo->cstrs)
			pfree(sinfo->cstrs);
		sinfo->cstrs = palloc_array(char *, nfields);
	}

	/* Should have a single-row result if we get here
	 *
	 * 如果我们到达这里应该有一个单行结果
	 */
	Assert(PQntuples(res) == 1);

	/*
	 * Do the following work in a temp context that we reset after each tuple.
	 * This cleans up not only the data we have direct access to, but any
	 * cruft the I/O functions might leak.
	 *
	 * 在我们在每个元组之后重置的临时上下文中执行以下工作。这不仅清理了我们可以直接访问的数据，还清理了 I/O 函数可能泄漏的任何残骸。
	 */
	oldcontext = MemoryContextSwitchTo(sinfo->tmpcontext);

	/*
	 * Fill cstrs with null-terminated strings of column values.
	 *
	 * 使用以 null 结尾的列值字符串填充 cstr。
	 */
	for (i = 0; i < nfields; i++)
	{
		if (PQgetisnull(res, 0, i))
			sinfo->cstrs[i] = NULL;
		else
			sinfo->cstrs[i] = PQgetvalue(res, 0, i);
	}

	/* Convert row to a tuple, and add it to the tuplestore
	 *
	 * 将行转换为元组，并将其添加到元组存储中
	 */
	tuple = BuildTupleFromCStrings(sinfo->attinmeta, sinfo->cstrs);

	tuplestore_puttuple(sinfo->tuplestore, tuple);

	/* Clean up
	 *
	 * 清理
	 */
	MemoryContextSwitchTo(oldcontext);
	MemoryContextReset(sinfo->tmpcontext);
}

/*
 * List all open dblink connections by name.
 * Returns an array of all connection names.
 * Takes no params
 *
 * 按名称列出所有打开的 dblink 连接。返回所有连接名称的数组。不带参数
 */
PG_FUNCTION_INFO_V1(dblink_get_connections);
Datum
dblink_get_connections(PG_FUNCTION_ARGS)
{
	HASH_SEQ_STATUS status;
	remoteConnHashEnt *hentry;
	ArrayBuildState *astate = NULL;

	if (remoteConnHash)
	{
		hash_seq_init(&status, remoteConnHash);
		while ((hentry = (remoteConnHashEnt *) hash_seq_search(&status)) != NULL)
		{
			/* ignore it if it's not an open connection
			 *
			 * 如果不是打开的连接则忽略它
			 */
			if (hentry->rconn.conn == NULL)
				continue;
			/* stash away current value
			 *
			 * 隐藏当前值
			 */
			astate = accumArrayResult(astate,
									  CStringGetTextDatum(hentry->name),
									  false, TEXTOID, CurrentMemoryContext);
		}
	}

	if (astate)
		PG_RETURN_DATUM(makeArrayResult(astate,
										CurrentMemoryContext));
	else
		PG_RETURN_NULL();
}

/*
 * Checks if a given remote connection is busy
 *
 * 检查给定的远程连接是否繁忙
 *
 * Returns 1 if the connection is busy, 0 otherwise
 * Params:
 *	text connection_name - name of the connection to check
 *
 * 如果连接繁忙则返回 1，否则返回 0 参数：text connection_name - 要检查的连接名称
 *
 */
PG_FUNCTION_INFO_V1(dblink_is_busy);
Datum
dblink_is_busy(PG_FUNCTION_ARGS)
{
	PGconn	   *conn;

	dblink_init();
	conn = dblink_get_named_conn(text_to_cstring(PG_GETARG_TEXT_PP(0)));

	PQconsumeInput(conn);
	PG_RETURN_INT32(PQisBusy(conn));
}

/*
 * Cancels a running request on a connection
 *
 * 取消连接上正在运行的请求
 *
 * Returns text:
 *	"OK" if the cancel request has been sent correctly,
 *		an error message otherwise
 *
 * 返回文本：如果取消请求已正确发送，则返回“OK”，否则返回错误消息
 *
 * Params:
 *	text connection_name - name of the connection to check
 *
 * 参数：文本connection_name - 要检查的连接的名称
 *
 */
PG_FUNCTION_INFO_V1(dblink_cancel_query);
Datum
dblink_cancel_query(PG_FUNCTION_ARGS)
{
	PGconn	   *conn;
	const char *msg;
	TimestampTz endtime;

	dblink_init();
	conn = dblink_get_named_conn(text_to_cstring(PG_GETARG_TEXT_PP(0)));
	endtime = TimestampTzPlusMilliseconds(GetCurrentTimestamp(),
										  30000);
	msg = libpqsrv_cancel(conn, endtime);
	if (msg == NULL)
		msg = "OK";

	PG_RETURN_TEXT_P(cstring_to_text(msg));
}


/*
 * Get error message from a connection
 *
 * 从连接获取错误消息
 *
 * Returns text:
 *	"OK" if no error, an error message otherwise
 *
 * 返回文本：如果没有错误则返回“OK”，否则返回错误消息
 *
 * Params:
 *	text connection_name - name of the connection to check
 *
 * 参数：文本connection_name - 要检查的连接的名称
 *
 */
PG_FUNCTION_INFO_V1(dblink_error_message);
Datum
dblink_error_message(PG_FUNCTION_ARGS)
{
	char	   *msg;
	PGconn	   *conn;

	dblink_init();
	conn = dblink_get_named_conn(text_to_cstring(PG_GETARG_TEXT_PP(0)));

	msg = PQerrorMessage(conn);
	if (msg == NULL || msg[0] == '\0')
		PG_RETURN_TEXT_P(cstring_to_text("OK"));
	else
		PG_RETURN_TEXT_P(cstring_to_text(pchomp(msg)));
}

/*
 * Execute an SQL non-SELECT command
 *
 * 执行 SQL 非 SELECT 命令
 */
PG_FUNCTION_INFO_V1(dblink_exec);
Datum
dblink_exec(PG_FUNCTION_ARGS)
{
	text	   *volatile sql_cmd_status = NULL;
	PGconn	   *volatile conn = NULL;
	volatile bool freeconn = false;

	dblink_init();

	PG_TRY();
	{
		PGresult   *res = NULL;
		char	   *sql = NULL;
		char	   *conname = NULL;
		bool		fail = true;	/* default to backward compatible behavior */

		if (PG_NARGS() == 3)
		{
			/* must be text,text,bool
			 *
			 * 必须是文本、文本、布尔值
			 */
			conname = text_to_cstring(PG_GETARG_TEXT_PP(0));
			sql = text_to_cstring(PG_GETARG_TEXT_PP(1));
			fail = PG_GETARG_BOOL(2);
			dblink_get_conn(conname, &conn, &conname, &freeconn);
		}
		else if (PG_NARGS() == 2)
		{
			/* might be text,text or text,bool
			 *
			 * 可能是文本、文本或文本、布尔值
			 */
			if (get_fn_expr_argtype(fcinfo->flinfo, 1) == BOOLOID)
			{
				sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
				fail = PG_GETARG_BOOL(1);
				conn = pconn->conn;
			}
			else
			{
				conname = text_to_cstring(PG_GETARG_TEXT_PP(0));
				sql = text_to_cstring(PG_GETARG_TEXT_PP(1));
				dblink_get_conn(conname, &conn, &conname, &freeconn);
			}
		}
		else if (PG_NARGS() == 1)
		{
			/* must be single text argument
			 *
			 * 必须是单个文本参数
			 */
			conn = pconn->conn;
			sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
		}
		else
			/* shouldn't happen
			 *
			 * 不应该发生
			 */
			elog(ERROR, "wrong number of arguments");

		if (!conn)
			dblink_conn_not_avail(conname);

		res = libpqsrv_exec(conn, sql, dblink_we_get_result);
		if (!res ||
			(PQresultStatus(res) != PGRES_COMMAND_OK &&
			 PQresultStatus(res) != PGRES_TUPLES_OK))
		{
			dblink_res_error(conn, conname, res, fail,
							 "while executing command");

			/*
			 * and save a copy of the command status string to return as our
			 * result tuple
			 *
			 * 并保存命令状态字符串的副本以作为结果元组返回
			 */
			sql_cmd_status = cstring_to_text("ERROR");
		}
		else if (PQresultStatus(res) == PGRES_COMMAND_OK)
		{
			/*
			 * and save a copy of the command status string to return as our
			 * result tuple
			 *
			 * 并保存命令状态字符串的副本以作为结果元组返回
			 */
			sql_cmd_status = cstring_to_text(PQcmdStatus(res));
			PQclear(res);
		}
		else
		{
			PQclear(res);
			ereport(ERROR,
					(errcode(ERRCODE_S_R_E_PROHIBITED_SQL_STATEMENT_ATTEMPTED),
					 errmsg("statement returning results not allowed")));
		}
	}
	PG_FINALLY();
	{
		/* if needed, close the connection to the database
		 *
		 * 如果需要，关闭与数据库的连接
		 */
		if (freeconn)
			libpqsrv_disconnect(conn);
	}
	PG_END_TRY();

	PG_RETURN_TEXT_P(sql_cmd_status);
}


/*
 * dblink_get_pkey
 *
 * Return list of primary key fields for the supplied relation,
 * or NULL if none exists.
 *
 * 返回所提供关系的主键字段列表，如果不存在则返回 NULL。
 */
PG_FUNCTION_INFO_V1(dblink_get_pkey);
Datum
dblink_get_pkey(PG_FUNCTION_ARGS)
{
	int16		indnkeyatts;
	char	  **results;
	FuncCallContext *funcctx;
	int32		call_cntr;
	int32		max_calls;
	AttInMetadata *attinmeta;
	MemoryContext oldcontext;

	/* stuff done only on the first call of the function
	 *
	 * 仅在第一次调用函数时完成的操作
	 */
	if (SRF_IS_FIRSTCALL())
	{
		Relation	rel;
		TupleDesc	tupdesc;

		/* create a function context for cross-call persistence
		 *
		 * 创建用于交叉调用持久化的函数上下文
		 */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * switch to memory context appropriate for multiple function calls
		 *
		 * 切换到适合多个函数调用的内存上下文
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* open target relation
		 *
		 * 开放目标关系
		 */
		rel = get_rel_from_relname(PG_GETARG_TEXT_PP(0), AccessShareLock, ACL_SELECT);

		/* get the array of attnums
		 *
		 * 获取 attnums 数组
		 */
		results = get_pkey_attnames(rel, &indnkeyatts);

		relation_close(rel, AccessShareLock);

		/*
		 * need a tuple descriptor representing one INT and one TEXT column
		 *
		 * 需要一个表示一个 INT 和一个 TEXT 列的元组描述符
		 */
		tupdesc = CreateTemplateTupleDesc(2);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "position",
						   INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "colname",
						   TEXTOID, -1, 0);

		/*
		 * Generate attribute metadata needed later to produce tuples from raw
		 * C strings
		 *
		 * 生成稍后需要的属性元数据，以从原始 C 字符串生成元组
		 */
		attinmeta = TupleDescGetAttInMetadata(tupdesc);
		funcctx->attinmeta = attinmeta;

		if ((results != NULL) && (indnkeyatts > 0))
		{
			funcctx->max_calls = indnkeyatts;

			/* got results, keep track of them
			 *
			 * 得到结果，跟踪它们
			 */
			funcctx->user_fctx = results;
		}
		else
		{
			/* fast track when no results
			 *
			 * 没有结果时快速跟踪
			 */
			MemoryContextSwitchTo(oldcontext);
			SRF_RETURN_DONE(funcctx);
		}

		MemoryContextSwitchTo(oldcontext);
	}

	/* stuff done on every call of the function
	 *
	 * 每次调用函数时完成的事情
	 */
	funcctx = SRF_PERCALL_SETUP();

	/*
	 * initialize per-call variables
	 *
	 * 初始化每次调用变量
	 */
	call_cntr = funcctx->call_cntr;
	max_calls = funcctx->max_calls;

	results = (char **) funcctx->user_fctx;
	attinmeta = funcctx->attinmeta;

	if (call_cntr < max_calls)	/* do when there is more left to send */
	{
		char	  **values;
		HeapTuple	tuple;
		Datum		result;

		values = palloc_array(char *, 2);
		values[0] = psprintf("%d", call_cntr + 1);
		values[1] = results[call_cntr];

		/* build the tuple
		 *
		 * 构建元组
		 */
		tuple = BuildTupleFromCStrings(attinmeta, values);

		/* make the tuple into a datum
		 *
		 * 使元组成为数据
		 */
		result = HeapTupleGetDatum(tuple);

		SRF_RETURN_NEXT(funcctx, result);
	}
	else
	{
		/* do when there is no more left
		 *
		 * 当没有剩下的时候做
		 */
		SRF_RETURN_DONE(funcctx);
	}
}


/*
 * dblink_build_sql_insert
 *
 * Used to generate an SQL insert statement
 * based on an existing tuple in a local relation.
 * This is useful for selectively replicating data
 * to another server via dblink.
 *
 * 用于根据本地关系中的现有元组生成 SQL 插入语句。这对于通过 dblink 有选择地将数据复制到另一台服务器非常有用。
 *
 * API:
 * <relname> - name of local table of interest
 * <pkattnums> - an int2vector of attnums which will be used
 * to identify the local tuple of interest
 * <pknumatts> - number of attnums in pkattnums
 * <src_pkattvals_arry> - text array of key values which will be used
 * to identify the local tuple of interest
 * <tgt_pkattvals_arry> - text array of key values which will be used
 * to build the string for execution remotely. These are substituted
 * for their counterparts in src_pkattvals_arry
 *
 * API： <relname> - 感兴趣的本地表的名称 <pkattnums> - 将用于标识感兴趣的本地元组的 attnums 的 int2vector <pknumatts> - pkattnums 中的 attnum 数量 <src_pkattvals_arry> - 将用于标识感兴趣的本地元组的键值文本数组 <tgt_pkattvals_arry> - 将用于构建的键值文本数组用于远程执行的字符串。这些被替换为 src_pkattvals_arry 中的对应项
 */
PG_FUNCTION_INFO_V1(dblink_build_sql_insert);
Datum
dblink_build_sql_insert(PG_FUNCTION_ARGS)
{
	text	   *relname_text = PG_GETARG_TEXT_PP(0);
	int2vector *pkattnums_arg = (int2vector *) PG_GETARG_POINTER(1);
	int32		pknumatts_arg = PG_GETARG_INT32(2);
	ArrayType  *src_pkattvals_arry = PG_GETARG_ARRAYTYPE_P(3);
	ArrayType  *tgt_pkattvals_arry = PG_GETARG_ARRAYTYPE_P(4);
	Relation	rel;
	int		   *pkattnums;
	int			pknumatts;
	char	  **src_pkattvals;
	char	  **tgt_pkattvals;
	int			src_nitems;
	int			tgt_nitems;
	char	   *sql;

	/*
	 * Open target relation.
	 *
	 * 打开目标关系。
	 */
	rel = get_rel_from_relname(relname_text, AccessShareLock, ACL_SELECT);

	/*
	 * Process pkattnums argument.
	 *
	 * 处理 pkattnums 参数。
	 */
	validate_pkattnums(rel, pkattnums_arg, pknumatts_arg,
					   &pkattnums, &pknumatts);

	/*
	 * Source array is made up of key values that will be used to locate the
	 * tuple of interest from the local system.
	 *
	 * 源数组由键值组成，这些键值将用于从本地系统定位感兴趣的元组。
	 */
	src_pkattvals = get_text_array_contents(src_pkattvals_arry, &src_nitems);

	/*
	 * There should be one source array key value for each key attnum
	 *
	 * 每个键 attnum 应该有一个源数组键值
	 */
	if (src_nitems != pknumatts)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("source key array length must match number of key attributes")));

	/*
	 * Target array is made up of key values that will be used to build the
	 * SQL string for use on the remote system.
	 *
	 * 目标数组由键值组成，这些键值将用于构建在远程系统上使用的 SQL 字符串。
	 */
	tgt_pkattvals = get_text_array_contents(tgt_pkattvals_arry, &tgt_nitems);

	/*
	 * There should be one target array key value for each key attnum
	 *
	 * 每个键值应该有一个目标数组键值
	 */
	if (tgt_nitems != pknumatts)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("target key array length must match number of key attributes")));

	/*
	 * Prep work is finally done. Go get the SQL string.
	 *
	 * 准备工作终于完成了。去获取 SQL 字符串。
	 */
	sql = get_sql_insert(rel, pkattnums, pknumatts, src_pkattvals, tgt_pkattvals);

	/*
	 * Now we can close the relation.
	 *
	 * 现在我们可以关闭关系了。
	 */
	relation_close(rel, AccessShareLock);

	/*
	 * And send it
	 *
	 * 并发送它
	 */
	PG_RETURN_TEXT_P(cstring_to_text(sql));
}


/*
 * dblink_build_sql_delete
 *
 * Used to generate an SQL delete statement.
 * This is useful for selectively replicating a
 * delete to another server via dblink.
 *
 * 用于生成 SQL 删除语句。这对于通过 dblink 有选择地将删除复制到另一台服务器非常有用。
 *
 * API:
 * <relname> - name of remote table of interest
 * <pkattnums> - an int2vector of attnums which will be used
 * to identify the remote tuple of interest
 * <pknumatts> - number of attnums in pkattnums
 * <tgt_pkattvals_arry> - text array of key values which will be used
 * to build the string for execution remotely.
 *
 * API： <relname> - 感兴趣的远程表的名称 <pkattnums> - attnums 的 int2vector，将用于标识感兴趣的远程元组 <pknumatts> - pkattnums 中的 attnums 数量 <tgt_pkattvals_arry> - 键值的文本数组，将用于构建远程执行的字符串。
 */
PG_FUNCTION_INFO_V1(dblink_build_sql_delete);
Datum
dblink_build_sql_delete(PG_FUNCTION_ARGS)
{
	text	   *relname_text = PG_GETARG_TEXT_PP(0);
	int2vector *pkattnums_arg = (int2vector *) PG_GETARG_POINTER(1);
	int32		pknumatts_arg = PG_GETARG_INT32(2);
	ArrayType  *tgt_pkattvals_arry = PG_GETARG_ARRAYTYPE_P(3);
	Relation	rel;
	int		   *pkattnums;
	int			pknumatts;
	char	  **tgt_pkattvals;
	int			tgt_nitems;
	char	   *sql;

	/*
	 * Open target relation.
	 *
	 * 打开目标关系。
	 */
	rel = get_rel_from_relname(relname_text, AccessShareLock, ACL_SELECT);

	/*
	 * Process pkattnums argument.
	 *
	 * 处理 pkattnums 参数。
	 */
	validate_pkattnums(rel, pkattnums_arg, pknumatts_arg,
					   &pkattnums, &pknumatts);

	/*
	 * Target array is made up of key values that will be used to build the
	 * SQL string for use on the remote system.
	 *
	 * 目标数组由键值组成，这些键值将用于构建在远程系统上使用的 SQL 字符串。
	 */
	tgt_pkattvals = get_text_array_contents(tgt_pkattvals_arry, &tgt_nitems);

	/*
	 * There should be one target array key value for each key attnum
	 *
	 * 每个键值应该有一个目标数组键值
	 */
	if (tgt_nitems != pknumatts)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("target key array length must match number of key attributes")));

	/*
	 * Prep work is finally done. Go get the SQL string.
	 *
	 * 准备工作终于完成了。去获取 SQL 字符串。
	 */
	sql = get_sql_delete(rel, pkattnums, pknumatts, tgt_pkattvals);

	/*
	 * Now we can close the relation.
	 *
	 * 现在我们可以关闭关系了。
	 */
	relation_close(rel, AccessShareLock);

	/*
	 * And send it
	 *
	 * 并发送它
	 */
	PG_RETURN_TEXT_P(cstring_to_text(sql));
}


/*
 * dblink_build_sql_update
 *
 * Used to generate an SQL update statement
 * based on an existing tuple in a local relation.
 * This is useful for selectively replicating data
 * to another server via dblink.
 *
 * 用于根据本地关系中的现有元组生成 SQL 更新语句。这对于通过 dblink 有选择地将数据复制到另一台服务器非常有用。
 *
 * API:
 * <relname> - name of local table of interest
 * <pkattnums> - an int2vector of attnums which will be used
 * to identify the local tuple of interest
 * <pknumatts> - number of attnums in pkattnums
 * <src_pkattvals_arry> - text array of key values which will be used
 * to identify the local tuple of interest
 * <tgt_pkattvals_arry> - text array of key values which will be used
 * to build the string for execution remotely. These are substituted
 * for their counterparts in src_pkattvals_arry
 *
 * API： <relname> - 感兴趣的本地表的名称 <pkattnums> - 将用于标识感兴趣的本地元组的 attnums 的 int2vector <pknumatts> - pkattnums 中的 attnum 数量 <src_pkattvals_arry> - 将用于标识感兴趣的本地元组的键值文本数组 <tgt_pkattvals_arry> - 将用于构建的键值文本数组用于远程执行的字符串。这些被替换为 src_pkattvals_arry 中的对应项
 */
PG_FUNCTION_INFO_V1(dblink_build_sql_update);
Datum
dblink_build_sql_update(PG_FUNCTION_ARGS)
{
	text	   *relname_text = PG_GETARG_TEXT_PP(0);
	int2vector *pkattnums_arg = (int2vector *) PG_GETARG_POINTER(1);
	int32		pknumatts_arg = PG_GETARG_INT32(2);
	ArrayType  *src_pkattvals_arry = PG_GETARG_ARRAYTYPE_P(3);
	ArrayType  *tgt_pkattvals_arry = PG_GETARG_ARRAYTYPE_P(4);
	Relation	rel;
	int		   *pkattnums;
	int			pknumatts;
	char	  **src_pkattvals;
	char	  **tgt_pkattvals;
	int			src_nitems;
	int			tgt_nitems;
	char	   *sql;

	/*
	 * Open target relation.
	 *
	 * 打开目标关系。
	 */
	rel = get_rel_from_relname(relname_text, AccessShareLock, ACL_SELECT);

	/*
	 * Process pkattnums argument.
	 *
	 * 处理 pkattnums 参数。
	 */
	validate_pkattnums(rel, pkattnums_arg, pknumatts_arg,
					   &pkattnums, &pknumatts);

	/*
	 * Source array is made up of key values that will be used to locate the
	 * tuple of interest from the local system.
	 *
	 * 源数组由键值组成，这些键值将用于从本地系统定位感兴趣的元组。
	 */
	src_pkattvals = get_text_array_contents(src_pkattvals_arry, &src_nitems);

	/*
	 * There should be one source array key value for each key attnum
	 *
	 * 每个键 attnum 应该有一个源数组键值
	 */
	if (src_nitems != pknumatts)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("source key array length must match number of key attributes")));

	/*
	 * Target array is made up of key values that will be used to build the
	 * SQL string for use on the remote system.
	 *
	 * 目标数组由键值组成，这些键值将用于构建在远程系统上使用的 SQL 字符串。
	 */
	tgt_pkattvals = get_text_array_contents(tgt_pkattvals_arry, &tgt_nitems);

	/*
	 * There should be one target array key value for each key attnum
	 *
	 * 每个键值应该有一个目标数组键值
	 */
	if (tgt_nitems != pknumatts)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("target key array length must match number of key attributes")));

	/*
	 * Prep work is finally done. Go get the SQL string.
	 *
	 * 准备工作终于完成了。去获取 SQL 字符串。
	 */
	sql = get_sql_update(rel, pkattnums, pknumatts, src_pkattvals, tgt_pkattvals);

	/*
	 * Now we can close the relation.
	 *
	 * 现在我们可以关闭关系了。
	 */
	relation_close(rel, AccessShareLock);

	/*
	 * And send it
	 *
	 * 并发送它
	 */
	PG_RETURN_TEXT_P(cstring_to_text(sql));
}

/*
 * dblink_current_query
 * return the current query string
 * to allow its use in (among other things)
 * rewrite rules
 *
 * dblink_current_query 返回当前查询字符串以允许其在（除其他外）重写规则中使用
 */
PG_FUNCTION_INFO_V1(dblink_current_query);
Datum
dblink_current_query(PG_FUNCTION_ARGS)
{
	/* This is now just an alias for the built-in function current_query()
	 *
	 * 现在这只是内置函数 current_query() 的别名
	 */
	PG_RETURN_DATUM(current_query(fcinfo));
}

/*
 * Retrieve async notifications for a connection.
 *
 * 检索连接的异步通知。
 *
 * Returns a setof record of notifications, or an empty set if none received.
 * Can optionally take a named connection as parameter, but uses the unnamed
 * connection per default.
 *
 * 返回一组通知记录，如果没有收到，则返回一个空集。可以选择将命名连接作为参数，但默认使用未命名连接。
 *
 */
#define DBLINK_NOTIFY_COLS		3

PG_FUNCTION_INFO_V1(dblink_get_notify);
Datum
dblink_get_notify(PG_FUNCTION_ARGS)
{
	PGconn	   *conn;
	PGnotify   *notify;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	dblink_init();
	if (PG_NARGS() == 1)
		conn = dblink_get_named_conn(text_to_cstring(PG_GETARG_TEXT_PP(0)));
	else
		conn = pconn->conn;

	InitMaterializedSRF(fcinfo, 0);

	PQconsumeInput(conn);
	while ((notify = PQnotifies(conn)) != NULL)
	{
		Datum		values[DBLINK_NOTIFY_COLS];
		bool		nulls[DBLINK_NOTIFY_COLS];

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		if (notify->relname != NULL)
			values[0] = CStringGetTextDatum(notify->relname);
		else
			nulls[0] = true;

		values[1] = Int32GetDatum(notify->be_pid);

		if (notify->extra != NULL)
			values[2] = CStringGetTextDatum(notify->extra);
		else
			nulls[2] = true;

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);

		PQfreemem(notify);
		PQconsumeInput(conn);
	}

	return (Datum) 0;
}

/*
 * Validate the options given to a dblink foreign server or user mapping.
 * Raise an error if any option is invalid.
 *
 * 验证为 dblink 外部服务器或用户映射提供的选项。如果任何选项无效，则引发错误。
 *
 * We just check the names of options here, so semantic errors in options,
 * such as invalid numeric format, will be detected at the attempt to connect.
 *
 * 我们只是在这里检查选项的名称，因此在尝试连接时将检测到选项中的语义错误，例如无效的数字格式。
 */
PG_FUNCTION_INFO_V1(dblink_fdw_validator);
Datum
dblink_fdw_validator(PG_FUNCTION_ARGS)
{
	List	   *options_list = untransformRelOptions(PG_GETARG_DATUM(0));
	Oid			context = PG_GETARG_OID(1);
	ListCell   *cell;

	static const PQconninfoOption *options = NULL;

	/*
	 * Get list of valid libpq options.
	 *
	 * 获取有效 libpq 选项的列表。
	 *
	 * To avoid unnecessary work, we get the list once and use it throughout
	 * the lifetime of this backend process.  We don't need to care about
	 * memory context issues, because PQconndefaults allocates with malloc.
	 *
	 * 为了避免不必要的工作，我们只获取一次列表，并在后端进程的整个生命周期中使用它。  我们不需要关心内存上下文问题，因为 PQconndefaults 使用 malloc 进行分配。
	 */
	if (!options)
	{
		options = PQconndefaults();
		if (!options)			/* assume reason for failure is OOM */
			ereport(ERROR,
					(errcode(ERRCODE_FDW_OUT_OF_MEMORY),
					 errmsg("out of memory"),
					 errdetail("Could not get libpq's default connection options.")));
	}

	/* Validate each supplied option.
	 *
	 * 验证每个提供的选项。
	 */
	foreach(cell, options_list)
	{
		DefElem    *def = (DefElem *) lfirst(cell);

		if (!is_valid_dblink_fdw_option(options, def->defname, context))
		{
			/*
			 * Unknown option, or invalid option for the context specified, so
			 * complain about it.  Provide a hint with a valid option that
			 * looks similar, if there is one.
			 *
			 * 未知选项，或指定上下文的选项无效，因此请投诉。  提供一个提示，其中包含看起来相似的有效选项（如果有）。
			 */
			const PQconninfoOption *opt;
			const char *closest_match;
			ClosestMatchState match_state;
			bool		has_valid_options = false;

			initClosestMatch(&match_state, def->defname, 4);
			for (opt = options; opt->keyword; opt++)
			{
				if (is_valid_dblink_option(options, opt->keyword, context))
				{
					has_valid_options = true;
					updateClosestMatch(&match_state, opt->keyword);
				}
			}

			closest_match = getClosestMatch(&match_state);
			ereport(ERROR,
					(errcode(ERRCODE_FDW_OPTION_NAME_NOT_FOUND),
					 errmsg("invalid option \"%s\"", def->defname),
					 has_valid_options ? closest_match ?
					 errhint("Perhaps you meant the option \"%s\".",
							 closest_match) : 0 :
					 errhint("There are no valid options in this context.")));
		}
	}

	PG_RETURN_VOID();
}


/*************************************************************
 * internal functions
 *
 * 内部功能
 */


/*
 * get_pkey_attnames
 *
 * Get the primary key attnames for the given relation.
 * Return NULL, and set indnkeyatts = 0, if no primary key exists.
 *
 * 获取给定关系的主键属性名称。如果不存在主键，则返回 NULL，并设置 indnkeyatts = 0。
 */
static char **
get_pkey_attnames(Relation rel, int16 *indnkeyatts)
{
	Relation	indexRelation;
	ScanKeyData skey;
	SysScanDesc scan;
	HeapTuple	indexTuple;
	int			i;
	char	  **result = NULL;
	TupleDesc	tupdesc;

	/* initialize indnkeyatts to 0 in case no primary key exists
	 *
	 * 如果不存在主键，则将 indnkeyatts 初始化为 0
	 */
	*indnkeyatts = 0;

	tupdesc = rel->rd_att;

	/* Prepare to scan pg_index for entries having indrelid = this rel.
	 *
	 * 准备扫描 pg_index 查找具有 indrelid = this rel 的条目。
	 */
	indexRelation = table_open(IndexRelationId, AccessShareLock);
	ScanKeyInit(&skey,
				Anum_pg_index_indrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));

	scan = systable_beginscan(indexRelation, IndexIndrelidIndexId, true,
							  NULL, 1, &skey);

	while (HeapTupleIsValid(indexTuple = systable_getnext(scan)))
	{
		Form_pg_index index = (Form_pg_index) GETSTRUCT(indexTuple);

		/* we're only interested if it is the primary key
		 *
		 * 我们只对它是主键感兴趣
		 */
		if (index->indisprimary)
		{
			*indnkeyatts = index->indnkeyatts;
			if (*indnkeyatts > 0)
			{
				result = palloc_array(char *, *indnkeyatts);

				for (i = 0; i < *indnkeyatts; i++)
					result[i] = SPI_fname(tupdesc, index->indkey.values[i]);
			}
			break;
		}
	}

	systable_endscan(scan);
	table_close(indexRelation, AccessShareLock);

	return result;
}

/*
 * Deconstruct a text[] into C-strings (note any NULL elements will be
 * returned as NULL pointers)
 *
 * 将 text[] 解构为 C 字符串（注意任何 NULL 元素都将作为 NULL 指针返回）
 */
static char **
get_text_array_contents(ArrayType *array, int *numitems)
{
	int			ndim = ARR_NDIM(array);
	int		   *dims = ARR_DIMS(array);
	int			nitems;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	char	  **values;
	char	   *ptr;
	bits8	   *bitmap;
	int			bitmask;
	int			i;

	Assert(ARR_ELEMTYPE(array) == TEXTOID);

	*numitems = nitems = ArrayGetNItems(ndim, dims);

	get_typlenbyvalalign(ARR_ELEMTYPE(array),
						 &typlen, &typbyval, &typalign);

	values = palloc_array(char *, nitems);

	ptr = ARR_DATA_PTR(array);
	bitmap = ARR_NULLBITMAP(array);
	bitmask = 1;

	for (i = 0; i < nitems; i++)
	{
		if (bitmap && (*bitmap & bitmask) == 0)
		{
			values[i] = NULL;
		}
		else
		{
			values[i] = TextDatumGetCString(PointerGetDatum(ptr));
			ptr = att_addlength_pointer(ptr, typlen, ptr);
			ptr = (char *) att_align_nominal(ptr, typalign);
		}

		/* advance bitmap pointer if any
		 *
		 * 提前位图指针（如果有）
		 */
		if (bitmap)
		{
			bitmask <<= 1;
			if (bitmask == 0x100)
			{
				bitmap++;
				bitmask = 1;
			}
		}
	}

	return values;
}

static char *
get_sql_insert(Relation rel, int *pkattnums, int pknumatts, char **src_pkattvals, char **tgt_pkattvals)
{
	char	   *relname;
	HeapTuple	tuple;
	TupleDesc	tupdesc;
	int			natts;
	StringInfoData buf;
	char	   *val;
	int			key;
	int			i;
	bool		needComma;

	initStringInfo(&buf);

	/* get relation name including any needed schema prefix and quoting
	 *
	 * 获取关系名称，包括任何所需的模式前缀和引用
	 */
	relname = generate_relation_name(rel);

	tupdesc = rel->rd_att;
	natts = tupdesc->natts;

	tuple = get_tuple_of_interest(rel, pkattnums, pknumatts, src_pkattvals);
	if (!tuple)
		ereport(ERROR,
				(errcode(ERRCODE_CARDINALITY_VIOLATION),
				 errmsg("source row not found")));

	appendStringInfo(&buf, "INSERT INTO %s(", relname);

	needComma = false;
	for (i = 0; i < natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		if (att->attisdropped)
			continue;

		if (needComma)
			appendStringInfoChar(&buf, ',');

		appendStringInfoString(&buf,
							   quote_ident_cstr(NameStr(att->attname)));
		needComma = true;
	}

	appendStringInfoString(&buf, ") VALUES(");

	/*
	 * Note: i is physical column number (counting from 0).
	 *
	 * 注：i为物理列号（从0开始计数）。
	 */
	needComma = false;
	for (i = 0; i < natts; i++)
	{
		if (TupleDescAttr(tupdesc, i)->attisdropped)
			continue;

		if (needComma)
			appendStringInfoChar(&buf, ',');

		key = get_attnum_pk_pos(pkattnums, pknumatts, i);

		if (key >= 0)
			val = tgt_pkattvals[key] ? pstrdup(tgt_pkattvals[key]) : NULL;
		else
			val = SPI_getvalue(tuple, tupdesc, i + 1);

		if (val != NULL)
		{
			appendStringInfoString(&buf, quote_literal_cstr(val));
			pfree(val);
		}
		else
			appendStringInfoString(&buf, "NULL");
		needComma = true;
	}
	appendStringInfoChar(&buf, ')');

	return buf.data;
}

static char *
get_sql_delete(Relation rel, int *pkattnums, int pknumatts, char **tgt_pkattvals)
{
	char	   *relname;
	TupleDesc	tupdesc;
	StringInfoData buf;
	int			i;

	initStringInfo(&buf);

	/* get relation name including any needed schema prefix and quoting
	 *
	 * 获取关系名称，包括任何所需的模式前缀和引用
	 */
	relname = generate_relation_name(rel);

	tupdesc = rel->rd_att;

	appendStringInfo(&buf, "DELETE FROM %s WHERE ", relname);
	for (i = 0; i < pknumatts; i++)
	{
		int			pkattnum = pkattnums[i];
		Form_pg_attribute attr = TupleDescAttr(tupdesc, pkattnum);

		if (i > 0)
			appendStringInfoString(&buf, " AND ");

		appendStringInfoString(&buf,
							   quote_ident_cstr(NameStr(attr->attname)));

		if (tgt_pkattvals[i] != NULL)
			appendStringInfo(&buf, " = %s",
							 quote_literal_cstr(tgt_pkattvals[i]));
		else
			appendStringInfoString(&buf, " IS NULL");
	}

	return buf.data;
}

static char *
get_sql_update(Relation rel, int *pkattnums, int pknumatts, char **src_pkattvals, char **tgt_pkattvals)
{
	char	   *relname;
	HeapTuple	tuple;
	TupleDesc	tupdesc;
	int			natts;
	StringInfoData buf;
	char	   *val;
	int			key;
	int			i;
	bool		needComma;

	initStringInfo(&buf);

	/* get relation name including any needed schema prefix and quoting
	 *
	 * 获取关系名称，包括任何所需的模式前缀和引用
	 */
	relname = generate_relation_name(rel);

	tupdesc = rel->rd_att;
	natts = tupdesc->natts;

	tuple = get_tuple_of_interest(rel, pkattnums, pknumatts, src_pkattvals);
	if (!tuple)
		ereport(ERROR,
				(errcode(ERRCODE_CARDINALITY_VIOLATION),
				 errmsg("source row not found")));

	appendStringInfo(&buf, "UPDATE %s SET ", relname);

	/*
	 * Note: i is physical column number (counting from 0).
	 *
	 * 注：i为物理列号（从0开始计数）。
	 */
	needComma = false;
	for (i = 0; i < natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

		if (attr->attisdropped)
			continue;

		if (needComma)
			appendStringInfoString(&buf, ", ");

		appendStringInfo(&buf, "%s = ",
						 quote_ident_cstr(NameStr(attr->attname)));

		key = get_attnum_pk_pos(pkattnums, pknumatts, i);

		if (key >= 0)
			val = tgt_pkattvals[key] ? pstrdup(tgt_pkattvals[key]) : NULL;
		else
			val = SPI_getvalue(tuple, tupdesc, i + 1);

		if (val != NULL)
		{
			appendStringInfoString(&buf, quote_literal_cstr(val));
			pfree(val);
		}
		else
			appendStringInfoString(&buf, "NULL");
		needComma = true;
	}

	appendStringInfoString(&buf, " WHERE ");

	for (i = 0; i < pknumatts; i++)
	{
		int			pkattnum = pkattnums[i];
		Form_pg_attribute attr = TupleDescAttr(tupdesc, pkattnum);

		if (i > 0)
			appendStringInfoString(&buf, " AND ");

		appendStringInfoString(&buf,
							   quote_ident_cstr(NameStr(attr->attname)));

		val = tgt_pkattvals[i];

		if (val != NULL)
			appendStringInfo(&buf, " = %s", quote_literal_cstr(val));
		else
			appendStringInfoString(&buf, " IS NULL");
	}

	return buf.data;
}

/*
 * Return a properly quoted identifier.
 * Uses quote_ident in quote.c
 *
 * 返回正确引用的标识符。在 quote.c 中使用 quote_ident
 */
static char *
quote_ident_cstr(char *rawstr)
{
	text	   *rawstr_text;
	text	   *result_text;
	char	   *result;

	rawstr_text = cstring_to_text(rawstr);
	result_text = DatumGetTextPP(DirectFunctionCall1(quote_ident,
													 PointerGetDatum(rawstr_text)));
	result = text_to_cstring(result_text);

	return result;
}

static int
get_attnum_pk_pos(int *pkattnums, int pknumatts, int key)
{
	int			i;

	/*
	 * Not likely a long list anyway, so just scan for the value
	 *
	 * 无论如何，列表不太可能很长，所以只需扫描该值即可
	 */
	for (i = 0; i < pknumatts; i++)
		if (key == pkattnums[i])
			return i;

	return -1;
}

static HeapTuple
get_tuple_of_interest(Relation rel, int *pkattnums, int pknumatts, char **src_pkattvals)
{
	char	   *relname;
	TupleDesc	tupdesc;
	int			natts;
	StringInfoData buf;
	int			ret;
	HeapTuple	tuple;
	int			i;

	/*
	 * Connect to SPI manager
	 *
	 * 连接到 SPI 管理器
	 */
	SPI_connect();

	initStringInfo(&buf);

	/* get relation name including any needed schema prefix and quoting
	 *
	 * 获取关系名称，包括任何所需的模式前缀和引用
	 */
	relname = generate_relation_name(rel);

	tupdesc = rel->rd_att;
	natts = tupdesc->natts;

	/*
	 * Build sql statement to look up tuple of interest, ie, the one matching
	 * src_pkattvals.  We used to use "SELECT *" here, but it's simpler to
	 * generate a result tuple that matches the table's physical structure,
	 * with NULLs for any dropped columns.  Otherwise we have to deal with two
	 * different tupdescs and everything's very confusing.
	 *
	 * 构建 sql 语句来查找感兴趣的元组，即匹配 src_pkattvals 的元组。  我们曾经在这里使用“SELECT *”，但生成与表的物理结构匹配的结果元组更简单，并且任何删除的列都为 NULL。  否则我们必须处理两个不同的 tupdesc，一切都会变得非常混乱。
	 */
	appendStringInfoString(&buf, "SELECT ");

	for (i = 0; i < natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

		if (i > 0)
			appendStringInfoString(&buf, ", ");

		if (attr->attisdropped)
			appendStringInfoString(&buf, "NULL");
		else
			appendStringInfoString(&buf,
								   quote_ident_cstr(NameStr(attr->attname)));
	}

	appendStringInfo(&buf, " FROM %s WHERE ", relname);

	for (i = 0; i < pknumatts; i++)
	{
		int			pkattnum = pkattnums[i];
		Form_pg_attribute attr = TupleDescAttr(tupdesc, pkattnum);

		if (i > 0)
			appendStringInfoString(&buf, " AND ");

		appendStringInfoString(&buf,
							   quote_ident_cstr(NameStr(attr->attname)));

		if (src_pkattvals[i] != NULL)
			appendStringInfo(&buf, " = %s",
							 quote_literal_cstr(src_pkattvals[i]));
		else
			appendStringInfoString(&buf, " IS NULL");
	}

	/*
	 * Retrieve the desired tuple
	 *
	 * 检索所需的元组
	 */
	ret = SPI_exec(buf.data, 0);
	pfree(buf.data);

	/*
	 * Only allow one qualifying tuple
	 *
	 * 只允许一个符合条件的元组
	 */
	if ((ret == SPI_OK_SELECT) && (SPI_processed > 1))
		ereport(ERROR,
				(errcode(ERRCODE_CARDINALITY_VIOLATION),
				 errmsg("source criteria matched more than one record")));

	else if (ret == SPI_OK_SELECT && SPI_processed == 1)
	{
		SPITupleTable *tuptable = SPI_tuptable;

		tuple = SPI_copytuple(tuptable->vals[0]);
		SPI_finish();

		return tuple;
	}
	else
	{
		/*
		 * no qualifying tuples
		 *
		 * 没有合格的元组
		 */
		SPI_finish();

		return NULL;
	}

	/*
	 * never reached, but keep compiler quiet
	 *
	 * 从未达到，但保持编译器安静
	 */
	return NULL;
}

/*
 * Open the relation named by relname_text, acquire specified type of lock,
 * verify we have specified permissions.
 * Caller must close rel when done with it.
 *
 * 打开relname_text命名的关系，获取指定类型的锁，验证我们是否具有指定的权限。调用者必须在完成后关闭 rel。
 */
static Relation
get_rel_from_relname(text *relname_text, LOCKMODE lockmode, AclMode aclmode)
{
	RangeVar   *relvar;
	Relation	rel;
	AclResult	aclresult;

	relvar = makeRangeVarFromNameList(textToQualifiedNameList(relname_text));
	rel = table_openrv(relvar, lockmode);

	aclresult = pg_class_aclcheck(RelationGetRelid(rel), GetUserId(),
								  aclmode);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, get_relkind_objtype(rel->rd_rel->relkind),
					   RelationGetRelationName(rel));

	return rel;
}

/*
 * generate_relation_name - copied from ruleutils.c
 *		Compute the name to display for a relation
 *
 * generate_relation_name - 从ruleutils.c复制计算要显示的关系的名称
 *
 * The result includes all necessary quoting and schema-prefixing.
 *
 * 结果包括所有必要的引用和模式前缀。
 */
static char *
generate_relation_name(Relation rel)
{
	char	   *nspname;
	char	   *result;

	/* Qualify the name if not visible in search path
	 *
	 * 如果在搜索路径中不可见，则限定名称
	 */
	if (RelationIsVisible(RelationGetRelid(rel)))
		nspname = NULL;
	else
		nspname = get_namespace_name(rel->rd_rel->relnamespace);

	result = quote_qualified_identifier(nspname, RelationGetRelationName(rel));

	return result;
}


static remoteConn *
getConnectionByName(const char *name)
{
	remoteConnHashEnt *hentry;
	char	   *key;

	if (!remoteConnHash)
		remoteConnHash = createConnHash();

	key = pstrdup(name);
	truncate_identifier(key, strlen(key), false);
	hentry = (remoteConnHashEnt *) hash_search(remoteConnHash,
											   key, HASH_FIND, NULL);

	if (hentry && hentry->rconn.conn != NULL)
		return &hentry->rconn;

	return NULL;
}

static HTAB *
createConnHash(void)
{
	HASHCTL		ctl;

	ctl.keysize = NAMEDATALEN;
	ctl.entrysize = sizeof(remoteConnHashEnt);

	return hash_create("Remote Con hash", NUMCONN, &ctl,
					   HASH_ELEM | HASH_STRINGS);
}

static remoteConn *
createNewConnection(const char *name)
{
	remoteConnHashEnt *hentry;
	bool		found;
	char	   *key;

	if (!remoteConnHash)
		remoteConnHash = createConnHash();

	key = pstrdup(name);
	truncate_identifier(key, strlen(key), true);
	hentry = (remoteConnHashEnt *) hash_search(remoteConnHash, key,
											   HASH_ENTER, &found);

	if (found && hentry->rconn.conn != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("duplicate connection name")));

	/* New, or reusable, so initialize the rconn struct to zeroes
	 *
	 * 新的或可重用的，因此将 rconn 结构初始化为零
	 */
	memset(&hentry->rconn, 0, sizeof(remoteConn));

	return &hentry->rconn;
}

static void
deleteConnection(const char *name)
{
	remoteConnHashEnt *hentry;
	bool		found;
	char	   *key;

	if (!remoteConnHash)
		remoteConnHash = createConnHash();

	key = pstrdup(name);
	truncate_identifier(key, strlen(key), false);
	hentry = (remoteConnHashEnt *) hash_search(remoteConnHash,
											   key, HASH_REMOVE, &found);

	if (!hentry)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("undefined connection name")));
}

 /*
  * Ensure that require_auth and SCRAM keys are correctly set on connstr.
  * SCRAM keys used to pass-through are coming from the initial connection
  * from the client with the server.
  *
  * 确保在 connstr 上正确设置 require_auth 和 SCRAM 密钥。用于传递的 SCRAM 密钥来自客户端与服务器的初始连接。
  *
  * All required SCRAM options are set by dblink, so we just need to ensure
  * that these options are not overwritten by the user.
  *
  * 所有必需的 SCRAM 选项均由 dblink 设置，因此我们只需确保这些选项不被用户覆盖即可。
  *
  * See appendSCRAMKeysInfo and its usage for more.
  *
  * 有关更多信息，请参阅appendSCRAMKeysInfo 及其用法。
  */
bool
dblink_connstr_has_required_scram_options(const char *connstr)
{
	PQconninfoOption *options;
	bool		has_scram_server_key = false;
	bool		has_scram_client_key = false;
	bool		has_require_auth = false;
	bool		has_scram_keys = false;

	options = PQconninfoParse(connstr, NULL);
	if (options)
	{
		/*
		 * Continue iterating even if we found the keys that we need to
		 * validate to make sure that there is no other declaration of these
		 * keys that can overwrite the first.
		 *
		 * 即使我们找到了需要验证的键，也要继续迭代，以确保这些键没有其他声明可以覆盖第一个。
		 */
		for (PQconninfoOption *option = options; option->keyword != NULL; option++)
		{
			if (strcmp(option->keyword, "require_auth") == 0)
			{
				if (option->val != NULL && strcmp(option->val, "scram-sha-256") == 0)
					has_require_auth = true;
				else
					has_require_auth = false;
			}

			if (strcmp(option->keyword, "scram_client_key") == 0)
			{
				if (option->val != NULL && option->val[0] != '\0')
					has_scram_client_key = true;
				else
					has_scram_client_key = false;
			}

			if (strcmp(option->keyword, "scram_server_key") == 0)
			{
				if (option->val != NULL && option->val[0] != '\0')
					has_scram_server_key = true;
				else
					has_scram_server_key = false;
			}
		}
		PQconninfoFree(options);
	}

	has_scram_keys = has_scram_client_key && has_scram_server_key && MyProcPort != NULL && MyProcPort->has_scram_keys;

	return (has_scram_keys && has_require_auth);
}

/*
 * We need to make sure that the connection made used credentials
 * which were provided by the user, so check what credentials were
 * used to connect and then make sure that they came from the user.
 *
 * 我们需要确保连接使用了用户提供的凭据，因此请检查用于连接的凭据，然后确保它们来自用户。
 *
 * On failure, we close "conn" and also delete the hashtable entry
 * identified by "connname" (if that's not NULL).
 *
 * 失败时，我们关闭“conn”，并删除由“connname”标识的哈希表条目（如果它不为 NULL）。
 */
static void
dblink_security_check(PGconn *conn, const char *connname, const char *connstr)
{
	/* Superuser bypasses security check
	 *
	 * 超级用户绕过安全检查
	 */
	if (superuser())
		return;

	/* If password was used to connect, make sure it was one provided
	 *
	 * 如果使用密码进行连接，请确保已提供密码
	 */
	if (PQconnectionUsedPassword(conn) && dblink_connstr_has_pw(connstr))
		return;

	/*
	 * Password was not used to connect, check if SCRAM pass-through is in
	 * use.
	 *
	 * 未使用密码进行连接，请检查 SCRAM 直通是否正在使用。
	 *
	 * If dblink_connstr_has_required_scram_options is true we assume that
	 * UseScramPassthrough is also true because the required SCRAM keys are
	 * only added if UseScramPassthrough is set, and the user is not allowed
	 * to add the SCRAM keys on fdw and user mapping options.
	 *
	 * 如果 dblink_connstr_has_required_scram_options 为 true，我们假设 UseScramPassthrough 也为 true，因为仅当设置了 UseScramPassthrough 时才会添加所需的 SCRAM 密钥，并且不允许用户在 fdw 和用户映射选项上添加 SCRAM 密钥。
	 */
	if (MyProcPort != NULL && MyProcPort->has_scram_keys && dblink_connstr_has_required_scram_options(connstr))
		return;

#ifdef ENABLE_GSS
	/* If GSSAPI creds used to connect, make sure it was one delegated
	 *
	 * 如果使用 GSSAPI 信用进行连接，请确保它是委托的
	 */
	if (PQconnectionUsedGSSAPI(conn) && be_gssapi_get_delegation(MyProcPort))
		return;
#endif

	/* Otherwise, fail out
	 *
	 * 否则失败
	 */
	libpqsrv_disconnect(conn);
	if (connname)
		deleteConnection(connname);

	ereport(ERROR,
			(errcode(ERRCODE_S_R_E_PROHIBITED_SQL_STATEMENT_ATTEMPTED),
			 errmsg("password or GSSAPI delegated credentials required"),
			 errdetail("Non-superusers may only connect using credentials they provide, eg: password in connection string or delegated GSSAPI credentials"),
			 errhint("Ensure provided credentials match target server's authentication method.")));
}

/*
 * Function to check if the connection string includes an explicit
 * password, needed to ensure that non-superuser password-based auth
 * is using a provided password and not one picked up from the
 * environment.
 *
 * 检查连接字符串是否包含显式密码的函数，需要确保基于非超级用户密码的身份验证使用提供的密码，而不是从环境中获取的密码。
 */
static bool
dblink_connstr_has_pw(const char *connstr)
{
	PQconninfoOption *options;
	PQconninfoOption *option;
	bool		connstr_gives_password = false;

	options = PQconninfoParse(connstr, NULL);
	if (options)
	{
		for (option = options; option->keyword != NULL; option++)
		{
			if (strcmp(option->keyword, "password") == 0)
			{
				if (option->val != NULL && option->val[0] != '\0')
				{
					connstr_gives_password = true;
					break;
				}
			}
		}
		PQconninfoFree(options);
	}

	return connstr_gives_password;
}

/*
 * For non-superusers, insist that the connstr specify a password, except if
 * GSSAPI credentials have been delegated (and we check that they are used for
 * the connection in dblink_security_check later) or if SCRAM pass-through is
 * being used.  This prevents a password or GSSAPI credentials from being
 * picked up from .pgpass, a service file, the environment, etc.  We don't want
 * the postgres user's passwords or Kerberos credentials to be accessible to
 * non-superusers. In case of SCRAM pass-through insist that the connstr
 * has the required SCRAM pass-through options.
 *
 * 对于非超级用户，坚持要求 connstr 指定密码，除非已委派 GSSAPI 凭据（并且我们稍后检查它们是否用于 dblink_security_check 中的连接）或者是否正在使用 SCRAM 直通。  这可以防止从 .pgpass、服务文件、环境等中获取密码或 GSSAPI 凭据。我们不希望非超级用户可以访问 postgres 用户的密码或 Kerberos 凭据。如果是 SCRAM 直通，请坚持 connstr 具有所需的 SCRAM 直通选项。
 */
static void
dblink_connstr_check(const char *connstr)
{
	if (superuser())
		return;

	if (dblink_connstr_has_pw(connstr))
		return;

	if (MyProcPort != NULL && MyProcPort->has_scram_keys && dblink_connstr_has_required_scram_options(connstr))
		return;

#ifdef ENABLE_GSS
	if (be_gssapi_get_delegation(MyProcPort))
		return;
#endif

	ereport(ERROR,
			(errcode(ERRCODE_S_R_E_PROHIBITED_SQL_STATEMENT_ATTEMPTED),
			 errmsg("password or GSSAPI delegated credentials required"),
			 errdetail("Non-superusers must provide a password in the connection string or send delegated GSSAPI credentials.")));
}

/*
 * Report an error received from the remote server
 *
 * 报告从远程服务器收到的错误
 *
 * res: the received error result (will be freed)
 * fail: true for ERROR ereport, false for NOTICE
 * fmt and following args: sprintf-style format and values for errcontext;
 * the resulting string should be worded like "while <some action>"
 *
 * res：接收到的错误结果（将被释放）失败：对于 ERROR ereport 为 true，对于 NOTICE fmt 为 false，以及以下参数：sprintf 样式格式和 errcontext 的值；结果字符串的措辞应类似于“while <some action>”
 */
static void
dblink_res_error(PGconn *conn, const char *conname, PGresult *res,
				 bool fail, const char *fmt,...)
{
	int			level;
	char	   *pg_diag_sqlstate = PQresultErrorField(res, PG_DIAG_SQLSTATE);
	char	   *pg_diag_message_primary = PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY);
	char	   *pg_diag_message_detail = PQresultErrorField(res, PG_DIAG_MESSAGE_DETAIL);
	char	   *pg_diag_message_hint = PQresultErrorField(res, PG_DIAG_MESSAGE_HINT);
	char	   *pg_diag_context = PQresultErrorField(res, PG_DIAG_CONTEXT);
	int			sqlstate;
	char	   *message_primary;
	char	   *message_detail;
	char	   *message_hint;
	char	   *message_context;
	va_list		ap;
	char		dblink_context_msg[512];

	if (fail)
		level = ERROR;
	else
		level = NOTICE;

	if (pg_diag_sqlstate)
		sqlstate = MAKE_SQLSTATE(pg_diag_sqlstate[0],
								 pg_diag_sqlstate[1],
								 pg_diag_sqlstate[2],
								 pg_diag_sqlstate[3],
								 pg_diag_sqlstate[4]);
	else
		sqlstate = ERRCODE_CONNECTION_FAILURE;

	message_primary = xpstrdup(pg_diag_message_primary);
	message_detail = xpstrdup(pg_diag_message_detail);
	message_hint = xpstrdup(pg_diag_message_hint);
	message_context = xpstrdup(pg_diag_context);

	/*
	 * If we don't get a message from the PGresult, try the PGconn.  This is
	 * needed because for connection-level failures, PQgetResult may just
	 * return NULL, not a PGresult at all.
	 *
	 * 如果我们没有从 PGresult 收到消息，请尝试 PGconn。  这是必需的，因为对于连接级故障，PQgetResult 可能只返回 NULL，而不是 PGresult。
	 */
	if (message_primary == NULL)
		message_primary = pchomp(PQerrorMessage(conn));

	/*
	 * Now that we've copied all the data we need out of the PGresult, it's
	 * safe to free it.  We must do this to avoid PGresult leakage.  We're
	 * leaking all the strings too, but those are in palloc'd memory that will
	 * get cleaned up eventually.
	 *
	 * 现在我们已经从 PGresult 中复制了所需的所有数据，可以安全地释放它了。  我们必须这样做以避免 PGresult 泄漏。  我们也泄漏了所有字符串，但这些字符串位于已分配的内存中，最终会被清除。
	 */
	PQclear(res);

	/*
	 * Format the basic errcontext string.  Below, we'll add on something
	 * about the connection name.  That's a violation of the translatability
	 * guidelines about constructing error messages out of parts, but since
	 * there's no translation support for dblink, there's no need to worry
	 * about that (yet).
	 *
	 * 设置基本 errcontext 字符串的格式。  下面，我们将添加有关连接名称的内容。  这违反了关于用部件构造错误消息的可翻译性准则，但由于 dblink 没有翻译支持，因此（目前）还无需担心这一点。
	 */
	va_start(ap, fmt);
	vsnprintf(dblink_context_msg, sizeof(dblink_context_msg), fmt, ap);
	va_end(ap);

	ereport(level,
			(errcode(sqlstate),
			 (message_primary != NULL && message_primary[0] != '\0') ?
			 errmsg_internal("%s", message_primary) :
			 errmsg("could not obtain message string for remote error"),
			 message_detail ? errdetail_internal("%s", message_detail) : 0,
			 message_hint ? errhint("%s", message_hint) : 0,
			 message_context ? (errcontext("%s", message_context)) : 0,
			 conname ?
			 (errcontext("%s on dblink connection named \"%s\"",
						 dblink_context_msg, conname)) :
			 (errcontext("%s on unnamed dblink connection",
						 dblink_context_msg))));
}

/*
 * Obtain connection string for a foreign server
 *
 * 获取外部服务器的连接字符串
 */
static char *
get_connect_string(const char *servername)
{
	ForeignServer *foreign_server = NULL;
	UserMapping *user_mapping;
	ListCell   *cell;
	StringInfoData buf;
	ForeignDataWrapper *fdw;
	AclResult	aclresult;
	char	   *srvname;

	static const PQconninfoOption *options = NULL;

	initStringInfo(&buf);

	/*
	 * Get list of valid libpq options.
	 *
	 * 获取有效 libpq 选项的列表。
	 *
	 * To avoid unnecessary work, we get the list once and use it throughout
	 * the lifetime of this backend process.  We don't need to care about
	 * memory context issues, because PQconndefaults allocates with malloc.
	 *
	 * 为了避免不必要的工作，我们只获取一次列表，并在后端进程的整个生命周期中使用它。  我们不需要关心内存上下文问题，因为 PQconndefaults 使用 malloc 进行分配。
	 */
	if (!options)
	{
		options = PQconndefaults();
		if (!options)			/* assume reason for failure is OOM */
			ereport(ERROR,
					(errcode(ERRCODE_FDW_OUT_OF_MEMORY),
					 errmsg("out of memory"),
					 errdetail("Could not get libpq's default connection options.")));
	}

	/* first gather the server connstr options
	 *
	 * 首先收集服务器 connstr 选项
	 */
	srvname = pstrdup(servername);
	truncate_identifier(srvname, strlen(srvname), false);
	foreign_server = GetForeignServerByName(srvname, true);

	if (foreign_server)
	{
		Oid			serverid = foreign_server->serverid;
		Oid			fdwid = foreign_server->fdwid;
		Oid			userid = GetUserId();

		user_mapping = GetUserMapping(userid, serverid);
		fdw = GetForeignDataWrapper(fdwid);

		/* Check permissions, user must have usage on the server.
		 *
		 * 检查权限，用户必须在服务器上有使用权。
		 */
		aclresult = object_aclcheck(ForeignServerRelationId, serverid, userid, ACL_USAGE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_FOREIGN_SERVER, foreign_server->servername);

		/*
		 * First append hardcoded options needed for SCRAM pass-through, so if
		 * the user overwrites these options we can ereport on
		 * dblink_connstr_check and dblink_security_check.
		 *
		 * 首先附加 SCRAM 传递所需的硬编码选项，因此如果用户覆盖这些选项，我们可以在 dblink_connstr_check 和 dblink_security_check 上报告。
		 */
		if (MyProcPort != NULL && MyProcPort->has_scram_keys && UseScramPassthrough(foreign_server, user_mapping))
			appendSCRAMKeysInfo(&buf);

		foreach(cell, fdw->options)
		{
			DefElem    *def = lfirst(cell);

			if (is_valid_dblink_option(options, def->defname, ForeignDataWrapperRelationId))
				appendStringInfo(&buf, "%s='%s' ", def->defname,
								 escape_param_str(strVal(def->arg)));
		}

		foreach(cell, foreign_server->options)
		{
			DefElem    *def = lfirst(cell);

			if (is_valid_dblink_option(options, def->defname, ForeignServerRelationId))
				appendStringInfo(&buf, "%s='%s' ", def->defname,
								 escape_param_str(strVal(def->arg)));
		}

		foreach(cell, user_mapping->options)
		{

			DefElem    *def = lfirst(cell);

			if (is_valid_dblink_option(options, def->defname, UserMappingRelationId))
				appendStringInfo(&buf, "%s='%s' ", def->defname,
								 escape_param_str(strVal(def->arg)));
		}

		return buf.data;
	}
	else
		return NULL;
}

/*
 * Escaping libpq connect parameter strings.
 *
 * 转义 libpq 连接参数字符串。
 *
 * Replaces "'" with "\'" and "\" with "\\".
 *
 * 将“'”替换为“\'”，将“\”替换为“\\”。
 */
static char *
escape_param_str(const char *str)
{
	const char *cp;
	StringInfoData buf;

	initStringInfo(&buf);

	for (cp = str; *cp; cp++)
	{
		if (*cp == '\\' || *cp == '\'')
			appendStringInfoChar(&buf, '\\');
		appendStringInfoChar(&buf, *cp);
	}

	return buf.data;
}

/*
 * Validate the PK-attnums argument for dblink_build_sql_insert() and related
 * functions, and translate to the internal representation.
 *
 * 验证 dblink_build_sql_insert() 和相关函数的 PK-attnums 参数，并转换为内部表示形式。
 *
 * The user supplies an int2vector of 1-based logical attnums, plus a count
 * argument (the need for the separate count argument is historical, but we
 * still check it).  We check that each attnum corresponds to a valid,
 * non-dropped attribute of the rel.  We do *not* prevent attnums from being
 * listed twice, though the actual use-case for such things is dubious.
 * Note that before Postgres 9.0, the user's attnums were interpreted as
 * physical not logical column numbers; this was changed for future-proofing.
 *
 * 用户提供一个基于 1 的逻辑属性的 int2vector，加上一个计数参数（历史上需要单独的计数参数，但我们仍然检查它）。  我们检查每个 attnum 是否对应于 rel 的一个有效的、未删除的属性。  我们*不*阻止 attnums 被列出两次，尽管此类事情的实际用例是可疑的。请注意，在 Postgres 9.0 之前，用户的 attnums 被解释为物理而不是逻辑列号；这是为了面向未来而改变的。
 *
 * The internal representation is a palloc'd int array of 0-based physical
 * attnums.
 *
 * 内部表示是一个从 0 开始的物理属性的 palloc'd int 数组。
 */
static void
validate_pkattnums(Relation rel,
				   int2vector *pkattnums_arg, int32 pknumatts_arg,
				   int **pkattnums, int *pknumatts)
{
	TupleDesc	tupdesc = rel->rd_att;
	int			natts = tupdesc->natts;
	int			i;

	/* Don't take more array elements than there are
	 *
	 * 不要获取比实际数量更多的数组元素
	 */
	pknumatts_arg = Min(pknumatts_arg, pkattnums_arg->dim1);

	/* Must have at least one pk attnum selected
	 *
	 * 必须至少选择一项PK属性
	 */
	if (pknumatts_arg <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("number of key attributes must be > 0")));

	/* Allocate output array
	 *
	 * 分配输出数组
	 */
	*pkattnums = palloc_array(int, pknumatts_arg);
	*pknumatts = pknumatts_arg;

	/* Validate attnums and convert to internal form
	 *
	 * 验证 attnums 并转换为内部形式
	 */
	for (i = 0; i < pknumatts_arg; i++)
	{
		int			pkattnum = pkattnums_arg->values[i];
		int			lnum;
		int			j;

		/* Can throw error immediately if out of range
		 *
		 * 如果超出范围可以立即抛出错误
		 */
		if (pkattnum <= 0 || pkattnum > natts)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid attribute number %d", pkattnum)));

		/* Identify which physical column has this logical number
		 *
		 * 确定哪个物理列具有该逻辑编号
		 */
		lnum = 0;
		for (j = 0; j < natts; j++)
		{
			/* dropped columns don't count
			 *
			 * 删除的列不算数
			 */
			if (TupleDescAttr(tupdesc, j)->attisdropped)
				continue;

			if (++lnum == pkattnum)
				break;
		}

		if (j < natts)
			(*pkattnums)[i] = j;
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid attribute number %d", pkattnum)));
	}
}

/*
 * Check if the specified connection option is valid.
 *
 * 检查指定的连接选项是否有效。
 *
 * We basically allow whatever libpq thinks is an option, with these
 * restrictions:
 *		debug options: disallowed
 *		"client_encoding": disallowed
 *		"user": valid only in USER MAPPING options
 *		secure options (eg password): valid only in USER MAPPING options
 *		others: valid only in FOREIGN SERVER options
 *
 * 我们基本上允许 libpq 认为的任何选项，但有以下限制： 调试选项：不允许的“client_encoding”：不允许的“user”：仅在 USER MAPPING 选项中有效 安全选项（例如密码）：仅在 USER MAPPING 选项中有效 其他：仅在 FOREIGN SERVER 选项中有效
 *
 * We disallow client_encoding because it would be overridden anyway via
 * PQclientEncoding; allowing it to be specified would merely promote
 * confusion.
 *
 * 我们不允许 client_encoding，因为它无论如何都会通过 PQclientEncoding 被覆盖；允许对其进行具体说明只会造成混乱。
 */
static bool
is_valid_dblink_option(const PQconninfoOption *options, const char *option,
					   Oid context)
{
	const PQconninfoOption *opt;

	/* Look up the option in libpq result
	 *
	 * 在 libpq 结果中查找选项
	 */
	for (opt = options; opt->keyword; opt++)
	{
		if (strcmp(opt->keyword, option) == 0)
			break;
	}
	if (opt->keyword == NULL)
		return false;

	/* Disallow debug options (particularly "replication")
	 *
	 * 禁止调试选项（特别是“复制”）
	 */
	if (strchr(opt->dispchar, 'D'))
		return false;

	/* Disallow "client_encoding"
	 *
	 * 禁止“client_encoding”
	 */
	if (strcmp(opt->keyword, "client_encoding") == 0)
		return false;

	/*
	 * Disallow OAuth options for now, since the builtin flow communicates on
	 * stderr by default and can't cache tokens yet.
	 *
	 * 目前禁止 OAuth 选项，因为默认情况下内置流在 stderr 上进行通信并且尚无法缓存令牌。
	 */
	if (strncmp(opt->keyword, "oauth_", strlen("oauth_")) == 0)
		return false;

	/*
	 * If the option is "user" or marked secure, it should be specified only
	 * in USER MAPPING.  Others should be specified only in SERVER.
	 *
	 * 如果选项是“user”或标记为安全，则应仅在 USER MAPPING 中指定。  其他的只能在 SERVER 中指定。
	 */
	if (strcmp(opt->keyword, "user") == 0 || strchr(opt->dispchar, '*'))
	{
		if (context != UserMappingRelationId)
			return false;
	}
	else
	{
		if (context != ForeignServerRelationId)
			return false;
	}

	return true;
}

/*
 * Same as is_valid_dblink_option but also check for only dblink_fdw specific
 * options.
 *
 * 与 is_valid_dblink_option 相同，但也仅检查 dblink_fdw 特定选项。
 */
static bool
is_valid_dblink_fdw_option(const PQconninfoOption *options, const char *option,
						   Oid context)
{
	if (strcmp(option, "use_scram_passthrough") == 0)
		return true;

	return is_valid_dblink_option(options, option, context);
}

/*
 * Copy the remote session's values of GUCs that affect datatype I/O
 * and apply them locally in a new GUC nesting level.  Returns the new
 * nestlevel (which is needed by restoreLocalGucs to undo the settings),
 * or -1 if no new nestlevel was needed.
 *
 * 复制影响数据类型 I/O 的 GUC 远程会话值，并将它们本地应用到新的 GUC 嵌套级别中。  返回新的嵌套级别（restoreLocalGucs 需要它来撤消设置），如果不需要新的嵌套级别，则返回 -1。
 *
 * We use the equivalent of a function SET option to allow the settings to
 * persist only until the caller calls restoreLocalGucs.  If an error is
 * thrown in between, guc.c will take care of undoing the settings.
 *
 * 我们使用函数 SET 选项的等效项来允许设置仅持续到调用者调用 RestoreLocalGucs 为止。  如果中间抛出错误，guc.c 将负责撤消设置。
 */
static int
applyRemoteGucs(PGconn *conn)
{
	static const char *const GUCsAffectingIO[] = {
		"DateStyle",
		"IntervalStyle"
	};

	int			nestlevel = -1;
	int			i;

	for (i = 0; i < lengthof(GUCsAffectingIO); i++)
	{
		const char *gucName = GUCsAffectingIO[i];
		const char *remoteVal = PQparameterStatus(conn, gucName);
		const char *localVal;

		/*
		 * If the remote server is pre-8.4, it won't have IntervalStyle, but
		 * that's okay because its output format won't be ambiguous.  So just
		 * skip the GUC if we don't get a value for it.  (We might eventually
		 * need more complicated logic with remote-version checks here.)
		 *
		 * 如果远程服务器是 8.4 之前的版本，则它不会有 IntervalStyle，但这没关系，因为它的输出格式不会含糊不清。  因此，如果我们没有得到 GUC 的值，就跳过它。  （我们最终可能需要更复杂的逻辑来进行远程版本检查。）
		 */
		if (remoteVal == NULL)
			continue;

		/*
		 * Avoid GUC-setting overhead if the remote and local GUCs already
		 * have the same value.
		 *
		 * 如果远程和本地 GUC 已具有相同的值，请避免 GUC 设置开销。
		 */
		localVal = GetConfigOption(gucName, false, false);
		Assert(localVal != NULL);

		if (strcmp(remoteVal, localVal) == 0)
			continue;

		/* Create new GUC nest level if we didn't already
		 *
		 * 如果我们还没有创建新的 GUC 嵌套级别，请创建新的 GUC 嵌套级别
		 */
		if (nestlevel < 0)
			nestlevel = NewGUCNestLevel();

		/* Apply the option (this will throw error on failure)
		 *
		 * 应用该选项（失败时会抛出错误）
		 */
		(void) set_config_option(gucName, remoteVal,
								 PGC_USERSET, PGC_S_SESSION,
								 GUC_ACTION_SAVE, true, 0, false);
	}

	return nestlevel;
}

/*
 * Restore local GUCs after they have been overlaid with remote settings.
 *
 * 在本地 GUC 被远程设置覆盖后恢复。
 */
static void
restoreLocalGucs(int nestlevel)
{
	/* Do nothing if no new nestlevel was created
	 *
	 * 如果没有创建新的嵌套级别，则不执行任何操作
	 */
	if (nestlevel > 0)
		AtEOXact_GUC(true, nestlevel);
}

/*
 * Append SCRAM client key and server key information from the global
 * MyProcPort into the given StringInfo buffer.
 *
 * 将 SCRAM 客户端密钥和服务器密钥信息从全局 MyProcPort 附加到给定的 StringInfo 缓冲区。
 */
static void
appendSCRAMKeysInfo(StringInfo buf)
{
	int			len;
	int			encoded_len;
	char	   *client_key;
	char	   *server_key;

	len = pg_b64_enc_len(sizeof(MyProcPort->scram_ClientKey));
	/* don't forget the zero-terminator
	 *
	 * 不要忘记零终止符
	 */
	client_key = palloc0(len + 1);
	encoded_len = pg_b64_encode(MyProcPort->scram_ClientKey,
								sizeof(MyProcPort->scram_ClientKey),
								client_key, len);
	if (encoded_len < 0)
		elog(ERROR, "could not encode SCRAM client key");

	len = pg_b64_enc_len(sizeof(MyProcPort->scram_ServerKey));
	/* don't forget the zero-terminator
	 *
	 * 不要忘记零终止符
	 */
	server_key = palloc0(len + 1);
	encoded_len = pg_b64_encode(MyProcPort->scram_ServerKey,
								sizeof(MyProcPort->scram_ServerKey),
								server_key, len);
	if (encoded_len < 0)
		elog(ERROR, "could not encode SCRAM server key");

	appendStringInfo(buf, "scram_client_key='%s' ", client_key);
	appendStringInfo(buf, "scram_server_key='%s' ", server_key);
	appendStringInfoString(buf, "require_auth='scram-sha-256' ");

	pfree(client_key);
	pfree(server_key);
}


static bool
UseScramPassthrough(ForeignServer *foreign_server, UserMapping *user)
{
	ListCell   *cell;

	foreach(cell, foreign_server->options)
	{
		DefElem    *def = lfirst(cell);

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

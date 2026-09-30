/*-------------------------------------------------------------------------
 *
 * proto.c
 *		logical replication protocol functions
 *
 * 逻辑复制协议函数。
 *
 * Copyright (c) 2015-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/backend/replication/logical/proto.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/sysattr.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_type.h"
#include "libpq/pqformat.h"
#include "replication/logicalproto.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

/*
 * 核心流程：
 * 发布端用 logicalrep_write_* 把 BEGIN、COMMIT、INSERT、UPDATE、DELETE、TRUNCATE、
 * RELATION、TYPE 以及 STREAM START、STOP、COMMIT、ABORT 等消息写入 StringInfo。
 * 订阅端用对应的 logicalrep_read_* 按同一布局读回。
 * 元组由 logicalrep_write_tuple 与 logicalrep_read_tuple 按列传输，未变化的 TOAST 值可以省略。
 * 列是否发布由 column_in_publication 根据发布列清单和 publish_generated_columns 决定。
 */

/*
 * Protocol message flags.
 *
 * 协议消息标志。
 */
#define LOGICALREP_IS_REPLICA_IDENTITY 1

#define MESSAGE_TRANSACTIONAL (1<<0)
#define TRUNCATE_CASCADE		(1<<0)
#define TRUNCATE_RESTART_SEQS	(1<<1)

static void logicalrep_write_attrs(StringInfo out, Relation rel,
								   Bitmapset *columns,
								   PublishGencolsType include_gencols_type);
static void logicalrep_write_tuple(StringInfo out, Relation rel,
								   TupleTableSlot *slot,
								   bool binary, Bitmapset *columns,
								   PublishGencolsType include_gencols_type);
static void logicalrep_read_attrs(StringInfo in, LogicalRepRelation *rel);
static void logicalrep_read_tuple(StringInfo in, LogicalRepTupleData *tuple);

static void logicalrep_write_namespace(StringInfo out, Oid nspid);
static const char *logicalrep_read_namespace(StringInfo in);

/*
 * Write BEGIN to the output stream.
 *
 * 把 BEGIN 写入输出流。
 */
void
logicalrep_write_begin(StringInfo out, ReorderBufferTXN *txn)
{
	pq_sendbyte(out, LOGICAL_REP_MSG_BEGIN);

	/* fixed fields
	 *
	 * 固定字段
	 */
	pq_sendint64(out, txn->final_lsn);
	pq_sendint64(out, txn->xact_time.commit_time);
	pq_sendint32(out, txn->xid);
}

/*
 * Read transaction BEGIN from the stream.
 *
 * 从流中读取事务 BEGIN。
 */
void
logicalrep_read_begin(StringInfo in, LogicalRepBeginData *begin_data)
{
	/* read fields
	 *
	 * 读取字段
	 */
	begin_data->final_lsn = pq_getmsgint64(in);
	if (begin_data->final_lsn == InvalidXLogRecPtr)
		elog(ERROR, "final_lsn not set in begin message");
	begin_data->committime = pq_getmsgint64(in);
	begin_data->xid = pq_getmsgint(in, 4);
}


/*
 * Write COMMIT to the output stream.
 *
 * 把 COMMIT 写入输出流。
 */
void
logicalrep_write_commit(StringInfo out, ReorderBufferTXN *txn,
						XLogRecPtr commit_lsn)
{
	uint8		flags = 0;

	pq_sendbyte(out, LOGICAL_REP_MSG_COMMIT);

	/* send the flags field (unused for now)
	 *
	 * 发送 flags 字段（目前未使用）
	 */
	pq_sendbyte(out, flags);

	/* send fields
	 *
	 * 发送各字段
	 */
	pq_sendint64(out, commit_lsn);
	pq_sendint64(out, txn->end_lsn);
	pq_sendint64(out, txn->xact_time.commit_time);
}

/*
 * Read transaction COMMIT from the stream.
 *
 * 从流中读取事务 COMMIT。
 */
void
logicalrep_read_commit(StringInfo in, LogicalRepCommitData *commit_data)
{
	/* read flags (unused for now)
	 *
	 * 读取 flags（目前未使用）
	 */
	uint8		flags = pq_getmsgbyte(in);

	if (flags != 0)
		elog(ERROR, "unrecognized flags %u in commit message", flags);

	/* read fields
	 *
	 * 读取字段
	 */
	commit_data->commit_lsn = pq_getmsgint64(in);
	commit_data->end_lsn = pq_getmsgint64(in);
	commit_data->committime = pq_getmsgint64(in);
}

/*
 * Write BEGIN PREPARE to the output stream.
 *
 * 把 BEGIN PREPARE 写入输出流。
 */
void
logicalrep_write_begin_prepare(StringInfo out, ReorderBufferTXN *txn)
{
	pq_sendbyte(out, LOGICAL_REP_MSG_BEGIN_PREPARE);

	/* fixed fields
	 *
	 * 固定字段
	 */
	pq_sendint64(out, txn->final_lsn);
	pq_sendint64(out, txn->end_lsn);
	pq_sendint64(out, txn->xact_time.prepare_time);
	pq_sendint32(out, txn->xid);

	/* send gid
	 *
	 * 发送 gid
	 */
	pq_sendstring(out, txn->gid);
}

/*
 * Read transaction BEGIN PREPARE from the stream.
 *
 * 从流中读取事务 BEGIN PREPARE。
 */
void
logicalrep_read_begin_prepare(StringInfo in, LogicalRepPreparedTxnData *begin_data)
{
	/* read fields
	 *
	 * 读取字段
	 */
	begin_data->prepare_lsn = pq_getmsgint64(in);
	if (begin_data->prepare_lsn == InvalidXLogRecPtr)
		elog(ERROR, "prepare_lsn not set in begin prepare message");
	begin_data->end_lsn = pq_getmsgint64(in);
	if (begin_data->end_lsn == InvalidXLogRecPtr)
		elog(ERROR, "end_lsn not set in begin prepare message");
	begin_data->prepare_time = pq_getmsgint64(in);
	begin_data->xid = pq_getmsgint(in, 4);

	/* read gid (copy it into a pre-allocated buffer)
	 *
	 * 读取 gid（复制到预先分配的缓冲区）
	 */
	strlcpy(begin_data->gid, pq_getmsgstring(in), sizeof(begin_data->gid));
}

/*
 * The core functionality for logicalrep_write_prepare and
 * logicalrep_write_stream_prepare.
 *
 * logicalrep_write_prepare 与 logicalrep_write_stream_prepare 的核心实现。
 */
static void
logicalrep_write_prepare_common(StringInfo out, LogicalRepMsgType type,
								ReorderBufferTXN *txn, XLogRecPtr prepare_lsn)
{
	uint8		flags = 0;

	pq_sendbyte(out, type);

	/*
	 * This should only ever happen for two-phase commit transactions, in
	 * which case we expect to have a valid GID.
	 *
	 * 这只应发生在两阶段提交事务中，此时应当有一个有效的 GID。
	 */
	Assert(txn->gid != NULL);
	Assert(rbtxn_is_prepared(txn));
	Assert(TransactionIdIsValid(txn->xid));

	/* send the flags field
	 *
	 * 发送 flags 字段
	 */
	pq_sendbyte(out, flags);

	/* send fields
	 *
	 * 发送各字段
	 */
	pq_sendint64(out, prepare_lsn);
	pq_sendint64(out, txn->end_lsn);
	pq_sendint64(out, txn->xact_time.prepare_time);
	pq_sendint32(out, txn->xid);

	/* send gid
	 *
	 * 发送 gid
	 */
	pq_sendstring(out, txn->gid);
}

/*
 * Write PREPARE to the output stream.
 *
 * 把 PREPARE 写入输出流。
 */
void
logicalrep_write_prepare(StringInfo out, ReorderBufferTXN *txn,
						 XLogRecPtr prepare_lsn)
{
	logicalrep_write_prepare_common(out, LOGICAL_REP_MSG_PREPARE,
									txn, prepare_lsn);
}

/*
 * The core functionality for logicalrep_read_prepare and
 * logicalrep_read_stream_prepare.
 *
 * logicalrep_read_prepare 与 logicalrep_read_stream_prepare 的核心实现。
 */
static void
logicalrep_read_prepare_common(StringInfo in, char *msgtype,
							   LogicalRepPreparedTxnData *prepare_data)
{
	/* read flags
	 *
	 * 读取 flags
	 */
	uint8		flags = pq_getmsgbyte(in);

	if (flags != 0)
		elog(ERROR, "unrecognized flags %u in %s message", flags, msgtype);

	/* read fields
	 *
	 * 读取字段
	 */
	prepare_data->prepare_lsn = pq_getmsgint64(in);
	if (prepare_data->prepare_lsn == InvalidXLogRecPtr)
		elog(ERROR, "prepare_lsn is not set in %s message", msgtype);
	prepare_data->end_lsn = pq_getmsgint64(in);
	if (prepare_data->end_lsn == InvalidXLogRecPtr)
		elog(ERROR, "end_lsn is not set in %s message", msgtype);
	prepare_data->prepare_time = pq_getmsgint64(in);
	prepare_data->xid = pq_getmsgint(in, 4);
	if (prepare_data->xid == InvalidTransactionId)
		elog(ERROR, "invalid two-phase transaction ID in %s message", msgtype);

	/* read gid (copy it into a pre-allocated buffer)
	 *
	 * 读取 gid（复制到预先分配的缓冲区）
	 */
	strlcpy(prepare_data->gid, pq_getmsgstring(in), sizeof(prepare_data->gid));
}

/*
 * Read transaction PREPARE from the stream.
 *
 * 从流中读取事务 PREPARE。
 */
void
logicalrep_read_prepare(StringInfo in, LogicalRepPreparedTxnData *prepare_data)
{
	logicalrep_read_prepare_common(in, "prepare", prepare_data);
}

/*
 * Write COMMIT PREPARED to the output stream.
 *
 * 把 COMMIT PREPARED 写入输出流。
 */
void
logicalrep_write_commit_prepared(StringInfo out, ReorderBufferTXN *txn,
								 XLogRecPtr commit_lsn)
{
	uint8		flags = 0;

	pq_sendbyte(out, LOGICAL_REP_MSG_COMMIT_PREPARED);

	/*
	 * This should only ever happen for two-phase commit transactions, in
	 * which case we expect to have a valid GID.
	 *
	 * 这只应发生在两阶段提交事务中，此时应当有一个有效的 GID。
	 */
	Assert(txn->gid != NULL);

	/* send the flags field
	 *
	 * 发送 flags 字段
	 */
	pq_sendbyte(out, flags);

	/* send fields
	 *
	 * 发送各字段
	 */
	pq_sendint64(out, commit_lsn);
	pq_sendint64(out, txn->end_lsn);
	pq_sendint64(out, txn->xact_time.commit_time);
	pq_sendint32(out, txn->xid);

	/* send gid
	 *
	 * 发送 gid
	 */
	pq_sendstring(out, txn->gid);
}

/*
 * Read transaction COMMIT PREPARED from the stream.
 *
 * 从流中读取事务 COMMIT PREPARED。
 */
void
logicalrep_read_commit_prepared(StringInfo in, LogicalRepCommitPreparedTxnData *prepare_data)
{
	/* read flags
	 *
	 * 读取 flags
	 */
	uint8		flags = pq_getmsgbyte(in);

	if (flags != 0)
		elog(ERROR, "unrecognized flags %u in commit prepared message", flags);

	/* read fields
	 *
	 * 读取字段
	 */
	prepare_data->commit_lsn = pq_getmsgint64(in);
	if (prepare_data->commit_lsn == InvalidXLogRecPtr)
		elog(ERROR, "commit_lsn is not set in commit prepared message");
	prepare_data->end_lsn = pq_getmsgint64(in);
	if (prepare_data->end_lsn == InvalidXLogRecPtr)
		elog(ERROR, "end_lsn is not set in commit prepared message");
	prepare_data->commit_time = pq_getmsgint64(in);
	prepare_data->xid = pq_getmsgint(in, 4);

	/* read gid (copy it into a pre-allocated buffer)
	 *
	 * 读取 gid（复制到预先分配的缓冲区）
	 */
	strlcpy(prepare_data->gid, pq_getmsgstring(in), sizeof(prepare_data->gid));
}

/*
 * Write ROLLBACK PREPARED to the output stream.
 *
 * 把 ROLLBACK PREPARED 写入输出流。
 */
void
logicalrep_write_rollback_prepared(StringInfo out, ReorderBufferTXN *txn,
								   XLogRecPtr prepare_end_lsn,
								   TimestampTz prepare_time)
{
	uint8		flags = 0;

	pq_sendbyte(out, LOGICAL_REP_MSG_ROLLBACK_PREPARED);

	/*
	 * This should only ever happen for two-phase commit transactions, in
	 * which case we expect to have a valid GID.
	 *
	 * 这只应发生在两阶段提交事务中，此时应当有一个有效的 GID。
	 */
	Assert(txn->gid != NULL);

	/* send the flags field
	 *
	 * 发送 flags 字段
	 */
	pq_sendbyte(out, flags);

	/* send fields
	 *
	 * 发送各字段
	 */
	pq_sendint64(out, prepare_end_lsn);
	pq_sendint64(out, txn->end_lsn);
	pq_sendint64(out, prepare_time);
	pq_sendint64(out, txn->xact_time.commit_time);
	pq_sendint32(out, txn->xid);

	/* send gid
	 *
	 * 发送 gid
	 */
	pq_sendstring(out, txn->gid);
}

/*
 * Read transaction ROLLBACK PREPARED from the stream.
 *
 * 从流中读取事务 ROLLBACK PREPARED。
 */
void
logicalrep_read_rollback_prepared(StringInfo in,
								  LogicalRepRollbackPreparedTxnData *rollback_data)
{
	/* read flags
	 *
	 * 读取 flags
	 */
	uint8		flags = pq_getmsgbyte(in);

	if (flags != 0)
		elog(ERROR, "unrecognized flags %u in rollback prepared message", flags);

	/* read fields
	 *
	 * 读取字段
	 */
	rollback_data->prepare_end_lsn = pq_getmsgint64(in);
	if (rollback_data->prepare_end_lsn == InvalidXLogRecPtr)
		elog(ERROR, "prepare_end_lsn is not set in rollback prepared message");
	rollback_data->rollback_end_lsn = pq_getmsgint64(in);
	if (rollback_data->rollback_end_lsn == InvalidXLogRecPtr)
		elog(ERROR, "rollback_end_lsn is not set in rollback prepared message");
	rollback_data->prepare_time = pq_getmsgint64(in);
	rollback_data->rollback_time = pq_getmsgint64(in);
	rollback_data->xid = pq_getmsgint(in, 4);

	/* read gid (copy it into a pre-allocated buffer)
	 *
	 * 读取 gid（复制到预先分配的缓冲区）
	 */
	strlcpy(rollback_data->gid, pq_getmsgstring(in), sizeof(rollback_data->gid));
}

/*
 * Write STREAM PREPARE to the output stream.
 *
 * 把 STREAM PREPARE 写入输出流。
 */
void
logicalrep_write_stream_prepare(StringInfo out,
								ReorderBufferTXN *txn,
								XLogRecPtr prepare_lsn)
{
	logicalrep_write_prepare_common(out, LOGICAL_REP_MSG_STREAM_PREPARE,
									txn, prepare_lsn);
}

/*
 * Read STREAM PREPARE from the stream.
 *
 * 从流中读取 STREAM PREPARE。
 */
void
logicalrep_read_stream_prepare(StringInfo in, LogicalRepPreparedTxnData *prepare_data)
{
	logicalrep_read_prepare_common(in, "stream prepare", prepare_data);
}

/*
 * Write ORIGIN to the output stream.
 *
 * 把 ORIGIN 写入输出流。
 */
void
logicalrep_write_origin(StringInfo out, const char *origin,
						XLogRecPtr origin_lsn)
{
	pq_sendbyte(out, LOGICAL_REP_MSG_ORIGIN);

	/* fixed fields
	 *
	 * 固定字段
	 */
	pq_sendint64(out, origin_lsn);

	/* origin string
	 *
	 * 源名称字符串
	 */
	pq_sendstring(out, origin);
}

/*
 * Read ORIGIN from the output stream.
 *
 * 从输出流读取 ORIGIN。
 */
char *
logicalrep_read_origin(StringInfo in, XLogRecPtr *origin_lsn)
{
	/* fixed fields
	 *
	 * 固定字段
	 */
	*origin_lsn = pq_getmsgint64(in);

	/* return origin
	 *
	 * 返回 origin
	 */
	return pstrdup(pq_getmsgstring(in));
}

/*
 * Write INSERT to the output stream.
 *
 * 把 INSERT 写入输出流。
 */
void
logicalrep_write_insert(StringInfo out, TransactionId xid, Relation rel,
						TupleTableSlot *newslot, bool binary,
						Bitmapset *columns,
						PublishGencolsType include_gencols_type)
{
	pq_sendbyte(out, LOGICAL_REP_MSG_INSERT);

	/* transaction ID (if not valid, we're not streaming)
	 *
	 * 事务 ID（若无效，表示当前不是流式传输）
	 */
	if (TransactionIdIsValid(xid))
		pq_sendint32(out, xid);

	/* use Oid as relation identifier
	 *
	 * 用 Oid 作为关系标识
	 */
	pq_sendint32(out, RelationGetRelid(rel));

	pq_sendbyte(out, 'N');		/* new tuple follows
								 *
								 * 后面跟着新元组
								 */
	logicalrep_write_tuple(out, rel, newslot, binary, columns,
						   include_gencols_type);
}

/*
 * Read INSERT from stream.
 *
 * 从流中读取 INSERT。
 *
 * Fills the new tuple.
 *
 * 填入新元组。
 */
LogicalRepRelId
logicalrep_read_insert(StringInfo in, LogicalRepTupleData *newtup)
{
	char		action;
	LogicalRepRelId relid;

	/* read the relation id
	 *
	 * 读取关系 id
	 */
	relid = pq_getmsgint(in, 4);

	action = pq_getmsgbyte(in);
	if (action != 'N')
		elog(ERROR, "expected new tuple but got %d",
			 action);

	logicalrep_read_tuple(in, newtup);

	return relid;
}

/*
 * Write UPDATE to the output stream.
 *
 * 把 UPDATE 写入输出流。
 */
void
logicalrep_write_update(StringInfo out, TransactionId xid, Relation rel,
						TupleTableSlot *oldslot, TupleTableSlot *newslot,
						bool binary, Bitmapset *columns,
						PublishGencolsType include_gencols_type)
{
	pq_sendbyte(out, LOGICAL_REP_MSG_UPDATE);

	Assert(rel->rd_rel->relreplident == REPLICA_IDENTITY_DEFAULT ||
		   rel->rd_rel->relreplident == REPLICA_IDENTITY_FULL ||
		   rel->rd_rel->relreplident == REPLICA_IDENTITY_INDEX);

	/* transaction ID (if not valid, we're not streaming)
	 *
	 * 事务 ID（若无效，表示当前不是流式传输）
	 */
	if (TransactionIdIsValid(xid))
		pq_sendint32(out, xid);

	/* use Oid as relation identifier
	 *
	 * 用 Oid 作为关系标识
	 */
	pq_sendint32(out, RelationGetRelid(rel));

	if (oldslot != NULL)
	{
		if (rel->rd_rel->relreplident == REPLICA_IDENTITY_FULL)
			pq_sendbyte(out, 'O');	/* old tuple follows
									 *
									 * 后面跟着旧元组
									 */
		else
			pq_sendbyte(out, 'K');	/* old key follows
									 *
									 * 后面跟着旧键
									 */
		logicalrep_write_tuple(out, rel, oldslot, binary, columns,
							   include_gencols_type);
	}

	pq_sendbyte(out, 'N');		/* new tuple follows
								 *
								 * 后面跟着新元组
								 */
	logicalrep_write_tuple(out, rel, newslot, binary, columns,
						   include_gencols_type);
}

/*
 * Read UPDATE from stream.
 *
 * 从流中读取 UPDATE。
 */
LogicalRepRelId
logicalrep_read_update(StringInfo in, bool *has_oldtuple,
					   LogicalRepTupleData *oldtup,
					   LogicalRepTupleData *newtup)
{
	char		action;
	LogicalRepRelId relid;

	/* read the relation id
	 *
	 * 读取关系 id
	 */
	relid = pq_getmsgint(in, 4);

	/* read and verify action
	 *
	 * 读取并校验动作
	 */
	action = pq_getmsgbyte(in);
	if (action != 'K' && action != 'O' && action != 'N')
		elog(ERROR, "expected action 'N', 'O' or 'K', got %c",
			 action);

	/* check for old tuple
	 *
	 * 检查是否有旧元组
	 */
	if (action == 'K' || action == 'O')
	{
		logicalrep_read_tuple(in, oldtup);
		*has_oldtuple = true;

		action = pq_getmsgbyte(in);
	}
	else
		*has_oldtuple = false;

	/* check for new  tuple
	 *
	 * 检查是否有新元组
	 */
	if (action != 'N')
		elog(ERROR, "expected action 'N', got %c",
			 action);

	logicalrep_read_tuple(in, newtup);

	return relid;
}

/*
 * Write DELETE to the output stream.
 *
 * 把 DELETE 写入输出流。
 */
void
logicalrep_write_delete(StringInfo out, TransactionId xid, Relation rel,
						TupleTableSlot *oldslot, bool binary,
						Bitmapset *columns,
						PublishGencolsType include_gencols_type)
{
	Assert(rel->rd_rel->relreplident == REPLICA_IDENTITY_DEFAULT ||
		   rel->rd_rel->relreplident == REPLICA_IDENTITY_FULL ||
		   rel->rd_rel->relreplident == REPLICA_IDENTITY_INDEX);

	pq_sendbyte(out, LOGICAL_REP_MSG_DELETE);

	/* transaction ID (if not valid, we're not streaming)
	 *
	 * 事务 ID（若无效，表示当前不是流式传输）
	 */
	if (TransactionIdIsValid(xid))
		pq_sendint32(out, xid);

	/* use Oid as relation identifier
	 *
	 * 用 Oid 作为关系标识
	 */
	pq_sendint32(out, RelationGetRelid(rel));

	if (rel->rd_rel->relreplident == REPLICA_IDENTITY_FULL)
		pq_sendbyte(out, 'O');	/* old tuple follows
								 *
								 * 后面跟着旧元组
								 */
	else
		pq_sendbyte(out, 'K');	/* old key follows
								 *
								 * 后面跟着旧键
								 */

	logicalrep_write_tuple(out, rel, oldslot, binary, columns,
						   include_gencols_type);
}

/*
 * Read DELETE from stream.
 *
 * 从流中读取 DELETE。
 *
 * Fills the old tuple.
 *
 * 填入旧元组。
 */
LogicalRepRelId
logicalrep_read_delete(StringInfo in, LogicalRepTupleData *oldtup)
{
	char		action;
	LogicalRepRelId relid;

	/* read the relation id
	 *
	 * 读取关系 id
	 */
	relid = pq_getmsgint(in, 4);

	/* read and verify action
	 *
	 * 读取并校验动作
	 */
	action = pq_getmsgbyte(in);
	if (action != 'K' && action != 'O')
		elog(ERROR, "expected action 'O' or 'K', got %c", action);

	logicalrep_read_tuple(in, oldtup);

	return relid;
}

/*
 * Write TRUNCATE to the output stream.
 *
 * 把 TRUNCATE 写入输出流。
 */
void
logicalrep_write_truncate(StringInfo out,
						  TransactionId xid,
						  int nrelids,
						  Oid relids[],
						  bool cascade, bool restart_seqs)
{
	int			i;
	uint8		flags = 0;

	pq_sendbyte(out, LOGICAL_REP_MSG_TRUNCATE);

	/* transaction ID (if not valid, we're not streaming)
	 *
	 * 事务 ID（若无效，表示当前不是流式传输）
	 */
	if (TransactionIdIsValid(xid))
		pq_sendint32(out, xid);

	pq_sendint32(out, nrelids);

	/* encode and send truncate flags
	 *
	 * 编码并发送 truncate 标志
	 */
	if (cascade)
		flags |= TRUNCATE_CASCADE;
	if (restart_seqs)
		flags |= TRUNCATE_RESTART_SEQS;
	pq_sendint8(out, flags);

	for (i = 0; i < nrelids; i++)
		pq_sendint32(out, relids[i]);
}

/*
 * Read TRUNCATE from stream.
 *
 * 从流中读取 TRUNCATE。
 */
List *
logicalrep_read_truncate(StringInfo in,
						 bool *cascade, bool *restart_seqs)
{
	int			i;
	int			nrelids;
	List	   *relids = NIL;
	uint8		flags;

	nrelids = pq_getmsgint(in, 4);

	/* read and decode truncate flags
	 *
	 * 读取并解码 truncate 标志
	 */
	flags = pq_getmsgint(in, 1);
	*cascade = (flags & TRUNCATE_CASCADE) > 0;
	*restart_seqs = (flags & TRUNCATE_RESTART_SEQS) > 0;

	for (i = 0; i < nrelids; i++)
		relids = lappend_oid(relids, pq_getmsgint(in, 4));

	return relids;
}

/*
 * Write MESSAGE to stream
 *
 * 把 MESSAGE 写入流
 */
void
logicalrep_write_message(StringInfo out, TransactionId xid, XLogRecPtr lsn,
						 bool transactional, const char *prefix, Size sz,
						 const char *message)
{
	uint8		flags = 0;

	pq_sendbyte(out, LOGICAL_REP_MSG_MESSAGE);

	/* encode and send message flags
	 *
	 * 编码并发送消息标志
	 */
	if (transactional)
		flags |= MESSAGE_TRANSACTIONAL;

	/* transaction ID (if not valid, we're not streaming)
	 *
	 * 事务 ID（若无效，表示当前不是流式传输）
	 */
	if (TransactionIdIsValid(xid))
		pq_sendint32(out, xid);

	pq_sendint8(out, flags);
	pq_sendint64(out, lsn);
	pq_sendstring(out, prefix);
	pq_sendint32(out, sz);
	pq_sendbytes(out, message, sz);
}

/*
 * Write relation description to the output stream.
 *
 * 把关系描述写入输出流。
 */
void
logicalrep_write_rel(StringInfo out, TransactionId xid, Relation rel,
					 Bitmapset *columns,
					 PublishGencolsType include_gencols_type)
{
	char	   *relname;

	pq_sendbyte(out, LOGICAL_REP_MSG_RELATION);

	/* transaction ID (if not valid, we're not streaming)
	 *
	 * 事务 ID（若无效，表示当前不是流式传输）
	 */
	if (TransactionIdIsValid(xid))
		pq_sendint32(out, xid);

	/* use Oid as relation identifier
	 *
	 * 用 Oid 作为关系标识
	 */
	pq_sendint32(out, RelationGetRelid(rel));

	/* send qualified relation name
	 *
	 * 发送带模式限定的关系名
	 */
	logicalrep_write_namespace(out, RelationGetNamespace(rel));
	relname = RelationGetRelationName(rel);
	pq_sendstring(out, relname);

	/* send replica identity
	 *
	 * 发送 replica identity
	 */
	pq_sendbyte(out, rel->rd_rel->relreplident);

	/* send the attribute info
	 *
	 * 发送属性信息
	 */
	logicalrep_write_attrs(out, rel, columns, include_gencols_type);
}

/*
 * Read the relation info from stream and return as LogicalRepRelation.
 *
 * 从流中读取关系信息，并以 LogicalRepRelation 返回。
 */
LogicalRepRelation *
logicalrep_read_rel(StringInfo in)
{
	LogicalRepRelation *rel = palloc(sizeof(LogicalRepRelation));

	rel->remoteid = pq_getmsgint(in, 4);

	/* Read relation name from stream
	 *
	 * 从流中读取关系名
	 */
	rel->nspname = pstrdup(logicalrep_read_namespace(in));
	rel->relname = pstrdup(pq_getmsgstring(in));

	/* Read the replica identity.
	 *
	 * 读取 replica identity。
	 */
	rel->replident = pq_getmsgbyte(in);

	/* Get attribute description
	 *
	 * 取得属性描述
	 */
	logicalrep_read_attrs(in, rel);

	return rel;
}

/*
 * Write type info to the output stream.
 *
 * 把类型信息写入输出流。
 *
 * This function will always write base type info.
 *
 * 本函数总是写基类型的信息。
 */
void
logicalrep_write_typ(StringInfo out, TransactionId xid, Oid typoid)
{
	Oid			basetypoid = getBaseType(typoid);
	HeapTuple	tup;
	Form_pg_type typtup;

	pq_sendbyte(out, LOGICAL_REP_MSG_TYPE);

	/* transaction ID (if not valid, we're not streaming)
	 *
	 * 事务 ID（若无效，表示当前不是流式传输）
	 */
	if (TransactionIdIsValid(xid))
		pq_sendint32(out, xid);

	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(basetypoid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", basetypoid);
	typtup = (Form_pg_type) GETSTRUCT(tup);

	/* use Oid as type identifier
	 *
	 * 用 Oid 作为类型标识
	 */
	pq_sendint32(out, typoid);

	/* send qualified type name
	 *
	 * 发送带模式限定的类型名
	 */
	logicalrep_write_namespace(out, typtup->typnamespace);
	pq_sendstring(out, NameStr(typtup->typname));

	ReleaseSysCache(tup);
}

/*
 * Read type info from the output stream.
 *
 * 从输出流读取类型信息。
 */
void
logicalrep_read_typ(StringInfo in, LogicalRepTyp *ltyp)
{
	ltyp->remoteid = pq_getmsgint(in, 4);

	/* Read type name from stream
	 *
	 * 从流中读取类型名
	 */
	ltyp->nspname = pstrdup(logicalrep_read_namespace(in));
	ltyp->typname = pstrdup(pq_getmsgstring(in));
}

/*
 * Write a tuple to the outputstream, in the most efficient format possible.
 *
 * 以尽可能高效的格式把元组写入输出流。
 */
static void
logicalrep_write_tuple(StringInfo out, Relation rel, TupleTableSlot *slot,
					   bool binary, Bitmapset *columns,
					   PublishGencolsType include_gencols_type)
{
	TupleDesc	desc;
	Datum	   *values;
	bool	   *isnull;
	int			i;
	uint16		nliveatts = 0;

	desc = RelationGetDescr(rel);

	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		if (!logicalrep_should_publish_column(att, columns,
											  include_gencols_type))
			continue;

		nliveatts++;
	}
	pq_sendint16(out, nliveatts);

	slot_getallattrs(slot);
	values = slot->tts_values;
	isnull = slot->tts_isnull;

	/* Write the values
	 *
	 * 写入各值
	 */
	for (i = 0; i < desc->natts; i++)
	{
		HeapTuple	typtup;
		Form_pg_type typclass;
		Form_pg_attribute att = TupleDescAttr(desc, i);

		if (!logicalrep_should_publish_column(att, columns,
											  include_gencols_type))
			continue;

		if (isnull[i])
		{
			pq_sendbyte(out, LOGICALREP_COLUMN_NULL);
			continue;
		}

		if (att->attlen == -1 && VARATT_IS_EXTERNAL_ONDISK(values[i]))
		{
			/*
			 * Unchanged toasted datum.  (Note that we don't promise to detect
			 * unchanged data in general; this is just a cheap check to avoid
			 * sending large values unnecessarily.)
			 *
			 * 未变化的 TOAST 数据。（注意：我们并不保证一般地检测出未变化的数据；
			 * 这只是一个廉价检查，避免不必要地发送大值。）
			 */
			pq_sendbyte(out, LOGICALREP_COLUMN_UNCHANGED);
			continue;
		}

		typtup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(att->atttypid));
		if (!HeapTupleIsValid(typtup))
			elog(ERROR, "cache lookup failed for type %u", att->atttypid);
		typclass = (Form_pg_type) GETSTRUCT(typtup);

		/*
		 * Send in binary if requested and type has suitable send function.
		 *
		 * 若调用方要求且该类型有合适的发送函数，则按二进制发送。
		 */
		if (binary && OidIsValid(typclass->typsend))
		{
			bytea	   *outputbytes;
			int			len;

			pq_sendbyte(out, LOGICALREP_COLUMN_BINARY);
			outputbytes = OidSendFunctionCall(typclass->typsend, values[i]);
			len = VARSIZE(outputbytes) - VARHDRSZ;
			pq_sendint(out, len, 4);	/* length
										 *
										 * 长度
										 */
			pq_sendbytes(out, VARDATA(outputbytes), len);	/* data
															 *
															 * 数据
															 */
			pfree(outputbytes);
		}
		else
		{
			char	   *outputstr;

			pq_sendbyte(out, LOGICALREP_COLUMN_TEXT);
			outputstr = OidOutputFunctionCall(typclass->typoutput, values[i]);
			pq_sendcountedtext(out, outputstr, strlen(outputstr));
			pfree(outputstr);
		}

		ReleaseSysCache(typtup);
	}
}

/*
 * Read tuple in logical replication format from stream.
 *
 * 从流中按逻辑复制格式读取元组。
 */
static void
logicalrep_read_tuple(StringInfo in, LogicalRepTupleData *tuple)
{
	int			i;
	int			natts;

	/* Get number of attributes
	 *
	 * 取得属性个数
	 */
	natts = pq_getmsgint(in, 2);

	/* Allocate space for per-column values; zero out unused StringInfoDatas
	 *
	 * 为每列的值分配空间；把未使用的 StringInfoData 清零
	 */
	tuple->colvalues = (StringInfoData *) palloc0(natts * sizeof(StringInfoData));
	tuple->colstatus = (char *) palloc(natts * sizeof(char));
	tuple->ncols = natts;

	/* Read the data
	 *
	 * 读取数据
	 */
	for (i = 0; i < natts; i++)
	{
		char	   *buff;
		char		kind;
		int			len;
		StringInfo	value = &tuple->colvalues[i];

		kind = pq_getmsgbyte(in);
		tuple->colstatus[i] = kind;

		switch (kind)
		{
			case LOGICALREP_COLUMN_NULL:
				/* nothing more to do
				 *
				 * 没有更多事情要做
				 */
				break;
			case LOGICALREP_COLUMN_UNCHANGED:
				/* we don't receive the value of an unchanged column
				 *
				 * 未变化的列不会收到值
				 */
				break;
			case LOGICALREP_COLUMN_TEXT:
			case LOGICALREP_COLUMN_BINARY:
				len = pq_getmsgint(in, 4);	/* read length
											 *
											 * 读取长度
											 */

				/* and data
				 *
				 * 以及数据
				 */
				buff = palloc(len + 1);
				pq_copymsgbytes(in, buff, len);

				/*
				 * NUL termination is required for LOGICALREP_COLUMN_TEXT mode
				 * as input functions require that.  For
				 * LOGICALREP_COLUMN_BINARY it's not technically required, but
				 * it's harmless.
				 *
				 * LOGICALREP_COLUMN_TEXT 模式需要以 NUL 结尾，因为输入函数要求如此。
				 * LOGICALREP_COLUMN_BINARY 严格来说不需要，但加上也无害。
				 */
				buff[len] = '\0';

				initStringInfoFromString(value, buff, len);
				break;
			default:
				elog(ERROR, "unrecognized data representation type '%c'", kind);
		}
	}
}

/*
 * Write relation attribute metadata to the stream.
 *
 * 把关系的属性元数据写入流。
 */
static void
logicalrep_write_attrs(StringInfo out, Relation rel, Bitmapset *columns,
					   PublishGencolsType include_gencols_type)
{
	TupleDesc	desc;
	int			i;
	uint16		nliveatts = 0;
	Bitmapset  *idattrs = NULL;
	bool		replidentfull;

	desc = RelationGetDescr(rel);

	/* send number of live attributes
	 *
	 * 发送仍然存在的属性个数
	 */
	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		if (!logicalrep_should_publish_column(att, columns,
											  include_gencols_type))
			continue;

		nliveatts++;
	}
	pq_sendint16(out, nliveatts);

	/* fetch bitmap of REPLICATION IDENTITY attributes
	 *
	 * 取出 REPLICATION IDENTITY 属性的位图
	 */
	replidentfull = (rel->rd_rel->relreplident == REPLICA_IDENTITY_FULL);
	if (!replidentfull)
		idattrs = RelationGetIdentityKeyBitmap(rel);

	/* send the attributes
	 *
	 * 发送这些属性
	 */
	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);
		uint8		flags = 0;

		if (!logicalrep_should_publish_column(att, columns,
											  include_gencols_type))
			continue;

		/* REPLICA IDENTITY FULL means all columns are sent as part of key.
		 *
		 * REPLICA IDENTITY FULL 表示所有列都作为键的一部分发送。
		 */
		if (replidentfull ||
			bms_is_member(att->attnum - FirstLowInvalidHeapAttributeNumber,
						  idattrs))
			flags |= LOGICALREP_IS_REPLICA_IDENTITY;

		pq_sendbyte(out, flags);

		/* attribute name
		 *
		 * 属性名
		 */
		pq_sendstring(out, NameStr(att->attname));

		/* attribute type id
		 *
		 * 属性类型 id
		 */
		pq_sendint32(out, (int) att->atttypid);

		/* attribute mode
		 *
		 * 属性模式
		 */
		pq_sendint32(out, att->atttypmod);
	}

	bms_free(idattrs);
}

/*
 * Read relation attribute metadata from the stream.
 *
 * 从流中读取关系的属性元数据。
 */
static void
logicalrep_read_attrs(StringInfo in, LogicalRepRelation *rel)
{
	int			i;
	int			natts;
	char	  **attnames;
	Oid		   *atttyps;
	Bitmapset  *attkeys = NULL;

	natts = pq_getmsgint(in, 2);
	attnames = palloc(natts * sizeof(char *));
	atttyps = palloc(natts * sizeof(Oid));

	/* read the attributes
	 *
	 * 读取这些属性
	 */
	for (i = 0; i < natts; i++)
	{
		uint8		flags;

		/* Check for replica identity column
		 *
		 * 检查是否为 replica identity 列
		 */
		flags = pq_getmsgbyte(in);
		if (flags & LOGICALREP_IS_REPLICA_IDENTITY)
			attkeys = bms_add_member(attkeys, i);

		/* attribute name
		 *
		 * 属性名
		 */
		attnames[i] = pstrdup(pq_getmsgstring(in));

		/* attribute type id
		 *
		 * 属性类型 id
		 */
		atttyps[i] = (Oid) pq_getmsgint(in, 4);

		/* we ignore attribute mode for now
		 *
		 * 目前忽略属性模式
		 */
		(void) pq_getmsgint(in, 4);
	}

	rel->attnames = attnames;
	rel->atttyps = atttyps;
	rel->attkeys = attkeys;
	rel->natts = natts;
}

/*
 * Write the namespace name or empty string for pg_catalog (to save space).
 *
 * 写入命名空间名称；若是 pg_catalog 则写空串以节省空间。
 */
static void
logicalrep_write_namespace(StringInfo out, Oid nspid)
{
	if (nspid == PG_CATALOG_NAMESPACE)
		pq_sendbyte(out, '\0');
	else
	{
		char	   *nspname = get_namespace_name(nspid);

		if (nspname == NULL)
			elog(ERROR, "cache lookup failed for namespace %u",
				 nspid);

		pq_sendstring(out, nspname);
	}
}

/*
 * Read the namespace name while treating empty string as pg_catalog.
 *
 * 读取命名空间名称，并把空串当作 pg_catalog。
 */
static const char *
logicalrep_read_namespace(StringInfo in)
{
	const char *nspname = pq_getmsgstring(in);

	if (nspname[0] == '\0')
		nspname = "pg_catalog";

	return nspname;
}

/*
 * Write the information for the start stream message to the output stream.
 *
 * 把开始流式传输消息的信息写入输出流。
 */
void
logicalrep_write_stream_start(StringInfo out,
							  TransactionId xid, bool first_segment)
{
	pq_sendbyte(out, LOGICAL_REP_MSG_STREAM_START);

	Assert(TransactionIdIsValid(xid));

	/* transaction ID (we're starting to stream, so must be valid)
	 *
	 * 事务 ID（即将开始流式传输，因此必须有效）
	 */
	pq_sendint32(out, xid);

	/* 1 if this is the first streaming segment for this xid
	 *
	 * 若这是该 xid 的第一个流式分段则为 1
	 */
	pq_sendbyte(out, first_segment ? 1 : 0);
}

/*
 * Read the information about the start stream message from output stream.
 *
 * 从输出流读取开始流式传输消息的信息。
 */
TransactionId
logicalrep_read_stream_start(StringInfo in, bool *first_segment)
{
	TransactionId xid;

	Assert(first_segment);

	xid = pq_getmsgint(in, 4);
	*first_segment = (pq_getmsgbyte(in) == 1);

	return xid;
}

/*
 * Write the stop stream message to the output stream.
 *
 * 把停止流式传输的消息写入输出流。
 */
void
logicalrep_write_stream_stop(StringInfo out)
{
	pq_sendbyte(out, LOGICAL_REP_MSG_STREAM_STOP);
}

/*
 * Write STREAM COMMIT to the output stream.
 *
 * 把 STREAM COMMIT 写入输出流。
 */
void
logicalrep_write_stream_commit(StringInfo out, ReorderBufferTXN *txn,
							   XLogRecPtr commit_lsn)
{
	uint8		flags = 0;

	pq_sendbyte(out, LOGICAL_REP_MSG_STREAM_COMMIT);

	Assert(TransactionIdIsValid(txn->xid));

	/* transaction ID
	 *
	 * 事务 ID
	 */
	pq_sendint32(out, txn->xid);

	/* send the flags field (unused for now)
	 *
	 * 发送 flags 字段（目前未使用）
	 */
	pq_sendbyte(out, flags);

	/* send fields
	 *
	 * 发送各字段
	 */
	pq_sendint64(out, commit_lsn);
	pq_sendint64(out, txn->end_lsn);
	pq_sendint64(out, txn->xact_time.commit_time);
}

/*
 * Read STREAM COMMIT from the output stream.
 *
 * 从输出流读取 STREAM COMMIT。
 */
TransactionId
logicalrep_read_stream_commit(StringInfo in, LogicalRepCommitData *commit_data)
{
	TransactionId xid;
	uint8		flags;

	xid = pq_getmsgint(in, 4);

	/* read flags (unused for now)
	 *
	 * 读取 flags（目前未使用）
	 */
	flags = pq_getmsgbyte(in);

	if (flags != 0)
		elog(ERROR, "unrecognized flags %u in commit message", flags);

	/* read fields
	 *
	 * 读取字段
	 */
	commit_data->commit_lsn = pq_getmsgint64(in);
	commit_data->end_lsn = pq_getmsgint64(in);
	commit_data->committime = pq_getmsgint64(in);

	return xid;
}

/*
 * Write STREAM ABORT to the output stream. Note that xid and subxid will be
 * same for the top-level transaction abort.
 *
 * 把 STREAM ABORT 写入输出流。顶层事务中止时，xid 与 subxid 相同。
 *
 * If write_abort_info is true, send the abort_lsn and abort_time fields,
 * otherwise don't.
 *
 * 若 write_abort_info 为真，则发送 abort_lsn 和 abort_time 字段，否则
 * 不发送。
 */
void
logicalrep_write_stream_abort(StringInfo out, TransactionId xid,
							  TransactionId subxid, XLogRecPtr abort_lsn,
							  TimestampTz abort_time, bool write_abort_info)
{
	pq_sendbyte(out, LOGICAL_REP_MSG_STREAM_ABORT);

	Assert(TransactionIdIsValid(xid) && TransactionIdIsValid(subxid));

	/* transaction ID
	 *
	 * 事务 ID
	 */
	pq_sendint32(out, xid);
	pq_sendint32(out, subxid);

	if (write_abort_info)
	{
		pq_sendint64(out, abort_lsn);
		pq_sendint64(out, abort_time);
	}
}

/*
 * Read STREAM ABORT from the output stream.
 *
 * 从输出流读取 STREAM ABORT。
 *
 * If read_abort_info is true, read the abort_lsn and abort_time fields,
 * otherwise don't.
 *
 * 若 read_abort_info 为真，则读取 abort_lsn 和 abort_time 字段，否则不
 * 读取。
 */
void
logicalrep_read_stream_abort(StringInfo in,
							 LogicalRepStreamAbortData *abort_data,
							 bool read_abort_info)
{
	Assert(abort_data);

	abort_data->xid = pq_getmsgint(in, 4);
	abort_data->subxid = pq_getmsgint(in, 4);

	if (read_abort_info)
	{
		abort_data->abort_lsn = pq_getmsgint64(in);
		abort_data->abort_time = pq_getmsgint64(in);
	}
	else
	{
		abort_data->abort_lsn = InvalidXLogRecPtr;
		abort_data->abort_time = 0;
	}
}

/*
 * Get string representing LogicalRepMsgType.
 *
 * 取得表示 LogicalRepMsgType 的字符串。
 */
const char *
logicalrep_message_type(LogicalRepMsgType action)
{
	static char err_unknown[20];

	switch (action)
	{
		case LOGICAL_REP_MSG_BEGIN:
			return "BEGIN";
		case LOGICAL_REP_MSG_COMMIT:
			return "COMMIT";
		case LOGICAL_REP_MSG_ORIGIN:
			return "ORIGIN";
		case LOGICAL_REP_MSG_INSERT:
			return "INSERT";
		case LOGICAL_REP_MSG_UPDATE:
			return "UPDATE";
		case LOGICAL_REP_MSG_DELETE:
			return "DELETE";
		case LOGICAL_REP_MSG_TRUNCATE:
			return "TRUNCATE";
		case LOGICAL_REP_MSG_RELATION:
			return "RELATION";
		case LOGICAL_REP_MSG_TYPE:
			return "TYPE";
		case LOGICAL_REP_MSG_MESSAGE:
			return "MESSAGE";
		case LOGICAL_REP_MSG_BEGIN_PREPARE:
			return "BEGIN PREPARE";
		case LOGICAL_REP_MSG_PREPARE:
			return "PREPARE";
		case LOGICAL_REP_MSG_COMMIT_PREPARED:
			return "COMMIT PREPARED";
		case LOGICAL_REP_MSG_ROLLBACK_PREPARED:
			return "ROLLBACK PREPARED";
		case LOGICAL_REP_MSG_STREAM_START:
			return "STREAM START";
		case LOGICAL_REP_MSG_STREAM_STOP:
			return "STREAM STOP";
		case LOGICAL_REP_MSG_STREAM_COMMIT:
			return "STREAM COMMIT";
		case LOGICAL_REP_MSG_STREAM_ABORT:
			return "STREAM ABORT";
		case LOGICAL_REP_MSG_STREAM_PREPARE:
			return "STREAM PREPARE";
	}

	/*
	 * This message provides context in the error raised when applying a
	 * logical message. So we can't throw an error here. Return an unknown
	 * indicator value so that the original error is still reported.
	 *
	 * 这条消息为应用逻辑消息时抛出的错误提供上下文。因此这里不能再抛错。返
	 * 回一个未知指示值，以便仍然报告原来的错误。
	 */
	snprintf(err_unknown, sizeof(err_unknown), "??? (%d)", action);

	return err_unknown;
}

/*
 * Check if the column 'att' of a table should be published.
 *
 * 检查表的列 att 是否应当发布。
 *
 * 'columns' represents the publication column list (if any) for that table.
 *
 * columns 表示该表的发布列清单（如果有）。
 *
 * 'include_gencols_type' value indicates whether generated columns should be
 * published when there is no column list. Typically, this will have the same
 * value as the 'publish_generated_columns' publication parameter.
 *
 * include_gencols_type 表示在没有列清单时是否发布生成列。通常它与发布
 * 参数 publish_generated_columns 的值相同。
 *
 * Note that generated columns can be published only when present in a
 * publication column list, or when include_gencols_type is
 * PUBLISH_GENCOLS_STORED.
 *
 * 注意：生成列只有出现在发布列清单中，或者 include_gencols_type 为
 * PUBLISH_GENCOLS_STORED 时才能发布。
 */
bool
logicalrep_should_publish_column(Form_pg_attribute att, Bitmapset *columns,
								 PublishGencolsType include_gencols_type)
{
	if (att->attisdropped)
		return false;

	/* If a column list is provided, publish only the cols in that list.
	 *
	 * 若提供了列清单，则只发布该清单中的列。
	 */
	if (columns)
		return bms_is_member(att->attnum, columns);

	/* All non-generated columns are always published.
	 *
	 * 所有非生成列总会被发布。
	 */
	if (!att->attgenerated)
		return true;

	/*
	 * Stored generated columns are only published when the user sets
	 * publish_generated_columns as stored.
	 *
	 * 仅当用户把 publish_generated_columns 设为 stored 时，才发布已存储的
	 * 生成列。
	 */
	if (att->attgenerated == ATTRIBUTE_GENERATED_STORED)
		return include_gencols_type == PUBLISH_GENCOLS_STORED;

	return false;
}

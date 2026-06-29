/*-------------------------------------------------------------------------
 *
 * message.c
 *	  Generic logical messages.
 *	  通用逻辑解码消息（写入 WAL 供逻辑复制/解码插件消费）。
 *
 * Copyright (c) 2013-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/message.c
 *
 * NOTES
 *
 * Generic logical messages allow XLOG logging of arbitrary binary blobs that
 * get passed to the logical decoding plugin. In normal XLOG processing they
 * are same as NOOP.
 * 允许把任意二进制负载记入 XLOG，逻辑解码时交给插件；常规崩溃恢复重放时等价于空操作。
 *
 * These messages can be either transactional or non-transactional.
 * Transactional messages are part of current transaction and will be sent to
 * decoding plugin using in a same way as DML operations.
 * Non-transactional messages are sent to the plugin at the time when the
 * logical decoding reads them from XLOG. This also means that transactional
 * messages won't be delivered if the transaction was rolled back but the
 * non-transactional one will always be delivered.
 * 分事务性与非事务性：事务性随当前事务与 DML 一样交付解码；非事务性在解码读 WAL 到时即交给插件。
 * 事务回滚则事务性消息不交付，非事务性消息仍会交付。
 *
 * Every message carries prefix to avoid conflicts between different decoding
 * plugins. The plugin authors must take extra care to use unique prefix,
 * good options seems to be for example to use the name of the extension.
 * 每条消息带 prefix，避免不同插件/扩展冲突；作者应保证前缀唯一（如扩展名）。
 *
 * ---------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xact.h"
#include "access/xloginsert.h"
#include "miscadmin.h"
#include "replication/message.h"

/*
 * 核心流程：
 * 1) LogLogicalMessage：扩展/SQL 调用侧组装 xl_logical_message + prefix + payload，XLogInsert(RM_LOGICALMSG_ID)；
 *    事务性消息会确保已分配 XID；非事务性且 flush 为真则 XLogFlush 保证落盘。
 * 2) logicalmsg_redo：物理恢复重放时不做实质工作（仅校验 op），逻辑解码在 decode.c 中识别并回调插件。
 */

/*
 * Write logical decoding message into XLog.
 * 将一条逻辑解码消息写入 WAL（供逻辑复制输出插件消费）。
 */
XLogRecPtr
LogLogicalMessage(const char *prefix, const char *message, size_t size,
				  bool transactional, bool flush)
{
	xl_logical_message xlrec;
	XLogRecPtr	lsn;

	/*
	 * Force xid to be allocated if we're emitting a transactional message.
	 * 事务性消息必须属于某事务，确保当前事务已分配 XID。
	 */
	if (transactional)
	{
		Assert(IsTransactionState());
		GetCurrentTransactionId();
	}

	xlrec.dbId = MyDatabaseId;
	xlrec.transactional = transactional;
	/* trailing zero is critical; see logicalmsg_desc */
	/* prefix 含结尾 NUL，长度含该字节；见 logicalmsg_desc */
	xlrec.prefix_size = strlen(prefix) + 1;
	xlrec.message_size = size;

	XLogBeginInsert();
	XLogRegisterData(&xlrec, SizeOfLogicalMessage);
	XLogRegisterData(prefix, xlrec.prefix_size);
	XLogRegisterData(message, size);

	/* allow origin filtering */
	/* 记录含 origin，便于下游按复制源过滤 */
	XLogSetRecordFlags(XLOG_INCLUDE_ORIGIN);

	lsn = XLogInsert(RM_LOGICALMSG_ID, XLOG_LOGICAL_MESSAGE);

	/*
	 * Make sure that the message hits disk before leaving if emitting a
	 * non-transactional message when flush is requested.
	 * 非事务性且无事务提交保证落盘时，若调用方要求 flush 则立即刷 WAL。
	 */
	if (!transactional && flush)
		XLogFlush(lsn);
	return lsn;
}

/*
 * Redo is basically just noop for logical decoding messages.
 * 崩溃恢复重放：逻辑消息对堆数据无影响，redo 为空操作（逻辑解码在 decode.c 单独处理）。
 */
void
logicalmsg_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	if (info != XLOG_LOGICAL_MESSAGE)
		elog(PANIC, "logicalmsg_redo: unknown op code %u", info);

	/* This is only interesting for logical decoding, see decode.c. */
	/* 实际消费侧为逻辑解码，见 decode.c */
}

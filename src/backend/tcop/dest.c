/*-------------------------------------------------------------------------
 *
 * dest.c
 *	  support for communication destinations
 *
 *	  通信目标支持
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/tcop/dest.c
 *
 *-------------------------------------------------------------------------
 */
/*
 *	 INTERFACE ROUTINES
 *		BeginCommand - initialize the destination at start of command
 *		CreateDestReceiver - create tuple receiver object for destination
 *		EndCommand - clean up the destination at end of command
 *		NullCommand - tell dest that an empty query string was recognized
 *		ReadyForQuery - tell dest that we are ready for a new query
 *
 *	 接口例程
 *		BeginCommand - 在命令开始时初始化目标
 *		CreateDestReceiver - 为目标创建元组接收器对象
 *		EndCommand - 在命令结束时清理目标
 *		NullCommand - 告知目标识别到了空查询字符串
 *		ReadyForQuery - 告知目标我们已准备好接收新查询
 *
 *	 NOTES
 *		These routines do the appropriate work before and after
 *		tuples are returned by a query to keep the backend and the
 *		"destination" portals synchronized.
 *
 *	 说明
 *		这些例程会在查询返回元组之前和之后执行相应工作，
 *		以保持后端和“目标”门户同步。
 */

#include "postgres.h"

#include "access/printsimple.h"
#include "access/printtup.h"
#include "access/xact.h"
#include "commands/copy.h"
#include "commands/createas.h"
#include "commands/explain_dr.h"
#include "commands/matview.h"
#include "executor/functions.h"
#include "executor/tqueue.h"
#include "executor/tstoreReceiver.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"


/* ----------------
 *		dummy DestReceiver functions
 *
 *		虚设 DestReceiver 函数
 * ----------------
 */

/*
 * donothingReceive
 *		Accept a tuple for DestNone-like receivers and report success.
 *
 *		为类似 DestNone 的接收器接受一个元组，并报告成功。
 */
static bool
donothingReceive(TupleTableSlot *slot, DestReceiver *self)
{
	return true;
}

/*
 * donothingStartup
 *		Perform no startup work for stateless receivers.
 *
 *		对无状态接收器不执行任何启动工作。
 */
static void
donothingStartup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
}

/*
 * donothingCleanup
 *		Perform no shutdown or destroy work for stateless receivers.
 *
 *		对无状态接收器不执行任何关闭或销毁工作。
 */
static void
donothingCleanup(DestReceiver *self)
{
	/*
	 * this is used for both shutdown and destroy methods
	 *
	 * 这同时用于 shutdown 和 destroy 方法。
	 */
}

/* ----------------
 *		static DestReceiver structs for dest types needing no local state
 *
 *		用于无需本地状态的目标类型的静态 DestReceiver 结构
 * ----------------
 */
static const DestReceiver donothingDR = {
	donothingReceive, donothingStartup, donothingCleanup, donothingCleanup,
	DestNone
};

static const DestReceiver debugtupDR = {
	debugtup, debugStartup, donothingCleanup, donothingCleanup,
	DestDebug
};

static const DestReceiver printsimpleDR = {
	printsimple, printsimple_startup, donothingCleanup, donothingCleanup,
	DestRemoteSimple
};

static const DestReceiver spi_printtupDR = {
	spi_printtup, spi_dest_startup, donothingCleanup, donothingCleanup,
	DestSPI
};

/*
 * Globally available receiver for DestNone.
 *
 * DestNone 的全局可用接收器。
 *
 * It's ok to cast the constness away as any modification of the none receiver
 * would be a bug (which gets easier to catch this way).
 *
 * 可以去掉 const 限定，因为对 none 接收器的任何修改都是一个错误
 * （这样反而更容易捕获该错误）。
 */
DestReceiver *None_Receiver = (DestReceiver *) &donothingDR;

/* ----------------
 *		BeginCommand - initialize the destination at start of command
 *
 *		BeginCommand - 在命令开始时初始化目标
 *
 *		The current implementation has no per-command destination setup.
 *
 *		当前实现没有按命令执行的目标设置工作。
 * ----------------
 */
void
BeginCommand(CommandTag commandTag, CommandDest dest)
{
	/*
	 * Nothing to do at present
	 *
	 * 目前无需执行任何操作。
	 */
}

/* ----------------
 *		CreateDestReceiver - return appropriate receiver function set for dest
 *
 *		CreateDestReceiver - 返回适用于目标的接收器函数集
 *
 *		The switch maps each CommandDest to either a static receiver or a
 *		stateful receiver constructor.
 *
 *		该 switch 会将每个 CommandDest 映射到静态接收器或有状态接收器构造函数。
 * ----------------
 */
DestReceiver *
CreateDestReceiver(CommandDest dest)
{
	/*
	 * It's ok to cast the constness away as any modification of the none
	 * receiver would be a bug (which gets easier to catch this way).
	 *
	 * 可以去掉 const 限定，因为对 none 接收器的任何修改都是一个错误
	 * （这样反而更容易捕获该错误）。
	 */

	switch (dest)
	{
		case DestRemote:
		case DestRemoteExecute:
			return printtup_create_DR(dest);

		case DestRemoteSimple:
			return unconstify(DestReceiver *, &printsimpleDR);

		case DestNone:
			return unconstify(DestReceiver *, &donothingDR);

		case DestDebug:
			return unconstify(DestReceiver *, &debugtupDR);

		case DestSPI:
			return unconstify(DestReceiver *, &spi_printtupDR);

		case DestTuplestore:
			return CreateTuplestoreDestReceiver();

		case DestIntoRel:
			return CreateIntoRelDestReceiver(NULL);

		case DestCopyOut:
			return CreateCopyDestReceiver();

		case DestSQLFunction:
			return CreateSQLFunctionDestReceiver();

		case DestTransientRel:
			return CreateTransientRelDestReceiver(InvalidOid);

		case DestTupleQueue:
			return CreateTupleQueueDestReceiver(NULL);

		case DestExplainSerialize:
			return CreateExplainSerializeDestReceiver(NULL);
	}

	/*
	 * should never get here
	 *
	 * 不应执行到这里。
	 */
	pg_unreachable();
}

/* ----------------
 *		EndCommand - clean up the destination at end of command
 *
 *		EndCommand - 在命令结束时清理目标
 *
 *		Remote destinations receive a command-complete message; other
 *		destinations need no action here.
 *
 *		远程目标会收到命令完成消息；其他目标在此处无需操作。
 * ----------------
 */
void
EndCommand(const QueryCompletion *qc, CommandDest dest, bool force_undecorated_output)
{
	char		completionTag[COMPLETION_TAG_BUFSIZE];
	Size		len;

	switch (dest)
	{
		case DestRemote:
		case DestRemoteExecute:
		case DestRemoteSimple:

			len = BuildQueryCompletionString(completionTag, qc,
											 force_undecorated_output);
			pq_putmessage(PqMsg_CommandComplete, completionTag, len + 1);

		case DestNone:
		case DestDebug:
		case DestSPI:
		case DestTuplestore:
		case DestIntoRel:
		case DestCopyOut:
		case DestSQLFunction:
		case DestTransientRel:
		case DestTupleQueue:
		case DestExplainSerialize:
			break;
	}
}

/* ----------------
 *		EndReplicationCommand - stripped down version of EndCommand
 *
 *		EndReplicationCommand - EndCommand 的精简版本
 *
 *		For use by replication commands.
 *
 *		供复制命令使用。
 * ----------------
 */
void
EndReplicationCommand(const char *commandTag)
{
	pq_putmessage(PqMsg_CommandComplete, commandTag, strlen(commandTag) + 1);
}

/* ----------------
 *		NullCommand - tell dest that an empty query string was recognized
 *
 *		NullCommand - 告知目标识别到了空查询字符串
 *
 *		This ensures that there will be a recognizable end to the response
 *		to an Execute message in the extended query protocol.
 *
 *		这会确保扩展查询协议中 Execute 消息的响应具有可识别的结束位置。
 * ----------------
 */
void
NullCommand(CommandDest dest)
{
	switch (dest)
	{
		case DestRemote:
		case DestRemoteExecute:
		case DestRemoteSimple:

			/*
			 * Tell the FE that we saw an empty query string
			 *
			 * 告知前端我们看到了空查询字符串。
			 */
			pq_putemptymessage(PqMsg_EmptyQueryResponse);
			break;

		case DestNone:
		case DestDebug:
		case DestSPI:
		case DestTuplestore:
		case DestIntoRel:
		case DestCopyOut:
		case DestSQLFunction:
		case DestTransientRel:
		case DestTupleQueue:
		case DestExplainSerialize:
			break;
	}
}

/* ----------------
 *		ReadyForQuery - tell dest that we are ready for a new query
 *
 *		ReadyForQuery - 告知目标我们已准备好接收新查询
 *
 *		The ReadyForQuery message is sent so that the FE can tell when
 *		we are done processing a query string.
 *		In versions 3.0 and up, it also carries a transaction state indicator.
 *
 *		发送 ReadyForQuery 消息是为了让前端知道我们何时完成了查询字符串处理。
 *		在 3.0 及更高版本中，它还携带事务状态指示符。
 *
 *		Note that by flushing the stdio buffer here, we can avoid doing it
 *		most other places and thus reduce the number of separate packets sent.
 *
 *		注意，通过在这里刷新 stdio 缓冲区，我们可以避免在大多数其他地方刷新它，
 *		从而减少发送的独立数据包数量。
 * ----------------
 */
void
ReadyForQuery(CommandDest dest)
{
	switch (dest)
	{
		case DestRemote:
		case DestRemoteExecute:
		case DestRemoteSimple:
			{
				StringInfoData buf;

				pq_beginmessage(&buf, PqMsg_ReadyForQuery);
				pq_sendbyte(&buf, TransactionBlockStatusCode());
				pq_endmessage(&buf);
			}
			/*
			 * Flush output at end of cycle in any case.
			 *
			 * 无论如何都在循环结束时刷新输出。
			 */
			pq_flush();
			break;

		case DestNone:
		case DestDebug:
		case DestSPI:
		case DestTuplestore:
		case DestIntoRel:
		case DestCopyOut:
		case DestSQLFunction:
		case DestTransientRel:
		case DestTupleQueue:
		case DestExplainSerialize:
			break;
	}
}

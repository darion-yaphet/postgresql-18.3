/*-------------------------------------------------------------------------
 *
 * explain_dr.c
 *	  Explain DestReceiver to measure serialization overhead
 *
 * 用于测量序列化开销的 EXPLAIN DestReceiver。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/commands/explain.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "commands/explain.h"
#include "commands/explain_dr.h"
#include "commands/explain_state.h"
#include "libpq/pqformat.h"
#include "libpq/protocol.h"
#include "utils/lsyscache.h"

/*
 * 核心流程概览：
 * CreateExplainSerializeDestReceiver 构造只序列化、不发送的 DestReceiver。
 * serializeAnalyzeStartup 准备输出函数与行上下文；serializeAnalyzeReceive
 * 按 printtup 的方式生成 RowData 并累计时间、缓冲区和字节数；
 * serializeAnalyzeShutdown 收尾，GetSerializationMetrics 取出指标。
 */
/*
 * DestReceiver functions for SERIALIZE option
 *
 * SERIALIZE 选项使用的 DestReceiver 函数。
 *
 * A DestReceiver for query tuples, that serializes passed rows into RowData
 * messages while measuring the resources expended and total serialized size,
 * while never sending the data to the client.  This allows measuring the
 * overhead of deTOASTing and datatype out/sendfuncs, which are not otherwise
 * exercisable without actually hitting the network.
 *
 * 把查询元组序列化成 RowData 消息，统计消耗的资源和序列化总大小，
 * 但不把数据发给客户端。这样可以测量 deTOAST 以及类型 out/send 函数的开销，
 * 否则这些开销只有真正走网络才能观察到。
 */
typedef struct SerializeDestReceiver
{
	DestReceiver pub;
	ExplainState *es;			/* this EXPLAIN statement's ExplainState */
	/*
	 *
	 * 本条 EXPLAIN 语句的 ExplainState。
	 */
	int8		format;			/* text or binary, like pq wire protocol */
	/*
	 *
	 * 文本或二进制，与 pq 线协议相同。
	 */
	TupleDesc	attrinfo;		/* the output tuple desc */
	/*
	 *
	 * 输出元组描述符。
	 */
	int			nattrs;			/* current number of columns */
	/*
	 *
	 * 当前列数。
	 */
	FmgrInfo   *finfos;			/* precomputed call info for output fns */
	/*
	 *
	 * 输出函数的预计算调用信息。
	 */
	MemoryContext tmpcontext;	/* per-row temporary memory context */
	/*
	 *
	 * 每行临时内存上下文。
	 */
	StringInfoData buf;			/* buffer to hold the constructed message */
	/*
	 *
	 * 存放构造出的消息的缓冲区。
	 */
	SerializeMetrics metrics;	/* collected metrics */
	/*
	 *
	 * 收集到的指标。
	 */
} SerializeDestReceiver;

/*
 * Get the function lookup info that we'll need for output.
 *
 * 取得输出所需的函数查找信息。
 *
 * This is a subset of what printtup_prepare_info() does.  We don't need to
 * cope with format choices varying across columns, so it's slightly simpler.
 *
 * 这是 printtup_prepare_info() 的一个子集。各列格式不会不同，因此稍简单。
 */
static void
serialize_prepare_info(SerializeDestReceiver *receiver,
					   TupleDesc typeinfo, int nattrs)
{
	/* get rid of any old data */
	/*
	 *
	 * 丢掉旧数据。
	 */
	if (receiver->finfos)
		pfree(receiver->finfos);
	receiver->finfos = NULL;

	receiver->attrinfo = typeinfo;
	receiver->nattrs = nattrs;
	if (nattrs <= 0)
		return;

	receiver->finfos = (FmgrInfo *) palloc0(nattrs * sizeof(FmgrInfo));

	for (int i = 0; i < nattrs; i++)
	{
		FmgrInfo   *finfo = receiver->finfos + i;
		Form_pg_attribute attr = TupleDescAttr(typeinfo, i);
		Oid			typoutput;
		Oid			typsend;
		bool		typisvarlena;

		if (receiver->format == 0)
		{
			/* wire protocol format text */
			/*
			 *
			 * 线协议格式：文本。
			 */
			getTypeOutputInfo(attr->atttypid,
							  &typoutput,
							  &typisvarlena);
			fmgr_info(typoutput, finfo);
		}
		else if (receiver->format == 1)
		{
			/* wire protocol format binary */
			/*
			 *
			 * 线协议格式：二进制。
			 */
			getTypeBinaryOutputInfo(attr->atttypid,
									&typsend,
									&typisvarlena);
			fmgr_info(typsend, finfo);
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unsupported format code: %d", receiver->format)));
	}
}

/*
 * serializeAnalyzeReceive - collect tuples for EXPLAIN (SERIALIZE)
 *
 * serializeAnalyzeReceive：为 EXPLAIN (SERIALIZE) 收集元组。
 *
 * This should match printtup() in printtup.c as closely as possible,
 * except for the addition of measurement code.
 *
 * 应尽量与 printtup.c 中的 printtup() 一致，只是增加了测量代码。
 */
static bool
serializeAnalyzeReceive(TupleTableSlot *slot, DestReceiver *self)
{
	TupleDesc	typeinfo = slot->tts_tupleDescriptor;
	SerializeDestReceiver *myState = (SerializeDestReceiver *) self;
	MemoryContext oldcontext;
	StringInfo	buf = &myState->buf;
	int			natts = typeinfo->natts;
	instr_time	start,
				end;
	BufferUsage instr_start;

	/* only measure time, buffers if requested */
	/*
	 *
	 * 仅在请求时测量时间和缓冲区。
	 */
	if (myState->es->timing)
		INSTR_TIME_SET_CURRENT(start);
	if (myState->es->buffers)
		instr_start = pgBufferUsage;

	/* Set or update my derived attribute info, if needed */
	/*
	 *
	 * 必要时设置或更新派生的属性信息。
	 */
	if (myState->attrinfo != typeinfo || myState->nattrs != natts)
		serialize_prepare_info(myState, typeinfo, natts);

	/* Make sure the tuple is fully deconstructed */
	/*
	 *
	 * 确保元组已完全拆开。
	 */
	slot_getallattrs(slot);

	/* Switch into per-row context so we can recover memory below */
	/*
	 *
	 * 切换到每行上下文，以便随后回收内存。
	 */
	oldcontext = MemoryContextSwitchTo(myState->tmpcontext);

	/*
	 * Prepare a DataRow message (note buffer is in per-query context)
	 *
	 * 准备 DataRow 消息（缓冲区位于每查询上下文）。
	 *
	 * Note that we fill a StringInfo buffer the same as printtup() does, so
	 * as to capture the costs of manipulating the strings accurately.
	 *
	 * 与 printtup() 一样填充 StringInfo，以便准确计入字符串操作的开销。
	 */
	pq_beginmessage_reuse(buf, PqMsg_DataRow);

	pq_sendint16(buf, natts);

	/*
	 * send the attributes of this tuple
	 *
	 * 输出该元组的各个属性。
	 */
	for (int i = 0; i < natts; i++)
	{
		FmgrInfo   *finfo = myState->finfos + i;
		Datum		attr = slot->tts_values[i];

		if (slot->tts_isnull[i])
		{
			pq_sendint32(buf, -1);
			continue;
		}

		if (myState->format == 0)
		{
			/* Text output */
			/*
			 *
			 * 文本输出。
			 */
			char	   *outputstr;

			outputstr = OutputFunctionCall(finfo, attr);
			pq_sendcountedtext(buf, outputstr, strlen(outputstr));
		}
		else
		{
			/* Binary output */
			/*
			 *
			 * 二进制输出。
			 */
			bytea	   *outputbytes;

			outputbytes = SendFunctionCall(finfo, attr);
			pq_sendint32(buf, VARSIZE(outputbytes) - VARHDRSZ);
			pq_sendbytes(buf, VARDATA(outputbytes),
						 VARSIZE(outputbytes) - VARHDRSZ);
		}
	}

	/*
	 * We mustn't call pq_endmessage_reuse(), since that would actually send
	 * the data to the client.  Just count the data, instead.  We can leave
	 * the buffer alone; it'll be reset on the next iteration (as would also
	 * happen in printtup()).
	 *
	 * 不能调用 pq_endmessage_reuse()，否则会把数据真正发给客户端。
	 * 这里只计数。缓冲区留到下一轮重置，printtup() 也是这样。
	 */
	myState->metrics.bytesSent += buf->len;

	/* Return to caller's context, and flush row's temporary memory */
	/*
	 *
	 * 回到调用者的上下文，并清掉本行的临时内存。
	 */
	MemoryContextSwitchTo(oldcontext);
	MemoryContextReset(myState->tmpcontext);

	/* Update timing data */
	/*
	 *
	 * 更新计时数据。
	 */
	if (myState->es->timing)
	{
		INSTR_TIME_SET_CURRENT(end);
		INSTR_TIME_ACCUM_DIFF(myState->metrics.timeSpent, end, start);
	}

	/* Update buffer metrics */
	/*
	 *
	 * 更新缓冲区指标。
	 */
	if (myState->es->buffers)
		BufferUsageAccumDiff(&myState->metrics.bufferUsage,
							 &pgBufferUsage,
							 &instr_start);

	return true;
}

/*
 * serializeAnalyzeStartup - start up the serializeAnalyze receiver
 *
 * serializeAnalyzeStartup：启动 serializeAnalyze 接收器。
 */
static void
serializeAnalyzeStartup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	SerializeDestReceiver *receiver = (SerializeDestReceiver *) self;

	Assert(receiver->es != NULL);

	switch (receiver->es->serialize)
	{
		case EXPLAIN_SERIALIZE_NONE:
			Assert(false);
			break;
		case EXPLAIN_SERIALIZE_TEXT:
			receiver->format = 0;	/* wire protocol format text */
			/*
			 *
			 * 线协议格式：文本。
			 */
			break;
		case EXPLAIN_SERIALIZE_BINARY:
			receiver->format = 1;	/* wire protocol format binary */
			/*
			 *
			 * 线协议格式：二进制。
			 */
			break;
	}

	/* Create per-row temporary memory context */
	/*
	 *
	 * 创建每行临时内存上下文。
	 */
	receiver->tmpcontext = AllocSetContextCreate(CurrentMemoryContext,
												 "SerializeTupleReceive",
												 ALLOCSET_DEFAULT_SIZES);

	/* The output buffer is re-used across rows, as in printtup.c */
	/*
	 *
	 * 输出缓冲区在各行之间复用，与 printtup.c 相同。
	 */
	initStringInfo(&receiver->buf);

	/* Initialize results counters */
	/*
	 *
	 * 初始化结果计数器。
	 */
	memset(&receiver->metrics, 0, sizeof(SerializeMetrics));
	INSTR_TIME_SET_ZERO(receiver->metrics.timeSpent);
}

/*
 * serializeAnalyzeShutdown - shut down the serializeAnalyze receiver
 *
 * serializeAnalyzeShutdown：关闭 serializeAnalyze 接收器。
 */
static void
serializeAnalyzeShutdown(DestReceiver *self)
{
	SerializeDestReceiver *receiver = (SerializeDestReceiver *) self;

	if (receiver->finfos)
		pfree(receiver->finfos);
	receiver->finfos = NULL;

	if (receiver->buf.data)
		pfree(receiver->buf.data);
	receiver->buf.data = NULL;

	if (receiver->tmpcontext)
		MemoryContextDelete(receiver->tmpcontext);
	receiver->tmpcontext = NULL;
}

/*
 * serializeAnalyzeDestroy - destroy the serializeAnalyze receiver
 *
 * serializeAnalyzeDestroy：销毁 serializeAnalyze 接收器。
 */
static void
serializeAnalyzeDestroy(DestReceiver *self)
{
	pfree(self);
}

/*
 * Build a DestReceiver for EXPLAIN (SERIALIZE) instrumentation.
 *
 * 构造用于 EXPLAIN (SERIALIZE) 测量的 DestReceiver。
 */
DestReceiver *
CreateExplainSerializeDestReceiver(ExplainState *es)
{
	SerializeDestReceiver *self;

	self = (SerializeDestReceiver *) palloc0(sizeof(SerializeDestReceiver));

	self->pub.receiveSlot = serializeAnalyzeReceive;
	self->pub.rStartup = serializeAnalyzeStartup;
	self->pub.rShutdown = serializeAnalyzeShutdown;
	self->pub.rDestroy = serializeAnalyzeDestroy;
	self->pub.mydest = DestExplainSerialize;

	self->es = es;

	return (DestReceiver *) self;
}

/*
 * GetSerializationMetrics - collect metrics
 *
 * GetSerializationMetrics：收集指标。
 *
 * We have to be careful here since the receiver could be an IntoRel
 * receiver if the subject statement is CREATE TABLE AS.  In that
 * case, return all-zeroes stats.
 *
 * 语句若是 CREATE TABLE AS，接收器可能是 IntoRel。
 * 此时返回全零统计。
 */
SerializeMetrics
GetSerializationMetrics(DestReceiver *dest)
{
	SerializeMetrics empty;

	if (dest->mydest == DestExplainSerialize)
		return ((SerializeDestReceiver *) dest)->metrics;

	memset(&empty, 0, sizeof(SerializeMetrics));
	INSTR_TIME_SET_ZERO(empty.timeSpent);

	return empty;
}

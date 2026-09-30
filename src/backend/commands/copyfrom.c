/*-------------------------------------------------------------------------
 *
 * copyfrom.c
 *		COPY <table> FROM file/program/client
 *
 * 从文件、程序或客户端把数据装入表。
 *
 * This file contains routines needed to efficiently load tuples into a
 * table.  That includes looking up the correct partition, firing triggers,
 * calling the table AM function to insert the data, and updating indexes.
 * Reading data from the input file or client and parsing it into Datums
 * is handled in copyfromparse.c.
 *
 * 本文件包含高效把元组装入表所需的例程。包括查找正确的分区、触发触发器、
 * 调用表访问方法函数插入数据，以及更新索引。
 * 从输入文件或客户端读取数据并解析为 Datum 的工作在 copyfromparse.c 中处理。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/copyfrom.c
 *
 * 标识
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>

#include "access/heapam.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "commands/copyapi.h"
#include "commands/copyfrom_internal.h"
#include "commands/progress.h"
#include "commands/trigger.h"
#include "executor/execPartition.h"
#include "executor/executor.h"
#include "executor/nodeModifyTable.h"
#include "executor/tuptable.h"
#include "foreign/fdwapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/miscnodes.h"
#include "optimizer/optimizer.h"
#include "pgstat.h"
#include "rewrite/rewriteHandler.h"
#include "storage/fd.h"
#include "tcop/tcopprot.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/portal.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

/*
 * No more than this many tuples per CopyMultiInsertBuffer
 *
 * 每个 CopyMultiInsertBuffer 最多这么多个元组
 *
 * Caution: Don't make this too big, as we could end up with this many
 * CopyMultiInsertBuffer items stored in CopyMultiInsertInfo's
 * multiInsertBuffers list.  Increasing this can cause quadratic growth in
 * memory requirements during copies into partitioned tables with a large
 * number of partitions.
 *
 * 注意：不要把这个值设得太大，否则 CopyMultiInsertInfo 的 multiInsertBuffers 列表里会有这么多项。
 * 增大它会使拷贝到分区很多的分区表时内存需求呈平方增长。
 */
#define MAX_BUFFERED_TUPLES		1000

/*
 * Flush buffers if there are >= this many bytes, as counted by the input
 * size, of tuples stored.
 *
 * 若按输入大小计，已存储元组的字节数达到该值，则刷出缓冲区。
 */
#define MAX_BUFFERED_BYTES		65535

/*
 * Trim the list of buffers back down to this number after flushing.  This
 * must be >= 2.
 *
 * 刷出后把缓冲区列表裁剪回这个数量。必须 >= 2。
 */
#define MAX_PARTITION_BUFFERS	32

/* Stores multi-insert data related to a single relation in CopyFrom. */
/*
 *
 * 保存 CopyFrom 中与单个关系相关的多行插入数据。
 */
typedef struct CopyMultiInsertBuffer
{
	TupleTableSlot *slots[MAX_BUFFERED_TUPLES]; /* Array to store tuples */
	/*
	 *
	 * 用于存放元组的数组
	 */
	ResultRelInfo *resultRelInfo;	/* ResultRelInfo for 'relid' */
	/*
	 *
	 * relid 对应的 ResultRelInfo
	 */
	BulkInsertState bistate;	/* BulkInsertState for this rel if plain
								 * table; NULL if foreign table */
	/*
	 *
	 * 若是普通表，则为该关系的 BulkInsertState；若是外部表则为 NULL
	 */
	int			nused;			/* number of 'slots' containing tuples */
	/*
	 *
	 * 含有元组的 slots 数量
	 */
	uint64		linenos[MAX_BUFFERED_TUPLES];	/* Line # of tuple in copy
												 * stream */
	/*
	 *
	 * 拷贝流中该元组的行号
	 */
} CopyMultiInsertBuffer;

/*
 * Stores one or many CopyMultiInsertBuffers and details about the size and
 * number of tuples which are stored in them.  This allows multiple buffers to
 * exist at once when COPYing into a partitioned table.
 *
 * 保存一个或多个 CopyMultiInsertBuffer，以及其中存储的元组大小与数量。
 * 这样在 COPY 到分区表时可以同时存在多个缓冲区。
 */
typedef struct CopyMultiInsertInfo
{
	List	   *multiInsertBuffers; /* List of tracked CopyMultiInsertBuffers */
	/*
	 *
	 * 正在跟踪的 CopyMultiInsertBuffer 列表
	 */
	int			bufferedTuples; /* number of tuples buffered over all buffers */
	/*
	 *
	 * 所有缓冲区中缓冲的元组数
	 */
	int			bufferedBytes;	/* number of bytes from all buffered tuples */
	/*
	 *
	 * 所有缓冲元组的字节数
	 */
	CopyFromState cstate;		/* Copy state for this CopyMultiInsertInfo */
	/*
	 *
	 * 该 CopyMultiInsertInfo 的 Copy 状态
	 */
	EState	   *estate;			/* Executor state used for COPY */
	/*
	 *
	 * COPY 使用的执行器状态
	 */
	CommandId	mycid;			/* Command Id used for COPY */
	/*
	 *
	 * COPY 使用的命令 Id
	 */
	int			ti_options;		/* table insert options */
	/*
	 *
	 * 表插入选项
	 */
} CopyMultiInsertInfo;


/* non-export function prototypes */
/*
 *
 * 非导出函数的原型
 */
static void ClosePipeFromProgram(CopyFromState cstate);

/*
 * Built-in format-specific routines. One-row callbacks are defined in
 * copyfromparse.c.
 *
 * 内置的格式专用例程。单行回调定义在 copyfromparse.c 中。
 */
static void CopyFromTextLikeInFunc(CopyFromState cstate, Oid atttypid, FmgrInfo *finfo,
								   Oid *typioparam);
static void CopyFromTextLikeStart(CopyFromState cstate, TupleDesc tupDesc);
static void CopyFromTextLikeEnd(CopyFromState cstate);
static void CopyFromBinaryInFunc(CopyFromState cstate, Oid atttypid,
								 FmgrInfo *finfo, Oid *typioparam);
static void CopyFromBinaryStart(CopyFromState cstate, TupleDesc tupDesc);
static void CopyFromBinaryEnd(CopyFromState cstate);


/*
 * COPY FROM routines for built-in formats.
 *
 * 内置格式的 COPY FROM 例程。
 *
 * CSV and text formats share the same TextLike routines except for the
 * one-row callback.
 *
 * CSV 与 text 格式除单行回调外共用同一套 TextLike 例程。
 */

/* text format */
/*
 *
 * text 格式
 */
static const CopyFromRoutine CopyFromRoutineText = {
	.CopyFromInFunc = CopyFromTextLikeInFunc,
	.CopyFromStart = CopyFromTextLikeStart,
	.CopyFromOneRow = CopyFromTextOneRow,
	.CopyFromEnd = CopyFromTextLikeEnd,
};

/* CSV format */
/*
 *
 * CSV 格式
 */
static const CopyFromRoutine CopyFromRoutineCSV = {
	.CopyFromInFunc = CopyFromTextLikeInFunc,
	.CopyFromStart = CopyFromTextLikeStart,
	.CopyFromOneRow = CopyFromCSVOneRow,
	.CopyFromEnd = CopyFromTextLikeEnd,
};

/* binary format */
/*
 *
 * binary 格式
 */
static const CopyFromRoutine CopyFromRoutineBinary = {
	.CopyFromInFunc = CopyFromBinaryInFunc,
	.CopyFromStart = CopyFromBinaryStart,
	.CopyFromOneRow = CopyFromBinaryOneRow,
	.CopyFromEnd = CopyFromBinaryEnd,
};

/* Return a COPY FROM routine for the given options */
/*
 *
 * 按给定选项返回 COPY FROM 例程
 */
static const CopyFromRoutine *
CopyFromGetRoutine(const CopyFormatOptions *opts)
{
	if (opts->csv_mode)
		return &CopyFromRoutineCSV;
	else if (opts->binary)
		return &CopyFromRoutineBinary;

	/* default is text */
	/*
	 *
	 * 默认是 text
	 */
	return &CopyFromRoutineText;
}

/* Implementation of the start callback for text and CSV formats */
/*
 *
 * text 与 CSV 格式的开始回调实现
 */
static void
CopyFromTextLikeStart(CopyFromState cstate, TupleDesc tupDesc)
{
	AttrNumber	attr_count;

	/*
	 * If encoding conversion is needed, we need another buffer to hold the
	 * converted input data.  Otherwise, we can just point input_buf to the
	 * same buffer as raw_buf.
	 *
	 * 若需要编码转换，则还需要一个缓冲区来保存转换后的输入数据。否则只需让 input_buf 指向与 raw_buf 相同的缓冲区。
	 */
	if (cstate->need_transcoding)
	{
		cstate->input_buf = (char *) palloc(INPUT_BUF_SIZE + 1);
		cstate->input_buf_index = cstate->input_buf_len = 0;
	}
	else
		cstate->input_buf = cstate->raw_buf;
	cstate->input_reached_eof = false;

	initStringInfo(&cstate->line_buf);

	/*
	 * Create workspace for CopyReadAttributes results; used by CSV and text
	 * format.
	 *
	 * 为 CopyReadAttributes 的结果创建工作区；CSV 与 text 格式使用。
	 */
	attr_count = list_length(cstate->attnumlist);
	cstate->max_fields = attr_count;
	cstate->raw_fields = (char **) palloc(attr_count * sizeof(char *));
}

/*
 * Implementation of the infunc callback for text and CSV formats. Assign
 * the input function data to the given *finfo.
 *
 * text 与 CSV 格式的 infunc 回调实现。把输入函数数据赋给给定的 finfo。
 */
static void
CopyFromTextLikeInFunc(CopyFromState cstate, Oid atttypid, FmgrInfo *finfo,
					   Oid *typioparam)
{
	Oid			func_oid;

	getTypeInputInfo(atttypid, &func_oid, typioparam);
	fmgr_info(func_oid, finfo);
}

/* Implementation of the end callback for text and CSV formats */
/*
 *
 * text 与 CSV 格式的结束回调实现
 */
static void
CopyFromTextLikeEnd(CopyFromState cstate)
{
	/* nothing to do */
	/*
	 *
	 * 无事可做
	 */
}

/* Implementation of the start callback for binary format */
/*
 *
 * binary 格式的开始回调实现
 */
static void
CopyFromBinaryStart(CopyFromState cstate, TupleDesc tupDesc)
{
	/* Read and verify binary header */
	/*
	 *
	 * 读取并校验二进制头
	 */
	ReceiveCopyBinaryHeader(cstate);
}

/*
 * Implementation of the infunc callback for binary format. Assign
 * the binary input function to the given *finfo.
 *
 * binary 格式的 infunc 回调实现。把二进制输入函数赋给给定的 finfo。
 */
static void
CopyFromBinaryInFunc(CopyFromState cstate, Oid atttypid,
					 FmgrInfo *finfo, Oid *typioparam)
{
	Oid			func_oid;

	getTypeBinaryInputInfo(atttypid, &func_oid, typioparam);
	fmgr_info(func_oid, finfo);
}

/* Implementation of the end callback for binary format */
/*
 *
 * binary 格式的结束回调实现
 */
static void
CopyFromBinaryEnd(CopyFromState cstate)
{
	/* nothing to do */
	/*
	 *
	 * 无事可做
	 */
}

/*
 * error context callback for COPY FROM
 *
 * COPY FROM 的错误上下文回调
 *
 * The argument for the error context must be CopyFromState.
 *
 * 错误上下文的参数必须是 CopyFromState。
 */
void
CopyFromErrorCallback(void *arg)
{
	CopyFromState cstate = (CopyFromState) arg;

	if (cstate->relname_only)
	{
		errcontext("COPY %s",
				   cstate->cur_relname);
		return;
	}
	if (cstate->opts.binary)
	{
		/* can't usefully display the data */
		/*
		 *
		 * 无法有效地显示这些数据
		 */
		if (cstate->cur_attname)
			errcontext("COPY %s, line %" PRIu64 ", column %s",
					   cstate->cur_relname,
					   cstate->cur_lineno,
					   cstate->cur_attname);
		else
			errcontext("COPY %s, line %" PRIu64,
					   cstate->cur_relname,
					   cstate->cur_lineno);
	}
	else
	{
		if (cstate->cur_attname && cstate->cur_attval)
		{
			/* error is relevant to a particular column */
			/*
			 *
			 * 错误与某一列相关
			 */
			char	   *attval;

			attval = CopyLimitPrintoutLength(cstate->cur_attval);
			errcontext("COPY %s, line %" PRIu64 ", column %s: \"%s\"",
					   cstate->cur_relname,
					   cstate->cur_lineno,
					   cstate->cur_attname,
					   attval);
			pfree(attval);
		}
		else if (cstate->cur_attname)
		{
			/* error is relevant to a particular column, value is NULL */
			/*
			 *
			 * 错误与某一列相关，且值为 NULL
			 */
			errcontext("COPY %s, line %" PRIu64 ", column %s: null input",
					   cstate->cur_relname,
					   cstate->cur_lineno,
					   cstate->cur_attname);
		}
		else
		{
			/*
			 * Error is relevant to a particular line.
			 *
			 * 错误与某一行相关。
			 *
			 * If line_buf still contains the correct line, print it.
			 *
			 * 若 line_buf 仍包含正确的那一行，则打印它。
			 */
			if (cstate->line_buf_valid)
			{
				char	   *lineval;

				lineval = CopyLimitPrintoutLength(cstate->line_buf.data);
				errcontext("COPY %s, line %" PRIu64 ": \"%s\"",
						   cstate->cur_relname,
						   cstate->cur_lineno, lineval);
				pfree(lineval);
			}
			else
			{
				errcontext("COPY %s, line %" PRIu64,
						   cstate->cur_relname,
						   cstate->cur_lineno);
			}
		}
	}
}

/*
 * Make sure we don't print an unreasonable amount of COPY data in a message.
 *
 * 确保消息中不会打印数量不合理的 COPY 数据。
 *
 * Returns a pstrdup'd copy of the input.
 *
 * 返回输入的 pstrdup 副本。
 */
char *
CopyLimitPrintoutLength(const char *str)
{
#define MAX_COPY_DATA_DISPLAY 100

	int			slen = strlen(str);
	int			len;
	char	   *res;

	/* Fast path if definitely okay */
	/*
	 *
	 * 若肯定没问题，则走快速路径
	 */
	if (slen <= MAX_COPY_DATA_DISPLAY)
		return pstrdup(str);

	/* Apply encoding-dependent truncation */
	/*
	 *
	 * 按编码相关的方式截断
	 */
	len = pg_mbcliplen(str, slen, MAX_COPY_DATA_DISPLAY);

	/*
	 * Truncate, and add "..." to show we truncated the input.
	 *
	 * 截断，并加上 ... 表示输入已被截断。
	 */
	res = (char *) palloc(len + 4);
	memcpy(res, str, len);
	strcpy(res + len, "...");

	return res;
}

/*
 * Allocate memory and initialize a new CopyMultiInsertBuffer for this
 * ResultRelInfo.
 *
 * 分配内存并为该 ResultRelInfo 初始化一个新的 CopyMultiInsertBuffer。
 */
static CopyMultiInsertBuffer *
CopyMultiInsertBufferInit(ResultRelInfo *rri)
{
	CopyMultiInsertBuffer *buffer;

	buffer = (CopyMultiInsertBuffer *) palloc(sizeof(CopyMultiInsertBuffer));
	memset(buffer->slots, 0, sizeof(TupleTableSlot *) * MAX_BUFFERED_TUPLES);
	buffer->resultRelInfo = rri;
	buffer->bistate = (rri->ri_FdwRoutine == NULL) ? GetBulkInsertState() : NULL;
	buffer->nused = 0;

	return buffer;
}

/*
 * Make a new buffer for this ResultRelInfo.
 *
 * 为该 ResultRelInfo 创建一个新缓冲区。
 */
static inline void
CopyMultiInsertInfoSetupBuffer(CopyMultiInsertInfo *miinfo,
							   ResultRelInfo *rri)
{
	CopyMultiInsertBuffer *buffer;

	buffer = CopyMultiInsertBufferInit(rri);

	/* Setup back-link so we can easily find this buffer again */
	/*
	 *
	 * 建立反向链接，以便以后容易再找到该缓冲区
	 */
	rri->ri_CopyMultiInsertBuffer = buffer;
	/* Record that we're tracking this buffer */
	/*
	 *
	 * 记录我们正在跟踪该缓冲区
	 */
	miinfo->multiInsertBuffers = lappend(miinfo->multiInsertBuffers, buffer);
}

/*
 * Initialize an already allocated CopyMultiInsertInfo.
 *
 * 初始化一个已经分配的 CopyMultiInsertInfo。
 *
 * If rri is a non-partitioned table then a CopyMultiInsertBuffer is set up
 * for that table.
 *
 * 若 rri 是非分区表，则为该表设置一个 CopyMultiInsertBuffer。
 */
static void
CopyMultiInsertInfoInit(CopyMultiInsertInfo *miinfo, ResultRelInfo *rri,
						CopyFromState cstate, EState *estate, CommandId mycid,
						int ti_options)
{
	miinfo->multiInsertBuffers = NIL;
	miinfo->bufferedTuples = 0;
	miinfo->bufferedBytes = 0;
	miinfo->cstate = cstate;
	miinfo->estate = estate;
	miinfo->mycid = mycid;
	miinfo->ti_options = ti_options;

	/*
	 * Only setup the buffer when not dealing with a partitioned table.
	 * Buffers for partitioned tables will just be setup when we need to send
	 * tuples their way for the first time.
	 *
	 * 只有在不是分区表时才设置缓冲区。分区表的缓冲区要到第一次需要向它们发送元组时才设置。
	 */
	if (rri->ri_RelationDesc->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		CopyMultiInsertInfoSetupBuffer(miinfo, rri);
}

/*
 * Returns true if the buffers are full
 *
 * 若缓冲区已满则返回 true
 */
static inline bool
CopyMultiInsertInfoIsFull(CopyMultiInsertInfo *miinfo)
{
	if (miinfo->bufferedTuples >= MAX_BUFFERED_TUPLES ||
		miinfo->bufferedBytes >= MAX_BUFFERED_BYTES)
		return true;
	return false;
}

/*
 * Returns true if we have no buffered tuples
 *
 * 若没有缓冲的元组则返回 true
 */
static inline bool
CopyMultiInsertInfoIsEmpty(CopyMultiInsertInfo *miinfo)
{
	return miinfo->bufferedTuples == 0;
}

/*
 * Write the tuples stored in 'buffer' out to the table.
 *
 * 把 buffer 中存储的元组写到表中。
 */
static inline void
CopyMultiInsertBufferFlush(CopyMultiInsertInfo *miinfo,
						   CopyMultiInsertBuffer *buffer,
						   int64 *processed)
{
	CopyFromState cstate = miinfo->cstate;
	EState	   *estate = miinfo->estate;
	int			nused = buffer->nused;
	ResultRelInfo *resultRelInfo = buffer->resultRelInfo;
	TupleTableSlot **slots = buffer->slots;
	int			i;

	if (resultRelInfo->ri_FdwRoutine)
	{
		int			batch_size = resultRelInfo->ri_BatchSize;
		int			sent = 0;

		Assert(buffer->bistate == NULL);

		/* Ensure that the FDW supports batching and it's enabled */
		/*
		 *
		 * 确保 FDW 支持批处理且已启用
		 */
		Assert(resultRelInfo->ri_FdwRoutine->ExecForeignBatchInsert);
		Assert(batch_size > 1);

		/*
		 * We suppress error context information other than the relation name,
		 * if one of the operations below fails.
		 *
		 * 若下面某个操作失败，除关系名外抑制其他错误上下文信息。
		 */
		Assert(!cstate->relname_only);
		cstate->relname_only = true;

		while (sent < nused)
		{
			int			size = (batch_size < nused - sent) ? batch_size : (nused - sent);
			int			inserted = size;
			TupleTableSlot **rslots;

			/* insert into foreign table: let the FDW do it */
			/*
			 *
			 * 插入外部表：交给 FDW 处理
			 */
			rslots =
				resultRelInfo->ri_FdwRoutine->ExecForeignBatchInsert(estate,
																	 resultRelInfo,
																	 &slots[sent],
																	 NULL,
																	 &inserted);

			sent += size;

			/* No need to do anything if there are no inserted rows */
			/*
			 *
			 * 若没有插入的行，则不必做任何事
			 */
			if (inserted <= 0)
				continue;

			/* Triggers on foreign tables should not have transition tables */
			/*
			 *
			 * 外部表上的触发器不应有 transition table
			 */
			Assert(resultRelInfo->ri_TrigDesc == NULL ||
				   resultRelInfo->ri_TrigDesc->trig_insert_new_table == false);

			/* Run AFTER ROW INSERT triggers */
			/*
			 *
			 * 运行 AFTER ROW INSERT 触发器
			 */
			if (resultRelInfo->ri_TrigDesc != NULL &&
				resultRelInfo->ri_TrigDesc->trig_insert_after_row)
			{
				Oid			relid = RelationGetRelid(resultRelInfo->ri_RelationDesc);

				for (i = 0; i < inserted; i++)
				{
					TupleTableSlot *slot = rslots[i];

					/*
					 * AFTER ROW Triggers might reference the tableoid column,
					 * so (re-)initialize tts_tableOid before evaluating them.
					 *
					 * AFTER ROW 触发器可能会引用 tableoid 列，因此在计算它们之前（重新）初始化 tts_tableOid。
					 */
					slot->tts_tableOid = relid;

					ExecARInsertTriggers(estate, resultRelInfo,
										 slot, NIL,
										 cstate->transition_capture);
				}
			}

			/* Update the row counter and progress of the COPY command */
			/*
			 *
			 * 更新行计数和 COPY 命令的进度
			 */
			*processed += inserted;
			pgstat_progress_update_param(PROGRESS_COPY_TUPLES_PROCESSED,
										 *processed);
		}

		for (i = 0; i < nused; i++)
			ExecClearTuple(slots[i]);

		/* reset relname_only */
		/*
		 *
		 * 重置 relname_only
		 */
		cstate->relname_only = false;
	}
	else
	{
		CommandId	mycid = miinfo->mycid;
		int			ti_options = miinfo->ti_options;
		bool		line_buf_valid = cstate->line_buf_valid;
		uint64		save_cur_lineno = cstate->cur_lineno;
		MemoryContext oldcontext;

		Assert(buffer->bistate != NULL);

		/*
		 * Print error context information correctly, if one of the operations
		 * below fails.
		 *
		 * 若下面某个操作失败，正确打印错误上下文信息。
		 */
		cstate->line_buf_valid = false;

		/*
		 * table_multi_insert may leak memory, so switch to short-lived memory
		 * context before calling it.
		 *
		 * table_multi_insert 可能泄漏内存，因此调用前切换到短生命周期的内存上下文。
		 */
		oldcontext = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));
		table_multi_insert(resultRelInfo->ri_RelationDesc,
						   slots,
						   nused,
						   mycid,
						   ti_options,
						   buffer->bistate);
		MemoryContextSwitchTo(oldcontext);

		for (i = 0; i < nused; i++)
		{
			/*
			 * If there are any indexes, update them for all the inserted
			 * tuples, and run AFTER ROW INSERT triggers.
			 *
			 * 若有索引，则为所有插入的元组更新索引，并运行 AFTER ROW INSERT 触发器。
			 */
			if (resultRelInfo->ri_NumIndices > 0)
			{
				List	   *recheckIndexes;

				cstate->cur_lineno = buffer->linenos[i];
				recheckIndexes =
					ExecInsertIndexTuples(resultRelInfo,
										  buffer->slots[i], estate, false,
										  false, NULL, NIL, false);
				ExecARInsertTriggers(estate, resultRelInfo,
									 slots[i], recheckIndexes,
									 cstate->transition_capture);
				list_free(recheckIndexes);
			}

			/*
			 * There's no indexes, but see if we need to run AFTER ROW INSERT
			 * triggers anyway.
			 *
			 * 没有索引，但仍要看是否需要运行 AFTER ROW INSERT 触发器。
			 */
			else if (resultRelInfo->ri_TrigDesc != NULL &&
					 (resultRelInfo->ri_TrigDesc->trig_insert_after_row ||
					  resultRelInfo->ri_TrigDesc->trig_insert_new_table))
			{
				cstate->cur_lineno = buffer->linenos[i];
				ExecARInsertTriggers(estate, resultRelInfo,
									 slots[i], NIL,
									 cstate->transition_capture);
			}

			ExecClearTuple(slots[i]);
		}

		/* Update the row counter and progress of the COPY command */
		/*
		 *
		 * 更新行计数和 COPY 命令的进度
		 */
		*processed += nused;
		pgstat_progress_update_param(PROGRESS_COPY_TUPLES_PROCESSED,
									 *processed);

		/* reset cur_lineno and line_buf_valid to what they were */
		/*
		 *
		 * 把 cur_lineno 和 line_buf_valid 恢复成原来的值
		 */
		cstate->line_buf_valid = line_buf_valid;
		cstate->cur_lineno = save_cur_lineno;
	}

	/* Mark that all slots are free */
	/*
	 *
	 * 把所有槽标记为空闲
	 */
	buffer->nused = 0;
}

/*
 * Drop used slots and free member for this buffer.
 *
 * 丢弃已使用的槽，并释放该缓冲区的成员。
 *
 * The buffer must be flushed before cleanup.
 *
 * 清理之前必须先刷出缓冲区。
 */
static inline void
CopyMultiInsertBufferCleanup(CopyMultiInsertInfo *miinfo,
							 CopyMultiInsertBuffer *buffer)
{
	ResultRelInfo *resultRelInfo = buffer->resultRelInfo;
	int			i;

	/* Ensure buffer was flushed */
	/*
	 *
	 * 确保缓冲区已经刷出
	 */
	Assert(buffer->nused == 0);

	/* Remove back-link to ourself */
	/*
	 *
	 * 移除指向自身的反向链接
	 */
	resultRelInfo->ri_CopyMultiInsertBuffer = NULL;

	if (resultRelInfo->ri_FdwRoutine == NULL)
	{
		Assert(buffer->bistate != NULL);
		FreeBulkInsertState(buffer->bistate);
	}
	else
		Assert(buffer->bistate == NULL);

	/* Since we only create slots on demand, just drop the non-null ones. */
	/*
	 *
	 * 因为槽是按需创建的，只丢弃非空的那些。
	 */
	for (i = 0; i < MAX_BUFFERED_TUPLES && buffer->slots[i] != NULL; i++)
		ExecDropSingleTupleTableSlot(buffer->slots[i]);

	if (resultRelInfo->ri_FdwRoutine == NULL)
		table_finish_bulk_insert(resultRelInfo->ri_RelationDesc,
								 miinfo->ti_options);

	pfree(buffer);
}

/*
 * Write out all stored tuples in all buffers out to the tables.
 *
 * 把所有缓冲区中存储的元组全部写到表中。
 *
 * Once flushed we also trim the tracked buffers list down to size by removing
 * the buffers created earliest first.
 *
 * 刷出之后，再把跟踪的缓冲区列表裁剪到规定大小，优先移除最早创建的缓冲区。
 *
 * Callers should pass 'curr_rri' as the ResultRelInfo that's currently being
 * used.  When cleaning up old buffers we'll never remove the one for
 * 'curr_rri'.
 *
 * 调用方应把 curr_rri 设为当前正在使用的 ResultRelInfo。清理旧缓冲区时，永远不会移除 curr_rri 对应的那个。
 */
static inline void
CopyMultiInsertInfoFlush(CopyMultiInsertInfo *miinfo, ResultRelInfo *curr_rri,
						 int64 *processed)
{
	ListCell   *lc;

	foreach(lc, miinfo->multiInsertBuffers)
	{
		CopyMultiInsertBuffer *buffer = (CopyMultiInsertBuffer *) lfirst(lc);

		CopyMultiInsertBufferFlush(miinfo, buffer, processed);
	}

	miinfo->bufferedTuples = 0;
	miinfo->bufferedBytes = 0;

	/*
	 * Trim the list of tracked buffers down if it exceeds the limit.  Here we
	 * remove buffers starting with the ones we created first.  It seems less
	 * likely that these older ones will be needed than the ones that were
	 * just created.
	 *
	 * 若跟踪的缓冲区列表超过上限，则裁剪它。这里从最早创建的缓冲区开始移除。
	 * 这些较旧的缓冲区比刚刚创建的更不可能再被需要。
	 */
	while (list_length(miinfo->multiInsertBuffers) > MAX_PARTITION_BUFFERS)
	{
		CopyMultiInsertBuffer *buffer;

		buffer = (CopyMultiInsertBuffer *) linitial(miinfo->multiInsertBuffers);

		/*
		 * We never want to remove the buffer that's currently being used, so
		 * if we happen to find that then move it to the end of the list.
		 *
		 * 永远不想移除当前正在使用的缓冲区，因此若碰巧找到它，就把它移到列表末尾。
		 */
		if (buffer->resultRelInfo == curr_rri)
		{
			/*
			 * The code below would misbehave if we were trying to reduce the
			 * list to less than two items.
			 *
			 * 若试图把列表缩减到少于两项，下面的代码会行为异常。
			 */
			StaticAssertDecl(MAX_PARTITION_BUFFERS >= 2,
							 "MAX_PARTITION_BUFFERS must be >= 2");

			miinfo->multiInsertBuffers = list_delete_first(miinfo->multiInsertBuffers);
			miinfo->multiInsertBuffers = lappend(miinfo->multiInsertBuffers, buffer);
			buffer = (CopyMultiInsertBuffer *) linitial(miinfo->multiInsertBuffers);
		}

		CopyMultiInsertBufferCleanup(miinfo, buffer);
		miinfo->multiInsertBuffers = list_delete_first(miinfo->multiInsertBuffers);
	}
}

/*
 * Cleanup allocated buffers and free memory
 *
 * 清理已分配的缓冲区并释放内存
 */
static inline void
CopyMultiInsertInfoCleanup(CopyMultiInsertInfo *miinfo)
{
	ListCell   *lc;

	foreach(lc, miinfo->multiInsertBuffers)
		CopyMultiInsertBufferCleanup(miinfo, lfirst(lc));

	list_free(miinfo->multiInsertBuffers);
}

/*
 * Get the next TupleTableSlot that the next tuple should be stored in.
 *
 * 取得下一个元组应存入的 TupleTableSlot。
 *
 * Callers must ensure that the buffer is not full.
 *
 * 调用方必须确保缓冲区未满。
 *
 * Note: 'miinfo' is unused but has been included for consistency with the
 * other functions in this area.
 *
 * 注意：miinfo 未被使用，但为了与这一带的其他函数保持一致而保留。
 */
static inline TupleTableSlot *
CopyMultiInsertInfoNextFreeSlot(CopyMultiInsertInfo *miinfo,
								ResultRelInfo *rri)
{
	CopyMultiInsertBuffer *buffer = rri->ri_CopyMultiInsertBuffer;
	int			nused;

	Assert(buffer != NULL);
	Assert(buffer->nused < MAX_BUFFERED_TUPLES);

	nused = buffer->nused;

	if (buffer->slots[nused] == NULL)
		buffer->slots[nused] = table_slot_create(rri->ri_RelationDesc, NULL);
	return buffer->slots[nused];
}

/*
 * Record the previously reserved TupleTableSlot that was reserved by
 * CopyMultiInsertInfoNextFreeSlot as being consumed.
 *
 * 把先前由 CopyMultiInsertInfoNextFreeSlot 预留的 TupleTableSlot 记为已消耗。
 */
static inline void
CopyMultiInsertInfoStore(CopyMultiInsertInfo *miinfo, ResultRelInfo *rri,
						 TupleTableSlot *slot, int tuplen, uint64 lineno)
{
	CopyMultiInsertBuffer *buffer = rri->ri_CopyMultiInsertBuffer;

	Assert(buffer != NULL);
	Assert(slot == buffer->slots[buffer->nused]);

	/* Store the line number so we can properly report any errors later */
	/*
	 *
	 * 记录行号，以便稍后正确报告错误
	 */
	buffer->linenos[buffer->nused] = lineno;

	/* Record this slot as being used */
	/*
	 *
	 * 把该槽记为已使用
	 */
	buffer->nused++;

	/* Update how many tuples are stored and their size */
	/*
	 *
	 * 更新已存储的元组数量及其大小
	 */
	miinfo->bufferedTuples++;
	miinfo->bufferedBytes += tuplen;
}

/*
 * 核心流程概览：
 * BeginCopyFrom：打开输入，准备目标表、WHERE 过滤与分区路由。
 * CopyFrom：循环 NextCopyFrom，经 BEFORE 触发器、分区选择后插入。
 * CopyMultiInsert*：按分区缓存元组并批量 table_multi_insert。
 * EndCopyFrom / ClosePipeFromProgram：结束输入并关闭管道。
 */

/*
 * Copy FROM file to relation.
 *
 * 把文件 COPY 到关系中。
 */
uint64
CopyFrom(CopyFromState cstate)
{
	ResultRelInfo *resultRelInfo;
	ResultRelInfo *target_resultRelInfo;
	ResultRelInfo *prevResultRelInfo = NULL;
	EState	   *estate = CreateExecutorState(); /* for ExecConstraints() */
	/*
	 *
	 * 供 ExecConstraints() 使用
	 */
	ModifyTableState *mtstate;
	ExprContext *econtext;
	TupleTableSlot *singleslot = NULL;
	MemoryContext oldcontext = CurrentMemoryContext;

	PartitionTupleRouting *proute = NULL;
	ErrorContextCallback errcallback;
	CommandId	mycid = GetCurrentCommandId(true);
	int			ti_options = 0; /* start with default options for insert */
	/*
	 *
	 * 从插入的默认选项开始
	 */
	BulkInsertState bistate = NULL;
	CopyInsertMethod insertMethod;
	CopyMultiInsertInfo multiInsertInfo = {0};	/* pacify compiler */
	/*
	 *
	 * 安抚编译器
	 */
	int64		processed = 0;
	int64		excluded = 0;
	bool		has_before_insert_row_trig;
	bool		has_instead_insert_row_trig;
	bool		leafpart_use_multi_insert = false;

	Assert(cstate->rel);
	Assert(list_length(cstate->range_table) == 1);

	if (cstate->opts.on_error != COPY_ON_ERROR_STOP)
		Assert(cstate->escontext);

	/*
	 * The target must be a plain, foreign, or partitioned relation, or have
	 * an INSTEAD OF INSERT row trigger.  (Currently, such triggers are only
	 * allowed on views, so we only hint about them in the view case.)
	 *
	 * 目标必须是普通表、外部表或分区关系，或者有 INSTEAD OF INSERT 行触发器。
	 * （目前这类触发器只允许在视图上，因此只在视图情况下提示它们。）
	 */
	if (cstate->rel->rd_rel->relkind != RELKIND_RELATION &&
		cstate->rel->rd_rel->relkind != RELKIND_FOREIGN_TABLE &&
		cstate->rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE &&
		!(cstate->rel->trigdesc &&
		  cstate->rel->trigdesc->trig_insert_instead_row))
	{
		if (cstate->rel->rd_rel->relkind == RELKIND_VIEW)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot copy to view \"%s\"",
							RelationGetRelationName(cstate->rel)),
					 errhint("To enable copying to a view, provide an INSTEAD OF INSERT trigger.")));
		else if (cstate->rel->rd_rel->relkind == RELKIND_MATVIEW)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot copy to materialized view \"%s\"",
							RelationGetRelationName(cstate->rel))));
		else if (cstate->rel->rd_rel->relkind == RELKIND_SEQUENCE)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot copy to sequence \"%s\"",
							RelationGetRelationName(cstate->rel))));
		else
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot copy to non-table relation \"%s\"",
							RelationGetRelationName(cstate->rel))));
	}

	/*
	 * If the target file is new-in-transaction, we assume that checking FSM
	 * for free space is a waste of time.  This could possibly be wrong, but
	 * it's unlikely.
	 *
	 * 若目标文件是本事务中新建的，则假定检查 FSM 空闲空间是浪费时间。这有可能是错的，但不太可能。
	 */
	if (RELKIND_HAS_STORAGE(cstate->rel->rd_rel->relkind) &&
		(cstate->rel->rd_createSubid != InvalidSubTransactionId ||
		 cstate->rel->rd_firstRelfilelocatorSubid != InvalidSubTransactionId))
		ti_options |= TABLE_INSERT_SKIP_FSM;

	/*
	 * Optimize if new relation storage was created in this subxact or one of
	 * its committed children and we won't see those rows later as part of an
	 * earlier scan or command. The subxact test ensures that if this subxact
	 * aborts then the frozen rows won't be visible after xact cleanup.  Note
	 * that the stronger test of exactly which subtransaction created it is
	 * crucial for correctness of this optimization. The test for an earlier
	 * scan or command tolerates false negatives. FREEZE causes other sessions
	 * to see rows they would not see under MVCC, and a false negative merely
	 * spreads that anomaly to the current session.
	 *
	 * 若新关系存储是在本子事务或其已提交的子事务中创建的，并且之后不会作为更早的扫描或命令的一部分再看到这些行，则可以优化。
	 * 子事务测试保证：若本子事务中止，冻结的行在事务清理后不可见。
	 * 精确判断是哪个子事务创建了它，对这个优化的正确性至关重要。对更早扫描或命令的测试允许假阴性。
	 * FREEZE 会让其他会话看到在 MVCC 下看不到的行，假阴性只是把这种异常扩散到当前会话。
	 */
	if (cstate->opts.freeze)
	{
		/*
		 * We currently disallow COPY FREEZE on partitioned tables.  The
		 * reason for this is that we've simply not yet opened the partitions
		 * to determine if the optimization can be applied to them.  We could
		 * go and open them all here, but doing so may be quite a costly
		 * overhead for small copies.  In any case, we may just end up routing
		 * tuples to a small number of partitions.  It seems better just to
		 * raise an ERROR for partitioned tables.
		 *
		 * 目前不允许对分区表做 COPY FREEZE。原因是我们还没有打开各个分区，无法判断能否对它们应用该优化。
		 * 可以在这里把它们全部打开，但对小规模拷贝来说开销可能很大。而且最终也许只会把元组路由到少数分区。
		 * 对分区表直接报错似乎更好。
		 */
		if (cstate->rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		{
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot perform COPY FREEZE on a partitioned table")));
		}

		/* There's currently no support for COPY FREEZE on foreign tables. */
		/*
		 *
		 * 目前不支持对外部表做 COPY FREEZE。
		 */
		if (cstate->rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot perform COPY FREEZE on a foreign table")));

		/*
		 * Tolerate one registration for the benefit of FirstXactSnapshot.
		 * Scan-bearing queries generally create at least two registrations,
		 * though relying on that is fragile, as is ignoring ActiveSnapshot.
		 * Clear CatalogSnapshot to avoid counting its registration.  We'll
		 * still detect ongoing catalog scans, each of which separately
		 * registers the snapshot it uses.
		 *
		 * 为 FirstXactSnapshot 容忍一次注册。
		 * 带扫描的查询通常至少创建两次注册，但依赖这一点并不牢靠，忽略 ActiveSnapshot 也同样脆弱。
		 * 清除 CatalogSnapshot，以免把它的注册算进去。仍在进行的目录扫描各自会注册自己使用的快照，我们仍能检测到。
		 */
		InvalidateCatalogSnapshot();
		if (!ThereAreNoPriorRegisteredSnapshots() || !ThereAreNoReadyPortals())
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
					 errmsg("cannot perform COPY FREEZE because of prior transaction activity")));

		if (cstate->rel->rd_createSubid != GetCurrentSubTransactionId() &&
			cstate->rel->rd_newRelfilelocatorSubid != GetCurrentSubTransactionId())
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot perform COPY FREEZE because the table was not created or truncated in the current subtransaction")));

		ti_options |= TABLE_INSERT_FROZEN;
	}

	/*
	 * We need a ResultRelInfo so we can use the regular executor's
	 * index-entry-making machinery.  (There used to be a huge amount of code
	 * here that basically duplicated execUtils.c ...)
	 *
	 * 需要一个 ResultRelInfo，才能使用常规执行器创建索引项的机制。
	 * （这里曾经有大量基本上重复 execUtils.c 的代码……）
	 */
	ExecInitRangeTable(estate, cstate->range_table, cstate->rteperminfos,
					   bms_make_singleton(1));
	resultRelInfo = target_resultRelInfo = makeNode(ResultRelInfo);
	ExecInitResultRelation(estate, resultRelInfo, 1);

	/* Verify the named relation is a valid target for INSERT */
	/*
	 *
	 * 确认指定的关系是合法的 INSERT 目标
	 */
	CheckValidResultRel(resultRelInfo, CMD_INSERT, ONCONFLICT_NONE, NIL);

	ExecOpenIndices(resultRelInfo, false);

	/*
	 * Set up a ModifyTableState so we can let FDW(s) init themselves for
	 * foreign-table result relation(s).
	 *
	 * 建立一个 ModifyTableState，以便让 FDW 为外部表结果关系做初始化。
	 */
	mtstate = makeNode(ModifyTableState);
	mtstate->ps.plan = NULL;
	mtstate->ps.state = estate;
	mtstate->operation = CMD_INSERT;
	mtstate->mt_nrels = 1;
	mtstate->resultRelInfo = resultRelInfo;
	mtstate->rootResultRelInfo = resultRelInfo;

	if (resultRelInfo->ri_FdwRoutine != NULL &&
		resultRelInfo->ri_FdwRoutine->BeginForeignInsert != NULL)
		resultRelInfo->ri_FdwRoutine->BeginForeignInsert(mtstate,
														 resultRelInfo);

	/*
	 * Also, if the named relation is a foreign table, determine if the FDW
	 * supports batch insert and determine the batch size (a FDW may support
	 * batching, but it may be disabled for the server/table).
	 *
	 * 另外，若指定的关系是外部表，则判断 FDW 是否支持批量插入，并确定批大小（FDW 可能支持批处理，但在服务器或表上被禁用）。
	 *
	 * If the FDW does not support batching, we set the batch size to 1.
	 *
	 * 若 FDW 不支持批处理，则把批大小设为 1。
	 */
	if (resultRelInfo->ri_FdwRoutine != NULL &&
		resultRelInfo->ri_FdwRoutine->GetForeignModifyBatchSize &&
		resultRelInfo->ri_FdwRoutine->ExecForeignBatchInsert)
		resultRelInfo->ri_BatchSize =
			resultRelInfo->ri_FdwRoutine->GetForeignModifyBatchSize(resultRelInfo);
	else
		resultRelInfo->ri_BatchSize = 1;

	Assert(resultRelInfo->ri_BatchSize >= 1);

	/* Prepare to catch AFTER triggers. */
	/*
	 *
	 * 准备捕获 AFTER 触发器。
	 */
	AfterTriggerBeginQuery();

	/*
	 * If there are any triggers with transition tables on the named relation,
	 * we need to be prepared to capture transition tuples.
	 *
	 * 若指定关系上有带 transition table 的触发器，则需要准备捕获 transition 元组。
	 *
	 * Because partition tuple routing would like to know about whether
	 * transition capture is active, we also set it in mtstate, which is
	 * passed to ExecFindPartition() below.
	 *
	 * 因为分区元组路由想知道 transition 捕获是否处于活动状态，我们也在传给下面 ExecFindPartition() 的 mtstate 中设置它。
	 */
	cstate->transition_capture = mtstate->mt_transition_capture =
		MakeTransitionCaptureState(cstate->rel->trigdesc,
								   RelationGetRelid(cstate->rel),
								   CMD_INSERT);

	/*
	 * If the named relation is a partitioned table, initialize state for
	 * CopyFrom tuple routing.
	 *
	 * 若指定的关系是分区表，则为 CopyFrom 的元组路由初始化状态。
	 */
	if (cstate->rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		proute = ExecSetupPartitionTupleRouting(estate, cstate->rel);

	if (cstate->whereClause)
		cstate->qualexpr = ExecInitQual(castNode(List, cstate->whereClause),
										&mtstate->ps);

	/*
	 * It's generally more efficient to prepare a bunch of tuples for
	 * insertion, and insert them in one
	 * table_multi_insert()/ExecForeignBatchInsert() call, than call
	 * table_tuple_insert()/ExecForeignInsert() separately for every tuple.
	 * However, there are a number of reasons why we might not be able to do
	 * this.  These are explained below.
	 *
	 * 通常先准备一批元组，再用一次 table_multi_insert()/ExecForeignBatchInsert() 插入，比每个元组分别调用 table_tuple_insert()/ExecForeignInsert() 更高效。
	 * 但有多种原因使我们无法这样做。下面会说明。
	 */
	if (resultRelInfo->ri_TrigDesc != NULL &&
		(resultRelInfo->ri_TrigDesc->trig_insert_before_row ||
		 resultRelInfo->ri_TrigDesc->trig_insert_instead_row))
	{
		/*
		 * Can't support multi-inserts when there are any BEFORE/INSTEAD OF
		 * triggers on the table. Such triggers might query the table we're
		 * inserting into and act differently if the tuples that have already
		 * been processed and prepared for insertion are not there.
		 *
		 * 若表上有任何 BEFORE/INSTEAD OF 触发器，则不能支持多行插入。
		 * 这类触发器可能会查询正在插入的表，若已经处理并准备插入的元组还不在表中，行为会不同。
		 */
		insertMethod = CIM_SINGLE;
	}
	else if (resultRelInfo->ri_FdwRoutine != NULL &&
			 resultRelInfo->ri_BatchSize == 1)
	{
		/*
		 * Can't support multi-inserts to a foreign table if the FDW does not
		 * support batching, or it's disabled for the server or foreign table.
		 *
		 * 若 FDW 不支持批处理，或在服务器或外部表上被禁用，则不能对外部表做多行插入。
		 */
		insertMethod = CIM_SINGLE;
	}
	else if (proute != NULL && resultRelInfo->ri_TrigDesc != NULL &&
			 resultRelInfo->ri_TrigDesc->trig_insert_new_table)
	{
		/*
		 * For partitioned tables we can't support multi-inserts when there
		 * are any statement level insert triggers. It might be possible to
		 * allow partitioned tables with such triggers in the future, but for
		 * now, CopyMultiInsertInfoFlush expects that any after row insert and
		 * statement level insert triggers are on the same relation.
		 *
		 * 对分区表，若存在任何语句级插入触发器，则不能支持多行插入。
		 * 将来也许可以允许带这类触发器的分区表，但目前 CopyMultiInsertInfoFlush 要求 after row insert 与语句级插入触发器都在同一关系上。
		 */
		insertMethod = CIM_SINGLE;
	}
	else if (cstate->volatile_defexprs)
	{
		/*
		 * Can't support multi-inserts if there are any volatile default
		 * expressions in the table.  Similarly to the trigger case above,
		 * such expressions may query the table we're inserting into.
		 *
		 * 若表中有任何 volatile 默认表达式，则不能支持多行插入。与上面的触发器情况类似，这类表达式可能会查询正在插入的表。
		 *
		 * Note: It does not matter if any partitions have any volatile
		 * default expressions as we use the defaults from the target of the
		 * COPY command.
		 *
		 * 注意：分区是否有 volatile 默认表达式并不重要，因为我们使用的是 COPY 命令目标上的默认值。
		 */
		insertMethod = CIM_SINGLE;
	}
	else if (contain_volatile_functions(cstate->whereClause))
	{
		/*
		 * Can't support multi-inserts if there are any volatile function
		 * expressions in WHERE clause.  Similarly to the trigger case above,
		 * such expressions may query the table we're inserting into.
		 *
		 * 若 WHERE 子句中有任何 volatile 函数表达式，则不能支持多行插入。与上面的触发器情况类似，这类表达式可能会查询正在插入的表。
		 *
		 * Note: the whereClause was already preprocessed in DoCopy(), so it's
		 * okay to use contain_volatile_functions() directly.
		 *
		 * 注意：whereClause 已在 DoCopy() 中预处理过，因此可以直接使用 contain_volatile_functions()。
		 */
		insertMethod = CIM_SINGLE;
	}
	else
	{
		/*
		 * For partitioned tables, we may still be able to perform bulk
		 * inserts.  However, the possibility of this depends on which types
		 * of triggers exist on the partition.  We must disable bulk inserts
		 * if the partition is a foreign table that can't use batching or it
		 * has any before row insert or insert instead triggers (same as we
		 * checked above for the parent table).  Since the partition's
		 * resultRelInfos are initialized only when we actually need to insert
		 * the first tuple into them, we must have the intermediate insert
		 * method of CIM_MULTI_CONDITIONAL to flag that we must later
		 * determine if we can use bulk-inserts for the partition being
		 * inserted into.
		 *
		 * 对分区表，仍可能执行批量插入。但这取决于分区上存在哪类触发器。
		 * 若分区是不能批处理的外部表，或有任何 before row insert 或 insert instead 触发器（与上面检查父表时相同），则必须禁用批量插入。
		 * 分区的 resultRelInfo 要到真正需要插入第一行时才初始化，因此必须使用中间插入方法 CIM_MULTI_CONDITIONAL，
		 * 表示稍后必须再判断正在插入的分区能否使用批量插入。
		 */
		if (proute)
			insertMethod = CIM_MULTI_CONDITIONAL;
		else
			insertMethod = CIM_MULTI;

		CopyMultiInsertInfoInit(&multiInsertInfo, resultRelInfo, cstate,
								estate, mycid, ti_options);
	}

	/*
	 * If not using batch mode (which allocates slots as needed) set up a
	 * tuple slot too. When inserting into a partitioned table, we also need
	 * one, even if we might batch insert, to read the tuple in the root
	 * partition's form.
	 *
	 * 若不使用按需分配槽的批处理模式，也要设置一个元组槽。
	 * 插入分区表时，即使可能批量插入，也需要一个槽，以便以根分区的形式读取元组。
	 */
	if (insertMethod == CIM_SINGLE || insertMethod == CIM_MULTI_CONDITIONAL)
	{
		singleslot = table_slot_create(resultRelInfo->ri_RelationDesc,
									   &estate->es_tupleTable);
		bistate = GetBulkInsertState();
	}

	has_before_insert_row_trig = (resultRelInfo->ri_TrigDesc &&
								  resultRelInfo->ri_TrigDesc->trig_insert_before_row);

	has_instead_insert_row_trig = (resultRelInfo->ri_TrigDesc &&
								   resultRelInfo->ri_TrigDesc->trig_insert_instead_row);

	/*
	 * Check BEFORE STATEMENT insertion triggers. It's debatable whether we
	 * should do this for COPY, since it's not really an "INSERT" statement as
	 * such. However, executing these triggers maintains consistency with the
	 * EACH ROW triggers that we already fire on COPY.
	 *
	 * 检查 BEFORE STATEMENT 插入触发器。COPY 是否应该这样做有争议，因为它并不是真正的 INSERT 语句。
	 * 不过执行这些触发器可以与我们已经在 COPY 上触发的 EACH ROW 触发器保持一致。
	 */
	ExecBSInsertTriggers(estate, resultRelInfo);

	econtext = GetPerTupleExprContext(estate);

	/* Set up callback to identify error line number */
	/*
	 *
	 * 设置用于标识错误行号的回调
	 */
	errcallback.callback = CopyFromErrorCallback;
	errcallback.arg = cstate;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	for (;;)
	{
		TupleTableSlot *myslot;
		bool		skip_tuple;

		CHECK_FOR_INTERRUPTS();

		/*
		 * Reset the per-tuple exprcontext. We do this after every tuple, to
		 * clean-up after expression evaluations etc.
		 *
		 * 重置每元组表达式上下文。每处理完一个元组就做一次，以清理表达式计算等留下的内容。
		 */
		ResetPerTupleExprContext(estate);

		/* select slot to (initially) load row into */
		/*
		 *
		 * 选择（最初）用来装入行的槽
		 */
		if (insertMethod == CIM_SINGLE || proute)
		{
			myslot = singleslot;
			Assert(myslot != NULL);
		}
		else
		{
			Assert(resultRelInfo == target_resultRelInfo);
			Assert(insertMethod == CIM_MULTI);

			myslot = CopyMultiInsertInfoNextFreeSlot(&multiInsertInfo,
													 resultRelInfo);
		}

		/*
		 * Switch to per-tuple context before calling NextCopyFrom, which does
		 * evaluate default expressions etc. and requires per-tuple context.
		 *
		 * 调用 NextCopyFrom 之前切换到每元组上下文，因为它会计算默认表达式等，需要每元组上下文。
		 */
		MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));

		ExecClearTuple(myslot);

		/* Directly store the values/nulls array in the slot */
		/*
		 *
		 * 直接把 values/nulls 数组存入槽
		 */
		if (!NextCopyFrom(cstate, econtext, myslot->tts_values, myslot->tts_isnull))
			break;

		if (cstate->opts.on_error == COPY_ON_ERROR_IGNORE &&
			cstate->escontext->error_occurred)
		{
			/*
			 * Soft error occurred, skip this tuple and just make
			 * ErrorSaveContext ready for the next NextCopyFrom. Since we
			 * don't set details_wanted and error_data is not to be filled,
			 * just resetting error_occurred is enough.
			 *
			 * 发生了软错误，跳过该元组，并使 ErrorSaveContext 为下一次 NextCopyFrom 做好准备。
			 * 因为没有设置 details_wanted，也不需要填充 error_data，只重置 error_occurred 即可。
			 */
			cstate->escontext->error_occurred = false;

			/* Report that this tuple was skipped by the ON_ERROR clause */
			/*
			 *
			 * 报告该元组被 ON_ERROR 子句跳过
			 */
			pgstat_progress_update_param(PROGRESS_COPY_TUPLES_SKIPPED,
										 cstate->num_errors);

			if (cstate->opts.reject_limit > 0 &&
				cstate->num_errors > cstate->opts.reject_limit)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("skipped more than REJECT_LIMIT (%" PRId64 ") rows due to data type incompatibility",
								cstate->opts.reject_limit)));

			/* Repeat NextCopyFrom() until no soft error occurs */
			/*
			 *
			 * 重复调用 NextCopyFrom()，直到不再发生软错误
			 */
			continue;
		}

		ExecStoreVirtualTuple(myslot);

		/*
		 * Constraints and where clause might reference the tableoid column,
		 * so (re-)initialize tts_tableOid before evaluating them.
		 *
		 * 约束和 WHERE 子句可能会引用 tableoid 列，因此在计算它们之前（重新）初始化 tts_tableOid。
		 */
		myslot->tts_tableOid = RelationGetRelid(target_resultRelInfo->ri_RelationDesc);

		/* Triggers and stuff need to be invoked in query context. */
		/*
		 *
		 * 触发器等需要在查询上下文中调用。
		 */
		MemoryContextSwitchTo(oldcontext);

		if (cstate->whereClause)
		{
			econtext->ecxt_scantuple = myslot;
			/* Skip items that don't match COPY's WHERE clause */
			/*
			 *
			 * 跳过不匹配 COPY 的 WHERE 子句的项
			 */
			if (!ExecQual(cstate->qualexpr, econtext))
			{
				/*
				 * Report that this tuple was filtered out by the WHERE
				 * clause.
				 *
				 * 报告该元组被 WHERE 子句过滤掉。
				 */
				pgstat_progress_update_param(PROGRESS_COPY_TUPLES_EXCLUDED,
											 ++excluded);
				continue;
			}
		}

		/* Determine the partition to insert the tuple into */
		/*
		 *
		 * 确定要把元组插入哪个分区
		 */
		if (proute)
		{
			TupleConversionMap *map;

			/*
			 * Attempt to find a partition suitable for this tuple.
			 * ExecFindPartition() will raise an error if none can be found or
			 * if the found partition is not suitable for INSERTs.
			 *
			 * 尝试为该元组找到合适的分区。若找不到，或找到的分区不适合 INSERT，ExecFindPartition() 会报错。
			 */
			resultRelInfo = ExecFindPartition(mtstate, target_resultRelInfo,
											  proute, myslot, estate);

			if (prevResultRelInfo != resultRelInfo)
			{
				/* Determine which triggers exist on this partition */
				/*
				 *
				 * 确定该分区上存在哪些触发器
				 */
				has_before_insert_row_trig = (resultRelInfo->ri_TrigDesc &&
											  resultRelInfo->ri_TrigDesc->trig_insert_before_row);

				has_instead_insert_row_trig = (resultRelInfo->ri_TrigDesc &&
											   resultRelInfo->ri_TrigDesc->trig_insert_instead_row);

				/*
				 * Disable multi-inserts when the partition has BEFORE/INSTEAD
				 * OF triggers, or if the partition is a foreign table that
				 * can't use batching.
				 *
				 * 若分区有 BEFORE/INSTEAD OF 触发器，或分区是不能批处理的外部表，则禁用多行插入。
				 */
				leafpart_use_multi_insert = insertMethod == CIM_MULTI_CONDITIONAL &&
					!has_before_insert_row_trig &&
					!has_instead_insert_row_trig &&
					(resultRelInfo->ri_FdwRoutine == NULL ||
					 resultRelInfo->ri_BatchSize > 1);

				/* Set the multi-insert buffer to use for this partition. */
				/*
				 *
				 * 设置该分区使用的多行插入缓冲。
				 */
				if (leafpart_use_multi_insert)
				{
					if (resultRelInfo->ri_CopyMultiInsertBuffer == NULL)
						CopyMultiInsertInfoSetupBuffer(&multiInsertInfo,
													   resultRelInfo);
				}
				else if (insertMethod == CIM_MULTI_CONDITIONAL &&
						 !CopyMultiInsertInfoIsEmpty(&multiInsertInfo))
				{
					/*
					 * Flush pending inserts if this partition can't use
					 * batching, so rows are visible to triggers etc.
					 *
					 * 若该分区不能使用批处理，则刷出待处理的插入，以便触发器等能看到这些行。
					 */
					CopyMultiInsertInfoFlush(&multiInsertInfo,
											 resultRelInfo,
											 &processed);
				}

				if (bistate != NULL)
					ReleaseBulkInsertStatePin(bistate);
				prevResultRelInfo = resultRelInfo;
			}

			/*
			 * If we're capturing transition tuples, we might need to convert
			 * from the partition rowtype to root rowtype. But if there are no
			 * BEFORE triggers on the partition that could change the tuple,
			 * we can just remember the original unconverted tuple to avoid a
			 * needless round trip conversion.
			 *
			 * 若正在捕获 transition 元组，可能需要把分区行类型转换回根表行类型。
			 * 但若分区上没有可能改变元组的 BEFORE 触发器，就可以记住原来未转换的元组，避免一次无谓的往返转换。
			 */
			if (cstate->transition_capture != NULL)
				cstate->transition_capture->tcs_original_insert_tuple =
					!has_before_insert_row_trig ? myslot : NULL;

			/*
			 * We might need to convert from the root rowtype to the partition
			 * rowtype.
			 *
			 * 可能需要把根表行类型转换为分区行类型。
			 */
			map = ExecGetRootToChildMap(resultRelInfo, estate);
			if (insertMethod == CIM_SINGLE || !leafpart_use_multi_insert)
			{
				/* non batch insert */
				/*
				 *
				 * 非批量插入
				 */
				if (map != NULL)
				{
					TupleTableSlot *new_slot;

					new_slot = resultRelInfo->ri_PartitionTupleSlot;
					myslot = execute_attr_map_slot(map->attrMap, myslot, new_slot);
				}
			}
			else
			{
				/*
				 * Prepare to queue up tuple for later batch insert into
				 * current partition.
				 *
				 * 准备把元组排队，稍后批量插入当前分区。
				 */
				TupleTableSlot *batchslot;

				/* no other path available for partitioned table */
				/*
				 *
				 * 分区表没有其他可用路径
				 */
				Assert(insertMethod == CIM_MULTI_CONDITIONAL);

				batchslot = CopyMultiInsertInfoNextFreeSlot(&multiInsertInfo,
															resultRelInfo);

				if (map != NULL)
					myslot = execute_attr_map_slot(map->attrMap, myslot,
												   batchslot);
				else
				{
					/*
					 * This looks more expensive than it is (Believe me, I
					 * optimized it away. Twice.). The input is in virtual
					 * form, and we'll materialize the slot below - for most
					 * slot types the copy performs the work materialization
					 * would later require anyway.
					 *
					 * 这看起来比实际更贵（相信我，我优化掉过两次）。输入是 virtual 形式，下面会物化该槽。
					 * 对大多数槽类型，这次拷贝本来就会完成稍后物化所需的工作。
					 */
					ExecCopySlot(batchslot, myslot);
					myslot = batchslot;
				}
			}

			/* ensure that triggers etc see the right relation  */
			/*
			 *
			 * 确保触发器等看到的是正确的关系
			 */
			myslot->tts_tableOid = RelationGetRelid(resultRelInfo->ri_RelationDesc);
		}

		skip_tuple = false;

		/* BEFORE ROW INSERT Triggers */
		/*
		 *
		 * BEFORE ROW INSERT 触发器
		 */
		if (has_before_insert_row_trig)
		{
			if (!ExecBRInsertTriggers(estate, resultRelInfo, myslot))
				skip_tuple = true;	/* "do nothing" */
				/*
				 *
				 * 什么也不做
				 */
		}

		if (!skip_tuple)
		{
			/*
			 * If there is an INSTEAD OF INSERT ROW trigger, let it handle the
			 * tuple.  Otherwise, proceed with inserting the tuple into the
			 * table or foreign table.
			 *
			 * 若存在 INSTEAD OF INSERT ROW 触发器，则由它处理该元组。否则继续把元组插入表或外部表。
			 */
			if (has_instead_insert_row_trig)
			{
				ExecIRInsertTriggers(estate, resultRelInfo, myslot);
			}
			else
			{
				/* Compute stored generated columns */
				/*
				 *
				 * 计算存储生成列
				 */
				if (resultRelInfo->ri_RelationDesc->rd_att->constr &&
					resultRelInfo->ri_RelationDesc->rd_att->constr->has_generated_stored)
					ExecComputeStoredGenerated(resultRelInfo, estate, myslot,
											   CMD_INSERT);

				/*
				 * If the target is a plain table, check the constraints of
				 * the tuple.
				 *
				 * 若目标是普通表，则检查元组的约束。
				 */
				if (resultRelInfo->ri_FdwRoutine == NULL &&
					resultRelInfo->ri_RelationDesc->rd_att->constr)
					ExecConstraints(resultRelInfo, myslot, estate);

				/*
				 * Also check the tuple against the partition constraint, if
				 * there is one; except that if we got here via tuple-routing,
				 * we don't need to if there's no BR trigger defined on the
				 * partition.
				 *
				 * 同时对照分区约束检查元组（若有约束）；
				 * 但若是经由元组路由到达这里，且分区上没有定义 BR 触发器，则不必检查。
				 */
				if (resultRelInfo->ri_RelationDesc->rd_rel->relispartition &&
					(proute == NULL || has_before_insert_row_trig))
					ExecPartitionCheck(resultRelInfo, myslot, estate, true);

				/* Store the slot in the multi-insert buffer, when enabled. */
				/*
				 *
				 * 在启用时，把槽存入多行插入缓冲。
				 */
				if (insertMethod == CIM_MULTI || leafpart_use_multi_insert)
				{
					/*
					 * The slot previously might point into the per-tuple
					 * context. For batching it needs to be longer lived.
					 *
					 * 该槽先前可能指向每元组上下文。批处理时它需要活得更久。
					 */
					ExecMaterializeSlot(myslot);

					/* Add this tuple to the tuple buffer */
					/*
					 *
					 * 把该元组加入元组缓冲
					 */
					CopyMultiInsertInfoStore(&multiInsertInfo,
											 resultRelInfo, myslot,
											 cstate->line_buf.len,
											 cstate->cur_lineno);

					/*
					 * If enough inserts have queued up, then flush all
					 * buffers out to their tables.
					 *
					 * 若排队的插入已经足够多，则把所有缓冲区刷到各自的表。
					 */
					if (CopyMultiInsertInfoIsFull(&multiInsertInfo))
						CopyMultiInsertInfoFlush(&multiInsertInfo,
												 resultRelInfo,
												 &processed);

					/*
					 * We delay updating the row counter and progress of the
					 * COPY command until after writing the tuples stored in
					 * the buffer out to the table, as in single insert mode.
					 * See CopyMultiInsertBufferFlush().
					 *
					 * 我们推迟更新行计数和 COPY 命令进度，直到把缓冲中的元组写到表中之后，与单行插入模式一样。
					 * 参见 CopyMultiInsertBufferFlush()。
					 */
					continue;	/* next tuple please */
					/*
					 *
					 * 请给出下一个元组
					 */
				}
				else
				{
					List	   *recheckIndexes = NIL;

					/* OK, store the tuple */
					/*
					 *
					 * 可以，存储该元组
					 */
					if (resultRelInfo->ri_FdwRoutine != NULL)
					{
						myslot = resultRelInfo->ri_FdwRoutine->ExecForeignInsert(estate,
																				 resultRelInfo,
																				 myslot,
																				 NULL);

						if (myslot == NULL) /* "do nothing" */
						/*
						 *
						 * 什么也不做
						 */
							continue;	/* next tuple please */
							/*
							 *
							 * 请给出下一个元组
							 */

						/*
						 * AFTER ROW Triggers might reference the tableoid
						 * column, so (re-)initialize tts_tableOid before
						 * evaluating them.
						 *
						 * AFTER ROW 触发器可能会引用 tableoid 列，因此在计算它们之前（重新）初始化 tts_tableOid。
						 */
						myslot->tts_tableOid = RelationGetRelid(resultRelInfo->ri_RelationDesc);
					}
					else
					{
						/* OK, store the tuple and create index entries for it */
						/*
						 *
						 * 可以，存储该元组并为它创建索引项
						 */
						table_tuple_insert(resultRelInfo->ri_RelationDesc,
										   myslot, mycid, ti_options, bistate);

						if (resultRelInfo->ri_NumIndices > 0)
							recheckIndexes = ExecInsertIndexTuples(resultRelInfo,
																   myslot,
																   estate,
																   false,
																   false,
																   NULL,
																   NIL,
																   false);
					}

					/* AFTER ROW INSERT Triggers */
					/*
					 *
					 * AFTER ROW INSERT 触发器
					 */
					ExecARInsertTriggers(estate, resultRelInfo, myslot,
										 recheckIndexes, cstate->transition_capture);

					list_free(recheckIndexes);
				}
			}

			/*
			 * We count only tuples not suppressed by a BEFORE INSERT trigger
			 * or FDW; this is the same definition used by nodeModifyTable.c
			 * for counting tuples inserted by an INSERT command.  Update
			 * progress of the COPY command as well.
			 *
			 * 只统计未被 BEFORE INSERT 触发器或 FDW 抑制的元组；
			 * 这与 nodeModifyTable.c 统计 INSERT 命令插入元组的定义相同。同时更新 COPY 命令的进度。
			 */
			pgstat_progress_update_param(PROGRESS_COPY_TUPLES_PROCESSED,
										 ++processed);
		}
	}

	/* Flush any remaining buffered tuples */
	/*
	 *
	 * 刷出所有剩余的缓冲元组
	 */
	if (insertMethod != CIM_SINGLE)
	{
		if (!CopyMultiInsertInfoIsEmpty(&multiInsertInfo))
			CopyMultiInsertInfoFlush(&multiInsertInfo, NULL, &processed);
	}

	/* Done, clean up */
	/*
	 *
	 * 完成，进行清理
	 */
	error_context_stack = errcallback.previous;

	if (cstate->opts.on_error != COPY_ON_ERROR_STOP &&
		cstate->num_errors > 0 &&
		cstate->opts.log_verbosity >= COPY_LOG_VERBOSITY_DEFAULT)
		ereport(NOTICE,
				errmsg_plural("%" PRIu64 " row was skipped due to data type incompatibility",
							  "%" PRIu64 " rows were skipped due to data type incompatibility",
							  cstate->num_errors,
							  cstate->num_errors));

	if (bistate != NULL)
		FreeBulkInsertState(bistate);

	MemoryContextSwitchTo(oldcontext);

	/* Execute AFTER STATEMENT insertion triggers */
	/*
	 *
	 * 执行 AFTER STATEMENT 插入触发器
	 */
	ExecASInsertTriggers(estate, target_resultRelInfo, cstate->transition_capture);

	/* Handle queued AFTER triggers */
	/*
	 *
	 * 处理排队的 AFTER 触发器
	 */
	AfterTriggerEndQuery(estate);

	ExecResetTupleTable(estate->es_tupleTable, false);

	/* Allow the FDW to shut down */
	/*
	 *
	 * 允许 FDW 关闭
	 */
	if (target_resultRelInfo->ri_FdwRoutine != NULL &&
		target_resultRelInfo->ri_FdwRoutine->EndForeignInsert != NULL)
		target_resultRelInfo->ri_FdwRoutine->EndForeignInsert(estate,
															  target_resultRelInfo);

	/* Tear down the multi-insert buffer data */
	/*
	 *
	 * 拆除多行插入缓冲数据
	 */
	if (insertMethod != CIM_SINGLE)
		CopyMultiInsertInfoCleanup(&multiInsertInfo);

	/* Close all the partitioned tables, leaf partitions, and their indices */
	/*
	 *
	 * 关闭所有分区表、叶分区及其索引
	 */
	if (proute)
		ExecCleanupTupleRouting(mtstate, proute);

	/* Close the result relations, including any trigger target relations */
	/*
	 *
	 * 关闭结果关系，包括任何触发器目标关系
	 */
	ExecCloseResultRelations(estate);
	ExecCloseRangeTableRelations(estate);

	FreeExecutorState(estate);

	return processed;
}

/*
 * Setup to read tuples from a file for COPY FROM.
 *
 * 准备为 COPY FROM 从文件读取元组。
 *
 * 'rel': Used as a template for the tuples
 * 'whereClause': WHERE clause from the COPY FROM command
 * 'filename': Name of server-local file to read, NULL for STDIN
 * 'is_program': true if 'filename' is program to execute
 * 'data_source_cb': callback that provides the input data
 * 'attnamelist': List of char *, columns to include. NIL selects all cols.
 * 'options': List of DefElem. See copy_opt_item in gram.y for selections.
 *
 * rel：用作元组的模板
 * whereClause：COPY FROM 命令中的 WHERE 子句
 * filename：要读取的服务器本地文件名，STDIN 时为 NULL
 * is_program：若 filename 是要执行的程序则为 true
 * data_source_cb：提供输入数据的回调
 * attnamelist：要包含的列名（char *）列表。NIL 表示选择全部列。
 * options：DefElem 列表。可选项见 gram.y 中的 copy_opt_item。
 *
 * Returns a CopyFromState, to be passed to NextCopyFrom and related functions.
 *
 * 返回 CopyFromState，供 NextCopyFrom 及相关函数使用。
 */
CopyFromState
BeginCopyFrom(ParseState *pstate,
			  Relation rel,
			  Node *whereClause,
			  const char *filename,
			  bool is_program,
			  copy_data_source_cb data_source_cb,
			  List *attnamelist,
			  List *options)
{
	CopyFromState cstate;
	bool		pipe = (filename == NULL);
	TupleDesc	tupDesc;
	AttrNumber	num_phys_attrs,
				num_defaults;
	FmgrInfo   *in_functions;
	Oid		   *typioparams;
	int		   *defmap;
	ExprState **defexprs;
	MemoryContext oldcontext;
	bool		volatile_defexprs;
	const int	progress_cols[] = {
		PROGRESS_COPY_COMMAND,
		PROGRESS_COPY_TYPE,
		PROGRESS_COPY_BYTES_TOTAL
	};
	int64		progress_vals[] = {
		PROGRESS_COPY_COMMAND_FROM,
		0,
		0
	};

	/* Allocate workspace and zero all fields */
	/*
	 *
	 * 分配工作区并把所有字段清零
	 */
	cstate = (CopyFromStateData *) palloc0(sizeof(CopyFromStateData));

	/*
	 * We allocate everything used by a cstate in a new memory context. This
	 * avoids memory leaks during repeated use of COPY in a query.
	 *
	 * 在新的内存上下文中分配 cstate 使用的全部内容。这样可以避免在查询中反复使用 COPY 时泄漏内存。
	 */
	cstate->copycontext = AllocSetContextCreate(CurrentMemoryContext,
												"COPY",
												ALLOCSET_DEFAULT_SIZES);

	oldcontext = MemoryContextSwitchTo(cstate->copycontext);

	/* Extract options from the statement node tree */
	/*
	 *
	 * 从语句节点树中提取选项
	 */
	ProcessCopyOptions(pstate, &cstate->opts, true /* is_from */ , options);
	/*
	 *
	 * 方向为 COPY FROM
	 */

	/* Set the format routine */
	/*
	 *
	 * 设置格式例程
	 */
	cstate->routine = CopyFromGetRoutine(&cstate->opts);

	/* Process the target relation */
	/*
	 *
	 * 处理目标关系
	 */
	cstate->rel = rel;

	tupDesc = RelationGetDescr(cstate->rel);

	/* process common options or initialization */
	/*
	 *
	 * 处理公共选项或做初始化
	 */

	/* Generate or convert list of attributes to process */
	/*
	 *
	 * 生成或转换要处理的属性列表
	 */
	cstate->attnumlist = CopyGetAttnums(tupDesc, cstate->rel, attnamelist);

	num_phys_attrs = tupDesc->natts;

	/* Convert FORCE_NOT_NULL name list to per-column flags, check validity */
	/*
	 *
	 * 把 FORCE_NOT_NULL 名字列表转换成按列标志，并检查有效性
	 */
	cstate->opts.force_notnull_flags = (bool *) palloc0(num_phys_attrs * sizeof(bool));
	if (cstate->opts.force_notnull_all)
		MemSet(cstate->opts.force_notnull_flags, true, num_phys_attrs * sizeof(bool));
	else if (cstate->opts.force_notnull)
	{
		List	   *attnums;
		ListCell   *cur;

		attnums = CopyGetAttnums(tupDesc, cstate->rel, cstate->opts.force_notnull);

		foreach(cur, attnums)
		{
			int			attnum = lfirst_int(cur);
			Form_pg_attribute attr = TupleDescAttr(tupDesc, attnum - 1);

			if (!list_member_int(cstate->attnumlist, attnum))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				/*- translator: first %s is the name of a COPY option, e.g. FORCE_NOT_NULL */
				/*
				 *
				 * 翻译提示：第一个 %s 是 COPY 选项名，例如 FORCE_NOT_NULL
				 */
						 errmsg("%s column \"%s\" not referenced by COPY",
								"FORCE_NOT_NULL", NameStr(attr->attname))));
			cstate->opts.force_notnull_flags[attnum - 1] = true;
		}
	}

	/* Set up soft error handler for ON_ERROR */
	/*
	 *
	 * 为 ON_ERROR 设置软错误处理器
	 */
	if (cstate->opts.on_error != COPY_ON_ERROR_STOP)
	{
		cstate->escontext = makeNode(ErrorSaveContext);
		cstate->escontext->type = T_ErrorSaveContext;
		cstate->escontext->error_occurred = false;

		/*
		 * Currently we only support COPY_ON_ERROR_IGNORE. We'll add other
		 * options later
		 *
		 * 目前只支持 COPY_ON_ERROR_IGNORE。以后会增加其他选项
		 */
		if (cstate->opts.on_error == COPY_ON_ERROR_IGNORE)
			cstate->escontext->details_wanted = false;
	}
	else
		cstate->escontext = NULL;

	/* Convert FORCE_NULL name list to per-column flags, check validity */
	/*
	 *
	 * 把 FORCE_NULL 名字列表转换成按列标志，并检查有效性
	 */
	cstate->opts.force_null_flags = (bool *) palloc0(num_phys_attrs * sizeof(bool));
	if (cstate->opts.force_null_all)
		MemSet(cstate->opts.force_null_flags, true, num_phys_attrs * sizeof(bool));
	else if (cstate->opts.force_null)
	{
		List	   *attnums;
		ListCell   *cur;

		attnums = CopyGetAttnums(tupDesc, cstate->rel, cstate->opts.force_null);

		foreach(cur, attnums)
		{
			int			attnum = lfirst_int(cur);
			Form_pg_attribute attr = TupleDescAttr(tupDesc, attnum - 1);

			if (!list_member_int(cstate->attnumlist, attnum))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				/*- translator: first %s is the name of a COPY option, e.g. FORCE_NOT_NULL */
				/*
				 *
				 * 翻译提示：第一个 %s 是 COPY 选项名，例如 FORCE_NOT_NULL
				 */
						 errmsg("%s column \"%s\" not referenced by COPY",
								"FORCE_NULL", NameStr(attr->attname))));
			cstate->opts.force_null_flags[attnum - 1] = true;
		}
	}

	/* Convert convert_selectively name list to per-column flags */
	/*
	 *
	 * 把 convert_selectively 的名字列表转换成按列的标志
	 */
	if (cstate->opts.convert_selectively)
	{
		List	   *attnums;
		ListCell   *cur;

		cstate->convert_select_flags = (bool *) palloc0(num_phys_attrs * sizeof(bool));

		attnums = CopyGetAttnums(tupDesc, cstate->rel, cstate->opts.convert_select);

		foreach(cur, attnums)
		{
			int			attnum = lfirst_int(cur);
			Form_pg_attribute attr = TupleDescAttr(tupDesc, attnum - 1);

			if (!list_member_int(cstate->attnumlist, attnum))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
						 errmsg_internal("selected column \"%s\" not referenced by COPY",
										 NameStr(attr->attname))));
			cstate->convert_select_flags[attnum - 1] = true;
		}
	}

	/* Use client encoding when ENCODING option is not specified. */
	/*
	 *
	 * 未指定 ENCODING 选项时使用客户端编码。
	 */
	if (cstate->opts.file_encoding < 0)
		cstate->file_encoding = pg_get_client_encoding();
	else
		cstate->file_encoding = cstate->opts.file_encoding;

	/*
	 * Look up encoding conversion function.
	 *
	 * 查找编码转换函数。
	 */
	if (cstate->file_encoding == GetDatabaseEncoding() ||
		cstate->file_encoding == PG_SQL_ASCII ||
		GetDatabaseEncoding() == PG_SQL_ASCII)
	{
		cstate->need_transcoding = false;
	}
	else
	{
		cstate->need_transcoding = true;
		cstate->conversion_proc = FindDefaultConversionProc(cstate->file_encoding,
															GetDatabaseEncoding());
		if (!OidIsValid(cstate->conversion_proc))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("default conversion function for encoding \"%s\" to \"%s\" does not exist",
							pg_encoding_to_char(cstate->file_encoding),
							pg_encoding_to_char(GetDatabaseEncoding()))));
	}

	cstate->copy_src = COPY_FILE;	/* default */
	/*
	 *
	 * 默认
	 */

	cstate->whereClause = whereClause;

	/* Initialize state variables */
	/*
	 *
	 * 初始化状态变量
	 */
	cstate->eol_type = EOL_UNKNOWN;
	cstate->cur_relname = RelationGetRelationName(cstate->rel);
	cstate->cur_lineno = 0;
	cstate->cur_attname = NULL;
	cstate->cur_attval = NULL;
	cstate->relname_only = false;

	/*
	 * Allocate buffers for the input pipeline.
	 *
	 * 为输入流水线分配缓冲区。
	 *
	 * attribute_buf and raw_buf are used in both text and binary modes, but
	 * input_buf and line_buf only in text mode.
	 *
	 * attribute_buf 与 raw_buf 在 text 和 binary 模式下都会用到，input_buf 与 line_buf 只在 text 模式下使用。
	 */
	cstate->raw_buf = palloc(RAW_BUF_SIZE + 1);
	cstate->raw_buf_index = cstate->raw_buf_len = 0;
	cstate->raw_reached_eof = false;

	initStringInfo(&cstate->attribute_buf);

	/* Assign range table and rteperminfos, we'll need them in CopyFrom. */
	/*
	 *
	 * 设置 range table 与 rteperminfos，CopyFrom 中会用到它们。
	 */
	if (pstate)
	{
		cstate->range_table = pstate->p_rtable;
		cstate->rteperminfos = pstate->p_rteperminfos;
	}

	num_defaults = 0;
	volatile_defexprs = false;

	/*
	 * Pick up the required catalog information for each attribute in the
	 * relation, including the input function, the element type (to pass to
	 * the input function), and info about defaults and constraints. (Which
	 * input function we use depends on text/binary format choice.)
	 *
	 * 为关系的每个属性收集所需的目录信息，包括输入函数、传给输入函数的元素类型，以及默认值与约束信息。
	 * 使用哪个输入函数取决于选择的是 text 还是 binary 格式。
	 */
	in_functions = (FmgrInfo *) palloc(num_phys_attrs * sizeof(FmgrInfo));
	typioparams = (Oid *) palloc(num_phys_attrs * sizeof(Oid));
	defmap = (int *) palloc(num_phys_attrs * sizeof(int));
	defexprs = (ExprState **) palloc(num_phys_attrs * sizeof(ExprState *));

	for (int attnum = 1; attnum <= num_phys_attrs; attnum++)
	{
		Form_pg_attribute att = TupleDescAttr(tupDesc, attnum - 1);

		/* We don't need info for dropped attributes */
		/*
		 *
		 * 已删除的属性不需要这些信息
		 */
		if (att->attisdropped)
			continue;

		/* Fetch the input function and typioparam info */
		/*
		 *
		 * 取得输入函数与 typioparam 信息
		 */
		cstate->routine->CopyFromInFunc(cstate, att->atttypid,
										&in_functions[attnum - 1],
										&typioparams[attnum - 1]);

		/* Get default info if available */
		/*
		 *
		 * 若有默认值信息则取得它
		 */
		defexprs[attnum - 1] = NULL;

		/*
		 * We only need the default values for columns that do not appear in
		 * the column list, unless the DEFAULT option was given. We never need
		 * default values for generated columns.
		 *
		 * 除非给出了 DEFAULT 选项，否则只需要未出现在列清单中的列的默认值。生成列永远不需要默认值。
		 */
		if ((cstate->opts.default_print != NULL ||
			 !list_member_int(cstate->attnumlist, attnum)) &&
			!att->attgenerated)
		{
			Expr	   *defexpr = (Expr *) build_column_default(cstate->rel,
																attnum);

			if (defexpr != NULL)
			{
				/* Run the expression through planner */
				/*
				 *
				 * 让表达式经过规划器
				 */
				defexpr = expression_planner(defexpr);

				/* Initialize executable expression in copycontext */
				/*
				 *
				 * 在 copycontext 中初始化可执行表达式
				 */
				defexprs[attnum - 1] = ExecInitExpr(defexpr, NULL);

				/* if NOT copied from input */
				/*
				 *
				 * 若不是从输入拷贝来的
				 */
				/* use default value if one exists */
				/*
				 *
				 * 若存在默认值则使用默认值
				 */
				if (!list_member_int(cstate->attnumlist, attnum))
				{
					defmap[num_defaults] = attnum - 1;
					num_defaults++;
				}

				/*
				 * If a default expression looks at the table being loaded,
				 * then it could give the wrong answer when using
				 * multi-insert. Since database access can be dynamic this is
				 * hard to test for exactly, so we use the much wider test of
				 * whether the default expression is volatile. We allow for
				 * the special case of when the default expression is the
				 * nextval() of a sequence which in this specific case is
				 * known to be safe for use with the multi-insert
				 * optimization. Hence we use this special case function
				 * checker rather than the standard check for
				 * contain_volatile_functions().  Note also that we already
				 * ran the expression through expression_planner().
				 *
				 * 若默认表达式会查看正在装载的表，则在使用多行插入时可能给出错误结果。
				 * 数据库访问可以是动态的，很难精确检测，因此采用更宽的测试：默认表达式是否为 volatile。
				 * 特例是默认表达式为序列的 nextval()，在这种情况下已知可以安全地使用多行插入优化。
				 * 因此使用这个专用检查函数，而不是标准的 contain_volatile_functions()。
				 * 另外注意表达式已经过 expression_planner()。
				 */
				if (!volatile_defexprs)
					volatile_defexprs = contain_volatile_functions_not_nextval((Node *) defexpr);
			}
		}
	}

	cstate->defaults = (bool *) palloc0(tupDesc->natts * sizeof(bool));

	/* initialize progress */
	/*
	 *
	 * 初始化进度报告
	 */
	pgstat_progress_start_command(PROGRESS_COMMAND_COPY,
								  cstate->rel ? RelationGetRelid(cstate->rel) : InvalidOid);
	cstate->bytes_processed = 0;

	/* We keep those variables in cstate. */
	/*
	 *
	 * 这些变量保存在 cstate 中。
	 */
	cstate->in_functions = in_functions;
	cstate->typioparams = typioparams;
	cstate->defmap = defmap;
	cstate->defexprs = defexprs;
	cstate->volatile_defexprs = volatile_defexprs;
	cstate->num_defaults = num_defaults;
	cstate->is_program = is_program;

	if (data_source_cb)
	{
		progress_vals[1] = PROGRESS_COPY_TYPE_CALLBACK;
		cstate->copy_src = COPY_CALLBACK;
		cstate->data_source_cb = data_source_cb;
	}
	else if (pipe)
	{
		progress_vals[1] = PROGRESS_COPY_TYPE_PIPE;
		Assert(!is_program);	/* the grammar does not allow this */
		/*
		 *
		 * 语法不允许这样做
		 */
		if (whereToSendOutput == DestRemote)
			ReceiveCopyBegin(cstate);
		else
			cstate->copy_file = stdin;
	}
	else
	{
		cstate->filename = pstrdup(filename);

		if (cstate->is_program)
		{
			progress_vals[1] = PROGRESS_COPY_TYPE_PROGRAM;
			cstate->copy_file = OpenPipeStream(cstate->filename, PG_BINARY_R);
			if (cstate->copy_file == NULL)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not execute command \"%s\": %m",
								cstate->filename)));
		}
		else
		{
			struct stat st;

			progress_vals[1] = PROGRESS_COPY_TYPE_FILE;
			cstate->copy_file = AllocateFile(cstate->filename, PG_BINARY_R);
			if (cstate->copy_file == NULL)
			{
				/* copy errno because ereport subfunctions might change it */
				/*
				 *
				 * 保存 errno，因为 ereport 的子函数可能会改掉它
				 */
				int			save_errno = errno;

				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not open file \"%s\" for reading: %m",
								cstate->filename),
						 (save_errno == ENOENT || save_errno == EACCES) ?
						 errhint("COPY FROM instructs the PostgreSQL server process to read a file. "
								 "You may want a client-side facility such as psql's \\copy.") : 0));
			}

			if (fstat(fileno(cstate->copy_file), &st))
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not stat file \"%s\": %m",
								cstate->filename)));

			if (S_ISDIR(st.st_mode))
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("\"%s\" is a directory", cstate->filename)));

			progress_vals[2] = st.st_size;
		}
	}

	pgstat_progress_update_multi_param(3, progress_cols, progress_vals);

	cstate->routine->CopyFromStart(cstate, tupDesc);

	MemoryContextSwitchTo(oldcontext);

	return cstate;
}

/*
 * Clean up storage and release resources for COPY FROM.
 *
 * 清理存储并释放 COPY FROM 的资源。
 */
void
EndCopyFrom(CopyFromState cstate)
{
	/* Invoke the end callback */
	/*
	 *
	 * 调用结束回调
	 */
	cstate->routine->CopyFromEnd(cstate);

	/* No COPY FROM related resources except memory. */
	/*
	 *
	 * 除内存外没有与 COPY FROM 相关的资源。
	 */
	if (cstate->is_program)
	{
		ClosePipeFromProgram(cstate);
	}
	else
	{
		if (cstate->filename != NULL && FreeFile(cstate->copy_file))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not close file \"%s\": %m",
							cstate->filename)));
	}

	pgstat_progress_end_command();

	MemoryContextDelete(cstate->copycontext);
	pfree(cstate);
}

/*
 * Closes the pipe from an external program, checking the pclose() return code.
 *
 * 关闭来自外部程序的管道，并检查 pclose() 的返回码。
 */
static void
ClosePipeFromProgram(CopyFromState cstate)
{
	int			pclose_rc;

	Assert(cstate->is_program);

	pclose_rc = ClosePipeStream(cstate->copy_file);
	if (pclose_rc == -1)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close pipe to external command: %m")));
	else if (pclose_rc != 0)
	{
		/*
		 * If we ended a COPY FROM PROGRAM before reaching EOF, then it's
		 * expectable for the called program to fail with SIGPIPE, and we
		 * should not report that as an error.  Otherwise, SIGPIPE indicates a
		 * problem.
		 *
		 * 若在到达 EOF 之前结束了 COPY FROM PROGRAM，则被调用程序因 SIGPIPE 失败是可预期的，不应报成错误。
		 * 否则 SIGPIPE 表示出现了问题。
		 */
		if (!cstate->raw_reached_eof &&
			wait_result_is_signal(pclose_rc, SIGPIPE))
			return;

		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("program \"%s\" failed",
						cstate->filename),
				 errdetail_internal("%s", wait_result_to_str(pclose_rc))));
	}
}

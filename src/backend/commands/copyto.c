/*-------------------------------------------------------------------------
 *
 * copyto.c
 *		COPY <table> TO file/program/client
 *
 * 把表或查询结果送到文件、程序或客户端。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/copyto.c
 *
 * 标识
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>

#include "access/tableam.h"
#include "commands/copyapi.h"
#include "commands/progress.h"
#include "executor/execdesc.h"
#include "executor/executor.h"
#include "executor/tuptable.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/fd.h"
#include "tcop/tcopprot.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

/*
 * Represents the different dest cases we need to worry about at
 * the bottom level
 *
 * 表示底层需要区分的不同目标情况
 */
typedef enum CopyDest
{
	COPY_FILE,					/* to file (or a piped program) */
	/*
	 *
	 * 发往文件（或管道程序）
	 */
	COPY_FRONTEND,				/* to frontend */
	/*
	 *
	 * 发往前端
	 */
	COPY_CALLBACK,				/* to callback function */
	/*
	 *
	 * 发往回调函数
	 */
} CopyDest;

/*
 * This struct contains all the state variables used throughout a COPY TO
 * operation.
 *
 * 该结构包含整个 COPY TO 操作中使用的全部状态变量。
 *
 * Multi-byte encodings: all supported client-side encodings encode multi-byte
 * characters by having the first byte's high bit set. Subsequent bytes of the
 * character can have the high bit not set. When scanning data in such an
 * encoding to look for a match to a single-byte (ie ASCII) character, we must
 * use the full pg_encoding_mblen() machinery to skip over multibyte
 * characters, else we might find a false match to a trailing byte. In
 * supported server encodings, there is no possibility of a false match, and
 * it's faster to make useless comparisons to trailing bytes than it is to
 * invoke pg_encoding_mblen() to skip over them. encoding_embeds_ascii is true
 * when we have to do it the hard way.
 *
 * 多字节编码：所有支持的客户端编码都通过设置首字节的高位来编码多字节字符。该字符的后续字节可以不设置高位。
 * 用这种编码扫描数据以匹配单字节（即 ASCII）字符时，必须用完整的 pg_encoding_mblen() 机制跳过多字节字符，否则可能把尾字节误当成匹配。
 * 在支持的服务器编码中不可能误匹配，对尾字节做无用比较也比调用 pg_encoding_mblen() 跳过它们更快。
 * 必须用较麻烦的方式处理时，encoding_embeds_ascii 为 true。
 */
typedef struct CopyToStateData
{
	/* format-specific routines */
	/*
	 *
	 * 格式专用例程
	 */
	const CopyToRoutine *routine;

	/* low-level state data */
	/*
	 *
	 * 底层状态数据
	 */
	CopyDest	copy_dest;		/* type of copy source/destination */
	/*
	 *
	 * 拷贝源/目标的类型
	 */
	FILE	   *copy_file;		/* used if copy_dest == COPY_FILE */
	/*
	 *
	 * 当 copy_dest == COPY_FILE 时使用
	 */
	StringInfo	fe_msgbuf;		/* used for all dests during COPY TO */
	/*
	 *
	 * COPY TO 期间所有目标都使用
	 */

	int			file_encoding;	/* file or remote side's character encoding */
	/*
	 *
	 * 文件或远端的字符编码
	 */
	bool		need_transcoding;	/* file encoding diff from server? */
	/*
	 *
	 * 文件编码是否与服务器不同？
	 */
	bool		encoding_embeds_ascii;	/* ASCII can be non-first byte? */
	/*
	 *
	 * ASCII 能否出现在非首字节？
	 */

	/* parameters from the COPY command */
	/*
	 *
	 * 来自 COPY 命令的参数
	 */
	Relation	rel;			/* relation to copy to */
	/*
	 *
	 * 要拷贝到的关系
	 */
	QueryDesc  *queryDesc;		/* executable query to copy from */
	/*
	 *
	 * 要作为拷贝来源的可执行查询
	 */
	List	   *attnumlist;		/* integer list of attnums to copy */
	/*
	 *
	 * 要拷贝的 attnum 整数列表
	 */
	char	   *filename;		/* filename, or NULL for STDOUT */
	/*
	 *
	 * 文件名，或对 STDOUT 为 NULL
	 */
	bool		is_program;		/* is 'filename' a program to popen? */
	/*
	 *
	 * filename 是否为要 popen 的程序？
	 */
	copy_data_dest_cb data_dest_cb; /* function for writing data */
	/*
	 *
	 * 用于写数据的函数
	 */

	CopyFormatOptions opts;
	Node	   *whereClause;	/* WHERE condition (or NULL) */
	/*
	 *
	 * WHERE 条件（或 NULL）
	 */

	/*
	 * Working state
	 *
	 * 工作状态
	 */
	MemoryContext copycontext;	/* per-copy execution context */
	/*
	 *
	 * 每次拷贝的执行上下文
	 */

	FmgrInfo   *out_functions;	/* lookup info for output functions */
	/*
	 *
	 * 输出函数的查找信息
	 */
	MemoryContext rowcontext;	/* per-row evaluation context */
	/*
	 *
	 * 每行求值上下文
	 */
	uint64		bytes_processed;	/* number of bytes processed so far */
	/*
	 *
	 * 迄今已处理的字节数
	 */
} CopyToStateData;

/* DestReceiver for COPY (query) TO */
/*
 *
 * 用于 COPY (query) TO 的 DestReceiver
 */
typedef struct
{
	DestReceiver pub;			/* publicly-known function pointers */
	/*
	 *
	 * 对外公开的函数指针
	 */
	CopyToState cstate;			/* CopyToStateData for the command */
	/*
	 *
	 * 该命令的 CopyToStateData
	 */
	uint64		processed;		/* # of tuples processed */
	/*
	 *
	 * 已处理的元组数
	 */
} DR_copy;

/* NOTE: there's a copy of this in copyfromparse.c */
/*
 *
 * 注意：copyfromparse.c 中有一份相同代码
 */
static const char BinarySignature[11] = "PGCOPY\n\377\r\n\0";


/* non-export function prototypes */
/*
 *
 * 非导出函数的原型
 */
static void EndCopy(CopyToState cstate);
static void ClosePipeToProgram(CopyToState cstate);
static void CopyOneRowTo(CopyToState cstate, TupleTableSlot *slot);
static void CopyAttributeOutText(CopyToState cstate, const char *string);
static void CopyAttributeOutCSV(CopyToState cstate, const char *string,
								bool use_quote);

/* built-in format-specific routines */
/*
 *
 * 内置的格式专用例程
 */
static void CopyToTextLikeStart(CopyToState cstate, TupleDesc tupDesc);
static void CopyToTextLikeOutFunc(CopyToState cstate, Oid atttypid, FmgrInfo *finfo);
static void CopyToTextOneRow(CopyToState cstate, TupleTableSlot *slot);
static void CopyToCSVOneRow(CopyToState cstate, TupleTableSlot *slot);
static void CopyToTextLikeOneRow(CopyToState cstate, TupleTableSlot *slot,
								 bool is_csv);
static void CopyToTextLikeEnd(CopyToState cstate);
static void CopyToBinaryStart(CopyToState cstate, TupleDesc tupDesc);
static void CopyToBinaryOutFunc(CopyToState cstate, Oid atttypid, FmgrInfo *finfo);
static void CopyToBinaryOneRow(CopyToState cstate, TupleTableSlot *slot);
static void CopyToBinaryEnd(CopyToState cstate);

/* Low-level communications functions */
/*
 *
 * 底层通信函数
 */
static void SendCopyBegin(CopyToState cstate);
static void SendCopyEnd(CopyToState cstate);
static void CopySendData(CopyToState cstate, const void *databuf, int datasize);
static void CopySendString(CopyToState cstate, const char *str);
static void CopySendChar(CopyToState cstate, char c);
static void CopySendEndOfRow(CopyToState cstate);
static void CopySendTextLikeEndOfRow(CopyToState cstate);
static void CopySendInt32(CopyToState cstate, int32 val);
static void CopySendInt16(CopyToState cstate, int16 val);

/*
 * COPY TO routines for built-in formats.
 *
 * 内置格式的 COPY TO 例程。
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
static const CopyToRoutine CopyToRoutineText = {
	.CopyToStart = CopyToTextLikeStart,
	.CopyToOutFunc = CopyToTextLikeOutFunc,
	.CopyToOneRow = CopyToTextOneRow,
	.CopyToEnd = CopyToTextLikeEnd,
};

/* CSV format */
/*
 *
 * CSV 格式
 */
static const CopyToRoutine CopyToRoutineCSV = {
	.CopyToStart = CopyToTextLikeStart,
	.CopyToOutFunc = CopyToTextLikeOutFunc,
	.CopyToOneRow = CopyToCSVOneRow,
	.CopyToEnd = CopyToTextLikeEnd,
};

/* binary format */
/*
 *
 * binary 格式
 */
static const CopyToRoutine CopyToRoutineBinary = {
	.CopyToStart = CopyToBinaryStart,
	.CopyToOutFunc = CopyToBinaryOutFunc,
	.CopyToOneRow = CopyToBinaryOneRow,
	.CopyToEnd = CopyToBinaryEnd,
};

/* Return a COPY TO routine for the given options */
/*
 *
 * 按给定选项返回 COPY TO 例程
 */
static const CopyToRoutine *
CopyToGetRoutine(const CopyFormatOptions *opts)
{
	if (opts->csv_mode)
		return &CopyToRoutineCSV;
	else if (opts->binary)
		return &CopyToRoutineBinary;

	/* default is text */
	/*
	 *
	 * 默认是 text
	 */
	return &CopyToRoutineText;
}

/* Implementation of the start callback for text and CSV formats */
/*
 *
 * text 与 CSV 格式的开始回调实现
 */
static void
CopyToTextLikeStart(CopyToState cstate, TupleDesc tupDesc)
{
	/*
	 * For non-binary copy, we need to convert null_print to file encoding,
	 * because it will be sent directly with CopySendString.
	 *
	 * 对于非二进制拷贝，需要把 null_print 转换成文件编码，因为它会由 CopySendString 直接发送。
	 */
	if (cstate->need_transcoding)
		cstate->opts.null_print_client = pg_server_to_any(cstate->opts.null_print,
														  cstate->opts.null_print_len,
														  cstate->file_encoding);

	/* if a header has been requested send the line */
	/*
	 *
	 * 若请求了标题行，则发送该行
	 */
	if (cstate->opts.header_line)
	{
		ListCell   *cur;
		bool		hdr_delim = false;

		foreach(cur, cstate->attnumlist)
		{
			int			attnum = lfirst_int(cur);
			char	   *colname;

			if (hdr_delim)
				CopySendChar(cstate, cstate->opts.delim[0]);
			hdr_delim = true;

			colname = NameStr(TupleDescAttr(tupDesc, attnum - 1)->attname);

			if (cstate->opts.csv_mode)
				CopyAttributeOutCSV(cstate, colname, false);
			else
				CopyAttributeOutText(cstate, colname);
		}

		CopySendTextLikeEndOfRow(cstate);
	}
}

/*
 * Implementation of the outfunc callback for text and CSV formats. Assign
 * the output function data to the given *finfo.
 *
 * text 与 CSV 格式的 outfunc 回调实现。把输出函数数据赋给给定的 finfo。
 */
static void
CopyToTextLikeOutFunc(CopyToState cstate, Oid atttypid, FmgrInfo *finfo)
{
	Oid			func_oid;
	bool		is_varlena;

	/* Set output function for an attribute */
	/*
	 *
	 * 设置属性的输出函数
	 */
	getTypeOutputInfo(atttypid, &func_oid, &is_varlena);
	fmgr_info(func_oid, finfo);
}

/* Implementation of the per-row callback for text format */
/*
 *
 * text 格式的逐行回调实现
 */
static void
CopyToTextOneRow(CopyToState cstate, TupleTableSlot *slot)
{
	CopyToTextLikeOneRow(cstate, slot, false);
}

/* Implementation of the per-row callback for CSV format */
/*
 *
 * CSV 格式的逐行回调实现
 */
static void
CopyToCSVOneRow(CopyToState cstate, TupleTableSlot *slot)
{
	CopyToTextLikeOneRow(cstate, slot, true);
}

/*
 * Workhorse for CopyToTextOneRow() and CopyToCSVOneRow().
 *
 * CopyToTextOneRow() 与 CopyToCSVOneRow() 的主要实现。
 *
 * We use pg_attribute_always_inline to reduce function call overhead
 * and to help compilers to optimize away the 'is_csv' condition.
 *
 * 使用 pg_attribute_always_inline 以减少函数调用开销，并帮助编译器优化掉 is_csv 条件。
 */
static pg_attribute_always_inline void
CopyToTextLikeOneRow(CopyToState cstate,
					 TupleTableSlot *slot,
					 bool is_csv)
{
	bool		need_delim = false;
	FmgrInfo   *out_functions = cstate->out_functions;

	foreach_int(attnum, cstate->attnumlist)
	{
		Datum		value = slot->tts_values[attnum - 1];
		bool		isnull = slot->tts_isnull[attnum - 1];

		if (need_delim)
			CopySendChar(cstate, cstate->opts.delim[0]);
		need_delim = true;

		if (isnull)
		{
			CopySendString(cstate, cstate->opts.null_print_client);
		}
		else
		{
			char	   *string;

			string = OutputFunctionCall(&out_functions[attnum - 1],
										value);

			if (is_csv)
				CopyAttributeOutCSV(cstate, string,
									cstate->opts.force_quote_flags[attnum - 1]);
			else
				CopyAttributeOutText(cstate, string);
		}
	}

	CopySendTextLikeEndOfRow(cstate);
}

/* Implementation of the end callback for text and CSV formats */
/*
 *
 * text 与 CSV 格式的结束回调实现
 */
static void
CopyToTextLikeEnd(CopyToState cstate)
{
	/* Nothing to do here */
	/*
	 *
	 * 这里无事可做
	 */
}

/*
 * Implementation of the start callback for binary format. Send a header
 * for a binary copy.
 *
 * binary 格式的开始回调实现。为二进制拷贝发送一个头。
 */
static void
CopyToBinaryStart(CopyToState cstate, TupleDesc tupDesc)
{
	int32		tmp;

	/* Signature */
	/*
	 *
	 * 签名
	 */
	CopySendData(cstate, BinarySignature, 11);
	/* Flags field */
	/*
	 *
	 * 标志字段
	 */
	tmp = 0;
	CopySendInt32(cstate, tmp);
	/* No header extension */
	/*
	 *
	 * 没有头扩展
	 */
	tmp = 0;
	CopySendInt32(cstate, tmp);
}

/*
 * Implementation of the outfunc callback for binary format. Assign
 * the binary output function to the given *finfo.
 *
 * binary 格式的 outfunc 回调实现。把二进制输出函数赋给给定的 finfo。
 */
static void
CopyToBinaryOutFunc(CopyToState cstate, Oid atttypid, FmgrInfo *finfo)
{
	Oid			func_oid;
	bool		is_varlena;

	/* Set output function for an attribute */
	/*
	 *
	 * 设置属性的输出函数
	 */
	getTypeBinaryOutputInfo(atttypid, &func_oid, &is_varlena);
	fmgr_info(func_oid, finfo);
}

/* Implementation of the per-row callback for binary format */
/*
 *
 * binary 格式的逐行回调实现
 */
static void
CopyToBinaryOneRow(CopyToState cstate, TupleTableSlot *slot)
{
	FmgrInfo   *out_functions = cstate->out_functions;

	/* Binary per-tuple header */
	/*
	 *
	 * 二进制的每元组头
	 */
	CopySendInt16(cstate, list_length(cstate->attnumlist));

	foreach_int(attnum, cstate->attnumlist)
	{
		Datum		value = slot->tts_values[attnum - 1];
		bool		isnull = slot->tts_isnull[attnum - 1];

		if (isnull)
		{
			CopySendInt32(cstate, -1);
		}
		else
		{
			bytea	   *outputbytes;

			outputbytes = SendFunctionCall(&out_functions[attnum - 1],
										   value);
			CopySendInt32(cstate, VARSIZE(outputbytes) - VARHDRSZ);
			CopySendData(cstate, VARDATA(outputbytes),
						 VARSIZE(outputbytes) - VARHDRSZ);
		}
	}

	CopySendEndOfRow(cstate);
}

/* Implementation of the end callback for binary format */
/*
 *
 * binary 格式的结束回调实现
 */
static void
CopyToBinaryEnd(CopyToState cstate)
{
	/* Generate trailer for a binary copy */
	/*
	 *
	 * 为二进制拷贝生成尾部
	 */
	CopySendInt16(cstate, -1);
	/* Need to flush out the trailer */
	/*
	 *
	 * 需要把尾部刷出去
	 */
	CopySendEndOfRow(cstate);
}

/*
 * Send copy start/stop messages for frontend copies.  These have changed
 * in past protocol redesigns.
 *
 * 为前端拷贝发送开始/结束消息。这些消息在以往的协议重新设计中有过变化。
 */
static void
SendCopyBegin(CopyToState cstate)
{
	StringInfoData buf;
	int			natts = list_length(cstate->attnumlist);
	int16		format = (cstate->opts.binary ? 1 : 0);
	int			i;

	pq_beginmessage(&buf, PqMsg_CopyOutResponse);
	pq_sendbyte(&buf, format);	/* overall format */
	/*
	 *
	 * 整体格式
	 */
	pq_sendint16(&buf, natts);
	for (i = 0; i < natts; i++)
		pq_sendint16(&buf, format); /* per-column formats */
		/*
		 *
		 * 每列的格式
		 */
	pq_endmessage(&buf);
	cstate->copy_dest = COPY_FRONTEND;
}

/*
 * 向前端发送 CopyDone，结束 COPY TO STDOUT。
 */
static void
SendCopyEnd(CopyToState cstate)
{
	/* Shouldn't have any unsent data */
	/*
	 *
	 * 不应有任何未发送的数据
	 */
	Assert(cstate->fe_msgbuf->len == 0);
	/* Send Copy Done message */
	/*
	 *
	 * 发送 Copy Done 消息
	 */
	pq_putemptymessage(PqMsg_CopyDone);
}

/*----------
 * CopySendData sends output data to the destination (file or frontend)
 * CopySendString does the same for null-terminated strings
 * CopySendChar does the same for single characters
 * CopySendEndOfRow does the appropriate thing at end of each data row
 *	(data is not actually flushed except by CopySendEndOfRow)
 *
 * CopySendData 把输出数据发到目标（文件或前端）。
 * CopySendString 对以 NUL 结尾的字符串做同样的事。
 * CopySendChar 对单个字符做同样的事。
 * CopySendEndOfRow 在每个数据行结束时做相应处理。
 * （数据实际上只由 CopySendEndOfRow 刷出）
 *
 * NB: no data conversion is applied by these functions
 *
 * 注意：这些函数不做数据转换
 *----------
 */
static void
CopySendData(CopyToState cstate, const void *databuf, int datasize)
{
	appendBinaryStringInfo(cstate->fe_msgbuf, databuf, datasize);
}

/*
 * 将以 NUL 结尾的字符串追加到 COPY 输出缓冲。
 */
static void
CopySendString(CopyToState cstate, const char *str)
{
	appendBinaryStringInfo(cstate->fe_msgbuf, str, strlen(str));
}

/*
 * 将单个字符追加到 COPY 输出缓冲。
 */
static void
CopySendChar(CopyToState cstate, char c)
{
	appendStringInfoCharMacro(cstate->fe_msgbuf, c);
}

/*
 * 结束当前输出行，把缓冲写入文件、发给前端或交给回调。
 */
static void
CopySendEndOfRow(CopyToState cstate)
{
	StringInfo	fe_msgbuf = cstate->fe_msgbuf;

	switch (cstate->copy_dest)
	{
		case COPY_FILE:
			if (fwrite(fe_msgbuf->data, fe_msgbuf->len, 1,
					   cstate->copy_file) != 1 ||
				ferror(cstate->copy_file))
			{
				if (cstate->is_program)
				{
					if (errno == EPIPE)
					{
						/*
						 * The pipe will be closed automatically on error at
						 * the end of transaction, but we might get a better
						 * error message from the subprocess' exit code than
						 * just "Broken Pipe"
						 *
						 * 出错时管道会在事务结束时自动关闭，但子进程的退出码也许能给出比 Broken Pipe 更好的错误信息
						 */
						ClosePipeToProgram(cstate);

						/*
						 * If ClosePipeToProgram() didn't throw an error, the
						 * program terminated normally, but closed the pipe
						 * first. Restore errno, and throw an error.
						 *
						 * 若 ClosePipeToProgram() 没有抛错，说明程序正常结束，但先关闭了管道。恢复 errno 并抛出错误。
						 */
						errno = EPIPE;
					}
					ereport(ERROR,
							(errcode_for_file_access(),
							 errmsg("could not write to COPY program: %m")));
				}
				else
					ereport(ERROR,
							(errcode_for_file_access(),
							 errmsg("could not write to COPY file: %m")));
			}
			break;
		case COPY_FRONTEND:
			/* Dump the accumulated row as one CopyData message */
			/*
			 *
			 * 把累积的行作为一条 CopyData 消息转储出去
			 */
			(void) pq_putmessage(PqMsg_CopyData, fe_msgbuf->data, fe_msgbuf->len);
			break;
		case COPY_CALLBACK:
			cstate->data_dest_cb(fe_msgbuf->data, fe_msgbuf->len);
			break;
	}

	/* Update the progress */
	/*
	 *
	 * 更新进度
	 */
	cstate->bytes_processed += fe_msgbuf->len;
	pgstat_progress_update_param(PROGRESS_COPY_BYTES_PROCESSED, cstate->bytes_processed);

	resetStringInfo(fe_msgbuf);
}

/*
 * Wrapper function of CopySendEndOfRow for text and CSV formats. Sends the
 * line termination and do common appropriate things for the end of row.
 *
 * text 与 CSV 格式的 CopySendEndOfRow 包装函数。发送行终止符，并做行结束时的公共处理。
 */
static inline void
CopySendTextLikeEndOfRow(CopyToState cstate)
{
	switch (cstate->copy_dest)
	{
		case COPY_FILE:
			/* Default line termination depends on platform */
			/*
			 *
			 * 默认行终止符取决于平台
			 */
#ifndef WIN32
			CopySendChar(cstate, '\n');
#else
			CopySendString(cstate, "\r\n");
#endif
			break;
		case COPY_FRONTEND:
			/* The FE/BE protocol uses \n as newline for all platforms */
			/*
			 *
			 * 前端/后端协议在所有平台上都用 \n 作为换行
			 */
			CopySendChar(cstate, '\n');
			break;
		default:
			break;
	}

	/* Now take the actions related to the end of a row */
	/*
	 *
	 * 现在执行与行结束相关的动作
	 */
	CopySendEndOfRow(cstate);
}

/*
 * These functions do apply some data conversion
 *
 * 这些函数会做一些数据转换
 */

/*
 * CopySendInt32 sends an int32 in network byte order
 *
 * CopySendInt32 以网络字节序发送 int32
 */
static inline void
CopySendInt32(CopyToState cstate, int32 val)
{
	uint32		buf;

	buf = pg_hton32((uint32) val);
	CopySendData(cstate, &buf, sizeof(buf));
}

/*
 * CopySendInt16 sends an int16 in network byte order
 *
 * CopySendInt16 以网络字节序发送 int16
 */
static inline void
CopySendInt16(CopyToState cstate, int16 val)
{
	uint16		buf;

	buf = pg_hton16((uint16) val);
	CopySendData(cstate, &buf, sizeof(buf));
}

/*
 * Closes the pipe to an external program, checking the pclose() return code.
 *
 * 关闭通向外部程序的管道，并检查 pclose() 的返回码。
 */
static void
ClosePipeToProgram(CopyToState cstate)
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
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("program \"%s\" failed",
						cstate->filename),
				 errdetail_internal("%s", wait_result_to_str(pclose_rc))));
	}
}

/*
 * Release resources allocated in a cstate for COPY TO/FROM.
 *
 * 释放在 cstate 中为 COPY TO/FROM 分配的资源。
 */
static void
EndCopy(CopyToState cstate)
{
	if (cstate->is_program)
	{
		ClosePipeToProgram(cstate);
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
 * 核心流程概览：
 * BeginCopyTo：解析 COPY TO 的选项、目标与查询，建立 CopyToState。
 * DoCopyTo：扫描表或执行查询，逐行调用 CopyOneRowTo。
 * CopyToTextLikeOneRow / CopyToBinaryOneRow：按格式输出一行。
 * CopySendEndOfRow：把缓冲写到文件、前端或回调。
 * EndCopyTo：结束输出并释放状态。
 */

/*
 * Setup CopyToState to read tuples from a table or a query for COPY TO.
 *
 * 设置 CopyToState，以便为 COPY TO 从表或查询读取元组。
 *
 * 'rel': Relation to be copied
 * 'raw_query': Query whose results are to be copied
 * 'queryRelId': OID of base relation to convert to a query (for RLS)
 * 'filename': Name of server-local file to write, NULL for STDOUT
 * 'is_program': true if 'filename' is program to execute
 * 'data_dest_cb': Callback that processes the output data
 * 'attnamelist': List of char *, columns to include. NIL selects all cols.
 * 'options': List of DefElem. See copy_opt_item in gram.y for selections.
 *
 * rel：要拷贝的关系
 * raw_query：其结果要被拷贝的查询
 * queryRelId：要转换成查询的基表 OID（用于 RLS）
 * filename：要写入的服务器本地文件名，STDOUT 时为 NULL
 * is_program：若 filename 是要执行的程序则为 true
 * data_dest_cb：处理输出数据的回调
 * attnamelist：要包含的列名（char *）列表。NIL 表示选择全部列。
 * options：DefElem 列表。可选项见 gram.y 中的 copy_opt_item。
 *
 * Returns a CopyToState, to be passed to DoCopyTo() and related functions.
 *
 * 返回 CopyToState，供 DoCopyTo() 及相关函数使用。
 */
CopyToState
BeginCopyTo(ParseState *pstate,
			Relation rel,
			RawStmt *raw_query,
			Oid queryRelId,
			const char *filename,
			bool is_program,
			copy_data_dest_cb data_dest_cb,
			List *attnamelist,
			List *options)
{
	CopyToState cstate;
	bool		pipe = (filename == NULL && data_dest_cb == NULL);
	TupleDesc	tupDesc;
	int			num_phys_attrs;
	MemoryContext oldcontext;
	const int	progress_cols[] = {
		PROGRESS_COPY_COMMAND,
		PROGRESS_COPY_TYPE
	};
	int64		progress_vals[] = {
		PROGRESS_COPY_COMMAND_TO,
		0
	};

	if (rel != NULL && rel->rd_rel->relkind != RELKIND_RELATION)
	{
		if (rel->rd_rel->relkind == RELKIND_VIEW)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot copy from view \"%s\"",
							RelationGetRelationName(rel)),
					 errhint("Try the COPY (SELECT ...) TO variant.")));
		else if (rel->rd_rel->relkind == RELKIND_MATVIEW)
		{
			if (!RelationIsPopulated(rel))
				ereport(ERROR,
						errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("cannot copy from unpopulated materialized view \"%s\"",
							   RelationGetRelationName(rel)),
						errhint("Use the REFRESH MATERIALIZED VIEW command."));
		}
		else if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot copy from foreign table \"%s\"",
							RelationGetRelationName(rel)),
					 errhint("Try the COPY (SELECT ...) TO variant.")));
		else if (rel->rd_rel->relkind == RELKIND_SEQUENCE)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot copy from sequence \"%s\"",
							RelationGetRelationName(rel))));
		else if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot copy from partitioned table \"%s\"",
							RelationGetRelationName(rel)),
					 errhint("Try the COPY (SELECT ...) TO variant.")));
		else
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot copy from non-table relation \"%s\"",
							RelationGetRelationName(rel))));
	}


	/* Allocate workspace and zero all fields */
	/*
	 *
	 * 分配工作区并把所有字段清零
	 */
	cstate = (CopyToStateData *) palloc0(sizeof(CopyToStateData));

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
	ProcessCopyOptions(pstate, &cstate->opts, false /* is_from */ , options);
	/*
	 *
	 * 方向为 COPY FROM
	 */

	/* Set format routine */
	/*
	 *
	 * 设置格式例程
	 */
	cstate->routine = CopyToGetRoutine(&cstate->opts);

	/* Process the source/target relation or query */
	/*
	 *
	 * 处理源/目标关系或查询
	 */
	if (rel)
	{
		Assert(!raw_query);

		cstate->rel = rel;

		tupDesc = RelationGetDescr(cstate->rel);
	}
	else
	{
		List	   *rewritten;
		Query	   *query;
		PlannedStmt *plan;
		DestReceiver *dest;

		cstate->rel = NULL;

		/*
		 * Run parse analysis and rewrite.  Note this also acquires sufficient
		 * locks on the source table(s).
		 *
		 * 运行语法分析和重写。注意这也会在源表上取得足够的锁。
		 */
		rewritten = pg_analyze_and_rewrite_fixedparams(raw_query,
													   pstate->p_sourcetext, NULL, 0,
													   NULL);

		/* check that we got back something we can work with */
		/*
		 *
		 * 确认得到的是我们可以处理的结果
		 */
		if (rewritten == NIL)
		{
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("DO INSTEAD NOTHING rules are not supported for COPY")));
		}
		else if (list_length(rewritten) > 1)
		{
			ListCell   *lc;

			/* examine queries to determine which error message to issue */
			/*
			 *
			 * 检查这些查询，以决定发出哪条错误信息
			 */
			foreach(lc, rewritten)
			{
				Query	   *q = lfirst_node(Query, lc);

				if (q->querySource == QSRC_QUAL_INSTEAD_RULE)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("conditional DO INSTEAD rules are not supported for COPY")));
				if (q->querySource == QSRC_NON_INSTEAD_RULE)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("DO ALSO rules are not supported for COPY")));
			}

			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("multi-statement DO INSTEAD rules are not supported for COPY")));
		}

		query = linitial_node(Query, rewritten);

		/* The grammar allows SELECT INTO, but we don't support that */
		/*
		 *
		 * 语法允许 SELECT INTO，但我们不支持
		 */
		if (query->utilityStmt != NULL &&
			IsA(query->utilityStmt, CreateTableAsStmt))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("COPY (SELECT INTO) is not supported")));

		/* The only other utility command we could see is NOTIFY */
		/*
		 *
		 * 我们可能看到的唯一另一种实用命令是 NOTIFY
		 */
		if (query->utilityStmt != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("COPY query must not be a utility command")));

		/*
		 * Similarly the grammar doesn't enforce the presence of a RETURNING
		 * clause, but this is required here.
		 *
		 * 语法同样不强制必须有 RETURNING 子句，但这里需要它。
		 */
		if (query->commandType != CMD_SELECT &&
			query->returningList == NIL)
		{
			Assert(query->commandType == CMD_INSERT ||
				   query->commandType == CMD_UPDATE ||
				   query->commandType == CMD_DELETE ||
				   query->commandType == CMD_MERGE);

			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("COPY query must have a RETURNING clause")));
		}

		/* plan the query */
		/*
		 *
		 * 规划该查询
		 */
		plan = pg_plan_query(query, pstate->p_sourcetext,
							 CURSOR_OPT_PARALLEL_OK, NULL);

		/*
		 * With row-level security and a user using "COPY relation TO", we
		 * have to convert the "COPY relation TO" to a query-based COPY (eg:
		 * "COPY (SELECT * FROM ONLY relation) TO"), to allow the rewriter to
		 * add in any RLS clauses.
		 *
		 * 在有行级安全且用户使用 COPY relation TO 时，必须把 COPY relation TO 转换成基于查询的 COPY
		 * （例如 COPY (SELECT * FROM ONLY relation) TO），以便重写器加入任何 RLS 子句。
		 *
		 * When this happens, we are passed in the relid of the originally
		 * found relation (which we have locked).  As the planner will look up
		 * the relation again, we double-check here to make sure it found the
		 * same one that we have locked.
		 *
		 * 发生这种情况时，会传入最初找到的关系的 relid（我们已经锁定它）。
		 * 规划器会再次查找该关系，因此这里再检查一次，确认它找到的是我们锁定的同一个关系。
		 */
		if (queryRelId != InvalidOid)
		{
			/*
			 * Note that with RLS involved there may be multiple relations,
			 * and while the one we need is almost certainly first, we don't
			 * make any guarantees of that in the planner, so check the whole
			 * list and make sure we find the original relation.
			 *
			 * 注意涉及 RLS 时可能有多个关系，我们需要的那个几乎肯定排在最前，
			 * 但规划器并不保证这一点，因此要检查整个列表，确认找到的是原来的关系。
			 */
			if (!list_member_oid(plan->relationOids, queryRelId))
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("relation referenced by COPY statement has changed")));
		}

		/*
		 * Use a snapshot with an updated command ID to ensure this query sees
		 * results of any previously executed queries.
		 *
		 * 使用带有更新后命令 ID 的快照，以确保本查询能看到此前已执行查询的结果。
		 */
		PushCopiedSnapshot(GetActiveSnapshot());
		UpdateActiveSnapshotCommandId();

		/* Create dest receiver for COPY OUT */
		/*
		 *
		 * 为 COPY OUT 创建目标接收器
		 */
		dest = CreateDestReceiver(DestCopyOut);
		((DR_copy *) dest)->cstate = cstate;

		/* Create a QueryDesc requesting no output */
		/*
		 *
		 * 创建一个不请求输出的 QueryDesc
		 */
		cstate->queryDesc = CreateQueryDesc(plan, pstate->p_sourcetext,
											GetActiveSnapshot(),
											InvalidSnapshot,
											dest, NULL, NULL, 0);

		/*
		 * Call ExecutorStart to prepare the plan for execution.
		 *
		 * 调用 ExecutorStart 以准备执行该计划。
		 *
		 * ExecutorStart computes a result tupdesc for us
		 *
		 * ExecutorStart 会为我们计算一个结果 tupdesc
		 */
		ExecutorStart(cstate->queryDesc, 0);

		tupDesc = cstate->queryDesc->tupDesc;
	}

	/* Generate or convert list of attributes to process */
	/*
	 *
	 * 生成或转换要处理的属性列表
	 */
	cstate->attnumlist = CopyGetAttnums(tupDesc, cstate->rel, attnamelist);

	num_phys_attrs = tupDesc->natts;

	/* Convert FORCE_QUOTE name list to per-column flags, check validity */
	/*
	 *
	 * 把 FORCE_QUOTE 名字列表转换成按列标志，并检查有效性
	 */
	cstate->opts.force_quote_flags = (bool *) palloc0(num_phys_attrs * sizeof(bool));
	if (cstate->opts.force_quote_all)
	{
		MemSet(cstate->opts.force_quote_flags, true, num_phys_attrs * sizeof(bool));
	}
	else if (cstate->opts.force_quote)
	{
		List	   *attnums;
		ListCell   *cur;

		attnums = CopyGetAttnums(tupDesc, cstate->rel, cstate->opts.force_quote);

		foreach(cur, attnums)
		{
			int			attnum = lfirst_int(cur);
			Form_pg_attribute attr = TupleDescAttr(tupDesc, attnum - 1);

			if (!list_member_int(cstate->attnumlist, attnum))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				/*- translator: %s is the name of a COPY option, e.g. FORCE_NOT_NULL */
				/*
				 *
				 * 翻译提示：%s 是 COPY 选项名，例如 FORCE_NOT_NULL
				 */
						 errmsg("%s column \"%s\" not referenced by COPY",
								"FORCE_QUOTE", NameStr(attr->attname))));
			cstate->opts.force_quote_flags[attnum - 1] = true;
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
	 * Set up encoding conversion info if the file and server encodings differ
	 * (see also pg_server_to_any).
	 *
	 * 若文件编码与服务器编码不同，则设置编码转换信息（另见 pg_server_to_any）。
	 */
	if (cstate->file_encoding == GetDatabaseEncoding() ||
		cstate->file_encoding == PG_SQL_ASCII)
		cstate->need_transcoding = false;
	else
		cstate->need_transcoding = true;

	/* See Multibyte encoding comment above */
	/*
	 *
	 * 参见上面关于多字节编码的注释
	 */
	cstate->encoding_embeds_ascii = PG_ENCODING_IS_CLIENT_ONLY(cstate->file_encoding);

	cstate->copy_dest = COPY_FILE;	/* default */
	/*
	 *
	 * 默认
	 */

	if (data_dest_cb)
	{
		progress_vals[1] = PROGRESS_COPY_TYPE_CALLBACK;
		cstate->copy_dest = COPY_CALLBACK;
		cstate->data_dest_cb = data_dest_cb;
	}
	else if (pipe)
	{
		progress_vals[1] = PROGRESS_COPY_TYPE_PIPE;

		Assert(!is_program);	/* the grammar does not allow this */
		/*
		 *
		 * 语法不允许这样做
		 */
		if (whereToSendOutput != DestRemote)
			cstate->copy_file = stdout;
	}
	else
	{
		cstate->filename = pstrdup(filename);
		cstate->is_program = is_program;

		if (is_program)
		{
			progress_vals[1] = PROGRESS_COPY_TYPE_PROGRAM;
			cstate->copy_file = OpenPipeStream(cstate->filename, PG_BINARY_W);
			if (cstate->copy_file == NULL)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not execute command \"%s\": %m",
								cstate->filename)));
		}
		else
		{
			mode_t		oumask; /* Pre-existing umask value */
			/*
			 *
			 * 先前的 umask 值
			 */
			struct stat st;

			progress_vals[1] = PROGRESS_COPY_TYPE_FILE;

			/*
			 * Prevent write to relative path ... too easy to shoot oneself in
			 * the foot by overwriting a database file ...
			 *
			 * 禁止写到相对路径……太容易因覆盖数据库文件而搬起石头砸自己的脚……
			 */
			if (!is_absolute_path(filename))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_NAME),
						 errmsg("relative path not allowed for COPY to file")));

			oumask = umask(S_IWGRP | S_IWOTH);
			PG_TRY();
			{
				cstate->copy_file = AllocateFile(cstate->filename, PG_BINARY_W);
			}
			PG_FINALLY();
			{
				umask(oumask);
			}
			PG_END_TRY();
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
						 errmsg("could not open file \"%s\" for writing: %m",
								cstate->filename),
						 (save_errno == ENOENT || save_errno == EACCES) ?
						 errhint("COPY TO instructs the PostgreSQL server process to write a file. "
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
		}
	}

	/* initialize progress */
	/*
	 *
	 * 初始化进度报告
	 */
	pgstat_progress_start_command(PROGRESS_COMMAND_COPY,
								  cstate->rel ? RelationGetRelid(cstate->rel) : InvalidOid);
	pgstat_progress_update_multi_param(2, progress_cols, progress_vals);

	cstate->bytes_processed = 0;

	MemoryContextSwitchTo(oldcontext);

	return cstate;
}

/*
 * Clean up storage and release resources for COPY TO.
 *
 * 清理存储并释放 COPY TO 的资源。
 */
void
EndCopyTo(CopyToState cstate)
{
	if (cstate->queryDesc != NULL)
	{
		/* Close down the query and free resources. */
		/*
		 *
		 * 关闭查询并释放资源。
		 */
		ExecutorFinish(cstate->queryDesc);
		ExecutorEnd(cstate->queryDesc);
		FreeQueryDesc(cstate->queryDesc);
		PopActiveSnapshot();
	}

	/* Clean up storage */
	/*
	 *
	 * 清理存储
	 */
	EndCopy(cstate);
}

/*
 * Copy from relation or query TO file.
 *
 * 把关系或查询 COPY 到文件。
 *
 * Returns the number of rows processed.
 *
 * 返回已处理的行数。
 */
uint64
DoCopyTo(CopyToState cstate)
{
	bool		pipe = (cstate->filename == NULL && cstate->data_dest_cb == NULL);
	bool		fe_copy = (pipe && whereToSendOutput == DestRemote);
	TupleDesc	tupDesc;
	int			num_phys_attrs;
	ListCell   *cur;
	uint64		processed;

	if (fe_copy)
		SendCopyBegin(cstate);

	if (cstate->rel)
		tupDesc = RelationGetDescr(cstate->rel);
	else
		tupDesc = cstate->queryDesc->tupDesc;
	num_phys_attrs = tupDesc->natts;
	cstate->opts.null_print_client = cstate->opts.null_print;	/* default */
	/*
	 *
	 * 默认
	 */

	/* We use fe_msgbuf as a per-row buffer regardless of copy_dest */
	/*
	 *
	 * 无论 copy_dest 是什么，都用 fe_msgbuf 作为每行缓冲区
	 */
	cstate->fe_msgbuf = makeStringInfo();

	/* Get info about the columns we need to process. */
	/*
	 *
	 * 取得需要处理的列的信息。
	 */
	cstate->out_functions = (FmgrInfo *) palloc(num_phys_attrs * sizeof(FmgrInfo));
	foreach(cur, cstate->attnumlist)
	{
		int			attnum = lfirst_int(cur);
		Form_pg_attribute attr = TupleDescAttr(tupDesc, attnum - 1);

		cstate->routine->CopyToOutFunc(cstate, attr->atttypid,
									   &cstate->out_functions[attnum - 1]);
	}

	/*
	 * Create a temporary memory context that we can reset once per row to
	 * recover palloc'd memory.  This avoids any problems with leaks inside
	 * datatype output routines, and should be faster than retail pfree's
	 * anyway.  (We don't need a whole econtext as CopyFrom does.)
	 *
	 * 创建一个临时内存上下文，每行重置一次以回收 palloc 的内存。
	 * 这可以避免数据类型输出例程内部泄漏的问题，而且应该比逐个 pfree 更快。
	 * （这里不像 CopyFrom 那样需要完整的 econtext。）
	 */
	cstate->rowcontext = AllocSetContextCreate(CurrentMemoryContext,
											   "COPY TO",
											   ALLOCSET_DEFAULT_SIZES);

	cstate->routine->CopyToStart(cstate, tupDesc);

	if (cstate->rel)
	{
		TupleTableSlot *slot;
		TableScanDesc scandesc;

		scandesc = table_beginscan(cstate->rel, GetActiveSnapshot(), 0, NULL);
		slot = table_slot_create(cstate->rel, NULL);

		processed = 0;
		while (table_scan_getnextslot(scandesc, ForwardScanDirection, slot))
		{
			CHECK_FOR_INTERRUPTS();

			/* Deconstruct the tuple ... */
			/*
			 *
			 * 拆解元组……
			 */
			slot_getallattrs(slot);

			/* Format and send the data */
			/*
			 *
			 * 格式化并发送数据
			 */
			CopyOneRowTo(cstate, slot);

			/*
			 * Increment the number of processed tuples, and report the
			 * progress.
			 *
			 * 增加已处理元组数，并报告进度。
			 */
			pgstat_progress_update_param(PROGRESS_COPY_TUPLES_PROCESSED,
										 ++processed);
		}

		ExecDropSingleTupleTableSlot(slot);
		table_endscan(scandesc);
	}
	else
	{
		/* run the plan --- the dest receiver will send tuples */
		/*
		 *
		 * 运行计划，由目标接收器发送元组
		 */
		ExecutorRun(cstate->queryDesc, ForwardScanDirection, 0);
		processed = ((DR_copy *) cstate->queryDesc->dest)->processed;
	}

	cstate->routine->CopyToEnd(cstate);

	MemoryContextDelete(cstate->rowcontext);

	if (fe_copy)
		SendCopyEnd(cstate);

	return processed;
}

/*
 * Emit one row during DoCopyTo().
 *
 * 在 DoCopyTo() 期间输出一行。
 */
static inline void
CopyOneRowTo(CopyToState cstate, TupleTableSlot *slot)
{
	MemoryContext oldcontext;

	MemoryContextReset(cstate->rowcontext);
	oldcontext = MemoryContextSwitchTo(cstate->rowcontext);

	/* Make sure the tuple is fully deconstructed */
	/*
	 *
	 * 确保元组已被完全拆解
	 */
	slot_getallattrs(slot);

	cstate->routine->CopyToOneRow(cstate, slot);

	MemoryContextSwitchTo(oldcontext);
}

/*
 * Send text representation of one attribute, with conversion and escaping
 *
 * 发送一个属性的文本表示，并做转换和转义
 */
#define DUMPSOFAR() \
	do { \
		if (ptr > start) \
			CopySendData(cstate, start, ptr - start); \
	} while (0)

/*
 * 按 text 格式转义并发送一个字段值。
 */
static void
CopyAttributeOutText(CopyToState cstate, const char *string)
{
	const char *ptr;
	const char *start;
	char		c;
	char		delimc = cstate->opts.delim[0];

	if (cstate->need_transcoding)
		ptr = pg_server_to_any(string, strlen(string), cstate->file_encoding);
	else
		ptr = string;

	/*
	 * We have to grovel through the string searching for control characters
	 * and instances of the delimiter character.  In most cases, though, these
	 * are infrequent.  To avoid overhead from calling CopySendData once per
	 * character, we dump out all characters between escaped characters in a
	 * single call.  The loop invariant is that the data from "start" to "ptr"
	 * can be sent literally, but hasn't yet been.
	 *
	 * 必须遍历字符串，查找控制字符和分隔符实例。不过在大多数情况下它们并不常见。
	 * 为避免每个字符都调用一次 CopySendData 的开销，我们把转义字符之间的所有字符一次输出。
	 * 循环不变量是：从 start 到 ptr 的数据可以按字面发送，但尚未发送。
	 *
	 * We can skip pg_encoding_mblen() overhead when encoding is safe, because
	 * in valid backend encodings, extra bytes of a multibyte character never
	 * look like ASCII.  This loop is sufficiently performance-critical that
	 * it's worth making two copies of it to get the IS_HIGHBIT_SET() test out
	 * of the normal safe-encoding path.
	 *
	 * 当编码安全时可以跳过 pg_encoding_mblen() 的开销，因为在合法的后端编码中，多字节字符的后续字节看起来从不会像 ASCII。
	 * 这个循环对性能足够关键，值得复制两份，以便在正常的安全编码路径上拿掉 IS_HIGHBIT_SET() 测试。
	 */
	if (cstate->encoding_embeds_ascii)
	{
		start = ptr;
		while ((c = *ptr) != '\0')
		{
			if ((unsigned char) c < (unsigned char) 0x20)
			{
				/*
				 * \r and \n must be escaped, the others are traditional. We
				 * prefer to dump these using the C-like notation, rather than
				 * a backslash and the literal character, because it makes the
				 * dump file a bit more proof against Microsoftish data
				 * mangling.
				 *
				 * \r 和 \n 必须转义，其余是传统做法。
				 * 我们倾向于用类似 C 的记法转储它们，而不是反斜杠加字面字符，这样转储文件更能抵御类似微软式的数据破坏。
				 */
				switch (c)
				{
					case '\b':
						c = 'b';
						break;
					case '\f':
						c = 'f';
						break;
					case '\n':
						c = 'n';
						break;
					case '\r':
						c = 'r';
						break;
					case '\t':
						c = 't';
						break;
					case '\v':
						c = 'v';
						break;
					default:
						/* If it's the delimiter, must backslash it */
						/*
						 *
						 * 若它是分隔符，必须用反斜杠转义
						 */
						if (c == delimc)
							break;
						/* All ASCII control chars are length 1 */
						/*
						 *
						 * 所有 ASCII 控制字符的长度都是 1
						 */
						ptr++;
						continue;	/* fall to end of loop */
						/*
						 *
						 * 落到循环末尾
						 */
				}
				/* if we get here, we need to convert the control char */
				/*
				 *
				 * 若到达这里，需要转换该控制字符
				 */
				DUMPSOFAR();
				CopySendChar(cstate, '\\');
				CopySendChar(cstate, c);
				start = ++ptr;	/* do not include char in next run */
				/*
				 *
				 * 下一轮不包含该字符
				 */
			}
			else if (c == '\\' || c == delimc)
			{
				DUMPSOFAR();
				CopySendChar(cstate, '\\');
				start = ptr++;	/* we include char in next run */
				/*
				 *
				 * 下一轮包含该字符
				 */
			}
			else if (IS_HIGHBIT_SET(c))
				ptr += pg_encoding_mblen(cstate->file_encoding, ptr);
			else
				ptr++;
		}
	}
	else
	{
		start = ptr;
		while ((c = *ptr) != '\0')
		{
			if ((unsigned char) c < (unsigned char) 0x20)
			{
				/*
				 * \r and \n must be escaped, the others are traditional. We
				 * prefer to dump these using the C-like notation, rather than
				 * a backslash and the literal character, because it makes the
				 * dump file a bit more proof against Microsoftish data
				 * mangling.
				 *
				 * \r 和 \n 必须转义，其余是传统做法。
				 * 我们倾向于用类似 C 的记法转储它们，而不是反斜杠加字面字符，这样转储文件更能抵御类似微软式的数据破坏。
				 */
				switch (c)
				{
					case '\b':
						c = 'b';
						break;
					case '\f':
						c = 'f';
						break;
					case '\n':
						c = 'n';
						break;
					case '\r':
						c = 'r';
						break;
					case '\t':
						c = 't';
						break;
					case '\v':
						c = 'v';
						break;
					default:
						/* If it's the delimiter, must backslash it */
						/*
						 *
						 * 若它是分隔符，必须用反斜杠转义
						 */
						if (c == delimc)
							break;
						/* All ASCII control chars are length 1 */
						/*
						 *
						 * 所有 ASCII 控制字符的长度都是 1
						 */
						ptr++;
						continue;	/* fall to end of loop */
						/*
						 *
						 * 落到循环末尾
						 */
				}
				/* if we get here, we need to convert the control char */
				/*
				 *
				 * 若到达这里，需要转换该控制字符
				 */
				DUMPSOFAR();
				CopySendChar(cstate, '\\');
				CopySendChar(cstate, c);
				start = ++ptr;	/* do not include char in next run */
				/*
				 *
				 * 下一轮不包含该字符
				 */
			}
			else if (c == '\\' || c == delimc)
			{
				DUMPSOFAR();
				CopySendChar(cstate, '\\');
				start = ptr++;	/* we include char in next run */
				/*
				 *
				 * 下一轮包含该字符
				 */
			}
			else
				ptr++;
		}
	}

	DUMPSOFAR();
}

/*
 * Send text representation of one attribute, with conversion and
 * CSV-style escaping
 *
 * 发送一个属性的文本表示，并做转换和 CSV 风格转义
 */
static void
CopyAttributeOutCSV(CopyToState cstate, const char *string,
					bool use_quote)
{
	const char *ptr;
	const char *start;
	char		c;
	char		delimc = cstate->opts.delim[0];
	char		quotec = cstate->opts.quote[0];
	char		escapec = cstate->opts.escape[0];
	bool		single_attr = (list_length(cstate->attnumlist) == 1);

	/* force quoting if it matches null_print (before conversion!) */
	/*
	 *
	 * 若它匹配 null_print，则强制加引号（在转换之前！）
	 */
	if (!use_quote && strcmp(string, cstate->opts.null_print) == 0)
		use_quote = true;

	if (cstate->need_transcoding)
		ptr = pg_server_to_any(string, strlen(string), cstate->file_encoding);
	else
		ptr = string;

	/*
	 * Make a preliminary pass to discover if it needs quoting
	 *
	 * 先做一遍初步扫描，看是否需要加引号
	 */
	if (!use_quote)
	{
		/*
		 * Quote '\.' if it appears alone on a line, so that it will not be
		 * interpreted as an end-of-data marker.  (PG 18 and up will not
		 * interpret '\.' in CSV that way, except in embedded-in-SQL data; but
		 * we want the data to be loadable by older versions too.  Also, this
		 * avoids breaking clients that are still using PQgetline().)
		 *
		 * 若 '\.' 单独出现在一行上，则给它加引号，以免被解释为数据结束标记。
		 * （PG 18 及以上不会把 CSV 中的 '\.' 那样解释，嵌入 SQL 的数据除外；但我们希望数据也能被旧版本装载。
		 * 同时这也避免破坏仍在使用 PQgetline() 的客户端。）
		 */
		if (single_attr && strcmp(ptr, "\\.") == 0)
			use_quote = true;
		else
		{
			const char *tptr = ptr;

			while ((c = *tptr) != '\0')
			{
				if (c == delimc || c == quotec || c == '\n' || c == '\r')
				{
					use_quote = true;
					break;
				}
				if (IS_HIGHBIT_SET(c) && cstate->encoding_embeds_ascii)
					tptr += pg_encoding_mblen(cstate->file_encoding, tptr);
				else
					tptr++;
			}
		}
	}

	if (use_quote)
	{
		CopySendChar(cstate, quotec);

		/*
		 * We adopt the same optimization strategy as in CopyAttributeOutText
		 *
		 * 采用与 CopyAttributeOutText 相同的优化策略
		 */
		start = ptr;
		while ((c = *ptr) != '\0')
		{
			if (c == quotec || c == escapec)
			{
				DUMPSOFAR();
				CopySendChar(cstate, escapec);
				start = ptr;	/* we include char in next run */
				/*
				 *
				 * 下一轮包含该字符
				 */
			}
			if (IS_HIGHBIT_SET(c) && cstate->encoding_embeds_ascii)
				ptr += pg_encoding_mblen(cstate->file_encoding, ptr);
			else
				ptr++;
		}
		DUMPSOFAR();

		CopySendChar(cstate, quotec);
	}
	else
	{
		/* If it doesn't need quoting, we can just dump it as-is */
		/*
		 *
		 * 若不需要加引号，可以直接原样输出
		 */
		CopySendString(cstate, ptr);
	}
}

/*
 * copy_dest_startup --- executor startup
 *
 * copy_dest_startup：执行器启动
 */
static void
copy_dest_startup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	/* no-op */
	/*
	 *
	 * 空操作
	 */
}

/*
 * copy_dest_receive --- receive one tuple
 *
 * copy_dest_receive：接收一个元组
 */
static bool
copy_dest_receive(TupleTableSlot *slot, DestReceiver *self)
{
	DR_copy    *myState = (DR_copy *) self;
	CopyToState cstate = myState->cstate;

	/* Send the data */
	/*
	 *
	 * 发送数据
	 */
	CopyOneRowTo(cstate, slot);

	/* Increment the number of processed tuples, and report the progress */
	/*
	 *
	 * 增加已处理元组数，并报告进度
	 */
	pgstat_progress_update_param(PROGRESS_COPY_TUPLES_PROCESSED,
								 ++myState->processed);

	return true;
}

/*
 * copy_dest_shutdown --- executor end
 *
 * copy_dest_shutdown：执行器结束
 */
static void
copy_dest_shutdown(DestReceiver *self)
{
	/* no-op */
	/*
	 *
	 * 空操作
	 */
}

/*
 * copy_dest_destroy --- release DestReceiver object
 *
 * copy_dest_destroy：释放 DestReceiver 对象
 */
static void
copy_dest_destroy(DestReceiver *self)
{
	pfree(self);
}

/*
 * CreateCopyDestReceiver -- create a suitable DestReceiver object
 *
 * CreateCopyDestReceiver：创建一个合适的 DestReceiver 对象
 */
DestReceiver *
CreateCopyDestReceiver(void)
{
	DR_copy    *self = (DR_copy *) palloc(sizeof(DR_copy));

	self->pub.receiveSlot = copy_dest_receive;
	self->pub.rStartup = copy_dest_startup;
	self->pub.rShutdown = copy_dest_shutdown;
	self->pub.rDestroy = copy_dest_destroy;
	self->pub.mydest = DestCopyOut;

	self->cstate = NULL;		/* will be set later */
	/*
	 *
	 * 稍后设置
	 */
	self->processed = 0;

	return (DestReceiver *) self;
}

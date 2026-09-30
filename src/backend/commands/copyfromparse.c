/*-------------------------------------------------------------------------
 *
 * copyfromparse.c
 *		Parse CSV/text/binary format for COPY FROM.
 *
 * 解析 COPY FROM 的 CSV/text/binary 格式。
 *
 * This file contains routines to parse the text, CSV and binary input
 * formats.  The main entry point is NextCopyFrom(), which parses the
 * next input line and returns it as Datums.
 *
 * 本文件包含解析 text、CSV 和 binary 输入格式的例程。主入口是 NextCopyFrom()，它解析下一行输入并作为 Datum 返回。
 *
 * In text/CSV mode, the parsing happens in multiple stages:
 *
 * 在 text/CSV 模式下，解析分多个阶段进行：
 *
 * [data source] --> raw_buf --> input_buf --> line_buf --> attribute_buf
 *                1.          2.            3.           4.
 *
 * 1. CopyLoadRawBuf() reads raw data from the input file or client, and
 *    places it into 'raw_buf'.
 *
 * 1. CopyLoadRawBuf() 从输入文件或客户端读取原始数据，并放入 raw_buf。
 *
 * 2. CopyConvertBuf() calls the encoding conversion function to convert
 *    the data in 'raw_buf' from client to server encoding, placing the
 *    converted result in 'input_buf'.
 *
 * 2. CopyConvertBuf() 调用编码转换函数，把 raw_buf 中的数据从客户端编码转换为服务器编码，结果放入 input_buf。
 *
 * 3. CopyReadLine() parses the data in 'input_buf', one line at a time.
 *    It is responsible for finding the next newline marker, taking quote and
 *    escape characters into account according to the COPY options.  The line
 *    is copied into 'line_buf', with quotes and escape characters still
 *    intact.
 *
 * 3. CopyReadLine() 逐行解析 input_buf 中的数据。
 * 它负责按 COPY 选项考虑引号和转义字符，找到下一个换行标记。
 * 该行被拷贝到 line_buf，引号和转义字符仍保持原样。
 *
 * 4. CopyReadAttributesText/CSV() function takes the input line from
 *    'line_buf', and splits it into fields, unescaping the data as required.
 *    The fields are stored in 'attribute_buf', and 'raw_fields' array holds
 *    pointers to each field.
 *
 * 4. CopyReadAttributesText/CSV() 从 line_buf 取出输入行，按需去转义并拆成字段。
 * 字段存放在 attribute_buf 中，raw_fields 数组保存指向每个字段的指针。
 *
 * If encoding conversion is not required, a shortcut is taken in step 2 to
 * avoid copying the data unnecessarily.  The 'input_buf' pointer is set to
 * point directly to 'raw_buf', so that CopyLoadRawBuf() loads the raw data
 * directly into 'input_buf'.  CopyConvertBuf() then merely validates that
 * the data is valid in the current encoding.
 *
 * 若不需要编码转换，第 2 步会走捷径，避免不必要的数据拷贝。
 * input_buf 指针直接指向 raw_buf，这样 CopyLoadRawBuf() 就把原始数据直接装入 input_buf。
 * CopyConvertBuf() 随后只校验数据在当前编码下是否合法。
 *
 * In binary mode, the pipeline is much simpler.  Input is loaded into
 * 'raw_buf', and encoding conversion is done in the datatype-specific
 * receive functions, if required.  'input_buf' and 'line_buf' are not used,
 * but 'attribute_buf' is used as a temporary buffer to hold one attribute's
 * data when it's passed the receive function.
 *
 * binary 模式下流水线简单得多。输入装入 raw_buf，若需要，编码转换在各数据类型的 receive 函数中完成。
 * 不使用 input_buf 和 line_buf，但 attribute_buf 用作临时缓冲区，在把一个属性的数据传给 receive 函数时保存它。
 *
 * 'raw_buf' is always 64 kB in size (RAW_BUF_SIZE).  'input_buf' is also
 * 64 kB (INPUT_BUF_SIZE), if encoding conversion is required.  'line_buf'
 * and 'attribute_buf' are expanded on demand, to hold the longest line
 * encountered so far.
 *
 * raw_buf 的大小始终是 64 kB（RAW_BUF_SIZE）。若需要编码转换，input_buf 也是 64 kB（INPUT_BUF_SIZE）。
 * line_buf 与 attribute_buf 按需扩展，以容纳迄今遇到的最长行。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/copyfromparse.c
 *
 * 标识
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>

#include "commands/copyapi.h"
#include "commands/copyfrom_internal.h"
#include "commands/progress.h"
#include "executor/executor.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "port/pg_bswap.h"
#include "utils/builtins.h"
#include "utils/rel.h"

#define ISOCTAL(c) (((c) >= '0') && ((c) <= '7'))
#define OCTVALUE(c) ((c) - '0')

/*
 * These macros centralize code used to process line_buf and input_buf buffers.
 * They are macros because they often do continue/break control and to avoid
 * function call overhead in tight COPY loops.
 *
 * 这些宏集中了处理 line_buf 与 input_buf 的代码。
 * 它们是宏，因为经常要做 continue/break 控制，并且要避免在紧凑的 COPY 循环中产生函数调用开销。
 *
 * We must use "if (1)" because the usual "do {...} while(0)" wrapper would
 * prevent the continue/break processing from working.  We end the "if (1)"
 * with "else ((void) 0)" to ensure the "if" does not unintentionally match
 * any "else" in the calling code, and to avoid any compiler warnings about
 * empty statements.  See http://www.cit.gu.edu.au/~anthony/info/C/C.macros.
 *
 * 必须使用 if (1)，因为常见的 do {...} while(0) 包装会让 continue/break 无法工作。
 * 在 if (1) 末尾加上 else ((void) 0)，以确保这个 if 不会无意匹配调用处的 else，并避免编译器对空语句发出警告。
 * 参见 http://www.cit.gu.edu.au/~anthony/info/C/C.macros。
 */

/*
 * This keeps the character read at the top of the loop in the buffer
 * even if there is more than one read-ahead.
 *
 * 即使有不止一次预读，也把循环顶部读到的字符留在缓冲区中。
 */
#define IF_NEED_REFILL_AND_NOT_EOF_CONTINUE(extralen) \
if (1) \
{ \
	if (input_buf_ptr + (extralen) >= copy_buf_len && !hit_eof) \
	{ \
		input_buf_ptr = prev_raw_ptr; /* undo fetch */ \
		need_data = true; \
		continue; \
	} \
} else ((void) 0)

/* This consumes the remainder of the buffer and breaks */
/*
 *
 * 这会消耗缓冲区的剩余部分并跳出
 */
#define IF_NEED_REFILL_AND_EOF_BREAK(extralen) \
if (1) \
{ \
	if (input_buf_ptr + (extralen) >= copy_buf_len && hit_eof) \
	{ \
		if (extralen) \
			input_buf_ptr = copy_buf_len; /* consume the partial character */ \
		/* backslash just before EOF, treat as data char */ \
		result = true; \
		break; \
	} \
} else ((void) 0)

/*
 * Transfer any approved data to line_buf; must do this to be sure
 * there is some room in input_buf.
 *
 * 把已确认的数据转移到 line_buf；必须这样做才能确保 input_buf 中有空间。
 */
#define REFILL_LINEBUF \
if (1) \
{ \
	if (input_buf_ptr > cstate->input_buf_index) \
	{ \
		appendBinaryStringInfo(&cstate->line_buf, \
							 cstate->input_buf + cstate->input_buf_index, \
							   input_buf_ptr - cstate->input_buf_index); \
		cstate->input_buf_index = input_buf_ptr; \
	} \
} else ((void) 0)

/* NOTE: there's a copy of this in copyto.c */
/*
 *
 * 注意：copyto.c 中有一份相同代码
 */
static const char BinarySignature[11] = "PGCOPY\n\377\r\n\0";


/* non-export function prototypes */
/*
 *
 * 非导出函数的原型
 */
static bool CopyReadLine(CopyFromState cstate, bool is_csv);
static bool CopyReadLineText(CopyFromState cstate, bool is_csv);
static int	CopyReadAttributesText(CopyFromState cstate);
static int	CopyReadAttributesCSV(CopyFromState cstate);
static Datum CopyReadBinaryAttribute(CopyFromState cstate, FmgrInfo *flinfo,
									 Oid typioparam, int32 typmod,
									 bool *isnull);
static pg_attribute_always_inline bool CopyFromTextLikeOneRow(CopyFromState cstate,
															  ExprContext *econtext,
															  Datum *values,
															  bool *nulls,
															  bool is_csv);
static pg_attribute_always_inline bool NextCopyFromRawFieldsInternal(CopyFromState cstate,
																	 char ***fields,
																	 int *nfields,
																	 bool is_csv);


/* Low-level communications functions */
/*
 *
 * 底层通信函数
 */
static int	CopyGetData(CopyFromState cstate, void *databuf,
						int minread, int maxread);
static inline bool CopyGetInt32(CopyFromState cstate, int32 *val);
static inline bool CopyGetInt16(CopyFromState cstate, int16 *val);
static void CopyLoadInputBuf(CopyFromState cstate);
static int	CopyReadBinaryData(CopyFromState cstate, char *dest, int nbytes);

/*
 * 向前端发送 CopyInResponse，开始 COPY FROM STDIN。
 */
void
ReceiveCopyBegin(CopyFromState cstate)
{
	StringInfoData buf;
	int			natts = list_length(cstate->attnumlist);
	int16		format = (cstate->opts.binary ? 1 : 0);
	int			i;

	pq_beginmessage(&buf, PqMsg_CopyInResponse);
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
	cstate->copy_src = COPY_FRONTEND;
	cstate->fe_msgbuf = makeStringInfo();
	/* We *must* flush here to ensure FE knows it can send. */
	/*
	 *
	 * 必须在这里刷新，以确保前端知道它可以发送。
	 */
	pq_flush();
}

/*
 * 读取并校验二进制 COPY 文件头中的签名与标志字段。
 */
void
ReceiveCopyBinaryHeader(CopyFromState cstate)
{
	char		readSig[11];
	int32		tmp;

	/* Signature */
	/*
	 *
	 * 签名
	 */
	if (CopyReadBinaryData(cstate, readSig, 11) != 11 ||
		memcmp(readSig, BinarySignature, 11) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("COPY file signature not recognized")));
	/* Flags field */
	/*
	 *
	 * 标志字段
	 */
	if (!CopyGetInt32(cstate, &tmp))
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("invalid COPY file header (missing flags)")));
	if ((tmp & (1 << 16)) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("invalid COPY file header (WITH OIDS)")));
	tmp &= ~(1 << 16);
	if ((tmp >> 16) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("unrecognized critical flags in COPY file header")));
	/* Header extension length */
	/*
	 *
	 * 头扩展长度
	 */
	if (!CopyGetInt32(cstate, &tmp) ||
		tmp < 0)
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("invalid COPY file header (missing length)")));
	/* Skip extension header, if present */
	/*
	 *
	 * 若存在扩展头则跳过
	 */
	while (tmp-- > 0)
	{
		if (CopyReadBinaryData(cstate, readSig, 1) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
					 errmsg("invalid COPY file header (wrong length)")));
	}
}

/*
 * CopyGetData reads data from the source (file or frontend)
 *
 * CopyGetData 从数据源（文件或前端）读取数据
 *
 * We attempt to read at least minread, and at most maxread, bytes from
 * the source.  The actual number of bytes read is returned; if this is
 * less than minread, EOF was detected.
 *
 * 尝试从数据源至少读取 minread 字节、至多 maxread 字节。
 * 返回实际读到的字节数；若少于 minread，则检测到了 EOF。
 *
 * Note: when copying from the frontend, we expect a proper EOF mark per
 * protocol; if the frontend simply drops the connection, we raise error.
 * It seems unwise to allow the COPY IN to complete normally in that case.
 *
 * 注意：从前端拷贝时，我们按协议期待一个正规的 EOF 标记；若前端直接断开连接，则报错。
 * 那种情况下让 COPY IN 正常完成似乎并不明智。
 *
 * NB: no data conversion is applied here.
 *
 * 注意：这里不做数据转换。
 */
static int
CopyGetData(CopyFromState cstate, void *databuf, int minread, int maxread)
{
	int			bytesread = 0;

	switch (cstate->copy_src)
	{
		case COPY_FILE:
			bytesread = fread(databuf, 1, maxread, cstate->copy_file);
			if (ferror(cstate->copy_file))
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not read from COPY file: %m")));
			if (bytesread == 0)
				cstate->raw_reached_eof = true;
			break;
		case COPY_FRONTEND:
			while (maxread > 0 && bytesread < minread && !cstate->raw_reached_eof)
			{
				int			avail;

				while (cstate->fe_msgbuf->cursor >= cstate->fe_msgbuf->len)
				{
					/* Try to receive another message */
					/*
					 *
					 * 尝试再接收一条消息
					 */
					int			mtype;
					int			maxmsglen;

			readmessage:
					HOLD_CANCEL_INTERRUPTS();
					pq_startmsgread();
					mtype = pq_getbyte();
					if (mtype == EOF)
						ereport(ERROR,
								(errcode(ERRCODE_CONNECTION_FAILURE),
								 errmsg("unexpected EOF on client connection with an open transaction")));
					/* Validate message type and set packet size limit */
					/*
					 *
					 * 校验消息类型并设置包大小上限
					 */
					switch (mtype)
					{
						case PqMsg_CopyData:
							maxmsglen = PQ_LARGE_MESSAGE_LIMIT;
							break;
						case PqMsg_CopyDone:
						case PqMsg_CopyFail:
						case PqMsg_Flush:
						case PqMsg_Sync:
							maxmsglen = PQ_SMALL_MESSAGE_LIMIT;
							break;
						default:
							ereport(ERROR,
									(errcode(ERRCODE_PROTOCOL_VIOLATION),
									 errmsg("unexpected message type 0x%02X during COPY from stdin",
											mtype)));
							maxmsglen = 0;	/* keep compiler quiet */
							/*
							 *
							 * 避免编译器告警
							 */
							break;
					}
					/* Now collect the message body */
					/*
					 *
					 * 现在收集消息体
					 */
					if (pq_getmessage(cstate->fe_msgbuf, maxmsglen))
						ereport(ERROR,
								(errcode(ERRCODE_CONNECTION_FAILURE),
								 errmsg("unexpected EOF on client connection with an open transaction")));
					RESUME_CANCEL_INTERRUPTS();
					/* ... and process it */
					/*
					 *
					 * ……并处理它
					 */
					switch (mtype)
					{
						case PqMsg_CopyData:
							break;
						case PqMsg_CopyDone:
							/* COPY IN correctly terminated by frontend */
							/*
							 *
							 * COPY IN 被前端正确终止
							 */
							cstate->raw_reached_eof = true;
							return bytesread;
						case PqMsg_CopyFail:
							ereport(ERROR,
									(errcode(ERRCODE_QUERY_CANCELED),
									 errmsg("COPY from stdin failed: %s",
											pq_getmsgstring(cstate->fe_msgbuf))));
							break;
						case PqMsg_Flush:
						case PqMsg_Sync:

							/*
							 * Ignore Flush/Sync for the convenience of client
							 * libraries (such as libpq) that may send those
							 * without noticing that the command they just
							 * sent was COPY.
							 *
							 * 为了方便客户端库（如 libpq），忽略 Flush/Sync。它们可能在没注意到刚发送的命令是 COPY 时发出这些消息。
							 */
							goto readmessage;
						default:
							Assert(false);	/* NOT REACHED */
							/*
							 *
							 * 不会到达
							 */
					}
				}
				avail = cstate->fe_msgbuf->len - cstate->fe_msgbuf->cursor;
				if (avail > maxread)
					avail = maxread;
				pq_copymsgbytes(cstate->fe_msgbuf, databuf, avail);
				databuf = (void *) ((char *) databuf + avail);
				maxread -= avail;
				bytesread += avail;
			}
			break;
		case COPY_CALLBACK:
			bytesread = cstate->data_source_cb(databuf, minread, maxread);
			break;
	}

	return bytesread;
}


/*
 * These functions do apply some data conversion
 *
 * 这些函数会做一些数据转换
 */

/*
 * CopyGetInt32 reads an int32 that appears in network byte order
 *
 * CopyGetInt32 读取以网络字节序出现的 int32
 *
 * Returns true if OK, false if EOF
 *
 * 成功返回 true，遇到 EOF 返回 false
 */
static inline bool
CopyGetInt32(CopyFromState cstate, int32 *val)
{
	uint32		buf;

	if (CopyReadBinaryData(cstate, (char *) &buf, sizeof(buf)) != sizeof(buf))
	{
		*val = 0;				/* suppress compiler warning */
		/*
		 *
		 * 抑制编译器警告
		 */
		return false;
	}
	*val = (int32) pg_ntoh32(buf);
	return true;
}

/*
 * CopyGetInt16 reads an int16 that appears in network byte order
 *
 * CopyGetInt16 读取以网络字节序出现的 int16
 */
static inline bool
CopyGetInt16(CopyFromState cstate, int16 *val)
{
	uint16		buf;

	if (CopyReadBinaryData(cstate, (char *) &buf, sizeof(buf)) != sizeof(buf))
	{
		*val = 0;				/* suppress compiler warning */
		/*
		 *
		 * 抑制编译器警告
		 */
		return false;
	}
	*val = (int16) pg_ntoh16(buf);
	return true;
}


/*
 * Perform encoding conversion on data in 'raw_buf', writing the converted
 * data into 'input_buf'.
 *
 * 对 raw_buf 中的数据做编码转换，把转换结果写入 input_buf。
 *
 * On entry, there must be some data to convert in 'raw_buf'.
 *
 * 进入时，raw_buf 中必须有一些待转换的数据。
 */
static void
CopyConvertBuf(CopyFromState cstate)
{
	/*
	 * If the file and server encoding are the same, no encoding conversion is
	 * required.  However, we still need to verify that the input is valid for
	 * the encoding.
	 *
	 * 若文件编码与服务器编码相同，则不需要编码转换。但仍需校验输入对该编码是否合法。
	 */
	if (!cstate->need_transcoding)
	{
		/*
		 * When conversion is not required, input_buf and raw_buf are the
		 * same.  raw_buf_len is the total number of bytes in the buffer, and
		 * input_buf_len tracks how many of those bytes have already been
		 * verified.
		 *
		 * 不需要转换时，input_buf 与 raw_buf 是同一块。
		 * raw_buf_len 是缓冲区中的总字节数，input_buf_len 记录其中已校验的字节数。
		 */
		int			preverifiedlen = cstate->input_buf_len;
		int			unverifiedlen = cstate->raw_buf_len - cstate->input_buf_len;
		int			nverified;

		if (unverifiedlen == 0)
		{
			/*
			 * If no more raw data is coming, report the EOF to the caller.
			 *
			 * 若不会再有原始数据，则向调用方报告 EOF。
			 */
			if (cstate->raw_reached_eof)
				cstate->input_reached_eof = true;
			return;
		}

		/*
		 * Verify the new data, including any residual unverified bytes from
		 * previous round.
		 *
		 * 校验新数据，包括上一轮残留的尚未校验的字节。
		 */
		nverified = pg_encoding_verifymbstr(cstate->file_encoding,
											cstate->raw_buf + preverifiedlen,
											unverifiedlen);
		if (nverified == 0)
		{
			/*
			 * Could not verify anything.
			 *
			 * 什么都校验不了。
			 *
			 * If there is no more raw input data coming, it means that there
			 * was an incomplete multi-byte sequence at the end.  Also, if
			 * there's "enough" input left, we should be able to verify at
			 * least one character, and a failure to do so means that we've
			 * hit an invalid byte sequence.
			 *
			 * 若不会再有原始输入数据，说明结尾处有不完整的多字节序列。
			 * 另外，若还剩下“足够”的输入，至少应能校验一个字符，失败则说明遇到了非法字节序列。
			 */
			if (cstate->raw_reached_eof || unverifiedlen >= pg_encoding_max_length(cstate->file_encoding))
				cstate->input_reached_error = true;
			return;
		}
		cstate->input_buf_len += nverified;
	}
	else
	{
		/*
		 * Encoding conversion is needed.
		 *
		 * 需要编码转换。
		 */
		int			nbytes;
		unsigned char *src;
		int			srclen;
		unsigned char *dst;
		int			dstlen;
		int			convertedlen;

		if (RAW_BUF_BYTES(cstate) == 0)
		{
			/*
			 * If no more raw data is coming, report the EOF to the caller.
			 *
			 * 若不会再有原始数据，则向调用方报告 EOF。
			 */
			if (cstate->raw_reached_eof)
				cstate->input_reached_eof = true;
			return;
		}

		/*
		 * First, copy down any unprocessed data.
		 *
		 * 首先，把任何未处理的数据向下拷贝。
		 */
		nbytes = INPUT_BUF_BYTES(cstate);
		if (nbytes > 0 && cstate->input_buf_index > 0)
			memmove(cstate->input_buf, cstate->input_buf + cstate->input_buf_index,
					nbytes);
		cstate->input_buf_index = 0;
		cstate->input_buf_len = nbytes;
		cstate->input_buf[nbytes] = '\0';

		src = (unsigned char *) cstate->raw_buf + cstate->raw_buf_index;
		srclen = cstate->raw_buf_len - cstate->raw_buf_index;
		dst = (unsigned char *) cstate->input_buf + cstate->input_buf_len;
		dstlen = INPUT_BUF_SIZE - cstate->input_buf_len + 1;

		/*
		 * Do the conversion.  This might stop short, if there is an invalid
		 * byte sequence in the input.  We'll convert as much as we can in
		 * that case.
		 *
		 * 执行转换。若输入中有非法字节序列，转换可能会提前停止。那种情况下我们会尽可能多地转换。
		 *
		 * Note: Even if we hit an invalid byte sequence, we don't report the
		 * error until all the valid bytes have been consumed.  The input
		 * might contain an end-of-input marker (\.), and we don't want to
		 * report an error if the invalid byte sequence is after the
		 * end-of-input marker.  We might unnecessarily convert some data
		 * after the end-of-input marker as long as it's valid for the
		 * encoding, but that's harmless.
		 *
		 * 注意：即使遇到非法字节序列，也要等所有合法字节都被消耗之后才报告错误。
		 * 输入中可能包含输入结束标记（\.），若非法字节序列在该标记之后，我们不想报错。
		 * 只要数据对该编码合法，结束标记之后的一些数据可能会被多余地转换，但这无害。
		 */
		convertedlen = pg_do_encoding_conversion_buf(cstate->conversion_proc,
													 cstate->file_encoding,
													 GetDatabaseEncoding(),
													 src, srclen,
													 dst, dstlen,
													 true);
		if (convertedlen == 0)
		{
			/*
			 * Could not convert anything.  If there is no more raw input data
			 * coming, it means that there was an incomplete multi-byte
			 * sequence at the end.  Also, if there is plenty of input left,
			 * we should be able to convert at least one character, so a
			 * failure to do so must mean that we've hit a byte sequence
			 * that's invalid.
			 *
			 * 什么都转换不了。若不会再有原始输入数据，说明结尾处有不完整的多字节序列。
			 * 另外，若还剩下足够多的输入，至少应能转换一个字符，失败则说明遇到了非法字节序列。
			 */
			if (cstate->raw_reached_eof || srclen >= MAX_CONVERSION_INPUT_LENGTH)
				cstate->input_reached_error = true;
			return;
		}
		cstate->raw_buf_index += convertedlen;
		cstate->input_buf_len += strlen((char *) dst);
	}
}

/*
 * Report an encoding or conversion error.
 *
 * 报告编码或转换错误。
 */
static void
CopyConversionError(CopyFromState cstate)
{
	Assert(cstate->raw_buf_len > 0);
	Assert(cstate->input_reached_error);

	if (!cstate->need_transcoding)
	{
		/*
		 * Everything up to input_buf_len was successfully verified, and
		 * input_buf_len points to the invalid or incomplete character.
		 *
		 * 直到 input_buf_len 的内容都已成功校验，input_buf_len 指向非法或不完整的字符。
		 */
		report_invalid_encoding(cstate->file_encoding,
								cstate->raw_buf + cstate->input_buf_len,
								cstate->raw_buf_len - cstate->input_buf_len);
	}
	else
	{
		/*
		 * raw_buf_index points to the invalid or untranslatable character. We
		 * let the conversion routine report the error, because it can provide
		 * a more specific error message than we could here.  An earlier call
		 * to the conversion routine in CopyConvertBuf() detected that there
		 * is an error, now we call the conversion routine again with
		 * noError=false, to have it throw the error.
		 *
		 * raw_buf_index 指向非法或无法翻译的字符。我们让转换例程报告错误，因为它能给出比这里更具体的错误信息。
		 * CopyConvertBuf() 中早先对转换例程的调用已经检测到错误，现在以 noError = false 再次调用，让它抛出错误。
		 */
		unsigned char *src;
		int			srclen;
		unsigned char *dst;
		int			dstlen;

		src = (unsigned char *) cstate->raw_buf + cstate->raw_buf_index;
		srclen = cstate->raw_buf_len - cstate->raw_buf_index;
		dst = (unsigned char *) cstate->input_buf + cstate->input_buf_len;
		dstlen = INPUT_BUF_SIZE - cstate->input_buf_len + 1;

		(void) pg_do_encoding_conversion_buf(cstate->conversion_proc,
											 cstate->file_encoding,
											 GetDatabaseEncoding(),
											 src, srclen,
											 dst, dstlen,
											 false);

		/*
		 * The conversion routine should have reported an error, so this
		 * should not be reached.
		 *
		 * 转换例程应当已经报告了错误，因此不应到达这里。
		 */
		elog(ERROR, "encoding conversion failed without error");
	}
}

/*
 * Load more data from data source to raw_buf.
 *
 * 从数据源向 raw_buf 装载更多数据。
 *
 * If RAW_BUF_BYTES(cstate) > 0, the unprocessed bytes are moved to the
 * beginning of the buffer, and we load new data after that.
 *
 * 若 RAW_BUF_BYTES(cstate) > 0，则把未处理的字节移到缓冲区开头，然后在其后装载新数据。
 */
static void
CopyLoadRawBuf(CopyFromState cstate)
{
	int			nbytes;
	int			inbytes;

	/*
	 * In text mode, if encoding conversion is not required, raw_buf and
	 * input_buf point to the same buffer.  Their len/index better agree, too.
	 *
	 * 在 text 模式下，若不需要编码转换，raw_buf 与 input_buf 指向同一缓冲区。它们的 len/index 也应当一致。
	 */
	if (cstate->raw_buf == cstate->input_buf)
	{
		Assert(!cstate->need_transcoding);
		Assert(cstate->raw_buf_index == cstate->input_buf_index);
		Assert(cstate->input_buf_len <= cstate->raw_buf_len);
	}

	/*
	 * Copy down the unprocessed data if any.
	 *
	 * 若有未处理的数据，则把它向下拷贝。
	 */
	nbytes = RAW_BUF_BYTES(cstate);
	if (nbytes > 0 && cstate->raw_buf_index > 0)
		memmove(cstate->raw_buf, cstate->raw_buf + cstate->raw_buf_index,
				nbytes);
	cstate->raw_buf_len -= cstate->raw_buf_index;
	cstate->raw_buf_index = 0;

	/*
	 * If raw_buf and input_buf are in fact the same buffer, adjust the
	 * input_buf variables, too.
	 *
	 * 若 raw_buf 与 input_buf 实际上是同一缓冲区，也调整 input_buf 的变量。
	 */
	if (cstate->raw_buf == cstate->input_buf)
	{
		cstate->input_buf_len -= cstate->input_buf_index;
		cstate->input_buf_index = 0;
	}

	/* Load more data */
	/*
	 *
	 * 装载更多数据
	 */
	inbytes = CopyGetData(cstate, cstate->raw_buf + cstate->raw_buf_len,
						  1, RAW_BUF_SIZE - cstate->raw_buf_len);
	nbytes += inbytes;
	cstate->raw_buf[nbytes] = '\0';
	cstate->raw_buf_len = nbytes;

	cstate->bytes_processed += inbytes;
	pgstat_progress_update_param(PROGRESS_COPY_BYTES_PROCESSED, cstate->bytes_processed);

	if (inbytes == 0)
		cstate->raw_reached_eof = true;
}

/*
 * CopyLoadInputBuf loads some more data into input_buf
 *
 * CopyLoadInputBuf 向 input_buf 再装入一些数据
 *
 * On return, at least one more input character is loaded into
 * input_buf, or input_reached_eof is set.
 *
 * 返回时，input_buf 中至少又装入了一个输入字符，或者设置了 input_reached_eof。
 *
 * If INPUT_BUF_BYTES(cstate) > 0, the unprocessed bytes are moved to the start
 * of the buffer and then we load more data after that.
 *
 * 若 INPUT_BUF_BYTES(cstate) > 0，则把未处理的字节移到缓冲区开头，然后在其后装载更多数据。
 */
static void
CopyLoadInputBuf(CopyFromState cstate)
{
	int			nbytes = INPUT_BUF_BYTES(cstate);

	/*
	 * The caller has updated input_buf_index to indicate how much of the
	 * input has been consumed and isn't needed anymore.  If input_buf is the
	 * same physical area as raw_buf, update raw_buf_index accordingly.
	 *
	 * 调用方已更新 input_buf_index，表示有多少输入已被消耗、不再需要。
	 * 若 input_buf 与 raw_buf 是同一块物理区域，则相应地更新 raw_buf_index。
	 */
	if (cstate->raw_buf == cstate->input_buf)
	{
		Assert(!cstate->need_transcoding);
		Assert(cstate->input_buf_index >= cstate->raw_buf_index);
		cstate->raw_buf_index = cstate->input_buf_index;
	}

	for (;;)
	{
		/* If we now have some unconverted data, try to convert it */
		/*
		 *
		 * 若现在还有一些未转换的数据，则尝试转换它
		 */
		CopyConvertBuf(cstate);

		/* If we now have some more input bytes ready, return them */
		/*
		 *
		 * 若现在又有一些输入字节准备好了，则返回它们
		 */
		if (INPUT_BUF_BYTES(cstate) > nbytes)
			return;

		/*
		 * If we reached an invalid byte sequence, or we're at an incomplete
		 * multi-byte character but there is no more raw input data, report
		 * conversion error.
		 *
		 * 若遇到非法字节序列，或停在不完整的多字节字符上且没有更多原始输入，则报告转换错误。
		 */
		if (cstate->input_reached_error)
			CopyConversionError(cstate);

		/* no more input, and everything has been converted */
		/*
		 *
		 * 没有更多输入，且所有内容都已转换
		 */
		if (cstate->input_reached_eof)
			break;

		/* Try to load more raw data */
		/*
		 *
		 * 尝试装载更多原始数据
		 */
		Assert(!cstate->raw_reached_eof);
		CopyLoadRawBuf(cstate);
	}
}

/*
 * CopyReadBinaryData
 *
 * 读取二进制 COPY 数据
 *
 * Reads up to 'nbytes' bytes from cstate->copy_file via cstate->raw_buf
 * and writes them to 'dest'.  Returns the number of bytes read (which
 * would be less than 'nbytes' only if we reach EOF).
 *
 * 经 cstate->raw_buf 从 cstate->copy_file 最多读取 nbytes 个字节，并写到 dest。
 * 返回读到的字节数（只有到达 EOF 时才会少于 nbytes）。
 */
static int
CopyReadBinaryData(CopyFromState cstate, char *dest, int nbytes)
{
	int			copied_bytes = 0;

	if (RAW_BUF_BYTES(cstate) >= nbytes)
	{
		/* Enough bytes are present in the buffer. */
		/*
		 *
		 * 缓冲区中已有足够的字节。
		 */
		memcpy(dest, cstate->raw_buf + cstate->raw_buf_index, nbytes);
		cstate->raw_buf_index += nbytes;
		copied_bytes = nbytes;
	}
	else
	{
		/*
		 * Not enough bytes in the buffer, so must read from the file.  Need
		 * to loop since 'nbytes' could be larger than the buffer size.
		 *
		 * 缓冲区中的字节不够，因此必须从文件读取。需要循环，因为 nbytes 可能大于缓冲区大小。
		 */
		do
		{
			int			copy_bytes;

			/* Load more data if buffer is empty. */
			/*
			 *
			 * 若缓冲区为空则装载更多数据。
			 */
			if (RAW_BUF_BYTES(cstate) == 0)
			{
				CopyLoadRawBuf(cstate);
				if (cstate->raw_reached_eof)
					break;		/* EOF */
					/*
					 *
					 * 到达文件结束
					 */
			}

			/* Transfer some bytes. */
			/*
			 *
			 * 转移若干字节。
			 */
			copy_bytes = Min(nbytes - copied_bytes, RAW_BUF_BYTES(cstate));
			memcpy(dest, cstate->raw_buf + cstate->raw_buf_index, copy_bytes);
			cstate->raw_buf_index += copy_bytes;
			dest += copy_bytes;
			copied_bytes += copy_bytes;
		} while (copied_bytes < nbytes);
	}

	return copied_bytes;
}

/*
 * This function is exposed for use by extensions that read raw fields in the
 * next line. See NextCopyFromRawFieldsInternal() for details.
 *
 * 本函数开放给读取下一行原始字段的扩展使用。细节见 NextCopyFromRawFieldsInternal()。
 */
bool
NextCopyFromRawFields(CopyFromState cstate, char ***fields, int *nfields)
{
	return NextCopyFromRawFieldsInternal(cstate, fields, nfields,
										 cstate->opts.csv_mode);
}

/*
 * Workhorse for NextCopyFromRawFields().
 *
 * NextCopyFromRawFields() 的主要实现。
 *
 * Read raw fields in the next line for COPY FROM in text or csv mode. Return
 * false if no more lines.
 *
 * 在 text 或 csv 模式下为 COPY FROM 读取下一行的原始字段。若没有更多行则返回 false。
 *
 * An internal temporary buffer is returned via 'fields'. It is valid until
 * the next call of the function. Since the function returns all raw fields
 * in the input file, 'nfields' could be different from the number of columns
 * in the relation.
 *
 * 通过 fields 返回一个内部临时缓冲区。它在下次调用本函数之前有效。
 * 因为函数返回输入文件中的全部原始字段，nfields 可能与关系的列数不同。
 *
 * NOTE: force_not_null option are not applied to the returned fields.
 *
 * 注意：返回的字段不会应用 force_not_null 选项。
 *
 * We use pg_attribute_always_inline to reduce function call overhead
 * and to help compilers to optimize away the 'is_csv' condition when called
 * by internal functions such as CopyFromTextLikeOneRow().
 *
 * 使用 pg_attribute_always_inline 以减少函数调用开销，并帮助编译器在被 CopyFromTextLikeOneRow() 等内部函数调用时优化掉 is_csv 条件。
 */
static pg_attribute_always_inline bool
NextCopyFromRawFieldsInternal(CopyFromState cstate, char ***fields, int *nfields, bool is_csv)
{
	int			fldct;
	bool		done;

	/* only available for text or csv input */
	/*
	 *
	 * 仅适用于 text 或 csv 输入
	 */
	Assert(!cstate->opts.binary);

	/* on input check that the header line is correct if needed */
	/*
	 *
	 * 输入时若需要，则检查标题行是否正确
	 */
	if (cstate->cur_lineno == 0 && cstate->opts.header_line)
	{
		ListCell   *cur;
		TupleDesc	tupDesc;

		tupDesc = RelationGetDescr(cstate->rel);

		cstate->cur_lineno++;
		done = CopyReadLine(cstate, is_csv);

		if (cstate->opts.header_line == COPY_HEADER_MATCH)
		{
			int			fldnum;

			if (is_csv)
				fldct = CopyReadAttributesCSV(cstate);
			else
				fldct = CopyReadAttributesText(cstate);

			if (fldct != list_length(cstate->attnumlist))
				ereport(ERROR,
						(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
						 errmsg("wrong number of fields in header line: got %d, expected %d",
								fldct, list_length(cstate->attnumlist))));

			fldnum = 0;
			foreach(cur, cstate->attnumlist)
			{
				int			attnum = lfirst_int(cur);
				char	   *colName;
				Form_pg_attribute attr = TupleDescAttr(tupDesc, attnum - 1);

				Assert(fldnum < cstate->max_fields);

				colName = cstate->raw_fields[fldnum++];
				if (colName == NULL)
					ereport(ERROR,
							(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
							 errmsg("column name mismatch in header line field %d: got null value (\"%s\"), expected \"%s\"",
									fldnum, cstate->opts.null_print, NameStr(attr->attname))));

				if (namestrcmp(&attr->attname, colName) != 0)
				{
					ereport(ERROR,
							(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
							 errmsg("column name mismatch in header line field %d: got \"%s\", expected \"%s\"",
									fldnum, colName, NameStr(attr->attname))));
				}
			}
		}

		if (done)
			return false;
	}

	cstate->cur_lineno++;

	/* Actually read the line into memory here */
	/*
	 *
	 * 在这里真正把该行读入内存
	 */
	done = CopyReadLine(cstate, is_csv);

	/*
	 * EOF at start of line means we're done.  If we see EOF after some
	 * characters, we act as though it was newline followed by EOF, ie,
	 * process the line and then exit loop on next iteration.
	 *
	 * 行首遇到 EOF 表示已经结束。若在若干字符之后看到 EOF，则当作换行后再跟 EOF，即处理该行，然后在下一次迭代退出循环。
	 */
	if (done && cstate->line_buf.len == 0)
		return false;

	/* Parse the line into de-escaped field values */
	/*
	 *
	 * 把该行解析成去转义后的字段值
	 */
	if (is_csv)
		fldct = CopyReadAttributesCSV(cstate);
	else
		fldct = CopyReadAttributesText(cstate);

	*fields = cstate->raw_fields;
	*nfields = fldct;
	return true;
}

/*
 * 核心流程概览：
 * CopyLoadRawBuf / CopyConvertBuf：把输入读入 raw_buf，并转换到 input_buf。
 * CopyReadLine：从 input_buf 切出一行放入 line_buf。
 * CopyReadAttributesText / CopyReadAttributesCSV：把 line_buf 拆成字段。
 * NextCopyFrom：text、CSV、binary 的统一入口，返回一行 Datum。
 */

/*
 * Read next tuple from file for COPY FROM. Return false if no more tuples.
 *
 * 为 COPY FROM 从文件读取下一个元组。若没有更多元组则返回 false。
 *
 * 'econtext' is used to evaluate default expression for each column that is
 * either not read from the file or is using the DEFAULT option of COPY FROM.
 * It can be NULL when no default values are used, i.e. when all columns are
 * read from the file, and DEFAULT option is unset.
 *
 * econtext 用于计算那些未从文件读取、或使用了 COPY FROM 的 DEFAULT 选项的列的默认表达式。
 * 若不使用默认值，即所有列都从文件读取且未设置 DEFAULT 选项，则可以为 NULL。
 *
 * 'values' and 'nulls' arrays must be the same length as columns of the
 * relation passed to BeginCopyFrom. This function fills the arrays.
 *
 * values 与 nulls 数组的长度必须与传给 BeginCopyFrom 的关系列数相同。本函数填充这两个数组。
 */
bool
NextCopyFrom(CopyFromState cstate, ExprContext *econtext,
			 Datum *values, bool *nulls)
{
	TupleDesc	tupDesc;
	AttrNumber	num_phys_attrs,
				num_defaults = cstate->num_defaults;
	int			i;
	int		   *defmap = cstate->defmap;
	ExprState **defexprs = cstate->defexprs;

	tupDesc = RelationGetDescr(cstate->rel);
	num_phys_attrs = tupDesc->natts;

	/* Initialize all values for row to NULL */
	/*
	 *
	 * 把该行的所有值初始化为 NULL
	 */
	MemSet(values, 0, num_phys_attrs * sizeof(Datum));
	MemSet(nulls, true, num_phys_attrs * sizeof(bool));
	MemSet(cstate->defaults, false, num_phys_attrs * sizeof(bool));

	/* Get one row from source */
	/*
	 *
	 * 从数据源取得一行
	 */
	if (!cstate->routine->CopyFromOneRow(cstate, econtext, values, nulls))
		return false;

	/*
	 * Now compute and insert any defaults available for the columns not
	 * provided by the input data.  Anything not processed here or above will
	 * remain NULL.
	 *
	 * 现在为输入数据未提供的列计算并填入可用的默认值。这里和上面都没处理的列将保持 NULL。
	 */
	for (i = 0; i < num_defaults; i++)
	{
		/*
		 * The caller must supply econtext and have switched into the
		 * per-tuple memory context in it.
		 *
		 * 调用方必须提供 econtext，并且已经切换到其中的每元组内存上下文。
		 */
		Assert(econtext != NULL);
		Assert(CurrentMemoryContext == econtext->ecxt_per_tuple_memory);

		values[defmap[i]] = ExecEvalExpr(defexprs[defmap[i]], econtext,
										 &nulls[defmap[i]]);
	}

	return true;
}

/* Implementation of the per-row callback for text format */
/*
 *
 * text 格式的逐行回调实现
 */
bool
CopyFromTextOneRow(CopyFromState cstate, ExprContext *econtext, Datum *values,
				   bool *nulls)
{
	return CopyFromTextLikeOneRow(cstate, econtext, values, nulls, false);
}

/* Implementation of the per-row callback for CSV format */
/*
 *
 * CSV 格式的逐行回调实现
 */
bool
CopyFromCSVOneRow(CopyFromState cstate, ExprContext *econtext, Datum *values,
				  bool *nulls)
{
	return CopyFromTextLikeOneRow(cstate, econtext, values, nulls, true);
}

/*
 * Workhorse for CopyFromTextOneRow() and CopyFromCSVOneRow().
 *
 * CopyFromTextOneRow() 与 CopyFromCSVOneRow() 的主要实现。
 *
 * We use pg_attribute_always_inline to reduce function call overhead
 * and to help compilers to optimize away the 'is_csv' condition.
 *
 * 使用 pg_attribute_always_inline 以减少函数调用开销，并帮助编译器优化掉 is_csv 条件。
 */
static pg_attribute_always_inline bool
CopyFromTextLikeOneRow(CopyFromState cstate, ExprContext *econtext,
					   Datum *values, bool *nulls, bool is_csv)
{
	TupleDesc	tupDesc;
	AttrNumber	attr_count;
	FmgrInfo   *in_functions = cstate->in_functions;
	Oid		   *typioparams = cstate->typioparams;
	ExprState **defexprs = cstate->defexprs;
	char	  **field_strings;
	ListCell   *cur;
	int			fldct;
	int			fieldno;
	char	   *string;

	tupDesc = RelationGetDescr(cstate->rel);
	attr_count = list_length(cstate->attnumlist);

	/* read raw fields in the next line */
	/*
	 *
	 * 读取下一行中的原始字段
	 */
	if (!NextCopyFromRawFieldsInternal(cstate, &field_strings, &fldct, is_csv))
		return false;

	/* check for overflowing fields */
	/*
	 *
	 * 检查字段是否溢出
	 */
	if (attr_count > 0 && fldct > attr_count)
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("extra data after last expected column")));

	fieldno = 0;

	/* Loop to read the user attributes on the line. */
	/*
	 *
	 * 循环读取该行上的用户属性。
	 */
	foreach(cur, cstate->attnumlist)
	{
		int			attnum = lfirst_int(cur);
		int			m = attnum - 1;
		Form_pg_attribute att = TupleDescAttr(tupDesc, m);

		if (fieldno >= fldct)
			ereport(ERROR,
					(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
					 errmsg("missing data for column \"%s\"",
							NameStr(att->attname))));
		string = field_strings[fieldno++];

		if (cstate->convert_select_flags &&
			!cstate->convert_select_flags[m])
		{
			/* ignore input field, leaving column as NULL */
			/*
			 *
			 * 忽略输入字段，把该列留为 NULL
			 */
			continue;
		}

		if (is_csv)
		{
			if (string == NULL &&
				cstate->opts.force_notnull_flags[m])
			{
				/*
				 * FORCE_NOT_NULL option is set and column is NULL - convert
				 * it to the NULL string.
				 *
				 * 设置了 FORCE_NOT_NULL 且该列为 NULL，则把它转换成 NULL 字符串。
				 */
				string = cstate->opts.null_print;
			}
			else if (string != NULL && cstate->opts.force_null_flags[m]
					 && strcmp(string, cstate->opts.null_print) == 0)
			{
				/*
				 * FORCE_NULL option is set and column matches the NULL
				 * string. It must have been quoted, or otherwise the string
				 * would already have been set to NULL. Convert it to NULL as
				 * specified.
				 *
				 * 设置了 FORCE_NULL 且该列匹配 NULL 字符串。它一定加过引号，否则字符串早就被设成 NULL。按指定把它转换成 NULL。
				 */
				string = NULL;
			}
		}

		cstate->cur_attname = NameStr(att->attname);
		cstate->cur_attval = string;

		if (string != NULL)
			nulls[m] = false;

		if (cstate->defaults[m])
		{
			/* We must have switched into the per-tuple memory context */
			/*
			 *
			 * 必须已经切换到每元组内存上下文
			 */
			Assert(econtext != NULL);
			Assert(CurrentMemoryContext == econtext->ecxt_per_tuple_memory);

			values[m] = ExecEvalExpr(defexprs[m], econtext, &nulls[m]);
		}

		/*
		 * If ON_ERROR is specified with IGNORE, skip rows with soft errors
		 *
		 * 若指定了 ON_ERROR 且为 IGNORE，则跳过带软错误的行
		 */
		else if (!InputFunctionCallSafe(&in_functions[m],
										string,
										typioparams[m],
										att->atttypmod,
										(Node *) cstate->escontext,
										&values[m]))
		{
			Assert(cstate->opts.on_error != COPY_ON_ERROR_STOP);

			cstate->num_errors++;

			if (cstate->opts.log_verbosity == COPY_LOG_VERBOSITY_VERBOSE)
			{
				/*
				 * Since we emit line number and column info in the below
				 * notice message, we suppress error context information other
				 * than the relation name.
				 *
				 * 因为下面的提示信息会给出行号和列信息，所以除关系名外抑制其他错误上下文信息。
				 */
				Assert(!cstate->relname_only);
				cstate->relname_only = true;

				if (cstate->cur_attval)
				{
					char	   *attval;

					attval = CopyLimitPrintoutLength(cstate->cur_attval);
					ereport(NOTICE,
							errmsg("skipping row due to data type incompatibility at line %" PRIu64 " for column \"%s\": \"%s\"",
								   cstate->cur_lineno,
								   cstate->cur_attname,
								   attval));
					pfree(attval);
				}
				else
					ereport(NOTICE,
							errmsg("skipping row due to data type incompatibility at line %" PRIu64 " for column \"%s\": null input",
								   cstate->cur_lineno,
								   cstate->cur_attname));

				/* reset relname_only */
				/*
				 *
				 * 重置 relname_only
				 */
				cstate->relname_only = false;
			}

			return true;
		}

		cstate->cur_attname = NULL;
		cstate->cur_attval = NULL;
	}

	Assert(fieldno == attr_count);

	return true;
}

/* Implementation of the per-row callback for binary format */
/*
 *
 * binary 格式的逐行回调实现
 */
bool
CopyFromBinaryOneRow(CopyFromState cstate, ExprContext *econtext, Datum *values,
					 bool *nulls)
{
	TupleDesc	tupDesc;
	AttrNumber	attr_count;
	FmgrInfo   *in_functions = cstate->in_functions;
	Oid		   *typioparams = cstate->typioparams;
	int16		fld_count;
	ListCell   *cur;

	tupDesc = RelationGetDescr(cstate->rel);
	attr_count = list_length(cstate->attnumlist);

	cstate->cur_lineno++;

	if (!CopyGetInt16(cstate, &fld_count))
	{
		/* EOF detected (end of file, or protocol-level EOF) */
		/*
		 *
		 * 检测到 EOF（文件结束，或协议级 EOF）
		 */
		return false;
	}

	if (fld_count == -1)
	{
		/*
		 * Received EOF marker.  Wait for the protocol-level EOF, and complain
		 * if it doesn't come immediately.  In COPY FROM STDIN, this ensures
		 * that we correctly handle CopyFail, if client chooses to send that
		 * now.  When copying from file, we could ignore the rest of the file
		 * like in text mode, but we choose to be consistent with the COPY
		 * FROM STDIN case.
		 *
		 * 收到 EOF 标记。等待协议级 EOF，若它没有立刻到来则报错。
		 * 在 COPY FROM STDIN 中，这保证若客户端此时选择发送 CopyFail，我们能正确处理。
		 * 从文件拷贝时本可以像 text 模式那样忽略文件其余部分，但我们选择与 COPY FROM STDIN 保持一致。
		 */
		char		dummy;

		if (CopyReadBinaryData(cstate, &dummy, 1) > 0)
			ereport(ERROR,
					(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
					 errmsg("received copy data after EOF marker")));
		return false;
	}

	if (fld_count != attr_count)
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("row field count is %d, expected %d",
						(int) fld_count, attr_count)));

	foreach(cur, cstate->attnumlist)
	{
		int			attnum = lfirst_int(cur);
		int			m = attnum - 1;
		Form_pg_attribute att = TupleDescAttr(tupDesc, m);

		cstate->cur_attname = NameStr(att->attname);
		values[m] = CopyReadBinaryAttribute(cstate,
											&in_functions[m],
											typioparams[m],
											att->atttypmod,
											&nulls[m]);
		cstate->cur_attname = NULL;
	}

	return true;
}

/*
 * Read the next input line and stash it in line_buf.
 *
 * 读取下一行输入并存入 line_buf。
 *
 * Result is true if read was terminated by EOF, false if terminated
 * by newline.  The terminating newline or EOF marker is not included
 * in the final value of line_buf.
 *
 * 若读取因 EOF 结束则结果为 true，若因换行结束则为 false。
 * 终止用的换行或 EOF 标记不包含在 line_buf 的最终值中。
 */
static bool
CopyReadLine(CopyFromState cstate, bool is_csv)
{
	bool		result;

	resetStringInfo(&cstate->line_buf);
	cstate->line_buf_valid = false;

	/* Parse data and transfer into line_buf */
	/*
	 *
	 * 解析数据并转移到 line_buf
	 */
	result = CopyReadLineText(cstate, is_csv);

	if (result)
	{
		/*
		 * Reached EOF.  In protocol version 3, we should ignore anything
		 * after \. up to the protocol end of copy data.  (XXX maybe better
		 * not to treat \. as special?)
		 *
		 * 到达 EOF。在协议版本 3 中，应忽略 \. 之后直到协议级拷贝数据结束的任何内容。
		 * （XXX：也许最好不要把 \. 当作特殊标记？）
		 */
		if (cstate->copy_src == COPY_FRONTEND)
		{
			int			inbytes;

			do
			{
				inbytes = CopyGetData(cstate, cstate->input_buf,
									  1, INPUT_BUF_SIZE);
			} while (inbytes > 0);
			cstate->input_buf_index = 0;
			cstate->input_buf_len = 0;
			cstate->raw_buf_index = 0;
			cstate->raw_buf_len = 0;
		}
	}
	else
	{
		/*
		 * If we didn't hit EOF, then we must have transferred the EOL marker
		 * to line_buf along with the data.  Get rid of it.
		 *
		 * 若没有遇到 EOF，则一定已经把 EOL 标记连同数据一起转移到了 line_buf。把它去掉。
		 */
		switch (cstate->eol_type)
		{
			case EOL_NL:
				Assert(cstate->line_buf.len >= 1);
				Assert(cstate->line_buf.data[cstate->line_buf.len - 1] == '\n');
				cstate->line_buf.len--;
				cstate->line_buf.data[cstate->line_buf.len] = '\0';
				break;
			case EOL_CR:
				Assert(cstate->line_buf.len >= 1);
				Assert(cstate->line_buf.data[cstate->line_buf.len - 1] == '\r');
				cstate->line_buf.len--;
				cstate->line_buf.data[cstate->line_buf.len] = '\0';
				break;
			case EOL_CRNL:
				Assert(cstate->line_buf.len >= 2);
				Assert(cstate->line_buf.data[cstate->line_buf.len - 2] == '\r');
				Assert(cstate->line_buf.data[cstate->line_buf.len - 1] == '\n');
				cstate->line_buf.len -= 2;
				cstate->line_buf.data[cstate->line_buf.len] = '\0';
				break;
			case EOL_UNKNOWN:
				/* shouldn't get here */
				/*
				 *
				 * 不应到达这里
				 */
				Assert(false);
				break;
		}
	}

	/* Now it's safe to use the buffer in error messages */
	/*
	 *
	 * 现在可以安全地在错误信息中使用该缓冲区
	 */
	cstate->line_buf_valid = true;

	return result;
}

/*
 * CopyReadLineText - inner loop of CopyReadLine for text mode
 *
 * CopyReadLineText：text 模式下 CopyReadLine 的内层循环
 */
static bool
CopyReadLineText(CopyFromState cstate, bool is_csv)
{
	char	   *copy_input_buf;
	int			input_buf_ptr;
	int			copy_buf_len;
	bool		need_data = false;
	bool		hit_eof = false;
	bool		result = false;

	/* CSV variables */
	/*
	 *
	 * CSV 变量
	 */
	bool		in_quote = false,
				last_was_esc = false;
	char		quotec = '\0';
	char		escapec = '\0';

	if (is_csv)
	{
		quotec = cstate->opts.quote[0];
		escapec = cstate->opts.escape[0];
		/* ignore special escape processing if it's the same as quotec */
		/*
		 *
		 * 若转义字符与 quotec 相同，则忽略特殊转义处理
		 */
		if (quotec == escapec)
			escapec = '\0';
	}

	/*
	 * The objective of this loop is to transfer the entire next input line
	 * into line_buf.  Hence, we only care for detecting newlines (\r and/or
	 * \n) and the end-of-copy marker (\.).
	 *
	 * 这个循环的目标是把下一整行输入转移到 line_buf。
	 * 因此我们只关心检测换行（\r 和/或 \n）以及拷贝结束标记（\.）。
	 *
	 * In CSV mode, \r and \n inside a quoted field are just part of the data
	 * value and are put in line_buf.  We keep just enough state to know if we
	 * are currently in a quoted field or not.
	 *
	 * 在 CSV 模式下，引号字段内的 \r 和 \n 只是数据值的一部分，会放进 line_buf。
	 * 我们只保留足够的状态，以知道当前是否处于引号字段内。
	 *
	 * The input has already been converted to the database encoding.  All
	 * supported server encodings have the property that all bytes in a
	 * multi-byte sequence have the high bit set, so a multibyte character
	 * cannot contain any newline or escape characters embedded in the
	 * multibyte sequence.  Therefore, we can process the input byte-by-byte,
	 * regardless of the encoding.
	 *
	 * 输入已经转换成数据库编码。所有支持的服务器编码都有这样的性质：多字节序列中的每个字节都设置了高位，
	 * 因此多字节字符的字节序列内部不可能嵌入换行或转义字符。所以无论编码如何，都可以逐字节处理输入。
	 *
	 * For speed, we try to move data from input_buf to line_buf in chunks
	 * rather than one character at a time.  input_buf_ptr points to the next
	 * character to examine; any characters from input_buf_index to
	 * input_buf_ptr have been determined to be part of the line, but not yet
	 * transferred to line_buf.
	 *
	 * 为了速度，我们尽量按块而不是逐字符把数据从 input_buf 搬到 line_buf。
	 * input_buf_ptr 指向下一个要检查的字符；从 input_buf_index 到 input_buf_ptr 的字符已确定属于本行，但尚未转移到 line_buf。
	 *
	 * For a little extra speed within the loop, we copy input_buf and
	 * input_buf_len into local variables.
	 *
	 * 为了让循环里再快一点，把 input_buf 和 input_buf_len 复制到局部变量。
	 */
	copy_input_buf = cstate->input_buf;
	input_buf_ptr = cstate->input_buf_index;
	copy_buf_len = cstate->input_buf_len;

	for (;;)
	{
		int			prev_raw_ptr;
		char		c;

		/*
		 * Load more data if needed.
		 *
		 * 若需要则装载更多数据。
		 *
		 * TODO: We could just force four bytes of read-ahead and avoid the
		 * many calls to IF_NEED_REFILL_AND_NOT_EOF_CONTINUE().  That was
		 * unsafe with the old v2 COPY protocol, but we don't support that
		 * anymore.
		 *
		 * TODO：可以强制预读四个字节，从而避免多次调用 IF_NEED_REFILL_AND_NOT_EOF_CONTINUE()。
		 * 旧的 v2 COPY 协议下这样做不安全，但我们已不再支持该协议。
		 */
		if (input_buf_ptr >= copy_buf_len || need_data)
		{
			REFILL_LINEBUF;

			CopyLoadInputBuf(cstate);
			/* update our local variables */
			/*
			 *
			 * 更新局部变量
			 */
			hit_eof = cstate->input_reached_eof;
			input_buf_ptr = cstate->input_buf_index;
			copy_buf_len = cstate->input_buf_len;

			/*
			 * If we are completely out of data, break out of the loop,
			 * reporting EOF.
			 *
			 * 若数据已经完全用尽，则跳出循环并报告 EOF。
			 */
			if (INPUT_BUF_BYTES(cstate) <= 0)
			{
				result = true;
				break;
			}
			need_data = false;
		}

		/* OK to fetch a character */
		/*
		 *
		 * 可以取一个字符
		 */
		prev_raw_ptr = input_buf_ptr;
		c = copy_input_buf[input_buf_ptr++];

		if (is_csv)
		{
			/*
			 * If character is '\r', we may need to look ahead below.  Force
			 * fetch of the next character if we don't already have it.  We
			 * need to do this before changing CSV state, in case '\r' is also
			 * the quote or escape character.
			 *
			 * 若字符是 '\r'，下面可能需要向前看。若还没有下一个字符，则强制取它。
			 * 必须在改变 CSV 状态之前做这件事，以防 '\r' 同时也是引号或转义字符。
			 */
			if (c == '\r')
			{
				IF_NEED_REFILL_AND_NOT_EOF_CONTINUE(0);
			}

			/*
			 * Dealing with quotes and escapes here is mildly tricky. If the
			 * quote char is also the escape char, there's no problem - we
			 * just use the char as a toggle. If they are different, we need
			 * to ensure that we only take account of an escape inside a
			 * quoted field and immediately preceding a quote char, and not
			 * the second in an escape-escape sequence.
			 *
			 * 这里处理引号和转义有点棘手。若引号字符同时也是转义字符，则没有问题，只需把它当作开关。
			 * 若两者不同，则必须保证只把引号字段内部、且紧挨在引号字符之前的转义算进去，而不把转义-转义序列中的第二个算进去。
			 */
			if (in_quote && c == escapec)
				last_was_esc = !last_was_esc;
			if (c == quotec && !last_was_esc)
				in_quote = !in_quote;
			if (c != escapec)
				last_was_esc = false;

			/*
			 * Updating the line count for embedded CR and/or LF chars is
			 * necessarily a little fragile - this test is probably about the
			 * best we can do.  (XXX it's arguable whether we should do this
			 * at all --- is cur_lineno a physical or logical count?)
			 *
			 * 为嵌入的 CR 和/或 LF 字符更新行号必然有些脆弱——这个测试大概已经是我们能做的最好办法。
			 * （XXX：到底该不该这样做也有争议——cur_lineno 算的是物理行还是逻辑行？）
			 */
			if (in_quote && c == (cstate->eol_type == EOL_NL ? '\n' : '\r'))
				cstate->cur_lineno++;
		}

		/* Process \r */
		/*
		 *
		 * 处理 \r
		 */
		if (c == '\r' && (!is_csv || !in_quote))
		{
			/* Check for \r\n on first line, _and_ handle \r\n. */
			/*
			 *
			 * 检查第一行是否为 \r\n，并处理 \r\n。
			 */
			if (cstate->eol_type == EOL_UNKNOWN ||
				cstate->eol_type == EOL_CRNL)
			{
				/*
				 * If need more data, go back to loop top to load it.
				 *
				 * 若需要更多数据，回到循环顶部去装载。
				 *
				 * Note that if we are at EOF, c will wind up as '\0' because
				 * of the guaranteed pad of input_buf.
				 *
				 * 注意若处于 EOF，由于 input_buf 保证有填充，c 最终会变成 '\0'。
				 */
				IF_NEED_REFILL_AND_NOT_EOF_CONTINUE(0);

				/* get next char */
				/*
				 *
				 * 取下一个字符
				 */
				c = copy_input_buf[input_buf_ptr];

				if (c == '\n')
				{
					input_buf_ptr++;	/* eat newline */
					/*
					 *
					 * 吃掉换行
					 */
					cstate->eol_type = EOL_CRNL;	/* in case not set yet */
					/*
					 *
					 * 以防尚未设置
					 */
				}
				else
				{
					/* found \r, but no \n */
					/*
					 *
					 * 找到了 \r，但没有 \n
					 */
					if (cstate->eol_type == EOL_CRNL)
						ereport(ERROR,
								(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
								 !is_csv ?
								 errmsg("literal carriage return found in data") :
								 errmsg("unquoted carriage return found in data"),
								 !is_csv ?
								 errhint("Use \"\\r\" to represent carriage return.") :
								 errhint("Use quoted CSV field to represent carriage return.")));

					/*
					 * if we got here, it is the first line and we didn't find
					 * \n, so don't consume the peeked character
					 *
					 * 若到达这里，说明这是第一行且没有找到 \n，因此不要消耗已经窥看的字符
					 */
					cstate->eol_type = EOL_CR;
				}
			}
			else if (cstate->eol_type == EOL_NL)
				ereport(ERROR,
						(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
						 !is_csv ?
						 errmsg("literal carriage return found in data") :
						 errmsg("unquoted carriage return found in data"),
						 !is_csv ?
						 errhint("Use \"\\r\" to represent carriage return.") :
						 errhint("Use quoted CSV field to represent carriage return.")));
			/* If reach here, we have found the line terminator */
			/*
			 *
			 * 若到达这里，说明已经找到行终止符
			 */
			break;
		}

		/* Process \n */
		/*
		 *
		 * 处理 \n
		 */
		if (c == '\n' && (!is_csv || !in_quote))
		{
			if (cstate->eol_type == EOL_CR || cstate->eol_type == EOL_CRNL)
				ereport(ERROR,
						(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
						 !is_csv ?
						 errmsg("literal newline found in data") :
						 errmsg("unquoted newline found in data"),
						 !is_csv ?
						 errhint("Use \"\\n\" to represent newline.") :
						 errhint("Use quoted CSV field to represent newline.")));
			cstate->eol_type = EOL_NL;	/* in case not set yet */
			/*
			 *
			 * 以防尚未设置
			 */
			/* If reach here, we have found the line terminator */
			/*
			 *
			 * 若到达这里，说明已经找到行终止符
			 */
			break;
		}

		/*
		 * Process backslash, except in CSV mode where backslash is a normal
		 * character.
		 *
		 * 处理反斜杠，但在 CSV 模式下反斜杠是普通字符，除外。
		 */
		if (c == '\\' && !is_csv)
		{
			char		c2;

			IF_NEED_REFILL_AND_NOT_EOF_CONTINUE(0);
			IF_NEED_REFILL_AND_EOF_BREAK(0);

			/* -----
			 * get next character
			 * Note: we do not change c so if it isn't \., we can fall
			 * through and continue processing.
			 *
			 * 取下一个字符。
			 * 注意：我们不改变 c，因此若它不是 \.，可以贯穿下去继续处理。
			 * -----
			 */
			c2 = copy_input_buf[input_buf_ptr];

			if (c2 == '.')
			{
				input_buf_ptr++;	/* consume the '.' */
				/*
				 *
				 * 消耗掉 '.'
				 */
				if (cstate->eol_type == EOL_CRNL)
				{
					/* Get the next character */
					/*
					 *
					 * 取下一个字符
					 */
					IF_NEED_REFILL_AND_NOT_EOF_CONTINUE(0);
					/* if hit_eof, c2 will become '\0' */
					/*
					 *
					 * 若 hit_eof，c2 将变成 '\0'
					 */
					c2 = copy_input_buf[input_buf_ptr++];

					if (c2 == '\n')
						ereport(ERROR,
								(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
								 errmsg("end-of-copy marker does not match previous newline style")));
					else if (c2 != '\r')
						ereport(ERROR,
								(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
								 errmsg("end-of-copy marker is not alone on its line")));
				}

				/* Get the next character */
				/*
				 *
				 * 取下一个字符
				 */
				IF_NEED_REFILL_AND_NOT_EOF_CONTINUE(0);
				/* if hit_eof, c2 will become '\0' */
				/*
				 *
				 * 若 hit_eof，c2 将变成 '\0'
				 */
				c2 = copy_input_buf[input_buf_ptr++];

				if (c2 != '\r' && c2 != '\n')
					ereport(ERROR,
							(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
							 errmsg("end-of-copy marker is not alone on its line")));

				if ((cstate->eol_type == EOL_NL && c2 != '\n') ||
					(cstate->eol_type == EOL_CRNL && c2 != '\n') ||
					(cstate->eol_type == EOL_CR && c2 != '\r'))
					ereport(ERROR,
							(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
							 errmsg("end-of-copy marker does not match previous newline style")));

				/*
				 * If there is any data on this line before the \., complain.
				 *
				 * 若这一行在 \. 之前还有任何数据，则报错。
				 */
				if (cstate->line_buf.len > 0 ||
					prev_raw_ptr > cstate->input_buf_index)
					ereport(ERROR,
							(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
							 errmsg("end-of-copy marker is not alone on its line")));

				/*
				 * Discard the \. and newline, then report EOF.
				 *
				 * 丢弃 \. 和换行，然后报告 EOF。
				 */
				cstate->input_buf_index = input_buf_ptr;
				result = true;	/* report EOF */
				/*
				 *
				 * 报告 EOF
				 */
				break;
			}
			else
			{
				/*
				 * If we are here, it means we found a backslash followed by
				 * something other than a period.  In non-CSV mode, anything
				 * after a backslash is special, so we skip over that second
				 * character too.  If we didn't do that \\. would be
				 * considered an eof-of copy, while in non-CSV mode it is a
				 * literal backslash followed by a period.
				 *
				 * 若到达这里，说明找到了反斜杠，后面跟的不是句点。
				 * 在非 CSV 模式下，反斜杠后面的任何字符都是特殊的，因此也要跳过第二个字符。
				 * 若不这样做，\\. 会被当成拷贝结束，而在非 CSV 模式下它是一个字面反斜杠后跟句点。
				 */
				input_buf_ptr++;
			}
		}
	}							/* end of outer loop */
	/*
	 *
	 * 外层循环结束
	 */

	/*
	 * Transfer any still-uncopied data to line_buf.
	 *
	 * 把尚未拷贝的数据转移到 line_buf。
	 */
	REFILL_LINEBUF;

	return result;
}

/*
 *	Return decimal value for a hexadecimal digit
 *
 * 返回十六进制数字对应的十进制值
 */
static int
GetDecimalFromHex(char hex)
{
	if (isdigit((unsigned char) hex))
		return hex - '0';
	else
		return tolower((unsigned char) hex) - 'a' + 10;
}

/*
 * Parse the current line into separate attributes (fields),
 * performing de-escaping as needed.
 *
 * 把当前行解析成独立的属性（字段），并按需去转义。
 *
 * The input is in line_buf.  We use attribute_buf to hold the result
 * strings.  cstate->raw_fields[k] is set to point to the k'th attribute
 * string, or NULL when the input matches the null marker string.
 * This array is expanded as necessary.
 *
 * 输入在 line_buf 中。我们用 attribute_buf 保存结果字符串。
 * cstate->raw_fields[k] 指向第 k 个属性字符串；若输入匹配空值标记字符串则为 NULL。
 * 该数组会按需扩展。
 *
 * (Note that the caller cannot check for nulls since the returned
 * string would be the post-de-escaping equivalent, which may look
 * the same as some valid data string.)
 *
 * （注意调用方无法检查空值，因为返回的字符串是去转义之后的结果，它可能与某些合法数据字符串看起来一样。）
 *
 * delim is the column delimiter string (must be just one byte for now).
 * null_print is the null marker string.  Note that this is compared to
 * the pre-de-escaped input string.
 *
 * delim 是列分隔字符串（目前必须只有一个字节）。
 * null_print 是空值标记字符串。注意它与去转义之前的输入字符串比较。
 *
 * The return value is the number of fields actually read.
 *
 * 返回值是实际读到的字段数。
 */
static int
CopyReadAttributesText(CopyFromState cstate)
{
	char		delimc = cstate->opts.delim[0];
	int			fieldno;
	char	   *output_ptr;
	char	   *cur_ptr;
	char	   *line_end_ptr;

	/*
	 * We need a special case for zero-column tables: check that the input
	 * line is empty, and return.
	 *
	 * 零列表需要特例：检查输入行为空，然后返回。
	 */
	if (cstate->max_fields <= 0)
	{
		if (cstate->line_buf.len != 0)
			ereport(ERROR,
					(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
					 errmsg("extra data after last expected column")));
		return 0;
	}

	resetStringInfo(&cstate->attribute_buf);

	/*
	 * The de-escaped attributes will certainly not be longer than the input
	 * data line, so we can just force attribute_buf to be large enough and
	 * then transfer data without any checks for enough space.  We need to do
	 * it this way because enlarging attribute_buf mid-stream would invalidate
	 * pointers already stored into cstate->raw_fields[].
	 *
	 * 去转义后的属性肯定不会比输入数据行更长，因此可以直接把 attribute_buf 扩到足够大，再无检查地搬运数据。
	 * 必须这样做，因为中途扩大 attribute_buf 会使已经存入 cstate->raw_fields[] 的指针失效。
	 */
	if (cstate->attribute_buf.maxlen <= cstate->line_buf.len)
		enlargeStringInfo(&cstate->attribute_buf, cstate->line_buf.len);
	output_ptr = cstate->attribute_buf.data;

	/* set pointer variables for loop */
	/*
	 *
	 * 为循环设置指针变量
	 */
	cur_ptr = cstate->line_buf.data;
	line_end_ptr = cstate->line_buf.data + cstate->line_buf.len;

	/* Outer loop iterates over fields */
	/*
	 *
	 * 外层循环遍历各个字段
	 */
	fieldno = 0;
	for (;;)
	{
		bool		found_delim = false;
		char	   *start_ptr;
		char	   *end_ptr;
		int			input_len;
		bool		saw_non_ascii = false;

		/* Make sure there is enough space for the next value */
		/*
		 *
		 * 确保下一个值有足够空间
		 */
		if (fieldno >= cstate->max_fields)
		{
			cstate->max_fields *= 2;
			cstate->raw_fields =
				repalloc(cstate->raw_fields, cstate->max_fields * sizeof(char *));
		}

		/* Remember start of field on both input and output sides */
		/*
		 *
		 * 记住输入侧和输出侧字段的起始位置
		 */
		start_ptr = cur_ptr;
		cstate->raw_fields[fieldno] = output_ptr;

		/*
		 * Scan data for field.
		 *
		 * 扫描字段数据。
		 *
		 * Note that in this loop, we are scanning to locate the end of field
		 * and also speculatively performing de-escaping.  Once we find the
		 * end-of-field, we can match the raw field contents against the null
		 * marker string.  Only after that comparison fails do we know that
		 * de-escaping is actually the right thing to do; therefore we *must
		 * not* throw any syntax errors before we've done the null-marker
		 * check.
		 *
		 * 注意在这个循环中，我们一边扫描以定位字段结尾，一边推测性地去转义。
		 * 找到字段结尾后，可以把原始字段内容与空值标记字符串比较。
		 * 只有比较失败之后，才知道去转义确实是正确做法；因此在做空值标记检查之前，绝不能抛出任何语法错误。
		 */
		for (;;)
		{
			char		c;

			end_ptr = cur_ptr;
			if (cur_ptr >= line_end_ptr)
				break;
			c = *cur_ptr++;
			if (c == delimc)
			{
				found_delim = true;
				break;
			}
			if (c == '\\')
			{
				if (cur_ptr >= line_end_ptr)
					break;
				c = *cur_ptr++;
				switch (c)
				{
					case '0':
					case '1':
					case '2':
					case '3':
					case '4':
					case '5':
					case '6':
					case '7':
						{
							/* handle \013 */
							/*
							 *
							 * 处理 \013
							 */
							int			val;

							val = OCTVALUE(c);
							if (cur_ptr < line_end_ptr)
							{
								c = *cur_ptr;
								if (ISOCTAL(c))
								{
									cur_ptr++;
									val = (val << 3) + OCTVALUE(c);
									if (cur_ptr < line_end_ptr)
									{
										c = *cur_ptr;
										if (ISOCTAL(c))
										{
											cur_ptr++;
											val = (val << 3) + OCTVALUE(c);
										}
									}
								}
							}
							c = val & 0377;
							if (c == '\0' || IS_HIGHBIT_SET(c))
								saw_non_ascii = true;
						}
						break;
					case 'x':
						/* Handle \x3F */
						/*
						 *
						 * 处理 \x3F
						 */
						if (cur_ptr < line_end_ptr)
						{
							char		hexchar = *cur_ptr;

							if (isxdigit((unsigned char) hexchar))
							{
								int			val = GetDecimalFromHex(hexchar);

								cur_ptr++;
								if (cur_ptr < line_end_ptr)
								{
									hexchar = *cur_ptr;
									if (isxdigit((unsigned char) hexchar))
									{
										cur_ptr++;
										val = (val << 4) + GetDecimalFromHex(hexchar);
									}
								}
								c = val & 0xff;
								if (c == '\0' || IS_HIGHBIT_SET(c))
									saw_non_ascii = true;
							}
						}
						break;
					case 'b':
						c = '\b';
						break;
					case 'f':
						c = '\f';
						break;
					case 'n':
						c = '\n';
						break;
					case 'r':
						c = '\r';
						break;
					case 't':
						c = '\t';
						break;
					case 'v':
						c = '\v';
						break;

						/*
						 * in all other cases, take the char after '\'
						 * literally
						 *
						 * 在所有其他情况下，按字面取反斜杠后面的字符
						 */
				}
			}

			/* Add c to output string */
			/*
			 *
			 * 把 c 追加到输出字符串
			 */
			*output_ptr++ = c;
		}

		/* Check whether raw input matched null marker */
		/*
		 *
		 * 检查原始输入是否匹配空值标记
		 */
		input_len = end_ptr - start_ptr;
		if (input_len == cstate->opts.null_print_len &&
			strncmp(start_ptr, cstate->opts.null_print, input_len) == 0)
			cstate->raw_fields[fieldno] = NULL;
		/* Check whether raw input matched default marker */
		/*
		 *
		 * 检查原始输入是否匹配默认值标记
		 */
		else if (fieldno < list_length(cstate->attnumlist) &&
				 cstate->opts.default_print &&
				 input_len == cstate->opts.default_print_len &&
				 strncmp(start_ptr, cstate->opts.default_print, input_len) == 0)
		{
			/* fieldno is 0-indexed and attnum is 1-indexed */
			/*
			 *
			 * fieldno 从 0 起算，attnum 从 1 起算
			 */
			int			m = list_nth_int(cstate->attnumlist, fieldno) - 1;

			if (cstate->defexprs[m] != NULL)
			{
				/* defaults contain entries for all physical attributes */
				/*
				 *
				 * defaults 为所有物理属性都包含条目
				 */
				cstate->defaults[m] = true;
			}
			else
			{
				TupleDesc	tupDesc = RelationGetDescr(cstate->rel);
				Form_pg_attribute att = TupleDescAttr(tupDesc, m);

				ereport(ERROR,
						(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
						 errmsg("unexpected default marker in COPY data"),
						 errdetail("Column \"%s\" has no default value.",
								   NameStr(att->attname))));
			}
		}
		else
		{
			/*
			 * At this point we know the field is supposed to contain data.
			 *
			 * 到这里已经知道该字段应当包含数据。
			 *
			 * If we de-escaped any non-7-bit-ASCII chars, make sure the
			 * resulting string is valid data for the db encoding.
			 *
			 * 若去转义过程中出现了任何非 7 位 ASCII 字符，则确认结果字符串对该数据库编码是合法数据。
			 */
			if (saw_non_ascii)
			{
				char	   *fld = cstate->raw_fields[fieldno];

				pg_verifymbstr(fld, output_ptr - fld, false);
			}
		}

		/* Terminate attribute value in output area */
		/*
		 *
		 * 在输出区结束属性值
		 */
		*output_ptr++ = '\0';

		fieldno++;
		/* Done if we hit EOL instead of a delim */
		/*
		 *
		 * 若遇到的是行尾而不是分隔符，则结束
		 */
		if (!found_delim)
			break;
	}

	/* Clean up state of attribute_buf */
	/*
	 *
	 * 清理 attribute_buf 的状态
	 */
	output_ptr--;
	Assert(*output_ptr == '\0');
	cstate->attribute_buf.len = (output_ptr - cstate->attribute_buf.data);

	return fieldno;
}

/*
 * Parse the current line into separate attributes (fields),
 * performing de-escaping as needed.  This has exactly the same API as
 * CopyReadAttributesText, except we parse the fields according to
 * "standard" (i.e. common) CSV usage.
 *
 * 把当前行解析成独立的属性（字段），并按需去转义。
 * API 与 CopyReadAttributesText 完全相同，只是按“标准”（即常见）CSV 用法解析字段。
 */
static int
CopyReadAttributesCSV(CopyFromState cstate)
{
	char		delimc = cstate->opts.delim[0];
	char		quotec = cstate->opts.quote[0];
	char		escapec = cstate->opts.escape[0];
	int			fieldno;
	char	   *output_ptr;
	char	   *cur_ptr;
	char	   *line_end_ptr;

	/*
	 * We need a special case for zero-column tables: check that the input
	 * line is empty, and return.
	 *
	 * 零列表需要特例：检查输入行为空，然后返回。
	 */
	if (cstate->max_fields <= 0)
	{
		if (cstate->line_buf.len != 0)
			ereport(ERROR,
					(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
					 errmsg("extra data after last expected column")));
		return 0;
	}

	resetStringInfo(&cstate->attribute_buf);

	/*
	 * The de-escaped attributes will certainly not be longer than the input
	 * data line, so we can just force attribute_buf to be large enough and
	 * then transfer data without any checks for enough space.  We need to do
	 * it this way because enlarging attribute_buf mid-stream would invalidate
	 * pointers already stored into cstate->raw_fields[].
	 *
	 * 去转义后的属性肯定不会比输入数据行更长，因此可以直接把 attribute_buf 扩到足够大，再无检查地搬运数据。
	 * 必须这样做，因为中途扩大 attribute_buf 会使已经存入 cstate->raw_fields[] 的指针失效。
	 */
	if (cstate->attribute_buf.maxlen <= cstate->line_buf.len)
		enlargeStringInfo(&cstate->attribute_buf, cstate->line_buf.len);
	output_ptr = cstate->attribute_buf.data;

	/* set pointer variables for loop */
	/*
	 *
	 * 为循环设置指针变量
	 */
	cur_ptr = cstate->line_buf.data;
	line_end_ptr = cstate->line_buf.data + cstate->line_buf.len;

	/* Outer loop iterates over fields */
	/*
	 *
	 * 外层循环遍历各个字段
	 */
	fieldno = 0;
	for (;;)
	{
		bool		found_delim = false;
		bool		saw_quote = false;
		char	   *start_ptr;
		char	   *end_ptr;
		int			input_len;

		/* Make sure there is enough space for the next value */
		/*
		 *
		 * 确保下一个值有足够空间
		 */
		if (fieldno >= cstate->max_fields)
		{
			cstate->max_fields *= 2;
			cstate->raw_fields =
				repalloc(cstate->raw_fields, cstate->max_fields * sizeof(char *));
		}

		/* Remember start of field on both input and output sides */
		/*
		 *
		 * 记住输入侧和输出侧字段的起始位置
		 */
		start_ptr = cur_ptr;
		cstate->raw_fields[fieldno] = output_ptr;

		/*
		 * Scan data for field,
		 *
		 * 扫描字段数据，
		 *
		 * The loop starts in "not quote" mode and then toggles between that
		 * and "in quote" mode. The loop exits normally if it is in "not
		 * quote" mode and a delimiter or line end is seen.
		 *
		 * 循环从“不在引号内”模式开始，然后在该模式与“在引号内”模式之间切换。
		 * 若处于“不在引号内”模式并看到分隔符或行尾，则正常退出循环。
		 */
		for (;;)
		{
			char		c;

			/* Not in quote */
			/*
			 *
			 * 不在引号之内
			 */
			for (;;)
			{
				end_ptr = cur_ptr;
				if (cur_ptr >= line_end_ptr)
					goto endfield;
				c = *cur_ptr++;
				/* unquoted field delimiter */
				/*
				 *
				 * 未加引号的字段分隔符
				 */
				if (c == delimc)
				{
					found_delim = true;
					goto endfield;
				}
				/* start of quoted field (or part of field) */
				/*
				 *
				 * 带引号字段的开始（或字段的一部分）
				 */
				if (c == quotec)
				{
					saw_quote = true;
					break;
				}
				/* Add c to output string */
				/*
				 *
				 * 把 c 追加到输出字符串
				 */
				*output_ptr++ = c;
			}

			/* In quote */
			/*
			 *
			 * 处于引号之内
			 */
			for (;;)
			{
				end_ptr = cur_ptr;
				if (cur_ptr >= line_end_ptr)
					ereport(ERROR,
							(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
							 errmsg("unterminated CSV quoted field")));

				c = *cur_ptr++;

				/* escape within a quoted field */
				/*
				 *
				 * 带引号字段内部的转义
				 */
				if (c == escapec)
				{
					/*
					 * peek at the next char if available, and escape it if it
					 * is an escape char or a quote char
					 *
					 * 若下一个字符可用则先看一眼；若它是转义字符或引号字符，则转义它
					 */
					if (cur_ptr < line_end_ptr)
					{
						char		nextc = *cur_ptr;

						if (nextc == escapec || nextc == quotec)
						{
							*output_ptr++ = nextc;
							cur_ptr++;
							continue;
						}
					}
				}

				/*
				 * end of quoted field. Must do this test after testing for
				 * escape in case quote char and escape char are the same
				 * (which is the common case).
				 *
				 * 带引号字段的结尾。必须在检查转义之后做这个测试，以防引号字符与转义字符相同（这是常见情况）。
				 */
				if (c == quotec)
					break;

				/* Add c to output string */
				/*
				 *
				 * 把 c 追加到输出字符串
				 */
				*output_ptr++ = c;
			}
		}
endfield:

		/* Terminate attribute value in output area */
		/*
		 *
		 * 在输出区结束属性值
		 */
		*output_ptr++ = '\0';

		/* Check whether raw input matched null marker */
		/*
		 *
		 * 检查原始输入是否匹配空值标记
		 */
		input_len = end_ptr - start_ptr;
		if (!saw_quote && input_len == cstate->opts.null_print_len &&
			strncmp(start_ptr, cstate->opts.null_print, input_len) == 0)
			cstate->raw_fields[fieldno] = NULL;
		/* Check whether raw input matched default marker */
		/*
		 *
		 * 检查原始输入是否匹配默认值标记
		 */
		else if (fieldno < list_length(cstate->attnumlist) &&
				 cstate->opts.default_print &&
				 input_len == cstate->opts.default_print_len &&
				 strncmp(start_ptr, cstate->opts.default_print, input_len) == 0)
		{
			/* fieldno is 0-index and attnum is 1-index */
			/*
			 *
			 * fieldno 从 0 起算，attnum 从 1 起算
			 */
			int			m = list_nth_int(cstate->attnumlist, fieldno) - 1;

			if (cstate->defexprs[m] != NULL)
			{
				/* defaults contain entries for all physical attributes */
				/*
				 *
				 * defaults 为所有物理属性都包含条目
				 */
				cstate->defaults[m] = true;
			}
			else
			{
				TupleDesc	tupDesc = RelationGetDescr(cstate->rel);
				Form_pg_attribute att = TupleDescAttr(tupDesc, m);

				ereport(ERROR,
						(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
						 errmsg("unexpected default marker in COPY data"),
						 errdetail("Column \"%s\" has no default value.",
								   NameStr(att->attname))));
			}
		}

		fieldno++;
		/* Done if we hit EOL instead of a delim */
		/*
		 *
		 * 若遇到的是行尾而不是分隔符，则结束
		 */
		if (!found_delim)
			break;
	}

	/* Clean up state of attribute_buf */
	/*
	 *
	 * 清理 attribute_buf 的状态
	 */
	output_ptr--;
	Assert(*output_ptr == '\0');
	cstate->attribute_buf.len = (output_ptr - cstate->attribute_buf.data);

	return fieldno;
}


/*
 * Read a binary attribute
 *
 * 读取一个二进制属性
 */
static Datum
CopyReadBinaryAttribute(CopyFromState cstate, FmgrInfo *flinfo,
						Oid typioparam, int32 typmod,
						bool *isnull)
{
	int32		fld_size;
	Datum		result;

	if (!CopyGetInt32(cstate, &fld_size))
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("unexpected EOF in COPY data")));
	if (fld_size == -1)
	{
		*isnull = true;
		return ReceiveFunctionCall(flinfo, NULL, typioparam, typmod);
	}
	if (fld_size < 0)
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("invalid field size")));

	/* reset attribute_buf to empty, and load raw data in it */
	/*
	 *
	 * 把 attribute_buf 重置为空，并把原始数据装进去
	 */
	resetStringInfo(&cstate->attribute_buf);

	enlargeStringInfo(&cstate->attribute_buf, fld_size);
	if (CopyReadBinaryData(cstate, cstate->attribute_buf.data,
						   fld_size) != fld_size)
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("unexpected EOF in COPY data")));

	cstate->attribute_buf.len = fld_size;
	cstate->attribute_buf.data[fld_size] = '\0';

	/* Call the column type's binary input converter */
	/*
	 *
	 * 调用列类型的二进制输入转换器
	 */
	result = ReceiveFunctionCall(flinfo, &cstate->attribute_buf,
								 typioparam, typmod);

	/* Trouble if it didn't eat the whole buffer */
	/*
	 *
	 * 若没有吃掉整个缓冲区，就有麻烦了
	 */
	if (cstate->attribute_buf.cursor != cstate->attribute_buf.len)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("incorrect binary data format")));

	*isnull = false;
	return result;
}

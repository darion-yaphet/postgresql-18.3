/*-------------------------------------------------------------------------
 *
 * xlogreader.h
 *		Definitions for the generic XLog reading facility
 *
 * 中文翻译：xlogreader.h 通用 XLog 读取工具的定义
 *
 * Portions Copyright (c) 2013-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/include/access/xlogreader.h
 *
 * 中文翻译：识别 src/include/access/xlogreader.h
 *
 * NOTES
 *		See the definition of the XLogReaderState struct for instructions on
 *		how to use the XLogReader infrastructure.
 *
 * 中文翻译：注意 有关如何使用 XLogReader 基础结构的说明，请参阅 XLogReaderState 结构的定义。
 *
 *		The basic idea is to allocate an XLogReaderState via
 *		XLogReaderAllocate(), position the reader to the first record with
 *		XLogBeginRead() or XLogFindNextRecord(), and call XLogReadRecord()
 *		until it returns NULL.
 *
 * 中文翻译：基本思想是通过 XLogReaderAllocate() 分配 XLogReaderState，使用 XLogBeginRead() 或 XLogFindNextRecord() 将读取器定位到第一条记录，然后调用 XLogReadRecord() 直到返回 NULL。
 *
 *		Callers supply a page_read callback if they want to call
 *		XLogReadRecord or XLogFindNextRecord; it can be passed in as NULL
 *		otherwise.  The WALRead function can be used as a helper to write
 *		page_read callbacks, but it is not mandatory; callers that use it,
 *		must supply segment_open callbacks.  The segment_close callback
 *		must always be supplied.
 *
 * 中文翻译：如果调用者想要调用 XLogReadRecord 或 XLogFindNextRecord，则提供 page_read 回调；否则它可以作为 NULL 传入。 WALRead 函数可以用作编写 page_read 回调的帮助程序，但这不是强制性的；使用它的调用者必须提供segment_open回调。必须始终提供segment_close 回调。
 *
 *		After reading a record with XLogReadRecord(), it's decomposed into
 *		the per-block and main data parts, and the parts can be accessed
 *		with the XLogRec* macros and functions. You can also decode a
 *		record that's already constructed in memory, without reading from
 *		disk, by calling the DecodeXLogRecord() function.
 *
 * 中文翻译：使用 XLogReadRecord() 读取记录后，它被分解为每块和主数据部分，并且可以使用 XLogRec* 宏和函数访问这些部分。您还可以通过调用 DecodeXLogRecord() 函数对内存中已构建的记录进行解码，而无需从磁盘读取。
 *-------------------------------------------------------------------------
 */
#ifndef XLOGREADER_H
#define XLOGREADER_H

#ifndef FRONTEND
#include "access/transam.h"
#endif

#include "access/xlogrecord.h"
#include "storage/buf.h"

/* WALOpenSegment represents a WAL segment being read. */

/* WALOpenSegment 表示正在读取的 WAL 段。 */
typedef struct WALOpenSegment
{
	int			ws_file;		/* segment file descriptor */

	/* 段文件描述符。 */
	XLogSegNo	ws_segno;		/* segment number */

	/* 段号。 */
	TimeLineID	ws_tli;			/* timeline ID of the currently open file */

	/* 当前打开文件的时间线 ID。 */
} WALOpenSegment;

/* WALSegmentContext carries context information about WAL segments to read */

/* WALSegmentContext 保存待读取 WAL 段的上下文信息。 */
typedef struct WALSegmentContext
{
	char		ws_dir[MAXPGPATH];
	int			ws_segsize;
} WALSegmentContext;

typedef struct XLogReaderState XLogReaderState;

/* Function type definitions for various xlogreader interactions */

/* 各种 xlogreader 交互的函数类型定义。 */
typedef int (*XLogPageReadCB) (XLogReaderState *xlogreader,
							   XLogRecPtr targetPagePtr,
							   int reqLen,
							   XLogRecPtr targetRecPtr,
							   char *readBuf);
typedef void (*WALSegmentOpenCB) (XLogReaderState *xlogreader,
								  XLogSegNo nextSegNo,
								  TimeLineID *tli_p);
typedef void (*WALSegmentCloseCB) (XLogReaderState *xlogreader);

typedef struct XLogReaderRoutine
{
	/*
	 * Data input callback
 *
 * 中文翻译：数据输入回调
	 *
	 * This callback shall read at least reqLen valid bytes of the xlog page
	 * starting at targetPagePtr, and store them in readBuf.  The callback
	 * shall return the number of bytes read (never more than XLOG_BLCKSZ), or
	 * -1 on failure.  The callback shall sleep, if necessary, to wait for the
	 * requested bytes to become available.  The callback will not be invoked
	 * again for the same page unless more than the returned number of bytes
	 * are needed.
 *
 * 中文翻译：此回调应至少读取从 targetPagePtr 开始的 xlog 页面的 reqLen 有效字节，并将它们存储在 readBuf 中。回调应返回读取的字节数（不超过 XLOG_BLCKSZ），失败时返回 -1。如有必要，回调应休眠以等待请求的字节变得可用。除非需要的字节数超过返回的字节数，否则不会对同一页面再次调用回调。
	 *
	 * targetRecPtr is the position of the WAL record we're reading.  Usually
	 * it is equal to targetPagePtr + reqLen, but sometimes xlogreader needs
	 * to read and verify the page or segment header, before it reads the
	 * actual WAL record it's interested in.  In that case, targetRecPtr can
	 * be used to determine which timeline to read the page from.
 *
 * 中文翻译：targetRecPtr 是我们正在读取的 WAL 记录的位置。通常它等于 targetPagePtr + reqLen，但有时 xlogreader 需要在读取它感兴趣的实际 WAL 记录之前读取并验证页或段标头。在这种情况下，targetRecPtr 可用于确定从哪个时间线读取页。
	 *
	 * The callback shall set ->seg.ws_tli to the TLI of the file the page was
	 * read from.
 *
 * 中文翻译：回调应将 ->seg.ws_tli 设置为从中读取页面的文件的 TLI。
	 */
	XLogPageReadCB page_read;

	/*
	 * Callback to open the specified WAL segment for reading.  ->seg.ws_file
	 * shall be set to the file descriptor of the opened segment.  In case of
	 * failure, an error shall be raised by the callback and it shall not
	 * return.
 *
 * 中文翻译：打开指定 WAL 段进行读取的回调。 ->seg.ws_file 应设置为打开段的文件描述符。如果失败，回调将引发错误并且不会返回。
	 *
	 * "nextSegNo" is the number of the segment to be opened.
 *
 * 中文翻译：“nextSegNo”是要打开的段的编号。
	 *
	 * "tli_p" is an input/output argument. WALRead() uses it to pass the
	 * timeline in which the new segment should be found, but the callback can
	 * use it to return the TLI that it actually opened.
 *
 * 中文翻译：“tli_p”是输入/输出参数。 WALRead() 使用它来传递应在其中找到新段的时间线，但回调可以使用它来返回它实际打开的 TLI。
	 */
	WALSegmentOpenCB segment_open;

	/*
	 * WAL segment close callback.  ->seg.ws_file shall be set to a negative
	 * number.
 *
 * 中文翻译：WAL 段关闭回调。 ->seg.ws_file 应设置为负数。
	 */
	WALSegmentCloseCB segment_close;
} XLogReaderRoutine;

#define XL_ROUTINE(...) &(XLogReaderRoutine){__VA_ARGS__}

typedef struct
{
	/* Is this block ref in use? */

	/* 该块引用是否正在使用？ */
	bool		in_use;

	/* Identify the block this refers to */

	/* 标识该引用所指向的块。 */
	RelFileLocator rlocator;
	ForkNumber	forknum;
	BlockNumber blkno;

	/* Prefetching workspace. */

	/* 预取工作区。 */
	Buffer		prefetch_buffer;

	/* copy of the fork_flags field from the XLogRecordBlockHeader */

	/* XLogRecordBlockHeader 中 fork_flags 字段的副本。 */
	uint8		flags;

	/* Information on full-page image, if any */

	/* 完整页面镜像的信息（如有）。 */
	bool		has_image;		/* has image, even for consistency checking */

	/* 具有页面镜像，即使仅用于一致性检查。 */
	bool		apply_image;	/* has image that should be restored */

	/* 具有应当恢复的页面镜像。 */
	char	   *bkp_image;
	uint16		hole_offset;
	uint16		hole_length;
	uint16		bimg_len;
	uint8		bimg_info;

	/* Buffer holding the rmgr-specific data associated with this block */

	/* 保存与该块关联的资源管理器专用数据的缓冲区。 */
	bool		has_data;
	char	   *data;
	uint16		data_len;
	uint16		data_bufsz;
} DecodedBkpBlock;

/*
 * The decoded contents of a record.  This occupies a contiguous region of
 * memory, with main_data and blocks[n].data pointing to memory after the
 * members declared here.
 *
 * 中文翻译：记录的解码内容。它占用了一个连续的内存区域，其中 main_data 和blocks[n].data 指向此处声明的成员之后的内存。
 */
typedef struct DecodedXLogRecord
{
	/* Private member used for resource management. */

	/* 用于资源管理的私有成员。 */
	size_t		size;			/* total size of decoded record */

	/* 已解码记录的总大小。 */
	bool		oversized;		/* outside the regular decode buffer? */

	/* 位于常规解码缓冲区之外？ */
	struct DecodedXLogRecord *next; /* decoded record queue link */

	/* 已解码记录队列链接。 */

	/* Public members. */

	/* 公共成员。 */
	XLogRecPtr	lsn;			/* location */

	/* 位置。 */
	XLogRecPtr	next_lsn;		/* location of next record */

	/* 下一条记录的位置。 */
	XLogRecord	header;			/* header */

	/* 中文翻译：标头 */
	RepOriginId record_origin;
	TransactionId toplevel_xid; /* XID of top-level transaction */

	/* 中文翻译：顶级交易的XID */
	char	   *main_data;		/* record's main data portion */

	/* 记录的主数据部分。 */
	uint32		main_data_len;	/* main data portion's length */

	/* 中文翻译：主要数据部分的长度 */
	int			max_block_id;	/* highest block_id in use (-1 if none) */

	/* 正在使用的最高 block_id（无则为 -1）。 */
	DecodedBkpBlock blocks[FLEXIBLE_ARRAY_MEMBER];
} DecodedXLogRecord;

struct XLogReaderState
{
	/*
	 * Operational callbacks
 *
 * 中文翻译：操作回调
	 */
	XLogReaderRoutine routine;

	/* ----------------------------------------
	 * Public parameters
 *
 * 中文翻译：公共参数
	 * ----------------------------------------
	 */

	/*
	 * System identifier of the xlog files we're about to read.  Set to zero
	 * (the default value) if unknown or unimportant.
 *
 * 中文翻译：我们将要读取的 xlog 文件的系统标识符。如果未知或不重要，则设置为零（默认值）。
	 */
	uint64		system_identifier;

	/*
	 * Opaque data for callbacks to use.  Not used by XLogReader.
 *
 * 中文翻译：供回调使用的不透明数据。 XLogReader 不使用。
	 */
	void	   *private_data;

	/*
	 * Start and end point of last record read.  EndRecPtr is also used as the
	 * position to read next.  Calling XLogBeginRead() sets EndRecPtr to the
	 * starting position and ReadRecPtr to invalid.
 *
 * 中文翻译：最后读取的记录的起点和终点。 EndRecPtr 也用作下一个读取的位置。调用 XLogBeginRead() 将 EndRecPtr 设置为起始位置，并将 ReadRecPtr 设置为无效。
	 *
	 * Start and end point of last record returned by XLogReadRecord().  These
	 * are also available as record->lsn and record->next_lsn.
 *
 * 中文翻译：XLogReadRecord() 返回的最后一条记录的起点和终点。这些也可用作 record->lsn 和 record->next_lsn。
	 */
	XLogRecPtr	ReadRecPtr;		/* start of last record read */

	/* 最近读取记录的起始位置。 */
	XLogRecPtr	EndRecPtr;		/* end+1 of last record read */

	/* 最近读取记录的结束位置加一。 */

	/*
	 * Set at the end of recovery: the start point of a partial record at the
	 * end of WAL (InvalidXLogRecPtr if there wasn't one), and the start
	 * location of its first contrecord that went missing.
 *
 * 中文翻译：在恢复结束时设置：WAL 末尾的部分记录的起始点（如果没有则为 InvalidXLogRecPtr），以及其第一个丢失的连续记录的起始位置。
	 */
	XLogRecPtr	abortedRecPtr;
	XLogRecPtr	missingContrecPtr;
	/* Set when XLP_FIRST_IS_OVERWRITE_CONTRECORD is found */

	/* 发现 XLP_FIRST_IS_OVERWRITE_CONTRECORD 时设置。 */
	XLogRecPtr	overwrittenRecPtr;


	/* ----------------------------------------
	 * Decoded representation of current record
 *
 * 中文翻译：当前记录的解码表示
	 *
	 * Use XLogRecGet* functions to investigate the record; these fields
	 * should not be accessed directly.
 *
 * 中文翻译：使用 XLogRecGet* 函数来调查记录；不应直接访问这些字段。
	 * ----------------------------------------
	 * Start and end point of the last record read and decoded by
	 * XLogReadRecord().  NextRecPtr is also used as the position to decode
	 * next.  Calling XLogBeginRead() sets NextRecPtr and EndRecPtr to the
	 * requested starting position.
 *
 * 中文翻译：由 XLogReadRecord() 读取和解码的最后一条记录的起点和终点。 NextRecPtr 也用作下一个解码的位置。调用 XLogBeginRead() 将 NextRecPtr 和 EndRecPtr 设置为请求的起始位置。
	 */
	XLogRecPtr	DecodeRecPtr;	/* start of last record decoded */

	/* 中文翻译：解码的最后一条记录的开头 */
	XLogRecPtr	NextRecPtr;		/* end+1 of last record decoded */

	/* 中文翻译：解码的最后一条记录的 end+1 */
	XLogRecPtr	PrevRecPtr;		/* start of previous record decoded */

	/* 前一条解码记录的起始位置。 */

	/* Last record returned by XLogReadRecord(). */

	/* XLogReadRecord() 最近返回的记录。 */
	DecodedXLogRecord *record;

	/* ----------------------------------------
	 * private/internal state
 *
 * 中文翻译：私人/内部国家
	 * ----------------------------------------
	 */

	/*
	 * Buffer for decoded records.  This is a circular buffer, though
	 * individual records can't be split in the middle, so some space is often
	 * wasted at the end.  Oversized records that don't fit in this space are
	 * allocated separately.
 *
 * 中文翻译：解码记录的缓冲区。这是一个循环缓冲区，虽然各个记录不能在中间分割，所以最后常常会浪费一些空间。不适合此空间的超大记录将单独分配。
	 */
	char	   *decode_buffer;
	size_t		decode_buffer_size;
	bool		free_decode_buffer; /* need to free? */

	/* 是否需要释放？ */
	char	   *decode_buffer_head; /* data is read from the head */

	/* 从头部读取数据。 */
	char	   *decode_buffer_tail; /* new data is written at the tail */

	/* 新数据写入尾部。 */

	/*
	 * Queue of records that have been decoded.  This is a linked list that
	 * usually consists of consecutive records in decode_buffer, but may also
	 * contain oversized records allocated with palloc().
 *
 * 中文翻译：已解码的记录队列。这是一个链表，通常由decode_buffer中的连续记录组成，但也可能包含使用palloc()分配的超大记录。
	 */
	DecodedXLogRecord *decode_queue_head;	/* oldest decoded record */

	/* 最早解码的记录。 */
	DecodedXLogRecord *decode_queue_tail;	/* newest decoded record */

	/* 最新解码的记录。 */

	/*
	 * Buffer for currently read page (XLOG_BLCKSZ bytes, valid up to at least
	 * readLen bytes)
 *
 * 中文翻译：当前读取页的缓冲区（XLOG_BLCKSZ 字节，有效至至少 readLen 字节）
	 */
	char	   *readBuf;
	uint32		readLen;

	/* last read XLOG position for data currently in readBuf */

	/* 中文翻译：当前 readBuf 中数据的最后读取 XLOG 位置 */
	WALSegmentContext segcxt;
	WALOpenSegment seg;
	uint32		segoff;

	/*
	 * beginning of prior page read, and its TLI.  Doesn't necessarily
	 * correspond to what's in readBuf; used for timeline sanity checks.
 *
 * 中文翻译：前一页读取的开始及其 TLI。不一定对应readBuf中的内容；用于时间线健全性检查。
	 */
	XLogRecPtr	latestPagePtr;
	TimeLineID	latestPageTLI;

	/* beginning of the WAL record being read. */

	/* 中文翻译：正在读取的 WAL 记录的开头。 */
	XLogRecPtr	currRecPtr;
	/* timeline to read it from, 0 if a lookup is required */

	/* 中文翻译：从中读取的时间线，如果需要查找则为 0 */
	TimeLineID	currTLI;

	/*
	 * Safe point to read to in currTLI if current TLI is historical
	 * (tliSwitchPoint) or InvalidXLogRecPtr if on current timeline.
 *
 * 中文翻译：如果当前 TLI 是历史的 (tliSwitchPoint)，则在 currTLI 中读取安全点；如果在当前时间线上，则在 InvalidXLogRecPtr 中读取。
	 *
	 * Actually set to the start of the segment containing the timeline switch
	 * that ends currTLI's validity, not the LSN of the switch its self, since
	 * we can't assume the old segment will be present.
 *
 * 中文翻译：实际上设置为包含结束 currTLI 有效性的时间线开关的段的开头，而不是开关本身的 LSN，因为我们不能假设旧段将存在。
	 */
	XLogRecPtr	currTLIValidUntil;

	/*
	 * If currTLI is not the most recent known timeline, the next timeline to
	 * read from when currTLIValidUntil is reached.
 *
 * 中文翻译：如果 currTLI 不是最近的已知时间线，则为到达 currTLIValidUntil 时读取的下一个时间线。
	 */
	TimeLineID	nextTLI;

	/*
	 * Buffer for current ReadRecord result (expandable), used when a record
	 * crosses a page boundary.
 *
 * 中文翻译：当前 ReadRecord 结果的缓冲区（可扩展），当记录跨越页边界时使用。
	 */
	char	   *readRecordBuf;
	uint32		readRecordBufSize;

	/* Buffer to hold error message */

	/* 保存错误消息的缓冲区。 */
	char	   *errormsg_buf;
	bool		errormsg_deferred;

	/*
	 * Flag to indicate to XLogPageReadCB that it should not block waiting for
	 * data.
 *
 * 中文翻译：向 XLogPageReadCB 指示它不应阻塞等待数据的标志。
	 */
	bool		nonblocking;
};

/*
 * Check if XLogNextRecord() has any more queued records or an error to return.
 *
 * 中文翻译：检查 XLogNextRecord() 是否还有更多排队记录或返回错误。
 */
/*
 * Function: XLogReaderHasQueuedRecordOrError.
 * Purpose: Performs the WAL reading or recovery operation represented by xlog reader has queued record or error.
 * Core flow: It prepares the reader or recovery context, processes the requested WAL state, and exposes the result.
 *
 * 函数：XLogReaderHasQueuedRecordOrError。
 * 作用：执行 xlog reader has queued record or error 所表示的 WAL 读取或恢复操作。
 * 核心流程：它准备读取器或恢复上下文，处理请求的 WAL 状态，并提供结果。
 */
static inline bool
XLogReaderHasQueuedRecordOrError(XLogReaderState *state)
{
	return (state->decode_queue_head != NULL) || state->errormsg_deferred;
}

/* Get a new XLogReader */

/* 获取一个新的 XLogReader。 */
/*
 * Function: XLogReaderAllocate.
 * Purpose: Performs the WAL reading or recovery operation represented by xlog reader allocate.
 * Core flow: It prepares the reader or recovery context, processes the requested WAL state, and exposes the result.
 *
 * 函数：XLogReaderAllocate。
 * 作用：执行 xlog reader allocate 所表示的 WAL 读取或恢复操作。
 * 核心流程：它准备读取器或恢复上下文，处理请求的 WAL 状态，并提供结果。
 */
extern XLogReaderState *XLogReaderAllocate(int wal_segment_size,
										   const char *waldir,
										   XLogReaderRoutine *routine,
										   void *private_data);

/* Free an XLogReader */

/* 释放 XLogReader。 */
/*
 * Function: XLogReaderFree.
 * Purpose: Performs the WAL reading or recovery operation represented by xlog reader free.
 * Core flow: It prepares the reader or recovery context, processes the requested WAL state, and exposes the result.
 *
 * 函数：XLogReaderFree。
 * 作用：执行 xlog reader free 所表示的 WAL 读取或恢复操作。
 * 核心流程：它准备读取器或恢复上下文，处理请求的 WAL 状态，并提供结果。
 */
extern void XLogReaderFree(XLogReaderState *state);

/* Optionally provide a circular decoding buffer to allow readahead. */

/* 可选地提供循环解码缓冲区以支持预读。 */
/*
 * Function: XLogReaderSetDecodeBuffer.
 * Purpose: Performs the WAL reading or recovery operation represented by xlog reader set decode buffer.
 * Core flow: It prepares the reader or recovery context, processes the requested WAL state, and exposes the result.
 *
 * 函数：XLogReaderSetDecodeBuffer。
 * 作用：执行 xlog reader set decode buffer 所表示的 WAL 读取或恢复操作。
 * 核心流程：它准备读取器或恢复上下文，处理请求的 WAL 状态，并提供结果。
 */
extern void XLogReaderSetDecodeBuffer(XLogReaderState *state,
									  void *buffer,
									  size_t size);

/* Position the XLogReader to given record */

/* 将 XLogReader 定位到给定记录。 */
/*
 * Function: XLogBeginRead.
 * Purpose: Performs the WAL reading or recovery operation represented by xlog begin read.
 * Core flow: It prepares the reader or recovery context, processes the requested WAL state, and exposes the result.
 *
 * 函数：XLogBeginRead。
 * 作用：执行 xlog begin read 所表示的 WAL 读取或恢复操作。
 * 核心流程：它准备读取器或恢复上下文，处理请求的 WAL 状态，并提供结果。
 */
extern void XLogBeginRead(XLogReaderState *state, XLogRecPtr RecPtr);
/*
 * Function: XLogFindNextRecord.
 * Purpose: Performs the WAL reading or recovery operation represented by xlog find next record.
 * Core flow: It prepares the reader or recovery context, processes the requested WAL state, and exposes the result.
 *
 * 函数：XLogFindNextRecord。
 * 作用：执行 xlog find next record 所表示的 WAL 读取或恢复操作。
 * 核心流程：它准备读取器或恢复上下文，处理请求的 WAL 状态，并提供结果。
 */
extern XLogRecPtr XLogFindNextRecord(XLogReaderState *state, XLogRecPtr RecPtr);

/* Return values from XLogPageReadCB. */

/* XLogPageReadCB 的返回值。 */
typedef enum XLogPageReadResult
{
	XLREAD_SUCCESS = 0,			/* record is successfully read */

	/* 记录已成功读取。 */
	XLREAD_FAIL = -1,			/* failed during reading a record */

	/* 读取记录时失败。 */
	XLREAD_WOULDBLOCK = -2,		/* nonblocking mode only, no data */

	/* 仅限非阻塞模式：没有数据。 */
} XLogPageReadResult;

/* Read the next XLog record. Returns NULL on end-of-WAL or failure */

/* 读取下一条 XLog 记录；到达 WAL 末尾或失败时返回 NULL。 */
/*
 * Function: XLogReadRecord.
 * Purpose: Performs the WAL reading or recovery operation represented by xlog read record.
 * Core flow: It prepares the reader or recovery context, processes the requested WAL state, and exposes the result.
 *
 * 函数：XLogReadRecord。
 * 作用：执行 xlog read record 所表示的 WAL 读取或恢复操作。
 * 核心流程：它准备读取器或恢复上下文，处理请求的 WAL 状态，并提供结果。
 */
extern struct XLogRecord *XLogReadRecord(XLogReaderState *state,
										 char **errormsg);

/* Consume the next record or error. */

/* 消费下一条记录或错误。 */
/*
 * Function: XLogNextRecord.
 * Purpose: Performs the WAL reading or recovery operation represented by xlog next record.
 * Core flow: It prepares the reader or recovery context, processes the requested WAL state, and exposes the result.
 *
 * 函数：XLogNextRecord。
 * 作用：执行 xlog next record 所表示的 WAL 读取或恢复操作。
 * 核心流程：它准备读取器或恢复上下文，处理请求的 WAL 状态，并提供结果。
 */
extern DecodedXLogRecord *XLogNextRecord(XLogReaderState *state,
										 char **errormsg);

/* Release the previously returned record, if necessary. */

/* 必要时释放之前返回的记录。 */
/*
 * Function: XLogReleasePreviousRecord.
 * Purpose: Performs the WAL reading or recovery operation represented by xlog release previous record.
 * Core flow: It prepares the reader or recovery context, processes the requested WAL state, and exposes the result.
 *
 * 函数：XLogReleasePreviousRecord。
 * 作用：执行 xlog release previous record 所表示的 WAL 读取或恢复操作。
 * 核心流程：它准备读取器或恢复上下文，处理请求的 WAL 状态，并提供结果。
 */
extern XLogRecPtr XLogReleasePreviousRecord(XLogReaderState *state);

/* Try to read ahead, if there is data and space. */

/* 如果存在数据和空间，尝试预读。 */
/*
 * Function: XLogReadAhead.
 * Purpose: Performs the WAL reading or recovery operation represented by xlog read ahead.
 * Core flow: It prepares the reader or recovery context, processes the requested WAL state, and exposes the result.
 *
 * 函数：XLogReadAhead。
 * 作用：执行 xlog read ahead 所表示的 WAL 读取或恢复操作。
 * 核心流程：它准备读取器或恢复上下文，处理请求的 WAL 状态，并提供结果。
 */
extern DecodedXLogRecord *XLogReadAhead(XLogReaderState *state,
										bool nonblocking);

/* Validate a page */

/* 验证页面。 */
/*
 * Function: XLogReaderValidatePageHeader.
 * Purpose: Performs the WAL reading or recovery operation represented by xlog reader validate page header.
 * Core flow: It prepares the reader or recovery context, processes the requested WAL state, and exposes the result.
 *
 * 函数：XLogReaderValidatePageHeader。
 * 作用：执行 xlog reader validate page header 所表示的 WAL 读取或恢复操作。
 * 核心流程：它准备读取器或恢复上下文，处理请求的 WAL 状态，并提供结果。
 */
extern bool XLogReaderValidatePageHeader(XLogReaderState *state,
										 XLogRecPtr recptr, char *phdr);

/* Forget error produced by XLogReaderValidatePageHeader(). */

/* 清除 XLogReaderValidatePageHeader() 产生的错误。 */
/*
 * Function: XLogReaderResetError.
 * Purpose: Performs the WAL reading or recovery operation represented by xlog reader reset error.
 * Core flow: It prepares the reader or recovery context, processes the requested WAL state, and exposes the result.
 *
 * 函数：XLogReaderResetError。
 * 作用：执行 xlog reader reset error 所表示的 WAL 读取或恢复操作。
 * 核心流程：它准备读取器或恢复上下文，处理请求的 WAL 状态，并提供结果。
 */
extern void XLogReaderResetError(XLogReaderState *state);

/*
 * Error information from WALRead that both backend and frontend caller can
 * process.  Currently only errors from pg_pread can be reported.
 *
 * 中文翻译：来自 WALRead 的错误信息，后端和前端调用者都可以处理。目前只能报告来自 pg_pread 的错误。
 */
typedef struct WALReadError
{
	int			wre_errno;		/* errno set by the last pg_pread() */

	/* 最近一次 pg_pread() 设置的 errno。 */
	int			wre_off;		/* Offset we tried to read from. */

	/* 尝试读取的偏移量。 */
	int			wre_req;		/* Bytes requested to be read. */

	/* 请求读取的字节数。 */
	int			wre_read;		/* Bytes read by the last read(). */

	/* 最近一次 read() 读取的字节数。 */
	WALOpenSegment wre_seg;		/* Segment we tried to read from. */

	/* 尝试从中读取的段。 */
} WALReadError;

/*
 * Function: WALRead.
 * Purpose: Performs the WAL reading or recovery operation represented by walread.
 * Core flow: It prepares the reader or recovery context, processes the requested WAL state, and exposes the result.
 *
 * 函数：WALRead。
 * 作用：执行 walread 所表示的 WAL 读取或恢复操作。
 * 核心流程：它准备读取器或恢复上下文，处理请求的 WAL 状态，并提供结果。
 */
extern bool WALRead(XLogReaderState *state,
					char *buf, XLogRecPtr startptr, Size count,
					TimeLineID tli, WALReadError *errinfo);

/* Functions for decoding an XLogRecord */

/* 用于解码 XLogRecord 的函数。 */

/*
 * Function: DecodeXLogRecordRequiredSpace.
 * Purpose: Obtains, validates, or decodes the WAL state represented by decode xlog record required space.
 * Core flow: It examines the supplied record, block, page, or recovery state and returns the derived result.
 *
 * 函数：DecodeXLogRecordRequiredSpace。
 * 作用：获取、验证或解码 decode xlog record required space 所表示的 WAL 状态。
 * 核心流程：它检查给定记录、块、页面或恢复状态，并返回推导结果。
 */
extern size_t DecodeXLogRecordRequiredSpace(size_t xl_tot_len);
/*
 * Function: DecodeXLogRecord.
 * Purpose: Obtains, validates, or decodes the WAL state represented by decode xlog record.
 * Core flow: It examines the supplied record, block, page, or recovery state and returns the derived result.
 *
 * 函数：DecodeXLogRecord。
 * 作用：获取、验证或解码 decode xlog record 所表示的 WAL 状态。
 * 核心流程：它检查给定记录、块、页面或恢复状态，并返回推导结果。
 */
extern bool DecodeXLogRecord(XLogReaderState *state,
							 DecodedXLogRecord *decoded,
							 XLogRecord *record,
							 XLogRecPtr lsn,
							 char **errormsg);

/*
 * Macros that provide access to parts of the record most recently returned by
 * XLogReadRecord() or XLogNextRecord().
 *
 * 中文翻译：提供对 XLogReadRecord() 或 XLogNextRecord() 最近返回的记录部分的访问的宏。
 */
#define XLogRecGetTotalLen(decoder) ((decoder)->record->header.xl_tot_len)
#define XLogRecGetPrev(decoder) ((decoder)->record->header.xl_prev)
#define XLogRecGetInfo(decoder) ((decoder)->record->header.xl_info)
#define XLogRecGetRmid(decoder) ((decoder)->record->header.xl_rmid)
#define XLogRecGetXid(decoder) ((decoder)->record->header.xl_xid)
#define XLogRecGetOrigin(decoder) ((decoder)->record->record_origin)
#define XLogRecGetTopXid(decoder) ((decoder)->record->toplevel_xid)
#define XLogRecGetData(decoder) ((decoder)->record->main_data)
#define XLogRecGetDataLen(decoder) ((decoder)->record->main_data_len)
#define XLogRecHasAnyBlockRefs(decoder) ((decoder)->record->max_block_id >= 0)
#define XLogRecMaxBlockId(decoder) ((decoder)->record->max_block_id)
#define XLogRecGetBlock(decoder, i) (&(decoder)->record->blocks[(i)])
#define XLogRecHasBlockRef(decoder, block_id)			\
	(((decoder)->record->max_block_id >= (block_id)) &&	\
	 ((decoder)->record->blocks[block_id].in_use))
#define XLogRecHasBlockImage(decoder, block_id)		\
	((decoder)->record->blocks[block_id].has_image)
#define XLogRecBlockImageApply(decoder, block_id)		\
	((decoder)->record->blocks[block_id].apply_image)
#define XLogRecHasBlockData(decoder, block_id)		\
	((decoder)->record->blocks[block_id].has_data)

#ifndef FRONTEND
/*
 * Function: XLogRecGetFullXid.
 * Purpose: Performs the WAL reading or recovery operation represented by xlog rec get full xid.
 * Core flow: It prepares the reader or recovery context, processes the requested WAL state, and exposes the result.
 *
 * 函数：XLogRecGetFullXid。
 * 作用：执行 xlog rec get full xid 所表示的 WAL 读取或恢复操作。
 * 核心流程：它准备读取器或恢复上下文，处理请求的 WAL 状态，并提供结果。
 */
extern FullTransactionId XLogRecGetFullXid(XLogReaderState *record);
#endif

/*
 * Function: RestoreBlockImage.
 * Purpose: Obtains, validates, or decodes the WAL state represented by restore block image.
 * Core flow: It examines the supplied record, block, page, or recovery state and returns the derived result.
 *
 * 函数：RestoreBlockImage。
 * 作用：获取、验证或解码 restore block image 所表示的 WAL 状态。
 * 核心流程：它检查给定记录、块、页面或恢复状态，并返回推导结果。
 */
extern bool RestoreBlockImage(XLogReaderState *record, uint8 block_id, char *page);
/*
 * Function: XLogRecGetBlockData.
 * Purpose: Performs the WAL reading or recovery operation represented by xlog rec get block data.
 * Core flow: It prepares the reader or recovery context, processes the requested WAL state, and exposes the result.
 *
 * 函数：XLogRecGetBlockData。
 * 作用：执行 xlog rec get block data 所表示的 WAL 读取或恢复操作。
 * 核心流程：它准备读取器或恢复上下文，处理请求的 WAL 状态，并提供结果。
 */
extern char *XLogRecGetBlockData(XLogReaderState *record, uint8 block_id, Size *len);
/*
 * Function: XLogRecGetBlockTag.
 * Purpose: Performs the WAL reading or recovery operation represented by xlog rec get block tag.
 * Core flow: It prepares the reader or recovery context, processes the requested WAL state, and exposes the result.
 *
 * 函数：XLogRecGetBlockTag。
 * 作用：执行 xlog rec get block tag 所表示的 WAL 读取或恢复操作。
 * 核心流程：它准备读取器或恢复上下文，处理请求的 WAL 状态，并提供结果。
 */
extern void XLogRecGetBlockTag(XLogReaderState *record, uint8 block_id,
							   RelFileLocator *rlocator, ForkNumber *forknum,
							   BlockNumber *blknum);
/*
 * Function: XLogRecGetBlockTagExtended.
 * Purpose: Performs the WAL reading or recovery operation represented by xlog rec get block tag extended.
 * Core flow: It prepares the reader or recovery context, processes the requested WAL state, and exposes the result.
 *
 * 函数：XLogRecGetBlockTagExtended。
 * 作用：执行 xlog rec get block tag extended 所表示的 WAL 读取或恢复操作。
 * 核心流程：它准备读取器或恢复上下文，处理请求的 WAL 状态，并提供结果。
 */
extern bool XLogRecGetBlockTagExtended(XLogReaderState *record, uint8 block_id,
									   RelFileLocator *rlocator, ForkNumber *forknum,
									   BlockNumber *blknum,
									   Buffer *prefetch_buffer);

#endif							/* XLOGREADER_H */

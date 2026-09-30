/*
 * xlog_internal.h
 *
 * 中文翻译：xlog_internal.h
 *
 * PostgreSQL write-ahead log internal declarations
 *
 * 中文翻译：PostgreSQL 预写日志内部声明
 *
 * NOTE: this file is intended to contain declarations useful for
 * manipulating the XLOG files directly, but it is not supposed to be
 * needed by rmgr routines (redo support for individual record types).
 * So the XLogRecord typedef and associated stuff appear in xlogrecord.h.
 *
 * 中文翻译：注意：此文件旨在包含可用于直接操作 XLOG 文件的声明，但 rmgr 例程不需要它（对单个记录类型的重做支持）。因此 XLogRecord typedef 和相关内容出现在 xlogrecord.h 中。
 *
 * Note: This file must be includable in both frontend and backend contexts,
 * to allow stand-alone tools like pg_receivewal to deal with WAL files.
 *
 * 中文翻译：注意：此文件必须可包含在前端和后端上下文中，以允许像 pg_receivewal 这样的独立工具处理 WAL 文件。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/xlog_internal.h
 *
 * 中文翻译：src/include/access/xlog_internal.h
 */
#ifndef XLOG_INTERNAL_H
#define XLOG_INTERNAL_H

#include "access/xlogdefs.h"
#include "access/xlogreader.h"
#include "datatype/timestamp.h"
#include "lib/stringinfo.h"
#include "pgtime.h"
#include "storage/block.h"
#include "storage/relfilelocator.h"


/*
 * Each page of XLOG file has a header like this:
 *
 * XLOG 文件的每个页面都具有这样的头部。
 */
#define XLOG_PAGE_MAGIC 0xD118	/* can be used as WAL version indicator */

/* 可用作 WAL 版本标识。 */

typedef struct XLogPageHeaderData
{
	uint16		xlp_magic;		/* magic value for correctness checks */

	/* 中文翻译：正确性检查的魔法值 */
	uint16		xlp_info;		/* flag bits, see below */

	/* 中文翻译：标志位，见下文 */
	TimeLineID	xlp_tli;		/* TimeLineID of first record on page */

	/* 中文翻译：页上第一条记录的 TimeLineID */
	XLogRecPtr	xlp_pageaddr;	/* XLOG address of this page */

	/* 中文翻译：本页XLOG地址 */

	/*
	 * When there is not enough space on current page for whole record, we
	 * continue on the next page.  xlp_rem_len is the number of bytes
	 * remaining from a previous page; it tracks xl_tot_len in the initial
	 * header.  Note that the continuation data isn't necessarily aligned.
 *
 * 当当前页面空间不足以容纳完整记录时，记录会续接到下一页；xlp_rem_len 保存来自前一页的剩余字节数。
	 */
	uint32		xlp_rem_len;	/* total len of remaining data for record */

	/* 中文翻译：记录剩余数据的总长度 */
} XLogPageHeaderData;

#define SizeOfXLogShortPHD	MAXALIGN(sizeof(XLogPageHeaderData))

typedef XLogPageHeaderData *XLogPageHeader;

/*
 * When the XLP_LONG_HEADER flag is set, we store additional fields in the
 * page header.  (This is ordinarily done just in the first page of an
 * XLOG file.)	The additional fields serve to identify the file accurately.
 *
 * 设置 XLP_LONG_HEADER 时，页面头会存储额外字段，通常仅位于 XLOG 文件的第一页，以准确标识文件。
 */
typedef struct XLogLongPageHeaderData
{
	XLogPageHeaderData std;		/* standard header fields */

	/* 中文翻译：标准头字段 */
	uint64		xlp_sysid;		/* system identifier from pg_control */

	/* 中文翻译：来自 pg_control 的系统标识符 */
	uint32		xlp_seg_size;	/* just as a cross-check */

	/* 中文翻译：只是作为交叉检查 */
	uint32		xlp_xlog_blcksz;	/* just as a cross-check */

	/* 中文翻译：只是作为交叉检查 */
} XLogLongPageHeaderData;

#define SizeOfXLogLongPHD	MAXALIGN(sizeof(XLogLongPageHeaderData))

typedef XLogLongPageHeaderData *XLogLongPageHeader;

/* When record crosses page boundary, set this flag in new page's header */

/* 当记录跨越页面边界时，在新页面的头部设置此标志。 */
#define XLP_FIRST_IS_CONTRECORD		0x0001
/* This flag indicates a "long" page header */

/* 此标志表示“长”页面头。 */
#define XLP_LONG_HEADER				0x0002
/* This flag indicates backup blocks starting in this page are optional */

/* 此标志表示从该页面开始的备份块是可选的。 */
#define XLP_BKP_REMOVABLE			0x0004
/* Replaces a missing contrecord; see CreateOverwriteContrecordRecord */

/* 替换缺失的续接记录；参见 CreateOverwriteContrecordRecord。 */
#define XLP_FIRST_IS_OVERWRITE_CONTRECORD 0x0008
/* All defined flag bits in xlp_info (used for validity checking of header) */

/* xlp_info 中全部已定义的标志位（用于校验页面头有效性）。 */
#define XLP_ALL_FLAGS				0x000F

#define XLogPageHeaderSize(hdr)		\
	(((hdr)->xlp_info & XLP_LONG_HEADER) ? SizeOfXLogLongPHD : SizeOfXLogShortPHD)

/* wal_segment_size can range from 1MB to 1GB */

/* wal_segment_size 的范围可以是 1MB 至 1GB。 */
#define WalSegMinSize 1024 * 1024
#define WalSegMaxSize 1024 * 1024 * 1024
/* default number of min and max wal segments */

/* 中文翻译：最小和最大 wal 段的默认数量 */
#define DEFAULT_MIN_WAL_SEGS 5
#define DEFAULT_MAX_WAL_SEGS 64

/* check that the given size is a valid wal_segment_size */

/* 检查给定大小是否为有效的 wal_segment_size。 */
#define IsPowerOf2(x) (x > 0 && ((x) & ((x)-1)) == 0)
#define IsValidWalSegSize(size) \
	 (IsPowerOf2(size) && \
	 ((size) >= WalSegMinSize && (size) <= WalSegMaxSize))

#define XLogSegmentsPerXLogId(wal_segsz_bytes)	\
	(UINT64CONST(0x100000000) / (wal_segsz_bytes))

#define XLogSegNoOffsetToRecPtr(segno, offset, wal_segsz_bytes, dest) \
		(dest) = (segno) * (wal_segsz_bytes) + (offset)

#define XLogSegmentOffset(xlogptr, wal_segsz_bytes)	\
	((xlogptr) & ((wal_segsz_bytes) - 1))

/*
 * Compute a segment number from an XLogRecPtr.
 *
 * 根据 XLogRecPtr 计算段号；XLByteToPrevSeg 会把边界字节视为前一段，适用于根据记录末尾指针确定写入段。
 *
 * For XLByteToSeg, do the computation at face value.  For XLByteToPrevSeg,
 * a boundary byte is taken to be in the previous segment.  This is suitable
 * for deciding which segment to write given a pointer to a record end,
 * for example.
 *
 * 中文翻译：对于 XLByteToSeg，按面值进行计算。对于XLByteToPrevSeg，边界字节被认为是在前一个段中。例如，这适合于在给定指向记录末尾的指针的情况下决定写入哪个段。
 */
#define XLByteToSeg(xlrp, logSegNo, wal_segsz_bytes) \
	logSegNo = (xlrp) / (wal_segsz_bytes)

#define XLByteToPrevSeg(xlrp, logSegNo, wal_segsz_bytes) \
	logSegNo = ((xlrp) - 1) / (wal_segsz_bytes)

/*
 * Convert values of GUCs measured in megabytes to equiv. segment count.
 * Rounds down.
 *
 * 把以 MB 表示的 GUC 值换算为等价的段数量，并向下取整。
 */
#define XLogMBVarToSegs(mbvar, wal_segsz_bytes) \
	((mbvar) / ((wal_segsz_bytes) / (1024 * 1024)))

/*
 * Is an XLogRecPtr within a particular XLOG segment?
 *
 * 判断 XLogRecPtr 是否位于指定 XLOG 段中；边界处理与对应宏的语义一致。
 *
 * For XLByteInSeg, do the computation at face value.  For XLByteInPrevSeg,
 * a boundary byte is taken to be in the previous segment.
 *
 * 中文翻译：对于XLByteInSeg，按面值进行计算。对于XLByteInPrevSeg，边界字节被认为是在前一个段中。
 */
#define XLByteInSeg(xlrp, logSegNo, wal_segsz_bytes) \
	(((xlrp) / (wal_segsz_bytes)) == (logSegNo))

#define XLByteInPrevSeg(xlrp, logSegNo, wal_segsz_bytes) \
	((((xlrp) - 1) / (wal_segsz_bytes)) == (logSegNo))

/* Check if an XLogRecPtr value is in a plausible range */

/* 检查 XLogRecPtr 值是否处于合理范围内。 */
#define XRecOffIsValid(xlrp) \
		((xlrp) % XLOG_BLCKSZ >= SizeOfXLogShortPHD)

/*
 * The XLog directory and control file (relative to $PGDATA)
 *
 * XLog 目录和控制文件（相对于 $PGDATA）。
 */
#define XLOGDIR				"pg_wal"
#define XLOG_CONTROL_FILE	"global/pg_control"

/*
 * These macros encapsulate knowledge about the exact layout of XLog file
 * names, timeline history file names, and archive-status file names.
 *
 * 这些宏封装 XLog 文件名、时间线历史文件名和归档状态文件名的精确布局。
 */
#define MAXFNAMELEN		64

/* Length of XLog file name */

/* XLog 文件名长度。 */
#define XLOG_FNAME_LEN	   24

/*
 * Generate a WAL segment file name.  Do not use this function in a helper
 * function allocating the result generated.
 *
 * 生成 WAL 段文件名；不要在会分配其结果的辅助函数中使用该函数。
 */
/*
 * Function: XLogFileName.
 * Purpose: Performs the WAL operation represented by xlog file name.
 * Core flow: It prepares the WAL context, performs the requested write, recovery, or control action, and exposes the result.
 *
 * 函数：XLogFileName。
 * 作用：执行 xlog file name 所表示的 WAL 操作。
 * 核心流程：它准备 WAL 上下文，执行请求的写入、恢复或控制操作，并提供结果。
 */
static inline void
XLogFileName(char *fname, TimeLineID tli, XLogSegNo logSegNo, int wal_segsz_bytes)
{
	snprintf(fname, MAXFNAMELEN, "%08X%08X%08X", tli,
			 (uint32) (logSegNo / XLogSegmentsPerXLogId(wal_segsz_bytes)),
			 (uint32) (logSegNo % XLogSegmentsPerXLogId(wal_segsz_bytes)));
}

/*
 * Function: XLogFileNameById.
 * Purpose: Performs the WAL operation represented by xlog file name by id.
 * Core flow: It prepares the WAL context, performs the requested write, recovery, or control action, and exposes the result.
 *
 * 函数：XLogFileNameById。
 * 作用：执行 xlog file name by id 所表示的 WAL 操作。
 * 核心流程：它准备 WAL 上下文，执行请求的写入、恢复或控制操作，并提供结果。
 */
static inline void
XLogFileNameById(char *fname, TimeLineID tli, uint32 log, uint32 seg)
{
	snprintf(fname, MAXFNAMELEN, "%08X%08X%08X", tli, log, seg);
}

/*
 * Function: IsXLogFileName.
 * Purpose: Obtains or checks the WAL state represented by is xlog file name.
 * Core flow: It inspects the supplied position, identifier, or shared WAL state and returns the derived result.
 *
 * 函数：IsXLogFileName。
 * 作用：获取或检查 is xlog file name 所表示的 WAL 状态。
 * 核心流程：它检查给定位置、标识符或共享 WAL 状态，并返回推导结果。
 */
static inline bool
IsXLogFileName(const char *fname)
{
	return (strlen(fname) == XLOG_FNAME_LEN && \
			strspn(fname, "0123456789ABCDEF") == XLOG_FNAME_LEN);
}

/*
 * XLOG segment with .partial suffix.  Used by pg_receivewal and at end of
 * archive recovery, when we want to archive a WAL segment but it might not
 * be complete yet.
 *
 * 带 .partial 后缀的 XLOG 段供 pg_receivewal 和归档恢复末尾使用，此时该段可能尚未完整。
 */
/*
 * Function: IsPartialXLogFileName.
 * Purpose: Obtains or checks the WAL state represented by is partial xlog file name.
 * Core flow: It inspects the supplied position, identifier, or shared WAL state and returns the derived result.
 *
 * 函数：IsPartialXLogFileName。
 * 作用：获取或检查 is partial xlog file name 所表示的 WAL 状态。
 * 核心流程：它检查给定位置、标识符或共享 WAL 状态，并返回推导结果。
 */
static inline bool
IsPartialXLogFileName(const char *fname)
{
	return (strlen(fname) == XLOG_FNAME_LEN + strlen(".partial") &&
			strspn(fname, "0123456789ABCDEF") == XLOG_FNAME_LEN &&
			strcmp(fname + XLOG_FNAME_LEN, ".partial") == 0);
}

/*
 * Function: XLogFromFileName.
 * Purpose: Performs the WAL operation represented by xlog from file name.
 * Core flow: It prepares the WAL context, performs the requested write, recovery, or control action, and exposes the result.
 *
 * 函数：XLogFromFileName。
 * 作用：执行 xlog from file name 所表示的 WAL 操作。
 * 核心流程：它准备 WAL 上下文，执行请求的写入、恢复或控制操作，并提供结果。
 */
static inline void
XLogFromFileName(const char *fname, TimeLineID *tli, XLogSegNo *logSegNo, int wal_segsz_bytes)
{
	uint32		log;
	uint32		seg;

	sscanf(fname, "%08X%08X%08X", tli, &log, &seg);
	*logSegNo = (uint64) log * XLogSegmentsPerXLogId(wal_segsz_bytes) + seg;
}

/*
 * Function: XLogFilePath.
 * Purpose: Performs the WAL operation represented by xlog file path.
 * Core flow: It prepares the WAL context, performs the requested write, recovery, or control action, and exposes the result.
 *
 * 函数：XLogFilePath。
 * 作用：执行 xlog file path 所表示的 WAL 操作。
 * 核心流程：它准备 WAL 上下文，执行请求的写入、恢复或控制操作，并提供结果。
 */
static inline void
XLogFilePath(char *path, TimeLineID tli, XLogSegNo logSegNo, int wal_segsz_bytes)
{
	snprintf(path, MAXPGPATH, XLOGDIR "/%08X%08X%08X", tli,
			 (uint32) (logSegNo / XLogSegmentsPerXLogId(wal_segsz_bytes)),
			 (uint32) (logSegNo % XLogSegmentsPerXLogId(wal_segsz_bytes)));
}

/*
 * Function: TLHistoryFileName.
 * Purpose: Performs the WAL operation represented by tlhistory file name.
 * Core flow: It uses the supplied context to process WAL state and returns or records the result.
 *
 * 函数：TLHistoryFileName。
 * 作用：执行 tlhistory file name 所表示的 WAL 操作。
 * 核心流程：它使用给定上下文处理 WAL 状态，并返回或记录结果。
 */
static inline void
TLHistoryFileName(char *fname, TimeLineID tli)
{
	snprintf(fname, MAXFNAMELEN, "%08X.history", tli);
}

/*
 * Function: IsTLHistoryFileName.
 * Purpose: Obtains or checks the WAL state represented by is tlhistory file name.
 * Core flow: It inspects the supplied position, identifier, or shared WAL state and returns the derived result.
 *
 * 函数：IsTLHistoryFileName。
 * 作用：获取或检查 is tlhistory file name 所表示的 WAL 状态。
 * 核心流程：它检查给定位置、标识符或共享 WAL 状态，并返回推导结果。
 */
static inline bool
IsTLHistoryFileName(const char *fname)
{
	return (strlen(fname) == 8 + strlen(".history") &&
			strspn(fname, "0123456789ABCDEF") == 8 &&
			strcmp(fname + 8, ".history") == 0);
}

/*
 * Function: TLHistoryFilePath.
 * Purpose: Performs the WAL operation represented by tlhistory file path.
 * Core flow: It uses the supplied context to process WAL state and returns or records the result.
 *
 * 函数：TLHistoryFilePath。
 * 作用：执行 tlhistory file path 所表示的 WAL 操作。
 * 核心流程：它使用给定上下文处理 WAL 状态，并返回或记录结果。
 */
static inline void
TLHistoryFilePath(char *path, TimeLineID tli)
{
	snprintf(path, MAXPGPATH, XLOGDIR "/%08X.history", tli);
}

/*
 * Function: StatusFilePath.
 * Purpose: Performs the WAL operation represented by status file path.
 * Core flow: It uses the supplied context to process WAL state and returns or records the result.
 *
 * 函数：StatusFilePath。
 * 作用：执行 status file path 所表示的 WAL 操作。
 * 核心流程：它使用给定上下文处理 WAL 状态，并返回或记录结果。
 */
static inline void
StatusFilePath(char *path, const char *xlog, const char *suffix)
{
	snprintf(path, MAXPGPATH, XLOGDIR "/archive_status/%s%s", xlog, suffix);
}

/*
 * Function: BackupHistoryFileName.
 * Purpose: Performs the WAL operation represented by backup history file name.
 * Core flow: It uses the supplied context to process WAL state and returns or records the result.
 *
 * 函数：BackupHistoryFileName。
 * 作用：执行 backup history file name 所表示的 WAL 操作。
 * 核心流程：它使用给定上下文处理 WAL 状态，并返回或记录结果。
 */
static inline void
BackupHistoryFileName(char *fname, TimeLineID tli, XLogSegNo logSegNo, XLogRecPtr startpoint, int wal_segsz_bytes)
{
	snprintf(fname, MAXFNAMELEN, "%08X%08X%08X.%08X.backup", tli,
			 (uint32) (logSegNo / XLogSegmentsPerXLogId(wal_segsz_bytes)),
			 (uint32) (logSegNo % XLogSegmentsPerXLogId(wal_segsz_bytes)),
			 (uint32) (XLogSegmentOffset(startpoint, wal_segsz_bytes)));
}

/*
 * Function: IsBackupHistoryFileName.
 * Purpose: Obtains or checks the WAL state represented by is backup history file name.
 * Core flow: It inspects the supplied position, identifier, or shared WAL state and returns the derived result.
 *
 * 函数：IsBackupHistoryFileName。
 * 作用：获取或检查 is backup history file name 所表示的 WAL 状态。
 * 核心流程：它检查给定位置、标识符或共享 WAL 状态，并返回推导结果。
 */
static inline bool
IsBackupHistoryFileName(const char *fname)
{
	return (strlen(fname) > XLOG_FNAME_LEN &&
			strspn(fname, "0123456789ABCDEF") == XLOG_FNAME_LEN &&
			strcmp(fname + strlen(fname) - strlen(".backup"), ".backup") == 0);
}

/*
 * Function: BackupHistoryFilePath.
 * Purpose: Performs the WAL operation represented by backup history file path.
 * Core flow: It uses the supplied context to process WAL state and returns or records the result.
 *
 * 函数：BackupHistoryFilePath。
 * 作用：执行 backup history file path 所表示的 WAL 操作。
 * 核心流程：它使用给定上下文处理 WAL 状态，并返回或记录结果。
 */
static inline void
BackupHistoryFilePath(char *path, TimeLineID tli, XLogSegNo logSegNo, XLogRecPtr startpoint, int wal_segsz_bytes)
{
	snprintf(path, MAXPGPATH, XLOGDIR "/%08X%08X%08X.%08X.backup", tli,
			 (uint32) (logSegNo / XLogSegmentsPerXLogId(wal_segsz_bytes)),
			 (uint32) (logSegNo % XLogSegmentsPerXLogId(wal_segsz_bytes)),
			 (uint32) (XLogSegmentOffset((startpoint), wal_segsz_bytes)));
}

/*
 * Information logged when we detect a change in one of the parameters
 * important for Hot Standby.
 *
 * 记录检测到的、对热备重要的参数变更。
 */
typedef struct xl_parameter_change
{
	int			MaxConnections;
	int			max_worker_processes;
	int			max_wal_senders;
	int			max_prepared_xacts;
	int			max_locks_per_xact;
	int			wal_level;
	bool		wal_log_hints;
	bool		track_commit_timestamp;
} xl_parameter_change;

/* logs restore point */

/* 记录还原点。 */
typedef struct xl_restore_point
{
	TimestampTz rp_time;
	char		rp_name[MAXFNAMELEN];
} xl_restore_point;

/* Overwrite of prior contrecord */

/* 覆盖之前的续接记录。 */
typedef struct xl_overwrite_contrecord
{
	XLogRecPtr	overwritten_lsn;
	TimestampTz overwrite_time;
} xl_overwrite_contrecord;

/* End of recovery mark, when we don't do an END_OF_RECOVERY checkpoint */

/* 中文翻译：当我们不执行 END_OF_RECOVERY 检查点时，恢复结束标记 */
typedef struct xl_end_of_recovery
{
	TimestampTz end_time;
	TimeLineID	ThisTimeLineID; /* new TLI */

	/* 新的时间线 ID。 */
	TimeLineID	PrevTimeLineID; /* previous TLI we forked off from */

	/* 此前分叉的时间线 ID。 */
	int			wal_level;
} xl_end_of_recovery;

/*
 * The functions in xloginsert.c construct a chain of XLogRecData structs
 * to represent the final WAL record.
 *
 * xloginsert.c 中的函数构造 XLogRecData 链表，以表示最终 WAL 记录。
 */
typedef struct XLogRecData
{
	struct XLogRecData *next;	/* next struct in chain, or NULL */

	/* 中文翻译：链中的下一个结构，或 NULL */
	const void *data;			/* start of rmgr data to include */

	/* 中文翻译：要包含的 rmgr 数据的开始 */
	uint32		len;			/* length of rmgr data to include */

	/* 中文翻译：要包含的 rmgr 数据的长度 */
} XLogRecData;

/*
 * Recovery target action.
 *
 * 恢复目标操作。
 */
typedef enum
{
	RECOVERY_TARGET_ACTION_PAUSE,
	RECOVERY_TARGET_ACTION_PROMOTE,
	RECOVERY_TARGET_ACTION_SHUTDOWN,
}			RecoveryTargetAction;

struct LogicalDecodingContext;
struct XLogRecordBuffer;

/*
 * Method table for resource managers.
 *
 * 资源管理器的方法表必须与 rmgr.c 中的 PG_RMGR 定义保持同步；各回调用于重做、描述、标识、启动、清理、掩码和逻辑解码。
 *
 * This struct must be kept in sync with the PG_RMGR definition in
 * rmgr.c.
 *
 * 中文翻译：该结构必须与 rmgr.c 中的 PG_RMGR 定义保持同步。
 *
 * rm_identify must return a name for the record based on xl_info (without
 * reference to the rmid). For example, XLOG_BTREE_VACUUM would be named
 * "VACUUM". rm_desc can then be called to obtain additional detail for the
 * record, if available (e.g. the last block).
 *
 * 中文翻译：rm_identify 必须根据 xl_info 返回记录的名称（不引用 rmid）。例如，XLOG_BTREE_VACUUM 将被命名为“VACUUM”。然后可以调用 rm_desc 来获取记录的其他详细信息（如果可用）（例如最后一个块）。
 *
 * rm_mask takes as input a page modified by the resource manager and masks
 * out bits that shouldn't be flagged by wal_consistency_checking.
 *
 * 中文翻译：rm_mask 将资源管理器修改的页面作为输入，并屏蔽掉不应由 wal_consistency_checking 标记的位。
 *
 * RmgrTable[] is indexed by RmgrId values (see rmgrlist.h). If rm_name is
 * NULL, the corresponding RmgrTable entry is considered invalid.
 *
 * 中文翻译：RmgrTable[] 由 RmgrId 值索引（请参阅 rmgrlist.h）。如果 rm_name 为 NULL，则相应的 RmgrTable 条目被视为无效。
 */
typedef struct RmgrData
{
	const char *rm_name;
	void		(*rm_redo) (XLogReaderState *record);
	void		(*rm_desc) (StringInfo buf, XLogReaderState *record);
	const char *(*rm_identify) (uint8 info);
	void		(*rm_startup) (void);
	void		(*rm_cleanup) (void);
	void		(*rm_mask) (char *pagedata, BlockNumber blkno);
	void		(*rm_decode) (struct LogicalDecodingContext *ctx,
							  struct XLogRecordBuffer *buf);
} RmgrData;

extern PGDLLIMPORT RmgrData RmgrTable[];
/*
 * Function: RmgrStartup.
 * Purpose: Performs the resource-manager operation represented by rmgr startup.
 * Core flow: It dispatches or interprets WAL record handling and returns or reports the resulting state.
 *
 * 函数：RmgrStartup。
 * 作用：执行 rmgr startup 所表示的资源管理器操作。
 * 核心流程：它分派或解释 WAL 记录处理，并返回或报告生成状态。
 */
extern void RmgrStartup(void);
/*
 * Function: RmgrCleanup.
 * Purpose: Performs the resource-manager operation represented by rmgr cleanup.
 * Core flow: It dispatches or interprets WAL record handling and returns or reports the resulting state.
 *
 * 函数：RmgrCleanup。
 * 作用：执行 rmgr cleanup 所表示的资源管理器操作。
 * 核心流程：它分派或解释 WAL 记录处理，并返回或报告生成状态。
 */
extern void RmgrCleanup(void);
/*
 * Function: RmgrNotFound.
 * Purpose: Performs the resource-manager operation represented by rmgr not found.
 * Core flow: It dispatches or interprets WAL record handling and returns or reports the resulting state.
 *
 * 函数：RmgrNotFound。
 * 作用：执行 rmgr not found 所表示的资源管理器操作。
 * 核心流程：它分派或解释 WAL 记录处理，并返回或报告生成状态。
 */
extern void RmgrNotFound(RmgrId rmid);
/*
 * Function: RegisterCustomRmgr.
 * Purpose: Performs the WAL operation represented by register custom rmgr.
 * Core flow: It prepares the WAL context, performs the requested write, recovery, or control action, and exposes the result.
 *
 * 函数：RegisterCustomRmgr。
 * 作用：执行 register custom rmgr 所表示的 WAL 操作。
 * 核心流程：它准备 WAL 上下文，执行请求的写入、恢复或控制操作，并提供结果。
 */
extern void RegisterCustomRmgr(RmgrId rmid, const RmgrData *rmgr);

#ifndef FRONTEND
/*
 * Function: RmgrIdExists.
 * Purpose: Performs the resource-manager operation represented by rmgr id exists.
 * Core flow: It dispatches or interprets WAL record handling and returns or reports the resulting state.
 *
 * 函数：RmgrIdExists。
 * 作用：执行 rmgr id exists 所表示的资源管理器操作。
 * 核心流程：它分派或解释 WAL 记录处理，并返回或报告生成状态。
 */
static inline bool
RmgrIdExists(RmgrId rmid)
{
	return RmgrTable[rmid].rm_name != NULL;
}

/*
 * Function: GetRmgr.
 * Purpose: Obtains or checks the WAL state represented by get rmgr.
 * Core flow: It inspects the supplied position, identifier, or shared WAL state and returns the derived result.
 *
 * 函数：GetRmgr。
 * 作用：获取或检查 get rmgr 所表示的 WAL 状态。
 * 核心流程：它检查给定位置、标识符或共享 WAL 状态，并返回推导结果。
 */
static inline RmgrData
GetRmgr(RmgrId rmid)
{
	if (unlikely(!RmgrIdExists(rmid)))
		RmgrNotFound(rmid);
	return RmgrTable[rmid];
}
#endif

/*
 * Exported to support xlog switching from checkpointer
 *
 * 导出以支持检查点进程切换 xlog。
 */
/*
 * Function: GetLastSegSwitchData.
 * Purpose: Obtains or checks the WAL state represented by get last seg switch data.
 * Core flow: It inspects the supplied position, identifier, or shared WAL state and returns the derived result.
 *
 * 函数：GetLastSegSwitchData。
 * 作用：获取或检查 get last seg switch data 所表示的 WAL 状态。
 * 核心流程：它检查给定位置、标识符或共享 WAL 状态，并返回推导结果。
 */
extern pg_time_t GetLastSegSwitchData(XLogRecPtr *lastSwitchLSN);
/*
 * Function: RequestXLogSwitch.
 * Purpose: Performs the WAL operation represented by request xlog switch.
 * Core flow: It prepares the WAL context, performs the requested write, recovery, or control action, and exposes the result.
 *
 * 函数：RequestXLogSwitch。
 * 作用：执行 request xlog switch 所表示的 WAL 操作。
 * 核心流程：它准备 WAL 上下文，执行请求的写入、恢复或控制操作，并提供结果。
 */
extern XLogRecPtr RequestXLogSwitch(bool mark_unimportant);

/*
 * Function: GetOldestRestartPoint.
 * Purpose: Obtains or checks the WAL state represented by get oldest restart point.
 * Core flow: It inspects the supplied position, identifier, or shared WAL state and returns the derived result.
 *
 * 函数：GetOldestRestartPoint。
 * 作用：获取或检查 get oldest restart point 所表示的 WAL 状态。
 * 核心流程：它检查给定位置、标识符或共享 WAL 状态，并返回推导结果。
 */
extern void GetOldestRestartPoint(XLogRecPtr *oldrecptr, TimeLineID *oldtli);

/*
 * Function: XLogRecGetBlockRefInfo.
 * Purpose: Performs the WAL operation represented by xlog rec get block ref info.
 * Core flow: It prepares the WAL context, performs the requested write, recovery, or control action, and exposes the result.
 *
 * 函数：XLogRecGetBlockRefInfo。
 * 作用：执行 xlog rec get block ref info 所表示的 WAL 操作。
 * 核心流程：它准备 WAL 上下文，执行请求的写入、恢复或控制操作，并提供结果。
 */
extern void XLogRecGetBlockRefInfo(XLogReaderState *record, bool pretty,
								   bool detailed_format, StringInfo buf,
								   uint32 *fpi_len);

/*
 * Exported for the functions in timeline.c and xlogarchive.c.  Only valid
 * in the startup process.
 *
 * 这些导出的归档恢复状态仅在启动进程中有效。
 */
extern PGDLLIMPORT bool ArchiveRecoveryRequested;
extern PGDLLIMPORT bool InArchiveRecovery;
extern PGDLLIMPORT bool StandbyMode;
extern PGDLLIMPORT char *recoveryRestoreCommand;

#endif							/* XLOG_INTERNAL_H */

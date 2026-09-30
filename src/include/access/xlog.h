/*
 * xlog.h
 *
 * 中文翻译：xlog.h
 *
 * PostgreSQL write-ahead log manager
 *
 * 中文翻译：PostgreSQL 预写日志管理器
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/xlog.h
 *
 * 中文翻译：src/include/access/xlog.h
 */
#ifndef XLOG_H
#define XLOG_H

#include "access/xlogbackup.h"
#include "access/xlogdefs.h"
#include "datatype/timestamp.h"
#include "lib/stringinfo.h"
#include "nodes/pg_list.h"


/* Sync methods */

/* 同步方法。 */
enum WalSyncMethod
{
	WAL_SYNC_METHOD_FSYNC = 0,
	WAL_SYNC_METHOD_FDATASYNC,
	WAL_SYNC_METHOD_OPEN,		/* for O_SYNC */

	/* 中文翻译：对于 O_SYNC */
	WAL_SYNC_METHOD_FSYNC_WRITETHROUGH,
	WAL_SYNC_METHOD_OPEN_DSYNC	/* for O_DSYNC */

	/* 中文翻译：对于 O_DSYNC */
};
extern PGDLLIMPORT int wal_sync_method;

extern PGDLLIMPORT XLogRecPtr ProcLastRecPtr;
extern PGDLLIMPORT XLogRecPtr XactLastRecEnd;
extern PGDLLIMPORT XLogRecPtr XactLastCommitEnd;

/* these variables are GUC parameters related to XLOG */

/* 这些变量是与 XLOG 相关的 GUC 参数。 */
extern PGDLLIMPORT int wal_segment_size;
extern PGDLLIMPORT int min_wal_size_mb;
extern PGDLLIMPORT int max_wal_size_mb;
extern PGDLLIMPORT int wal_keep_size_mb;
extern PGDLLIMPORT int max_slot_wal_keep_size_mb;
extern PGDLLIMPORT int XLOGbuffers;
extern PGDLLIMPORT int XLogArchiveTimeout;
extern PGDLLIMPORT int wal_retrieve_retry_interval;
extern PGDLLIMPORT char *XLogArchiveCommand;
extern PGDLLIMPORT bool EnableHotStandby;
extern PGDLLIMPORT bool fullPageWrites;
extern PGDLLIMPORT bool wal_log_hints;
extern PGDLLIMPORT int wal_compression;
extern PGDLLIMPORT bool wal_init_zero;
extern PGDLLIMPORT bool wal_recycle;
extern PGDLLIMPORT bool *wal_consistency_checking;
extern PGDLLIMPORT char *wal_consistency_checking_string;
extern PGDLLIMPORT bool log_checkpoints;
extern PGDLLIMPORT int CommitDelay;
extern PGDLLIMPORT int CommitSiblings;
extern PGDLLIMPORT bool track_wal_io_timing;
extern PGDLLIMPORT int wal_decode_buffer_size;

extern PGDLLIMPORT int CheckPointSegments;

/* Archive modes */

/* 归档模式。 */
typedef enum ArchiveMode
{
	ARCHIVE_MODE_OFF = 0,		/* disabled */

	/* 已禁用。 */
	ARCHIVE_MODE_ON,			/* enabled while server is running normally */

	/* 中文翻译：服务器正常运行时启用 */
	ARCHIVE_MODE_ALWAYS,		/* enabled always (even during recovery) */

	/* 中文翻译：始终启用（即使在恢复期间） */
} ArchiveMode;
extern PGDLLIMPORT int XLogArchiveMode;

/* WAL levels */

/* WAL 级别。 */
typedef enum WalLevel
{
	WAL_LEVEL_MINIMAL = 0,
	WAL_LEVEL_REPLICA,
	WAL_LEVEL_LOGICAL,
} WalLevel;

/* Compression algorithms for WAL */

/* WAL 压缩算法。 */
typedef enum WalCompression
{
	WAL_COMPRESSION_NONE = 0,
	WAL_COMPRESSION_PGLZ,
	WAL_COMPRESSION_LZ4,
	WAL_COMPRESSION_ZSTD,
} WalCompression;

/* Recovery states */

/* 恢复状态。 */
typedef enum RecoveryState
{
	RECOVERY_STATE_CRASH = 0,	/* crash recovery */

	/* 崩溃恢复。 */
	RECOVERY_STATE_ARCHIVE,		/* archive recovery */

	/* 归档恢复。 */
	RECOVERY_STATE_DONE,		/* currently in production */

	/* 当前处于生产状态。 */
} RecoveryState;

extern PGDLLIMPORT int wal_level;

/* Is WAL archiving enabled (always or only while server is running normally)? */

/* WAL 归档是否已启用（始终启用，或仅在服务器正常运行时启用）？ */
#define XLogArchivingActive() \
	(AssertMacro(XLogArchiveMode == ARCHIVE_MODE_OFF || wal_level >= WAL_LEVEL_REPLICA), XLogArchiveMode > ARCHIVE_MODE_OFF)
/* Is WAL archiving enabled always (even during recovery)? */

/* WAL 归档是否始终启用（包括恢复期间）？ */
#define XLogArchivingAlways() \
	(AssertMacro(XLogArchiveMode == ARCHIVE_MODE_OFF || wal_level >= WAL_LEVEL_REPLICA), XLogArchiveMode == ARCHIVE_MODE_ALWAYS)

/*
 * Is WAL-logging necessary for archival or log-shipping, or can we skip
 * WAL-logging if we fsync() the data before committing instead?
 *
 * 中文翻译：WAL 日志记录对于归档或日志传送来说是必需的吗？或者如果我们在提交之前使用 fsync() 数据，我们可以跳过 WAL 日志记录吗？
 */
#define XLogIsNeeded() (wal_level >= WAL_LEVEL_REPLICA)

/*
 * Is a full-page image needed for hint bit updates?
 *
 * 中文翻译：提示位更新是否需要整页图像？
 *
 * Normally, we don't WAL-log hint bit updates, but if checksums are enabled,
 * we have to protect them against torn page writes.  When you only set
 * individual bits on a page, it's still consistent no matter what combination
 * of the bits make it to disk, but the checksum wouldn't match.  Also WAL-log
 * them if forced by wal_log_hints=on.
 *
 * 中文翻译：通常，我们不会 WAL 记录提示位更新，但如果启用了校验和，我们必须保护它们免受损坏的页面写入的影响。当您仅在页面上设置各个位时，无论位的组合如何进入磁盘，它仍然是一致的，但校验和不匹配。如果 wal_log_hints=on 强制的话，也会对它们进行 WAL 记录。
 */
#define XLogHintBitIsNeeded() (DataChecksumsEnabled() || wal_log_hints)

/* Do we need to WAL-log information required only for Hot Standby and logical replication? */

/* 是否需要写入仅供热备和逻辑复制使用的信息？ */
#define XLogStandbyInfoActive() (wal_level >= WAL_LEVEL_REPLICA)

/* Do we need to WAL-log information required only for logical replication? */

/* 是否需要写入仅供逻辑复制使用的信息？ */
#define XLogLogicalInfoActive() (wal_level >= WAL_LEVEL_LOGICAL)

#ifdef WAL_DEBUG
extern PGDLLIMPORT bool XLOG_DEBUG;
#endif

/*
 * OR-able request flag bits for checkpoints.  The "cause" bits are used only
 * for logging purposes.  Note: the flags must be defined so that it's
 * sensible to OR together request flags arising from different requestors.
 *
 * 中文翻译：检查点的可或请求标志位。 “原因”位仅用于记录目的。注意：必须定义标志，以便将不同请求者产生的请求标志组合在一起是明智的。
 */

/* These directly affect the behavior of CreateCheckPoint and subsidiaries */

/* 这些标志直接影响 CreateCheckPoint 及其相关操作的行为。 */
#define CHECKPOINT_IS_SHUTDOWN	0x0001	/* Checkpoint is for shutdown */

/* 中文翻译：检查点用于关闭 */
#define CHECKPOINT_END_OF_RECOVERY	0x0002	/* Like shutdown checkpoint, but
											 * issued at end of WAL recovery */
#define CHECKPOINT_IMMEDIATE	0x0004	/* Do it without delays */

/* 中文翻译：立即执行 */
#define CHECKPOINT_FORCE		0x0008	/* Force even if no activity */

/* 中文翻译：即使没有活动也强制 */
#define CHECKPOINT_FLUSH_ALL	0x0010	/* Flush all pages, including those
										 * belonging to unlogged tables */
/* These are important to RequestCheckpoint */

/* 这些标志对 RequestCheckpoint 很重要。 */
#define CHECKPOINT_WAIT			0x0020	/* Wait for completion */

/* 中文翻译：等待完成 */
#define CHECKPOINT_REQUESTED	0x0040	/* Checkpoint request has been made */

/* 中文翻译：已发出检查点请求 */
/* These indicate the cause of a checkpoint request */

/* 这些标志表示请求检查点的原因。 */
#define CHECKPOINT_CAUSE_XLOG	0x0080	/* XLOG consumption */

/* 中文翻译：XLOG消耗 */
#define CHECKPOINT_CAUSE_TIME	0x0100	/* Elapsed time */

/* 中文翻译：经过的时间 */

/*
 * Flag bits for the record being inserted, set using XLogSetRecordFlags().
 *
 * 中文翻译：正在插入的记录的标志位，使用 XLogSetRecordFlags() 设置。
 */
#define XLOG_INCLUDE_ORIGIN		0x01	/* include the replication origin */

/* 中文翻译：包含复制源 */
#define XLOG_MARK_UNIMPORTANT	0x02	/* record not important for durability */

/* 中文翻译：记录对于持久性并不重要 */


/* Checkpoint statistics */

/* 检查点统计信息。 */
typedef struct CheckpointStatsData
{
	TimestampTz ckpt_start_t;	/* start of checkpoint */

	/* 中文翻译：检查点开始 */
	TimestampTz ckpt_write_t;	/* start of flushing buffers */

	/* 中文翻译：开始刷新缓冲区 */
	TimestampTz ckpt_sync_t;	/* start of fsyncs */

	/* 中文翻译：fsync 的开始 */
	TimestampTz ckpt_sync_end_t;	/* end of fsyncs */

	/* 中文翻译：fsync 结束 */
	TimestampTz ckpt_end_t;		/* end of checkpoint */

	/* 中文翻译：检查点结束 */

	int			ckpt_bufs_written;	/* # of buffers written */

	/* 中文翻译：写入的缓冲区数 */
	int			ckpt_slru_written;	/* # of SLRU buffers written */

	/* 中文翻译：写入的 SLRU 缓冲区数量 */

	int			ckpt_segs_added;	/* # of new xlog segments created */

	/* 中文翻译：创建的新 xlog 段数 */
	int			ckpt_segs_removed;	/* # of xlog segments deleted */

	/* 中文翻译：已删除的 xlog 段数 */
	int			ckpt_segs_recycled; /* # of xlog segments recycled */

	/* 中文翻译：回收的 xlog 段数 */

	int			ckpt_sync_rels; /* # of relations synced */

	/* 中文翻译：已同步的关系数 */
	uint64		ckpt_longest_sync;	/* Longest sync for one relation */

	/* 中文翻译：一个关系的最长同步 */
	uint64		ckpt_agg_sync_time; /* The sum of all the individual sync
									 * times, which is not necessarily the
									 * same as the total elapsed time for the
									 * entire sync phase. */
} CheckpointStatsData;

extern PGDLLIMPORT CheckpointStatsData CheckpointStats;

/*
 * GetWALAvailability return codes
 *
 * 中文翻译：GetWALAvailability 返回代码
 */
typedef enum WALAvailability
{
	WALAVAIL_INVALID_LSN,		/* parameter error */

	/* 中文翻译：参数错误 */
	WALAVAIL_RESERVED,			/* WAL segment is within max_wal_size */

	/* 中文翻译：WAL 段在 max_wal_size 范围内 */
	WALAVAIL_EXTENDED,			/* WAL segment is reserved by a slot or
								 * wal_keep_size */
	WALAVAIL_UNRESERVED,		/* no longer reserved, but not removed yet */

	/* 中文翻译：不再保留，但尚未删除 */
	WALAVAIL_REMOVED,			/* WAL segment has been removed */

	/* 中文翻译：WAL 段已被删除 */
} WALAvailability;

struct XLogRecData;
struct XLogReaderState;

/*
 * Function: XLogInsertRecord.
 * Purpose: Performs the WAL operation represented by xlog insert record.
 * Core flow: It prepares the WAL context, performs the requested write, recovery, or control action, and exposes the result.
 *
 * 函数：XLogInsertRecord。
 * 作用：执行 xlog insert record 所表示的 WAL 操作。
 * 核心流程：它准备 WAL 上下文，执行请求的写入、恢复或控制操作，并提供结果。
 */
extern XLogRecPtr XLogInsertRecord(struct XLogRecData *rdata,
								   XLogRecPtr fpw_lsn,
								   uint8 flags,
								   int num_fpi,
								   bool topxid_included);
/*
 * Function: XLogFlush.
 * Purpose: Performs the WAL operation represented by xlog flush.
 * Core flow: It prepares the WAL context, performs the requested write, recovery, or control action, and exposes the result.
 *
 * 函数：XLogFlush。
 * 作用：执行 xlog flush 所表示的 WAL 操作。
 * 核心流程：它准备 WAL 上下文，执行请求的写入、恢复或控制操作，并提供结果。
 */
extern void XLogFlush(XLogRecPtr record);
/*
 * Function: XLogBackgroundFlush.
 * Purpose: Performs the WAL operation represented by xlog background flush.
 * Core flow: It prepares the WAL context, performs the requested write, recovery, or control action, and exposes the result.
 *
 * 函数：XLogBackgroundFlush。
 * 作用：执行 xlog background flush 所表示的 WAL 操作。
 * 核心流程：它准备 WAL 上下文，执行请求的写入、恢复或控制操作，并提供结果。
 */
extern bool XLogBackgroundFlush(void);
/*
 * Function: XLogNeedsFlush.
 * Purpose: Performs the WAL operation represented by xlog needs flush.
 * Core flow: It prepares the WAL context, performs the requested write, recovery, or control action, and exposes the result.
 *
 * 函数：XLogNeedsFlush。
 * 作用：执行 xlog needs flush 所表示的 WAL 操作。
 * 核心流程：它准备 WAL 上下文，执行请求的写入、恢复或控制操作，并提供结果。
 */
extern bool XLogNeedsFlush(XLogRecPtr record);
/*
 * Function: XLogFileInit.
 * Purpose: Performs the WAL operation represented by xlog file init.
 * Core flow: It prepares the WAL context, performs the requested write, recovery, or control action, and exposes the result.
 *
 * 函数：XLogFileInit。
 * 作用：执行 xlog file init 所表示的 WAL 操作。
 * 核心流程：它准备 WAL 上下文，执行请求的写入、恢复或控制操作，并提供结果。
 */
extern int	XLogFileInit(XLogSegNo logsegno, TimeLineID logtli);
/*
 * Function: XLogFileOpen.
 * Purpose: Performs the WAL operation represented by xlog file open.
 * Core flow: It prepares the WAL context, performs the requested write, recovery, or control action, and exposes the result.
 *
 * 函数：XLogFileOpen。
 * 作用：执行 xlog file open 所表示的 WAL 操作。
 * 核心流程：它准备 WAL 上下文，执行请求的写入、恢复或控制操作，并提供结果。
 */
extern int	XLogFileOpen(XLogSegNo segno, TimeLineID tli);

/*
 * Function: CheckXLogRemoved.
 * Purpose: Obtains or checks the WAL state represented by check xlog removed.
 * Core flow: It inspects the supplied position, identifier, or shared WAL state and returns the derived result.
 *
 * 函数：CheckXLogRemoved。
 * 作用：获取或检查 check xlog removed 所表示的 WAL 状态。
 * 核心流程：它检查给定位置、标识符或共享 WAL 状态，并返回推导结果。
 */
extern void CheckXLogRemoved(XLogSegNo segno, TimeLineID tli);
/*
 * Function: XLogGetLastRemovedSegno.
 * Purpose: Performs the WAL operation represented by xlog get last removed segno.
 * Core flow: It prepares the WAL context, performs the requested write, recovery, or control action, and exposes the result.
 *
 * 函数：XLogGetLastRemovedSegno。
 * 作用：执行 xlog get last removed segno 所表示的 WAL 操作。
 * 核心流程：它准备 WAL 上下文，执行请求的写入、恢复或控制操作，并提供结果。
 */
extern XLogSegNo XLogGetLastRemovedSegno(void);
/*
 * Function: XLogGetOldestSegno.
 * Purpose: Performs the WAL operation represented by xlog get oldest segno.
 * Core flow: It prepares the WAL context, performs the requested write, recovery, or control action, and exposes the result.
 *
 * 函数：XLogGetOldestSegno。
 * 作用：执行 xlog get oldest segno 所表示的 WAL 操作。
 * 核心流程：它准备 WAL 上下文，执行请求的写入、恢复或控制操作，并提供结果。
 */
extern XLogSegNo XLogGetOldestSegno(TimeLineID tli);
/*
 * Function: XLogSetAsyncXactLSN.
 * Purpose: Performs the WAL operation represented by xlog set async xact lsn.
 * Core flow: It prepares the WAL context, performs the requested write, recovery, or control action, and exposes the result.
 *
 * 函数：XLogSetAsyncXactLSN。
 * 作用：执行 xlog set async xact lsn 所表示的 WAL 操作。
 * 核心流程：它准备 WAL 上下文，执行请求的写入、恢复或控制操作，并提供结果。
 */
extern void XLogSetAsyncXactLSN(XLogRecPtr asyncXactLSN);
/*
 * Function: XLogSetReplicationSlotMinimumLSN.
 * Purpose: Performs the WAL operation represented by xlog set replication slot minimum lsn.
 * Core flow: It prepares the WAL context, performs the requested write, recovery, or control action, and exposes the result.
 *
 * 函数：XLogSetReplicationSlotMinimumLSN。
 * 作用：执行 xlog set replication slot minimum lsn 所表示的 WAL 操作。
 * 核心流程：它准备 WAL 上下文，执行请求的写入、恢复或控制操作，并提供结果。
 */
extern void XLogSetReplicationSlotMinimumLSN(XLogRecPtr lsn);
/*
 * Function: XLogGetReplicationSlotMinimumLSN.
 * Purpose: Performs the WAL operation represented by xlog get replication slot minimum lsn.
 * Core flow: It prepares the WAL context, performs the requested write, recovery, or control action, and exposes the result.
 *
 * 函数：XLogGetReplicationSlotMinimumLSN。
 * 作用：执行 xlog get replication slot minimum lsn 所表示的 WAL 操作。
 * 核心流程：它准备 WAL 上下文，执行请求的写入、恢复或控制操作，并提供结果。
 */
extern XLogRecPtr XLogGetReplicationSlotMinimumLSN(void);

/*
 * Function: xlog_redo.
 * Purpose: Performs the WAL operation represented by xlog redo.
 * Core flow: It prepares the WAL context, performs the requested write, recovery, or control action, and exposes the result.
 *
 * 函数：xlog_redo。
 * 作用：执行 xlog redo 所表示的 WAL 操作。
 * 核心流程：它准备 WAL 上下文，执行请求的写入、恢复或控制操作，并提供结果。
 */
extern void xlog_redo(struct XLogReaderState *record);
/*
 * Function: xlog_desc.
 * Purpose: Performs the WAL operation represented by xlog desc.
 * Core flow: It prepares the WAL context, performs the requested write, recovery, or control action, and exposes the result.
 *
 * 函数：xlog_desc。
 * 作用：执行 xlog desc 所表示的 WAL 操作。
 * 核心流程：它准备 WAL 上下文，执行请求的写入、恢复或控制操作，并提供结果。
 */
extern void xlog_desc(StringInfo buf, struct XLogReaderState *record);
/*
 * Function: xlog_identify.
 * Purpose: Performs the WAL operation represented by xlog identify.
 * Core flow: It prepares the WAL context, performs the requested write, recovery, or control action, and exposes the result.
 *
 * 函数：xlog_identify。
 * 作用：执行 xlog identify 所表示的 WAL 操作。
 * 核心流程：它准备 WAL 上下文，执行请求的写入、恢复或控制操作，并提供结果。
 */
extern const char *xlog_identify(uint8 info);

/*
 * Function: issue_xlog_fsync.
 * Purpose: Obtains or checks the WAL state represented by issue xlog fsync.
 * Core flow: It inspects the supplied position, identifier, or shared WAL state and returns the derived result.
 *
 * 函数：issue_xlog_fsync。
 * 作用：获取或检查 issue xlog fsync 所表示的 WAL 状态。
 * 核心流程：它检查给定位置、标识符或共享 WAL 状态，并返回推导结果。
 */
extern void issue_xlog_fsync(int fd, XLogSegNo segno, TimeLineID tli);

/*
 * Function: RecoveryInProgress.
 * Purpose: Performs the WAL operation represented by recovery in progress.
 * Core flow: It uses the supplied context to process WAL state and returns or records the result.
 *
 * 函数：RecoveryInProgress。
 * 作用：执行 recovery in progress 所表示的 WAL 操作。
 * 核心流程：它使用给定上下文处理 WAL 状态，并返回或记录结果。
 */
extern bool RecoveryInProgress(void);
/*
 * Function: GetRecoveryState.
 * Purpose: Obtains or checks the WAL state represented by get recovery state.
 * Core flow: It inspects the supplied position, identifier, or shared WAL state and returns the derived result.
 *
 * 函数：GetRecoveryState。
 * 作用：获取或检查 get recovery state 所表示的 WAL 状态。
 * 核心流程：它检查给定位置、标识符或共享 WAL 状态，并返回推导结果。
 */
extern RecoveryState GetRecoveryState(void);
/*
 * Function: XLogInsertAllowed.
 * Purpose: Performs the WAL operation represented by xlog insert allowed.
 * Core flow: It prepares the WAL context, performs the requested write, recovery, or control action, and exposes the result.
 *
 * 函数：XLogInsertAllowed。
 * 作用：执行 xlog insert allowed 所表示的 WAL 操作。
 * 核心流程：它准备 WAL 上下文，执行请求的写入、恢复或控制操作，并提供结果。
 */
extern bool XLogInsertAllowed(void);
/*
 * Function: GetXLogInsertRecPtr.
 * Purpose: Obtains or checks the WAL state represented by get xlog insert rec ptr.
 * Core flow: It inspects the supplied position, identifier, or shared WAL state and returns the derived result.
 *
 * 函数：GetXLogInsertRecPtr。
 * 作用：获取或检查 get xlog insert rec ptr 所表示的 WAL 状态。
 * 核心流程：它检查给定位置、标识符或共享 WAL 状态，并返回推导结果。
 */
extern XLogRecPtr GetXLogInsertRecPtr(void);
/*
 * Function: GetXLogWriteRecPtr.
 * Purpose: Obtains or checks the WAL state represented by get xlog write rec ptr.
 * Core flow: It inspects the supplied position, identifier, or shared WAL state and returns the derived result.
 *
 * 函数：GetXLogWriteRecPtr。
 * 作用：获取或检查 get xlog write rec ptr 所表示的 WAL 状态。
 * 核心流程：它检查给定位置、标识符或共享 WAL 状态，并返回推导结果。
 */
extern XLogRecPtr GetXLogWriteRecPtr(void);

/*
 * Function: GetSystemIdentifier.
 * Purpose: Obtains or checks the WAL state represented by get system identifier.
 * Core flow: It inspects the supplied position, identifier, or shared WAL state and returns the derived result.
 *
 * 函数：GetSystemIdentifier。
 * 作用：获取或检查 get system identifier 所表示的 WAL 状态。
 * 核心流程：它检查给定位置、标识符或共享 WAL 状态，并返回推导结果。
 */
extern uint64 GetSystemIdentifier(void);
/*
 * Function: GetMockAuthenticationNonce.
 * Purpose: Obtains or checks the WAL state represented by get mock authentication nonce.
 * Core flow: It inspects the supplied position, identifier, or shared WAL state and returns the derived result.
 *
 * 函数：GetMockAuthenticationNonce。
 * 作用：获取或检查 get mock authentication nonce 所表示的 WAL 状态。
 * 核心流程：它检查给定位置、标识符或共享 WAL 状态，并返回推导结果。
 */
extern char *GetMockAuthenticationNonce(void);
/*
 * Function: DataChecksumsEnabled.
 * Purpose: Obtains or checks the WAL state represented by data checksums enabled.
 * Core flow: It inspects the supplied position, identifier, or shared WAL state and returns the derived result.
 *
 * 函数：DataChecksumsEnabled。
 * 作用：获取或检查 data checksums enabled 所表示的 WAL 状态。
 * 核心流程：它检查给定位置、标识符或共享 WAL 状态，并返回推导结果。
 */
extern bool DataChecksumsEnabled(void);
/*
 * Function: GetDefaultCharSignedness.
 * Purpose: Obtains or checks the WAL state represented by get default char signedness.
 * Core flow: It inspects the supplied position, identifier, or shared WAL state and returns the derived result.
 *
 * 函数：GetDefaultCharSignedness。
 * 作用：获取或检查 get default char signedness 所表示的 WAL 状态。
 * 核心流程：它检查给定位置、标识符或共享 WAL 状态，并返回推导结果。
 */
extern bool GetDefaultCharSignedness(void);
/*
 * Function: GetFakeLSNForUnloggedRel.
 * Purpose: Obtains or checks the WAL state represented by get fake lsnfor unlogged rel.
 * Core flow: It inspects the supplied position, identifier, or shared WAL state and returns the derived result.
 *
 * 函数：GetFakeLSNForUnloggedRel。
 * 作用：获取或检查 get fake lsnfor unlogged rel 所表示的 WAL 状态。
 * 核心流程：它检查给定位置、标识符或共享 WAL 状态，并返回推导结果。
 */
extern XLogRecPtr GetFakeLSNForUnloggedRel(void);
/*
 * Function: XLOGShmemSize.
 * Purpose: Performs the WAL operation represented by xlogshmem size.
 * Core flow: It prepares the WAL context, performs the requested write, recovery, or control action, and exposes the result.
 *
 * 函数：XLOGShmemSize。
 * 作用：执行 xlogshmem size 所表示的 WAL 操作。
 * 核心流程：它准备 WAL 上下文，执行请求的写入、恢复或控制操作，并提供结果。
 */
extern Size XLOGShmemSize(void);
/*
 * Function: XLOGShmemInit.
 * Purpose: Performs the WAL operation represented by xlogshmem init.
 * Core flow: It prepares the WAL context, performs the requested write, recovery, or control action, and exposes the result.
 *
 * 函数：XLOGShmemInit。
 * 作用：执行 xlogshmem init 所表示的 WAL 操作。
 * 核心流程：它准备 WAL 上下文，执行请求的写入、恢复或控制操作，并提供结果。
 */
extern void XLOGShmemInit(void);
/*
 * Function: BootStrapXLOG.
 * Purpose: Performs the WAL operation represented by boot strap xlog.
 * Core flow: It prepares the WAL context, performs the requested write, recovery, or control action, and exposes the result.
 *
 * 函数：BootStrapXLOG。
 * 作用：执行 boot strap xlog 所表示的 WAL 操作。
 * 核心流程：它准备 WAL 上下文，执行请求的写入、恢复或控制操作，并提供结果。
 */
extern void BootStrapXLOG(uint32 data_checksum_version);
/*
 * Function: InitializeWalConsistencyChecking.
 * Purpose: Performs the WAL operation represented by initialize wal consistency checking.
 * Core flow: It prepares the WAL context, performs the requested write, recovery, or control action, and exposes the result.
 *
 * 函数：InitializeWalConsistencyChecking。
 * 作用：执行 initialize wal consistency checking 所表示的 WAL 操作。
 * 核心流程：它准备 WAL 上下文，执行请求的写入、恢复或控制操作，并提供结果。
 */
extern void InitializeWalConsistencyChecking(void);
/*
 * Function: LocalProcessControlFile.
 * Purpose: Performs the WAL operation represented by local process control file.
 * Core flow: It prepares the WAL context, performs the requested write, recovery, or control action, and exposes the result.
 *
 * 函数：LocalProcessControlFile。
 * 作用：执行 local process control file 所表示的 WAL 操作。
 * 核心流程：它准备 WAL 上下文，执行请求的写入、恢复或控制操作，并提供结果。
 */
extern void LocalProcessControlFile(bool reset);
/*
 * Function: GetActiveWalLevelOnStandby.
 * Purpose: Obtains or checks the WAL state represented by get active wal level on standby.
 * Core flow: It inspects the supplied position, identifier, or shared WAL state and returns the derived result.
 *
 * 函数：GetActiveWalLevelOnStandby。
 * 作用：获取或检查 get active wal level on standby 所表示的 WAL 状态。
 * 核心流程：它检查给定位置、标识符或共享 WAL 状态，并返回推导结果。
 */
extern WalLevel GetActiveWalLevelOnStandby(void);
/*
 * Function: StartupXLOG.
 * Purpose: Performs the WAL operation represented by startup xlog.
 * Core flow: It prepares the WAL context, performs the requested write, recovery, or control action, and exposes the result.
 *
 * 函数：StartupXLOG。
 * 作用：执行 startup xlog 所表示的 WAL 操作。
 * 核心流程：它准备 WAL 上下文，执行请求的写入、恢复或控制操作，并提供结果。
 */
extern void StartupXLOG(void);
/*
 * Function: ShutdownXLOG.
 * Purpose: Completes or releases the WAL work represented by shutdown xlog.
 * Core flow: It finishes the required processing and releases or finalizes associated WAL state.
 *
 * 函数：ShutdownXLOG。
 * 作用：完成或释放 shutdown xlog 所表示的 WAL 工作。
 * 核心流程：它完成所需处理，并释放或最终确定关联 WAL 状态。
 */
extern void ShutdownXLOG(int code, Datum arg);
/*
 * Function: CreateCheckPoint.
 * Purpose: Performs the WAL operation represented by create check point.
 * Core flow: It prepares the WAL context, performs the requested write, recovery, or control action, and exposes the result.
 *
 * 函数：CreateCheckPoint。
 * 作用：执行 create check point 所表示的 WAL 操作。
 * 核心流程：它准备 WAL 上下文，执行请求的写入、恢复或控制操作，并提供结果。
 */
extern bool CreateCheckPoint(int flags);
/*
 * Function: CreateRestartPoint.
 * Purpose: Performs the WAL operation represented by create restart point.
 * Core flow: It prepares the WAL context, performs the requested write, recovery, or control action, and exposes the result.
 *
 * 函数：CreateRestartPoint。
 * 作用：执行 create restart point 所表示的 WAL 操作。
 * 核心流程：它准备 WAL 上下文，执行请求的写入、恢复或控制操作，并提供结果。
 */
extern bool CreateRestartPoint(int flags);
/*
 * Function: GetWALAvailability.
 * Purpose: Obtains or checks the WAL state represented by get walavailability.
 * Core flow: It inspects the supplied position, identifier, or shared WAL state and returns the derived result.
 *
 * 函数：GetWALAvailability。
 * 作用：获取或检查 get walavailability 所表示的 WAL 状态。
 * 核心流程：它检查给定位置、标识符或共享 WAL 状态，并返回推导结果。
 */
extern WALAvailability GetWALAvailability(XLogRecPtr targetLSN);
/*
 * Function: XLogPutNextOid.
 * Purpose: Performs the WAL operation represented by xlog put next oid.
 * Core flow: It prepares the WAL context, performs the requested write, recovery, or control action, and exposes the result.
 *
 * 函数：XLogPutNextOid。
 * 作用：执行 xlog put next oid 所表示的 WAL 操作。
 * 核心流程：它准备 WAL 上下文，执行请求的写入、恢复或控制操作，并提供结果。
 */
extern void XLogPutNextOid(Oid nextOid);
/*
 * Function: XLogRestorePoint.
 * Purpose: Performs the WAL operation represented by xlog restore point.
 * Core flow: It prepares the WAL context, performs the requested write, recovery, or control action, and exposes the result.
 *
 * 函数：XLogRestorePoint。
 * 作用：执行 xlog restore point 所表示的 WAL 操作。
 * 核心流程：它准备 WAL 上下文，执行请求的写入、恢复或控制操作，并提供结果。
 */
extern XLogRecPtr XLogRestorePoint(const char *rpName);
/*
 * Function: UpdateFullPageWrites.
 * Purpose: Updates the WAL state represented by update full page writes.
 * Core flow: It validates the request, changes the relevant WAL metadata or files, and keeps the state consistent.
 *
 * 函数：UpdateFullPageWrites。
 * 作用：更新 update full page writes 所表示的 WAL 状态。
 * 核心流程：它校验请求，变更相关 WAL 元数据或文件，并保持状态一致。
 */
extern void UpdateFullPageWrites(void);
/*
 * Function: GetFullPageWriteInfo.
 * Purpose: Obtains or checks the WAL state represented by get full page write info.
 * Core flow: It inspects the supplied position, identifier, or shared WAL state and returns the derived result.
 *
 * 函数：GetFullPageWriteInfo。
 * 作用：获取或检查 get full page write info 所表示的 WAL 状态。
 * 核心流程：它检查给定位置、标识符或共享 WAL 状态，并返回推导结果。
 */
extern void GetFullPageWriteInfo(XLogRecPtr *RedoRecPtr_p, bool *doPageWrites_p);
/*
 * Function: GetRedoRecPtr.
 * Purpose: Obtains or checks the WAL state represented by get redo rec ptr.
 * Core flow: It inspects the supplied position, identifier, or shared WAL state and returns the derived result.
 *
 * 函数：GetRedoRecPtr。
 * 作用：获取或检查 get redo rec ptr 所表示的 WAL 状态。
 * 核心流程：它检查给定位置、标识符或共享 WAL 状态，并返回推导结果。
 */
extern XLogRecPtr GetRedoRecPtr(void);
/*
 * Function: GetInsertRecPtr.
 * Purpose: Obtains or checks the WAL state represented by get insert rec ptr.
 * Core flow: It inspects the supplied position, identifier, or shared WAL state and returns the derived result.
 *
 * 函数：GetInsertRecPtr。
 * 作用：获取或检查 get insert rec ptr 所表示的 WAL 状态。
 * 核心流程：它检查给定位置、标识符或共享 WAL 状态，并返回推导结果。
 */
extern XLogRecPtr GetInsertRecPtr(void);
/*
 * Function: GetFlushRecPtr.
 * Purpose: Obtains or checks the WAL state represented by get flush rec ptr.
 * Core flow: It inspects the supplied position, identifier, or shared WAL state and returns the derived result.
 *
 * 函数：GetFlushRecPtr。
 * 作用：获取或检查 get flush rec ptr 所表示的 WAL 状态。
 * 核心流程：它检查给定位置、标识符或共享 WAL 状态，并返回推导结果。
 */
extern XLogRecPtr GetFlushRecPtr(TimeLineID *insertTLI);
/*
 * Function: GetWALInsertionTimeLine.
 * Purpose: Obtains or checks the WAL state represented by get walinsertion time line.
 * Core flow: It inspects the supplied position, identifier, or shared WAL state and returns the derived result.
 *
 * 函数：GetWALInsertionTimeLine。
 * 作用：获取或检查 get walinsertion time line 所表示的 WAL 状态。
 * 核心流程：它检查给定位置、标识符或共享 WAL 状态，并返回推导结果。
 */
extern TimeLineID GetWALInsertionTimeLine(void);
/*
 * Function: GetWALInsertionTimeLineIfSet.
 * Purpose: Obtains or checks the WAL state represented by get walinsertion time line if set.
 * Core flow: It inspects the supplied position, identifier, or shared WAL state and returns the derived result.
 *
 * 函数：GetWALInsertionTimeLineIfSet。
 * 作用：获取或检查 get walinsertion time line if set 所表示的 WAL 状态。
 * 核心流程：它检查给定位置、标识符或共享 WAL 状态，并返回推导结果。
 */
extern TimeLineID GetWALInsertionTimeLineIfSet(void);
/*
 * Function: GetLastImportantRecPtr.
 * Purpose: Obtains or checks the WAL state represented by get last important rec ptr.
 * Core flow: It inspects the supplied position, identifier, or shared WAL state and returns the derived result.
 *
 * 函数：GetLastImportantRecPtr。
 * 作用：获取或检查 get last important rec ptr 所表示的 WAL 状态。
 * 核心流程：它检查给定位置、标识符或共享 WAL 状态，并返回推导结果。
 */
extern XLogRecPtr GetLastImportantRecPtr(void);

/*
 * Function: SetWalWriterSleeping.
 * Purpose: Updates the WAL state represented by set wal writer sleeping.
 * Core flow: It validates the request, changes the relevant WAL metadata or files, and keeps the state consistent.
 *
 * 函数：SetWalWriterSleeping。
 * 作用：更新 set wal writer sleeping 所表示的 WAL 状态。
 * 核心流程：它校验请求，变更相关 WAL 元数据或文件，并保持状态一致。
 */
extern void SetWalWriterSleeping(bool sleeping);

/*
 * Function: WALReadFromBuffers.
 * Purpose: Performs the WAL operation represented by walread from buffers.
 * Core flow: It uses the supplied context to process WAL state and returns or records the result.
 *
 * 函数：WALReadFromBuffers。
 * 作用：执行 walread from buffers 所表示的 WAL 操作。
 * 核心流程：它使用给定上下文处理 WAL 状态，并返回或记录结果。
 */
extern Size WALReadFromBuffers(char *dstbuf, XLogRecPtr startptr, Size count,
							   TimeLineID tli);

/*
 * Routines used by xlogrecovery.c to call back into xlog.c during recovery.
 *
 * 中文翻译：xlogrecovery.c 在恢复期间回调 xlog.c 所使用的例程。
 */
/*
 * Function: RemoveNonParentXlogFiles.
 * Purpose: Updates the WAL state represented by remove non parent xlog files.
 * Core flow: It validates the request, changes the relevant WAL metadata or files, and keeps the state consistent.
 *
 * 函数：RemoveNonParentXlogFiles。
 * 作用：更新 remove non parent xlog files 所表示的 WAL 状态。
 * 核心流程：它校验请求，变更相关 WAL 元数据或文件，并保持状态一致。
 */
extern void RemoveNonParentXlogFiles(XLogRecPtr switchpoint, TimeLineID newTLI);
/*
 * Function: XLogCheckpointNeeded.
 * Purpose: Performs the WAL operation represented by xlog checkpoint needed.
 * Core flow: It prepares the WAL context, performs the requested write, recovery, or control action, and exposes the result.
 *
 * 函数：XLogCheckpointNeeded。
 * 作用：执行 xlog checkpoint needed 所表示的 WAL 操作。
 * 核心流程：它准备 WAL 上下文，执行请求的写入、恢复或控制操作，并提供结果。
 */
extern bool XLogCheckpointNeeded(XLogSegNo new_segno);
/*
 * Function: SwitchIntoArchiveRecovery.
 * Purpose: Updates the WAL state represented by switch into archive recovery.
 * Core flow: It validates the request, changes the relevant WAL metadata or files, and keeps the state consistent.
 *
 * 函数：SwitchIntoArchiveRecovery。
 * 作用：更新 switch into archive recovery 所表示的 WAL 状态。
 * 核心流程：它校验请求，变更相关 WAL 元数据或文件，并保持状态一致。
 */
extern void SwitchIntoArchiveRecovery(XLogRecPtr EndRecPtr, TimeLineID replayTLI);
/*
 * Function: ReachedEndOfBackup.
 * Purpose: Performs the WAL operation represented by reached end of backup.
 * Core flow: It uses the supplied context to process WAL state and returns or records the result.
 *
 * 函数：ReachedEndOfBackup。
 * 作用：执行 reached end of backup 所表示的 WAL 操作。
 * 核心流程：它使用给定上下文处理 WAL 状态，并返回或记录结果。
 */
extern void ReachedEndOfBackup(XLogRecPtr EndRecPtr, TimeLineID tli);
/*
 * Function: SetInstallXLogFileSegmentActive.
 * Purpose: Updates the WAL state represented by set install xlog file segment active.
 * Core flow: It validates the request, changes the relevant WAL metadata or files, and keeps the state consistent.
 *
 * 函数：SetInstallXLogFileSegmentActive。
 * 作用：更新 set install xlog file segment active 所表示的 WAL 状态。
 * 核心流程：它校验请求，变更相关 WAL 元数据或文件，并保持状态一致。
 */
extern void SetInstallXLogFileSegmentActive(void);
/*
 * Function: IsInstallXLogFileSegmentActive.
 * Purpose: Obtains or checks the WAL state represented by is install xlog file segment active.
 * Core flow: It inspects the supplied position, identifier, or shared WAL state and returns the derived result.
 *
 * 函数：IsInstallXLogFileSegmentActive。
 * 作用：获取或检查 is install xlog file segment active 所表示的 WAL 状态。
 * 核心流程：它检查给定位置、标识符或共享 WAL 状态，并返回推导结果。
 */
extern bool IsInstallXLogFileSegmentActive(void);
/*
 * Function: ResetInstallXLogFileSegmentActive.
 * Purpose: Updates the WAL state represented by reset install xlog file segment active.
 * Core flow: It validates the request, changes the relevant WAL metadata or files, and keeps the state consistent.
 *
 * 函数：ResetInstallXLogFileSegmentActive。
 * 作用：更新 reset install xlog file segment active 所表示的 WAL 状态。
 * 核心流程：它校验请求，变更相关 WAL 元数据或文件，并保持状态一致。
 */
extern void ResetInstallXLogFileSegmentActive(void);
/*
 * Function: XLogShutdownWalRcv.
 * Purpose: Performs the WAL operation represented by xlog shutdown wal rcv.
 * Core flow: It prepares the WAL context, performs the requested write, recovery, or control action, and exposes the result.
 *
 * 函数：XLogShutdownWalRcv。
 * 作用：执行 xlog shutdown wal rcv 所表示的 WAL 操作。
 * 核心流程：它准备 WAL 上下文，执行请求的写入、恢复或控制操作，并提供结果。
 */
extern void XLogShutdownWalRcv(void);

/*
 * Routines to start, stop, and get status of a base backup.
 *
 * 中文翻译：启动、停止和获取基本备份状态的例程。
 */

/*
 * Session-level status of base backups
 *
 * 中文翻译：基础备份的会话级状态
 *
 * This is used in parallel with the shared memory status to control parallel
 * execution of base backup functions for a given session, be it a backend
 * dedicated to replication or a normal backend connected to a database. The
 * update of the session-level status happens at the same time as the shared
 * memory counters to keep a consistent global and local state of the backups
 * running.
 *
 * 中文翻译：这与共享内存状态并行使用，以控制给定会话的基本备份功能的并行执行，无论是专用于复制的后端还是连接到数据库的普通后端。会话级状态的更新与共享内存计数器同时发生，以保持备份运行的全局和本地状态一致。
 */
typedef enum SessionBackupState
{
	SESSION_BACKUP_NONE,
	SESSION_BACKUP_RUNNING,
} SessionBackupState;

/*
 * Function: do_pg_backup_start.
 * Purpose: Performs the WAL operation represented by do pg backup start.
 * Core flow: It uses the supplied context to process WAL state and returns or records the result.
 *
 * 函数：do_pg_backup_start。
 * 作用：执行 do pg backup start 所表示的 WAL 操作。
 * 核心流程：它使用给定上下文处理 WAL 状态，并返回或记录结果。
 */
extern void do_pg_backup_start(const char *backupidstr, bool fast,
							   List **tablespaces, BackupState *state,
							   StringInfo tblspcmapfile);
/*
 * Function: do_pg_backup_stop.
 * Purpose: Performs the WAL operation represented by do pg backup stop.
 * Core flow: It uses the supplied context to process WAL state and returns or records the result.
 *
 * 函数：do_pg_backup_stop。
 * 作用：执行 do pg backup stop 所表示的 WAL 操作。
 * 核心流程：它使用给定上下文处理 WAL 状态，并返回或记录结果。
 */
extern void do_pg_backup_stop(BackupState *state, bool waitforarchive);
/*
 * Function: do_pg_abort_backup.
 * Purpose: Performs the WAL operation represented by do pg abort backup.
 * Core flow: It uses the supplied context to process WAL state and returns or records the result.
 *
 * 函数：do_pg_abort_backup。
 * 作用：执行 do pg abort backup 所表示的 WAL 操作。
 * 核心流程：它使用给定上下文处理 WAL 状态，并返回或记录结果。
 */
extern void do_pg_abort_backup(int code, Datum arg);
/*
 * Function: register_persistent_abort_backup_handler.
 * Purpose: Performs the WAL operation represented by register persistent abort backup handler.
 * Core flow: It prepares the WAL context, performs the requested write, recovery, or control action, and exposes the result.
 *
 * 函数：register_persistent_abort_backup_handler。
 * 作用：执行 register persistent abort backup handler 所表示的 WAL 操作。
 * 核心流程：它准备 WAL 上下文，执行请求的写入、恢复或控制操作，并提供结果。
 */
extern void register_persistent_abort_backup_handler(void);
/*
 * Function: get_backup_status.
 * Purpose: Obtains or checks the WAL state represented by get backup status.
 * Core flow: It inspects the supplied position, identifier, or shared WAL state and returns the derived result.
 *
 * 函数：get_backup_status。
 * 作用：获取或检查 get backup status 所表示的 WAL 状态。
 * 核心流程：它检查给定位置、标识符或共享 WAL 状态，并返回推导结果。
 */
extern SessionBackupState get_backup_status(void);

/* File path names (all relative to $PGDATA) */

/* 文件路径名（均相对于 $PGDATA）。 */
#define RECOVERY_SIGNAL_FILE	"recovery.signal"
#define STANDBY_SIGNAL_FILE		"standby.signal"
#define BACKUP_LABEL_FILE		"backup_label"
#define BACKUP_LABEL_OLD		"backup_label.old"

#define TABLESPACE_MAP			"tablespace_map"
#define TABLESPACE_MAP_OLD		"tablespace_map.old"

/* files to signal promotion to primary */

/* 用于通知提升为主库的文件。 */
#define PROMOTE_SIGNAL_FILE		"promote"

#endif							/* XLOG_H */

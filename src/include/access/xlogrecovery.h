/*
 * xlogrecovery.h
 *
 * 中文翻译：xlogrecovery.h
 *
 * Functions for WAL recovery and standby mode
 *
 * 中文翻译：WAL恢复和待机模式的函数
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/xlogrecovery.h
 *
 * 中文翻译：src/include/access/xlogrecovery.h
 */
#ifndef XLOGRECOVERY_H
#define XLOGRECOVERY_H

#include "access/xlogreader.h"
#include "catalog/pg_control.h"
#include "lib/stringinfo.h"
#include "utils/timestamp.h"

/*
 * Recovery target type.
 * Only set during a Point in Time recovery, not when in standby mode.
 *
 * 中文翻译：恢复目标类型。仅在时间点恢复期间设置，而不是在待机模式下设置。
 */
typedef enum
{
	RECOVERY_TARGET_UNSET,
	RECOVERY_TARGET_XID,
	RECOVERY_TARGET_TIME,
	RECOVERY_TARGET_NAME,
	RECOVERY_TARGET_LSN,
	RECOVERY_TARGET_IMMEDIATE,
} RecoveryTargetType;

/*
 * Recovery target TimeLine goal
 *
 * 中文翻译：恢复目标 时间线目标
 */
typedef enum
{
	RECOVERY_TARGET_TIMELINE_CONTROLFILE,
	RECOVERY_TARGET_TIMELINE_LATEST,
	RECOVERY_TARGET_TIMELINE_NUMERIC,
} RecoveryTargetTimeLineGoal;

/* Recovery pause states */

/* 恢复暂停状态。 */
typedef enum RecoveryPauseState
{
	RECOVERY_NOT_PAUSED,		/* pause not requested */

	/* 未请求暂停。 */
	RECOVERY_PAUSE_REQUESTED,	/* pause requested, but not yet paused */

	/* 已请求暂停，但尚未暂停。 */
	RECOVERY_PAUSED,			/* recovery is paused */

	/* 恢复已暂停。 */
} RecoveryPauseState;

/* User-settable GUC parameters */

/* 用户可设置的 GUC 参数。 */
extern PGDLLIMPORT bool recoveryTargetInclusive;
extern PGDLLIMPORT int recoveryTargetAction;
extern PGDLLIMPORT int recovery_min_apply_delay;
extern PGDLLIMPORT char *PrimaryConnInfo;
extern PGDLLIMPORT char *PrimarySlotName;
extern PGDLLIMPORT char *recoveryRestoreCommand;
extern PGDLLIMPORT char *recoveryEndCommand;
extern PGDLLIMPORT char *archiveCleanupCommand;

/* indirectly set via GUC system */

/* 通过 GUC 系统间接设置。 */
extern PGDLLIMPORT TransactionId recoveryTargetXid;
extern PGDLLIMPORT char *recovery_target_time_string;
extern PGDLLIMPORT TimestampTz recoveryTargetTime;
extern PGDLLIMPORT const char *recoveryTargetName;
extern PGDLLIMPORT XLogRecPtr recoveryTargetLSN;
extern PGDLLIMPORT RecoveryTargetType recoveryTarget;
extern PGDLLIMPORT bool wal_receiver_create_temp_slot;
extern PGDLLIMPORT RecoveryTargetTimeLineGoal recoveryTargetTimeLineGoal;
extern PGDLLIMPORT TimeLineID recoveryTargetTLIRequested;
extern PGDLLIMPORT TimeLineID recoveryTargetTLI;

/* Have we already reached a consistent database state? */

/* 是否已经达到一致的数据库状态？ */
extern PGDLLIMPORT bool reachedConsistency;

/* Are we currently in standby mode? */

/* 当前是否处于备用模式？ */
extern PGDLLIMPORT bool StandbyMode;

/*
 * Function: XLogRecoveryShmemSize.
 * Purpose: Performs the WAL reading or recovery operation represented by xlog recovery shmem size.
 * Core flow: It prepares the reader or recovery context, processes the requested WAL state, and exposes the result.
 *
 * 函数：XLogRecoveryShmemSize。
 * 作用：执行 xlog recovery shmem size 所表示的 WAL 读取或恢复操作。
 * 核心流程：它准备读取器或恢复上下文，处理请求的 WAL 状态，并提供结果。
 */
extern Size XLogRecoveryShmemSize(void);
/*
 * Function: XLogRecoveryShmemInit.
 * Purpose: Performs the WAL reading or recovery operation represented by xlog recovery shmem init.
 * Core flow: It prepares the reader or recovery context, processes the requested WAL state, and exposes the result.
 *
 * 函数：XLogRecoveryShmemInit。
 * 作用：执行 xlog recovery shmem init 所表示的 WAL 读取或恢复操作。
 * 核心流程：它准备读取器或恢复上下文，处理请求的 WAL 状态，并提供结果。
 */
extern void XLogRecoveryShmemInit(void);

/*
 * Function: InitWalRecovery.
 * Purpose: Performs the WAL reading or recovery operation represented by init wal recovery.
 * Core flow: It prepares the reader or recovery context, processes the requested WAL state, and exposes the result.
 *
 * 函数：InitWalRecovery。
 * 作用：执行 init wal recovery 所表示的 WAL 读取或恢复操作。
 * 核心流程：它准备读取器或恢复上下文，处理请求的 WAL 状态，并提供结果。
 */
extern void InitWalRecovery(ControlFileData *ControlFile,
							bool *wasShutdown_ptr, bool *haveBackupLabel_ptr,
							bool *haveTblspcMap_ptr);
/*
 * Function: PerformWalRecovery.
 * Purpose: Performs the WAL reading or recovery operation represented by perform wal recovery.
 * Core flow: It prepares the reader or recovery context, processes the requested WAL state, and exposes the result.
 *
 * 函数：PerformWalRecovery。
 * 作用：执行 perform wal recovery 所表示的 WAL 读取或恢复操作。
 * 核心流程：它准备读取器或恢复上下文，处理请求的 WAL 状态，并提供结果。
 */
extern void PerformWalRecovery(void);

/*
 * FinishWalRecovery() returns this.  It contains information about the point
 * where recovery ended, and why it ended.
 *
 * 中文翻译：FinishWalRecovery() 返回此值。它包含有关恢复结束的时间点以及结束原因的信息。
 */
typedef struct
{
	/*
	 * Information about the last valid or applied record, after which new WAL
	 * can be appended.  'lastRec' is the position where the last record
	 * starts, and 'endOfLog' is its end.  'lastPage' is a copy of the last
	 * partial page that contains endOfLog (or NULL if endOfLog is exactly at
	 * page boundary).  'lastPageBeginPtr' is the position where the last page
	 * begins.
 *
 * 中文翻译：有关最后一个有效或应用的记录的信息，之后可以附加新的 WAL。 'lastRec' 是最后一条记录开始的位置，'endOfLog' 是最后一条记录的结束位置。 'lastPage' 是包含 endOfLog 的最后一个部分页面的副本（如果 endOfLog 恰好位于页面边界，则为 NULL）。 'lastPageBeginPtr' 是最后一页开始的位置。
	 *
	 * endOfLogTLI is the TLI in the filename of the XLOG segment containing
	 * the last applied record.  It could be different from lastRecTLI, if
	 * there was a timeline switch in that segment, and we were reading the
	 * old WAL from a segment belonging to a higher timeline.
 *
 * 中文翻译：endOfLogTLI 是包含最后应用记录的 XLOG 段的文件名中的 TLI。如果该段中存在时间线切换，并且我们正在从属于更高时间线的段读取旧 WAL，则它可能与 lastRecTLI 不同。
	 */
	XLogRecPtr	lastRec;		/* start of last valid or applied record */

	/* 中文翻译：最后一个有效或应用记录的开始 */
	TimeLineID	lastRecTLI;
	XLogRecPtr	endOfLog;		/* end of last valid or applied record */

	/* 中文翻译：最后一个有效或应用记录的结尾 */
	TimeLineID	endOfLogTLI;

	XLogRecPtr	lastPageBeginPtr;	/* LSN of page that contains endOfLog */

	/* 中文翻译：包含 endOfLog 的页面的 LSN */
	char	   *lastPage;		/* copy of the last page, up to endOfLog */

	/* 中文翻译：最后一页的副本，直到 endOfLog */

	/*
	 * abortedRecPtr is the start pointer of a broken record at end of WAL
	 * when recovery completes; missingContrecPtr is the location of the first
	 * contrecord that went missing.  See CreateOverwriteContrecordRecord for
	 * details.
 *
 * 中文翻译：abortedRecPtr 是恢复完成时位于 WAL 末尾的损坏记录的起始指针； MissingContrecPtr 是第一个丢失的 contrecord 的位置。有关详细信息，请参阅 CreateOverwriteContrecordRecord。
	 */
	XLogRecPtr	abortedRecPtr;
	XLogRecPtr	missingContrecPtr;

	/* short human-readable string describing why recovery ended */

	/* 说明恢复结束原因的简短可读字符串。 */
	char	   *recoveryStopReason;

	/*
	 * If standby or recovery signal file was found, these flags are set
	 * accordingly.
 *
 * 中文翻译：如果找到备用或恢复信号文件，则相应地设置这些标志。
	 */
	bool		standby_signal_file_found;
	bool		recovery_signal_file_found;
} EndOfWalRecoveryInfo;

/*
 * Function: FinishWalRecovery.
 * Purpose: Completes or releases the WAL work represented by finish wal recovery.
 * Core flow: It finalizes processing and releases or preserves the state required by subsequent recovery work.
 *
 * 函数：FinishWalRecovery。
 * 作用：完成或释放 finish wal recovery 所表示的 WAL 工作。
 * 核心流程：它结束处理，并释放或保留后续恢复工作所需的状态。
 */
extern EndOfWalRecoveryInfo *FinishWalRecovery(void);
/*
 * Function: ShutdownWalRecovery.
 * Purpose: Completes or releases the WAL work represented by shutdown wal recovery.
 * Core flow: It finalizes processing and releases or preserves the state required by subsequent recovery work.
 *
 * 函数：ShutdownWalRecovery。
 * 作用：完成或释放 shutdown wal recovery 所表示的 WAL 工作。
 * 核心流程：它结束处理，并释放或保留后续恢复工作所需的状态。
 */
extern void ShutdownWalRecovery(void);
/*
 * Function: RemovePromoteSignalFiles.
 * Purpose: Updates the WAL state represented by remove promote signal files.
 * Core flow: It applies the requested state change and keeps the related WAL or recovery metadata consistent.
 *
 * 函数：RemovePromoteSignalFiles。
 * 作用：更新 remove promote signal files 所表示的 WAL 状态。
 * 核心流程：它应用请求的状态变更，并保持相关 WAL 或恢复元数据一致。
 */
extern void RemovePromoteSignalFiles(void);

/*
 * Function: HotStandbyActive.
 * Purpose: Performs the WAL operation represented by hot standby active.
 * Core flow: It uses the supplied context to process WAL data and returns or records the result.
 *
 * 函数：HotStandbyActive。
 * 作用：执行 hot standby active 所表示的 WAL 操作。
 * 核心流程：它使用给定上下文处理 WAL 数据，并返回或记录结果。
 */
extern bool HotStandbyActive(void);
/*
 * Function: GetXLogReplayRecPtr.
 * Purpose: Obtains, validates, or decodes the WAL state represented by get xlog replay rec ptr.
 * Core flow: It examines the supplied record, block, page, or recovery state and returns the derived result.
 *
 * 函数：GetXLogReplayRecPtr。
 * 作用：获取、验证或解码 get xlog replay rec ptr 所表示的 WAL 状态。
 * 核心流程：它检查给定记录、块、页面或恢复状态，并返回推导结果。
 */
extern XLogRecPtr GetXLogReplayRecPtr(TimeLineID *replayTLI);
/*
 * Function: GetRecoveryPauseState.
 * Purpose: Obtains, validates, or decodes the WAL state represented by get recovery pause state.
 * Core flow: It examines the supplied record, block, page, or recovery state and returns the derived result.
 *
 * 函数：GetRecoveryPauseState。
 * 作用：获取、验证或解码 get recovery pause state 所表示的 WAL 状态。
 * 核心流程：它检查给定记录、块、页面或恢复状态，并返回推导结果。
 */
extern RecoveryPauseState GetRecoveryPauseState(void);
/*
 * Function: SetRecoveryPause.
 * Purpose: Updates the WAL state represented by set recovery pause.
 * Core flow: It applies the requested state change and keeps the related WAL or recovery metadata consistent.
 *
 * 函数：SetRecoveryPause。
 * 作用：更新 set recovery pause 所表示的 WAL 状态。
 * 核心流程：它应用请求的状态变更，并保持相关 WAL 或恢复元数据一致。
 */
extern void SetRecoveryPause(bool recoveryPause);
/*
 * Function: GetXLogReceiptTime.
 * Purpose: Obtains, validates, or decodes the WAL state represented by get xlog receipt time.
 * Core flow: It examines the supplied record, block, page, or recovery state and returns the derived result.
 *
 * 函数：GetXLogReceiptTime。
 * 作用：获取、验证或解码 get xlog receipt time 所表示的 WAL 状态。
 * 核心流程：它检查给定记录、块、页面或恢复状态，并返回推导结果。
 */
extern void GetXLogReceiptTime(TimestampTz *rtime, bool *fromStream);
/*
 * Function: GetLatestXTime.
 * Purpose: Obtains, validates, or decodes the WAL state represented by get latest xtime.
 * Core flow: It examines the supplied record, block, page, or recovery state and returns the derived result.
 *
 * 函数：GetLatestXTime。
 * 作用：获取、验证或解码 get latest xtime 所表示的 WAL 状态。
 * 核心流程：它检查给定记录、块、页面或恢复状态，并返回推导结果。
 */
extern TimestampTz GetLatestXTime(void);
/*
 * Function: GetCurrentChunkReplayStartTime.
 * Purpose: Obtains, validates, or decodes the WAL state represented by get current chunk replay start time.
 * Core flow: It examines the supplied record, block, page, or recovery state and returns the derived result.
 *
 * 函数：GetCurrentChunkReplayStartTime。
 * 作用：获取、验证或解码 get current chunk replay start time 所表示的 WAL 状态。
 * 核心流程：它检查给定记录、块、页面或恢复状态，并返回推导结果。
 */
extern TimestampTz GetCurrentChunkReplayStartTime(void);
/*
 * Function: GetCurrentReplayRecPtr.
 * Purpose: Obtains, validates, or decodes the WAL state represented by get current replay rec ptr.
 * Core flow: It examines the supplied record, block, page, or recovery state and returns the derived result.
 *
 * 函数：GetCurrentReplayRecPtr。
 * 作用：获取、验证或解码 get current replay rec ptr 所表示的 WAL 状态。
 * 核心流程：它检查给定记录、块、页面或恢复状态，并返回推导结果。
 */
extern XLogRecPtr GetCurrentReplayRecPtr(TimeLineID *replayEndTLI);

/*
 * Function: PromoteIsTriggered.
 * Purpose: Performs the WAL operation represented by promote is triggered.
 * Core flow: It uses the supplied context to process WAL data and returns or records the result.
 *
 * 函数：PromoteIsTriggered。
 * 作用：执行 promote is triggered 所表示的 WAL 操作。
 * 核心流程：它使用给定上下文处理 WAL 数据，并返回或记录结果。
 */
extern bool PromoteIsTriggered(void);
/*
 * Function: CheckPromoteSignal.
 * Purpose: Performs the WAL operation represented by check promote signal.
 * Core flow: It uses the supplied context to process WAL data and returns or records the result.
 *
 * 函数：CheckPromoteSignal。
 * 作用：执行 check promote signal 所表示的 WAL 操作。
 * 核心流程：它使用给定上下文处理 WAL 数据，并返回或记录结果。
 */
extern bool CheckPromoteSignal(void);
/*
 * Function: WakeupRecovery.
 * Purpose: Performs the WAL reading or recovery operation represented by wakeup recovery.
 * Core flow: It prepares the reader or recovery context, processes the requested WAL state, and exposes the result.
 *
 * 函数：WakeupRecovery。
 * 作用：执行 wakeup recovery 所表示的 WAL 读取或恢复操作。
 * 核心流程：它准备读取器或恢复上下文，处理请求的 WAL 状态，并提供结果。
 */
extern void WakeupRecovery(void);

/*
 * Function: StartupRequestWalReceiverRestart.
 * Purpose: Performs the WAL reading or recovery operation represented by startup request wal receiver restart.
 * Core flow: It prepares the reader or recovery context, processes the requested WAL state, and exposes the result.
 *
 * 函数：StartupRequestWalReceiverRestart。
 * 作用：执行 startup request wal receiver restart 所表示的 WAL 读取或恢复操作。
 * 核心流程：它准备读取器或恢复上下文，处理请求的 WAL 状态，并提供结果。
 */
extern void StartupRequestWalReceiverRestart(void);
/*
 * Function: XLogRequestWalReceiverReply.
 * Purpose: Performs the WAL reading or recovery operation represented by xlog request wal receiver reply.
 * Core flow: It prepares the reader or recovery context, processes the requested WAL state, and exposes the result.
 *
 * 函数：XLogRequestWalReceiverReply。
 * 作用：执行 xlog request wal receiver reply 所表示的 WAL 读取或恢复操作。
 * 核心流程：它准备读取器或恢复上下文，处理请求的 WAL 状态，并提供结果。
 */
extern void XLogRequestWalReceiverReply(void);

/*
 * Function: RecoveryRequiresIntParameter.
 * Purpose: Performs the WAL operation represented by recovery requires int parameter.
 * Core flow: It uses the supplied context to process WAL data and returns or records the result.
 *
 * 函数：RecoveryRequiresIntParameter。
 * 作用：执行 recovery requires int parameter 所表示的 WAL 操作。
 * 核心流程：它使用给定上下文处理 WAL 数据，并返回或记录结果。
 */
extern void RecoveryRequiresIntParameter(const char *param_name, int currValue, int minValue);

/*
 * Function: xlog_outdesc.
 * Purpose: Performs the WAL reading or recovery operation represented by xlog outdesc.
 * Core flow: It prepares the reader or recovery context, processes the requested WAL state, and exposes the result.
 *
 * 函数：xlog_outdesc。
 * 作用：执行 xlog outdesc 所表示的 WAL 读取或恢复操作。
 * 核心流程：它准备读取器或恢复上下文，处理请求的 WAL 状态，并提供结果。
 */
extern void xlog_outdesc(StringInfo buf, XLogReaderState *record);

#endif							/* XLOGRECOVERY_H */

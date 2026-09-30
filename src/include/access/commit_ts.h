/*
 * commit_ts.h
 *
  * commit_ts.h
 *
 * PostgreSQL commit timestamp manager
 *
 * 定义提交时间戳子系统的接口、状态维护和 WAL 重放支持。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/commit_ts.h
 *
  * src/include/access/commit_ts.h
 */
#ifndef COMMIT_TS_H
#define COMMIT_TS_H

#include "access/xlog.h"
#include "datatype/timestamp.h"
#include "replication/origin.h"
#include "storage/sync.h"


extern PGDLLIMPORT bool track_commit_timestamp;

/*
 * Function: TransactionTreeSetCommitTsData.
 * Purpose: Performs the operation represented by transaction tree set commit ts data.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：TransactionTreeSetCommitTsData。
 * 作用：执行 transaction tree set commit ts data 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
extern void TransactionTreeSetCommitTsData(TransactionId xid, int nsubxids,
										   TransactionId *subxids, TimestampTz timestamp,
										   RepOriginId nodeid);
/*
 * Function: TransactionIdGetCommitTsData.
 * Purpose: Performs the operation represented by transaction id get commit ts data.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：TransactionIdGetCommitTsData。
 * 作用：执行 transaction id get commit ts data 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
extern bool TransactionIdGetCommitTsData(TransactionId xid,
										 TimestampTz *ts, RepOriginId *nodeid);
/*
 * Function: GetLatestCommitTsData.
 * Purpose: Obtains or checks the state represented by get latest commit ts data.
 * Core flow: It examines the supplied identifiers and the relevant subsystem state, then returns the derived result without changing the requested state.
 *
 * 函数：GetLatestCommitTsData。
 * 作用：获取或检查 get latest commit ts data 所表示的状态。
 * 核心流程：它检查给定标识符和相关子系统状态，然后返回推导出的结果，而不改变请求的状态。
 */
extern TransactionId GetLatestCommitTsData(TimestampTz *ts,
										   RepOriginId *nodeid);

/*
 * Function: CommitTsShmemSize.
 * Purpose: Reports the shared-memory space required by commit ts shmem size.
 * Core flow: It derives the allocation size from the subsystem structures and returns it before shared-memory initialization.
 *
 * 函数：CommitTsShmemSize。
 * 作用：报告 commit ts shmem size 所需的共享内存空间。
 * 核心流程：它根据子系统结构计算分配大小，并在共享内存初始化前返回该值。
 */
extern Size CommitTsShmemSize(void);
/*
 * Function: CommitTsShmemInit.
 * Purpose: Initializes the state required for commit ts shmem init.
 * Core flow: It establishes the relevant shared, local, or on-disk state so later operations can use the subsystem safely.
 *
 * 函数：CommitTsShmemInit。
 * 作用：初始化 commit ts shmem init 所需的状态。
 * 核心流程：它建立相关的共享、本地或磁盘状态，使后续操作能够安全使用该子系统。
 */
extern void CommitTsShmemInit(void);
/*
 * Function: BootStrapCommitTs.
 * Purpose: Initializes the state required for boot strap commit ts.
 * Core flow: It establishes the relevant shared, local, or on-disk state so later operations can use the subsystem safely.
 *
 * 函数：BootStrapCommitTs。
 * 作用：初始化 boot strap commit ts 所需的状态。
 * 核心流程：它建立相关的共享、本地或磁盘状态，使后续操作能够安全使用该子系统。
 */
extern void BootStrapCommitTs(void);
/*
 * Function: StartupCommitTs.
 * Purpose: Initializes the state required for startup commit ts.
 * Core flow: It establishes the relevant shared, local, or on-disk state so later operations can use the subsystem safely.
 *
 * 函数：StartupCommitTs。
 * 作用：初始化 startup commit ts 所需的状态。
 * 核心流程：它建立相关的共享、本地或磁盘状态，使后续操作能够安全使用该子系统。
 */
extern void StartupCommitTs(void);
/*
 * Function: CommitTsParameterChange.
 * Purpose: Completes or releases the work represented by commit ts parameter change.
 * Core flow: It applies the required completion or cleanup actions and leaves the related transaction or WAL state ready for subsequent work.
 *
 * 函数：CommitTsParameterChange。
 * 作用：完成或释放 commit ts parameter change 所表示的工作。
 * 核心流程：它执行所需的完成或清理操作，并使相关事务或 WAL 状态可用于后续工作。
 */
extern void CommitTsParameterChange(bool newvalue, bool oldvalue);
/*
 * Function: CompleteCommitTsInitialization.
 * Purpose: Performs the operation represented by complete commit ts initialization.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：CompleteCommitTsInitialization。
 * 作用：执行 complete commit ts initialization 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
extern void CompleteCommitTsInitialization(void);
/*
 * Function: CheckPointCommitTs.
 * Purpose: Obtains or checks the state represented by check point commit ts.
 * Core flow: It examines the supplied identifiers and the relevant subsystem state, then returns the derived result without changing the requested state.
 *
 * 函数：CheckPointCommitTs。
 * 作用：获取或检查 check point commit ts 所表示的状态。
 * 核心流程：它检查给定标识符和相关子系统状态，然后返回推导出的结果，而不改变请求的状态。
 */
extern void CheckPointCommitTs(void);
/*
 * Function: ExtendCommitTs.
 * Purpose: Updates the state represented by extend commit ts.
 * Core flow: It validates the supplied inputs, changes the relevant transaction or WAL state, and keeps the associated metadata consistent.
 *
 * 函数：ExtendCommitTs。
 * 作用：更新 extend commit ts 所表示的状态。
 * 核心流程：它校验输入，变更相关事务或 WAL 状态，并保持关联元数据一致。
 */
extern void ExtendCommitTs(TransactionId newestXact);
/*
 * Function: TruncateCommitTs.
 * Purpose: Updates the state represented by truncate commit ts.
 * Core flow: It validates the supplied inputs, changes the relevant transaction or WAL state, and keeps the associated metadata consistent.
 *
 * 函数：TruncateCommitTs。
 * 作用：更新 truncate commit ts 所表示的状态。
 * 核心流程：它校验输入，变更相关事务或 WAL 状态，并保持关联元数据一致。
 */
extern void TruncateCommitTs(TransactionId oldestXact);
/*
 * Function: SetCommitTsLimit.
 * Purpose: Updates the state represented by set commit ts limit.
 * Core flow: It validates the supplied inputs, changes the relevant transaction or WAL state, and keeps the associated metadata consistent.
 *
 * 函数：SetCommitTsLimit。
 * 作用：更新 set commit ts limit 所表示的状态。
 * 核心流程：它校验输入，变更相关事务或 WAL 状态，并保持关联元数据一致。
 */
extern void SetCommitTsLimit(TransactionId oldestXact,
							 TransactionId newestXact);
/*
 * Function: AdvanceOldestCommitTsXid.
 * Purpose: Updates the state represented by advance oldest commit ts xid.
 * Core flow: It validates the supplied inputs, changes the relevant transaction or WAL state, and keeps the associated metadata consistent.
 *
 * 函数：AdvanceOldestCommitTsXid。
 * 作用：更新 advance oldest commit ts xid 所表示的状态。
 * 核心流程：它校验输入，变更相关事务或 WAL 状态，并保持关联元数据一致。
 */
extern void AdvanceOldestCommitTsXid(TransactionId oldestXact);

/*
 * Function: committssyncfiletag.
 * Purpose: Completes or releases the work represented by committssyncfiletag.
 * Core flow: It applies the required completion or cleanup actions and leaves the related transaction or WAL state ready for subsequent work.
 *
 * 函数：committssyncfiletag。
 * 作用：完成或释放 committssyncfiletag 所表示的工作。
 * 核心流程：它执行所需的完成或清理操作，并使相关事务或 WAL 状态可用于后续工作。
 */
extern int	committssyncfiletag(const FileTag *ftag, char *path);

/* XLOG stuff */

/* XLOG 的东西 */
#define COMMIT_TS_ZEROPAGE		0x00
#define COMMIT_TS_TRUNCATE		0x10

typedef struct xl_commit_ts_set
{
	TimestampTz timestamp;
	RepOriginId nodeid;
	TransactionId mainxid;
	/* subxact Xids follow */

	/* subxact Xids 遵循 */
}			xl_commit_ts_set;

#define SizeOfCommitTsSet	(offsetof(xl_commit_ts_set, mainxid) + \
							 sizeof(TransactionId))

typedef struct xl_commit_ts_truncate
{
	int64		pageno;
	TransactionId oldestXid;
} xl_commit_ts_truncate;

#define SizeOfCommitTsTruncate	(offsetof(xl_commit_ts_truncate, oldestXid) + \
								 sizeof(TransactionId))

/*
 * Function: commit_ts_redo.
 * Purpose: Completes or releases the work represented by commit ts redo.
 * Core flow: It applies the required completion or cleanup actions and leaves the related transaction or WAL state ready for subsequent work.
 *
 * 函数：commit_ts_redo。
 * 作用：完成或释放 commit ts redo 所表示的工作。
 * 核心流程：它执行所需的完成或清理操作，并使相关事务或 WAL 状态可用于后续工作。
 */
extern void commit_ts_redo(XLogReaderState *record);
/*
 * Function: commit_ts_desc.
 * Purpose: Completes or releases the work represented by commit ts desc.
 * Core flow: It applies the required completion or cleanup actions and leaves the related transaction or WAL state ready for subsequent work.
 *
 * 函数：commit_ts_desc。
 * 作用：完成或释放 commit ts desc 所表示的工作。
 * 核心流程：它执行所需的完成或清理操作，并使相关事务或 WAL 状态可用于后续工作。
 */
extern void commit_ts_desc(StringInfo buf, XLogReaderState *record);
/*
 * Function: commit_ts_identify.
 * Purpose: Completes or releases the work represented by commit ts identify.
 * Core flow: It applies the required completion or cleanup actions and leaves the related transaction or WAL state ready for subsequent work.
 *
 * 函数：commit_ts_identify。
 * 作用：完成或释放 commit ts identify 所表示的工作。
 * 核心流程：它执行所需的完成或清理操作，并使相关事务或 WAL 状态可用于后续工作。
 */
extern const char *commit_ts_identify(uint8 info);

#endif							/* COMMIT_TS_H */

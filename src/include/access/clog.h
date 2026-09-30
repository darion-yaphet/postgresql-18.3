/*
 * clog.h
 *
  * clog.h
 *
 * PostgreSQL transaction-commit-log manager
 *
  * PostgreSQL 事务提交日志管理器
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/clog.h
 *
  * src/include/access/clog.h
 */
#ifndef CLOG_H
#define CLOG_H

#include "access/xlogreader.h"
#include "storage/sync.h"
#include "lib/stringinfo.h"

/*
 * Possible transaction statuses --- note that all-zeroes is the initial
 * state.
 *
  * 可能的事务状态 --- 请注意，全零是初始状态。
 *
 * A "subcommitted" transaction is a committed subtransaction whose parent
 * hasn't committed or aborted yet.
 *
  * “子提交”事务是指其父事务尚未提交或中止的已提交子事务。
 */
typedef int XidStatus;

#define TRANSACTION_STATUS_IN_PROGRESS		0x00
#define TRANSACTION_STATUS_COMMITTED		0x01
#define TRANSACTION_STATUS_ABORTED			0x02
#define TRANSACTION_STATUS_SUB_COMMITTED	0x03

typedef struct xl_clog_truncate
{
	int64		pageno;
	TransactionId oldestXact;
	Oid			oldestXactDb;
} xl_clog_truncate;

/*
 * Function: TransactionIdSetTreeStatus.
 * Purpose: Performs the operation represented by transaction id set tree status.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：TransactionIdSetTreeStatus。
 * 作用：执行 transaction id set tree status 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
extern void TransactionIdSetTreeStatus(TransactionId xid, int nsubxids,
									   TransactionId *subxids, XidStatus status, XLogRecPtr lsn);
/*
 * Function: TransactionIdGetStatus.
 * Purpose: Performs the operation represented by transaction id get status.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：TransactionIdGetStatus。
 * 作用：执行 transaction id get status 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
extern XidStatus TransactionIdGetStatus(TransactionId xid, XLogRecPtr *lsn);

/*
 * Function: CLOGShmemSize.
 * Purpose: Reports the shared-memory space required by clogshmem size.
 * Core flow: It derives the allocation size from the subsystem structures and returns it before shared-memory initialization.
 *
 * 函数：CLOGShmemSize。
 * 作用：报告 clogshmem size 所需的共享内存空间。
 * 核心流程：它根据子系统结构计算分配大小，并在共享内存初始化前返回该值。
 */
extern Size CLOGShmemSize(void);
/*
 * Function: CLOGShmemInit.
 * Purpose: Initializes the state required for clogshmem init.
 * Core flow: It establishes the relevant shared, local, or on-disk state so later operations can use the subsystem safely.
 *
 * 函数：CLOGShmemInit。
 * 作用：初始化 clogshmem init 所需的状态。
 * 核心流程：它建立相关的共享、本地或磁盘状态，使后续操作能够安全使用该子系统。
 */
extern void CLOGShmemInit(void);
/*
 * Function: BootStrapCLOG.
 * Purpose: Initializes the state required for boot strap clog.
 * Core flow: It establishes the relevant shared, local, or on-disk state so later operations can use the subsystem safely.
 *
 * 函数：BootStrapCLOG。
 * 作用：初始化 boot strap clog 所需的状态。
 * 核心流程：它建立相关的共享、本地或磁盘状态，使后续操作能够安全使用该子系统。
 */
extern void BootStrapCLOG(void);
/*
 * Function: StartupCLOG.
 * Purpose: Initializes the state required for startup clog.
 * Core flow: It establishes the relevant shared, local, or on-disk state so later operations can use the subsystem safely.
 *
 * 函数：StartupCLOG。
 * 作用：初始化 startup clog 所需的状态。
 * 核心流程：它建立相关的共享、本地或磁盘状态，使后续操作能够安全使用该子系统。
 */
extern void StartupCLOG(void);
/*
 * Function: TrimCLOG.
 * Purpose: Updates the state represented by trim clog.
 * Core flow: It validates the supplied inputs, changes the relevant transaction or WAL state, and keeps the associated metadata consistent.
 *
 * 函数：TrimCLOG。
 * 作用：更新 trim clog 所表示的状态。
 * 核心流程：它校验输入，变更相关事务或 WAL 状态，并保持关联元数据一致。
 */
extern void TrimCLOG(void);
/*
 * Function: CheckPointCLOG.
 * Purpose: Obtains or checks the state represented by check point clog.
 * Core flow: It examines the supplied identifiers and the relevant subsystem state, then returns the derived result without changing the requested state.
 *
 * 函数：CheckPointCLOG。
 * 作用：获取或检查 check point clog 所表示的状态。
 * 核心流程：它检查给定标识符和相关子系统状态，然后返回推导出的结果，而不改变请求的状态。
 */
extern void CheckPointCLOG(void);
/*
 * Function: ExtendCLOG.
 * Purpose: Updates the state represented by extend clog.
 * Core flow: It validates the supplied inputs, changes the relevant transaction or WAL state, and keeps the associated metadata consistent.
 *
 * 函数：ExtendCLOG。
 * 作用：更新 extend clog 所表示的状态。
 * 核心流程：它校验输入，变更相关事务或 WAL 状态，并保持关联元数据一致。
 */
extern void ExtendCLOG(TransactionId newestXact);
/*
 * Function: TruncateCLOG.
 * Purpose: Updates the state represented by truncate clog.
 * Core flow: It validates the supplied inputs, changes the relevant transaction or WAL state, and keeps the associated metadata consistent.
 *
 * 函数：TruncateCLOG。
 * 作用：更新 truncate clog 所表示的状态。
 * 核心流程：它校验输入，变更相关事务或 WAL 状态，并保持关联元数据一致。
 */
extern void TruncateCLOG(TransactionId oldestXact, Oid oldestxid_datoid);

/*
 * Function: clogsyncfiletag.
 * Purpose: Performs the operation represented by clogsyncfiletag.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：clogsyncfiletag。
 * 作用：执行 clogsyncfiletag 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
extern int	clogsyncfiletag(const FileTag *ftag, char *path);

/* XLOG stuff */

/* XLOG 的东西 */
#define CLOG_ZEROPAGE		0x00
#define CLOG_TRUNCATE		0x10

/*
 * Function: clog_redo.
 * Purpose: Performs the operation represented by clog redo.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：clog_redo。
 * 作用：执行 clog redo 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
extern void clog_redo(XLogReaderState *record);
/*
 * Function: clog_desc.
 * Purpose: Performs the operation represented by clog desc.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：clog_desc。
 * 作用：执行 clog desc 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
extern void clog_desc(StringInfo buf, XLogReaderState *record);
/*
 * Function: clog_identify.
 * Purpose: Performs the operation represented by clog identify.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：clog_identify。
 * 作用：执行 clog identify 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
extern const char *clog_identify(uint8 info);

#endif							/* CLOG_H */

/*-------------------------------------------------------------------------
 *
 * twophase.h
 *	  Two-phase-commit related declarations.
 *
 * 两阶段提交相关的声明。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/twophase.h
 *
 * 两阶段提交相关的声明。
 *
 *-------------------------------------------------------------------------
 */
#ifndef TWOPHASE_H
#define TWOPHASE_H

#include "access/xact.h"
#include "access/xlogdefs.h"
#include "datatype/timestamp.h"
#include "storage/lock.h"

/*
 * GlobalTransactionData is defined in twophase.c; other places have no
 * business knowing the internal definition.
 *
 * GlobalTransactionData 定义在 twophase.c 中；其他地方无需知道其内部定义。
 */
typedef struct GlobalTransactionData *GlobalTransaction;

/* GUC variable */

/* GUC 变量。 */
extern PGDLLIMPORT int max_prepared_xacts;

/*
 * Function: TwoPhaseShmemSize.
 * Purpose: Reports the shared-memory space required by two phase shmem size.
 * Core flow: It derives the allocation size from subsystem structures before initialization.
 *
 * 函数：TwoPhaseShmemSize。
 * 作用：报告 two phase shmem size 所需的共享内存空间。
 * 核心流程：它在初始化前根据子系统结构计算分配大小。
 */
extern Size TwoPhaseShmemSize(void);
/*
 * Function: TwoPhaseShmemInit.
 * Purpose: Initializes the state required for two phase shmem init.
 * Core flow: It establishes the shared, local, or on-disk state needed by later operations.
 *
 * 函数：TwoPhaseShmemInit。
 * 作用：初始化 two phase shmem init 所需的状态。
 * 核心流程：它建立后续操作所需的共享、本地或磁盘状态。
 */
extern void TwoPhaseShmemInit(void);

/*
 * Function: AtAbort_Twophase.
 * Purpose: Performs the operation represented by at abort twophase.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：AtAbort_Twophase。
 * 作用：执行 at abort twophase 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
extern void AtAbort_Twophase(void);
/*
 * Function: PostPrepare_Twophase.
 * Purpose: Completes or releases the work represented by post prepare twophase.
 * Core flow: It applies the required completion or cleanup actions and leaves related state ready for later work.
 *
 * 函数：PostPrepare_Twophase。
 * 作用：完成或释放 post prepare twophase 所表示的工作。
 * 核心流程：它执行所需的完成或清理操作，并使相关状态可供后续工作使用。
 */
extern void PostPrepare_Twophase(void);

/*
 * Function: TwoPhaseGetXidByVirtualXID.
 * Purpose: Performs the operation represented by two phase get xid by virtual xid.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：TwoPhaseGetXidByVirtualXID。
 * 作用：执行 two phase get xid by virtual xid 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
extern TransactionId TwoPhaseGetXidByVirtualXID(VirtualTransactionId vxid,
												bool *have_more);
/*
 * Function: TwoPhaseGetDummyProc.
 * Purpose: Performs the operation represented by two phase get dummy proc.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：TwoPhaseGetDummyProc。
 * 作用：执行 two phase get dummy proc 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
extern PGPROC *TwoPhaseGetDummyProc(TransactionId xid, bool lock_held);
/*
 * Function: TwoPhaseGetDummyProcNumber.
 * Purpose: Performs the operation represented by two phase get dummy proc number.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：TwoPhaseGetDummyProcNumber。
 * 作用：执行 two phase get dummy proc number 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
extern int	TwoPhaseGetDummyProcNumber(TransactionId xid, bool lock_held);

/*
 * Function: MarkAsPreparing.
 * Purpose: Updates the state represented by mark as preparing.
 * Core flow: It validates its inputs, changes the relevant state, and keeps associated metadata consistent.
 *
 * 函数：MarkAsPreparing。
 * 作用：更新 mark as preparing 所表示的状态。
 * 核心流程：它校验输入，变更相关状态，并保持关联元数据一致。
 */
extern GlobalTransaction MarkAsPreparing(TransactionId xid, const char *gid,
										 TimestampTz prepared_at,
										 Oid owner, Oid databaseid);

/*
 * Function: StartPrepare.
 * Purpose: Creates or starts the work represented by start prepare.
 * Core flow: It prepares the required context, performs the requested operation, and makes the result available to the owner.
 *
 * 函数：StartPrepare。
 * 作用：创建或启动 start prepare 所表示的工作。
 * 核心流程：它准备所需上下文，完成请求操作，并向所属方提供结果。
 */
extern void StartPrepare(GlobalTransaction gxact);
/*
 * Function: EndPrepare.
 * Purpose: Completes or releases the work represented by end prepare.
 * Core flow: It applies the required completion or cleanup actions and leaves related state ready for later work.
 *
 * 函数：EndPrepare。
 * 作用：完成或释放 end prepare 所表示的工作。
 * 核心流程：它执行所需的完成或清理操作，并使相关状态可供后续工作使用。
 */
extern void EndPrepare(GlobalTransaction gxact);
/*
 * Function: StandbyTransactionIdIsPrepared.
 * Purpose: Performs the operation represented by standby transaction id is prepared.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：StandbyTransactionIdIsPrepared。
 * 作用：执行 standby transaction id is prepared 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
extern bool StandbyTransactionIdIsPrepared(TransactionId xid);

/*
 * Function: PrescanPreparedTransactions.
 * Purpose: Performs the operation represented by prescan prepared transactions.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：PrescanPreparedTransactions。
 * 作用：执行 prescan prepared transactions 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
extern TransactionId PrescanPreparedTransactions(TransactionId **xids_p,
												 int *nxids_p);
/*
 * Function: StandbyRecoverPreparedTransactions.
 * Purpose: Performs the operation represented by standby recover prepared transactions.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：StandbyRecoverPreparedTransactions。
 * 作用：执行 standby recover prepared transactions 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
extern void StandbyRecoverPreparedTransactions(void);
/*
 * Function: RecoverPreparedTransactions.
 * Purpose: Performs the WAL-related operation represented by recover prepared transactions.
 * Core flow: It interprets the supplied record or context and records or reports the resulting state.
 *
 * 函数：RecoverPreparedTransactions。
 * 作用：执行 recover prepared transactions 所表示的 WAL 相关操作。
 * 核心流程：它解释给定记录或上下文，并记录或报告生成的状态。
 */
extern void RecoverPreparedTransactions(void);

/*
 * Function: CheckPointTwoPhase.
 * Purpose: Obtains or checks the state represented by check point two phase.
 * Core flow: It examines the supplied identifiers and subsystem state, then returns the derived result.
 *
 * 函数：CheckPointTwoPhase。
 * 作用：获取或检查 check point two phase 所表示的状态。
 * 核心流程：它检查给定标识符和子系统状态，然后返回推导结果。
 */
extern void CheckPointTwoPhase(XLogRecPtr redo_horizon);

/*
 * Function: FinishPreparedTransaction.
 * Purpose: Completes or releases the work represented by finish prepared transaction.
 * Core flow: It applies the required completion or cleanup actions and leaves related state ready for later work.
 *
 * 函数：FinishPreparedTransaction。
 * 作用：完成或释放 finish prepared transaction 所表示的工作。
 * 核心流程：它执行所需的完成或清理操作，并使相关状态可供后续工作使用。
 */
extern void FinishPreparedTransaction(const char *gid, bool isCommit);

/*
 * Function: PrepareRedoAdd.
 * Purpose: Creates or starts the work represented by prepare redo add.
 * Core flow: It prepares the required context, performs the requested operation, and makes the result available to the owner.
 *
 * 函数：PrepareRedoAdd。
 * 作用：创建或启动 prepare redo add 所表示的工作。
 * 核心流程：它准备所需上下文，完成请求操作，并向所属方提供结果。
 */
extern void PrepareRedoAdd(char *buf, XLogRecPtr start_lsn,
						   XLogRecPtr end_lsn, RepOriginId origin_id);
/*
 * Function: PrepareRedoRemove.
 * Purpose: Creates or starts the work represented by prepare redo remove.
 * Core flow: It prepares the required context, performs the requested operation, and makes the result available to the owner.
 *
 * 函数：PrepareRedoRemove。
 * 作用：创建或启动 prepare redo remove 所表示的工作。
 * 核心流程：它准备所需上下文，完成请求操作，并向所属方提供结果。
 */
extern void PrepareRedoRemove(TransactionId xid, bool giveWarning);
/*
 * Function: restoreTwoPhaseData.
 * Purpose: Creates or starts the work represented by restore two phase data.
 * Core flow: It prepares the required context, performs the requested operation, and makes the result available to the owner.
 *
 * 函数：restoreTwoPhaseData。
 * 作用：创建或启动 restore two phase data 所表示的工作。
 * 核心流程：它准备所需上下文，完成请求操作，并向所属方提供结果。
 */
extern void restoreTwoPhaseData(void);
/*
 * Function: LookupGXact.
 * Purpose: Obtains or checks the state represented by lookup gxact.
 * Core flow: It examines the supplied identifiers and subsystem state, then returns the derived result.
 *
 * 函数：LookupGXact。
 * 作用：获取或检查 lookup gxact 所表示的状态。
 * 核心流程：它检查给定标识符和子系统状态，然后返回推导结果。
 */
extern bool LookupGXact(const char *gid, XLogRecPtr prepare_end_lsn,
						TimestampTz origin_prepare_timestamp);

/*
 * Function: TwoPhaseTransactionGid.
 * Purpose: Performs the operation represented by two phase transaction gid.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：TwoPhaseTransactionGid。
 * 作用：执行 two phase transaction gid 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
extern void TwoPhaseTransactionGid(Oid subid, TransactionId xid, char *gid_res,
								   int szgid);
/*
 * Function: LookupGXactBySubid.
 * Purpose: Obtains or checks the state represented by lookup gxact by subid.
 * Core flow: It examines the supplied identifiers and subsystem state, then returns the derived result.
 *
 * 函数：LookupGXactBySubid。
 * 作用：获取或检查 lookup gxact by subid 所表示的状态。
 * 核心流程：它检查给定标识符和子系统状态，然后返回推导结果。
 */
extern bool LookupGXactBySubid(Oid subid);

#endif							/* TWOPHASE_H */

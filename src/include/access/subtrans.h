/*
 * subtrans.h
 *
 * 说明子事务状态日志的读写、初始化或截断处理。
 *
 * PostgreSQL subtransaction-log manager
 *
 * 说明子事务状态日志的读写、初始化或截断处理。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/subtrans.h
 *
 * 说明子事务状态日志的读写、初始化或截断处理。
 */
#ifndef SUBTRANS_H
#define SUBTRANS_H

/*
 * Function: SubTransSetParent.
 * Purpose: Performs the operation represented by sub trans set parent.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：SubTransSetParent。
 * 作用：执行 sub trans set parent 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
extern void SubTransSetParent(TransactionId xid, TransactionId parent);
/*
 * Function: SubTransGetParent.
 * Purpose: Performs the operation represented by sub trans get parent.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：SubTransGetParent。
 * 作用：执行 sub trans get parent 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
extern TransactionId SubTransGetParent(TransactionId xid);
/*
 * Function: SubTransGetTopmostTransaction.
 * Purpose: Performs the operation represented by sub trans get topmost transaction.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：SubTransGetTopmostTransaction。
 * 作用：执行 sub trans get topmost transaction 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
extern TransactionId SubTransGetTopmostTransaction(TransactionId xid);

/*
 * Function: SUBTRANSShmemSize.
 * Purpose: Reports the shared-memory space required by subtransshmem size.
 * Core flow: It derives the allocation size from subsystem structures before initialization.
 *
 * 函数：SUBTRANSShmemSize。
 * 作用：报告 subtransshmem size 所需的共享内存空间。
 * 核心流程：它在初始化前根据子系统结构计算分配大小。
 */
extern Size SUBTRANSShmemSize(void);
/*
 * Function: SUBTRANSShmemInit.
 * Purpose: Initializes the state required for subtransshmem init.
 * Core flow: It establishes the shared, local, or on-disk state needed by later operations.
 *
 * 函数：SUBTRANSShmemInit。
 * 作用：初始化 subtransshmem init 所需的状态。
 * 核心流程：它建立后续操作所需的共享、本地或磁盘状态。
 */
extern void SUBTRANSShmemInit(void);
/*
 * Function: BootStrapSUBTRANS.
 * Purpose: Initializes the state required for boot strap subtrans.
 * Core flow: It establishes the shared, local, or on-disk state needed by later operations.
 *
 * 函数：BootStrapSUBTRANS。
 * 作用：初始化 boot strap subtrans 所需的状态。
 * 核心流程：它建立后续操作所需的共享、本地或磁盘状态。
 */
extern void BootStrapSUBTRANS(void);
/*
 * Function: StartupSUBTRANS.
 * Purpose: Initializes the state required for startup subtrans.
 * Core flow: It establishes the shared, local, or on-disk state needed by later operations.
 *
 * 函数：StartupSUBTRANS。
 * 作用：初始化 startup subtrans 所需的状态。
 * 核心流程：它建立后续操作所需的共享、本地或磁盘状态。
 */
extern void StartupSUBTRANS(TransactionId oldestActiveXID);
/*
 * Function: CheckPointSUBTRANS.
 * Purpose: Obtains or checks the state represented by check point subtrans.
 * Core flow: It examines the supplied identifiers and subsystem state, then returns the derived result.
 *
 * 函数：CheckPointSUBTRANS。
 * 作用：获取或检查 check point subtrans 所表示的状态。
 * 核心流程：它检查给定标识符和子系统状态，然后返回推导结果。
 */
extern void CheckPointSUBTRANS(void);
/*
 * Function: ExtendSUBTRANS.
 * Purpose: Updates the state represented by extend subtrans.
 * Core flow: It validates its inputs, changes the relevant state, and keeps associated metadata consistent.
 *
 * 函数：ExtendSUBTRANS。
 * 作用：更新 extend subtrans 所表示的状态。
 * 核心流程：它校验输入，变更相关状态，并保持关联元数据一致。
 */
extern void ExtendSUBTRANS(TransactionId newestXact);
/*
 * Function: TruncateSUBTRANS.
 * Purpose: Updates the state represented by truncate subtrans.
 * Core flow: It validates its inputs, changes the relevant state, and keeps associated metadata consistent.
 *
 * 函数：TruncateSUBTRANS。
 * 作用：更新 truncate subtrans 所表示的状态。
 * 核心流程：它校验输入，变更相关状态，并保持关联元数据一致。
 */
extern void TruncateSUBTRANS(TransactionId oldestXact);

#endif							/* SUBTRANS_H */

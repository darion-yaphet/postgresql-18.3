/*-------------------------------------------------------------------------
 *
 * twophase_rmgr.h
 *	  Two-phase-commit resource managers definition
 *
 * 两阶段提交资源管理器定义。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/twophase_rmgr.h
 *
 * 两阶段提交资源管理器定义。
 *
 *-------------------------------------------------------------------------
 */
#ifndef TWOPHASE_RMGR_H
#define TWOPHASE_RMGR_H

typedef void (*TwoPhaseCallback) (TransactionId xid, uint16 info,
								  void *recdata, uint32 len);
typedef uint8 TwoPhaseRmgrId;

/*
 * Built-in resource managers
 *
 * 内置资源管理器。
 */
#define TWOPHASE_RM_END_ID			0
#define TWOPHASE_RM_LOCK_ID			1
#define TWOPHASE_RM_PGSTAT_ID		2
#define TWOPHASE_RM_MULTIXACT_ID	3
#define TWOPHASE_RM_PREDICATELOCK_ID	4
#define TWOPHASE_RM_MAX_ID			TWOPHASE_RM_PREDICATELOCK_ID

extern PGDLLIMPORT const TwoPhaseCallback twophase_recover_callbacks[];
extern PGDLLIMPORT const TwoPhaseCallback twophase_postcommit_callbacks[];
extern PGDLLIMPORT const TwoPhaseCallback twophase_postabort_callbacks[];
extern PGDLLIMPORT const TwoPhaseCallback twophase_standby_recover_callbacks[];


/*
 * Function: RegisterTwoPhaseRecord.
 * Purpose: Creates or starts the work represented by register two phase record.
 * Core flow: It prepares the required context, performs the requested operation, and makes the result available to the owner.
 *
 * 函数：RegisterTwoPhaseRecord。
 * 作用：创建或启动 register two phase record 所表示的工作。
 * 核心流程：它准备所需上下文，完成请求操作，并向所属方提供结果。
 */
extern void RegisterTwoPhaseRecord(TwoPhaseRmgrId rmid, uint16 info,
								   const void *data, uint32 len);

#endif							/* TWOPHASE_RMGR_H */

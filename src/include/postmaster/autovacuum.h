/*-------------------------------------------------------------------------
 *
 * autovacuum.h
 *	  header file for integrated autovacuum daemon
 *	  集成自动清理（autovacuum）守护进程的头文件
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/postmaster/autovacuum.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef AUTOVACUUM_H
#define AUTOVACUUM_H

#include "storage/block.h"

/*
 * Other processes can request specific work from autovacuum, identified by
 * AutoVacuumWorkItem elements.
 *
 * 其他进程可以请求自动清理进程执行特定的工作，由 AutoVacuumWorkItem 元素标识。
 */
typedef enum
{
	AVW_BRINSummarizeRange,
} AutoVacuumWorkItemType;


/* GUC variables */
/* GUC 变量 */
extern PGDLLIMPORT bool autovacuum_start_daemon;
extern PGDLLIMPORT int autovacuum_worker_slots;
extern PGDLLIMPORT int autovacuum_max_workers;
extern PGDLLIMPORT int autovacuum_work_mem;
extern PGDLLIMPORT int autovacuum_naptime;
extern PGDLLIMPORT int autovacuum_vac_thresh;
extern PGDLLIMPORT int autovacuum_vac_max_thresh;
extern PGDLLIMPORT double autovacuum_vac_scale;
extern PGDLLIMPORT int autovacuum_vac_ins_thresh;
extern PGDLLIMPORT double autovacuum_vac_ins_scale;
extern PGDLLIMPORT int autovacuum_anl_thresh;
extern PGDLLIMPORT double autovacuum_anl_scale;
extern PGDLLIMPORT int autovacuum_freeze_max_age;
extern PGDLLIMPORT int autovacuum_multixact_freeze_max_age;
extern PGDLLIMPORT double autovacuum_vac_cost_delay;
extern PGDLLIMPORT int autovacuum_vac_cost_limit;

/* autovacuum launcher PID, only valid when worker is shutting down */
/* 自动清理启动器 PID，仅在工作进程关闭时有效 */
extern PGDLLIMPORT int AutovacuumLauncherPid;

extern PGDLLIMPORT int Log_autovacuum_min_duration;

/* Status inquiry functions */
/* 状态查询函数 */
extern bool AutoVacuumingActive(void);

/* called from postmaster at server startup */
/* 在服务器启动时由 postmaster 调用 */
extern void autovac_init(void);

/* called from postmaster when a worker could not be forked */
/* 当无法派生（fork）工作进程时由 postmaster 调用 */
extern void AutoVacWorkerFailed(void);

pg_noreturn extern void AutoVacLauncherMain(const void *startup_data, size_t startup_data_len);
pg_noreturn extern void AutoVacWorkerMain(const void *startup_data, size_t startup_data_len);

extern bool AutoVacuumRequestWork(AutoVacuumWorkItemType type,
								  Oid relationId, BlockNumber blkno);

/* shared memory stuff */
/* 共享内存相关内容 */
extern Size AutoVacuumShmemSize(void);
extern void AutoVacuumShmemInit(void);

#endif							/* AUTOVACUUM_H */

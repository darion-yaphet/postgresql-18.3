/*-------------------------------------------------------------------------
 *
 * xlogbackup.h
 *		Definitions for internals of base backups.
 *
 * 说明基准备份状态、开始点、结束点和历史文件信息。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/include/access/xlogbackup.h
 *
 * 说明基准备份状态、开始点、结束点和历史文件信息。
 *-------------------------------------------------------------------------
 */

#ifndef XLOG_BACKUP_H
#define XLOG_BACKUP_H

#include "access/xlogdefs.h"
#include "pgtime.h"

/* Structure to hold backup state. */

/* 保存备份状态的结构。 */
typedef struct BackupState
{
	/* Fields saved at backup start */

	/* 在备份开始时保存的字段。 */
	/* Backup label name one extra byte for null-termination */

	/* 备份标签名称额外保留一个字节用于空字符终止。 */
	char		name[MAXPGPATH + 1];
	XLogRecPtr	startpoint;		/* backup start WAL location */

	/* 备份开始的 WAL 位置。 */
	TimeLineID	starttli;		/* backup start TLI */

	/* 备份开始的时间线 ID。 */
	XLogRecPtr	checkpointloc;	/* last checkpoint location */

	/* 上一个检查点位置。 */
	pg_time_t	starttime;		/* backup start time */

	/* 备份开始时间。 */
	bool		started_in_recovery;	/* backup started in recovery? */

	/* 备份是否在恢复期间启动？ */
	XLogRecPtr	istartpoint;	/* incremental based on backup at this LSN */

	/* 说明基准备份状态、开始点、结束点和历史文件信息。 */
	TimeLineID	istarttli;		/* incremental based on backup on this TLI */

	/* 说明基准备份状态、开始点、结束点和历史文件信息。 */

	/* Fields saved at the end of backup */

	/* 在备份结束时保存的字段。 */
	XLogRecPtr	stoppoint;		/* backup stop WAL location */

	/* 备份停止的 WAL 位置。 */
	TimeLineID	stoptli;		/* backup stop TLI */

	/* 备份停止的时间线 ID。 */
	pg_time_t	stoptime;		/* backup stop time */

	/* 备份停止时间。 */
} BackupState;

/*
 * Function: build_backup_content.
 * Purpose: Performs the WAL operation represented by build backup content.
 * Core flow: It uses the supplied context to process the requested WAL work and reports or returns the result.
 *
 * 函数：build_backup_content。
 * 作用：执行 build backup content 所表示的 WAL 操作。
 * 核心流程：它使用给定上下文处理请求的 WAL 工作，并报告或返回结果。
 */
extern char *build_backup_content(BackupState *state,
								  bool ishistoryfile);

#endif							/* XLOG_BACKUP_H */

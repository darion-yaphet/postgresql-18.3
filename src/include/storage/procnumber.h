/*-------------------------------------------------------------------------
 *
 * procnumber.h
 *	  definition of process number
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/procnumber.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROCNUMBER_H
#define PROCNUMBER_H

/*
 * ProcNumber uniquely identifies an active backend or auxiliary process.
 * It's assigned at backend startup after authentication, when the process
 * adds itself to the proc array.  It is an index into the proc array,
 * starting from 0. Note that a ProcNumber can be reused for a different
 * backend immediately after a backend exits.
 */

/*
 * ProcNumber 唯一标识活动后端或辅助进程。它在身份验证后的后端启动阶段分配，进程
 * 将自身加入进程数组时获得该编号。它是从 0 开始的进程数组索引。请注意，后端退出后，
 * 同一个 ProcNumber 可立即复用于不同后端。
 */
typedef int ProcNumber;

#define INVALID_PROC_NUMBER		(-1)

/*
 * Note: MAX_BACKENDS_BITS is 18 as that is the space available for buffer
 * refcounts in buf_internals.h.  This limitation could be lifted by using a
 * 64bit state; but it's unlikely to be worthwhile as 2^18-1 backends exceed
 * currently realistic configurations. Even if that limitation were removed,
 * we still could not a) exceed 2^23-1 because inval.c stores the ProcNumber
 * as a 3-byte signed integer, b) INT_MAX/4 because some places compute
 * 4*MaxBackends without any overflow check.  We check that the configured
 * number of backends does not exceed MAX_BACKENDS in InitializeMaxBackends().
 */

/*
 * 注意：MAX_BACKENDS_BITS 为 18，因为 buf_internals.h 中的缓冲区引用计数仅有该
 * 空间。通过使用 64 位状态可解除该限制；但由于 2^18-1 个后端已超出现实配置，通常
 * 不值得这样做。即使移除此限制，仍不能 a) 超过 2^23-1，因为 inval.c 将 ProcNumber
 * 存为 3 字节有符号整数，b) 超过 INT_MAX/4，因为某些位置计算 4*MaxBackends 时没有
 * 溢出检查。InitializeMaxBackends() 会检查配置的后端数不超过 MAX_BACKENDS。
 */
#define MAX_BACKENDS_BITS		18
#define MAX_BACKENDS			((1U << MAX_BACKENDS_BITS)-1)

/*
 * Proc number of this backend (same as GetNumberFromPGProc(MyProc))
 */

/*
 * 本后端的进程编号（等同于 GetNumberFromPGProc(MyProc)）。
 */
extern PGDLLIMPORT ProcNumber MyProcNumber;

/* proc number of our parallel session leader, or INVALID_PROC_NUMBER if none */

/* 并行会话领导者的进程编号；若不存在则为 INVALID_PROC_NUMBER。 */
extern PGDLLIMPORT ProcNumber ParallelLeaderProcNumber;

/*
 * The ProcNumber to use for our session's temp relations is normally our own,
 * but parallel workers should use their leader's proc number.
 */

/*
 * 本会话临时关系通常使用本进程的 ProcNumber，但并行工作进程应使用其领导者的进程编号。
 */
#define ProcNumberForTempRelations() \
	(ParallelLeaderProcNumber == INVALID_PROC_NUMBER ? MyProcNumber : ParallelLeaderProcNumber)

#endif							/* PROCNUMBER_H */

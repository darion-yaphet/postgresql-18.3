/*-------------------------------------------------------------------------
 *
 * aio_types.h
 *    AIO related types that are useful to include separately, to reduce the
 *    "include burden".
 *    AIO 相关类型，分开包含很有用，以减少“包含负担”。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/aio_types.h
 *
 * Core Flow:
 * 核心流程：
 * 1. Tracking: Create a 'PgAioWaitRef' to monitor an IO operation's progress across process boundaries.
 *    跟踪：创建一个 'PgAioWaitRef'，用以跨进程边界监控 IO 操作的进度。
 * 2. Targeting: Use 'PgAioTargetData' to define the physical location (rel, block, fork) of the IO.
 *    目标定位：使用 'PgAioTargetData' 定义 IO 的物理位置（关系、块、分叉）。
 * 3. Result Reporting: 'PgAioResult' packs the outcome (status, error bits, result) into a compact 8-byte structure.
 *    结果报告：'PgAioResult' 将结果（状态、错误位、结果）封装进一个紧凑的 8 字节结构中。
 * 4. Error Handling: 'PgAioReturn' combines the result with target info for meaningful error logging.
 *    错误处理：'PgAioReturn' 将结果与目标信息结合，以便进行有意义的错误记录。
 *
 *-------------------------------------------------------------------------
 */
#ifndef AIO_TYPES_H
#define AIO_TYPES_H

#include "storage/block.h"
#include "storage/relfilelocator.h"


typedef struct PgAioHandle PgAioHandle;
typedef struct PgAioHandleCallbacks PgAioHandleCallbacks;
typedef struct PgAioTargetInfo PgAioTargetInfo;

/*
 * A reference to an IO that can be used to wait for the IO (using
 * pgaio_wref_wait()) to complete.
 * 一个 IO 引用，可以用来等待 IO 完成（使用 pgaio_wref_wait()）。
 *
 * These can be passed across process boundaries.
 * 这些可以跨进程边界传递。
 */
typedef struct PgAioWaitRef
{
	/* internal ID identifying the specific PgAioHandle */
	/* 标识特定 PgAioHandle 的内部 ID */
	uint32		aio_index;

	/*
	 * IO handles are reused. To detect if a handle was reused, and thereby
	 * avoid unnecessarily waiting for a newer IO, each time the handle is
	 * reused a generation number is increased.
	 * IO 句柄会被重用。为了检测句柄是否被重用（从而避免不必要地等待较新的 IO），
	 * 每次句柄重用时，代次编号（generation number）都会增加。
	 *
	 * To avoid requiring alignment sufficient for an int64, split the
	 * generation into two.
	 * 为了避免要求达到 int64 所需的对齐，将代次拆分为两个部分。
	 */
	uint32		generation_upper;
	uint32		generation_lower;
} PgAioWaitRef;


/*
 * Information identifying what the IO is being performed on.
 * 标识正在对其执行 IO 的对象的信息。
 *
 * This needs sufficient information to
 * 这需要足够的信息来：
 *
 * a) Reopen the file for the IO if the IO is executed in a context that
 *    cannot use the FD provided initially (e.g. because the IO is executed in
 *    a worker process).
 *    a) 如果 IO 在不能使用最初提供的 FD 的上下文中执行（例如，因为 IO 在工作进程中执行），
 *       则为该 IO 重新打开文件。
 *
 * b) Describe the object the IO is performed on in log / error messages.
 *    b) 在日志/错误消息中描述执行 IO 的对象。
 */
typedef union PgAioTargetData
{
	struct
	{
		RelFileLocator rlocator;	/* physical relation identifier */
		BlockNumber blockNum;	/* blknum relative to begin of reln */
		BlockNumber nblocks;
		ForkNumber	forkNum:8;	/* don't waste 4 byte for four values */
		bool		is_temp:1;	/* proc can be inferred by owning AIO */
		bool		skip_fsync:1;
	}			smgr;
} PgAioTargetData;


/*
 * The status of an AIO operation.
 * AIO 操作的状态。
 */
typedef enum PgAioResultStatus
{
	PGAIO_RS_UNKNOWN,			/* not yet completed / uninitialized */
	PGAIO_RS_OK,
	PGAIO_RS_PARTIAL,			/* did not fully succeed, no warning/error */
	PGAIO_RS_WARNING,			/* [partially] succeeded, with a warning */
	PGAIO_RS_ERROR,				/* failed entirely */
} PgAioResultStatus;


/*
 * Result of IO operation, visible only to the initiator of IO.
 * IO 操作的结果，仅对 IO 发起者可见。
 *
 * We need to be careful about the size of PgAioResult, as it is embedded in
 * every PgAioHandle, as well as every PgAioReturn. Currently we assume we can
 * fit it into one 8 byte value, restricting the space for per-callback error
 * data to PGAIO_RESULT_ERROR_BITS.
 * 我们需要小心 PgAioResult 的大小，因为它嵌入在每个 PgAioHandle 以及每个 PgAioReturn 中。
 * 目前我们假设可以将它放入一个 8 字节的值中，将每个回调错误数据的空间限制为 PGAIO_RESULT_ERROR_BITS。
 */
#define PGAIO_RESULT_ID_BITS 6
#define PGAIO_RESULT_STATUS_BITS 3
#define PGAIO_RESULT_ERROR_BITS 23
typedef struct PgAioResult
{
	/*
	 * This is of type PgAioHandleCallbackID, but can't use a bitfield of an
	 * enum, because some compilers treat enums as signed.
	 */
	uint32		id:PGAIO_RESULT_ID_BITS;

	/* of type PgAioResultStatus, see above */
	uint32		status:PGAIO_RESULT_STATUS_BITS;

	/* meaning defined by callback->report */
	/* 含义由 callback->report 定义 */
	uint32		error_data:PGAIO_RESULT_ERROR_BITS;

	int32		result;
} PgAioResult;


StaticAssertDecl(PGAIO_RESULT_ID_BITS +
				 PGAIO_RESULT_STATUS_BITS +
				 PGAIO_RESULT_ERROR_BITS == 32,
				 "PgAioResult bits divided up incorrectly");
StaticAssertDecl(sizeof(PgAioResult) == 8,
				 "PgAioResult has unexpected size");

/*
 * Combination of PgAioResult with minimal metadata about the IO.
 * PgAioResult 与有关 IO 的最小元数据的组合。
 *
 * Contains sufficient information to be able, in case the IO [partially]
 * fails, to log/raise an error under control of the IO issuing code.
 * 包含足够的信息，以便在 IO [部分] 失败的情况下，能够在 IO 发布代码的控制下记录/引发错误。
 */
typedef struct PgAioReturn
{
	PgAioResult result;
	PgAioTargetData target_data;
} PgAioReturn;


#endif							/* AIO_TYPES_H */

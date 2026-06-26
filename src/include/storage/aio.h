/*-------------------------------------------------------------------------
 *
 * aio.h
 *    Main AIO interface
 *    主要 AIO 接口
 *
 * This is the header to include when actually issuing AIO. When just
 * declaring functions involving an AIO related type, it might suffice to
 * include aio_types.h. Initialization related functions are in the dedicated
 * aio_init.h.
 * 当实际发起 AIO 操作时需要包含此头文件。如果仅仅是声明涉及 AIO 相关类型的函数，
 * 包含 aio_types.h 可能就足够了。初始化相关的函数位于专门的 aio_init.h 中。
 *
 * Core Flow Explanation / 核心流程说明:
 * - PostgreSQL's AIO system provides a way to issue asynchronous I/O requests.
 * - PostgreSQL 的 AIO 系统提供了一种发起异步 I/O 请求的方法。
 * - The basic life cycle of an AIO operation is:
 * - 一个 AIO 操作的基本生命周期如下：
 *   1. Acquire: A backend gets an IO handle using pgaio_io_acquire().
 *      获取：后台进程通过 pgaio_io_acquire() 获取一个 IO 句柄。
 *   2. Setup: Set flags, target, and register callbacks. Callbacks are identified
 *      by IDs to be portable across backends.
 *      设置：设置标志、目标（Target）并注册回调函数。回调函数通过 ID 标识，以便在不同后台进程间移植。
 *   3. Start: Initiate the operation (e.g., pgaio_io_start_readv()).
 *      开始：启动操作（例如 pgaio_io_start_readv()）。
 *   4. Submit: IOs can be staged and submitted in batches using pgaio_submit_staged().
 *      提交：IO 操作可以进行分阶段（staged）处理，并使用 pgaio_submit_staged() 批量提交。
 *   5. Wait/Complete: Wait for completion using PgAioWaitRef.
 *      等待/完成：使用 PgAioWaitRef 等待完成。
 *   6. Release: Release the IO handle back to the pool using pgaio_io_release().
 *      释放：使用 pgaio_io_release() 将 IO 句柄释放回线程池。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/aio.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef AIO_H
#define AIO_H

#include "storage/aio_types.h"
#include "storage/procnumber.h"


/* io_uring is incompatible with EXEC_BACKEND */
#if defined(USE_LIBURING) && !defined(EXEC_BACKEND)
#define IOMETHOD_IO_URING_ENABLED
#endif


/* Enum for io_method GUC. */
/* io_method GUC（全局配置参数）的枚举。 */
typedef enum IoMethod
{
	IOMETHOD_SYNC = 0,			/* Synchronous IO */
								/* 同步 IO */
	IOMETHOD_WORKER,			/* AIO worker processes */
								/* AIO 工作后台进程 */
#ifdef IOMETHOD_IO_URING_ENABLED
	IOMETHOD_IO_URING,			/* Linux io_uring */
								/* Linux io_uring 接口 */
#endif
}			IoMethod;

/* We'll default to worker based execution. */
/* 我们将默认使用基于工作进程的执行方式。 */
#define DEFAULT_IO_METHOD IOMETHOD_WORKER


/*
 * Flags for an IO that can be set with pgaio_io_set_flag().
 * 可以使用 pgaio_io_set_flag() 为 IO 设置的标志。
 */
typedef enum PgAioHandleFlags
{
	/*
	 * The IO references backend local memory.
	 * 此 IO 引用了后台本地内存。
	 *
	 * This needs to be set on an IO whenever the IO references process-local
	 * memory. Some IO methods do not support executing IO that references
	 * process local memory and thus need to fall back to executing IO
	 * synchronously for IOs with this flag set.
	 * 当 IO 引用进程本地内存时，需要为此 IO 设置此标志。
	 * 某些 IO 方法不支持引用进程本地内存的 IO 执行，
	 * 因此对于设置了此标志的 IO，需要回退到同步执行。
	 *
	 * Required for correctness.
	 * 为了保证正确性，此项是必需的。
	 */
	PGAIO_HF_REFERENCES_LOCAL = 1 << 1,

	/*
	 * Hint that IO will be executed synchronously.
	 * 提示 IO 将同步执行。
	 *
	 * This can make it a bit cheaper to execute synchronous IO via the AIO
	 * interface, to avoid needing an AIO and non-AIO version of code.
	 * 这可以使通过 AIO 接口执行同步 IO 稍微廉价一些，
	 * 从而避免需要同时开发 AIO 和非 AIO 两个版本的代码。
	 *
	 * Advantageous to set, if applicable, but not required for correctness.
	 * 如果适用，设置此标志是有利的，但并非保证正确性所必需。
	 */
	PGAIO_HF_SYNCHRONOUS = 1 << 0,

	/*
	 * IO is using buffered IO, used to control heuristic in some IO methods.
	 * IO 正在使用缓冲 IO，用于在某些 IO 方法中控制启发式算法。
	 *
	 * Advantageous to set, if applicable, but not required for correctness.
	 * 如果适用，设置此标志是有利的，但并非保证正确性所必需。
	 */
	PGAIO_HF_BUFFERED = 1 << 2,
} PgAioHandleFlags;

/*
 * The IO operations supported by the AIO subsystem.
 * AIO 子系统支持的 IO 操作。
 *
 * This could be in aio_internal.h, as it is not publicly referenced, but
 * PgAioOpData currently *does* need to be public, therefore keeping this
 * public seems to make sense.
 * 这本可以放在 aio_internal.h 中，因为它没有被公开引用，
 * 但 PgAioOpData 目前*确实*需要公开，因此保持其公开性似乎是有意义的。
 */
typedef enum PgAioOp
{
	/* intentionally the zero value, to help catch zeroed memory etc */
	/* 故意将其设为零值，以帮助捕捉零初始化内存等问题 */
	PGAIO_OP_INVALID = 0,

	PGAIO_OP_READV,				/* Vectorized read */
								/* 向量化读取 */
	PGAIO_OP_WRITEV,			/* Vectorized write */
								/* 向量化写入 */

	/**
	 * In the near term we'll need at least:
	 * 在短期内，我们至少需要：
	 * - fsync / fdatasync
	 * - flush_range
	 *
	 * Eventually we'll additionally want at least:
	 * 最终，我们还会额外需要至少：
	 * - send
	 * - recv
	 * - accept
	 **/
} PgAioOp;

#define PGAIO_OP_COUNT	(PGAIO_OP_WRITEV + 1)


/*
 * On what is IO being performed?
 * 正在对什么执行 IO？
 *
 * PgAioTargetID specific behaviour should be implemented in
 * aio_target.c.
 * PgAioTargetID 特定的行为应在 aio_target.c 中实现。
 */
typedef enum PgAioTargetID
{
	/* intentionally the zero value, to help catch zeroed memory etc */
	/* 故意将其设为零值，以帮助捕捉零初始化内存等问题 */
	PGAIO_TID_INVALID = 0,
	PGAIO_TID_SMGR,				/* smgr (storage manager) target */
								/* smgr（存储管理器）目标 */
} PgAioTargetID;

#define PGAIO_TID_COUNT (PGAIO_TID_SMGR + 1)


/*
 * Data necessary for support IO operations (see PgAioOp).
 * 支持 IO 操作所需的数据（参见 PgAioOp）。
 *
 * NB: Note that the FDs in here may *not* be relied upon for re-issuing
 * requests (e.g. for partial reads/writes or in an IO worker) - the FD might
 * be from another process, or closed since. That's not a problem for staged
 * IOs, as all staged IOs are submitted when closing an FD.
 * 注意：这里的 FD 可能*无法*被依赖于重新发起请求（例如分段读写或在 IO 工作进程中）——
 * 该 FD 可能来自另一个进程，或者之后已被关闭。这对于分阶段（staged）的 IO 不是问题，
 * 因为所有分阶段的 IO 在关闭 FD 时都会被提交。
 */
typedef union
{
	struct
	{
		int			fd;
		uint16		iov_length;
		uint64		offset;
	}			read;

	struct
	{
		int			fd;
		uint16		iov_length;
		uint64		offset;
	}			write;
} PgAioOpData;


/*
 * Information the object that IO is executed on. Mostly callbacks that
 * operate on PgAioTargetData.
 * 有关执行 IO 的对象的信息。主要是针对 PgAioTargetData 进行操作的回调函数。
 *
 * typedef is in aio_types.h
 */
struct PgAioTargetInfo
{
	/*
	 * To support executing using worker processes, the file descriptor for an
	 * IO may need to be reopened in a different process.
	 * 为了支持使用工作进程执行，一个 IO 的文件描述符可能需要在不同的进程中重新打开。
	 */
	void		(*reopen) (PgAioHandle *ioh);

	/* describe the target of the IO, used for log messages and views */
	/* 描述 IO 的目标，用于日志消息和视图 */
	char	   *(*describe_identity) (const PgAioTargetData *sd);

	/* name of the target, used in log messages / views */
	/* 目标的名称，用于日志消息/视图 */
	const char *name;
};


/*
 * IDs for callbacks that can be registered on an IO.
 * 可以注册在 IO 上的回调函数的 ID。
 *
 * Callbacks are identified by an ID rather than a function pointer. There are
 * two main reasons:
 * 回调函数通过 ID 而非函数指针来标识。主要有两个原因：
 *
 * 1) Memory within PgAioHandle is precious, due to the number of PgAioHandle
 *    structs in pre-allocated shared memory.
 *    PgAioHandle 内部的内存非常宝贵，因为预分配的共享内存中存有大量的 PgAioHandle 结构体。
 *
 * 2) Due to EXEC_BACKEND function pointers are not necessarily stable between
 *    different backends, therefore function pointers cannot directly be in
 *    shared memory.
 *    由于使用了 EXEC_BACKEND，函数指针在不同的后台进程之间不一定稳定，
 *    因此函数指针不能直接存储在共享内存中。
 *
 * Without 2), we could fairly easily allow to add new callbacks, by filling a
 * ID->pointer mapping table on demand. In the presence of 2 that's still
 * doable, but harder, because every process has to re-register the pointers
 * so that a local ID->"backend local pointer" mapping can be maintained.
 * 如果没有第 2 点，我们可以通过按需填充 ID->指针映射表，相当容易地允许添加新的回调。
 * 在存在第 2 点的情况下，这仍然是可行的，但更困难，因为每个进程都必须重新注册指针，
 * 以便维护一个本地 ID->“后台本地指针”的映射。
 */
typedef enum PgAioHandleCallbackID
{
	PGAIO_HCB_INVALID = 0,

	PGAIO_HCB_MD_READV,

	PGAIO_HCB_SHARED_BUFFER_READV,

	PGAIO_HCB_LOCAL_BUFFER_READV,
} PgAioHandleCallbackID;

#define PGAIO_HCB_MAX	PGAIO_HCB_LOCAL_BUFFER_READV
StaticAssertDecl(PGAIO_HCB_MAX < (1 << PGAIO_RESULT_ID_BITS),
				 "PGAIO_HCB_MAX is too big for PGAIO_RESULT_ID_BITS");


typedef void (*PgAioHandleCallbackStage) (PgAioHandle *ioh, uint8 cb_flags);
typedef PgAioResult (*PgAioHandleCallbackComplete) (PgAioHandle *ioh, PgAioResult prior_result, uint8 cb_flags);
typedef void (*PgAioHandleCallbackReport) (PgAioResult result, const PgAioTargetData *target_data, int elevel);

/* typedef is in aio_types.h */
struct PgAioHandleCallbacks
{
	/*
	 * Prepare resources affected by the IO for execution. This could e.g.
	 * include moving ownership of buffer pins to the AIO subsystem.
	 * 为执行准备受此 IO 影响的资源。例如，这可以包括将缓冲区引脚（pins）
	 * 的所有权转移到 AIO 子系统。
	 */
	PgAioHandleCallbackStage stage;

	/*
	 * Update the state of resources affected by the IO to reflect completion
	 * of the IO. This could e.g. include updating shared buffer state to
	 * signal the IO has finished.
	 * 更新受此 IO 影响的资源状态，以反映 IO 的完成。
	 * 例如，这可以包括更新共享缓冲区状态以发出 IO 已结束的信号。
	 *
	 * The _shared suffix indicates that this is executed by the backend that
	 * completed the IO, which may or may not be the backend that issued the
	 * IO.  Obviously the callback thus can only modify resources in shared
	 * memory.
	 * _shared 后缀表示这是由完成该 IO 的后台进程执行的，
	 * 该进程可能是、也可能不是发起该 IO 的后台进程。
	 * 显然，该回调因此只能修改共享内存中的资源。
	 *
	 * The latest registered callback is called first. This allows
	 * higher-level code to register callbacks that can rely on callbacks
	 * registered by lower-level code to already have been executed.
	 * 最后注册的回调会首先被调用。这允许更高级别的代码在注册回调时，
	 * 能够依赖于较低级别代码注册的回调已经执行完毕。
	 *
	 * NB: This is called in a critical section. Errors can be signalled by
	 * the callback's return value, it's the responsibility of the IO's issuer
	 * to react appropriately.
	 * 注意：这是在关键段中调用的。错误可以通过回调函数的返回值发出信号，
	 * 由 IO 的发起方负责做出适当的反应。
	 */
	PgAioHandleCallbackComplete complete_shared;

	/*
	 * Like complete_shared, except called in the issuing backend.
	 * 与 complete_shared 类似，区别在于这是在发起请求的后台进程中调用的。
	 *
	 * This variant of the completion callback is useful when backend-local
	 * state has to be updated to reflect the IO's completion. E.g. a
	 * temporary buffer's BufferDesc isn't accessible in complete_shared.
	 * 当必须更新后台本地状态以反映 IO 完成时，此变体的完成回调很有用。
	 * 例如，临时缓冲区的 BufferDesc 在 complete_shared 中是不可访问的。
	 *
	 * Local callbacks are only called after complete_shared for all
	 * registered callbacks has been called.
	 * 本地回调仅在所有已注册回调的 complete_shared 被调用之后才会被调用。
	 */
	PgAioHandleCallbackComplete complete_local;

	/*
	 * Report the result of an IO operation. This is e.g. used to raise an
	 * error after an IO failed at the appropriate time (i.e. not when the IO
	 * failed, but under control of the code that issued the IO).
	 * 报告一个 IO 操作的结果。例如，这用于在 IO 失败后，
	 * 在适当的时间抛出错误（即：不是在 IO 失败时抛出，而是在发起该 IO 的代码的控制下抛出）。
	 */
	PgAioHandleCallbackReport report;
};



/*
 * How many callbacks can be registered for one IO handle. Currently we only
 * need two, but it's not hard to imagine needing a few more.
 * 一个 IO 句柄可以注册多少个回调函数。目前我们只需要两个，
 * 但不难想象可能额外需要更多。
 */
#define PGAIO_HANDLE_MAX_CALLBACKS	4



/* --------------------------------------------------------------------------------
 * IO Handles
 * --------------------------------------------------------------------------------
 */

/* functions in aio.c */
struct ResourceOwnerData;

/*
 * pgaio_io_acquire -
 *    Acquire an IO handle, optionally associating it with a resource owner.
 * pgaio_io_acquire -
 *    获取一个 IO 句柄，并可选择将其与资源持有者（resource owner）关联。
 */
extern PgAioHandle *pgaio_io_acquire(struct ResourceOwnerData *resowner, PgAioReturn *ret);

/*
 * pgaio_io_acquire_nb -
 *    Non-blocking version of pgaio_io_acquire.
 * pgaio_io_acquire_nb -
 *    pgaio_io_acquire 的非阻塞版本。
 */
extern PgAioHandle *pgaio_io_acquire_nb(struct ResourceOwnerData *resowner, PgAioReturn *ret);

/*
 * pgaio_io_release -
 *    Release an IO handle back to the pool.
 * pgaio_io_release -
 *    将 IO 句柄释放回线程池。
 */
extern void pgaio_io_release(PgAioHandle *ioh);

struct dlist_node;

/*
 * pgaio_io_release_resowner -
 *    Release IO handles associated with a resource owner (e.g., during transaction abort).
 * pgaio_io_release_resowner -
 *    释放与资源持有者关联的 IO 句柄（例如在事务中止期间）。
 */
extern void pgaio_io_release_resowner(struct dlist_node *ioh_node, bool on_error);

/*
 * pgaio_io_set_flag -
 *    Set a flag on an IO handle.
 * pgaio_io_set_flag -
 *    在 IO 句柄上设置标志。
 */
extern void pgaio_io_set_flag(PgAioHandle *ioh, PgAioHandleFlags flag);

/*
 * pgaio_io_get_id -
 *    Get the unique identifier of an IO handle.
 * pgaio_io_get_id -
 *    获取 IO 句柄的唯一标识符。
 */
extern int	pgaio_io_get_id(PgAioHandle *ioh);

/*
 * pgaio_io_get_owner -
 *    Get the ProcNumber of the backend that owns the IO handle.
 * pgaio_io_get_owner -
 *    获取拥有该 IO 句柄的后台进程的 ProcNumber。
 */
extern ProcNumber pgaio_io_get_owner(PgAioHandle *ioh);

/*
 * pgaio_io_get_wref -
 *    Obtain a wait reference for the specified IO handle.
 * pgaio_io_get_wref -
 *    获取指定 IO 句柄的等待引用。
 */
extern void pgaio_io_get_wref(PgAioHandle *ioh, PgAioWaitRef *iow);

/* functions in aio_io.c */
struct iovec;

/*
 * pgaio_io_get_iovec -
 *    Retrieve the iovec associated with the IO handle.
 * pgaio_io_get_iovec -
 *    检索与 IO 句柄关联的 iovec。
 */
extern int	pgaio_io_get_iovec(PgAioHandle *ioh, struct iovec **iov);

/*
 * pgaio_io_get_op -
 *    Get the type of IO operation.
 * pgaio_io_get_op -
 *    获取 IO 操作的类型。
 */
extern PgAioOp pgaio_io_get_op(PgAioHandle *ioh);

/*
 * pgaio_io_get_op_data -
 *    Get the operation-specific data for the IO.
 * pgaio_io_get_op_data -
 *    获取此 IO 的特定操作数据。
 */
extern PgAioOpData *pgaio_io_get_op_data(PgAioHandle *ioh);

/*
 * pgaio_io_start_readv -
 *    Initiate a vectorized read operation.
 * pgaio_io_start_readv -
 *    发起向量化读取操作。
 */
extern void pgaio_io_start_readv(PgAioHandle *ioh,
								 int fd, int iovcnt, uint64 offset);

/*
 * pgaio_io_start_writev -
 *    Initiate a vectorized write operation.
 * pgaio_io_start_writev -
 *    发起向量化写入操作。
 */
extern void pgaio_io_start_writev(PgAioHandle *ioh,
								  int fd, int iovcnt, uint64 offset);

/* functions in aio_target.c */

/*
 * pgaio_io_set_target -
 *    Set the target for the IO operation.
 * pgaio_io_set_target -
 *    设置 IO 操作的目标。
 */
extern void pgaio_io_set_target(PgAioHandle *ioh, PgAioTargetID targetid);

/*
 * pgaio_io_has_target -
 *    Check if the IO handle has a target assigned.
 * pgaio_io_has_target -
 *    检查 IO 句柄是否分配了目标。
 */
extern bool pgaio_io_has_target(PgAioHandle *ioh);

/*
 * pgaio_io_get_target_data -
 *    Get the data associated with the IO target.
 * pgaio_io_get_target_data -
 *    获取与 IO 目标关联的数据。
 */
extern PgAioTargetData *pgaio_io_get_target_data(PgAioHandle *ioh);

/*
 * pgaio_io_get_target_description -
 *    Get a human-readable description of the IO target.
 * pgaio_io_get_target_description -
 *    获取 IO 目标的可读描述。
 */
extern char *pgaio_io_get_target_description(PgAioHandle *ioh);

/* functions in aio_callback.c */

/*
 * pgaio_io_register_callbacks -
 *    Register a set of callbacks for the IO handle using a callback ID.
 * pgaio_io_register_callbacks -
 *    使用回调 ID 为 IO 句柄注册一组回调函数。
 */
extern void pgaio_io_register_callbacks(PgAioHandle *ioh, PgAioHandleCallbackID cb_id,
										uint8 cb_data);

/*
 * pgaio_io_set_handle_data_64/32 -
 *    Stores arbitrary data in the handle.
 * pgaio_io_set_handle_data_64/32 -
 *    在句柄中存储任意数据。
 */
extern void pgaio_io_set_handle_data_64(PgAioHandle *ioh, uint64 *data, uint8 len);
extern void pgaio_io_set_handle_data_32(PgAioHandle *ioh, uint32 *data, uint8 len);

/*
 * pgaio_io_get_handle_data -
 *    Retrieves data stored in the handle.
 * pgaio_io_get_handle_data -
 *    检索存储在句柄中的数据。
 */
extern uint64 *pgaio_io_get_handle_data(PgAioHandle *ioh, uint8 *len);



/* --------------------------------------------------------------------------------
 * IO Wait References
 * IO 等待引用
 * --------------------------------------------------------------------------------
 */

/*
 * pgaio_wref_clear -
 *    Invalidate a wait reference.
 * pgaio_wref_clear -
 *    使等待引用失效。
 */
extern void pgaio_wref_clear(PgAioWaitRef *iow);

/*
 * pgaio_wref_valid -
 *    Check if a wait reference is valid.
 * pgaio_wref_valid -
 *    检查等待引用是否有效。
 */
extern bool pgaio_wref_valid(PgAioWaitRef *iow);

/*
 * pgaio_wref_get_id -
 *    Get the IO handle identifier from a wait reference.
 * pgaio_wref_get_id -
 *    从等待引用中获取 IO 句柄标识符。
 */
extern int	pgaio_wref_get_id(PgAioWaitRef *iow);

/*
 * pgaio_wref_wait -
 *    Wait for the IO associated with the wait reference to complete.
 * pgaio_wref_wait -
 *    等待与等待引用相关联的 IO 完成。
 */
extern void pgaio_wref_wait(PgAioWaitRef *iow);

/*
 * pgaio_wref_check_done -
 *    Poll whether the IO associated with the wait reference is finished.
 * pgaio_wref_check_done -
 *    轮询与等待引用相关联的 IO 是否已结束。
 */
extern bool pgaio_wref_check_done(PgAioWaitRef *iow);



/* --------------------------------------------------------------------------------
 * IO Result
 * IO 结果
 * --------------------------------------------------------------------------------
 */

/*
 * pgaio_result_report -
 *    Report IO failure or status at a suitable time.
 * pgaio_result_report -
 *    在合适的时间报告 IO 失败或状态。
 */
extern void pgaio_result_report(PgAioResult result, const PgAioTargetData *target_data,
								int elevel);



/* --------------------------------------------------------------------------------
 * Actions on multiple IOs.
 * 针对多个 IO 的操作。
 * --------------------------------------------------------------------------------
 */

/*
 * pgaio_enter_batchmode -
 *    Enter a mode that delays IO submission.
 * pgaio_enter_batchmode -
 *    进入延迟提交 IO 的模式。
 */
extern void pgaio_enter_batchmode(void);

/*
 * pgaio_exit_batchmode -
 *    Exit delay-submission mode, potentially submitting IOs.
 * pgaio_exit_batchmode -
 *    退出延迟提交模式，并可能提交 IO 操作。
 */
extern void pgaio_exit_batchmode(void);

/*
 * pgaio_submit_staged -
 *    Submit all IOs that have been staged but not yet issued.
 * pgaio_submit_staged -
 *    提交所有已分阶段（staged）但尚未发起的 IO 操作。
 */
extern void pgaio_submit_staged(void);

/*
 * pgaio_have_staged -
 *    Check if there are any staged IOs waiting for submission.
 * pgaio_have_staged -
 *    检查是否有任何分阶段的 IO 正在等待提交。
 */
extern bool pgaio_have_staged(void);



/* --------------------------------------------------------------------------------
 * Other
 * 其他
 * --------------------------------------------------------------------------------
 */

/*
 * pgaio_closing_fd -
 *    Notification that an FD is being closed, so staged IOs should be submitted.
 * pgaio_closing_fd -
 *    FD 正在关闭的通知，因此应提交分阶段的 IO 操作。
 */
extern void pgaio_closing_fd(int fd);



/* GUCs */
/* 全局配置参数 (GUCs) */

/*
 * io_method -
 *    The selected AIO execution method (sync, worker, io_uring).
 * io_method -
 *    选择的 AIO 执行方法（同步、工作进程、io_uring）。
 */
extern PGDLLIMPORT int io_method;

/*
 * io_max_concurrency -
 *    Maximum number of concurrent AIO operations.
 * io_max_concurrency -
 *    最大并发 AIO 操作数。
 */
extern PGDLLIMPORT int io_max_concurrency;


#endif							/* AIO_H */

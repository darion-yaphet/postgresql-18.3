/*-------------------------------------------------------------------------
 *
 * aio_internal.h
 *    AIO related declarations that should only be used by the AIO subsystem
 *    internally.
 *
 *    仅供 AIO 子系统内部使用的 AIO 相关声明。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/aio_internal.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef AIO_INTERNAL_H
#define AIO_INTERNAL_H


#include "lib/ilist.h"
#include "port/pg_iovec.h"
#include "storage/aio.h"
#include "storage/condition_variable.h"


/*
 * The maximum number of IOs that can be batch submitted at once.
 *
 * 一次可批量提交的最大 IO 数量。
 */
#define PGAIO_SUBMIT_BATCH_SIZE 32



/*
 * State machine for handles. With some exceptions, noted below, handles move
 * linearly through all states.
 *
 * State changes should all go through pgaio_io_update_state().
 *
 * Note that the externally visible functions to start IO
 * (e.g. FileStartReadV(), via pgaio_io_start_readv()) move an IO from
 * PGAIO_HS_HANDED_OUT to at least PGAIO_HS_STAGED and at most
 * PGAIO_HS_COMPLETED_LOCAL (at which point the handle will be reused).
 *
 * 句柄的状态机。除下文注明的例外外，句柄会线性地经过所有状态。
 *
 * 所有状态变更都应通过 pgaio_io_update_state() 完成。
 *
 * 请注意，启动 IO 的外部可见函数（例如通过 pgaio_io_start_readv() 调用的
 * FileStartReadV()）会将 IO 从 PGAIO_HS_HANDED_OUT 移动到至少
 * PGAIO_HS_STAGED、至多 PGAIO_HS_COMPLETED_LOCAL（此时句柄将被重用）。
 */
typedef enum PgAioHandleState
{
	/* not in use */

	/* 未使用。 */
	PGAIO_HS_IDLE = 0,

	/*
	 * Returned by pgaio_io_acquire(). The next state is either DEFINED (if
	 * pgaio_io_start_*() is called), or IDLE (if pgaio_io_release() is
	 * called).
	 *
	 * 由 pgaio_io_acquire() 返回。下一状态为 DEFINED（若调用
	 * pgaio_io_start_*()）或 IDLE（若调用 pgaio_io_release()）。
	 */
	PGAIO_HS_HANDED_OUT,

	/*
	 * pgaio_io_start_*() has been called, but IO is not yet staged. At this
	 * point the handle has all the information for the IO to be executed.
	 *
	 * 已调用 pgaio_io_start_*()，但 IO 尚未暂存。此时句柄已具备执行 IO 所需的
	 * 全部信息。
	 */
	PGAIO_HS_DEFINED,

	/*
	 * stage() callbacks have been called, handle ready to be submitted for
	 * execution. Unless in batchmode (see c.f. pgaio_enter_batchmode()), the
	 * IO will be submitted immediately after.
	 *
	 * 已调用 stage() 回调，句柄可提交执行。除非处于批处理模式（参见
	 * pgaio_enter_batchmode()），否则随后会立即提交该 IO。
	 */
	PGAIO_HS_STAGED,

	/* IO has been submitted to the IO method for execution */

	/* IO 已提交给 IO 方法执行。 */
	PGAIO_HS_SUBMITTED,

	/* IO finished, but result has not yet been processed */

	/* IO 已结束，但结果尚未处理。 */
	PGAIO_HS_COMPLETED_IO,

	/*
	 * IO completed, shared completion has been called.
	 *
	 * If the IO completion occurs in the issuing backend, local callbacks
	 * will immediately be called. Otherwise the handle stays in
	 * COMPLETED_SHARED until the issuing backend waits for the completion of
	 * the IO.
	 *
	 * IO 已完成，并且已调用共享完成回调。
	 *
	 * 如果 IO 完成发生在发起后端，本地回调将立即被调用。否则，句柄将保持在
	 * COMPLETED_SHARED，直到发起后端等待 IO 完成。
	 */
	PGAIO_HS_COMPLETED_SHARED,

	/*
	 * IO completed, local completion has been called.
	 *
	 * After this the handle will be made reusable and go into IDLE state.
	 *
	 * IO 已完成，并且已调用本地完成回调。
	 *
	 * 此后句柄会变为可重用并进入 IDLE 状态。
	 */
	PGAIO_HS_COMPLETED_LOCAL,
} PgAioHandleState;


struct ResourceOwnerData;

/*
 * Typedef is in aio_types.h
 *
 * We don't use the underlying enums for state, target and op to avoid wasting
 * space. We tried using bitfields, but several compilers generate rather
 * horrid code for that.
 *
 * typedef 位于 aio_types.h 中。
 *
 * 我们不使用状态、目标和操作的底层枚举，以避免浪费空间。我们尝试过使用位域，
 * 但若干编译器会为此生成相当糟糕的代码。
 */
struct PgAioHandle
{
	/* all state updates should go through pgaio_io_update_state() */

	/* 所有状态更新都应通过 pgaio_io_update_state()。 */
	uint8		state;

	/* what are we operating on */

	/* 正在操作的对象。 */
	uint8		target;

	/* which IO operation */

	/* 使用的 IO 操作。 */
	uint8		op;

	/* bitfield of PgAioHandleFlags */

	/* PgAioHandleFlags 的位域。 */
	uint8		flags;

	uint8		num_callbacks;

	/* using the proper type here would use more space */

	/* 此处使用正确的类型会占用更多空间。 */
	uint8		callbacks[PGAIO_HANDLE_MAX_CALLBACKS];

	/* data forwarded to each callback */

	/* 转交给每个回调的数据。 */
	uint8		callbacks_data[PGAIO_HANDLE_MAX_CALLBACKS];

	/*
	 * Length of data associated with handle using
	 * pgaio_io_set_handle_data_*().
	 *
	 * 使用 pgaio_io_set_handle_data_*() 与句柄关联的数据长度。
	 */
	uint8		handle_data_len;

	/* XXX: could be optimized out with some pointer math */

	/* XXX：可通过一些指针运算优化掉。 */
	int32		owner_procno;

	/* raw result of the IO operation */

	/* IO 操作的原始结果。 */
	int32		result;

	/**
	 * In which list the handle is registered, depends on the state:
	 * - IDLE, in per-backend list
	 * - HANDED_OUT - not in a list
	 * - DEFINED - not in a list
	 * - STAGED - in per-backend staged array
	 * - SUBMITTED - in issuer's in_flight list
	 * - COMPLETED_IO - in issuer's in_flight list
	 * - COMPLETED_SHARED - in issuer's in_flight list
	 *
	 * 句柄注册到的列表取决于状态：
	 * - IDLE：位于每后端列表中
	 * - HANDED_OUT：不在列表中
	 * - DEFINED：不在列表中
	 * - STAGED：位于每后端暂存数组中
	 * - SUBMITTED：位于发起方的 in_flight 列表中
	 * - COMPLETED_IO：位于发起方的 in_flight 列表中
	 * - COMPLETED_SHARED：位于发起方的 in_flight 列表中
	 **/
	dlist_node	node;

	struct ResourceOwnerData *resowner;
	dlist_node	resowner_node;

	/* incremented every time the IO handle is reused */

	/* 每次重用 IO 句柄时递增。 */
	uint64		generation;

	/*
	 * To wait for the IO to complete other backends can wait on this CV. Note
	 * that, if in SUBMITTED state, a waiter first needs to check if it needs
	 * to do work via IoMethodOps->wait_one().
	 *
	 * 为等待 IO 完成，其他后端可以在此条件变量上等待。请注意，如果处于
	 * SUBMITTED 状态，等待者首先需要通过 IoMethodOps->wait_one() 检查是否
	 * 需要执行工作。
	 */
	ConditionVariable cv;

	/* result of shared callback, passed to issuer callback */

	/* 共享回调的结果，传递给发起方回调。 */
	PgAioResult distilled_result;

	/*
	 * Index into PgAioCtl->iovecs and PgAioCtl->handle_data.
	 *
	 * At the moment there's no need to differentiate between the two, but
	 * that won't necessarily stay that way.
	 *
	 * PgAioCtl->iovecs 和 PgAioCtl->handle_data 中的索引。
	 *
	 * 目前无需区分两者，但这种情况未必会保持不变。
	 */
	uint32		iovec_off;

	/*
	 * If not NULL, this memory location will be updated with information
	 * about the IOs completion iff the issuing backend learns about the IOs
	 * completion.
	 *
	 * 如果不为 NULL，则仅当发起后端获知 IO 完成时，此内存位置才会更新为有关
	 * IO 完成的信息。
	 */
	PgAioReturn *report_return;

	/* Data necessary for the IO to be performed */

	/* 执行 IO 所需的数据。 */
	PgAioOpData op_data;

	/*
	 * Data necessary to identify the object undergoing IO to higher-level
	 * code. Needs to be sufficient to allow another backend to reopen the
	 * file.
	 *
	 * 高层代码识别正在进行 IO 的对象所需的数据。它必须足以让另一个后端重新打开
	 * 该文件。
	 */
	PgAioTargetData target_data;
};


typedef struct PgAioBackend
{
	/* index into PgAioCtl->io_handles */

	/* PgAioCtl->io_handles 中的索引。 */
	uint32		io_handle_off;

	/* IO Handles that currently are not used */

	/* 当前未使用的 IO 句柄。 */
	dclist_head idle_ios;

	/*
	 * Only one IO may be returned by pgaio_io_acquire()/pgaio_io_acquire_nb()
	 * without having been either defined (by actually associating it with IO)
	 * or released (with pgaio_io_release()). This restriction is necessary to
	 * guarantee that we always can acquire an IO. ->handed_out_io is used to
	 * enforce that rule.
	 *
	 * 只有一个由 pgaio_io_acquire()/pgaio_io_acquire_nb() 返回的 IO 可以尚未被
	 * 定义（通过实际关联 IO）或释放（通过 pgaio_io_release()）。该限制保证我们
	 * 始终可以获取一个 IO。->handed_out_io 用于强制执行此规则。
	 */
	PgAioHandle *handed_out_io;

	/* Are we currently in batchmode? See pgaio_enter_batchmode(). */

	/* 当前是否处于批处理模式？参见 pgaio_enter_batchmode()。 */
	bool		in_batchmode;

	/*
	 * IOs that are defined, but not yet submitted.
 *
 * 已定义但尚未提交的 IO。
	 */
	uint16		num_staged_ios;
	PgAioHandle *staged_ios[PGAIO_SUBMIT_BATCH_SIZE];

	/*
	 * List of in-flight IOs. Also contains IOs that aren't strictly speaking
	 * in-flight anymore, but have been waited-for and completed by another
	 * backend. Once this backend sees such an IO it'll be reclaimed.
	 *
	 * The list is ordered by submission time, with more recently submitted
	 * IOs being appended at the end.
 *
 * 正在进行的 IO 列表。它也包含严格来说已不再进行、但已由其他后端等待并完成的
 * IO。一旦该后端看到此类 IO，它将被回收。
 *
 * 此列表按提交时间排序，较新提交的 IO 会附加在末尾。
	 */
	dclist_head in_flight_ios;
} PgAioBackend;


typedef struct PgAioCtl
{
	int			backend_state_count;
	PgAioBackend *backend_state;

	/*
	 * Array of iovec structs. Each iovec is owned by a specific backend. The
	 * allocation is in PgAioCtl to allow the maximum number of iovecs for
	 * individual IOs to be configurable with PGC_POSTMASTER GUC.
 *
 * iovec 结构体数组。每个 iovec 由一个特定后端拥有。该分配位于 PgAioCtl 中，
 * 以允许通过 PGC_POSTMASTER GUC 配置单个 IO 的最大 iovec 数。
	 */
	uint32		iovec_count;
	struct iovec *iovecs;

	/*
	 * For, e.g., an IO covering multiple buffers in shared / temp buffers, we
	 * need to get Buffer IDs during completion to be able to change the
	 * BufferDesc state accordingly. This space can be used to store e.g.
	 * Buffer IDs.  Note that the actual iovec might be shorter than this,
	 * because we combine neighboring pages into one larger iovec entry.
 *
 * 例如，对于覆盖共享／临时缓冲区中多个缓冲区的 IO，我们需要在完成期间获取
 * Buffer ID，以便相应地更改 BufferDesc 状态。此空间可用于存储 Buffer ID 等
 * 数据。请注意，实际 iovec 可能短于此空间，因为我们会将相邻页面合并成一个更大的
 * iovec 条目。
	 */
	uint64	   *handle_data;

	uint32		io_handle_count;
	PgAioHandle *io_handles;
} PgAioCtl;



/*
 * Callbacks used to implement an IO method.
 *
 * 用于实现 IO 方法的回调。
 */
typedef struct IoMethodOps
{
	/* properties */

	/* 属性。 */

	/*
	 * If an FD is about to be closed, do we need to wait for all in-flight
	 * IOs referencing that FD?
	 *
	 * 如果一个 FD 即将关闭，是否需要等待所有引用该 FD 的正在进行的 IO？
	 */
	bool		wait_on_fd_before_close;


	/* global initialization */

	/* 全局初始化。 */

	/*
	 * Amount of additional shared memory to reserve for the io_method. Called
	 * just like a normal ipci.c style *Size() function. Optional.
	 *
	 * 为 io_method 预留的额外共享内存量。其调用方式与普通 ipci.c 风格的
	 * *Size() 函数相同。可选。
	 */
	size_t		(*shmem_size) (void);

	/*
	 * Initialize shared memory. First time is true if AIO's shared memory was
	 * just initialized, false otherwise. Optional.
	 *
	 * 初始化共享内存。若刚初始化 AIO 的共享内存，first_time 为 true；否则为
	 * false。可选。
	 */
	void		(*shmem_init) (bool first_time);

	/*
	 * Per-backend initialization. Optional.
 *
 * 每后端初始化。可选。
	 */
	void		(*init_backend) (void);


	/* handling of IOs */

	/* IO 的处理。 */

	/* optional */

	/* 可选。 */
	bool		(*needs_synchronous_execution) (PgAioHandle *ioh);

	/*
	 * Start executing passed in IOs.
	 *
	 * Shall advance state to at least PGAIO_HS_SUBMITTED.  (By the time this
	 * returns, other backends might have advanced the state further.)
	 *
	 * Will not be called if ->needs_synchronous_execution() returned true.
	 *
	 * num_staged_ios is <= PGAIO_SUBMIT_BATCH_SIZE.
	 *
	 * Always called in a critical section.
	 *
	 * 开始执行传入的 IO。
	 *
	 * 应将状态至少推进到 PGAIO_HS_SUBMITTED。（在此函数返回时，其他后端可能已将
	 * 状态推进得更远。）
	 *
	 * 如果 ->needs_synchronous_execution() 返回 true，则不会调用此函数。
	 *
	 * num_staged_ios 小于或等于 PGAIO_SUBMIT_BATCH_SIZE。
	 *
	 * 始终在关键段中调用。
	 */
	int			(*submit) (uint16 num_staged_ios, PgAioHandle **staged_ios);

	/* ---
	 * Wait for the IO to complete. Optional.
	 *
	 * On return, state shall be on of
	 * - PGAIO_HS_COMPLETED_IO
	 * - PGAIO_HS_COMPLETED_SHARED
	 * - PGAIO_HS_COMPLETED_LOCAL
	 *
	 * The callback must not block if the handle is already in one of those
	 * states, or has been reused (see pgaio_io_was_recycled()).  If, on
	 * return, the state is PGAIO_HS_COMPLETED_IO, state will reach
	 * PGAIO_HS_COMPLETED_SHARED without further intervention by the IO
	 * method.
	 *
	 * If not provided, it needs to be guaranteed that the IO method calls
	 * pgaio_io_process_completion() without further interaction by the
	 * issuing backend.
	 * ---
	 *
	 * 等待 IO 完成。可选。
	 *
	 * 返回时，状态应为以下之一：
	 * - PGAIO_HS_COMPLETED_IO
	 * - PGAIO_HS_COMPLETED_SHARED
	 * - PGAIO_HS_COMPLETED_LOCAL
	 *
	 * 如果句柄已处于这些状态之一或已被重用（参见 pgaio_io_was_recycled()），
	 * 回调不得阻塞。如果返回时状态为 PGAIO_HS_COMPLETED_IO，状态将在无需 IO
	 * 方法进一步干预的情况下到达 PGAIO_HS_COMPLETED_SHARED。
	 *
	 * 若未提供该回调，必须保证 IO 方法无需发起后端进一步交互即可调用
	 * pgaio_io_process_completion()。
	 * ---
	 */
	void		(*wait_one) (PgAioHandle *ioh,
							 uint64 ref_generation);
} IoMethodOps;


/* aio.c */

/* aio.c 中的内部接口。 */

/* Check whether an IO handle was recycled and return its observed state.
 * The function compares the generation before callers act on a reused handle.
 *
 * 检查 IO 句柄是否被回收，并返回观察到的状态。
 * 该函数会先比较代次，再让调用者操作可能被重用的句柄。
 */
extern bool pgaio_io_was_recycled(PgAioHandle *ioh, uint64 ref_generation, PgAioHandleState *state);

/* Stage an IO operation on a configured handle.
 * The function records the operation and advances the handle toward submission.
 *
 * 在已配置的句柄上暂存 IO 操作。
 * 该函数记录操作并将句柄推进到提交前的状态。
 */
extern void pgaio_io_stage(PgAioHandle *ioh, PgAioOp op);

/* Process the completion result of an IO operation.
 * The function records the result and drives shared completion processing.
 *
 * 处理 IO 操作的完成结果。
 * 该函数记录结果并推进共享完成处理。
 */
extern void pgaio_io_process_completion(PgAioHandle *ioh, int result);

/* Prepare a staged IO for submission.
 * The function performs pre-submit state and callback handling.
 *
 * 为提交准备已暂存的 IO。
 * 该函数执行提交前的状态和回调处理。
 */
extern void pgaio_io_prepare_submit(PgAioHandle *ioh);

/* Determine whether an IO must execute synchronously.
 * The function delegates the decision to the selected IO method.
 *
 * 确定 IO 是否必须同步执行。
 * 该函数将决定委托给所选 IO 方法。
 */
extern bool pgaio_io_needs_synchronous_execution(PgAioHandle *ioh);

/* Return a printable name for the handle state.
 * The function maps the current state to diagnostic text.
 *
 * 返回句柄状态的可打印名称。
 * 该函数将当前状态映射为诊断文本。
 */
extern const char *pgaio_io_get_state_name(PgAioHandle *ioh);

/* Return a printable name for an AIO result status.
 * The function maps the status value to diagnostic text.
 *
 * 返回 AIO 结果状态的可打印名称。
 * 该函数将状态值映射为诊断文本。
 */
const char *pgaio_result_status_string(PgAioResultStatus rs);

/* Shut down AIO resources during process exit.
 * The function performs exit-time cleanup using the supplied callback arguments.
 *
 * 在进程退出期间关闭 AIO 资源。
 * 该函数使用给定的回调参数执行退出时清理。
 */
extern void pgaio_shutdown(int code, Datum arg);

/* aio_callback.c */

/* aio_callback.c 中的内部接口。 */

/* Invoke staging callbacks for an IO handle.
 * The function prepares affected resources before the IO is submitted.
 *
 * 调用 IO 句柄的暂存回调。
 * 该函数在提交 IO 前准备受影响的资源。
 */
extern void pgaio_io_call_stage(PgAioHandle *ioh);

/* Invoke shared completion callbacks for an IO handle.
 * The function updates shared resources after IO completion.
 *
 * 调用 IO 句柄的共享完成回调。
 * 该函数在 IO 完成后更新共享资源。
 */
extern void pgaio_io_call_complete_shared(PgAioHandle *ioh);

/* Invoke issuing-backend completion callbacks for an IO handle.
 * The function finalizes local state and returns the distilled result.
 *
 * 调用 IO 句柄的发起后端完成回调。
 * 该函数完成本地状态并返回提炼后的结果。
 */
extern PgAioResult pgaio_io_call_complete_local(PgAioHandle *ioh);

/* aio_io.c */

/* aio_io.c 中的内部接口。 */

/* Execute an IO synchronously through the AIO path.
 * The function performs the operation directly and processes its completion.
 *
 * 通过 AIO 路径同步执行 IO。
 * 该函数直接执行操作并处理其完成。
 */
extern void pgaio_io_perform_synchronously(PgAioHandle *ioh);

/* Return a printable name for an IO operation.
 * The function maps the handle's operation code to diagnostic text.
 *
 * 返回 IO 操作的可打印名称。
 * 该函数将句柄的操作码映射为诊断文本。
 */
extern const char *pgaio_io_get_op_name(PgAioHandle *ioh);

/* Test whether an IO handle references a file descriptor.
 * The function compares the operation's descriptor with the supplied descriptor.
 *
 * 测试 IO 句柄是否引用某个文件描述符。
 * 该函数将操作的描述符与给定描述符进行比较。
 */
extern bool pgaio_io_uses_fd(PgAioHandle *ioh, int fd);

/* Get the handle's iovec array and its length.
 * The function exposes the prepared vector IO data to the selected method.
 *
 * 获取句柄的 iovec 数组及其长度。
 * 该函数向所选方法公开已准备的向量 IO 数据。
 */
extern int	pgaio_io_get_iovec_length(PgAioHandle *ioh, struct iovec **iov);

/* aio_target.c */

/* aio_target.c 中的内部接口。 */

/* Determine whether an IO target can reopen its file.
 * The function checks the target's callback capabilities before retry paths.
 *
 * 确定 IO 目标能否重新打开其文件。
 * 该函数在重试路径前检查目标的回调能力。
 */
extern bool pgaio_io_can_reopen(PgAioHandle *ioh);

/* Reopen the file for an IO target.
 * The function invokes the target-specific reopen path in the current backend.
 *
 * 重新打开 IO 目标的文件。
 * 该函数在当前后端调用目标特有的重新打开路径。
 */
extern void pgaio_io_reopen(PgAioHandle *ioh);

/* Return a printable name for an IO target.
 * The function maps target metadata to diagnostic text.
 *
 * 返回 IO 目标的可打印名称。
 * 该函数将目标元数据映射为诊断文本。
 */
extern const char *pgaio_io_get_target_name(PgAioHandle *ioh);


/*
 * The AIO subsystem has fairly verbose debug logging support. This can be
 * enabled/disabled at build time. The reason for this is that
 * a) the verbosity can make debugging things on higher levels hard
 * b) even if logging can be skipped due to elevel checks, it still causes a
 *    measurable slowdown
 *
 * XXX: This likely should be eventually be disabled by default, at least in
 * non-assert builds.
 *
 * AIO 子系统具有相当详细的调试日志支持。可在构建时启用或禁用。原因是：
 * a) 这种详细程度可能使调试更高层的问题变得困难；
 * b) 即使因 elevel 检查而跳过日志，它仍会造成可测量的减速。
 *
 * XXX：最终可能应当默认禁用它，至少在非断言构建中如此。
 */
#define PGAIO_VERBOSE		1

/*
 * Simple ereport() wrapper that only logs if PGAIO_VERBOSE is defined.
 *
 * This intentionally still compiles the code, guarded by a constant if (0),
 * if verbose logging is disabled, to make it less likely that debug logging
 * is silently broken.
 *
 * The current definition requires passing at least one argument.
 *
 * 仅当定义 PGAIO_VERBOSE 时才记录日志的简单 ereport() 包装器。
 *
 * 如果禁用了详细日志，此包装器仍会在常量 if (0) 的保护下编译代码，
 * 以降低调试日志悄然失效的可能性。
 *
 * 当前定义要求至少传入一个参数。
 */
#define pgaio_debug(elevel, msg, ...)  \
	do { \
		if (PGAIO_VERBOSE) \
			ereport(elevel, \
					errhidestmt(true), errhidecontext(true), \
					errmsg_internal(msg, \
									__VA_ARGS__)); \
	} while(0)

/*
 * Simple ereport() wrapper. Note that the definition requires passing at
 * least one argument.
 *
 * 简单的 ereport() 包装器。请注意，此定义要求至少传入一个参数。
 */
#define pgaio_debug_io(elevel, ioh, msg, ...)  \
	pgaio_debug(elevel, "io %-10d|op %-5s|target %-4s|state %-16s: " msg, \
				pgaio_io_get_id(ioh), \
				pgaio_io_get_op_name(ioh), \
				pgaio_io_get_target_name(ioh), \
				pgaio_io_get_state_name(ioh), \
				__VA_ARGS__)

/* Declarations for the tables of function pointers exposed by each IO method. */

/* 每种 IO 方法公开的函数指针表声明。 */
extern PGDLLIMPORT const IoMethodOps pgaio_sync_ops;
extern PGDLLIMPORT const IoMethodOps pgaio_worker_ops;
#ifdef IOMETHOD_IO_URING_ENABLED
extern PGDLLIMPORT const IoMethodOps pgaio_uring_ops;
#endif

extern PGDLLIMPORT const IoMethodOps *pgaio_method_ops;
extern PGDLLIMPORT PgAioCtl *pgaio_ctl;
extern PGDLLIMPORT PgAioBackend *pgaio_my_backend;



#endif							/* AIO_INTERNAL_H */

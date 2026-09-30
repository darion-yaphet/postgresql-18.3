/*-------------------------------------------------------------------------
 *
 * aio_subsys.h
 *    Interaction with AIO as a subsystem, rather than actually issuing AIO
 *
 *    将 AIO 作为子系统进行交互，而不是实际提交 AIO。
 *
 * This header is for AIO related functionality that's being called by files
 * that don't perform AIO, but interact with the AIO subsystem in some
 * form. E.g. postmaster.c and shared memory initialization need to initialize
 * AIO but don't perform AIO.
 *
 * 此头文件提供由不执行 AIO、但以某种方式与 AIO 子系统交互的文件调用的
 * AIO 相关功能。例如，postmaster.c 和共享内存初始化需要初始化 AIO，但不执行 AIO。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/aio_subsys.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef AIO_SUBSYS_H
#define AIO_SUBSYS_H


/* aio_init.c */

/* aio_init.c 中的接口。 */

/*
 * Return the shared-memory size required by the AIO subsystem.
 * The function accounts for AIO control structures before shared-memory allocation.
 *
 * 返回 AIO 子系统所需的共享内存大小。
 * 该函数在分配共享内存前统计 AIO 控制结构所需的空间。
 */
extern Size AioShmemSize(void);

/*
 * Initialize AIO shared memory.
 * The function creates or attaches the shared AIO control state during startup.
 *
 * 初始化 AIO 共享内存。
 * 该函数在启动期间创建或附接共享的 AIO 控制状态。
 */
extern void AioShmemInit(void);

/*
 * Initialize AIO state for the current backend.
 * The function connects backend-local resources to the initialized AIO subsystem.
 *
 * 为当前后端初始化 AIO 状态。
 * 该函数将后端本地资源连接到已初始化的 AIO 子系统。
 */
extern void pgaio_init_backend(void);


/* aio.c */

/* aio.c 中的接口。 */

/*
 * Clean up AIO state after an error.
 * The function releases or resets outstanding AIO resources for error recovery.
 *
 * 在发生错误后清理 AIO 状态。
 * 该函数释放或重置未完成的 AIO 资源以进行错误恢复。
 */
extern void pgaio_error_cleanup(void);

/*
 * Perform end-of-transaction AIO processing.
 * The function resolves transaction-scoped AIO state according to commit or abort.
 *
 * 执行事务结束时的 AIO 处理。
 * 该函数根据提交或中止处理事务范围内的 AIO 状态。
 */
extern void AtEOXact_Aio(bool is_commit);


/* method_worker.c */

/* method_worker.c 中的接口。 */

/*
 * Report whether AIO worker processes are enabled.
 * The function exposes the configured worker-method availability to callers.
 *
 * 报告 AIO 工作进程是否启用。
 * 该函数向调用者公开已配置的工作进程方法可用性。
 */
extern bool pgaio_workers_enabled(void);

#endif							/* AIO_SUBSYS_H */

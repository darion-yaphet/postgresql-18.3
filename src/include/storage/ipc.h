/*-------------------------------------------------------------------------
 *
 * ipc.h
 *	  POSTGRES inter-process communication definitions.
 *
 * This file is misnamed, as it no longer has much of anything directly
 * to do with IPC.  The functionality here is concerned with managing
 * exit-time cleanup for either a postmaster or a backend.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/ipc.h
 *
 *-------------------------------------------------------------------------
 */

/*
 * 此文件名称并不准确，因为它已几乎不再直接处理 IPC。这里的功能涉及管理 postmaster 或
 * 后端在退出时执行的清理工作。
 */
#ifndef IPC_H
#define IPC_H

typedef void (*pg_on_exit_callback) (int code, Datum arg);
typedef void (*shmem_startup_hook_type) (void);

/*----------
 * API for handling cleanup that must occur during either ereport(ERROR)
 * or ereport(FATAL) exits from a block of code.  (Typical examples are
 * undoing transient changes to shared-memory state.)
 *
 *		PG_ENSURE_ERROR_CLEANUP(cleanup_function, arg);
 *		{
 *			... code that might throw ereport(ERROR) or ereport(FATAL) ...
 *		}
 *		PG_END_ENSURE_ERROR_CLEANUP(cleanup_function, arg);
 *
 * where the cleanup code is in a function declared per pg_on_exit_callback.
 * The Datum value "arg" can carry any information the cleanup function
 * needs.
 *
 * This construct ensures that cleanup_function() will be called during
 * either ERROR or FATAL exits.  It will not be called on successful
 * exit from the controlled code.  (If you want it to happen then too,
 * call the function yourself from just after the construct.)
 *
 * Note: the macro arguments are multiply evaluated, so avoid side-effects.
 *----------
 */

/*
 * 用于处理代码块经 ereport(ERROR) 或 ereport(FATAL) 退出时必须执行的清理的 API。
 * 典型用例是撤销对共享内存状态的临时修改。
 * PG_ENSURE_ERROR_CLEANUP(cleanup_function, arg); 与
 * PG_END_ENSURE_ERROR_CLEANUP(cleanup_function, arg); 包围可能抛出 ERROR 或 FATAL 的代码。
 * 清理代码位于按 pg_on_exit_callback 声明的函数中，Datum 值 arg 可携带该函数所需信息。
 * 该构造保证 cleanup_function() 会在 ERROR 或 FATAL 退出时调用，而在受控代码成功退出时
 * 不调用。如成功退出时也需清理，请在构造之后自行调用该函数。
 * 注意：宏参数会被多次求值，因此应避免副作用。
 */
#define PG_ENSURE_ERROR_CLEANUP(cleanup_function, arg)	\
	do { \
		before_shmem_exit(cleanup_function, arg); \
		PG_TRY()

#define PG_END_ENSURE_ERROR_CLEANUP(cleanup_function, arg)	\
		cancel_before_shmem_exit(cleanup_function, arg); \
		PG_CATCH(); \
		{ \
			cancel_before_shmem_exit(cleanup_function, arg); \
			cleanup_function (0, arg); \
			PG_RE_THROW(); \
		} \
		PG_END_TRY(); \
	} while (0)


/* ipc.c */

/*
 * ipc.c 中的接口。
 */
extern PGDLLIMPORT bool proc_exit_inprogress;
extern PGDLLIMPORT bool shmem_exit_inprogress;

/*
 * Terminates the process after running process-exit callbacks.
 */

/*
 * 运行进程退出回调后终止进程。
 */
pg_noreturn extern void proc_exit(int code);
/*
 * Runs shared-memory exit cleanup without necessarily terminating the process.
 */

/*
 * 运行共享内存退出清理，但不一定终止进程。
 */
extern void shmem_exit(int code);
/*
 * Registers a callback to run when the process exits.
 */

/*
 * 注册在进程退出时运行的回调函数。
 */
extern void on_proc_exit(pg_on_exit_callback function, Datum arg);
/*
 * Registers a callback to run during shared-memory exit cleanup.
 */

/*
 * 注册在共享内存退出清理期间运行的回调函数。
 */
extern void on_shmem_exit(pg_on_exit_callback function, Datum arg);
/*
 * Registers a cleanup callback that runs before shared-memory exit callbacks.
 */

/*
 * 注册在共享内存退出回调之前运行的清理回调函数。
 */
extern void before_shmem_exit(pg_on_exit_callback function, Datum arg);
/*
 * Cancels a previously registered pre-shared-memory-exit callback.
 */

/*
 * 取消先前注册的共享内存退出前回调函数。
 */
extern void cancel_before_shmem_exit(pg_on_exit_callback function, Datum arg);
/*
 * Clears all process and shared-memory exit callback state.
 */

/*
 * 清除所有进程和共享内存退出回调状态。
 */
extern void on_exit_reset(void);
/*
 * Verifies that shared-memory exit callback lists are empty.
 */

/*
 * 验证共享内存退出回调列表为空。
 */
extern void check_on_shmem_exit_lists_are_empty(void);

/* ipci.c */

/*
 * ipci.c 中的接口。
 */
extern PGDLLIMPORT shmem_startup_hook_type shmem_startup_hook;

/*
 * Calculates shared-memory requirements and returns the semaphore count.
 */

/*
 * 计算共享内存需求，并返回信号量数量。
 */
extern Size CalculateShmemSize(int *num_semaphores);
/*
 * Creates the server's shared-memory region and its semaphores.
 */

/*
 * 创建服务器的共享内存区域及其信号量。
 */
extern void CreateSharedMemoryAndSemaphores(void);
#ifdef EXEC_BACKEND
/*
 * Attaches an EXEC_BACKEND child to inherited shared-memory structures.
 */

/*
 * 将 EXEC_BACKEND 子进程附接到继承的共享内存结构。
 */
extern void AttachSharedMemoryStructs(void);
#endif
/*
 * Initializes GUC variables that depend on shared memory.
 */

/*
 * 初始化依赖共享内存的 GUC 变量。
 */
extern void InitializeShmemGUCs(void);

#endif							/* IPC_H */

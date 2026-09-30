/*-------------------------------------------------------------------------
 *
 * io_worker.h
 *    IO worker for implementing AIO "ourselves"
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/io.h
 *
 *-------------------------------------------------------------------------
 */

/*
 * 用于自行实现 AIO 的 I/O 工作进程。
 */
#ifndef IO_WORKER_H
#define IO_WORKER_H


/*
 * Runs the I/O worker main loop using the supplied startup payload.
 * It initializes worker state and then services asynchronous I/O requests.
 */

/*
 * 使用提供的启动载荷运行 I/O 工作进程主循环；它先初始化工作进程状态，再处理异步 I/O 请求。
 */
pg_noreturn extern void IoWorkerMain(const void *startup_data, size_t startup_data_len);

extern PGDLLIMPORT int io_workers;

#endif							/* IO_WORKER_H */

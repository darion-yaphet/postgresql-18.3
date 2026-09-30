/*-------------------------------------------------------------------------
 *
 * dsm_impl.h
 *	  low-level dynamic shared memory primitives
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/dsm_impl.h
 *
 *-------------------------------------------------------------------------
 */

/*
 * 底层动态共享内存原语。
 */
#ifndef DSM_IMPL_H
#define DSM_IMPL_H

/* Dynamic shared memory implementations. */

/*
 * 动态共享内存实现。
 */
#define DSM_IMPL_POSIX			1
#define DSM_IMPL_SYSV			2
#define DSM_IMPL_WINDOWS		3
#define DSM_IMPL_MMAP			4

/*
 * Determine which dynamic shared memory implementations will be supported
 * on this platform, and which one will be the default.
 */

/*
 * 确定此平台支持哪些动态共享内存实现，以及默认使用哪一种。
 */
#ifdef WIN32
#define USE_DSM_WINDOWS
#define DEFAULT_DYNAMIC_SHARED_MEMORY_TYPE		DSM_IMPL_WINDOWS
#else
#ifdef HAVE_SHM_OPEN
#define USE_DSM_POSIX
#define DEFAULT_DYNAMIC_SHARED_MEMORY_TYPE		DSM_IMPL_POSIX
#endif
#define USE_DSM_SYSV
#ifndef DEFAULT_DYNAMIC_SHARED_MEMORY_TYPE
#define DEFAULT_DYNAMIC_SHARED_MEMORY_TYPE		DSM_IMPL_SYSV
#endif
#define USE_DSM_MMAP
#endif

/* GUC. */

/*
 * GUC 配置变量。
 */
extern PGDLLIMPORT int dynamic_shared_memory_type;
extern PGDLLIMPORT int min_dynamic_shared_memory;

/*
 * Directory for on-disk state.
 *
 * This is used by all implementations for crash recovery and by the mmap
 * implementation for storage.
 */

/*
 * 所有实现都将其用于崩溃恢复，mmap 实现还将其用于存储。
 */
#define PG_DYNSHMEM_DIR					"pg_dynshmem"
#define PG_DYNSHMEM_MMAP_FILE_PREFIX	"mmap."

/* A "name" for a dynamic shared memory segment. */

/*
 * 动态共享内存段的“名称”。
 */
typedef uint32 dsm_handle;

/* Sentinel value to use for invalid DSM handles. */

/*
 * 用于无效 DSM 句柄的哨兵值。
 */
#define DSM_HANDLE_INVALID ((dsm_handle) 0)

/* All the shared-memory operations we know about. */

/*
 * 已知的全部共享内存操作。
 */
typedef enum
{
	DSM_OP_CREATE,
	DSM_OP_ATTACH,
	DSM_OP_DETACH,
	DSM_OP_DESTROY,
} dsm_op;

/* Create, attach to, detach from, resize, or destroy a segment. */

/*
 * 创建、附接、分离、调整大小或销毁一个段；它将请求的操作分派给选定的 DSM 实现。
 */
extern bool dsm_impl_op(dsm_op op, dsm_handle handle, Size request_size,
						void **impl_private, void **mapped_address, Size *mapped_size,
						int elevel);

/* Implementation-dependent actions required to keep segment until shutdown. */

/*
 * 使段保持到关闭所需的、依赖实现的操作。
 */
/*
 * Pins a segment through the implementation so it survives until shutdown.
 */

/*
 * 通过具体实现固定一个段，使其存活到关闭时。
 */
extern void dsm_impl_pin_segment(dsm_handle handle, void *impl_private,
								 void **impl_private_pm_handle);
/*
 * Removes an implementation-level persistent pin from a DSM segment.
 */

/*
 * 从 DSM 段移除实现层面的持久固定。
 */
extern void dsm_impl_unpin_segment(dsm_handle handle, void **impl_private);

#endif							/* DSM_IMPL_H */

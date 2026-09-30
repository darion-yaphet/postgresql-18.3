/*-------------------------------------------------------------------------
 *
 * dsm.h
 *	  manage dynamic shared memory segments
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/dsm.h
 *
 *-------------------------------------------------------------------------
 */

/*
 * 管理动态共享内存段。
 */
#ifndef DSM_H
#define DSM_H

#include "storage/dsm_impl.h"

typedef struct dsm_segment dsm_segment;

#define DSM_CREATE_NULL_IF_MAXSEGMENTS			0x0001

/* Startup and shutdown functions. */

/*
 * 启动和关闭函数。
 */
struct PGShmemHeader;			/* avoid including pg_shmem.h */

/*
 * 避免包含 pg_shmem.h。
 */
/*
 * Cleans up dynamic shared memory left by a previous control segment.
 */

/*
 * 使用旧控制段清理由其遗留的动态共享内存。
 */
extern void dsm_cleanup_using_control_segment(dsm_handle old_control_handle);
/*
 * Starts dynamic shared memory during postmaster initialization.
 */

/*
 * 在 postmaster 初始化期间启动动态共享内存。
 */
extern void dsm_postmaster_startup(struct PGShmemHeader *);
/*
 * Releases backend-local dynamic shared memory state at backend shutdown.
 */

/*
 * 在后端关闭时释放后端本地的动态共享内存状态。
 */
extern void dsm_backend_shutdown(void);
/*
 * Detaches every dynamic shared memory mapping owned by this backend.
 */

/*
 * 分离当前后端拥有的全部动态共享内存映射。
 */
extern void dsm_detach_all(void);

/*
 * Estimates the shared-memory space needed for DSM control data.
 */

/*
 * 估算 DSM 控制数据所需的共享内存空间。
 */
extern size_t dsm_estimate_size(void);
/*
 * Initializes DSM control data in shared memory.
 */

/*
 * 初始化共享内存中的 DSM 控制数据。
 */
extern void dsm_shmem_init(void);

#ifdef EXEC_BACKEND
/*
 * Sets the DSM control handle inherited by an EXEC_BACKEND child.
 */

/*
 * 设置 EXEC_BACKEND 子进程继承的 DSM 控制句柄。
 */
extern void dsm_set_control_handle(dsm_handle h);
#endif

/* Functions that create or remove mappings. */

/*
 * 创建或移除映射的函数。
 */
/*
 * Creates a DSM segment and maps it into the current backend.
 */

/*
 * 创建 DSM 段并将其映射到当前后端。
 */
extern dsm_segment *dsm_create(Size size, int flags);
/*
 * Attaches the current backend to the DSM segment identified by the handle.
 */

/*
 * 将当前后端附接到该句柄标识的 DSM 段。
 */
extern dsm_segment *dsm_attach(dsm_handle h);
/*
 * Detaches one DSM mapping from the current backend.
 */

/*
 * 从当前后端分离一个 DSM 映射。
 */
extern void dsm_detach(dsm_segment *seg);

/* Resource management functions. */

/*
 * 资源管理函数。
 */
/*
 * Keeps a mapping attached until it is explicitly unpinned.
 */

/*
 * 使映射保持附接，直到显式取消固定。
 */
extern void dsm_pin_mapping(dsm_segment *seg);
/*
 * Allows a previously pinned mapping to be detached.
 */

/*
 * 允许分离先前固定的映射。
 */
extern void dsm_unpin_mapping(dsm_segment *seg);
/*
 * Keeps the underlying DSM segment alive through shutdown.
 */

/*
 * 使底层 DSM 段在关闭前保持存活。
 */
extern void dsm_pin_segment(dsm_segment *seg);
/*
 * Removes the persistent pin from the DSM segment identified by the handle.
 */

/*
 * 移除该句柄标识的 DSM 段上的持久固定。
 */
extern void dsm_unpin_segment(dsm_handle handle);
/*
 * Finds this backend's mapping for a DSM handle, if one exists.
 */

/*
 * 查找当前后端中与 DSM 句柄对应的映射（如存在）。
 */
extern dsm_segment *dsm_find_mapping(dsm_handle handle);

/* Informational functions. */

/*
 * 信息查询函数。
 */
/*
 * Returns the mapped base address of a DSM segment.
 */

/*
 * 返回 DSM 段已映射的基地址。
 */
extern void *dsm_segment_address(dsm_segment *seg);
/*
 * Returns the length of a DSM segment's mapping.
 */

/*
 * 返回 DSM 段映射的长度。
 */
extern Size dsm_segment_map_length(dsm_segment *seg);
/*
 * Returns the handle that identifies a DSM segment.
 */

/*
 * 返回标识 DSM 段的句柄。
 */
extern dsm_handle dsm_segment_handle(dsm_segment *seg);

/* Cleanup hooks. */

/*
 * 清理钩子。
 */
typedef void (*on_dsm_detach_callback) (dsm_segment *, Datum arg);
/*
 * Registers a callback that runs when the segment is detached.
 */

/*
 * 注册在分离该段时运行的回调函数。
 */
extern void on_dsm_detach(dsm_segment *seg,
						  on_dsm_detach_callback function, Datum arg);
/*
 * Cancels a callback previously registered for segment detachment.
 */

/*
 * 取消先前为段分离注册的回调函数。
 */
extern void cancel_on_dsm_detach(dsm_segment *seg,
								 on_dsm_detach_callback function, Datum arg);
/*
 * Removes all DSM detachment callbacks for the current backend.
 */

/*
 * 移除当前后端的全部 DSM 分离回调。
 */
extern void reset_on_dsm_detach(void);

#endif							/* DSM_H */

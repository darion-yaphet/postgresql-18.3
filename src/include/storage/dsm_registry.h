/*-------------------------------------------------------------------------
 *
 * dsm_registry.h
 *	  Functions for interfacing with the dynamic shared memory registry.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/dsm_registry.h
 *
 *-------------------------------------------------------------------------
 */

/*
 * 用于与动态共享内存注册表交互的函数。
 */
#ifndef DSM_REGISTRY_H
#define DSM_REGISTRY_H

/*
 * Finds or creates a named DSM segment and optionally initializes new memory.
 * The function returns the segment address and records whether it already existed.
 */

/*
 * 查找或创建具名 DSM 段，并可初始化新内存。该函数返回段地址，并记录该段是否已存在。
 */
extern void *GetNamedDSMSegment(const char *name, size_t size,
								void (*init_callback) (void *ptr),
								bool *found);

/*
 * Returns the shared-memory size required by the DSM registry.
 */

/*
 * 返回 DSM 注册表所需的共享内存大小。
 */
extern Size DSMRegistryShmemSize(void);
/*
 * Initializes DSM registry data in shared memory.
 */

/*
 * 初始化共享内存中的 DSM 注册表数据。
 */
extern void DSMRegistryShmemInit(void);

#endif							/* DSM_REGISTRY_H */

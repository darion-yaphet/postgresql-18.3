/*-------------------------------------------------------------------------
 *
 * shm_toc.h
 *	  shared memory segment table of contents
 *
 * This is intended to provide a simple way to divide a chunk of shared
 * memory (probably dynamic shared memory allocated via dsm_create) into
 * a number of regions and keep track of the addresses of those regions or
 * key data structures within those regions.  This is not intended to
 * scale to a large number of keys and will perform poorly if used that
 * way; if you need a large number of pointers, store them within some
 * other data structure within the segment and only put the pointer to
 * the data structure itself in the table of contents.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/shm_toc.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SHM_TOC_H
#define SHM_TOC_H

#include "storage/shmem.h"		/* for add_size() */

/* 用于 add_size()。 */

/* shm_toc is an opaque type known only within shm_toc.c */

/* shm_toc 是仅在 shm_toc.c 中可知的不透明类型。 */
typedef struct shm_toc shm_toc;

/*
 * Creates a table of contents in a supplied shared-memory region.
 */

/*
 * 在提供的共享内存区域中创建目录表。
 */
extern shm_toc *shm_toc_create(uint64 magic, void *address, Size nbytes);
/*
 * Attaches to a validated shared-memory table of contents.
 */

/*
 * 附加到经验证的共享内存目录表。
 */
extern shm_toc *shm_toc_attach(uint64 magic, void *address);
/*
 * Allocates an aligned chunk from a table-of-contents region.
 */

/*
 * 从目录表区域分配对齐的块。
 */
extern void *shm_toc_allocate(shm_toc *toc, Size nbytes);
/*
 * Returns the free space remaining in a table-of-contents region.
 */

/*
 * 返回目录表区域剩余的空闲空间。
 */
extern Size shm_toc_freespace(shm_toc *toc);
/*
 * Inserts an address under a key in a table of contents.
 */

/*
 * 将地址以键的形式插入目录表。
 */
extern void shm_toc_insert(shm_toc *toc, uint64 key, void *address);
/*
 * Looks up a keyed address in a table of contents.
 */

/*
 * 在目录表中查找带键的地址。
 */
extern void *shm_toc_lookup(shm_toc *toc, uint64 key, bool noError);

/*
 * Tools for estimating how large a chunk of shared memory will be needed
 * to store a TOC and its dependent objects.  Note: we don't really support
 * large numbers of keys, but it's convenient to declare number_of_keys
 * as a Size anyway.
 */

/*
 * 用于估算存储目录表及其依赖对象所需共享内存块大小的工具。注意：实际上并不支持大量
 * 键，但仍将 number_of_keys 声明为 Size 很方便。
 */
typedef struct
{
	Size		space_for_chunks;
	Size		number_of_keys;
} shm_toc_estimator;

#define shm_toc_initialize_estimator(e) \
	((e)->space_for_chunks = 0, (e)->number_of_keys = 0)
#define shm_toc_estimate_chunk(e, sz) \
	((e)->space_for_chunks = add_size((e)->space_for_chunks, BUFFERALIGN(sz)))
#define shm_toc_estimate_keys(e, cnt) \
	((e)->number_of_keys = add_size((e)->number_of_keys, cnt))

/*
 * Computes the total shared-memory space estimated for a TOC.
 */

/*
 * 计算为目录表估算的共享内存总空间。
 */
extern Size shm_toc_estimate(shm_toc_estimator *e);

#endif							/* SHM_TOC_H */

/*-------------------------------------------------------------------------
 *
 * shmem.h
 *	  shared memory management structures
 *
 *
 *	  共享内存管理结构。
 *
 * Historical note:
 * A long time ago, Postgres' shared memory region was allowed to be mapped
 * at a different address in each process, and shared memory "pointers" were
 * passed around as offsets relative to the start of the shared memory region.
 * That is no longer the case: each process must map the shared memory region
 * at the same address.  This means shared memory pointers can be passed
 * around directly between different processes.
 *
 * 历史说明：
 * 很久以前，Postgres 允许在每个进程中以不同地址映射共享内存区域，
 * 并将共享内存“指针”作为相对于共享内存区域起始位置的偏移量来传递。
 * 现在已不再如此：每个进程都必须在相同地址映射共享内存区域。
 * 这意味着可以在不同进程之间直接传递共享内存指针。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/shmem.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SHMEM_H
#define SHMEM_H

#include "storage/spin.h"
#include "utils/hsearch.h"


/* shmem.c */

/* shmem.c 中的函数。 */
extern PGDLLIMPORT slock_t *ShmemLock;
struct PGShmemHeader;			/* avoid including storage/pg_shmem.h here */

							/* 避免在此包含 storage/pg_shmem.h。 */

/* Initialize access to the already-mapped shared-memory segment.
 * The supplied segment header establishes the common mapping used by this
 * backend before later allocation and index operations access shared state.
 *
 * 初始化对已映射共享内存段的访问。
 * 提供的段头会建立本后端使用的公共映射，随后分配和索引操作才能访问共享状态。
 */
extern void InitShmemAccess(struct PGShmemHeader *seghdr);

/* Initialize the shared-memory allocator.
 * This prepares allocator metadata so subsequent allocation requests can
 * reserve space from the shared segment.
 *
 * 初始化共享内存分配器。
 * 此函数准备分配器元数据，使后续分配请求能够从共享内存段保留空间。
 */
extern void InitShmemAllocation(void);

/* Allocate shared memory or raise an error when no space remains.
 * The allocator advances shared allocation state and returns the reserved
 * address.
 *
 * 分配共享内存；空间不足时报告错误。
 * 分配器推进共享分配状态并返回保留空间的地址。
 */
extern void *ShmemAlloc(Size size);

/* Attempt to allocate shared memory without reporting an allocation error.
 * It follows normal allocation handling but returns NULL when space is not
 * available.
 *
 * 尝试分配共享内存而不报告分配错误。
 * 此函数遵循常规分配处理，但空间不可用时返回 NULL。
 */
extern void *ShmemAllocNoError(Size size);

/* Allocate shared memory while the caller manages synchronization.
 * This variant updates allocation state without acquiring the normal lock,
 * so callers must already provide the required exclusion.
 *
 * 在调用方负责同步时分配共享内存。
 * 此变体不会获取常规锁便更新分配状态，因此调用方必须已提供所需的互斥保护。
 */
extern void *ShmemAllocUnlocked(Size size);

/* Test whether an address belongs to the shared-memory segment.
 * The check compares the address with the mapped segment bounds before code
 * treats it as shared state.
 *
 * 测试地址是否属于共享内存段。
 * 此检查先将地址与映射段边界比较，再允许代码将其视为共享状态。
 */
extern bool ShmemAddrIsValid(const void *addr);

/* Initialize the shared-memory object index.
 * The index maps stable object names to their allocated locations so
 * independently initialized backends can find the same objects.
 *
 * 初始化共享内存对象索引。
 * 该索引将稳定的对象名称映射到已分配位置，使独立初始化的后端能找到相同对象。
 */
extern void InitShmemIndex(void);

/* Find or initialize a named hash table in shared memory.
 * The name is resolved through the shared index, then the existing table is
 * returned or a new table is created with the supplied hash parameters.
 *
 * 在共享内存中查找或初始化具名哈希表。
 * 函数通过共享索引解析名称，然后返回现有表，或用给定哈希参数创建新表。
 */
extern HTAB *ShmemInitHash(const char *name, long init_size, long max_size,
						   HASHCTL *infoP, int hash_flags);

/* Find or allocate a named shared-memory structure.
 * The shared index is consulted first; foundPtr tells the caller whether the
 * returned address was already initialized or has just been allocated.
 *
 * 查找或分配具名共享内存结构。
 * 函数先查询共享索引；foundPtr 告知调用方返回地址是已初始化对象还是刚分配的对象。
 */
extern void *ShmemInitStruct(const char *name, Size size, bool *foundPtr);

/* Add two allocation sizes with overflow checking.
 * The result is safe to use when computing a combined shared-memory request.
 *
 * 以溢出检查方式相加两个分配大小。
 * 返回值可安全地用于计算合并后的共享内存请求。
 */
extern Size add_size(Size s1, Size s2);

/* Multiply two allocation sizes with overflow checking.
 * The result is safe to use when computing an array's shared-memory request.
 *
 * 以溢出检查方式相乘两个分配大小。
 * 返回值可安全地用于计算数组的共享内存请求。
 */
extern Size mul_size(Size s1, Size s2);

/* Return the operating system page size used for shared memory.
 * Callers use the value to align shared-memory layout and allocation sizes.
 *
 * 返回共享内存使用的操作系统页大小。
 * 调用方使用该值对齐共享内存布局和分配大小。
 */
extern PGDLLIMPORT Size pg_get_shmem_pagesize(void);

/* ipci.c */

/* ipci.c 中的函数。 */

/* Reserve additional shared-memory space for an add-in module.
 * The request contributes to the total segment size before shared memory is
 * created.
 *
 * 为附加模块预留额外共享内存空间。
 * 该请求会在创建共享内存前计入总段大小。
 */
extern void RequestAddinShmemSpace(Size size);

/* size constants for the shmem index table */

/* 共享内存索引表的大小常量。 */
 /* max size of data structure string name */

 /* 数据结构字符串名称的最大大小。 */
#define SHMEM_INDEX_KEYSIZE		 (48)
 /* estimated size of the shmem index table (not a hard limit) */

 /* 共享内存索引表的估计大小（不是硬性限制）。 */
#define SHMEM_INDEX_SIZE		 (64)

/* this is a hash bucket in the shmem index table */

/* 这是共享内存索引表中的一个哈希桶。 */
typedef struct
{
	char		key[SHMEM_INDEX_KEYSIZE];	/* string name */

							/* 字符串名称。 */
	void	   *location;		/* location in shared mem */

							/* 共享内存中的位置。 */
	Size		size;			/* # bytes requested for the structure */

							/* 为该结构请求的字节数。 */
	Size		allocated_size; /* # bytes actually allocated */

							/* 实际分配的字节数。 */
} ShmemIndexEnt;

#endif							/* SHMEM_H */

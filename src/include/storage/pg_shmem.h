/*-------------------------------------------------------------------------
 *
 * pg_shmem.h
 *	  Platform-independent API for shared memory support.
 *
 * Every port is expected to support shared memory with approximately
 * SysV-ish semantics; in particular, a memory block is not anonymous
 * but has an ID, and we must be able to tell whether there are any
 * remaining processes attached to a block of a specified ID.
 *
 * To simplify life for the SysV implementation, the ID is assumed to
 * consist of two unsigned long values (these are key and ID in SysV
 * terms).  Other platforms may ignore the second value if they need
 * only one ID number.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/pg_shmem.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_SHMEM_H
#define PG_SHMEM_H

#include "storage/dsm_impl.h"

typedef struct PGShmemHeader	/* standard header for all Postgres shmem */

/* 所有 Postgres 共享内存段的标准头部。 */
{
	int32		magic;			/* magic # to identify Postgres segments */

	/* 用于识别 Postgres 共享内存段的魔数。 */
#define PGShmemMagic  679834894
	pid_t		creatorPID;		/* PID of creating process (set but unread) */

	/* 创建进程的 PID（已设置但未读取）。 */
	Size		totalsize;		/* total size of segment */

	/* 段的总大小。 */
	Size		freeoffset;		/* offset to first free space */

	/* 第一处空闲空间的偏移量。 */
	dsm_handle	dsm_control;	/* ID of dynamic shared memory control seg */

	/* 动态共享内存控制段的 ID。 */
	void	   *index;			/* pointer to ShmemIndex table */

	/* 指向 ShmemIndex 表的指针。 */
#ifndef WIN32					/* Windows doesn't have useful inode#s */

/* Windows 没有可用的 inode 编号。 */
	dev_t		device;			/* device data directory is on */

	/* 数据目录所在的设备。 */
	ino_t		inode;			/* inode number of data directory */

	/* 数据目录的 inode 编号。 */
#endif
} PGShmemHeader;

/* GUC variables */

/* GUC 变量。 */
extern PGDLLIMPORT int shared_memory_type;
extern PGDLLIMPORT int huge_pages;
extern PGDLLIMPORT int huge_page_size;
extern PGDLLIMPORT int huge_pages_status;

/* Possible values for huge_pages and huge_pages_status */

/* huge_pages 和 huge_pages_status 的可能取值。 */
typedef enum
{
	HUGE_PAGES_OFF,
	HUGE_PAGES_ON,
	HUGE_PAGES_TRY,				/* only for huge_pages */

	/* 仅用于 huge_pages。 */
	HUGE_PAGES_UNKNOWN,			/* only for huge_pages_status */

	/* 仅用于 huge_pages_status。 */
}			HugePagesType;

/* Possible values for shared_memory_type */

/* shared_memory_type 的可能取值。 */
typedef enum
{
	SHMEM_TYPE_WINDOWS,
	SHMEM_TYPE_SYSV,
	SHMEM_TYPE_MMAP,
}			PGShmemType;

#ifndef WIN32
extern PGDLLIMPORT unsigned long UsedShmemSegID;
#else
extern PGDLLIMPORT HANDLE UsedShmemSegID;
extern PGDLLIMPORT void *ShmemProtectiveRegion;
#endif
extern PGDLLIMPORT void *UsedShmemSegAddr;

#if !defined(WIN32) && !defined(EXEC_BACKEND)
#define DEFAULT_SHARED_MEMORY_TYPE SHMEM_TYPE_MMAP
#elif !defined(WIN32)
#define DEFAULT_SHARED_MEMORY_TYPE SHMEM_TYPE_SYSV
#else
#define DEFAULT_SHARED_MEMORY_TYPE SHMEM_TYPE_WINDOWS
#endif

#ifdef EXEC_BACKEND
/*
 * Reattaches the process to the already-created shared-memory segment.
 */

/*
 * 将进程重新附加到已创建的共享内存段。
 */
extern void PGSharedMemoryReAttach(void);
/*
 * Disables subsequent shared-memory reattachment for this process.
 */

/*
 * 禁止此进程后续重新附加共享内存。
 */
extern void PGSharedMemoryNoReAttach(void);
#endif

/*
 * Creates the main shared-memory segment and returns its header.
 */

/*
 * 创建主共享内存段并返回其头部。
 */
extern PGShmemHeader *PGSharedMemoryCreate(Size size,
										   PGShmemHeader **shim);
/*
 * Tests whether the shared-memory segment identified by two IDs is in use.
 */

/*
 * 检查由两个 ID 标识的共享内存段是否正在使用。
 */
extern bool PGSharedMemoryIsInUse(unsigned long id1, unsigned long id2);
/*
 * Detaches the current process from the main shared-memory segment.
 */

/*
 * 将当前进程从主共享内存段分离。
 */
extern void PGSharedMemoryDetach(void);
/*
 * Determines the huge-page size and mmap flags available to the process.
 */

/*
 * 确定进程可使用的大页大小和 mmap 标志。
 */
extern void GetHugePageSize(Size *hugepagesize, int *mmap_flags);

#endif							/* PG_SHMEM_H */

/*-------------------------------------------------------------------------
 *
 * sharedfileset.h
 *	  Shared temporary file management.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/sharedfileset.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef SHAREDFILESET_H
#define SHAREDFILESET_H

#include "storage/dsm.h"
#include "storage/fd.h"
#include "storage/fileset.h"
#include "storage/spin.h"

/*
 * A set of temporary files that can be shared by multiple backends.
 */

/*
 * 可由多个后端共享的一组临时文件。
 */
typedef struct SharedFileSet
{
	FileSet		fs;
	slock_t		mutex;			/* mutex protecting the reference count */

	/* 保护引用计数的互斥锁。 */
	int			refcnt;			/* number of attached backends */

	/* 已附加的后端数量。 */
} SharedFileSet;

/*
 * Initializes a shared file set in a dynamic shared-memory segment.
 */

/*
 * 在动态共享内存段中初始化共享文件集。
 */
extern void SharedFileSetInit(SharedFileSet *fileset, dsm_segment *seg);
/*
 * Attaches a backend to an existing shared file set.
 */

/*
 * 将后端附加到现有共享文件集。
 */
extern void SharedFileSetAttach(SharedFileSet *fileset, dsm_segment *seg);
/*
 * Deletes all files in a shared file set after coordinating references.
 */

/*
 * 协调引用后删除共享文件集中的所有文件。
 */
extern void SharedFileSetDeleteAll(SharedFileSet *fileset);

#endif

/*-------------------------------------------------------------------------
 *
 * fileset.h
 *	  Management of named temporary files.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/fileset.h
 *
 *-------------------------------------------------------------------------
 */

/*
 * 具名临时文件的管理。
 */

#ifndef FILESET_H
#define FILESET_H

#include "storage/fd.h"

/*
 * A set of temporary files.
 */

/*
 * 一组临时文件。
 */
typedef struct FileSet
{
	pid_t		creator_pid;	/* PID of the creating process */

/*
 * 创建进程的 PID。
 */
	uint32		number;			/* per-PID identifier */

/*
 * 每个 PID 内的标识符。
 */
	int			ntablespaces;	/* number of tablespaces to use */

/*
 * 要使用的表空间数量。
 */
	Oid			tablespaces[8]; /* OIDs of tablespaces to use. Assumes that
								 * it's rare that there more than temp
								 * tablespaces. */

/*
 * 要使用的表空间 OID。假定临时表空间数量很少会超过此数量。
 */
} FileSet;

/*
 * Initializes a file set with a new per-process identifier.
 */

/*
 * 使用新的进程内标识符初始化文件集。
 */
extern void FileSetInit(FileSet *fileset);
/*
 * Creates a named temporary file in a file set and returns its virtual descriptor.
 */

/*
 * 在文件集中创建具名临时文件，并返回其虚拟文件描述符。
 */
extern File FileSetCreate(FileSet *fileset, const char *name);
/*
 * Opens an existing named temporary file in a file set.
 */

/*
 * 打开文件集中已有的具名临时文件。
 */
extern File FileSetOpen(FileSet *fileset, const char *name,
						int mode);
/*
 * Deletes a named temporary file, optionally reporting deletion errors.
 */

/*
 * 删除具名临时文件，并可选择报告删除错误。
 */
extern bool FileSetDelete(FileSet *fileset, const char *name,
						  bool error_on_failure);
/*
 * Deletes every temporary file belonging to a file set.
 */

/*
 * 删除属于该文件集的所有临时文件。
 */
extern void FileSetDeleteAll(FileSet *fileset);

#endif

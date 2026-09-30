/*-------------------------------------------------------------------------
 *
 * copydir.h
 *	  Copy a directory.
 *
 *	  复制目录。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/copydir.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef COPYDIR_H
#define COPYDIR_H

typedef enum FileCopyMethod
{
	FILE_COPY_METHOD_COPY,
	FILE_COPY_METHOD_CLONE,
}			FileCopyMethod;

/* GUC parameters */

/* GUC 参数。 */
extern PGDLLIMPORT int file_copy_method;

/*
 * Copy a directory to another location.
 * The function copies its entries and optionally descends into subdirectories.
 *
 * 将一个目录复制到另一个位置。
 * 该函数复制其中的条目，并可选择进入子目录。
 */
extern void copydir(const char *fromdir, const char *todir, bool recurse);

/*
 * Copy one file to another location.
 * The function transfers the source file's contents to the destination file.
 *
 * 将一个文件复制到另一个位置。
 * 该函数把源文件的内容传输到目标文件。
 */
extern void copy_file(const char *fromfile, const char *tofile);

#endif							/* COPYDIR_H */

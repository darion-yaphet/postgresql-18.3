/*-------------------------------------------------------------------------
 *
 * buffile.h
 *	  Management of large buffered temporary files.
 *	  大规模缓冲临时文件的管理。
 *
 * The BufFile routines provide a partial replacement for stdio atop
 * virtual file descriptors managed by fd.c.  Currently they only support
 * buffered access to a virtual file, without any of stdio's formatting
 * features.  That's enough for immediate needs, but the set of facilities
 * could be expanded if necessary.
 * BufFile 例程在 fd.c 管理的虚拟文件描述符之上提供了一个 stdio 的部分替代方案。
 * 目前它们只支持对虚拟文件的缓冲访问，没有任何 stdio 的格式化功能。
 * 这对于即时需求来说已经足够了，但如果有必要，这些功能可以进一步扩展。
 *
 * BufFile also supports working with temporary files that exceed the OS
 * file size limit and/or the largest offset representable in an int.
 * It might be better to split that out as a separately accessible module,
 * but currently we have no need for oversize temp files without buffered
 * access.
 * BufFile 还支持处理超过操作系统文件大小限制和/或 int 类型可表示的最大偏移量的临时文件。
 * 将其拆分为一个单独可访问的模块可能会更好，但目前我们不需要没有缓冲访问的超大临时文件。
 *
 * Core Flow:
 * 核心流程：
 * 1. Creation: BufFile can be created as a simple temp file (BufFileCreateTemp) or as part of a FileSet
 *    (BufFileCreateFileSet) for sharing between parallel processes.
 *    创建：BufFile 可以作为简单的临时文件创建（BufFileCreateTemp），或者作为 FileSet 的一部分创建（BufFileCreateFileSet），以便在并行进程之间共享。
 * 2. Buffering: It manages an internal buffer (typically 8KB) to consolidate small I/O requests.
 *    缓冲：它管理一个内部缓冲区（通常为 8KB），以整合小的 I/O 请求。
 * 3. Segmentation: To bypass OS file size limits, BufFile splits a logical file into multiple
 *    physical segments (e.g., 1GB each). The user sees a single continuous stream.
 *    分段：为了绕过操作系统文件大小限制，BufFile 将逻辑文件拆分为多个物理分段（例如，每个 1GB）。用户看到的是一个单一的连续流。
 * 4. Lifecycle: Files are typically transient and automatically unlinked upon closure or transaction end.
 *    生命周期：文件通常是暂存的，在关闭或事务结束时自动取消链接（删除）。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/buffile.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef BUFFILE_H
#define BUFFILE_H

#include "storage/fileset.h"

/* BufFile is an opaque type whose details are not known outside buffile.c. */
/* BufFile 是一个不透明类型，其细节在 buffile.c 之外是不可见的。 */

typedef struct BufFile BufFile;

/*
 * prototypes for functions in buffile.c
 */
/*
 * buffile.c 中函数的原型
 */

/* Create a temporary BufFile / 创建一个临时 BufFile */
extern BufFile *BufFileCreateTemp(bool interXact);
/* Close a BufFile and release resources / 关闭 BufFile 并释放资源 */
extern void BufFileClose(BufFile *file);
/* Read data from BufFile / 从 BufFile 读取数据 */
pg_nodiscard extern size_t BufFileRead(BufFile *file, void *ptr, size_t size);
/* Read exact amount of data, error if EOF / 读取精确数量的数据，如果是 EOF 则报错 */
extern void BufFileReadExact(BufFile *file, void *ptr, size_t size);
/* Read data, optionally allow EOF / 读取数据，可选是否允许 EOF */
extern size_t BufFileReadMaybeEOF(BufFile *file, void *ptr, size_t size, bool eofOK);
/* Write data to BufFile / 向 BufFile 写入数据 */
extern void BufFileWrite(BufFile *file, const void *ptr, size_t size);
/* Seek to a position in BufFile / 定位到 BufFile 中的某个位置 */
extern int	BufFileSeek(BufFile *file, int fileno, off_t offset, int whence);
/* Get current position in BufFile / 获取 BufFile 中的当前位置 */
extern void BufFileTell(BufFile *file, int *fileno, off_t *offset);
/* Seek to a specific block / 定位到特定块 */
extern int	BufFileSeekBlock(BufFile *file, int64 blknum);
/* Get total size of BufFile / 获取 BufFile 的总大小 */
extern int64 BufFileSize(BufFile *file);
/* Append content of one BufFile to another / 将一个 BufFile 的内容追加到另一个 */
extern int64 BufFileAppend(BufFile *target, BufFile *source);

/* Create BufFile within a FileSet / 在 FileSet 中创建 BufFile */
extern BufFile *BufFileCreateFileSet(FileSet *fileset, const char *name);
/* Mark BufFile for export in FileSet / 标记 BufFile 以在 FileSet 中导出 */
extern void BufFileExportFileSet(BufFile *file);
/* Open an existing BufFile in a FileSet / 打开 FileSet 中现有的 BufFile */
extern BufFile *BufFileOpenFileSet(FileSet *fileset, const char *name,
								   int mode, bool missing_ok);
/* Delete a BufFile from a FileSet / 从 FileSet 中删除 BufFile */
extern void BufFileDeleteFileSet(FileSet *fileset, const char *name,
								 bool missing_ok);
/* Truncate a BufFile in a FileSet / 截断 FileSet 中的 BufFile */
extern void BufFileTruncateFileSet(BufFile *file, int fileno, off_t offset);

#endif							/* BUFFILE_H */

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
/*
 * The function creates a buffered temporary file and registers its transaction lifetime.
 *
 * 该函数创建一个缓冲临时文件，并登记其事务生命周期。
 */
extern BufFile *BufFileCreateTemp(bool interXact);
/* Close a BufFile and release resources / 关闭 BufFile 并释放资源 */
/*
 * The function flushes pending output, closes file segments, and releases metadata.
 *
 * 该函数刷出待处理输出、关闭文件分段并释放元数据。
 */
extern void BufFileClose(BufFile *file);
/* Read data from BufFile / 从 BufFile 读取数据 */
/*
 * The function consumes buffered bytes first and refills from segments as needed.
 *
 * 该函数先消耗缓冲字节，并在需要时从分段重新填充。
 */
pg_nodiscard extern size_t BufFileRead(BufFile *file, void *ptr, size_t size);
/* Read exact amount of data, error if EOF / 读取精确数量的数据，如果是 EOF 则报错 */
/*
 * The function repeatedly reads until the requested size is satisfied or reports EOF.
 *
 * 该函数重复读取，直到满足请求大小或报告 EOF。
 */
extern void BufFileReadExact(BufFile *file, void *ptr, size_t size);
/* Read data, optionally allow EOF / 读取数据，可选是否允许 EOF */
/*
 * The function reads buffered data and applies the caller's EOF policy.
 *
 * 该函数读取缓冲数据并应用调用者的 EOF 策略。
 */
extern size_t BufFileReadMaybeEOF(BufFile *file, void *ptr, size_t size, bool eofOK);
/* Write data to BufFile / 向 BufFile 写入数据 */
/*
 * The function appends bytes to the output buffer and flushes segments when necessary.
 *
 * 该函数将字节追加到输出缓冲区，并在必要时刷写分段。
 */
extern void BufFileWrite(BufFile *file, const void *ptr, size_t size);
/* Seek to a position in BufFile / 定位到 BufFile 中的某个位置 */
/*
 * The function flushes or resets buffered state, then positions the requested segment offset.
 *
 * 该函数刷写或重置缓冲状态，然后定位到请求的分段偏移量。
 */
extern int	BufFileSeek(BufFile *file, int fileno, off_t offset, int whence);
/* Get current position in BufFile / 获取 BufFile 中的当前位置 */
/*
 * The function derives the logical file number and offset from buffered state.
 *
 * 该函数从缓冲状态派生逻辑文件编号和偏移量。
 */
extern void BufFileTell(BufFile *file, int *fileno, off_t *offset);
/* Seek to a specific block / 定位到特定块 */
/*
 * The function converts a logical block number to a segment position and seeks there.
 *
 * 该函数将逻辑块号转换为分段位置并定位到那里。
 */
extern int	BufFileSeekBlock(BufFile *file, int64 blknum);
/* Get total size of BufFile / 获取 BufFile 的总大小 */
/*
 * The function combines segment sizes and buffered output to report logical length.
 *
 * 该函数合并分段大小和缓冲输出，以报告逻辑长度。
 */
extern int64 BufFileSize(BufFile *file);
/* Append content of one BufFile to another / 将一个 BufFile 的内容追加到另一个 */
/*
 * The function transfers source contents to the target while advancing both file states.
 *
 * 该函数将源内容传输到目标，同时推进两个文件状态。
 */
extern int64 BufFileAppend(BufFile *target, BufFile *source);

/* Create BufFile within a FileSet / 在 FileSet 中创建 BufFile */
/*
 * The function creates named temporary segments in a shared FileSet namespace.
 *
 * 该函数在共享 FileSet 命名空间中创建命名临时分段。
 */
extern BufFile *BufFileCreateFileSet(FileSet *fileset, const char *name);
/* Mark BufFile for export in FileSet / 标记 BufFile 以在 FileSet 中导出 */
/*
 * The function publishes file metadata so other processes can open its segments.
 *
 * 该函数发布文件元数据，使其他进程可以打开其分段。
 */
extern void BufFileExportFileSet(BufFile *file);
/* Open an existing BufFile in a FileSet / 打开 FileSet 中现有的 BufFile */
/*
 * The function locates named FileSet segments and initializes a buffered reader or writer.
 *
 * 该函数定位命名 FileSet 分段，并初始化缓冲读取器或写入器。
 */
extern BufFile *BufFileOpenFileSet(FileSet *fileset, const char *name,
								   int mode, bool missing_ok);
/* Delete a BufFile from a FileSet / 从 FileSet 中删除 BufFile */
/*
 * The function removes the named file's exported segments, optionally accepting absence.
 *
 * 该函数移除命名文件已导出的分段，并可选择接受其不存在。
 */
extern void BufFileDeleteFileSet(FileSet *fileset, const char *name,
								 bool missing_ok);
/* Truncate a BufFile in a FileSet / 截断 FileSet 中的 BufFile */
/*
 * The function drops data beyond the requested segment and offset and resets buffered state.
 *
 * 该函数丢弃请求分段和偏移量之后的数据，并重置缓冲状态。
 */
extern void BufFileTruncateFileSet(BufFile *file, int fileno, off_t offset);

#endif							/* BUFFILE_H */

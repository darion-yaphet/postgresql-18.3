/*-------------------------------------------------------------------------
 *
 * fd.h
 *	  Virtual file descriptor definitions.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/fd.h
 *
 *-------------------------------------------------------------------------
 */

/*
 * calls:
 *
 *	File {Close, Read, ReadV, Write, WriteV, Size, Sync}
 *	{Path Name Open, Allocate, Free} File
 *
 * These are NOT JUST RENAMINGS OF THE UNIX ROUTINES.
 * Use them for all file activity...
 *
 *	File fd;
 *	fd = PathNameOpenFile("foo", O_RDONLY);
 *
 *	AllocateFile();
 *	FreeFile();
 *
 * Use AllocateFile, not fopen, if you need a stdio file (FILE*); then
 * use FreeFile, not fclose, to close it.  AVOID using stdio for files
 * that you intend to hold open for any length of time, since there is
 * no way for them to share kernel file descriptors with other files.
 *
 * Likewise, use AllocateDir/FreeDir, not opendir/closedir, to allocate
 * open directories (DIR*), and OpenTransientFile/CloseTransientFile for an
 * unbuffered file descriptor.
 *
 * If you really can't use any of the above, at least call AcquireExternalFD
 * or ReserveExternalFD to report any file descriptors that are held for any
 * length of time.  Failure to do so risks unnecessary EMFILE errors.
 */

/*
 * 调用：File {Close, Read, ReadV, Write, WriteV, Size, Sync} 以及
 * 这些并非 UNIX 例程的简单重命名。所有文件操作都应使用它们。例如，使用
 * PathNameOpenFile("foo", O_RDONLY) 打开虚拟文件描述符；使用 AllocateFile() 和
 * FreeFile() 管理标准 I/O 文件。
 * 如需 stdio 文件（FILE*），应使用 AllocateFile 而非 fopen，并使用 FreeFile 而非 fclose。
 * 应避免对需要长期保持打开的文件使用 stdio，因为它们无法与其他文件共享内核文件描述符。
 * 同样，应使用 AllocateDir/FreeDir 而非 opendir/closedir 管理打开的目录（DIR*），并使用
 * OpenTransientFile/CloseTransientFile 管理无缓冲文件描述符。
 * 若确实无法使用以上接口，至少应调用 AcquireExternalFD 或 ReserveExternalFD 报告长期持有的
 * 文件描述符；否则可能产生不必要的 EMFILE 错误。
 */
#ifndef FD_H
#define FD_H

#include "port/pg_iovec.h"

#include <dirent.h>
#include <fcntl.h>

typedef int File;


#define IO_DIRECT_DATA			0x01
#define IO_DIRECT_WAL			0x02
#define IO_DIRECT_WAL_INIT		0x04

enum FileExtendMethod
{
#ifdef HAVE_POSIX_FALLOCATE
	FILE_EXTEND_METHOD_POSIX_FALLOCATE,
#endif
	FILE_EXTEND_METHOD_WRITE_ZEROS,
};

/* Default to the first available file_extend_method. */

/*
 * 默认使用第一个可用的 file_extend_method。
 */
#define DEFAULT_FILE_EXTEND_METHOD 0

/* GUC parameter */
extern PGDLLIMPORT int max_files_per_process;
extern PGDLLIMPORT bool data_sync_retry;
extern PGDLLIMPORT int recovery_init_sync_method;
extern PGDLLIMPORT int io_direct_flags;
extern PGDLLIMPORT int file_extend_method;

/*
 * This is private to fd.c, but exported for save/restore_backend_variables()
 */

/*
 * 此变量仅供 fd.c 使用，但为 save/restore_backend_variables() 而导出。
 */
extern PGDLLIMPORT int max_safe_fds;

/*
 * On Windows, we have to interpret EACCES as possibly meaning the same as
 * ENOENT, because if a file is unlinked-but-not-yet-gone on that platform,
 * that's what you get.  Ugh.  This code is designed so that we don't
 * actually believe these cases are okay without further evidence (namely,
 * a pending fsync request getting canceled ... see ProcessSyncRequests).
 */

/*
 * 在 Windows 上，EACCES 可能与 ENOENT 含义相同：如果文件已 unlink 但尚未完全消失，平台会返回它。
 * 不过代码不会在没有额外证据前认为这些情况正常，所需证据是待处理 fsync 请求被取消（见
 */
#ifndef WIN32
#define FILE_POSSIBLY_DELETED(err)	((err) == ENOENT)
#else
#define FILE_POSSIBLY_DELETED(err)	((err) == ENOENT || (err) == EACCES)
#endif

/*
 * O_DIRECT is not standard, but almost every Unix has it.  We translate it
 * to the appropriate Windows flag in src/port/open.c.  We simulate it with
 * fcntl(F_NOCACHE) on macOS inside fd.c's open() wrapper.  We use the name
 * PG_O_DIRECT rather than defining O_DIRECT in that case (probably not a good
 * idea on a Unix).  We can only use it if the compiler will correctly align
 * PGIOAlignedBlock for us, though.
 */

/*
 * O_DIRECT 不是标准接口，但几乎所有 Unix 都具备它。src/port/open.c 将其转换为适当的 Windows 标志；
 * fd.c 的 open() 包装器在 macOS 上通过 fcntl(F_NOCACHE) 模拟它。为避免在该场景定义 O_DIRECT
 * （在 Unix 上可能不是好主意），使用名称 PG_O_DIRECT。前提是编译器能正确对齐 PGIOAlignedBlock。
 */
#if defined(O_DIRECT) && defined(pg_attribute_aligned)
#define		PG_O_DIRECT O_DIRECT
#elif defined(F_NOCACHE)
#define		PG_O_DIRECT 0x80000000
#define		PG_O_DIRECT_USE_F_NOCACHE
#else
#define		PG_O_DIRECT 0
#endif

/*
 * prototypes for functions in fd.c
 */

/*
 * fd.c 中函数的原型。
 */

struct PgAioHandle;

/* Operations on virtual Files --- equivalent to Unix kernel file ops */

/*
 * 虚拟 File 的操作，等价于 Unix 内核文件操作。
 */
/*
 * Opens a pathname through the virtual-file-descriptor layer.
 */

/*
 * 通过虚拟文件描述符层打开路径名。
 */
extern File PathNameOpenFile(const char *fileName, int fileFlags);
/*
 * Opens a pathname with explicit permissions through the VFD layer.
 */

/*
 * 通过 VFD 层以显式权限打开路径名。
 */
extern File PathNameOpenFilePerm(const char *fileName, int fileFlags, mode_t fileMode);
/*
 * Creates a temporary virtual file, optionally lasting across transactions.
 */

/*
 * 创建临时虚拟文件，并可选择使其跨事务存活。
 */
extern File OpenTemporaryFile(bool interXact);
/*
 * Closes a virtual file and releases its VFD resources.
 */

/*
 * 关闭虚拟文件并释放其 VFD 资源。
 */
extern void FileClose(File file);
/*
 * Requests asynchronous prefetching of a virtual file range.
 */

/*
 * 请求异步预取虚拟文件范围。
 */
extern int	FilePrefetch(File file, off_t offset, off_t amount, uint32 wait_event_info);
/*
 * Reads vector buffers from a virtual file at an offset.
 */

/*
 * 从虚拟文件的指定偏移量读取向量缓冲区。
 */
extern ssize_t FileReadV(File file, const struct iovec *iov, int iovcnt, off_t offset, uint32 wait_event_info);
/*
 * Writes vector buffers to a virtual file at an offset.
 */

/*
 * 将向量缓冲区写入虚拟文件的指定偏移量。
 */
extern ssize_t FileWriteV(File file, const struct iovec *iov, int iovcnt, off_t offset, uint32 wait_event_info);
/*
 * Starts asynchronous vector reading into a supplied AIO handle.
 */

/*
 * 使用提供的 AIO 句柄启动异步向量读取。
 */
extern int	FileStartReadV(struct PgAioHandle *ioh, File file, int iovcnt, off_t offset, uint32 wait_event_info);
/*
 * Synchronizes a virtual file's contents to durable storage.
 */

/*
 * 将虚拟文件内容同步到持久存储。
 */
extern int	FileSync(File file, uint32 wait_event_info);
/*
 * Writes zero bytes across a virtual file range.
 */

/*
 * 在虚拟文件范围内写入零字节。
 */
extern int	FileZero(File file, off_t offset, off_t amount, uint32 wait_event_info);
/*
 * Preallocates space for a virtual file range.
 */

/*
 * 为虚拟文件范围预分配空间。
 */
extern int	FileFallocate(File file, off_t offset, off_t amount, uint32 wait_event_info);

/*
 * Returns the current size of a virtual file.
 */

/*
 * 返回虚拟文件的当前大小。
 */
extern off_t FileSize(File file);
/*
 * Truncates or extends a virtual file to the specified offset.
 */

/*
 * 将虚拟文件截断或扩展到指定偏移量。
 */
extern int	FileTruncate(File file, off_t offset, uint32 wait_event_info);
/*
 * Advises the kernel to write back a virtual file range.
 */

/*
 * 建议内核回写虚拟文件范围。
 */
extern void FileWriteback(File file, off_t offset, off_t nbytes, uint32 wait_event_info);
/*
 * Returns the pathname associated with a virtual file.
 */

/*
 * 返回与虚拟文件关联的路径名。
 */
extern char *FilePathName(File file);
/*
 * Returns the underlying kernel descriptor of a virtual file.
 */

/*
 * 返回虚拟文件的底层内核描述符。
 */
extern int	FileGetRawDesc(File file);
/*
 * Returns the raw open flags of a virtual file.
 */

/*
 * 返回虚拟文件的原始打开标志。
 */
extern int	FileGetRawFlags(File file);
/*
 * Returns the raw creation mode associated with a virtual file.
 */

/*
 * 返回与虚拟文件关联的原始创建模式。
 */
extern mode_t FileGetRawMode(File file);

/* Operations used for sharing named temporary files */

/*
 * 用于共享具名临时文件的操作。
 */
/*
 * Creates a named temporary file and returns its virtual descriptor.
 */

/*
 * 创建具名临时文件并返回其虚拟描述符。
 */
extern File PathNameCreateTemporaryFile(const char *path, bool error_on_failure);
/* Operations used for sharing named temporary files */

/*
 * 打开已有具名临时文件。
 */
extern File PathNameOpenTemporaryFile(const char *path, int mode);
/*
 * Deletes a named temporary file and reports success.
 */

/*
 * 删除具名临时文件并报告是否成功。
 */
extern bool PathNameDeleteTemporaryFile(const char *path, bool error_on_failure);
/*
 * Creates a temporary directory below a base directory.
 */

/*
 * 在基础目录下创建临时目录。
 */
extern void PathNameCreateTemporaryDir(const char *basedir, const char *directory);
/*
 * Deletes a temporary directory and its temporary-file bookkeeping.
 */

/*
 * 删除临时目录及其临时文件记录。
 */
extern void PathNameDeleteTemporaryDir(const char *dirname);
/*
 * Constructs the temporary-file path for a tablespace.
 */

/*
 * 构造表空间的临时文件路径。
 */
extern void TempTablespacePath(char *path, Oid tablespace);

/* Operations that allow use of regular stdio --- USE WITH CAUTION */

/*
 * 允许使用常规 stdio 的操作，请谨慎使用。
 */
/*
 * Opens a stdio file while accounting for PostgreSQL file limits.
 */

/*
 * 打开 stdio 文件，同时计入 PostgreSQL 文件限制。
 */
extern FILE *AllocateFile(const char *name, const char *mode);
/*
 * Closes a stdio file acquired through AllocateFile.
 */

/*
 * 关闭通过 AllocateFile 获取的 stdio 文件。
 */
extern int	FreeFile(FILE *file);

/* Operations that allow use of pipe streams (popen/pclose) */

/*
 * 允许使用管道流（popen/pclose）的操作。
 */
/*
 * Opens a pipe stream while accounting for descriptor limits.
 */

/*
 * 打开管道流，同时计入描述符限制。
 */
extern FILE *OpenPipeStream(const char *command, const char *mode);
/*
 * Closes a pipe stream opened by OpenPipeStream.
 */

/*
 * 关闭由 OpenPipeStream 打开的管道流。
 */
extern int	ClosePipeStream(FILE *file);

/* Operations to allow use of the <dirent.h> library routines */

/*
 * 允许使用 <dirent.h> 库例程的操作。
 */
/*
 * Opens a directory while accounting for PostgreSQL file limits.
 */

/*
 * 打开目录，同时计入 PostgreSQL 文件限制。
 */
extern DIR *AllocateDir(const char *dirname);
/*
 * Reads one directory entry and handles ordinary end-of-directory errors.
 */

/*
 * 读取一个目录条目，并处理普通的目录结束错误。
 */
extern struct dirent *ReadDir(DIR *dir, const char *dirname);
/*
 * Reads one directory entry with caller-selected error reporting severity.
 */

/*
 * 读取一个目录条目，并使用调用方选择的错误报告级别。
 */
extern struct dirent *ReadDirExtended(DIR *dir, const char *dirname,
									  int elevel);
/*
 * Closes a directory opened by AllocateDir.
 */

/*
 * 关闭由 AllocateDir 打开的目录。
 */
extern int	FreeDir(DIR *dir);

/* Operations to allow use of a plain kernel FD, with automatic cleanup */

/*
 * 允许使用普通内核 FD 且具备自动清理的操作。
 */
/*
 * Opens a transient kernel descriptor that is automatically cleaned up.
 */

/*
 * 打开会被自动清理的临时内核描述符。
 */
extern int	OpenTransientFile(const char *fileName, int fileFlags);
/*
 * Opens a transient kernel descriptor with explicit file permissions.
 */

/*
 * 以显式文件权限打开临时内核描述符。
 */
extern int	OpenTransientFilePerm(const char *fileName, int fileFlags, mode_t fileMode);
/*
 * Closes a transient kernel descriptor.
 */

/*
 * 关闭临时内核描述符。
 */
extern int	CloseTransientFile(int fd);

/* If you've really really gotta have a plain kernel FD, use this */

/*
 * 若确实必须使用普通内核 FD，请使用这些函数。
 */
/*
 * Opens a plain kernel descriptor with PostgreSQL error handling.
 */

/*
 * 使用 PostgreSQL 错误处理打开普通内核描述符。
 */
extern int	BasicOpenFile(const char *fileName, int fileFlags);
/* Make a directory with default permissions */

/*
 * 以显式权限打开普通内核描述符。
 */
extern int	BasicOpenFilePerm(const char *fileName, int fileFlags, mode_t fileMode);

/* Use these for other cases, and also for long-lived BasicOpenFile FDs */

/*
 * 将这些函数用于其他情况，以及长期存在的 BasicOpenFile FD。
 */
/*
 * Acquires one external-descriptor slot if capacity is available.
 */

/*
 * 若容量可用，获取一个外部描述符槽位。
 */
extern bool AcquireExternalFD(void);
/*
 * Reserves one external-descriptor slot without opening a descriptor.
 */

/*
 * 预留一个外部描述符槽位，但不打开描述符。
 */
extern void ReserveExternalFD(void);
/*
 * Releases a previously acquired or reserved external-descriptor slot.
 */

/*
 * 释放先前获取或预留的外部描述符槽位。
 */
extern void ReleaseExternalFD(void);

/* Make a directory with default permissions */

/*
 * 使用默认权限创建目录。
 */
/*
 * Creates a PostgreSQL directory using the default mode.
 */

/*
 * 使用默认模式创建 PostgreSQL 目录。
 */
extern int	MakePGDirectory(const char *directoryName);

/* Miscellaneous support routines */

/*
 * 杂项支持例程。
 */
/*
 * Initializes virtual-file access state.
 */

/*
 * 初始化虚拟文件访问状态。
 */
extern void InitFileAccess(void);
/*
 * Initializes temporary-file access state.
 */

/*
 * 初始化临时文件访问状态。
 */
extern void InitTemporaryFileAccess(void);
/*
 * Computes the maximum number of safely usable file descriptors.
 */

/*
 * 计算可安全使用的最大文件描述符数。
 */
extern void set_max_safe_fds(void);
/*
 * Closes all currently open virtual file descriptors.
 */

/*
 * 关闭所有当前打开的虚拟文件描述符。
 */
extern void closeAllVfds(void);
/*
 * Sets the tablespaces available for temporary files.
 */

/*
 * 设置可供临时文件使用的表空间。
 */
extern void SetTempTablespaces(Oid *tableSpaces, int numSpaces);
/*
 * Reports whether temporary tablespaces have been configured.
 */

/*
 * 报告是否已配置临时表空间。
 */
extern bool TempTablespacesAreSet(void);
/*
 * Copies configured temporary tablespaces to a caller array.
 */

/*
 * 将已配置的临时表空间复制到调用方数组。
 */
extern int	GetTempTablespaces(Oid *tableSpaces, int numSpaces);
/*
 * Selects the next temporary tablespace using the configured policy.
 */

/*
 * 根据配置策略选择下一个临时表空间。
 */
extern Oid	GetNextTempTableSpace(void);
/*
 * Performs end-of-transaction temporary-file cleanup.
 */

/*
 * 执行事务结束时的临时文件清理。
 */
extern void AtEOXact_Files(bool isCommit);
/*
 * Performs end-of-subtransaction temporary-file cleanup.
 */

/*
 * 执行子事务结束时的临时文件清理。
 */
extern void AtEOSubXact_Files(bool isCommit, SubTransactionId mySubid,
							  SubTransactionId parentSubid);
/*
 * Removes temporary files from the standard temporary-file locations.
 */

/*
 * 从标准临时文件位置移除临时文件。
 */
extern void RemovePgTempFiles(void);
/*
 * Removes temporary files in one directory according to cleanup options.
 */

/*
 * 根据清理选项移除一个目录中的临时文件。
 */
extern void RemovePgTempFilesInDir(const char *tmpdirname, bool missing_ok,
								   bool unlink_all);
/*
 * Tests whether a name has the form of a temporary relation name.
 */

/*
 * 测试名称是否具有临时关系名称的形式。
 */
extern bool looks_like_temp_rel_name(const char *name);

/*
 * Synchronizes a kernel file descriptor.
 */

/*
 * 同步内核文件描述符。
 */
extern int	pg_fsync(int fd);
/*
 * Synchronizes a descriptor without Windows write-through behavior.
 */

/*
 * 同步描述符，但不采用 Windows 写穿透行为。
 */
extern int	pg_fsync_no_writethrough(int fd);
/*
 * Synchronizes a descriptor with Windows write-through behavior.
 */

/*
 * 使用 Windows 写穿透行为同步描述符。
 */
extern int	pg_fsync_writethrough(int fd);
/*
 * Synchronizes file data without necessarily syncing metadata.
 */

/*
 * 同步文件数据，但不一定同步元数据。
 */
extern int	pg_fdatasync(int fd);
/*
 * Tests whether a filesystem entry exists.
 */

/*
 * 测试文件系统条目是否存在。
 */
extern bool pg_file_exists(const char *name);
/*
 * Flushes a file-data range from the operating system cache.
 */

/*
 * 从操作系统缓存中刷新文件数据范围。
 */
extern void pg_flush_data(int fd, off_t offset, off_t nbytes);
/*
 * Truncates a named file to the requested length.
 */

/*
 * 将具名文件截断到请求的长度。
 */
extern int	pg_truncate(const char *path, off_t length);
/*
 * Synchronizes a file or directory named by pathname.
 */

/*
 * 同步由路径名指定的文件或目录。
 */
extern void fsync_fname(const char *fname, bool isdir);
/*
 * Synchronizes a pathname with permission and error-level controls.
 */

/*
 * 使用权限和错误级别控制同步路径名。
 */
extern int	fsync_fname_ext(const char *fname, bool isdir, bool ignore_perm, int elevel);
/*
 * Renames a file and makes the rename durable.
 */

/*
 * 重命名文件并确保重命名持久化。
 */
extern int	durable_rename(const char *oldfile, const char *newfile, int elevel);
/*
 * Removes a file and makes the removal durable.
 */

/*
 * 删除文件并确保删除持久化。
 */
extern int	durable_unlink(const char *fname, int elevel);
/*
 * Synchronizes all data-directory contents that require durability.
 */

/*
 * 同步数据目录中所有需要持久化的内容。
 */
extern void SyncDataDirectory(void);
/*
 * Maps a data-sync error level to the configured reporting level.
 */

/*
 * 将数据同步错误级别映射为配置的报告级别。
 */
extern int	data_sync_elevel(int elevel);

/*
 * Reads one contiguous buffer by delegating to the vector-read primitive.
 * It wraps the buffer in a single iovec and calls FileReadV.
 */

/*
 * 通过委托给向量读取原语来读取一个连续缓冲区；它将缓冲区包装为单个 iovec 并调用 FileReadV。
 */
static inline ssize_t
FileRead(File file, void *buffer, size_t amount, off_t offset,
		 uint32 wait_event_info)
{
	struct iovec iov = {
		.iov_base = buffer,
		.iov_len = amount
	};

	return FileReadV(file, &iov, 1, offset, wait_event_info);
}

/*
 * Writes one contiguous buffer by delegating to the vector-write primitive.
 * It wraps the buffer in a single iovec and calls FileWriteV.
 */

/*
 * 通过委托给向量写入原语来写入一个连续缓冲区；它将缓冲区包装为单个 iovec 并调用 FileWriteV。
 */
static inline ssize_t
FileWrite(File file, const void *buffer, size_t amount, off_t offset,
		  uint32 wait_event_info)
{
	struct iovec iov = {
		.iov_base = unconstify(void *, buffer),
		.iov_len = amount
	};

	return FileWriteV(file, &iov, 1, offset, wait_event_info);
}

#endif							/* FD_H */

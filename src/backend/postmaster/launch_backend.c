/*-------------------------------------------------------------------------
 *
 * launch_backend.c
 *	  Functions for launching backends and other postmaster child
 *	  processes.
 *	  启动后端进程和其他 postmaster 子进程的函数。
 *
 * On Unix systems, a new child process is launched with fork().  It inherits
 * all the global variables and data structures that had been initialized in
 * the postmaster.  After forking, the child process closes the file
 * descriptors that are not needed in the child process, and sets up the
 * mechanism to detect death of the parent postmaster process, etc.  After
 * that, it calls the right Main function depending on the kind of child
 * process.
 * 在 Unix 系统上，使用 fork() 启动一个新的子进程。它继承了在 postmaster 中已初始化的
 * 所有全局变量和数据结构。fork 之后，子进程关闭在子进程中不需要的文件描述符，
 * 并建立检测父 postmaster 进程死亡的机制等。之后，它根据子进程的种类调用相应的 Main 函数。
 *
 * In EXEC_BACKEND mode, which is used on Windows but can be enabled on other
 * platforms for testing, the child process is launched by fork() + exec() (or
 * CreateProcess() on Windows).  It does not inherit the state from the
 * postmaster, so it needs to re-attach to the shared memory, re-initialize
 * global variables, reload the config file etc. to get the process to the
 * same state as after fork() on a Unix system.
 * 在 EXEC_BACKEND 模式下（该模式在 Windows 上使用，但也可以在其他平台上启用以进行测试），
 * 子进程通过 fork() + exec()（在 Windows 上是 CreateProcess()）启动。
 * 它不继承自 postmaster 的状态，因此它需要重新连接到共享内存，重新初始化全局变量，
 * 重新加载配置文件等，以使进程达到与 Unix 系统上 fork() 之后相同的状态。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/launch_backend.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>

#include "libpq/libpq-be.h"
#include "miscadmin.h"
#include "postmaster/autovacuum.h"
#include "postmaster/bgworker_internals.h"
#include "postmaster/bgwriter.h"
#include "postmaster/fork_process.h"
#include "postmaster/pgarch.h"
#include "postmaster/postmaster.h"
#include "postmaster/startup.h"
#include "postmaster/syslogger.h"
#include "postmaster/walsummarizer.h"
#include "postmaster/walwriter.h"
#include "replication/slotsync.h"
#include "replication/walreceiver.h"
#include "storage/dsm.h"
#include "storage/io_worker.h"
#include "storage/pg_shmem.h"
#include "tcop/backend_startup.h"
#include "utils/memutils.h"

#ifdef EXEC_BACKEND
#include "nodes/queryjumble.h"
#include "storage/pg_shmem.h"
#include "storage/spin.h"
#endif


#ifdef EXEC_BACKEND

#include "common/file_utils.h"
#include "storage/fd.h"
#include "storage/lwlock.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "tcop/tcopprot.h"
#include "utils/injection_point.h"

/* Type for a socket that can be inherited to a client process */
/* 可以继承到客户端进程的套接字的类型 */
#ifdef WIN32
typedef struct
{
	SOCKET		origsocket;		/* Original socket value, or PGINVALID_SOCKET
								 * if not a socket */
								/* 原始套接字值，如果不是套接字则为 PGINVALID_SOCKET */
	WSAPROTOCOL_INFO wsainfo;
} InheritableSocket;
#else
typedef int InheritableSocket;
#endif

/*
 * Structure contains all variables passed to exec:ed backends
 * 包含传递给 exec 执行的后端的所有变量的结构体
 */
typedef struct
{
	char		DataDir[MAXPGPATH];
#ifndef WIN32
	unsigned long UsedShmemSegID;
#else
	void	   *ShmemProtectiveRegion;
	HANDLE		UsedShmemSegID;
#endif
	void	   *UsedShmemSegAddr;
	slock_t    *ShmemLock;
#ifdef USE_INJECTION_POINTS
	struct InjectionPointsCtl *ActiveInjectionPoints;
#endif
	int			NamedLWLockTrancheRequests;
	NamedLWLockTranche *NamedLWLockTrancheArray;
	LWLockPadded *MainLWLockArray;
	slock_t    *ProcStructLock;
	PROC_HDR   *ProcGlobal;
	PGPROC	   *AuxiliaryProcs;
	PGPROC	   *PreparedXactProcs;
	volatile PMSignalData *PMSignalState;
	ProcSignalHeader *ProcSignal;
	pid_t		PostmasterPid;
	TimestampTz PgStartTime;
	TimestampTz PgReloadTime;
	pg_time_t	first_syslogger_file_time;
	bool		redirection_done;
	bool		IsBinaryUpgrade;
	bool		query_id_enabled;
	int			max_safe_fds;
	int			MaxBackends;
	int			num_pmchild_slots;
#ifdef WIN32
	HANDLE		PostmasterHandle;
	HANDLE		initial_signal_pipe;
	HANDLE		syslogPipe[2];
#else
	int			postmaster_alive_fds[2];
	int			syslogPipe[2];
#endif
	char		my_exec_path[MAXPGPATH];
	char		pkglib_path[MAXPGPATH];

	int			MyPMChildSlot;

	/*
	 * These are only used by backend processes, but are here because passing
	 * a socket needs some special handling on Windows. 'client_sock' is an
	 * explicit argument to postmaster_child_launch, but is stored in
	 * MyClientSocket in the child process.
	 * 这些仅由后端进程使用，但在放在此处是因为在 Windows 上传递套接字需要一些特殊处理。
	 * 'client_sock' 是 postmaster_child_launch 的显式参数，但会存储在子进程的 MyClientSocket 中。
	 */
	ClientSocket client_sock;
	InheritableSocket inh_sock;

	/*
	 * Extra startup data, content depends on the child process.
	 * 额外的启动数据，内容取决于子进程。
	 */
	size_t		startup_data_len;
	char		startup_data[FLEXIBLE_ARRAY_MEMBER];
} BackendParameters;

#define SizeOfBackendParameters(startup_data_len) (offsetof(BackendParameters, startup_data) + startup_data_len)

static void read_backend_variables(char *id, void **startup_data, size_t *startup_data_len);
static void restore_backend_variables(BackendParameters *param);

static bool save_backend_variables(BackendParameters *param, int child_slot,
								   ClientSocket *client_sock,
#ifdef WIN32
								   HANDLE childProcess, pid_t childPid,
#endif
								   const void *startup_data, size_t startup_data_len);

static pid_t internal_forkexec(const char *child_kind, int child_slot,
							   const void *startup_data, size_t startup_data_len,
							   ClientSocket *client_sock);

#endif							/* EXEC_BACKEND */

/*
 * Information needed to launch different kinds of child processes.
 * 启动不同种类的子进程所需的信息。
 */
typedef struct
{
	const char *name;
	void		(*main_fn) (const void *startup_data, size_t startup_data_len);
	bool		shmem_attach;
} child_process_kind;

static child_process_kind child_process_kinds[] = {
	[B_INVALID] = {"invalid", NULL, false},

	[B_BACKEND] = {"backend", BackendMain, true},
	[B_DEAD_END_BACKEND] = {"dead-end backend", BackendMain, true},
	[B_AUTOVAC_LAUNCHER] = {"autovacuum launcher", AutoVacLauncherMain, true},
	[B_AUTOVAC_WORKER] = {"autovacuum worker", AutoVacWorkerMain, true},
	[B_BG_WORKER] = {"bgworker", BackgroundWorkerMain, true},

	/*
	 * WAL senders start their life as regular backend processes, and change
	 * their type after authenticating the client for replication.  We list it
	 * here for PostmasterChildName() but cannot launch them directly.
	 * WAL 发送器（WAL senders）起初是常规后端进程，在对进行复制的客户端进行身份验证后
	 * 改变其类型。我们在此处列出它是为了配合 PostmasterChildName()，但无法直接启动它们。
	 */
	[B_WAL_SENDER] = {"wal sender", NULL, true},
	[B_SLOTSYNC_WORKER] = {"slot sync worker", ReplSlotSyncWorkerMain, true},

	[B_STANDALONE_BACKEND] = {"standalone backend", NULL, false},

	[B_ARCHIVER] = {"archiver", PgArchiverMain, true},
	[B_BG_WRITER] = {"bgwriter", BackgroundWriterMain, true},
	[B_CHECKPOINTER] = {"checkpointer", CheckpointerMain, true},
	[B_IO_WORKER] = {"io_worker", IoWorkerMain, true},
	[B_STARTUP] = {"startup", StartupProcessMain, true},
	[B_WAL_RECEIVER] = {"wal_receiver", WalReceiverMain, true},
	[B_WAL_SUMMARIZER] = {"wal_summarizer", WalSummarizerMain, true},
	[B_WAL_WRITER] = {"wal_writer", WalWriterMain, true},

	[B_LOGGER] = {"syslogger", SysLoggerMain, false},
};

const char *
PostmasterChildName(BackendType child_type)
{
	return child_process_kinds[child_type].name;
}

/*
 * Start a new postmaster child process.
 * 启动一个新的 postmaster 子进程。
 *
 * The child process will be restored to roughly the same state whether
 * EXEC_BACKEND is used or not: it will be attached to shared memory if
 * appropriate, and fds and other resources that we've inherited from
 * postmaster that are not needed in a child process have been closed.
 * 无论是否使用 EXEC_BACKEND，子进程都将被恢复到大致相同的状态：
 * 如果合适，它将连接到共享内存，并且从 postmaster 继承但在子进程中不需要的
 * 文件描述符和其他资源已被关闭。
 *
 * 'child_slot' is the PMChildFlags array index reserved for the child
 * process.  'startup_data' is an optional contiguous chunk of data that is
 * passed to the child process.
 * 'child_slot' 是为子进程保留的 PMChildFlags 数组索引。
 * 'startup_data' 是传递给子进程的可选连续数据块。
 */
pid_t
postmaster_child_launch(BackendType child_type, int child_slot,
						void *startup_data, size_t startup_data_len,
						ClientSocket *client_sock)
{
	pid_t		pid;

	Assert(IsPostmasterEnvironment && !IsUnderPostmaster);

	/* Capture time Postmaster initiates process creation for logging */
	/* 捕获 Postmaster 发起进程创建的时间，用于记录日志 */
	if (IsExternalConnectionBackend(child_type))
		((BackendStartupData *) startup_data)->fork_started = GetCurrentTimestamp();

#ifdef EXEC_BACKEND
	pid = internal_forkexec(child_process_kinds[child_type].name, child_slot,
							startup_data, startup_data_len, client_sock);
	/* the child process will arrive in SubPostmasterMain */
	/* 子进程将在 SubPostmasterMain 中到达 */
#else							/* !EXEC_BACKEND */
	pid = fork_process();
	if (pid == 0)				/* child */
	{
		/* Capture and transfer timings that may be needed for logging */
		/* 捕获并传输日志记录可能需要的计时数据 */
		if (IsExternalConnectionBackend(child_type))
		{
			conn_timing.socket_create =
				((BackendStartupData *) startup_data)->socket_created;
			conn_timing.fork_start =
				((BackendStartupData *) startup_data)->fork_started;
			conn_timing.fork_end = GetCurrentTimestamp();
		}

		/* Close the postmaster's sockets */
		/* 关闭 postmaster 的套接字 */
		ClosePostmasterPorts(child_type == B_LOGGER);

		/* Detangle from postmaster */
		/* 脱离 postmaster 的控制 */
		InitPostmasterChild();

		/* Detach shared memory if not needed. */
		/* 如果不需要，则分离共享内存。 */
		if (!child_process_kinds[child_type].shmem_attach)
		{
			dsm_detach_all();
			PGSharedMemoryDetach();
		}

		/*
		 * Enter the Main function with TopMemoryContext.  The startup data is
		 * allocated in PostmasterContext, so we cannot release it here yet.
		 * The Main function will do it after it's done handling the startup
		 * data.
		 * 使用 TopMemoryContext 进入 Main 函数。启动数据是在 PostmasterContext 
		 * 中分配的，所以我们现在还不能在这里释放它。
		 * Main 函数将在处理完启动数据后释放它。
		 */
		MemoryContextSwitchTo(TopMemoryContext);

		MyPMChildSlot = child_slot;
		if (client_sock)
		{
			MyClientSocket = palloc(sizeof(ClientSocket));
			memcpy(MyClientSocket, client_sock, sizeof(ClientSocket));
		}

		/*
		 * Run the appropriate Main function
		 * 运行相应的 Main 函数
		 */
		child_process_kinds[child_type].main_fn(startup_data, startup_data_len);
		pg_unreachable();		/* main_fn never returns */
	}
#endif							/* EXEC_BACKEND */
	return pid;
}

#ifdef EXEC_BACKEND
#ifndef WIN32

/*
 * internal_forkexec non-win32 implementation
 * internal_forkexec 非 win32 实现
 *
 * - writes out backend variables to the parameter file
 * - fork():s, and then exec():s the child process
 * - 将后端变量写入参数文件
 * - fork()，然后 exec() 子进程
 */
static pid_t
internal_forkexec(const char *child_kind, int child_slot,
				  const void *startup_data, size_t startup_data_len, ClientSocket *client_sock)
{
	static unsigned long tmpBackendFileNum = 0;
	pid_t		pid;
	char		tmpfilename[MAXPGPATH];
	size_t		paramsz;
	BackendParameters *param;
	FILE	   *fp;
	char	   *argv[4];
	char		forkav[MAXPGPATH];

	/*
	 * Use palloc0 to make sure padding bytes are initialized, to prevent
	 * Valgrind from complaining about writing uninitialized bytes to the
	 * file.  This isn't performance critical, and the win32 implementation
	 * initializes the padding bytes to zeros, so do it even when not using
	 * Valgrind.
	 * 使用 palloc0 确保初始化填充字节，以防止 Valgrind 抱怨将未初始化的字节
	 * 写入文件。这对于性能并不关键，而且 win32 实现将填充字节初始化为零，
	 * 所以即使不使用 Valgrind，也请这样做。
	 */
	paramsz = SizeOfBackendParameters(startup_data_len);
	param = palloc0(paramsz);
	if (!save_backend_variables(param, child_slot, client_sock, startup_data, startup_data_len))
	{
		pfree(param);
		return -1;				/* log made by save_backend_variables */
	}

	/* Calculate name for temp file */
	/* 计算临时文件的名称 */
	snprintf(tmpfilename, MAXPGPATH, "%s/%s.backend_var.%d.%lu",
			 PG_TEMP_FILES_DIR, PG_TEMP_FILE_PREFIX,
			 MyProcPid, ++tmpBackendFileNum);

	/* Open file */
	/* 打开文件 */
	fp = AllocateFile(tmpfilename, PG_BINARY_W);
	if (!fp)
	{
		/*
		 * As in OpenTemporaryFileInTablespace, try to make the temp-file
		 * directory, ignoring errors.
		 * 如同 OpenTemporaryFileInTablespace 一样，尝试创建临时文件目录，忽略错误。
		 */
		(void) MakePGDirectory(PG_TEMP_FILES_DIR);

		fp = AllocateFile(tmpfilename, PG_BINARY_W);
		if (!fp)
		{
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not create file \"%s\": %m",
							tmpfilename)));
			pfree(param);
			return -1;
		}
	}

	if (fwrite(param, paramsz, 1, fp) != 1)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m", tmpfilename)));
		FreeFile(fp);
		pfree(param);
		return -1;
	}
	pfree(param);

	/* Release file */
	/* 释放文件 */
	if (FreeFile(fp))
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m", tmpfilename)));
		return -1;
	}

	/* set up argv properly */
	/* 正确设置 argv */
	argv[0] = "postgres";
	snprintf(forkav, MAXPGPATH, "--forkchild=%s", child_kind);
	argv[1] = forkav;
	/* Insert temp file name after --forkchild argument */
	/* 在 --forkchild 参数后插入临时文件名 */
	argv[2] = tmpfilename;
	argv[3] = NULL;

	/* Fire off execv in child */
	/* 在子进程中启动 execv */
	if ((pid = fork_process()) == 0)
	{
		if (execv(postgres_exec_path, argv) < 0)
		{
			ereport(LOG,
					(errmsg("could not execute server process \"%s\": %m",
							postgres_exec_path)));
			/* We're already in the child process here, can't return */
			/* 我们此时已经处于子进程中，无法返回 */
			exit(1);
		}
	}

	return pid;					/* Parent returns pid, or -1 on fork failure */
								/* 父进程返回 pid，或者在 fork 失败时返回 -1 */
}
#else							/* WIN32 */

/*
 * internal_forkexec win32 implementation
 * internal_forkexec win32 实现
 *
 * - starts backend using CreateProcess(), in suspended state
 * - writes out backend variables to the parameter file
 *	- during this, duplicates handles and sockets required for
 *	  inheritance into the new process
 * - resumes execution of the new process once the backend parameter
 *	 file is complete.
 * - 使用 CreateProcess() 启动后端，处于挂起（suspended）状态
 * - 将后端变量写入参数文件
 *  - 在此期间，复制继承到新进程中所需的句柄和套接字
 * - 一旦后端参数文件完成，恢复新进程的执行。
 */
static pid_t
internal_forkexec(const char *child_kind, int child_slot,
				  const void *startup_data, size_t startup_data_len, ClientSocket *client_sock)
{
	int			retry_count = 0;
	STARTUPINFO si;
	PROCESS_INFORMATION pi;
	char		cmdLine[MAXPGPATH * 2];
	HANDLE		paramHandle;
	BackendParameters *param;
	SECURITY_ATTRIBUTES sa;
	size_t		paramsz;
	char		paramHandleStr[32];
	int			l;

	paramsz = SizeOfBackendParameters(startup_data_len);

	/* Resume here if we need to retry */
	/* 如果我们需要重试，请在此处恢复 */
retry:

	/* Set up shared memory for parameter passing */
	/* 设置共享内存以传递参数 */
	ZeroMemory(&sa, sizeof(sa));
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;
	paramHandle = CreateFileMapping(INVALID_HANDLE_VALUE,
									&sa,
									PAGE_READWRITE,
									0,
									paramsz,
									NULL);
	if (paramHandle == INVALID_HANDLE_VALUE)
	{
		ereport(LOG,
				(errmsg("could not create backend parameter file mapping: error code %lu",
						GetLastError())));
		return -1;
	}
	param = MapViewOfFile(paramHandle, FILE_MAP_WRITE, 0, 0, paramsz);
	if (!param)
	{
		ereport(LOG,
				(errmsg("could not map backend parameter memory: error code %lu",
						GetLastError())));
		CloseHandle(paramHandle);
		return -1;
	}

	/* Format the cmd line */
	/* 格式化命令行 */
#ifdef _WIN64
	sprintf(paramHandleStr, "%llu", (LONG_PTR) paramHandle);
#else
	sprintf(paramHandleStr, "%lu", (DWORD) paramHandle);
#endif
	l = snprintf(cmdLine, sizeof(cmdLine) - 1, "\"%s\" --forkchild=\"%s\" %s",
				 postgres_exec_path, child_kind, paramHandleStr);
	if (l >= sizeof(cmdLine))
	{
		ereport(LOG,
				(errmsg("subprocess command line too long")));
		UnmapViewOfFile(param);
		CloseHandle(paramHandle);
		return -1;
	}

	memset(&pi, 0, sizeof(pi));
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);

	/*
	 * Create the subprocess in a suspended state. This will be resumed later,
	 * once we have written out the parameter file.
	 * 以挂起状态创建子进程。一旦我们写出参数文件，这将在稍后恢复。
	 */
	if (!CreateProcess(NULL, cmdLine, NULL, NULL, TRUE, CREATE_SUSPENDED,
					   NULL, NULL, &si, &pi))
	{
		ereport(LOG,
				(errmsg("CreateProcess() call failed: %m (error code %lu)",
						GetLastError())));
		UnmapViewOfFile(param);
		CloseHandle(paramHandle);
		return -1;
	}

	if (!save_backend_variables(param, child_slot, client_sock,
								pi.hProcess, pi.dwProcessId,
								startup_data, startup_data_len))
	{
		/*
		 * log made by save_backend_variables, but we have to clean up the
		 * mess with the half-started process
		 * 日志由 save_backend_variables 生成，但我们必须清理半启动进程留下的混乱局面
		 */
		if (!TerminateProcess(pi.hProcess, 255))
			ereport(LOG,
					(errmsg_internal("could not terminate unstarted process: error code %lu",
									 GetLastError())));
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
		UnmapViewOfFile(param);
		CloseHandle(paramHandle);
		return -1;				/* log made by save_backend_variables */
	}

	/* Drop the parameter shared memory that is now inherited to the backend */
	/* 丢弃现在已继承到后端的参数共享内存 */
	if (!UnmapViewOfFile(param))
		ereport(LOG,
				(errmsg("could not unmap view of backend parameter file: error code %lu",
						GetLastError())));
	if (!CloseHandle(paramHandle))
		ereport(LOG,
				(errmsg("could not close handle to backend parameter file: error code %lu",
						GetLastError())));

	/*
	 * Reserve the memory region used by our main shared memory segment before
	 * we resume the child process.  Normally this should succeed, but if ASLR
	 * is active then it might sometimes fail due to the stack or heap having
	 * gotten mapped into that range.  In that case, just terminate the
	 * process and retry.
	 * 在恢复子进程之前，预留我们主共享内存段使用的内存区域。
	 * 通常这应该会成功，但如果 ASLR 处于活动状态，那么有时可能会因为栈或堆
	 * 被映射到该范围内而失败。在这种情况下，只需终止进程并重试。
	 */
	if (!pgwin32_ReserveSharedMemoryRegion(pi.hProcess))
	{
		/* pgwin32_ReserveSharedMemoryRegion already made a log entry */
		if (!TerminateProcess(pi.hProcess, 255))
			ereport(LOG,
					(errmsg_internal("could not terminate process that failed to reserve memory: error code %lu",
									 GetLastError())));
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
		if (++retry_count < 100)
			goto retry;
		ereport(LOG,
				(errmsg("giving up after too many tries to reserve shared memory"),
				 errhint("This might be caused by ASLR or antivirus software.")));
		return -1;
	}

	/*
	 * Now that the backend variables are written out, we start the child
	 * thread so it can start initializing while we set up the rest of the
	 * parent state.
	 * 既然已经写出了后端变量，我们启动子线程，以便在我们设置父进程状态的其余部分时，
	 * 它可以开始初始化。
	 */
	if (ResumeThread(pi.hThread) == -1)
	{
		if (!TerminateProcess(pi.hProcess, 255))
		{
			ereport(LOG,
					(errmsg_internal("could not terminate unstartable process: error code %lu",
									 GetLastError())));
			CloseHandle(pi.hProcess);
			CloseHandle(pi.hThread);
			return -1;
		}
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
		ereport(LOG,
				(errmsg_internal("could not resume thread of unstarted process: error code %lu",
								 GetLastError())));
		return -1;
	}

	/* Setup notification when the child process dies */
	/* 设置子进程死亡时的通知 */
	pgwin32_register_deadchild_callback(pi.hProcess, pi.dwProcessId);

	/* Don't close pi.hProcess, it's owned by the deadchild callback now */

	CloseHandle(pi.hThread);

	return pi.dwProcessId;
}
#endif							/* WIN32 */

/*
 * SubPostmasterMain -- Get the fork/exec'd process into a state equivalent
 *			to what it would be if we'd simply forked on Unix, and then
 *			dispatch to the appropriate place.
 * SubPostmasterMain -- 让 fork/exec 启动的进程进入与在 Unix 上单纯 fork 
 *          相当的状态，然后调度到适当的位置。
 *
 * The first two command line arguments are expected to be "--forkchild=<name>",
 * where <name> indicates which postmaster child we are to become, and
 * the name of a variables file that we can read to load data that would
 * have been inherited by fork() on Unix.
 * 前两个命令行参数预期为 "--forkchild=<name>"，其中 <name> 指示我们要成为哪个 
 * postmaster 子进程，以及一个变量文件的名称，我们可以读取该文件来加载那些在 Unix 上
 * 本会通过 fork() 继承的数据。
 */
void
SubPostmasterMain(int argc, char *argv[])
{
	void	   *startup_data;
	size_t		startup_data_len;
	char	   *child_kind;
	BackendType child_type;
	bool		found = false;
	TimestampTz fork_end;

	/* In EXEC_BACKEND case we will not have inherited these settings */
	/* 在 EXEC_BACKEND 情况下，我们将无法继承这些设置 */
	IsPostmasterEnvironment = true;
	whereToSendOutput = DestNone;

	/*
	 * Capture the end of process creation for logging. We don't include the
	 * time spent copying data from shared memory and setting up the backend.
	 * 捕获进程创建的结束以进行记录。我们不包括从共享内存复制数据和设置后端所花费的时间。
	 */
	fork_end = GetCurrentTimestamp();

	/* Setup essential subsystems (to ensure elog() behaves sanely) */
	/* 设置基本子系统（确保 elog() 表现正常） */
	InitializeGUCOptions();

	/* Check we got appropriate args */
	/* 检查我们是否获得了合适的参数 */
	if (argc != 3)
		elog(FATAL, "invalid subpostmaster invocation");

	/* Find the entry in child_process_kinds */
	/* 在 child_process_kinds 中查找条目 */
	if (strncmp(argv[1], "--forkchild=", 12) != 0)
		elog(FATAL, "invalid subpostmaster invocation (--forkchild argument missing)");
	child_kind = argv[1] + 12;
	found = false;
	for (int idx = 0; idx < lengthof(child_process_kinds); idx++)
	{
		if (strcmp(child_process_kinds[idx].name, child_kind) == 0)
		{
			child_type = (BackendType) idx;
			found = true;
			break;
		}
	}
	if (!found)
		elog(ERROR, "unknown child kind %s", child_kind);

	/* Read in the variables file */
	/* 读取变量文件 */
	read_backend_variables(argv[2], &startup_data, &startup_data_len);

	/* Close the postmaster's sockets (as soon as we know them) */
	/* 关闭 postmaster 的套接字（一旦我们知道了它们） */
	ClosePostmasterPorts(child_type == B_LOGGER);

	/* Setup as postmaster child */
	/* 设置为 postmaster 子进程 */
	InitPostmasterChild();

	/*
	 * If appropriate, physically re-attach to shared memory segment. We want
	 * to do this before going any further to ensure that we can attach at the
	 * same address the postmaster used.  On the other hand, if we choose not
	 * to re-attach, we may have other cleanup to do.
	 * 如果合适，物理上重新连接到共享内存段。我们希望在做任何进一步操作之前执行此操作，
	 * 以确保我们可以依附于 postmaster 所使用的相同地址。另一方面，如果我们选择不重新连接，
	 * 我们可能还有其他清理工作要做。
	 *
	 * If testing EXEC_BACKEND on Linux, you should run this as root before
	 * starting the postmaster:
	 * 如果在 Linux 上测试 EXEC_BACKEND，你应该在启动 postmaster 之前以 root 身份运行此命令：
	 *
	 * sysctl -w kernel.randomize_va_space=0
	 *
	 * This prevents using randomized stack and code addresses that cause the
	 * child process's memory map to be different from the parent's, making it
	 * sometimes impossible to attach to shared memory at the desired address.
	 * Return the setting to its old value (usually '1' or '2') when finished.
	 * 这可以防止使用随机的栈和代码地址，它们会导致子进程的内存映射与父进程不同，
	 * 使得有时无法在所需的地址连接到共享内存。完成后将该设置恢复为旧值（通常为 '1' 或 '2'）。
	 */
	if (child_process_kinds[child_type].shmem_attach)
		PGSharedMemoryReAttach();
	else
		PGSharedMemoryNoReAttach();

	/* Read in remaining GUC variables */
	/* 读取其余的 GUC 变量 */
	read_nondefault_variables();

	/* Capture and transfer timings that may be needed for log_connections */
	/* 捕获并传输 log_connections 可能需要的计时信息 */
	if (IsExternalConnectionBackend(child_type))
	{
		conn_timing.socket_create =
			((BackendStartupData *) startup_data)->socket_created;
		conn_timing.fork_start =
			((BackendStartupData *) startup_data)->fork_started;
		conn_timing.fork_end = fork_end;
	}

	/*
	 * Check that the data directory looks valid, which will also check the
	 * privileges on the data directory and update our umask and file/group
	 * variables for creating files later.  Note: this should really be done
	 * before we create any files or directories.
	 * 检查数据目录看起来是否有效，这也将检查数据目录上的特权，
	 * 并更新我们的 umask 和文件/组变量，以便稍后创建文件。
	 * 注意：这实际上应该在我们创建任何文件或目录之前完成。
	 */
	checkDataDir();

	/*
	 * (re-)read control file, as it contains config. The postmaster will
	 * already have read this, but this process doesn't know about that.
	 * （重新）读取控制文件，因为它包含配置。postmaster 应该已经读取过了，
	 * 但此进程并不知道这件事。
	 */
	LocalProcessControlFile(false);

	/*
	 * Reload any libraries that were preloaded by the postmaster.  Since we
	 * exec'd this process, those libraries didn't come along with us; but we
	 * should load them into all child processes to be consistent with the
	 * non-EXEC_BACKEND behavior.
	 * 重新加载由 postmaster 预加载的任何库。因为我们 exec 了该进程，
	 * 那些库并没有跟随我们一起加载；但我们应该将它们加载到所有子进程中，
	 * 以便与非 EXEC_BACKEND 行为保持一致。
	 */
	process_shared_preload_libraries();

	/* Restore basic shared memory pointers */
	/* 恢复基本共享内存指针 */
	if (UsedShmemSegAddr != NULL)
		InitShmemAccess(UsedShmemSegAddr);

	/*
	 * Run the appropriate Main function
	 * 运行相应的 Main 函数
	 */
	child_process_kinds[child_type].main_fn(startup_data, startup_data_len);
	pg_unreachable();			/* main_fn never returns */
}

#ifndef WIN32
#define write_inheritable_socket(dest, src, childpid) ((*(dest) = (src)), true)
#define read_inheritable_socket(dest, src) (*(dest) = *(src))
#else
static bool write_duplicated_handle(HANDLE *dest, HANDLE src, HANDLE child);
static bool write_inheritable_socket(InheritableSocket *dest, SOCKET src,
									 pid_t childPid);
static void read_inheritable_socket(SOCKET *dest, InheritableSocket *src);
#endif


/* Save critical backend variables into the BackendParameters struct */
/* 将关键后端变量保存到 BackendParameters 结构中 */
static bool
save_backend_variables(BackendParameters *param,
					   int child_slot, ClientSocket *client_sock,
#ifdef WIN32
					   HANDLE childProcess, pid_t childPid,
#endif
					   const void *startup_data, size_t startup_data_len)
{
	if (client_sock)
		memcpy(&param->client_sock, client_sock, sizeof(ClientSocket));
	else
		memset(&param->client_sock, 0, sizeof(ClientSocket));
	if (!write_inheritable_socket(&param->inh_sock,
								  client_sock ? client_sock->sock : PGINVALID_SOCKET,
								  childPid))
		return false;

	strlcpy(param->DataDir, DataDir, MAXPGPATH);

	param->MyPMChildSlot = child_slot;

#ifdef WIN32
	param->ShmemProtectiveRegion = ShmemProtectiveRegion;
#endif
	param->UsedShmemSegID = UsedShmemSegID;
	param->UsedShmemSegAddr = UsedShmemSegAddr;

	param->ShmemLock = ShmemLock;

#ifdef USE_INJECTION_POINTS
	param->ActiveInjectionPoints = ActiveInjectionPoints;
#endif

	param->NamedLWLockTrancheRequests = NamedLWLockTrancheRequests;
	param->NamedLWLockTrancheArray = NamedLWLockTrancheArray;
	param->MainLWLockArray = MainLWLockArray;
	param->ProcStructLock = ProcStructLock;
	param->ProcGlobal = ProcGlobal;
	param->AuxiliaryProcs = AuxiliaryProcs;
	param->PreparedXactProcs = PreparedXactProcs;
	param->PMSignalState = PMSignalState;
	param->ProcSignal = ProcSignal;

	param->PostmasterPid = PostmasterPid;
	param->PgStartTime = PgStartTime;
	param->PgReloadTime = PgReloadTime;
	param->first_syslogger_file_time = first_syslogger_file_time;

	param->redirection_done = redirection_done;
	param->IsBinaryUpgrade = IsBinaryUpgrade;
	param->query_id_enabled = query_id_enabled;
	param->max_safe_fds = max_safe_fds;

	param->MaxBackends = MaxBackends;
	param->num_pmchild_slots = num_pmchild_slots;

#ifdef WIN32
	param->PostmasterHandle = PostmasterHandle;
	if (!write_duplicated_handle(&param->initial_signal_pipe,
								 pgwin32_create_signal_listener(childPid),
								 childProcess))
		return false;
#else
	memcpy(&param->postmaster_alive_fds, &postmaster_alive_fds,
		   sizeof(postmaster_alive_fds));
#endif

	memcpy(&param->syslogPipe, &syslogPipe, sizeof(syslogPipe));

	strlcpy(param->my_exec_path, my_exec_path, MAXPGPATH);

	strlcpy(param->pkglib_path, pkglib_path, MAXPGPATH);

	param->startup_data_len = startup_data_len;
	if (startup_data_len > 0)
		memcpy(param->startup_data, startup_data, startup_data_len);

	return true;
}

#ifdef WIN32
/*
 * Duplicate a handle for usage in a child process, and write the child
 * process instance of the handle to the parameter file.
 * 复制一个句柄供子进程使用，并将该句柄在子进程中的实例写入参数文件。
 */
static bool
write_duplicated_handle(HANDLE *dest, HANDLE src, HANDLE childProcess)
{
	HANDLE		hChild = INVALID_HANDLE_VALUE;

	if (!DuplicateHandle(GetCurrentProcess(),
						 src,
						 childProcess,
						 &hChild,
						 0,
						 TRUE,
						 DUPLICATE_CLOSE_SOURCE | DUPLICATE_SAME_ACCESS))
	{
		ereport(LOG,
				(errmsg_internal("could not duplicate handle to be written to backend parameter file: error code %lu",
								 GetLastError())));
		return false;
	}

	*dest = hChild;
	return true;
}

/*
 * Duplicate a socket for usage in a child process, and write the resulting
 * structure to the parameter file.
 * This is required because a number of LSPs (Layered Service Providers) very
 * common on Windows (antivirus, firewalls, download managers etc) break
 * straight socket inheritance.
 * 复制套接字以供子进程使用，并将生成的结构写入参数文件。
 * 这是必需的，因为 Windows 上许多非常常见的 LSP（分层服务提供程序，如杀毒软件、
 * 防火墙、下载管理器等）会破坏直接的套接字继承。
 */
static bool
write_inheritable_socket(InheritableSocket *dest, SOCKET src, pid_t childpid)
{
	dest->origsocket = src;
	if (src != 0 && src != PGINVALID_SOCKET)
	{
		/* Actual socket */
		/* 实际套接字 */
		if (WSADuplicateSocket(src, childpid, &dest->wsainfo) != 0)
		{
			ereport(LOG,
					(errmsg("could not duplicate socket %d for use in backend: error code %d",
							(int) src, WSAGetLastError())));
			return false;
		}
	}
	return true;
}

/*
 * Read a duplicate socket structure back, and get the socket descriptor.
 * 读回复制的套接字结构，并获取套接字描述符。
 */
static void
read_inheritable_socket(SOCKET *dest, InheritableSocket *src)
{
	SOCKET		s;

	if (src->origsocket == PGINVALID_SOCKET || src->origsocket == 0)
	{
		/* Not a real socket! */
		/* 不是真实的套接字！ */
		*dest = src->origsocket;
	}
	else
	{
		/* Actual socket, so create from structure */
		/* 真实的套接字，因此从结构体中创建 */
		s = WSASocket(FROM_PROTOCOL_INFO,
					  FROM_PROTOCOL_INFO,
					  FROM_PROTOCOL_INFO,
					  &src->wsainfo,
					  0,
					  0);
		if (s == INVALID_SOCKET)
		{
			write_stderr("could not create inherited socket: error code %d\n",
						 WSAGetLastError());
			exit(1);
		}
		*dest = s;

		/*
		 * To make sure we don't get two references to the same socket, close
		 * the original one. (This would happen when inheritance actually
		 * works..
		 * 为了确保我们不会获得指向同一套接字的两个引用，请关闭原始套接字。
		 * （这会在继承实际上工作时发生..）
		 */
		closesocket(src->origsocket);
	}
}
#endif

static void
read_backend_variables(char *id, void **startup_data, size_t *startup_data_len)
{
	BackendParameters param;

#ifndef WIN32
	/* Non-win32 implementation reads from file */
	/* 非 win32 实现从文件读取 */
	FILE	   *fp;

	/* Open file */
	/* 打开文件 */
	fp = AllocateFile(id, PG_BINARY_R);
	if (!fp)
	{
		write_stderr("could not open backend variables file \"%s\": %m\n", id);
		exit(1);
	}

	if (fread(&param, sizeof(param), 1, fp) != 1)
	{
		write_stderr("could not read from backend variables file \"%s\": %m\n", id);
		exit(1);
	}

	/* read startup data */
	/* 读取启动数据 */
	*startup_data_len = param.startup_data_len;
	if (param.startup_data_len > 0)
	{
		*startup_data = palloc(*startup_data_len);
		if (fread(*startup_data, *startup_data_len, 1, fp) != 1)
		{
			write_stderr("could not read startup data from backend variables file \"%s\": %m\n",
						 id);
			exit(1);
		}
	}
	else
		*startup_data = NULL;

	/* Release file */
	/* 释放文件 */
	FreeFile(fp);
	if (unlink(id) != 0)
	{
		write_stderr("could not remove file \"%s\": %m\n", id);
		exit(1);
	}
#else
	/* Win32 version uses mapped file */
	/* Win32 版本使用映射文件 */
	HANDLE		paramHandle;
	BackendParameters *paramp;

#ifdef _WIN64
	paramHandle = (HANDLE) _atoi64(id);
#else
	paramHandle = (HANDLE) atol(id);
#endif
	paramp = MapViewOfFile(paramHandle, FILE_MAP_READ, 0, 0, 0);
	if (!paramp)
	{
		write_stderr("could not map view of backend variables: error code %lu\n",
					 GetLastError());
		exit(1);
	}

	memcpy(&param, paramp, sizeof(BackendParameters));

	/* read startup data */
	/* 读取启动数据 */
	*startup_data_len = param.startup_data_len;
	if (param.startup_data_len > 0)
	{
		*startup_data = palloc(paramp->startup_data_len);
		memcpy(*startup_data, paramp->startup_data, param.startup_data_len);
	}
	else
		*startup_data = NULL;

	if (!UnmapViewOfFile(paramp))
	{
		write_stderr("could not unmap view of backend variables: error code %lu\n",
					 GetLastError());
		exit(1);
	}

	if (!CloseHandle(paramHandle))
	{
		write_stderr("could not close handle to backend parameter variables: error code %lu\n",
					 GetLastError());
		exit(1);
	}
#endif

	restore_backend_variables(&param);
}

/* Restore critical backend variables from the BackendParameters struct */
/* 从 BackendParameters 结构恢复关键后端变量 */
static void
restore_backend_variables(BackendParameters *param)
{
	if (param->client_sock.sock != PGINVALID_SOCKET)
	{
		MyClientSocket = MemoryContextAlloc(TopMemoryContext, sizeof(ClientSocket));
		memcpy(MyClientSocket, &param->client_sock, sizeof(ClientSocket));
		read_inheritable_socket(&MyClientSocket->sock, &param->inh_sock);
	}

	SetDataDir(param->DataDir);

	MyPMChildSlot = param->MyPMChildSlot;

#ifdef WIN32
	ShmemProtectiveRegion = param->ShmemProtectiveRegion;
#endif
	UsedShmemSegID = param->UsedShmemSegID;
	UsedShmemSegAddr = param->UsedShmemSegAddr;

	ShmemLock = param->ShmemLock;

#ifdef USE_INJECTION_POINTS
	ActiveInjectionPoints = param->ActiveInjectionPoints;
#endif

	NamedLWLockTrancheRequests = param->NamedLWLockTrancheRequests;
	NamedLWLockTrancheArray = param->NamedLWLockTrancheArray;
	MainLWLockArray = param->MainLWLockArray;
	ProcStructLock = param->ProcStructLock;
	ProcGlobal = param->ProcGlobal;
	AuxiliaryProcs = param->AuxiliaryProcs;
	PreparedXactProcs = param->PreparedXactProcs;
	PMSignalState = param->PMSignalState;
	ProcSignal = param->ProcSignal;

	PostmasterPid = param->PostmasterPid;
	PgStartTime = param->PgStartTime;
	PgReloadTime = param->PgReloadTime;
	first_syslogger_file_time = param->first_syslogger_file_time;

	redirection_done = param->redirection_done;
	IsBinaryUpgrade = param->IsBinaryUpgrade;
	query_id_enabled = param->query_id_enabled;
	max_safe_fds = param->max_safe_fds;

	MaxBackends = param->MaxBackends;
	num_pmchild_slots = param->num_pmchild_slots;

#ifdef WIN32
	PostmasterHandle = param->PostmasterHandle;
	pgwin32_initial_signal_pipe = param->initial_signal_pipe;
#else
	memcpy(&postmaster_alive_fds, &param->postmaster_alive_fds,
		   sizeof(postmaster_alive_fds));
#endif

	memcpy(&syslogPipe, &param->syslogPipe, sizeof(syslogPipe));

	strlcpy(my_exec_path, param->my_exec_path, MAXPGPATH);

	strlcpy(pkglib_path, param->pkglib_path, MAXPGPATH);

	/*
	 * We need to restore fd.c's counts of externally-opened FDs; to avoid
	 * confusion, be sure to do this after restoring max_safe_fds.  (Note:
	 * BackendInitialize will handle this for (*client_sock)->sock.)
	 * 我们需要恢复 fd.c 对外部打开的 FD 的计数；为避免混淆，
	 * 请务必在恢复 max_safe_fds 之后执行此操作。（注意：
	 * BackendInitialize 将为 (*client_sock)->sock 处理此操作。）
	 */
#ifndef WIN32
	if (postmaster_alive_fds[0] >= 0)
		ReserveExternalFD();
	if (postmaster_alive_fds[1] >= 0)
		ReserveExternalFD();
#endif
}

#endif							/* EXEC_BACKEND */

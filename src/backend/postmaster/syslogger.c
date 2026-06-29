/*-------------------------------------------------------------------------
 *
 * syslogger.c
 *
 * The system logger (syslogger) appeared in Postgres 8.0. It catches all
 * stderr output from the postmaster, backends, and other subprocesses
 * by redirecting to a pipe, and writes it to a set of logfiles.
 * It's possible to have size and age limits for the logfile configured
 * in postgresql.conf. If these limits are reached or passed, the
 * current logfile is closed and a new one is created (rotated).
 * The logfiles are stored in a subdirectory (configurable in
 * postgresql.conf), using a user-selectable naming scheme.
 * 系统日志进程（syslogger）自 Postgres 8.0 起引入。它通过管道重定向，
 * 捕获来自 postmaster、后端和其他子进程的所有 stderr（标准错误输出），
 * 并将其写入一组日志文件中。可以通过 postgresql.conf 配置日志文件的大小
 * 和保留时长限制。如果达到了这些限制，则关闭当前日志文件并创建新日志文件（轮转）。
 * 日志文件存储在子目录中（可在 postgresql.conf 中配置），采用用户可选的命名方案。
 *
 * Author: Andreas Pflug <pgadmin@pse-consulting.de>
 *
 * Copyright (c) 2004-2025, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/syslogger.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "common/file_perm.h"
#include "lib/stringinfo.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "pgstat.h"
#include "pgtime.h"
#include "port/pg_bitutils.h"
#include "postmaster/interrupt.h"
#include "postmaster/postmaster.h"
#include "postmaster/syslogger.h"
#include "storage/dsm.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/pg_shmem.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"

/*
 * We read() into a temp buffer twice as big as a chunk, so that any fragment
 * left after processing can be moved down to the front and we'll still have
 * room to read a full chunk.
 */
/*
 * 我们将读取的数据存入两倍于 chunk 大小的临时缓冲区中，这样在处理完后，
 * 剩余的任何碎片都可以移到最前面，而我们仍然有空间读取一个完整的 chunk。
 */
#define READ_BUF_SIZE (2 * PIPE_CHUNK_SIZE)

/* Log rotation signal file path, relative to $PGDATA */
/* 日志轮转信号文件路径，相对于 $PGDATA */
#define LOGROTATE_SIGNAL_FILE	"logrotate"


/*
 * GUC parameters.  Logging_collector cannot be changed after postmaster
 * start, but the rest can change at SIGHUP.
 */
/*
 * GUC 参数。Logging_collector 在 postmaster 启动后不能修改，
 * 但其余参数可以在 SIGHUP 时更改。
 */
bool		Logging_collector = false;
int			Log_RotationAge = HOURS_PER_DAY * MINS_PER_HOUR;
int			Log_RotationSize = 10 * 1024;
char	   *Log_directory = NULL;
char	   *Log_filename = NULL;
bool		Log_truncate_on_rotation = false;
int			Log_file_mode = S_IRUSR | S_IWUSR;

/*
 * Private state
 * 私有状态
 */
static pg_time_t next_rotation_time;
static bool pipe_eof_seen = false;
static bool rotation_disabled = false;
static FILE *syslogFile = NULL;
static FILE *csvlogFile = NULL;
static FILE *jsonlogFile = NULL;
NON_EXEC_STATIC pg_time_t first_syslogger_file_time = 0;
static char *last_sys_file_name = NULL;
static char *last_csv_file_name = NULL;
static char *last_json_file_name = NULL;

/*
 * Buffers for saving partial messages from different backends.
 *
 * Keep NBUFFER_LISTS lists of these, with the entry for a given source pid
 * being in the list numbered (pid % NBUFFER_LISTS), so as to cut down on
 * the number of entries we have to examine for any one incoming message.
 * There must never be more than one entry for the same source pid.
 *
 * An inactive buffer is not removed from its list, just held for re-use.
 * An inactive buffer has pid == 0 and undefined contents of data.
 */
/*
 * 用于保存来自不同后端的局部（未完成）消息的缓冲区。
 *
 * 为此维护 NBUFFER_LISTS 个列表，其中给定源 pid 的条目位于编号为 (pid % NBUFFER_LISTS) 
 * 的列表中，以减少我们针对任一传入消息必须检查的条目数量。
 * 同一个源 pid 绝不能有多个条目。
 *
 * 非活动的缓冲区不会从其列表中删除，只是保留以便重用。
 * 非活动缓冲区的 pid == 0 且数据内容未定义。
 */
typedef struct
{
	int32		pid;			/* PID of source process */
								/* 源进程的 PID */
	StringInfoData data;		/* accumulated data, as a StringInfo */
								/* 累积的数据，作为 StringInfo */
} save_buffer;

#define NBUFFER_LISTS 256
static List *buffer_lists[NBUFFER_LISTS];

/* These must be exported for EXEC_BACKEND case ... annoying */
/* 这些必须为 EXEC_BACKEND 情况导出……令人烦恼 */
#ifndef WIN32
int			syslogPipe[2] = {-1, -1};
#else
HANDLE		syslogPipe[2] = {0, 0};
#endif

#ifdef WIN32
static HANDLE threadHandle = 0;
static CRITICAL_SECTION sysloggerSection;
#endif

/*
 * Flags set by interrupt handlers for later service in the main loop.
 */
/*
 * 中断处理程序设置的标志，以便稍后在主循环中进行服务。
 */
static volatile sig_atomic_t rotation_requested = false;


/* Local subroutines */
/* 本地子程序 */
#ifdef EXEC_BACKEND
static int	syslogger_fdget(FILE *file);
static FILE *syslogger_fdopen(int fd);
#endif
static void process_pipe_input(char *logbuffer, int *bytes_in_logbuffer);
static void flush_pipe_input(char *logbuffer, int *bytes_in_logbuffer);
static FILE *logfile_open(const char *filename, const char *mode,
						  bool allow_errors);

#ifdef WIN32
static unsigned int __stdcall pipeThread(void *arg);
#endif
static void logfile_rotate(bool time_based_rotation, int size_rotation_for);
static bool logfile_rotate_dest(bool time_based_rotation,
								int size_rotation_for, pg_time_t fntime,
								int target_dest, char **last_file_name,
								FILE **logFile);
static char *logfile_getname(pg_time_t timestamp, const char *suffix);
static void set_next_rotation_time(void);
static void sigUsr1Handler(SIGNAL_ARGS);
static void update_metainfo_datafile(void);

typedef struct
{
	int			syslogFile;
	int			csvlogFile;
	int			jsonlogFile;
} SysloggerStartupData;

/*
 * Main entry point for syslogger process
 * argc/argv parameters are valid only in EXEC_BACKEND case.
 */
/*
 * syslogger 进程的主入口点
 * argc/argv 参数仅在 EXEC_BACKEND 情况下有效。
 *
 * Function purpose: Main entry loop of SysLogger.
 * 函数作用：系统日志收集进程的主循环。
 *
 * Core workflow:
 * 核心流程：
 * 1. Initialize process type as B_LOGGER and setup stderr redirection to DEVNULL to avoid loop.
 *    初始化进程为 B_LOGGER 并重定向 stderr 到 /dev/null（避免自循环）。
 * 2. Setup SIGHUP, SIGUSR1 (rotation signal) handlers.
 *    设置 SIGHUP、SIGUSR1（日志轮转）处理程序。
 * 3. Precompute next rotation time and update metainfo.
 *    计算下一次轮转时间并更新元信息。
 * 4. Enter loop: Process reload config, check timeouts/file size limits, perform logfile_rotate().
 *    进入主循环：处理重配置，检测超时和文件大小并执行日志轮转。
 * 5. Call WaitEventSetWait/read to extract messages from syslogPipe, and call process_pipe_input().
 *    通过 WaitEventSetWait 阻塞从 pipe 读取数据，调用 process_pipe_input 处理日志重组并写入。
 */
void
SysLoggerMain(const void *startup_data, size_t startup_data_len)
{
#ifndef WIN32
	char		logbuffer[READ_BUF_SIZE];
	int			bytes_in_logbuffer = 0;
#endif
	char	   *currentLogDir;
	char	   *currentLogFilename;
	int			currentLogRotationAge;
	pg_time_t	now;
	WaitEventSet *wes;

	/*
	 * Re-open the error output files that were opened by SysLogger_Start().
	 *
	 * We expect this will always succeed, which is too optimistic, but if it
	 * fails there's not a lot we can do to report the problem anyway.  As
	 * coded, we'll just crash on a null pointer dereference after failure...
	 */
	/*
	 * 重新打开由 SysLogger_Start() 打开的的错误输出文件。
	 *
	 * 我们希望这总是成功，这过于乐观，但如果它失败了，我们也无能为力去报告问题。
	 * 按照编写的代码，我们将在失败后由于空指针解引用而崩溃……
	 */
#ifdef EXEC_BACKEND
	{
		const SysloggerStartupData *slsdata = startup_data;

		Assert(startup_data_len == sizeof(*slsdata));
		syslogFile = syslogger_fdopen(slsdata->syslogFile);
		csvlogFile = syslogger_fdopen(slsdata->csvlogFile);
		jsonlogFile = syslogger_fdopen(slsdata->jsonlogFile);
	}
#else
	Assert(startup_data_len == 0);
#endif

	/*
	 * Now that we're done reading the startup data, release postmaster's
	 * working memory context.
	 */
	/*
	 * 既然我们已经读完了启动数据，请释放 postmaster 的工作内存上下文。
	 */
	if (PostmasterContext)
	{
		MemoryContextDelete(PostmasterContext);
		PostmasterContext = NULL;
	}

	now = MyStartTime;

	MyBackendType = B_LOGGER;
	init_ps_display(NULL);

	/*
	 * If we restarted, our stderr is already redirected into our own input
	 * pipe.  This is of course pretty useless, not to mention that it
	 * interferes with detecting pipe EOF.  Point stderr to /dev/null. This
	 * assumes that all interesting messages generated in the syslogger will
	 * come through elog.c and will be sent to write_syslogger_file.
	 */
	/*
	 * 如果我们重启，我们的 stderr 已经重定向到我们自己的输入管道中。
	 * 这当然相当无用，更不用说它会干扰对管道 EOF 的检测。将 stderr 指向 /dev/null。
	 * 这假定在 syslogger 中生成的有价值的消息都将通过 elog.c 并发送到 write_syslogger_file。
	 */
	if (redirection_done)
	{
		int			fd = open(DEVNULL, O_WRONLY, 0);

		/*
		 * The closes might look redundant, but they are not: we want to be
		 * darn sure the pipe gets closed even if the open failed.  We can
		 * survive running with stderr pointing nowhere, but we can't afford
		 * to have extra pipe input descriptors hanging around.
		 *
		 * As we're just trying to reset these to go to DEVNULL, there's not
		 * much point in checking for failure from the close/dup2 calls here,
		 * if they fail then presumably the file descriptors are closed and
		 * any writes will go into the bitbucket anyway.
		 */
		/*
		 * 关闭可能看起来多余，但事实并非如此：我们希望确保即使打开失败，管道也会被关闭。
		 * 我们可以允许 stderr 指向虚无，但我们无法承受多余的管道输入描述符挂在周围。
		 *
		 * 因为我们只是试图重置这些以去向 DEVNULL，所以在此处检查 close/dup2 调用是否失败没有太大意义，
		 * 如果它们失败，大概文件描述符是关闭的，并且任何写入无论如何都会进入黑洞。
		 */
		close(STDOUT_FILENO);
		close(STDERR_FILENO);
		if (fd != -1)
		{
			(void) dup2(fd, STDOUT_FILENO);
			(void) dup2(fd, STDERR_FILENO);
			close(fd);
		}
	}

	/*
	 * Syslogger's own stderr can't be the syslogPipe, so set it back to text
	 * mode if we didn't just close it. (It was set to binary in
	 * SubPostmasterMain).
	 */
	/*
	 * Syslogger 本身的 stderr 不能是 syslogPipe，所以如果没关闭它，将其重新设置回文本模式。
	 * （它在 SubPostmasterMain 中被设置为二进制模式）。
	 */
#ifdef WIN32
	else
		_setmode(STDERR_FILENO, _O_TEXT);
#endif

	/*
	 * Also close our copy of the write end of the pipe.  This is needed to
	 * ensure we can detect pipe EOF correctly.  (But note that in the restart
	 * case, the postmaster already did this.)
	 */
	/*
	 * 还要关闭我们持有的管道写端副本。这对于确保我们能正确检测到管道 EOF 是必需的。
	 * （但注意，在重启的情况下，postmaster 已经执行了此操作。）
	 */
#ifndef WIN32
	if (syslogPipe[1] >= 0)
		close(syslogPipe[1]);
	syslogPipe[1] = -1;
#else
	if (syslogPipe[1])
		CloseHandle(syslogPipe[1]);
	syslogPipe[1] = 0;
#endif

	/*
	 * Properly accept or ignore signals the postmaster might send us
	 *
	 * Note: we ignore all termination signals, and instead exit only when all
	 * upstream processes are gone, to ensure we don't miss any dying gasps of
	 * broken backends...
	 */
	/*
	 * 正确接受或忽略 postmaster 可能发送给我们的信号
	 *
	 * 注意：我们忽略所有终止信号，而仅在所有上游进程都消失时才退出，
	 * 以确保我们不会错过损坏的后端的临终悲鸣……
	 */

	pqsignal(SIGHUP, SignalHandlerForConfigReload); /* set flag to read config
													 * file */
													/* 设置读取配置文件的标志 */
	pqsignal(SIGINT, SIG_IGN);
	pqsignal(SIGTERM, SIG_IGN);
	pqsignal(SIGQUIT, SIG_IGN);
	pqsignal(SIGALRM, SIG_IGN);
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, sigUsr1Handler);	/* request log rotation */ /* 请求日志轮转 */
	pqsignal(SIGUSR2, SIG_IGN);

	/*
	 * Reset some signals that are accepted by postmaster but not here
	 */
	/*
	 * 重置一些由 postmaster 接受但在此处不接受的信号
	 */
	pqsignal(SIGCHLD, SIG_DFL);

	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

#ifdef WIN32
	/* Fire up separate data transfer thread */
	/* 启动单独的数据传输线程 */
	InitializeCriticalSection(&sysloggerSection);
	EnterCriticalSection(&sysloggerSection);

	threadHandle = (HANDLE) _beginthreadex(NULL, 0, pipeThread, NULL, 0, NULL);
	if (threadHandle == 0)
		elog(FATAL, "could not create syslogger data transfer thread: %m");
#endif							/* WIN32 */

	/*
	 * Remember active logfiles' name(s).  We recompute 'em from the reference
	 * time because passing down just the pg_time_t is a lot cheaper than
	 * passing a whole file path in the EXEC_BACKEND case.
	 */
	/*
	 * 记住活动日志文件的名称。我们根据参考时间重新计算它们，因为在 EXEC_BACKEND 
	 * 情况下，只传递 pg_time_t 要比传递整个文件路径便宜得多。
	 */
	last_sys_file_name = logfile_getname(first_syslogger_file_time, NULL);
	if (csvlogFile != NULL)
		last_csv_file_name = logfile_getname(first_syslogger_file_time, ".csv");
	if (jsonlogFile != NULL)
		last_json_file_name = logfile_getname(first_syslogger_file_time, ".json");

	/* remember active logfile parameters */
	/* 记住活动日志文件参数 */
	currentLogDir = pstrdup(Log_directory);
	currentLogFilename = pstrdup(Log_filename);
	currentLogRotationAge = Log_RotationAge;
	/* set next planned rotation time */
	/* 设置下一次计划轮转时间 */
	set_next_rotation_time();
	update_metainfo_datafile();

	/*
	 * Reset whereToSendOutput, as the postmaster will do (but hasn't yet, at
	 * the point where we forked).  This prevents duplicate output of messages
	 * from syslogger itself.
	 */
	/*
	 * 重置 whereToSendOutput，正如 postmaster 将要做的那样（但在我们 fork 时还没有做）。
	 * 这可以防止 syslogger 本身消息的重复输出。
	 */
	whereToSendOutput = DestNone;

	/*
	 * Set up a reusable WaitEventSet object we'll use to wait for our latch,
	 * and (except on Windows) our socket.
	 *
	 * Unlike all other postmaster child processes, we'll ignore postmaster
	 * death because we want to collect final log output from all backends and
	 * then exit last.  We'll do that by running until we see EOF on the
	 * syslog pipe, which implies that all other backends have exited
	 * (including the postmaster).
	 */
	/*
	 * 设置一个可重用的 WaitEventSet 对象，我们将使用它来等待我们的锁存器，以及（除 Windows 外）我们的套接字。
	 *
	 * 与所有其他 postmaster 子进程不同，我们将忽略 postmaster 的死亡，因为我们希望收集
	 * 所有后端的最终日志输出，然后最后一个退出。我们将通过运行直到在 syslog 管道上看到 
	 * EOF 来做到这一点，这暗示着所有其他后端都已退出（包括 postmaster）。
	 */
	wes = CreateWaitEventSet(NULL, 2);
	AddWaitEventToSet(wes, WL_LATCH_SET, PGINVALID_SOCKET, MyLatch, NULL);
#ifndef WIN32
	AddWaitEventToSet(wes, WL_SOCKET_READABLE, syslogPipe[0], NULL, NULL);
#endif

	/* main worker loop */
	/* 主工作循环 */
	for (;;)
	{
		bool		time_based_rotation = false;
		int			size_rotation_for = 0;
		long		cur_timeout;
		WaitEvent	event;

#ifndef WIN32
		int			rc;
#endif

		/* Clear any already-pending wakeups */
		/* 清除任何已挂起的唤醒信号 */
		ResetLatch(MyLatch);

		/*
		 * Process any requests or signals received recently.
		 */
		/*
		 * 处理最近收到的任何请求或信号。
		 */
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);

			/*
			 * Check if the log directory or filename pattern changed in
			 * postgresql.conf. If so, force rotation to make sure we're
			 * writing the logfiles in the right place.
			 */
			/*
			 * 检查 postgresql.conf 中的日志目录或文件名模式是否已更改。如果是，
			 * 强制轮转以确保我们将日志文件写入正确的位置。
			 */
			if (strcmp(Log_directory, currentLogDir) != 0)
			{
				pfree(currentLogDir);
				currentLogDir = pstrdup(Log_directory);
				rotation_requested = true;

				/*
				 * Also, create new directory if not present; ignore errors
				 */
				/*
				 * 此外，如果不存在，则创建新目录；忽略错误
				 */
				(void) MakePGDirectory(Log_directory);
			}
			if (strcmp(Log_filename, currentLogFilename) != 0)
			{
				pfree(currentLogFilename);
				currentLogFilename = pstrdup(Log_filename);
				rotation_requested = true;
			}

			/*
			 * Force a rotation if CSVLOG output was just turned on or off and
			 * we need to open or close csvlogFile accordingly.
			 */
			/*
			 * 如果刚开启或关闭了 CSVLOG 输出且我们需要打开或关闭 csvlogFile，
			 * 则强制进行轮转。
			 */
			if (((Log_destination & LOG_DESTINATION_CSVLOG) != 0) !=
				(csvlogFile != NULL))
				rotation_requested = true;

			/*
			 * Force a rotation if JSONLOG output was just turned on or off
			 * and we need to open or close jsonlogFile accordingly.
			 */
			/*
			 * 如果刚开启或关闭了 JSONLOG 输出且我们需要打开或关闭 jsonlogFile，
			 * 则强制进行轮转。
			 */
			if (((Log_destination & LOG_DESTINATION_JSONLOG) != 0) !=
				(jsonlogFile != NULL))
				rotation_requested = true;

			/*
			 * If rotation time parameter changed, reset next rotation time,
			 * but don't immediately force a rotation.
			 */
			/*
			 * 如果轮转时间参数发生更改，请重置下一次轮转时间，但不要立即强制轮转。
			 */
			if (currentLogRotationAge != Log_RotationAge)
			{
				currentLogRotationAge = Log_RotationAge;
				set_next_rotation_time();
			}

			/*
			 * If we had a rotation-disabling failure, re-enable rotation
			 * attempts after SIGHUP, and force one immediately.
			 */
			/*
			 * 如果我们遇到了导致轮转禁用的失败，在 SIGHUP 后重新启用轮转尝试，并立即强制执行一次。
			 */
			if (rotation_disabled)
			{
				rotation_disabled = false;
				rotation_requested = true;
			}

			/*
			 * Force rewriting last log filename when reloading configuration.
			 * Even if rotation_requested is false, log_destination may have
			 * been changed and we don't want to wait the next file rotation.
			 */
			/*
			 * 重新加载配置时强制重写上一个日志文件名。即使 rotation_requested 为 false，
			 * log_destination 可能也发生了更改，我们不想等待下一次文件轮转。
			 */
			update_metainfo_datafile();
		}

		if (Log_RotationAge > 0 && !rotation_disabled)
		{
			/* Do a logfile rotation if it's time */
			/* 如果时间到了，进行日志文件轮转 */
			now = (pg_time_t) time(NULL);
			if (now >= next_rotation_time)
				rotation_requested = time_based_rotation = true;
		}

		if (!rotation_requested && Log_RotationSize > 0 && !rotation_disabled)
		{
			/* Do a rotation if file is too big */
			/* 如果文件太大，则进行轮转 */
			if (ftello(syslogFile) >= Log_RotationSize * (pgoff_t) 1024)
			{
				rotation_requested = true;
				size_rotation_for |= LOG_DESTINATION_STDERR;
			}
			if (csvlogFile != NULL &&
				ftello(csvlogFile) >= Log_RotationSize * (pgoff_t) 1024)
			{
				rotation_requested = true;
				size_rotation_for |= LOG_DESTINATION_CSVLOG;
			}
			if (jsonlogFile != NULL &&
				ftello(jsonlogFile) >= Log_RotationSize * (pgoff_t) 1024)
			{
				rotation_requested = true;
				size_rotation_for |= LOG_DESTINATION_JSONLOG;
			}
		}

		if (rotation_requested)
		{
			/*
			 * Force rotation when both values are zero. It means the request
			 * was sent by pg_rotate_logfile() or "pg_ctl logrotate".
			 */
			/*
			 * 当两个值都为零时强制轮转。这意味着该请求由 pg_rotate_logfile() 
			 * 或 “pg_ctl logrotate” 发送。
			 */
			if (!time_based_rotation && size_rotation_for == 0)
				size_rotation_for = LOG_DESTINATION_STDERR |
					LOG_DESTINATION_CSVLOG |
					LOG_DESTINATION_JSONLOG;
			logfile_rotate(time_based_rotation, size_rotation_for);
		}

		/*
		 * Calculate time till next time-based rotation, so that we don't
		 * sleep longer than that.  We assume the value of "now" obtained
		 * above is still close enough.  Note we can't make this calculation
		 * until after calling logfile_rotate(), since it will advance
		 * next_rotation_time.
		 *
		 * Also note that we need to beware of overflow in calculation of the
		 * timeout: with large settings of Log_RotationAge, next_rotation_time
		 * could be more than INT_MAX msec in the future.  In that case we'll
		 * wait no more than INT_MAX msec, and try again.
		 */
		/*
		 * 计算到下一次基于时间的轮转之间的时间，以便我们睡眠不超过该时间。
		 * 我们假设上面获得的 “now” 值仍然足够接近。注意，在调用 logfile_rotate() 
		 * 之前，我们无法进行此计算，因为它将使 next_rotation_time 提前。
		 *
		 * 还请注意，在计算超时值时我们需要谨防溢出：在 Log_RotationAge 设置很大时，
		 * next_rotation_time 可能会超过未来 INT_MAX 毫秒。在这种情况下，我们将
		 * 等待不超过 INT_MAX 毫秒，然后重试。
		 */
		if (Log_RotationAge > 0 && !rotation_disabled)
		{
			pg_time_t	delay;

			delay = next_rotation_time - now;
			if (delay > 0)
			{
				if (delay > INT_MAX / 1000)
					delay = INT_MAX / 1000;
				cur_timeout = delay * 1000L;	/* msec */
			}
			else
				cur_timeout = 0;
		}
		else
			cur_timeout = -1L;

		/*
		 * Sleep until there's something to do
		 */
		/*
		 * 睡眠直到有事情要做
		 */
#ifndef WIN32
		rc = WaitEventSetWait(wes, cur_timeout, &event, 1,
							  WAIT_EVENT_SYSLOGGER_MAIN);

		if (rc == 1 && event.events == WL_SOCKET_READABLE)
		{
			int			bytesRead;

			bytesRead = read(syslogPipe[0],
							 logbuffer + bytes_in_logbuffer,
							 sizeof(logbuffer) - bytes_in_logbuffer);
			if (bytesRead < 0)
			{
				if (errno != EINTR)
					ereport(LOG,
							(errcode_for_socket_access(),
							 errmsg("could not read from logger pipe: %m")));
			}
			else if (bytesRead > 0)
			{
				bytes_in_logbuffer += bytesRead;
				process_pipe_input(logbuffer, &bytes_in_logbuffer);
				continue;
			}
			else
			{
				/*
				 * Zero bytes read when select() is saying read-ready means
				 * EOF on the pipe: that is, there are no longer any processes
				 * with the pipe write end open.  Therefore, the postmaster
				 * and all backends are shut down, and we are done.
				 */
				/*
				 * 当 select() 表示可读但读取到零字节时，意味着管道上出现 EOF：
				 * 也就是说，不再有进程保持管道写端打开。因此，postmaster 
				 * 和所有后端均已关闭，我们已完成任务。
				 */
				pipe_eof_seen = true;

				/* if there's any data left then force it out now */
				/* 如果还有任何数据留下，现在强制将其输出 */
				flush_pipe_input(logbuffer, &bytes_in_logbuffer);
			}
		}
#else							/* WIN32 */

		/*
		 * On Windows we leave it to a separate thread to transfer data and
		 * detect pipe EOF.  The main thread just wakes up to handle SIGHUP
		 * and rotation conditions.
		 *
		 * Server code isn't generally thread-safe, so we ensure that only one
		 * of the threads is active at a time by entering the critical section
		 * whenever we're not sleeping.
		 */
		/*
		 * 在 Windows 上，我们留给单独的线程来传输数据并检测管道 EOF。
		 * 主线程只是醒来处理 SIGHUP 和轮转条件。
		 *
		 * 服务器代码通常不是线程安全的，因此我们通过在不睡眠时进入临界区，
		 * 确保一次只有一个线程处于活动状态。
		 */
		LeaveCriticalSection(&sysloggerSection);

		(void) WaitEventSetWait(wes, cur_timeout, &event, 1,
								WAIT_EVENT_SYSLOGGER_MAIN);

		EnterCriticalSection(&sysloggerSection);
#endif							/* WIN32 */

		if (pipe_eof_seen)
		{
			/*
			 * seeing this message on the real stderr is annoying - so we make
			 * it DEBUG1 to suppress in normal use.
			 */
			/*
			 * 在真实的 stderr 上看到此消息很烦人 - 因此我们将其设置为 DEBUG1，以在正常使用中进行抑制。
			 */
			ereport(DEBUG1,
					(errmsg_internal("logger shutting down")));

			/*
			 * Normal exit from the syslogger is here.  Note that we
			 * deliberately do not close syslogFile before exiting; this is to
			 * allow for the possibility of elog messages being generated
			 * inside proc_exit.  Regular exit() will take care of flushing
			 * and closing stdio channels.
			 */
			/*
			 * syslogger 的正常退出就在这里。请注意，我们在退出前特意不关闭 syslogFile；
			 * 这是为了允许在 proc_exit 内部生成 elog 消息的可能性。
			 * 常规 exit() 将负责刷新和关闭 stdio 通道。
			 */
			proc_exit(0);
		}
	}
}

/*
 * Postmaster subroutine to start a syslogger subprocess.
 */
/*
 * Postmaster 子程序，用于启动 syslogger 子进程。
 *
 * Function purpose: Setup logs directory/pipes and launch syslogger backend.
 * 函数作用：由 postmaster 调用以创建日志目录/管道并派生日志收集子进程。
 */
int
SysLogger_Start(int child_slot)
{
	pid_t		sysloggerPid;
	char	   *filename;
#ifdef EXEC_BACKEND
	SysloggerStartupData startup_data;
#endif							/* EXEC_BACKEND */

	Assert(Logging_collector);

	/*
	 * If first time through, create the pipe which will receive stderr
	 * output.
	 *
	 * If the syslogger crashes and needs to be restarted, we continue to use
	 * the same pipe (indeed must do so, since extant backends will be writing
	 * into that pipe).
	 *
	 * This means the postmaster must continue to hold the read end of the
	 * pipe open, so we can pass it down to the reincarnated syslogger. This
	 * is a bit klugy but we have little choice.
	 *
	 * Also note that we don't bother counting the pipe FDs by calling
	 * Reserve/ReleaseExternalFD.  There's no real need to account for them
	 * accurately in the postmaster or syslogger process, and both ends of the
	 * pipe will wind up closed in all other postmaster children.
	 */
	/*
	 * 如果是第一次运行，则创建将接收 stderr 输出的管道。
	 *
	 * 如果 syslogger 崩溃并需要重启，我们继续使用相同的管道
	 * （确实必须这样做，因为现存的后端将写入该管道）。
	 *
	 * 这意味着 postmaster 必须继续保持管道读端打开，以便我们可以将其传递给
	 * 重生的 syslogger。这有点笨拙，但我们别无选择。
	 *
	 * 另请注意，我们不麻烦通过调用 Reserve/ReleaseExternalFD 来计算管道文件描述符（FD）。
	 * 没有实际必要在 postmaster 或 syslogger 进程中精确统计它们，并且管道的两端
	 * 在所有其他 postmaster 子进程中最终都会被关闭。
	 */
#ifndef WIN32
	if (syslogPipe[0] < 0)
	{
		if (pipe(syslogPipe) < 0)
			ereport(FATAL,
					(errcode_for_socket_access(),
					 errmsg("could not create pipe for syslog: %m")));
	}
#else
	if (!syslogPipe[0])
	{
		SECURITY_ATTRIBUTES sa;

		memset(&sa, 0, sizeof(SECURITY_ATTRIBUTES));
		sa.nLength = sizeof(SECURITY_ATTRIBUTES);
		sa.bInheritHandle = TRUE;

		if (!CreatePipe(&syslogPipe[0], &syslogPipe[1], &sa, 32768))
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not create pipe for syslog: %m")));
	}
#endif

	/*
	 * Create log directory if not present; ignore errors
	 */
	/*
	 * 如果不存在日志目录，则创建它；忽略错误
	 */
	(void) MakePGDirectory(Log_directory);

	/*
	 * The initial logfile is created right in the postmaster, to verify that
	 * the Log_directory is writable.  We save the reference time so that the
	 * syslogger child process can recompute this file name.
	 *
	 * It might look a bit strange to re-do this during a syslogger restart,
	 * but we must do so since the postmaster closed syslogFile after the
	 * previous fork (and remembering that old file wouldn't be right anyway).
	 * Note we always append here, we won't overwrite any existing file.  This
	 * is consistent with the normal rules, because by definition this is not
	 * a time-based rotation.
	 */
	/*
	 * 初始日志文件直接在 postmaster 中创建，以验证 Log_directory 是否可写。
	 * 我们保存参考时间，以便 syslogger 子进程可以重新计算此文件名。
	 *
	 * 在 syslogger 重启期间重新执行此操作可能看起来有点奇怪，但我们必须这样做，
	 * 因为 postmaster 在上一次 fork 之后关闭了 syslogFile（而且记住那个旧文件
	 * 无论如何都是不对的）。注意，我们在这里总是追加内容，不会覆盖任何现有文件。
	 * 这与正常规则一致，因为根据定义，这并不是基于时间的轮转。
	 */
	first_syslogger_file_time = time(NULL);

	filename = logfile_getname(first_syslogger_file_time, NULL);

	syslogFile = logfile_open(filename, "a", false);

	pfree(filename);

	/*
	 * Likewise for the initial CSV log file, if that's enabled.  (Note that
	 * we open syslogFile even when only CSV output is nominally enabled,
	 * since some code paths will write to syslogFile anyway.)
	 */
	/*
	 * 同样，如果启用了初始 CSV 日志文件，亦然。（请注意，即使名义上仅启用了 
	 * CSV 输出，我们也会打开 syslogFile，因为无论如何某些代码路径都会写入 syslogFile。）
	 */
	if (Log_destination & LOG_DESTINATION_CSVLOG)
	{
		filename = logfile_getname(first_syslogger_file_time, ".csv");

		csvlogFile = logfile_open(filename, "a", false);

		pfree(filename);
	}

	/*
	 * Likewise for the initial JSON log file, if that's enabled.  (Note that
	 * we open syslogFile even when only JSON output is nominally enabled,
	 * since some code paths will write to syslogFile anyway.)
	 */
	/*
	 * 同样，如果启用了初始 JSON 日志文件，亦然。（请注意，即使名义上仅启用了 
	 * JSON 输出，我们也会打开 syslogFile，因为无论如何某些代码路径都会写入 syslogFile。）
	 */
	if (Log_destination & LOG_DESTINATION_JSONLOG)
	{
		filename = logfile_getname(first_syslogger_file_time, ".json");

		jsonlogFile = logfile_open(filename, "a", false);

		pfree(filename);
	}

#ifdef EXEC_BACKEND
	startup_data.syslogFile = syslogger_fdget(syslogFile);
	startup_data.csvlogFile = syslogger_fdget(csvlogFile);
	startup_data.jsonlogFile = syslogger_fdget(jsonlogFile);
	sysloggerPid = postmaster_child_launch(B_LOGGER, child_slot,
										   &startup_data, sizeof(startup_data), NULL);
#else
	sysloggerPid = postmaster_child_launch(B_LOGGER, child_slot,
										   NULL, 0, NULL);
#endif							/* EXEC_BACKEND */

	if (sysloggerPid == -1)
	{
		ereport(LOG,
				(errmsg("could not fork system logger: %m")));
		return 0;
	}

	/* success, in postmaster */
	/* 成功，在 postmaster 中 */

	/* now we redirect stderr, if not done already */
	/* 现在我们重定向 stderr（如果尚未执行此操作） */
	if (!redirection_done)
	{
#ifdef WIN32
		int			fd;
#endif

		/*
		 * Leave a breadcrumb trail when redirecting, in case the user forgets
		 * that redirection is active and looks only at the original stderr
		 * target file.
		 */
		/*
		 * 重定向时留下痕迹，以防用户忘记重定向处于活动状态而仅查看原始 stderr 目标文件。
		 */
		ereport(LOG,
				(errmsg("redirecting log output to logging collector process"),
				 errhint("Future log output will appear in directory \"%s\".",
						 Log_directory)));

#ifndef WIN32
		fflush(stdout);
		if (dup2(syslogPipe[1], STDOUT_FILENO) < 0)
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not redirect stdout: %m")));
		fflush(stderr);
		if (dup2(syslogPipe[1], STDERR_FILENO) < 0)
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not redirect stderr: %m")));
		/* Now we are done with the write end of the pipe. */
		/* 现在我们完成了管道的写端。 */
		close(syslogPipe[1]);
		syslogPipe[1] = -1;
#else

		/*
		 * open the pipe in binary mode and make sure stderr is binary after
		 * it's been dup'ed into, to avoid disturbing the pipe chunking
		 * protocol.
		 */
		/*
		 * 在二进制模式下打开管道，并确保将 stderr 复制到其中之后也是二进制的，
		 * 以避免干扰管道分块协议。
		 */
		fflush(stderr);
		fd = _open_osfhandle((intptr_t) syslogPipe[1],
							 _O_APPEND | _O_BINARY);
		if (dup2(fd, STDERR_FILENO) < 0)
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not redirect stderr: %m")));
		close(fd);
		_setmode(STDERR_FILENO, _O_BINARY);

		/*
		 * Now we are done with the write end of the pipe.  CloseHandle() must
		 * not be called because the preceding close() closes the underlying
		 * handle.
		 */
		/*
		 * 现在我们完成了管道的写端。不得调用 CloseHandle()，因为先前的 close() 
		 * 会关闭底层句柄。
		 */
		syslogPipe[1] = 0;
#endif
		redirection_done = true;
	}

	/* postmaster will never write the file(s); close 'em */
	/* postmaster 永远不会写入文件；关闭它们 */
	fclose(syslogFile);
	syslogFile = NULL;
	if (csvlogFile != NULL)
	{
		fclose(csvlogFile);
		csvlogFile = NULL;
	}
	if (jsonlogFile != NULL)
	{
		fclose(jsonlogFile);
		jsonlogFile = NULL;
	}
	return (int) sysloggerPid;
}


#ifdef EXEC_BACKEND

/*
 * syslogger_fdget() -
 *
 * Utility wrapper to grab the file descriptor of an opened error output
 * file.  Used when building the command to fork the logging collector.
 *
 * Function purpose: Get file descriptor of a file wrapper.
 * 函数作用：获取已打开文件的文件描述符值。
 */
static int
syslogger_fdget(FILE *file)
{
#ifndef WIN32
	if (file != NULL)
		return fileno(file);
	else
		return -1;
#else
	if (file != NULL)
		return (int) _get_osfhandle(_fileno(file));
	else
		return 0;
#endif							/* WIN32 */
}

/*
 * syslogger_fdopen() -
 *
 * Utility wrapper to re-open an error output file, using the given file
 * descriptor.  Used when parsing arguments in a forked logging collector.
 *
 * Function purpose: Open file by descriptor.
 * 函数作用：通过描述符重新关联打开文件句柄。
 */
static FILE *
syslogger_fdopen(int fd)
{
	FILE	   *file = NULL;

#ifndef WIN32
	if (fd != -1)
	{
		file = fdopen(fd, "a");
		setvbuf(file, NULL, PG_IOLBF, 0);
	}
#else							/* WIN32 */
	if (fd != 0)
	{
		fd = _open_osfhandle(fd, _O_APPEND | _O_TEXT);
		if (fd > 0)
		{
			file = fdopen(fd, "a");
			setvbuf(file, NULL, PG_IOLBF, 0);
		}
	}
#endif							/* WIN32 */

	return file;
}
#endif							/* EXEC_BACKEND */


/* --------------------------------
 *		pipe protocol handling
 *		管道协议处理
 * --------------------------------
 */

/*
 * Process data received through the syslogger pipe.
 *
 * This routine interprets the log pipe protocol which sends log messages as
 * (hopefully atomic) chunks - such chunks are detected and reassembled here.
 *
 * The protocol has a header that starts with two nul bytes, then has a 16 bit
 * length, the pid of the sending process, and a flag to indicate if it is
 * the last chunk in a message. Incomplete chunks are saved until we read some
 * more, and non-final chunks are accumulated until we get the final chunk.
 *
 * All of this is to avoid 2 problems:
 * . partial messages being written to logfiles (messes rotation), and
 * . messages from different backends being interleaved (messages garbled).
 *
 * Any non-protocol messages are written out directly. These should only come
 * from non-PostgreSQL sources, however (e.g. third party libraries writing to
 * stderr).
 *
 * logbuffer is the data input buffer, and *bytes_in_logbuffer is the number
 * of bytes present.  On exit, any not-yet-eaten data is left-justified in
 * logbuffer, and *bytes_in_logbuffer is updated.
 */
/*
 * 处理通过 syslogger 管道接收的数据。
 *
 * 该例程解释了将日志消息作为（希望是原子的）块发送的日志管道协议 - 此处检测并重新装配此类块。
 *
 * 该协议有一个头，以两个空字节开始，然后有 16 位长度，发送进程的 pid，以及一个标志
 * 指示它是否是消息中的最后一个块。不完整的块将被保存，直到我们读取更多数据，
 * 并且非最终块会累积，直到我们获得最终块。
 *
 * 所有这一切都是为了避免 2 个问题：
 * 1. 局部消息被写入日志文件（使轮转混乱），以及
 * 2. 来自不同后端的消息交织在一起（消息混淆）。
 *
 * 任何非协议消息都会直接写出。然而，这些应该只来自非 PostgreSQL 源（例如写入 stderr 
 * 的第三方库）。
 *
 * logbuffer 是数据输入缓冲区，*bytes_in_logbuffer 是存在的字节数。
 * 退出时，任何尚未吃掉的数据在 logbuffer 中左对齐，并且更新 *bytes_in_logbuffer。
 *
 * Function purpose: Parse pipe protocols and write consolidated logs to file.
 * 函数作用：解析管道中的消息协议块，重组出完整日志并写入磁盘。
 *
 * Core workflow:
 * 核心流程：
 * 1. Loop through buffer to find valid PipeProtoHeader.
 *    在缓冲区中循环查找合法的 PipeProtoHeader。
 * 2. Verify payload size, identify target destination (stderr, csvlog, jsonlog).
 *    验证负载长度，识别出输出的目标通道。
 * 3. Buffer non-final chunks into per-pid save_buffer list.
 *    如果是非尾部块，将其缓存进以 pid 作为 Key 的缓存列表。
 * 4. Write final chunks and unbuffered single chunks immediately using write_syslogger_file().
 *    遇到尾部块或非协议裸行时，调用 write_syslogger_file 将拼接完成的数据刷盘。
 */
static void
process_pipe_input(char *logbuffer, int *bytes_in_logbuffer)
{
	char	   *cursor = logbuffer;
	int			count = *bytes_in_logbuffer;
	int			dest = LOG_DESTINATION_STDERR;

	/* While we have enough for a header, process data... */
	/* 当我们有足够的数据容纳一个头部时，处理数据…… */
	while (count >= (int) (offsetof(PipeProtoHeader, data) + 1))
	{
		PipeProtoHeader p;
		int			chunklen;
		bits8		dest_flags;

		/* Do we have a valid header? */
		/* 我们有一个有效的头部吗？ */
		memcpy(&p, cursor, offsetof(PipeProtoHeader, data));
		dest_flags = p.flags & (PIPE_PROTO_DEST_STDERR |
								PIPE_PROTO_DEST_CSVLOG |
								PIPE_PROTO_DEST_JSONLOG);
		if (p.nuls[0] == '\0' && p.nuls[1] == '\0' &&
			p.len > 0 && p.len <= PIPE_MAX_PAYLOAD &&
			p.pid != 0 &&
			pg_number_of_ones[dest_flags] == 1)
		{
			List	   *buffer_list;
			ListCell   *cell;
			save_buffer *existing_slot = NULL,
					   *free_slot = NULL;
			StringInfo	str;

			chunklen = PIPE_HEADER_SIZE + p.len;

			/* Fall out of loop if we don't have the whole chunk yet */
			/* 如果我们还没有完整的块，退出循环 */
			if (count < chunklen)
				break;

			if ((p.flags & PIPE_PROTO_DEST_STDERR) != 0)
				dest = LOG_DESTINATION_STDERR;
			else if ((p.flags & PIPE_PROTO_DEST_CSVLOG) != 0)
				dest = LOG_DESTINATION_CSVLOG;
			else if ((p.flags & PIPE_PROTO_DEST_JSONLOG) != 0)
				dest = LOG_DESTINATION_JSONLOG;
			else
			{
				/* this should never happen as of the header validation */
				/* 从头部验证来看，这应该永远不会发生 */
				Assert(false);
			}

			/* Locate any existing buffer for this source pid */
			/* 找到此源 pid 的任何现有缓冲区 */
			buffer_list = buffer_lists[p.pid % NBUFFER_LISTS];
			foreach(cell, buffer_list)
			{
				save_buffer *buf = (save_buffer *) lfirst(cell);

				if (buf->pid == p.pid)
				{
					existing_slot = buf;
					break;
				}
				if (buf->pid == 0 && free_slot == NULL)
					free_slot = buf;
			}

			if ((p.flags & PIPE_PROTO_IS_LAST) == 0)
			{
				/*
				 * Save a complete non-final chunk in a per-pid buffer
				 */
				/*
				 * 将完整的非最终块保存在每个 pid 的缓冲区中
				 */
				if (existing_slot != NULL)
				{
					/* Add chunk to data from preceding chunks */
					/* 将块添加到来自先前块的数据中 */
					str = &(existing_slot->data);
					appendBinaryStringInfo(str,
										   cursor + PIPE_HEADER_SIZE,
										   p.len);
				}
				else
				{
					/* First chunk of message, save in a new buffer */
					/* 消息的第一块，保存在新缓冲区中 */
					if (free_slot == NULL)
					{
						/*
						 * Need a free slot, but there isn't one in the list,
						 * so create a new one and extend the list with it.
						 */
						/*
						 * 需要一个空闲槽，但列表中没有，所以创建一个新槽并用它扩展列表。
						 */
						free_slot = palloc(sizeof(save_buffer));
						buffer_list = lappend(buffer_list, free_slot);
						buffer_lists[p.pid % NBUFFER_LISTS] = buffer_list;
					}
					free_slot->pid = p.pid;
					str = &(free_slot->data);
					initStringInfo(str);
					appendBinaryStringInfo(str,
										   cursor + PIPE_HEADER_SIZE,
										   p.len);
				}
			}
			else
			{
				/*
				 * Final chunk --- add it to anything saved for that pid, and
				 * either way write the whole thing out.
				 */
				/*
				 * 最终块 --- 将其添加到为该 pid 保存的任何内容中，无论哪种方式都写出整个内容。
				 */
				if (existing_slot != NULL)
				{
					str = &(existing_slot->data);
					appendBinaryStringInfo(str,
										   cursor + PIPE_HEADER_SIZE,
										   p.len);
					write_syslogger_file(str->data, str->len, dest);
					/* Mark the buffer unused, and reclaim string storage */
					/* 将缓冲区标记为未使用，并回收字符串存储空间 */
					existing_slot->pid = 0;
					pfree(str->data);
				}
				else
				{
					/* The whole message was one chunk, evidently. */
					/* 显然，整个消息就是一个块。 */
					write_syslogger_file(cursor + PIPE_HEADER_SIZE, p.len,
										 dest);
				}
			}

			/* Finished processing this chunk */
			/* 完成对此块的处理 */
			cursor += chunklen;
			count -= chunklen;
		}
		else
		{
			/* Process non-protocol data */
			/* 处理非协议数据 */

			/*
			 * Look for the start of a protocol header.  If found, dump data
			 * up to there and repeat the loop.  Otherwise, dump it all and
			 * fall out of the loop.  (Note: we want to dump it all if at all
			 * possible, so as to avoid dividing non-protocol messages across
			 * logfiles.  We expect that in many scenarios, a non-protocol
			 * message will arrive all in one read(), and we want to respect
			 * the read() boundary if possible.)
			 */
			/*
			 * 寻找协议头的开始。如果找到，将数据倾倒到那里并重复循环。
			 * 否则，将其全部倾倒并退出循环。（注意：如果可能的话，我们希望倾倒全部数据，
			 * 以避免将非协议消息划分在不同日志文件上。我们期望在许多情况下，
			 * 非协议消息会一次性到达，我们希望尽可能尊重 read() 的边界。）
			 */
			for (chunklen = 1; chunklen < count; chunklen++)
			{
				if (cursor[chunklen] == '\0')
					break;
			}
			/* fall back on the stderr log as the destination */
			/* 回退到 stderr 日志作为目的地 */
			write_syslogger_file(cursor, chunklen, LOG_DESTINATION_STDERR);
			cursor += chunklen;
			count -= chunklen;
		}
	}

	/* We don't have a full chunk, so left-align what remains in the buffer */
	/* 我们没有完整的块，因此将缓冲区中保留的内容左对齐 */
	if (count > 0 && cursor != logbuffer)
		memmove(logbuffer, cursor, count);
	*bytes_in_logbuffer = count;
}

/*
 * Force out any buffered data
 *
 * This is currently used only at syslogger shutdown, but could perhaps be
 * useful at other times, so it is careful to leave things in a clean state.
 */
/*
 * 强制输出任何缓存的数据
 *
 * 这目前仅在 syslogger 关闭时使用，但也许在其他时候有用，所以小心将事物保持在干净状态。
 *
 * Function purpose: Flush any remaining incomplete log chunks.
 * 函数作用：强制倾倒出剩余不完整的段缓存数据。
 */
static void
flush_pipe_input(char *logbuffer, int *bytes_in_logbuffer)
{
	int			i;

	/* Dump any incomplete protocol messages */
	/* 倾倒任何不完整的协议消息 */
	for (i = 0; i < NBUFFER_LISTS; i++)
	{
		List	   *list = buffer_lists[i];
		ListCell   *cell;

		foreach(cell, list)
		{
			save_buffer *buf = (save_buffer *) lfirst(cell);

			if (buf->pid != 0)
			{
				StringInfo	str = &(buf->data);

				write_syslogger_file(str->data, str->len,
									 LOG_DESTINATION_STDERR);
				/* Mark the buffer unused, and reclaim string storage */
				/* 将缓冲区标记为未使用，并回收字符串存储空间 */
				buf->pid = 0;
				pfree(str->data);
			}
		}
	}

	/*
	 * Force out any remaining pipe data as-is; we don't bother trying to
	 * remove any protocol headers that may exist in it.
	 */
	/*
	 * 强制按原样输出任何剩余的管道数据；我们不麻烦试图删除其中可能存在的任何协议头。
	 */
	if (*bytes_in_logbuffer > 0)
		write_syslogger_file(logbuffer, *bytes_in_logbuffer,
							 LOG_DESTINATION_STDERR);
	*bytes_in_logbuffer = 0;
}


/* --------------------------------
 *		logfile routines
 *		日志文件例程
 * --------------------------------
 */

/*
 * Write text to the currently open logfile
 *
 * This is exported so that elog.c can call it when MyBackendType is B_LOGGER.
 * This allows the syslogger process to record elog messages of its own,
 * even though its stderr does not point at the syslog pipe.
 */
/*
 * 将文本写入当前打开的日志文件。
 *
 * 这被导出，以便当 MyBackendType 是 B_LOGGER 时，elog.c 可以调用它。
 * 这允许 syslogger 进程记录它自己的 elog 消息，即使它的 stderr 并没有指向 syslog 管道。
 *
 * Function purpose: Direct writing to currently active log destination (syslogFile/csvlogFile/jsonlogFile).
 * 函数作用：将日志内容写入到当前的底层物理文件流中。
 */
void
write_syslogger_file(const char *buffer, int count, int destination)
{
	int			rc;
	FILE	   *logfile;

	/*
	 * If we're told to write to a structured log file, but it's not open,
	 * dump the data to syslogFile (which is always open) instead.  This can
	 * happen if structured output is enabled after postmaster start and we've
	 * been unable to open logFile.  There are also race conditions during a
	 * parameter change whereby backends might send us structured output
	 * before we open the logFile or after we close it.  Writing formatted
	 * output to the regular log file isn't great, but it beats dropping log
	 * output on the floor.
	 *
	 * Think not to improve this by trying to open logFile on-the-fly.  Any
	 * failure in that would lead to recursion.
	 */
	/*
	 * 如果我们被告知写入结构化日志文件，但它没有打开，则将数据转储到 syslogFile
	 * （它总是打开的）中。如果结构化输出在 postmaster 启动后启用且我们一直无法打开
	 * logFile，这可能会发生。在参数更改期间也存在竞态条件，由此后端可能在我们将 
	 * logFile 打开之前或在我们将其关闭之后向我们发送结构化输出。
	 * 将格式化输出写入常规日志文件并不是很好，但这比完全丢失日志输出要好。
	 *
	 * 不要考虑通过试图实时打开 logFile 来改进这一点。其中的任何失败都会导致递归。
	 */
	if ((destination & LOG_DESTINATION_CSVLOG) && csvlogFile != NULL)
		logfile = csvlogFile;
	else if ((destination & LOG_DESTINATION_JSONLOG) && jsonlogFile != NULL)
		logfile = jsonlogFile;
	else
		logfile = syslogFile;

	rc = fwrite(buffer, 1, count, logfile);

	/*
	 * Try to report any failure.  We mustn't use ereport because it would
	 * just recurse right back here, but write_stderr is OK: it will write
	 * either to the postmaster's original stderr, or to /dev/null, but never
	 * to our input pipe which would result in a different sort of looping.
	 */
	/*
	 * 尝试报告任何失败。我们绝不能使用 ereport，因为它会直接递归回这里，但 write_stderr 
	 * 是可以的：它将写入 postmaster 的原始 stderr，或写入 /dev/null，但绝不会写入我们的
	 * 输入管道，因为这会导致另一种形式的死循环。
	 */
	if (rc != count)
		write_stderr("could not write to log file: %m\n");
}

#ifdef WIN32

/*
 * Worker thread to transfer data from the pipe to the current logfile.
 *
 * We need this because on Windows, WaitForMultipleObjects does not work on
 * unnamed pipes: it always reports "signaled", so the blocking ReadFile won't
 * allow for SIGHUP; and select is for sockets only.
 */
/*
 * 工作线程，用于将数据从管道传输到当前的日志文件。
 *
 * 我们在 Windows 上需要这个，因为 WaitForMultipleObjects 不适用于无名管道：
 * 它总是报告 “已发出信号”，所以阻塞 ReadFile 将不允许 SIGHUP；select 仅适用于套接字。
 */
static unsigned int __stdcall
pipeThread(void *arg)
{
	char		logbuffer[READ_BUF_SIZE];
	int			bytes_in_logbuffer = 0;

	for (;;)
	{
		DWORD		bytesRead;
		BOOL		result;

		result = ReadFile(syslogPipe[0],
						  logbuffer + bytes_in_logbuffer,
						  sizeof(logbuffer) - bytes_in_logbuffer,
						  &bytesRead, 0);

		/*
		 * Enter critical section before doing anything that might touch
		 * global state shared by the main thread. Anything that uses
		 * palloc()/pfree() in particular are not safe outside the critical
		 * section.
		 */
		/*
		 * 在做任何可能触及由主线程共享的全局状态的事情之前进入临界区。
		 * 特别是任何使用 palloc()/pfree() 的操作在临界区外都是不安全的。
		 */
		EnterCriticalSection(&sysloggerSection);
		if (!result)
		{
			DWORD		error = GetLastError();

			if (error == ERROR_HANDLE_EOF ||
				error == ERROR_BROKEN_PIPE)
				break;
			_dosmaperr(error);
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not read from logger pipe: %m")));
		}
		else if (bytesRead > 0)
		{
			bytes_in_logbuffer += bytesRead;
			process_pipe_input(logbuffer, &bytes_in_logbuffer);
		}

		/*
		 * If we've filled the current logfile, nudge the main thread to do a
		 * log rotation.
		 */
		/*
		 * 如果我们填满了当前日志文件，提醒主线程进行日志轮转。
		 */
		if (Log_RotationSize > 0)
		{
			if (ftello(syslogFile) >= Log_RotationSize * (pgoff_t) 1024 ||
				(csvlogFile != NULL &&
				 ftello(csvlogFile) >= Log_RotationSize * (pgoff_t) 1024) ||
				(jsonlogFile != NULL &&
				 ftello(jsonlogFile) >= Log_RotationSize * (pgoff_t) 1024))
				SetLatch(MyLatch);
		}
		LeaveCriticalSection(&sysloggerSection);
	}

	/* We exit the above loop only upon detecting pipe EOF */
	/* 我们仅在检测到管道 EOF 后退出上述循环 */
	pipe_eof_seen = true;

	/* if there's any data left then force it out now */
	/* 如果有任何数据留下，现在强制将其输出 */
	flush_pipe_input(logbuffer, &bytes_in_logbuffer);

	/* set the latch to waken the main thread, which will quit */
	/* 设置锁存器以唤醒主线程，主线程将退出 */
	SetLatch(MyLatch);

	LeaveCriticalSection(&sysloggerSection);
	_endthread();
	return 0;
}
#endif							/* WIN32 */

/*
 * Open a new logfile with proper permissions and buffering options.
 *
 * If allow_errors is true, we just log any open failure and return NULL
 * (with errno still correct for the fopen failure).
 * Otherwise, errors are treated as fatal.
 */
/*
 * 使用适当的权限和缓存选项打开新日志文件。
 *
 * 如果 allow_errors 为 true，我们只是记录任何打开失败并返回 NULL
 * （errno 仍符合 fopen 失败的情况）。
 * 否则，错误将被视为致命错误。
 *
 * Function purpose: Open a log file with configured file permission mode mask.
 * 函数作用：根据配置的日志权限掩码创建或打开指定路径下的物理文件。
 */
static FILE *
logfile_open(const char *filename, const char *mode, bool allow_errors)
{
	FILE	   *fh;
	mode_t		oumask;

	/*
	 * Note we do not let Log_file_mode disable IWUSR, since we certainly want
	 * to be able to write the files ourselves.
	 */
	/*
	 * 注意我们不让 Log_file_mode 禁用 IWUSR，因为我们当然希望能够自己写入文件。
	 */
	oumask = umask((mode_t) ((~(Log_file_mode | S_IWUSR)) & (S_IRWXU | S_IRWXG | S_IRWXO)));
	fh = fopen(filename, mode);
	umask(oumask);

	if (fh)
	{
		setvbuf(fh, NULL, PG_IOLBF, 0);

#ifdef WIN32
		/* use CRLF line endings on Windows */
		_setmode(_fileno(fh), _O_TEXT);
#endif
	}
	else
	{
		int			save_errno = errno;

		ereport(allow_errors ? LOG : FATAL,
				(errcode_for_file_access(),
				 errmsg("could not open log file \"%s\": %m",
						filename)));
		errno = save_errno;
	}

	return fh;
}

/*
 * Do logfile rotation for a single destination, as specified by target_dest.
 * The information stored in *last_file_name and *logFile is updated on a
 * successful file rotation.
 *
 * Returns false if the rotation has been stopped, or true to move on to
 * the processing of other formats.
 */
/*
 * 为 target_dest 指定的单个目的地执行日志文件轮转。
 * 成功进行文件轮转时，存储在 *last_file_name 和 *logFile 中的信息将被更新。
 *
 * 如果轮转已停止，则返回 false，或者返回 true 以继续处理其他格式。
 *
 * Function purpose: Helper to rotate logs for a specific destination (stderr/csvlog/jsonlog).
 * 函数作用：辅助实现单个特定日志目标的文件轮转，自动判定 truncate 还是 append 并替换句柄。
 */
static bool
logfile_rotate_dest(bool time_based_rotation, int size_rotation_for,
					pg_time_t fntime, int target_dest,
					char **last_file_name, FILE **logFile)
{
	char	   *logFileExt = NULL;
	char	   *filename;
	FILE	   *fh;

	/*
	 * If the target destination was just turned off, close the previous file
	 * and unregister its data.  This cannot happen for stderr as syslogFile
	 * is assumed to be always opened even if stderr is disabled in
	 * log_destination.
	 */
	/*
	 * 如果目标目的地刚刚被关闭，关闭上一个文件并注销其数据。这对于 stderr 
	 * 来说是不可能发生的，因为即使在 log_destination 中禁用了 stderr，
	 * 也假定 syslogFile 始终处于打开状态。
	 */
	if ((Log_destination & target_dest) == 0 &&
		target_dest != LOG_DESTINATION_STDERR)
	{
		if (*logFile != NULL)
			fclose(*logFile);
		*logFile = NULL;
		if (*last_file_name != NULL)
			pfree(*last_file_name);
		*last_file_name = NULL;
		return true;
	}

	/*
	 * Leave if it is not time for a rotation or if the target destination has
	 * no need to do a rotation based on the size of its file.
	 */
	/*
	 * 如果还没到轮转时间，或者如果目标目的地没有基于其文件大小进行轮转的需求，则离开。
	 */
	if (!time_based_rotation && (size_rotation_for & target_dest) == 0)
		return true;

	/* file extension depends on the destination type */
	/* 文件扩展名取决于目的地类型 */
	if (target_dest == LOG_DESTINATION_STDERR)
		logFileExt = NULL;
	else if (target_dest == LOG_DESTINATION_CSVLOG)
		logFileExt = ".csv";
	else if (target_dest == LOG_DESTINATION_JSONLOG)
		logFileExt = ".json";
	else
	{
		/* cannot happen */
		/* 不可能发生 */
		Assert(false);
	}

	/* build the new file name */
	/* 构建新的文件名 */
	filename = logfile_getname(fntime, logFileExt);

	/*
	 * Decide whether to overwrite or append.  We can overwrite if (a)
	 * Log_truncate_on_rotation is set, (b) the rotation was triggered by
	 * elapsed time and not something else, and (c) the computed file name is
	 * different from what we were previously logging into.
	 */
	/*
	 * 决定是覆盖还是追加。我们可以覆盖，如果 (a) 设置了 Log_truncate_on_rotation，
	 * (b) 轮转是由流逝的时间触发的，而不是其他原因，以及 (c) 计算出的文件名与我们
	 * 先前记录的文件名不同。
	 */
	if (Log_truncate_on_rotation && time_based_rotation &&
		*last_file_name != NULL &&
		strcmp(filename, *last_file_name) != 0)
		fh = logfile_open(filename, "w", true);
	else
		fh = logfile_open(filename, "a", true);

	if (!fh)
	{
		/*
		 * ENFILE/EMFILE are not too surprising on a busy system; just keep
		 * using the old file till we manage to get a new one.  Otherwise,
		 * assume something's wrong with Log_directory and stop trying to
		 * create files.
		 */
		/*
		 * 在繁忙的系统上，ENFILE/EMFILE 并不太令人惊讶；只需继续使用旧文件，直到我们设法
		 * 获得一个新文件。否则，假定 Log_directory 有问题，停止尝试创建文件。
		 */
		if (errno != ENFILE && errno != EMFILE)
		{
			ereport(LOG,
					(errmsg("disabling automatic rotation (use SIGHUP to re-enable)")));
			rotation_disabled = true;
		}

		if (filename)
			pfree(filename);
		return false;
	}

	/* fill in the new information */
	/* 填入新信息 */
	if (*logFile != NULL)
		fclose(*logFile);
	*logFile = fh;

	/* instead of pfree'ing filename, remember it for next time */
	/* 记住它以备下次使用，而不是释放文件名 */
	if (*last_file_name != NULL)
		pfree(*last_file_name);
	*last_file_name = filename;

	return true;
}

/*
 * perform logfile rotation
 */
/*
 * 执行日志文件轮转
 *
 * Function purpose: Handle log rotation for active formats and update state.
 * 函数作用：顺序执行每一个有效日志管道的物理文件轮转，并设定下一次轮转时间。
 */
static void
logfile_rotate(bool time_based_rotation, int size_rotation_for)
{
	pg_time_t	fntime;

	rotation_requested = false;

	/*
	 * When doing a time-based rotation, invent the new logfile name based on
	 * the planned rotation time, not current time, to avoid "slippage" in the
	 * file name when we don't do the rotation immediately.
	 */
	/*
	 * 进行基于时间的轮转时，根据计划的轮转时间而不是当前时间来编造新的日志文件名，
	 * 以避免在我们没有立即进行轮转时文件名中出现 “偏移”。
	 */
	if (time_based_rotation)
		fntime = next_rotation_time;
	else
		fntime = time(NULL);

	/* file rotation for stderr */
	/* stderr 的文件轮转 */
	if (!logfile_rotate_dest(time_based_rotation, size_rotation_for, fntime,
							 LOG_DESTINATION_STDERR, &last_sys_file_name,
							 &syslogFile))
		return;

	/* file rotation for csvlog */
	/* csvlog 的文件轮转 */
	if (!logfile_rotate_dest(time_based_rotation, size_rotation_for, fntime,
							 LOG_DESTINATION_CSVLOG, &last_csv_file_name,
							 &csvlogFile))
		return;

	/* file rotation for jsonlog */
	/* jsonlog 的文件轮转 */
	if (!logfile_rotate_dest(time_based_rotation, size_rotation_for, fntime,
							 LOG_DESTINATION_JSONLOG, &last_json_file_name,
							 &jsonlogFile))
		return;

	update_metainfo_datafile();

	set_next_rotation_time();
}


/*
 * construct logfile name using timestamp information
 *
 * If suffix isn't NULL, append it to the name, replacing any ".log"
 * that may be in the pattern.
 *
 * Result is palloc'd.
 */
/*
 * 使用时间戳信息构造日志文件名
 *
 * 如果 suffix 不为 NULL，将其附加到名称中，替换模式中可能存在的任何 “.log”。
 *
 * 结果是经过 palloc 分配的。
 *
 * Function purpose: Build the log filename using strftime formatting.
 * 函数作用：利用 strftime 格式控制生成对应时间戳和后缀形式的日志物理路径字符串。
 */
static char *
logfile_getname(pg_time_t timestamp, const char *suffix)
{
	char	   *filename;
	int			len;

	filename = palloc(MAXPGPATH);

	snprintf(filename, MAXPGPATH, "%s/", Log_directory);

	len = strlen(filename);

	/* treat Log_filename as a strftime pattern */
	/* 将 Log_filename 视为 strftime 模式 */
	pg_strftime(filename + len, MAXPGPATH - len, Log_filename,
				pg_localtime(&timestamp, log_timezone));

	if (suffix != NULL)
	{
		len = strlen(filename);
		if (len > 4 && (strcmp(filename + (len - 4), ".log") == 0))
			len -= 4;
		strlcpy(filename + len, suffix, MAXPGPATH - len);
	}

	return filename;
}

/*
 * Determine the next planned rotation time, and store in next_rotation_time.
 */
/*
 * 确定下一次计划轮转时间，并存储在 next_rotation_time 中。
 *
 * Function purpose: Calculate the next point of log rotation.
 * 函数作用：计算并设置下一次日志基于时间应当轮转的具体时间戳。
 */
static void
set_next_rotation_time(void)
{
	pg_time_t	now;
	struct pg_tm *tm;
	int			rotinterval;

	/* nothing to do if time-based rotation is disabled */
	/* 如果禁用了基于时间的轮转，则无事可做 */
	if (Log_RotationAge <= 0)
		return;

	/*
	 * The requirements here are to choose the next time > now that is a
	 * "multiple" of the log rotation interval.  "Multiple" can be interpreted
	 * fairly loosely.  In this version we align to log_timezone rather than
	 * GMT.
	 */
	/*
	 * 这里的要求是选择下一个时间 > now 且是日志轮转间隔的 “倍数”。“倍数”可以解释得相当宽松。
	 * 在此版本中，我们与 log_timezone 对齐，而不是与 GMT 对齐。
	 */
	rotinterval = Log_RotationAge * SECS_PER_MINUTE;	/* convert to seconds */ /* 转换为秒 */
	now = (pg_time_t) time(NULL);
	tm = pg_localtime(&now, log_timezone);
	now += tm->tm_gmtoff;
	now -= now % rotinterval;
	now += rotinterval;
	now -= tm->tm_gmtoff;
	next_rotation_time = now;
}

/*
 * Store the name of the file(s) where the log collector, when enabled, writes
 * log messages.  Useful for finding the name(s) of the current log file(s)
 * when there is time-based logfile rotation.  Filenames are stored in a
 * temporary file and which is renamed into the final destination for
 * atomicity.  The file is opened with the same permissions as what gets
 * created in the data directory and has proper buffering options.
 */
/*
 * 存储启用时日志收集器向其中写入日志消息的一个或多个文件的名称。
 * 在基于时间的日志文件轮转时，这对于寻找当前一个或多个日志文件的名称很有用。
 * 文件名存储在临时文件中，为原子性起见，将其重命名为最终目的地。
 * 该文件以与在数据目录中创建的文件相同的权限打开，并具有适当的缓冲选项。
 *
 * Function purpose: Maintain log metainfo file (current logfile paths) on disk.
 * 函数作用：在磁盘中维护日志的元数据信息（主要包含当前最新激活的文件名称）。
 */
static void
update_metainfo_datafile(void)
{
	FILE	   *fh;
	mode_t		oumask;

	if (!(Log_destination & LOG_DESTINATION_STDERR) &&
		!(Log_destination & LOG_DESTINATION_CSVLOG) &&
		!(Log_destination & LOG_DESTINATION_JSONLOG))
	{
		if (unlink(LOG_METAINFO_DATAFILE) < 0 && errno != ENOENT)
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not remove file \"%s\": %m",
							LOG_METAINFO_DATAFILE)));
		return;
	}

	/* use the same permissions as the data directory for the new file */
	/* 为新文件使用与数据目录相同的权限 */
	oumask = umask(pg_mode_mask);
	fh = fopen(LOG_METAINFO_DATAFILE_TMP, "w");
	umask(oumask);

	if (fh)
	{
		setvbuf(fh, NULL, PG_IOLBF, 0);

#ifdef WIN32
		/* use CRLF line endings on Windows */
		_setmode(_fileno(fh), _O_TEXT);
#endif
	}
	else
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m",
						LOG_METAINFO_DATAFILE_TMP)));
		return;
	}

	if (last_sys_file_name && (Log_destination & LOG_DESTINATION_STDERR))
	{
		if (fprintf(fh, "stderr %s\n", last_sys_file_name) < 0)
		{
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not write file \"%s\": %m",
							LOG_METAINFO_DATAFILE_TMP)));
			fclose(fh);
			return;
		}
	}

	if (last_csv_file_name && (Log_destination & LOG_DESTINATION_CSVLOG))
	{
		if (fprintf(fh, "csvlog %s\n", last_csv_file_name) < 0)
		{
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not write file \"%s\": %m",
							LOG_METAINFO_DATAFILE_TMP)));
			fclose(fh);
			return;
		}
	}

	if (last_json_file_name && (Log_destination & LOG_DESTINATION_JSONLOG))
	{
		if (fprintf(fh, "jsonlog %s\n", last_json_file_name) < 0)
		{
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not write file \"%s\": %m",
							LOG_METAINFO_DATAFILE_TMP)));
			fclose(fh);
			return;
		}
	}
	fclose(fh);

	if (rename(LOG_METAINFO_DATAFILE_TMP, LOG_METAINFO_DATAFILE) != 0)
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not rename file \"%s\" to \"%s\": %m",
						LOG_METAINFO_DATAFILE_TMP, LOG_METAINFO_DATAFILE)));
}

/* --------------------------------
 *		signal handler routines
 *		信号处理程序例程
 * --------------------------------
 */

/*
 * Check to see if a log rotation request has arrived.  Should be
 * called by postmaster after receiving SIGUSR1.
 */
/*
 * 检查日志轮转请求是否已到达。应该在 postmaster 收到 SIGUSR1 后调用。
 *
 * Function purpose: Check if logrotate signal file is present.
 * 函数作用：通过检查是否存在 logrotate 物理信号文件来判定是否由外部请求轮转。
 */
bool
CheckLogrotateSignal(void)
{
	struct stat stat_buf;

	if (stat(LOGROTATE_SIGNAL_FILE, &stat_buf) == 0)
		return true;

	return false;
}

/*
 * Remove the file signaling a log rotation request.
 */
/*
 * 删除指示日志轮转请求的文件。
 *
 * Function purpose: Remove the logrotate signal file.
 * 函数作用：删除 logrotate 信号文件以清除轮转标识。
 */
void
RemoveLogrotateSignalFiles(void)
{
	unlink(LOGROTATE_SIGNAL_FILE);
}

/* SIGUSR1: set flag to rotate logfile */
/* SIGUSR1：设置轮转日志文件的标志 */
static void
sigUsr1Handler(SIGNAL_ARGS)
{
	rotation_requested = true;
	SetLatch(MyLatch);
}

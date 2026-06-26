/*-------------------------------------------------------------------------
 *
 * pg_ctl --- start/stops/restarts the PostgreSQL server
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 *
 * src/bin/pg_ctl/pg_ctl.c
 *
 *-------------------------------------------------------------------------
 * pg_ctl --- start/stops/restarts the PostgreSQL server
 * pg_ctl --- 启动/停止/重启 PostgreSQL 服务器
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * 部分版权所有 (c) 1996-2025, PostgreSQL 全球开发组
 *
 * src/bin/pg_ctl/pg_ctl.c
 */

#include "postgres_fe.h"

#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>


#include "catalog/pg_control.h"
#include "common/controldata_utils.h"
#include "common/file_perm.h"
#include "common/logging.h"
#include "common/string.h"
#include "getopt_long.h"
#include "utils/pidfile.h"

#ifdef WIN32					/* on Unix, we don't need libpq - 在 Unix 上，我们不需要 libpq */
#include "pqexpbuffer.h"
#endif


typedef enum
{
	SMART_MODE,
	FAST_MODE,
	IMMEDIATE_MODE,
} ShutdownMode;

typedef enum
{
	POSTMASTER_READY,
	POSTMASTER_STILL_STARTING,
	POSTMASTER_SHUTDOWN_IN_RECOVERY,
	POSTMASTER_FAILED,
} WaitPMResult;

typedef enum
{
	NO_COMMAND = 0,
	INIT_COMMAND,
	START_COMMAND,
	STOP_COMMAND,
	RESTART_COMMAND,
	RELOAD_COMMAND,
	STATUS_COMMAND,
	PROMOTE_COMMAND,
	LOGROTATE_COMMAND,
	KILL_COMMAND,
	REGISTER_COMMAND,
	UNREGISTER_COMMAND,
	RUN_AS_SERVICE_COMMAND,
} CtlCommand;

#define DEFAULT_WAIT	60

#define USEC_PER_SEC	1000000

#define WAITS_PER_SEC	10		/* should divide USEC_PER_SEC evenly - 应该整除 USEC_PER_SEC */

static bool do_wait = true;
static int	wait_seconds = DEFAULT_WAIT;
static bool wait_seconds_arg = false;
static bool silent_mode = false;
static ShutdownMode shutdown_mode = FAST_MODE;
static int	sig = SIGINT;		/* default - 默认值 */
static CtlCommand ctl_command = NO_COMMAND;
static char *pg_data = NULL;
static char *pg_config = NULL;
static char *pgdata_opt = NULL;
static char *post_opts = NULL;
static const char *progname;
static char *log_file = NULL;
static char *exec_path = NULL;
static char *event_source = NULL;
static char *register_servicename = "PostgreSQL";	/* FIXME: + version ID? - FIXME: 是否加上版本 ID？ */
static char *register_username = NULL;
static char *register_password = NULL;
static char *argv0 = NULL;
static bool allow_core_files = false;
static time_t start_time;

static char postopts_file[MAXPGPATH];
static char version_file[MAXPGPATH];
static char pid_file[MAXPGPATH];
static char promote_file[MAXPGPATH];
static char logrotate_file[MAXPGPATH];

static volatile pid_t postmasterPID = -1;

#ifdef WIN32
static DWORD pgctl_start_type = SERVICE_AUTO_START;
static SERVICE_STATUS status;
static SERVICE_STATUS_HANDLE hStatus = (SERVICE_STATUS_HANDLE) 0;
static HANDLE shutdownHandles[2];

#define shutdownEvent	  shutdownHandles[0]
#define postmasterProcess shutdownHandles[1]
#endif


static void write_stderr(const char *fmt,...) pg_attribute_printf(1, 2);
static void do_advice(void);
static void do_help(void);
static void set_mode(char *modeopt);
static void set_sig(char *signame);
static void do_init(void);
static void do_start(void);
static void do_stop(void);
static void do_restart(void);
static void do_reload(void);
static void do_status(void);
static void do_promote(void);
static void do_logrotate(void);
static void do_kill(pid_t pid);
static void print_msg(const char *msg);
static void adjust_data_dir(void);

#ifdef WIN32
#include <versionhelpers.h>
static bool pgwin32_IsInstalled(SC_HANDLE);
static char *pgwin32_CommandLine(bool);
static void pgwin32_doRegister(void);
static void pgwin32_doUnregister(void);
static void pgwin32_SetServiceStatus(DWORD);
static void WINAPI pgwin32_ServiceHandler(DWORD);
static void WINAPI pgwin32_ServiceMain(DWORD, LPTSTR *);
static void pgwin32_doRunAsService(void);
static int	CreateRestrictedProcess(char *cmd, PROCESS_INFORMATION *processInfo, bool as_service);
static PTOKEN_PRIVILEGES GetPrivilegesToDelete(HANDLE hToken);
#endif

static pid_t get_pgpid(bool is_status_request);
static char **readfile(const char *path, int *numlines);
static void free_readfile(char **optlines);
static pid_t start_postmaster(void);
static void read_post_opts(void);

static WaitPMResult wait_for_postmaster_start(pid_t pm_pid, bool do_checkpoint);
static bool wait_for_postmaster_stop(void);
static bool wait_for_postmaster_promote(void);
static bool postmaster_is_alive(pid_t pid);

#if defined(HAVE_GETRLIMIT)
static void unlimit_core_size(void);
#endif

static DBState get_control_dbstate(void);


#ifdef WIN32
static void
write_eventlog(int level, const char *line)
{
	static HANDLE evtHandle = INVALID_HANDLE_VALUE;

	if (silent_mode && level == EVENTLOG_INFORMATION_TYPE)
		return;

	if (evtHandle == INVALID_HANDLE_VALUE)
	{
		evtHandle = RegisterEventSource(NULL,
										event_source ? event_source : DEFAULT_EVENT_SOURCE);
		if (evtHandle == NULL)
		{
			evtHandle = INVALID_HANDLE_VALUE;
			return;
		}
	}

	ReportEvent(evtHandle,
				level,
				0,
				0,				/* All events are Id 0 - 所有事件都是 Id 0 */
				NULL,
				1,
				0,
				&line,
				NULL);
}
#endif

/*
 * Write errors to stderr (or by equal means when stderr is
 * not available).
 * Write errors to stderr (or by equal means when stderr is not available).
 * 将错误写入 stderr（或者在 stderr 不可用时通过同等方式写入）。
 */
/*
 * write_stderr --- Output error messages.
 * 函数作用：输出错误信息。如果是 Unix 平台，直接 fprintf 到 stderr；如果是 Windows 并且作为服务运行，则写入 Windows 事件日志。
 */
static void
write_stderr(const char *fmt,...)
{
	va_list		ap;

	va_start(ap, fmt);
#ifndef WIN32
	/* On Unix, we just fprintf to stderr - 在 Unix 上，我们直接 fprintf 到 stderr */
	vfprintf(stderr, fmt, ap);
#else

	/*
	 * On Win32, we print to stderr if running on a console, or write to
	 * eventlog if running as a service
	 * On Win32, we print to stderr if running on a console, or write to eventlog if running as a service
	 * 在 Win32 上，如果是控制台运行则打印到 stderr，如果是作为服务运行则写入 eventlog
	 */
	if (pgwin32_is_service())	/* Running as a service - 作为服务运行 */
	{
		char		errbuf[2048];	/* Arbitrary size? - 任意大小？ */

		vsnprintf(errbuf, sizeof(errbuf), fmt, ap);

		write_eventlog(EVENTLOG_ERROR_TYPE, errbuf);
	}
	else
		/* Not running as service, write to stderr - 不是作为服务运行，写入 stderr */
		vfprintf(stderr, fmt, ap);
#endif
	va_end(ap);
}

/*
 * Given an already-localized string, print it to stdout unless the
 * user has specified that no messages should be printed.
 * Given an already-localized string, print it to stdout unless the user has specified that no messages should be printed.
 * 给定一个已经本地化的字符串，将其打印到 stdout，除非用户指定不打印任何消息。
 */
/*
 * print_msg --- Print informational messages.
 * 函数作用：如果不是静默模式，打印提示信息到 stdout，并刷新缓冲区。
 */
static void
print_msg(const char *msg)
{
	if (!silent_mode)
	{
		fputs(msg, stdout);
		fflush(stdout);
	}
}

/*
 * get_pgpid --- Retrieve postmaster PID from locking file.
 * 函数作用：读取 postmaster.pid 文件并获取其中的 PID 值。做一些基础校验，比如验证数据目录是否存在。
 */
static pid_t
get_pgpid(bool is_status_request)
{
	FILE	   *pidf;
	int			pid;
	struct stat statbuf;

	if (stat(pg_data, &statbuf) != 0)
	{
		if (errno == ENOENT)
			write_stderr(_("%s: directory \"%s\" does not exist\n"), progname,
						 pg_data);
		else
			write_stderr(_("%s: could not access directory \"%s\": %m\n"), progname,
						 pg_data);

		/*
		 * The Linux Standard Base Core Specification 3.1 says this should
		 * return '4, program or service status is unknown'
		 * https://refspecs.linuxbase.org/LSB_3.1.0/LSB-Core-generic/LSB-Core-generic/iniscrptact.html
		 * The Linux Standard Base Core Specification 3.1 says this should return '4, program or service status is unknown'
		 * https://refspecs.linuxbase.org/LSB_3.1.0/LSB-Core-generic/LSB-Core-generic/iniscrptact.html
		 * Linux Standard Base Core Specification 3.1 指出这应该返回 '4，程序或服务状态未知'
		 * https://refspecs.linuxbase.org/LSB_3.1.0/LSB-Core-generic/LSB-Core-generic/iniscrptact.html
		 */
		exit(is_status_request ? 4 : 1);
	}

	if (stat(version_file, &statbuf) != 0 && errno == ENOENT)
	{
		write_stderr(_("%s: directory \"%s\" is not a database cluster directory\n"),
					 progname, pg_data);
		exit(is_status_request ? 4 : 1);
	}

	pidf = fopen(pid_file, "r");
	if (pidf == NULL)
	{
		/* No pid file, not an error on startup - 没有 pid 文件，在启动时不是错误 */
		if (errno == ENOENT)
			return 0;
		else
		{
			write_stderr(_("%s: could not open PID file \"%s\": %m\n"),
						 progname, pid_file);
			exit(1);
		}
	}
	if (fscanf(pidf, "%d", &pid) != 1)
	{
		/* Is the file empty? - 文件是否为空？ */
		if (ftell(pidf) == 0 && feof(pidf))
			write_stderr(_("%s: the PID file \"%s\" is empty\n"),
						 progname, pid_file);
		else
			write_stderr(_("%s: invalid data in PID file \"%s\"\n"),
						 progname, pid_file);
		exit(1);
	}
	fclose(pidf);
	return (pid_t) pid;
}


/*
 * get the lines from a text file - return NULL if file can't be opened
 *
 * Trailing newlines are deleted from the lines (this is a change from pre-v10)
 *
 * *numlines is set to the number of line pointers returned; there is
 * also an additional NULL pointer after the last real line.
 * get the lines from a text file - return NULL if file can't be opened
 * 从文本文件中获取行 - 如果无法打开文件则返回 NULL
 *
 * Trailing newlines are deleted from the lines (this is a change from pre-v10)
 * 从行中删除尾随换行符（这是相比 v10 之前的更改）
 *
 * *numlines is set to the number of line pointers returned; there is
 * also an additional NULL pointer after the last real line.
 * *numlines 设置为返回的行指针数；在最后一行真实行之后还有一个额外的 NULL 指针。
 */
/*
 * readfile --- Read entire file by lines.
 * 函数作用：将指定文件的内容全部读入内存，并切分为以行组成的字符串数组，末尾附加 NULL 指针。
 */
static char **
readfile(const char *path, int *numlines)
{
	int			fd;
	int			nlines;
	char	  **result;
	char	   *buffer;
	char	   *linebegin;
	int			i;
	int			n;
	int			len;
	struct stat statbuf;

	*numlines = 0;				/* in case of failure or empty file - 空文件 */

	/*
	 * Slurp the file into memory.
	 *
	 * The file can change concurrently, so we read the whole file into memory
	 * with a single read() call. That's not guaranteed to get an atomic
	 * snapshot, but in practice, for a small file, it's close enough for the
	 * current use.
	 * Slurp the file into memory.
	 *
	 * The file can change concurrently, so we read the whole file into memory
	 * with a single read() call. That's not guaranteed to get an atomic
	 * snapshot, but in practice, for a small file, it's close enough for the
	 * current use.
	 * 将文件吞入内存。
	 * 文件可能会并发更改，因此我们通过单个 read() 调用将整个文件读入内存。
	 * 这不能保证获得原子快照，但实际上，对于一个小文件，它对于当前用途来说已经足够接近了。
	 */
	fd = open(path, O_RDONLY | PG_BINARY, 0);
	if (fd < 0)
		return NULL;
	if (fstat(fd, &statbuf) < 0)
	{
		close(fd);
		return NULL;
	}
	if (statbuf.st_size == 0)
	{
		/* empty file - 空文件 */
		close(fd);
		result = (char **) pg_malloc(sizeof(char *));
		*result = NULL;
		return result;
	}
	buffer = pg_malloc(statbuf.st_size + 1);

	len = read(fd, buffer, statbuf.st_size + 1);
	close(fd);
	if (len != statbuf.st_size)
	{
		/* oops, the file size changed between fstat and read - 糟糕，文件大小在 fstat 和 read 之间发生了变化 */
		free(buffer);
		return NULL;
	}

	/*
	 * Count newlines. We expect there to be a newline after each full line,
	 * including one at the end of file. If there isn't a newline at the end,
	 * any characters after the last newline will be ignored.
	 * Count newlines. We expect there to be a newline after each full line,
	 * including one at the end of file. If there isn't a newline at the end,
	 * any characters after the last newline will be ignored.
	 * 计算换行符。我们希望在每行完整行之后都有一个换行符，包括文件末尾的一个。
	 * 如果末尾没有换行符，则最后一个换行符之后的任何字符都将被忽略。
	 */
	nlines = 0;
	for (i = 0; i < len; i++)
	{
		if (buffer[i] == '\n')
			nlines++;
	}

	/* set up the result buffer - 设置结果缓冲区 */
	result = (char **) pg_malloc((nlines + 1) * sizeof(char *));
	*numlines = nlines;

	/* now split the buffer into lines - 现在将缓冲区拆分为行 */
	linebegin = buffer;
	n = 0;
	for (i = 0; i < len; i++)
	{
		if (buffer[i] == '\n')
		{
			int			slen = &buffer[i] - linebegin;
			char	   *linebuf = pg_malloc(slen + 1);

			memcpy(linebuf, linebegin, slen);
			/* we already dropped the \n, but get rid of any \r too - 我们已经去掉了 \n，但也要去掉所有的 \r */
			if (slen > 0 && linebuf[slen - 1] == '\r')
				slen--;
			linebuf[slen] = '\0';
			result[n++] = linebuf;
			linebegin = &buffer[i + 1];
		}
	}
	result[n] = NULL;

	free(buffer);

	return result;
}


/*
 * Free memory allocated for optlines through readfile()
 * 释放通过 readfile() 为 optlines 分配的内存
 */
/*
 * free_readfile --- Free lines buffer memory.
 * 函数作用：释放由 readfile() 申请的每行和行指针数组的内存。
 */
static void
free_readfile(char **optlines)
{
	char	   *curr_line = NULL;
	int			i = 0;

	if (!optlines)
		return;

	while ((curr_line = optlines[i++]))
		free(curr_line);

	free(optlines);
}

/*
 * start/test/stop routines
 * 启动/测试/停止例程
 */

/*
 * Start the postmaster and return its PID.
 *
 * Currently, on Windows what we return is the PID of the shell process
 * that launched the postmaster (and, we trust, is waiting for it to exit).
 * So the PID is usable for "is the postmaster still running" checks,
 * but cannot be compared directly to postmaster.pid.
 *
 * On Windows, we also save aside a handle to the shell process in
 * "postmasterProcess", which the caller should close when done with it.
 * Start the postmaster and return its PID.
 *
 * Currently, on Windows what we return is the PID of the shell process
 * that launched the postmaster (and, we trust, is waiting for it to exit).
 * So the PID is usable for "is the postmaster still running" checks,
 * but cannot be compared directly to postmaster.pid.
 * 目前，在 Windows 上，我们返回的是启动 postmaster 的 shell 进程的 PID
 * （我们相信它正在等待它退出）。因此，该 PID 可用于“postmaster 是否仍在运行”的检查，
 * 但不能直接与 postmaster.pid 进行比较。
 *
 * On Windows, we also save aside a handle to the shell process in
 * "postmasterProcess", which the caller should close when done with it.
 * 在 Windows 上，我们还在 "postmasterProcess" 中保存了 shell 进程 of 句柄，
 * 调用者在使用完毕后应该关闭它。
 */
/*
 * start_postmaster --- Fork/Spawn postmaster process.
 * 函数作用：在 Unix 上执行 fork 并 execl 运行 shell 执行启动命令；在 Windows 上通过 CreateRestrictedProcess 启动进程。
 */
static pid_t
start_postmaster(void)
{
	char	   *cmd;

#ifndef WIN32
	pid_t		pm_pid;

	/* Flush stdio channels just before fork, to avoid double-output problems - 在 fork 之前刷新 stdio 通道，以避免双重输出问题 */
	fflush(NULL);

#ifdef EXEC_BACKEND
	pg_disable_aslr();
#endif

	pm_pid = fork();
	if (pm_pid < 0)
	{
		/* fork failed - fork 失败 */
		write_stderr(_("%s: could not start server: %m\n"),
					 progname);
		exit(1);
	}
	if (pm_pid > 0)
	{
		/* fork succeeded, in parent - fork 成功，在父进程中 */
		return pm_pid;
	}

	/* fork succeeded, in child - fork 成功，在子进程中 */

	/*
	 * If possible, detach the postmaster process from the launching process
	 * group and make it a group leader, so that it doesn't get signaled along
	 * with the current group that launched it.
	 * If possible, detach the postmaster process from the launching process
	 * group and make it a group leader, so that it doesn't get signaled along
	 * with the current group that launched it.
	 * 如果可能，将 postmaster 进程与启动进程组分离，并使其成为组长，
	 * 这样它就不会与启动它的当前组一起收到信号。
	 */
#ifdef HAVE_SETSID
	if (setsid() < 0)
	{
		write_stderr(_("%s: could not start server due to setsid() failure: %m\n"),
					 progname);
		exit(1);
	}
#endif

	/*
	 * Since there might be quotes to handle here, it is easier simply to pass
	 * everything to a shell to process them.  Use exec so that the postmaster
	 * has the same PID as the current child process.
	 * Since there might be quotes to handle here, it is easier simply to pass
	 * everything to a shell to process them.  Use exec so that the postmaster
	 * has the same PID as the current child process.
	 * 由于这里可能需要处理引号，因此将所有内容传递给 shell 来处理会更容易。
	 * 使用 exec 以便 postmaster 具有与当前子进程相同的 PID。
	 */
	if (log_file != NULL)
		cmd = psprintf("exec \"%s\" %s%s < \"%s\" >> \"%s\" 2>&1",
					   exec_path, pgdata_opt, post_opts,
					   DEVNULL, log_file);
	else
		cmd = psprintf("exec \"%s\" %s%s < \"%s\" 2>&1",
					   exec_path, pgdata_opt, post_opts, DEVNULL);

	(void) execl("/bin/sh", "/bin/sh", "-c", cmd, (char *) NULL);

	/* exec failed - exec 失败 */
	write_stderr(_("%s: could not start server: %m\n"),
				 progname);
	exit(1);

	return 0;					/* keep dumb compilers quiet - 让哑编译器保持安静 */

#else							/* WIN32 - WIN32 平台 */

	/*
	 * As with the Unix case, it's easiest to use the shell (CMD.EXE) to
	 * handle redirection etc.  Unfortunately CMD.EXE lacks any equivalent of
	 * "exec", so we don't get to find out the postmaster's PID immediately.
	 * As with the Unix case, it's easiest to use the shell (CMD.EXE) to
	 * handle redirection etc.  Unfortunately CMD.EXE lacks any equivalent of
	 * "exec", so we don't get to find out the postmaster's PID immediately.
	 * 与 Unix 情况一样，最简单的方法是使用 shell (CMD.EXE) 来处理重定向等。
	 * 不幸的是，CMD.EXE 没有任何等同于 "exec" 的功能，因此我们无法立即找出 postmaster 的 PID。
	 */
	PROCESS_INFORMATION pi;
	const char *comspec;

	/* Find CMD.EXE location using COMSPEC, if it's set - 如果设置了 COMSPEC，使用其查找 CMD.EXE 的位置 */
	comspec = getenv("COMSPEC");
	if (comspec == NULL)
		comspec = "CMD";

	if (log_file != NULL)
	{
		/*
		 * First, open the log file if it exists.  The idea is that if the
		 * file is still locked by a previous postmaster run, we'll wait until
		 * it comes free, instead of failing with ERROR_SHARING_VIOLATION.
		 * (It'd be better to open the file in a sharing-friendly mode, but we
		 * can't use CMD.EXE to do that, so work around it.  Note that the
		 * previous postmaster will still have the file open for a short time
		 * after removing postmaster.pid.)
		 *
		 * If the log file doesn't exist, we *must not* create it here.  If we
		 * were launched with higher privileges than the restricted process
		 * will have, the log file might end up with permissions settings that
		 * prevent the postmaster from writing on it.
		 * First, open the log file if it exists.  The idea is that if the
		 * file is still locked by a previous postmaster run, we'll wait until
		 * it comes free, instead of failing with ERROR_SHARING_VIOLATION.
		 * (It'd be better to open the file in a sharing-friendly mode, but we
		 * can't use CMD.EXE to do that, so work around it.  Note that the
		 * previous postmaster will still have the file open for a short time
		 * after removing postmaster.pid.)
		 * 首先，如果日志文件存在，打开它。这样如果文件仍被前一次 postmaster 运行锁定，
		 * 我们将等待它释放，而不是由于 ERROR_SHARING_VIOLATION 而失败。
		 * （更好的办法是在共享友好模式下打开文件，但我们不能使用 CMD.EXE 来做这件事，所以要绕过它。
		 * 请注意，前一个 postmaster 在删除 postmaster.pid 之后仍会在短时间内打开该文件。）
		 *
		 * If the log file doesn't exist, we *must not* create it here.  If we
		 * were launched with higher privileges than the restricted process
		 * will have, the log file might end up with permissions settings that
		 * prevent the postmaster from writing on it.
		 * 如果日志文件不存在，我们 *绝对不能* 在这里创建它。如果我们的启动权限高于受限进程的权限，
		 * 则日志文件最终的权限设置可能会阻止 postmaster 写入。
		 */
		int			fd = open(log_file, O_RDWR, 0);

		if (fd == -1)
		{
			/*
			 * ENOENT is expectable since we didn't use O_CREAT.  Otherwise
			 * complain.  We could just fall through and let CMD.EXE report
			 * the problem, but its error reporting is pretty miserable.
			 * ENOENT is expectable since we didn't use O_CREAT.  Otherwise
			 * complain.  We could just fall through and let CMD.EXE report
			 * the problem, but its error reporting is pretty miserable.
			 * 因为我们没有使用 O_CREAT，所以 ENOENT 是可以预期的。否则需要抱怨。
			 * 我们本可以落入下一层并让 CMD.EXE 报告问题，但它的错误报告相当糟糕。
			 */
			if (errno != ENOENT)
			{
				write_stderr(_("%s: could not open log file \"%s\": %m\n"),
							 progname, log_file);
				exit(1);
			}
		}
		else
			close(fd);

		cmd = psprintf("\"%s\" /C \"\"%s\" %s%s < \"%s\" >> \"%s\" 2>&1\"",
					   comspec, exec_path, pgdata_opt, post_opts, DEVNULL, log_file);
	}
	else
		cmd = psprintf("\"%s\" /C \"\"%s\" %s%s < \"%s\" 2>&1\"",
					   comspec, exec_path, pgdata_opt, post_opts, DEVNULL);

	if (!CreateRestrictedProcess(cmd, &pi, false))
	{
		write_stderr(_("%s: could not start server: error code %lu\n"),
					 progname, (unsigned long) GetLastError());
		exit(1);
	}
	/* Don't close command process handle here; caller must do so - 不要在这里关闭命令进程句柄；调用者必须这样做 */
	postmasterProcess = pi.hProcess;
	CloseHandle(pi.hThread);
	return pi.dwProcessId;		/* Shell's PID, not postmaster's! - Shell 的 PID，不是 postmaster 的！ */
#endif							/* WIN32 - WIN32 平台 */
}



/*
 * Wait for the postmaster to become ready.
 *
 * On Unix, pm_pid is the PID of the just-launched postmaster.  On Windows,
 * it may be the PID of an ancestor shell process, so we can't check the
 * contents of postmaster.pid quite as carefully.
 *
 * On Windows, the static variable postmasterProcess is an implicit argument
 * to this routine; it contains a handle to the postmaster process or an
 * ancestor shell process thereof.
 *
 * Note that the checkpoint parameter enables a Windows service control
 * manager checkpoint, it's got nothing to do with database checkpoints!!
 * Wait for the postmaster to become ready.
 *
 * On Unix, pm_pid is the PID of the just-launched postmaster.  On Windows,
 * it may be the PID of an ancestor shell process, so we can't check the
 * contents of postmaster.pid quite as carefully.
 * 在 Unix 上，pm_pid 是刚启动 postmaster 的 PID。在 Windows 上，
 * 它可能是祖先 shell 进程的 PID，因此我们无法非常仔细地检查 postmaster.pid 的内容。
 *
 * On Windows, the static variable postmasterProcess is an implicit argument
 * to this routine; it contains a handle to the postmaster process or an
 * ancestor shell process thereof.
 * 在 Windows 上，静态变量 postmasterProcess 是该例程的隐式参数；
 * 它包含指向 postmaster 进程或其祖先 shell 进程的句柄。
 *
 * Note that the checkpoint parameter enables a Windows service control
 * manager checkpoint, it's got nothing to do with database checkpoints!!
 * 请注意，checkpoint 参数启用 Windows 服务控制管理器检查点，这与数据库检查点完全没有关系！
 */
/*
 * wait_for_postmaster_start --- Wait until postmaster ready.
 * 函数作用：轮询读取 pid 文件，等待状态变成 READY 或者 STANDBY，若超时或子进程异常退出则返回对应状态。
 */
static WaitPMResult
wait_for_postmaster_start(pid_t pm_pid, bool do_checkpoint)
{
	int			i;

	for (i = 0; i < wait_seconds * WAITS_PER_SEC; i++)
	{
		char	  **optlines;
		int			numlines;

		/*
		 * Try to read the postmaster.pid file.  If it's not valid, or if the
		 * status line isn't there yet, just keep waiting.
		 * Try to read the postmaster.pid file.  If it's not valid, or if the
		 * status line isn't there yet, just keep waiting.
		 * 尝试读取 postmaster.pid 文件。如果它无效，或者状态行还没有，就继续等待。
		 */
		if ((optlines = readfile(pid_file, &numlines)) != NULL &&
			numlines >= LOCK_FILE_LINE_PM_STATUS)
		{
			/* File is complete enough for us, parse it - 文件对我们来说已经足够完整，解析它 */
			pid_t		pmpid;
			time_t		pmstart;

			/*
			 * Make sanity checks.  If it's for the wrong PID, or the recorded
			 * start time is before pg_ctl started, then either we are looking
			 * at the wrong data directory, or this is a pre-existing pidfile
			 * that hasn't (yet?) been overwritten by our child postmaster.
			 * Allow 2 seconds slop for possible cross-process clock skew.
			 * Make sanity checks.  If it's for the wrong PID, or the recorded
			 * start time is before pg_ctl started, then either we are looking
			 * at the wrong data directory, or this is a pre-existing pidfile
			 * that hasn't (yet?) been overwritten by our child postmaster.
			 * Allow 2 seconds slop for possible cross-process clock skew.
			 * 进行安全检查。如果是错误的 PID，或者记录的启动时间在 pg_ctl 启动之前，
			 * 那么要么我们正在查看错误的数据目录，要么这是一个预先存在的 pid 文件，
			 * 尚未被我们的子 postmaster 覆盖。允许 2 秒的偏差以应对可能的跨进程时钟偏差。
			 */
			pmpid = atol(optlines[LOCK_FILE_LINE_PID - 1]);
			pmstart = atoll(optlines[LOCK_FILE_LINE_START_TIME - 1]);
			if (pmstart >= start_time - 2 &&
#ifndef WIN32
				pmpid == pm_pid
#else
			/* Windows can only reject standalone-backend PIDs - Windows 只能拒绝单用户（standalone-backend）的 PID */
				pmpid > 0
#endif
				)
			{
				/*
				 * OK, seems to be a valid pidfile from our child.  Check the
				 * status line (this assumes a v10 or later server).
				 * OK, seems to be a valid pidfile from our child.  Check the
				 * status line (this assumes a v10 or later server).
				 * 好的，似乎是来自我们子进程的有效 pid 文件。检查状态行（这假设是 v10 或更高版本的服务器）。
				 */
				char	   *pmstatus = optlines[LOCK_FILE_LINE_PM_STATUS - 1];

				if (strcmp(pmstatus, PM_STATUS_READY) == 0 ||
					strcmp(pmstatus, PM_STATUS_STANDBY) == 0)
				{
					/* postmaster is done starting up - postmaster 启动完成 */
					free_readfile(optlines);
					return POSTMASTER_READY;
				}
			}
		}

		/*
		 * Free the results of readfile.
		 *
		 * This is safe to call even if optlines is NULL.
		 * Free the results of readfile.
		 *
		 * This is safe to call even if optlines is NULL.
		 * 释放 readfile 的结果。即使 optlines 为 NULL，调用它也是安全的。
		 */
		free_readfile(optlines);

		/*
		 * Check whether the child postmaster process is still alive.  This
		 * lets us exit early if the postmaster fails during startup.
		 *
		 * On Windows, we may be checking the postmaster's parent shell, but
		 * that's fine for this purpose.
		 * Check whether the child postmaster process is still alive.  This
		 * lets us exit early if the postmaster fails during startup.
		 *
		 * On Windows, we may be checking the postmaster's parent shell, but
		 * that's fine for this purpose.
		 * 检查子 postmaster 进程是否仍然存活。这使我们能够在 postmaster 在启动期间失败时提前退出。
		 * 在 Windows 上，我们可能是在检查 postmaster 的父 shell，但出于这个目的，这没有问题。
		 */
		{
			bool		pm_died;
#ifndef WIN32
			int			exitstatus;

			pm_died = (waitpid(pm_pid, &exitstatus, WNOHANG) == pm_pid);
#else
			pm_died = (WaitForSingleObject(postmasterProcess, 0) == WAIT_OBJECT_0);
#endif
			if (pm_died)
			{
				/* See if postmaster terminated intentionally - 查看 postmaster 是否是故意终止的 */
				if (get_control_dbstate() == DB_SHUTDOWNED_IN_RECOVERY)
					return POSTMASTER_SHUTDOWN_IN_RECOVERY;
				else
					return POSTMASTER_FAILED;
			}
		}

		/* Startup still in process; wait, printing a dot once per second - 启动仍在进行中；等待，每秒打印一个点 */
		if (i % WAITS_PER_SEC == 0)
		{
#ifdef WIN32
			if (do_checkpoint)
			{
				/*
				 * Increment the wait hint by 6 secs (connection timeout +
				 * sleep).  We must do this to indicate to the SCM that our
				 * startup time is changing, otherwise it'll usually send a
				 * stop signal after 20 seconds, despite incrementing the
				 * checkpoint counter.
				 * Increment the wait hint by 6 secs (connection timeout +
				 * sleep).  We must do this to indicate to the SCM that our
				 * startup time is changing, otherwise it'll usually send a
				 * stop signal after 20 seconds, despite incrementing the
				 * checkpoint counter.
				 * 将等待提示增加 6 秒（连接超时 + 休眠）。我们必须这样做以向 SCM 指示我们的
				 * 启动时间正在改变，否则即使增加了检查点计数器，它通常也会在 20 秒后发送停止信号。
				 */
				status.dwWaitHint += 6000;
				status.dwCheckPoint++;
				SetServiceStatus(hStatus, (LPSERVICE_STATUS) &status);
			}
			else
#endif
				print_msg(".");
		}

		pg_usleep(USEC_PER_SEC / WAITS_PER_SEC);
	}

	/* out of patience; report that postmaster is still starting up - 失去耐心；报告 postmaster 仍在启动中 */
	return POSTMASTER_STILL_STARTING;
}


/*
 * Wait for the postmaster to stop.
 *
 * Returns true if the postmaster stopped cleanly (i.e., removed its pidfile).
 * Returns false if the postmaster dies uncleanly, or if we time out.
 * Wait for the postmaster to stop.
 *
 * Returns true if the postmaster stopped cleanly (i.e., removed its pidfile).
 * 如果 postmaster 干净地停止（即删除了其 pid 文件）则返回 true。
 * Returns false if the postmaster dies uncleanly, or if we time out.
 * 如果 postmaster 不干净地死亡或超时，则返回 false。
 */
/*
 * wait_for_postmaster_stop --- Wait until postmaster completely exits.
 * 函数作用：循环检查 get_pgpid 是否返回 0 或者检测该 pid 是否已经被终止，若是则说明停止完成。
 */
static bool
wait_for_postmaster_stop(void)
{
	int			cnt;

	for (cnt = 0; cnt < wait_seconds * WAITS_PER_SEC; cnt++)
	{
		pid_t		pid;

		if ((pid = get_pgpid(false)) == 0)
			return true;		/* pid file is gone - pid 文件已消失 */

		if (kill(pid, 0) != 0)
		{
			/*
			 * Postmaster seems to have died.  Check the pid file once more to
			 * avoid a race condition, but give up waiting.
			 * Postmaster seems to have died.  Check the pid file once more to
			 * avoid a race condition, but give up waiting.
			 * Postmaster 似乎已经死掉了。再次检查 pid 文件以避免竞争条件，但放弃等待。
			 */
			if (get_pgpid(false) == 0)
				return true;	/* pid file is gone - pid 文件已消失 */
			return false;		/* postmaster died untimely - postmaster 异常死亡 */
		}

		if (cnt % WAITS_PER_SEC == 0)
			print_msg(".");
		pg_usleep(USEC_PER_SEC / WAITS_PER_SEC);
	}
	return false;				/* timeout reached - 达到超时时间 */
}


/*
 * Wait for the postmaster to promote.
 *
 * Returns true on success, else false.
 * To avoid waiting uselessly, we check for postmaster death here too.
 * Wait for the postmaster to promote.
 *
 * Returns true on success, else false.
 * 成功时返回 true，否则返回 false。
 * To avoid waiting uselessly, we check for postmaster death here too.
 * 为了避免无谓的等待，我们这里也检查 postmaster 是否死亡。
 */
/*
 * wait_for_postmaster_promote --- Wait until standby server promotes.
 * 函数作用：循环检查控制文件的 DBState 是否已变为 DB_IN_PRODUCTION（主库就绪状态）。
 */
static bool
wait_for_postmaster_promote(void)
{
	int			cnt;

	for (cnt = 0; cnt < wait_seconds * WAITS_PER_SEC; cnt++)
	{
		pid_t		pid;
		DBState		state;

		if ((pid = get_pgpid(false)) == 0)
			return false;		/* pid file is gone - pid 文件已消失 */
		if (kill(pid, 0) != 0)
			return false;		/* postmaster died - postmaster 已死亡 */

		state = get_control_dbstate();
		if (state == DB_IN_PRODUCTION)
			return true;		/* successful promotion - 成功晋升 */

		if (cnt % WAITS_PER_SEC == 0)
			print_msg(".");
		pg_usleep(USEC_PER_SEC / WAITS_PER_SEC);
	}
	return false;				/* timeout reached - 达到超时时间 */
}


#if defined(HAVE_GETRLIMIT)
static void
unlimit_core_size(void)
{
	struct rlimit lim;

	getrlimit(RLIMIT_CORE, &lim);
	if (lim.rlim_max == 0)
	{
		write_stderr(_("%s: cannot set core file size limit; disallowed by hard limit\n"),
					 progname);
		return;
	}
	else if (lim.rlim_max == RLIM_INFINITY || lim.rlim_cur < lim.rlim_max)
	{
		lim.rlim_cur = lim.rlim_max;
		setrlimit(RLIMIT_CORE, &lim);
	}
}
#endif

/*
 * read_post_opts --- Read postmaster start options.
 * 函数作用：在执行 restart 时，从旧 of postmaster.opts 中读取原启动参数以重新传递给新启动的进程。
 */
static void
read_post_opts(void)
{
	if (post_opts == NULL)
	{
		post_opts = "";			/* default - 默认值 */
		if (ctl_command == RESTART_COMMAND)
		{
			char	  **optlines;
			int			numlines;

			optlines = readfile(postopts_file, &numlines);
			if (optlines == NULL)
			{
				write_stderr(_("%s: could not read file \"%s\"\n"), progname, postopts_file);
				exit(1);
			}
			else if (numlines != 1)
			{
				write_stderr(_("%s: option file \"%s\" must have exactly one line\n"),
							 progname, postopts_file);
				exit(1);
			}
			else
			{
				char	   *optline;
				char	   *arg1;

				optline = optlines[0];

				/*
				 * Are we at the first option, as defined by space and
				 * double-quote?
				 * 我们是否处于由空格和双引号定义的第一个选项？
				 */
				if ((arg1 = strstr(optline, " \"")) != NULL)
				{
					*arg1 = '\0';	/* terminate so we get only program name - 终止以仅获取程序名 */
					post_opts = pg_strdup(arg1 + 1);	/* point past whitespace - 指向空白字符之后 */
				}
				if (exec_path == NULL)
					exec_path = pg_strdup(optline);
			}

			/* Free the results of readfile. - 释放 readfile 的结果。 */
			free_readfile(optlines);
		}
	}
}

/*
 * SIGINT signal handler used while waiting for postmaster to start up.
 * Forwards the SIGINT to the postmaster process, asking it to shut down,
 * before terminating pg_ctl itself. This way, if the user hits CTRL-C while
 * waiting for the server to start up, the server launch is aborted.
 * SIGINT signal handler used while waiting for postmaster to start up.
 * Forwards the SIGINT to the postmaster process, asking it to shut down,
 * before terminating pg_ctl itself. This way, if the user hits CTRL-C while
 * waiting for the server to start up, the server launch is aborted.
 * 在等待 postmaster 启动时使用的 SIGINT 信号处理程序。
 * 在终止 pg_ctl 自身之前，将 SIGINT 转发给 postmaster 进程，请求其关闭。
 * 这样，如果用户在等待服务器启动时按下 CTRL-C，服务器的启动将被终止。
 */
static void
trap_sigint_during_startup(SIGNAL_ARGS)
{
	if (postmasterPID != -1)
	{
		if (kill(postmasterPID, SIGINT) != 0)
			write_stderr(_("%s: could not send stop signal (PID: %d): %m\n"),
						 progname, (int) postmasterPID);
	}

	/*
	 * Clear the signal handler, and send the signal again, to terminate the
	 * process as normal.
	 * Clear the signal handler, and send the signal again, to terminate the
	 * process as normal.
	 * 清除信号处理程序，并再次发送信号，以正常终止进程。
	 */
	pqsignal(postgres_signal_arg, SIG_DFL);
	raise(postgres_signal_arg);
}

static char *
find_other_exec_or_die(const char *argv0, const char *target, const char *versionstr)
{
	int			ret;
	char	   *found_path;

	found_path = pg_malloc(MAXPGPATH);

	if ((ret = find_other_exec(argv0, target, versionstr, found_path)) < 0)
	{
		char		full_path[MAXPGPATH];

		if (find_my_exec(argv0, full_path) < 0)
			strlcpy(full_path, progname, sizeof(full_path));

		if (ret == -1)
			write_stderr(_("program \"%s\" is needed by %s but was not found in the same directory as \"%s\"\n"),
						 target, progname, full_path);
		else
			write_stderr(_("program \"%s\" was found by \"%s\" but was not the same version as %s\n"),
						 target, full_path, progname);
		exit(1);
	}

	return found_path;
}

/*
 * do_init --- Run database system initialization.
 * 函数作用：调用 initdb 进行数据库初始化。
 */
static void
do_init(void)
{
	char	   *cmd;

	if (exec_path == NULL)
		exec_path = find_other_exec_or_die(argv0, "initdb", "initdb (PostgreSQL) " PG_VERSION "\n");

	if (pgdata_opt == NULL)
		pgdata_opt = "";

	if (post_opts == NULL)
		post_opts = "";

	if (!silent_mode)
		cmd = psprintf("\"%s\" %s%s",
					   exec_path, pgdata_opt, post_opts);
	else
		cmd = psprintf("\"%s\" %s%s > \"%s\"",
					   exec_path, pgdata_opt, post_opts, DEVNULL);

	fflush(NULL);
	if (system(cmd) != 0)
	{
		write_stderr(_("%s: database system initialization failed\n"), progname);
		exit(1);
	}
}

/*
 * do_start --- Start PostgreSQL server.
 * 函数作用：检查是否运行，调用 start_postmaster，并可选地等待启动就绪。
 */
static void
do_start(void)
{
	pid_t		old_pid = 0;
	pid_t		pm_pid;

	if (ctl_command != RESTART_COMMAND)
	{
		old_pid = get_pgpid(false);
		if (old_pid != 0)
			write_stderr(_("%s: another server might be running; "
						   "trying to start server anyway\n"),
						 progname);
	}

	read_post_opts();

	/* No -D or -D already added during server start - 没有 -D 或在服务器启动期间已添加 -D */
	if (ctl_command == RESTART_COMMAND || pgdata_opt == NULL)
		pgdata_opt = "";

	if (exec_path == NULL)
		exec_path = find_other_exec_or_die(argv0, "postgres", PG_BACKEND_VERSIONSTR);

#if defined(HAVE_GETRLIMIT)
	if (allow_core_files)
		unlimit_core_size();
#endif

	/*
	 * If possible, tell the postmaster our parent shell's PID (see the
	 * comments in CreateLockFile() for motivation).  Windows hasn't got
	 * getppid() unfortunately.
	 * If possible, tell the postmaster our parent shell's PID (see the
	 * comments in CreateLockFile() for motivation).  Windows hasn't got
	 * getppid() unfortunately.
	 * 如果可能的话，告诉 postmaster 我们父 shell 的 PID（有关动机请参见 CreateLockFile() 中的注释）。
	 * 不幸的是，Windows 没有 getppid()。
	 */
#ifndef WIN32
	{
		char		env_var[32];

		snprintf(env_var, sizeof(env_var), "%d", (int) getppid());
		setenv("PG_GRANDPARENT_PID", env_var, 1);
	}
#endif

	pm_pid = start_postmaster();

	if (do_wait)
	{
		/*
		 * If the user interrupts the startup (e.g. with CTRL-C), we'd like to
		 * abort the server launch.  Install a signal handler that will
		 * forward SIGINT to the postmaster process, while we wait.
		 *
		 * (We don't bother to reset the signal handler after the launch, as
		 * we're about to exit, anyway.)
		 * If the user interrupts the startup (e.g. with CTRL-C), we'd like to
		 * abort the server launch.  Install a signal handler that will
		 * forward SIGINT to the postmaster process, while we wait.
		 *
		 * (We don't bother to reset the signal handler after the launch, as
		 * we're about to exit, anyway.)
		 * 如果用户中断了启动（例如通过 CTRL-C），我们希望中止服务器启动。
		 * 安装一个信号处理程序，在我们等待时将 SIGINT 转发给 postmaster 进程。
		 *
		 * （我们不用麻烦地在启动后重置信号处理程序，因为我们反正即将退出。）
		 */
		postmasterPID = pm_pid;
		pqsignal(SIGINT, trap_sigint_during_startup);

		print_msg(_("waiting for server to start..."));

		switch (wait_for_postmaster_start(pm_pid, false))
		{
			case POSTMASTER_READY:
				print_msg(_(" done\n"));
				print_msg(_("server started\n"));
				break;
			case POSTMASTER_STILL_STARTING:
				print_msg(_(" stopped waiting\n"));
				write_stderr(_("%s: server did not start in time\n"),
							 progname);
				exit(1);
				break;
			case POSTMASTER_SHUTDOWN_IN_RECOVERY:
				print_msg(_(" done\n"));
				print_msg(_("server shut down because of recovery target settings\n"));
				break;
			case POSTMASTER_FAILED:
				print_msg(_(" stopped waiting\n"));
				write_stderr(_("%s: could not start server\n"
							   "Examine the log output.\n"),
							 progname);
				exit(1);
				break;
		}
	}
	else
		print_msg(_("server starting\n"));

#ifdef WIN32
	/* Now we don't need the handle to the shell process anymore - 现在我们不再需要 shell 进程的句柄了 */
	CloseHandle(postmasterProcess);
	postmasterProcess = INVALID_HANDLE_VALUE;
#endif
}


/*
 * do_stop --- Stop PostgreSQL server.
 * 函数作用：读取 pid，发送停止信号（如 SIGINT / SIGTERM / SIGQUIT），并等待进程退出。
 */
static void
do_stop(void)
{
	pid_t		pid;

	pid = get_pgpid(false);

	if (pid == 0)				/* no pid file - 没有 pid 文件 */
	{
		write_stderr(_("%s: PID file \"%s\" does not exist\n"), progname, pid_file);
		write_stderr(_("Is server running?\n"));
		exit(1);
	}
	else if (pid < 0)			/* standalone backend, not postmaster - 单用户后端，而非 postmaster */
	{
		pid = -pid;
		write_stderr(_("%s: cannot stop server; "
					   "single-user server is running (PID: %d)\n"),
					 progname, (int) pid);
		exit(1);
	}

	if (kill(pid, sig) != 0)
	{
		write_stderr(_("%s: could not send stop signal (PID: %d): %m\n"), progname, (int) pid);
		exit(1);
	}

	if (!do_wait)
	{
		print_msg(_("server shutting down\n"));
		return;
	}
	else
	{
		print_msg(_("waiting for server to shut down..."));

		if (!wait_for_postmaster_stop())
		{
			print_msg(_(" failed\n"));

			write_stderr(_("%s: server does not shut down\n"), progname);
			if (shutdown_mode == SMART_MODE)
				write_stderr(_("HINT: The \"-m fast\" option immediately disconnects sessions rather than\n"
							   "waiting for session-initiated disconnection.\n"));
			exit(1);
		}
		print_msg(_(" done\n"));

		print_msg(_("server stopped\n"));
	}
}


/*
 *	restart/reload routines
 * 重启/重新加载例程
 */

/*
 * do_restart --- Restart PostgreSQL server.
 * 函数作用：先发送信号停止当前运行的 server，确认退出后，再调用 do_start 启动新实例。
 */
static void
do_restart(void)
{
	pid_t		pid;

	pid = get_pgpid(false);

	if (pid == 0)				/* no pid file - 没有 pid 文件 */
	{
		write_stderr(_("%s: PID file \"%s\" does not exist\n"),
					 progname, pid_file);
		write_stderr(_("Is server running?\n"));
		write_stderr(_("trying to start server anyway\n"));
		do_start();
		return;
	}
	else if (pid < 0)			/* standalone backend, not postmaster - 单用户后端，而非 postmaster */
	{
		pid = -pid;
		if (postmaster_is_alive(pid))
		{
			write_stderr(_("%s: cannot restart server; "
						   "single-user server is running (PID: %d)\n"),
						 progname, (int) pid);
			write_stderr(_("Please terminate the single-user server and try again.\n"));
			exit(1);
		}
	}

	if (postmaster_is_alive(pid))
	{
		if (kill(pid, sig) != 0)
		{
			write_stderr(_("%s: could not send stop signal (PID: %d): %m\n"), progname, (int) pid);
			exit(1);
		}

		print_msg(_("waiting for server to shut down..."));

		/* always wait for restart - 重启时总是进行等待 */
		if (!wait_for_postmaster_stop())
		{
			print_msg(_(" failed\n"));

			write_stderr(_("%s: server does not shut down\n"), progname);
			if (shutdown_mode == SMART_MODE)
				write_stderr(_("HINT: The \"-m fast\" option immediately disconnects sessions rather than\n"
							   "waiting for session-initiated disconnection.\n"));
			exit(1);
		}

		print_msg(_(" done\n"));
		print_msg(_("server stopped\n"));
	}
	else
	{
		write_stderr(_("%s: old server process (PID: %d) seems to be gone\n"),
					 progname, (int) pid);
		write_stderr(_("starting server anyway\n"));
	}

	do_start();
}

/*
 * do_reload --- Reload configuration.
 * 函数作用：向 postmaster 进程发送 SIGHUP 信号以促使其重新加载配置文件。
 */
static void
do_reload(void)
{
	pid_t		pid;

	pid = get_pgpid(false);
	if (pid == 0)				/* no pid file - 没有 pid 文件 */
	{
		write_stderr(_("%s: PID file \"%s\" does not exist\n"), progname, pid_file);
		write_stderr(_("Is server running?\n"));
		exit(1);
	}
	else if (pid < 0)			/* standalone backend, not postmaster - 单用户后端，而非 postmaster */
	{
		pid = -pid;
		write_stderr(_("%s: cannot reload server; "
					   "single-user server is running (PID: %d)\n"),
					 progname, (int) pid);
		write_stderr(_("Please terminate the single-user server and try again.\n"));
		exit(1);
	}

	if (kill(pid, sig) != 0)
	{
		write_stderr(_("%s: could not send reload signal (PID: %d): %m\n"),
					 progname, (int) pid);
		exit(1);
	}

	print_msg(_("server signaled\n"));
}


/*
 * promote
 * 晋升（主备切换）
 */

/*
 * do_promote --- Promote standby to primary.
 * 函数作用：写入 promote 信号文件并向 postmaster 发送 SIGUSR1 信号，促其切换为主库运行。
 */
static void
do_promote(void)
{
	FILE	   *prmfile;
	pid_t		pid;

	pid = get_pgpid(false);

	if (pid == 0)				/* no pid file - 没有 pid 文件 */
	{
		write_stderr(_("%s: PID file \"%s\" does not exist\n"), progname, pid_file);
		write_stderr(_("Is server running?\n"));
		exit(1);
	}
	else if (pid < 0)			/* standalone backend, not postmaster - 单用户后端，而非 postmaster */
	{
		pid = -pid;
		write_stderr(_("%s: cannot promote server; "
					   "single-user server is running (PID: %d)\n"),
					 progname, (int) pid);
		exit(1);
	}

	if (get_control_dbstate() != DB_IN_ARCHIVE_RECOVERY)
	{
		write_stderr(_("%s: cannot promote server; "
					   "server is not in standby mode\n"),
					 progname);
		exit(1);
	}

	snprintf(promote_file, MAXPGPATH, "%s/promote", pg_data);

	if ((prmfile = fopen(promote_file, "w")) == NULL)
	{
		write_stderr(_("%s: could not create promote signal file \"%s\": %m\n"),
					 progname, promote_file);
		exit(1);
	}
	if (fclose(prmfile))
	{
		write_stderr(_("%s: could not write promote signal file \"%s\": %m\n"),
					 progname, promote_file);
		exit(1);
	}

	sig = SIGUSR1;
	if (kill(pid, sig) != 0)
	{
		write_stderr(_("%s: could not send promote signal (PID: %d): %m\n"),
					 progname, (int) pid);
		if (unlink(promote_file) != 0)
			write_stderr(_("%s: could not remove promote signal file \"%s\": %m\n"),
						 progname, promote_file);
		exit(1);
	}

	if (do_wait)
	{
		print_msg(_("waiting for server to promote..."));
		if (wait_for_postmaster_promote())
		{
			print_msg(_(" done\n"));
			print_msg(_("server promoted\n"));
		}
		else
		{
			print_msg(_(" stopped waiting\n"));
			write_stderr(_("%s: server did not promote in time\n"),
						 progname);
			exit(1);
		}
	}
	else
		print_msg(_("server promoting\n"));
}

/*
 * log rotate
 * 日志轮转
 */

/*
 * do_logrotate --- Rotate log files.
 * 函数作用：写入 logrotate 信号文件并向 postmaster 发送 SIGUSR1 信号通知其做日志轮转。
 */
static void
do_logrotate(void)
{
	FILE	   *logrotatefile;
	pid_t		pid;

	pid = get_pgpid(false);

	if (pid == 0)				/* no pid file - 没有 pid 文件 */
	{
		write_stderr(_("%s: PID file \"%s\" does not exist\n"), progname, pid_file);
		write_stderr(_("Is server running?\n"));
		exit(1);
	}
	else if (pid < 0)			/* standalone backend, not postmaster - 单用户后端，而非 postmaster */
	{
		pid = -pid;
		write_stderr(_("%s: cannot rotate log file; "
					   "single-user server is running (PID: %d)\n"),
					 progname, (int) pid);
		exit(1);
	}

	snprintf(logrotate_file, MAXPGPATH, "%s/logrotate", pg_data);

	if ((logrotatefile = fopen(logrotate_file, "w")) == NULL)
	{
		write_stderr(_("%s: could not create log rotation signal file \"%s\": %m\n"),
					 progname, logrotate_file);
		exit(1);
	}
	if (fclose(logrotatefile))
	{
		write_stderr(_("%s: could not write log rotation signal file \"%s\": %m\n"),
					 progname, logrotate_file);
		exit(1);
	}

	sig = SIGUSR1;
	if (kill(pid, sig) != 0)
	{
		write_stderr(_("%s: could not send log rotation signal (PID: %d): %m\n"),
					 progname, (int) pid);
		if (unlink(logrotate_file) != 0)
			write_stderr(_("%s: could not remove log rotation signal file \"%s\": %m\n"),
						 progname, logrotate_file);
		exit(1);
	}

	print_msg(_("server signaled to rotate log file\n"));
}


/*
 *	utility routines
 * 工具函数例程
 */

static bool
postmaster_is_alive(pid_t pid)
{
	/*
	 * Test to see if the process is still there.  Note that we do not
	 * consider an EPERM failure to mean that the process is still there;
	 * EPERM must mean that the given PID belongs to some other userid, and
	 * considering the permissions on $PGDATA, that means it's not the
	 * postmaster we are after.
	 *
	 * Don't believe that our own PID or parent shell's PID is the postmaster,
	 * either.  (Windows hasn't got getppid(), though.)
	 * Test to see if the process is still there.  Note that we do not
	 * consider an EPERM failure to mean that the process is still there;
	 * EPERM must mean that the given PID belongs to some other userid, and
	 * considering the permissions on $PGDATA, that means it's not the
	 * postmaster we are after.
	 *
	 * Don't believe that our own PID or parent shell's PID is the postmaster,
	 * either.  (Windows hasn't got getppid(), though.)
	 * 测试进程是否仍然存在。请注意，我们不认为 EPERM 失败意味着进程仍在；
	 * EPERM 必定意味着给定的 PID 属于其他某个用户，考虑到 $PGDATA 的权限，
	 * 这意味着它不是我们所寻找的 postmaster。
	 *
	 * 也不要相信我们自己的 PID 或父 shell 的 PID 是 postmaster。（虽然 Windows 没有 getppid()。）
	 */
	if (pid == getpid())
		return false;
#ifndef WIN32
	if (pid == getppid())
		return false;
#endif
	if (kill(pid, 0) == 0)
		return true;
	return false;
}

/*
 * do_status --- Show running status of PostgreSQL server.
 * 函数作用：检查 pid 和对应的进程是否存在，并打印服务器运行状态及 PID 等信息。
 */
static void
do_status(void)
{
	pid_t		pid;

	pid = get_pgpid(true);
	/* Is there a pid file? - 是否存在 pid 文件？ */
	if (pid != 0)
	{
		/* standalone backend? - 是否为单用户后端？ */
		if (pid < 0)
		{
			pid = -pid;
			if (postmaster_is_alive(pid))
			{
				printf(_("%s: single-user server is running (PID: %d)\n"),
					   progname, (int) pid);
				return;
			}
		}
		else
			/* must be a postmaster - 必定是 postmaster */
		{
			if (postmaster_is_alive(pid))
			{
				char	  **optlines;
				char	  **curr_line;
				int			numlines;

				printf(_("%s: server is running (PID: %d)\n"),
					   progname, (int) pid);

				optlines = readfile(postopts_file, &numlines);
				if (optlines != NULL)
				{
					for (curr_line = optlines; *curr_line != NULL; curr_line++)
						puts(*curr_line);

					/* Free the results of readfile - 释放 readfile 的结果 */
					free_readfile(optlines);
				}
				return;
			}
		}
	}
	printf(_("%s: no server running\n"), progname);

	/*
	 * The Linux Standard Base Core Specification 3.1 says this should return
	 * '3, program is not running'
	 * https://refspecs.linuxbase.org/LSB_3.1.0/LSB-Core-generic/LSB-Core-generic/iniscrptact.html
	 * The Linux Standard Base Core Specification 3.1 says this should return
	 * '3, program is not running'
	 * https://refspecs.linuxbase.org/LSB_3.1.0/LSB-Core-generic/LSB-Core-generic/iniscrptact.html
	 * Linux Standard Base Core Specification 3.1 指出这应该返回
	 * '3，程序未运行'
	 * https://refspecs.linuxbase.org/LSB_3.1.0/LSB-Core-generic/LSB-Core-generic/iniscrptact.html
	 */
	exit(3);
}



/*
 * do_kill --- Send custom signal to process.
 * 函数作用：向特定进程发送用户指定的信号。
 */
static void
do_kill(pid_t pid)
{
	if (kill(pid, sig) != 0)
	{
		write_stderr(_("%s: could not send signal %d (PID: %d): %m\n"),
					 progname, sig, (int) pid);
		exit(1);
	}
}

#ifdef WIN32

static bool
pgwin32_IsInstalled(SC_HANDLE hSCM)
{
	SC_HANDLE	hService = OpenService(hSCM, register_servicename, SERVICE_QUERY_CONFIG);
	bool		bResult = (hService != NULL);

	if (bResult)
		CloseServiceHandle(hService);
	return bResult;
}

static char *
pgwin32_CommandLine(bool registration)
{
	PQExpBuffer cmdLine = createPQExpBuffer();
	char		cmdPath[MAXPGPATH];
	int			ret;

	if (registration)
	{
		ret = find_my_exec(argv0, cmdPath);
		if (ret != 0)
		{
			write_stderr(_("%s: could not find own program executable\n"), progname);
			exit(1);
		}
	}
	else
	{
		ret = find_other_exec(argv0, "postgres", PG_BACKEND_VERSIONSTR,
							  cmdPath);
		if (ret != 0)
		{
			write_stderr(_("%s: could not find postgres program executable\n"), progname);
			exit(1);
		}
	}

	/* if path does not end in .exe, append it - 如果路径不以 .exe 结尾，则追加它 */
	if (strlen(cmdPath) < 4 ||
		pg_strcasecmp(cmdPath + strlen(cmdPath) - 4, ".exe") != 0)
		snprintf(cmdPath + strlen(cmdPath), sizeof(cmdPath) - strlen(cmdPath),
				 ".exe");

	/* use backslashes in path to avoid problems with some third-party tools - 在路径中使用反斜杠以避免一些第三方工具的问题 */
	make_native_path(cmdPath);

	/* be sure to double-quote the executable's name in the command - 务必在命令中对可执行文件名使用双引号 */
	appendPQExpBuffer(cmdLine, "\"%s\"", cmdPath);

	/* append assorted switches to the command line, as needed - 根据需要将各种开关追加到命令行 */

	if (registration)
		appendPQExpBuffer(cmdLine, " runservice -N \"%s\"",
						  register_servicename);

	if (pg_config)
	{
		/* We need the -D path to be absolute - 我们需要 -D 路径为绝对路径 */
		char	   *dataDir;

		if ((dataDir = make_absolute_path(pg_config)) == NULL)
		{
			/* make_absolute_path already reported the error - make_absolute_path 已经报告了错误 */
			exit(1);
		}
		make_native_path(dataDir);
		appendPQExpBuffer(cmdLine, " -D \"%s\"", dataDir);
		free(dataDir);
	}

	if (registration && event_source != NULL)
		appendPQExpBuffer(cmdLine, " -e \"%s\"", event_source);

	if (registration && do_wait)
		appendPQExpBufferStr(cmdLine, " -w");

	/* Don't propagate a value from an environment variable. - 不要从环境变量中传递该值。 */
	if (registration && wait_seconds_arg && wait_seconds != DEFAULT_WAIT)
		appendPQExpBuffer(cmdLine, " -t %d", wait_seconds);

	if (registration && silent_mode)
		appendPQExpBufferStr(cmdLine, " -s");

	if (post_opts)
	{
		if (registration)
			appendPQExpBuffer(cmdLine, " -o \"%s\"", post_opts);
		else
			appendPQExpBuffer(cmdLine, " %s", post_opts);
	}

	return cmdLine->data;
}

static void
pgwin32_doRegister(void)
{
	SC_HANDLE	hService;
	SC_HANDLE	hSCM = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);

	if (hSCM == NULL)
	{
		write_stderr(_("%s: could not open service manager\n"), progname);
		exit(1);
	}
	if (pgwin32_IsInstalled(hSCM))
	{
		CloseServiceHandle(hSCM);
		write_stderr(_("%s: service \"%s\" already registered\n"), progname, register_servicename);
		exit(1);
	}

	if ((hService = CreateService(hSCM, register_servicename, register_servicename,
								  SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
								  pgctl_start_type, SERVICE_ERROR_NORMAL,
								  pgwin32_CommandLine(true),
								  NULL, NULL, "RPCSS\0", register_username, register_password)) == NULL)
	{
		CloseServiceHandle(hSCM);
		write_stderr(_("%s: could not register service \"%s\": error code %lu\n"),
					 progname, register_servicename,
					 (unsigned long) GetLastError());
		exit(1);
	}
	CloseServiceHandle(hService);
	CloseServiceHandle(hSCM);
}

static void
pgwin32_doUnregister(void)
{
	SC_HANDLE	hService;
	SC_HANDLE	hSCM = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);

	if (hSCM == NULL)
	{
		write_stderr(_("%s: could not open service manager\n"), progname);
		exit(1);
	}
	if (!pgwin32_IsInstalled(hSCM))
	{
		CloseServiceHandle(hSCM);
		write_stderr(_("%s: service \"%s\" not registered\n"), progname, register_servicename);
		exit(1);
	}

	if ((hService = OpenService(hSCM, register_servicename, DELETE)) == NULL)
	{
		CloseServiceHandle(hSCM);
		write_stderr(_("%s: could not open service \"%s\": error code %lu\n"),
					 progname, register_servicename,
					 (unsigned long) GetLastError());
		exit(1);
	}
	if (!DeleteService(hService))
	{
		CloseServiceHandle(hService);
		CloseServiceHandle(hSCM);
		write_stderr(_("%s: could not unregister service \"%s\": error code %lu\n"),
					 progname, register_servicename,
					 (unsigned long) GetLastError());
		exit(1);
	}
	CloseServiceHandle(hService);
	CloseServiceHandle(hSCM);
}

static void
pgwin32_SetServiceStatus(DWORD currentState)
{
	status.dwCurrentState = currentState;
	SetServiceStatus(hStatus, (LPSERVICE_STATUS) &status);
}

static void WINAPI
pgwin32_ServiceHandler(DWORD request)
{
	switch (request)
	{
		case SERVICE_CONTROL_STOP:
		case SERVICE_CONTROL_SHUTDOWN:

			/*
			 * We only need a short wait hint here as it just needs to wait
			 * for the next checkpoint. They occur every 5 seconds during
			 * shutdown
			 * We only need a short wait hint here as it just needs to wait
			 * for the next checkpoint. They occur every 5 seconds during
			 * shutdown
			 * 我们这里只需要一个简短的等待提示，因为它只需要等待下一个检查点。
			 * 在关机期间，它们每 5 秒发生一次
			 */
			status.dwWaitHint = 10000;
			pgwin32_SetServiceStatus(SERVICE_STOP_PENDING);
			SetEvent(shutdownEvent);
			return;

		case SERVICE_CONTROL_PAUSE:
			/* Win32 config reloading - Win32 配置重新加载 */
			status.dwWaitHint = 5000;
			kill(postmasterPID, SIGHUP);
			return;

			/* FIXME: These could be used to replace other signals etc - FIXME: 这些可以用来替换其他信号等 */
		case SERVICE_CONTROL_CONTINUE:
		case SERVICE_CONTROL_INTERROGATE:
		default:
			break;
	}
}

static void WINAPI
pgwin32_ServiceMain(DWORD argc, LPTSTR *argv)
{
	PROCESS_INFORMATION pi;
	DWORD		ret;

	/* Initialize variables - 初始化变量 */
	status.dwWin32ExitCode = S_OK;
	status.dwCheckPoint = 0;
	status.dwWaitHint = 60000;
	status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
	status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_PAUSE_CONTINUE;
	status.dwServiceSpecificExitCode = 0;
	status.dwCurrentState = SERVICE_START_PENDING;

	memset(&pi, 0, sizeof(pi));

	read_post_opts();

	/* Register the control request handler - 注册控制请求处理程序 */
	if ((hStatus = RegisterServiceCtrlHandler(register_servicename, pgwin32_ServiceHandler)) == (SERVICE_STATUS_HANDLE) 0)
		return;

	if ((shutdownEvent = CreateEvent(NULL, true, false, NULL)) == NULL)
		return;

	/* Start the postmaster - 启动 postmaster */
	pgwin32_SetServiceStatus(SERVICE_START_PENDING);
	if (!CreateRestrictedProcess(pgwin32_CommandLine(false), &pi, true))
	{
		pgwin32_SetServiceStatus(SERVICE_STOPPED);
		return;
	}
	postmasterPID = pi.dwProcessId;
	postmasterProcess = pi.hProcess;
	CloseHandle(pi.hThread);

	if (do_wait)
	{
		write_eventlog(EVENTLOG_INFORMATION_TYPE, _("Waiting for server startup...\n"));
		if (wait_for_postmaster_start(postmasterPID, true) != POSTMASTER_READY)
		{
			write_eventlog(EVENTLOG_ERROR_TYPE, _("Timed out waiting for server startup\n"));
			pgwin32_SetServiceStatus(SERVICE_STOPPED);
			return;
		}
		write_eventlog(EVENTLOG_INFORMATION_TYPE, _("Server started and accepting connections\n"));
	}

	pgwin32_SetServiceStatus(SERVICE_RUNNING);

	/* Wait for quit... - 等待退出... */
	ret = WaitForMultipleObjects(2, shutdownHandles, FALSE, INFINITE);

	pgwin32_SetServiceStatus(SERVICE_STOP_PENDING);
	switch (ret)
	{
		case WAIT_OBJECT_0:		/* shutdown event - 关闭事件 */
			{
				/*
				 * status.dwCheckPoint can be incremented by
				 * wait_for_postmaster_start(), so it might not start from 0.
				 * status.dwCheckPoint can be incremented by
				 * wait_for_postmaster_start(), so it might not start from 0.
				 * status.dwCheckPoint 可由 wait_for_postmaster_start() 递增，因此它可能不从 0 开始。
				 */
				int			maxShutdownCheckPoint = status.dwCheckPoint + 12;

				kill(postmasterPID, SIGINT);

				/*
				 * Increment the checkpoint and try again. Abort after 12
				 * checkpoints as the postmaster has probably hung.
				 * Increment the checkpoint and try again. Abort after 12
				 * checkpoints as the postmaster has probably hung.
				 * 递增检查点并重试。在 12 个检查点后中止，因为 postmaster 可能已经挂起。
				 */
				while (WaitForSingleObject(postmasterProcess, 5000) == WAIT_TIMEOUT && status.dwCheckPoint < maxShutdownCheckPoint)
				{
					status.dwCheckPoint++;
					SetServiceStatus(hStatus, (LPSERVICE_STATUS) &status);
				}
				break;
			}

		case (WAIT_OBJECT_0 + 1):	/* postmaster went down - postmaster 下线了 */
			break;

		default:
			/* shouldn't get here? - 不应该到达这里？ */
			break;
	}

	CloseHandle(shutdownEvent);
	CloseHandle(postmasterProcess);

	pgwin32_SetServiceStatus(SERVICE_STOPPED);
}

static void
pgwin32_doRunAsService(void)
{
	SERVICE_TABLE_ENTRY st[] = {{register_servicename, pgwin32_ServiceMain},
	{NULL, NULL}};

	if (StartServiceCtrlDispatcher(st) == 0)
	{
		write_stderr(_("%s: could not start service \"%s\": error code %lu\n"),
					 progname, register_servicename,
					 (unsigned long) GetLastError());
		exit(1);
	}
}


/*
 * Set up STARTUPINFO for the new process to inherit this process' handles.
 *
 * Process started as services appear to have "empty" handles (GetStdHandle()
 * returns NULL) rather than invalid ones. But passing down NULL ourselves
 * doesn't work, it's interpreted as STARTUPINFO->hStd* not being set. But we
 * can pass down INVALID_HANDLE_VALUE - which makes GetStdHandle() in the new
 * process (and its child processes!) return INVALID_HANDLE_VALUE. Which
 * achieves the goal of postmaster running in a similar environment as pg_ctl.
 * Set up STARTUPINFO for the new process to inherit this process' handles.
 *
 * Process started as services appear to have "empty" handles (GetStdHandle()
 * returns NULL) rather than invalid ones. But passing down NULL ourselves
 * doesn't work, it's interpreted as STARTUPINFO->hStd* not being set. But we
 * can pass down INVALID_HANDLE_VALUE - which makes GetStdHandle() in the new
 * process (and its child processes!) return INVALID_HANDLE_VALUE. Which
 * achieves the goal of postmaster running in a similar environment as pg_ctl.
 * 为新进程设置 STARTUPINFO，以便继承此进程的句柄。
 * 作为服务启动的进程似乎具有“空”句柄（GetStdHandle() 返回 NULL）而不是无效句柄。
 * 但我们自己传递 NULL 是行不通的，它会被解释为 STARTUPINFO->hStd* 未设置。
 * 但我们可以传递 INVALID_HANDLE_VALUE - 这使得新进程（及其子进程！）中的
 * GetStdHandle() 返回 INVALID_HANDLE_VALUE。这达到了在类似于 pg_ctl 的环境中运行 postmaster 的目标。
 */
static void
InheritStdHandles(STARTUPINFO *si)
{
	si->dwFlags |= STARTF_USESTDHANDLES;
	si->hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	if (si->hStdInput == NULL)
		si->hStdInput = INVALID_HANDLE_VALUE;
	si->hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
	if (si->hStdOutput == NULL)
		si->hStdOutput = INVALID_HANDLE_VALUE;
	si->hStdError = GetStdHandle(STD_ERROR_HANDLE);
	if (si->hStdError == NULL)
		si->hStdError = INVALID_HANDLE_VALUE;
}

/*
 * Create a restricted token, a job object sandbox, and execute the specified
 * process with it.
 *
 * Returns 0 on success, non-zero on failure, same as CreateProcess().
 *
 * NOTE! Job object will only work when running as a service, because it's
 * automatically destroyed when pg_ctl exits.
 * Create a restricted token, a job object sandbox, and execute the specified
 * process with it.
 *
 * Returns 0 on success, non-zero on failure, same as CreateProcess().
 *
 * NOTE! Job object will only work when running as a service, because it's
 * automatically destroyed when pg_ctl exits.
 * 创建受限令牌、作业对象沙箱，并使用它执行指定的进程。
 * 成功时返回 0，失败时返回非零值，与 CreateProcess() 相同。
 * 注意！作业对象仅在作为服务运行时有效，因为它在 pg_ctl 退出时会自动销毁。
 */
static int
CreateRestrictedProcess(char *cmd, PROCESS_INFORMATION *processInfo, bool as_service)
{
	int			r;
	BOOL		b;
	STARTUPINFO si;
	HANDLE		origToken;
	HANDLE		restrictedToken;
	BOOL		inJob;
	SID_IDENTIFIER_AUTHORITY NtAuthority = {SECURITY_NT_AUTHORITY};
	SID_AND_ATTRIBUTES dropSids[2];
	PTOKEN_PRIVILEGES delPrivs;

	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);

	/*
	 * Set stdin/stdout/stderr handles to be inherited in the child process.
	 * That allows postmaster and the processes it starts to perform
	 * additional checks to see if running in a service (otherwise they get
	 * the default console handles - which point to "somewhere").
	 * 默认值
	 */
	InheritStdHandles(&si);

	/* Open the current token to use as a base for the restricted one - 打开当前令牌以用作受限令牌的基础 */
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &origToken))
	{
		/*
		 * Most Windows targets make DWORD a 32-bit unsigned long, but in case
		 * it doesn't cast DWORD before printing.
		 * Most Windows targets make DWORD a 32-bit unsigned long, but in case
		 * it doesn't cast DWORD before printing.
		 * 大多数 Windows 目标将 DWORD 设为 32 位无符号长整型，但以防万一在打印前强制转换它。
		 */
		write_stderr(_("%s: could not open process token: error code %lu\n"),
					 progname, (unsigned long) GetLastError());
		return 0;
	}

	/* Allocate list of SIDs to remove - 分配要移除的 SID 列表 */
	ZeroMemory(&dropSids, sizeof(dropSids));
	if (!AllocateAndInitializeSid(&NtAuthority, 2,
								  SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0,
								  0, &dropSids[0].Sid) ||
		!AllocateAndInitializeSid(&NtAuthority, 2,
								  SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_POWER_USERS, 0, 0, 0, 0, 0,
								  0, &dropSids[1].Sid))
	{
		write_stderr(_("%s: could not allocate SIDs: error code %lu\n"),
					 progname, (unsigned long) GetLastError());
		return 0;
	}

	/* Get list of privileges to remove - 获取要移除的特权列表 */
	delPrivs = GetPrivilegesToDelete(origToken);
	if (delPrivs == NULL)
		/* Error message already printed - 错误消息已打印 */
		return 0;

	b = CreateRestrictedToken(origToken,
							  0,
							  sizeof(dropSids) / sizeof(dropSids[0]),
							  dropSids,
							  delPrivs->PrivilegeCount, delPrivs->Privileges,
							  0, NULL,
							  &restrictedToken);

	free(delPrivs);
	FreeSid(dropSids[1].Sid);
	FreeSid(dropSids[0].Sid);
	CloseHandle(origToken);

	if (!b)
	{
		write_stderr(_("%s: could not create restricted token: error code %lu\n"),
					 progname, (unsigned long) GetLastError());
		return 0;
	}

	AddUserToTokenDacl(restrictedToken);
	r = CreateProcessAsUser(restrictedToken, NULL, cmd, NULL, NULL, TRUE, CREATE_SUSPENDED, NULL, NULL, &si, processInfo);

	if (IsProcessInJob(processInfo->hProcess, NULL, &inJob))
	{
		if (!inJob)
		{
			/*
			 * Job objects are working, and the new process isn't in one, so
			 * we can create one safely. If any problems show up when setting
			 * it, we're going to ignore them.
			 * Job objects are working, and the new process isn't in one, so
			 * we can create one safely. If any problems show up when setting
			 * it, we're going to ignore them.
			 * 作业对象可用，并且新进程不在其中，因此我们可以安全地创建一个。如果在设置它时出现任何问题，我们将忽略它们。
			 */
			HANDLE		job;
			char		jobname[128];

			sprintf(jobname, "PostgreSQL_%lu",
					(unsigned long) processInfo->dwProcessId);

			job = CreateJobObject(NULL, jobname);
			if (job)
			{
				JOBOBJECT_BASIC_LIMIT_INFORMATION basicLimit;
				JOBOBJECT_BASIC_UI_RESTRICTIONS uiRestrictions;
				JOBOBJECT_SECURITY_LIMIT_INFORMATION securityLimit;

				ZeroMemory(&basicLimit, sizeof(basicLimit));
				ZeroMemory(&uiRestrictions, sizeof(uiRestrictions));
				ZeroMemory(&securityLimit, sizeof(securityLimit));

				basicLimit.LimitFlags = JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION | JOB_OBJECT_LIMIT_PRIORITY_CLASS;
				basicLimit.PriorityClass = NORMAL_PRIORITY_CLASS;
				SetInformationJobObject(job, JobObjectBasicLimitInformation, &basicLimit, sizeof(basicLimit));

				uiRestrictions.UIRestrictionsClass = JOB_OBJECT_UILIMIT_DESKTOP | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS |
					JOB_OBJECT_UILIMIT_EXITWINDOWS | JOB_OBJECT_UILIMIT_READCLIPBOARD |
					JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS | JOB_OBJECT_UILIMIT_WRITECLIPBOARD;

				SetInformationJobObject(job, JobObjectBasicUIRestrictions, &uiRestrictions, sizeof(uiRestrictions));

				securityLimit.SecurityLimitFlags = JOB_OBJECT_SECURITY_NO_ADMIN | JOB_OBJECT_SECURITY_ONLY_TOKEN;
				securityLimit.JobToken = restrictedToken;
				SetInformationJobObject(job, JobObjectSecurityLimitInformation, &securityLimit, sizeof(securityLimit));

				AssignProcessToJobObject(job, processInfo->hProcess);
			}
		}
	}

	CloseHandle(restrictedToken);

	ResumeThread(processInfo->hThread);

	/*
	 * We intentionally don't close the job object handle, because we want the
	 * object to live on until pg_ctl shuts down.
	 * We intentionally don't close the job object handle, because we want the
	 * object to live on until pg_ctl shuts down.
	 * 我们故意不关闭作业对象句柄，因为我们希望该对象一直存活到 pg_ctl 关闭。
	 */
	return r;
}

/*
 * Get a list of privileges to delete from the access token. We delete all privileges
 * except SeLockMemoryPrivilege which is needed to use large pages, and
 * SeChangeNotifyPrivilege which is enabled by default in DISABLE_MAX_PRIVILEGE.
 * 默认值
 */
static PTOKEN_PRIVILEGES
GetPrivilegesToDelete(HANDLE hToken)
{
	int			i,
				j;
	DWORD		length;
	PTOKEN_PRIVILEGES tokenPrivs;
	LUID		luidLockPages;
	LUID		luidChangeNotify;

	if (!LookupPrivilegeValue(NULL, SE_LOCK_MEMORY_NAME, &luidLockPages) ||
		!LookupPrivilegeValue(NULL, SE_CHANGE_NOTIFY_NAME, &luidChangeNotify))
	{
		write_stderr(_("%s: could not get LUIDs for privileges: error code %lu\n"),
					 progname, (unsigned long) GetLastError());
		return NULL;
	}

	if (!GetTokenInformation(hToken, TokenPrivileges, NULL, 0, &length) &&
		GetLastError() != ERROR_INSUFFICIENT_BUFFER)
	{
		write_stderr(_("%s: could not get token information: error code %lu\n"),
					 progname, (unsigned long) GetLastError());
		return NULL;
	}

	tokenPrivs = (PTOKEN_PRIVILEGES) pg_malloc_extended(length,
														MCXT_ALLOC_NO_OOM);
	if (tokenPrivs == NULL)
	{
		write_stderr(_("%s: out of memory\n"), progname);
		return NULL;
	}

	if (!GetTokenInformation(hToken, TokenPrivileges, tokenPrivs, length, &length))
	{
		write_stderr(_("%s: could not get token information: error code %lu\n"),
					 progname, (unsigned long) GetLastError());
		free(tokenPrivs);
		return NULL;
	}

	for (i = 0; i < tokenPrivs->PrivilegeCount; i++)
	{
		if (memcmp(&tokenPrivs->Privileges[i].Luid, &luidLockPages, sizeof(LUID)) == 0 ||
			memcmp(&tokenPrivs->Privileges[i].Luid, &luidChangeNotify, sizeof(LUID)) == 0)
		{
			for (j = i; j < tokenPrivs->PrivilegeCount - 1; j++)
				tokenPrivs->Privileges[j] = tokenPrivs->Privileges[j + 1];
			tokenPrivs->PrivilegeCount--;
		}
	}

	return tokenPrivs;
}
#endif							/* WIN32 - WIN32 平台 */

static void
do_advice(void)
{
	write_stderr(_("Try \"%s --help\" for more information.\n"), progname);
}



static void
do_help(void)
{
	printf(_("%s is a utility to initialize, start, stop, or control a PostgreSQL server.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s init[db]   [-D DATADIR] [-s] [-o OPTIONS]\n"), progname);
	printf(_("  %s start      [-D DATADIR] [-l FILENAME] [-W] [-t SECS] [-s]\n"
			 "                    [-o OPTIONS] [-p PATH] [-c]\n"), progname);
	printf(_("  %s stop       [-D DATADIR] [-m SHUTDOWN-MODE] [-W] [-t SECS] [-s]\n"), progname);
	printf(_("  %s restart    [-D DATADIR] [-m SHUTDOWN-MODE] [-W] [-t SECS] [-s]\n"
			 "                    [-o OPTIONS] [-c]\n"), progname);
	printf(_("  %s reload     [-D DATADIR] [-s]\n"), progname);
	printf(_("  %s status     [-D DATADIR]\n"), progname);
	printf(_("  %s promote    [-D DATADIR] [-W] [-t SECS] [-s]\n"), progname);
	printf(_("  %s logrotate  [-D DATADIR] [-s]\n"), progname);
	printf(_("  %s kill       SIGNALNAME PID\n"), progname);
#ifdef WIN32
	printf(_("  %s register   [-D DATADIR] [-N SERVICENAME] [-U USERNAME] [-P PASSWORD]\n"
			 "                    [-S START-TYPE] [-e SOURCE] [-W] [-t SECS] [-s] [-o OPTIONS]\n"), progname);
	printf(_("  %s unregister [-N SERVICENAME]\n"), progname);
#endif

	printf(_("\nCommon options:\n"));
	printf(_("  -D, --pgdata=DATADIR   location of the database storage area\n"));
#ifdef WIN32
	printf(_("  -e SOURCE              event source for logging when running as a service\n"));
#endif
	printf(_("  -s, --silent           only print errors, no informational messages\n"));
	printf(_("  -t, --timeout=SECS     seconds to wait when using -w option\n"));
	printf(_("  -V, --version          output version information, then exit\n"));
	printf(_("  -w, --wait             wait until operation completes (default)\n"));
	printf(_("  -W, --no-wait          do not wait until operation completes\n"));
	printf(_("  -?, --help             show this help, then exit\n"));
	printf(_("If the -D option is omitted, the environment variable PGDATA is used.\n"));

	printf(_("\nOptions for start or restart:\n"));
#if defined(HAVE_GETRLIMIT)
	printf(_("  -c, --core-files       allow postgres to produce core files\n"));
#else
	printf(_("  -c, --core-files       not applicable on this platform\n"));
#endif
	printf(_("  -l, --log=FILENAME     write (or append) server log to FILENAME\n"));
	printf(_("  -o, --options=OPTIONS  command line options to pass to postgres\n"
			 "                         (PostgreSQL server executable) or initdb\n"));
	printf(_("  -p PATH-TO-POSTGRES    normally not necessary\n"));
	printf(_("\nOptions for stop or restart:\n"));
	printf(_("  -m, --mode=MODE        MODE can be \"smart\", \"fast\", or \"immediate\"\n"));

	printf(_("\nShutdown modes are:\n"));
	printf(_("  smart       quit after all clients have disconnected\n"));
	printf(_("  fast        quit directly, with proper shutdown (default)\n"));
	printf(_("  immediate   quit without complete shutdown; will lead to recovery on restart\n"));

	printf(_("\nAllowed signal names for kill:\n"));
	printf("  ABRT HUP INT KILL QUIT TERM USR1 USR2\n");

#ifdef WIN32
	printf(_("\nOptions for register and unregister:\n"));
	printf(_("  -N SERVICENAME  service name with which to register PostgreSQL server\n"));
	printf(_("  -P PASSWORD     password of account to register PostgreSQL server\n"));
	printf(_("  -U USERNAME     user name of account to register PostgreSQL server\n"));
	printf(_("  -S START-TYPE   service start type to register PostgreSQL server\n"));

	printf(_("\nStart types are:\n"));
	printf(_("  auto       start service automatically during system startup (default)\n"));
	printf(_("  demand     start service on demand\n"));
#endif

	printf(_("\nReport bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}



static void
set_mode(char *modeopt)
{
	if (strcmp(modeopt, "s") == 0 || strcmp(modeopt, "smart") == 0)
	{
		shutdown_mode = SMART_MODE;
		sig = SIGTERM;
	}
	else if (strcmp(modeopt, "f") == 0 || strcmp(modeopt, "fast") == 0)
	{
		shutdown_mode = FAST_MODE;
		sig = SIGINT;
	}
	else if (strcmp(modeopt, "i") == 0 || strcmp(modeopt, "immediate") == 0)
	{
		shutdown_mode = IMMEDIATE_MODE;
		sig = SIGQUIT;
	}
	else
	{
		write_stderr(_("%s: unrecognized shutdown mode \"%s\"\n"), progname, modeopt);
		do_advice();
		exit(1);
	}
}



static void
set_sig(char *signame)
{
	if (strcmp(signame, "HUP") == 0)
		sig = SIGHUP;
	else if (strcmp(signame, "INT") == 0)
		sig = SIGINT;
	else if (strcmp(signame, "QUIT") == 0)
		sig = SIGQUIT;
	else if (strcmp(signame, "ABRT") == 0)
		sig = SIGABRT;
	else if (strcmp(signame, "KILL") == 0)
		sig = SIGKILL;
	else if (strcmp(signame, "TERM") == 0)
		sig = SIGTERM;
	else if (strcmp(signame, "USR1") == 0)
		sig = SIGUSR1;
	else if (strcmp(signame, "USR2") == 0)
		sig = SIGUSR2;
	else
	{
		write_stderr(_("%s: unrecognized signal name \"%s\"\n"), progname, signame);
		do_advice();
		exit(1);
	}
}


#ifdef WIN32
static void
set_starttype(char *starttypeopt)
{
	if (strcmp(starttypeopt, "a") == 0 || strcmp(starttypeopt, "auto") == 0)
		pgctl_start_type = SERVICE_AUTO_START;
	else if (strcmp(starttypeopt, "d") == 0 || strcmp(starttypeopt, "demand") == 0)
		pgctl_start_type = SERVICE_DEMAND_START;
	else
	{
		write_stderr(_("%s: unrecognized start type \"%s\"\n"), progname, starttypeopt);
		do_advice();
		exit(1);
	}
}
#endif

/*
 * adjust_data_dir
 *
 * If a configuration-only directory was specified, find the real data dir.
 * adjust_data_dir
 *
 * If a configuration-only directory was specified, find the real data dir.
 * 调整数据目录。如果指定了仅配置目录，找到真实的数据目录。
 */
/*
 * adjust_data_dir --- Find real data directory if config-only dir specified.
 * 函数作用：检查是否为仅配置目录，若是则调用 postgres -C data_directory 命令获取真实 PGDATA。
 */
static void
adjust_data_dir(void)
{
	char		filename[MAXPGPATH];
	char	   *my_exec_path,
			   *cmd;
	FILE	   *fd;

	/* do nothing if we're working without knowledge of data dir - 如果是在不知道数据目录的情况下工作，则什么都不做 */
	if (pg_config == NULL)
		return;

	/* If there is no postgresql.conf, it can't be a config-only dir - 如果没有 postgresql.conf，则它不可能是仅配置目录 */
	snprintf(filename, sizeof(filename), "%s/postgresql.conf", pg_config);
	if ((fd = fopen(filename, "r")) == NULL)
		return;
	fclose(fd);

	/* If PG_VERSION exists, it can't be a config-only dir - 如果 PG_VERSION 存在，则它不可能是仅配置目录 */
	snprintf(filename, sizeof(filename), "%s/PG_VERSION", pg_config);
	if ((fd = fopen(filename, "r")) != NULL)
	{
		fclose(fd);
		return;
	}

	/* Must be a configuration directory, so find the data directory - 必定是配置目录，因此查找数据目录 */

	/* we use a private my_exec_path to avoid interfering with later uses - 我们使用私有的 my_exec_path 以避免干扰以后的使用 */
	if (exec_path == NULL)
		my_exec_path = find_other_exec_or_die(argv0, "postgres", PG_BACKEND_VERSIONSTR);
	else
		my_exec_path = pg_strdup(exec_path);

	/* it's important for -C to be the first option, see main.c - -C 作为第一个选项非常重要，参见 main.c */
	cmd = psprintf("\"%s\" -C data_directory %s%s",
				   my_exec_path,
				   pgdata_opt ? pgdata_opt : "",
				   post_opts ? post_opts : "");
	fflush(NULL);

	fd = popen(cmd, "r");
	if (fd == NULL || fgets(filename, sizeof(filename), fd) == NULL || pclose(fd) != 0)
	{
		write_stderr(_("%s: could not determine the data directory using command \"%s\"\n"), progname, cmd);
		exit(1);
	}
	free(my_exec_path);

	/* strip trailing newline and carriage return - 去除尾随的换行符和回车符 */
	(void) pg_strip_crlf(filename);

	free(pg_data);
	pg_data = pg_strdup(filename);
	canonicalize_path(pg_data);
}


/*
 * get_control_dbstate --- Read DB state from global pg_control file.
 * 函数作用：从 pg_control 提取底层数据库运行状态，例如是否处于归档恢复中（DB_IN_ARCHIVE_RECOVERY）。
 */
static DBState
get_control_dbstate(void)
{
	DBState		ret;
	bool		crc_ok;
	ControlFileData *control_file_data = get_controlfile(pg_data, &crc_ok);

	if (!crc_ok)
	{
		write_stderr(_("%s: control file appears to be corrupt\n"), progname);
		exit(1);
	}

	ret = control_file_data->state;
	pfree(control_file_data);
	return ret;
}


/*
 * main --- Core entry point.
 * 核心流程解释：
 * 1. 初始化日志、语言环境和启动时间；
 * 2. 检查并阻止 root 运行（Unix 下）；
 * 3. 循环解析命令行参数，设置对应操作变量（init, start, stop, restart, reload, status 等）以及选项（PGDATA 等）；
 * 4. 调整并解析出真实的数据库物理路径 PGDATA (adjust_data_dir)；
 * 5. 进入 switch 分发逻辑，跳转执行具体的动作函数。
 */
int
main(int argc, char **argv)
{
	static struct option long_options[] = {
		{"help", no_argument, NULL, '?'},
		{"version", no_argument, NULL, 'V'},
		{"log", required_argument, NULL, 'l'},
		{"mode", required_argument, NULL, 'm'},
		{"pgdata", required_argument, NULL, 'D'},
		{"options", required_argument, NULL, 'o'},
		{"silent", no_argument, NULL, 's'},
		{"timeout", required_argument, NULL, 't'},
		{"core-files", no_argument, NULL, 'c'},
		{"wait", no_argument, NULL, 'w'},
		{"no-wait", no_argument, NULL, 'W'},
		{NULL, 0, NULL, 0}
	};

	char	   *env_wait;
	int			option_index;
	int			c;
	pid_t		killproc = 0;

	pg_logging_init(argv[0]);
	progname = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_ctl"));
	start_time = time(NULL);

	/*
	 * save argv[0] so do_start() can look for the postmaster if necessary. we
	 * don't look for postmaster here because in many cases we won't need it.
	 * save argv[0] so do_start() can look for the postmaster if necessary. we
	 * don't look for postmaster here because in many cases we won't need it.
	 * 保存 argv[0] 以便 do_start() 在必要时可以寻找 postmaster。我们不在这里寻找
	 * postmaster，因为在很多情况下我们并不需要它。
	 */
	argv0 = argv[0];

	/* Set restrictive mode mask until PGDATA permissions are checked - 在检查 PGDATA 权限之前，设置限制性模式掩码 */
	umask(PG_MODE_MASK_OWNER);

	/* support --help and --version even if invoked as root - 即使以 root 身份调用，也支持 --help 和 --version */
	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			do_help();
			exit(0);
		}
		else if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pg_ctl (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	/*
	 * Disallow running as root, to forestall any possible security holes.
	 * Disallow running as root, to forestall any possible security holes.
	 * 不允许以 root 身份运行，以防止任何可能的安全漏洞。
	 */
#ifndef WIN32
	if (geteuid() == 0)
	{
		write_stderr(_("%s: cannot be run as root\n"
					   "Please log in (using, e.g., \"su\") as the "
					   "(unprivileged) user that will\n"
					   "own the server process.\n"),
					 progname);
		exit(1);
	}
#endif

	env_wait = getenv("PGCTLTIMEOUT");
	if (env_wait != NULL)
		wait_seconds = atoi(env_wait);

	/* process command-line options - 处理命令行选项 */
	while ((c = getopt_long(argc, argv, "cD:e:l:m:N:o:p:P:sS:t:U:wW",
							long_options, &option_index)) != -1)
	{
		switch (c)
		{
			case 'D':
				{
					char	   *pgdata_D;

					pgdata_D = pg_strdup(optarg);
					canonicalize_path(pgdata_D);
					setenv("PGDATA", pgdata_D, 1);

					/*
					 * We could pass PGDATA just in an environment variable
					 * but we do -D too for clearer postmaster 'ps' display
					 * We could pass PGDATA just in an environment variable
					 * but we do -D too for clearer postmaster 'ps' display
					 * 我们本可以仅在环境变量中传递 PGDATA，但我们也传递了 -D 选项，
					 * 以使 postmaster 'ps' 显示更清晰。
					 */
					pgdata_opt = psprintf("-D \"%s\" ", pgdata_D);
					free(pgdata_D);
					break;
				}
			case 'e':
				event_source = pg_strdup(optarg);
				break;
			case 'l':
				log_file = pg_strdup(optarg);
				break;
			case 'm':
				set_mode(optarg);
				break;
			case 'N':
				register_servicename = pg_strdup(optarg);
				break;
			case 'o':
				/* append option? - 追加选项？ */
				if (!post_opts)
					post_opts = pg_strdup(optarg);
				else
				{
					char	   *old_post_opts = post_opts;

					post_opts = psprintf("%s %s", old_post_opts, optarg);
					free(old_post_opts);
				}
				break;
			case 'p':
				exec_path = pg_strdup(optarg);
				break;
			case 'P':
				register_password = pg_strdup(optarg);
				break;
			case 's':
				silent_mode = true;
				break;
			case 'S':
#ifdef WIN32
				set_starttype(optarg);
#else
				write_stderr(_("%s: -S option not supported on this platform\n"),
							 progname);
				exit(1);
#endif
				break;
			case 't':
				wait_seconds = atoi(optarg);
				wait_seconds_arg = true;
				break;
			case 'U':
				if (strchr(optarg, '\\'))
					register_username = pg_strdup(optarg);
				else
					/* Prepend .\ for local accounts - 为本地账户前置 .\ */
					register_username = psprintf(".\\%s", optarg);
				break;
			case 'w':
				do_wait = true;
				break;
			case 'W':
				do_wait = false;
				break;
			case 'c':
				allow_core_files = true;
				break;
			default:
				/* getopt_long already issued a suitable error message - getopt_long 已经发出了相应的错误消息 */
				do_advice();
				exit(1);
		}
	}

	/* Process an action - 处理操作 */
	if (optind < argc)
	{
		if (strcmp(argv[optind], "init") == 0
			|| strcmp(argv[optind], "initdb") == 0)
			ctl_command = INIT_COMMAND;
		else if (strcmp(argv[optind], "start") == 0)
			ctl_command = START_COMMAND;
		else if (strcmp(argv[optind], "stop") == 0)
			ctl_command = STOP_COMMAND;
		else if (strcmp(argv[optind], "restart") == 0)
			ctl_command = RESTART_COMMAND;
		else if (strcmp(argv[optind], "reload") == 0)
			ctl_command = RELOAD_COMMAND;
		else if (strcmp(argv[optind], "status") == 0)
			ctl_command = STATUS_COMMAND;
		else if (strcmp(argv[optind], "promote") == 0)
			ctl_command = PROMOTE_COMMAND;
		else if (strcmp(argv[optind], "logrotate") == 0)
			ctl_command = LOGROTATE_COMMAND;
		else if (strcmp(argv[optind], "kill") == 0)
		{
			if (argc - optind < 3)
			{
				write_stderr(_("%s: missing arguments for kill mode\n"), progname);
				do_advice();
				exit(1);
			}
			ctl_command = KILL_COMMAND;
			set_sig(argv[++optind]);
			killproc = atol(argv[++optind]);
		}
#ifdef WIN32
		else if (strcmp(argv[optind], "register") == 0)
			ctl_command = REGISTER_COMMAND;
		else if (strcmp(argv[optind], "unregister") == 0)
			ctl_command = UNREGISTER_COMMAND;
		else if (strcmp(argv[optind], "runservice") == 0)
			ctl_command = RUN_AS_SERVICE_COMMAND;
#endif
		else
		{
			write_stderr(_("%s: unrecognized operation mode \"%s\"\n"), progname, argv[optind]);
			do_advice();
			exit(1);
		}
		optind++;
	}

	if (optind < argc)
	{
		write_stderr(_("%s: too many command-line arguments (first is \"%s\")\n"), progname, argv[optind]);
		do_advice();
		exit(1);
	}

	if (ctl_command == NO_COMMAND)
	{
		write_stderr(_("%s: no operation specified\n"), progname);
		do_advice();
		exit(1);
	}

	/* Note we put any -D switch into the env var above - 注意，我们在上面将所有 -D 开关放入了环境变量中 */
	pg_config = getenv("PGDATA");
	if (pg_config)
	{
		pg_config = pg_strdup(pg_config);
		canonicalize_path(pg_config);
		pg_data = pg_strdup(pg_config);
	}

	/* -D might point at config-only directory; if so find the real PGDATA - -D 可能指向仅配置目录；如果是这样，寻找真实的 PGDATA */
	adjust_data_dir();

	/* Complain if -D needed and not provided - 如果需要 -D 且未提供则报错 */
	if (pg_config == NULL &&
		ctl_command != KILL_COMMAND && ctl_command != UNREGISTER_COMMAND)
	{
		write_stderr(_("%s: no database directory specified and environment variable PGDATA unset\n"),
					 progname);
		do_advice();
		exit(1);
	}

	if (ctl_command == RELOAD_COMMAND)
	{
		sig = SIGHUP;
		do_wait = false;
	}

	if (pg_data)
	{
		snprintf(postopts_file, MAXPGPATH, "%s/postmaster.opts", pg_data);
		snprintf(version_file, MAXPGPATH, "%s/PG_VERSION", pg_data);
		snprintf(pid_file, MAXPGPATH, "%s/postmaster.pid", pg_data);

		/*
		 * Set mask based on PGDATA permissions,
		 *
		 * Don't error here if the data directory cannot be stat'd. This is
		 * handled differently based on the command and we don't want to
		 * interfere with that logic.
		 * Set mask based on PGDATA permissions,
		 *
		 * Don't error here if the data directory cannot be stat'd. This is
		 * handled differently based on the command and we don't want to
		 * interfere with that logic.
		 * 根据 PGDATA 权限设置掩码。
		 * 如果无法对数据目录执行 stat，不要在此处报错。这会根据命令不同进行不同处理，
		 * 我们不想干预该逻辑。
		 */
		if (GetDataDirectoryCreatePerm(pg_data))
			umask(pg_mode_mask);
	}

	switch (ctl_command)
	{
		case INIT_COMMAND:
			do_init();
			break;
		case STATUS_COMMAND:
			do_status();
			break;
		case START_COMMAND:
			do_start();
			break;
		case STOP_COMMAND:
			do_stop();
			break;
		case RESTART_COMMAND:
			do_restart();
			break;
		case RELOAD_COMMAND:
			do_reload();
			break;
		case PROMOTE_COMMAND:
			do_promote();
			break;
		case LOGROTATE_COMMAND:
			do_logrotate();
			break;
		case KILL_COMMAND:
			do_kill(killproc);
			break;
#ifdef WIN32
		case REGISTER_COMMAND:
			pgwin32_doRegister();
			break;
		case UNREGISTER_COMMAND:
			pgwin32_doUnregister();
			break;
		case RUN_AS_SERVICE_COMMAND:
			pgwin32_doRunAsService();
			break;
#endif
		default:
			break;
	}

	exit(0);
}

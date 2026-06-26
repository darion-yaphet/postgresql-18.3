/*-------------------------------------------------------------------------
 *
 * pg_test_fsync --- tests all supported fsync() methods
 *
 * pg_test_fsync --- 测试所有支持的 fsync() 方法
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 *
 * src/bin/pg_test_fsync/pg_test_fsync.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <limits.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>

#include "common/logging.h"
#include "common/pg_prng.h"
#include "getopt_long.h"

/*
 * put the temp files in the local directory
 * unless the user specifies otherwise
 *
 * 除非用户另有指定，否则将临时文件放在本地目录中
 */
#define FSYNC_FILENAME	"./pg_test_fsync.out"

#define XLOG_BLCKSZ_K	(XLOG_BLCKSZ / 1024)

#define LABEL_FORMAT		"        %-30s"
#define NA_FORMAT			"%21s\n"
/* translator: maintain alignment with NA_FORMAT */
/* 翻译器：保持与 NA_FORMAT 的对齐 */
#define OPS_FORMAT			gettext_noop("%13.3f ops/sec  %6.0f usecs/op\n")
#define USECS_SEC			1000000

/* These are macros to avoid timing the function call overhead. */
/* 这些是用于避免计算函数调用开销时间的宏。 */
#ifndef WIN32
#define START_TIMER \
do { \
	alarm_triggered = false; \
	alarm(secs_per_test); \
	gettimeofday(&start_t, NULL); \
} while (0)
#else
/* WIN32 doesn't support alarm, so we create a thread and sleep there */
/* WIN32 不支持 alarm，所以我们创建一个线程并在那里睡眠 */
#define START_TIMER \
do { \
	alarm_triggered = false; \
	if (CreateThread(NULL, 0, process_alarm, NULL, 0, NULL) == \
		INVALID_HANDLE_VALUE) \
		pg_fatal("could not create thread for alarm"); \
	gettimeofday(&start_t, NULL); \
} while (0)
#endif

#define STOP_TIMER	\
do { \
	gettimeofday(&stop_t, NULL); \
	print_elapse(start_t, stop_t, ops); \
} while (0)


static const char *progname;

static unsigned int secs_per_test = 5;
static int	needs_unlink = 0;
static char full_buf[DEFAULT_XLOG_SEG_SIZE],
		   *buf,
		   *filename = FSYNC_FILENAME;
static struct timeval start_t,
			stop_t;
static sig_atomic_t alarm_triggered = false;


static void handle_args(int argc, char *argv[]);
static void prepare_buf(void);
static void test_open(void);
static void test_non_sync(void);
static void test_sync(int writes_per_op);
static void test_open_syncs(void);
static void test_open_sync(const char *msg, int writes_size);
static void test_file_descriptor_sync(void);

#ifndef WIN32
static void process_alarm(SIGNAL_ARGS);
#else
static DWORD WINAPI process_alarm(LPVOID param);
#endif
static void signal_cleanup(SIGNAL_ARGS);

#ifdef HAVE_FSYNC_WRITETHROUGH
static int	pg_fsync_writethrough(int fd);
#endif
static void print_elapse(struct timeval start_t, struct timeval stop_t, int ops);

#define die(msg) pg_fatal("%s: %m", _(msg))


/*
 * main --- 主函数入口点
 * 核心流程：初始化日志与区域设置，解析命令行参数，设置信号和闹钟清理程序，
 * 随后执行各项 I/O 与 fsync 测试（包含单块、双块写入测试，不同大小的 open_sync 写入，
 * 文件描述符共享同步性测试以及无同步写入测试），最终删除临时文件并退出。
 */
int
main(int argc, char *argv[])
{
	pg_logging_init(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_test_fsync"));
	progname = get_progname(argv[0]);

	handle_args(argc, argv);

	/* Prevent leaving behind the test file */
	/* 防止留下测试文件 */
	pqsignal(SIGINT, signal_cleanup);
	pqsignal(SIGTERM, signal_cleanup);

	/* the following are not valid on Windows */
	/* 以下在 Windows 上无效 */
#ifndef WIN32
	pqsignal(SIGALRM, process_alarm);
	pqsignal(SIGHUP, signal_cleanup);
#endif

	pg_prng_seed(&pg_global_prng_state, (uint64) time(NULL));

	prepare_buf();

	test_open();

	/* Test using 1 XLOG_BLCKSZ write */
	/* 使用 1 个 XLOG_BLCKSZ 写入进行测试 */
	test_sync(1);

	/* Test using 2 XLOG_BLCKSZ writes */
	/* 使用 2 个 XLOG_BLCKSZ 写入进行测试 */
	test_sync(2);

	test_open_syncs();

	test_file_descriptor_sync();

	test_non_sync();

	unlink(filename);

	return 0;
}

/*
 * handle_args --- 处理并解析命令行参数
 * 支持设置测试文件名 (-f) 以及每个测试的持续时间 (-s)。
 */
static void
handle_args(int argc, char *argv[])
{
	static struct option long_options[] = {
		{"filename", required_argument, NULL, 'f'},
		{"secs-per-test", required_argument, NULL, 's'},
		{NULL, 0, NULL, 0}
	};

	int			option;			/* Command line option */
	int			optindex = 0;	/* used by getopt_long */
	unsigned long optval;		/* used for option parsing */
	char	   *endptr;

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			printf(_("Usage: %s [-f FILENAME] [-s SECS-PER-TEST]\n"), progname);
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pg_test_fsync (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	while ((option = getopt_long(argc, argv, "f:s:",
								 long_options, &optindex)) != -1)
	{
		switch (option)
		{
			case 'f':
				filename = pg_strdup(optarg);
				break;

			case 's':
				errno = 0;
				optval = strtoul(optarg, &endptr, 10);

				if (endptr == optarg || *endptr != '\0' ||
					errno != 0 || optval != (unsigned int) optval)
				{
					pg_log_error("invalid argument for option %s", "--secs-per-test");
					pg_log_error_hint("Try \"%s --help\" for more information.", progname);
					exit(1);
				}

				secs_per_test = (unsigned int) optval;
				if (secs_per_test == 0)
					pg_fatal("%s must be in range %u..%u",
							 "--secs-per-test", 1, UINT_MAX);
				break;

			default:
				/* getopt_long already emitted a complaint */
				/* getopt_long 已经发出了投诉/报错 */
				pg_log_error_hint("Try \"%s --help\" for more information.", progname);
				exit(1);
		}
	}

	if (argc > optind)
	{
		pg_log_error("too many command-line arguments (first is \"%s\")",
					 argv[optind]);
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	printf(ngettext("%u second per test\n",
					"%u seconds per test\n",
					secs_per_test),
		   secs_per_test);
#if defined(O_DIRECT)
	printf(_("O_DIRECT supported on this platform for open_datasync and open_sync.\n"));
#elif defined(F_NOCACHE)
	printf(_("F_NOCACHE supported on this platform for open_datasync and open_sync.\n"));
#else
	printf(_("Direct I/O is not supported on this platform.\n"));
#endif
}

/*
 * prepare_buf --- 准备测试缓冲区
 * 使用随机数据填充缓冲区，并使指针按 XLOG_BLCKSZ 字节对齐，以模拟 WAL 缓冲区。
 */
static void
prepare_buf(void)
{
	int			ops;

	/* write random data into buffer */
	/* 将随机数据写入缓冲区 */
	for (ops = 0; ops < DEFAULT_XLOG_SEG_SIZE; ops++)
		full_buf[ops] = (char) pg_prng_int32(&pg_global_prng_state);

	buf = (char *) TYPEALIGN(XLOG_BLCKSZ, full_buf);
}

/*
 * test_open --- 测试是否可成功创建和写入目标文件
 * 并发出一次初始的 fsync 清理脏数据，以避免干扰后续的计时测试。
 */
static void
test_open(void)
{
	int			tmpfile;

	/*
	 * test if we can open the target file
	 *
	 * 测试我们是否可以打开目标文件
	 */
	if ((tmpfile = open(filename, O_RDWR | O_CREAT | PG_BINARY, S_IRUSR | S_IWUSR)) == -1)
		die("could not open output file");
	needs_unlink = 1;
	if (write(tmpfile, full_buf, DEFAULT_XLOG_SEG_SIZE) !=
		DEFAULT_XLOG_SEG_SIZE)
		die("write failed");

	/* fsync now so that dirty buffers don't skew later tests */
	/* 现在进行 fsync，以免脏缓冲区影响后面的测试 */
	if (fsync(tmpfile) != 0)
		die("fsync failed");

	close(tmpfile);
}

static int
open_direct(const char *path, int flags, mode_t mode)
{
	int			fd;

#ifdef O_DIRECT
	flags |= O_DIRECT;
#endif

	fd = open(path, flags, mode);

#if !defined(O_DIRECT) && defined(F_NOCACHE)
	if (fd >= 0 && fcntl(fd, F_NOCACHE, 1) < 0)
	{
		int			save_errno = errno;

		close(fd);
		errno = save_errno;
		return -1;
	}
#endif

	return fd;
}

/*
 * test_sync --- 对比各种支持的 WAL 同步方法 (wal_sync_method)
 * 测试包括 open_datasync、fdatasync、fsync、fsync_writethrough 以及 open_sync。
 */
static void
test_sync(int writes_per_op)
{
	int			tmpfile,
				ops,
				writes;
	bool		fs_warning = false;

	if (writes_per_op == 1)
		printf(_("\nCompare file sync methods using one %dkB write:\n"), XLOG_BLCKSZ_K);
	else
		printf(_("\nCompare file sync methods using two %dkB writes:\n"), XLOG_BLCKSZ_K);
	printf(_("(in \"wal_sync_method\" preference order, except fdatasync is Linux's default)\n"));

	/*
	 * Test open_datasync if available
	 *
	 * 如果可用，测试 open_datasync
	 */
	printf(LABEL_FORMAT, "open_datasync");
	fflush(stdout);

#ifdef O_DSYNC
	if ((tmpfile = open_direct(filename, O_RDWR | O_DSYNC | PG_BINARY, 0)) == -1)
	{
		printf(NA_FORMAT, _("n/a*"));
		fs_warning = true;
	}
	else
	{
		START_TIMER;
		for (ops = 0; alarm_triggered == false; ops++)
		{
			for (writes = 0; writes < writes_per_op; writes++)
				if (pg_pwrite(tmpfile,
							  buf,
							  XLOG_BLCKSZ,
							  writes * XLOG_BLCKSZ) != XLOG_BLCKSZ)
					die("write failed");
		}
		STOP_TIMER;
		close(tmpfile);
	}
#else
	printf(NA_FORMAT, _("n/a"));
#endif

/*
 * Test fdatasync if available
 *
 * 如果可用，测试 fdatasync
 */
	printf(LABEL_FORMAT, "fdatasync");
	fflush(stdout);

	if ((tmpfile = open(filename, O_RDWR | PG_BINARY, 0)) == -1)
		die("could not open output file");
	START_TIMER;
	for (ops = 0; alarm_triggered == false; ops++)
	{
		for (writes = 0; writes < writes_per_op; writes++)
			if (pg_pwrite(tmpfile,
						  buf,
						  XLOG_BLCKSZ,
						  writes * XLOG_BLCKSZ) != XLOG_BLCKSZ)
				die("write failed");
		fdatasync(tmpfile);
	}
	STOP_TIMER;
	close(tmpfile);

/*
 * Test fsync
 *
 * 测试 fsync
 */
	printf(LABEL_FORMAT, "fsync");
	fflush(stdout);

	if ((tmpfile = open(filename, O_RDWR | PG_BINARY, 0)) == -1)
		die("could not open output file");
	START_TIMER;
	for (ops = 0; alarm_triggered == false; ops++)
	{
		for (writes = 0; writes < writes_per_op; writes++)
			if (pg_pwrite(tmpfile,
						  buf,
						  XLOG_BLCKSZ,
						  writes * XLOG_BLCKSZ) != XLOG_BLCKSZ)
				die("write failed");
		if (fsync(tmpfile) != 0)
			die("fsync failed");
	}
	STOP_TIMER;
	close(tmpfile);

/*
 * If fsync_writethrough is available, test as well
 *
 * 如果 fsync_writethrough 可用，也对其进行测试
 */
	printf(LABEL_FORMAT, "fsync_writethrough");
	fflush(stdout);

#ifdef HAVE_FSYNC_WRITETHROUGH
	if ((tmpfile = open(filename, O_RDWR | PG_BINARY, 0)) == -1)
		die("could not open output file");
	START_TIMER;
	for (ops = 0; alarm_triggered == false; ops++)
	{
		for (writes = 0; writes < writes_per_op; writes++)
			if (pg_pwrite(tmpfile,
						  buf,
						  XLOG_BLCKSZ,
						  writes * XLOG_BLCKSZ) != XLOG_BLCKSZ)
				die("write failed");
		if (pg_fsync_writethrough(tmpfile) != 0)
			die("fsync failed");
	}
	STOP_TIMER;
	close(tmpfile);
#else
	printf(NA_FORMAT, _("n/a"));
#endif

/*
 * Test open_sync if available
 *
 * 如果可用，测试 open_sync
 */
	printf(LABEL_FORMAT, "open_sync");
	fflush(stdout);

#ifdef O_SYNC
	if ((tmpfile = open_direct(filename, O_RDWR | O_SYNC | PG_BINARY, 0)) == -1)
	{
		printf(NA_FORMAT, _("n/a*"));
		fs_warning = true;
	}
	else
	{
		START_TIMER;
		for (ops = 0; alarm_triggered == false; ops++)
		{
			for (writes = 0; writes < writes_per_op; writes++)
				if (pg_pwrite(tmpfile,
							  buf,
							  XLOG_BLCKSZ,
							  writes * XLOG_BLCKSZ) != XLOG_BLCKSZ)

					/*
					 * This can generate write failures if the filesystem has
					 * a large block size, e.g. 4k, and there is no support
					 * for O_DIRECT writes smaller than the file system block
					 * size, e.g. XFS.
					 *
					 * 如果文件系统的块大小较大（例如 4k），并且不支持小于文件系统块
					 * 大小（例如 XFS）的 O_DIRECT 写入，这可能会产生写入失败。
					 */
					die("write failed");
		}
		STOP_TIMER;
		close(tmpfile);
	}
#else
	printf(NA_FORMAT, _("n/a"));
#endif

	if (fs_warning)
	{
		printf(_("* This file system and its mount options do not support direct\n"
				 "  I/O, e.g. ext4 in journaled mode.\n"));
	}
}

/*
 * test_open_syncs --- 比较不同写入大小下 open_sync 的成本
 * 会循环调用 test_open_sync 分别以 16kB, 8kB, 4kB, 2kB, 1kB 大小执行写入。
 */
static void
test_open_syncs(void)
{
	printf(_("\nCompare open_sync with different write sizes:\n"));
	printf(_("(This is designed to compare the cost of writing 16kB in different write\n"
			 "open_sync sizes.)\n"));

	test_open_sync(_(" 1 * 16kB open_sync write"), 16);
	test_open_sync(_(" 2 *  8kB open_sync writes"), 8);
	test_open_sync(_(" 4 *  4kB open_sync writes"), 4);
	test_open_sync(_(" 8 *  2kB open_sync writes"), 2);
	test_open_sync(_("16 *  1kB open_sync writes"), 1);
}

/*
 * Test open_sync with different size files
 *
 * 测试不同大小文件的 open_sync
 */
/*
 * test_open_sync --- 在指定写入块大小下测试 open_sync I/O 吞吐
 */
static void
test_open_sync(const char *msg, int writes_size)
{
#ifdef O_SYNC
	int			tmpfile,
				ops,
				writes;
#endif

	printf(LABEL_FORMAT, msg);
	fflush(stdout);

#ifdef O_SYNC
	if ((tmpfile = open_direct(filename, O_RDWR | O_SYNC | PG_BINARY, 0)) == -1)
		printf(NA_FORMAT, _("n/a*"));
	else
	{
		START_TIMER;
		for (ops = 0; alarm_triggered == false; ops++)
		{
			for (writes = 0; writes < 16 / writes_size; writes++)
				if (pg_pwrite(tmpfile,
							  buf,
							  writes_size * 1024,
							  writes * writes_size * 1024) !=
					writes_size * 1024)
					die("write failed");
		}
		STOP_TIMER;
		close(tmpfile);
	}
#else
	printf(NA_FORMAT, _("n/a"));
#endif
}

/*
 * test_file_descriptor_sync --- 测试在只读或非写入的文件描述符上调用 fsync 是否有效
 * 这模拟了多个进程向同一个文件写入数据，并相互对彼此的写入执行 fsync 同步的情况。
 */
static void
test_file_descriptor_sync(void)
{
	int			tmpfile,
				ops;

	/*
	 * Test whether fsync can sync data written on a different descriptor for
	 * the same file.  This checks the efficiency of multi-process fsyncs
	 * against the same file. Possibly this should be done with writethrough
	 * on platforms which support it.
	 *
	 * 测试 fsync 是否可以同步在同一文件的不同描述符上写入的数据。
	 * 这将检查针对同一文件的多进程 fsync 的效率。在支持的平台上，
	 * 这可能应该通过 writethrough 来完成。
	 */
	printf(_("\nTest if fsync on non-write file descriptor is honored:\n"));
	printf(_("(If the times are similar, fsync() can sync data written on a different\n"
			 "descriptor.)\n"));

	/*
	 * first write, fsync and close, which is the normal behavior without
	 * multiple descriptors
	 *
	 * 首先写入、fsync 并关闭，这是没有多个描述符时的正常行为
	 */
	printf(LABEL_FORMAT, "write, fsync, close");
	fflush(stdout);

	START_TIMER;
	for (ops = 0; alarm_triggered == false; ops++)
	{
		if ((tmpfile = open(filename, O_RDWR | PG_BINARY, 0)) == -1)
			die("could not open output file");
		if (write(tmpfile, buf, XLOG_BLCKSZ) != XLOG_BLCKSZ)
			die("write failed");
		if (fsync(tmpfile) != 0)
			die("fsync failed");
		close(tmpfile);

		/*
		 * open and close the file again to be consistent with the following
		 * test
		 *
		 * 再次打开并关闭文件以与以下测试保持一致
		 */
		if ((tmpfile = open(filename, O_RDWR | PG_BINARY, 0)) == -1)
			die("could not open output file");
		close(tmpfile);
	}
	STOP_TIMER;

	/*
	 * Now open, write, close, open again and fsync This simulates processes
	 * fsyncing each other's writes.
	 *
	 * 现在打开、写入、关闭、再次打开并 fsync。这模拟了进程相互 fsync 对方写入的内容。
	 */
	printf(LABEL_FORMAT, "write, close, fsync");
	fflush(stdout);

	START_TIMER;
	for (ops = 0; alarm_triggered == false; ops++)
	{
		if ((tmpfile = open(filename, O_RDWR | PG_BINARY, 0)) == -1)
			die("could not open output file");
		if (write(tmpfile, buf, XLOG_BLCKSZ) != XLOG_BLCKSZ)
			die("write failed");
		close(tmpfile);
		/* reopen file */
		/* 重新打开文件 */
		if ((tmpfile = open(filename, O_RDWR | PG_BINARY, 0)) == -1)
			die("could not open output file");
		if (fsync(tmpfile) != 0)
			die("fsync failed");
		close(tmpfile);
	}
	STOP_TIMER;
}

/*
 * test_non_sync --- 测试无同步/无 fsync 情况下的纯写入吞吐
 */
static void
test_non_sync(void)
{
	int			tmpfile,
				ops;

	/*
	 * Test a simple write without fsync
	 *
	 * 测试没有 fsync 的简单写入
	 */
	printf(_("\nNon-sync'ed %dkB writes:\n"), XLOG_BLCKSZ_K);
	printf(LABEL_FORMAT, "write");
	fflush(stdout);

	if ((tmpfile = open(filename, O_RDWR | PG_BINARY, 0)) == -1)
		die("could not open output file");
	START_TIMER;
	for (ops = 0; alarm_triggered == false; ops++)
	{
		if (pg_pwrite(tmpfile, buf, XLOG_BLCKSZ, 0) != XLOG_BLCKSZ)
			die("write failed");
	}
	STOP_TIMER;
	close(tmpfile);
}

/*
 * signal_cleanup --- 信号处理清理程序
 * 当进程接收到 SIGINT/SIGTERM 等中断信号时被调用，负责清理测试产生的临时文件并安全退出。
 */
static void
signal_cleanup(SIGNAL_ARGS)
{
	int			rc;

	/* Delete the file if it exists. Ignore errors */
	/* 如果文件存在则删除。忽略错误 */
	if (needs_unlink)
		unlink(filename);
	/* Finish incomplete line on stdout */
	/* 在 stdout 上完成未完成的行 */
	rc = write(STDOUT_FILENO, "\n", 1);
	(void) rc;					/* silence compiler warnings */
	_exit(1);
}

#ifdef HAVE_FSYNC_WRITETHROUGH

static int
pg_fsync_writethrough(int fd)
{
#if defined(F_FULLFSYNC)
	return (fcntl(fd, F_FULLFSYNC, 0) == -1) ? -1 : 0;
#else
	errno = ENOSYS;
	return -1;
#endif
}
#endif

/*
 * print out the writes per second for tests
 *
 * 打印出测试中每秒的写入次数
 */
/*
 * print_elapse --- 计算并打印每次操作的平均耗时与每秒操作次数
 */
static void
print_elapse(struct timeval start_t, struct timeval stop_t, int ops)
{
	double		total_time = (stop_t.tv_sec - start_t.tv_sec) +
		(stop_t.tv_usec - start_t.tv_usec) * 0.000001;
	double		per_second = ops / total_time;
	double		avg_op_time_us = (total_time / ops) * USECS_SEC;

	printf(_(OPS_FORMAT), per_second, avg_op_time_us);
}

#ifndef WIN32
/*
 * process_alarm --- 闹钟信号处理函数 (UNIX/Linux)
 * 设置 alarm_triggered = true 以终止当前测试。
 */
static void
process_alarm(SIGNAL_ARGS)
{
	alarm_triggered = true;
}
#else
/*
 * process_alarm --- 闹钟线程入口函数 (WIN32)
 * 睡眠指定秒数后设置 alarm_triggered = true 以终止当前测试。
 */
static DWORD WINAPI
process_alarm(LPVOID param)
{
	/* WIN32 doesn't support alarm, so we create a thread and sleep there */
	/* WIN32 不支持 alarm，所以我们创建一个线程并在那里睡眠 */
	Sleep(secs_per_test * 1000);
	alarm_triggered = true;
	ExitThread(0);
}
#endif

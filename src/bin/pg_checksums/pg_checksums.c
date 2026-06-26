/*-------------------------------------------------------------------------
 *
 * pg_checksums.c
 *	  Checks, enables or disables page level checksums for an offline
 *	  cluster
 *	  检查、启用或禁用离线数据库集群的页面级校验和 (checksum)
 *
 * Copyright (c) 2010-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/bin/pg_checksums/pg_checksums.c
 *
 *-------------------------------------------------------------------------
 */

/*
 * pg_checksums.c 核心流程与主要函数说明：
 *
 * 1. 核心流程：
 *    - 初始化日志和本地化设置。
 *    - 解析命令行参数，确定操作模式：检查 (CHECK)、启用 (ENABLE) 或禁用 (DISABLE) 校验和。
 *    - 读取 pg_control 控制文件，验证其 CRC 以及与当前 pg_checksums 编译时 block size 的兼容性。
 *    - 检查数据库集群的状态。为了避免写断页 (torn pages) 导致的随机校验和失败，集群必须处于完全关闭状态 (DB_SHUTDOWNED/DB_SHUTDOWNED_IN_RECOVERY)。
 *    - 若是检查或启用模式，则遍历 DataDir 下的 "global"、"base" 以及表空间目录 "pg_tblspc"，对数据文件进行处理。
 *    - 若是启用或禁用模式，在操作完成后，更新控制文件 (pg_control) 中的 data_checksum_version 字段，并根据需要调用 sync_pgdata 确保数据落盘。
 *
 * 2. 核心函数：
 *    - usage(): 打印工具的帮助信息和选项说明。
 *    - progress_report(): 计算并输出校验和处理的进度百分比。
 *    - skipfile(): 根据排除列表判断是否跳过特定文件（如 pg_control、pg_internal.init 等）。
 *    - scan_file(): 扫描并读取指定的关系文件块，在 CHECK 模式下验证页面的校验和，在 ENABLE 模式下为新页面计算并写入校验和。
 *    - scan_directory(): 递归扫描指定的数据目录或表空间目录，过滤无效文件，并将符合条件的文件传递给 scan_file() 进行处理。
 */

#include "postgres_fe.h"

#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "common/controldata_utils.h"
#include "common/file_utils.h"
#include "common/logging.h"
#include "common/relpath.h"
#include "fe_utils/option_utils.h"
#include "getopt_long.h"
#include "pg_getopt.h"
#include "storage/bufpage.h"
#include "storage/checksum.h"
#include "storage/checksum_impl.h"


static int64 files_scanned = 0;
static int64 files_written = 0;
static int64 blocks_scanned = 0;
static int64 blocks_written = 0;
static int64 badblocks = 0;
static ControlFileData *ControlFile;

static char *only_filenode = NULL;
static bool do_sync = true;
static bool verbose = false;
static bool showprogress = false;
static DataDirSyncMethod sync_method = DATA_DIR_SYNC_METHOD_FSYNC;

typedef enum
{
	PG_MODE_CHECK,
	PG_MODE_DISABLE,
	PG_MODE_ENABLE,
} PgChecksumMode;

static PgChecksumMode mode = PG_MODE_CHECK;

static const char *progname;

/*
 * Progress status information.
 */
/*
 * 进度状态信息。
 */
static int64 total_size = 0;
static int64 current_size = 0;
static pg_time_t last_progress_report = 0;

/*
 * usage - Prints help and usage information for pg_checksums.
 *
 * usage - 打印 pg_checksums 的帮助和使用方法信息。
 */
static void
usage(void)
{
	printf(_("%s enables, disables, or verifies data checksums in a PostgreSQL database cluster.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]... [DATADIR]\n"), progname);
	printf(_("\nOptions:\n"));
	printf(_(" [-D, --pgdata=]DATADIR    data directory\n"));
	printf(_("  -c, --check              check data checksums (default)\n"));
	printf(_("  -d, --disable            disable data checksums\n"));
	printf(_("  -e, --enable             enable data checksums\n"));
	printf(_("  -f, --filenode=FILENODE  check only relation with specified filenode\n"));
	printf(_("  -N, --no-sync            do not wait for changes to be written safely to disk\n"));
	printf(_("  -P, --progress           show progress information\n"));
	printf(_("      --sync-method=METHOD set method for syncing files to disk\n"));
	printf(_("  -v, --verbose            output verbose messages\n"));
	printf(_("  -V, --version            output version information, then exit\n"));
	printf(_("  -?, --help               show this help, then exit\n"));
	printf(_("\nIf no data directory (DATADIR) is specified, "
			 "the environment variable PGDATA\nis used.\n\n"));
	printf(_("Report bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}

/*
 * Definition of one element part of an exclusion list, used for files
 * to exclude from checksum validation.  "name" is the name of the file
 * or path to check for exclusion.  If "match_prefix" is true, any items
 * matching the name as prefix are excluded.
 */
/*
 * 排除列表中的一个元素定义，用于指定不需要进行校验和验证的文件。
 * "name" 是要检查排除的文件名或路径。
 * 如果 "match_prefix" 为真，则任何前缀匹配该名称的项都将被排除。
 */
struct exclude_list_item
{
	const char *name;
	bool		match_prefix;
};

/*
 * List of files excluded from checksum validation.
 *
 * Note: this list should be kept in sync with what basebackup.c includes.
 */
/*
 * 校验和验证排除的文件列表。
 *
 * 注意：此列表应与 basebackup.c 中包含的列表保持同步。
 */
static const struct exclude_list_item skip[] = {
	{"pg_control", false},
	{"pg_filenode.map", false},
	{"pg_internal.init", true},
	{"PG_VERSION", false},
#ifdef EXEC_BACKEND
	{"config_exec_params", true},
#endif
	{NULL, false}
};

/*
 * Report current progress status.  Parts borrowed from
 * src/bin/pg_basebackup/pg_basebackup.c.
 */
/*
 * 报告当前进度状态。部分代码借用自
 * src/bin/pg_basebackup/pg_basebackup.c。
 */
/*
 * progress_report - Calculates and prints the current progress percentage.
 *
 * progress_report - 计算并打印当前的进度百分比。
 */
static void
progress_report(bool finished)
{
	int			percent;
	pg_time_t	now;

	Assert(showprogress);

	now = time(NULL);
	if (now == last_progress_report && !finished)
		return;					/* Max once per second */

	/* Save current time */
	/* 保存当前时间 */
	last_progress_report = now;

	/* Adjust total size if current_size is larger */
	/* 如果 current_size 更大，则调整总大小 */
	if (current_size > total_size)
		total_size = current_size;

	/* Calculate current percentage of size done */
	/* 计算当前完成大小的百分比 */
	percent = total_size ? (int) ((current_size) * 100 / total_size) : 0;

	fprintf(stderr, _("%" PRId64 "/%" PRId64 " MB (%d%%) computed"),
			(current_size / (1024 * 1024)),
			(total_size / (1024 * 1024)),
			percent);

	/*
	 * Stay on the same line if reporting to a terminal and we're not done
	 * yet.
	 */
	/*
	 * 如果输出到终端并且尚未完成，则保持在同一行。
	 */
	fputc((!finished && isatty(fileno(stderr))) ? '\r' : '\n', stderr);
}

/*
 * skipfile - Checks whether a file should be skipped based on the exclusion list.
 *
 * skipfile - 根据排除列表检查是否应跳过某个文件。
 */
static bool
skipfile(const char *fn)
{
	int			excludeIdx;

	for (excludeIdx = 0; skip[excludeIdx].name != NULL; excludeIdx++)
	{
		int			cmplen = strlen(skip[excludeIdx].name);

		if (!skip[excludeIdx].match_prefix)
			cmplen++;
		if (strncmp(skip[excludeIdx].name, fn, cmplen) == 0)
			return true;
	}

	return false;
}

/*
 * scan_file - Scans an individual relation file, verifies or enables checksums.
 *
 * scan_file - 扫描单个关系文件，验证或启用校验和。
 */
static void
scan_file(const char *fn, int segmentno)
{
	PGIOAlignedBlock buf;
	PageHeader	header = (PageHeader) buf.data;
	int			f;
	BlockNumber blockno;
	int			flags;
	int64		blocks_written_in_file = 0;

	Assert(mode == PG_MODE_ENABLE ||
		   mode == PG_MODE_CHECK);

	flags = (mode == PG_MODE_ENABLE) ? O_RDWR : O_RDONLY;
	f = open(fn, PG_BINARY | flags, 0);

	if (f < 0)
		pg_fatal("could not open file \"%s\": %m", fn);

	files_scanned++;

	for (blockno = 0;; blockno++)
	{
		uint16		csum;
		int			r = read(f, buf.data, BLCKSZ);

		if (r == 0)
			break;
		if (r != BLCKSZ)
		{
			if (r < 0)
				pg_fatal("could not read block %u in file \"%s\": %m",
						 blockno, fn);
			else
				pg_fatal("could not read block %u in file \"%s\": read %d of %d",
						 blockno, fn, r, BLCKSZ);
		}
		blocks_scanned++;

		/*
		 * Since the file size is counted as total_size for progress status
		 * information, the sizes of all pages including new ones in the file
		 * should be counted as current_size. Otherwise the progress reporting
		 * calculated using those counters may not reach 100%.
		 */
		/*
		 * 由于文件大小被计为进度状态信息的 total_size，因此文件中包括新页面在内的
		 * 所有页面的大小都应计为 current_size。否则，使用这些计数器计算的进度报告
		 * 可能无法达到 100%。
		 */
		current_size += r;

		/* New pages have no checksum yet */
		/* 新页面还没有校验和 */
		if (PageIsNew(buf.data))
			continue;

		csum = pg_checksum_page(buf.data, blockno + segmentno * RELSEG_SIZE);
		if (mode == PG_MODE_CHECK)
		{
			if (csum != header->pd_checksum)
			{
				if (ControlFile->data_checksum_version == PG_DATA_CHECKSUM_VERSION)
					pg_log_error("checksum verification failed in file \"%s\", block %u: calculated checksum %X but block contains %X",
								 fn, blockno, csum, header->pd_checksum);
				badblocks++;
			}
		}
		else if (mode == PG_MODE_ENABLE)
		{
			int			w;

			/*
			 * Do not rewrite if the checksum is already set to the expected
			 * value.
			 */
			/*
			 * 如果校验和已经设置为预期值，则不要重写。
			 */
			if (header->pd_checksum == csum)
				continue;

			blocks_written_in_file++;

			/* Set checksum in page header */
			/* 在页面头部设置校验和 */
			header->pd_checksum = csum;

			/* Seek back to beginning of block */
			/* 寻址回块的开始位置 */
			if (lseek(f, -BLCKSZ, SEEK_CUR) < 0)
				pg_fatal("seek failed for block %u in file \"%s\": %m", blockno, fn);

			/* Write block with checksum */
			/* 写入带有校验和的块 */
			w = write(f, buf.data, BLCKSZ);
			if (w != BLCKSZ)
			{
				if (w < 0)
					pg_fatal("could not write block %u in file \"%s\": %m",
							 blockno, fn);
				else
					pg_fatal("could not write block %u in file \"%s\": wrote %d of %d",
							 blockno, fn, w, BLCKSZ);
			}
		}

		if (showprogress)
			progress_report(false);
	}

	if (verbose)
	{
		if (mode == PG_MODE_CHECK)
			pg_log_info("checksums verified in file \"%s\"", fn);
		if (mode == PG_MODE_ENABLE)
			pg_log_info("checksums enabled in file \"%s\"", fn);
	}

	/* Update write counters if any write activity has happened */
	/* 如果发生了任何写入活动，则更新写入计数器 */
	if (blocks_written_in_file > 0)
	{
		files_written++;
		blocks_written += blocks_written_in_file;
	}

	close(f);
}

/*
 * Scan the given directory for items which can be checksummed and
 * operate on each one of them.  If "sizeonly" is true, the size of
 * all the items which have checksums is computed and returned back
 * to the caller without operating on the files.  This is used to compile
 * the total size of the data directory for progress reports.
 */
/*
 * 扫描给定目录中可以计算校验和的项，并对其中每一个进行操作。
 * 如果 "sizeonly" 为真，则计算所有具有校验和的项的大小并返回给调用者，
 * 而不对文件进行操作。这用于编译数据目录的总大小以生成进度报告。
 */
/*
 * scan_directory - Recursively scans the data directories to process relation files.
 *
 * scan_directory - 递归扫描数据目录以处理关系文件。
 */
static int64
scan_directory(const char *basedir, const char *subdir, bool sizeonly)
{
	int64		dirsize = 0;
	char		path[MAXPGPATH];
	DIR		   *dir;
	struct dirent *de;

	snprintf(path, sizeof(path), "%s/%s", basedir, subdir);
	dir = opendir(path);
	if (!dir)
		pg_fatal("could not open directory \"%s\": %m", path);
	while ((de = readdir(dir)) != NULL)
	{
		char		fn[MAXPGPATH];
		struct stat st;

		if (strcmp(de->d_name, ".") == 0 ||
			strcmp(de->d_name, "..") == 0)
			continue;

		/* Skip temporary files */
		/* 跳过临时文件 */
		if (strncmp(de->d_name,
					PG_TEMP_FILE_PREFIX,
					strlen(PG_TEMP_FILE_PREFIX)) == 0)
			continue;

		/* Skip temporary folders */
		/* 跳过临时文件夹 */
		if (strncmp(de->d_name,
					PG_TEMP_FILES_DIR,
					strlen(PG_TEMP_FILES_DIR)) == 0)
			continue;

		/* Skip macOS system files */
		/* 跳过 macOS 系统文件 */
		if (strcmp(de->d_name, ".DS_Store") == 0)
			continue;

		snprintf(fn, sizeof(fn), "%s/%s", path, de->d_name);
		if (lstat(fn, &st) < 0)
			pg_fatal("could not stat file \"%s\": %m", fn);
		if (S_ISREG(st.st_mode))
		{
			char		fnonly[MAXPGPATH];
			char	   *forkpath,
					   *segmentpath;
			int			segmentno = 0;

			if (skipfile(de->d_name))
				continue;

			/*
			 * Cut off at the segment boundary (".") to get the segment number
			 * in order to mix it into the checksum. Then also cut off at the
			 * fork boundary, to get the filenode the file belongs to for
			 * filtering.
			 */
			/*
			 * 在段边界 (".") 处截断以获取段号，以便将其混合到校验和中。
			 * 然后在分叉边界处截断，以获取该文件所属的 filenode 以进行过滤。
			 */
			strlcpy(fnonly, de->d_name, sizeof(fnonly));
			segmentpath = strchr(fnonly, '.');
			if (segmentpath != NULL)
			{
				*segmentpath++ = '\0';
				segmentno = atoi(segmentpath);
				if (segmentno == 0)
					pg_fatal("invalid segment number %d in file name \"%s\"",
							 segmentno, fn);
			}

			forkpath = strchr(fnonly, '_');
			if (forkpath != NULL)
				*forkpath++ = '\0';

			if (only_filenode && strcmp(only_filenode, fnonly) != 0)
				/* filenode not to be included */
				/* 不包含此 filenode */
				continue;

			dirsize += st.st_size;

			/*
			 * No need to work on the file when calculating only the size of
			 * the items in the data folder.
			 */
			/*
			 * 当仅计算数据文件夹中项的大小时，无需对文件进行处理。
			 */
			if (!sizeonly)
				scan_file(fn, segmentno);
		}
		else if (S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode))
		{
			/*
			 * If going through the entries of pg_tblspc, we assume to operate
			 * on tablespace locations where only TABLESPACE_VERSION_DIRECTORY
			 * is valid, resolving the linked locations and dive into them
			 * directly.
			 */
			/*
			 * 如果遍历 pg_tblspc 的条目，我们假定在表空间位置上操作，其中只有
			 * TABLESPACE_VERSION_DIRECTORY 是有效的，解析链接位置并直接深入其中。
			 */
			if (strncmp(PG_TBLSPC_DIR, subdir, strlen(PG_TBLSPC_DIR)) == 0)
			{
				char		tblspc_path[MAXPGPATH];
				struct stat tblspc_st;

				/*
				 * Resolve tablespace location path and check whether
				 * TABLESPACE_VERSION_DIRECTORY exists.  Not finding a valid
				 * location is unexpected, since there should be no orphaned
				 * links and no links pointing to something else than a
				 * directory.
				 */
				/*
				 * 解析表空间位置路径并检查 TABLESPACE_VERSION_DIRECTORY 是否存在。
				 * 未找到有效位置是不可预料的，因为不应该存在孤立的链接，也不应该存在指向
				 * 非目录的链接。
				 */
				snprintf(tblspc_path, sizeof(tblspc_path), "%s/%s/%s",
						 path, de->d_name, TABLESPACE_VERSION_DIRECTORY);

				if (lstat(tblspc_path, &tblspc_st) < 0)
					pg_fatal("could not stat file \"%s\": %m",
							 tblspc_path);

				/*
				 * Move backwards once as the scan needs to happen for the
				 * contents of TABLESPACE_VERSION_DIRECTORY.
				 */
				/*
				 * 向后移动一次，因为扫描需要对 TABLESPACE_VERSION_DIRECTORY 的内容进行。
				 */
				snprintf(tblspc_path, sizeof(tblspc_path), "%s/%s",
						 path, de->d_name);

				/* Looks like a valid tablespace location */
				/* 看起来是一个有效的表空间位置 */
				dirsize += scan_directory(tblspc_path,
										  TABLESPACE_VERSION_DIRECTORY,
										  sizeonly);
			}
			else
			{
				dirsize += scan_directory(path, de->d_name, sizeonly);
			}
		}
	}
	closedir(dir);
	return dirsize;
}

int
main(int argc, char *argv[])
{
	static struct option long_options[] = {
		{"check", no_argument, NULL, 'c'},
		{"pgdata", required_argument, NULL, 'D'},
		{"disable", no_argument, NULL, 'd'},
		{"enable", no_argument, NULL, 'e'},
		{"filenode", required_argument, NULL, 'f'},
		{"no-sync", no_argument, NULL, 'N'},
		{"progress", no_argument, NULL, 'P'},
		{"verbose", no_argument, NULL, 'v'},
		{"sync-method", required_argument, NULL, 1},
		{NULL, 0, NULL, 0}
	};

	char	   *DataDir = NULL;
	int			c;
	int			option_index;
	bool		crc_ok;

	pg_logging_init(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_checksums"));
	progname = get_progname(argv[0]);

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage();
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pg_checksums (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	while ((c = getopt_long(argc, argv, "cdD:ef:NPv", long_options, &option_index)) != -1)
	{
		switch (c)
		{
			case 'c':
				mode = PG_MODE_CHECK;
				break;
			case 'd':
				mode = PG_MODE_DISABLE;
				break;
			case 'D':
				DataDir = optarg;
				break;
			case 'e':
				mode = PG_MODE_ENABLE;
				break;
			case 'f':
				if (!option_parse_int(optarg, "-f/--filenode", 0,
									  INT_MAX,
									  NULL))
					exit(1);
				only_filenode = pstrdup(optarg);
				break;
			case 'N':
				do_sync = false;
				break;
			case 'P':
				showprogress = true;
				break;
			case 'v':
				verbose = true;
				break;
			case 1:
				if (!parse_sync_method(optarg, &sync_method))
					exit(1);
				break;
			default:
				/* getopt_long already emitted a complaint */
				/* getopt_long 已经发出了投诉 */
				pg_log_error_hint("Try \"%s --help\" for more information.", progname);
				exit(1);
		}
	}

	if (DataDir == NULL)
	{
		if (optind < argc)
			DataDir = argv[optind++];
		else
			DataDir = getenv("PGDATA");

		/* If no DataDir was specified, and none could be found, error out */
		/* 如果未指定 DataDir，且找不到任何 DataDir，则报错退出 */
		if (DataDir == NULL)
		{
			pg_log_error("no data directory specified");
			pg_log_error_hint("Try \"%s --help\" for more information.", progname);
			exit(1);
		}
	}

	/* Complain if any arguments remain */
	/* 如果还有剩余参数，则报错 */
	if (optind < argc)
	{
		pg_log_error("too many command-line arguments (first is \"%s\")",
					 argv[optind]);
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	/* filenode checking only works in --check mode */
	/* filenode 检查仅在 --check 模式下有效 */
	if (mode != PG_MODE_CHECK && only_filenode)
	{
		pg_log_error("option -f/--filenode can only be used with --check");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	/* Read the control file and check compatibility */
	/* 读取控制文件并检查兼容性 */
	ControlFile = get_controlfile(DataDir, &crc_ok);
	if (!crc_ok)
		pg_fatal("pg_control CRC value is incorrect");

	if (ControlFile->pg_control_version != PG_CONTROL_VERSION)
		pg_fatal("cluster is not compatible with this version of pg_checksums");

	if (ControlFile->blcksz != BLCKSZ)
	{
		pg_log_error("database cluster is not compatible");
		pg_log_error_detail("The database cluster was initialized with block size %u, but pg_checksums was compiled with block size %u.",
							ControlFile->blcksz, BLCKSZ);
		exit(1);
	}

	/*
	 * Check if cluster is running.  A clean shutdown is required to avoid
	 * random checksum failures caused by torn pages.  Note that this doesn't
	 * guard against someone starting the cluster concurrently.
	 */
	/*
	 * 检查集群是否正在运行。需要干净的关机以避免由于写断页 (torn pages)
	 * 导致的随机校验和失败。请注意，这无法防止有人并发启动集群。
	 */
	if (ControlFile->state != DB_SHUTDOWNED &&
		ControlFile->state != DB_SHUTDOWNED_IN_RECOVERY)
		pg_fatal("cluster must be shut down");

	if (ControlFile->data_checksum_version == 0 &&
		mode == PG_MODE_CHECK)
		pg_fatal("data checksums are not enabled in cluster");

	if (ControlFile->data_checksum_version == 0 &&
		mode == PG_MODE_DISABLE)
		pg_fatal("data checksums are already disabled in cluster");

	if (ControlFile->data_checksum_version > 0 &&
		mode == PG_MODE_ENABLE)
		pg_fatal("data checksums are already enabled in cluster");

	/* Operate on all files if checking or enabling checksums */
	/* 如果是检查或启用校验和，则对所有文件进行操作 */
	if (mode == PG_MODE_CHECK || mode == PG_MODE_ENABLE)
	{
		/*
		 * If progress status information is requested, we need to scan the
		 * directory tree twice: once to know how much total data needs to be
		 * processed and once to do the real work.
		 */
		/*
		 * 如果请求了进度状态信息，我们需要扫描目录树两次：
		 * 一次是为了知道需要处理的总数据量，另一次是为了进行实际工作。
		 */
		if (showprogress)
		{
			total_size = scan_directory(DataDir, "global", true);
			total_size += scan_directory(DataDir, "base", true);
			total_size += scan_directory(DataDir, PG_TBLSPC_DIR, true);
		}

		(void) scan_directory(DataDir, "global", false);
		(void) scan_directory(DataDir, "base", false);
		(void) scan_directory(DataDir, PG_TBLSPC_DIR, false);

		if (showprogress)
			progress_report(true);

		printf(_("Checksum operation completed\n"));
		printf(_("Files scanned:   %" PRId64 "\n"), files_scanned);
		printf(_("Blocks scanned:  %" PRId64 "\n"), blocks_scanned);
		if (mode == PG_MODE_CHECK)
		{
			printf(_("Bad checksums:  %" PRId64 "\n"), badblocks);
			printf(_("Data checksum version: %u\n"), ControlFile->data_checksum_version);

			if (badblocks > 0)
				exit(1);
		}
		else if (mode == PG_MODE_ENABLE)
		{
			printf(_("Files written:  %" PRId64 "\n"), files_written);
			printf(_("Blocks written: %" PRId64 "\n"), blocks_written);
		}
	}

	/*
	 * Finally make the data durable on disk if enabling or disabling
	 * checksums.  Flush first the data directory for safety, and then update
	 * the control file to keep the switch consistent.
	 */
	/*
	 * 最后，如果启用或禁用校验和，使数据在磁盘上持久化。
	 * 为了安全起见，首先刷新数据目录，然后更新控制文件以保持开关的一致性。
	 */
	if (mode == PG_MODE_ENABLE || mode == PG_MODE_DISABLE)
	{
		ControlFile->data_checksum_version =
			(mode == PG_MODE_ENABLE) ? PG_DATA_CHECKSUM_VERSION : 0;

		if (do_sync)
		{
			pg_log_info("syncing data directory");
			sync_pgdata(DataDir, PG_VERSION_NUM, sync_method, true);
		}

		pg_log_info("updating control file");
		update_controlfile(DataDir, ControlFile, do_sync);

		if (verbose)
			printf(_("Data checksum version: %u\n"), ControlFile->data_checksum_version);
		if (mode == PG_MODE_ENABLE)
			printf(_("Checksums enabled in cluster\n"));
		else
			printf(_("Checksums disabled in cluster\n"));
	}

	return 0;
}

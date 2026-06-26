/*
 * pg_archivecleanup.c
 *
 * To be used as archive_cleanup_command to clean an archive when using
 * standby mode.
 *
 * 用作 archive_cleanup_command，以便在使用备用模式时清理归档。
 *
 * src/bin/pg_archivecleanup/pg_archivecleanup.c
 */

/*
 * pg_archivecleanup 核心流程与主要函数说明（中文）：
 *
 * 1. 核心流程：
 *    - 作为一个独立的命令行工具，或者配置在 postgresql.conf 的 archive_cleanup_command 中，在 Standby 模式下清理不需要的 WAL 归档文件。
 *    - 解析命令行选项（如 -d 调试日志，-n 模拟运行，-b 同时清理备份历史文件，-x 剥离文件名指定的扩展名后缀）。
 *    - 获取并验证两个必需的命令行位置参数：归档路径目录（archiveLocation）和需要保留的最旧 WAL 文件名（restartWALFileName）。
 *    - 调用 Initialize() 确认归档目标路径目录是否存在。
 *    - 调用 SetWALFileNameForCleanup()：
 *      - 检查 restartWALFileName 的合法性（是否是标准的 WAL 段名，或带 .partial，或带 .backup 的历史文件名）。
 *      - 提取相应的文件标识前缀，并存入全局变量 exclusiveCleanupFileName 中，作为判断哪些文件可以被删除的“临界线”。
 *    - 调用 CleanupPriorWALFiles() 遍历归档目录：
 *      - 过滤不属于 WAL、.partial 或备份历史文件特征的文件名。
 *      - 通过字母顺序排序比较（strcmp），判定文件名小于 exclusiveCleanupFileName 临界线的文件即为过时文件。
 *      - 在 dryrun 模式下仅做打印输出，正常模式下调用 unlink() 函数在磁盘上执行物理删除。
 *
 * 2. 核心函数说明：
 *    - Initialize(): 初始化并使用 stat() 检测归档目录的合法性。
 *    - TrimExtension(): 从文件名中安全剥离指定的额外文件后缀扩展名（例如 .gz, .bz2 等），以匹配规范的 WAL 段格式进行顺序比对。
 *    - SetWALFileNameForCleanup(): 解析入参中最旧的可恢复 WAL 文件名，并在排除了 .partial / .backup 等特殊后缀后，提取其基础 WAL 段名称。
 *    - CleanupPriorWALFiles(): 遍历归档目录中的文件，过滤合法 WAL 相关名称，并通过文件名比对实现过期文件的清理和删除。
 */
#include "postgres_fe.h"

#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/time.h>

#include "access/xlog_internal.h"
#include "common/logging.h"
#include "getopt_long.h"

static const char *progname;

/* Options and defaults */
/* 选项与默认值 */
static bool dryrun = false;		/* are we performing a dry-run operation? */
								/* 我们是否在执行模拟运行（dry-run）？ */
static bool cleanBackupHistory = false; /* remove files including backup
										 * history files */
										/* 清理包括备份历史文件在内的文件 */
static char *additional_ext = NULL; /* Extension to remove from filenames */
									/* 要从文件名中删除的扩展名 */

static char *archiveLocation;	/* where to find the archive? */
								/* 在何处寻找归档？ */
static char *restartWALFileName;	/* the file from which we can restart
									 * restore */
									/* 我们可以从中重新开始恢复（restore）的文件 */
static char exclusiveCleanupFileName[MAXFNAMELEN];	/* the oldest file we want
													 * to remain in archive */
													/* 我们希望保留在归档中的最旧文件 */


/* =====================================================================
 *
 *		  Customizable section
 *
 * =====================================================================
 *
 *	Currently, this section assumes that the Archive is a locally
 *	accessible directory. If you want to make other assumptions,
 *	such as using a vendor-specific archive and access API, these
 *	routines are the ones you'll need to change. You're
 *	encouraged to submit any changes to pgsql-hackers@lists.postgresql.org
 *	or personally to the current maintainer. Those changes may be
 *	folded in to later versions of this program.
 */
/* =====================================================================
 *
 *		  可自定义部分
 *
 * =====================================================================
 *
 *	当前，此部分假定归档是一个本地可访问的目录。如果您想做出其他假设，
 *	例如使用特定厂商的归档和访问 API，则需要修改这些例程。
 *	鼓励您将任何更改提交到 pgsql-hackers@lists.postgresql.org
 *	或亲自提交给当前的维护者。这些更改可能会整合到此程序的后续版本中。
 */

/*
 *	Initialize allows customized commands into the archive cleanup program.
 *
 *	You may wish to add code to check for tape libraries, etc..
 *
 *	Initialize 允许将自定义命令引入归档清理程序。
 *	您可能希望添加代码以检查磁带库等。
 */
/*
 * Initialize --- 初始化归档清理程序，检查归档路径是否存在
 */
static void
Initialize(void)
{
	/*
	 * This code assumes that archiveLocation is a directory, so we use stat
	 * to test if it's accessible.
	 *
	 * 此代码假定 archiveLocation 是一个目录，因此我们使用 stat 来测试它是否可访问。
	 */
	struct stat stat_buf;

	if (stat(archiveLocation, &stat_buf) != 0 ||
		!S_ISDIR(stat_buf.st_mode))
	{
		pg_log_error("archive location \"%s\" does not exist",
					 archiveLocation);
		exit(2);
	}
}

/*
 * TrimExtension --- 剥离文件名末尾的指定扩展名
 */
static void
TrimExtension(char *filename, char *extension)
{
	int			flen;
	int			elen;

	if (extension == NULL)
		return;

	elen = strlen(extension);
	flen = strlen(filename);

	if (flen > elen && strcmp(filename + flen - elen, extension) == 0)
		filename[flen - elen] = '\0';
}

/*
 * CleanupPriorWALFiles --- 遍历归档目录并删除比 exclusiveCleanupFileName 更旧的 WAL/备份历史文件
 */
static void
CleanupPriorWALFiles(void)
{
	int			rc;
	DIR		   *xldir;
	struct dirent *xlde;
	char		walfile[MAXPGPATH];

	xldir = opendir(archiveLocation);
	if (xldir == NULL)
		pg_fatal("could not open archive location \"%s\": %m",
				 archiveLocation);

	while (errno = 0, (xlde = readdir(xldir)) != NULL)
	{
		char		WALFilePath[MAXPGPATH * 2]; /* the file path including
												 * archive */
												/* 包含归档路径的文件路径 */

		/*
		 * Truncation is essentially harmless, because we skip files whose
		 * format is different from WAL files and backup history files. (In
		 * principle, one could use a 1000-character additional_ext and get
		 * trouble.)
		 *
		 * 截断本质上是无害的，因为我们跳过了格式与 WAL 文件和备份历史文件不同的文件。
		 *（原则上，如果使用 1000 个字符的 additional_ext 可能会带来麻烦。）
		 */
		strlcpy(walfile, xlde->d_name, MAXPGPATH);
		TrimExtension(walfile, additional_ext);

		/*
		 * Ignore anything does that not look like a WAL segment, a .partial
		 * WAL segment or a backup history file (if requested).
	 *
		 * 忽略任何看起来不像 WAL 段、.partial WAL 段或备份历史文件（如果要求）的文件。
		 */
		if (!IsXLogFileName(walfile) && !IsPartialXLogFileName(walfile) &&
			!(cleanBackupHistory && IsBackupHistoryFileName(walfile)))
			continue;

		/*
		 * We ignore the timeline part of the XLOG segment identifiers in
		 * deciding whether a segment is still needed.  This ensures that we
		 * won't prematurely remove a segment from a parent timeline. We could
		 * probably be a little more proactive about removing segments of
		 * non-parent timelines, but that would be a whole lot more
		 * complicated.
		 *
		 * We use the alphanumeric sorting property of the filenames to decide
		 * which ones are earlier than the exclusiveCleanupFileName file. Note
		 * that this means files are not removed in the order they were
		 * originally written, in case this worries you.
		 *
		 * 在决定是否仍需要某个段时，我们忽略 XLOG 段标识符中的时间线部分。
		 * 这确保了我们不会过早地从父时间线中删除段。我们可能可以更主动地删除
		 * 非父时间线的段，但那会复杂得多。
		 *
		 * 我们使用文件名的字母顺序排序特性来决定哪些文件早于 exclusiveCleanupFileName
		 * 文件。请注意，这意味着文件不会按照最初写入的顺序被删除，以免您为此担心。
		 */
		if (strcmp(walfile + 8, exclusiveCleanupFileName + 8) >= 0)
			continue;

		/*
		 * Use the original file name again now, including any extension that
		 * might have been chopped off before testing the sequence.
		 *
		 * 现在再次使用原始文件名，包括在测试顺序之前可能已被切掉的任何扩展名。
		 */
		snprintf(WALFilePath, sizeof(WALFilePath), "%s/%s",
				 archiveLocation, xlde->d_name);

		if (dryrun)
		{
			/*
			 * Prints the name of the file to be removed and skips the actual
			 * removal.  The regular printout is so that the user can pipe the
			 * output into some other program.
			 *
			 * 打印要删除的文件名并跳过实际删除。常规打印是为了让用户可以将输出
			 * 通过管道传输到其他程序中。
			 */
			printf("%s\n", WALFilePath);
			pg_log_debug("file \"%s\" would be removed", WALFilePath);
			continue;
		}

		pg_log_debug("removing file \"%s\"", WALFilePath);

		rc = unlink(WALFilePath);
		if (rc != 0)
			pg_fatal("could not remove file \"%s\": %m",
					 WALFilePath);
	}

	if (errno)
		pg_fatal("could not read archive location \"%s\": %m",
				 archiveLocation);
	if (closedir(xldir))
		pg_fatal("could not close archive location \"%s\": %m",
				 archiveLocation);
}

/*
 * SetWALFileNameForCleanup()
 *
 *	  Set the earliest WAL filename that we want to keep on the archive
 *	  and decide whether we need cleanup
 *
 *	  设置我们希望保留在归档上的最旧 WAL 文件名，并决定我们是否需要进行清理
 */
/*
 * SetWALFileNameForCleanup --- 决定需要被保留的最旧 WAL 文件，将其存入 exclusiveCleanupFileName
 */
static void
SetWALFileNameForCleanup(void)
{
	bool		fnameOK = false;

	TrimExtension(restartWALFileName, additional_ext);

	/*
	 * If restartWALFileName is a WAL file name then just use it directly. If
	 * restartWALFileName is a .partial or .backup filename, make sure we use
	 * the prefix of the filename, otherwise we will remove wrong files since
	 * 000000010000000000000010.partial and
	 * 000000010000000000000010.00000020.backup are after
	 * 000000010000000000000010.
	 *
	 * 如果 restartWALFileName 是 WAL 文件名，则直接使用它。如果 restartWALFileName
	 * 是 .partial 或 .backup 文件名，请确保我们使用的是文件名的前缀，否则我们将删除
	 * 错误的文件，因为 000000010000000000000010.partial 和
	 * 000000010000000000000010.00000020.backup 位于 000000010000000000000010 之后。
	 */
	if (IsXLogFileName(restartWALFileName))
	{
		strcpy(exclusiveCleanupFileName, restartWALFileName);
		fnameOK = true;
	}
	else if (IsPartialXLogFileName(restartWALFileName))
	{
		int			args;
		uint32		tli = 1,
					log = 0,
					seg = 0;

		args = sscanf(restartWALFileName, "%08X%08X%08X.partial",
					  &tli, &log, &seg);
		if (args == 3)
		{
			fnameOK = true;

			/*
			 * Use just the prefix of the filename, ignore everything after
			 * first period
			 *
			 * 仅使用文件名的前缀，忽略第一个句点之后的所有内容
			 */
			XLogFileNameById(exclusiveCleanupFileName, tli, log, seg);
		}
	}
	else if (IsBackupHistoryFileName(restartWALFileName))
	{
		int			args;
		uint32		tli = 1,
					log = 0,
					seg = 0,
					offset = 0;

		args = sscanf(restartWALFileName, "%08X%08X%08X.%08X.backup", &tli, &log, &seg, &offset);
		if (args == 4)
		{
			fnameOK = true;

			/*
			 * Use just the prefix of the filename, ignore everything after
			 * first period
			 *
			 * 仅使用文件名的前缀，忽略第一个句点之后的所有内容
			 */
			XLogFileNameById(exclusiveCleanupFileName, tli, log, seg);
		}
	}

	if (!fnameOK)
	{
		pg_log_error("invalid file name argument");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(2);
	}
}

/* =====================================================================
 *		  End of Customizable section
 * =====================================================================
 */

/*
 * usage --- 打印程序用法与帮助信息
 */
static void
usage(void)
{
	printf(_("%s removes older WAL files from PostgreSQL archives.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]... ARCHIVELOCATION OLDESTKEPTWALFILE\n"), progname);
	printf(_("\nOptions:\n"));
	printf(_("  -b, --clean-backup-history  clean up files including backup history files\n"));
	printf(_("  -d, --debug                 generate debug output (verbose mode)\n"));
	printf(_("  -n, --dry-run               dry run, show the names of the files that would be\n"
			 "                              removed\n"));
	printf(_("  -V, --version               output version information, then exit\n"));
	printf(_("  -x, --strip-extension=EXT   strip this extension before identifying files for\n"
			 "                              clean up\n"));
	printf(_("  -?, --help                  show this help, then exit\n"));
	printf(_("\n"
			 "For use as \"archive_cleanup_command\" in postgresql.conf:\n"
			 "  archive_cleanup_command = 'pg_archivecleanup [OPTION]... ARCHIVELOCATION %%r'\n"
			 "e.g.\n"
			 "  archive_cleanup_command = 'pg_archivecleanup /mnt/server/archiverdir %%r'\n"));
	printf(_("\n"
			 "Or for use as a standalone archive cleaner:\n"
			 "e.g.\n"
			 "  pg_archivecleanup /mnt/server/archiverdir 000000010000000000000010.00000020.backup\n"));
	printf(_("\nReport bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}

/*------------ MAIN ----------------------------------------*/
/*
 * main --- 主入口点
 */
int
main(int argc, char **argv)
{
	static struct option long_options[] = {
		{"clean-backup-history", no_argument, NULL, 'b'},
		{"debug", no_argument, NULL, 'd'},
		{"dry-run", no_argument, NULL, 'n'},
		{"strip-extension", required_argument, NULL, 'x'},
		{NULL, 0, NULL, 0}
	};
	int			c;

	pg_logging_init(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_archivecleanup"));
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
			puts("pg_archivecleanup (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	while ((c = getopt_long(argc, argv, "bdnx:", long_options, NULL)) != -1)
	{
		switch (c)
		{
			case 'b':			/* Remove backup history files as well */
							/* 同时也删除备份历史文件 */
				cleanBackupHistory = true;
				break;
			case 'd':			/* Debug mode */
							/* 调试模式 */
				pg_logging_increase_verbosity();
				break;
			case 'n':			/* Dry-Run mode */
							/* 模拟运行模式 */
				dryrun = true;
				break;
			case 'x':
				additional_ext = pg_strdup(optarg); /* Extension to remove
													 * from xlogfile names */
													/* 从 xlog 文件名中剥离的后缀扩展名 */
				break;
			default:
				/* getopt already emitted a complaint */
				/* getopt 已经发出了投诉/报错 */
				pg_log_error_hint("Try \"%s --help\" for more information.", progname);
				exit(2);
		}
	}

	/*
	 * We will go to the archiveLocation to check restartWALFileName.
	 * restartWALFileName may not exist anymore, which would not be an error,
	 * so we separate the archiveLocation and restartWALFileName so we can
	 * check separately whether archiveLocation exists, if not that is an
	 * error
	 *
	 * 我们将转到 archiveLocation 来检查 restartWALFileName。
	 * restartWALFileName 可能已不存在，这不会是错误，因此我们将 archiveLocation
	 * 和 restartWALFileName 分开，以便我们可以分别检查 archiveLocation 是否存在，
	 * 如果不存在那就是错误。
	 */
	if (optind < argc)
	{
		archiveLocation = argv[optind];
		optind++;
	}
	else
	{
		pg_log_error("must specify archive location");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(2);
	}

	if (optind < argc)
	{
		restartWALFileName = argv[optind];
		optind++;
	}
	else
	{
		pg_log_error("must specify oldest kept WAL file");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(2);
	}

	if (optind < argc)
	{
		pg_log_error("too many command-line arguments");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(2);
	}

	/*
	 * Check archive exists and other initialization if required.
	 *
	 * 检查归档是否存在，并在需要时进行其他初始化。
	 */
	Initialize();

	/*
	 * Check filename is a valid name, then process to find cut-off
	 *
	 * 检查文件名是否有效，然后处理以找到切断点（cut-off）
	 */
	SetWALFileNameForCleanup();

	pg_log_debug("keeping WAL file \"%s/%s\" and later",
				 archiveLocation, exclusiveCleanupFileName);

	/*
	 * Remove WAL files older than cut-off
	 *
	 * 删除早于切断点的 WAL 文件
	 */
	CleanupPriorWALFiles();

	exit(0);
}

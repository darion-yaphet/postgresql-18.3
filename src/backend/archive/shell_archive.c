/*-------------------------------------------------------------------------
 *
 * shell_archive.c
 *
 * This archiving function uses a user-specified shell command (the
 * archive_command GUC) to copy write-ahead log files.  It is used as the
 * default, but other modules may define their own custom archiving logic.
 *
 * 本归档函数使用用户指定的 shell 命令（archive_command GUC 参数）来复制
 * 预写日志（WAL）文件。它作为默认归档模块使用，但其他模块也可以定义
 * 自己的自定义归档逻辑。
 *
 * Copyright (c) 2022-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/archive/shell_archive.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/wait.h>

#include "access/xlog.h"
#include "archive/archive_module.h"
#include "archive/shell_archive.h"
#include "common/percentrepl.h"
#include "pgstat.h"

static bool shell_archive_configured(ArchiveModuleState *state);
static bool shell_archive_file(ArchiveModuleState *state,
							   const char *file,
							   const char *path);
static void shell_archive_shutdown(ArchiveModuleState *state);

/*
 * shell 归档模块的回调表：启动时无需特殊准备（startup_cb 为 NULL），
 * 配置检查、归档执行、关闭清理分别由下面三个回调完成。
 */
static const ArchiveModuleCallbacks shell_archive_callbacks = {
	.startup_cb = NULL,
	.check_configured_cb = shell_archive_configured,
	.archive_file_cb = shell_archive_file,
	.shutdown_cb = shell_archive_shutdown
};

/*
 * shell_archive_init
 *		返回 shell 归档模块的回调表，是系统默认的归档模块入口。
 *		归档主逻辑在 archiver 进程启动时调用本函数，取得各回调后
 *		在模块的整个生命周期内使用。
 */
const ArchiveModuleCallbacks *
shell_archive_init(void)
{
	return &shell_archive_callbacks;
}

/*
 * shell_archive_configured
 *		检查归档配置是否已就绪：只要 archive_command GUC 变量
 *		XLogArchiveCommand 非空即认为已配置；未设置时通过
 *		arch_module_check_errdetail() 为调用方提供失败原因，返回 false。
 */
static bool
shell_archive_configured(ArchiveModuleState *state)
{
	if (XLogArchiveCommand[0] != '\0')
		return true;

	arch_module_check_errdetail("\"%s\" is not set.",
								"archive_command");
	return false;
}

/*
 * shell_archive_file
 *		核心归档入口：用 archive_command 指定的 shell 命令复制一个 WAL 文件。
 *		流程：先把文件路径转换为系统原生格式；再用实际文件名（%f）与路径
 *		（%p）替换命令中的百分号占位符；然后调用 system() 执行该命令并检查
 *		返回码。返回非零时按退出码、被信号终止、异常状态三种情况分别报告，
 *		其中因信号导致的失败以 FATAL 级别处理（本进程直接退出，由 postmaster
 *		重新拉起 archiver），其余以 LOG 级别报告，本轮归档失败稍后重试；
 *		成功时记录 DEBUG1 日志。返回 true 表示归档成功。
 */
static bool
shell_archive_file(ArchiveModuleState *state, const char *file,
				   const char *path)
{
	char	   *xlogarchcmd;
	char	   *nativePath = NULL;
	int			rc;

	if (path)
	{
		nativePath = pstrdup(path);
		make_native_path(nativePath);
	}

	xlogarchcmd = replace_percent_placeholders(XLogArchiveCommand,
											   "archive_command", "fp",
											   file, nativePath);

	ereport(DEBUG3,
			(errmsg_internal("executing archive command \"%s\"",
							 xlogarchcmd)));

	fflush(NULL);
	pgstat_report_wait_start(WAIT_EVENT_ARCHIVE_COMMAND);
	rc = system(xlogarchcmd);
	pgstat_report_wait_end();

	if (rc != 0)
	{
		/*
		 * If either the shell itself, or a called command, died on a signal,
		 * abort the archiver.  We do this because system() ignores SIGINT and
		 * SIGQUIT while waiting; so a signal is very likely something that
		 * should have interrupted us too.  Also die if the shell got a hard
		 * "command not found" type of error.  If we overreact it's no big
		 * deal, the postmaster will just start the archiver again.
		 */

		/*
		 * 如果 shell 本身或被调用的命令因信号而终止，则中止归档进程。
		 * 我们这样做是因为 system() 在等待期间会忽略 SIGINT 和 SIGQUIT；
		 * 因此出现信号很可能意味着它本来也应该打断我们。如果 shell 遇到
		 * 类似 "command not found" 的硬性错误，同样退出。即使我们反应过度
		 * 也无大碍，postmaster 会重新启动归档进程。
		 */
		int			lev = wait_result_is_any_signal(rc, true) ? FATAL : LOG;

		if (WIFEXITED(rc))
		{
			ereport(lev,
					(errmsg("archive command failed with exit code %d",
							WEXITSTATUS(rc)),
					 errdetail("The failed archive command was: %s",
							   xlogarchcmd)));
		}
		else if (WIFSIGNALED(rc))
		{
#if defined(WIN32)
			ereport(lev,
					(errmsg("archive command was terminated by exception 0x%X",
							WTERMSIG(rc)),
					 errhint("See C include file \"ntstatus.h\" for a description of the hexadecimal value."),
					 errdetail("The failed archive command was: %s",
							   xlogarchcmd)));
#else
			ereport(lev,
					(errmsg("archive command was terminated by signal %d: %s",
							WTERMSIG(rc), pg_strsignal(WTERMSIG(rc))),
					 errdetail("The failed archive command was: %s",
							   xlogarchcmd)));
#endif
		}
		else
		{
			ereport(lev,
					(errmsg("archive command exited with unrecognized status %d",
							rc),
					 errdetail("The failed archive command was: %s",
							   xlogarchcmd)));
		}
		pfree(xlogarchcmd);

		return false;
	}
	pfree(xlogarchcmd);

	elog(DEBUG1, "archived write-ahead log file \"%s\"", file);
	return true;
}

/*
 * shell_archive_shutdown
 *		归档模块的关闭回调：shell 归档模块没有需要释放的资源，
 *		仅输出一条 DEBUG1 日志表示归档进程正在关闭。
 */
static void
shell_archive_shutdown(ArchiveModuleState *state)
{
	elog(DEBUG1, "archiver process shutting down");
}

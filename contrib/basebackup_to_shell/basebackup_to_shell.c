/*-------------------------------------------------------------------------
 *
 * basebackup_to_shell.c
 *	  target base backup files to a shell command
 *
 * Copyright (c) 2016-2025, PostgreSQL Global Development Group
 *
 *	  contrib/basebackup_to_shell/basebackup_to_shell.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "backup/basebackup_target.h"
#include "common/percentrepl.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/acl.h"
#include "utils/guc.h"

PG_MODULE_MAGIC_EXT(
					.name = "basebackup_to_shell",
					.version = PG_VERSION
);

typedef struct bbsink_shell
{
	/* Common information for all types of sink.
	 *
	 * 所有类型水槽的通用信息。
	 */
	bbsink		base;

	/* User-supplied target detail string.
	 *
	 * 用户提供的目标详细信息字符串。
	 */
	char	   *target_detail;

	/* Shell command pattern being used for this backup.
	 *
	 * 用于此备份的 Shell 命令模式。
	 */
	char	   *shell_command;

	/* The command that is currently running.
	 *
	 * 当前正在运行的命令。
	 */
	char	   *current_command;

	/* Pipe to the running command.
	 *
	 * 通过管道传输到正在运行的命令。
	 */
	FILE	   *pipe;
} bbsink_shell;

static void *shell_check_detail(char *target, char *target_detail);
static bbsink *shell_get_sink(bbsink *next_sink, void *detail_arg);

static void bbsink_shell_begin_archive(bbsink *sink,
									   const char *archive_name);
static void bbsink_shell_archive_contents(bbsink *sink, size_t len);
static void bbsink_shell_end_archive(bbsink *sink);
static void bbsink_shell_begin_manifest(bbsink *sink);
static void bbsink_shell_manifest_contents(bbsink *sink, size_t len);
static void bbsink_shell_end_manifest(bbsink *sink);

static const bbsink_ops bbsink_shell_ops = {
	.begin_backup = bbsink_forward_begin_backup,
	.begin_archive = bbsink_shell_begin_archive,
	.archive_contents = bbsink_shell_archive_contents,
	.end_archive = bbsink_shell_end_archive,
	.begin_manifest = bbsink_shell_begin_manifest,
	.manifest_contents = bbsink_shell_manifest_contents,
	.end_manifest = bbsink_shell_end_manifest,
	.end_backup = bbsink_forward_end_backup,
	.cleanup = bbsink_forward_cleanup
};

static char *shell_command = "";
static char *shell_required_role = "";

void
_PG_init(void)
{
	DefineCustomStringVariable("basebackup_to_shell.command",
							   "Shell command to be executed for each backup file.",
							   NULL,
							   &shell_command,
							   "",
							   PGC_SIGHUP,
							   0,
							   NULL, NULL, NULL);

	DefineCustomStringVariable("basebackup_to_shell.required_role",
							   "Backup user must be a member of this role to use shell backup target.",
							   NULL,
							   &shell_required_role,
							   "",
							   PGC_SIGHUP,
							   0,
							   NULL, NULL, NULL);

	MarkGUCPrefixReserved("basebackup_to_shell");

	BaseBackupAddTarget("shell", shell_check_detail, shell_get_sink);
}

/*
 * We choose to defer sanity checking until shell_get_sink(), and so
 * just pass the target detail through without doing anything. However, we do
 * permissions checks here, before any real work has been done.
 *
 * 我们选择将健全性检查推迟到 shell_get_sink() 之前，因此只需传递目标详细信息而不执行任何操作。但是，在完成任何实际工作之前，我们会在这里进行权限检查。
 */
static void *
shell_check_detail(char *target, char *target_detail)
{
	if (shell_required_role[0] != '\0')
	{
		Oid			roleid;

		StartTransactionCommand();
		roleid = get_role_oid(shell_required_role, true);
		if (!has_privs_of_role(GetUserId(), roleid))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to use basebackup_to_shell")));
		CommitTransactionCommand();
	}

	return target_detail;
}

/*
 * Set up a bbsink to implement this base backup target.
 *
 * 设置 bbsink 来实现此基本备份目标。
 *
 * This is also a convenient place to sanity check that a target detail was
 * given if and only if %d is present.
 *
 * 这也是一个方便的地方，可以方便地检查当且仅当 %d 存在时才给出目标详细信息。
 */
static bbsink *
shell_get_sink(bbsink *next_sink, void *detail_arg)
{
	bbsink_shell *sink;
	bool		has_detail_escape = false;
	char	   *c;

	/*
	 * Set up the bbsink.
	 *
	 * 设置 bbsink。
	 *
	 * We remember the current value of basebackup_to_shell.shell_command to
	 * be certain that it can't change under us during the backup.
	 *
	 * 我们记住basebackup_to_shell.shell_command的当前值，以确保在备份期间它不会在我们的控制下更改。
	 */
	sink = palloc0(sizeof(bbsink_shell));
	*((const bbsink_ops **) &sink->base.bbs_ops) = &bbsink_shell_ops;
	sink->base.bbs_next = next_sink;
	sink->target_detail = detail_arg;
	sink->shell_command = pstrdup(shell_command);

	/* Reject an empty shell command.
	 *
	 * 拒绝空 shell 命令。
	 */
	if (sink->shell_command[0] == '\0')
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("shell command for backup is not configured"));

	/* Determine whether the shell command we're using contains %d.
	 *
	 * 确定我们使用的 shell 命令是否包含 %d。
	 */
	for (c = sink->shell_command; *c != '\0'; ++c)
	{
		if (c[0] == '%' && c[1] != '\0')
		{
			if (c[1] == 'd')
				has_detail_escape = true;
			++c;
		}
	}

	/* There should be a target detail if %d was used, and not otherwise.
	 *
	 * 如果使用了%d，则应该有目标详细信息，否则没有。
	 */
	if (has_detail_escape && sink->target_detail == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a target detail is required because the configured command includes %%d"),
				 errhint("Try \"pg_basebackup --target shell:DETAIL ...\"")));
	else if (!has_detail_escape && sink->target_detail != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a target detail is not permitted because the configured command does not include %%d")));

	/*
	 * Since we're passing the string provided by the user to popen(), it will
	 * be interpreted by the shell, which is a potential security
	 * vulnerability, since the user invoking this module is not necessarily a
	 * superuser. To stay out of trouble, we must disallow any shell
	 * metacharacters here; to be conservative and keep things simple, we
	 * allow only alphanumerics.
	 *
	 * 由于我们将用户提供的字符串传递给 popen()，它将由 shell 解释，这是一个潜在的安全漏洞，因为调用此模块的用户不一定是超级用户。为了避免麻烦，我们必须禁止这里使用任何 shell 元字符；为了保守和简单起见，我们只允许使用字母数字。
	 */
	if (sink->target_detail != NULL)
	{
		char	   *d;
		bool		scary = false;

		for (d = sink->target_detail; *d != '\0'; ++d)
		{
			if (*d >= 'a' && *d <= 'z')
				continue;
			if (*d >= 'A' && *d <= 'Z')
				continue;
			if (*d >= '0' && *d <= '9')
				continue;
			scary = true;
			break;
		}

		if (scary)
			ereport(ERROR,
					errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg("target detail must contain only alphanumeric characters"));
	}

	return &sink->base;
}

/*
 * Construct the exact shell command that we're actually going to run,
 * making substitutions as appropriate for escape sequences.
 *
 * 构造我们实际要运行的确切 shell 命令，并根据转义序列进行适当的替换。
 */
static char *
shell_construct_command(const char *base_command, const char *filename,
						const char *target_detail)
{
	return replace_percent_placeholders(base_command, "basebackup_to_shell.command",
										"df", target_detail, filename);
}

/*
 * Finish executing the shell command once all data has been written.
 *
 * 所有数据写入后，完成 shell 命令的执行。
 */
static void
shell_finish_command(bbsink_shell *sink)
{
	int			pclose_rc;

	/* There should be a command running.
	 *
	 * 应该有一个命令正在运行。
	 */
	Assert(sink->current_command != NULL);
	Assert(sink->pipe != NULL);

	/* Close down the pipe we opened.
	 *
	 * 关闭我们打开的管道。
	 */
	pclose_rc = ClosePipeStream(sink->pipe);
	if (pclose_rc == -1)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close pipe to external command: %m")));
	else if (pclose_rc != 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("shell command \"%s\" failed",
						sink->current_command),
				 errdetail_internal("%s", wait_result_to_str(pclose_rc))));
	}

	/* Clean up.
	 *
	 * 清理。
	 */
	sink->pipe = NULL;
	pfree(sink->current_command);
	sink->current_command = NULL;
}

/*
 * Start up the shell command, substituting %f in for the current filename.
 *
 * 启动 shell 命令，用 %f 替换当前文件名。
 */
static void
shell_run_command(bbsink_shell *sink, const char *filename)
{
	/* There should not be anything already running.
	 *
	 * 不应该有任何东西已经在运行。
	 */
	Assert(sink->current_command == NULL);
	Assert(sink->pipe == NULL);

	/* Construct a suitable command.
	 *
	 * 构造一个合适的命令。
	 */
	sink->current_command = shell_construct_command(sink->shell_command,
													filename,
													sink->target_detail);

	/* Run it.
	 *
	 * 运行它。
	 */
	sink->pipe = OpenPipeStream(sink->current_command, PG_BINARY_W);
	if (sink->pipe == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not execute command \"%s\": %m",
						sink->current_command)));
}

/*
 * Send accumulated data to the running shell command.
 *
 * 将累积的数据发送到正在运行的 shell 命令。
 */
static void
shell_send_data(bbsink_shell *sink, size_t len)
{
	/* There should be a command running.
	 *
	 * 应该有一个命令正在运行。
	 */
	Assert(sink->current_command != NULL);
	Assert(sink->pipe != NULL);

	/* Try to write the data.
	 *
	 * 尝试写入数据。
	 */
	if (fwrite(sink->base.bbs_buffer, len, 1, sink->pipe) != 1 ||
		ferror(sink->pipe))
	{
		if (errno == EPIPE)
		{
			/*
			 * The error we're about to throw would shut down the command
			 * anyway, but we may get a more meaningful error message by doing
			 * this. If not, we'll fall through to the generic error below.
			 *
			 * 我们即将抛出的错误无论如何都会关闭命令，但通过这样做我们可能会得到更有意义的错误消息。如果没有，我们将遇到下面的一般错误。
			 */
			shell_finish_command(sink);
			errno = EPIPE;
		}
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to shell backup program: %m")));
	}
}

/*
 * At start of archive, start up the shell command and forward to next sink.
 *
 * 在归档开始时，启动 shell 命令并转发到下一个接收器。
 */
static void
bbsink_shell_begin_archive(bbsink *sink, const char *archive_name)
{
	bbsink_shell *mysink = (bbsink_shell *) sink;

	shell_run_command(mysink, archive_name);
	bbsink_forward_begin_archive(sink, archive_name);
}

/*
 * Send archive contents to command's stdin and forward to next sink.
 *
 * 将存档内容发送到命令的标准输入并转发到下一个接收器。
 */
static void
bbsink_shell_archive_contents(bbsink *sink, size_t len)
{
	bbsink_shell *mysink = (bbsink_shell *) sink;

	shell_send_data(mysink, len);
	bbsink_forward_archive_contents(sink, len);
}

/*
 * At end of archive, shut down the shell command and forward to next sink.
 *
 * 归档结束时，关闭 shell 命令并转发到下一个接收器。
 */
static void
bbsink_shell_end_archive(bbsink *sink)
{
	bbsink_shell *mysink = (bbsink_shell *) sink;

	shell_finish_command(mysink);
	bbsink_forward_end_archive(sink);
}

/*
 * At start of manifest, start up the shell command and forward to next sink.
 *
 * 在清单开始时，启动 shell 命令并转发到下一个接收器。
 */
static void
bbsink_shell_begin_manifest(bbsink *sink)
{
	bbsink_shell *mysink = (bbsink_shell *) sink;

	shell_run_command(mysink, "backup_manifest");
	bbsink_forward_begin_manifest(sink);
}

/*
 * Send manifest contents to command's stdin and forward to next sink.
 *
 * 将清单内容发送到命令的标准输入并转发到下一个接收器。
 */
static void
bbsink_shell_manifest_contents(bbsink *sink, size_t len)
{
	bbsink_shell *mysink = (bbsink_shell *) sink;

	shell_send_data(mysink, len);
	bbsink_forward_manifest_contents(sink, len);
}

/*
 * At end of manifest, shut down the shell command and forward to next sink.
 *
 * 在清单末尾，关闭 shell 命令并转发到下一个接收器。
 */
static void
bbsink_shell_end_manifest(bbsink *sink)
{
	bbsink_shell *mysink = (bbsink_shell *) sink;

	shell_finish_command(mysink);
	bbsink_forward_end_manifest(sink);
}

# Copyright (c) 2021-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# For testing purposes, we just want basebackup_to_shell to write standard
#
# 出于测试目的，我们只希望 basebackup_to_shell 编写标准
# input to a file.  However, Windows doesn't have "cat" or any equivalent, so
#
# 输入到文件。  但是，Windows 没有“cat”或任何等效项，因此
# we use "gzip" for this purpose.
#
# 为此，我们使用“gzip”。
my $gzip = $ENV{'GZIP_PROGRAM'};
if (!defined $gzip || $gzip eq '')
{
	plan skip_all => 'gzip not available';
}

# to ensure path can be embedded in postgresql.conf
#
# 确保路径可以嵌入到 postgresql.conf 中
$gzip =~ s{\\}{/}g if ($PostgreSQL::Test::Utils::windows_os);

my $node = PostgreSQL::Test::Cluster->new('primary');

# Make sure pg_hba.conf is set up to allow connections from backupuser.
#
# 确保 pg_hba.conf 设置为允许来自 backupuser 的连接。
# This is only needed on Windows machines that don't use UNIX sockets.
#
# 仅在不使用 UNIX 套接字的 Windows 计算机上需要这样做。
$node->init(
	allows_streaming => 1,
	auth_extra => [ '--create-role' => 'backupuser' ]);

$node->append_conf('postgresql.conf',
	"shared_preload_libraries = 'basebackup_to_shell'");
$node->start;
$node->safe_psql('postgres', 'CREATE USER backupuser REPLICATION');
$node->safe_psql('postgres', 'CREATE ROLE trustworthy');

# For nearly all pg_basebackup invocations some options should be specified,
#
# 对于几乎所有 pg_basebackup 调用，都应该指定一些选项，
# to keep test times reasonable. Using @pg_basebackup_defs as the first
#
# 保持考试时间合理。使用 @pg_basebackup_defs 作为第一个
# element of the array passed to IPC::Run interpolate the array (as it is
#
# 传递给 IPC::Run 的数组元素对数组进行插值（因为它是
# not a reference to an array)...
#
# 不是对数组的引用）...
my @pg_basebackup_defs =
  ('pg_basebackup', '--no-sync', '--checkpoint' => 'fast');

# This particular test module generally wants to run with -Xfetch, because
#
# 这个特定的测试模块通常希望与 -Xfetch 一起运行，因为
# -Xstream is not supported with a backup target, and with -U backupuser.
#
# 备份目标和 -U backupuser 不支持 -Xstream。
my @pg_basebackup_cmd = (
	@pg_basebackup_defs,
	'--username' => 'backupuser',
	'--wal-method' => 'fetch');

# Can't use this module without setting basebackup_to_shell.command.
#
# 如果不设置basebackup_to_shell.command，则无法使用此模块。
$node->command_fails_like(
	[ @pg_basebackup_cmd, '--target' => 'shell' ],
	qr/shell command for backup is not configured/,
	'fails if basebackup_to_shell.command is not set');

# Configure basebackup_to_shell.command and reload the configuration file.
#
# 配置basebackup_to_shell.command并重新加载配置文件。
my $backup_path = PostgreSQL::Test::Utils::tempdir;
my $escaped_backup_path = $backup_path;
$escaped_backup_path =~ s{\\}{\\\\}g
  if ($PostgreSQL::Test::Utils::windows_os);
my $shell_command =
  $PostgreSQL::Test::Utils::windows_os
  ? qq{"$gzip" --fast > "$escaped_backup_path\\\\%f.gz"}
  : qq{"$gzip" --fast > "$escaped_backup_path/%f.gz"};
$node->append_conf('postgresql.conf',
	"basebackup_to_shell.command='$shell_command'");
$node->reload();

# Should work now.
#
# 现在应该可以工作了。
$node->command_ok(
	[ @pg_basebackup_cmd, '--target' => 'shell' ],
	'backup with no detail: pg_basebackup');
verify_backup('', $backup_path, "backup with no detail");

# Should fail with a detail.
#
# 应该因细节而失败。
$node->command_fails_like(
	[ @pg_basebackup_cmd, '--target' => 'shell:foo' ],
	qr/a target detail is not permitted because the configured command does not include %d/,
	'fails if detail provided without %d');

# Reconfigure to restrict access and require a detail.
#
# 重新配置以限制访问并需要详细信息。
$shell_command =
  $PostgreSQL::Test::Utils::windows_os
  ? qq{"$gzip" --fast > "$escaped_backup_path\\\\%d.%f.gz"}
  : qq{"$gzip" --fast > "$escaped_backup_path/%d.%f.gz"};
$node->append_conf('postgresql.conf',
	"basebackup_to_shell.command='$shell_command'");
$node->append_conf('postgresql.conf',
	"basebackup_to_shell.required_role='trustworthy'");
$node->reload();

# Should fail due to lack of permission.
#
# 应该由于缺乏许可而失败。
$node->command_fails_like(
	[ @pg_basebackup_cmd, '--target' => 'shell' ],
	qr/permission denied to use basebackup_to_shell/,
	'fails if required_role not granted');

# Should fail due to lack of a detail.
#
# 应该会因为缺乏细节而失败。
$node->safe_psql('postgres', 'GRANT trustworthy TO backupuser');
$node->command_fails_like(
	[ @pg_basebackup_cmd, '--target' => 'shell' ],
	qr/a target detail is required because the configured command includes %d/,
	'fails if %d is present and detail not given');

# Should work.
#
# 应该有效。
$node->command_ok([ @pg_basebackup_cmd, '--target' => 'shell:bar' ],
	'backup with detail: pg_basebackup');
verify_backup('bar.', $backup_path, "backup with detail");

done_testing();

sub verify_backup
{
	my ($prefix, $backup_dir, $test_name) = @_;

	ok( -f "$backup_dir/${prefix}backup_manifest.gz",
		"$test_name: backup_manifest.gz was created");
	ok( -f "$backup_dir/${prefix}base.tar.gz",
		"$test_name: base.tar.gz was created");

  SKIP:
	{
		my $tar = $ENV{TAR};
		skip "no tar program available", 1 if (!defined $tar || $tar eq '');

		# Decompress.
		system_or_bail($gzip, '-d',
			$backup_dir . '/' . $prefix . 'backup_manifest.gz');
		system_or_bail($gzip, '-d',
			$backup_dir . '/' . $prefix . 'base.tar.gz');

		# Untar.
		my $extract_path = PostgreSQL::Test::Utils::tempdir;
		system_or_bail(
			$tar,
			'xf' => $backup_dir . '/' . $prefix . 'base.tar',
			'-C' => $extract_path);

		# Verify.
		$node->command_ok(
			[
				'pg_verifybackup',
				'--no-parse-wal',
				'--manifest-path' => "${backup_dir}/${prefix}backup_manifest",
				'--exit-on-error',
				$extract_path
			],
			"$test_name: backup verifies ok");
	}
}

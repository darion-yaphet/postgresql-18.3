
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

# Test replication statistics data in pg_stat_replication_slots is sane after
#
# 测试 pg_stat_replication_slots 中的复制统计数据是否正常
# drop replication slot and restart.
#
# 删除复制槽并重新启动。
use strict;
use warnings FATAL => 'all';
use File::Path qw(rmtree);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Test set-up
#
# 测试设置
my $node = PostgreSQL::Test::Cluster->new('test');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', 'synchronous_commit = on');
$node->start;

# Check that replication slot stats are expected.
#
# 检查复制槽统计信息是否符合预期。
sub test_slot_stats
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($node, $expected, $msg) = @_;

	my $result = $node->safe_psql(
		'postgres', qq[
		SELECT slot_name, total_txns > 0 AS total_txn,
			   total_bytes > 0 AS total_bytes
			   FROM pg_stat_replication_slots
			   ORDER BY slot_name]);
	is($result, $expected, $msg);
}

# Create table.
#
# 创建表。
$node->safe_psql('postgres', "CREATE TABLE test_repl_stat(col1 int)");

# Create replication slots.
#
# 创建复制槽。
$node->safe_psql(
	'postgres', qq[
	SELECT pg_create_logical_replication_slot('regression_slot1', 'test_decoding');
	SELECT pg_create_logical_replication_slot('regression_slot2', 'test_decoding');
	SELECT pg_create_logical_replication_slot('regression_slot3', 'test_decoding');
	SELECT pg_create_logical_replication_slot('regression_slot4', 'test_decoding');
]);

# Insert some data.
#
# 插入一些数据。
$node->safe_psql('postgres',
	"INSERT INTO test_repl_stat values(generate_series(1, 5));");

$node->safe_psql(
	'postgres', qq[
	SELECT data FROM pg_logical_slot_get_changes('regression_slot1', NULL,
	NULL, 'include-xids', '0', 'skip-empty-xacts', '1');
	SELECT data FROM pg_logical_slot_get_changes('regression_slot2', NULL,
	NULL, 'include-xids', '0', 'skip-empty-xacts', '1');
	SELECT data FROM pg_logical_slot_get_changes('regression_slot3', NULL,
	NULL, 'include-xids', '0', 'skip-empty-xacts', '1');
	SELECT data FROM pg_logical_slot_get_changes('regression_slot4', NULL,
	NULL, 'include-xids', '0', 'skip-empty-xacts', '1');
]);

# Wait for the statistics to be updated.
#
# 等待统计数据更新。
$node->poll_query_until(
	'postgres', qq[
	SELECT count(slot_name) >= 4 FROM pg_stat_replication_slots
	WHERE slot_name ~ 'regression_slot'
	AND total_txns > 0 AND total_bytes > 0;
]) or die "Timed out while waiting for statistics to be updated";

# Test to drop one of the replication slot and verify replication statistics data is
#
# 测试删除复制槽之一并验证复制统计数据是否正确
# fine after restart.
#
# 重启后就好了。
$node->safe_psql('postgres',
	"SELECT pg_drop_replication_slot('regression_slot4')");

$node->stop;
$node->start;

# Verify statistics data present in pg_stat_replication_slots are sane after
#
# 验证 pg_stat_replication_slots 中存在的统计数据在之后是否正常
# restart.
test_slot_stats(
	$node,
	qq(regression_slot1|t|t
regression_slot2|t|t
regression_slot3|t|t),
	'check replication statistics are updated');

# Test to remove one of the replication slots and adjust
#
# 尝试移除其中一个复制槽并进行调整
# max_replication_slots accordingly to the number of slots. This leads
#
# max_replication_slots 相应于槽的数量。这导致
# to a mismatch between the number of slots present in the stats file and the
#
# 统计文件中存在的槽数与
# number of stats present in shared memory. We verify
#
# 共享内存中存在的统计数据数量。我们验证
# replication statistics data is fine after restart.
#
# 重启后复制统计数据正常。

$node->stop;
my $datadir = $node->data_dir;
my $slot3_replslotdir = "$datadir/pg_replslot/regression_slot3";

rmtree($slot3_replslotdir);

$node->append_conf('postgresql.conf', 'max_replication_slots = 2');
$node->start;

# Verify statistics data present in pg_stat_replication_slots are sane after
#
# 验证 pg_stat_replication_slots 中存在的统计数据在之后是否正常
# restart.
test_slot_stats(
	$node,
	qq(regression_slot1|t|t
regression_slot2|t|t),
	'check replication statistics after removing the slot file');

# cleanup
$node->safe_psql('postgres', "DROP TABLE test_repl_stat");
$node->safe_psql('postgres',
	"SELECT pg_drop_replication_slot('regression_slot1')");
$node->safe_psql('postgres',
	"SELECT pg_drop_replication_slot('regression_slot2')");

# shutdown
$node->stop;

# Test replication slot stats persistence in a single session.  The slot
#
# 测试单个会话中复制槽统计数据的持久性。  插槽
# is dropped and created concurrently of a session peeking at its data
#
# 在查看其数据的会话的同时删除和创建
# repeatedly, hence holding in its local cache a reference to the stats.
#
# 重复，因此在其本地缓存中保存对统计数据的引用。
$node->start;

my $slot_name_restart = 'regression_slot5';
$node->safe_psql('postgres',
	"SELECT pg_create_logical_replication_slot('$slot_name_restart', 'test_decoding');"
);

# Look at slot data, with a persistent connection.
#
# 看槽数据，有持久连接。
my $bpgsql = $node->background_psql('postgres', on_error_stop => 1);

# Launch query and look at slot data, incrementing the refcount of the
#
# 启动查询并查看槽数据，增加槽的引用计数
# stats entry.
#
# 统计条目。
$bpgsql->query_safe(
	"SELECT pg_logical_slot_peek_binary_changes('$slot_name_restart', NULL, NULL)"
);

# Drop the slot entry.  The stats entry is not dropped yet as the previous
#
# 删除插槽条目。  统计条目尚未像之前那样被删除
# session still holds a reference to it.
#
# session 仍然保留对其的引用。
$node->safe_psql('postgres',
	"SELECT pg_drop_replication_slot('$slot_name_restart')");

# Create again the same slot.  The stats entry is reinitialized, not marked
#
# 再次创建相同的插槽。  统计条目重新初始化，未标记
# as dropped anymore.
#
# 不再掉落了。
$node->safe_psql('postgres',
	"SELECT pg_create_logical_replication_slot('$slot_name_restart', 'test_decoding');"
);

# Look again at the slot data.  The local stats reference should be refreshed
#
# 再看一下插槽数据。  应刷新本地统计参考
# to the reinitialized entry.
#
# 到重新初始化的条目。
$bpgsql->query_safe(
	"SELECT pg_logical_slot_peek_binary_changes('$slot_name_restart', NULL, NULL)"
);
# Drop again the slot, the entry is not dropped yet as the previous session
#
# 再次删除该插槽，该条目尚未像前一个会话那样删除
# still has a refcount on it.
#
# 仍然有重新计数。
$node->safe_psql('postgres',
	"SELECT pg_drop_replication_slot('$slot_name_restart')");

# Shutdown the node, which should happen cleanly with the stats file written
#
# 关闭节点，这应该在写入统计文件后干净地发生
# to disk.  Note that the background session created previously needs to be
#
# 到磁盘。  请注意，之前创建的后台会话需要
# hold *while* the node is shutting down to check that it drops the stats
#
# 按住节点正在关闭的*同时*以检查它是否删除了统计信息
# entry of the slot before writing the stats file.
#
# 在写入统计文件之前槽的条目。
$node->stop;

# Make sure that the node is correctly shut down.  Checking the control file
#
# 确保节点正确关闭。  检查控制文件
# is not enough, as the node may detect that something is incorrect after the
#
# 还不够，因为节点可能会在
# control file has been updated and the shutdown checkpoint is finished, so
#
# 控制文件已更新并且关闭检查点已完成，因此
# also check that the stats file has been written out.
#
# 还要检查统计文件是否已写出。
command_like(
	[ 'pg_controldata', $node->data_dir ],
	qr/Database cluster state:\s+shut down\n/,
	'node shut down ok');

my $stats_file = "$datadir/pg_stat/pgstat.stat";
ok(-f "$stats_file", "stats file must exist after shutdown");

$bpgsql->quit;

done_testing();

# Copyright (c) 2021-2025, PostgreSQL Global Development Group

# Test integrity of intermediate states by PITR to those states
#
# 通过 PITR 测试中间状态的完整性
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# origin node: generate WAL records of interest.
#
# 起源节点：生成感兴趣的WAL记录。
my $origin = PostgreSQL::Test::Cluster->new('origin');
$origin->init(has_archiving => 1, allows_streaming => 1);
$origin->append_conf('postgresql.conf', 'autovacuum = off');
$origin->start;
$origin->backup('my_backup');
# Create a table with each of 6 PK values spanning 1/4 of a block.  Delete the
#
# 创建一个表，其中 6 个 PK 值中的每一个值跨越一个块的 1/4。  删除
# first four, so one index leaf is eligible for deletion.  Make a replication
#
# 前四个，因此一个索引叶符合删除条件。  进行复制
# slot just so pg_walinspect will always have access to later WAL.
#
# slot 只是这样 pg_walinspect 总是可以访问后面的 WAL。
my $setup = <<EOSQL;
BEGIN;
CREATE EXTENSION amcheck;
CREATE EXTENSION pg_walinspect;
CREATE TABLE not_leftmost (c text STORAGE PLAIN);
INSERT INTO not_leftmost
  SELECT repeat(n::text, database_block_size / 4)
  FROM generate_series(1,6) t(n), pg_control_init();
ALTER TABLE not_leftmost ADD CONSTRAINT not_leftmost_pk PRIMARY KEY (c);
DELETE FROM not_leftmost WHERE c ~ '^[1-4]';
SELECT pg_create_physical_replication_slot('for_walinspect', true, false);
COMMIT;
EOSQL
$origin->safe_psql('postgres', $setup);
my $before_vacuum_lsn =
  $origin->safe_psql('postgres', "SELECT pg_current_wal_lsn()");
# VACUUM to delete the aforementioned leaf page.  Force an XLogFlush() by
#
# VACUUM 删除上述叶页。  通过以下方式强制执行 XLogFlush()
# dropping a permanent table.  That way, the XLogReader infrastructure can
#
# 删除永久表。  这样，XLogReader 基础设施就可以
# always see VACUUM's records, even under synchronous_commit=off.  Finally,
#
# 始终会看到 VACUUM 的记录，即使在 synchronous_commit=off 下也是如此。  最后，
# find the LSN of that VACUUM's last UNLINK_PAGE record.
#
# 找到该 VACUUM 的最后一个 UNLINK_PAGE 记录的 LSN。
my $vacuum = <<EOSQL;
SET synchronous_commit = off;
VACUUM (VERBOSE, INDEX_CLEANUP ON) not_leftmost;
CREATE TABLE XLogFlush ();
DROP TABLE XLogFlush;
SELECT max(start_lsn)
  FROM pg_get_wal_records_info('$before_vacuum_lsn', 'FFFFFFFF/FFFFFFFF')
  WHERE resource_manager = 'Btree' AND record_type = 'UNLINK_PAGE';
EOSQL
my $unlink_lsn = $origin->safe_psql('postgres', $vacuum);
$origin->stop;
die "did not find UNLINK_PAGE record" unless $unlink_lsn;

# replica node: amcheck at notable points in the WAL stream
#
# 副本节点：在 WAL 流中的显着点进行 amcheck
my $replica = PostgreSQL::Test::Cluster->new('replica');
$replica->init_from_backup($origin, 'my_backup', has_restoring => 1);
$replica->append_conf('postgresql.conf',
	"recovery_target_lsn = '$unlink_lsn'");
$replica->append_conf('postgresql.conf', 'recovery_target_inclusive = off');
$replica->append_conf('postgresql.conf', 'recovery_target_action = promote');
$replica->start;
$replica->poll_query_until('postgres', "SELECT pg_is_in_recovery() = 'f';")
  or die "Timed out while waiting for PITR promotion";
# recovery done; run amcheck
#
# 恢复完成；运行amcheck
my $debug = "SET client_min_messages = 'debug1'";
my ($rc, $stderr);
$rc = $replica->psql(
	'postgres',
	"$debug; SELECT bt_index_parent_check('not_leftmost_pk', true)",
	stderr => \$stderr);
print STDERR $stderr, "\n";
is($rc, 0, "bt_index_parent_check passes");
like(
	$stderr,
	qr/interrupted page deletion detected/,
	"bt_index_parent_check: interrupted page deletion detected");
$rc = $replica->psql(
	'postgres',
	"$debug; SELECT bt_index_check('not_leftmost_pk', true)",
	stderr => \$stderr);
print STDERR $stderr, "\n";
is($rc, 0, "bt_index_check passes");

done_testing();

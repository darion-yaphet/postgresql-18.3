
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

# Check that a concurrent transaction doesn't cause false negatives in
#
# 检查并发事务不会导致漏报
# pg_check_visible() function
#
# pg_check_visible() 函数
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;


# Initialize the primary node
#
# 初始化主节点
my $node = PostgreSQL::Test::Cluster->new('main');
$node->init(allows_streaming => 1);
$node->start;

# Initialize the streaming standby
#
# 初始化流媒体待机
my $backup_name = 'my_backup';
$node->backup($backup_name);
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($node, $backup_name, has_streaming => 1);
$standby->start;

# Setup another database
#
# 设置另一个数据库
$node->safe_psql("postgres", "CREATE DATABASE other_database;\n");
my $bsession = $node->background_psql('other_database');

# Run a concurrent transaction
#
# 运行并发事务
$bsession->query_safe(
	qq[
	BEGIN;
	SELECT txid_current();
]);

# Create a sample table and run vacuum
#
# 创建示例表并运行vacuum
$node->safe_psql("postgres",
		"CREATE EXTENSION pg_visibility;\n"
	  . "CREATE TABLE vacuum_test AS SELECT 42 i;\n"
	  . "VACUUM (disable_page_skipping) vacuum_test;");

# Run pg_check_visible()
#
# 运行 pg_check_visible()
my $result = $node->safe_psql("postgres",
	"SELECT * FROM pg_check_visible('vacuum_test');");

# There should be no false negatives
#
# 不应该有假阴性
ok($result eq "", "pg_check_visible() detects no errors");

# Run pg_check_visible() on standby
#
# 在待机状态下运行 pg_check_visible()
$node->wait_for_catchup($standby);
$result = $standby->safe_psql("postgres",
	"SELECT * FROM pg_check_visible('vacuum_test');");

# There should be no false negatives either
#
# 也不应该有假阴性
ok($result eq "", "pg_check_visible() detects no errors");

# Shutdown
$bsession->query_safe("COMMIT;");
$bsession->quit;
$node->stop;
$standby->stop;

done_testing();

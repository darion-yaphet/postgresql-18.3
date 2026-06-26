
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

# Test generic xlog record work for bloom index replication.
#
# 测试布隆索引复制的通用 xlog 记录工作。
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node_primary;
my $node_standby;

# Run few queries on both primary and standby and check their results match.
#
# 在主数据库和备用数据库上运行一些查询并检查它们的结果是否匹配。
sub test_index_replay
{
	my ($test_name) = @_;

	local $Test::Builder::Level = $Test::Builder::Level + 1;

	# Wait for standby to catch up
	#
	# 等待待机赶上
	$node_primary->wait_for_catchup($node_standby);

	my $queries = qq(SET enable_seqscan=off;
SET enable_bitmapscan=on;
SET enable_indexscan=on;
SELECT * FROM tst WHERE i = 0;
SELECT * FROM tst WHERE i = 3;
SELECT * FROM tst WHERE t = 'b';
SELECT * FROM tst WHERE t = 'f';
SELECT * FROM tst WHERE i = 3 AND t = 'c';
SELECT * FROM tst WHERE i = 7 AND t = 'e';
);

	# Run test queries and compare their result
	#
	# 运行测试查询并比较它们的结果
	my $primary_result = $node_primary->safe_psql("postgres", $queries);
	my $standby_result = $node_standby->safe_psql("postgres", $queries);

	is($primary_result, $standby_result, "$test_name: query result matches");
	return;
}

# Initialize primary node
#
# 初始化主节点
$node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1);
$node_primary->start;
my $backup_name = 'my_backup';

# Take backup
#
# 进行备份
$node_primary->backup($backup_name);

# Create streaming standby linking to primary
#
# 创建链接到主设备的流式备用设备
$node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->init_from_backup($node_primary, $backup_name,
	has_streaming => 1);
$node_standby->start;

# Create some bloom index on primary
#
# 在主节点上创建一些Bloom索引
$node_primary->safe_psql("postgres", "CREATE EXTENSION bloom;");
$node_primary->safe_psql("postgres", "CREATE TABLE tst (i int4, t text);");
$node_primary->safe_psql("postgres",
	"INSERT INTO tst SELECT i%10, substr(encode(sha256(i::text::bytea), 'hex'), 1, 1) FROM generate_series(1,10000) i;"
);
$node_primary->safe_psql("postgres",
	"CREATE INDEX bloomidx ON tst USING bloom (i, t) WITH (col1 = 3);");

# Test that queries give same result
#
# 测试查询是否给出相同的结果
test_index_replay('initial');

# Run 10 cycles of table modification. Run test queries after each modification.
#
# 运行 10 个表修改周期。每次修改后运行测试查询。
for my $i (1 .. 10)
{
	$node_primary->safe_psql("postgres", "DELETE FROM tst WHERE i = $i;");
	test_index_replay("delete $i");
	$node_primary->safe_psql("postgres", "VACUUM tst;");
	test_index_replay("vacuum $i");
	my ($start, $end) = (100001 + ($i - 1) * 10000, 100000 + $i * 10000);
	$node_primary->safe_psql("postgres",
		"INSERT INTO tst SELECT i%10, substr(encode(sha256(i::text::bytea), 'hex'), 1, 1) FROM generate_series($start,$end) i;"
	);
	test_index_replay("insert $i");
}

done_testing();

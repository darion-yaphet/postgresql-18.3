
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;

use Test::More;

my $node;

#
# Test set-up
#
# 测试设置
#
$node = PostgreSQL::Test::Cluster->new('test');
$node->init(no_data_checksums => 1);
$node->append_conf('postgresql.conf', 'autovacuum=off');
$node->start;
$node->safe_psql('postgres', q(CREATE EXTENSION amcheck));

#
# Check a table with data loaded but no corruption, freezing, etc.
#
# 检查已加载数据但没有损坏、冻结等情况的表。
#
fresh_test_table('test');
check_all_options_uncorrupted('test', 'plain');

#
# Check a corrupt table
#
# 检查损坏的表
#
fresh_test_table('test');
corrupt_first_page('test');
detects_heap_corruption("verify_heapam('test')", "plain corrupted table");
detects_heap_corruption(
	"verify_heapam('test', skip := 'all-visible')",
	"plain corrupted table skipping all-visible");
detects_heap_corruption(
	"verify_heapam('test', skip := 'all-frozen')",
	"plain corrupted table skipping all-frozen");
detects_heap_corruption(
	"verify_heapam('test', check_toast := false)",
	"plain corrupted table skipping toast");
detects_heap_corruption(
	"verify_heapam('test', startblock := 0, endblock := 0)",
	"plain corrupted table checking only block zero");

#
# Check a corrupt table with all-frozen data
#
# 检查包含全部冻结数据的损坏表
#
fresh_test_table('test');
$node->safe_psql('postgres', q(VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) test));
detects_no_corruption("verify_heapam('test')",
	"all-frozen not corrupted table");
corrupt_first_page('test');
detects_heap_corruption("verify_heapam('test')",
	"all-frozen corrupted table");
detects_no_corruption(
	"verify_heapam('test', skip := 'all-frozen')",
	"all-frozen corrupted table skipping all-frozen");

#
# Check a sequence with no corruption.  The current implementation of sequences
#
# 检查序列没有损坏。  当前序列的实现
# doesn't require its own test setup, since sequences are really just heap
#
# 不需要自己的测试设置，因为序列实际上只是堆
# tables under-the-hood.  To guard against future implementation changes made
#
# 桌子在引擎盖下。  防止未来实施变更
# without remembering to update verify_heapam, we create and exercise a
#
# 在不记得更新 verify_heapam 的情况下，我们创建并执行了
# sequence, checking along the way that it passes corruption checks.
#
# 序列，沿途检查它是否通过损坏检查。
#
fresh_test_sequence('test_seq');
check_all_options_uncorrupted('test_seq', 'plain');
advance_test_sequence('test_seq');
check_all_options_uncorrupted('test_seq', 'plain');
set_test_sequence('test_seq');
check_all_options_uncorrupted('test_seq', 'plain');
reset_test_sequence('test_seq');
check_all_options_uncorrupted('test_seq', 'plain');

# Returns the filesystem path for the named relation.
#
# 返回命名关系的文件系统路径。
sub relation_filepath
{
	my ($relname) = @_;

	my $pgdata = $node->data_dir;
	my $rel = $node->safe_psql('postgres',
		qq(SELECT pg_relation_filepath('$relname')));
	die "path not found for relation $relname" unless defined $rel;
	return "$pgdata/$rel";
}

# (Re)create and populate a test table of the given name.
#
# （重新）创建并填充给定名称的测试表。
sub fresh_test_table
{
	my ($relname) = @_;

	return $node->safe_psql(
		'postgres', qq(
		DROP TABLE IF EXISTS $relname CASCADE;
		CREATE TABLE $relname (a integer, b text);
		ALTER TABLE $relname SET (autovacuum_enabled=false);
		ALTER TABLE $relname ALTER b SET STORAGE external;
		INSERT INTO $relname (a, b)
			(SELECT gs, repeat('b',gs*10) FROM generate_series(1,1000) gs);
		BEGIN;
		SAVEPOINT s1;
		SELECT 1 FROM $relname WHERE a = 42 FOR UPDATE;
		UPDATE $relname SET b = b WHERE a = 42;
		RELEASE s1;
		SAVEPOINT s1;
		SELECT 1 FROM $relname WHERE a = 42 FOR UPDATE;
		UPDATE $relname SET b = b WHERE a = 42;
		COMMIT;
	));
}

# Create a test sequence of the given name.
#
# 创建给定名称的测试序列。
sub fresh_test_sequence
{
	my ($seqname) = @_;

	return $node->safe_psql(
		'postgres', qq(
		DROP SEQUENCE IF EXISTS $seqname CASCADE;
		CREATE SEQUENCE $seqname
			INCREMENT BY 13
			MINVALUE 17
			START WITH 23;
		SELECT nextval('$seqname');
		SELECT setval('$seqname', currval('$seqname') + nextval('$seqname'));
	));
}

# Call SQL functions to increment the sequence
#
# 调用 SQL 函数来递增序列
sub advance_test_sequence
{
	my ($seqname) = @_;

	return $node->safe_psql(
		'postgres', qq(
		SELECT nextval('$seqname');
	));
}

# Call SQL functions to set the sequence
#
# 调用SQL函数设置顺序
sub set_test_sequence
{
	my ($seqname) = @_;

	return $node->safe_psql(
		'postgres', qq(
		SELECT setval('$seqname', 102);
	));
}

# Call SQL functions to reset the sequence
#
# 调用SQL函数重置序列
sub reset_test_sequence
{
	my ($seqname) = @_;

	return $node->safe_psql(
		'postgres', qq(
		ALTER SEQUENCE $seqname RESTART WITH 51
	));
}

# Stops the test node, corrupts the first page of the named relation, and
#
# 停止测试节点，损坏指定关系的第一页，并且
# restarts the node.
#
# 重新启动节点。
sub corrupt_first_page
{
	my ($relname) = @_;
	my $relpath = relation_filepath($relname);

	$node->stop;

	my $fh;
	open($fh, '+<', $relpath)
	  or BAIL_OUT("open failed: $!");
	binmode $fh;

	# Corrupt some line pointers.  The values are chosen to hit the
	#
	# 损坏一些行指针。  选择的值是为了达到
	# various line-pointer-corruption checks in verify_heapam.c
	#
	# verify_heapam.c 中的各种行指针损坏检查
	# on both little-endian and big-endian architectures.
	#
	# 在小端和大端架构上。
	sysseek($fh, 32, 0)
	  or BAIL_OUT("sysseek failed: $!");
	syswrite(
		$fh,
		pack("L*",
			0xAAA15550, 0xAAA0D550, 0x00010000,
			0x00008000, 0x0000800F, 0x001e8000)
	) or BAIL_OUT("syswrite failed: $!");
	close($fh)
	  or BAIL_OUT("close failed: $!");

	$node->start;
}

sub detects_heap_corruption
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($function, $testname) = @_;

	detects_corruption(
		$function,
		$testname,
		qr/line pointer redirection to item at offset \d+ precedes minimum offset \d+/,
		qr/line pointer redirection to item at offset \d+ exceeds maximum offset \d+/,
		qr/line pointer to page offset \d+ is not maximally aligned/,
		qr/line pointer length \d+ is less than the minimum tuple header size \d+/,
		qr/line pointer to page offset \d+ with length \d+ ends beyond maximum page offset \d+/,
	);
}

sub detects_corruption
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($function, $testname, @re) = @_;

	my $result = $node->safe_psql('postgres', qq(SELECT * FROM $function));
	like($result, $_, $testname) for (@re);
}

sub detects_no_corruption
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($function, $testname) = @_;

	my $result = $node->safe_psql('postgres', qq(SELECT * FROM $function));
	is($result, '', $testname);
}

# Check various options are stable (don't abort) and do not report corruption
#
# 检查各种选项是否稳定（不要中止）并且不报告损坏
# when running verify_heapam on an uncorrupted test table.
#
# 在未损坏的测试表上运行 verify_heapam 时。
#
# The relname *must* be an uncorrupted table, or this will fail.
#
# relname *必须*是一个未损坏的表，否则将会失败。
#
# The prefix is used to identify the test, along with the options,
#
# 前缀用于识别测试以及选项，
# and should be unique.
#
# 并且应该是唯一的。
sub check_all_options_uncorrupted
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($relname, $prefix) = @_;

	for my $stop (qw(true false))
	{
		for my $check_toast (qw(true false))
		{
			for my $skip ("'none'", "'all-frozen'", "'all-visible'")
			{
				for my $startblock (qw(NULL 0))
				{
					for my $endblock (qw(NULL 0))
					{
						my $opts =
							"on_error_stop := $stop, "
						  . "check_toast := $check_toast, "
						  . "skip := $skip, "
						  . "startblock := $startblock, "
						  . "endblock := $endblock";

						detects_no_corruption(
							"verify_heapam('$relname', $opts)",
							"$prefix: $opts");
					}
				}
			}
		}
	}
}

done_testing();

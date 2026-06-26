CREATE TABLE heaptest (a integer, b text);
REVOKE ALL ON heaptest FROM PUBLIC;

-- Check that invalid skip option is rejected
--
-- 检查无效的跳过选项是否被拒绝
SELECT * FROM verify_heapam(relation := 'heaptest', skip := 'rope');

-- Check specifying invalid block ranges when verifying an empty table
--
-- 验证空表时检查指定无效块范围
SELECT * FROM verify_heapam(relation := 'heaptest', startblock := 0, endblock := 0);
SELECT * FROM verify_heapam(relation := 'heaptest', startblock := 5, endblock := 8);

-- Check that valid options are not rejected nor corruption reported
--
-- 检查有效选项未被拒绝或损坏报告
-- for an empty table, and that skip enum-like parameter is case-insensitive
--
-- 对于空表，并且跳过类似枚举的参数不区分大小写
SELECT * FROM verify_heapam(relation := 'heaptest', skip := 'none');
SELECT * FROM verify_heapam(relation := 'heaptest', skip := 'all-frozen');
SELECT * FROM verify_heapam(relation := 'heaptest', skip := 'all-visible');
SELECT * FROM verify_heapam(relation := 'heaptest', skip := 'None');
SELECT * FROM verify_heapam(relation := 'heaptest', skip := 'All-Frozen');
SELECT * FROM verify_heapam(relation := 'heaptest', skip := 'All-Visible');
SELECT * FROM verify_heapam(relation := 'heaptest', skip := 'NONE');
SELECT * FROM verify_heapam(relation := 'heaptest', skip := 'ALL-FROZEN');
SELECT * FROM verify_heapam(relation := 'heaptest', skip := 'ALL-VISIBLE');


-- Add some data so subsequent tests are not entirely trivial
--
-- 添加一些数据，以便后续测试变得不那么简单
INSERT INTO heaptest (a, b)
	(SELECT gs, repeat('x', gs)
		FROM generate_series(1,50) gs);

-- pg_stat_io test:
--
-- pg_stat_io测试：
-- verify_heapam always uses a BAS_BULKREAD BufferAccessStrategy, whereas a
--
-- verify_heapam 始终使用 BAS_BULKREAD BufferAccessStrategy，而
-- sequential scan does so only if the table is large enough when compared to
--
-- 仅当与比较时表足够大时，顺序扫描才会执行此操作
-- shared buffers (see initscan()). CREATE DATABASE ... also unconditionally
--
-- 共享缓冲区（请参阅 initscan()）。 CREATE DATABASE ...也是无条件的
-- uses a BAS_BULKREAD strategy, but we have chosen to use a tablespace and
--
-- 使用 BAS_BULKREAD 策略，但我们选择使用表空间并且
-- verify_heapam to provide coverage instead of adding another expensive
--
-- verify_heapam 提供覆盖范围，而不是添加另一个昂贵的
-- operation to the main regression test suite.
--
-- 对主回归测试套件的操作。
--
-- Create an alternative tablespace and move the heaptest table to it, causing
--
-- 创建一个备用表空间并将 heaptest 表移至其中，导致
-- it to be rewritten and all the blocks to reliably evicted from shared
--
-- 它被重写并且所有块都被可靠地从共享中逐出
-- buffers -- guaranteeing actual reads when we next select from it in the
--
-- 缓冲区——保证当我们下次从中选择时实际读取
-- same transaction.  The heaptest table is smaller than the default
--
-- 相同的交易。  heaptest 表小于默认值
-- wal_skip_threshold, so a wal_level=minimal commit reads the table into
--
-- wal_skip_threshold，因此 wal_level=minimal 提交将表读入
-- shared_buffers.  A transaction delays that and excludes any autovacuum.
--
-- 共享缓冲区。  事务会延迟该过程并排除任何自动清理。
SET allow_in_place_tablespaces = true;
CREATE TABLESPACE regress_test_stats_tblspc LOCATION '';
SELECT sum(reads) AS stats_bulkreads_before
  FROM pg_stat_io WHERE context = 'bulkread' \gset
BEGIN;
ALTER TABLE heaptest SET TABLESPACE regress_test_stats_tblspc;
-- Check that valid options are not rejected nor corruption reported
--
-- 检查有效选项未被拒绝或损坏报告
-- for a non-empty table
--
-- 对于非空表
SELECT * FROM verify_heapam(relation := 'heaptest', skip := 'none');
SELECT * FROM verify_heapam(relation := 'heaptest', skip := 'all-frozen');
SELECT * FROM verify_heapam(relation := 'heaptest', skip := 'all-visible');
SELECT * FROM verify_heapam(relation := 'heaptest', startblock := 0, endblock := 0);
COMMIT;

-- verify_heapam should have read in the page written out by
--
-- verify_heapam 应该读入由 写出的页面
--   ALTER TABLE ... SET TABLESPACE ...
--
-- 更改表...设置表空间...
-- causing an additional bulkread, which should be reflected in pg_stat_io.
--
-- 导致额外的批量读取，这应该反映在 pg_stat_io 中。
SELECT pg_stat_force_next_flush();
SELECT sum(reads) AS stats_bulkreads_after
  FROM pg_stat_io WHERE context = 'bulkread' \gset
SELECT :stats_bulkreads_after > :stats_bulkreads_before;

CREATE ROLE regress_heaptest_role;

-- verify permissions are checked (error due to function not callable)
--
-- 验证权限已检查（由于函数不可调用而导致错误）
SET ROLE regress_heaptest_role;
SELECT * FROM verify_heapam(relation := 'heaptest');
RESET ROLE;

GRANT EXECUTE ON FUNCTION verify_heapam(regclass, boolean, boolean, text, bigint, bigint) TO regress_heaptest_role;

-- verify permissions are now sufficient
--
-- 验证权限现在是否足够
SET ROLE regress_heaptest_role;
SELECT * FROM verify_heapam(relation := 'heaptest');
RESET ROLE;

-- Check specifying invalid block ranges when verifying a non-empty table.
--
-- 验证非空表时检查指定无效块范围。
SELECT * FROM verify_heapam(relation := 'heaptest', startblock := 0, endblock := 10000);
SELECT * FROM verify_heapam(relation := 'heaptest', startblock := 10000, endblock := 11000);

-- Vacuum freeze to change the xids encountered in subsequent tests
--
-- 真空冻结以改变后续测试中遇到的xids
VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) heaptest;

-- Check that valid options are not rejected nor corruption reported
--
-- 检查有效选项未被拒绝或损坏报告
-- for a non-empty frozen table
--
-- 对于非空的冻结表
SELECT * FROM verify_heapam(relation := 'heaptest', skip := 'none');
SELECT * FROM verify_heapam(relation := 'heaptest', skip := 'all-frozen');
SELECT * FROM verify_heapam(relation := 'heaptest', skip := 'all-visible');
SELECT * FROM verify_heapam(relation := 'heaptest', startblock := 0, endblock := 0);

-- Check that partitioned tables (the parent ones) which don't have visibility
--
-- 检查不具有可见性的分区表（父表）
-- maps are rejected
--
-- 地图被拒绝
CREATE TABLE test_partitioned (a int, b text default repeat('x', 5000))
			 PARTITION BY list (a);
SELECT * FROM verify_heapam('test_partitioned',
							startblock := NULL,
							endblock := NULL);

-- Check that valid options are not rejected nor corruption reported
--
-- 检查有效选项未被拒绝或损坏报告
-- for an empty partition table (the child one)
--
-- 对于空分区表（子分区表）
CREATE TABLE test_partition partition OF test_partitioned FOR VALUES IN (1);
SELECT * FROM verify_heapam('test_partition',
							startblock := NULL,
							endblock := NULL);

-- Check that valid options are not rejected nor corruption reported
--
-- 检查有效选项未被拒绝或损坏报告
-- for a non-empty partition table (the child one)
--
-- 对于非空分区表（子分区表）
INSERT INTO test_partitioned (a) (SELECT 1 FROM generate_series(1,1000) gs);
SELECT * FROM verify_heapam('test_partition',
							startblock := NULL,
							endblock := NULL);

-- Check that indexes are rejected
--
-- 检查索引是否被拒绝
CREATE INDEX test_index ON test_partition (a);
SELECT * FROM verify_heapam('test_index',
							startblock := NULL,
							endblock := NULL);

-- Check that views are rejected
--
-- 检查视图是否被拒绝
CREATE VIEW test_view AS SELECT 1;
SELECT * FROM verify_heapam('test_view',
							startblock := NULL,
							endblock := NULL);

-- Check that sequences are rejected
--
-- 检查序列是否被拒绝
CREATE SEQUENCE test_sequence;
SELECT * FROM verify_heapam('test_sequence',
							startblock := NULL,
							endblock := NULL);

-- Check that foreign tables are rejected
--
-- 检查外部表是否被拒绝
CREATE FOREIGN DATA WRAPPER dummy;
CREATE SERVER dummy_server FOREIGN DATA WRAPPER dummy;
CREATE FOREIGN TABLE test_foreign_table () SERVER dummy_server;
SELECT * FROM verify_heapam('test_foreign_table',
							startblock := NULL,
							endblock := NULL);

-- cleanup
DROP TABLE heaptest;
DROP TABLESPACE regress_test_stats_tblspc;
DROP TABLE test_partition;
DROP TABLE test_partitioned;
DROP OWNED BY regress_heaptest_role; -- permissions
DROP ROLE regress_heaptest_role;

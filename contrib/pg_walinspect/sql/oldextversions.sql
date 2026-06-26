-- Test old extension version entry points.
--
-- 测试旧的扩展版本入口点。

CREATE EXTENSION pg_walinspect WITH VERSION '1.0';

-- Mask DETAIL messages as these could refer to current LSN positions.
--
-- 屏蔽 DETAIL 消息，因为这些消息可能引用当前的 LSN 位置。
\set VERBOSITY terse

-- List what version 1.0 contains, using a locale-independent sorting.
--
-- 使用与区域设置无关的排序列出版本 1.0 包含的内容。
SELECT pg_describe_object(classid, objid, 0) AS obj
  FROM pg_depend
  WHERE refclassid = 'pg_extension'::regclass AND
    refobjid = (SELECT oid FROM pg_extension
                  WHERE extname = 'pg_walinspect') AND deptype = 'e'
  ORDER BY pg_describe_object(classid, objid, 0) COLLATE "C";

-- Make sure checkpoints don't interfere with the test.
--
-- 确保检查点不会干扰测试。
SELECT 'init' FROM pg_create_physical_replication_slot('regress_pg_walinspect_slot', true, false);

CREATE TABLE sample_tbl(col1 int, col2 int);
SELECT pg_current_wal_lsn() AS wal_lsn1 \gset
INSERT INTO sample_tbl SELECT * FROM generate_series(1, 2);

-- Tests for the past functions.
--
-- 测试过去的功能。
SELECT COUNT(*) >= 1 AS ok FROM pg_get_wal_records_info_till_end_of_wal(:'wal_lsn1');
SELECT COUNT(*) >= 1 AS ok FROM pg_get_wal_stats_till_end_of_wal(:'wal_lsn1');
-- Failures with start LSNs.
--
-- 启动 LSN 失败。
SELECT * FROM pg_get_wal_records_info_till_end_of_wal('FFFFFFFF/FFFFFFFF');
SELECT * FROM pg_get_wal_stats_till_end_of_wal('FFFFFFFF/FFFFFFFF');

-- Move to new version 1.1.
--
-- 移至新版本 1.1。
ALTER EXTENSION pg_walinspect UPDATE TO '1.1';

-- List what version 1.1 contains.
--
-- 列出 1.1 版本包含的内容。
\dx+ pg_walinspect

SELECT pg_drop_replication_slot('regress_pg_walinspect_slot');

DROP TABLE sample_tbl;
DROP EXTENSION pg_walinspect;

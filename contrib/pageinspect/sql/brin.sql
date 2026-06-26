CREATE TABLE test1 (a int, b text);
INSERT INTO test1 VALUES (1, 'one');
CREATE INDEX test1_a_idx ON test1 USING brin (a);

SELECT brin_page_type(get_raw_page('test1_a_idx', 0));
SELECT brin_page_type(get_raw_page('test1_a_idx', 1));
SELECT brin_page_type(get_raw_page('test1_a_idx', 2));

SELECT * FROM brin_metapage_info(get_raw_page('test1_a_idx', 0));
SELECT * FROM brin_metapage_info(get_raw_page('test1_a_idx', 1));

SELECT * FROM brin_revmap_data(get_raw_page('test1_a_idx', 0)) LIMIT 5;
SELECT * FROM brin_revmap_data(get_raw_page('test1_a_idx', 1)) LIMIT 5;

SELECT * FROM brin_page_items(get_raw_page('test1_a_idx', 2), 'test1_a_idx')
    ORDER BY blknum, attnum LIMIT 5;

-- Mask DETAIL messages as these are not portable across architectures.
--
-- 屏蔽 DETAIL 消息，因为这些消息不可跨架构移植。
\set VERBOSITY terse

-- Failures for non-BRIN index.
--
-- 非 BRIN 索引失败。
CREATE INDEX test1_a_btree ON test1 (a);
SELECT brin_page_items(get_raw_page('test1_a_btree', 0), 'test1_a_btree');
SELECT brin_page_items(get_raw_page('test1_a_btree', 0), 'test1_a_idx');

-- Invalid special area size
--
-- 特殊区域大小无效
SELECT brin_page_type(get_raw_page('test1', 0));
SELECT * FROM brin_metapage_info(get_raw_page('test1', 0));
SELECT * FROM brin_revmap_data(get_raw_page('test1', 0));
\set VERBOSITY default

-- Tests with all-zero pages.
--
-- 使用全零页面进行测试。
SHOW block_size \gset
SELECT brin_page_type(decode(repeat('00', :block_size), 'hex'));
SELECT brin_page_items(decode(repeat('00', :block_size), 'hex'), 'test1_a_idx');
SELECT brin_metapage_info(decode(repeat('00', :block_size), 'hex'));
SELECT brin_revmap_data(decode(repeat('00', :block_size), 'hex'));

-- Test that partial indexes have all pages, including empty ones.
--
-- 测试部分索引是否包含所有页面，包括空页面。
CREATE TABLE test2 (a int);
INSERT INTO test2 SELECT i FROM generate_series(1,1000) s(i);

-- No rows match the index predicate, make sure the index has the right number
--
-- 没有行与索引谓词匹配，请确保索引具有正确的数字
-- of ranges (same as number of page ranges).
--
-- 范围数（与页面范围数相同）。
CREATE INDEX ON test2 USING brin (a) WITH (pages_per_range=1) WHERE (a IS NULL);

ANALYZE test2;

-- Does the index have one summary of the relation?
--
-- 该索引是否有一种关系摘要？
SELECT (COUNT(*) = (SELECT relpages FROM pg_class WHERE relname = 'test2')) AS ranges_do_match
 FROM generate_series((SELECT (lastrevmappage + 1) FROM brin_metapage_info(get_raw_page('test2_a_idx', 0))),
                      (SELECT (relpages - 1) FROM pg_class WHERE relname = 'test2_a_idx')) AS pages(p),
      LATERAL brin_page_items(get_raw_page('test2_a_idx', p), 'test2_a_idx') AS items;

DROP TABLE test1;
DROP TABLE test2;

-- Test that parallel index build produces the same BRIN index as serial build.
--
-- 测试并行索引构建是否生成与串行构建相同的 BRIN 索引。
CREATE TABLE brin_parallel_test (a int, b text, c bigint) WITH (fillfactor=40);

-- Generate a table with a mix of NULLs and non-NULL values (and data suitable
--
-- 生成一个混合有 NULL 和非 NULL 值的表（以及适合的数据）
-- for the different opclasses we build later).
--
-- 对于我们稍后构建的不同操作类）。
INSERT INTO brin_parallel_test
SELECT (CASE WHEN (mod(i,231) = 0) OR (i BETWEEN 3500 AND 4000) THEN NULL ELSE i END),
       (CASE WHEN (mod(i,233) = 0) OR (i BETWEEN 3750 AND 4250) THEN NULL ELSE encode(sha256(i::text::bytea), 'hex') END),
       (CASE WHEN (mod(i,233) = 0) OR (i BETWEEN 3850 AND 4500) THEN NULL ELSE (i/100) + mod(i,8) END)
  FROM generate_series(1,5000) S(i);

-- Build an index with different opclasses - minmax, bloom and minmax-multi.
--
-- 使用不同的操作类构建索引 - minmax、bloom 和 minmax-multi。
--
-- For minmax and opclass this is simple, but for minmax-multi we need to be
--
-- 对于 minmax 和 opclass 这很简单，但是对于 minmax-multi 我们需要
-- careful, because the result depends on the order in which values are added
--
-- 小心，因为结果取决于值添加的顺序
-- to the summary, which in turn affects how are values merged etc. The order
--
-- 到摘要，这反过来又影响值的合并方式等。顺序
-- of merging results from workers has similar effect. All those summaries
--
-- 合并工人的结果也有类似的效果。所有这些总结
-- should produce correct query results, but it means we can't compare them
--
-- 应该产生正确的查询结果，但这意味着我们无法比较它们
-- using equality (which is what EXCEPT does). To work around this issue, we
--
-- 使用相等（这就是 EXCEPT 所做的）。为了解决这个问题，我们
-- generated the data to only have very small number of distinct values per
--
-- 生成的数据每个只有很少数量的不同值
-- range, so that no merging is needed. This makes the results deterministic.
--
-- 范围，因此不需要合并。这使得结果具有确定性。

-- build index without parallelism
--
-- 建立无并行索引
SET max_parallel_maintenance_workers = 0;
CREATE INDEX brin_test_serial_idx ON brin_parallel_test
 USING brin (a int4_minmax_ops, a int4_bloom_ops, b, c int8_minmax_multi_ops)
  WITH (pages_per_range=7)
 WHERE NOT (a BETWEEN 1000 and 1500);

-- build index using parallelism
--
-- 使用并行性构建索引
--
-- Set a couple parameters to force parallel build for small table. There's a
--
-- 设置几个参数以强制并行构建小表。有一个
-- requirement for table size, so disable that. Also, plan_create_index_workers
--
-- 对表大小有要求，因此禁用它。另外，plan_create_index_workers
-- assumes each worker will use work_mem=32MB for sorting (which works for btree,
--
-- 假设每个工作人员将使用 work_mem=32MB 进行排序（适用于 btree，
-- but not really for BRIN), so we set maintenance_work_mem for 4 workers.
--
-- 但对于 BRIN 来说并非如此），所以我们为 4 个工人设置了 Maintenance_work_mem。
SET min_parallel_table_scan_size = 0;
SET max_parallel_maintenance_workers = 4;
SET maintenance_work_mem = '128MB';
CREATE INDEX brin_test_parallel_idx ON brin_parallel_test
 USING brin (a int4_minmax_ops, a int4_bloom_ops, b, c int8_minmax_multi_ops)
  WITH (pages_per_range=7)
 WHERE NOT (a BETWEEN 1000 and 1500);

SELECT relname, relpages
  FROM pg_class
 WHERE relname IN ('brin_test_serial_idx', 'brin_test_parallel_idx')
 ORDER BY relname;

-- Check that (A except B) and (B except A) is empty, which means the indexes
--
-- 检查(A except B)和(B except A)是否为空，这意味着索引
-- are the same.
--
-- 是一样的。

SELECT * FROM brin_page_items(get_raw_page('brin_test_parallel_idx', 2), 'brin_test_parallel_idx')
EXCEPT
SELECT * FROM brin_page_items(get_raw_page('brin_test_serial_idx', 2), 'brin_test_serial_idx');

SELECT * FROM brin_page_items(get_raw_page('brin_test_serial_idx', 2), 'brin_test_serial_idx')
EXCEPT
SELECT * FROM brin_page_items(get_raw_page('brin_test_parallel_idx', 2), 'brin_test_parallel_idx');

DROP INDEX brin_test_parallel_idx;

-- force parallel build, but don't allow starting parallel workers to force
--
-- 强制并行构建，但不允许启动并行工作人员强制
-- fallback to serial build, and repeat the checks
--
-- 回退到串行构建，并重复检查

SET max_parallel_workers = 0;
CREATE INDEX brin_test_parallel_idx ON brin_parallel_test
 USING brin (a int4_minmax_ops, a int4_bloom_ops, b, c int8_minmax_multi_ops)
  WITH (pages_per_range=7)
 WHERE NOT (a BETWEEN 1000 and 1500);

SELECT relname, relpages
  FROM pg_class
 WHERE relname IN ('brin_test_serial_idx', 'brin_test_parallel_idx')
 ORDER BY relname;

SELECT * FROM brin_page_items(get_raw_page('brin_test_parallel_idx', 2), 'brin_test_parallel_idx')
EXCEPT
SELECT * FROM brin_page_items(get_raw_page('brin_test_serial_idx', 2), 'brin_test_serial_idx');

SELECT * FROM brin_page_items(get_raw_page('brin_test_serial_idx', 2), 'brin_test_serial_idx')
EXCEPT
SELECT * FROM brin_page_items(get_raw_page('brin_test_parallel_idx', 2), 'brin_test_parallel_idx');

DROP TABLE brin_parallel_test;
RESET min_parallel_table_scan_size;
RESET max_parallel_maintenance_workers;
RESET maintenance_work_mem;

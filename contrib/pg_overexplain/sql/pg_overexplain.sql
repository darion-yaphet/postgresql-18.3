-- These tests display internal details that would not be stable under
--
-- 这些测试显示的内部细节在以下情况下不稳定
-- debug_parallel_query, so make sure that option is disabled.
--
-- debug_parallel_query，因此请确保禁用该选项。
SET debug_parallel_query = off;

-- Make sure that we don't print any JIT-related information, as that
--
-- 确保我们不打印任何与 JIT 相关的信息，因为
-- would also make results unstable.
--
-- 也会使结果不稳定。
SET jit = off;

-- These options do not exist, so these queries should all fail.
--
-- 这些选项不存在，因此这些查询都应该失败。
EXPLAIN (DEBUFF) SELECT 1;
EXPLAIN (DEBUG) SELECT 1;
EXPLAIN (RANGE_TABLE) SELECT 1;

-- Load the module that creates the options.
--
-- 加载创建选项的模块。
LOAD 'pg_overexplain';

-- The first option still does not exist, but the others do.
--
-- 第一个选项仍然不存在，但其他选项存在。
EXPLAIN (DEBUFF) SELECT 1;
EXPLAIN (DEBUG) SELECT 1;
EXPLAIN (RANGE_TABLE) SELECT 1;

-- Create a partitioned table.
--
-- 创建分区表。
CREATE TABLE vegetables (id serial, name text, genus text)
PARTITION BY LIST (genus);
CREATE TABLE daucus PARTITION OF vegetables FOR VALUES IN ('daucus');
CREATE TABLE brassica PARTITION OF vegetables FOR VALUES IN ('brassica');
INSERT INTO vegetables (name, genus)
	VALUES ('carrot', 'daucus'), ('bok choy', 'brassica'),
		   ('brocooli', 'brassica'), ('cauliflower', 'brassica'),
		   ('cabbage', 'brassica'), ('kohlrabi', 'brassica'),
		   ('rutabaga', 'brassica'), ('turnip', 'brassica');
VACUUM ANALYZE vegetables;

-- We filter relation OIDs out of the test output in order to avoid
--
-- 我们从测试输出中过滤掉关系 OID，以避免
-- test instability. This is currently only needed for EXPLAIN (DEBUG), not
--
-- 测试不稳定。目前仅需要 EXPLAIN (DEBUG)，不需要
-- EXPLAIN (RANGE_TABLE). Also suppress actual row counts, which are not
--
-- 解释（范围_表）。还抑制实际行数，这不是
-- stable (e.g. 1/8 is 0.12 on some buildfarm machines and 0.13 on others).
--
-- 稳定（例如，1/8 在某些 buildfarm 机器上为 0.12，在其他机器上为 0.13）。
CREATE FUNCTION explain_filter(text) RETURNS SETOF text
LANGUAGE plpgsql AS
$$
DECLARE
    ln text;
BEGIN
    FOR ln IN EXECUTE $1
	LOOP
		ln := regexp_replace(ln, 'Relation OIDs:( \m\d+\M)+',
								 'Relation OIDs: NNN...', 'g');
		ln := regexp_replace(ln, '<Relation-OIDs>( ?\m\d+\M)+</Relation-OIDs>',
								 '<Relation-OIDs>NNN...</Relation-OIDs>', 'g');
		ln := regexp_replace(ln, 'actual rows=\d+\.\d+',
								 'actual rows=N.NN', 'g');
		RETURN NEXT ln;
	END LOOP;
END;
$$;

-- Test with both options together and an aggregate.
--
-- 一起测试这两个选项并进行聚合。
SELECT explain_filter($$
EXPLAIN (DEBUG, RANGE_TABLE, COSTS OFF)
SELECT genus, array_agg(name ORDER BY name) FROM vegetables GROUP BY genus
$$);

-- Test a different output format.
--
-- 测试不同的输出格式。
SELECT explain_filter($$
EXPLAIN (DEBUG, RANGE_TABLE, FORMAT XML, COSTS OFF)
SELECT genus, array_agg(name ORDER BY name) FROM vegetables GROUP BY genus
$$);

-- Test just the DEBUG option. Verify that it shows information about
--
-- 仅测试 DEBUG 选项。验证它是否显示有关的信息
-- disabled nodes, parallel safety, and the parallelModeNeeded flag.
--
-- 禁用节点、并行安全性和parallelModeNeeded 标志。
SET enable_seqscan = false;
SET debug_parallel_query = true;
SELECT explain_filter($$
EXPLAIN (DEBUG, COSTS OFF)
SELECT genus, array_agg(name ORDER BY name) FROM vegetables GROUP BY genus
$$);
SET debug_parallel_query = false;
RESET enable_seqscan;

-- Test the DEBUG option with a non-SELECT query, and also verify that the
--
-- 使用非 SELECT 查询测试 DEBUG 选项，并验证
-- hasReturning flag is shown.
--
-- 显示 hasReturning 标志。
SELECT explain_filter($$
EXPLAIN (DEBUG, COSTS OFF)
INSERT INTO vegetables (name, genus)
	VALUES ('Brotero''s carrot', 'brassica') RETURNING id
$$);

-- Create an index, and then attempt to force a nested loop with inner index
--
-- 创建索引，然后尝试强制使用内部索引进行嵌套循环
-- scan so that we can see parameter-related information. Also, let's try
--
-- 扫描以便我们可以看到参数相关的信息。另外，我们来尝试一下
-- actually running the query, but try to suppress potentially variable output.
--
-- 实际运行查询，但尝试抑制潜在的可变输出。
CREATE INDEX ON vegetables (id);
ANALYZE vegetables;
SET enable_hashjoin = false;
SET enable_material = false;
SET enable_mergejoin = false;
SET enable_seqscan = false;
SELECT explain_filter($$
EXPLAIN (BUFFERS OFF, COSTS OFF, SUMMARY OFF, TIMING OFF, ANALYZE, DEBUG)
SELECT * FROM vegetables v1, vegetables v2 WHERE v1.id = v2.id;
$$);
RESET enable_hashjoin;
RESET enable_material;
RESET enable_mergejoin;
RESET enable_seqscan;

-- Test the RANGE_TABLE option with a case that allows partition pruning.
--
-- 使用允许分区修剪的情况测试 RANGE_TABLE 选项。
EXPLAIN (RANGE_TABLE, COSTS OFF)
SELECT * FROM vegetables WHERE genus = 'daucus';

-- Also test a case that involves a write.
--
-- 还测试涉及写入的情况。
EXPLAIN (RANGE_TABLE, COSTS OFF)
INSERT INTO vegetables (name, genus) VALUES ('broccoflower', 'brassica');

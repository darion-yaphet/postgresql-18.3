CREATE TABLE test1 (a int8, b int4range);
INSERT INTO test1 VALUES (72057594037927937, '[0,1)');
CREATE INDEX test1_a_idx ON test1 USING btree (a);

\x

SELECT * FROM bt_metap('test1_a_idx');

SELECT * FROM bt_page_stats('test1_a_idx', -1);
SELECT * FROM bt_page_stats('test1_a_idx', 0);
SELECT * FROM bt_page_stats('test1_a_idx', 1);
SELECT * FROM bt_page_stats('test1_a_idx', 2);

-- bt_multi_page_stats() function returns a set of records of page statistics.
--
-- bt_multi_page_stats() 函数返回一组页面统计记录。
CREATE TABLE test2 AS (SELECT generate_series(1, 1000)::int8 AS col1);
CREATE INDEX test2_col1_idx ON test2(col1);
SELECT * FROM bt_multi_page_stats('test2_col1_idx', 0, 1);
SELECT * FROM bt_multi_page_stats('test2_col1_idx', 1, -1);
SELECT * FROM bt_multi_page_stats('test2_col1_idx', 1, 0);
SELECT * FROM bt_multi_page_stats('test2_col1_idx', 1, 2);
SELECT * FROM bt_multi_page_stats('test2_col1_idx', 3, 2);
SELECT * FROM bt_multi_page_stats('test2_col1_idx', 7, 2);
DROP TABLE test2;

SELECT * FROM bt_page_items('test1_a_idx', -1);
SELECT * FROM bt_page_items('test1_a_idx', 0);
SELECT * FROM bt_page_items('test1_a_idx', 1);
SELECT * FROM bt_page_items('test1_a_idx', 2);

SELECT * FROM bt_page_items(get_raw_page('test1_a_idx', -1));
SELECT * FROM bt_page_items(get_raw_page('test1_a_idx', 0));
SELECT * FROM bt_page_items(get_raw_page('test1_a_idx', 1));
SELECT * FROM bt_page_items(get_raw_page('test1_a_idx', 2));

-- Failure when using a non-btree index.
--
-- 使用非 btree 索引时失败。
CREATE INDEX test1_a_hash ON test1 USING hash(a);
SELECT bt_metap('test1_a_hash');
SELECT bt_page_stats('test1_a_hash', 0);
SELECT bt_page_items('test1_a_hash', 0);
SELECT bt_page_items(get_raw_page('test1_a_hash', 0));
CREATE INDEX test1_b_gist ON test1 USING gist(b);
-- Special area of GiST is the same as btree, this complains about inconsistent
--
-- GiST 的特殊区域与 btree 相同，这会抱怨不一致
-- leaf data on the page.
--
-- 页面上的叶子数据。
SELECT bt_page_items(get_raw_page('test1_b_gist', 0));

-- Several failure modes.
--
-- 几种故障模式。
-- Suppress the DETAIL message, to allow the tests to work across various
--
-- 抑制 DETAIL 消息，以允许测试在不同的环境中工作
-- page sizes and architectures.
--
-- 页面大小和体系结构。
\set VERBOSITY terse
-- invalid page size
--
-- 页面大小无效
SELECT bt_page_items('aaa'::bytea);
-- invalid special area size
--
-- 无效的特殊区域大小
CREATE INDEX test1_a_brin ON test1 USING brin(a);
SELECT bt_page_items(get_raw_page('test1', 0));
SELECT bt_page_items(get_raw_page('test1_a_brin', 0));
\set VERBOSITY default

-- Tests with all-zero pages.
--
-- 使用全零页面进行测试。
SHOW block_size \gset
SELECT bt_page_items(decode(repeat('00', :block_size), 'hex'));

DROP TABLE test1;

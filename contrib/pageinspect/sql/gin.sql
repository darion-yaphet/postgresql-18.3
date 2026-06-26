CREATE TABLE test1 (x int, y int[]);
INSERT INTO test1 VALUES (1, ARRAY[11, 111]);
CREATE INDEX test1_y_idx ON test1 USING gin (y) WITH (fastupdate = off);

\x

SELECT * FROM gin_metapage_info(get_raw_page('test1_y_idx', 0));
SELECT * FROM gin_metapage_info(get_raw_page('test1_y_idx', 1));

SELECT * FROM gin_page_opaque_info(get_raw_page('test1_y_idx', 1));

SELECT * FROM gin_leafpage_items(get_raw_page('test1_y_idx', 1));

INSERT INTO test1 SELECT x, ARRAY[1,10] FROM generate_series(2,10000) x;

SELECT COUNT(*) > 0
FROM gin_leafpage_items(get_raw_page('test1_y_idx',
                        (pg_relation_size('test1_y_idx') /
                         current_setting('block_size')::bigint)::int - 1));

-- Failure with various modes.
--
-- 各种模式失败。
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
SELECT gin_leafpage_items('aaa'::bytea);
SELECT gin_metapage_info('bbb'::bytea);
SELECT gin_page_opaque_info('ccc'::bytea);
-- invalid special area size
--
-- 无效的特殊区域大小
SELECT * FROM gin_metapage_info(get_raw_page('test1', 0));
SELECT * FROM gin_page_opaque_info(get_raw_page('test1', 0));
SELECT * FROM gin_leafpage_items(get_raw_page('test1', 0));
\set VERBOSITY default

-- Tests with all-zero pages.
--
-- 使用全零页面进行测试。
SHOW block_size \gset
SELECT gin_leafpage_items(decode(repeat('00', :block_size), 'hex'));
SELECT gin_metapage_info(decode(repeat('00', :block_size), 'hex'));
SELECT gin_page_opaque_info(decode(repeat('00', :block_size), 'hex'));

DROP TABLE test1;

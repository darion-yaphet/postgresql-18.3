-- The gist_page_opaque_info() function prints the page's LSN.
--
-- gist_page_opaque_info() 函数打印页面的 LSN。
-- Use an unlogged index, so that the LSN is predictable.
--
-- 使用未记录的索引，以便 LSN 是可预测的。
CREATE UNLOGGED TABLE test_gist AS SELECT point(i,i) p, i::text t FROM
    generate_series(1,1000) i;
CREATE INDEX test_gist_idx ON test_gist USING gist (p);

-- Page 0 is the root, the rest are leaf pages
--
-- 第0页为根页，其余为叶页
SELECT * FROM gist_page_opaque_info(get_raw_page('test_gist_idx', 0));
SELECT * FROM gist_page_opaque_info(get_raw_page('test_gist_idx', 1));
SELECT * FROM gist_page_opaque_info(get_raw_page('test_gist_idx', 2));

SELECT * FROM gist_page_items(get_raw_page('test_gist_idx', 0), 'test_gist_idx');
SELECT * FROM gist_page_items(get_raw_page('test_gist_idx', 1), 'test_gist_idx') LIMIT 5;

-- gist_page_items_bytea prints the raw key data as a bytea. The output of that is
--
-- gist_page_items_bytea 将原始关键数据打印为 bytea。其输出是
-- platform-dependent (endianness), so omit the actual key data from the output.
--
-- 平台相关（字节序），因此从输出中省略实际的关键数据。
SELECT itemoffset, ctid, itemlen FROM gist_page_items_bytea(get_raw_page('test_gist_idx', 0));

-- Suppress the DETAIL message, to allow the tests to work across various
--
-- 抑制 DETAIL 消息，以允许测试在不同的环境中工作
-- page sizes and architectures.
--
-- 页面大小和体系结构。
\set VERBOSITY terse

-- Failures with non-GiST index.
--
-- 非 GiST 索引失败。
CREATE INDEX test_gist_btree on test_gist(t);
SELECT gist_page_items(get_raw_page('test_gist_btree', 0), 'test_gist_btree');
SELECT gist_page_items(get_raw_page('test_gist_btree', 0), 'test_gist_idx');

-- Failure with various modes.
--
-- 各种模式失败。
-- invalid page size
--
-- 页面大小无效
SELECT gist_page_items_bytea('aaa'::bytea);
SELECT gist_page_items('aaa'::bytea, 'test_gist_idx'::regclass);
SELECT gist_page_opaque_info('aaa'::bytea);
-- invalid special area size
--
-- 无效的特殊区域大小
SELECT * FROM gist_page_opaque_info(get_raw_page('test_gist', 0));
SELECT gist_page_items_bytea(get_raw_page('test_gist', 0));
SELECT gist_page_items_bytea(get_raw_page('test_gist_btree', 0));
\set VERBOSITY default

-- Tests with all-zero pages.
--
-- 使用全零页面进行测试。
SHOW block_size \gset
SELECT gist_page_items_bytea(decode(repeat('00', :block_size), 'hex'));
SELECT gist_page_items(decode(repeat('00', :block_size), 'hex'), 'test_gist_idx'::regclass);
SELECT gist_page_opaque_info(decode(repeat('00', :block_size), 'hex'));

-- Test gist_page_items with included columns.
--
-- 使用包含的列测试 gist_page_items。
-- Non-leaf pages contain only the key attributes, and leaf pages contain
--
-- 非叶子页面只包含关键属性，叶子页面包含
-- the included attributes.
--
-- 包含的属性。
ALTER TABLE test_gist ADD COLUMN i int DEFAULT NULL;
CREATE INDEX test_gist_idx_inc ON test_gist
  USING gist (p) INCLUDE (t, i);
-- Mask the value of the key attribute to avoid alignment issues.
--
-- 屏蔽键属性的值以避免对齐问题。
SELECT regexp_replace(keys, '\(p\)=\("(.*?)"\)', '(p)=("<val>")') AS keys_nonleaf_1
  FROM gist_page_items(get_raw_page('test_gist_idx_inc', 0), 'test_gist_idx_inc')
  WHERE itemoffset = 1;
SELECT keys AS keys_leaf_1
  FROM gist_page_items(get_raw_page('test_gist_idx_inc', 1), 'test_gist_idx_inc')
  WHERE itemoffset = 1;

DROP TABLE test_gist;

CREATE EXTENSION pageinspect;

-- Use a temp table so that effects of VACUUM are predictable
--
-- 使用临时表以便可以预测 VACUUM 的影响
CREATE TEMP TABLE test1 (a int, b int);
INSERT INTO test1 VALUES (16777217, 131584);

VACUUM (DISABLE_PAGE_SKIPPING) test1;  -- set up FSM

-- The page contents can vary, so just test that it can be read
--
-- 页面内容可能会有所不同，因此只需测试它是否可以读取
-- successfully, but don't keep the output.
--
-- 成功，但不保留输出。

SELECT octet_length(get_raw_page('test1', 'main', 0)) AS main_0;
SELECT octet_length(get_raw_page('test1', 'main', 1)) AS main_1;

SELECT octet_length(get_raw_page('test1', 'fsm', 0)) AS fsm_0;
SELECT octet_length(get_raw_page('test1', 'fsm', 1)) AS fsm_1;

SELECT octet_length(get_raw_page('test1', 'vm', 0)) AS vm_0;
SELECT octet_length(get_raw_page('test1', 'vm', 1)) AS vm_1;

SELECT octet_length(get_raw_page('test1', 'main', -1));
SELECT octet_length(get_raw_page('xxx', 'main', 0));
SELECT octet_length(get_raw_page('test1', 'xxx', 0));

SELECT get_raw_page('test1', 0) = get_raw_page('test1', 'main', 0);

SELECT pagesize, version FROM page_header(get_raw_page('test1', 0));

SELECT page_checksum(get_raw_page('test1', 0), 0) IS NOT NULL AS silly_checksum_test;
SELECT page_checksum(get_raw_page('test1', 0), -1);

SELECT tuple_data_split('test1'::regclass, t_data, t_infomask, t_infomask2, t_bits)
    FROM heap_page_items(get_raw_page('test1', 0));

SELECT * FROM fsm_page_contents(get_raw_page('test1', 'fsm', 0));

-- If we freeze the only tuple on test1, the infomask should
--
-- 如果我们冻结 test1 上唯一的元组，则 infomask 应该
-- always be the same in all test runs.
--
-- 在所有测试运行中始终相同。
VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) test1;

SELECT t_infomask, t_infomask2, raw_flags, combined_flags
FROM heap_page_items(get_raw_page('test1', 0)),
     LATERAL heap_tuple_infomask_flags(t_infomask, t_infomask2);

-- tests for decoding of combined flags
--
-- 测试组合标志的解码
-- HEAP_XMAX_SHR_LOCK = (HEAP_XMAX_EXCL_LOCK | HEAP_XMAX_KEYSHR_LOCK)
--
-- HEAP_XMAX_SHR_LOCK = (HEAP_XMAX_EXCL_LOCK | HEAP_XMAX_KEYSHR_LOCK)
SELECT * FROM heap_tuple_infomask_flags(x'0050'::int, 0);
-- HEAP_XMIN_FROZEN = (HEAP_XMIN_COMMITTED | HEAP_XMIN_INVALID)
--
-- HEAP_XMIN_FROZEN = (HEAP_XMIN_COMMITTED | HEAP_XMIN_INVALID)
SELECT * FROM heap_tuple_infomask_flags(x'0300'::int, 0);
-- HEAP_MOVED = (HEAP_MOVED_IN | HEAP_MOVED_OFF)
--
-- HEAP_MOVED = (HEAP_MOVED_IN | HEAP_MOVED_OFF)
SELECT * FROM heap_tuple_infomask_flags(x'C000'::int, 0);
SELECT * FROM heap_tuple_infomask_flags(x'C000'::int, 0);

-- test all flags of t_infomask and t_infomask2
--
-- 测试 t_infomask 和 t_infomask2 的所有标志
SELECT unnest(raw_flags)
  FROM heap_tuple_infomask_flags(x'FFFF'::int, x'FFFF'::int) ORDER BY 1;
SELECT unnest(combined_flags)
  FROM heap_tuple_infomask_flags(x'FFFF'::int, x'FFFF'::int) ORDER BY 1;

-- no flags at all
--
-- 根本没有旗帜
SELECT * FROM heap_tuple_infomask_flags(0, 0);
-- no combined flags
--
-- 没有组合标志
SELECT * FROM heap_tuple_infomask_flags(x'0010'::int, 0);

DROP TABLE test1;

-- check that using any of these functions with a partitioned table or index
--
-- 检查是否将这些函数中的任何一个与分区表或索引一起使用
-- would fail
--
-- 会失败
create table test_partitioned (a int) partition by range (a);
create index test_partitioned_index on test_partitioned (a);
select get_raw_page('test_partitioned', 0); -- error about partitioned table
select get_raw_page('test_partitioned_index', 0); -- error about partitioned index

-- a regular table which is a member of a partition set should work though
--
-- 作为分区集成员的常规表应该可以工作
create table test_part1 partition of test_partitioned for values from ( 1 ) to (100);
select get_raw_page('test_part1', 0); -- get farther and error about empty table
drop table test_partitioned;

-- check null bitmap alignment for table whose number of attributes is multiple of 8
--
-- 检查属性数量为 8 倍数的表的空位图对齐情况
create table test8 (f1 int, f2 int, f3 int, f4 int, f5 int, f6 int, f7 int, f8 int);
insert into test8(f1, f8) values (x'7f00007f'::int, 0);
select t_bits, t_data from heap_page_items(get_raw_page('test8', 0));
select tuple_data_split('test8'::regclass, t_data, t_infomask, t_infomask2, t_bits)
    from heap_page_items(get_raw_page('test8', 0));
drop table test8;

-- check storage of generated columns
--
-- 检查生成列的存储
-- stored
create table test9s (a int not null, b int generated always as (a * 2) stored);
insert into test9s values (131584);
select raw_flags, t_bits, t_data
    from heap_page_items(get_raw_page('test9s', 0)), lateral heap_tuple_infomask_flags(t_infomask, t_infomask2);
select tuple_data_split('test9s'::regclass, t_data, t_infomask, t_infomask2, t_bits)
    from heap_page_items(get_raw_page('test9s', 0));
drop table test9s;

-- virtual
create table test9v (a int not null, b int generated always as (a * 2) virtual);
insert into test9v values (131584);
select raw_flags, t_bits, t_data
    from heap_page_items(get_raw_page('test9v', 0)), lateral heap_tuple_infomask_flags(t_infomask, t_infomask2);
select tuple_data_split('test9v'::regclass, t_data, t_infomask, t_infomask2, t_bits)
    from heap_page_items(get_raw_page('test9v', 0));
drop table test9v;

-- Failure with incorrect page size
--
-- 页面大小不正确导致失败
-- Suppress the DETAIL message, to allow the tests to work across various
--
-- 抑制 DETAIL 消息，以允许测试在不同的环境中工作
-- page sizes.
--
-- 页面大小。
\set VERBOSITY terse
SELECT fsm_page_contents('aaa'::bytea);
SELECT page_checksum('bbb'::bytea, 0);
SELECT page_header('ccc'::bytea);
\set VERBOSITY default

-- Tests with all-zero pages.
--
-- 使用全零页面进行测试。
SHOW block_size \gset
SELECT fsm_page_contents(decode(repeat('00', :block_size), 'hex'));
SELECT page_header(decode(repeat('00', :block_size), 'hex'));
SELECT page_checksum(decode(repeat('00', :block_size), 'hex'), 1);

-- tests for sequences
--
-- 序列测试
create sequence test_sequence start 72057594037927937;
select tuple_data_split('test_sequence'::regclass, t_data, t_infomask, t_infomask2, t_bits)
  from heap_page_items(get_raw_page('test_sequence', 0));
drop sequence test_sequence;

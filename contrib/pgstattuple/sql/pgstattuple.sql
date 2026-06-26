CREATE EXTENSION pgstattuple;

--
-- It's difficult to come up with platform-independent test cases for
--
-- 很难提出独立于平台的测试用例
-- the pgstattuple functions, but the results for empty tables and
--
-- pgstattuple 函数，但空表的结果和
-- indexes should be that.
--
-- 索引应该是这样的。
--

create table test (a int primary key, b int[]);

select * from pgstattuple('test');
select * from pgstattuple('test'::text);
select * from pgstattuple('test'::name);
select * from pgstattuple('test'::regclass);
select pgstattuple(oid) from pg_class where relname = 'test';
select pgstattuple(relname) from pg_class where relname = 'test';

select version, tree_level,
    index_size / current_setting('block_size')::int as index_size,
    root_block_no, internal_pages, leaf_pages, empty_pages, deleted_pages,
    avg_leaf_density, leaf_fragmentation
    from pgstatindex('test_pkey');
select version, tree_level,
    index_size / current_setting('block_size')::int as index_size,
    root_block_no, internal_pages, leaf_pages, empty_pages, deleted_pages,
    avg_leaf_density, leaf_fragmentation
    from pgstatindex('test_pkey'::text);
select version, tree_level,
    index_size / current_setting('block_size')::int as index_size,
    root_block_no, internal_pages, leaf_pages, empty_pages, deleted_pages,
    avg_leaf_density, leaf_fragmentation
    from pgstatindex('test_pkey'::name);
select version, tree_level,
    index_size / current_setting('block_size')::int as index_size,
    root_block_no, internal_pages, leaf_pages, empty_pages, deleted_pages,
    avg_leaf_density, leaf_fragmentation
    from pgstatindex('test_pkey'::regclass);

select pg_relpages('test');
select pg_relpages('test_pkey');
select pg_relpages('test_pkey'::text);
select pg_relpages('test_pkey'::name);
select pg_relpages('test_pkey'::regclass);
select pg_relpages(oid) from pg_class where relname = 'test_pkey';
select pg_relpages(relname) from pg_class where relname = 'test_pkey';

create index test_ginidx on test using gin (b);

select * from pgstatginindex('test_ginidx');

create index test_hashidx on test using hash (b);

select * from pgstathashindex('test_hashidx');

-- these should error with the wrong type
--
-- 这些应该是错误的类型
select pgstatginindex('test_pkey');
select pgstathashindex('test_pkey');

select pgstatindex('test_ginidx');
select pgstathashindex('test_ginidx');

select pgstatindex('test_hashidx');
select pgstatginindex('test_hashidx');

-- check that using any of these functions with unsupported relations will fail
--
-- 检查使用任何具有不受支持的关系的函数都会失败
create table test_partitioned (a int) partition by range (a);
create index test_partitioned_index on test_partitioned(a);
create index test_partitioned_hash_index on test_partitioned using hash(a);
-- these should all fail
--
-- 这些都应该失败
select pgstattuple('test_partitioned');
select pgstattuple('test_partitioned_index');
select pgstattuple_approx('test_partitioned');
select pg_relpages('test_partitioned');
select pgstatindex('test_partitioned');
select pgstatginindex('test_partitioned');
select pgstathashindex('test_partitioned');
select pgstathashindex('test_partitioned_hash_index');

create view test_view as select 1;
-- these should all fail
--
-- 这些都应该失败
select pgstattuple('test_view');
select pgstattuple_approx('test_view');
select pg_relpages('test_view');
select pgstatindex('test_view');
select pgstatginindex('test_view');
select pgstathashindex('test_view');

create foreign data wrapper dummy;
create server dummy_server foreign data wrapper dummy;
create foreign table test_foreign_table () server dummy_server;
-- these should all fail
--
-- 这些都应该失败
select pgstattuple('test_foreign_table');
select pgstattuple_approx('test_foreign_table');
select pg_relpages('test_foreign_table');
select pgstatindex('test_foreign_table');
select pgstatginindex('test_foreign_table');
select pgstathashindex('test_foreign_table');

-- a partition of a partitioned table should work though
--
-- 分区表的分区应该可以工作
create table test_partition partition of test_partitioned for values from (1) to (100);
select pgstattuple('test_partition');
select pgstattuple_approx('test_partition');
select pg_relpages('test_partition');

-- toast tables should work
--
-- 吐司桌应该可以用
select pgstattuple((select reltoastrelid from pg_class where relname = 'test'));
select pgstattuple_approx((select reltoastrelid from pg_class where relname = 'test'));
select pg_relpages((select reltoastrelid from pg_class where relname = 'test'));

-- not for the index calls though, of course
--
-- 当然，不适用于索引调用
select pgstatindex('test_partition');
select pgstatginindex('test_partition');
select pgstathashindex('test_partition');

-- an actual index of a partitioned table should work though
--
-- 分区表的实际索引应该可以工作
create index test_partition_idx on test_partition(a);
create index test_partition_hash_idx on test_partition using hash (a);
-- these should work
--
-- 这些应该有效
select pgstatindex('test_partition_idx');
select pgstathashindex('test_partition_hash_idx');

-- these should work for sequences
--
-- 这些应该适用于序列
create sequence test_sequence;
select count(*) from pgstattuple('test_sequence');
select pg_relpages('test_sequence');

-- these should fail for sequences
--
-- 这些对于序列应该失败
select pgstatindex('test_sequence');
select pgstatginindex('test_sequence');
select pgstathashindex('test_sequence');
select pgstattuple_approx('test_sequence');

drop sequence test_sequence;
drop table test_partitioned;
drop view test_view;
drop foreign table test_foreign_table;
drop server dummy_server;
drop foreign data wrapper dummy;

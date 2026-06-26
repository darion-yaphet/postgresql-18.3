create extension pg_surgery;

-- create a normal heap table and insert some rows.
--
-- 创建一个普通的堆表并插入一些行。
-- use a temp table so that vacuum behavior doesn't depend on global xmin
--
-- 使用临时表，以便真空行为不依赖于全局 xmin
create temp table htab (a int);
insert into htab values (100), (200), (300), (400), (500);

-- test empty TID array
--
-- 测试空 TID 数组
select heap_force_freeze('htab'::regclass, ARRAY[]::tid[]);

-- nothing should be frozen yet
--
-- 还没有什么东西应该被冻结
select * from htab where xmin = 2;

-- freeze forcibly
--
-- 强行冻结
select heap_force_freeze('htab'::regclass, ARRAY['(0, 4)']::tid[]);

-- now we should have one frozen tuple
--
-- 现在我们应该有一个冻结的元组
select ctid, xmax from htab where xmin = 2;

-- kill forcibly
--
-- 强行杀人
select heap_force_kill('htab'::regclass, ARRAY['(0, 4)']::tid[]);

-- should be gone now
--
-- 现在应该消失了
select * from htab where ctid = '(0, 4)';

-- should now be skipped because it's already dead
--
-- 现在应该被跳过，因为它已经死了
select heap_force_kill('htab'::regclass, ARRAY['(0, 4)']::tid[]);
select heap_force_freeze('htab'::regclass, ARRAY['(0, 4)']::tid[]);

-- freeze two TIDs at once while skipping an out-of-range block number
--
-- 一次冻结两个 TID，同时跳过超出范围的块号
select heap_force_freeze('htab'::regclass,
						 ARRAY['(0, 1)', '(0, 3)', '(1, 1)']::tid[]);

-- we should now have two frozen tuples
--
-- 我们现在应该有两个冻结的元组
select ctid, xmax from htab where xmin = 2;

-- out-of-range TIDs should be skipped
--
-- 应跳过超出范围的 TID
select heap_force_freeze('htab'::regclass, ARRAY['(0, 0)', '(0, 6)']::tid[]);

-- set up a new table with a redirected line pointer
--
-- 使用重定向的行指针设置一个新表
-- use a temp table so that vacuum behavior doesn't depend on global xmin
--
-- 使用临时表，以便真空行为不依赖于全局 xmin
create temp table htab2(a int);
insert into htab2 values (100);
update htab2 set a = 200;
vacuum htab2;

-- redirected TIDs should be skipped
--
-- 应跳过重定向的 TID
select heap_force_kill('htab2'::regclass, ARRAY['(0, 1)']::tid[]);

-- now create an unused line pointer
--
-- 现在创建一个未使用的行指针
select ctid from htab2;
update htab2 set a = 300;
select ctid from htab2;
vacuum freeze htab2;

-- unused TIDs should be skipped
--
-- 应跳过未使用的 TID
select heap_force_kill('htab2'::regclass, ARRAY['(0, 2)']::tid[]);

-- multidimensional TID array should be rejected
--
-- 应拒绝多维 TID 数组
select heap_force_kill('htab2'::regclass, ARRAY[['(0, 2)']]::tid[]);

-- TID array with nulls should be rejected
--
-- 含有空值的 TID 数组应该被拒绝
select heap_force_kill('htab2'::regclass, ARRAY[NULL]::tid[]);

-- but we should be able to kill the one tuple we have
--
-- 但我们应该能够杀死我们拥有的一个元组
select heap_force_kill('htab2'::regclass, ARRAY['(0, 3)']::tid[]);

-- materialized view.
--
-- 物化视图。
-- note that we don't commit the transaction, so autovacuum can't interfere.
--
-- 请注意，我们不提交事务，因此 autovacuum 不会干扰。
begin;
create materialized view mvw as select a from generate_series(1, 3) a;

select * from mvw where xmin = 2;
select heap_force_freeze('mvw'::regclass, ARRAY['(0, 3)']::tid[]);
select * from mvw where xmin = 2;

select heap_force_kill('mvw'::regclass, ARRAY['(0, 3)']::tid[]);
select * from mvw where ctid = '(0, 3)';
rollback;

-- check that it fails on an unsupported relkind
--
-- 检查它在不受支持的relkind 上是否失败
create view vw as select 1;
select heap_force_kill('vw'::regclass, ARRAY['(0, 1)']::tid[]);
select heap_force_freeze('vw'::regclass, ARRAY['(0, 1)']::tid[]);

-- cleanup.
drop view vw;
drop extension pg_surgery;

-- Make sure we can create an exclusion constraint
--
-- 确保我们可以创建排除约束
-- across a partitioned table.
--
-- 跨分区表。
-- That code looks at strategy numbers that can differ in regular gist vs btree_gist,
--
-- 该代码着眼于常规 gist 与 btree_gist 中可能不同的策略编号，
-- so we want to make sure it works here too.
--
-- 所以我们想确保它在这里也能发挥作用。
create table parttmp (
  id int,
  valid_at daterange,
  exclude using gist (id with =, valid_at with &&)
) partition by range (id);

create table parttmp_1_to_10 partition of parttmp for values from (1) to (10);
create table parttmp_11_to_20 partition of parttmp for values from (11) to (20);

insert into parttmp (id, valid_at) values
  (1, '[2000-01-01, 2000-02-01)'),
  (1, '[2000-02-01, 2000-03-01)'),
  (2, '[2000-01-01, 2000-02-01)'),
  (11, '[2000-01-01, 2000-02-01)'),
  (11, '[2000-02-01, 2000-03-01)'),
  (12, '[2000-01-01, 2000-02-01)');

select * from parttmp order by id, valid_at;
select * from parttmp_1_to_10 order by id, valid_at;
select * from parttmp_11_to_20 order by id, valid_at;

update parttmp set valid_at = valid_at * '[2000-01-15,2000-02-15)' where id = 1;

select * from parttmp order by id, valid_at;
select * from parttmp_1_to_10 order by id, valid_at;
select * from parttmp_11_to_20 order by id, valid_at;

-- make sure the excluson constraint excludes:
--
-- 确保排除约束排除：
insert into parttmp (id, valid_at) values
  (2, '[2000-01-15, 2000-02-01)');

drop table parttmp;

-- should fail with a good error message:
--
-- 应该会失败并显示一条良好的错误消息：
create table parttmp (id int, valid_at daterange, exclude using gist (id with <>, valid_at with &&)) partition by range (id);

--
-- Test that partitioned-index operations cope with objects that are
--
-- 测试分区索引操作是否处理以下对象
-- not in the secure search path.  (This has little to do with seg,
--
-- 不在安全搜索路径中。  （这个和seg关系不大，
-- but we need an opclass that isn't in pg_catalog, and the base system
--
-- 但我们需要一个不在 pg_catalog 中的 opclass，以及基本系统
-- has no such opclass.)  Note that we need to test propagation of the
--
-- 没有这样的操作类。）请注意，我们需要测试
-- partitioned index's properties both to partitions that pre-date it
--
-- 分区索引的属性既适用于它之前的分区
-- and to partitions created later.
--
-- 以及稍后创建的分区。
--

create function mydouble(int) returns int strict immutable parallel safe
begin atomic select $1 * 2; end;

create collation mycollation from "POSIX";

create table pt (category int, sdata seg, tdata text)
  partition by list (category);

-- pre-existing partition
--
-- 预先存在的分区
create table pt12 partition of pt for values in (1,2);

insert into pt values(1, '0 .. 1'::seg, 'zed');

-- expression references object in public schema
--
-- 表达式引用公共模式中的对象
create index pti1 on pt ((mydouble(category) + 1));
-- opclass in public schema
--
-- 公共模式中的 opclass
create index pti2 on pt (sdata seg_ops);
-- collation in public schema
--
-- 公共模式中的排序规则
create index pti3 on pt (tdata collate mycollation);

-- new partition
--
-- 新分区
create table pt34 partition of pt for values in (3,4);

insert into pt values(4, '-1 .. 1'::seg, 'foo');

\d+ pt
\d+ pt12

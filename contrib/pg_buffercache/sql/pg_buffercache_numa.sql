SELECT NOT(pg_numa_available()) AS skip_test \gset
\if :skip_test
\quit
\endif

-- We expect at least one entry for each buffer
--
-- 我们期望每个缓冲区至少有一个条目
select count(*) >= (select setting::bigint
                    from pg_settings
                    where name = 'shared_buffers')
from pg_buffercache_numa;

-- Check that the functions / views can't be accessed by default. To avoid
--
-- 检查默认情况下无法访问功能/视图。为了避免
-- having to create a dedicated user, use the pg_database_owner pseudo-role.
--
-- 必须创建专用用户，请使用 pg_database_owner 伪角色。
SET ROLE pg_database_owner;
SELECT count(*) > 0 FROM pg_buffercache_numa;
RESET role;

-- Check that pg_monitor is allowed to query view / function
--
-- 检查是否允许 pg_monitor 查询视图/函数
SET ROLE pg_monitor;
SELECT count(*) > 0 FROM pg_buffercache_numa;
RESET role;

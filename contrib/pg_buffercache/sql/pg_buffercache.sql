CREATE EXTENSION pg_buffercache;

select count(*) = (select setting::bigint
                   from pg_settings
                   where name = 'shared_buffers')
from pg_buffercache;

select buffers_used + buffers_unused > 0,
        buffers_dirty <= buffers_used,
        buffers_pinned <= buffers_used
from pg_buffercache_summary();

SELECT count(*) > 0 FROM pg_buffercache_usage_counts() WHERE buffers >= 0;

-- Check that the functions / views can't be accessed by default. To avoid
--
-- 检查默认情况下无法访问功能/视图。为了避免
-- having to create a dedicated user, use the pg_database_owner pseudo-role.
--
-- 必须创建专用用户，请使用 pg_database_owner 伪角色。
SET ROLE pg_database_owner;
SELECT * FROM pg_buffercache;
SELECT * FROM pg_buffercache_pages() AS p (wrong int);
SELECT * FROM pg_buffercache_summary();
SELECT * FROM pg_buffercache_usage_counts();
RESET role;

-- Check that pg_monitor is allowed to query view / function
--
-- 检查是否允许 pg_monitor 查询视图/函数
SET ROLE pg_monitor;
SELECT count(*) > 0 FROM pg_buffercache;
SELECT buffers_used + buffers_unused > 0 FROM pg_buffercache_summary();
SELECT count(*) > 0 FROM pg_buffercache_usage_counts();
RESET role;


------
---- Test pg_buffercache_evict* functions
--
---- 测试 pg_buffercache_evict* 函数
------

CREATE ROLE regress_buffercache_normal;
SET ROLE regress_buffercache_normal;

-- These should fail because they need to be called as SUPERUSER
--
-- 这些应该会失败，因为它们需要被称为超级用户
SELECT * FROM pg_buffercache_evict(1);
SELECT * FROM pg_buffercache_evict_relation(1);
SELECT * FROM pg_buffercache_evict_all();

RESET ROLE;

-- These should return nothing, because these are STRICT functions
--
-- 这些应该不返回任何内容，因为这些是 STRICT 函数
SELECT * FROM pg_buffercache_evict(NULL);
SELECT * FROM pg_buffercache_evict_relation(NULL);

-- These should fail because they are not called by valid range of buffers
--
-- 这些应该失败，因为它们不是由有效的缓冲区范围调用的
-- Number of the shared buffers are limited by max integer
--
-- 共享缓冲区的数量受最大整数限制
SELECT 2147483647 max_buffers \gset
SELECT * FROM pg_buffercache_evict(-1);
SELECT * FROM pg_buffercache_evict(0);
SELECT * FROM pg_buffercache_evict(:max_buffers);

-- This should fail because pg_buffercache_evict_relation() doesn't accept
--
-- 这应该会失败，因为 pg_buffercache_evict_relation() 不接受
-- local relations
--
-- 地方关系
CREATE TEMP TABLE temp_pg_buffercache();
SELECT * FROM pg_buffercache_evict_relation('temp_pg_buffercache');
DROP TABLE temp_pg_buffercache;

-- These shouldn't fail
--
-- 这些不应该失败
SELECT buffer_evicted IS NOT NULL FROM pg_buffercache_evict(1);
SELECT buffers_evicted IS NOT NULL FROM pg_buffercache_evict_all();
CREATE TABLE shared_pg_buffercache();
SELECT buffers_evicted IS NOT NULL FROM pg_buffercache_evict_relation('shared_pg_buffercache');
DROP TABLE shared_pg_buffercache;

DROP ROLE regress_buffercache_normal;

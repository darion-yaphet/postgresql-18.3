-- Test pg_prewarm extension
--
-- 测试 pg_prewarm 扩展
CREATE EXTENSION pg_prewarm;

-- pg_prewarm() should fail if the target relation has no storage.
--
-- 如果目标关系没有存储，pg_prewarm() 应该失败。
CREATE TABLE test (c1 int) PARTITION BY RANGE (c1);
SELECT pg_prewarm('test', 'buffer');

-- Cleanup
DROP TABLE test;
DROP EXTENSION pg_prewarm;

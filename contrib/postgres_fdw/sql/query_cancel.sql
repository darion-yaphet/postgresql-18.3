SELECT version() ~ 'cygwin' AS skip_test \gset
\if :skip_test
\quit
\endif

-- Let's test canceling a remote query.  Use a table that does not have
--
-- 让我们测试一下取消远程查询。  使用没有的表
-- remote_estimate enabled, else there will be multiple queries to the
--
-- 启用remote_estimate，否则将会有多个查询
-- remote and we might unluckily send the cancel in between two of them.
--
-- 远程，我们可能不幸地在他们两个之间发送取消。
-- First let's confirm that the query is actually pushed down.
--
-- 首先我们确认查询确实被下推了。
EXPLAIN (VERBOSE, COSTS OFF)
SELECT count(*) FROM ft1 a CROSS JOIN ft1 b CROSS JOIN ft1 c CROSS JOIN ft1 d;

BEGIN;
-- Make sure that connection is open and set up.
--
-- 确保连接已打开并已设置。
SELECT count(*) FROM ft1 a;
-- On most machines, 10ms will be enough to be sure that we've sent the slow
--
-- 在大多数机器上，10 毫秒就足以确保我们已经发送了慢速数据
-- query.  We may sometimes exercise the race condition where we send cancel
--
-- 询问。  我们有时可能会执行发送取消的竞争条件
-- before the remote side starts the query, but that's fine too.
--
-- 在远程端开始查询之前，但这也很好。
SET LOCAL statement_timeout = '10ms';
-- This would take very long if not canceled:
--
-- 如果不取消，这将需要很长时间：
SELECT count(*) FROM ft1 a CROSS JOIN ft1 b CROSS JOIN ft1 c CROSS JOIN ft1 d;
COMMIT;

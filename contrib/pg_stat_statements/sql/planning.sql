--
-- Information related to planning
--
-- 与规划相关的信息
--

-- These tests require track_planning to be enabled.
--
-- 这些测试需要启用 track_planning。
SET pg_stat_statements.track_planning = TRUE;
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

--
-- [re]plan counting
--
-- [重新]计划计数
--
CREATE TABLE stats_plan_test ();
PREPARE prep1 AS SELECT COUNT(*) FROM stats_plan_test;
EXECUTE prep1;
EXECUTE prep1;
EXECUTE prep1;
ALTER TABLE stats_plan_test ADD COLUMN x int;
EXECUTE prep1;
SELECT 42;
SELECT 42;
SELECT 42;
SELECT plans, calls, rows, query FROM pg_stat_statements
  WHERE query NOT LIKE 'PREPARE%' ORDER BY query COLLATE "C";
-- for the prepared statement we expect at least one replan, but cache
--
-- 对于准备好的语句，我们期望至少有一次重新计划，但是缓存
-- invalidations could force more
--
-- 无效可能会迫使更多
SELECT plans >= 2 AND plans <= calls AS plans_ok, calls, rows, query FROM pg_stat_statements
  WHERE query LIKE 'PREPARE%' ORDER BY query COLLATE "C";

-- Cleanup
DROP TABLE stats_plan_test;
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

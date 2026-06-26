--
-- statement timestamps
--
-- 语句时间戳
--

-- planning time is needed during tests
--
-- 测试期间需要计划时间
SET pg_stat_statements.track_planning = TRUE;

SELECT 1 AS "STMTTS1";
SELECT now() AS ref_ts \gset
SELECT 1,2 AS "STMTTS2";
SELECT stats_since >= :'ref_ts', count(*) FROM pg_stat_statements
WHERE query LIKE '%STMTTS%'
GROUP BY stats_since >= :'ref_ts'
ORDER BY stats_since >= :'ref_ts';

SELECT now() AS ref_ts \gset
SELECT
  count(*) as total,
  count(*) FILTER (
    WHERE min_plan_time + max_plan_time = 0
  ) as minmax_plan_zero,
  count(*) FILTER (
    WHERE min_exec_time + max_exec_time = 0
  ) as minmax_exec_zero,
  count(*) FILTER (
    WHERE minmax_stats_since >= :'ref_ts'
  ) as minmax_stats_since_after_ref,
  count(*) FILTER (
    WHERE stats_since >= :'ref_ts'
  ) as stats_since_after_ref
FROM pg_stat_statements
WHERE query LIKE '%STMTTS%';

-- Perform single min/max reset
--
-- 执行单次最小/最大重置
SELECT pg_stat_statements_reset(0, 0, queryid, true) AS minmax_reset_ts
FROM pg_stat_statements
WHERE query LIKE '%STMTTS1%' \gset

-- check
SELECT
  count(*) as total,
  count(*) FILTER (
    WHERE min_plan_time + max_plan_time = 0
  ) as minmax_plan_zero,
  count(*) FILTER (
    WHERE min_exec_time + max_exec_time = 0
  ) as minmax_exec_zero,
  count(*) FILTER (
    WHERE minmax_stats_since >= :'ref_ts'
  ) as minmax_stats_since_after_ref,
  count(*) FILTER (
    WHERE stats_since >= :'ref_ts'
  ) as stats_since_after_ref
FROM pg_stat_statements
WHERE query LIKE '%STMTTS%';

-- check minmax reset timestamps
--
-- 检查最小最大重置时间戳
SELECT
query, minmax_stats_since = :'minmax_reset_ts' AS reset_ts_match
FROM pg_stat_statements
WHERE query LIKE '%STMTTS%'
ORDER BY query COLLATE "C";

-- check that minmax reset does not set stats_reset
--
-- 检查 minmax 重置是否未设置 stats_reset
SELECT
stats_reset = :'minmax_reset_ts' AS stats_reset_ts_match
FROM pg_stat_statements_info;

-- Perform common min/max reset
--
-- 执行公共最小/最大重置
SELECT pg_stat_statements_reset(0, 0, 0, true) AS minmax_reset_ts \gset

-- check again
--
-- 再次检查
SELECT
  count(*) as total,
  count(*) FILTER (
    WHERE min_plan_time + max_plan_time = 0
  ) as minmax_plan_zero,
  count(*) FILTER (
    WHERE min_exec_time + max_exec_time = 0
  ) as minmax_exec_zero,
  count(*) FILTER (
    WHERE minmax_stats_since >= :'ref_ts'
  ) as minmax_ts_after_ref,
  count(*) FILTER (
    WHERE minmax_stats_since = :'minmax_reset_ts'
  ) as minmax_ts_match,
  count(*) FILTER (
    WHERE stats_since >= :'ref_ts'
  ) as stats_since_after_ref
FROM pg_stat_statements
WHERE query LIKE '%STMTTS%';

-- Execute first query once more to check stats update
--
-- 再次执行第一个查询以检查统计信息更新
SELECT 1 AS "STMTTS1";

-- check
-- we don't check planing times here to be independent of
--
-- 我们不会在这里检查计划时间，以独立于
-- plan caching approach
--
-- 计划缓存方法
SELECT
  count(*) as total,
  count(*) FILTER (
    WHERE min_exec_time + max_exec_time = 0
  ) as minmax_exec_zero,
  count(*) FILTER (
    WHERE minmax_stats_since >= :'ref_ts'
  ) as minmax_ts_after_ref,
  count(*) FILTER (
    WHERE stats_since >= :'ref_ts'
  ) as stats_since_after_ref
FROM pg_stat_statements
WHERE query LIKE '%STMTTS%';

-- Cleanup
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

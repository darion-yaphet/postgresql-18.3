--
-- Cursors
--

-- These tests require track_utility to be enabled.
--
-- 这些测试需要启用 track_utility。
SET pg_stat_statements.track_utility = TRUE;
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- DECLARE
-- SELECT is normalized.
--
-- SELECT 已标准化。
DECLARE cursor_stats_1 CURSOR WITH HOLD FOR SELECT 1;
CLOSE cursor_stats_1;
DECLARE cursor_stats_1 CURSOR WITH HOLD FOR SELECT 2;
CLOSE cursor_stats_1;

SELECT calls, rows, query FROM pg_stat_statements ORDER BY query COLLATE "C";
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- FETCH
BEGIN;
DECLARE cursor_stats_1 CURSOR WITH HOLD FOR SELECT 2;
DECLARE cursor_stats_2 CURSOR WITH HOLD FOR SELECT 3;
FETCH 1 IN cursor_stats_1;
FETCH 1 IN cursor_stats_2;
CLOSE cursor_stats_1;
CLOSE cursor_stats_2;
COMMIT;

SELECT calls, rows, query FROM pg_stat_statements ORDER BY query COLLATE "C";
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

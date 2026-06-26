--
-- Only superusers and roles with privileges of the pg_read_all_stats role
--
-- 仅具有 pg_read_all_stats 角色权限的超级用户和角色
-- are allowed to see the SQL text and queryid of queries executed by
--
-- 允许查看 SQL 文本和执行的查询的 queryid
-- other users. Other users can see the statistics.
--
-- 其他用户。其他用户可以看到统计数据。
--

SET pg_stat_statements.track_utility = FALSE;
CREATE ROLE regress_stats_superuser SUPERUSER;
CREATE ROLE regress_stats_user1;
CREATE ROLE regress_stats_user2;
GRANT pg_read_all_stats TO regress_stats_user2;

SET ROLE regress_stats_superuser;
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT 1 AS "ONE";

SET ROLE regress_stats_user1;
SELECT 1+1 AS "TWO";

--
-- A superuser can read all columns of queries executed by others,
--
-- 超级用户可以读取其他人执行的查询的所有列，
-- including query text and queryid.
--
-- 包括查询文本和queryid。
--

SET ROLE regress_stats_superuser;
SELECT r.rolname, ss.queryid <> 0 AS queryid_bool, ss.query, ss.calls, ss.rows
  FROM pg_stat_statements ss JOIN pg_roles r ON ss.userid = r.oid
  ORDER BY r.rolname, ss.query COLLATE "C", ss.calls, ss.rows;

--
-- regress_stats_user1 has no privileges to read the query text or
--
-- regress_stats_user1 无权读取查询文本或
-- queryid of queries executed by others but can see statistics
--
-- 其他人执行的查询的queryid但可以看到统计信息
-- like calls and rows.
--
-- 比如通话和行。
--

SET ROLE regress_stats_user1;
SELECT r.rolname, ss.queryid <> 0 AS queryid_bool, ss.query, ss.calls, ss.rows
  FROM pg_stat_statements ss JOIN pg_roles r ON ss.userid = r.oid
  ORDER BY r.rolname, ss.query COLLATE "C", ss.calls, ss.rows;

--
-- regress_stats_user2, with pg_read_all_stats role privileges, can
--
-- regress_stats_user2，具有pg_read_all_stats角色权限，可以
-- read all columns, including query text and queryid, of queries
--
-- 读取查询的所有列，包括查询文本和查询 ID
-- executed by others.
--
-- 被他人执行。
--

SET ROLE regress_stats_user2;
SELECT r.rolname, ss.queryid <> 0 AS queryid_bool, ss.query, ss.calls, ss.rows
  FROM pg_stat_statements ss JOIN pg_roles r ON ss.userid = r.oid
  ORDER BY r.rolname, ss.query COLLATE "C", ss.calls, ss.rows;

--
-- cleanup
--

RESET ROLE;
DROP ROLE regress_stats_superuser;
DROP ROLE regress_stats_user1;
DROP ROLE regress_stats_user2;
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

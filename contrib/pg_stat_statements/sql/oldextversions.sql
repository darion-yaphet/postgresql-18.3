-- test old extension version entry points
--
-- 测试旧扩展版本入口点

CREATE EXTENSION pg_stat_statements WITH VERSION '1.4';
-- Execution of pg_stat_statements_reset() is granted only to
--
-- pg_stat_statements_reset() 的执行仅被授予
-- superusers in 1.4, so this fails.
--
-- 1.4 中的超级用户，因此失败。
SET SESSION AUTHORIZATION pg_read_all_stats;
SELECT pg_stat_statements_reset();
RESET SESSION AUTHORIZATION;

AlTER EXTENSION pg_stat_statements UPDATE TO '1.5';
-- Execution of pg_stat_statements_reset() should be granted to
--
-- pg_stat_statements_reset() 的执行应该被授予
-- pg_read_all_stats now, so this works.
--
-- 现在 pg_read_all_stats ，所以这有效。
SET SESSION AUTHORIZATION pg_read_all_stats;
SELECT pg_stat_statements_reset();
RESET SESSION AUTHORIZATION;

-- In 1.6, it got restricted back to superusers.
--
-- 在 1.6 中，它被限制回超级用户。
AlTER EXTENSION pg_stat_statements UPDATE TO '1.6';
SET SESSION AUTHORIZATION pg_read_all_stats;
SELECT pg_stat_statements_reset();
RESET SESSION AUTHORIZATION;
SELECT pg_get_functiondef('pg_stat_statements_reset'::regproc);

-- New function for pg_stat_statements_reset introduced, still
--
-- 引入了 pg_stat_statements_reset 的新函数，仍然
-- restricted for non-superusers.
--
-- 仅限于非超级用户。
AlTER EXTENSION pg_stat_statements UPDATE TO '1.7';
SET SESSION AUTHORIZATION pg_read_all_stats;
SELECT pg_stat_statements_reset();
RESET SESSION AUTHORIZATION;
SELECT pg_get_functiondef('pg_stat_statements_reset'::regproc);
SELECT pg_stat_statements_reset();
\d pg_stat_statements
SELECT count(*) > 0 AS has_data FROM pg_stat_statements;

-- New functions and views for pg_stat_statements in 1.8
--
-- 1.8 中 pg_stat_statements 的新函数和视图
AlTER EXTENSION pg_stat_statements UPDATE TO '1.8';
SELECT pg_get_functiondef('pg_stat_statements_reset'::regproc);
\d pg_stat_statements
SELECT count(*) > 0 AS has_data FROM pg_stat_statements;

-- New function pg_stat_statement_info, and new function
--
-- 新函数pg_stat_statement_info，以及新函数
-- and view for pg_stat_statements introduced in 1.9
--
-- 以及 1.9 中引入的 pg_stat_statements 视图
AlTER EXTENSION pg_stat_statements UPDATE TO '1.9';
SELECT pg_get_functiondef('pg_stat_statements_info'::regproc);
\d pg_stat_statements
SELECT count(*) > 0 AS has_data FROM pg_stat_statements;

-- New functions and views for pg_stat_statements in 1.10
--
-- 1.10 中 pg_stat_statements 的新函数和视图
AlTER EXTENSION pg_stat_statements UPDATE TO '1.10';
\d pg_stat_statements
SELECT count(*) > 0 AS has_data FROM pg_stat_statements;

-- New functions and views for pg_stat_statements in 1.11
--
-- 1.11 中 pg_stat_statements 的新函数和视图
AlTER EXTENSION pg_stat_statements UPDATE TO '1.11';
\d pg_stat_statements
SELECT count(*) > 0 AS has_data FROM pg_stat_statements;
-- New parameter minmax_only of pg_stat_statements_reset function
--
-- pg_stat_statements_reset函数的新参数minmax_only
SELECT pg_get_functiondef('pg_stat_statements_reset'::regproc);
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- New functions and views for pg_stat_statements in 1.12
--
-- 1.12 中 pg_stat_statements 的新函数和视图
AlTER EXTENSION pg_stat_statements UPDATE TO '1.12';
\d pg_stat_statements
SELECT count(*) > 0 AS has_data FROM pg_stat_statements;

DROP EXTENSION pg_stat_statements;

/* contrib/pg_stat_statements/pg_stat_statements--1.5--1.6.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
--
-- 抱怨脚本是否源自 psql，而不是通过 ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_stat_statements UPDATE TO '1.6'" to load this file. \quit

-- Execution is only allowed for superusers, fixing issue with 1.5.
--
-- 仅允许超级用户执行，解决了 1.5 的问题。
REVOKE EXECUTE ON FUNCTION pg_stat_statements_reset() FROM pg_read_all_stats;

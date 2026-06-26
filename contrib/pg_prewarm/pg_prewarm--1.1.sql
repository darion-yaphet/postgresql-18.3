/* contrib/pg_prewarm/pg_prewarm--1.1.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
--
-- 抱怨脚本是否源自 psql，而不是通过 CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_prewarm" to load this file. \quit

-- Register the function.
--
-- 注册该功能。
CREATE FUNCTION pg_prewarm(regclass,
						   mode text default 'buffer',
						   fork text default 'main',
						   first_block int8 default null,
						   last_block int8 default null)
RETURNS int8
AS 'MODULE_PATHNAME', 'pg_prewarm'
LANGUAGE C PARALLEL SAFE;

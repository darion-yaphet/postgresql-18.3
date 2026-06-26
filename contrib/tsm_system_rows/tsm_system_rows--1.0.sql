/* contrib/tsm_system_rows/tsm_system_rows--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
--
-- 抱怨脚本是否源自 psql，而不是通过 CREATE EXTENSION
\echo Use "CREATE EXTENSION tsm_system_rows" to load this file. \quit

CREATE FUNCTION system_rows(internal)
RETURNS tsm_handler
AS 'MODULE_PATHNAME', 'tsm_system_rows_handler'
LANGUAGE C STRICT;

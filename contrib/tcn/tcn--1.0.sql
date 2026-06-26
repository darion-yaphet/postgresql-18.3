/* contrib/tcn/tcn--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
--
-- 抱怨脚本是否源自 psql，而不是通过 CREATE EXTENSION
\echo Use "CREATE EXTENSION tcn" to load this file. \quit

CREATE FUNCTION triggered_change_notification()
RETURNS pg_catalog.trigger
AS 'MODULE_PATHNAME'
LANGUAGE C;

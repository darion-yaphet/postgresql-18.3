/* contrib/pgstattuple/pgstattuple--1.0--1.1.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
--
-- 抱怨脚本是否源自 psql，而不是通过 ALTER EXTENSION
\echo Use "ALTER EXTENSION pgstattuple UPDATE TO '1.1'" to load this file. \quit

CREATE FUNCTION pgstatginindex(IN relname regclass,
    OUT version INT4,
    OUT pending_pages INT4,
    OUT pending_tuples BIGINT)
AS 'MODULE_PATHNAME', 'pgstatginindex'
LANGUAGE C STRICT;

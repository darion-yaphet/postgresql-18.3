/* contrib/pgstattuple/pgstattuple--1.3--1.4.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
--
-- 抱怨脚本是否源自 psql，而不是通过 ALTER EXTENSION
\echo Use "ALTER EXTENSION pgstattuple UPDATE TO '1.4'" to load this file. \quit

ALTER FUNCTION pgstattuple(text) PARALLEL SAFE;
ALTER FUNCTION pgstatindex(text) PARALLEL SAFE;
ALTER FUNCTION pg_relpages(text) PARALLEL SAFE;
ALTER FUNCTION pgstatginindex(regclass) PARALLEL SAFE;
ALTER FUNCTION pgstattuple(regclass) PARALLEL SAFE;
ALTER FUNCTION pgstatindex(regclass) PARALLEL SAFE;
ALTER FUNCTION pg_relpages(regclass) PARALLEL SAFE;
ALTER FUNCTION pgstattuple_approx(regclass) PARALLEL SAFE;

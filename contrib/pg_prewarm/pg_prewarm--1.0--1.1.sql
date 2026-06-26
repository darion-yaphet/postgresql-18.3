/* contrib/pg_prewarm/pg_prewarm--1.0--1.1.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
--
-- 抱怨脚本是否源自 psql，而不是通过 ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_prewarm UPDATE TO '1.1'" to load this file. \quit

ALTER FUNCTION pg_prewarm(regclass, text, text, int8, int8) PARALLEL SAFE;

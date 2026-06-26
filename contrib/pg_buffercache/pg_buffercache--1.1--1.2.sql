/* contrib/pg_buffercache/pg_buffercache--1.1--1.2.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
--
-- 抱怨脚本是否源自 psql，而不是通过 ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_buffercache UPDATE TO '1.2'" to load this file. \quit

ALTER FUNCTION pg_buffercache_pages() PARALLEL SAFE;

/* contrib/pg_buffercache/pg_buffercache--1.5--1.6.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
--
-- 抱怨脚本是否源自 psql，而不是通过 CREATE EXTENSION
\echo Use "ALTER EXTENSION pg_buffercache UPDATE TO '1.6'" to load this file. \quit

-- Register the new functions.
--
-- 注册新功能。
CREATE OR REPLACE FUNCTION pg_buffercache_numa_pages()
RETURNS SETOF RECORD
AS 'MODULE_PATHNAME', 'pg_buffercache_numa_pages'
LANGUAGE C PARALLEL SAFE;

-- Create a view for convenient access.
--
-- 创建一个视图以方便访问。
CREATE VIEW pg_buffercache_numa AS
	SELECT P.* FROM pg_buffercache_numa_pages() AS P
	(bufferid integer, os_page_num bigint, numa_node integer);

-- Don't want these to be available to public.
--
-- 不希望这些内容向公众公开。
REVOKE ALL ON FUNCTION pg_buffercache_numa_pages() FROM PUBLIC;
REVOKE ALL ON pg_buffercache_numa FROM PUBLIC;

GRANT EXECUTE ON FUNCTION pg_buffercache_numa_pages() TO pg_monitor;
GRANT SELECT ON pg_buffercache_numa TO pg_monitor;


DROP FUNCTION pg_buffercache_evict(integer);
CREATE FUNCTION pg_buffercache_evict(
    IN int,
    OUT buffer_evicted boolean,
    OUT buffer_flushed boolean)
AS 'MODULE_PATHNAME', 'pg_buffercache_evict'
LANGUAGE C PARALLEL SAFE VOLATILE STRICT;

CREATE FUNCTION pg_buffercache_evict_relation(
    IN regclass,
    OUT buffers_evicted int4,
    OUT buffers_flushed int4,
    OUT buffers_skipped int4)
AS 'MODULE_PATHNAME', 'pg_buffercache_evict_relation'
LANGUAGE C PARALLEL SAFE VOLATILE STRICT;

CREATE FUNCTION pg_buffercache_evict_all(
    OUT buffers_evicted int4,
    OUT buffers_flushed int4,
    OUT buffers_skipped int4)
AS 'MODULE_PATHNAME', 'pg_buffercache_evict_all'
LANGUAGE C PARALLEL SAFE VOLATILE;

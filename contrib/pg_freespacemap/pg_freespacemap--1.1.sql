/* contrib/pg_freespacemap/pg_freespacemap--1.1.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
--
-- 抱怨脚本是否源自 psql，而不是通过 CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_freespacemap" to load this file. \quit

-- Register the C function.
--
-- 注册 C 函数。
CREATE FUNCTION pg_freespace(regclass, bigint)
RETURNS int2
AS 'MODULE_PATHNAME', 'pg_freespace'
LANGUAGE C STRICT PARALLEL SAFE;

-- pg_freespace shows the recorded space avail at each block in a relation
--
-- pg_freespace 显示关系中每个块记录的可用空间
CREATE FUNCTION
  pg_freespace(rel regclass, blkno OUT bigint, avail OUT int2)
RETURNS SETOF RECORD
AS $$
  SELECT blkno, pg_freespace($1, blkno) AS avail
  FROM generate_series(0, pg_relation_size($1) / current_setting('block_size')::bigint - 1) AS blkno;
$$
LANGUAGE SQL PARALLEL SAFE;


-- Don't want these to be available to public.
--
-- 不希望这些内容向公众公开。
REVOKE ALL ON FUNCTION pg_freespace(regclass, bigint) FROM PUBLIC;
REVOKE ALL ON FUNCTION pg_freespace(regclass) FROM PUBLIC;

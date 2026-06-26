/* contrib/pg_stat_statements/pg_stat_statements--1.0--1.1.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
--
-- 抱怨脚本是否源自 psql，而不是通过 ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_stat_statements UPDATE TO '1.1'" to load this file. \quit

/* First we have to remove them from the extension
 *
 * 首先我们必须从扩展中删除它们
 */
ALTER EXTENSION pg_stat_statements DROP VIEW pg_stat_statements;
ALTER EXTENSION pg_stat_statements DROP FUNCTION pg_stat_statements();

/* Then we can drop them
 *
 * 然后我们就可以丢掉它们
 */
DROP VIEW pg_stat_statements;
DROP FUNCTION pg_stat_statements();

/* Now redefine
 *
 * 现在重新定义
 */
CREATE FUNCTION pg_stat_statements(
    OUT userid oid,
    OUT dbid oid,
    OUT query text,
    OUT calls int8,
    OUT total_time float8,
    OUT rows int8,
    OUT shared_blks_hit int8,
    OUT shared_blks_read int8,
    OUT shared_blks_dirtied int8,
    OUT shared_blks_written int8,
    OUT local_blks_hit int8,
    OUT local_blks_read int8,
    OUT local_blks_dirtied int8,
    OUT local_blks_written int8,
    OUT temp_blks_read int8,
    OUT temp_blks_written int8,
    OUT blk_read_time float8,
    OUT blk_write_time float8
)
RETURNS SETOF record
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE VIEW pg_stat_statements AS
  SELECT * FROM pg_stat_statements();

GRANT SELECT ON pg_stat_statements TO PUBLIC;

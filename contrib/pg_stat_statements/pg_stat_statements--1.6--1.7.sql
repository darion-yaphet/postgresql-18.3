/* contrib/pg_stat_statements/pg_stat_statements--1.6--1.7.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
--
-- 抱怨脚本是否源自 psql，而不是通过 ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_stat_statements UPDATE TO '1.7'" to load this file. \quit

/* First we have to remove them from the extension
 *
 * 首先我们必须从扩展中删除它们
 */
ALTER EXTENSION pg_stat_statements DROP FUNCTION pg_stat_statements_reset();

/* Then we can drop them
 *
 * 然后我们就可以丢掉它们
 */
DROP FUNCTION pg_stat_statements_reset();

/* Now redefine
 *
 * 现在重新定义
 */
CREATE FUNCTION pg_stat_statements_reset(IN userid Oid DEFAULT 0,
	IN dbid Oid DEFAULT 0,
	IN queryid bigint DEFAULT 0
)
RETURNS void
AS 'MODULE_PATHNAME', 'pg_stat_statements_reset_1_7'
LANGUAGE C STRICT PARALLEL SAFE;

-- Don't want this to be available to non-superusers.
--
-- 不希望非超级用户可以使用此功能。
REVOKE ALL ON FUNCTION pg_stat_statements_reset(Oid, Oid, bigint) FROM PUBLIC;

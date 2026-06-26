/* contrib/amcheck/amcheck--1.3--1.4.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
--
-- 抱怨脚本是否源自 psql，而不是通过 CREATE EXTENSION
\echo Use "ALTER EXTENSION amcheck UPDATE TO '1.4'" to load this file. \quit

-- In order to avoid issues with dependencies when updating amcheck to 1.4,
--
-- 为了避免将 amcheck 更新到 1.4 时出现依赖关系问题，
-- create new, overloaded versions of the 1.2 bt_index_parent_check signature,
--
-- 创建 1.2 bt_index_parent_check 签名的新重载版本，
-- and 1.1 bt_index_check signature.
--
-- 和1.1 bt_index_check签名。

--
-- bt_index_parent_check()
--
CREATE FUNCTION bt_index_parent_check(index regclass,
    heapallindexed boolean, rootdescend boolean, checkunique boolean)
RETURNS VOID
AS 'MODULE_PATHNAME', 'bt_index_parent_check'
LANGUAGE C STRICT PARALLEL RESTRICTED;
--
-- bt_index_check()
--
CREATE FUNCTION bt_index_check(index regclass,
    heapallindexed boolean, checkunique boolean)
RETURNS VOID
AS 'MODULE_PATHNAME', 'bt_index_check'
LANGUAGE C STRICT PARALLEL RESTRICTED;

-- We don't want this to be available to public
--
-- 我们不希望将其公开
REVOKE ALL ON FUNCTION bt_index_parent_check(regclass, boolean, boolean, boolean) FROM PUBLIC;
REVOKE ALL ON FUNCTION bt_index_check(regclass, boolean, boolean) FROM PUBLIC;

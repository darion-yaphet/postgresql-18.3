/* contrib/amcheck/amcheck--1.1--1.2.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
--
-- 抱怨脚本是否源自 psql，而不是通过 CREATE EXTENSION
\echo Use "ALTER EXTENSION amcheck UPDATE TO '1.2'" to load this file. \quit

-- In order to avoid issues with dependencies when updating amcheck to 1.2,
--
-- 为了避免将 amcheck 更新到 1.2 时出现依赖关系问题，
-- create new, overloaded version of the 1.1 function signature
--
-- 创建 1.1 函数签名的新重载版本

--
-- bt_index_parent_check()
--
CREATE FUNCTION bt_index_parent_check(index regclass,
    heapallindexed boolean, rootdescend boolean)
RETURNS VOID
AS 'MODULE_PATHNAME', 'bt_index_parent_check'
LANGUAGE C STRICT PARALLEL RESTRICTED;

-- Don't want this to be available to public
--
-- 不希望公开此信息
REVOKE ALL ON FUNCTION bt_index_parent_check(regclass, boolean, boolean) FROM PUBLIC;

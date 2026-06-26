/* contrib/pg_visibility/pg_visibility--1.1.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
--
-- 抱怨脚本是否源自 psql，而不是通过 CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_visibility" to load this file. \quit

-- Show visibility map information.
--
-- 显示可见性地图信息。
CREATE FUNCTION pg_visibility_map(regclass, blkno bigint,
								  all_visible OUT boolean,
								  all_frozen OUT boolean)
RETURNS record
AS 'MODULE_PATHNAME', 'pg_visibility_map'
LANGUAGE C STRICT;

-- Show visibility map and page-level visibility information.
--
-- 显示可见性地图和页面级可见性信息。
CREATE FUNCTION pg_visibility(regclass, blkno bigint,
							  all_visible OUT boolean,
							  all_frozen OUT boolean,
							  pd_all_visible OUT boolean)
RETURNS record
AS 'MODULE_PATHNAME', 'pg_visibility'
LANGUAGE C STRICT;

-- Show visibility map information for each block in a relation.
--
-- 显示关系中每个块的可见性映射信息。
CREATE FUNCTION pg_visibility_map(regclass, blkno OUT bigint,
								  all_visible OUT boolean,
								  all_frozen OUT boolean)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_visibility_map_rel'
LANGUAGE C STRICT;

-- Show visibility map and page-level visibility information for each block.
--
-- 显示每个块的可见性地图和页面级可见性信息。
CREATE FUNCTION pg_visibility(regclass, blkno OUT bigint,
							  all_visible OUT boolean,
							  all_frozen OUT boolean,
							  pd_all_visible OUT boolean)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_visibility_rel'
LANGUAGE C STRICT;

-- Show summary of visibility map bits for a relation.
--
-- 显示关系的可见性映射位的摘要。
CREATE FUNCTION pg_visibility_map_summary(regclass,
    OUT all_visible bigint, OUT all_frozen bigint)
RETURNS record
AS 'MODULE_PATHNAME', 'pg_visibility_map_summary'
LANGUAGE C STRICT;

-- Show tupleids of non-frozen tuples if any in all_frozen pages
--
-- 显示 all_frozen 页面中非冻结元组的 tupleid（如果有）
-- for a relation.
--
-- 为了某种关系。
CREATE FUNCTION pg_check_frozen(regclass, t_ctid OUT tid)
RETURNS SETOF tid
AS 'MODULE_PATHNAME', 'pg_check_frozen'
LANGUAGE C STRICT;

-- Show tupleids of dead tuples if any in all_visible pages for a relation.
--
-- 显示关系的 all_visible 页面中的死元组（如果有）的元组 ID。
CREATE FUNCTION pg_check_visible(regclass, t_ctid OUT tid)
RETURNS SETOF tid
AS 'MODULE_PATHNAME', 'pg_check_visible'
LANGUAGE C STRICT;

-- Truncate the visibility map fork.
--
-- 截断可见性贴图分支。
CREATE FUNCTION pg_truncate_visibility_map(regclass)
RETURNS void
AS 'MODULE_PATHNAME', 'pg_truncate_visibility_map'
LANGUAGE C STRICT
PARALLEL UNSAFE;  -- let's not make this any more dangerous

-- Don't want these to be available to public.
--
-- 不希望这些内容向公众公开。
REVOKE ALL ON FUNCTION pg_visibility_map(regclass, bigint) FROM PUBLIC;
REVOKE ALL ON FUNCTION pg_visibility(regclass, bigint) FROM PUBLIC;
REVOKE ALL ON FUNCTION pg_visibility_map(regclass) FROM PUBLIC;
REVOKE ALL ON FUNCTION pg_visibility(regclass) FROM PUBLIC;
REVOKE ALL ON FUNCTION pg_visibility_map_summary(regclass) FROM PUBLIC;
REVOKE ALL ON FUNCTION pg_check_frozen(regclass) FROM PUBLIC;
REVOKE ALL ON FUNCTION pg_check_visible(regclass) FROM PUBLIC;
REVOKE ALL ON FUNCTION pg_truncate_visibility_map(regclass) FROM PUBLIC;

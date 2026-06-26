/* contrib/cube/cube--1.3--1.4.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
--
-- 抱怨脚本是否源自 psql，而不是通过 ALTER EXTENSION
\echo Use "ALTER EXTENSION cube UPDATE TO '1.4'" to load this file. \quit

--
-- Get rid of unnecessary compress and decompress support functions.
--
-- 摆脱不必要的压缩和解压缩支持功能。
--
-- To be allowed to drop the opclass entry for a support function,
--
-- 要被允许删除支持函数的 opclass 条目，
-- we must change the entry's dependency type from 'internal' to 'auto',
--
-- 我们必须将条目的依赖类型从“内部”更改为“自动”，
-- as though it were a loose member of the opfamily rather than being
--
-- 就好像它是opfamily中的一个松散成员，而不是
-- bound into a particular opclass.  There's no SQL command for that,
--
-- 绑定到特定的操作类。  没有 SQL 命令可以做到这一点，
-- so fake it with a manual update on pg_depend.
--
-- 所以通过手动更新 pg_depend 来伪造它。
--
DO LANGUAGE plpgsql
$$
DECLARE
  my_schema pg_catalog.text := pg_catalog.quote_ident(pg_catalog.current_schema());
  old_path pg_catalog.text := pg_catalog.current_setting('search_path');
BEGIN
-- for safety, transiently set search_path to just pg_catalog+pg_temp
--
-- 为了安全起见，暂时将 search_path 设置为 pg_catalog+pg_temp
PERFORM pg_catalog.set_config('search_path', 'pg_catalog, pg_temp', true);

UPDATE pg_catalog.pg_depend
SET deptype = 'a'
WHERE classid = 'pg_catalog.pg_amproc'::pg_catalog.regclass
  AND objid =
    (SELECT objid
     FROM pg_catalog.pg_depend
     WHERE classid = 'pg_catalog.pg_amproc'::pg_catalog.regclass
       AND refclassid = 'pg_catalog.pg_proc'::pg_catalog.regclass
       AND (refobjid = (my_schema || '.g_cube_compress(pg_catalog.internal)')::pg_catalog.regprocedure))
  AND refclassid = 'pg_catalog.pg_opclass'::pg_catalog.regclass
  AND deptype = 'i';

UPDATE pg_catalog.pg_depend
SET deptype = 'a'
WHERE classid = 'pg_catalog.pg_amproc'::pg_catalog.regclass
  AND objid =
    (SELECT objid
     FROM pg_catalog.pg_depend
     WHERE classid = 'pg_catalog.pg_amproc'::pg_catalog.regclass
       AND refclassid = 'pg_catalog.pg_proc'::pg_catalog.regclass
       AND (refobjid = (my_schema || '.g_cube_decompress(pg_catalog.internal)')::pg_catalog.regprocedure))
  AND refclassid = 'pg_catalog.pg_opclass'::pg_catalog.regclass
  AND deptype = 'i';

PERFORM pg_catalog.set_config('search_path', old_path, true);
END
$$;

ALTER OPERATOR FAMILY gist_cube_ops USING gist drop function 3 (cube);
ALTER EXTENSION cube DROP function g_cube_compress(pg_catalog.internal);
DROP FUNCTION g_cube_compress(pg_catalog.internal);

ALTER OPERATOR FAMILY gist_cube_ops USING gist drop function 4 (cube);
ALTER EXTENSION cube DROP function g_cube_decompress(pg_catalog.internal);
DROP FUNCTION g_cube_decompress(pg_catalog.internal);

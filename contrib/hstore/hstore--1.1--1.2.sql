/* contrib/hstore/hstore--1.1--1.2.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
--
-- 抱怨脚本是否源自 psql，而不是通过 ALTER EXTENSION
\echo Use "ALTER EXTENSION hstore UPDATE TO '1.2'" to load this file. \quit


-- A version of 1.1 was shipped with these objects mistakenly in 9.3.0.
--
-- 1.1 版本在 9.3.0 中错误地随这些对象一起提供。
-- Therefore we only add them if we detect that they aren't already there and
--
-- 因此，只有当我们检测到它们尚不存在时，我们才会添加它们，并且
-- dependent on the extension.
--
-- 取决于扩展名。

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

   PERFORM 1
   FROM pg_proc p
       JOIN  pg_depend d
          ON p.proname = 'hstore_to_json_loose'
            AND d.classid = 'pg_proc'::regclass
            AND d.objid = p.oid
            AND d.refclassid = 'pg_extension'::regclass
       JOIN pg_extension x
          ON d.refobjid = x.oid
            AND  x.extname = 'hstore';

   IF NOT FOUND
   THEN
        PERFORM pg_catalog.set_config('search_path', old_path, true);

        CREATE FUNCTION hstore_to_json(hstore)
        RETURNS json
        AS 'MODULE_PATHNAME', 'hstore_to_json'
        LANGUAGE C IMMUTABLE STRICT;

        CREATE CAST (hstore AS json)
          WITH FUNCTION hstore_to_json(hstore);

        CREATE FUNCTION hstore_to_json_loose(hstore)
        RETURNS json
        AS 'MODULE_PATHNAME', 'hstore_to_json_loose'
        LANGUAGE C IMMUTABLE STRICT;

   END IF;

PERFORM pg_catalog.set_config('search_path', old_path, true);
END;

$$;

/* contrib/btree_gist/btree_gist--1.1--1.2.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
--
-- 抱怨脚本是否源自 psql，而不是通过 ALTER EXTENSION
\echo Use "ALTER EXTENSION btree_gist UPDATE TO '1.2'" to load this file. \quit

-- Update procedure signatures the hard way.
--
-- 以困难的方式更新过程签名。
-- We use to_regprocedure() so that query doesn't fail if run against 9.6beta1 definitions,
--
-- 我们使用 to_regprocedure() 以便查询在针对 9.6beta1 定义运行时不会失败，
-- wherein the signatures have been updated already.  In that case to_regprocedure() will
--
-- 其中签名已经更新。  在这种情况下 to_regprocedure() 将
-- return NULL and no updates will happen.
--
-- 返回 NULL 并且不会发生任何更新。

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

UPDATE pg_catalog.pg_proc SET
  proargtypes = pg_catalog.array_to_string(newtypes::pg_catalog.oid[], ' ')::pg_catalog.oidvector,
  pronargs = pg_catalog.array_length(newtypes, 1)
FROM (VALUES
(NULL::pg_catalog.text, NULL::pg_catalog.text[]), -- establish column types
('gbt_oid_distance(internal,oid,int2,oid)', '{internal,oid,int2,oid,internal}'),
('gbt_oid_union(bytea,internal)', '{internal,internal}'),
('gbt_oid_same(internal,internal,internal)', '{SCH.gbtreekey8,SCH.gbtreekey8,internal}'),
('gbt_int2_distance(internal,int2,int2,oid)', '{internal,int2,int2,oid,internal}'),
('gbt_int2_union(bytea,internal)', '{internal,internal}'),
('gbt_int2_same(internal,internal,internal)', '{SCH.gbtreekey4,SCH.gbtreekey4,internal}'),
('gbt_int4_distance(internal,int4,int2,oid)', '{internal,int4,int2,oid,internal}'),
('gbt_int4_union(bytea,internal)', '{internal,internal}'),
('gbt_int4_same(internal,internal,internal)', '{SCH.gbtreekey8,SCH.gbtreekey8,internal}'),
('gbt_int8_distance(internal,int8,int2,oid)', '{internal,int8,int2,oid,internal}'),
('gbt_int8_union(bytea,internal)', '{internal,internal}'),
('gbt_int8_same(internal,internal,internal)', '{SCH.gbtreekey16,SCH.gbtreekey16,internal}'),
('gbt_float4_distance(internal,float4,int2,oid)', '{internal,float4,int2,oid,internal}'),
('gbt_float4_union(bytea,internal)', '{internal,internal}'),
('gbt_float4_same(internal,internal,internal)', '{SCH.gbtreekey8,SCH.gbtreekey8,internal}'),
('gbt_float8_distance(internal,float8,int2,oid)', '{internal,float8,int2,oid,internal}'),
('gbt_float8_union(bytea,internal)', '{internal,internal}'),
('gbt_float8_same(internal,internal,internal)', '{SCH.gbtreekey16,SCH.gbtreekey16,internal}'),
('gbt_ts_distance(internal,timestamp,int2,oid)', '{internal,timestamp,int2,oid,internal}'),
('gbt_tstz_distance(internal,timestamptz,int2,oid)', '{internal,timestamptz,int2,oid,internal}'),
('gbt_ts_union(bytea,internal)', '{internal,internal}'),
('gbt_ts_same(internal,internal,internal)', '{SCH.gbtreekey16,SCH.gbtreekey16,internal}'),
('gbt_time_distance(internal,time,int2,oid)', '{internal,time,int2,oid,internal}'),
('gbt_time_union(bytea,internal)', '{internal,internal}'),
('gbt_time_same(internal,internal,internal)', '{SCH.gbtreekey16,SCH.gbtreekey16,internal}'),
('gbt_date_distance(internal,date,int2,oid)', '{internal,date,int2,oid,internal}'),
('gbt_date_union(bytea,internal)', '{internal,internal}'),
('gbt_date_same(internal,internal,internal)', '{SCH.gbtreekey8,SCH.gbtreekey8,internal}'),
('gbt_intv_distance(internal,interval,int2,oid)', '{internal,interval,int2,oid,internal}'),
('gbt_intv_union(bytea,internal)', '{internal,internal}'),
('gbt_intv_same(internal,internal,internal)', '{SCH.gbtreekey32,SCH.gbtreekey32,internal}'),
('gbt_cash_distance(internal,money,int2,oid)', '{internal,money,int2,oid,internal}'),
('gbt_cash_union(bytea,internal)', '{internal,internal}'),
('gbt_cash_same(internal,internal,internal)', '{SCH.gbtreekey16,SCH.gbtreekey16,internal}'),
('gbt_macad_union(bytea,internal)', '{internal,internal}'),
('gbt_macad_same(internal,internal,internal)', '{SCH.gbtreekey16,SCH.gbtreekey16,internal}'),
('gbt_text_union(bytea,internal)', '{internal,internal}'),
('gbt_text_same(internal,internal,internal)', '{SCH.gbtreekey_var,SCH.gbtreekey_var,internal}'),
('gbt_bytea_union(bytea,internal)', '{internal,internal}'),
('gbt_bytea_same(internal,internal,internal)', '{SCH.gbtreekey_var,SCH.gbtreekey_var,internal}'),
('gbt_numeric_union(bytea,internal)', '{internal,internal}'),
('gbt_numeric_same(internal,internal,internal)', '{SCH.gbtreekey_var,SCH.gbtreekey_var,internal}'),
('gbt_bit_union(bytea,internal)', '{internal,internal}'),
('gbt_bit_same(internal,internal,internal)', '{SCH.gbtreekey_var,SCH.gbtreekey_var,internal}'),
('gbt_inet_union(bytea,internal)', '{internal,internal}'),
('gbt_inet_same(internal,internal,internal)', '{SCH.gbtreekey16,SCH.gbtreekey16,internal}')
) AS update_data (oldproc, newtypestext),
LATERAL (
  SELECT array_agg(replace(typ, 'SCH', my_schema)::regtype) as newtypes FROM unnest(newtypestext) typ
) ls
WHERE oid = to_regprocedure(my_schema || '.' || replace(oldproc, 'SCH', my_schema));

PERFORM pg_catalog.set_config('search_path', old_path, true);
END
$$;

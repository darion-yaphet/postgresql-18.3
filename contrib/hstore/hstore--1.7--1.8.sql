/* contrib/hstore/hstore--1.7--1.8.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
--
-- 抱怨脚本是否源自 psql，而不是通过 ALTER EXTENSION
\echo Use "ALTER EXTENSION hstore UPDATE TO '1.8'" to load this file. \quit

CREATE FUNCTION hstore_subscript_handler(internal)
RETURNS internal
AS 'MODULE_PATHNAME', 'hstore_subscript_handler'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

ALTER TYPE hstore SET (
  SUBSCRIPT = hstore_subscript_handler
);

-- Remove @ and ~
--
-- 删除@和~
DROP OPERATOR @ (hstore, hstore);
DROP OPERATOR ~ (hstore, hstore);

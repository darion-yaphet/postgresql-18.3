/* contrib/hstore/hstore--1.5--1.6.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
--
-- 抱怨脚本是否源自 psql，而不是通过 ALTER EXTENSION
\echo Use "ALTER EXTENSION hstore UPDATE TO '1.6'" to load this file. \quit

CREATE FUNCTION hstore_hash_extended(hstore, int8)
RETURNS int8
AS 'MODULE_PATHNAME','hstore_hash_extended'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

ALTER OPERATOR FAMILY hash_hstore_ops USING hash ADD
    FUNCTION    2   hstore_hash_extended(hstore, int8);

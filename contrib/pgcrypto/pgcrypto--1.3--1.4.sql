/* contrib/pgcrypto/pgcrypto--1.3--1.4.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
--
-- 抱怨脚本是否源自 psql，而不是通过 ALTER EXTENSION
\echo Use "ALTER EXTENSION pgcrypto UPDATE TO '1.4'" to load this file. \quit

CREATE FUNCTION fips_mode()
RETURNS bool
AS 'MODULE_PATHNAME', 'pg_check_fipsmode'
LANGUAGE C VOLATILE STRICT PARALLEL SAFE;

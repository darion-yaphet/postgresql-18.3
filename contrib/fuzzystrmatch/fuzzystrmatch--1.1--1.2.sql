/* contrib/fuzzystrmatch/fuzzystrmatch--1.1--1.2.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
--
-- 抱怨脚本是否源自 psql，而不是通过 ALTER EXTENSION
\echo Use "ALTER EXTENSION fuzzystrmatch UPDATE TO '1.2'" to load this file. \quit

CREATE FUNCTION daitch_mokotoff(text) RETURNS text[]
AS 'MODULE_PATHNAME', 'daitch_mokotoff'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

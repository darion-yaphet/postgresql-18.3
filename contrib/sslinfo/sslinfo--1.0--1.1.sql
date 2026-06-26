/* contrib/sslinfo/sslinfo--1.0--1.1.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
--
-- 抱怨脚本是否源自 psql，而不是通过 CREATE EXTENSION
\echo Use "ALTER EXTENSION sslinfo UPDATE TO '1.1'" to load this file. \quit

CREATE FUNCTION
ssl_extension_info(OUT name text,
	OUT value text,
    OUT critical boolean
) RETURNS SETOF record
AS 'MODULE_PATHNAME', 'ssl_extension_info'
LANGUAGE C STRICT;

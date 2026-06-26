/* contrib/isn/isn--1.2--1.3.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
--
-- 抱怨脚本是否源自 psql，而不是通过 ALTER EXTENSION
\echo Use "ALTER EXTENSION isn UPDATE TO '1.3'" to load this file. \quit

ALTER FUNCTION isn_weak(boolean) VOLATILE PARALLEL UNSAFE;
ALTER FUNCTION isn_weak() STABLE PARALLEL SAFE;

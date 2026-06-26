/* contrib/lo/lo--1.0--1.1.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
--
-- 抱怨脚本是否源自 psql，而不是通过 ALTER EXTENSION
\echo Use "ALTER EXTENSION lo UPDATE TO '1.1'" to load this file. \quit

ALTER FUNCTION lo_oid(lo) PARALLEL SAFE;

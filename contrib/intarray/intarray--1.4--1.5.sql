/* contrib/intarray/intarray--1.4--1.5.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
--
-- 抱怨脚本是否源自 psql，而不是通过 ALTER EXTENSION
\echo Use "ALTER EXTENSION intarray UPDATE TO '1.5'" to load this file. \quit

-- Remove @ and ~
--
-- 删除@和~
DROP OPERATOR @ (_int4, _int4);
DROP OPERATOR ~ (_int4, _int4);

/* contrib/seg/seg--1.3--1.4.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
--
-- 抱怨脚本是否源自 psql，而不是通过 ALTER EXTENSION
\echo Use "ALTER EXTENSION seg UPDATE TO '1.4'" to load this file. \quit

-- Remove @ and ~
--
-- 删除@和~
DROP OPERATOR @ (seg, seg);
DROP OPERATOR ~ (seg, seg);

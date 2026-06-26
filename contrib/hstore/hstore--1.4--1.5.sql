/* contrib/hstore/hstore--1.4--1.5.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
--
-- 抱怨脚本是否源自 psql，而不是通过 ALTER EXTENSION
\echo Use "ALTER EXTENSION hstore UPDATE TO '1.5'" to load this file. \quit

ALTER OPERATOR #<=# (hstore, hstore) SET (
       RESTRICT = scalarlesel,
       JOIN = scalarlejoinsel
);

ALTER OPERATOR #>=# (hstore, hstore) SET (
       RESTRICT = scalargesel,
       JOIN = scalargejoinsel
);

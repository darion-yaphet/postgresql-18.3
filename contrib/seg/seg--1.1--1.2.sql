/* contrib/seg/seg--1.1--1.2.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
--
-- 抱怨脚本是否源自 psql，而不是通过 ALTER EXTENSION
\echo Use "ALTER EXTENSION seg UPDATE TO '1.2'" to load this file. \quit

ALTER OPERATOR <= (seg, seg) SET (
	RESTRICT = scalarlesel,
	JOIN = scalarlejoinsel
);

ALTER OPERATOR >= (seg, seg) SET (
	RESTRICT = scalargesel,
	JOIN = scalargejoinsel
);

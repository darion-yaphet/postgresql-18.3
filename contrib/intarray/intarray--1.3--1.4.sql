/* contrib/intarray/intarray--1.3--1.4.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
--
-- 抱怨脚本是否源自 psql，而不是通过 ALTER EXTENSION
\echo Use "ALTER EXTENSION intarray UPDATE TO '1.4'" to load this file. \quit

-- Remove <@ from the GiST opclasses, as it's not usefully indexable
--
-- 从 GiST opclasses 中删除 <@，因为它不能有效地进行索引
-- due to mishandling of empty arrays.  (It's OK in GIN.)
--
-- 由于空数组处理不当。  （在 GIN 中没问题。）

ALTER OPERATOR FAMILY gist__int_ops USING gist
DROP OPERATOR 8 (_int4, _int4);

ALTER OPERATOR FAMILY gist__intbig_ops USING gist
DROP OPERATOR 8 (_int4, _int4);

-- Likewise for the old spelling ~.
--
-- 旧拼写也同样如此。

ALTER OPERATOR FAMILY gist__int_ops USING gist
DROP OPERATOR 14 (_int4, _int4);

ALTER OPERATOR FAMILY gist__intbig_ops USING gist
DROP OPERATOR 14 (_int4, _int4);

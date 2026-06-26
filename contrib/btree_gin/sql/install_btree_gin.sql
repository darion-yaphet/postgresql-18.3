CREATE EXTENSION btree_gin;

-- Check whether any of our opclasses fail amvalidate
--
-- 检查我们的任何 opclass 是否未通过 amvalidate
SELECT amname, opcname
FROM pg_opclass opc LEFT JOIN pg_am am ON am.oid = opcmethod
WHERE opc.oid >= 16384 AND NOT amvalidate(opc.oid);

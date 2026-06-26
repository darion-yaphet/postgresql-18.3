/* contrib/cube/cube--1.4--1.5.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
--
-- 抱怨脚本是否源自 psql，而不是通过 ALTER EXTENSION
\echo Use "ALTER EXTENSION cube UPDATE TO '1.5'" to load this file. \quit

-- Remove @ and ~
--
-- 删除@和~
DROP OPERATOR @ (cube, cube);
DROP OPERATOR ~ (cube, cube);

-- Add binary input/output handlers
--
-- 添加二进制输入/输出处理程序
CREATE FUNCTION cube_recv(internal)
RETURNS cube
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION cube_send(cube)
RETURNS bytea
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

ALTER TYPE cube SET ( RECEIVE = cube_recv, SEND = cube_send );

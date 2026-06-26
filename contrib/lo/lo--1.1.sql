/* contrib/lo/lo--1.1.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
--
-- 抱怨脚本是否源自 psql，而不是通过 CREATE EXTENSION
\echo Use "CREATE EXTENSION lo" to load this file. \quit

--
--	Create the data type ... now just a domain over OID
--
--	创建数据类型...现在只是 OID 上的域
--

CREATE DOMAIN lo AS pg_catalog.oid;

--
-- For backwards compatibility, define a function named lo_oid.
--
-- 为了向后兼容，定义一个名为 lo_oid 的函数。
--
-- The other functions that formerly existed are not needed because
--
-- 以前存在的其他功能不再需要，因为
-- the implicit casts between a domain and its underlying type handle them.
--
-- 域及其底层类型之间的隐式转换可以处理它们。
--
CREATE FUNCTION lo_oid(lo) RETURNS pg_catalog.oid AS
'SELECT $1::pg_catalog.oid' LANGUAGE SQL STRICT IMMUTABLE PARALLEL SAFE;

-- This is used in triggers
--
-- 这在触发器中使用
CREATE FUNCTION lo_manage()
RETURNS pg_catalog.trigger
AS 'MODULE_PATHNAME'
LANGUAGE C;

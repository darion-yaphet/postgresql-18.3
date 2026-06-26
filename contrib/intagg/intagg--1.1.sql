/* contrib/intagg/intagg--1.1.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
--
-- 抱怨脚本是否源自 psql，而不是通过 CREATE EXTENSION
\echo Use "CREATE EXTENSION intagg" to load this file. \quit

-- Internal function for the aggregate
--
-- 聚合的内部函数
-- Is called for each item in an aggregation
--
-- 为聚合中的每个项目调用
CREATE FUNCTION int_agg_state (internal, int4)
RETURNS internal
AS 'array_agg_transfn'
PARALLEL SAFE
LANGUAGE INTERNAL;

-- Internal function for the aggregate
--
-- 聚合的内部函数
-- Is called at the end of the aggregation, and returns an array.
--
-- 在聚合结束时调用，并返回一个数组。
CREATE FUNCTION int_agg_final_array (internal)
RETURNS int4[]
AS 'array_agg_finalfn'
PARALLEL SAFE
LANGUAGE INTERNAL;

-- The aggregate function itself
--
-- 聚合函数本身
-- uses the above functions to create an array of integers from an aggregation.
--
-- 使用上述函数从聚合创建整数数组。
CREATE AGGREGATE int_array_aggregate(int4) (
	SFUNC = int_agg_state,
	STYPE = internal,
	FINALFUNC = int_agg_final_array,
	PARALLEL = SAFE
);

-- The enumeration function
--
-- 枚举函数
-- returns each element in a one dimensional integer array
--
-- 返回一维整数数组中的每个元素
-- as a row.
--
-- 作为一排。
CREATE FUNCTION int_array_enum(int4[])
RETURNS setof integer
AS 'array_unnest'
LANGUAGE INTERNAL IMMUTABLE STRICT PARALLEL SAFE;

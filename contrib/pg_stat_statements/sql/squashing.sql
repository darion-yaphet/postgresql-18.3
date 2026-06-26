--
-- Const squashing functionality
--
-- 常量压缩功能
--
CREATE EXTENSION pg_stat_statements;

--
-- Simple Lists
--
-- 简单列表
--

CREATE TABLE test_squash (id int, data int);

-- single element will not be squashed
--
-- 单个元素不会被压扁
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM test_squash WHERE id IN (1);
SELECT ARRAY[1];
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- more than 1 element in a list will be squashed
--
-- 列表中超过 1 个元素将被压缩
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM test_squash WHERE id IN (1, 2, 3);
SELECT * FROM test_squash WHERE id IN (1, 2, 3, 4);
SELECT * FROM test_squash WHERE id IN (1, 2, 3, 4, 5);
SELECT ARRAY[1, 2, 3];
SELECT ARRAY[1, 2, 3, 4];
SELECT ARRAY[1, 2, 3, 4, 5];
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- built-in functions will be squashed
--
-- 内置函数将被压缩
-- the IN and ARRAY forms of this statement will have the same queryId
--
-- 该语句的 IN 和 ARRAY 形式将具有相同的 queryId
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT WHERE 1 IN (1, int4(1), int4(2), 2);
SELECT WHERE 1 = ANY (ARRAY[1, int4(1), int4(2), 2]);
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- external parameters will be squashed
--
-- 外部参数将被压缩
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM test_squash WHERE id IN ($1, $2, $3, $4, $5)  \bind 1 2 3 4 5
;
SELECT * FROM test_squash WHERE id::text = ANY(ARRAY[$1, $2, $3, $4, $5]) \bind 1 2 3 4 5
;
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- prepared statements will also be squashed
--
-- 准备好的语句也将被压缩
-- the IN and ARRAY forms of this statement will have the same queryId
--
-- 该语句的 IN 和 ARRAY 形式将具有相同的 queryId
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
PREPARE p1(int, int, int, int, int) AS
SELECT * FROM test_squash WHERE id IN ($1, $2, $3, $4, $5);
EXECUTE p1(1, 2, 3, 4, 5);
DEALLOCATE p1;
PREPARE p1(int, int, int, int, int) AS
SELECT * FROM test_squash WHERE id = ANY(ARRAY[$1, $2, $3, $4, $5]);
EXECUTE p1(1, 2, 3, 4, 5);
DEALLOCATE p1;
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- More conditions in the query
--
-- 查询更多条件
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM test_squash WHERE id IN (1, 2, 3, 4, 5, 6, 7, 8, 9) AND data = 2;
SELECT * FROM test_squash WHERE id IN (1, 2, 3, 4, 5, 6, 7, 8, 9, 10) AND data = 2;
SELECT * FROM test_squash WHERE id IN (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11) AND data = 2;
SELECT * FROM test_squash WHERE id = ANY (ARRAY[1, 2, 3, 4, 5, 6, 7, 8, 9]) AND data = 2;
SELECT * FROM test_squash WHERE id = ANY (ARRAY[1, 2, 3, 4, 5, 6, 7, 8, 9, 10]) AND data = 2;
SELECT * FROM test_squash WHERE id = ANY (ARRAY[1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]) AND data = 2;
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- Multiple squashed intervals
--
-- 多个压缩间隔
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM test_squash WHERE id IN (1, 2, 3, 4, 5, 6, 7, 8, 9)
    AND data IN (1, 2, 3, 4, 5, 6, 7, 8, 9);
SELECT * FROM test_squash WHERE id IN (1, 2, 3, 4, 5, 6, 7, 8, 9, 10)
    AND data IN (1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
SELECT * FROM test_squash WHERE id IN (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11)
    AND data IN (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11);
SELECT * FROM test_squash WHERE id = ANY (ARRAY[1, 2, 3, 4, 5, 6, 7, 8, 9])
    AND data = ANY (ARRAY[1, 2, 3, 4, 5, 6, 7, 8, 9]);
SELECT * FROM test_squash WHERE id = ANY (ARRAY[1, 2, 3, 4, 5, 6, 7, 8, 9, 10])
    AND data = ANY (ARRAY[1, 2, 3, 4, 5, 6, 7, 8, 9, 10]);
SELECT * FROM test_squash WHERE id = ANY (ARRAY[1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11])
    AND data = ANY (ARRAY[1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]);
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- No constants squashing for OpExpr
--
-- OpExpr 没有常量压缩
-- The IN and ARRAY forms of this statement will have the same queryId
--
-- 该语句的 IN 和 ARRAY 形式将具有相同的 queryId
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM test_squash WHERE id IN
	(1 + 1, 2 + 2, 3 + 3, 4 + 4, 5 + 5, 6 + 6, 7 + 7, 8 + 8, 9 + 9);
SELECT * FROM test_squash WHERE id IN
	(@ '-1', @ '-2', @ '-3', @ '-4', @ '-5', @ '-6', @ '-7', @ '-8', @ '-9');
SELECT * FROM test_squash WHERE id = ANY(ARRAY
	[1 + 1, 2 + 2, 3 + 3, 4 + 4, 5 + 5, 6 + 6, 7 + 7, 8 + 8, 9 + 9]);
SELECT * FROM test_squash WHERE id = ANY(ARRAY
	[@ '-1', @ '-2', @ '-3', @ '-4', @ '-5', @ '-6', @ '-7', @ '-8', @ '-9']);
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

--
-- FuncExpr
--

-- Verify multiple type representation end up with the same query_id
--
-- 验证多种类型表示以相同的 query_id 结束
CREATE TABLE test_float (data float);
-- The casted ARRAY expressions will have the same queryId as the IN clause
--
-- 转换后的 ARRAY 表达式将具有与 IN 子句相同的 queryId
-- form of the query
--
-- 查询的形式
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT data FROM test_float WHERE data IN (1, 2);
SELECT data FROM test_float WHERE data IN (1, '2');
SELECT data FROM test_float WHERE data IN ('1', 2);
SELECT data FROM test_float WHERE data IN ('1', '2');
SELECT data FROM test_float WHERE data IN (1.0, 1.0);
SELECT data FROM test_float WHERE data = ANY(ARRAY['1'::double precision, '2'::double precision]);
SELECT data FROM test_float WHERE data = ANY(ARRAY[1.0::double precision, 1.0::double precision]);
SELECT data FROM test_float WHERE data = ANY(ARRAY[1, 2]);
SELECT data FROM test_float WHERE data = ANY(ARRAY[1, '2']);
SELECT data FROM test_float WHERE data = ANY(ARRAY['1', 2]);
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- Numeric type, implicit cast is squashed
--
-- 数字类型，隐式强制转换被压缩
CREATE TABLE test_squash_numeric (id int, data numeric(5, 2));
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM test_squash_numeric WHERE data IN (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11);
SELECT * FROM test_squash_numeric WHERE data = ANY(ARRAY[1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]);
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- Bigint, implicit cast is squashed
--
-- Bigint，隐式转换被压缩
CREATE TABLE test_squash_bigint (id int, data bigint);
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM test_squash_bigint WHERE data IN (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11);
SELECT * FROM test_squash_bigint WHERE data = ANY(ARRAY[1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]);
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- Bigint, explicit cast is squashed
--
-- Bigint，显式强制转换被压缩
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM test_squash_bigint WHERE data IN
	(1::bigint, 2::bigint, 3::bigint, 4::bigint, 5::bigint, 6::bigint,
	 7::bigint, 8::bigint, 9::bigint, 10::bigint, 11::bigint);
SELECT * FROM test_squash_bigint WHERE data = ANY(ARRAY[
	 1::bigint, 2::bigint, 3::bigint, 4::bigint, 5::bigint, 6::bigint,
	 7::bigint, 8::bigint, 9::bigint, 10::bigint, 11::bigint]);
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- Bigint, long tokens with parenthesis, will not squash
--
-- Bigint，带括号的长标记，不会挤压
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM test_squash_bigint WHERE id IN
	(abs(100), abs(200), abs(300), abs(400), abs(500), abs(600), abs(700),
	 abs(800), abs(900), abs(1000), ((abs(1100))));
SELECT * FROM test_squash_bigint WHERE id = ANY(ARRAY[
	 abs(100), abs(200), abs(300), abs(400), abs(500), abs(600), abs(700),
	 abs(800), abs(900), abs(1000), ((abs(1100)))]);
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- Multiple FuncExpr's. Will not squash
--
-- 多个 FuncExpr。不会压扁
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT WHERE 1 IN (1::int::bigint::int, 2::int::bigint::int);
SELECT WHERE 1 = ANY(ARRAY[1::int::bigint::int, 2::int::bigint::int]);
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

--
-- CoerceViaIO
--

-- Create some dummy type to force CoerceViaIO
--
-- 创建一些虚拟类型来强制 CoerceViaIO
CREATE TYPE casttesttype;

CREATE FUNCTION casttesttype_in(cstring)
   RETURNS casttesttype
   AS 'textin'
   LANGUAGE internal STRICT IMMUTABLE;

CREATE FUNCTION casttesttype_out(casttesttype)
   RETURNS cstring
   AS 'textout'
   LANGUAGE internal STRICT IMMUTABLE;

CREATE TYPE casttesttype (
   internallength = variable,
   input = casttesttype_in,
   output = casttesttype_out,
   alignment = int4
);

CREATE CAST (int4 AS casttesttype) WITH INOUT;

CREATE FUNCTION casttesttype_eq(casttesttype, casttesttype)
returns boolean language sql immutable as $$
    SELECT true
$$;

CREATE OPERATOR = (
    leftarg = casttesttype,
    rightarg = casttesttype,
    procedure = casttesttype_eq,
    commutator = =);

CREATE TABLE test_squash_cast (id int, data casttesttype);

-- Use the introduced type to construct a list of CoerceViaIO around Const
--
-- 使用引入的类型围绕Const构造CoerceViaIO的列表
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM test_squash_cast WHERE data IN
	(1::int4::casttesttype, 2::int4::casttesttype, 3::int4::casttesttype,
	 4::int4::casttesttype, 5::int4::casttesttype, 6::int4::casttesttype,
	 7::int4::casttesttype, 8::int4::casttesttype, 9::int4::casttesttype,
	 10::int4::casttesttype, 11::int4::casttesttype);
SELECT * FROM test_squash_cast WHERE data = ANY (ARRAY
	[1::int4::casttesttype, 2::int4::casttesttype, 3::int4::casttesttype,
	 4::int4::casttesttype, 5::int4::casttesttype, 6::int4::casttesttype,
	 7::int4::casttesttype, 8::int4::casttesttype, 9::int4::casttesttype,
	 10::int4::casttesttype, 11::int4::casttesttype]);
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- Some casting expression are simplified to Const
--
-- 一些转换表达式被简化为 Const
CREATE TABLE test_squash_jsonb (id int, data jsonb);
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM test_squash_jsonb WHERE data IN
	(('"1"')::jsonb, ('"2"')::jsonb, ('"3"')::jsonb, ('"4"')::jsonb,
	 ('"5"')::jsonb, ('"6"')::jsonb, ('"7"')::jsonb, ('"8"')::jsonb,
	 ('"9"')::jsonb, ('"10"')::jsonb);
SELECT * FROM test_squash_jsonb WHERE data = ANY (ARRAY
	[('"1"')::jsonb, ('"2"')::jsonb, ('"3"')::jsonb, ('"4"')::jsonb,
	 ('"5"')::jsonb, ('"6"')::jsonb, ('"7"')::jsonb, ('"8"')::jsonb,
	 ('"9"')::jsonb, ('"10"')::jsonb]);
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- CoerceViaIO, SubLink instead of a Const. Will not squash
--
-- CoerceViaIO，SubLink 而不是 Const。不会压扁
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM test_squash_jsonb WHERE data IN
	((SELECT '"1"')::jsonb, (SELECT '"2"')::jsonb, (SELECT '"3"')::jsonb,
	 (SELECT '"4"')::jsonb, (SELECT '"5"')::jsonb, (SELECT '"6"')::jsonb,
	 (SELECT '"7"')::jsonb, (SELECT '"8"')::jsonb, (SELECT '"9"')::jsonb,
	 (SELECT '"10"')::jsonb);
SELECT * FROM test_squash_jsonb WHERE data = ANY(ARRAY
	[(SELECT '"1"')::jsonb, (SELECT '"2"')::jsonb, (SELECT '"3"')::jsonb,
	 (SELECT '"4"')::jsonb, (SELECT '"5"')::jsonb, (SELECT '"6"')::jsonb,
	 (SELECT '"7"')::jsonb, (SELECT '"8"')::jsonb, (SELECT '"9"')::jsonb,
	 (SELECT '"10"')::jsonb]);
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- Multiple CoerceViaIO are squashed
--
-- 多个 CoerceViaIO 被压扁
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT WHERE 1 IN (1::text::int::text::int, 1::text::int::text::int);
SELECT WHERE 1 = ANY(ARRAY[1::text::int::text::int, 1::text::int::text::int]);
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

--
-- RelabelType
--

SELECT pg_stat_statements_reset() IS NOT NULL AS t;
-- However many layers of RelabelType there are, the list will be squashable.
--
-- 无论 RelabelType 有多少层，该列表都将是可压缩的。
SELECT * FROM test_squash WHERE id IN
	(1::oid, 2::oid, 3::oid, 4::oid, 5::oid, 6::oid, 7::oid, 8::oid, 9::oid);
SELECT ARRAY[1::oid, 2::oid, 3::oid, 4::oid, 5::oid, 6::oid, 7::oid, 8::oid, 9::oid];
SELECT * FROM test_squash WHERE id IN (1::oid, 2::oid::int::oid);
SELECT * FROM test_squash WHERE id = ANY(ARRAY[1::oid, 2::oid::int::oid]);
-- RelabelType together with CoerceViaIO is also squashable
--
-- RelabelType 与 CoerceViaIO 一起也是可压缩的
SELECT * FROM test_squash WHERE id = ANY(ARRAY[1::oid::text::int::oid, 2::oid::int::oid]);
SELECT * FROM test_squash WHERE id = ANY(ARRAY[1::text::int::oid, 2::oid::int::oid]);
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

--
-- edge cases
--
-- 边缘情况
--

SELECT pg_stat_statements_reset() IS NOT NULL AS t;
-- for nested arrays, only constants are squashed
--
-- 对于嵌套数组，只有常量会被压缩
SELECT ARRAY[
    ARRAY[1, 2, 3, 4, 5, 6, 7, 8, 9, 10],
    ARRAY[1, 2, 3, 4, 5, 6, 7, 8, 9, 10],
    ARRAY[1, 2, 3, 4, 5, 6, 7, 8, 9, 10],
    ARRAY[1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
    ];
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- Test constants evaluation in a CTE, which was causing issues in the past
--
-- CTE 中的测试常数评估，这在过去引起了问题
WITH cte AS (
    SELECT 'const' as const FROM test_squash
)
SELECT ARRAY['a', 'b', 'c', const::varchar] AS result
FROM cte;

SELECT pg_stat_statements_reset() IS NOT NULL AS t;
-- Rewritten as an OpExpr, so it will not be squashed
--
-- 重写为 OpExpr，因此不会被压缩
select where '1' IN ('1'::int, '2'::int::text);
-- Rewritten as an ArrayExpr, so it will be squashed
--
-- 重写为 ArrayExpr，因此它将被压缩
select where '1' IN ('1'::int, '2'::int);
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

SELECT pg_stat_statements_reset() IS NOT NULL AS t;
-- Both of these queries will be rewritten as an ArrayExpr, so they
--
-- 这两个查询都将被重写为 ArrayExpr，因此它们
-- will be squashed, and have a similar queryId
--
-- 会被压扁，并且有相似的queryId
select where '1' IN ('1'::int::text, '2'::int::text);
select where '1' = ANY (array['1'::int::text, '2'::int::text]);
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- composite function with row expansion
--
-- 具有行扩展的复合函数
create table test_composite(x integer);
CREATE FUNCTION composite_f(a integer[], out x integer, out y integer) returns
record as $$            begin
        x = a[1];
        y = a[2];
    end;
$$ language plpgsql;
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT ((composite_f(array[1, 2]))).* FROM test_composite;
SELECT ((composite_f(array[1, 2, 3]))).* FROM test_composite;
SELECT ((composite_f(array[1, 2, 3]))).*, 1, 2, 3, ((composite_f(array[1, 2, 3]))).*, 1, 2
FROM test_composite
WHERE x IN (1, 2, 3);
SELECT ((composite_f(array[1, $1, 3]))).*, 1 FROM test_composite \bind 1
;
-- ROW() expression with row expansion
--
-- 具有行扩展的 ROW() 表达式
SELECT (ROW(ARRAY[1,2])).*;
SELECT (ROW(ARRAY[1, 2], ARRAY[1, 2, 3])).*;
SELECT 1, 2, (ROW(ARRAY[1, 2], ARRAY[1, 2, 3])).*, 3, 4;
SELECT (ROW(ARRAY[1, 2], ARRAY[1, $1, 3])).*, 1 \bind 1
;

-- IN and ANY clauses with Vars are not squashed.
--
-- 带有 Var 的 IN 和 ANY 子句不会被压缩。
SELECT * FROM test_squash a, test_squash b WHERE a.id IN (1, 2, 3, b.id, b.id + 1);
SELECT * FROM test_squash a, test_squash b WHERE a.id = ANY (array[1, ((b.id + b.id * 2)), 5]);
SELECT * FROM test_squash a, test_squash b WHERE a.id IN ($1, $2, $3, b.id, b.id + $4) \bind 1 2 3 1
;
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

--
-- cleanup
--
DROP TABLE test_squash;
DROP TABLE test_float;
DROP TABLE test_squash_numeric;
DROP TABLE test_squash_bigint;
DROP TABLE test_squash_cast CASCADE;
DROP TABLE test_squash_jsonb;
DROP TABLE test_composite;
DROP FUNCTION composite_f;

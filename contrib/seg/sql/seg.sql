--
--  Test seg datatype
--
-- 测试段数据类型
--

CREATE EXTENSION seg;

-- Check whether any of our opclasses fail amvalidate
--
-- 检查我们的任何 opclass 是否未通过 amvalidate
SELECT amname, opcname
FROM pg_opclass opc LEFT JOIN pg_am am ON am.oid = opcmethod
WHERE opc.oid >= 16384 AND NOT amvalidate(opc.oid);

--
-- testing the input and output functions
--
-- 测试输入和输出功能
--

-- Any number
--
-- 任意号码
SELECT '1'::seg AS seg;
SELECT '-1'::seg AS seg;
SELECT '1.0'::seg AS seg;
SELECT '-1.0'::seg AS seg;
SELECT '1e7'::seg AS seg;
SELECT '-1e7'::seg AS seg;
SELECT '1.0e7'::seg AS seg;
SELECT '-1.0e7'::seg AS seg;
SELECT '1e+7'::seg AS seg;
SELECT '-1e+7'::seg AS seg;
SELECT '1.0e+7'::seg AS seg;
SELECT '-1.0e+7'::seg AS seg;
SELECT '1e-7'::seg AS seg;
SELECT '-1e-7'::seg AS seg;
SELECT '1.0e-7'::seg AS seg;
SELECT '-1.0e-7'::seg AS seg;
SELECT '2e-6'::seg AS seg;
SELECT '2e-5'::seg AS seg;
SELECT '2e-4'::seg AS seg;
SELECT '2e-3'::seg AS seg;
SELECT '2e-2'::seg AS seg;
SELECT '2e-1'::seg AS seg;
SELECT '2e-0'::seg AS seg;
SELECT '2e+0'::seg AS seg;
SELECT '2e+1'::seg AS seg;
SELECT '2e+2'::seg AS seg;
SELECT '2e+3'::seg AS seg;
SELECT '2e+4'::seg AS seg;
SELECT '2e+5'::seg AS seg;
SELECT '2e+6'::seg AS seg;


-- Significant digits preserved
--
-- 保留有效数字
SELECT '1'::seg AS seg;
SELECT '1.0'::seg AS seg;
SELECT '1.00'::seg AS seg;
SELECT '1.000'::seg AS seg;
SELECT '1.0000'::seg AS seg;
SELECT '1.00000'::seg AS seg;
SELECT '1.000000'::seg AS seg;
SELECT '0.000000120'::seg AS seg;
SELECT '3.400e5'::seg AS seg;

-- Digits truncated
--
-- 数字被截断
SELECT '12.34567890123456'::seg AS seg;

-- Same, with a very long input
--
-- 相同，输入很长
SELECT '12.3456789012345600000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000'::seg AS seg;

-- Numbers with certainty indicators
--
-- 具有确定性指标的数字
SELECT '~6.5'::seg AS seg;
SELECT '<6.5'::seg AS seg;
SELECT '>6.5'::seg AS seg;
SELECT '~ 6.5'::seg AS seg;
SELECT '< 6.5'::seg AS seg;
SELECT '> 6.5'::seg AS seg;

-- Open intervals
--
-- 开放区间
SELECT '0..'::seg AS seg;
SELECT '0...'::seg AS seg;
SELECT '0 ..'::seg AS seg;
SELECT '0 ...'::seg AS seg;
SELECT '..0'::seg AS seg;
SELECT '...0'::seg AS seg;
SELECT '.. 0'::seg AS seg;
SELECT '... 0'::seg AS seg;

-- Finite intervals
--
-- 有限间隔
SELECT '0 .. 1'::seg AS seg;
SELECT '-1 .. 0'::seg AS seg;
SELECT '-1 .. 1'::seg AS seg;

-- (+/-) intervals
--
-- (+/-) 间隔
SELECT '0(+-)1'::seg AS seg;
SELECT '0(+-)1.0'::seg AS seg;
SELECT '1.0(+-)0.005'::seg AS seg;
SELECT '101(+-)1'::seg AS seg;
-- incorrect number of significant digits in 99.0:
--
-- 99.0 中的有效位数不正确：
SELECT '100(+-)1'::seg AS seg;

-- invalid input
--
-- 无效输入
SELECT ''::seg AS seg;
SELECT 'ABC'::seg AS seg;
SELECT '1ABC'::seg AS seg;
SELECT '1.'::seg AS seg;
SELECT '1.....'::seg AS seg;
SELECT '.1'::seg AS seg;
SELECT '1..2.'::seg AS seg;
SELECT '1 e7'::seg AS seg;
SELECT '1e700'::seg AS seg;

--
-- testing the  operators
--
-- 测试操作员
--

-- equality/inequality:
--
SELECT '24 .. 33.20'::seg = '24 .. 33.20'::seg AS bool;
SELECT '24 .. 33.20'::seg = '24 .. 33.21'::seg AS bool;
SELECT '24 .. 33.20'::seg != '24 .. 33.20'::seg AS bool;
SELECT '24 .. 33.20'::seg != '24 .. 33.21'::seg AS bool;

-- overlap
--
SELECT '1'::seg && '1'::seg AS bool;
SELECT '1'::seg && '2'::seg AS bool;
SELECT '0 ..'::seg && '0 ..'::seg AS bool;
SELECT '0 .. 1'::seg && '0 .. 1'::seg AS bool;
SELECT '..0'::seg && '0..'::seg AS bool;
SELECT '-1 .. 0.1'::seg && '0 .. 1'::seg AS bool;
SELECT '-1 .. 0'::seg && '0 .. 1'::seg AS bool;
SELECT '-1 .. -0.0001'::seg && '0 .. 1'::seg AS bool;
SELECT '0 ..'::seg && '1'::seg AS bool;
SELECT '0 .. 1'::seg && '1'::seg AS bool;
SELECT '0 .. 1'::seg && '2'::seg AS bool;
SELECT '0 .. 2'::seg && '1'::seg AS bool;
SELECT '1'::seg && '0 .. 1'::seg AS bool;
SELECT '2'::seg && '0 .. 1'::seg AS bool;
SELECT '1'::seg && '0 .. 2'::seg AS bool;

-- overlap on the left
--
-- 与左侧重叠
--
SELECT '1'::seg &< '0'::seg AS bool;
SELECT '1'::seg &< '1'::seg AS bool;
SELECT '1'::seg &< '2'::seg AS bool;
SELECT '0 .. 1'::seg &< '0'::seg AS bool;
SELECT '0 .. 1'::seg &< '1'::seg AS bool;
SELECT '0 .. 1'::seg &< '2'::seg AS bool;
SELECT '0 .. 1'::seg &< '0 .. 0.5'::seg AS bool;
SELECT '0 .. 1'::seg &< '0 .. 1'::seg AS bool;
SELECT '0 .. 1'::seg &< '0 .. 2'::seg AS bool;
SELECT '0 .. 1'::seg &< '1 .. 2'::seg AS bool;
SELECT '0 .. 1'::seg &< '2 .. 3'::seg AS bool;

-- overlap on the right
--
-- 与右侧重叠
--
SELECT '0'::seg &> '1'::seg AS bool;
SELECT '1'::seg &> '1'::seg AS bool;
SELECT '2'::seg &> '1'::seg AS bool;
SELECT '0'::seg &> '0 .. 1'::seg AS bool;
SELECT '1'::seg &> '0 .. 1'::seg AS bool;
SELECT '2'::seg &> '0 .. 1'::seg AS bool;
SELECT '0 .. 0.5'::seg &> '0 .. 1'::seg AS bool;
SELECT '0 .. 1'::seg &> '0 .. 1'::seg AS bool;
SELECT '0 .. 2'::seg &> '0 .. 2'::seg AS bool;
SELECT '1 .. 2'::seg &> '0 .. 1'::seg AS bool;
SELECT '2 .. 3'::seg &> '0 .. 1'::seg AS bool;

-- left
--
SELECT '1'::seg << '0'::seg AS bool;
SELECT '1'::seg << '1'::seg AS bool;
SELECT '1'::seg << '2'::seg AS bool;
SELECT '0 .. 1'::seg << '0'::seg AS bool;
SELECT '0 .. 1'::seg << '1'::seg AS bool;
SELECT '0 .. 1'::seg << '2'::seg AS bool;
SELECT '0 .. 1'::seg << '0 .. 0.5'::seg AS bool;
SELECT '0 .. 1'::seg << '0 .. 1'::seg AS bool;
SELECT '0 .. 1'::seg << '0 .. 2'::seg AS bool;
SELECT '0 .. 1'::seg << '1 .. 2'::seg AS bool;
SELECT '0 .. 1'::seg << '2 .. 3'::seg AS bool;

-- right
--
SELECT '0'::seg >> '1'::seg AS bool;
SELECT '1'::seg >> '1'::seg AS bool;
SELECT '2'::seg >> '1'::seg AS bool;
SELECT '0'::seg >> '0 .. 1'::seg AS bool;
SELECT '1'::seg >> '0 .. 1'::seg AS bool;
SELECT '2'::seg >> '0 .. 1'::seg AS bool;
SELECT '0 .. 0.5'::seg >> '0 .. 1'::seg AS bool;
SELECT '0 .. 1'::seg >> '0 .. 1'::seg AS bool;
SELECT '0 .. 2'::seg >> '0 .. 2'::seg AS bool;
SELECT '1 .. 2'::seg >> '0 .. 1'::seg AS bool;
SELECT '2 .. 3'::seg >> '0 .. 1'::seg AS bool;


-- "contained in" (the left value belongs within the interval specified in the right value):
--
-- “包含在”（左值属于右值指定的区间内）：
--
SELECT '0'::seg        <@ '0'::seg AS bool;
SELECT '0'::seg        <@ '0 ..'::seg AS bool;
SELECT '0'::seg        <@ '.. 0'::seg AS bool;
SELECT '0'::seg        <@ '-1 .. 1'::seg AS bool;
SELECT '0'::seg        <@ '-1 .. 1'::seg AS bool;
SELECT '-1'::seg       <@ '-1 .. 1'::seg AS bool;
SELECT '1'::seg        <@ '-1 .. 1'::seg AS bool;
SELECT '-1 .. 1'::seg  <@ '-1 .. 1'::seg AS bool;

-- "contains" (the left value contains the interval specified in the right value):
--
-- “包含”（左侧值包含右侧值中指定的间隔）：
--
SELECT '0'::seg @> '0'::seg AS bool;
SELECT '0 .. '::seg <@ '0'::seg AS bool;
SELECT '.. 0'::seg <@ '0'::seg AS bool;
SELECT '-1 .. 1'::seg <@ '0'::seg AS bool;
SELECT '0'::seg <@ '-1 .. 1'::seg AS bool;
SELECT '-1'::seg <@ '-1 .. 1'::seg AS bool;
SELECT '1'::seg <@ '-1 .. 1'::seg AS bool;

-- Load some example data and build the index
--
-- 加载一些示例数据并构建索引
--
CREATE TABLE test_seg (s seg);

\copy test_seg from 'data/test_seg.data'

CREATE INDEX test_seg_ix ON test_seg USING gist (s);

SET enable_indexscan = false;
EXPLAIN (COSTS OFF)
SELECT count(*) FROM test_seg WHERE s @> '11..11.3';
SELECT count(*) FROM test_seg WHERE s @> '11..11.3';
RESET enable_indexscan;

SET enable_bitmapscan = false;
EXPLAIN (COSTS OFF)
SELECT count(*) FROM test_seg WHERE s @> '11..11.3';
SELECT count(*) FROM test_seg WHERE s @> '11..11.3';
RESET enable_bitmapscan;

-- Test sorting
--
-- 测试排序
SELECT * FROM test_seg WHERE s @> '11..11.3' GROUP BY s;

-- Test functions
--
-- 测试功能
SELECT seg_lower(s), seg_center(s), seg_upper(s)
FROM test_seg WHERE s @> '11.2..11.3' OR s IS NULL ORDER BY s;


-- test non error throwing API
--
-- 测试非错误抛出 API

SELECT str as seg,
       pg_input_is_valid(str,'seg') as ok,
       errinfo.sql_error_code,
       errinfo.message,
       errinfo.detail,
       errinfo.hint
FROM unnest(ARRAY['-1 .. 1'::text,
                  '100(+-)1',
                  '',
                  'ABC',
                  '1 e7',
                  '1e700']) str,
     LATERAL pg_input_error_info(str, 'seg') as errinfo;

--
--  Test cube datatype
--
-- 测试多维数据集数据类型
--

CREATE EXTENSION cube;

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

-- Any number (a one-dimensional point)
--
-- 任意数（一维点）
SELECT '1'::cube AS cube;
SELECT '-1'::cube AS cube;
SELECT '1.'::cube AS cube;
SELECT '-1.'::cube AS cube;
SELECT '.1'::cube AS cube;
SELECT '-.1'::cube AS cube;
SELECT '1.0'::cube AS cube;
SELECT '-1.0'::cube AS cube;
SELECT 'infinity'::cube AS cube;
SELECT '-infinity'::cube AS cube;
SELECT 'NaN'::cube AS cube;
SELECT '.1234567890123456'::cube AS cube;
SELECT '+.1234567890123456'::cube AS cube;
SELECT '-.1234567890123456'::cube AS cube;

-- simple lists (points)
--
-- 简单列表（点）
SELECT '()'::cube AS cube;
SELECT '1,2'::cube AS cube;
SELECT '(1,2)'::cube AS cube;
SELECT '1,2,3,4,5'::cube AS cube;
SELECT '(1,2,3,4,5)'::cube AS cube;

-- double lists (cubes)
--
-- 双列表（立方体）
SELECT '(),()'::cube AS cube;
SELECT '(0),(0)'::cube AS cube;
SELECT '(0),(1)'::cube AS cube;
SELECT '[(0),(0)]'::cube AS cube;
SELECT '[(0),(1)]'::cube AS cube;
SELECT '(0,0,0,0),(0,0,0,0)'::cube AS cube;
SELECT '(0,0,0,0),(1,0,0,0)'::cube AS cube;
SELECT '[(0,0,0,0),(0,0,0,0)]'::cube AS cube;
SELECT '[(0,0,0,0),(1,0,0,0)]'::cube AS cube;

-- invalid input: parse errors
--
-- 无效输入：解析错误
SELECT ''::cube AS cube;
SELECT 'ABC'::cube AS cube;
SELECT '[]'::cube AS cube;
SELECT '[()]'::cube AS cube;
SELECT '[(1)]'::cube AS cube;
SELECT '[(1),]'::cube AS cube;
SELECT '[(1),2]'::cube AS cube;
SELECT '[(1),(2),(3)]'::cube AS cube;
SELECT '1,'::cube AS cube;
SELECT '1,2,'::cube AS cube;
SELECT '1,,2'::cube AS cube;
SELECT '(1,)'::cube AS cube;
SELECT '(1,2,)'::cube AS cube;
SELECT '(1,,2)'::cube AS cube;

-- invalid input: semantic errors and trailing garbage
--
-- 无效输入：语义错误和尾随垃圾
SELECT '[(1),(2)],'::cube AS cube; -- 0
SELECT '[(1,2,3),(2,3)]'::cube AS cube; -- 1
SELECT '[(1,2),(1,2,3)]'::cube AS cube; -- 1
SELECT '(1),(2),'::cube AS cube; -- 2
SELECT '(1,2,3),(2,3)'::cube AS cube; -- 3
SELECT '(1,2),(1,2,3)'::cube AS cube; -- 3
SELECT '(1,2,3)ab'::cube AS cube; -- 4
SELECT '(1,2,3)a'::cube AS cube; -- 5
SELECT '(1,2)('::cube AS cube; -- 5
SELECT '1,2ab'::cube AS cube; -- 6
SELECT '1 e7'::cube AS cube; -- 6
SELECT '1,2a'::cube AS cube; -- 7
SELECT '1..2'::cube AS cube; -- 7
SELECT '-1e-700'::cube AS cube; -- out of range

-- Also try it with non-error-throwing API
--
-- 也可以尝试使用非错误抛出 API
SELECT pg_input_is_valid('(1,2)', 'cube');
SELECT pg_input_is_valid('[(1),]', 'cube');
SELECT pg_input_is_valid('-1e-700', 'cube');
SELECT * FROM pg_input_error_info('-1e-700', 'cube');

--
-- Testing building cubes from float8 values
--
-- 测试从 float8 值构建立方体
--

SELECT cube(0::float8);
SELECT cube(1::float8);
SELECT cube(1,2);
SELECT cube(cube(1,2),3);
SELECT cube(cube(1,2),3,4);
SELECT cube(cube(cube(1,2),3,4),5);
SELECT cube(cube(cube(1,2),3,4),5,6);

--
-- Test that the text -> cube cast was installed.
--
-- 测试文本 -> 立方体演员是否已安装。
--

SELECT '(0)'::text::cube;

--
-- Test the float[] -> cube cast
--
-- 测试 float[] -> 立方体转换
--
SELECT cube('{0,1,2}'::float[], '{3,4,5}'::float[]);
SELECT cube('{0,1,2}'::float[], '{3}'::float[]);
SELECT cube(NULL::float[], '{3}'::float[]);
SELECT cube('{0,1,2}'::float[]);
SELECT cube_subset(cube('(1,3,5),(6,7,8)'), ARRAY[3,2,1,1]);
SELECT cube_subset(cube('(1,3,5),(1,3,5)'), ARRAY[3,2,1,1]);
SELECT cube_subset(cube('(1,3,5),(6,7,8)'), ARRAY[4,0]);
SELECT cube_subset(cube('(6,7,8),(6,7,8)'), ARRAY[4,0]);
-- test for limits: this should pass
--
-- 测试限制：这应该通过
SELECT cube_subset(cube('(6,7,8),(6,7,8)'), array(SELECT 1 as a FROM generate_series(1,100)));
-- and this should fail
--
-- 这应该会失败
SELECT cube_subset(cube('(6,7,8),(6,7,8)'), array(SELECT 1 as a FROM generate_series(1,101)));



--
-- Test point processing
--
-- 测试点处理
--
SELECT cube('(1,2),(1,2)'); -- cube_in
SELECT cube('{0,1,2}'::float[], '{0,1,2}'::float[]); -- cube_a_f8_f8
SELECT cube('{5,6,7,8}'::float[]); -- cube_a_f8
SELECT cube(1.37); -- cube_f8
SELECT cube(1.37, 1.37); -- cube_f8_f8
SELECT cube(cube(1,1), 42); -- cube_c_f8
SELECT cube(cube(1,2), 42); -- cube_c_f8
SELECT cube(cube(1,1), 42, 42); -- cube_c_f8_f8
SELECT cube(cube(1,1), 42, 24); -- cube_c_f8_f8
SELECT cube(cube(1,2), 42, 42); -- cube_c_f8_f8
SELECT cube(cube(1,2), 42, 24); -- cube_c_f8_f8

--
-- Testing limit of CUBE_MAX_DIM dimensions check in cube_in.
--
-- 在cube_in 中检查CUBE_MAX_DIM 维度的测试限制。
--
-- create too big cube from literal
--
-- 从文字创建太大的立方体
select '(0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0)'::cube;
select '(0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0),(0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0)'::cube;
-- from an array
--
-- 来自数组
select cube(array(SELECT 0 as a FROM generate_series(1,101)));
select cube(array(SELECT 0 as a FROM generate_series(1,101)),array(SELECT 0 as a FROM generate_series(1,101)));

-- extend cube beyond limit
--
-- 将立方体延伸到极限之外
-- this should work
--
-- 这应该有效
select cube(array(SELECT 0 as a FROM generate_series(1,100)));
select cube(array(SELECT 0 as a FROM generate_series(1,100)),array(SELECT 0 as a FROM generate_series(1,100)));
-- this should fail
--
-- 这应该会失败
select cube(cube(array(SELECT 0 as a FROM generate_series(1,100))), 0);
select cube(cube(array(SELECT 0 as a FROM generate_series(1,100)),array(SELECT 0 as a FROM generate_series(1,100))), 0, 0);


--
-- testing the  operators
--
-- 测试操作员
--

-- equality/inequality:
--
SELECT '24, 33.20'::cube    =  '24, 33.20'::cube AS bool;
SELECT '24, 33.20'::cube    != '24, 33.20'::cube AS bool;
SELECT '24, 33.20'::cube    =  '24, 33.21'::cube AS bool;
SELECT '24, 33.20'::cube    != '24, 33.21'::cube AS bool;
SELECT '(2,0),(3,1)'::cube  =  '(2,0,0,0,0),(3,1,0,0,0)'::cube AS bool;
SELECT '(2,0),(3,1)'::cube  =  '(2,0,0,0,0),(3,1,0,0,1)'::cube AS bool;

-- "lower than" / "greater than"
--
-- “低于”/“大于”
-- (these operators are not useful for anything but ordering)
--
-- （这些运算符除了排序之外没有任何用处）
--
SELECT '1'::cube   > '2'::cube AS bool;
SELECT '1'::cube   < '2'::cube AS bool;
SELECT '1,1'::cube > '1,2'::cube AS bool;
SELECT '1,1'::cube < '1,2'::cube AS bool;

SELECT '(2,0),(3,1)'::cube             > '(2,0,0,0,0),(3,1,0,0,1)'::cube AS bool;
SELECT '(2,0),(3,1)'::cube             < '(2,0,0,0,0),(3,1,0,0,1)'::cube AS bool;
SELECT '(2,0),(3,1)'::cube             > '(2,0,0,0,1),(3,1,0,0,0)'::cube AS bool;
SELECT '(2,0),(3,1)'::cube             < '(2,0,0,0,1),(3,1,0,0,0)'::cube AS bool;
SELECT '(2,0),(3,1)'::cube             > '(2,0,0,0,0),(3,1,0,0,0)'::cube AS bool;
SELECT '(2,0),(3,1)'::cube             < '(2,0,0,0,0),(3,1,0,0,0)'::cube AS bool;
SELECT '(2,0,0,0,0),(3,1,0,0,1)'::cube > '(2,0),(3,1)'::cube AS bool;
SELECT '(2,0,0,0,0),(3,1,0,0,1)'::cube < '(2,0),(3,1)'::cube AS bool;
SELECT '(2,0,0,0,1),(3,1,0,0,0)'::cube > '(2,0),(3,1)'::cube AS bool;
SELECT '(2,0,0,0,1),(3,1,0,0,0)'::cube < '(2,0),(3,1)'::cube AS bool;
SELECT '(2,0,0,0,0),(3,1,0,0,0)'::cube > '(2,0),(3,1)'::cube AS bool;
SELECT '(2,0,0,0,0),(3,1,0,0,0)'::cube < '(2,0),(3,1)'::cube AS bool;


-- "overlap"
--
-- “重叠”
--
SELECT '1'::cube && '1'::cube AS bool;
SELECT '1'::cube && '2'::cube AS bool;

SELECT '[(-1,-1,-1),(1,1,1)]'::cube && '0'::cube AS bool;
SELECT '[(-1,-1,-1),(1,1,1)]'::cube && '1'::cube AS bool;
SELECT '[(-1,-1,-1),(1,1,1)]'::cube && '1,1,1'::cube AS bool;
SELECT '[(-1,-1,-1),(1,1,1)]'::cube && '[(1,1,1),(2,2,2)]'::cube AS bool;
SELECT '[(-1,-1,-1),(1,1,1)]'::cube && '[(1,1),(2,2)]'::cube AS bool;
SELECT '[(-1,-1,-1),(1,1,1)]'::cube && '[(2,1,1),(2,2,2)]'::cube AS bool;


-- "contained in" (the left operand is the cube entirely enclosed by
--
-- “包含在”（左操作数是完全由
-- the right operand):
--
-- 正确的操作数）：
--
SELECT '0'::cube                 <@ '0'::cube                        AS bool;
SELECT '0,0,0'::cube             <@ '0,0,0'::cube                    AS bool;
SELECT '0,0'::cube               <@ '0,0,1'::cube                    AS bool;
SELECT '0,0,0'::cube             <@ '0,0,1'::cube                    AS bool;
SELECT '1,0,0'::cube             <@ '0,0,1'::cube                    AS bool;
SELECT '(1,0,0),(0,0,1)'::cube   <@ '(1,0,0),(0,0,1)'::cube          AS bool;
SELECT '(1,0,0),(0,0,1)'::cube   <@ '(-1,-1,-1),(1,1,1)'::cube       AS bool;
SELECT '(1,0,0),(0,0,1)'::cube   <@ '(-1,-1,-1,-1),(1,1,1,1)'::cube  AS bool;
SELECT '0'::cube                 <@ '(-1),(1)'::cube                 AS bool;
SELECT '1'::cube                 <@ '(-1),(1)'::cube                 AS bool;
SELECT '-1'::cube                <@ '(-1),(1)'::cube                 AS bool;
SELECT '(-1),(1)'::cube          <@ '(-1),(1)'::cube                 AS bool;
SELECT '(-1),(1)'::cube          <@ '(-1,-1),(1,1)'::cube            AS bool;
SELECT '(-2),(1)'::cube          <@ '(-1),(1)'::cube                 AS bool;
SELECT '(-2),(1)'::cube          <@ '(-1,-1),(1,1)'::cube            AS bool;


-- "contains" (the left operand is the cube that entirely encloses the
--
-- “包含”（左操作数是完全包围
-- right operand)
--
-- 右操作数）
--
SELECT '0'::cube                        @> '0'::cube                 AS bool;
SELECT '0,0,0'::cube                    @> '0,0,0'::cube             AS bool;
SELECT '0,0,1'::cube                    @> '0,0'::cube               AS bool;
SELECT '0,0,1'::cube                    @> '0,0,0'::cube             AS bool;
SELECT '0,0,1'::cube                    @> '1,0,0'::cube             AS bool;
SELECT '(1,0,0),(0,0,1)'::cube          @> '(1,0,0),(0,0,1)'::cube   AS bool;
SELECT '(-1,-1,-1),(1,1,1)'::cube       @> '(1,0,0),(0,0,1)'::cube   AS bool;
SELECT '(-1,-1,-1,-1),(1,1,1,1)'::cube  @> '(1,0,0),(0,0,1)'::cube   AS bool;
SELECT '(-1),(1)'::cube                 @> '0'::cube                 AS bool;
SELECT '(-1),(1)'::cube                 @> '1'::cube                 AS bool;
SELECT '(-1),(1)'::cube                 @> '-1'::cube                AS bool;
SELECT '(-1),(1)'::cube                 @> '(-1),(1)'::cube          AS bool;
SELECT '(-1,-1),(1,1)'::cube            @> '(-1),(1)'::cube          AS bool;
SELECT '(-1),(1)'::cube                 @> '(-2),(1)'::cube          AS bool;
SELECT '(-1,-1),(1,1)'::cube            @> '(-2),(1)'::cube          AS bool;

-- Test of distance function
--
-- 距离函数测试
--
SELECT cube_distance('(0)'::cube,'(2,2,2,2)'::cube);
SELECT cube_distance('(0)'::cube,'(.3,.4)'::cube);
SELECT cube_distance('(2,3,4)'::cube,'(2,3,4)'::cube);
SELECT cube_distance('(42,42,42,42)'::cube,'(137,137,137,137)'::cube);
SELECT cube_distance('(42,42,42)'::cube,'(137,137)'::cube);

-- Test of cube function (text to cube)
--
-- 立方体功能测试（文本转立方体）
--
SELECT cube('(1,1.2)'::text);
SELECT cube(NULL);

-- Test of cube_dim function (dimensions stored in cube)
--
-- 测试cube_dim函数（立方体中存储的尺寸）
--
SELECT cube_dim('(0)'::cube);
SELECT cube_dim('(0,0)'::cube);
SELECT cube_dim('(0,0,0)'::cube);
SELECT cube_dim('(42,42,42),(42,42,42)'::cube);
SELECT cube_dim('(4,8,15,16,23),(4,8,15,16,23)'::cube);

-- Test of cube_ll_coord function (retrieves LL coordinate values)
--
-- 测试cube_ll_coord函数（检索LL坐标值）
--
SELECT cube_ll_coord('(-1,1),(2,-2)'::cube, 1);
SELECT cube_ll_coord('(-1,1),(2,-2)'::cube, 2);
SELECT cube_ll_coord('(-1,1),(2,-2)'::cube, 3);
SELECT cube_ll_coord('(1,2),(1,2)'::cube, 1);
SELECT cube_ll_coord('(1,2),(1,2)'::cube, 2);
SELECT cube_ll_coord('(1,2),(1,2)'::cube, 3);
SELECT cube_ll_coord('(42,137)'::cube, 1);
SELECT cube_ll_coord('(42,137)'::cube, 2);
SELECT cube_ll_coord('(42,137)'::cube, 3);

-- Test of cube_ur_coord function (retrieves UR coordinate values)
--
-- 测试cube_ur_coord函数（检索UR坐标值）
--
SELECT cube_ur_coord('(-1,1),(2,-2)'::cube, 1);
SELECT cube_ur_coord('(-1,1),(2,-2)'::cube, 2);
SELECT cube_ur_coord('(-1,1),(2,-2)'::cube, 3);
SELECT cube_ur_coord('(1,2),(1,2)'::cube, 1);
SELECT cube_ur_coord('(1,2),(1,2)'::cube, 2);
SELECT cube_ur_coord('(1,2),(1,2)'::cube, 3);
SELECT cube_ur_coord('(42,137)'::cube, 1);
SELECT cube_ur_coord('(42,137)'::cube, 2);
SELECT cube_ur_coord('(42,137)'::cube, 3);

-- Test of cube_is_point
--
-- 测试cube_is_point
--
SELECT cube_is_point('(0)'::cube);
SELECT cube_is_point('(0,1,2)'::cube);
SELECT cube_is_point('(0,1,2),(0,1,2)'::cube);
SELECT cube_is_point('(0,1,2),(-1,1,2)'::cube);
SELECT cube_is_point('(0,1,2),(0,-1,2)'::cube);
SELECT cube_is_point('(0,1,2),(0,1,-2)'::cube);

-- Test of cube_enlarge (enlarging and shrinking cubes)
--
-- cube_enlarge的测试（放大和缩小立方体）
--
SELECT cube_enlarge('(0)'::cube, 0, 0);
SELECT cube_enlarge('(0)'::cube, 0, 1);
SELECT cube_enlarge('(0)'::cube, 0, 2);
SELECT cube_enlarge('(2),(-2)'::cube, 0, 4);
SELECT cube_enlarge('(0)'::cube, 1, 0);
SELECT cube_enlarge('(0)'::cube, 1, 1);
SELECT cube_enlarge('(0)'::cube, 1, 2);
SELECT cube_enlarge('(2),(-2)'::cube, 1, 4);
SELECT cube_enlarge('(0)'::cube, -1, 0);
SELECT cube_enlarge('(0)'::cube, -1, 1);
SELECT cube_enlarge('(0)'::cube, -1, 2);
SELECT cube_enlarge('(2),(-2)'::cube, -1, 4);
SELECT cube_enlarge('(0,0,0)'::cube, 1, 0);
SELECT cube_enlarge('(0,0,0)'::cube, 1, 2);
SELECT cube_enlarge('(2,-2),(-3,7)'::cube, 1, 2);
SELECT cube_enlarge('(2,-2),(-3,7)'::cube, 3, 2);
SELECT cube_enlarge('(2,-2),(-3,7)'::cube, -1, 2);
SELECT cube_enlarge('(2,-2),(-3,7)'::cube, -3, 2);
SELECT cube_enlarge('(42,-23,-23),(42,23,23)'::cube, -23, 5);
SELECT cube_enlarge('(42,-23,-23),(42,23,23)'::cube, -24, 5);

-- Test of cube_union (MBR for two cubes)
--
-- 测试cube_union（两个立方体的MBR）
--
SELECT cube_union('(1,2),(3,4)'::cube, '(5,6,7),(8,9,10)'::cube);
SELECT cube_union('(1,2)'::cube, '(4,2,0,0)'::cube);
SELECT cube_union('(1,2),(1,2)'::cube, '(4,2),(4,2)'::cube);
SELECT cube_union('(1,2),(1,2)'::cube, '(1,2),(1,2)'::cube);
SELECT cube_union('(1,2),(1,2)'::cube, '(1,2,0),(1,2,0)'::cube);

-- Test of cube_inter
--
-- 测试cube_inter
--
SELECT cube_inter('(1,2),(10,11)'::cube, '(3,4), (16,15)'::cube); -- intersects
SELECT cube_inter('(1,2),(10,11)'::cube, '(3,4), (6,5)'::cube); -- includes
SELECT cube_inter('(1,2),(10,11)'::cube, '(13,14), (16,15)'::cube); -- no intersection
SELECT cube_inter('(1,2),(10,11)'::cube, '(3,14), (16,15)'::cube); -- no intersection, but one dimension intersects
SELECT cube_inter('(1,2),(10,11)'::cube, '(10,11), (16,15)'::cube); -- point intersection
SELECT cube_inter('(1,2,3)'::cube, '(1,2,3)'::cube); -- point args
SELECT cube_inter('(1,2,3)'::cube, '(5,6,3)'::cube); -- point args

-- Test of cube_size
--
-- 测试cube_size
--
SELECT cube_size('(4,8),(15,16)'::cube);
SELECT cube_size('(42,137)'::cube);

-- Test of distances (euclidean distance may not be bit-exact)
--
-- 距离测试（欧氏距离可能不是位精确的）
--
SET extra_float_digits = 0;
SELECT cube_distance('(1,1)'::cube, '(4,5)'::cube);
SELECT '(1,1)'::cube <-> '(4,5)'::cube as d_e;
RESET extra_float_digits;
SELECT distance_chebyshev('(1,1)'::cube, '(4,5)'::cube);
SELECT '(1,1)'::cube <=> '(4,5)'::cube as d_c;
SELECT distance_taxicab('(1,1)'::cube, '(4,5)'::cube);
SELECT '(1,1)'::cube <#> '(4,5)'::cube as d_t;
-- zero for overlapping
--
-- 重叠为零
SELECT cube_distance('(2,2),(10,10)'::cube, '(0,0),(5,5)'::cube);
SELECT distance_chebyshev('(2,2),(10,10)'::cube, '(0,0),(5,5)'::cube);
SELECT distance_taxicab('(2,2),(10,10)'::cube, '(0,0),(5,5)'::cube);
-- coordinate access
--
-- 协调访问
SELECT cube(array[10,20,30], array[40,50,60])->1;
SELECT cube(array[40,50,60], array[10,20,30])->1;
SELECT cube(array[10,20,30], array[40,50,60])->6;
SELECT cube(array[10,20,30], array[40,50,60])->0;
SELECT cube(array[10,20,30], array[40,50,60])->7;
SELECT cube(array[10,20,30], array[40,50,60])->-1;
SELECT cube(array[10,20,30], array[40,50,60])->-6;
SELECT cube(array[10,20,30])->3;
SELECT cube(array[10,20,30])->6;
SELECT cube(array[10,20,30])->-6;
-- "normalized" coordinate access
--
-- “标准化”坐标访问
SELECT cube(array[10,20,30], array[40,50,60])~>1;
SELECT cube(array[40,50,60], array[10,20,30])~>1;
SELECT cube(array[10,20,30], array[40,50,60])~>2;
SELECT cube(array[40,50,60], array[10,20,30])~>2;
SELECT cube(array[10,20,30], array[40,50,60])~>3;
SELECT cube(array[40,50,60], array[10,20,30])~>3;

SELECT cube(array[40,50,60], array[10,20,30])~>0;
SELECT cube(array[40,50,60], array[10,20,30])~>4;
SELECT cube(array[40,50,60], array[10,20,30])~>(-1);

-- Load some example data and build the index
--
-- 加载一些示例数据并构建索引
--
CREATE TABLE test_cube (c cube);

\copy test_cube from 'data/test_cube.data'

CREATE INDEX test_cube_ix ON test_cube USING gist (c);
SELECT * FROM test_cube WHERE c && '(3000,1000),(0,0)' ORDER BY c;

-- Test sorting
--
-- 测试排序
SELECT * FROM test_cube WHERE c && '(3000,1000),(0,0)' GROUP BY c ORDER BY c;

-- Test index-only scans
--
-- 测试仅索引扫描
SET enable_bitmapscan = false;
EXPLAIN (COSTS OFF)
SELECT c FROM test_cube WHERE c <@ '(3000,1000),(0,0)' ORDER BY c;
SELECT c FROM test_cube WHERE c <@ '(3000,1000),(0,0)' ORDER BY c;
RESET enable_bitmapscan;

-- Test kNN
--
-- 测试kNN
INSERT INTO test_cube VALUES ('(1,1)'), ('(100000)'), ('(0, 100000)'); -- Some corner cases
SET enable_seqscan = false;

-- Test different metrics
--
-- 测试不同的指标
SET extra_float_digits = 0;
SELECT *, c <-> '(100, 100),(500, 500)'::cube as dist FROM test_cube ORDER BY c <-> '(100, 100),(500, 500)'::cube LIMIT 5;
RESET extra_float_digits;
SELECT *, c <=> '(100, 100),(500, 500)'::cube as dist FROM test_cube ORDER BY c <=> '(100, 100),(500, 500)'::cube LIMIT 5;
SELECT *, c <#> '(100, 100),(500, 500)'::cube as dist FROM test_cube ORDER BY c <#> '(100, 100),(500, 500)'::cube LIMIT 5;

-- Test sorting by coordinates
--
-- 测试按坐标排序
SELECT c~>1, c FROM test_cube ORDER BY c~>1 LIMIT 15; -- ascending by left bound
SELECT c~>2, c FROM test_cube ORDER BY c~>2 LIMIT 15; -- ascending by right bound
SELECT c~>3, c FROM test_cube ORDER BY c~>3 LIMIT 15; -- ascending by lower bound
SELECT c~>4, c FROM test_cube ORDER BY c~>4 LIMIT 15; -- ascending by upper bound
SELECT c~>(-1), c FROM test_cube ORDER BY c~>(-1) LIMIT 15; -- descending by left bound
SELECT c~>(-2), c FROM test_cube ORDER BY c~>(-2) LIMIT 15; -- descending by right bound
SELECT c~>(-3), c FROM test_cube ORDER BY c~>(-3) LIMIT 15; -- descending by lower bound
SELECT c~>(-4), c FROM test_cube ORDER BY c~>(-4) LIMIT 15; -- descending by upper bound

-- Same queries with sequential scan (should give the same results as above)
--
-- 顺序扫描的相同查询（应该给出与上面相同的结果）
RESET enable_seqscan;
SET enable_indexscan = OFF;
SET extra_float_digits = 0;
SELECT *, c <-> '(100, 100),(500, 500)'::cube as dist FROM test_cube ORDER BY c <-> '(100, 100),(500, 500)'::cube LIMIT 5;
RESET extra_float_digits;
SELECT *, c <=> '(100, 100),(500, 500)'::cube as dist FROM test_cube ORDER BY c <=> '(100, 100),(500, 500)'::cube LIMIT 5;
SELECT *, c <#> '(100, 100),(500, 500)'::cube as dist FROM test_cube ORDER BY c <#> '(100, 100),(500, 500)'::cube LIMIT 5;
SELECT c~>1, c FROM test_cube ORDER BY c~>1 LIMIT 15; -- ascending by left bound
SELECT c~>2, c FROM test_cube ORDER BY c~>2 LIMIT 15; -- ascending by right bound
SELECT c~>3, c FROM test_cube ORDER BY c~>3 LIMIT 15; -- ascending by lower bound
SELECT c~>4, c FROM test_cube ORDER BY c~>4 LIMIT 15; -- ascending by upper bound
SELECT c~>(-1), c FROM test_cube ORDER BY c~>(-1) LIMIT 15; -- descending by left bound
SELECT c~>(-2), c FROM test_cube ORDER BY c~>(-2) LIMIT 15; -- descending by right bound
SELECT c~>(-3), c FROM test_cube ORDER BY c~>(-3) LIMIT 15; -- descending by lower bound
SELECT c~>(-4), c FROM test_cube ORDER BY c~>(-4) LIMIT 15; -- descending by upper bound
RESET enable_indexscan;

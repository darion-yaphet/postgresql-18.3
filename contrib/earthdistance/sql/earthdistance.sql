--
--  Test earthdistance extension
--
-- 测试接地距离延长
--
-- In this file we also do some testing of extension create/drop scenarios.
--
-- 在此文件中，我们还对扩展创建/删除场景进行了一些测试。
-- That's really exercising the core database's dependency logic, so ideally
--
-- 这实际上是在锻炼核心数据库的依赖逻辑，所以理想情况下
-- we'd do it in the core regression tests, but we can't for lack of suitable
--
-- 我们会在核心回归测试中这样做，但由于缺乏合适的测试，我们不能这样做
-- guaranteed-available extensions.  earthdistance is a good test case because
--
-- 保证可用的扩展。  Earthdistance 是一个很好的测试用例，因为
-- it has a dependency on the cube extension.
--
-- 它依赖于多维数据集扩展。
--

CREATE EXTENSION earthdistance;  -- fail, must install cube first
CREATE EXTENSION cube;
CREATE EXTENSION earthdistance;

--
-- The radius of the Earth we are using.
--
-- 我们正在使用的地球半径。
--

SELECT earth()::numeric(20,5);

--
-- Convert straight line distances to great circle distances.
--
-- 将直线距离转换为大圆距离。
--
SELECT (pi()*earth())::numeric(20,5);
SELECT sec_to_gc(0)::numeric(20,5);
SELECT sec_to_gc(2*earth())::numeric(20,5);
SELECT sec_to_gc(10*earth())::numeric(20,5);
SELECT sec_to_gc(-earth())::numeric(20,5);
SELECT sec_to_gc(1000)::numeric(20,5);
SELECT sec_to_gc(10000)::numeric(20,5);
SELECT sec_to_gc(100000)::numeric(20,5);
SELECT sec_to_gc(1000000)::numeric(20,5);

--
-- Convert great circle distances to straight line distances.
--
-- 将大圆距离转换为直线距离。
--

SELECT gc_to_sec(0)::numeric(20,5);
SELECT gc_to_sec(sec_to_gc(2*earth()))::numeric(20,5);
SELECT gc_to_sec(10*earth())::numeric(20,5);
SELECT gc_to_sec(pi()*earth())::numeric(20,5);
SELECT gc_to_sec(-1000)::numeric(20,5);
SELECT gc_to_sec(1000)::numeric(20,5);
SELECT gc_to_sec(10000)::numeric(20,5);
SELECT gc_to_sec(100000)::numeric(20,5);
SELECT gc_to_sec(1000000)::numeric(20,5);

--
-- Set coordinates using latitude and longitude.
--
-- 使用纬度和经度设置坐标。
-- Extract each coordinate separately so we can round them.
--
-- 分别提取每个坐标，以便我们可以对它们进行舍入。
--

SELECT cube_ll_coord(ll_to_earth(0,0),1)::numeric(20,5),
 cube_ll_coord(ll_to_earth(0,0),2)::numeric(20,5),
 cube_ll_coord(ll_to_earth(0,0),3)::numeric(20,5);
SELECT cube_ll_coord(ll_to_earth(360,360),1)::numeric(20,5),
 cube_ll_coord(ll_to_earth(360,360),2)::numeric(20,5),
 cube_ll_coord(ll_to_earth(360,360),3)::numeric(20,5);
SELECT cube_ll_coord(ll_to_earth(180,180),1)::numeric(20,5),
 cube_ll_coord(ll_to_earth(180,180),2)::numeric(20,5),
 cube_ll_coord(ll_to_earth(180,180),3)::numeric(20,5);
SELECT cube_ll_coord(ll_to_earth(180,360),1)::numeric(20,5),
 cube_ll_coord(ll_to_earth(180,360),2)::numeric(20,5),
 cube_ll_coord(ll_to_earth(180,360),3)::numeric(20,5);
SELECT cube_ll_coord(ll_to_earth(-180,-360),1)::numeric(20,5),
 cube_ll_coord(ll_to_earth(-180,-360),2)::numeric(20,5),
 cube_ll_coord(ll_to_earth(-180,-360),3)::numeric(20,5);
SELECT cube_ll_coord(ll_to_earth(0,180),1)::numeric(20,5),
 cube_ll_coord(ll_to_earth(0,180),2)::numeric(20,5),
 cube_ll_coord(ll_to_earth(0,180),3)::numeric(20,5);
SELECT cube_ll_coord(ll_to_earth(0,-180),1)::numeric(20,5),
 cube_ll_coord(ll_to_earth(0,-180),2)::numeric(20,5),
 cube_ll_coord(ll_to_earth(0,-180),3)::numeric(20,5);
SELECT cube_ll_coord(ll_to_earth(90,0),1)::numeric(20,5),
 cube_ll_coord(ll_to_earth(90,0),2)::numeric(20,5),
 cube_ll_coord(ll_to_earth(90,0),3)::numeric(20,5);
SELECT cube_ll_coord(ll_to_earth(90,180),1)::numeric(20,5),
 cube_ll_coord(ll_to_earth(90,180),2)::numeric(20,5),
 cube_ll_coord(ll_to_earth(90,180),3)::numeric(20,5);
SELECT cube_ll_coord(ll_to_earth(-90,0),1)::numeric(20,5),
 cube_ll_coord(ll_to_earth(-90,0),2)::numeric(20,5),
 cube_ll_coord(ll_to_earth(-90,0),3)::numeric(20,5);
SELECT cube_ll_coord(ll_to_earth(-90,180),1)::numeric(20,5),
 cube_ll_coord(ll_to_earth(-90,180),2)::numeric(20,5),
 cube_ll_coord(ll_to_earth(-90,180),3)::numeric(20,5);

--
-- Test getting the latitude of a location.
--
-- 测试获取某个位置的纬度。
--

SELECT latitude(ll_to_earth(0,0))::numeric(20,10);
SELECT latitude(ll_to_earth(45,0))::numeric(20,10);
SELECT latitude(ll_to_earth(90,0))::numeric(20,10);
SELECT latitude(ll_to_earth(-45,0))::numeric(20,10);
SELECT latitude(ll_to_earth(-90,0))::numeric(20,10);
SELECT latitude(ll_to_earth(0,90))::numeric(20,10);
SELECT latitude(ll_to_earth(45,90))::numeric(20,10);
SELECT latitude(ll_to_earth(90,90))::numeric(20,10);
SELECT latitude(ll_to_earth(-45,90))::numeric(20,10);
SELECT latitude(ll_to_earth(-90,90))::numeric(20,10);
SELECT latitude(ll_to_earth(0,180))::numeric(20,10);
SELECT latitude(ll_to_earth(45,180))::numeric(20,10);
SELECT latitude(ll_to_earth(90,180))::numeric(20,10);
SELECT latitude(ll_to_earth(-45,180))::numeric(20,10);
SELECT latitude(ll_to_earth(-90,180))::numeric(20,10);
SELECT latitude(ll_to_earth(0,-90))::numeric(20,10);
SELECT latitude(ll_to_earth(45,-90))::numeric(20,10);
SELECT latitude(ll_to_earth(90,-90))::numeric(20,10);
SELECT latitude(ll_to_earth(-45,-90))::numeric(20,10);
SELECT latitude(ll_to_earth(-90,-90))::numeric(20,10);

--
-- Test getting the longitude of a location.
--
-- 测试获取某个位置的经度。
--

SELECT longitude(ll_to_earth(0,0))::numeric(20,10);
SELECT longitude(ll_to_earth(45,0))::numeric(20,10);
SELECT longitude(ll_to_earth(90,0))::numeric(20,10);
SELECT longitude(ll_to_earth(-45,0))::numeric(20,10);
SELECT longitude(ll_to_earth(-90,0))::numeric(20,10);
SELECT longitude(ll_to_earth(0,90))::numeric(20,10);
SELECT longitude(ll_to_earth(45,90))::numeric(20,10);
SELECT longitude(ll_to_earth(90,90))::numeric(20,10);
SELECT longitude(ll_to_earth(-45,90))::numeric(20,10);
SELECT longitude(ll_to_earth(-90,90))::numeric(20,10);
SELECT longitude(ll_to_earth(0,180))::numeric(20,10);
SELECT longitude(ll_to_earth(45,180))::numeric(20,10);
SELECT longitude(ll_to_earth(90,180))::numeric(20,10);
SELECT longitude(ll_to_earth(-45,180))::numeric(20,10);
SELECT longitude(ll_to_earth(-90,180))::numeric(20,10);
SELECT longitude(ll_to_earth(0,-90))::numeric(20,10);
SELECT longitude(ll_to_earth(45,-90))::numeric(20,10);
SELECT longitude(ll_to_earth(90,-90))::numeric(20,10);
SELECT longitude(ll_to_earth(-45,-90))::numeric(20,10);
SELECT longitude(ll_to_earth(-90,-90))::numeric(20,10);

--
-- For the distance tests the following is some real life data.
--
-- 对于距离测试，以下是一些现实生活数据。
--
-- Chicago has a latitude of 41.8 and a longitude of 87.6.
--
-- 芝加哥的纬度为 41.8，经度为 87.6。
-- Albuquerque has a latitude of 35.1 and a longitude of 106.7.
--
-- 阿尔伯克基的纬度为 35.1，经度为 106.7。
-- (Note that latitude and longitude are specified differently
--
-- （注意纬度和经度的指定不同
-- in the cube based functions than for the point based functions.)
--
-- 基于立方体的函数比基于点的函数。）
--

--
-- Test getting the distance between two points using earth_distance.
--
-- 测试使用 Earth_distance 获取两点之间的距离。
--

SELECT earth_distance(ll_to_earth(0,0),ll_to_earth(0,0))::numeric(20,5);
SELECT earth_distance(ll_to_earth(0,0),ll_to_earth(0,180))::numeric(20,5);
SELECT earth_distance(ll_to_earth(0,0),ll_to_earth(90,0))::numeric(20,5);
SELECT earth_distance(ll_to_earth(0,0),ll_to_earth(0,90))::numeric(20,5);
SELECT earth_distance(ll_to_earth(0,0),ll_to_earth(0,1))::numeric(20,5);
SELECT earth_distance(ll_to_earth(0,0),ll_to_earth(1,0))::numeric(20,5);
SELECT earth_distance(ll_to_earth(30,0),ll_to_earth(30,1))::numeric(20,5);
SELECT earth_distance(ll_to_earth(30,0),ll_to_earth(31,0))::numeric(20,5);
SELECT earth_distance(ll_to_earth(60,0),ll_to_earth(60,1))::numeric(20,5);
SELECT earth_distance(ll_to_earth(60,0),ll_to_earth(61,0))::numeric(20,5);
SELECT earth_distance(ll_to_earth(41.8,87.6),ll_to_earth(35.1,106.7))::numeric(20,5);
SELECT (earth_distance(ll_to_earth(41.8,87.6),ll_to_earth(35.1,106.7))*
      100./2.54/12./5280.)::numeric(20,5);

--
-- Test getting the distance between two points using geo_distance.
--
-- 测试使用 geo_distance 获取两点之间的距离。
--

SELECT geo_distance('(0,0)'::point,'(0,0)'::point)::numeric(20,5);
SELECT geo_distance('(0,0)'::point,'(180,0)'::point)::numeric(20,5);
SELECT geo_distance('(0,0)'::point,'(0,90)'::point)::numeric(20,5);
SELECT geo_distance('(0,0)'::point,'(90,0)'::point)::numeric(20,5);
SELECT geo_distance('(0,0)'::point,'(1,0)'::point)::numeric(20,5);
SELECT geo_distance('(0,0)'::point,'(0,1)'::point)::numeric(20,5);
SELECT geo_distance('(0,30)'::point,'(1,30)'::point)::numeric(20,5);
SELECT geo_distance('(0,30)'::point,'(0,31)'::point)::numeric(20,5);
SELECT geo_distance('(0,60)'::point,'(1,60)'::point)::numeric(20,5);
SELECT geo_distance('(0,60)'::point,'(0,61)'::point)::numeric(20,5);
SELECT geo_distance('(87.6,41.8)'::point,'(106.7,35.1)'::point)::numeric(20,5);
SELECT (geo_distance('(87.6,41.8)'::point,'(106.7,35.1)'::point)*5280.*12.*2.54/100.)::numeric(20,5);

--
-- Test getting the distance between two points using the <@> operator.
--
-- 测试使用 <@> 运算符获取两点之间的距离。
--

SELECT ('(0,0)'::point <@> '(0,0)'::point)::numeric(20,5);
SELECT ('(0,0)'::point <@> '(180,0)'::point)::numeric(20,5);
SELECT ('(0,0)'::point <@> '(0,90)'::point)::numeric(20,5);
SELECT ('(0,0)'::point <@> '(90,0)'::point)::numeric(20,5);
SELECT ('(0,0)'::point <@> '(1,0)'::point)::numeric(20,5);
SELECT ('(0,0)'::point <@> '(0,1)'::point)::numeric(20,5);
SELECT ('(0,30)'::point <@> '(1,30)'::point)::numeric(20,5);
SELECT ('(0,30)'::point <@> '(0,31)'::point)::numeric(20,5);
SELECT ('(0,60)'::point <@> '(1,60)'::point)::numeric(20,5);
SELECT ('(0,60)'::point <@> '(0,61)'::point)::numeric(20,5);
SELECT ('(87.6,41.8)'::point <@> '(106.7,35.1)'::point)::numeric(20,5);
SELECT (('(87.6,41.8)'::point <@> '(106.7,35.1)'::point)*5280.*12.*2.54/100.)::numeric(20,5);

--
-- Test getting a bounding box around points.
--
-- 测试获取点周围的边界框。
--

SELECT cube_ll_coord(earth_box(ll_to_earth(0,0),112000),1)::numeric(20,5),
       cube_ll_coord(earth_box(ll_to_earth(0,0),112000),2)::numeric(20,5),
       cube_ll_coord(earth_box(ll_to_earth(0,0),112000),3)::numeric(20,5),
       cube_ur_coord(earth_box(ll_to_earth(0,0),112000),1)::numeric(20,5),
       cube_ur_coord(earth_box(ll_to_earth(0,0),112000),2)::numeric(20,5),
       cube_ur_coord(earth_box(ll_to_earth(0,0),112000),3)::numeric(20,5);
SELECT cube_ll_coord(earth_box(ll_to_earth(0,0),pi()*earth()),1)::numeric(20,5),
       cube_ll_coord(earth_box(ll_to_earth(0,0),pi()*earth()),2)::numeric(20,5),
       cube_ll_coord(earth_box(ll_to_earth(0,0),pi()*earth()),3)::numeric(20,5),
       cube_ur_coord(earth_box(ll_to_earth(0,0),pi()*earth()),1)::numeric(20,5),
       cube_ur_coord(earth_box(ll_to_earth(0,0),pi()*earth()),2)::numeric(20,5),
       cube_ur_coord(earth_box(ll_to_earth(0,0),pi()*earth()),3)::numeric(20,5);
SELECT cube_ll_coord(earth_box(ll_to_earth(0,0),10*earth()),1)::numeric(20,5),
       cube_ll_coord(earth_box(ll_to_earth(0,0),10*earth()),2)::numeric(20,5),
       cube_ll_coord(earth_box(ll_to_earth(0,0),10*earth()),3)::numeric(20,5),
       cube_ur_coord(earth_box(ll_to_earth(0,0),10*earth()),1)::numeric(20,5),
       cube_ur_coord(earth_box(ll_to_earth(0,0),10*earth()),2)::numeric(20,5),
       cube_ur_coord(earth_box(ll_to_earth(0,0),10*earth()),3)::numeric(20,5);

--
-- Test for points that should be in bounding boxes.
--
-- 测试应位于边界框中的点。
--

SELECT earth_box(ll_to_earth(0,0),
       earth_distance(ll_to_earth(0,0),ll_to_earth(0,1))*1.00001) @>
       ll_to_earth(0,1);
SELECT earth_box(ll_to_earth(0,0),
       earth_distance(ll_to_earth(0,0),ll_to_earth(0,0.1))*1.00001) @>
       ll_to_earth(0,0.1);
SELECT earth_box(ll_to_earth(0,0),
       earth_distance(ll_to_earth(0,0),ll_to_earth(0,0.01))*1.00001) @>
       ll_to_earth(0,0.01);
SELECT earth_box(ll_to_earth(0,0),
       earth_distance(ll_to_earth(0,0),ll_to_earth(0,0.001))*1.00001) @>
       ll_to_earth(0,0.001);
SELECT earth_box(ll_to_earth(0,0),
       earth_distance(ll_to_earth(0,0),ll_to_earth(0,0.0001))*1.00001) @>
       ll_to_earth(0,0.0001);
SELECT earth_box(ll_to_earth(0,0),
       earth_distance(ll_to_earth(0,0),ll_to_earth(0.0001,0.0001))*1.00001) @>
       ll_to_earth(0.0001,0.0001);
SELECT earth_box(ll_to_earth(45,45),
       earth_distance(ll_to_earth(45,45),ll_to_earth(45.0001,45.0001))*1.00001) @>
       ll_to_earth(45.0001,45.0001);
SELECT earth_box(ll_to_earth(90,180),
       earth_distance(ll_to_earth(90,180),ll_to_earth(90.0001,180.0001))*1.00001) @>
       ll_to_earth(90.0001,180.0001);

--
-- Test for points that shouldn't be in bounding boxes. Note that we need
--
-- 测试不应位于边界框中的点。请注意，我们需要
-- to make points way outside, since some points close may be in the box
--
-- 使分数远离外侧，因为一些接近的分数可能在盒子内
-- but further away than the distance we are testing.
--
-- 但比我们正在测试的距离更远。
--

SELECT earth_box(ll_to_earth(0,0),
       earth_distance(ll_to_earth(0,0),ll_to_earth(0,1))*.57735) @>
       ll_to_earth(0,1);
SELECT earth_box(ll_to_earth(0,0),
       earth_distance(ll_to_earth(0,0),ll_to_earth(0,0.1))*.57735) @>
       ll_to_earth(0,0.1);
SELECT earth_box(ll_to_earth(0,0),
       earth_distance(ll_to_earth(0,0),ll_to_earth(0,0.01))*.57735) @>
       ll_to_earth(0,0.01);
SELECT earth_box(ll_to_earth(0,0),
       earth_distance(ll_to_earth(0,0),ll_to_earth(0,0.001))*.57735) @>
       ll_to_earth(0,0.001);
SELECT earth_box(ll_to_earth(0,0),
       earth_distance(ll_to_earth(0,0),ll_to_earth(0,0.0001))*.57735) @>
       ll_to_earth(0,0.0001);
SELECT earth_box(ll_to_earth(0,0),
       earth_distance(ll_to_earth(0,0),ll_to_earth(0.0001,0.0001))*.57735) @>
       ll_to_earth(0.0001,0.0001);
SELECT earth_box(ll_to_earth(45,45),
       earth_distance(ll_to_earth(45,45),ll_to_earth(45.0001,45.0001))*.57735) @>
       ll_to_earth(45.0001,45.0001);
SELECT earth_box(ll_to_earth(90,180),
       earth_distance(ll_to_earth(90,180),ll_to_earth(90.0001,180.0001))*.57735) @>
       ll_to_earth(90.0001,180.0001);

--
-- Test the recommended constraints.
--
-- 测试推荐的约束。
--

SELECT cube_is_point(ll_to_earth(0,0));
SELECT cube_dim(ll_to_earth(0,0)) <= 3;
SELECT abs(cube_distance(ll_to_earth(0,0), '(0)'::cube) / earth() - 1) <
       '10e-12'::float8;
SELECT cube_is_point(ll_to_earth(30,60));
SELECT cube_dim(ll_to_earth(30,60)) <= 3;
SELECT abs(cube_distance(ll_to_earth(30,60), '(0)'::cube) / earth() - 1) <
       '10e-12'::float8;
SELECT cube_is_point(ll_to_earth(60,90));
SELECT cube_dim(ll_to_earth(60,90)) <= 3;
SELECT abs(cube_distance(ll_to_earth(60,90), '(0)'::cube) / earth() - 1) <
       '10e-12'::float8;
SELECT cube_is_point(ll_to_earth(-30,-90));
SELECT cube_dim(ll_to_earth(-30,-90)) <= 3;
SELECT abs(cube_distance(ll_to_earth(-30,-90), '(0)'::cube) / earth() - 1) <
       '10e-12'::float8;

--
-- Now we are going to test extension create/drop scenarios.
--
-- 现在我们将测试扩展创建/删除场景。
--

-- list what's installed
--
-- 列出已安装的内容
\dT

drop extension cube;  -- fail, earthdistance requires it

drop extension earthdistance;

drop type cube;  -- fail, extension cube requires it

-- list what's installed
--
-- 列出已安装的内容
\dT

create table foo (f1 cube, f2 int);

drop extension cube;  -- fail, foo.f1 requires it

drop table foo;

drop extension cube;

-- list what's installed
--
-- 列出已安装的内容
\dT
\df
\do

create schema c;

create extension cube with schema c;

-- list what's installed
--
-- 列出已安装的内容
\dT public.*
\df public.*
\do public.*
\dT c.*

create table foo (f1 c.cube, f2 int);

drop extension cube;  -- fail, foo.f1 requires it

drop schema c;  -- fail, cube requires it

drop extension cube cascade;

\d foo

-- list what's installed
--
-- 列出已安装的内容
\dT public.*
\df public.*
\do public.*
\dT c.*
\df c.*
\do c.*

drop schema c;

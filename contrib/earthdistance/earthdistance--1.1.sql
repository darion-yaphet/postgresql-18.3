/* contrib/earthdistance/earthdistance--1.1.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
--
-- 抱怨脚本是否源自 psql，而不是通过 CREATE EXTENSION
\echo Use "CREATE EXTENSION earthdistance" to load this file. \quit

-- earth() returns the radius of the earth in meters. This is the only
--
-- Earth() 返回地球的半径（以米为单位）。这是唯一的
-- place you need to change things for the cube base distance functions
--
-- 您需要更改立方体基础距离函数的地方
-- in order to use different units (or a better value for the Earth's radius).
--
-- 为了使用不同的单位（或更好的地球半径值）。

CREATE FUNCTION earth() RETURNS float8
LANGUAGE SQL IMMUTABLE PARALLEL SAFE
AS 'SELECT ''6378168''::float8';

-- Astronomers may want to change the earth function so that distances will be
--
-- 天文学家可能想要改变地球的功能，以便距离
-- returned in degrees. To do this comment out the above definition and
--
-- 以度为单位返回。要做到这一点，请注释掉上面的定义和
-- uncomment the one below. Note that doing this will break the regression
--
-- 取消下面一项的注释。请注意，这样做会破坏回归
-- tests.
--
-- CREATE FUNCTION earth() RETURNS float8
--
-- 创建函数 Earth() 返回 float8
-- LANGUAGE SQL IMMUTABLE
--
-- 语言 SQL 不可变
-- AS 'SELECT 180/pi()';
--
-- AS '选择 180/pi()';

-- Define domain for locations on the surface of the earth using a cube
--
-- 使用立方体定义地球表面位置的域
-- datatype with constraints. cube provides 3D indexing.
--
-- 有约束的数据类型。 cube 提供 3D 索引。
-- The cube is restricted to be a point, no more than 3 dimensions
--
-- 立方体被限制为一个点，不超过3个维度
-- (for less than 3 dimensions 0 is assumed for the missing coordinates)
--
-- （对于小于 3 维的情况，假设缺失坐标为 0）
-- and that the point must be very near the surface of the sphere
--
-- 并且该点必须非常靠近球体的表面
-- centered about the origin with the radius of the earth.
--
-- 以地球半径为原点为中心。

CREATE DOMAIN earth AS @extschema:cube@.cube
  CONSTRAINT not_point CHECK(@extschema:cube@.cube_is_point(VALUE))
  CONSTRAINT not_3d CHECK(@extschema:cube@.cube_dim(VALUE) <= 3)
  CONSTRAINT on_surface CHECK(abs(@extschema:cube@.cube_distance(VALUE, '(0)'::@extschema:cube@.cube) /
  earth() - '1'::float8) < '10e-7'::float8);

CREATE FUNCTION sec_to_gc(float8)
RETURNS float8
LANGUAGE SQL
IMMUTABLE STRICT
PARALLEL SAFE
AS 'SELECT CASE WHEN $1 < 0 THEN 0::float8 WHEN $1/(2*earth()) > 1 THEN pi()*earth() ELSE 2*earth()*asin($1/(2*earth())) END';

CREATE FUNCTION gc_to_sec(float8)
RETURNS float8
LANGUAGE SQL
IMMUTABLE STRICT
PARALLEL SAFE
AS 'SELECT CASE WHEN $1 < 0 THEN 0::float8 WHEN $1/earth() > pi() THEN 2*earth() ELSE 2*earth()*sin($1/(2*earth())) END';

CREATE FUNCTION ll_to_earth(float8, float8)
RETURNS earth
LANGUAGE SQL
IMMUTABLE STRICT
PARALLEL SAFE
AS 'SELECT cube(cube(cube(earth()*cos(radians($1))*cos(radians($2))),earth()*cos(radians($1))*sin(radians($2))),earth()*sin(radians($1)))::earth';

CREATE FUNCTION latitude(earth)
RETURNS float8
LANGUAGE SQL
IMMUTABLE STRICT
PARALLEL SAFE
AS 'SELECT CASE WHEN cube_ll_coord($1, 3)/earth() < -1 THEN -90::float8 WHEN cube_ll_coord($1, 3)/earth() > 1 THEN 90::float8 ELSE degrees(asin(cube_ll_coord($1, 3)/earth())) END';

CREATE FUNCTION longitude(earth)
RETURNS float8
LANGUAGE SQL
IMMUTABLE STRICT
PARALLEL SAFE
AS 'SELECT degrees(atan2(cube_ll_coord($1, 2), cube_ll_coord($1, 1)))';

CREATE FUNCTION earth_distance(earth, earth)
RETURNS float8
LANGUAGE SQL
IMMUTABLE STRICT
PARALLEL SAFE
AS 'SELECT sec_to_gc(cube_distance($1, $2))';

CREATE FUNCTION earth_box(earth, float8)
RETURNS cube
LANGUAGE SQL
IMMUTABLE STRICT
PARALLEL SAFE
AS 'SELECT cube_enlarge($1, gc_to_sec($2), 3)';

--------------- geo_distance
--
--------------- 地理距离

CREATE FUNCTION geo_distance (point, point)
RETURNS float8
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE AS 'MODULE_PATHNAME';

--------------- geo_distance as operator <@>
--
--------------- geo_distance 作为运算符 <@>

CREATE OPERATOR <@> (
  LEFTARG = point,
  RIGHTARG = point,
  PROCEDURE = geo_distance,
  COMMUTATOR = <@>
);

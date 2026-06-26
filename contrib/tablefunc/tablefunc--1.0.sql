/* contrib/tablefunc/tablefunc--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
--
-- 抱怨脚本是否源自 psql，而不是通过 CREATE EXTENSION
\echo Use "CREATE EXTENSION tablefunc" to load this file. \quit

CREATE FUNCTION normal_rand(int4, float8, float8)
RETURNS setof float8
AS 'MODULE_PATHNAME','normal_rand'
LANGUAGE C VOLATILE STRICT;

-- the generic crosstab function:
--
-- 通用交叉表函数：
CREATE FUNCTION crosstab(text)
RETURNS setof record
AS 'MODULE_PATHNAME','crosstab'
LANGUAGE C STABLE STRICT;

-- examples of building custom type-specific crosstab functions:
--
-- 构建自定义类型特定交叉表函数的示例：
CREATE TYPE tablefunc_crosstab_2 AS
(
	row_name TEXT,
	category_1 TEXT,
	category_2 TEXT
);

CREATE TYPE tablefunc_crosstab_3 AS
(
	row_name TEXT,
	category_1 TEXT,
	category_2 TEXT,
	category_3 TEXT
);

CREATE TYPE tablefunc_crosstab_4 AS
(
	row_name TEXT,
	category_1 TEXT,
	category_2 TEXT,
	category_3 TEXT,
	category_4 TEXT
);

CREATE FUNCTION crosstab2(text)
RETURNS setof tablefunc_crosstab_2
AS 'MODULE_PATHNAME','crosstab'
LANGUAGE C STABLE STRICT;

CREATE FUNCTION crosstab3(text)
RETURNS setof tablefunc_crosstab_3
AS 'MODULE_PATHNAME','crosstab'
LANGUAGE C STABLE STRICT;

CREATE FUNCTION crosstab4(text)
RETURNS setof tablefunc_crosstab_4
AS 'MODULE_PATHNAME','crosstab'
LANGUAGE C STABLE STRICT;

-- obsolete:
CREATE FUNCTION crosstab(text,int)
RETURNS setof record
AS 'MODULE_PATHNAME','crosstab'
LANGUAGE C STABLE STRICT;

CREATE FUNCTION crosstab(text,text)
RETURNS setof record
AS 'MODULE_PATHNAME','crosstab_hash'
LANGUAGE C STABLE STRICT;

CREATE FUNCTION connectby(text,text,text,text,int,text)
RETURNS setof record
AS 'MODULE_PATHNAME','connectby_text'
LANGUAGE C STABLE STRICT;

CREATE FUNCTION connectby(text,text,text,text,int)
RETURNS setof record
AS 'MODULE_PATHNAME','connectby_text'
LANGUAGE C STABLE STRICT;

-- These 2 take the name of a field to ORDER BY as 4th arg (for sorting siblings)
--
-- 这 2 个将要 ORDER BY 的字段名称作为第四个参数（用于对同级进行排序）

CREATE FUNCTION connectby(text,text,text,text,text,int,text)
RETURNS setof record
AS 'MODULE_PATHNAME','connectby_text_serial'
LANGUAGE C STABLE STRICT;

CREATE FUNCTION connectby(text,text,text,text,text,int)
RETURNS setof record
AS 'MODULE_PATHNAME','connectby_text_serial'
LANGUAGE C STABLE STRICT;

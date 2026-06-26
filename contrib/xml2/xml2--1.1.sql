/* contrib/xml2/xml2--1.1.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
--
-- 抱怨脚本是否源自 psql，而不是通过 CREATE EXTENSION
\echo Use "CREATE EXTENSION xml2" to load this file. \quit

--SQL for XML parser
--
--用于 XML 解析器的 SQL

-- deprecated old name for xml_is_well_formed
--
-- 已弃用 xml_is_well_formed 的旧名称
CREATE FUNCTION xml_valid(text) RETURNS bool
AS 'xml_is_well_formed'
LANGUAGE INTERNAL STRICT STABLE PARALLEL SAFE;

CREATE FUNCTION xml_encode_special_chars(text) RETURNS text
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION xpath_string(text,text) RETURNS text
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION xpath_nodeset(text,text,text,text) RETURNS text
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION xpath_number(text,text) RETURNS float4
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION xpath_bool(text,text) RETURNS boolean
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

-- List function
--
-- 列表功能

CREATE FUNCTION xpath_list(text,text,text) RETURNS text
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION xpath_list(text,text) RETURNS text
AS 'SELECT xpath_list($1,$2,'','')'
LANGUAGE SQL STRICT IMMUTABLE PARALLEL SAFE;

-- Wrapper functions for nodeset where no tags needed
--
-- 不需要标签的节点集的包装函数

CREATE FUNCTION xpath_nodeset(text,text)
RETURNS text
AS 'SELECT xpath_nodeset($1,$2,'''','''')'
LANGUAGE SQL STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION xpath_nodeset(text,text,text)
RETURNS text
AS 'SELECT xpath_nodeset($1,$2,'''',$3)'
LANGUAGE SQL STRICT IMMUTABLE PARALLEL SAFE;

-- Table function
--
-- 表功能

CREATE FUNCTION xpath_table(text,text,text,text,text)
RETURNS setof record
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT STABLE PARALLEL SAFE;

-- XSLT functions
--
-- XSLT 函数

CREATE FUNCTION xslt_process(text,text,text)
RETURNS text
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT VOLATILE PARALLEL SAFE;

-- the function checks for the correct argument count
--
-- 该函数检查正确的参数计数
CREATE FUNCTION xslt_process(text,text)
RETURNS text
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

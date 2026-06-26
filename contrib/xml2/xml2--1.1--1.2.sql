/* contrib/xml2/xml2--1.1--1.2.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
--
-- 抱怨脚本是否源自 psql，而不是通过 ALTER EXTENSION
\echo Use "ALTER EXTENSION xml2 UPDATE TO '1.2'" to load this file. \quit

CREATE OR REPLACE FUNCTION xpath_list(text,text) RETURNS text
LANGUAGE SQL STRICT IMMUTABLE PARALLEL SAFE
RETURN xpath_list($1, $2, ','::text);

CREATE OR REPLACE FUNCTION xpath_nodeset(text,text)
RETURNS text
LANGUAGE SQL STRICT IMMUTABLE PARALLEL SAFE
RETURN xpath_nodeset($1, $2, ''::text, ''::text);

CREATE OR REPLACE FUNCTION xpath_nodeset(text,text,text)
RETURNS text
LANGUAGE SQL STRICT IMMUTABLE PARALLEL SAFE
RETURN xpath_nodeset($1, $2, ''::text, $3);

/* contrib/unaccent/unaccent--1.0--1.1.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
--
-- 抱怨脚本是否源自 psql，而不是通过 ALTER EXTENSION
\echo Use "ALTER EXTENSION unaccent UPDATE TO '1.1'" to load this file. \quit

ALTER FUNCTION unaccent(regdictionary, text) PARALLEL SAFE;
ALTER FUNCTION unaccent(text) PARALLEL SAFE;
ALTER FUNCTION unaccent_init(internal) PARALLEL SAFE;
ALTER FUNCTION unaccent_lexize(internal, internal, internal, internal) PARALLEL SAFE;

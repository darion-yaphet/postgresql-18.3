/* contrib/spi/refint--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
--
-- 抱怨脚本是否源自 psql，而不是通过 CREATE EXTENSION
\echo Use "CREATE EXTENSION refint" to load this file. \quit

CREATE FUNCTION check_primary_key()
RETURNS trigger
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION check_foreign_key()
RETURNS trigger
AS 'MODULE_PATHNAME'
LANGUAGE C;

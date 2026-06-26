/* contrib/citext/citext--1.0--1.1.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
--
-- 抱怨脚本是否源自 psql，而不是通过 ALTER EXTENSION
\echo Use "ALTER EXTENSION citext UPDATE TO '1.1'" to load this file. \quit

/* First we have to remove them from the extension
 *
 * 首先我们必须从扩展中删除它们
 */
ALTER EXTENSION citext DROP FUNCTION regexp_matches( citext, citext );
ALTER EXTENSION citext DROP FUNCTION regexp_matches( citext, citext, text );

/* Then we can drop them
 *
 * 然后我们就可以丢掉它们
 */
DROP FUNCTION regexp_matches( citext, citext );
DROP FUNCTION regexp_matches( citext, citext, text );

/* Now redefine
 *
 * 现在重新定义
 */
CREATE FUNCTION regexp_matches( citext, citext ) RETURNS SETOF TEXT[] AS $$
    SELECT pg_catalog.regexp_matches( $1::pg_catalog.text, $2::pg_catalog.text, 'i' );
$$ LANGUAGE SQL IMMUTABLE STRICT ROWS 1;

CREATE FUNCTION regexp_matches( citext, citext, text ) RETURNS SETOF TEXT[] AS $$
    SELECT pg_catalog.regexp_matches( $1::pg_catalog.text, $2::pg_catalog.text, CASE WHEN pg_catalog.strpos($3, 'c') = 0 THEN  $3 || 'i' ELSE $3 END );
$$ LANGUAGE SQL IMMUTABLE STRICT ROWS 10;

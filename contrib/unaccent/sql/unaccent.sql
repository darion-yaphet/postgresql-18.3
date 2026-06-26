/*
 * This test must be run in a database with UTF-8 encoding,
 * because other encodings don't support all the characters used.
 *
 * 此测试必须在使用 UTF-8 编码的数据库中运行，因为其他编码不支持所使用的所有字符。
 */

SELECT getdatabaseencoding() <> 'UTF8'
       AS skip_test \gset
\if :skip_test
\quit
\endif

CREATE EXTENSION unaccent;

SET client_encoding TO 'UTF8';

SELECT unaccent('foobar');
SELECT unaccent('ёлка');
SELECT unaccent('ЁЖИК');
SELECT unaccent('˃˖˗˜');
SELECT unaccent('À');  -- Remove combining diacritical 0x0300
SELECT unaccent('℃℉'); -- degree signs
SELECT unaccent('℗'); -- sound recording copyright
SELECT unaccent('1½'); -- math expression with whitespace
SELECT unaccent('〝'); -- quote

SELECT unaccent('unaccent', 'foobar');
SELECT unaccent('unaccent', 'ёлка');
SELECT unaccent('unaccent', 'ЁЖИК');
SELECT unaccent('unaccent', '˃˖˗˜');
SELECT unaccent('unaccent', 'À');
SELECT unaccent('unaccent', '℃℉');
SELECT unaccent('unaccent', '℗');
SELECT unaccent('unaccent', '1½');
SELECT unaccent('unaccent', '〝');

SELECT ts_lexize('unaccent', 'foobar');
SELECT ts_lexize('unaccent', 'ёлка');
SELECT ts_lexize('unaccent', 'ЁЖИК');
SELECT ts_lexize('unaccent', '˃˖˗˜');
SELECT ts_lexize('unaccent', 'À');
SELECT ts_lexize('unaccent', '℃℉');
SELECT ts_lexize('unaccent', '℗');
SELECT ts_lexize('unaccent', '1½');
SELECT ts_lexize('unaccent', '〝');

-- Controversial case.  Black-Letter Capital H (U+210C) is translated by
--
-- 有争议的案例。  黑体大写字母 H (U+210C) 翻译自
-- Latin-ASCII.xml as 'x', but it should be 'H'.
--
-- Latin-ASCII.xml 为“x”，但它应该是“H”。
SELECT unaccent('ℌ');

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

SET client_encoding = utf8;

-- UTF-8 locale bug on macOS: isspace(0x85) returns true.  \u0105 encodes
--
-- macOS 上的 UTF-8 语言环境错误：isspace(0x85) 返回 true。  \u0105 编码
-- as 0xc4 0x85 in UTF-8; the 0x85 was interpreted here as a whitespace.
--
-- 如 UTF-8 中的 0xc4 0x85； 0x85 在这里被解释为空格。
SELECT E'key\u0105=>value\u0105'::hstore;
SELECT 'keyą=>valueą'::hstore;
SELECT 'ą=>ą'::hstore;
SELECT 'keyąfoo=>valueą'::hstore;

-- More patterns that may depend on isspace() and locales, all discarded.
--
-- 更多可能依赖于 isspace() 和区域设置的模式，全部被丢弃。
SELECT E'key\u000A=>value\u000A'::hstore; -- \n
SELECT E'key\u0009=>value\u0009'::hstore; -- \t
SELECT E'key\u000D=>value\u000D'::hstore; -- \r
SELECT E'key\u000B=>value\u000B'::hstore; -- \v
SELECT E'key\u000C=>value\u000C'::hstore; -- \f

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

set client_encoding = utf8;

-- CREATE EXTENSION IF NOT EXISTS fuzzystrmatch;
--
-- 如果不存在则创建扩展 fuzzystrmatch;

-- Accents
SELECT daitch_mokotoff('Müller');
SELECT daitch_mokotoff('Schäfer');
SELECT daitch_mokotoff('Straßburg');
SELECT daitch_mokotoff('Éregon');

-- Special characters added at https://www.jewishgen.org/InfoFiles/Soundex.html
--
-- 在 https://www.jewishgen.org/InfoFiles/Soundex.html 添加特殊字符
SELECT daitch_mokotoff('gąszczu');
SELECT daitch_mokotoff('brzęczy');
SELECT daitch_mokotoff('ţamas');
SELECT daitch_mokotoff('țamas');

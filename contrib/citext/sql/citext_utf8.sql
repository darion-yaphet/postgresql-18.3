/*
 * This test must be run in a database with UTF-8 encoding
 * and a Unicode-aware locale.
 *
 * 此测试必须在具有 UTF-8 编码和 Unicode 感知区域设置的数据库中运行。
 *
 * Also disable this file for ICU, because the test for the
 * Turkish dotted I is not correct for many ICU locales. citext always
 * uses the default collation, so it's not easy to restrict the test
 * to the "tr-TR-x-icu" collation where it will succeed.
 *
 * 还要为 ICU 禁用此文件，因为土耳其语点线 I 的测试对于许多 ICU 区域设置来说并不正确。 citext 始终使用默认排序规则，因此将测试限制为成功的“tr-TR-x-icu”排序规则并不容易。
 *
 * Also disable for Windows.  It fails similarly, at least in some locales.
 *
 * 对于 Windows 也禁用。  它同样会失败，至少在某些地区是这样。
 */

SELECT getdatabaseencoding() <> 'UTF8' OR
       version() ~ '(Visual C\+\+|mingw32|windows)' OR
       (SELECT (datlocprovider = 'c' AND datctype = 'C') OR datlocprovider = 'i'
        FROM pg_database
        WHERE datname=current_database())
       AS skip_test \gset
\if :skip_test
\quit
\endif

set client_encoding = utf8;

-- CREATE EXTENSION IF NOT EXISTS citext;
--
-- 如果不存在，则创建扩展 citext；

-- Multibyte sanity tests.
--
-- 多字节健全性测试。
SELECT 'À'::citext =  'À'::citext AS t;
SELECT 'À'::citext =  'à'::citext AS t;
SELECT 'À'::text   =  'à'::text   AS f; -- text wins.
SELECT 'À'::citext <> 'B'::citext AS t;

-- Test combining characters making up canonically equivalent strings.
--
-- 测试组合组成规范等效字符串的字符。
SELECT 'Ä'::text   <> 'Ä'::text   AS t;
SELECT 'Ä'::citext <> 'Ä'::citext AS t;

-- Test the Turkish dotted I. The lowercase is a single byte while the
--
-- 测试土耳其语点分I。小写是单个字节，而大写是
-- uppercase is multibyte. This is why the comparison code can't be optimized
--
-- 大写是多字节。这就是为什么比较代码无法优化的原因
-- to compare string lengths.
--
-- 比较字符串长度。
SELECT 'i'::citext = 'İ'::citext AS t;

-- Regression.
SELECT 'láska'::citext <> 'laská'::citext AS t;

SELECT 'Ask Bjørn Hansen'::citext = 'Ask Bjørn Hansen'::citext AS t;
SELECT 'Ask Bjørn Hansen'::citext = 'ASK BJØRN HANSEN'::citext AS t;
SELECT 'Ask Bjørn Hansen'::citext <> 'Ask Bjorn Hansen'::citext AS t;
SELECT 'Ask Bjørn Hansen'::citext <> 'ASK BJORN HANSEN'::citext AS t;
SELECT citext_cmp('Ask Bjørn Hansen'::citext, 'Ask Bjørn Hansen'::citext) = 0 AS t;
SELECT citext_cmp('Ask Bjørn Hansen'::citext, 'ask bjørn hansen'::citext) = 0 AS t;
SELECT citext_cmp('Ask Bjørn Hansen'::citext, 'ASK BJØRN HANSEN'::citext) = 0 AS t;
SELECT citext_cmp('Ask Bjørn Hansen'::citext, 'Ask Bjorn Hansen'::citext) > 0 AS t;
SELECT citext_cmp('Ask Bjorn Hansen'::citext, 'Ask Bjørn Hansen'::citext) < 0 AS t;

-- Test ~<~ and ~<=~
--
-- 测试 ~<~ 和 ~<=~
SELECT 'à'::citext ~<~  'À'::citext AS f;
SELECT 'à'::citext ~<=~ 'À'::citext AS t;

-- Test ~>~ and ~>=~
--
-- 测试 ~>~ 和 ~>=~
SELECT 'à'::citext ~>~  'À'::citext AS f;
SELECT 'à'::citext ~>=~ 'À'::citext AS t;

--
-- Test ISN extension
--
-- 测试 ISN 扩展
--

CREATE EXTENSION isn;

-- Check whether any of our opclasses fail amvalidate
--
-- 检查我们的任何 opclass 是否未通过 amvalidate
-- ... they will, because of missing cross-type operators
--
-- ...他们会的，因为缺少跨类型运算符
SELECT amname, opcname
FROM (SELECT amname, opcname, opc.oid
      FROM pg_opclass opc LEFT JOIN pg_am am ON am.oid = opcmethod
      WHERE opc.oid >= 16384
      ORDER BY 1, 2 OFFSET 0) ss
WHERE NOT amvalidate(oid);

--
-- test valid conversions
--
-- 测试有效转换
--
SELECT '9780123456786'::EAN13, -- old book
       '9790123456785'::EAN13, -- music
       '9791234567896'::EAN13, -- new book
       '9771234567898'::EAN13, -- serial
       '0123456789012'::EAN13, -- upc
       '1234567890128'::EAN13;

SELECT '9780123456786'::ISBN,
       '123456789X'::ISBN,
       '9780123456786'::ISBN13::ISBN,
       '9780123456786'::EAN13::ISBN;

SELECT -- new books, shown as ISBN13 even for ISBN...
       '9791234567896'::ISBN,
       '9791234567896'::ISBN13::ISBN,
       '9791234567896'::EAN13::ISBN;

SELECT '9780123456786'::ISBN13,
       '123456789X'::ISBN13,
       '9791234567896'::ISBN13,
       '9791234567896'::EAN13::ISBN13;

SELECT '9790123456785'::ISMN,
       '9790123456785'::EAN13::ISMN,
       'M123456785'::ISMN,
       'M-1234-5678-5'::ISMN;

SELECT '9790123456785'::ISMN13,
       'M123456785'::ISMN13,
       'M-1234-5678-5'::ISMN13;

SELECT '9771234567003'::ISSN,
       '12345679'::ISSN;

SELECT '9771234567003'::ISSN13,
       '12345679'::ISSN13,
       '9771234567898'::ISSN13,
       '9771234567898'::EAN13::ISSN13;

SELECT '0123456789012'::UPC,
       '0123456789012'::EAN13::UPC;

--
-- test invalid checksums
--
-- 测试无效的校验和
--
SELECT '1234567890'::ISBN;
SELECT 'M123456780'::ISMN;
SELECT '12345670'::ISSN;
SELECT '9780123456780'::ISBN;
SELECT '9791234567890'::ISBN13;
SELECT '0123456789010'::UPC;
SELECT '1234567890120'::EAN13;

--
-- test invalid conversions
--
-- 测试无效转化
--
SELECT '9790123456785'::ISBN; -- not a book
SELECT '9771234567898'::ISBN; -- not a book
SELECT '0123456789012'::ISBN; -- not a book

SELECT '9790123456785'::ISBN13; -- not a book
SELECT '9771234567898'::ISBN13; -- not a book
SELECT '0123456789012'::ISBN13; -- not a book

SELECT '9780123456786'::ISMN; -- not music
SELECT '9771234567898'::ISMN; -- not music
SELECT '9791234567896'::ISMN; -- not music
SELECT '0123456789012'::ISMN; -- not music

SELECT '9780123456786'::ISSN; -- not serial
SELECT '9790123456785'::ISSN; -- not serial
SELECT '9791234567896'::ISSN; -- not serial
SELECT '0123456789012'::ISSN; -- not serial

SELECT '9780123456786'::UPC; -- not a product
SELECT '9771234567898'::UPC; -- not a product
SELECT '9790123456785'::UPC; -- not a product
SELECT '9791234567896'::UPC; -- not a product

SELECT 'postgresql...'::EAN13;
SELECT 'postgresql...'::ISBN;
SELECT 9780123456786::EAN13;
SELECT 9780123456786::ISBN;

--
-- test some comparisons, must yield true
--
-- 测试一些比较，必须得出正确的结果
--
SELECT '12345679'::ISSN = '9771234567003'::EAN13 AS "ok",
       'M-1234-5678-5'::ISMN = '9790123456785'::EAN13 AS "ok",
       '9791234567896'::EAN13 != '123456789X'::ISBN AS "nope";

-- test non-error-throwing input API
--
-- 测试不抛出错误的输入 API
SELECT str as isn, typ as "type",
       pg_input_is_valid(str,typ) as ok,
       errinfo.sql_error_code,
       errinfo.message,
       errinfo.detail,
       errinfo.hint
FROM (VALUES ('9780123456786', 'UPC'),
             ('postgresql...','EAN13'),
             ('9771234567003','ISSN'))
      AS a(str,typ),
     LATERAL pg_input_error_info(a.str, a.typ) as errinfo;

--
-- test weak mode
--
-- 测试弱模式
--
SELECT '2222222222221'::ean13;  -- fail
SET isn.weak TO TRUE;
SELECT '2222222222221'::ean13;
SELECT is_valid('2222222222221'::ean13);
SELECT make_valid('2222222222221'::ean13);

SELECT isn_weak();  -- backwards-compatibility wrappers for accessing the GUC
SELECT isn_weak(false);
SHOW isn.weak;

--
-- cleanup
--
DROP EXTENSION isn;

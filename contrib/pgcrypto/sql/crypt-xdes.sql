--
-- crypt() and gen_salt(): extended des
--
-- crypt() 和 gen_salt()：扩展 des
--

SELECT crypt('', '_J9..j2zz');

SELECT crypt('foox', '_J9..j2zz');

-- check XDES handling of keys longer than 8 chars
--
-- 检查长度超过 8 个字符的密钥的 XDES 处理
SELECT crypt('longlongpassword', '_J9..j2zz');

-- error, salt too short
--
-- 错误，盐太少
SELECT crypt('foox', '_J9..BWH');

-- error, count specified in the second argument is 0
--
-- 错误，第二个参数中指定的计数为 0
SELECT crypt('password', '_........');

-- error, count will wind up still being 0 due to invalid encoding
--
-- 错误，由于编码无效，计数最终仍为 0
-- of the count: only chars ``./0-9A-Za-z' are valid
--
-- 计数：只有字符“./0-9A-Za-z”有效
SELECT crypt('password', '_..!!!!!!');

-- count should be non-zero here, will work
--
-- 这里的计数应该不为零，可以工作
SELECT crypt('password', '_/!!!!!!!');

CREATE TABLE ctest (data text, res text, salt text);
INSERT INTO ctest VALUES ('password', '', '');

UPDATE ctest SET salt = gen_salt('xdes', 1001);
UPDATE ctest SET res = crypt(data, salt);
SELECT res = crypt(data, res) AS "worked"
FROM ctest;

DROP TABLE ctest;

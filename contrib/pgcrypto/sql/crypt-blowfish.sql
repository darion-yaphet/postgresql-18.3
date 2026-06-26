--
-- crypt() and gen_salt(): bcrypt
--
-- crypt() 和 gen_salt()：bcrypt
--

SELECT crypt('', '$2a$06$RQiOJ.3ELirrXwxIZY8q0O');

SELECT crypt('foox', '$2a$06$RQiOJ.3ELirrXwxIZY8q0O');

-- error, salt too short:
--
-- 错误，盐太短：
SELECT crypt('foox', '$2a$');

-- error, first digit of count in salt invalid
--
-- 错误，盐计数的第一位数字无效
SELECT crypt('foox', '$2a$40$RQiOJ.3ELirrXwxIZY8q0O');

-- error, count in salt too small
--
-- 错误，盐计数太小
SELECT crypt('foox', '$2a$00$RQiOJ.3ELirrXwxIZY8q0O');

CREATE TABLE ctest (data text, res text, salt text);
INSERT INTO ctest VALUES ('password', '', '');

UPDATE ctest SET salt = gen_salt('bf', 8);
UPDATE ctest SET res = crypt(data, salt);
SELECT res = crypt(data, res) AS "worked"
FROM ctest;

DROP TABLE ctest;

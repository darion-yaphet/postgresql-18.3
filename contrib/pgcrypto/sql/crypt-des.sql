--
-- crypt() and gen_salt(): crypt-des
--
-- crypt() 和 gen_salt(): crypt-des
--

SELECT crypt('', 'NB');

SELECT crypt('foox', 'NB');

-- We are supposed to pass in a 2-character salt.
--
-- 我们应该传递 2 个字符的盐。
-- error since salt is too short:
--
-- 由于盐太短而出现错误：
SELECT crypt('password', 'a');

CREATE TABLE ctest (data text, res text, salt text);
INSERT INTO ctest VALUES ('password', '', '');

UPDATE ctest SET salt = gen_salt('des');
UPDATE ctest SET res = crypt(data, salt);
SELECT res = crypt(data, res) AS "worked"
FROM ctest;

-- check disabling of built in crypto functions
--
-- 检查内置加密功能的禁用
SET pgcrypto.builtin_crypto_enabled = off;
UPDATE ctest SET salt = gen_salt('des');
UPDATE ctest SET res = crypt(data, salt);
RESET pgcrypto.builtin_crypto_enabled;

DROP TABLE ctest;

--
-- Cast5 cipher
--
-- Cast5密码
--

-- test vectors from RFC2144
--
-- 来自 RFC2144 的测试向量

-- 128 bit key
--
-- 128位密钥
SELECT encrypt('\x0123456789ABCDEF', '\x0123456712345678234567893456789A', 'cast5-ecb/pad:none');

-- 80 bit key
--
-- 80 位密钥
SELECT encrypt('\x0123456789ABCDEF', '\x01234567123456782345', 'cast5-ecb/pad:none');

-- 40 bit key
--
-- 40位密钥
SELECT encrypt('\x0123456789ABCDEF', '\x0123456712', 'cast5-ecb/pad:none');

-- cbc

-- empty data
--
-- 空数据
select encrypt('', 'foo', 'cast5');
-- 10 bytes key
--
-- 10字节密钥
select encrypt('foo', '0123456789', 'cast5');

-- decrypt
select encode(decrypt(encrypt('foo', '0123456', 'cast5'), '0123456', 'cast5'), 'escape');

-- iv
select encrypt_iv('foo', '0123456', 'abcd', 'cast5');
select encode(decrypt_iv('\x384a970695ce016a', '0123456', 'abcd', 'cast5'), 'escape');

-- long message
--
-- 长消息
select encrypt('Lets try a longer message.', '0123456789', 'cast5');
select encode(decrypt(encrypt('Lets try a longer message.', '0123456789', 'cast5'), '0123456789', 'cast5'), 'escape');

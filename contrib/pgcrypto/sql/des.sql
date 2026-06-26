--
-- DES cipher
--
-- DES 密码
--

-- no official test vectors atm
--
-- 没有官方测试向量 atm

-- from blowfish.sql
--
-- 来自blowfish.sql
SELECT encrypt('\x0123456789abcdef', '\xfedcba9876543210', 'des-ecb/pad:none');

-- empty data
--
-- 空数据
select encrypt('', 'foo', 'des');
-- 8 bytes key
--
-- 8字节密钥
select encrypt('foo', '01234589', 'des');

-- decrypt
select encode(decrypt(encrypt('foo', '0123456', 'des'), '0123456', 'des'), 'escape');

-- iv
select encrypt_iv('foo', '0123456', 'abcd', 'des');
select encode(decrypt_iv('\x50735067b073bb93', '0123456', 'abcd', 'des'), 'escape');

-- long message
--
-- 长消息
select encrypt('Lets try a longer message.', '01234567', 'des');
select encode(decrypt(encrypt('Lets try a longer message.', '01234567', 'des'), '01234567', 'des'), 'escape');

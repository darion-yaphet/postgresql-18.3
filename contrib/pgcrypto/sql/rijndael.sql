--
-- AES cipher (aka Rijndael-128, -192, or -256)
--
-- AES 密码（又名 Rijndael-128、-192 或 -256）
--

-- some standard Rijndael testvalues
--
-- 一些标准 Rijndael 测试值
SELECT encrypt(
'\x00112233445566778899aabbccddeeff',
'\x000102030405060708090a0b0c0d0e0f',
'aes-ecb/pad:none');

SELECT encrypt(
'\x00112233445566778899aabbccddeeff',
'\x000102030405060708090a0b0c0d0e0f1011121314151617',
'aes-ecb/pad:none');

SELECT encrypt(
'\x00112233445566778899aabbccddeeff',
'\x000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f',
'aes-ecb/pad:none');

-- cbc
SELECT encrypt(
'\x00112233445566778899aabbccddeeff',
'\x000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f',
'aes-cbc/pad:none');

-- without padding, input not multiple of block size
--
-- 没有填充，输入不是块大小的倍数
SELECT encrypt(
'\x00112233445566778899aabbccddeeff00',
'\x000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f',
'aes-cbc/pad:none');

-- key padding
--
-- 按键填充

SELECT encrypt(
'\x0011223344',
'\x000102030405',
'aes-cbc');

SELECT encrypt(
'\x0011223344',
'\x000102030405060708090a0b0c0d0e0f10111213',
'aes-cbc');

SELECT encrypt(
'\x0011223344',
'\x000102030405060708090a0b0c0d0e0f101112131415161718191a1b',
'aes-cbc');

-- empty data
--
-- 空数据
select encrypt('', 'foo', 'aes');
-- 10 bytes key
--
-- 10字节密钥
select encrypt('foo', '0123456789', 'aes');
-- 22 bytes key
--
-- 22字节密钥
select encrypt('foo', '0123456789012345678901', 'aes');

-- decrypt
select encode(decrypt(encrypt('foo', '0123456', 'aes'), '0123456', 'aes'), 'escape');
-- data not multiple of block size
--
-- 数据不是块大小的倍数
select encode(decrypt(encrypt('foo', '0123456', 'aes') || '\x00'::bytea, '0123456', 'aes'), 'escape');
-- bad padding
--
-- 填充不良
-- (The input value is the result of encrypt_iv('abcdefghijklmnopqrstuvwxyz', '0123456', 'abcd', 'aes')
--
-- （输入值是 encrypt_iv('abcdefghijklmnopqrstuvwxyz', '0123456', 'abcd', 'aes' 的结果)
-- with the 16th byte changed (s/db/eb/) to corrupt the padding of the last block.)
--
-- 第 16 个字节被更改 (s/db/eb/) 以破坏最后一个块的填充。）
select encode(decrypt_iv('\xa21a9c15231465964e3396d32095e67eb52bab05f556a581621dee1b85385789', '0123456', 'abcd', 'aes'), 'escape');

-- iv
select encrypt_iv('foo', '0123456', 'abcd', 'aes');
select encode(decrypt_iv('\x2c24cb7da91d6d5699801268b0f5adad', '0123456', 'abcd', 'aes'), 'escape');

-- long message
--
-- 长消息
select encrypt('Lets try a longer message.', '0123456789', 'aes');
select encode(decrypt(encrypt('Lets try a longer message.', '0123456789', 'aes'), '0123456789', 'aes'), 'escape');

-- cfb
SELECT encrypt(
'\x00112233445566778899aabbccddeeff',
'\x000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f',
'aes-cfb/pad:none');

-- without padding, input not multiple of block size
--
-- 没有填充，输入不是块大小的倍数
SELECT encrypt(
'\x00112233445566778899aabbccddeeff00',
'\x000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f',
'aes-cfb/pad:none');

-- key padding
--
-- 按键填充

SELECT encrypt(
'\x0011223344',
'\x000102030405',
'aes-cfb');

SELECT encrypt(
'\x0011223344',
'\x000102030405060708090a0b0c0d0e0f10111213',
'aes-cfb');

SELECT encrypt(
'\x0011223344',
'\x000102030405060708090a0b0c0d0e0f101112131415161718191a1b',
'aes-cfb');

-- empty data
--
-- 空数据
select encrypt('', 'foo', 'aes-cfb');
-- 10 bytes key
--
-- 10字节密钥
select encrypt('foo', '0123456789', 'aes-cfb');
-- 22 bytes key
--
-- 22字节密钥
select encrypt('foo', '0123456789012345678901', 'aes-cfb');

-- decrypt
select encode(decrypt(encrypt('foo', '0123456', 'aes-cfb'), '0123456', 'aes-cfb'), 'escape');
-- data not multiple of block size
--
-- 数据不是块大小的倍数
select encode(decrypt(encrypt('foo', '0123456', 'aes-cfb') || '\x00'::bytea, '0123456', 'aes-cfb'), 'escape');
-- bad padding
--
-- 填充不良
-- (The input value is the result of encrypt_iv('abcdefghijklmnopqrstuvwxyz', '0123456', 'abcd', 'aes-cfb')
--
-- （输入值是 encrypt_iv('abcdefghijklmnopqrstuvwxyz', '0123456', 'abcd', 'aes-cfb' 的结果）
-- with the 16th byte changed (s/c5/d5/) to corrupt the padding of the last block.)
--
-- 第 16 个字节发生更改 (s/c5/d5/) 以破坏最后一个块的填充。）
select encode(decrypt_iv('\xf9ad6817cb58d31dd9ba6571fbc4f55d56f65b631f0f437cb828', '0123456', 'abcd', 'aes-cfb'), 'escape');

-- iv
select encrypt_iv('foo', '0123456', 'abcd', 'aes-cfb');
select encode(decrypt_iv('\xfea064', '0123456', 'abcd', 'aes-cfb'), 'escape');

-- long message
--
-- 长消息
select encrypt('Lets try a longer message.', '0123456789', 'aes-cfb');
select encode(decrypt(encrypt('Lets try a longer message.', '0123456789', 'aes-cfb'), '0123456789', 'aes-cfb'), 'escape');

--
-- PGP compression support
--
-- PGP 压缩支持
--

select pgp_sym_decrypt(dearmor('
-----BEGIN PGP MESSAGE-----
--
-----开始 PGP 消息-----

ww0ECQMCsci6AdHnELlh0kQB4jFcVwHMJg0Bulop7m3Mi36s15TAhBo0AnzIrRFrdLVCkKohsS6+
DMcmR53SXfLoDJOv/M8uKj3QSq7oWNIp95pxfA==
=tbSn
-----END PGP MESSAGE-----
--
-----PGP 消息结束-----
'), 'key', 'expect-compress-algo=1');

select pgp_sym_decrypt(
	pgp_sym_encrypt('Secret message', 'key', 'compress-algo=0'),
	'key', 'expect-compress-algo=0');

select pgp_sym_decrypt(
	pgp_sym_encrypt('Secret message', 'key', 'compress-algo=1'),
	'key', 'expect-compress-algo=1');

select pgp_sym_decrypt(
	pgp_sym_encrypt('Secret message', 'key', 'compress-algo=2'),
	'key', 'expect-compress-algo=2');

-- level=0 should turn compression off
--
-- level=0 应该关闭压缩
select pgp_sym_decrypt(
	pgp_sym_encrypt('Secret message', 'key',
			'compress-algo=2, compress-level=0'),
	'key', 'expect-compress-algo=0');

-- check corner case involving an input string of 16kB, as per bug #16476.
--
-- 根据 bug #16476 检查涉及 16kB 输入字符串的极端情况。
SELECT setseed(0);
WITH random_string AS
(
  -- This generates a random string of 16366 bytes.  This is chosen
  --
  -- 这会生成一个 16366 字节的随机字符串。  这是选择的
  -- as random so that it does not get compressed, and the decompression
  --
  -- 作为随机，这样它就不会被压缩，并且解压
  -- would work on a string with the same length as the origin, making the
  --
  -- 将作用于与原点长度相同的字符串，使得
  -- test behavior more predictable.  lpad() ensures that the generated
  --
  -- 测试行为更加可预测。  lpad() 确保生成的
  -- hexadecimal value is completed by extra zero characters if random()
  --
  -- 如果 random()，则十六进制值由额外的零字符完成
  -- has generated a value strictly lower than 16.
  --
  -- 生成的值严格低于 16。
  SELECT string_agg(decode(lpad(to_hex((random()*256)::int), 2, '0'), 'hex'), '') as bytes
    FROM generate_series(0, 16365)
)
SELECT bytes =
    pgp_sym_decrypt_bytea(
      pgp_sym_encrypt_bytea(bytes, 'key',
                            'compress-algo=1,compress-level=1'),
                            'key', 'expect-compress-algo=1')
    AS is_same
  FROM random_string;

--
-- PGP encrypt
--
-- PGP加密
--

select pgp_sym_decrypt(pgp_sym_encrypt('Secret.', 'key'), 'key');

-- check whether the defaults are ok
--
-- 检查默认值是否ok
select pgp_sym_decrypt(pgp_sym_encrypt('Secret.', 'key'),
	'key', 'expect-cipher-algo=aes128,
		expect-disable-mdc=0,
		expect-sess-key=0,
		expect-s2k-mode=3,
		expect-s2k-digest-algo=sha1,
		expect-compress-algo=0
		');

-- maybe the expect- stuff simply does not work
--
-- 也许期望的东西根本不起作用
select pgp_sym_decrypt(pgp_sym_encrypt('Secret.', 'key'),
	'key', 'expect-cipher-algo=bf,
		expect-disable-mdc=1,
		expect-sess-key=1,
		expect-s2k-mode=0,
		expect-s2k-digest-algo=md5,
		expect-compress-algo=1
		');

-- bytea as text
--
-- 字节作为文本
select pgp_sym_decrypt(pgp_sym_encrypt_bytea('Binary', 'baz'), 'baz');

-- text as bytea
--
-- 文本为 bytea
select encode(pgp_sym_decrypt_bytea(pgp_sym_encrypt('Text', 'baz'), 'baz'), 'escape');


-- algorithm change
--
-- 算法改变
select pgp_sym_decrypt(
	pgp_sym_encrypt('Secret.', 'key', 'cipher-algo=bf'),
	'key', 'expect-cipher-algo=bf');
select pgp_sym_decrypt(
	pgp_sym_encrypt('Secret.', 'key', 'cipher-algo=aes'),
	'key', 'expect-cipher-algo=aes128');
select pgp_sym_decrypt(
	pgp_sym_encrypt('Secret.', 'key', 'cipher-algo=aes192'),
	'key', 'expect-cipher-algo=aes192');

-- s2k change
--
-- s2k 改变
select pgp_sym_decrypt(
	pgp_sym_encrypt('Secret.', 'key', 's2k-mode=0'),
	'key', 'expect-s2k-mode=0');
select pgp_sym_decrypt(
	pgp_sym_encrypt('Secret.', 'key', 's2k-mode=1'),
	'key', 'expect-s2k-mode=1');
select pgp_sym_decrypt(
	pgp_sym_encrypt('Secret.', 'key', 's2k-mode=3'),
	'key', 'expect-s2k-mode=3');

-- s2k count change
--
-- s2k 计数变化
select pgp_sym_decrypt(
	pgp_sym_encrypt('Secret.', 'key', 's2k-count=1024'),
	'key', 'expect-s2k-count=1024');
-- s2k_count rounds up
--
-- s2k_count 向上舍入
select pgp_sym_decrypt(
	pgp_sym_encrypt('Secret.', 'key', 's2k-count=65000000'),
	'key', 'expect-s2k-count=65000000');

-- s2k digest change
--
-- s2k 摘要变化
select pgp_sym_decrypt(
		pgp_sym_encrypt('Secret.', 'key', 's2k-digest-algo=sha1'),
	'key', 'expect-s2k-digest-algo=sha1');

-- sess key
--
-- 会话密钥
select pgp_sym_decrypt(
	pgp_sym_encrypt('Secret.', 'key', 'sess-key=0'),
	'key', 'expect-sess-key=0');
select pgp_sym_decrypt(
	pgp_sym_encrypt('Secret.', 'key', 'sess-key=1'),
	'key', 'expect-sess-key=1');
select pgp_sym_decrypt(
	pgp_sym_encrypt('Secret.', 'key', 'sess-key=1, cipher-algo=bf'),
	'key', 'expect-sess-key=1, expect-cipher-algo=bf');
select pgp_sym_decrypt(
	pgp_sym_encrypt('Secret.', 'key', 'sess-key=1, cipher-algo=aes192'),
	'key', 'expect-sess-key=1, expect-cipher-algo=aes192');
select pgp_sym_decrypt(
	pgp_sym_encrypt('Secret.', 'key', 'sess-key=1, cipher-algo=aes256'),
	'key', 'expect-sess-key=1, expect-cipher-algo=aes256');

-- no mdc
--
-- 没有MDC
select pgp_sym_decrypt(
		pgp_sym_encrypt('Secret.', 'key', 'disable-mdc=1'),
	'key', 'expect-disable-mdc=1');

-- crlf
select pgp_sym_decrypt_bytea(
	pgp_sym_encrypt(E'1\n2\n3\r\n', 'key', 'convert-crlf=1'),
	'key');

-- conversion should be lossless
--
-- 转换应该是无损的
select digest(pgp_sym_decrypt(
  pgp_sym_encrypt(E'\r\n0\n1\r\r\n\n2\r', 'key', 'convert-crlf=1'),
	'key', 'convert-crlf=1'), 'sha1') as result,
  digest(E'\r\n0\n1\r\r\n\n2\r', 'sha1') as expect;

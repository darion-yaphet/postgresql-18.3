--
-- PGP encrypt using MD5
--
-- 使用 MD5 进行 PGP 加密
--

select pgp_sym_decrypt(
	pgp_sym_encrypt('Secret.', 'key', 's2k-digest-algo=md5'),
	'key', 'expect-s2k-digest-algo=md5');

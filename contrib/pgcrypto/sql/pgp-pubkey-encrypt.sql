--
-- PGP Public Key Encryption
--
-- PGP 公钥加密
--

-- successful encrypt/decrypt
--
-- 加密/解密成功
select pgp_pub_decrypt(
	pgp_pub_encrypt('Secret msg', dearmor(pubkey)),
	dearmor(seckey))
from keytbl where keytbl.id=1;

select pgp_pub_decrypt(
		pgp_pub_encrypt('Secret msg', dearmor(pubkey)),
		dearmor(seckey))
from keytbl where keytbl.id=2;

select pgp_pub_decrypt(
		pgp_pub_encrypt('Secret msg', dearmor(pubkey)),
		dearmor(seckey))
from keytbl where keytbl.id=3;

select pgp_pub_decrypt(
		pgp_pub_encrypt('Secret msg', dearmor(pubkey)),
		dearmor(seckey))
from keytbl where keytbl.id=6;

-- try with rsa-sign only
--
-- 仅尝试使用 rsa 签名
select pgp_pub_decrypt(
		pgp_pub_encrypt('Secret msg', dearmor(pubkey)),
		dearmor(seckey))
from keytbl where keytbl.id=4;

-- try with secret key
--
-- 尝试使用密钥
select pgp_pub_decrypt(
		pgp_pub_encrypt('Secret msg', dearmor(seckey)),
		dearmor(seckey))
from keytbl where keytbl.id=1;

-- does text-to-bytea works
--
-- 文本到 bytea 是否有效
select encode(pgp_pub_decrypt_bytea(
		pgp_pub_encrypt('Secret msg', dearmor(pubkey)),
		dearmor(seckey)), 'escape')
from keytbl where keytbl.id=1;

-- and bytea-to-text?
--
-- 和字节到文本？
select pgp_pub_decrypt(
		pgp_pub_encrypt_bytea('Secret msg', dearmor(pubkey)),
		dearmor(seckey))
from keytbl where keytbl.id=1;

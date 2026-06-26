/* contrib/pgcrypto/pgcrypto--1.3.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
--
-- 抱怨脚本是否源自 psql，而不是通过 CREATE EXTENSION
\echo Use "CREATE EXTENSION pgcrypto" to load this file. \quit

CREATE FUNCTION digest(text, text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pg_digest'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION digest(bytea, text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pg_digest'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION hmac(text, text, text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pg_hmac'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION hmac(bytea, bytea, text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pg_hmac'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION crypt(text, text)
RETURNS text
AS 'MODULE_PATHNAME', 'pg_crypt'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gen_salt(text)
RETURNS text
AS 'MODULE_PATHNAME', 'pg_gen_salt'
LANGUAGE C VOLATILE STRICT PARALLEL SAFE;

CREATE FUNCTION gen_salt(text, int4)
RETURNS text
AS 'MODULE_PATHNAME', 'pg_gen_salt_rounds'
LANGUAGE C VOLATILE STRICT PARALLEL SAFE;

CREATE FUNCTION encrypt(bytea, bytea, text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pg_encrypt'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION decrypt(bytea, bytea, text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pg_decrypt'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION encrypt_iv(bytea, bytea, bytea, text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pg_encrypt_iv'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION decrypt_iv(bytea, bytea, bytea, text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pg_decrypt_iv'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gen_random_bytes(int4)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pg_random_bytes'
LANGUAGE C VOLATILE STRICT PARALLEL SAFE;

CREATE FUNCTION gen_random_uuid()
RETURNS uuid
AS 'MODULE_PATHNAME', 'pg_random_uuid'
LANGUAGE C VOLATILE PARALLEL SAFE;

--
-- pgp_sym_encrypt(data, key)
--
-- pgp_sym_encrypt（数据，密钥）
--
CREATE FUNCTION pgp_sym_encrypt(text, text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pgp_sym_encrypt_text'
LANGUAGE C STRICT PARALLEL SAFE;

CREATE FUNCTION pgp_sym_encrypt_bytea(bytea, text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pgp_sym_encrypt_bytea'
LANGUAGE C STRICT PARALLEL SAFE;

--
-- pgp_sym_encrypt(data, key, args)
--
-- pgp_sym_encrypt（数据，密钥，参数）
--
CREATE FUNCTION pgp_sym_encrypt(text, text, text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pgp_sym_encrypt_text'
LANGUAGE C STRICT PARALLEL SAFE;

CREATE FUNCTION pgp_sym_encrypt_bytea(bytea, text, text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pgp_sym_encrypt_bytea'
LANGUAGE C STRICT PARALLEL SAFE;

--
-- pgp_sym_decrypt(data, key)
--
-- pgp_sym_decrypt（数据，密钥）
--
CREATE FUNCTION pgp_sym_decrypt(bytea, text)
RETURNS text
AS 'MODULE_PATHNAME', 'pgp_sym_decrypt_text'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION pgp_sym_decrypt_bytea(bytea, text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pgp_sym_decrypt_bytea'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

--
-- pgp_sym_decrypt(data, key, args)
--
-- pgp_sym_decrypt（数据，密钥，参数）
--
CREATE FUNCTION pgp_sym_decrypt(bytea, text, text)
RETURNS text
AS 'MODULE_PATHNAME', 'pgp_sym_decrypt_text'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION pgp_sym_decrypt_bytea(bytea, text, text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pgp_sym_decrypt_bytea'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

--
-- pgp_pub_encrypt(data, key)
--
-- pgp_pub_encrypt（数据，密钥）
--
CREATE FUNCTION pgp_pub_encrypt(text, bytea)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pgp_pub_encrypt_text'
LANGUAGE C STRICT PARALLEL SAFE;

CREATE FUNCTION pgp_pub_encrypt_bytea(bytea, bytea)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pgp_pub_encrypt_bytea'
LANGUAGE C STRICT PARALLEL SAFE;

--
-- pgp_pub_encrypt(data, key, args)
--
-- pgp_pub_encrypt（数据，密钥，参数）
--
CREATE FUNCTION pgp_pub_encrypt(text, bytea, text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pgp_pub_encrypt_text'
LANGUAGE C STRICT PARALLEL SAFE;

CREATE FUNCTION pgp_pub_encrypt_bytea(bytea, bytea, text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pgp_pub_encrypt_bytea'
LANGUAGE C STRICT PARALLEL SAFE;

--
-- pgp_pub_decrypt(data, key)
--
-- pgp_pub_decrypt（数据，密钥）
--
CREATE FUNCTION pgp_pub_decrypt(bytea, bytea)
RETURNS text
AS 'MODULE_PATHNAME', 'pgp_pub_decrypt_text'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION pgp_pub_decrypt_bytea(bytea, bytea)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pgp_pub_decrypt_bytea'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

--
-- pgp_pub_decrypt(data, key, psw)
--
-- pgp_pub_decrypt（数据，密钥，密码）
--
CREATE FUNCTION pgp_pub_decrypt(bytea, bytea, text)
RETURNS text
AS 'MODULE_PATHNAME', 'pgp_pub_decrypt_text'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION pgp_pub_decrypt_bytea(bytea, bytea, text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pgp_pub_decrypt_bytea'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

--
-- pgp_pub_decrypt(data, key, psw, arg)
--
-- pgp_pub_decrypt（数据，密钥，psw，arg）
--
CREATE FUNCTION pgp_pub_decrypt(bytea, bytea, text, text)
RETURNS text
AS 'MODULE_PATHNAME', 'pgp_pub_decrypt_text'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION pgp_pub_decrypt_bytea(bytea, bytea, text, text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pgp_pub_decrypt_bytea'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

--
-- PGP key ID
--
-- PGP 密钥 ID
--
CREATE FUNCTION pgp_key_id(bytea)
RETURNS text
AS 'MODULE_PATHNAME', 'pgp_key_id_w'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

--
-- pgp armor
--
-- PGP装甲
--
CREATE FUNCTION armor(bytea)
RETURNS text
AS 'MODULE_PATHNAME', 'pg_armor'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION armor(bytea, text[], text[])
RETURNS text
AS 'MODULE_PATHNAME', 'pg_armor'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION dearmor(text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pg_dearmor'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION pgp_armor_headers(text, key OUT text, value OUT text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pgp_armor_headers'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

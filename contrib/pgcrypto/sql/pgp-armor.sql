--
-- PGP Armor
--
-- PGP装甲
--

select armor('');
select armor('test');
select encode(dearmor(armor('')), 'escape');
select encode(dearmor(armor('zooka')), 'escape');

select armor('0123456789abcdef0123456789abcdef0123456789abcdef
0123456789abcdef0123456789abcdef0123456789abcdef');

-- lots formatting
--
-- 批量格式化
select encode(dearmor(' a pgp msg:

-----BEGIN PGP MESSAGE-----
--
-----开始 PGP 消息-----
Comment: Some junk

em9va2E=

  =D5cR

-----END PGP MESSAGE-----'), 'escape');
--
-----结束 PGP 消息-----'), '转义');

-- lots messages
--
-- 很多消息
select encode(dearmor('
wrong packet:
  -----BEGIN PGP MESSAGE-----
  --
  -----开始 PGP 消息-----

  d3Jvbmc=
  =vCYP
  -----END PGP MESSAGE-----
  --
  -----PGP 消息结束-----

right packet:
-----BEGIN PGP MESSAGE-----
--
-----开始 PGP 消息-----

cmlnaHQ=
=nbpj
-----END PGP MESSAGE-----
--
-----PGP 消息结束-----

use only first packet
-----BEGIN PGP MESSAGE-----
--
-----开始 PGP 消息-----

d3Jvbmc=
=vCYP
-----END PGP MESSAGE-----
--
-----PGP 消息结束-----
'), 'escape');

-- bad crc
--
-- 错误的CRC
select dearmor('
-----BEGIN PGP MESSAGE-----
--
-----开始 PGP 消息-----

em9va2E=
=ZZZZ
-----END PGP MESSAGE-----
--
-----PGP 消息结束-----
');

-- corrupt (no space after the colon)
--
-- 损坏（冒号后没有空格）
select * from pgp_armor_headers('
-----BEGIN PGP MESSAGE-----
--
-----开始 PGP 消息-----
foo:

em9va2E=
=ZZZZ
-----END PGP MESSAGE-----
--
-----PGP 消息结束-----
');

-- corrupt (no empty line)
--
-- 损坏（无空行）
select * from pgp_armor_headers('
-----BEGIN PGP MESSAGE-----
--
-----开始 PGP 消息-----
em9va2E=
=ZZZZ
-----END PGP MESSAGE-----
--
-----PGP 消息结束-----
');

-- no headers
--
-- 没有标题
select * from pgp_armor_headers('
-----BEGIN PGP MESSAGE-----
--
-----开始 PGP 消息-----

em9va2E=
=ZZZZ
-----END PGP MESSAGE-----
--
-----PGP 消息结束-----
');

-- header with empty value
--
-- 标题为空值
select * from pgp_armor_headers('
-----BEGIN PGP MESSAGE-----
--
-----开始 PGP 消息-----
foo: 

em9va2E=
=ZZZZ
-----END PGP MESSAGE-----
--
-----PGP 消息结束-----
');

-- simple
select * from pgp_armor_headers('
-----BEGIN PGP MESSAGE-----
--
-----开始 PGP 消息-----
fookey: foovalue
barkey: barvalue

em9va2E=
=ZZZZ
-----END PGP MESSAGE-----
--
-----PGP 消息结束-----
');

-- insane keys, part 1
--
-- 疯狂的钥匙，第 1 部分
select * from pgp_armor_headers('
-----BEGIN PGP MESSAGE-----
--
-----开始 PGP 消息-----
insane:key : 

em9va2E=
=ZZZZ
-----END PGP MESSAGE-----
--
-----PGP 消息结束-----
');

-- insane keys, part 2
--
-- 疯狂的钥匙，第 2 部分
select * from pgp_armor_headers('
-----BEGIN PGP MESSAGE-----
--
-----开始 PGP 消息-----
insane:key : text value here

em9va2E=
=ZZZZ
-----END PGP MESSAGE-----
--
-----PGP 消息结束-----
');

-- long value
--
-- 长值
select * from pgp_armor_headers('
-----BEGIN PGP MESSAGE-----
--
-----开始 PGP 消息-----
long: this value is more than 76 characters long, but it should still parse correctly as that''s permitted by RFC 4880

em9va2E=
=ZZZZ
-----END PGP MESSAGE-----
--
-----PGP 消息结束-----
');

-- long value, split up
--
-- 长值，分割
select * from pgp_armor_headers('
-----BEGIN PGP MESSAGE-----
--
-----开始 PGP 消息-----
long: this value is more than 76 characters long, but it should still 
long: parse correctly as that''s permitted by RFC 4880

em9va2E=
=ZZZZ
-----END PGP MESSAGE-----
--
-----PGP 消息结束-----
');

-- long value, split up, part 2
--
-- 长值，拆分，第 2 部分
select * from pgp_armor_headers('
-----BEGIN PGP MESSAGE-----
--
-----开始 PGP 消息-----
long: this value is more than 
long: 76 characters long, but it should still 
long: parse correctly as that''s permitted by RFC 4880

em9va2E=
=ZZZZ
-----END PGP MESSAGE-----
--
-----PGP 消息结束-----
');

-- long value, split up, part 3
--
-- 长值，拆分，第 3 部分
select * from pgp_armor_headers('
-----BEGIN PGP MESSAGE-----
--
-----开始 PGP 消息-----
emptykey: 
long: this value is more than 
emptykey: 
long: 76 characters long, but it should still 
emptykey: 
long: parse correctly as that''s permitted by RFC 4880
emptykey: 

em9va2E=
=ZZZZ
-----END PGP MESSAGE-----
--
-----PGP 消息结束-----
');

select * from pgp_armor_headers('
-----BEGIN PGP MESSAGE-----
--
-----开始 PGP 消息-----
Comment: dat1.blowfish.sha1.mdc.s2k3.z0

jA0EBAMCfFNwxnvodX9g0jwB4n4s26/g5VmKzVab1bX1SmwY7gvgvlWdF3jKisvS
yA6Ce1QTMK3KdL2MPfamsTUSAML8huCJMwYQFfE=
=JcP+
-----END PGP MESSAGE-----
--
-----PGP 消息结束-----
');

-- test CR+LF line endings
--
-- 测试 CR+LF 行结尾
select * from pgp_armor_headers(replace('
-----BEGIN PGP MESSAGE-----
--
-----开始 PGP 消息-----
fookey: foovalue
barkey: barvalue

em9va2E=
=ZZZZ
-----END PGP MESSAGE-----
--
-----PGP 消息结束-----
', E'\n', E'\r\n'));

-- test header generation
--
-- 测试标头生成
select armor('zooka', array['foo'], array['bar']);
select armor('zooka', array['Version', 'Comment'], array['Created by pgcrypto', 'PostgreSQL, the world''s most advanced open source database']);
select * from pgp_armor_headers(
  armor('zooka', array['Version', 'Comment'],
                 array['Created by pgcrypto', 'PostgreSQL, the world''s most advanced open source database']));

-- error/corner cases
--
-- 错误/极端情况
select armor('', array['foo'], array['too', 'many']);
select armor('', array['too', 'many'], array['foo']);
select armor('', array[['']], array['foo']);
select armor('', array['foo'], array[['']]);
select armor('', array[null], array['foo']);
select armor('', array['foo'], array[null]);
select armor('', '[0:0]={"foo"}', array['foo']);
select armor('', array['foo'], '[0:0]={"foo"}');
select armor('', array[E'embedded\nnewline'], array['foo']);
select armor('', array['foo'], array[E'embedded\nnewline']);
select armor('', array['embedded: colon+space'], array['foo']);

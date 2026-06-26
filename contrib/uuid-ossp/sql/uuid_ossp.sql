CREATE EXTENSION "uuid-ossp";

SELECT uuid_nil();
SELECT uuid_ns_dns();
SELECT uuid_ns_url();
SELECT uuid_ns_oid();
SELECT uuid_ns_x500();

-- some quick and dirty field extraction functions
--
-- 一些快速而肮脏的字段提取功能

-- this is actually timestamp concatenated with clock sequence, per RFC 4122
--
-- 根据 RFC 4122，这实际上是与时钟序列连接的时间戳
CREATE FUNCTION uuid_timestamp_bits(uuid) RETURNS varbit AS
$$ SELECT ('x' || substr($1::text, 15, 4) || substr($1::text, 10, 4) ||
           substr($1::text, 1, 8) || substr($1::text, 20, 4))::bit(80)
          & x'0FFFFFFFFFFFFFFF3FFF' $$
LANGUAGE SQL STRICT IMMUTABLE;

CREATE FUNCTION uuid_version_bits(uuid) RETURNS varbit AS
$$ SELECT ('x' || substr($1::text, 15, 2))::bit(8) & '11110000' $$
LANGUAGE SQL STRICT IMMUTABLE;

CREATE FUNCTION uuid_reserved_bits(uuid) RETURNS varbit AS
$$ SELECT ('x' || substr($1::text, 20, 2))::bit(8) & '11000000' $$
LANGUAGE SQL STRICT IMMUTABLE;

CREATE FUNCTION uuid_multicast_bit(uuid) RETURNS bool AS
$$ SELECT (('x' || substr($1::text, 25, 2))::bit(8) & '00000001') != '00000000' $$
LANGUAGE SQL STRICT IMMUTABLE;

CREATE FUNCTION uuid_local_admin_bit(uuid) RETURNS bool AS
$$ SELECT (('x' || substr($1::text, 25, 2))::bit(8) & '00000010') != '00000000' $$
LANGUAGE SQL STRICT IMMUTABLE;

CREATE FUNCTION uuid_node(uuid) RETURNS text AS
$$ SELECT substr($1::text, 25) $$
LANGUAGE SQL STRICT IMMUTABLE;

-- Ideally, the multicast bit would never be set in V1 output, but the
--
-- 理想情况下，多播位永远不会在 V1 输出中设置，但
-- UUID library may fall back to MC if it can't get the system MAC address.
--
-- 如果无法获取系统 MAC 地址，UUID 库可能会回退到 MC。
-- Also, the local-admin bit might be set (if so, we're probably inside a VM).
--
-- 另外，本地管理位可能已设置（如果是这样，我们可能位于虚拟机内）。
-- So we can't test either bit here.
--
-- 所以我们不能在这里测试任何一位。
SELECT uuid_version_bits(uuid_generate_v1()),
       uuid_reserved_bits(uuid_generate_v1());

-- Although RFC 4122 only requires the multicast bit to be set in V1MC style
--
-- 尽管 RFC 4122 仅要求以 V1MC 方式设置多播位
-- UUIDs, our implementation always sets the local-admin bit as well.
--
-- UUID，我们的实现也始终设置本地管理位。
SELECT uuid_version_bits(uuid_generate_v1mc()),
       uuid_reserved_bits(uuid_generate_v1mc()),
       uuid_multicast_bit(uuid_generate_v1mc()),
       uuid_local_admin_bit(uuid_generate_v1mc());

-- timestamp+clock sequence should be monotonic increasing in v1
--
-- 时间戳+时钟序列在 v1 中应该是单调递增的
SELECT uuid_timestamp_bits(uuid_generate_v1()) < uuid_timestamp_bits(uuid_generate_v1());
SELECT uuid_timestamp_bits(uuid_generate_v1mc()) < uuid_timestamp_bits(uuid_generate_v1mc());

-- Ideally, the node value is stable in V1 addresses, but OSSP UUID
--
-- 理想情况下，节点值在V1地址中是稳定的，但OSSP UUID
-- falls back to V1MC behavior if it can't get the system MAC address.
--
-- 如果无法获取系统 MAC 地址，则回退到 V1MC 行为。
SELECT CASE WHEN uuid_multicast_bit(uuid_generate_v1()) AND
                 uuid_local_admin_bit(uuid_generate_v1()) THEN
         true -- punt, no test
       ELSE
         uuid_node(uuid_generate_v1()) = uuid_node(uuid_generate_v1())
       END;

-- In any case, V1MC node addresses should be random.
--
-- 无论如何，V1MC节点地址应该是随机的。
SELECT uuid_node(uuid_generate_v1()) <> uuid_node(uuid_generate_v1mc());
SELECT uuid_node(uuid_generate_v1mc()) <> uuid_node(uuid_generate_v1mc());

SELECT uuid_generate_v3(uuid_ns_dns(), 'www.widgets.com');
SELECT uuid_generate_v5(uuid_ns_dns(), 'www.widgets.com');

SELECT uuid_version_bits(uuid_generate_v4()),
       uuid_reserved_bits(uuid_generate_v4());

SELECT uuid_generate_v4() <> uuid_generate_v4();

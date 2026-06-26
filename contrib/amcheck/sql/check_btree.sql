CREATE TABLE bttest_a(id int8);
CREATE TABLE bttest_b(id int8);
CREATE TABLE bttest_multi(id int8, data int8);
CREATE TABLE delete_test_table (a bigint, b bigint, c bigint, d bigint);

-- Stabilize tests
--
-- 稳定测试
ALTER TABLE bttest_a SET (autovacuum_enabled = false);
ALTER TABLE bttest_b SET (autovacuum_enabled = false);
ALTER TABLE bttest_multi SET (autovacuum_enabled = false);
ALTER TABLE delete_test_table SET (autovacuum_enabled = false);

INSERT INTO bttest_a SELECT * FROM generate_series(1, 100000);
INSERT INTO bttest_b SELECT * FROM generate_series(100000, 1, -1);
INSERT INTO bttest_multi SELECT i, i%2  FROM generate_series(1, 100000) as i;

CREATE INDEX bttest_a_idx ON bttest_a USING btree (id) WITH (deduplicate_items = ON);
CREATE INDEX bttest_b_idx ON bttest_b USING btree (id);
CREATE UNIQUE INDEX bttest_multi_idx ON bttest_multi
USING btree (id) INCLUDE (data);

CREATE ROLE regress_bttest_role;

-- verify permissions are checked (error due to function not callable)
--
-- 验证权限已检查（由于函数不可调用而导致错误）
SET ROLE regress_bttest_role;
SELECT bt_index_check('bttest_a_idx'::regclass);
SELECT bt_index_parent_check('bttest_a_idx'::regclass);
RESET ROLE;

-- we, intentionally, don't check relation permissions - it's useful
--
-- 我们故意不检查关系权限 - 这很有用
-- to run this cluster-wide with a restricted account, and as tested
--
-- 使用受限帐户在集群范围内运行这个，并经过测试
-- above explicit permission has to be granted for that.
--
-- 为此必须获得上述明确许可。
GRANT EXECUTE ON FUNCTION bt_index_check(regclass) TO regress_bttest_role;
GRANT EXECUTE ON FUNCTION bt_index_parent_check(regclass) TO regress_bttest_role;
GRANT EXECUTE ON FUNCTION bt_index_check(regclass, boolean) TO regress_bttest_role;
GRANT EXECUTE ON FUNCTION bt_index_parent_check(regclass, boolean) TO regress_bttest_role;
SET ROLE regress_bttest_role;
SELECT bt_index_check('bttest_a_idx');
SELECT bt_index_parent_check('bttest_a_idx');
RESET ROLE;

-- verify plain tables are rejected (error)
--
-- 验证普通表被拒绝（错误）
SELECT bt_index_check('bttest_a');
SELECT bt_index_parent_check('bttest_a');

-- verify non-existing indexes are rejected (error)
--
-- 验证不存在的索引被拒绝（错误）
SELECT bt_index_check(17);
SELECT bt_index_parent_check(17);

-- verify wrong index types are rejected (error)
--
-- 验证错误的索引类型被拒绝（错误）
BEGIN;
CREATE INDEX bttest_a_brin_idx ON bttest_a USING brin(id);
SELECT bt_index_parent_check('bttest_a_brin_idx');
ROLLBACK;

-- normal check outside of xact
--
-- xact 之外的正常检查
SELECT bt_index_check('bttest_a_idx');
-- more expansive tests
--
-- 更广泛的测试
SELECT bt_index_check('bttest_a_idx', true);
SELECT bt_index_parent_check('bttest_b_idx', true);

BEGIN;
SELECT bt_index_check('bttest_a_idx');
SELECT bt_index_parent_check('bttest_b_idx');
-- make sure we don't have any leftover locks
--
-- 确保我们没有任何剩余的锁
SELECT * FROM pg_locks
WHERE relation = ANY(ARRAY['bttest_a', 'bttest_a_idx', 'bttest_b', 'bttest_b_idx']::regclass[])
    AND pid = pg_backend_pid();
COMMIT;

-- Deduplication
TRUNCATE bttest_a;
INSERT INTO bttest_a SELECT 42 FROM generate_series(1, 2000);
SELECT bt_index_check('bttest_a_idx', true);

-- normal check outside of xact for index with included columns
--
-- 在 xact 之外对包含列的索引进行正常检查
SELECT bt_index_check('bttest_multi_idx');
-- more expansive tests for index with included columns
--
-- 对包含列的索引进行更广泛的测试
SELECT bt_index_parent_check('bttest_multi_idx', true, true);

-- repeat expansive tests for index built using insertions
--
-- 对使用插入构建的索引重复进行广泛的测试
TRUNCATE bttest_multi;
INSERT INTO bttest_multi SELECT i, i%2  FROM generate_series(1, 100000) as i;
SELECT bt_index_parent_check('bttest_multi_idx', true, true);

--
-- Test for multilevel page deletion/downlink present checks, and rootdescend
--
-- 测试多级页面删除/下行链路存在检查和 rootdescend
-- checks
--
INSERT INTO delete_test_table SELECT i, 1, 2, 3 FROM generate_series(1,80000) i;
ALTER TABLE delete_test_table ADD PRIMARY KEY (a,b,c,d);
-- Delete most entries, and vacuum, deleting internal pages and creating "fast
--
-- 删除大部分条目，并清理、删除内部页面并创建“快速
-- root"
--
-- 根”
DELETE FROM delete_test_table WHERE a < 79990;
VACUUM delete_test_table;
SELECT bt_index_parent_check('delete_test_table_pkey', true);

--
-- BUG #15597: must not assume consistent input toasting state when forming
--
-- BUG #15597：成型时不得假设输入烘烤状态一致
-- tuple.  Bloom filter must fingerprint normalized index tuple representation.
--
-- 元组。  布隆过滤器必须对标准化索引元组表示进行指纹识别。
--
CREATE TABLE toast_bug(buggy text);
ALTER TABLE toast_bug ALTER COLUMN buggy SET STORAGE extended;
CREATE INDEX toasty ON toast_bug(buggy);

-- pg_attribute entry for toasty.buggy (the index) will have plain storage:
--
-- toasty.buggy（索引）的 pg_attribute 条目将具有普通存储：
UPDATE pg_attribute SET attstorage = 'p'
WHERE attrelid = 'toasty'::regclass AND attname = 'buggy';

-- Whereas pg_attribute entry for toast_bug.buggy (the table) still has extended storage:
--
-- 而 toast_bug.buggy （表）的 pg_attribute 条目仍然具有扩展存储：
SELECT attstorage FROM pg_attribute
WHERE attrelid = 'toast_bug'::regclass AND attname = 'buggy';

-- Insert compressible heap tuple (comfortably exceeds TOAST_TUPLE_THRESHOLD):
--
-- 插入可压缩堆元组（轻松超过 TOAST_TUPLE_THRESHOLD）：
INSERT INTO toast_bug SELECT repeat('a', 2200);
-- Should not get false positive report of corruption:
--
-- 不应收到腐败误报：
SELECT bt_index_check('toasty', true);

--
-- Check that index expressions and predicates are run as the table's owner
--
-- 检查索引表达式和谓词是否以表所有者的身份运行
--
TRUNCATE bttest_a;
INSERT INTO bttest_a SELECT * FROM generate_series(1, 1000);
ALTER TABLE bttest_a OWNER TO regress_bttest_role;
-- A dummy index function checking current_user
--
-- 检查 current_user 的虚拟索引函数
CREATE FUNCTION ifun(int8) RETURNS int8 AS $$
BEGIN
	ASSERT current_user = 'regress_bttest_role',
		format('ifun(%s) called by %s', $1, current_user);
	RETURN $1;
END;
$$ LANGUAGE plpgsql IMMUTABLE;

CREATE INDEX bttest_a_expr_idx ON bttest_a ((ifun(id) + ifun(0)))
	WHERE ifun(id + 10) > ifun(10);

SELECT bt_index_check('bttest_a_expr_idx', true);

-- UNIQUE constraint check
--
-- 唯一约束检查
SELECT bt_index_check('bttest_a_idx', heapallindexed => true, checkunique => true);
SELECT bt_index_check('bttest_b_idx', heapallindexed => false, checkunique => true);
SELECT bt_index_parent_check('bttest_a_idx', heapallindexed => true, rootdescend => true, checkunique => true);
SELECT bt_index_parent_check('bttest_b_idx', heapallindexed => true, rootdescend => false, checkunique => true);

-- Check that null values in an unique index are not treated as equal
--
-- 检查唯一索引中的空值是否不被视为相等
CREATE TABLE bttest_unique_nulls (a serial, b int, c int UNIQUE);
INSERT INTO bttest_unique_nulls VALUES (generate_series(1, 10000), 2, default);
SELECT bt_index_check('bttest_unique_nulls_c_key', heapallindexed => true, checkunique => true);
CREATE INDEX on bttest_unique_nulls (b,c);
SELECT bt_index_check('bttest_unique_nulls_b_c_idx', heapallindexed => true, checkunique => true);

-- Check support of both 1B and 4B header sizes of short varlena datum
--
-- 检查短 varlena 基准的 1B 和 4B 标头大小的支持
CREATE TABLE varlena_bug (v text);
ALTER TABLE varlena_bug ALTER column v SET storage plain;
INSERT INTO varlena_bug VALUES ('x');
COPY varlena_bug from stdin;
x
\.
CREATE INDEX varlena_bug_idx on varlena_bug(v);
SELECT bt_index_check('varlena_bug_idx', true);

-- Also check that we compress varlena values, which were previously stored
--
-- 还要检查我们是否压缩了之前存储的 varlena 值
-- uncompressed in index.
--
-- 索引中未压缩。
INSERT INTO varlena_bug VALUES (repeat('Test', 250));
ALTER TABLE varlena_bug ALTER COLUMN v SET STORAGE extended;
SELECT bt_index_check('varlena_bug_idx', true);

-- cleanup
DROP TABLE bttest_a;
DROP TABLE bttest_b;
DROP TABLE bttest_multi;
DROP TABLE delete_test_table;
DROP TABLE toast_bug;
DROP FUNCTION ifun(int8);
DROP TABLE bttest_unique_nulls;
DROP OWNED BY regress_bttest_role; -- permissions
DROP ROLE regress_bttest_role;
DROP TABLE varlena_bug;

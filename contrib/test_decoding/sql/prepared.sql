-- predictability
SET synchronous_commit = on;
SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot', 'test_decoding');

CREATE TABLE test_prepared1(id int);
CREATE TABLE test_prepared2(id int);

-- test simple successful use of a prepared xact
--
-- 测试简单成功使用准备好的xact
BEGIN;
INSERT INTO test_prepared1 VALUES (1);
PREPARE TRANSACTION 'test_prepared#1';
COMMIT PREPARED 'test_prepared#1';
INSERT INTO test_prepared1 VALUES (2);

-- test abort of a prepared xact
--
-- 测试中止准备好的 xact
BEGIN;
INSERT INTO test_prepared1 VALUES (3);
PREPARE TRANSACTION 'test_prepared#2';
ROLLBACK PREPARED 'test_prepared#2';

INSERT INTO test_prepared1 VALUES (4);

-- test prepared xact containing ddl
--
-- 测试准备包含 ddl 的 xact
BEGIN;
INSERT INTO test_prepared1 VALUES (5);
ALTER TABLE test_prepared1 ADD COLUMN data text;
INSERT INTO test_prepared1 VALUES (6, 'frakbar');
PREPARE TRANSACTION 'test_prepared#3';

-- test that we decode correctly while an uncommitted prepared xact
--
-- 测试我们在未提交的准备好的 xact 时是否正确解码
-- with ddl exists.
--
-- 与 ddl 存在。

-- separate table because of the lock from the ALTER
--
-- 由于 ALTER 的锁定而导致单独的表
-- this will come before the '5' row above, as this commits before it.
--
-- 这将出现在上面的“5”行之前，因为它在它之前提交。
INSERT INTO test_prepared2 VALUES (7);

COMMIT PREPARED 'test_prepared#3';

-- make sure stuff still works
--
-- 确保东西仍然有效
INSERT INTO test_prepared1 VALUES (8);
INSERT INTO test_prepared2 VALUES (9);

-- cleanup
DROP TABLE test_prepared1;
DROP TABLE test_prepared2;

-- show results
--
-- 显示结果
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');

SELECT pg_drop_replication_slot('regression_slot');

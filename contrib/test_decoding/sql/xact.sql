-- predictability
SET synchronous_commit = on;

SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot', 'test_decoding');

CREATE TABLE xact_test(data text);
INSERT INTO xact_test VALUES ('before-test');

-- bug #13844, xids in non-decoded records need to be inspected
--
-- bug #13844，需要检查未解码记录中的 xids
BEGIN;
-- perform operation in xact that creates and logs xid, but isn't decoded
--
-- 在 xact 中执行创建并记录 xid 的操作，但不进行解码
SELECT * FROM xact_test FOR UPDATE;
SAVEPOINT foo;
-- and now actually insert in subxact, xid is expected to be known
--
-- 现在实际插入 subxact，xid 应该是已知的
INSERT INTO xact_test VALUES ('after-assignment');
COMMIT;
-- and now show those changes
--
-- 现在显示这些更改
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');

-- bug #14279, do not propagate null snapshot from subtransaction
--
-- bug #14279，不要从子事务传播空快照
BEGIN;
-- first insert
--
-- 首先插入
INSERT INTO xact_test VALUES ('main-txn');
SAVEPOINT foo;
-- now perform operation in subxact that creates and logs xid, but isn't decoded
--
-- 现在在 subxact 中执行创建并记录 xid 的操作，但未解码
SELECT 1 FROM xact_test FOR UPDATE LIMIT 1;
COMMIT;
-- and now show those changes
--
-- 现在显示这些更改
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');

DROP TABLE xact_test;

SELECT pg_drop_replication_slot('regression_slot');

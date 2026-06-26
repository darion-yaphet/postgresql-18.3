-- Test streaming of two-phase commits
--
-- 两阶段提交的测试流

SET synchronous_commit = on;
SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot', 'test_decoding', false, true);

CREATE TABLE stream_test(data text);

-- consume DDL
--
-- 消耗DDL
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');

-- streaming test with sub-transaction and PREPARE/COMMIT PREPARED
--
-- 使用子事务和 PREPARE/COMMIT PREPARED 进行流测试
BEGIN;
SAVEPOINT s1;
SELECT 'msg5' FROM pg_logical_emit_message(true, 'test', repeat('a', 50));
INSERT INTO stream_test SELECT repeat('a', 2000) || g.i FROM generate_series(1, 35) g(i);
TRUNCATE table stream_test;
ROLLBACK TO s1;
INSERT INTO stream_test SELECT repeat('a', 10) || g.i FROM generate_series(1, 20) g(i);
PREPARE TRANSACTION 'test1';
-- should show the inserts after a ROLLBACK
--
-- 应该显示回滚后的插入
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL,NULL, 'include-xids', '0', 'skip-empty-xacts', '1', 'stream-changes', '1');

COMMIT PREPARED 'test1';
--should show the COMMIT PREPARED and the other changes in the transaction
--
--应该显示 COMMIT PREPARED 和事务中的其他更改
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL,NULL, 'include-xids', '0', 'skip-empty-xacts', '1', 'stream-changes', '1');

-- streaming test with sub-transaction and PREPARE/COMMIT PREPARED but with
--
-- 使用子事务和 PREPARE/COMMIT PREPARED 进行流测试，但使用
-- filtered gid. gids with '_nodecode' will not be decoded at prepare time.
--
-- 过滤后的 gid。带有“_nodecode”的 gids 在准备时不会被解码。
BEGIN;
SAVEPOINT s1;
SELECT 'msg5' FROM pg_logical_emit_message(true, 'test', repeat('a', 50));
INSERT INTO stream_test SELECT repeat('a', 2000) || g.i FROM generate_series(1, 35) g(i);
TRUNCATE table stream_test;
ROLLBACK to s1;
INSERT INTO stream_test SELECT repeat('a', 10) || g.i FROM generate_series(1, 20) g(i);
PREPARE TRANSACTION 'test1_nodecode';
-- should NOT show inserts after a ROLLBACK
--
-- 回滚后不应显示插入
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL,NULL, 'include-xids', '0', 'skip-empty-xacts', '1', 'stream-changes', '1');

COMMIT PREPARED 'test1_nodecode';
-- should show the inserts but not show a COMMIT PREPARED but a COMMIT
--
-- 应该显示插入但不显示 COMMIT PREPARED 而是 COMMIT
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL,NULL, 'include-xids', '0', 'skip-empty-xacts', '1', 'stream-changes', '1');

DROP TABLE stream_test;
SELECT pg_drop_replication_slot('regression_slot');

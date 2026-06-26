-- Test prepared transactions. When two-phase-commit is enabled, transactions are
--
-- 测试准备好的交易。当启用两阶段提交时，事务
-- decoded at PREPARE time rather than at COMMIT PREPARED time.
--
-- 在 PREPARE 时间而不是 COMMIT PREPARED 时间解码。
SET synchronous_commit = on;
SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot', 'test_decoding', false, true);

CREATE TABLE test_prepared1(id integer primary key);
CREATE TABLE test_prepared2(id integer primary key);

-- Test that decoding happens at PREPARE time when two-phase-commit is enabled.
--
-- 当启用两阶段提交时，测试解码是否在 PREPARE 时间发生。
-- Decoding after COMMIT PREPARED must have all the commands in the transaction.
--
-- COMMIT PREPARED后解码必须有事务中的所有命令。
BEGIN;
INSERT INTO test_prepared1 VALUES (1);
INSERT INTO test_prepared1 VALUES (2);
-- should show nothing because the xact has not been prepared yet.
--
-- 应该什么也不显示，因为 xact 尚未准备好。
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');
PREPARE TRANSACTION 'test_prepared#1';
-- should show both the above inserts and the PREPARE TRANSACTION.
--
-- 应显示上述插入内容和 PREPARE TRANSACTION。
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');
COMMIT PREPARED 'test_prepared#1';
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');

-- Test that rollback of a prepared xact is decoded.
--
-- 测试准备好的 xact 的回滚是否已解码。
BEGIN;
INSERT INTO test_prepared1 VALUES (3);
PREPARE TRANSACTION 'test_prepared#2';
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');
ROLLBACK PREPARED 'test_prepared#2';
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');

-- Test prepare of a xact containing ddl. Leaving xact uncommitted for next test.
--
-- 测试准备包含 ddl 的 xact。让 xact 不参与下一次测试。
BEGIN;
ALTER TABLE test_prepared1 ADD COLUMN data text;
INSERT INTO test_prepared1 VALUES (4, 'frakbar');
PREPARE TRANSACTION 'test_prepared#3';
-- confirm that exclusive lock from the ALTER command is held on test_prepared1 table
--
-- 确认 test_prepared1 表上持有来自 ALTER 命令的独占锁
SELECT 'test_prepared_1' AS relation, locktype, mode
FROM pg_locks
WHERE locktype = 'relation'
  AND relation = 'test_prepared1'::regclass;
-- The insert should show the newly altered column but not the DDL.
--
-- 插入应显示新更改的列，但不显示 DDL。
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');

-- Test that we decode correctly while an uncommitted prepared xact
--
-- 测试我们在未提交的准备好的 xact 时是否正确解码
-- with ddl exists.
--
-- 与 ddl 存在。
--
-- Use a separate table for the concurrent transaction because the lock from
--
-- 对并发事务使用单独的表，因为锁来自
-- the ALTER will stop us inserting into the other one.
--
-- ALTER 将阻止我们插入另一个。
--
INSERT INTO test_prepared2 VALUES (5);
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');

COMMIT PREPARED 'test_prepared#3';
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');
-- make sure stuff still works
--
-- 确保东西仍然有效
INSERT INTO test_prepared1 VALUES (6);
INSERT INTO test_prepared2 VALUES (7);
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');

-- Check 'CLUSTER' (as operation that hold exclusive lock) doesn't block
--
-- 检查“CLUSTER”（作为持有独占锁的操作）不会阻塞
-- logical decoding.
--
-- 逻辑解码。
BEGIN;
INSERT INTO test_prepared1 VALUES (8, 'othercol');
CLUSTER test_prepared1 USING test_prepared1_pkey;
INSERT INTO test_prepared1 VALUES (9, 'othercol2');
PREPARE TRANSACTION 'test_prepared_lock';

SELECT 'test_prepared1' AS relation, locktype, mode
FROM pg_locks
WHERE locktype = 'relation'
  AND relation = 'test_prepared1'::regclass;
-- The above CLUSTER command shouldn't cause a timeout on 2pc decoding.
--
-- 上述 CLUSTER 命令不应导致 2pc 解码超时。
\set env_timeout ''
\getenv env_timeout PG_TEST_TIMEOUT_DEFAULT
SELECT COALESCE(NULLIF(:'env_timeout', ''), '180') || 's' AS timeout \gset
SET statement_timeout = :'timeout';
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');
RESET statement_timeout;
COMMIT PREPARED 'test_prepared_lock';
-- consume the commit
--
-- 消耗提交
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');

-- Test savepoints and sub-xacts. Creating savepoints will create
--
-- 测试保存点和子行为。创建保存点将创建
-- sub-xacts implicitly.
--
-- 隐式子行为。
BEGIN;
CREATE TABLE test_prepared_savepoint (a int);
INSERT INTO test_prepared_savepoint VALUES (1);
SAVEPOINT test_savepoint;
INSERT INTO test_prepared_savepoint VALUES (2);
ROLLBACK TO SAVEPOINT test_savepoint;
PREPARE TRANSACTION 'test_prepared_savepoint';
-- should show only 1, not 2
--
-- 应该只显示 1，而不是 2
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');
COMMIT PREPARED 'test_prepared_savepoint';
-- consume the commit
--
-- 消耗提交
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');

-- Test that a GID containing "_nodecode" gets decoded at commit prepared time.
--
-- 测试包含“_nodecode”的 GID 在提交准备时是否被解码。
BEGIN;
INSERT INTO test_prepared1 VALUES (20);
PREPARE TRANSACTION 'test_prepared_nodecode';
-- should show nothing
--
-- 不应该显示任何内容
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');
COMMIT PREPARED 'test_prepared_nodecode';
-- should be decoded now
--
-- 现在应该被解码
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');

-- Test that accessing a TOAST table is permitted during the decoding of a
--
-- 测试在解码期间是否允许访问 TOAST 表
-- prepared transaction.
--
-- 准备交易。

-- Create a table with a column that uses a TOASTed default value.
--
-- 创建一个表，其中的列使用 TOAST 默认值。
-- (temporarily hide query, to avoid the long CREATE TABLE stmt)
--
-- （暂时隐藏查询，以避免冗长的 CREATE TABLE stmt）
\set ECHO none
SELECT 'CREATE TABLE test_tab (a text DEFAULT ''' || string_agg('toast value', '') || ''');' FROM generate_series(1, 4000)
\gexec
\set ECHO all

BEGIN;
INSERT INTO test_tab VALUES('test');
PREPARE TRANSACTION 'test_toast_table_access';

SELECT count(*) FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1', 'stream-changes', '1');

COMMIT PREPARED 'test_toast_table_access';

-- consume commit prepared
--
-- 消耗准备好的提交
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1', 'stream-changes', '1');

-- Test 8:
--
-- 测试8：
-- cleanup and make sure results are also empty
--
-- 清理并确保结果也是空的
DROP TABLE test_prepared1;
DROP TABLE test_prepared2;
DROP TABLE test_prepared_savepoint;
DROP TABLE test_tab;
-- show results. There should be nothing to show
--
-- 显示结果。应该没有什么可显示的
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');

SELECT pg_drop_replication_slot('regression_slot');

-- predictability
SET synchronous_commit = on;

-- superuser required by default
--
-- 默认需要超级用户
CREATE ROLE regress_origin_replication REPLICATION;
SET ROLE regress_origin_replication;
SELECT pg_replication_origin_advance('regress_test_decoding: perm', '0/1');
SELECT pg_replication_origin_create('regress_test_decoding: perm');
SELECT pg_replication_origin_drop('regress_test_decoding: perm');
SELECT pg_replication_origin_oid('regress_test_decoding: perm');
SELECT pg_replication_origin_progress('regress_test_decoding: perm', false);
SELECT pg_replication_origin_session_is_setup();
SELECT pg_replication_origin_session_progress(false);
SELECT pg_replication_origin_session_reset();
SELECT pg_replication_origin_session_setup('regress_test_decoding: perm');
SELECT pg_replication_origin_xact_reset();
SELECT pg_replication_origin_xact_setup('0/1', '2013-01-01 00:00');
SELECT pg_show_replication_origin_status();
RESET ROLE;
DROP ROLE regress_origin_replication;

CREATE TABLE origin_tbl(id serial primary key, data text);
CREATE TABLE target_tbl(id serial primary key, data text);

SELECT pg_replication_origin_create('regress_test_decoding: regression_slot');
-- ensure duplicate creations fail
--
-- 确保重复创建失败
SELECT pg_replication_origin_create('regress_test_decoding: regression_slot');

--ensure deletions work (once)
--
--确保删除有效（一次）
SELECT pg_replication_origin_create('regress_test_decoding: temp');
SELECT pg_replication_origin_drop('regress_test_decoding: temp');
SELECT pg_replication_origin_drop('regress_test_decoding: temp');

-- specifying reserved origin names is not supported
--
-- 不支持指定保留的原始名称
SELECT pg_replication_origin_create('any');
SELECT pg_replication_origin_create('none');
SELECT pg_replication_origin_create('pg_replication_origin');

-- various failure checks for undefined slots
--
-- 针对未定义插槽的各种故障检查
select pg_replication_origin_advance('regress_test_decoding: temp', '0/1');
select pg_replication_origin_session_setup('regress_test_decoding: temp');
select pg_replication_origin_progress('regress_test_decoding: temp', true);

SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot', 'test_decoding');

-- origin tx
--
-- 原始发送
INSERT INTO origin_tbl(data) VALUES ('will be replicated and decoded and decoded again');
INSERT INTO target_tbl(data)
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');

-- as is normal, the insert into target_tbl shows up
--
-- 像平常一样，插入到 target_tbl 显示
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');

INSERT INTO origin_tbl(data) VALUES ('will be replicated, but not decoded again');

-- mark session as replaying
--
-- 将会话标记为重播
SELECT pg_replication_origin_session_setup('regress_test_decoding: regression_slot');

-- ensure we prevent duplicate setup
--
-- 确保我们防止重复设置
SELECT pg_replication_origin_session_setup('regress_test_decoding: regression_slot');

SELECT '' FROM pg_logical_emit_message(false, 'test', 'this message will not be decoded');

BEGIN;
-- setup transaction origin
--
-- 设置交易来源
SELECT pg_replication_origin_xact_setup('0/aabbccdd', '2013-01-01 00:00');
INSERT INTO target_tbl(data)
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1', 'only-local', '1');
COMMIT;

-- check replication progress for the session is correct
--
-- 检查会话的复制进度是否正确
SELECT pg_replication_origin_session_progress(false);
SELECT pg_replication_origin_session_progress(true);

SELECT pg_replication_origin_session_reset();

SELECT local_id, external_id, remote_lsn, local_lsn <> '0/0' FROM pg_replication_origin_status;

-- check replication progress identified by name is correct
--
-- 检查名称标识的复制进度是否正确
SELECT pg_replication_origin_progress('regress_test_decoding: regression_slot', false);
SELECT pg_replication_origin_progress('regress_test_decoding: regression_slot', true);

-- ensure reset requires previously setup state
--
-- 确保重置需要先前设置的状态
SELECT pg_replication_origin_session_reset();

-- and magically the replayed xact will be filtered!
--
-- 神奇的是，重播的 xact 将被过滤！
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1', 'only-local', '1');

--but new original changes still show up
--
--但新的原始变化仍然出现
INSERT INTO origin_tbl(data) VALUES ('will be replicated');
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1',  'only-local', '1');

SELECT pg_drop_replication_slot('regression_slot');
SELECT pg_replication_origin_drop('regress_test_decoding: regression_slot');

-- Set of transactions with no origin LSNs and commit timestamps set for
--
-- 没有原始 LSN 和提交时间戳设置的事务集
-- this session.
--
-- 本次会议。
SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot_no_lsn', 'test_decoding');
SELECT pg_replication_origin_create('regress_test_decoding: regression_slot_no_lsn');
-- mark session as replaying
--
-- 将会话标记为重播
SELECT pg_replication_origin_session_setup('regress_test_decoding: regression_slot_no_lsn');
-- Simple transactions
--
-- 简单交易
BEGIN;
INSERT INTO origin_tbl(data) VALUES ('no_lsn, commit');
COMMIT;
BEGIN;
INSERT INTO origin_tbl(data) VALUES ('no_lsn, rollback');
ROLLBACK;
-- 2PC transactions
--
-- 2PC交易
BEGIN;
INSERT INTO origin_tbl(data) VALUES ('no_lsn, commit prepared');
PREPARE TRANSACTION 'replorigin_prepared';
COMMIT PREPARED 'replorigin_prepared';
BEGIN;
INSERT INTO origin_tbl(data) VALUES ('no_lsn, rollback prepared');
PREPARE TRANSACTION 'replorigin_prepared';
ROLLBACK PREPARED 'replorigin_prepared';
SELECT local_id, external_id,
       remote_lsn <> '0/0' AS valid_remote_lsn,
       local_lsn <> '0/0' AS valid_local_lsn
       FROM pg_replication_origin_status;
SELECT data FROM pg_logical_slot_get_changes('regression_slot_no_lsn', NULL, NULL, 'skip-empty-xacts', '1', 'include-xids', '0');
-- Clean up
--
-- 清理
SELECT pg_replication_origin_session_reset();
SELECT pg_drop_replication_slot('regression_slot_no_lsn');
SELECT pg_replication_origin_drop('regress_test_decoding: regression_slot_no_lsn');

-- Test that the pgoutput correctly filters changes corresponding to the provided origin value.
--
-- 测试 pgoutput 是否正确过滤与提供的原始值相对应的更改。
SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot', 'pgoutput');
CREATE PUBLICATION pub FOR TABLE target_tbl;
SELECT pg_replication_origin_create('regress_test_decoding: regression_slot');

-- mark session as replaying
--
-- 将会话标记为重播
SELECT pg_replication_origin_session_setup('regress_test_decoding: regression_slot');

INSERT INTO target_tbl(data) VALUES ('test data');

-- The replayed change will be filtered.
--
-- 重播的更改将被过滤。
SELECT count(*) = 0 FROM pg_logical_slot_peek_binary_changes('regression_slot', NULL, NULL, 'proto_version', '4', 'publication_names', 'pub', 'origin', 'none');

-- The replayed change will be output if the origin value is not specified.
--
-- 如果未指定原始值，则将输出重播的更改。
SELECT count(*) != 0 FROM pg_logical_slot_peek_binary_changes('regression_slot', NULL, NULL, 'proto_version', '4', 'publication_names', 'pub');

-- Clean up
--
-- 清理
SELECT pg_replication_origin_session_reset();
SELECT pg_drop_replication_slot('regression_slot');
SELECT pg_replication_origin_drop('regress_test_decoding: regression_slot');
DROP PUBLICATION pub;

-- predictability
SET synchronous_commit = on;

SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot_p', 'test_decoding');
SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot_t', 'test_decoding', true);

SELECT pg_drop_replication_slot('regression_slot_p');
SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot_p', 'test_decoding', false);

SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot_t2', 'test_decoding', true);

SELECT pg_create_logical_replication_slot('foo', 'nonexistent');

-- here we want to start a new session and wait till old one is gone
--
-- 在这里我们要开始一个新的会话并等待旧的会话消失
select pg_backend_pid() as oldpid \gset
\c -
SET synchronous_commit = on;

do 'declare c int = 0;
begin
  while (select count(*) from pg_replication_slots where active_pid = '
    :'oldpid'
  ') > 0 loop c := c + 1; perform pg_sleep(0.01); end loop;
  raise log ''slot test looped % times'', c;
end';

-- should fail because the temporary slots were dropped automatically
--
-- 应该失败，因为临时槽被自动删除
SELECT pg_drop_replication_slot('regression_slot_t');
SELECT pg_drop_replication_slot('regression_slot_t2');

-- monitoring functions for slot directories
--
-- 插槽目录的监控功能
SELECT count(*) >= 0 AS ok FROM pg_ls_logicalmapdir();
SELECT count(*) >= 0 AS ok FROM pg_ls_logicalsnapdir();
SELECT count(*) >= 0 AS ok FROM pg_ls_replslotdir('regression_slot_p');
SELECT count(*) >= 0 AS ok FROM pg_ls_replslotdir('not_existing_slot'); -- fails

-- permanent slot has survived
--
-- 永久插槽已幸存
SELECT pg_drop_replication_slot('regression_slot_p');

-- test switching between slots in a session
--
-- 测试会话中时隙之间的切换
SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot1', 'test_decoding', true);

CREATE TABLE replication_example(id SERIAL PRIMARY KEY, somedata int, text varchar(120));
BEGIN;
INSERT INTO replication_example(somedata, text) VALUES (1, 1);
INSERT INTO replication_example(somedata, text) VALUES (1, 2);
COMMIT;

SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot2', 'test_decoding', true);

INSERT INTO replication_example(somedata, text) VALUES (1, 3);

SELECT data FROM pg_logical_slot_get_changes('regression_slot1', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');
SELECT data FROM pg_logical_slot_get_changes('regression_slot2', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');

INSERT INTO replication_example(somedata, text) VALUES (1, 4);
INSERT INTO replication_example(somedata, text) VALUES (1, 5);

SELECT pg_current_wal_lsn() AS wal_lsn \gset

INSERT INTO replication_example(somedata, text) VALUES (1, 6);

SELECT end_lsn FROM pg_replication_slot_advance('regression_slot1', :'wal_lsn') \gset
SELECT slot_name FROM pg_replication_slot_advance('regression_slot2', pg_current_wal_lsn());

SELECT :'wal_lsn' = :'end_lsn';

SELECT data FROM pg_logical_slot_get_changes('regression_slot1', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');
SELECT data FROM pg_logical_slot_get_changes('regression_slot2', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');

DROP TABLE replication_example;

-- error
SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot1', 'test_decoding', true);

-- both should error as they should be dropped on error
--
-- 两者都应该出错，因为它们应该在出错时被丢弃
SELECT pg_drop_replication_slot('regression_slot1');
SELECT pg_drop_replication_slot('regression_slot2');

-- slot advance with physical slot, error with non-reserved slot
--
-- 物理插槽的插槽提前，非保留插槽的错误
SELECT slot_name FROM pg_create_physical_replication_slot('regression_slot3');
SELECT pg_replication_slot_advance('regression_slot3', '0/0'); -- invalid LSN
SELECT pg_replication_slot_advance('regression_slot3', '0/1'); -- error
SELECT pg_drop_replication_slot('regression_slot3');

--
-- Test copy functions for logical replication slots
--
-- 测试逻辑复制槽的复制功能
--

-- Create and copy logical slots
--
-- 创建和复制逻辑槽
SELECT 'init' FROM pg_create_logical_replication_slot('orig_slot1', 'test_decoding', false);
SELECT 'copy' FROM pg_copy_logical_replication_slot('orig_slot1', 'copied_slot1_no_change');
SELECT 'copy' FROM pg_copy_logical_replication_slot('orig_slot1', 'copied_slot1_change_plugin', false, 'pgoutput');
SELECT 'copy' FROM pg_copy_logical_replication_slot('orig_slot1', 'copied_slot1_change_plugin_temp', true, 'pgoutput');

-- Check all copied slots status
--
-- 检查所有复制的插槽状态
SELECT
    o.slot_name, o.plugin, o.temporary, c.slot_name, c.plugin, c.temporary
FROM
    (SELECT * FROM pg_replication_slots WHERE slot_name LIKE 'orig%') as o
    LEFT JOIN pg_replication_slots as c ON o.restart_lsn = c.restart_lsn  AND o.confirmed_flush_lsn = c.confirmed_flush_lsn
WHERE
    o.slot_name != c.slot_name
ORDER BY o.slot_name, c.slot_name;

-- Now we have maximum 4 replication slots. Check slots are properly
--
-- 现在我们最多有 4 个复制槽。检查插槽是否正确
-- released even when raise error during creating the target slot.
--
-- 即使在创建目标插槽期间引发错误也会被释放。
SELECT 'copy' FROM pg_copy_logical_replication_slot('orig_slot1', 'failed'); -- error

-- temporary slots were dropped automatically
--
-- 临时槽被自动删除
SELECT pg_drop_replication_slot('orig_slot1');
SELECT pg_drop_replication_slot('copied_slot1_no_change');
SELECT pg_drop_replication_slot('copied_slot1_change_plugin');

-- Test based on the temporary logical slot
--
-- 基于临时逻辑槽的测试
SELECT 'init' FROM pg_create_logical_replication_slot('orig_slot2', 'test_decoding', true);
SELECT 'copy' FROM pg_copy_logical_replication_slot('orig_slot2', 'copied_slot2_no_change');
SELECT 'copy' FROM pg_copy_logical_replication_slot('orig_slot2', 'copied_slot2_change_plugin', true, 'pgoutput');
SELECT 'copy' FROM pg_copy_logical_replication_slot('orig_slot2', 'copied_slot2_change_plugin_temp', false, 'pgoutput');

-- Check all copied slots status
--
-- 检查所有复制的插槽状态
SELECT
    o.slot_name, o.plugin, o.temporary, c.slot_name, c.plugin, c.temporary
FROM
    (SELECT * FROM pg_replication_slots WHERE slot_name LIKE 'orig%') as o
    LEFT JOIN pg_replication_slots as c ON o.restart_lsn = c.restart_lsn  AND o.confirmed_flush_lsn = c.confirmed_flush_lsn
WHERE
    o.slot_name != c.slot_name
ORDER BY o.slot_name, c.slot_name;

-- Cannot copy a logical slot to a physical slot
--
-- 无法将逻辑插槽复制到物理插槽
SELECT 'copy' FROM pg_copy_physical_replication_slot('orig_slot2', 'failed'); -- error

-- temporary slots were dropped automatically
--
-- 临时槽被自动删除
SELECT pg_drop_replication_slot('copied_slot2_change_plugin_temp');

--
-- Test copy functions for physical replication slots
--
-- 测试物理复制槽的复制功能
--

-- Create and copy physical slots
--
-- 创建和复制物理插槽
SELECT 'init' FROM pg_create_physical_replication_slot('orig_slot1', true);
SELECT 'init' FROM pg_create_physical_replication_slot('orig_slot2', false);
SELECT 'copy' FROM pg_copy_physical_replication_slot('orig_slot1', 'copied_slot1_no_change');
SELECT 'copy' FROM pg_copy_physical_replication_slot('orig_slot1', 'copied_slot1_temp', true);

-- Check all copied slots status. Since all slots don't reserve WAL we check only other fields.
--
-- 检查所有复制的插槽状态。由于所有槽位都不保留 WAL，因此我们只检查其他字段。
SELECT slot_name, slot_type, temporary FROM pg_replication_slots;

-- Cannot copy a physical slot to a logical slot
--
-- 无法将物理插槽复制到逻辑插槽
SELECT 'copy' FROM pg_copy_logical_replication_slot('orig_slot1', 'failed'); -- error

-- Cannot copy a physical slot that doesn't reserve WAL
--
-- 无法复制未保留 WAL 的物理插槽
SELECT 'copy' FROM pg_copy_physical_replication_slot('orig_slot2', 'failed'); -- error

-- temporary slots were dropped automatically
--
-- 临时槽被自动删除
SELECT pg_drop_replication_slot('orig_slot1');
SELECT pg_drop_replication_slot('orig_slot2');
SELECT pg_drop_replication_slot('copied_slot1_no_change');

-- Test based on the temporary physical slot
--
-- 基于临时物理槽位进行测试
SELECT 'init' FROM pg_create_physical_replication_slot('orig_slot2', true, true);
SELECT 'copy' FROM pg_copy_physical_replication_slot('orig_slot2', 'copied_slot2_no_change');
SELECT 'copy' FROM pg_copy_physical_replication_slot('orig_slot2', 'copied_slot2_notemp', false);

-- Check all copied slots status
--
-- 检查所有复制的插槽状态
SELECT
    o.slot_name, o.temporary, c.slot_name, c.temporary
FROM
    (SELECT * FROM pg_replication_slots WHERE slot_name LIKE 'orig%') as o
    LEFT JOIN pg_replication_slots as c ON o.restart_lsn = c.restart_lsn
WHERE
    o.slot_name != c.slot_name
ORDER BY o.slot_name, c.slot_name;

SELECT pg_drop_replication_slot('orig_slot2');
SELECT pg_drop_replication_slot('copied_slot2_no_change');
SELECT pg_drop_replication_slot('copied_slot2_notemp');

-- Test failover option of slots.
--
-- 测试插槽的故障转移选项。
SELECT 'init' FROM pg_create_logical_replication_slot('failover_true_slot', 'test_decoding', false, false, true);
SELECT 'init' FROM pg_create_logical_replication_slot('failover_false_slot', 'test_decoding', false, false, false);
SELECT 'init' FROM pg_create_logical_replication_slot('failover_default_slot', 'test_decoding', false, false);
SELECT 'init' FROM pg_create_logical_replication_slot('failover_true_temp_slot', 'test_decoding', true, false, true);
SELECT 'init' FROM pg_create_physical_replication_slot('physical_slot');

SELECT slot_name, slot_type, failover FROM pg_replication_slots;

SELECT pg_drop_replication_slot('failover_true_slot');
SELECT pg_drop_replication_slot('failover_false_slot');
SELECT pg_drop_replication_slot('failover_default_slot');
SELECT pg_drop_replication_slot('physical_slot');

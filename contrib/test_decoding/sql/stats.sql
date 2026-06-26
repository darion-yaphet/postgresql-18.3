-- predictability
SET synchronous_commit = on;

SELECT 'init' FROM
    pg_create_logical_replication_slot('regression_slot_stats1', 'test_decoding') s1,
    pg_create_logical_replication_slot('regression_slot_stats2', 'test_decoding') s2,
    pg_create_logical_replication_slot('regression_slot_stats3', 'test_decoding') s3;

CREATE TABLE stats_test(data text);

-- non-spilled xact
--
-- 不溢出 xact
SET logical_decoding_work_mem to '64MB';
INSERT INTO stats_test values(1);
SELECT count(*) FROM pg_logical_slot_get_changes('regression_slot_stats1', NULL, NULL, 'skip-empty-xacts', '1');
SELECT count(*) FROM pg_logical_slot_get_changes('regression_slot_stats2', NULL, NULL, 'skip-empty-xacts', '1');
SELECT count(*) FROM pg_logical_slot_get_changes('regression_slot_stats3', NULL, NULL, 'skip-empty-xacts', '1');
SELECT pg_stat_force_next_flush();
SELECT slot_name, spill_txns = 0 AS spill_txns, spill_count = 0 AS spill_count, total_txns > 0 AS total_txns, total_bytes > 0 AS total_bytes FROM pg_stat_replication_slots ORDER BY slot_name;
RESET logical_decoding_work_mem;

-- reset stats for one slot, others should be unaffected
--
-- 重置一个插槽的统计数据，其他插槽不受影响
SELECT pg_stat_reset_replication_slot('regression_slot_stats1');
SELECT slot_name, spill_txns = 0 AS spill_txns, spill_count = 0 AS spill_count, total_txns > 0 AS total_txns, total_bytes > 0 AS total_bytes FROM pg_stat_replication_slots ORDER BY slot_name;

-- reset stats for all slots
--
-- 重置所有插槽的统计数据
SELECT pg_stat_reset_replication_slot(NULL);
SELECT slot_name, spill_txns = 0 AS spill_txns, spill_count = 0 AS spill_count, total_txns > 0 AS total_txns, total_bytes > 0 AS total_bytes FROM pg_stat_replication_slots ORDER BY slot_name;

-- verify accessing/resetting stats for non-existent slot does something reasonable
--
-- 验证访问/重置不存在的插槽的统计信息是否合理
SELECT * FROM pg_stat_get_replication_slot('do-not-exist');
SELECT pg_stat_reset_replication_slot('do-not-exist');
SELECT * FROM pg_stat_get_replication_slot('do-not-exist');

-- spilling the xact
--
-- 溢出 xact
BEGIN;
INSERT INTO stats_test SELECT 'serialize-topbig--1:'||g.i FROM generate_series(1, 5000) g(i);
COMMIT;
SELECT count(*) FROM pg_logical_slot_peek_changes('regression_slot_stats1', NULL, NULL, 'skip-empty-xacts', '1');

-- Check stats. We can't test the exact stats count as that can vary if any
--
-- 检查统计数据。我们无法测试确切的统计数据，因为如果有的话可能会有所不同
-- background transaction (say by autovacuum) happens in parallel to the main
--
-- 后台事务（例如通过 autovacuum）与主事务并行发生
-- transaction.
SELECT pg_stat_force_next_flush();
SELECT slot_name, spill_txns > 0 AS spill_txns, spill_count > 0 AS spill_count FROM pg_stat_replication_slots;

-- Ensure stats can be repeatedly accessed using the same stats snapshot. See
--
-- 确保可以使用相同的统计快照重复访问统计数据。看
-- https://postgr.es/m/20210317230447.c7uc4g3vbs4wi32i%40alap3.anarazel.de
--
-- https://postgr.es/m/20210317230447.c7uc4g3vbs4wi32i%40alap3.anarazel.de
BEGIN;
SELECT slot_name FROM pg_stat_replication_slots;
SELECT slot_name FROM pg_stat_replication_slots;
COMMIT;


SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot_stats4_twophase', 'test_decoding', false, true) s4;

-- The INSERT changes are large enough to be spilled but will not be, because
--
-- INSERT 更改大到足以溢出，但不会溢出，因为
-- the transaction is aborted. The logical decoding skips collecting further
--
-- 交易被中止。逻辑解码跳过进一步收集
-- changes too. The transaction is prepared to make sure the decoding processes
--
-- 也发生变化。交易已准备好以确保解码过程
-- the aborted transaction.
--
-- 中止的交易。
BEGIN;
INSERT INTO stats_test SELECT 'serialize-toobig--1:'||g.i FROM generate_series(1, 5000) g(i);
PREPARE TRANSACTION 'test1_abort';
ROLLBACK PREPARED 'test1_abort';
SELECT count(*) FROM pg_logical_slot_get_changes('regression_slot_stats4_twophase', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');

-- Verify that the decoding doesn't spill already-aborted transaction's changes.
--
-- 验证解码不会泄漏已中止的事务的更改。
SELECT pg_stat_force_next_flush();
SELECT slot_name, spill_txns, spill_count FROM pg_stat_replication_slots WHERE slot_name = 'regression_slot_stats4_twophase';

DROP TABLE stats_test;
SELECT pg_drop_replication_slot('regression_slot_stats1'),
    pg_drop_replication_slot('regression_slot_stats2'),
    pg_drop_replication_slot('regression_slot_stats3'),
    pg_drop_replication_slot('regression_slot_stats4_twophase');

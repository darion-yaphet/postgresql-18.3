-- predictability
SET synchronous_commit = on;

SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot', 'test_decoding');
-- succeeds, textual plugin, textual consumer
--
-- 成功，文本插件，文本消费者
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'force-binary', '0', 'skip-empty-xacts', '1');
-- fails, binary plugin, textual consumer
--
-- 失败，二进制插件，文本消费者
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'force-binary', '1', 'skip-empty-xacts', '1');
-- succeeds, textual plugin, binary consumer
--
-- 成功，文本插件，二进制消费者
SELECT data FROM pg_logical_slot_get_binary_changes('regression_slot', NULL, NULL, 'force-binary', '0', 'skip-empty-xacts', '1');
-- succeeds, binary plugin, binary consumer
--
-- 成功，二进制插件，二进制消费者
SELECT data FROM pg_logical_slot_get_binary_changes('regression_slot', NULL, NULL, 'force-binary', '1', 'skip-empty-xacts', '1');

SELECT 'init' FROM pg_drop_replication_slot('regression_slot');

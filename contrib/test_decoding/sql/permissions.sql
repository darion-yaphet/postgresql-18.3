-- predictability
SET synchronous_commit = on;

-- setup
CREATE ROLE regress_lr_normal;
CREATE ROLE regress_lr_superuser SUPERUSER;
CREATE ROLE regress_lr_replication REPLICATION;
CREATE TABLE lr_test(data text);

-- superuser can control replication
--
-- 超级用户可以控制复制
SET ROLE regress_lr_superuser;
SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot', 'test_decoding');
INSERT INTO lr_test VALUES('lr_superuser_init');
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');
SELECT pg_drop_replication_slot('regression_slot');
RESET ROLE;

-- replication user can control replication
--
-- 复制用户可以控制复制
SET ROLE regress_lr_replication;
SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot', 'test_decoding');
INSERT INTO lr_test VALUES('lr_superuser_init');
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');
SELECT pg_drop_replication_slot('regression_slot');
RESET ROLE;

-- plain user *can't* can control replication
--
-- 普通用户*不能*可以控制复制
SET ROLE regress_lr_normal;
SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot', 'test_decoding');
INSERT INTO lr_test VALUES('lr_superuser_init');
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');
SELECT pg_drop_replication_slot('regression_slot');
SELECT pg_sync_replication_slots();
RESET ROLE;

-- replication users can drop superuser created slots
--
-- 复制用户可以删除超级用户创建的插槽
SET ROLE regress_lr_superuser;
SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot', 'test_decoding');
RESET ROLE;
SET ROLE regress_lr_replication;
SELECT pg_drop_replication_slot('regression_slot');
RESET ROLE;

-- normal users can't drop existing slots
--
-- 普通用户无法删除现有插槽
SET ROLE regress_lr_superuser;
SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot', 'test_decoding');
RESET ROLE;
SET ROLE regress_lr_normal;
SELECT pg_drop_replication_slot('regression_slot');
RESET ROLE;

-- all users can see existing slots
--
-- 所有用户都可以看到现有的插槽
SET ROLE regress_lr_superuser;
SELECT slot_name, plugin FROM pg_replication_slots;
RESET ROLE;

SET ROLE regress_lr_replication;
SELECT slot_name, plugin FROM pg_replication_slots;
RESET ROLE;

SET ROLE regress_lr_normal;
SELECT slot_name, plugin FROM pg_replication_slots;
RESET ROLE;

-- cleanup
SELECT pg_drop_replication_slot('regression_slot');

DROP ROLE regress_lr_normal;
DROP ROLE regress_lr_superuser;
DROP ROLE regress_lr_replication;
DROP TABLE lr_test;

-- test that we can insert the result of a get_changes call into a
--
-- 测试我们是否可以将 get_changes 调用的结果插入到
-- logged relation. That's really not a good idea in practical terms,
--
-- 记录的关系。从实际角度来说这确实不是一个好主意
-- but provides a nice test.
--
-- 但提供了一个很好的测试。

-- predictability
SET synchronous_commit = on;

SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot', 'test_decoding');

-- slot works
--
-- 老虎机作品
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');

-- create some changes
--
-- 做出一些改变
CREATE TABLE somechange(id serial primary key);
INSERT INTO somechange DEFAULT VALUES;

CREATE TABLE changeresult AS
    SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');

SELECT * FROM changeresult;

INSERT INTO changeresult
    SELECT data FROM pg_logical_slot_peek_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');
INSERT INTO changeresult
    SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');

SELECT * FROM changeresult;
DROP TABLE changeresult;
DROP TABLE somechange;

-- check calling logical decoding from pl/pgsql
--
-- 检查从 pl/pgsql 调用逻辑解码
CREATE FUNCTION slot_changes_wrapper(slot_name name) RETURNS SETOF TEXT AS $$
BEGIN
  RETURN QUERY
    SELECT data FROM pg_logical_slot_peek_changes(slot_name, NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');
END$$ LANGUAGE plpgsql;

SELECT * FROM slot_changes_wrapper('regression_slot');

SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');

SELECT 'stop' FROM pg_drop_replication_slot('regression_slot');

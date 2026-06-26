CREATE EXTENSION tsm_system_time;

CREATE TABLE test_tablesample (id int, name text);
INSERT INTO test_tablesample SELECT i, repeat(i::text, 1000)
  FROM generate_series(0, 30) s(i);
ANALYZE test_tablesample;

-- It's a bit tricky to test SYSTEM_TIME in a platform-independent way.
--
-- 以与平台无关的方式测试 SYSTEM_TIME 有点棘手。
-- We can test the zero-time corner case ...
--
-- 我们可以测试零时间极端情况......
SELECT count(*) FROM test_tablesample TABLESAMPLE system_time (0);
-- ... and we assume that this will finish before running out of time:
--
-- ...我们假设这将在时间耗尽之前完成：
SELECT count(*) FROM test_tablesample TABLESAMPLE system_time (100000);

-- bad parameters should get through planning, but not execution:
--
-- 错误的参数应该通过计划，而不是执行：
EXPLAIN (COSTS OFF)
SELECT id FROM test_tablesample TABLESAMPLE system_time (-1);

SELECT id FROM test_tablesample TABLESAMPLE system_time (-1);

-- fail, this method is not repeatable:
--
-- 失败，此方法不可重复：
SELECT * FROM test_tablesample TABLESAMPLE system_time (10) REPEATABLE (0);

-- since it's not repeatable, we expect a Materialize node in these plans:
--
-- 由于它不可重复，我们期望这些计划中有一个 Materialize 节点：
EXPLAIN (COSTS OFF)
SELECT * FROM
  (VALUES (0),(100000)) v(time),
  LATERAL (SELECT COUNT(*) FROM test_tablesample
           TABLESAMPLE system_time (100000)) ss;

SELECT * FROM
  (VALUES (0),(100000)) v(time),
  LATERAL (SELECT COUNT(*) FROM test_tablesample
           TABLESAMPLE system_time (100000)) ss;

EXPLAIN (COSTS OFF)
SELECT * FROM
  (VALUES (0),(100000)) v(time),
  LATERAL (SELECT COUNT(*) FROM test_tablesample
           TABLESAMPLE system_time (time)) ss;

SELECT * FROM
  (VALUES (0),(100000)) v(time),
  LATERAL (SELECT COUNT(*) FROM test_tablesample
           TABLESAMPLE system_time (time)) ss;

CREATE VIEW vv AS
  SELECT * FROM test_tablesample TABLESAMPLE system_time (20);

EXPLAIN (COSTS OFF) SELECT * FROM vv;

DROP EXTENSION tsm_system_time;  -- fail, view depends on extension

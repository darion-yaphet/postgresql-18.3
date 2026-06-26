--
-- Utility commands
--
-- 实用命令
--

-- These tests require track_utility to be enabled.
--
-- 这些测试需要启用 track_utility。
SET pg_stat_statements.track_utility = TRUE;
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- Tables, indexes, triggers
--
-- 表、索引、触发器
CREATE TEMP TABLE tab_stats (a int, b char(20));
CREATE INDEX index_stats ON tab_stats(b, (b || 'data1'), (b || 'data2')) WHERE a > 0;
ALTER TABLE tab_stats ALTER COLUMN b set default 'a';
ALTER TABLE tab_stats ALTER COLUMN b TYPE text USING 'data' || b;
ALTER TABLE tab_stats ADD CONSTRAINT a_nonzero CHECK (a <> 0);
DROP TABLE tab_stats \;
DROP TABLE IF EXISTS tab_stats \;
-- This DROP query uses two different strings, still they count as one entry.
--
-- 此 DROP 查询使用两个不同的字符串，但它们仍算作一个条目。
DROP TABLE IF EXISTS tab_stats \;
Drop Table If Exists tab_stats \;
SELECT calls, rows, query FROM pg_stat_statements ORDER BY query COLLATE "C";
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- Partitions
CREATE TABLE pt_stats (a int, b int) PARTITION BY range (a);
CREATE TABLE pt_stats1 (a int, b int);
ALTER TABLE pt_stats ATTACH PARTITION pt_stats1 FOR VALUES FROM (0) TO (100);
CREATE TABLE pt_stats2 PARTITION OF pt_stats FOR VALUES FROM (100) TO (200);
CREATE INDEX pt_stats_index ON ONLY pt_stats (a);
CREATE INDEX pt_stats2_index ON ONLY pt_stats2 (a);
ALTER INDEX pt_stats_index ATTACH PARTITION pt_stats2_index;
DROP TABLE pt_stats;

-- Views
CREATE VIEW view_stats AS SELECT 1::int AS a, 2::int AS b;
ALTER VIEW view_stats ALTER COLUMN a SET DEFAULT 2;
DROP VIEW view_stats;

-- Foreign tables
--
-- 国外表
CREATE FOREIGN DATA WRAPPER wrapper_stats;
CREATE SERVER server_stats FOREIGN DATA WRAPPER wrapper_stats;
CREATE FOREIGN TABLE foreign_stats (a int) SERVER server_stats;
ALTER FOREIGN TABLE foreign_stats ADD COLUMN b integer DEFAULT 1;
ALTER FOREIGN TABLE foreign_stats ADD CONSTRAINT b_nonzero CHECK (b <> 0);
DROP FOREIGN TABLE foreign_stats;
DROP SERVER server_stats;
DROP FOREIGN DATA WRAPPER wrapper_stats;

-- Functions
CREATE FUNCTION func_stats(a text DEFAULT 'a_data', b text DEFAULT lower('b_data'))
  RETURNS text AS $$ SELECT $1::text || '_' || $2::text; $$ LANGUAGE SQL
  SET work_mem = '256kB';
DROP FUNCTION func_stats;

-- Rules
CREATE TABLE tab_rule_stats (a int, b int);
CREATE TABLE tab_rule_stats_2 (a int, b int, c int, d int);
CREATE RULE rules_stats AS ON INSERT TO tab_rule_stats DO INSTEAD
  INSERT INTO tab_rule_stats_2 VALUES(new.*, 1, 2);
DROP RULE rules_stats ON tab_rule_stats;
DROP TABLE tab_rule_stats, tab_rule_stats_2;

-- Types
CREATE TYPE stats_type as (f1 numeric(35, 6), f2 numeric(35, 2));
DROP TYPE stats_type;

-- Triggers
CREATE TABLE trigger_tab_stats (a int, b int);
CREATE FUNCTION trigger_func_stats () RETURNS trigger LANGUAGE plpgsql
  AS $$ BEGIN return OLD; end; $$;
CREATE TRIGGER trigger_tab_stats
    AFTER UPDATE ON trigger_tab_stats
    FOR EACH ROW WHEN (OLD.a < 0 AND OLD.b < 1 AND true)
    EXECUTE FUNCTION trigger_func_stats();
DROP TABLE trigger_tab_stats;

-- Policies
CREATE TABLE tab_policy_stats (a int, b int);
CREATE POLICY policy_stats ON tab_policy_stats USING (a = 5) WITH CHECK (b < 5);
DROP TABLE tab_policy_stats;

-- Statistics
CREATE TABLE tab_expr_stats (a int, b int);
CREATE STATISTICS tab_expr_stats_1 (mcv) ON a, (2*a), (3*b) FROM tab_expr_stats;
DROP TABLE tab_expr_stats;

SELECT calls, rows, query FROM pg_stat_statements ORDER BY query COLLATE "C";
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- Transaction statements
--
-- 交易报表
BEGIN;
ABORT;
BEGIN;
ROLLBACK;
-- WORK
BEGIN WORK;
COMMIT WORK;
BEGIN WORK;
ABORT WORK;
-- TRANSACTION
BEGIN TRANSACTION;
COMMIT TRANSACTION;
BEGIN TRANSACTION;
ABORT TRANSACTION;
-- More isolation levels
--
-- 更多隔离级别
BEGIN TRANSACTION DEFERRABLE;
COMMIT TRANSACTION AND NO CHAIN;
BEGIN ISOLATION LEVEL SERIALIZABLE;
COMMIT;
BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE;
COMMIT;
-- List of A_Const nodes, same lists.
--
-- A_Const 节点列表，相同的列表。
BEGIN TRANSACTION READ ONLY, READ WRITE, DEFERRABLE, NOT DEFERRABLE;
COMMIT;
BEGIN TRANSACTION NOT DEFERRABLE, READ ONLY, READ WRITE, DEFERRABLE;
COMMIT;
SELECT calls, rows, query FROM pg_stat_statements ORDER BY query COLLATE "C";
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- Two-phase transactions
--
-- 两阶段交易
BEGIN;
PREPARE TRANSACTION 'stat_trans1';
COMMIT PREPARED 'stat_trans1';
BEGIN;
PREPARE TRANSACTION 'stat_trans2';
ROLLBACK PREPARED 'stat_trans2';
SELECT calls, rows, query FROM pg_stat_statements ORDER BY query COLLATE "C";
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- Savepoints
BEGIN;
SAVEPOINT sp1;
SAVEPOINT sp2;
SAVEPOINT sp3;
SAVEPOINT sp4;
ROLLBACK TO sp4;
ROLLBACK TO SAVEPOINT sp4;
ROLLBACK TRANSACTION TO SAVEPOINT sp3;
RELEASE sp3;
RELEASE SAVEPOINT sp2;
ROLLBACK TO sp1;
RELEASE SAVEPOINT sp1;
COMMIT;
SELECT calls, rows, query FROM pg_stat_statements ORDER BY query COLLATE "C";
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- EXPLAIN statements
--
-- 解释语句
-- A Query is used, normalized by the query jumbling.
--
-- 使用查询，通过查询混乱进行规范化。
EXPLAIN (costs off) SELECT 1;
EXPLAIN (costs off) SELECT 2;
EXPLAIN (costs off) SELECT a FROM generate_series(1,10) AS tab(a) WHERE a = 3;
EXPLAIN (costs off) SELECT a FROM generate_series(1,10) AS tab(a) WHERE a = 7;

SELECT calls, rows, query FROM pg_stat_statements ORDER BY query COLLATE "C";

-- CALL
CREATE OR REPLACE PROCEDURE sum_one(i int) AS $$
DECLARE
  r int;
BEGIN
  SELECT (i + i)::int INTO r;
END; $$ LANGUAGE plpgsql;
CREATE OR REPLACE PROCEDURE sum_two(i int, j int) AS $$
DECLARE
  r int;
BEGIN
  SELECT (i + j)::int INTO r;
END; $$ LANGUAGE plpgsql;
-- Overloaded functions.
--
-- 功能超载。
CREATE OR REPLACE PROCEDURE overload(i int) AS $$
DECLARE
  r int;
BEGIN
  SELECT (i + i)::int INTO r;
END; $$ LANGUAGE plpgsql;
CREATE OR REPLACE PROCEDURE overload(i text) AS $$
DECLARE
  r text;
BEGIN
  SELECT i::text INTO r;
END; $$ LANGUAGE plpgsql;
-- Mix of IN/OUT parameters.
--
-- 输入/输出参数的混合。
CREATE OR REPLACE PROCEDURE in_out(i int, i2 OUT int, i3 INOUT int) AS $$
DECLARE
  r int;
BEGIN
  i2 := i;
  i3 := i3 + i;
END; $$ LANGUAGE plpgsql;
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
CALL sum_one(3);
CALL sum_one(199);
CALL sum_two(1,1);
CALL sum_two(1,2);
CALL overload(1);
CALL overload('A');
CALL in_out(1, NULL, 1);
CALL in_out(2, 1, 2);
SELECT calls, rows, query FROM pg_stat_statements ORDER BY query COLLATE "C";

-- COPY
CREATE TABLE copy_stats (a int, b int);
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
-- Some queries with A_Const nodes.
--
-- 一些带有 A_Const 节点的查询。
COPY (SELECT 1) TO STDOUT;
COPY (SELECT 2) TO STDOUT;
COPY (INSERT INTO copy_stats VALUES (1, 1) RETURNING *) TO STDOUT;
COPY (INSERT INTO copy_stats VALUES (2, 2) RETURNING *) TO STDOUT;
COPY (UPDATE copy_stats SET b = b + 1 RETURNING *) TO STDOUT;
COPY (UPDATE copy_stats SET b = b + 2 RETURNING *) TO STDOUT;
COPY (DELETE FROM copy_stats WHERE a = 1 RETURNING *) TO STDOUT;

SELECT calls, rows, query FROM pg_stat_statements ORDER BY query COLLATE "C";
DROP TABLE copy_stats;
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- CREATE TABLE AS
--
-- 创建表为
-- SELECT queries are normalized, creating matching query IDs.
--
-- SELECT 查询被规范化，创建匹配的查询 ID。
CREATE TABLE ctas_stats_1 AS SELECT 1 AS a;
DROP TABLE ctas_stats_1;
CREATE TABLE ctas_stats_1 AS SELECT 2 AS a;
DROP TABLE ctas_stats_1;
CREATE TABLE ctas_stats_2 AS
  SELECT a AS col1, 2::int AS col2
    FROM generate_series(1, 10) AS tab(a) WHERE a < 5 AND a > 2;
DROP TABLE ctas_stats_2;
CREATE TABLE ctas_stats_2 AS
  SELECT a AS col1, 4::int AS col2
    FROM generate_series(1, 5) AS tab(a) WHERE a < 4 AND a > 1;
DROP TABLE ctas_stats_2;
SELECT calls, rows, query FROM pg_stat_statements ORDER BY query COLLATE "C";
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- CREATE MATERIALIZED VIEW
--
-- 创建物化视图
-- SELECT queries are normalized, creating matching query IDs.
--
-- SELECT 查询被规范化，创建匹配的查询 ID。
CREATE MATERIALIZED VIEW matview_stats_1 AS
  SELECT a AS col1, 2::int AS col2
    FROM generate_series(1, 10) AS tab(a) WHERE a < 5 AND a > 2;
DROP MATERIALIZED VIEW matview_stats_1;
CREATE MATERIALIZED VIEW matview_stats_1 AS
  SELECT a AS col1, 4::int AS col2
    FROM generate_series(1, 5) AS tab(a) WHERE a < 4 AND a > 3;
DROP MATERIALIZED VIEW matview_stats_1;
SELECT calls, rows, query FROM pg_stat_statements ORDER BY query COLLATE "C";
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- CREATE VIEW
--
-- 创建视图
CREATE VIEW view_stats_1 AS
  SELECT a AS col1, 2::int AS col2
    FROM generate_series(1, 10) AS tab(a) WHERE a < 5 AND a > 2;
DROP VIEW view_stats_1;
CREATE VIEW view_stats_1 AS
  SELECT a AS col1, 4::int AS col2
    FROM generate_series(1, 5) AS tab(a) WHERE a < 4 AND a > 3;
DROP VIEW view_stats_1;
SELECT calls, rows, query FROM pg_stat_statements ORDER BY query COLLATE "C";
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- Domains
CREATE DOMAIN domain_stats AS int CHECK (VALUE > 0);
ALTER DOMAIN domain_stats SET DEFAULT '3';
ALTER DOMAIN domain_stats ADD CONSTRAINT higher_than_one CHECK (VALUE > 1);
DROP DOMAIN domain_stats;
SELECT calls, rows, query FROM pg_stat_statements ORDER BY query COLLATE "C";
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- Execution statements
--
-- 执行语句
SELECT 1 as a;
PREPARE stat_select AS SELECT $1 AS a;
EXECUTE stat_select (1);
DEALLOCATE stat_select;
PREPARE stat_select AS SELECT $1 AS a;
EXECUTE stat_select (2);
DEALLOCATE PREPARE stat_select;
DEALLOCATE ALL;
DEALLOCATE PREPARE ALL;
SELECT calls, rows, query FROM pg_stat_statements ORDER BY query COLLATE "C";
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- SET statements.
--
-- SET 语句。
-- These use two different strings, still they count as one entry.
--
-- 它们使用两个不同的字符串，但它们仍然算作一个条目。
CREATE ROLE regress_stat_set_1;
CREATE ROLE regress_stat_set_2;
SET work_mem = '1MB';
Set work_mem = '1MB';
SET work_mem = '2MB';
SET work_mem = DEFAULT;
SET work_mem TO DEFAULT;
SET work_mem FROM CURRENT;
BEGIN;
SET LOCAL work_mem = '128kB';
SET LOCAL work_mem = '256kB';
SET LOCAL work_mem = DEFAULT;
SET LOCAL work_mem TO DEFAULT;
SET LOCAL work_mem FROM CURRENT;
COMMIT;
RESET work_mem;
SET enable_seqscan = off;
SET enable_seqscan = on;
SET SESSION work_mem = '300kB';
SET SESSION work_mem = '400kB';
RESET enable_seqscan;
-- SET TRANSACTION ISOLATION
--
-- 设置事务隔离
BEGIN;
SET TRANSACTION ISOLATION LEVEL READ COMMITTED;
SET TRANSACTION ISOLATION LEVEL REPEATABLE READ;
SET TRANSACTION ISOLATION LEVEL SERIALIZABLE;
COMMIT;
-- SET SESSION AUTHORIZATION
--
-- 设置会话授权
SET SESSION SESSION AUTHORIZATION DEFAULT;
SET SESSION AUTHORIZATION 'regress_stat_set_1';
SET SESSION AUTHORIZATION 'regress_stat_set_2';
RESET SESSION AUTHORIZATION;
BEGIN;
SET LOCAL SESSION AUTHORIZATION DEFAULT;
SET LOCAL SESSION AUTHORIZATION 'regress_stat_set_1';
SET LOCAL SESSION AUTHORIZATION 'regress_stat_set_2';
RESET SESSION AUTHORIZATION;
COMMIT;
-- SET SESSION CHARACTERISTICS
--
-- 设置会话特征
SET SESSION CHARACTERISTICS AS TRANSACTION READ ONLY;
SET SESSION CHARACTERISTICS AS TRANSACTION READ ONLY, READ ONLY;
SET SESSION CHARACTERISTICS AS TRANSACTION READ ONLY, READ WRITE;
-- SET XML OPTION
--
-- 设置 XML 选项
SET XML OPTION DOCUMENT;
SET XML OPTION CONTENT;
-- SET TIME ZONE
--
-- 设置时区
SET TIME ZONE 'America/New_York';
SET TIME ZONE 'Asia/Tokyo';
SET TIME ZONE DEFAULT;
SET TIME ZONE LOCAL;
SET TIME ZONE 'CST7CDT,M4.1.0,M10.5.0';
RESET TIME ZONE;

SELECT calls, rows, query FROM pg_stat_statements ORDER BY query COLLATE "C";
DROP ROLE regress_stat_set_1;
DROP ROLE regress_stat_set_2;
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

--
-- Track the total number of rows retrieved or affected by the utility
--
-- 跟踪实用程序检索或影响的总行数
-- commands of COPY, FETCH, CREATE TABLE AS, CREATE MATERIALIZED VIEW,
--
-- COPY、FETCH、CREATE TABLE AS、CREATE MATERIALIZED VIEW 命令，
-- REFRESH MATERIALIZED VIEW and SELECT INTO
--
-- 刷新物化视图并选择进入
--
CREATE TABLE pgss_ctas AS SELECT a, 'ctas' b FROM generate_series(1, 10) a;
SELECT generate_series(1, 10) c INTO pgss_select_into;
COPY pgss_ctas (a, b) FROM STDIN;
11	copy
12	copy
13	copy
\.
CREATE MATERIALIZED VIEW pgss_matv AS SELECT * FROM pgss_ctas;
REFRESH MATERIALIZED VIEW pgss_matv;
BEGIN;
DECLARE pgss_cursor CURSOR FOR SELECT * FROM pgss_matv;
FETCH NEXT pgss_cursor;
FETCH FORWARD 5 pgss_cursor;
FETCH FORWARD ALL pgss_cursor;
COMMIT;

SELECT calls, rows, query FROM pg_stat_statements ORDER BY query COLLATE "C";

DROP MATERIALIZED VIEW pgss_matv;
DROP TABLE pgss_ctas;
DROP TABLE pgss_select_into;

SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- Special cases.  Keep these ones at the end to avoid conflicts.
--
-- 特殊情况。  将这些保留在最后以避免冲突。
SET SCHEMA 'foo';
SET SCHEMA 'public';
RESET ALL;
SELECT calls, rows, query FROM pg_stat_statements ORDER BY query COLLATE "C";

SELECT pg_stat_statements_reset() IS NOT NULL AS t;

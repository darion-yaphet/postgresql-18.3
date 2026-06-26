-- ===================================================================
-- create FDW objects
--
-- 创建 FDW 对象
-- ===================================================================

CREATE EXTENSION postgres_fdw;

CREATE SERVER testserver1 FOREIGN DATA WRAPPER postgres_fdw;
DO $d$
    BEGIN
        EXECUTE $$CREATE SERVER loopback FOREIGN DATA WRAPPER postgres_fdw
            OPTIONS (dbname '$$||current_database()||$$',
                     port '$$||current_setting('port')||$$'
            )$$;
        EXECUTE $$CREATE SERVER loopback2 FOREIGN DATA WRAPPER postgres_fdw
            OPTIONS (dbname '$$||current_database()||$$',
                     port '$$||current_setting('port')||$$'
            )$$;
        EXECUTE $$CREATE SERVER loopback3 FOREIGN DATA WRAPPER postgres_fdw
            OPTIONS (dbname '$$||current_database()||$$',
                     port '$$||current_setting('port')||$$'
            )$$;
    END;
$d$;

CREATE USER MAPPING FOR public SERVER testserver1
	OPTIONS (user 'value', password 'value');
CREATE USER MAPPING FOR CURRENT_USER SERVER loopback;
CREATE USER MAPPING FOR CURRENT_USER SERVER loopback2;
CREATE USER MAPPING FOR public SERVER loopback3;

-- ===================================================================
-- create objects used through FDW loopback server
--
-- 创建通过 FDW 环回服务器使用的对象
-- ===================================================================
CREATE TYPE user_enum AS ENUM ('foo', 'bar', 'buz');
CREATE SCHEMA "S 1";
CREATE TABLE "S 1"."T 1" (
	"C 1" int NOT NULL,
	c2 int NOT NULL,
	c3 text,
	c4 timestamptz,
	c5 timestamp,
	c6 varchar(10),
	c7 char(10),
	c8 user_enum,
	CONSTRAINT t1_pkey PRIMARY KEY ("C 1")
);
CREATE TABLE "S 1"."T 2" (
	c1 int NOT NULL,
	c2 text,
	CONSTRAINT t2_pkey PRIMARY KEY (c1)
);
CREATE TABLE "S 1"."T 3" (
	c1 int NOT NULL,
	c2 int NOT NULL,
	c3 text,
	CONSTRAINT t3_pkey PRIMARY KEY (c1)
);
CREATE TABLE "S 1"."T 4" (
	c1 int NOT NULL,
	c2 int NOT NULL,
	c3 text,
	CONSTRAINT t4_pkey PRIMARY KEY (c1)
);

-- Disable autovacuum for these tables to avoid unexpected effects of that
--
-- 对这些表禁用 autovacuum 以避免意外影响
ALTER TABLE "S 1"."T 1" SET (autovacuum_enabled = 'false');
ALTER TABLE "S 1"."T 2" SET (autovacuum_enabled = 'false');
ALTER TABLE "S 1"."T 3" SET (autovacuum_enabled = 'false');
ALTER TABLE "S 1"."T 4" SET (autovacuum_enabled = 'false');

INSERT INTO "S 1"."T 1"
	SELECT id,
	       id % 10,
	       to_char(id, 'FM00000'),
	       '1970-01-01'::timestamptz + ((id % 100) || ' days')::interval,
	       '1970-01-01'::timestamp + ((id % 100) || ' days')::interval,
	       id % 10,
	       id % 10,
	       'foo'::user_enum
	FROM generate_series(1, 1000) id;
INSERT INTO "S 1"."T 2"
	SELECT id,
	       'AAA' || to_char(id, 'FM000')
	FROM generate_series(1, 100) id;
INSERT INTO "S 1"."T 3"
	SELECT id,
	       id + 1,
	       'AAA' || to_char(id, 'FM000')
	FROM generate_series(1, 100) id;
DELETE FROM "S 1"."T 3" WHERE c1 % 2 != 0;	-- delete for outer join tests
INSERT INTO "S 1"."T 4"
	SELECT id,
	       id + 1,
	       'AAA' || to_char(id, 'FM000')
	FROM generate_series(1, 100) id;
DELETE FROM "S 1"."T 4" WHERE c1 % 3 != 0;	-- delete for outer join tests

ANALYZE "S 1"."T 1";
ANALYZE "S 1"."T 2";
ANALYZE "S 1"."T 3";
ANALYZE "S 1"."T 4";

-- ===================================================================
-- create foreign tables
--
-- 创建外部表
-- ===================================================================
CREATE FOREIGN TABLE ft1 (
	c0 int,
	c1 int NOT NULL,
	c2 int NOT NULL,
	c3 text,
	c4 timestamptz,
	c5 timestamp,
	c6 varchar(10),
	c7 char(10) default 'ft1',
	c8 user_enum
) SERVER loopback;
ALTER FOREIGN TABLE ft1 DROP COLUMN c0;

CREATE FOREIGN TABLE ft2 (
	c1 int NOT NULL,
	c2 int NOT NULL,
	cx int,
	c3 text,
	c4 timestamptz,
	c5 timestamp,
	c6 varchar(10),
	c7 char(10) default 'ft2',
	c8 user_enum
) SERVER loopback;
ALTER FOREIGN TABLE ft2 DROP COLUMN cx;

CREATE FOREIGN TABLE ft4 (
	c1 int NOT NULL,
	c2 int NOT NULL,
	c3 text
) SERVER loopback OPTIONS (schema_name 'S 1', table_name 'T 3');

CREATE FOREIGN TABLE ft5 (
	c1 int NOT NULL,
	c2 int NOT NULL,
	c3 text
) SERVER loopback OPTIONS (schema_name 'S 1', table_name 'T 4');

CREATE FOREIGN TABLE ft6 (
	c1 int NOT NULL,
	c2 int NOT NULL,
	c3 text
) SERVER loopback2 OPTIONS (schema_name 'S 1', table_name 'T 4');

CREATE FOREIGN TABLE ft7 (
	c1 int NOT NULL,
	c2 int NOT NULL,
	c3 text
) SERVER loopback3 OPTIONS (schema_name 'S 1', table_name 'T 4');

-- ===================================================================
-- tests for validator
--
-- 验证器测试
-- ===================================================================
-- requiressl and some other parameters are omitted because
--
-- 省略了 requiresl 和其他一些参数，因为
-- valid values for them depend on configure options
--
-- 它们的有效值取决于配置选项
ALTER SERVER testserver1 OPTIONS (
	use_remote_estimate 'false',
	updatable 'true',
	fdw_startup_cost '123.456',
	fdw_tuple_cost '0.123',
	service 'value',
	connect_timeout 'value',
	dbname 'value',
	host 'value',
	hostaddr 'value',
	port 'value',
	--client_encoding 'value',
	--
	--client_encoding '值'，
	application_name 'value',
	--fallback_application_name 'value',
	--
	--Fallback_application_name '值'，
	keepalives 'value',
	keepalives_idle 'value',
	keepalives_interval 'value',
	tcp_user_timeout 'value',
	-- requiressl 'value',
	--
	-- 需要“价值”，
	sslcompression 'value',
	sslmode 'value',
	sslcert 'value',
	sslkey 'value',
	sslrootcert 'value',
	sslcrl 'value',
	--requirepeer 'value',
	--
	--要求同行“价值”，
	krbsrvname 'value',
	gsslib 'value',
	gssdelegation 'value'
	--replication 'value'
	--
	--复制“值”
);

-- Error, invalid list syntax
--
-- 错误，列表语法无效
ALTER SERVER testserver1 OPTIONS (ADD extensions 'foo; bar');

-- OK but gets a warning
--
-- 好的，但收到警告
ALTER SERVER testserver1 OPTIONS (ADD extensions 'foo, bar');
ALTER SERVER testserver1 OPTIONS (DROP extensions);

ALTER USER MAPPING FOR public SERVER testserver1
	OPTIONS (DROP user, DROP password);

-- Attempt to add a valid option that's not allowed in a user mapping
--
-- 尝试添加用户映射中不允许的有效选项
ALTER USER MAPPING FOR public SERVER testserver1
	OPTIONS (ADD sslmode 'require');

-- But we can add valid ones fine
--
-- 但我们可以添加有效的
ALTER USER MAPPING FOR public SERVER testserver1
	OPTIONS (ADD sslpassword 'dummy');

-- Ensure valid options we haven't used in a user mapping yet are
--
-- 确保我们尚未在用户映射中使用的有效选项
-- permitted to check validation.
--
-- 允许检查验证。
ALTER USER MAPPING FOR public SERVER testserver1
	OPTIONS (ADD sslkey 'value', ADD sslcert 'value');

-- OAuth options are not allowed in either context
--
-- 在任一上下文中都不允许使用 OAuth 选项
ALTER SERVER testserver1 OPTIONS (ADD oauth_issuer 'https://example.com');
ALTER SERVER testserver1 OPTIONS (ADD oauth_client_id 'myID');
ALTER USER MAPPING FOR public SERVER testserver1
	OPTIONS (ADD oauth_issuer 'https://example.com');
ALTER USER MAPPING FOR public SERVER testserver1
	OPTIONS (ADD oauth_client_id 'myID');

ALTER FOREIGN TABLE ft1 OPTIONS (schema_name 'S 1', table_name 'T 1');
ALTER FOREIGN TABLE ft2 OPTIONS (schema_name 'S 1', table_name 'T 1');
ALTER FOREIGN TABLE ft1 ALTER COLUMN c1 OPTIONS (column_name 'C 1');
ALTER FOREIGN TABLE ft2 ALTER COLUMN c1 OPTIONS (column_name 'C 1');
\det+

-- Test that alteration of server options causes reconnection
--
-- 测试服务器选项的更改是否会导致重新连接
-- Remote's errors might be non-English, so hide them to ensure stable results
--
-- Remote 的错误可能是非英语的，因此隐藏它们以确保稳定的结果
\set VERBOSITY terse
SELECT c3, c4 FROM ft1 ORDER BY c3, c1 LIMIT 1;  -- should work
ALTER SERVER loopback OPTIONS (SET dbname 'no such database');
SELECT c3, c4 FROM ft1 ORDER BY c3, c1 LIMIT 1;  -- should fail
DO $d$
    BEGIN
        EXECUTE $$ALTER SERVER loopback
            OPTIONS (SET dbname '$$||current_database()||$$')$$;
    END;
$d$;
SELECT c3, c4 FROM ft1 ORDER BY c3, c1 LIMIT 1;  -- should work again

-- Test that alteration of user mapping options causes reconnection
--
-- 测试用户映射选项的更改是否会导致重新连接
ALTER USER MAPPING FOR CURRENT_USER SERVER loopback
  OPTIONS (ADD user 'no such user');
SELECT c3, c4 FROM ft1 ORDER BY c3, c1 LIMIT 1;  -- should fail
ALTER USER MAPPING FOR CURRENT_USER SERVER loopback
  OPTIONS (DROP user);
SELECT c3, c4 FROM ft1 ORDER BY c3, c1 LIMIT 1;  -- should work again
\set VERBOSITY default

-- Now we should be able to run ANALYZE.
--
-- 现在我们应该能够运行 ANALYZE。
-- To exercise multiple code paths, we use local stats on ft1
--
-- 为了练习多个代码路径，我们在 ft1 上使用本地统计信息
-- and remote-estimate mode on ft2.
--
-- 和 ft2 上的远程估计模式。
ANALYZE ft1;
ALTER FOREIGN TABLE ft2 OPTIONS (use_remote_estimate 'true');

-- ===================================================================
-- test error case for create publication on foreign table
--
-- 在外表上创建发布的测试错误案例
-- ===================================================================
CREATE PUBLICATION testpub_ftbl FOR TABLE ft1;  -- should fail

-- ===================================================================
-- simple queries
--
-- 简单查询
-- ===================================================================
-- single table without alias
--
-- 没有别名的单表
EXPLAIN (COSTS OFF) SELECT * FROM ft1 ORDER BY c3, c1 OFFSET 100 LIMIT 10;
SELECT * FROM ft1 ORDER BY c3, c1 OFFSET 100 LIMIT 10;
-- single table with alias - also test that tableoid sort is not pushed to remote side
--
-- 具有别名的单个表 - 还测试 tableoid 排序是否未推送到远程端
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM ft1 t1 ORDER BY t1.c3, t1.c1, t1.tableoid OFFSET 100 LIMIT 10;
SELECT * FROM ft1 t1 ORDER BY t1.c3, t1.c1, t1.tableoid OFFSET 100 LIMIT 10;
-- whole-row reference
--
-- 整行参考
EXPLAIN (VERBOSE, COSTS OFF) SELECT t1 FROM ft1 t1 ORDER BY t1.c3, t1.c1 OFFSET 100 LIMIT 10;
SELECT t1 FROM ft1 t1 ORDER BY t1.c3, t1.c1 OFFSET 100 LIMIT 10;
-- empty result
--
-- 结果为空
SELECT * FROM ft1 WHERE false;
-- with WHERE clause
--
-- 带有 WHERE 子句
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM ft1 t1 WHERE t1.c1 = 101 AND t1.c6 = '1' AND t1.c7 >= '1';
SELECT * FROM ft1 t1 WHERE t1.c1 = 101 AND t1.c6 = '1' AND t1.c7 >= '1';
-- with FOR UPDATE/SHARE
--
-- 用于更新/共享
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM ft1 t1 WHERE c1 = 101 FOR UPDATE;
SELECT * FROM ft1 t1 WHERE c1 = 101 FOR UPDATE;
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM ft1 t1 WHERE c1 = 102 FOR SHARE;
SELECT * FROM ft1 t1 WHERE c1 = 102 FOR SHARE;
-- aggregate
SELECT COUNT(*) FROM ft1 t1;
-- subquery
SELECT * FROM ft1 t1 WHERE t1.c3 IN (SELECT c3 FROM ft2 t2 WHERE c1 <= 10) ORDER BY c1;
-- subquery+MAX
--
-- 子查询+MAX
SELECT * FROM ft1 t1 WHERE t1.c3 = (SELECT MAX(c3) FROM ft2 t2) ORDER BY c1;
-- used in CTE
--
-- 用于CTE
WITH t1 AS (SELECT * FROM ft1 WHERE c1 <= 10) SELECT t2.c1, t2.c2, t2.c3, t2.c4 FROM t1, ft2 t2 WHERE t1.c1 = t2.c1 ORDER BY t1.c1;
-- fixed values
--
-- 固定值
SELECT 'fixed', NULL FROM ft1 t1 WHERE c1 = 1;
-- Test forcing the remote server to produce sorted data for a merge join.
--
-- 测试强制远程服务器为合并连接生成排序数据。
SET enable_hashjoin TO false;
SET enable_nestloop TO false;
-- inner join; expressions in the clauses appear in the equivalence class list
--
-- 内连接；子句中的表达式出现在等价类列表中
EXPLAIN (VERBOSE, COSTS OFF)
	SELECT t1.c1, t2."C 1" FROM ft2 t1 JOIN "S 1"."T 1" t2 ON (t1.c1 = t2."C 1") OFFSET 100 LIMIT 10;
SELECT t1.c1, t2."C 1" FROM ft2 t1 JOIN "S 1"."T 1" t2 ON (t1.c1 = t2."C 1") OFFSET 100 LIMIT 10;
-- outer join; expressions in the clauses do not appear in equivalence class
--
-- 外连接；子句中的表达式未出现在等价类中
-- list but no output change as compared to the previous query
--
-- 列出但与之前的查询相比输出没有变化
EXPLAIN (VERBOSE, COSTS OFF)
	SELECT t1.c1, t2."C 1" FROM ft2 t1 LEFT JOIN "S 1"."T 1" t2 ON (t1.c1 = t2."C 1") OFFSET 100 LIMIT 10;
SELECT t1.c1, t2."C 1" FROM ft2 t1 LEFT JOIN "S 1"."T 1" t2 ON (t1.c1 = t2."C 1") OFFSET 100 LIMIT 10;
-- A join between local table and foreign join. ORDER BY clause is added to the
--
-- 本地表和外部连接之间的连接。 ORDER BY 子句添加到
-- foreign join so that the local table can be joined using merge join strategy.
--
-- 外部连接使得本地表可以使用合并连接策略进行连接。
EXPLAIN (VERBOSE, COSTS OFF)
	SELECT t1."C 1" FROM "S 1"."T 1" t1 left join ft1 t2 join ft2 t3 on (t2.c1 = t3.c1) on (t3.c1 = t1."C 1") OFFSET 100 LIMIT 10;
SELECT t1."C 1" FROM "S 1"."T 1" t1 left join ft1 t2 join ft2 t3 on (t2.c1 = t3.c1) on (t3.c1 = t1."C 1") OFFSET 100 LIMIT 10;
-- Test similar to above, except that the full join prevents any equivalence
--
-- 测试与上面类似，除了完全连接阻止任何等价
-- classes from being merged. This produces single relation equivalence classes
--
-- 类被合并。这会产生单关系等价类
-- included in join restrictions.
--
-- 包含在连接限制中。
EXPLAIN (VERBOSE, COSTS OFF)
	SELECT t1."C 1", t2.c1, t3.c1 FROM "S 1"."T 1" t1 left join ft1 t2 full join ft2 t3 on (t2.c1 = t3.c1) on (t3.c1 = t1."C 1") OFFSET 100 LIMIT 10;
SELECT t1."C 1", t2.c1, t3.c1 FROM "S 1"."T 1" t1 left join ft1 t2 full join ft2 t3 on (t2.c1 = t3.c1) on (t3.c1 = t1."C 1") OFFSET 100 LIMIT 10;
-- Test similar to above with all full outer joins
--
-- 使用所有完整外连接进行与上面类似的测试
EXPLAIN (VERBOSE, COSTS OFF)
	SELECT t1."C 1", t2.c1, t3.c1 FROM "S 1"."T 1" t1 full join ft1 t2 full join ft2 t3 on (t2.c1 = t3.c1) on (t3.c1 = t1."C 1") OFFSET 100 LIMIT 10;
SELECT t1."C 1", t2.c1, t3.c1 FROM "S 1"."T 1" t1 full join ft1 t2 full join ft2 t3 on (t2.c1 = t3.c1) on (t3.c1 = t1."C 1") OFFSET 100 LIMIT 10;
RESET enable_hashjoin;
RESET enable_nestloop;

-- Test executing assertion in estimate_path_cost_size() that makes sure that
--
-- 测试在estimate_path_cost_size()中执行断言，以确保
-- retrieved_rows for foreign rel re-used to cost pre-sorted foreign paths is
--
-- 重新用于计算预排序外部路径成本的外部关系的 returned_rows 是
-- a sensible value even when the rel has tuples=0
--
-- 即使 rel 的 tuples=0 也是一个合理的值
CREATE TABLE loct_empty (c1 int NOT NULL, c2 text);
CREATE FOREIGN TABLE ft_empty (c1 int NOT NULL, c2 text)
  SERVER loopback OPTIONS (table_name 'loct_empty');
INSERT INTO loct_empty
  SELECT id, 'AAA' || to_char(id, 'FM000') FROM generate_series(1, 100) id;
DELETE FROM loct_empty;
ANALYZE ft_empty;
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM ft_empty ORDER BY c1;

-- test restriction on non-system foreign tables.
--
-- 对非系统外部表的测试限制。
SET restrict_nonsystem_relation_kind TO 'foreign-table';
SELECT * from ft1 where c1 < 1; -- ERROR
INSERT INTO ft1 (c1) VALUES (1); -- ERROR
DELETE FROM ft1 WHERE c1 = 1; -- ERROR
TRUNCATE ft1; -- ERROR
RESET restrict_nonsystem_relation_kind;

-- ===================================================================
-- WHERE with remotely-executable conditions
--
-- WHERE 具有远程执行条件
-- ===================================================================
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM ft1 t1 WHERE t1.c1 = 1;         -- Var, OpExpr(b), Const
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM ft1 t1 WHERE t1.c1 = 100 AND t1.c2 = 0; -- BoolExpr
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM ft1 t1 WHERE c3 IS NULL;        -- NullTest
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM ft1 t1 WHERE c3 IS NOT NULL;    -- NullTest
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM ft1 t1 WHERE round(abs(c1), 0) = 1; -- FuncExpr
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM ft1 t1 WHERE c1 = -c1;          -- OpExpr(l)
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM ft1 t1 WHERE (c1 IS NOT NULL) IS DISTINCT FROM (c1 IS NOT NULL); -- DistinctExpr
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM ft1 t1 WHERE c1 = ANY(ARRAY[c2, 1, c1 + 0]); -- ScalarArrayOpExpr
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM ft1 t1 WHERE c1 = (ARRAY[c1,c2,3])[1]; -- SubscriptingRef
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM ft1 t1 WHERE c6 = E'foo''s\\bar';  -- check special chars
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM ft1 t1 WHERE c8 = 'foo';  -- can't be sent to remote
-- parameterized remote path for foreign table
--
-- 外部表的参数化远程路径
EXPLAIN (VERBOSE, COSTS OFF)
  SELECT * FROM "S 1"."T 1" a, ft2 b WHERE a."C 1" = 47 AND b.c1 = a.c2;
SELECT * FROM "S 1"."T 1" a, ft2 b WHERE a."C 1" = 47 AND b.c1 = a.c2;

-- check both safe and unsafe join conditions
--
-- 检查安全和不安全的连接条件
EXPLAIN (VERBOSE, COSTS OFF)
  SELECT * FROM ft2 a, ft2 b
  WHERE a.c2 = 6 AND b.c1 = a.c1 AND a.c8 = 'foo' AND b.c7 = upper(a.c7);
SELECT * FROM ft2 a, ft2 b
WHERE a.c2 = 6 AND b.c1 = a.c1 AND a.c8 = 'foo' AND b.c7 = upper(a.c7);
-- bug before 9.3.5 due to sloppy handling of remote-estimate parameters
--
-- 9.3.5 之前的错误是由于远程估计参数处理不当造成的
SELECT * FROM ft1 WHERE c1 = ANY (ARRAY(SELECT c1 FROM ft2 WHERE c1 < 5));
SELECT * FROM ft2 WHERE c1 = ANY (ARRAY(SELECT c1 FROM ft1 WHERE c1 < 5));

-- user-defined operator/function
--
-- 用户定义的运算符/函数
CREATE FUNCTION postgres_fdw_abs(int) RETURNS int AS $$
BEGIN
RETURN abs($1);
END
$$ LANGUAGE plpgsql IMMUTABLE;
CREATE OPERATOR === (
    LEFTARG = int,
    RIGHTARG = int,
    PROCEDURE = int4eq,
    COMMUTATOR = ===
);

-- built-in operators and functions can be shipped for remote execution
--
-- 内置运算符和函数可用于远程执行
EXPLAIN (VERBOSE, COSTS OFF)
  SELECT count(c3) FROM ft1 t1 WHERE t1.c1 = abs(t1.c2);
SELECT count(c3) FROM ft1 t1 WHERE t1.c1 = abs(t1.c2);
EXPLAIN (VERBOSE, COSTS OFF)
  SELECT count(c3) FROM ft1 t1 WHERE t1.c1 = t1.c2;
SELECT count(c3) FROM ft1 t1 WHERE t1.c1 = t1.c2;

-- by default, user-defined ones cannot
--
-- 默认情况下，用户定义的不能
EXPLAIN (VERBOSE, COSTS OFF)
  SELECT count(c3) FROM ft1 t1 WHERE t1.c1 = postgres_fdw_abs(t1.c2);
SELECT count(c3) FROM ft1 t1 WHERE t1.c1 = postgres_fdw_abs(t1.c2);
EXPLAIN (VERBOSE, COSTS OFF)
  SELECT count(c3) FROM ft1 t1 WHERE t1.c1 === t1.c2;
SELECT count(c3) FROM ft1 t1 WHERE t1.c1 === t1.c2;

-- ORDER BY can be shipped, though
--
-- 但 ORDER BY 可以发货
EXPLAIN (VERBOSE, COSTS OFF)
  SELECT * FROM ft1 t1 WHERE t1.c1 === t1.c2 order by t1.c2 limit 1;
SELECT * FROM ft1 t1 WHERE t1.c1 === t1.c2 order by t1.c2 limit 1;

-- but let's put them in an extension ...
--
-- 但让我们把它们放在扩展中......
ALTER EXTENSION postgres_fdw ADD FUNCTION postgres_fdw_abs(int);
ALTER EXTENSION postgres_fdw ADD OPERATOR === (int, int);
ALTER SERVER loopback OPTIONS (ADD extensions 'postgres_fdw');

-- ... now they can be shipped
--
-- ...现在可以发货了
EXPLAIN (VERBOSE, COSTS OFF)
  SELECT count(c3) FROM ft1 t1 WHERE t1.c1 = postgres_fdw_abs(t1.c2);
SELECT count(c3) FROM ft1 t1 WHERE t1.c1 = postgres_fdw_abs(t1.c2);
EXPLAIN (VERBOSE, COSTS OFF)
  SELECT count(c3) FROM ft1 t1 WHERE t1.c1 === t1.c2;
SELECT count(c3) FROM ft1 t1 WHERE t1.c1 === t1.c2;

-- and both ORDER BY and LIMIT can be shipped
--
-- 并且 ORDER BY 和 LIMIT 都可以发货
EXPLAIN (VERBOSE, COSTS OFF)
  SELECT * FROM ft1 t1 WHERE t1.c1 === t1.c2 order by t1.c2 limit 1;
SELECT * FROM ft1 t1 WHERE t1.c1 === t1.c2 order by t1.c2 limit 1;

-- Ensure we don't ship FETCH FIRST .. WITH TIES
--
-- 确保我们不会先运送 FETCH .. with TIES
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.c2 FROM ft1 t1 WHERE t1.c1 > 960 ORDER BY t1.c2 FETCH FIRST 2 ROWS WITH TIES;
SELECT t1.c2 FROM ft1 t1 WHERE t1.c1 > 960 ORDER BY t1.c2 FETCH FIRST 2 ROWS WITH TIES;

-- Test CASE pushdown
--
-- 测试用例下推
EXPLAIN (VERBOSE, COSTS OFF)
SELECT c1,c2,c3 FROM ft2 WHERE CASE WHEN c1 > 990 THEN c1 END < 1000 ORDER BY c1;
SELECT c1,c2,c3 FROM ft2 WHERE CASE WHEN c1 > 990 THEN c1 END < 1000 ORDER BY c1;

-- Nested CASE
--
-- 嵌套案例
EXPLAIN (VERBOSE, COSTS OFF)
SELECT c1,c2,c3 FROM ft2 WHERE CASE CASE WHEN c2 > 0 THEN c2 END WHEN 100 THEN 601 WHEN c2 THEN c2 ELSE 0 END > 600 ORDER BY c1;

SELECT c1,c2,c3 FROM ft2 WHERE CASE CASE WHEN c2 > 0 THEN c2 END WHEN 100 THEN 601 WHEN c2 THEN c2 ELSE 0 END > 600 ORDER BY c1;

-- CASE arg WHEN
--
-- 案例 arg 何时
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM ft1 WHERE c1 > (CASE mod(c1, 4) WHEN 0 THEN 1 WHEN 2 THEN 50 ELSE 100 END);

-- CASE cannot be pushed down because of unshippable arg clause
--
-- 由于无法传送的 arg 子句，CASE 无法下推
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM ft1 WHERE c1 > (CASE random()::integer WHEN 0 THEN 1 WHEN 2 THEN 50 ELSE 100 END);

-- these are shippable
--
-- 这些是可运送的
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM ft1 WHERE CASE c6 WHEN 'foo' THEN true ELSE c3 < 'bar' END;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM ft1 WHERE CASE c3 WHEN c6 THEN true ELSE c3 < 'bar' END;

-- but this is not because of collation
--
-- 但这不是因为整理
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM ft1 WHERE CASE c3 COLLATE "C" WHEN c6 THEN true ELSE c3 < 'bar' END;

-- a regconfig constant referring to this text search configuration
--
-- 引用此文本搜索配置的 regconfig 常量
-- is initially unshippable
--
-- 最初是无法发货的
CREATE TEXT SEARCH CONFIGURATION public.custom_search
  (COPY = pg_catalog.english);
EXPLAIN (VERBOSE, COSTS OFF)
SELECT c1, to_tsvector('custom_search'::regconfig, c3) FROM ft1
WHERE c1 = 642 AND length(to_tsvector('custom_search'::regconfig, c3)) > 0;
SELECT c1, to_tsvector('custom_search'::regconfig, c3) FROM ft1
WHERE c1 = 642 AND length(to_tsvector('custom_search'::regconfig, c3)) > 0;
-- but if it's in a shippable extension, it can be shipped
--
-- 但如果它在可发货扩展中，则可以发货
ALTER EXTENSION postgres_fdw ADD TEXT SEARCH CONFIGURATION public.custom_search;
-- however, that doesn't flush the shippability cache, so do a quick reconnect
--
-- 但是，这不会刷新可发货缓存，因此请快速重新连接
\c -
EXPLAIN (VERBOSE, COSTS OFF)
SELECT c1, to_tsvector('custom_search'::regconfig, c3) FROM ft1
WHERE c1 = 642 AND length(to_tsvector('custom_search'::regconfig, c3)) > 0;
SELECT c1, to_tsvector('custom_search'::regconfig, c3) FROM ft1
WHERE c1 = 642 AND length(to_tsvector('custom_search'::regconfig, c3)) > 0;

-- ===================================================================
-- ORDER BY queries
--
-- ORDER BY 查询
-- ===================================================================
-- we should not push order by clause with volatile expressions or unsafe
--
-- 我们不应该使用易失性表达式或不安全的方式推送 order by 子句
-- collations
EXPLAIN (VERBOSE, COSTS OFF)
	SELECT * FROM ft2 ORDER BY ft2.c1, random();
EXPLAIN (VERBOSE, COSTS OFF)
	SELECT * FROM ft2 ORDER BY ft2.c1, ft2.c3 collate "C";

-- Ensure we don't push ORDER BY expressions which are Consts at the UNION
--
-- 确保我们不推送 ORDER BY 表达式，这些表达式是 UNION 中的 Const
-- child level to the foreign server.
--
-- 子级到外部服务器。
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM (
    SELECT 1 AS type,c1 FROM ft1
    UNION ALL
    SELECT 2 AS type,c1 FROM ft2
) a ORDER BY type,c1;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM (
    SELECT 1 AS type,c1 FROM ft1
    UNION ALL
    SELECT 2 AS type,c1 FROM ft2
) a ORDER BY type;

-- ===================================================================
-- JOIN queries
--
-- 连接查询
-- ===================================================================
-- Analyze ft4 and ft5 so that we have better statistics. These tables do not
--
-- 分析ft4和ft5，以便我们有更好的统计。这些表不
-- have use_remote_estimate set.
--
-- 设置了 use_remote_estimate 。
ANALYZE ft4;
ANALYZE ft5;

-- join two tables
--
-- 连接两个表
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.c1, t2.c1 FROM ft1 t1 JOIN ft2 t2 ON (t1.c1 = t2.c1) ORDER BY t1.c3, t1.c1 OFFSET 100 LIMIT 10;
SELECT t1.c1, t2.c1 FROM ft1 t1 JOIN ft2 t2 ON (t1.c1 = t2.c1) ORDER BY t1.c3, t1.c1 OFFSET 100 LIMIT 10;
-- join three tables
--
-- 连接三个表
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.c1, t2.c2, t3.c3 FROM ft1 t1 JOIN ft2 t2 ON (t1.c1 = t2.c1) JOIN ft4 t3 ON (t3.c1 = t1.c1) ORDER BY t1.c3, t1.c1 OFFSET 10 LIMIT 10;
SELECT t1.c1, t2.c2, t3.c3 FROM ft1 t1 JOIN ft2 t2 ON (t1.c1 = t2.c1) JOIN ft4 t3 ON (t3.c1 = t1.c1) ORDER BY t1.c3, t1.c1 OFFSET 10 LIMIT 10;
-- left outer join
--
-- 左外连接
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.c1, t2.c1 FROM ft4 t1 LEFT JOIN ft5 t2 ON (t1.c1 = t2.c1) ORDER BY t1.c1, t2.c1 OFFSET 10 LIMIT 10;
SELECT t1.c1, t2.c1 FROM ft4 t1 LEFT JOIN ft5 t2 ON (t1.c1 = t2.c1) ORDER BY t1.c1, t2.c1 OFFSET 10 LIMIT 10;
-- left outer join three tables
--
-- 左外连接三个表
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.c1, t2.c2, t3.c3 FROM ft2 t1 LEFT JOIN ft2 t2 ON (t1.c1 = t2.c1) LEFT JOIN ft4 t3 ON (t2.c1 = t3.c1) OFFSET 10 LIMIT 10;
SELECT t1.c1, t2.c2, t3.c3 FROM ft2 t1 LEFT JOIN ft2 t2 ON (t1.c1 = t2.c1) LEFT JOIN ft4 t3 ON (t2.c1 = t3.c1) OFFSET 10 LIMIT 10;
-- left outer join + placement of clauses.
--
-- 左外连接+子句的放置。
-- clauses within the nullable side are not pulled up, but top level clause on
--
-- 可空一侧的子句不会被拉起，但顶级子句会被拉起
-- non-nullable side is pushed into non-nullable side
--
-- 不可为空的一侧被推入不可为空的一侧
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.c1, t1.c2, t2.c1, t2.c2 FROM ft4 t1 LEFT JOIN (SELECT * FROM ft5 WHERE c1 < 10) t2 ON (t1.c1 = t2.c1) WHERE t1.c1 < 10;
SELECT t1.c1, t1.c2, t2.c1, t2.c2 FROM ft4 t1 LEFT JOIN (SELECT * FROM ft5 WHERE c1 < 10) t2 ON (t1.c1 = t2.c1) WHERE t1.c1 < 10;
-- clauses within the nullable side are not pulled up, but the top level clause
--
-- 可空一侧的子句不会被拉起，而是顶级子句
-- on nullable side is not pushed down into nullable side
--
-- 可空一侧不会被推入可空一侧
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.c1, t1.c2, t2.c1, t2.c2 FROM ft4 t1 LEFT JOIN (SELECT * FROM ft5 WHERE c1 < 10) t2 ON (t1.c1 = t2.c1)
			WHERE (t2.c1 < 10 OR t2.c1 IS NULL) AND t1.c1 < 10;
SELECT t1.c1, t1.c2, t2.c1, t2.c2 FROM ft4 t1 LEFT JOIN (SELECT * FROM ft5 WHERE c1 < 10) t2 ON (t1.c1 = t2.c1)
			WHERE (t2.c1 < 10 OR t2.c1 IS NULL) AND t1.c1 < 10;
-- right outer join
--
-- 右外连接
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.c1, t2.c1 FROM ft5 t1 RIGHT JOIN ft4 t2 ON (t1.c1 = t2.c1) ORDER BY t2.c1, t1.c1 OFFSET 10 LIMIT 10;
SELECT t1.c1, t2.c1 FROM ft5 t1 RIGHT JOIN ft4 t2 ON (t1.c1 = t2.c1) ORDER BY t2.c1, t1.c1 OFFSET 10 LIMIT 10;
-- right outer join three tables
--
-- 右外连接三个表
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.c1, t2.c2, t3.c3 FROM ft2 t1 RIGHT JOIN ft2 t2 ON (t1.c1 = t2.c1) RIGHT JOIN ft4 t3 ON (t2.c1 = t3.c1) OFFSET 10 LIMIT 10;
SELECT t1.c1, t2.c2, t3.c3 FROM ft2 t1 RIGHT JOIN ft2 t2 ON (t1.c1 = t2.c1) RIGHT JOIN ft4 t3 ON (t2.c1 = t3.c1) OFFSET 10 LIMIT 10;
-- full outer join
--
-- 完全外连接
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.c1, t2.c1 FROM ft4 t1 FULL JOIN ft5 t2 ON (t1.c1 = t2.c1) ORDER BY t1.c1, t2.c1 OFFSET 45 LIMIT 10;
SELECT t1.c1, t2.c1 FROM ft4 t1 FULL JOIN ft5 t2 ON (t1.c1 = t2.c1) ORDER BY t1.c1, t2.c1 OFFSET 45 LIMIT 10;
-- full outer join with restrictions on the joining relations
--
-- 对连接关系有限制的完全外连接
-- a. the joining relations are both base relations
--
-- 一个。连接关系都是基础关系
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.c1, t2.c1 FROM (SELECT c1 FROM ft4 WHERE c1 between 50 and 60) t1 FULL JOIN (SELECT c1 FROM ft5 WHERE c1 between 50 and 60) t2 ON (t1.c1 = t2.c1) ORDER BY t1.c1, t2.c1;
SELECT t1.c1, t2.c1 FROM (SELECT c1 FROM ft4 WHERE c1 between 50 and 60) t1 FULL JOIN (SELECT c1 FROM ft5 WHERE c1 between 50 and 60) t2 ON (t1.c1 = t2.c1) ORDER BY t1.c1, t2.c1;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT 1 FROM (SELECT c1 FROM ft4 WHERE c1 between 50 and 60) t1 FULL JOIN (SELECT c1 FROM ft5 WHERE c1 between 50 and 60) t2 ON (TRUE) OFFSET 10 LIMIT 10;
SELECT 1 FROM (SELECT c1 FROM ft4 WHERE c1 between 50 and 60) t1 FULL JOIN (SELECT c1 FROM ft5 WHERE c1 between 50 and 60) t2 ON (TRUE) OFFSET 10 LIMIT 10;
-- b. one of the joining relations is a base relation and the other is a join
--
-- b.连接关系之一是基础关系，另一个是连接关系
-- relation
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.c1, ss.a, ss.b FROM (SELECT c1 FROM ft4 WHERE c1 between 50 and 60) t1 FULL JOIN (SELECT t2.c1, t3.c1 FROM ft4 t2 LEFT JOIN ft5 t3 ON (t2.c1 = t3.c1) WHERE (t2.c1 between 50 and 60)) ss(a, b) ON (t1.c1 = ss.a) ORDER BY t1.c1, ss.a, ss.b;
SELECT t1.c1, ss.a, ss.b FROM (SELECT c1 FROM ft4 WHERE c1 between 50 and 60) t1 FULL JOIN (SELECT t2.c1, t3.c1 FROM ft4 t2 LEFT JOIN ft5 t3 ON (t2.c1 = t3.c1) WHERE (t2.c1 between 50 and 60)) ss(a, b) ON (t1.c1 = ss.a) ORDER BY t1.c1, ss.a, ss.b;
-- c. test deparsing the remote query as nested subqueries
--
-- c.测试将远程查询解析为嵌套子查询
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.c1, ss.a, ss.b FROM (SELECT c1 FROM ft4 WHERE c1 between 50 and 60) t1 FULL JOIN (SELECT t2.c1, t3.c1 FROM (SELECT c1 FROM ft4 WHERE c1 between 50 and 60) t2 FULL JOIN (SELECT c1 FROM ft5 WHERE c1 between 50 and 60) t3 ON (t2.c1 = t3.c1) WHERE t2.c1 IS NULL OR t2.c1 IS NOT NULL) ss(a, b) ON (t1.c1 = ss.a) ORDER BY t1.c1, ss.a, ss.b;
SELECT t1.c1, ss.a, ss.b FROM (SELECT c1 FROM ft4 WHERE c1 between 50 and 60) t1 FULL JOIN (SELECT t2.c1, t3.c1 FROM (SELECT c1 FROM ft4 WHERE c1 between 50 and 60) t2 FULL JOIN (SELECT c1 FROM ft5 WHERE c1 between 50 and 60) t3 ON (t2.c1 = t3.c1) WHERE t2.c1 IS NULL OR t2.c1 IS NOT NULL) ss(a, b) ON (t1.c1 = ss.a) ORDER BY t1.c1, ss.a, ss.b;
-- d. test deparsing rowmarked relations as subqueries
--
-- d.测试将行标记关系解析为子查询
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.c1, ss.a, ss.b FROM (SELECT c1 FROM "S 1"."T 3" WHERE c1 = 50) t1 INNER JOIN (SELECT t2.c1, t3.c1 FROM (SELECT c1 FROM ft4 WHERE c1 between 50 and 60) t2 FULL JOIN (SELECT c1 FROM ft5 WHERE c1 between 50 and 60) t3 ON (t2.c1 = t3.c1) WHERE t2.c1 IS NULL OR t2.c1 IS NOT NULL) ss(a, b) ON (TRUE) ORDER BY t1.c1, ss.a, ss.b FOR UPDATE OF t1;
SELECT t1.c1, ss.a, ss.b FROM (SELECT c1 FROM "S 1"."T 3" WHERE c1 = 50) t1 INNER JOIN (SELECT t2.c1, t3.c1 FROM (SELECT c1 FROM ft4 WHERE c1 between 50 and 60) t2 FULL JOIN (SELECT c1 FROM ft5 WHERE c1 between 50 and 60) t3 ON (t2.c1 = t3.c1) WHERE t2.c1 IS NULL OR t2.c1 IS NOT NULL) ss(a, b) ON (TRUE) ORDER BY t1.c1, ss.a, ss.b FOR UPDATE OF t1;
-- full outer join + inner join
--
-- 全外连接+内连接
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.c1, t2.c1, t3.c1 FROM ft4 t1 INNER JOIN ft5 t2 ON (t1.c1 = t2.c1 + 1 and t1.c1 between 50 and 60) FULL JOIN ft4 t3 ON (t2.c1 = t3.c1) ORDER BY t1.c1, t2.c1, t3.c1 LIMIT 10;
SELECT t1.c1, t2.c1, t3.c1 FROM ft4 t1 INNER JOIN ft5 t2 ON (t1.c1 = t2.c1 + 1 and t1.c1 between 50 and 60) FULL JOIN ft4 t3 ON (t2.c1 = t3.c1) ORDER BY t1.c1, t2.c1, t3.c1 LIMIT 10;
-- full outer join three tables
--
-- 完全外连接三个表
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.c1, t2.c2, t3.c3 FROM ft2 t1 FULL JOIN ft2 t2 ON (t1.c1 = t2.c1) FULL JOIN ft4 t3 ON (t2.c1 = t3.c1) OFFSET 10 LIMIT 10;
SELECT t1.c1, t2.c2, t3.c3 FROM ft2 t1 FULL JOIN ft2 t2 ON (t1.c1 = t2.c1) FULL JOIN ft4 t3 ON (t2.c1 = t3.c1) OFFSET 10 LIMIT 10;
-- full outer join + right outer join
--
-- 全外连接+右外连接
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.c1, t2.c2, t3.c3 FROM ft2 t1 FULL JOIN ft2 t2 ON (t1.c1 = t2.c1) RIGHT JOIN ft4 t3 ON (t2.c1 = t3.c1) OFFSET 10 LIMIT 10;
SELECT t1.c1, t2.c2, t3.c3 FROM ft2 t1 FULL JOIN ft2 t2 ON (t1.c1 = t2.c1) RIGHT JOIN ft4 t3 ON (t2.c1 = t3.c1) OFFSET 10 LIMIT 10;
-- right outer join + full outer join
--
-- 右外连接+全外连接
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.c1, t2.c2, t3.c3 FROM ft2 t1 RIGHT JOIN ft2 t2 ON (t1.c1 = t2.c1) FULL JOIN ft4 t3 ON (t2.c1 = t3.c1) OFFSET 10 LIMIT 10;
SELECT t1.c1, t2.c2, t3.c3 FROM ft2 t1 RIGHT JOIN ft2 t2 ON (t1.c1 = t2.c1) FULL JOIN ft4 t3 ON (t2.c1 = t3.c1) OFFSET 10 LIMIT 10;
-- full outer join + left outer join
--
-- 全外连接+左外连接
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.c1, t2.c2, t3.c3 FROM ft2 t1 FULL JOIN ft2 t2 ON (t1.c1 = t2.c1) LEFT JOIN ft4 t3 ON (t2.c1 = t3.c1) OFFSET 10 LIMIT 10;
SELECT t1.c1, t2.c2, t3.c3 FROM ft2 t1 FULL JOIN ft2 t2 ON (t1.c1 = t2.c1) LEFT JOIN ft4 t3 ON (t2.c1 = t3.c1) OFFSET 10 LIMIT 10;
-- left outer join + full outer join
--
-- 左外连接+全外连接
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.c1, t2.c2, t3.c3 FROM ft2 t1 LEFT JOIN ft2 t2 ON (t1.c1 = t2.c1) FULL JOIN ft4 t3 ON (t2.c1 = t3.c1) OFFSET 10 LIMIT 10;
SELECT t1.c1, t2.c2, t3.c3 FROM ft2 t1 LEFT JOIN ft2 t2 ON (t1.c1 = t2.c1) FULL JOIN ft4 t3 ON (t2.c1 = t3.c1) OFFSET 10 LIMIT 10;
SET enable_memoize TO off;
-- right outer join + left outer join
--
-- 右外连接+左外连接
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.c1, t2.c2, t3.c3 FROM ft2 t1 RIGHT JOIN ft2 t2 ON (t1.c1 = t2.c1) LEFT JOIN ft4 t3 ON (t2.c1 = t3.c1) OFFSET 10 LIMIT 10;
SELECT t1.c1, t2.c2, t3.c3 FROM ft2 t1 RIGHT JOIN ft2 t2 ON (t1.c1 = t2.c1) LEFT JOIN ft4 t3 ON (t2.c1 = t3.c1) OFFSET 10 LIMIT 10;
RESET enable_memoize;
-- left outer join + right outer join
--
-- 左外连接+右外连接
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.c1, t2.c2, t3.c3 FROM ft2 t1 LEFT JOIN ft2 t2 ON (t1.c1 = t2.c1) RIGHT JOIN ft4 t3 ON (t2.c1 = t3.c1) OFFSET 10 LIMIT 10;
SELECT t1.c1, t2.c2, t3.c3 FROM ft2 t1 LEFT JOIN ft2 t2 ON (t1.c1 = t2.c1) RIGHT JOIN ft4 t3 ON (t2.c1 = t3.c1) OFFSET 10 LIMIT 10;
-- full outer join + WHERE clause, only matched rows
--
-- 全外连接 + WHERE 子句，仅匹配行
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.c1, t2.c1 FROM ft4 t1 FULL JOIN ft5 t2 ON (t1.c1 = t2.c1) WHERE (t1.c1 = t2.c1 OR t1.c1 IS NULL) ORDER BY t1.c1, t2.c1 OFFSET 10 LIMIT 10;
SELECT t1.c1, t2.c1 FROM ft4 t1 FULL JOIN ft5 t2 ON (t1.c1 = t2.c1) WHERE (t1.c1 = t2.c1 OR t1.c1 IS NULL) ORDER BY t1.c1, t2.c1 OFFSET 10 LIMIT 10;
-- full outer join + WHERE clause with shippable extensions set
--
-- 完整外连接 + WHERE 子句以及可交付扩展集
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.c1, t2.c2, t1.c3 FROM ft1 t1 FULL JOIN ft2 t2 ON (t1.c1 = t2.c1) WHERE postgres_fdw_abs(t1.c1) > 0 OFFSET 10 LIMIT 10;
ALTER SERVER loopback OPTIONS (DROP extensions);
-- full outer join + WHERE clause with shippable extensions not set
--
-- 未设置可交付扩展的完整外连接 + WHERE 子句
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.c1, t2.c2, t1.c3 FROM ft1 t1 FULL JOIN ft2 t2 ON (t1.c1 = t2.c1) WHERE postgres_fdw_abs(t1.c1) > 0 OFFSET 10 LIMIT 10;
ALTER SERVER loopback OPTIONS (ADD extensions 'postgres_fdw');
-- join two tables with FOR UPDATE clause
--
-- 使用 FOR UPDATE 子句连接两个表
-- tests whole-row reference for row marks
--
-- 测试行标记的整行参考
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.c1, t2.c1 FROM ft1 t1 JOIN ft2 t2 ON (t1.c1 = t2.c1) ORDER BY t1.c3, t1.c1 OFFSET 100 LIMIT 10 FOR UPDATE OF t1;
SELECT t1.c1, t2.c1 FROM ft1 t1 JOIN ft2 t2 ON (t1.c1 = t2.c1) ORDER BY t1.c3, t1.c1 OFFSET 100 LIMIT 10 FOR UPDATE OF t1;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.c1, t2.c1 FROM ft1 t1 JOIN ft2 t2 ON (t1.c1 = t2.c1) ORDER BY t1.c3, t1.c1 OFFSET 100 LIMIT 10 FOR UPDATE;
SELECT t1.c1, t2.c1 FROM ft1 t1 JOIN ft2 t2 ON (t1.c1 = t2.c1) ORDER BY t1.c3, t1.c1 OFFSET 100 LIMIT 10 FOR UPDATE;
-- join two tables with FOR SHARE clause
--
-- 使用 FOR SHARE 子句连接两个表
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.c1, t2.c1 FROM ft1 t1 JOIN ft2 t2 ON (t1.c1 = t2.c1) ORDER BY t1.c3, t1.c1 OFFSET 100 LIMIT 10 FOR SHARE OF t1;
SELECT t1.c1, t2.c1 FROM ft1 t1 JOIN ft2 t2 ON (t1.c1 = t2.c1) ORDER BY t1.c3, t1.c1 OFFSET 100 LIMIT 10 FOR SHARE OF t1;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.c1, t2.c1 FROM ft1 t1 JOIN ft2 t2 ON (t1.c1 = t2.c1) ORDER BY t1.c3, t1.c1 OFFSET 100 LIMIT 10 FOR SHARE;
SELECT t1.c1, t2.c1 FROM ft1 t1 JOIN ft2 t2 ON (t1.c1 = t2.c1) ORDER BY t1.c3, t1.c1 OFFSET 100 LIMIT 10 FOR SHARE;
-- join in CTE
--
-- 加入CTE
EXPLAIN (VERBOSE, COSTS OFF)
WITH t (c1_1, c1_3, c2_1) AS MATERIALIZED (SELECT t1.c1, t1.c3, t2.c1 FROM ft1 t1 JOIN ft2 t2 ON (t1.c1 = t2.c1)) SELECT c1_1, c2_1 FROM t ORDER BY c1_3, c1_1 OFFSET 100 LIMIT 10;
WITH t (c1_1, c1_3, c2_1) AS MATERIALIZED (SELECT t1.c1, t1.c3, t2.c1 FROM ft1 t1 JOIN ft2 t2 ON (t1.c1 = t2.c1)) SELECT c1_1, c2_1 FROM t ORDER BY c1_3, c1_1 OFFSET 100 LIMIT 10;
-- ctid with whole-row reference
--
-- 具有整行引用的 ctid
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.ctid, t1, t2, t1.c1 FROM ft1 t1 JOIN ft2 t2 ON (t1.c1 = t2.c1) ORDER BY t1.c3, t1.c1 OFFSET 100 LIMIT 10;
-- SEMI JOIN
--
-- 半连接
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.c1 FROM ft1 t1 WHERE EXISTS (SELECT 1 FROM ft2 t2 WHERE t1.c1 = t2.c1) ORDER BY t1.c1 OFFSET 100 LIMIT 10;
SELECT t1.c1 FROM ft1 t1 WHERE EXISTS (SELECT 1 FROM ft2 t2 WHERE t1.c1 = t2.c1) ORDER BY t1.c1 OFFSET 100 LIMIT 10;
-- ANTI JOIN, not pushed down
--
-- ANTI JOIN，不被推倒
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.c1 FROM ft1 t1 WHERE NOT EXISTS (SELECT 1 FROM ft2 t2 WHERE t1.c1 = t2.c2) ORDER BY t1.c1 OFFSET 100 LIMIT 10;
SELECT t1.c1 FROM ft1 t1 WHERE NOT EXISTS (SELECT 1 FROM ft2 t2 WHERE t1.c1 = t2.c2) ORDER BY t1.c1 OFFSET 100 LIMIT 10;
-- CROSS JOIN can be pushed down
--
-- CROSS JOIN 可以下推
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.c1, t2.c1 FROM ft1 t1 CROSS JOIN ft2 t2 ORDER BY t1.c1, t2.c1 OFFSET 100 LIMIT 10;
SELECT t1.c1, t2.c1 FROM ft1 t1 CROSS JOIN ft2 t2 ORDER BY t1.c1, t2.c1 OFFSET 100 LIMIT 10;
-- different server, not pushed down. No result expected.
--
-- 不同服务器，不推倒。没有预期的结果。
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.c1, t2.c1 FROM ft5 t1 JOIN ft6 t2 ON (t1.c1 = t2.c1) ORDER BY t1.c1, t2.c1 OFFSET 100 LIMIT 10;
SELECT t1.c1, t2.c1 FROM ft5 t1 JOIN ft6 t2 ON (t1.c1 = t2.c1) ORDER BY t1.c1, t2.c1 OFFSET 100 LIMIT 10;
-- unsafe join conditions (c8 has a UDT), not pushed down. Practically a CROSS
--
-- 不安全的连接条件（c8有一个UDT），不被推倒。几乎是一个十字架
-- JOIN since c8 in both tables has same value.
--
-- JOIN 因为两个表中的 c8 具有相同的值。
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.c1, t2.c1 FROM ft1 t1 LEFT JOIN ft2 t2 ON (t1.c8 = t2.c8) ORDER BY t1.c1, t2.c1 OFFSET 100 LIMIT 10;
SELECT t1.c1, t2.c1 FROM ft1 t1 LEFT JOIN ft2 t2 ON (t1.c8 = t2.c8) ORDER BY t1.c1, t2.c1 OFFSET 100 LIMIT 10;
-- unsafe conditions on one side (c8 has a UDT), not pushed down.
--
-- 一侧存在不安全情况（c8 有 UDT），未推倒。
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.c1, t2.c1 FROM ft1 t1 LEFT JOIN ft2 t2 ON (t1.c1 = t2.c1) WHERE t1.c8 = 'foo' ORDER BY t1.c3, t1.c1 OFFSET 100 LIMIT 10;
SELECT t1.c1, t2.c1 FROM ft1 t1 LEFT JOIN ft2 t2 ON (t1.c1 = t2.c1) WHERE t1.c8 = 'foo' ORDER BY t1.c3, t1.c1 OFFSET 100 LIMIT 10;
-- join where unsafe to pushdown condition in WHERE clause has a column not
--
-- join where 对 WHERE 子句中的下推条件不安全的列不安全
-- in the SELECT clause. In this test unsafe clause needs to have column
--
-- 在 SELECT 子句中。在此测试中，不安全子句需要有列
-- references from both joining sides so that the clause is not pushed down
--
-- 加入双方的参考资料，以免该条款被推倒
-- into one of the joining sides.
--
-- 进入连接侧之一。
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.c1, t2.c1 FROM ft1 t1 JOIN ft2 t2 ON (t1.c1 = t2.c1) WHERE t1.c8 = t2.c8 ORDER BY t1.c3, t1.c1 OFFSET 100 LIMIT 10;
SELECT t1.c1, t2.c1 FROM ft1 t1 JOIN ft2 t2 ON (t1.c1 = t2.c1) WHERE t1.c8 = t2.c8 ORDER BY t1.c3, t1.c1 OFFSET 100 LIMIT 10;
-- Aggregate after UNION, for testing setrefs
--
-- UNION 之后聚合，用于测试 setrefs
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1c1, avg(t1c1 + t2c1) FROM (SELECT t1.c1, t2.c1 FROM ft1 t1 JOIN ft2 t2 ON (t1.c1 = t2.c1) UNION SELECT t1.c1, t2.c1 FROM ft1 t1 JOIN ft2 t2 ON (t1.c1 = t2.c1)) AS t (t1c1, t2c1) GROUP BY t1c1 ORDER BY t1c1 OFFSET 100 LIMIT 10;
SELECT t1c1, avg(t1c1 + t2c1) FROM (SELECT t1.c1, t2.c1 FROM ft1 t1 JOIN ft2 t2 ON (t1.c1 = t2.c1) UNION SELECT t1.c1, t2.c1 FROM ft1 t1 JOIN ft2 t2 ON (t1.c1 = t2.c1)) AS t (t1c1, t2c1) GROUP BY t1c1 ORDER BY t1c1 OFFSET 100 LIMIT 10;
-- join with lateral reference
--
-- 与横向参考连接
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1."C 1" FROM "S 1"."T 1" t1, LATERAL (SELECT DISTINCT t2.c1, t3.c1 FROM ft1 t2, ft2 t3 WHERE t2.c1 = t3.c1 AND t2.c2 = t1.c2) q ORDER BY t1."C 1" OFFSET 10 LIMIT 10;
SELECT t1."C 1" FROM "S 1"."T 1" t1, LATERAL (SELECT DISTINCT t2.c1, t3.c1 FROM ft1 t2, ft2 t3 WHERE t2.c1 = t3.c1 AND t2.c2 = t1.c2) q ORDER BY t1."C 1" OFFSET 10 LIMIT 10;
-- join with pseudoconstant quals
--
-- 与伪常量连接
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.c1, t2.c1 FROM ft1 t1 JOIN ft2 t2 ON (t1.c1 = t2.c1 AND CURRENT_USER = SESSION_USER) ORDER BY t1.c3, t1.c1 OFFSET 100 LIMIT 10;

-- non-Var items in targetlist of the nullable rel of a join preventing
--
-- 连接的可空 rel 的 targetlist 中的非 Var 项阻止
-- push-down in some cases
--
-- 在某些情况下下推
-- unable to push {ft1, ft2}
--
-- 无法推送 {ft1, ft2}
EXPLAIN (VERBOSE, COSTS OFF)
SELECT q.a, ft2.c1 FROM (SELECT 13 FROM ft1 WHERE c1 = 13) q(a) RIGHT JOIN ft2 ON (q.a = ft2.c1) WHERE ft2.c1 BETWEEN 10 AND 15;
SELECT q.a, ft2.c1 FROM (SELECT 13 FROM ft1 WHERE c1 = 13) q(a) RIGHT JOIN ft2 ON (q.a = ft2.c1) WHERE ft2.c1 BETWEEN 10 AND 15;

-- ok to push {ft1, ft2} but not {ft1, ft2, ft4}
--
-- 可以推 {ft1, ft2} 但不能推 {ft1, ft2, ft4}
EXPLAIN (VERBOSE, COSTS OFF)
SELECT ft4.c1, q.* FROM ft4 LEFT JOIN (SELECT 13, ft1.c1, ft2.c1 FROM ft1 RIGHT JOIN ft2 ON (ft1.c1 = ft2.c1) WHERE ft1.c1 = 12) q(a, b, c) ON (ft4.c1 = q.b) WHERE ft4.c1 BETWEEN 10 AND 15;
SELECT ft4.c1, q.* FROM ft4 LEFT JOIN (SELECT 13, ft1.c1, ft2.c1 FROM ft1 RIGHT JOIN ft2 ON (ft1.c1 = ft2.c1) WHERE ft1.c1 = 12) q(a, b, c) ON (ft4.c1 = q.b) WHERE ft4.c1 BETWEEN 10 AND 15;

-- join with nullable side with some columns with null values
--
-- 将可空一侧与某些具有空值的列连接
UPDATE ft5 SET c3 = null where c1 % 9 = 0;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT ft5, ft5.c1, ft5.c2, ft5.c3, ft4.c1, ft4.c2 FROM ft5 left join ft4 on ft5.c1 = ft4.c1 WHERE ft4.c1 BETWEEN 10 and 30 ORDER BY ft5.c1, ft4.c1;
SELECT ft5, ft5.c1, ft5.c2, ft5.c3, ft4.c1, ft4.c2 FROM ft5 left join ft4 on ft5.c1 = ft4.c1 WHERE ft4.c1 BETWEEN 10 and 30 ORDER BY ft5.c1, ft4.c1;

-- multi-way join involving multiple merge joins
--
-- 涉及多个合并连接的多路连接
-- (this case used to have EPQ-related planning problems)
--
-- （本案例曾经有EPQ相关的规划问题）
CREATE TABLE local_tbl (c1 int NOT NULL, c2 int NOT NULL, c3 text, CONSTRAINT local_tbl_pkey PRIMARY KEY (c1));
INSERT INTO local_tbl SELECT id, id % 10, to_char(id, 'FM0000') FROM generate_series(1, 1000) id;
ANALYZE local_tbl;
SET enable_nestloop TO false;
SET enable_hashjoin TO false;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM ft1, ft2, ft4, ft5, local_tbl WHERE ft1.c1 = ft2.c1 AND ft1.c2 = ft4.c1
    AND ft1.c2 = ft5.c1 AND ft1.c2 = local_tbl.c1 AND ft1.c1 < 100 AND ft2.c1 < 100 FOR UPDATE;
SELECT * FROM ft1, ft2, ft4, ft5, local_tbl WHERE ft1.c1 = ft2.c1 AND ft1.c2 = ft4.c1
    AND ft1.c2 = ft5.c1 AND ft1.c2 = local_tbl.c1 AND ft1.c1 < 100 AND ft2.c1 < 100 FOR UPDATE;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM ft1, ft4, ft5, local_tbl WHERE ft1.c1 = ft4.c1 AND ft1.c1 = ft5.c1
    AND ft1.c2 = local_tbl.c1 AND ft1.c1 < 100 AND ft5.c1 < 100 FOR UPDATE;
SELECT * FROM ft1, ft4, ft5, local_tbl WHERE ft1.c1 = ft4.c1 AND ft1.c1 = ft5.c1
    AND ft1.c2 = local_tbl.c1 AND ft1.c1 < 100 AND ft5.c1 < 100 FOR UPDATE;
RESET enable_nestloop;
RESET enable_hashjoin;

-- test that add_paths_with_pathkeys_for_rel() arranges for the epq_path to
--
-- 测试 add_paths_with_pathkeys_for_rel() 是否将 epq_path 安排为
-- return columns needed by the parent ForeignScan node
--
-- 返回父ForeignScan节点所需的列
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM local_tbl LEFT JOIN (SELECT ft1.*, COALESCE(ft1.c3 || ft2.c3, 'foobar') FROM ft1 INNER JOIN ft2 ON (ft1.c1 = ft2.c1 AND ft1.c1 < 100)) ss ON (local_tbl.c1 = ss.c1) ORDER BY local_tbl.c1 FOR UPDATE OF local_tbl;

ALTER SERVER loopback OPTIONS (DROP extensions);
ALTER SERVER loopback OPTIONS (ADD fdw_startup_cost '10000.0');
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM local_tbl LEFT JOIN (SELECT ft1.* FROM ft1 INNER JOIN ft2 ON (ft1.c1 = ft2.c1 AND ft1.c1 < 100 AND (ft1.c1 - postgres_fdw_abs(ft2.c2)) = 0)) ss ON (local_tbl.c3 = ss.c3) ORDER BY local_tbl.c1 FOR UPDATE OF local_tbl;
ALTER SERVER loopback OPTIONS (DROP fdw_startup_cost);
ALTER SERVER loopback OPTIONS (ADD extensions 'postgres_fdw');

DROP TABLE local_tbl;

-- check join pushdown in situations where multiple userids are involved
--
-- 在涉及多个用户 ID 的情况下检查连接下推
CREATE ROLE regress_view_owner SUPERUSER;
CREATE USER MAPPING FOR regress_view_owner SERVER loopback;
GRANT SELECT ON ft4 TO regress_view_owner;
GRANT SELECT ON ft5 TO regress_view_owner;

CREATE VIEW v4 AS SELECT * FROM ft4;
CREATE VIEW v5 AS SELECT * FROM ft5;
ALTER VIEW v5 OWNER TO regress_view_owner;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.c1, t2.c2 FROM v4 t1 LEFT JOIN v5 t2 ON (t1.c1 = t2.c1) ORDER BY t1.c1, t2.c1 OFFSET 10 LIMIT 10;  -- can't be pushed down, different view owners
SELECT t1.c1, t2.c2 FROM v4 t1 LEFT JOIN v5 t2 ON (t1.c1 = t2.c1) ORDER BY t1.c1, t2.c1 OFFSET 10 LIMIT 10;
ALTER VIEW v4 OWNER TO regress_view_owner;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.c1, t2.c2 FROM v4 t1 LEFT JOIN v5 t2 ON (t1.c1 = t2.c1) ORDER BY t1.c1, t2.c1 OFFSET 10 LIMIT 10;  -- can be pushed down
SELECT t1.c1, t2.c2 FROM v4 t1 LEFT JOIN v5 t2 ON (t1.c1 = t2.c1) ORDER BY t1.c1, t2.c1 OFFSET 10 LIMIT 10;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.c1, t2.c2 FROM v4 t1 LEFT JOIN ft5 t2 ON (t1.c1 = t2.c1) ORDER BY t1.c1, t2.c1 OFFSET 10 LIMIT 10;  -- can't be pushed down, view owner not current user
SELECT t1.c1, t2.c2 FROM v4 t1 LEFT JOIN ft5 t2 ON (t1.c1 = t2.c1) ORDER BY t1.c1, t2.c1 OFFSET 10 LIMIT 10;
ALTER VIEW v4 OWNER TO CURRENT_USER;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.c1, t2.c2 FROM v4 t1 LEFT JOIN ft5 t2 ON (t1.c1 = t2.c1) ORDER BY t1.c1, t2.c1 OFFSET 10 LIMIT 10;  -- can be pushed down
SELECT t1.c1, t2.c2 FROM v4 t1 LEFT JOIN ft5 t2 ON (t1.c1 = t2.c1) ORDER BY t1.c1, t2.c1 OFFSET 10 LIMIT 10;
ALTER VIEW v4 OWNER TO regress_view_owner;

-- ====================================================================
-- Check that userid to use when querying the remote table is correctly
--
-- 检查查询远程表时使用的userid是否正确
-- propagated into foreign rels present in subqueries under an UNION ALL
--
-- 传播到 UNION ALL 下子查询中存在的外部关系中
-- ====================================================================
CREATE ROLE regress_view_owner_another;
ALTER VIEW v4 OWNER TO regress_view_owner_another;
GRANT SELECT ON ft4 TO regress_view_owner_another;
ALTER FOREIGN TABLE ft4 OPTIONS (ADD use_remote_estimate 'true');
-- The following should query the remote backing table of ft4 as user
--
-- 以下应以用户身份查询 ft4 的远程备份表
-- regress_view_owner_another, the view owner, though it fails as expected
--
-- regress_view_owner_another，视图所有者，尽管它按预期失败
-- due to the lack of a user mapping for that user.
--
-- 由于缺少该用户的用户映射。
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM v4;
-- Likewise, but with the query under an UNION ALL
--
-- 同样，但查询位于 UNION ALL 下
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM (SELECT * FROM v4 UNION ALL SELECT * FROM v4);
-- Should not get that error once a user mapping is created
--
-- 创建用户映射后不应出现该错误
CREATE USER MAPPING FOR regress_view_owner_another SERVER loopback OPTIONS (password_required 'false');
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM v4;
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM (SELECT * FROM v4 UNION ALL SELECT * FROM v4);
DROP USER MAPPING FOR regress_view_owner_another SERVER loopback;
DROP OWNED BY regress_view_owner_another;
DROP ROLE regress_view_owner_another;
ALTER FOREIGN TABLE ft4 OPTIONS (SET use_remote_estimate 'false');

-- cleanup
DROP OWNED BY regress_view_owner;
DROP ROLE regress_view_owner;


-- ===================================================================
-- Aggregate and grouping queries
--
-- 聚合和分组查询
-- ===================================================================

-- Simple aggregates
--
-- 简单聚合
explain (verbose, costs off)
select count(c6), sum(c1), avg(c1), min(c2), max(c1), stddev(c2), sum(c1) * (random() <= 1)::int as sum2 from ft1 where c2 < 5 group by c2 order by 1, 2;
select count(c6), sum(c1), avg(c1), min(c2), max(c1), stddev(c2), sum(c1) * (random() <= 1)::int as sum2 from ft1 where c2 < 5 group by c2 order by 1, 2;

explain (verbose, costs off)
select count(c6), sum(c1), avg(c1), min(c2), max(c1), stddev(c2), sum(c1) * (random() <= 1)::int as sum2 from ft1 where c2 < 5 group by c2 order by 1, 2 limit 1;
select count(c6), sum(c1), avg(c1), min(c2), max(c1), stddev(c2), sum(c1) * (random() <= 1)::int as sum2 from ft1 where c2 < 5 group by c2 order by 1, 2 limit 1;

-- Aggregate is not pushed down as aggregation contains random()
--
-- 聚合不会被下推，因为聚合包含 random()
explain (verbose, costs off)
select sum(c1 * (random() <= 1)::int) as sum, avg(c1) from ft1;

-- Aggregate over join query
--
-- 通过连接查询聚合
explain (verbose, costs off)
select count(*), sum(t1.c1), avg(t2.c1) from ft1 t1 inner join ft1 t2 on (t1.c2 = t2.c2) where t1.c2 = 6;
select count(*), sum(t1.c1), avg(t2.c1) from ft1 t1 inner join ft1 t2 on (t1.c2 = t2.c2) where t1.c2 = 6;

-- Not pushed down due to local conditions present in underneath input rel
--
-- 由于下面的输入 rel 中存在本地条件，因此未向下推
explain (verbose, costs off)
select sum(t1.c1), count(t2.c1) from ft1 t1 inner join ft2 t2 on (t1.c1 = t2.c1) where ((t1.c1 * t2.c1)/(t1.c1 * t2.c1)) * random() <= 1;

-- GROUP BY clause having expressions
--
-- 具有表达式的 GROUP BY 子句
explain (verbose, costs off)
select c2/2, sum(c2) * (c2/2) from ft1 group by c2/2 order by c2/2;
select c2/2, sum(c2) * (c2/2) from ft1 group by c2/2 order by c2/2;

-- Aggregates in subquery are pushed down.
--
-- 子查询中的聚合被下推。
set enable_incremental_sort = off;
explain (verbose, costs off)
select count(x.a), sum(x.a) from (select c2 a, sum(c1) b from ft1 group by c2, sqrt(c1) order by 1, 2) x;
select count(x.a), sum(x.a) from (select c2 a, sum(c1) b from ft1 group by c2, sqrt(c1) order by 1, 2) x;
reset enable_incremental_sort;

-- Aggregate is still pushed down by taking unshippable expression out
--
-- 通过取出无法传送的表达式，聚合仍然被压低
explain (verbose, costs off)
select c2 * (random() <= 1)::int as sum1, sum(c1) * c2 as sum2 from ft1 group by c2 order by 1, 2;
select c2 * (random() <= 1)::int as sum1, sum(c1) * c2 as sum2 from ft1 group by c2 order by 1, 2;

-- Aggregate with unshippable GROUP BY clause are not pushed
--
-- 具有不可发送的 GROUP BY 子句的聚合不会被推送
explain (verbose, costs off)
select c2 * (random() <= 1)::int as c2 from ft2 group by c2 * (random() <= 1)::int order by 1;

-- GROUP BY clause in various forms, cardinal, alias and constant expression
--
-- 各种形式的 GROUP BY 子句、基数、别名和常量表达式
explain (verbose, costs off)
select count(c2) w, c2 x, 5 y, 7.0 z from ft1 group by 2, y, 9.0::int order by 2;
select count(c2) w, c2 x, 5 y, 7.0 z from ft1 group by 2, y, 9.0::int order by 2;

-- GROUP BY clause referring to same column multiple times
--
-- GROUP BY 子句多次引用同一列
-- Also, ORDER BY contains an aggregate function
--
-- 另外，ORDER BY 包含一个聚合函数
explain (verbose, costs off)
select c2, c2 from ft1 where c2 > 6 group by 1, 2 order by sum(c1);
select c2, c2 from ft1 where c2 > 6 group by 1, 2 order by sum(c1);

-- Testing HAVING clause shippability
--
-- 测试 HAVING 条款的可运输性
explain (verbose, costs off)
select c2, sum(c1) from ft2 group by c2 having avg(c1) < 500 and sum(c1) < 49800 order by c2;
select c2, sum(c1) from ft2 group by c2 having avg(c1) < 500 and sum(c1) < 49800 order by c2;

-- Unshippable HAVING clause will be evaluated locally, and other qual in HAVING clause is pushed down
--
-- 不可交付的 HAVING 子句将在本地求值，HAVING 子句中的其他限定将被下推
explain (verbose, costs off)
select count(*) from (select c5, count(c1) from ft1 group by c5, sqrt(c2) having (avg(c1) / avg(c1)) * random() <= 1 and avg(c1) < 500) x;
select count(*) from (select c5, count(c1) from ft1 group by c5, sqrt(c2) having (avg(c1) / avg(c1)) * random() <= 1 and avg(c1) < 500) x;

-- Aggregate in HAVING clause is not pushable, and thus aggregation is not pushed down
--
-- HAVING 子句中的聚合不可推送，因此聚合不会被下推
explain (verbose, costs off)
select sum(c1) from ft1 group by c2 having avg(c1 * (random() <= 1)::int) > 100 order by 1;

-- Remote aggregate in combination with a local Param (for the output
--
-- 远程聚合与本地参数相结合（用于输出
-- of an initplan) can be trouble, per bug #15781
--
-- 一个 initplan 的）可能会很麻烦，根据 bug #15781
explain (verbose, costs off)
select exists(select 1 from pg_enum), sum(c1) from ft1;
select exists(select 1 from pg_enum), sum(c1) from ft1;

explain (verbose, costs off)
select exists(select 1 from pg_enum), sum(c1) from ft1 group by 1;
select exists(select 1 from pg_enum), sum(c1) from ft1 group by 1;


-- Testing ORDER BY, DISTINCT, FILTER, Ordered-sets and VARIADIC within aggregates
--
-- 测试聚合内的 ORDER BY、DISTINCT、FILTER、有序集和 VARIADIC

-- ORDER BY within aggregate, same column used to order
--
-- 聚合内的 ORDER BY，用于排序的同一列
explain (verbose, costs off)
select array_agg(c1 order by c1) from ft1 where c1 < 100 group by c2 order by 1;
select array_agg(c1 order by c1) from ft1 where c1 < 100 group by c2 order by 1;

-- ORDER BY within aggregate, different column used to order also using DESC
--
-- 聚合内的 ORDER BY，用于排序的不同列也使用 DESC
explain (verbose, costs off)
select array_agg(c5 order by c1 desc) from ft2 where c2 = 6 and c1 < 50;
select array_agg(c5 order by c1 desc) from ft2 where c2 = 6 and c1 < 50;

-- DISTINCT within aggregate
--
-- 聚合中的不同
explain (verbose, costs off)
select array_agg(distinct (t1.c1)%5) from ft4 t1 full join ft5 t2 on (t1.c1 = t2.c1) where t1.c1 < 20 or (t1.c1 is null and t2.c1 < 5) group by (t2.c1)%3 order by 1;
select array_agg(distinct (t1.c1)%5) from ft4 t1 full join ft5 t2 on (t1.c1 = t2.c1) where t1.c1 < 20 or (t1.c1 is null and t2.c1 < 5) group by (t2.c1)%3 order by 1;

-- DISTINCT combined with ORDER BY within aggregate
--
-- DISTINCT 与聚合内的 ORDER BY 相结合
explain (verbose, costs off)
select array_agg(distinct (t1.c1)%5 order by (t1.c1)%5) from ft4 t1 full join ft5 t2 on (t1.c1 = t2.c1) where t1.c1 < 20 or (t1.c1 is null and t2.c1 < 5) group by (t2.c1)%3 order by 1;
select array_agg(distinct (t1.c1)%5 order by (t1.c1)%5) from ft4 t1 full join ft5 t2 on (t1.c1 = t2.c1) where t1.c1 < 20 or (t1.c1 is null and t2.c1 < 5) group by (t2.c1)%3 order by 1;

explain (verbose, costs off)
select array_agg(distinct (t1.c1)%5 order by (t1.c1)%5 desc nulls last) from ft4 t1 full join ft5 t2 on (t1.c1 = t2.c1) where t1.c1 < 20 or (t1.c1 is null and t2.c1 < 5) group by (t2.c1)%3 order by 1;
select array_agg(distinct (t1.c1)%5 order by (t1.c1)%5 desc nulls last) from ft4 t1 full join ft5 t2 on (t1.c1 = t2.c1) where t1.c1 < 20 or (t1.c1 is null and t2.c1 < 5) group by (t2.c1)%3 order by 1;

-- FILTER within aggregate
--
-- 聚合内的过滤器
explain (verbose, costs off)
select sum(c1) filter (where c1 < 100 and c2 > 5) from ft1 group by c2 order by 1 nulls last;
select sum(c1) filter (where c1 < 100 and c2 > 5) from ft1 group by c2 order by 1 nulls last;

-- DISTINCT, ORDER BY and FILTER within aggregate
--
-- 聚合中的 DISTINCT、ORDER BY 和 FILTER
explain (verbose, costs off)
select sum(c1%3), sum(distinct c1%3 order by c1%3) filter (where c1%3 < 2), c2 from ft1 where c2 = 6 group by c2;
select sum(c1%3), sum(distinct c1%3 order by c1%3) filter (where c1%3 < 2), c2 from ft1 where c2 = 6 group by c2;

-- Outer query is aggregation query
--
-- 外层查询为聚合查询
explain (verbose, costs off)
select distinct (select count(*) filter (where t2.c2 = 6 and t2.c1 < 10) from ft1 t1 where t1.c1 = 6) from ft2 t2 where t2.c2 % 6 = 0 order by 1;
select distinct (select count(*) filter (where t2.c2 = 6 and t2.c1 < 10) from ft1 t1 where t1.c1 = 6) from ft2 t2 where t2.c2 % 6 = 0 order by 1;
-- Inner query is aggregation query
--
-- 内部查询是聚合查询
explain (verbose, costs off)
select distinct (select count(t1.c1) filter (where t2.c2 = 6 and t2.c1 < 10) from ft1 t1 where t1.c1 = 6) from ft2 t2 where t2.c2 % 6 = 0 order by 1;
select distinct (select count(t1.c1) filter (where t2.c2 = 6 and t2.c1 < 10) from ft1 t1 where t1.c1 = 6) from ft2 t2 where t2.c2 % 6 = 0 order by 1;

-- Aggregate not pushed down as FILTER condition is not pushable
--
-- 聚合未下推，因为 FILTER 条件不可下推
explain (verbose, costs off)
select sum(c1) filter (where (c1 / c1) * random() <= 1) from ft1 group by c2 order by 1;
explain (verbose, costs off)
select sum(c2) filter (where c2 in (select c2 from ft1 where c2 < 5)) from ft1;

-- Ordered-sets within aggregate
--
-- 聚合内的有序集
explain (verbose, costs off)
select c2, rank('10'::varchar) within group (order by c6), percentile_cont(c2/10::numeric) within group (order by c1) from ft1 where c2 < 10 group by c2 having percentile_cont(c2/10::numeric) within group (order by c1) < 500 order by c2;
select c2, rank('10'::varchar) within group (order by c6), percentile_cont(c2/10::numeric) within group (order by c1) from ft1 where c2 < 10 group by c2 having percentile_cont(c2/10::numeric) within group (order by c1) < 500 order by c2;

-- Using multiple arguments within aggregates
--
-- 在聚合中使用多个参数
explain (verbose, costs off)
select c1, rank(c1, c2) within group (order by c1, c2) from ft1 group by c1, c2 having c1 = 6 order by 1;
select c1, rank(c1, c2) within group (order by c1, c2) from ft1 group by c1, c2 having c1 = 6 order by 1;

-- User defined function for user defined aggregate, VARIADIC
--
-- 用户定义聚合的用户定义函数，VARIADIC
create function least_accum(anyelement, variadic anyarray)
returns anyelement language sql as
  'select least($1, min($2[i])) from generate_subscripts($2,1) g(i)';
create aggregate least_agg(variadic items anyarray) (
  stype = anyelement, sfunc = least_accum
);

-- Disable hash aggregation for plan stability.
--
-- 禁用哈希聚合以保证计划稳定性。
set enable_hashagg to false;

-- Not pushed down due to user defined aggregate
--
-- 由于用户定义的聚合而未下推
explain (verbose, costs off)
select c2, least_agg(c1) from ft1 group by c2 order by c2;

-- Add function and aggregate into extension
--
-- 添加功能并聚合到扩展中
alter extension postgres_fdw add function least_accum(anyelement, variadic anyarray);
alter extension postgres_fdw add aggregate least_agg(variadic items anyarray);
alter server loopback options (set extensions 'postgres_fdw');

-- Now aggregate will be pushed.  Aggregate will display VARIADIC argument.
--
-- 现在将推送聚合。  聚合将显示 VARIADIC 参数。
explain (verbose, costs off)
select c2, least_agg(c1) from ft1 where c2 < 100 group by c2 order by c2;
select c2, least_agg(c1) from ft1 where c2 < 100 group by c2 order by c2;

-- Remove function and aggregate from extension
--
-- 从扩展中删除函数和聚合
alter extension postgres_fdw drop function least_accum(anyelement, variadic anyarray);
alter extension postgres_fdw drop aggregate least_agg(variadic items anyarray);
alter server loopback options (set extensions 'postgres_fdw');

-- Not pushed down as we have dropped objects from extension.
--
-- 没有被推下，因为我们已经从扩展中删除了对象。
explain (verbose, costs off)
select c2, least_agg(c1) from ft1 group by c2 order by c2;

-- Cleanup
reset enable_hashagg;
drop aggregate least_agg(variadic items anyarray);
drop function least_accum(anyelement, variadic anyarray);


-- Testing USING OPERATOR() in ORDER BY within aggregate.
--
-- 在聚合内的 ORDER BY 中使用 OPERATOR() 进行测试。
-- For this, we need user defined operators along with operator family and
--
-- 为此，我们需要用户定义的运算符以及运算符系列和
-- operator class.  Create those and then add them in extension.  Note that
--
-- 运算符类。  创建它们，然后将它们添加到扩展中。  注意
-- user defined objects are considered unshippable unless they are part of
--
-- 用户定义的对象被认为是不可交付的，除非它们是
-- the extension.
--
-- 扩展名。
create operator public.<^ (
 leftarg = int4,
 rightarg = int4,
 procedure = int4eq
);

create operator public.=^ (
 leftarg = int4,
 rightarg = int4,
 procedure = int4lt
);

create operator public.>^ (
 leftarg = int4,
 rightarg = int4,
 procedure = int4gt
);

create operator family my_op_family using btree;

create function my_op_cmp(a int, b int) returns int as
  $$begin return btint4cmp(a, b); end $$ language plpgsql;

create operator class my_op_class for type int using btree family my_op_family as
 operator 1 public.<^,
 operator 3 public.=^,
 operator 5 public.>^,
 function 1 my_op_cmp(int, int);

-- This will not be pushed as user defined sort operator is not part of the
--
-- 这不会被推送，因为用户定义的排序运算符不是
-- extension yet.
--
-- 还没有延期。
explain (verbose, costs off)
select array_agg(c1 order by c1 using operator(public.<^)) from ft2 where c2 = 6 and c1 < 100 group by c2;

-- This should not be pushed either.
--
-- 这也不应该被推动。
explain (verbose, costs off)
select * from ft2 order by c1 using operator(public.<^);

-- Update local stats on ft2
--
-- 更新 ft2 上的本地统计信息
ANALYZE ft2;

-- Add into extension
--
-- 添加到扩展程序中
alter extension postgres_fdw add operator class my_op_class using btree;
alter extension postgres_fdw add function my_op_cmp(a int, b int);
alter extension postgres_fdw add operator family my_op_family using btree;
alter extension postgres_fdw add operator public.<^(int, int);
alter extension postgres_fdw add operator public.=^(int, int);
alter extension postgres_fdw add operator public.>^(int, int);
alter server loopback options (set extensions 'postgres_fdw');

-- Now this will be pushed as sort operator is part of the extension.
--
-- 现在这将被推送，因为排序运算符是扩展的一部分。
alter server loopback options (add fdw_tuple_cost '0.5');
explain (verbose, costs off)
select array_agg(c1 order by c1 using operator(public.<^)) from ft2 where c2 = 6 and c1 < 100 group by c2;
select array_agg(c1 order by c1 using operator(public.<^)) from ft2 where c2 = 6 and c1 < 100 group by c2;
alter server loopback options (drop fdw_tuple_cost);

-- This should be pushed too.
--
-- 这个也应该推一下。
explain (verbose, costs off)
select * from ft2 order by c1 using operator(public.<^);

-- Remove from extension
--
-- 从扩展中删除
alter extension postgres_fdw drop operator class my_op_class using btree;
alter extension postgres_fdw drop function my_op_cmp(a int, b int);
alter extension postgres_fdw drop operator family my_op_family using btree;
alter extension postgres_fdw drop operator public.<^(int, int);
alter extension postgres_fdw drop operator public.=^(int, int);
alter extension postgres_fdw drop operator public.>^(int, int);
alter server loopback options (set extensions 'postgres_fdw');

-- This will not be pushed as sort operator is now removed from the extension.
--
-- 由于排序运算符现已从扩展中删除，因此不会推送此操作。
explain (verbose, costs off)
select array_agg(c1 order by c1 using operator(public.<^)) from ft2 where c2 = 6 and c1 < 100 group by c2;

-- Cleanup
drop operator class my_op_class using btree;
drop function my_op_cmp(a int, b int);
drop operator family my_op_family using btree;
drop operator public.>^(int, int);
drop operator public.=^(int, int);
drop operator public.<^(int, int);

-- Input relation to aggregate push down hook is not safe to pushdown and thus
--
-- 与聚合下推钩子的输入关系对于下推来说是不安全的，因此
-- the aggregate cannot be pushed down to foreign server.
--
-- 聚合无法下推到外部服务器。
explain (verbose, costs off)
select count(t1.c3) from ft2 t1 left join ft2 t2 on (t1.c1 = random() * t2.c2);

-- Subquery in FROM clause having aggregate
--
-- FROM 子句中具有聚合的子查询
explain (verbose, costs off)
select count(*), x.b from ft1, (select c2 a, sum(c1) b from ft1 group by c2) x where ft1.c2 = x.a group by x.b order by 1, 2;
select count(*), x.b from ft1, (select c2 a, sum(c1) b from ft1 group by c2) x where ft1.c2 = x.a group by x.b order by 1, 2;

-- FULL join with IS NULL check in HAVING
--
-- FULL 连接并在 HAVING 中检查 IS NULL
explain (verbose, costs off)
select avg(t1.c1), sum(t2.c1) from ft4 t1 full join ft5 t2 on (t1.c1 = t2.c1) group by t2.c1 having (avg(t1.c1) is null and sum(t2.c1) < 10) or sum(t2.c1) is null order by 1 nulls last, 2;
select avg(t1.c1), sum(t2.c1) from ft4 t1 full join ft5 t2 on (t1.c1 = t2.c1) group by t2.c1 having (avg(t1.c1) is null and sum(t2.c1) < 10) or sum(t2.c1) is null order by 1 nulls last, 2;

-- Aggregate over FULL join needing to deparse the joining relations as
--
-- 聚合完整连接需要将连接关系分解为
-- subqueries.
explain (verbose, costs off)
select count(*), sum(t1.c1), avg(t2.c1) from (select c1 from ft4 where c1 between 50 and 60) t1 full join (select c1 from ft5 where c1 between 50 and 60) t2 on (t1.c1 = t2.c1);
select count(*), sum(t1.c1), avg(t2.c1) from (select c1 from ft4 where c1 between 50 and 60) t1 full join (select c1 from ft5 where c1 between 50 and 60) t2 on (t1.c1 = t2.c1);

-- ORDER BY expression is part of the target list but not pushed down to
--
-- ORDER BY 表达式是目标列表的一部分，但未下推到
-- foreign server.
--
-- 国外服务器。
explain (verbose, costs off)
select sum(c2) * (random() <= 1)::int as sum from ft1 order by 1;
select sum(c2) * (random() <= 1)::int as sum from ft1 order by 1;

-- LATERAL join, with parameterization
--
-- 横向连接，带参数化
set enable_hashagg to false;
explain (verbose, costs off)
select c2, sum from "S 1"."T 1" t1, lateral (select sum(t2.c1 + t1."C 1") sum from ft2 t2 group by t2.c1) qry where t1.c2 * 2 = qry.sum and t1.c2 < 3 and t1."C 1" < 100 order by 1;
select c2, sum from "S 1"."T 1" t1, lateral (select sum(t2.c1 + t1."C 1") sum from ft2 t2 group by t2.c1) qry where t1.c2 * 2 = qry.sum and t1.c2 < 3 and t1."C 1" < 100 order by 1;
reset enable_hashagg;

-- bug #15613: bad plan for foreign table scan with lateral reference
--
-- bug #15613：使用横向引用进行外部表扫描的糟糕计划
EXPLAIN (VERBOSE, COSTS OFF)
SELECT ref_0.c2, subq_1.*
FROM
    "S 1"."T 1" AS ref_0,
    LATERAL (
        SELECT ref_0."C 1" c1, subq_0.*
        FROM (SELECT ref_0.c2, ref_1.c3
              FROM ft1 AS ref_1) AS subq_0
             RIGHT JOIN ft2 AS ref_3 ON (subq_0.c3 = ref_3.c3)
    ) AS subq_1
WHERE ref_0."C 1" < 10 AND subq_1.c3 = '00001'
ORDER BY ref_0."C 1";

SELECT ref_0.c2, subq_1.*
FROM
    "S 1"."T 1" AS ref_0,
    LATERAL (
        SELECT ref_0."C 1" c1, subq_0.*
        FROM (SELECT ref_0.c2, ref_1.c3
              FROM ft1 AS ref_1) AS subq_0
             RIGHT JOIN ft2 AS ref_3 ON (subq_0.c3 = ref_3.c3)
    ) AS subq_1
WHERE ref_0."C 1" < 10 AND subq_1.c3 = '00001'
ORDER BY ref_0."C 1";

-- Check with placeHolderVars
--
-- 检查 placeHolderVars
explain (verbose, costs off)
select sum(q.a), count(q.b) from ft4 left join (select 13, avg(ft1.c1), sum(ft2.c1) from ft1 right join ft2 on (ft1.c1 = ft2.c1)) q(a, b, c) on (ft4.c1 <= q.b);
select sum(q.a), count(q.b) from ft4 left join (select 13, avg(ft1.c1), sum(ft2.c1) from ft1 right join ft2 on (ft1.c1 = ft2.c1)) q(a, b, c) on (ft4.c1 <= q.b);


-- Not supported cases
--
-- 不支持的情况
-- Grouping sets
--
-- 分组集
explain (verbose, costs off)
select c2, sum(c1) from ft1 where c2 < 3 group by rollup(c2) order by 1 nulls last;
select c2, sum(c1) from ft1 where c2 < 3 group by rollup(c2) order by 1 nulls last;
explain (verbose, costs off)
select c2, sum(c1) from ft1 where c2 < 3 group by cube(c2) order by 1 nulls last;
select c2, sum(c1) from ft1 where c2 < 3 group by cube(c2) order by 1 nulls last;
explain (verbose, costs off)
select c2, c6, sum(c1) from ft1 where c2 < 3 group by grouping sets(c2, c6) order by 1 nulls last, 2 nulls last;
select c2, c6, sum(c1) from ft1 where c2 < 3 group by grouping sets(c2, c6) order by 1 nulls last, 2 nulls last;
explain (verbose, costs off)
select c2, sum(c1), grouping(c2) from ft1 where c2 < 3 group by c2 order by 1 nulls last;
select c2, sum(c1), grouping(c2) from ft1 where c2 < 3 group by c2 order by 1 nulls last;

-- DISTINCT itself is not pushed down, whereas underneath aggregate is pushed
--
-- DISTINCT 本身不会被下推，而下面的聚合会被下推
explain (verbose, costs off)
select distinct sum(c1)/1000 s from ft2 where c2 < 6 group by c2 order by 1;
select distinct sum(c1)/1000 s from ft2 where c2 < 6 group by c2 order by 1;

-- WindowAgg
explain (verbose, costs off)
select c2, sum(c2), count(c2) over (partition by c2%2) from ft2 where c2 < 10 group by c2 order by 1;
select c2, sum(c2), count(c2) over (partition by c2%2) from ft2 where c2 < 10 group by c2 order by 1;
explain (verbose, costs off)
select c2, array_agg(c2) over (partition by c2%2 order by c2 desc) from ft1 where c2 < 10 group by c2 order by 1;
select c2, array_agg(c2) over (partition by c2%2 order by c2 desc) from ft1 where c2 < 10 group by c2 order by 1;
explain (verbose, costs off)
select c2, array_agg(c2) over (partition by c2%2 order by c2 range between current row and unbounded following) from ft1 where c2 < 10 group by c2 order by 1;
select c2, array_agg(c2) over (partition by c2%2 order by c2 range between current row and unbounded following) from ft1 where c2 < 10 group by c2 order by 1;


-- ===================================================================
-- parameterized queries
--
-- 参数化查询
-- ===================================================================
-- simple join
--
-- 简单连接
PREPARE st1(int, int) AS SELECT t1.c3, t2.c3 FROM ft1 t1, ft2 t2 WHERE t1.c1 = $1 AND t2.c1 = $2;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st1(1, 2);
EXECUTE st1(1, 1);
EXECUTE st1(101, 101);
SET enable_hashjoin TO off;
SET enable_sort TO off;
-- subquery using stable function (can't be sent to remote)
--
-- 使用稳定函数的子查询（无法发送到远程）
PREPARE st2(int) AS SELECT * FROM ft1 t1 WHERE t1.c1 < $2 AND t1.c3 IN (SELECT c3 FROM ft2 t2 WHERE c1 > $1 AND date(c4) = '1970-01-17'::date) ORDER BY c1;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st2(10, 20);
EXECUTE st2(10, 20);
EXECUTE st2(101, 121);
RESET enable_hashjoin;
RESET enable_sort;
-- subquery using immutable function (can be sent to remote)
--
-- 使用不可变函数的子查询（可以发送到远程）
PREPARE st3(int) AS SELECT * FROM ft1 t1 WHERE t1.c1 < $2 AND t1.c3 IN (SELECT c3 FROM ft2 t2 WHERE c1 > $1 AND date(c5) = '1970-01-17'::date) ORDER BY c1;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st3(10, 20);
EXECUTE st3(10, 20);
EXECUTE st3(20, 30);
-- custom plan should be chosen initially
--
-- 应首先选择定制计划
PREPARE st4(int) AS SELECT * FROM ft1 t1 WHERE t1.c1 = $1;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st4(1);
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st4(1);
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st4(1);
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st4(1);
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st4(1);
-- once we try it enough times, should switch to generic plan
--
-- 一旦我们尝试了足够多次，就应该切换到通用计划
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st4(1);
-- value of $1 should not be sent to remote
--
-- $1 的值不应发送到远程
PREPARE st5(user_enum,int) AS SELECT * FROM ft1 t1 WHERE c8 = $1 and c1 = $2;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st5('foo', 1);
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st5('foo', 1);
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st5('foo', 1);
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st5('foo', 1);
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st5('foo', 1);
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st5('foo', 1);
EXECUTE st5('foo', 1);

-- altering FDW options requires replanning
--
-- 改变外籍家政工人的选择需要重新规划
PREPARE st6 AS SELECT * FROM ft1 t1 WHERE t1.c1 = t1.c2;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st6;
PREPARE st7 AS INSERT INTO ft1 (c1,c2,c3) VALUES (1001,101,'foo');
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st7;
ALTER TABLE "S 1"."T 1" RENAME TO "T 0";
ALTER FOREIGN TABLE ft1 OPTIONS (SET table_name 'T 0');
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st6;
EXECUTE st6;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st7;
ALTER TABLE "S 1"."T 0" RENAME TO "T 1";
ALTER FOREIGN TABLE ft1 OPTIONS (SET table_name 'T 1');

PREPARE st8 AS SELECT count(c3) FROM ft1 t1 WHERE t1.c1 === t1.c2;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st8;
ALTER SERVER loopback OPTIONS (DROP extensions);
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st8;
EXECUTE st8;
ALTER SERVER loopback OPTIONS (ADD extensions 'postgres_fdw');

-- cleanup
DEALLOCATE st1;
DEALLOCATE st2;
DEALLOCATE st3;
DEALLOCATE st4;
DEALLOCATE st5;
DEALLOCATE st6;
DEALLOCATE st7;
DEALLOCATE st8;

-- System columns, except ctid and oid, should not be sent to remote
--
-- 系统列（ctid 和 oid 除外）不应发送到远程
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM ft1 t1 WHERE t1.tableoid = 'pg_class'::regclass LIMIT 1;
SELECT * FROM ft1 t1 WHERE t1.tableoid = 'ft1'::regclass LIMIT 1;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT tableoid::regclass, * FROM ft1 t1 LIMIT 1;
SELECT tableoid::regclass, * FROM ft1 t1 LIMIT 1;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM ft1 t1 WHERE t1.ctid = '(0,2)';
SELECT * FROM ft1 t1 WHERE t1.ctid = '(0,2)';
EXPLAIN (VERBOSE, COSTS OFF)
SELECT ctid, * FROM ft1 t1 LIMIT 1;
SELECT ctid, * FROM ft1 t1 LIMIT 1;

-- ===================================================================
-- used in PL/pgSQL function
--
-- 在 PL/pgSQL 函数中使用
-- ===================================================================
CREATE OR REPLACE FUNCTION f_test(p_c1 int) RETURNS int AS $$
DECLARE
	v_c1 int;
BEGIN
    SELECT c1 INTO v_c1 FROM ft1 WHERE c1 = p_c1 LIMIT 1;
    PERFORM c1 FROM ft1 WHERE c1 = p_c1 AND p_c1 = v_c1 LIMIT 1;
    RETURN v_c1;
END;
$$ LANGUAGE plpgsql;
SELECT f_test(100);
DROP FUNCTION f_test(int);

-- ===================================================================
-- REINDEX
-- ===================================================================
-- remote table is not created here
--
-- 这里没有创建远程表
CREATE FOREIGN TABLE reindex_foreign (c1 int, c2 int)
  SERVER loopback2 OPTIONS (table_name 'reindex_local');
REINDEX TABLE reindex_foreign; -- error
REINDEX TABLE CONCURRENTLY reindex_foreign; -- error
DROP FOREIGN TABLE reindex_foreign;
-- partitions and foreign tables
--
-- 分区和外部表
CREATE TABLE reind_fdw_parent (c1 int) PARTITION BY RANGE (c1);
CREATE TABLE reind_fdw_0_10 PARTITION OF reind_fdw_parent
  FOR VALUES FROM (0) TO (10);
CREATE FOREIGN TABLE reind_fdw_10_20 PARTITION OF reind_fdw_parent
  FOR VALUES FROM (10) TO (20)
  SERVER loopback OPTIONS (table_name 'reind_local_10_20');
REINDEX TABLE reind_fdw_parent; -- ok
REINDEX TABLE CONCURRENTLY reind_fdw_parent; -- ok
DROP TABLE reind_fdw_parent;

-- ===================================================================
-- conversion error
--
-- 转换错误
-- ===================================================================
ALTER FOREIGN TABLE ft1 ALTER COLUMN c8 TYPE int;
SELECT * FROM ft1 ftx(x1,x2,x3,x4,x5,x6,x7,x8) WHERE x1 = 1;  -- ERROR
SELECT ftx.x1, ft2.c2, ftx.x8 FROM ft1 ftx(x1,x2,x3,x4,x5,x6,x7,x8), ft2
  WHERE ftx.x1 = ft2.c1 AND ftx.x1 = 1; -- ERROR
SELECT ftx.x1, ft2.c2, ftx FROM ft1 ftx(x1,x2,x3,x4,x5,x6,x7,x8), ft2
  WHERE ftx.x1 = ft2.c1 AND ftx.x1 = 1; -- ERROR
SELECT sum(c2), array_agg(c8) FROM ft1 GROUP BY c8; -- ERROR
ANALYZE ft1; -- ERROR
ALTER FOREIGN TABLE ft1 ALTER COLUMN c8 TYPE user_enum;

-- ===================================================================
-- local type can be different from remote type in some cases,
--
-- 在某些情况下，本地类型可能与远程类型不同，
-- in particular if similarly-named operators do equivalent things
--
-- 特别是如果名称相似的运算符做相同的事情
-- ===================================================================
ALTER FOREIGN TABLE ft1 ALTER COLUMN c8 TYPE text;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM ft1 WHERE c8 = 'foo' LIMIT 1;
SELECT * FROM ft1 WHERE c8 = 'foo' LIMIT 1;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM ft1 WHERE 'foo' = c8 LIMIT 1;
SELECT * FROM ft1 WHERE 'foo' = c8 LIMIT 1;
-- we declared c8 to be text locally, but it's still the same type on
--
-- 我们在本地声明了 c8 为文本，但它仍然是相同的类型
-- the remote which will balk if we try to do anything incompatible
--
-- 如果我们尝试做任何不兼容的事情，遥控器就会犹豫不决
-- with that remote type
--
-- 与该远程类型
SELECT * FROM ft1 WHERE c8 LIKE 'foo' LIMIT 1; -- ERROR
SELECT * FROM ft1 WHERE c8::text LIKE 'foo' LIMIT 1; -- ERROR; cast not pushed down
ALTER FOREIGN TABLE ft1 ALTER COLUMN c8 TYPE user_enum;

-- ===================================================================
-- subtransaction
--  + local/remote error doesn't break cursor
--
-- + 本地/远程错误不会破坏光标
-- ===================================================================
BEGIN;
DECLARE c CURSOR FOR SELECT * FROM ft1 ORDER BY c1;
FETCH c;
SAVEPOINT s;
ERROR OUT;          -- ERROR
ROLLBACK TO s;
FETCH c;
SAVEPOINT s;
SELECT * FROM ft1 WHERE 1 / (c1 - 1) > 0;  -- ERROR
ROLLBACK TO s;
FETCH c;
SELECT * FROM ft1 ORDER BY c1 LIMIT 1;
COMMIT;

-- ===================================================================
-- test handling of collations
--
-- 测试排序规则的处理
-- ===================================================================
create table loct3 (f1 text collate "C" unique, f2 text, f3 varchar(10) unique);
create foreign table ft3 (f1 text collate "C", f2 text, f3 varchar(10))
  server loopback options (table_name 'loct3', use_remote_estimate 'true');

-- can be sent to remote
--
-- 可以发送到远程
explain (verbose, costs off) select * from ft3 where f1 = 'foo';
explain (verbose, costs off) select * from ft3 where f1 COLLATE "C" = 'foo';
explain (verbose, costs off) select * from ft3 where f2 = 'foo';
explain (verbose, costs off) select * from ft3 where f3 = 'foo';
explain (verbose, costs off) select * from ft3 f, loct3 l
  where f.f3 = l.f3 and l.f1 = 'foo';
-- can't be sent to remote
--
-- 无法发送到远程
explain (verbose, costs off) select * from ft3 where f1 COLLATE "POSIX" = 'foo';
explain (verbose, costs off) select * from ft3 where f1 = 'foo' COLLATE "C";
explain (verbose, costs off) select * from ft3 where f2 COLLATE "C" = 'foo';
explain (verbose, costs off) select * from ft3 where f2 = 'foo' COLLATE "C";
explain (verbose, costs off) select * from ft3 f, loct3 l
  where f.f3 = l.f3 COLLATE "POSIX" and l.f1 = 'foo';

-- ===================================================================
-- test SEMI-JOIN pushdown
--
-- 测试 SEMI-JOIN 下推
-- ===================================================================
EXPLAIN (verbose, costs off)
SELECT ft2.*, ft4.* FROM ft2 INNER JOIN ft4 ON ft2.c2 = ft4.c1
  WHERE ft2.c1 > 900
  AND EXISTS (SELECT 1 FROM ft5 WHERE ft4.c1 = ft5.c1)
  ORDER BY ft2.c1;
SELECT ft2.*, ft4.* FROM ft2 INNER JOIN ft4 ON ft2.c2 = ft4.c1
  WHERE ft2.c1 > 900
  AND EXISTS (SELECT 1 FROM ft5 WHERE ft4.c1 = ft5.c1)
  ORDER BY ft2.c1;

-- The same query, different join order
--
-- 相同的查询，不同的连接顺序
EXPLAIN (verbose, costs off)
SELECT ft2.*, ft4.* FROM ft2 INNER JOIN
  (SELECT * FROM ft4 WHERE
  EXISTS (SELECT 1 FROM ft5 WHERE ft4.c1 = ft5.c1)) ft4
  ON ft2.c2 = ft4.c1
  WHERE ft2.c1 > 900
  ORDER BY ft2.c1;
SELECT ft2.*, ft4.* FROM ft2 INNER JOIN
  (SELECT * FROM ft4 WHERE
  EXISTS (SELECT 1 FROM ft5 WHERE ft4.c1 = ft5.c1)) ft4
  ON ft2.c2 = ft4.c1
  WHERE ft2.c1 > 900
  ORDER BY ft2.c1;

-- Left join
--
-- 左连接
EXPLAIN (verbose, costs off)
SELECT ft2.*, ft4.* FROM ft2 LEFT JOIN
  (SELECT * FROM ft4 WHERE
  EXISTS (SELECT 1 FROM ft5 WHERE ft4.c1 = ft5.c1)) ft4
  ON ft2.c2 = ft4.c1
  WHERE ft2.c1 > 900
  ORDER BY ft2.c1 LIMIT 10;
SELECT ft2.*, ft4.* FROM ft2 LEFT JOIN
  (SELECT * FROM ft4 WHERE
  EXISTS (SELECT 1 FROM ft5 WHERE ft4.c1 = ft5.c1)) ft4
  ON ft2.c2 = ft4.c1
  WHERE ft2.c1 > 900
  ORDER BY ft2.c1 LIMIT 10;

-- Several semi-joins per upper level join
--
-- 每个上层连接有几个半连接
EXPLAIN (verbose, costs off)
SELECT ft2.*, ft4.* FROM ft2 INNER JOIN
  (SELECT * FROM ft4 WHERE
  EXISTS (SELECT 1 FROM ft5 WHERE ft4.c1 = ft5.c1)) ft4
  ON ft2.c2 = ft4.c1
  INNER JOIN (SELECT * FROM ft5 WHERE
  EXISTS (SELECT 1 FROM ft4 WHERE ft4.c1 = ft5.c1)) ft5
  ON ft2.c2 <= ft5.c1
  WHERE ft2.c1 > 900
  ORDER BY ft2.c1 LIMIT 10;
SELECT ft2.*, ft4.* FROM ft2 INNER JOIN
  (SELECT * FROM ft4 WHERE
  EXISTS (SELECT 1 FROM ft5 WHERE ft4.c1 = ft5.c1)) ft4
  ON ft2.c2 = ft4.c1
  INNER JOIN (SELECT * FROM ft5 WHERE
  EXISTS (SELECT 1 FROM ft4 WHERE ft4.c1 = ft5.c1)) ft5
  ON ft2.c2 <= ft5.c1
  WHERE ft2.c1 > 900
  ORDER BY ft2.c1 LIMIT 10;

-- Semi-join below Semi-join
--
-- 半连接下面的半连接
EXPLAIN (verbose, costs off)
SELECT ft2.* FROM ft2 WHERE
  c1 = ANY (
	SELECT c1 FROM ft2 WHERE
	  EXISTS (SELECT 1 FROM ft4 WHERE ft4.c2 = ft2.c2))
  AND ft2.c1 > 900
  ORDER BY ft2.c1 LIMIT 10;
SELECT ft2.* FROM ft2 WHERE
  c1 = ANY (
	SELECT c1 FROM ft2 WHERE
	  EXISTS (SELECT 1 FROM ft4 WHERE ft4.c2 = ft2.c2))
  AND ft2.c1 > 900
  ORDER BY ft2.c1 LIMIT 10;

-- Upper level relations shouldn't refer EXISTS() subqueries
--
-- 上层关系不应引用 EXISTS() 子查询
EXPLAIN (verbose, costs off)
SELECT * FROM ft2 ftupper WHERE
   EXISTS (
	SELECT c1 FROM ft2 WHERE
	  EXISTS (SELECT 1 FROM ft4 WHERE ft4.c2 = ft2.c2) AND c1 = ftupper.c1 )
  AND ftupper.c1 > 900
  ORDER BY ftupper.c1 LIMIT 10;
SELECT * FROM ft2 ftupper WHERE
   EXISTS (
	SELECT c1 FROM ft2 WHERE
	  EXISTS (SELECT 1 FROM ft4 WHERE ft4.c2 = ft2.c2) AND c1 = ftupper.c1 )
  AND ftupper.c1 > 900
  ORDER BY ftupper.c1 LIMIT 10;

-- EXISTS should be propagated to the highest upper inner join
--
-- EXISTS 应传播到最高的上内连接
EXPLAIN (verbose, costs off)
	SELECT ft2.*, ft4.* FROM ft2 INNER JOIN
	(SELECT * FROM ft4 WHERE EXISTS (
		SELECT 1 FROM ft2 WHERE ft2.c2 = ft4.c2)) ft4
	ON ft2.c2 = ft4.c1
	INNER JOIN
	(SELECT * FROM ft2 WHERE EXISTS (
		SELECT 1 FROM ft4 WHERE ft2.c2 = ft4.c2)) ft21
	ON ft2.c2 = ft21.c2
	WHERE ft2.c1 > 900
	ORDER BY ft2.c1 LIMIT 10;
SELECT ft2.*, ft4.* FROM ft2 INNER JOIN
	(SELECT * FROM ft4 WHERE EXISTS (
		SELECT 1 FROM ft2 WHERE ft2.c2 = ft4.c2)) ft4
	ON ft2.c2 = ft4.c1
	INNER JOIN
	(SELECT * FROM ft2 WHERE EXISTS (
		SELECT 1 FROM ft4 WHERE ft2.c2 = ft4.c2)) ft21
	ON ft2.c2 = ft21.c2
	WHERE ft2.c1 > 900
	ORDER BY ft2.c1 LIMIT 10;

-- Semi-join conditions shouldn't pop up as left/right join clauses.
--
-- 半连接条件不应作为左/右连接子句弹出。
SET enable_material TO off;
EXPLAIN (verbose, costs off)
SELECT x1.c1 FROM
		(SELECT * FROM ft2 WHERE EXISTS (SELECT 1 FROM ft4 WHERE ft4.c1 = ft2.c1 AND ft2.c2 < 10)) x1
	RIGHT JOIN
		(SELECT * FROM ft2 WHERE EXISTS (SELECT 1 FROM ft4 WHERE ft4.c1 = ft2.c1 AND ft2.c2 < 10)) x2
	ON (x1.c1 = x2.c1)
ORDER BY x1.c1 LIMIT 10;
SELECT x1.c1 FROM
		(SELECT * FROM ft2 WHERE EXISTS (SELECT 1 FROM ft4 WHERE ft4.c1 = ft2.c1 AND ft2.c2 < 10)) x1
	RIGHT JOIN
		(SELECT * FROM ft2 WHERE EXISTS (SELECT 1 FROM ft4 WHERE ft4.c1 = ft2.c1 AND ft2.c2 < 10)) x2
	ON (x1.c1 = x2.c1)
ORDER BY x1.c1 LIMIT 10;
RESET enable_material;

-- Can't push down semi-join with inner rel vars in targetlist
--
-- 无法下推与目标列表中的内部相关变量的半连接
EXPLAIN (verbose, costs off)
SELECT ft1.c1 FROM ft1 JOIN ft2 on ft1.c1 = ft2.c1 WHERE
	ft1.c1 IN (
		SELECT ft2.c1 FROM ft2 JOIN ft4 ON ft2.c1 = ft4.c1)
	ORDER BY ft1.c1 LIMIT 5;

-- ===================================================================
-- test writable foreign table stuff
--
-- 测试可写的外部表内容
-- ===================================================================
EXPLAIN (verbose, costs off)
INSERT INTO ft2 (c1,c2,c3) SELECT c1+1000,c2+100, c3 || c3 FROM ft2 LIMIT 20;
INSERT INTO ft2 (c1,c2,c3) SELECT c1+1000,c2+100, c3 || c3 FROM ft2 LIMIT 20;
INSERT INTO ft2 (c1,c2,c3)
  VALUES (1101,201,'aaa'), (1102,202,'bbb'), (1103,203,'ccc') RETURNING old, new, old.*, new.*;
INSERT INTO ft2 (c1,c2,c3) VALUES (1104,204,'ddd'), (1105,205,'eee');
EXPLAIN (verbose, costs off)
UPDATE ft2 SET c2 = c2 + 300, c3 = c3 || '_update3' WHERE c1 % 10 = 3;              -- can be pushed down
UPDATE ft2 SET c2 = c2 + 300, c3 = c3 || '_update3' WHERE c1 % 10 = 3;
EXPLAIN (verbose, costs off)
UPDATE ft2 SET c2 = c2 + 400, c3 = c3 || '_update7' WHERE c1 % 10 = 7 RETURNING *;  -- can be pushed down
UPDATE ft2 SET c2 = c2 + 400, c3 = c3 || '_update7' WHERE c1 % 10 = 7 RETURNING *;
BEGIN;
  EXPLAIN (verbose, costs off)
  UPDATE ft2 SET c2 = c2 + 400, c3 = c3 || '_update7b' WHERE c1 % 10 = 7 AND c1 < 40
    RETURNING old.*, new.*;                                                         -- can't be pushed down
  UPDATE ft2 SET c2 = c2 + 400, c3 = c3 || '_update7b' WHERE c1 % 10 = 7 AND c1 < 40
    RETURNING old.*, new.*;
ROLLBACK;
EXPLAIN (verbose, costs off)
UPDATE ft2 SET c2 = ft2.c2 + 500, c3 = ft2.c3 || '_update9', c7 = DEFAULT
  FROM ft1 WHERE ft1.c1 = ft2.c2 AND ft1.c1 % 10 = 9;                               -- can be pushed down
UPDATE ft2 SET c2 = ft2.c2 + 500, c3 = ft2.c3 || '_update9', c7 = DEFAULT
  FROM ft1 WHERE ft1.c1 = ft2.c2 AND ft1.c1 % 10 = 9;
EXPLAIN (verbose, costs off)
  DELETE FROM ft2 WHERE c1 % 10 = 5 RETURNING c1, c4;                               -- can be pushed down
DELETE FROM ft2 WHERE c1 % 10 = 5 RETURNING c1, c4;
BEGIN;
  EXPLAIN (verbose, costs off)
  DELETE FROM ft2 WHERE c1 % 10 = 6 AND c1 < 40 RETURNING old.c1, c4;               -- can't be pushed down
  DELETE FROM ft2 WHERE c1 % 10 = 6 AND c1 < 40 RETURNING old.c1, c4;
ROLLBACK;
EXPLAIN (verbose, costs off)
DELETE FROM ft2 USING ft1 WHERE ft1.c1 = ft2.c2 AND ft1.c1 % 10 = 2;                -- can be pushed down
DELETE FROM ft2 USING ft1 WHERE ft1.c1 = ft2.c2 AND ft1.c1 % 10 = 2;
SELECT c1,c2,c3,c4 FROM ft2 ORDER BY c1;
EXPLAIN (verbose, costs off)
INSERT INTO ft2 (c1,c2,c3) VALUES (1200,999,'foo') RETURNING tableoid::regclass;
INSERT INTO ft2 (c1,c2,c3) VALUES (1200,999,'foo') RETURNING tableoid::regclass;
EXPLAIN (verbose, costs off)
UPDATE ft2 SET c3 = 'bar' WHERE c1 = 1200 RETURNING tableoid::regclass;             -- can be pushed down
UPDATE ft2 SET c3 = 'bar' WHERE c1 = 1200 RETURNING tableoid::regclass;
EXPLAIN (verbose, costs off)
DELETE FROM ft2 WHERE c1 = 1200 RETURNING tableoid::regclass;                       -- can be pushed down
DELETE FROM ft2 WHERE c1 = 1200 RETURNING tableoid::regclass;

-- Test UPDATE/DELETE with RETURNING on a three-table join
--
-- 在三表连接上使用 RETURNING 测试 UPDATE/DELETE
INSERT INTO ft2 (c1,c2,c3)
  SELECT id, id - 1200, to_char(id, 'FM00000') FROM generate_series(1201, 1300) id;
EXPLAIN (verbose, costs off)
UPDATE ft2 SET c3 = 'foo'
  FROM ft4 INNER JOIN ft5 ON (ft4.c1 = ft5.c1)
  WHERE ft2.c1 > 1200 AND ft2.c2 = ft4.c1
  RETURNING ft2, ft2.*, ft4, ft4.*;       -- can be pushed down
UPDATE ft2 SET c3 = 'foo'
  FROM ft4 INNER JOIN ft5 ON (ft4.c1 = ft5.c1)
  WHERE ft2.c1 > 1200 AND ft2.c2 = ft4.c1
  RETURNING ft2, ft2.*, ft4, ft4.*;
BEGIN;
  EXPLAIN (verbose, costs off)
  UPDATE ft2 SET c3 = 'bar'
    FROM ft4 INNER JOIN ft5 ON (ft4.c1 = ft5.c1)
    WHERE ft2.c1 > 1200 AND ft2.c2 = ft4.c1
    RETURNING old, new, ft2, ft2.*, ft4, ft4.*;  -- can't be pushed down
  UPDATE ft2 SET c3 = 'bar'
    FROM ft4 INNER JOIN ft5 ON (ft4.c1 = ft5.c1)
    WHERE ft2.c1 > 1200 AND ft2.c2 = ft4.c1
    RETURNING old, new, ft2, ft2.*, ft4, ft4.*;
ROLLBACK;
EXPLAIN (verbose, costs off)
DELETE FROM ft2
  USING ft4 LEFT JOIN ft5 ON (ft4.c1 = ft5.c1)
  WHERE ft2.c1 > 1200 AND ft2.c1 % 10 = 0 AND ft2.c2 = ft4.c1
  RETURNING 100;                          -- can be pushed down
DELETE FROM ft2
  USING ft4 LEFT JOIN ft5 ON (ft4.c1 = ft5.c1)
  WHERE ft2.c1 > 1200 AND ft2.c1 % 10 = 0 AND ft2.c2 = ft4.c1
  RETURNING 100;
DELETE FROM ft2 WHERE ft2.c1 > 1200;

-- Test UPDATE with a MULTIEXPR sub-select
--
-- 使用 MULTIEXPR 子选择测试 UPDATE
-- (maybe someday this'll be remotely executable, but not today)
--
-- （也许有一天这将可以远程执行，但不是今天）
EXPLAIN (verbose, costs off)
UPDATE ft2 AS target SET (c2, c7) = (
    SELECT c2 * 10, c7
        FROM ft2 AS src
        WHERE target.c1 = src.c1
) WHERE c1 > 1100;
UPDATE ft2 AS target SET (c2, c7) = (
    SELECT c2 * 10, c7
        FROM ft2 AS src
        WHERE target.c1 = src.c1
) WHERE c1 > 1100;

UPDATE ft2 AS target SET (c2) = (
    SELECT c2 / 10
        FROM ft2 AS src
        WHERE target.c1 = src.c1
) WHERE c1 > 1100;

-- Test UPDATE involving a join that can be pushed down,
--
-- 测试涉及可下推连接的 UPDATE，
-- but a SET clause that can't be
--
-- 但 SET 子句不能
EXPLAIN (VERBOSE, COSTS OFF)
UPDATE ft2 d SET c2 = CASE WHEN random() >= 0 THEN d.c2 ELSE 0 END
  FROM ft2 AS t WHERE d.c1 = t.c1 AND d.c1 > 1000;
UPDATE ft2 d SET c2 = CASE WHEN random() >= 0 THEN d.c2 ELSE 0 END
  FROM ft2 AS t WHERE d.c1 = t.c1 AND d.c1 > 1000;

-- Test UPDATE/DELETE with WHERE or JOIN/ON conditions containing
--
-- 使用包含以下内容的 WHERE 或 JOIN/ON 条件测试 UPDATE/DELETE
-- user-defined operators/functions
--
-- 用户定义的运算符/函数
ALTER SERVER loopback OPTIONS (DROP extensions);
INSERT INTO ft2 (c1,c2,c3)
  SELECT id, id % 10, to_char(id, 'FM00000') FROM generate_series(2001, 2010) id;

-- this will do a remote seqscan, causing unstable result order, so sort
--
-- 这将进行远程 seqscan，导致结果顺序不稳定，因此排序
EXPLAIN (verbose, costs off)
WITH cte AS (
  UPDATE ft2 SET c3 = 'bar' WHERE postgres_fdw_abs(c1) > 2000 RETURNING *
) SELECT * FROM cte ORDER BY c1;          -- can't be pushed down
WITH cte AS (
  UPDATE ft2 SET c3 = 'bar' WHERE postgres_fdw_abs(c1) > 2000 RETURNING *
) SELECT * FROM cte ORDER BY c1;

EXPLAIN (verbose, costs off)
UPDATE ft2 SET c3 = 'baz'
  FROM ft4 INNER JOIN ft5 ON (ft4.c1 = ft5.c1)
  WHERE ft2.c1 > 2000 AND ft2.c2 === ft4.c1
  RETURNING ft2.*, ft4.*, ft5.*;                                                    -- can't be pushed down
UPDATE ft2 SET c3 = 'baz'
  FROM ft4 INNER JOIN ft5 ON (ft4.c1 = ft5.c1)
  WHERE ft2.c1 > 2000 AND ft2.c2 === ft4.c1
  RETURNING ft2.*, ft4.*, ft5.*;
EXPLAIN (verbose, costs off)
DELETE FROM ft2
  USING ft4 INNER JOIN ft5 ON (ft4.c1 === ft5.c1)
  WHERE ft2.c1 > 2000 AND ft2.c2 = ft4.c1
  RETURNING ft2.c1, ft2.c2, ft2.c3;       -- can't be pushed down
DELETE FROM ft2
  USING ft4 INNER JOIN ft5 ON (ft4.c1 === ft5.c1)
  WHERE ft2.c1 > 2000 AND ft2.c2 = ft4.c1
  RETURNING ft2.c1, ft2.c2, ft2.c3;
DELETE FROM ft2 WHERE ft2.c1 > 2000;
ALTER SERVER loopback OPTIONS (ADD extensions 'postgres_fdw');

-- Test that trigger on remote table works as expected
--
-- 测试远程表上的触发器是否按预期工作
CREATE OR REPLACE FUNCTION "S 1".F_BRTRIG() RETURNS trigger AS $$
BEGIN
    NEW.c3 = NEW.c3 || '_trig_update';
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;
CREATE TRIGGER t1_br_insert BEFORE INSERT OR UPDATE
    ON "S 1"."T 1" FOR EACH ROW EXECUTE PROCEDURE "S 1".F_BRTRIG();

INSERT INTO ft2 (c1,c2,c3) VALUES (1208, 818, 'fff') RETURNING *;
INSERT INTO ft2 (c1,c2,c3,c6) VALUES (1218, 818, 'ggg', '(--;') RETURNING *;
UPDATE ft2 SET c2 = c2 + 600 WHERE c1 % 10 = 8 AND c1 < 1200 RETURNING *;

-- Test errors thrown on remote side during update
--
-- 更新期间远程端抛出的测试错误
ALTER TABLE "S 1"."T 1" ADD CONSTRAINT c2positive CHECK (c2 >= 0);

INSERT INTO ft1(c1, c2) VALUES(11, 12);  -- duplicate key
INSERT INTO ft1(c1, c2) VALUES(11, 12) ON CONFLICT DO NOTHING; -- works
INSERT INTO ft1(c1, c2) VALUES(11, 12) ON CONFLICT (c1, c2) DO NOTHING; -- unsupported
INSERT INTO ft1(c1, c2) VALUES(11, 12) ON CONFLICT (c1, c2) DO UPDATE SET c3 = 'ffg'; -- unsupported
INSERT INTO ft1(c1, c2) VALUES(1111, -2);  -- c2positive
UPDATE ft1 SET c2 = -c2 WHERE c1 = 1;  -- c2positive

-- Test savepoint/rollback behavior
--
-- 测试保存点/回滚行为
select c2, count(*) from ft2 where c2 < 500 group by 1 order by 1;
select c2, count(*) from "S 1"."T 1" where c2 < 500 group by 1 order by 1;
begin;
update ft2 set c2 = 42 where c2 = 0;
select c2, count(*) from ft2 where c2 < 500 group by 1 order by 1;
savepoint s1;
update ft2 set c2 = 44 where c2 = 4;
select c2, count(*) from ft2 where c2 < 500 group by 1 order by 1;
release savepoint s1;
select c2, count(*) from ft2 where c2 < 500 group by 1 order by 1;
savepoint s2;
update ft2 set c2 = 46 where c2 = 6;
select c2, count(*) from ft2 where c2 < 500 group by 1 order by 1;
rollback to savepoint s2;
select c2, count(*) from ft2 where c2 < 500 group by 1 order by 1;
release savepoint s2;
select c2, count(*) from ft2 where c2 < 500 group by 1 order by 1;
savepoint s3;
update ft2 set c2 = -2 where c2 = 42 and c1 = 10; -- fail on remote side
rollback to savepoint s3;
select c2, count(*) from ft2 where c2 < 500 group by 1 order by 1;
release savepoint s3;
select c2, count(*) from ft2 where c2 < 500 group by 1 order by 1;
-- none of the above is committed yet remotely
--
-- 以上均未远程提交
select c2, count(*) from "S 1"."T 1" where c2 < 500 group by 1 order by 1;
commit;
select c2, count(*) from ft2 where c2 < 500 group by 1 order by 1;
select c2, count(*) from "S 1"."T 1" where c2 < 500 group by 1 order by 1;

VACUUM ANALYZE "S 1"."T 1";

-- Above DMLs add data with c6 as NULL in ft1, so test ORDER BY NULLS LAST and NULLs
--
-- 上面的 DML 在 ft1 中添加 c6 为 NULL 的数据，因此测试 ORDER BY NULLS LAST 和 NULL
-- FIRST behavior here.
--
-- 这里的第一个行为。
-- ORDER BY DESC NULLS LAST options
--
-- ORDER BY DESC NULLS 最后选项
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM ft1 ORDER BY c6 DESC NULLS LAST, c1 OFFSET 795 LIMIT 10;
SELECT * FROM ft1 ORDER BY c6 DESC NULLS LAST, c1 OFFSET 795  LIMIT 10;
-- ORDER BY DESC NULLS FIRST options
--
-- ORDER BY DESC NULLS FIRST 选项
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM ft1 ORDER BY c6 DESC NULLS FIRST, c1 OFFSET 15 LIMIT 10;
SELECT * FROM ft1 ORDER BY c6 DESC NULLS FIRST, c1 OFFSET 15 LIMIT 10;
-- ORDER BY ASC NULLS FIRST options
--
-- ORDER BY ASC NULLS FIRST 选项
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM ft1 ORDER BY c6 ASC NULLS FIRST, c1 OFFSET 15 LIMIT 10;
SELECT * FROM ft1 ORDER BY c6 ASC NULLS FIRST, c1 OFFSET 15 LIMIT 10;

-- Test ReScan code path that recreates the cursor even when no parameters
--
-- 测试即使没有参数也重新创建光标的 ReScan 代码路径
-- change (bug #17889)
--
-- 更改（错误#17889）
CREATE TABLE loct1 (c1 int);
CREATE TABLE loct2 (c1 int, c2 text);
INSERT INTO loct1 VALUES (1001);
INSERT INTO loct1 VALUES (1002);
INSERT INTO loct2 SELECT id, to_char(id, 'FM0000') FROM generate_series(1, 1000) id;
INSERT INTO loct2 VALUES (1001, 'foo');
INSERT INTO loct2 VALUES (1002, 'bar');
CREATE FOREIGN TABLE remt2 (c1 int, c2 text) SERVER loopback OPTIONS (table_name 'loct2');
ANALYZE loct1;
ANALYZE remt2;
SET enable_mergejoin TO false;
SET enable_hashjoin TO false;
SET enable_material TO false;
EXPLAIN (VERBOSE, COSTS OFF)
UPDATE remt2 SET c2 = remt2.c2 || remt2.c2 FROM loct1 WHERE loct1.c1 = remt2.c1 RETURNING remt2.*;
UPDATE remt2 SET c2 = remt2.c2 || remt2.c2 FROM loct1 WHERE loct1.c1 = remt2.c1 RETURNING remt2.*;
RESET enable_mergejoin;
RESET enable_hashjoin;
RESET enable_material;
DROP FOREIGN TABLE remt2;
DROP TABLE loct1;
DROP TABLE loct2;

-- ===================================================================
-- test check constraints
--
-- 测试检查约束
-- ===================================================================

-- Consistent check constraints provide consistent results
--
-- 一致的检查约束提供一致的结果
ALTER FOREIGN TABLE ft1 ADD CONSTRAINT ft1_c2positive CHECK (c2 >= 0);
EXPLAIN (VERBOSE, COSTS OFF) SELECT count(*) FROM ft1 WHERE c2 < 0;
SELECT count(*) FROM ft1 WHERE c2 < 0;
SET constraint_exclusion = 'on';
EXPLAIN (VERBOSE, COSTS OFF) SELECT count(*) FROM ft1 WHERE c2 < 0;
SELECT count(*) FROM ft1 WHERE c2 < 0;
RESET constraint_exclusion;
-- check constraint is enforced on the remote side, not locally
--
-- 检查约束在远程端强制执行，而不是在本地强制执行
INSERT INTO ft1(c1, c2) VALUES(1111, -2);  -- c2positive
UPDATE ft1 SET c2 = -c2 WHERE c1 = 1;  -- c2positive
ALTER FOREIGN TABLE ft1 DROP CONSTRAINT ft1_c2positive;

-- But inconsistent check constraints provide inconsistent results
--
-- 但不一致的检查约束会提供不一致的结果
ALTER FOREIGN TABLE ft1 ADD CONSTRAINT ft1_c2negative CHECK (c2 < 0);
EXPLAIN (VERBOSE, COSTS OFF) SELECT count(*) FROM ft1 WHERE c2 >= 0;
SELECT count(*) FROM ft1 WHERE c2 >= 0;
SET constraint_exclusion = 'on';
EXPLAIN (VERBOSE, COSTS OFF) SELECT count(*) FROM ft1 WHERE c2 >= 0;
SELECT count(*) FROM ft1 WHERE c2 >= 0;
RESET constraint_exclusion;
-- local check constraint is not actually enforced
--
-- 本地检查约束实际上并未强制执行
INSERT INTO ft1(c1, c2) VALUES(1111, 2);
UPDATE ft1 SET c2 = c2 + 1 WHERE c1 = 1;
ALTER FOREIGN TABLE ft1 DROP CONSTRAINT ft1_c2negative;

-- ===================================================================
-- test WITH CHECK OPTION constraints
--
-- 使用 CHECK OPTION 约束进行测试
-- ===================================================================

CREATE FUNCTION row_before_insupd_trigfunc() RETURNS trigger AS $$BEGIN NEW.a := NEW.a + 10; RETURN NEW; END$$ LANGUAGE plpgsql;

CREATE TABLE base_tbl (a int, b int);
ALTER TABLE base_tbl SET (autovacuum_enabled = 'false');
CREATE TRIGGER row_before_insupd_trigger BEFORE INSERT OR UPDATE ON base_tbl FOR EACH ROW EXECUTE PROCEDURE row_before_insupd_trigfunc();
CREATE FOREIGN TABLE foreign_tbl (a int, b int)
  SERVER loopback OPTIONS (table_name 'base_tbl');
CREATE VIEW rw_view AS SELECT * FROM foreign_tbl
  WHERE a < b WITH CHECK OPTION;
\d+ rw_view

EXPLAIN (VERBOSE, COSTS OFF)
INSERT INTO rw_view VALUES (0, 5);
INSERT INTO rw_view VALUES (0, 5); -- should fail
EXPLAIN (VERBOSE, COSTS OFF)
INSERT INTO rw_view VALUES (0, 15);
INSERT INTO rw_view VALUES (0, 15); -- ok
SELECT * FROM foreign_tbl;

EXPLAIN (VERBOSE, COSTS OFF)
UPDATE rw_view SET b = b + 5;
UPDATE rw_view SET b = b + 5; -- should fail
EXPLAIN (VERBOSE, COSTS OFF)
UPDATE rw_view SET b = b + 15;
UPDATE rw_view SET b = b + 15; -- ok
SELECT * FROM foreign_tbl;

-- We don't allow batch insert when there are any WCO constraints
--
-- 当存在任何 WCO 限制时，我们不允许批量插入
ALTER SERVER loopback OPTIONS (ADD batch_size '10');
EXPLAIN (VERBOSE, COSTS OFF)
INSERT INTO rw_view VALUES (0, 15), (0, 5);
INSERT INTO rw_view VALUES (0, 15), (0, 5); -- should fail
SELECT * FROM foreign_tbl;
ALTER SERVER loopback OPTIONS (DROP batch_size);

DROP FOREIGN TABLE foreign_tbl CASCADE;
DROP TRIGGER row_before_insupd_trigger ON base_tbl;
DROP TABLE base_tbl;

-- test WCO for partitions
--
-- 测试分区的 WCO

CREATE TABLE child_tbl (a int, b int);
ALTER TABLE child_tbl SET (autovacuum_enabled = 'false');
CREATE TRIGGER row_before_insupd_trigger BEFORE INSERT OR UPDATE ON child_tbl FOR EACH ROW EXECUTE PROCEDURE row_before_insupd_trigfunc();
CREATE FOREIGN TABLE foreign_tbl (a int, b int)
  SERVER loopback OPTIONS (table_name 'child_tbl');

CREATE TABLE parent_tbl (a int, b int) PARTITION BY RANGE(a);
ALTER TABLE parent_tbl ATTACH PARTITION foreign_tbl FOR VALUES FROM (0) TO (100);
-- Detach and re-attach once, to stress the concurrent detach case.
--
-- 分离并重新连接一次，以强调并发分离情况。
ALTER TABLE parent_tbl DETACH PARTITION foreign_tbl CONCURRENTLY;
ALTER TABLE parent_tbl ATTACH PARTITION foreign_tbl FOR VALUES FROM (0) TO (100);

CREATE VIEW rw_view AS SELECT * FROM parent_tbl
  WHERE a < b WITH CHECK OPTION;
\d+ rw_view

EXPLAIN (VERBOSE, COSTS OFF)
INSERT INTO rw_view VALUES (0, 5);
INSERT INTO rw_view VALUES (0, 5); -- should fail
EXPLAIN (VERBOSE, COSTS OFF)
INSERT INTO rw_view VALUES (0, 15);
INSERT INTO rw_view VALUES (0, 15); -- ok
SELECT * FROM foreign_tbl;

EXPLAIN (VERBOSE, COSTS OFF)
UPDATE rw_view SET b = b + 5;
UPDATE rw_view SET b = b + 5; -- should fail
EXPLAIN (VERBOSE, COSTS OFF)
UPDATE rw_view SET b = b + 15;
UPDATE rw_view SET b = b + 15; -- ok
SELECT * FROM foreign_tbl;

-- We don't allow batch insert when there are any WCO constraints
--
-- 当存在任何 WCO 限制时，我们不允许批量插入
ALTER SERVER loopback OPTIONS (ADD batch_size '10');
EXPLAIN (VERBOSE, COSTS OFF)
INSERT INTO rw_view VALUES (0, 15), (0, 5);
INSERT INTO rw_view VALUES (0, 15), (0, 5); -- should fail
SELECT * FROM foreign_tbl;
ALTER SERVER loopback OPTIONS (DROP batch_size);

DROP FOREIGN TABLE foreign_tbl CASCADE;
DROP TRIGGER row_before_insupd_trigger ON child_tbl;
DROP TABLE parent_tbl CASCADE;

DROP FUNCTION row_before_insupd_trigfunc;

-- Try a more complex permutation of WCO where there are multiple levels of
--
-- 尝试更复杂的 WCO 排列，其中有多个级别
-- partitioned tables with columns not all in the same order
--
-- 分区表的列顺序不完全相同
CREATE TABLE parent_tbl (a int, b text, c numeric) PARTITION BY RANGE(a);
CREATE TABLE sub_parent (c numeric, a int, b text) PARTITION BY RANGE(a);
ALTER TABLE parent_tbl ATTACH PARTITION sub_parent FOR VALUES FROM (1) TO (10);
CREATE TABLE child_local (b text, c numeric, a int);
CREATE FOREIGN TABLE child_foreign (b text, c numeric, a int)
  SERVER loopback OPTIONS (table_name 'child_local');
ALTER TABLE sub_parent ATTACH PARTITION child_foreign FOR VALUES FROM (1) TO (10);
CREATE VIEW rw_view AS SELECT * FROM parent_tbl WHERE a < 5 WITH CHECK OPTION;

INSERT INTO parent_tbl (a) VALUES(1),(5);
EXPLAIN (VERBOSE, COSTS OFF)
UPDATE rw_view SET b = 'text', c = 123.456;
UPDATE rw_view SET b = 'text', c = 123.456;
SELECT * FROM parent_tbl ORDER BY a;

DROP VIEW rw_view;
DROP TABLE child_local;
DROP FOREIGN TABLE child_foreign;
DROP TABLE sub_parent;
DROP TABLE parent_tbl;

-- ===================================================================
-- test serial columns (ie, sequence-based defaults)
--
-- 测试串行列（即基于序列的默认值）
-- ===================================================================
create table loc1 (f1 serial, f2 text);
alter table loc1 set (autovacuum_enabled = 'false');
create foreign table rem1 (f1 serial, f2 text)
  server loopback options(table_name 'loc1');
select pg_catalog.setval('rem1_f1_seq', 10, false);
insert into loc1(f2) values('hi');
insert into rem1(f2) values('hi remote');
insert into loc1(f2) values('bye');
insert into rem1(f2) values('bye remote');
select * from loc1;
select * from rem1;

-- ===================================================================
-- test generated columns
--
-- 测试生成的列
-- ===================================================================
create table gloc1 (
  a int,
  b int generated always as (a * 2) stored,
  c int
);
alter table gloc1 set (autovacuum_enabled = 'false');
create foreign table grem1 (
  a int,
  b int generated always as (a * 2) stored,
  c int generated always as (a * 3) virtual
) server loopback options(table_name 'gloc1');
explain (verbose, costs off)
insert into grem1 (a) values (1), (2);
insert into grem1 (a) values (1), (2);
explain (verbose, costs off)
update grem1 set a = 22 where a = 2;
update grem1 set a = 22 where a = 2;
select * from gloc1;
select * from grem1;
delete from grem1;

-- test copy from
--
-- 测试副本来自
copy grem1 from stdin;
1
2
\.
select * from gloc1;
select * from grem1;
delete from grem1;

-- test batch insert
--
-- 测试批次插入
alter server loopback options (add batch_size '10');
explain (verbose, costs off)
insert into grem1 (a) values (1), (2);
insert into grem1 (a) values (1), (2);
select * from gloc1;
select * from grem1;
delete from grem1;
-- batch insert with foreign partitions.
--
-- 批量插入外部分区。
-- This schema uses two partitions, one local and one remote with a modulo
--
-- 此架构使用两个分区，一个是本地分区，一个是远程分区，并带有模数
-- to loop across all of them in batches.
--
-- 批量循环遍历所有这些。
create table tab_batch_local (id int, data text);
insert into tab_batch_local select i, 'test'|| i from generate_series(1, 45) i;
create table tab_batch_sharded (id int, data text) partition by hash(id);
create table tab_batch_sharded_p0 partition of tab_batch_sharded
  for values with (modulus 2, remainder 0);
create table tab_batch_sharded_p1_remote (id int, data text);
create foreign table tab_batch_sharded_p1 partition of tab_batch_sharded
  for values with (modulus 2, remainder 1)
  server loopback options (table_name 'tab_batch_sharded_p1_remote');
insert into tab_batch_sharded select * from tab_batch_local;
select count(*) from tab_batch_sharded;
drop table tab_batch_local;
drop table tab_batch_sharded;
drop table tab_batch_sharded_p1_remote;

alter server loopback options (drop batch_size);

-- ===================================================================
-- test local triggers
--
-- 测试本地触发器
-- ===================================================================

-- Trigger functions "borrowed" from triggers regress test.
--
-- 从触发器回归测试“借用”的触发器函数。
CREATE FUNCTION trigger_func() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
	RAISE NOTICE 'trigger_func(%) called: action = %, when = %, level = %',
		TG_ARGV[0], TG_OP, TG_WHEN, TG_LEVEL;
	RETURN NULL;
END;$$;

CREATE TRIGGER trig_stmt_before BEFORE DELETE OR INSERT OR UPDATE OR TRUNCATE ON rem1
	FOR EACH STATEMENT EXECUTE PROCEDURE trigger_func();
CREATE TRIGGER trig_stmt_after AFTER DELETE OR INSERT OR UPDATE OR TRUNCATE ON rem1
	FOR EACH STATEMENT EXECUTE PROCEDURE trigger_func();

CREATE OR REPLACE FUNCTION trigger_data()  RETURNS trigger
LANGUAGE plpgsql AS $$

declare
	oldnew text[];
	relid text;
    argstr text;
begin

	relid := TG_relid::regclass;
	argstr := '';
	for i in 0 .. TG_nargs - 1 loop
		if i > 0 then
			argstr := argstr || ', ';
		end if;
		argstr := argstr || TG_argv[i];
	end loop;

    RAISE NOTICE '%(%) % % % ON %',
		tg_name, argstr, TG_when, TG_level, TG_OP, relid;
    oldnew := '{}'::text[];
	if TG_OP != 'INSERT' then
		oldnew := array_append(oldnew, format('OLD: %s', OLD));
	end if;

	if TG_OP != 'DELETE' then
		oldnew := array_append(oldnew, format('NEW: %s', NEW));
	end if;

    RAISE NOTICE '%', array_to_string(oldnew, ',');

	if TG_OP = 'DELETE' then
		return OLD;
	else
		return NEW;
	end if;
end;
$$;

-- Test basic functionality
--
-- 测试基本功能
CREATE TRIGGER trig_row_before
BEFORE INSERT OR UPDATE OR DELETE ON rem1
FOR EACH ROW EXECUTE PROCEDURE trigger_data(23,'skidoo');

CREATE TRIGGER trig_row_after
AFTER INSERT OR UPDATE OR DELETE ON rem1
FOR EACH ROW EXECUTE PROCEDURE trigger_data(23,'skidoo');

delete from rem1;
insert into rem1 values(1,'insert');
update rem1 set f2  = 'update' where f1 = 1;
update rem1 set f2 = f2 || f2;
truncate rem1;


-- cleanup
DROP TRIGGER trig_row_before ON rem1;
DROP TRIGGER trig_row_after ON rem1;
DROP TRIGGER trig_stmt_before ON rem1;
DROP TRIGGER trig_stmt_after ON rem1;

DELETE from rem1;

-- Test multiple AFTER ROW triggers on a foreign table
--
-- 在外表上测试多个 AFTER ROW 触发器
CREATE TRIGGER trig_row_after1
AFTER INSERT OR UPDATE OR DELETE ON rem1
FOR EACH ROW EXECUTE PROCEDURE trigger_data(23,'skidoo');

CREATE TRIGGER trig_row_after2
AFTER INSERT OR UPDATE OR DELETE ON rem1
FOR EACH ROW EXECUTE PROCEDURE trigger_data(23,'skidoo');

insert into rem1 values(1,'insert');
update rem1 set f2  = 'update' where f1 = 1;
update rem1 set f2 = f2 || f2;
delete from rem1;

-- cleanup
DROP TRIGGER trig_row_after1 ON rem1;
DROP TRIGGER trig_row_after2 ON rem1;

-- Test WHEN conditions
--
-- 测试 WHEN 条件

CREATE TRIGGER trig_row_before_insupd
BEFORE INSERT OR UPDATE ON rem1
FOR EACH ROW
WHEN (NEW.f2 like '%update%')
EXECUTE PROCEDURE trigger_data(23,'skidoo');

CREATE TRIGGER trig_row_after_insupd
AFTER INSERT OR UPDATE ON rem1
FOR EACH ROW
WHEN (NEW.f2 like '%update%')
EXECUTE PROCEDURE trigger_data(23,'skidoo');

-- Insert or update not matching: nothing happens
--
-- 插入或更新不匹配：没有任何反应
INSERT INTO rem1 values(1, 'insert');
UPDATE rem1 set f2 = 'test';

-- Insert or update matching: triggers are fired
--
-- 插入或更新匹配：触发触发器
INSERT INTO rem1 values(2, 'update');
UPDATE rem1 set f2 = 'update update' where f1 = '2';

CREATE TRIGGER trig_row_before_delete
BEFORE DELETE ON rem1
FOR EACH ROW
WHEN (OLD.f2 like '%update%')
EXECUTE PROCEDURE trigger_data(23,'skidoo');

CREATE TRIGGER trig_row_after_delete
AFTER DELETE ON rem1
FOR EACH ROW
WHEN (OLD.f2 like '%update%')
EXECUTE PROCEDURE trigger_data(23,'skidoo');

-- Trigger is fired for f1=2, not for f1=1
--
-- 当 f1=2 时触发触发器，而不是当 f1=1 时触发
DELETE FROM rem1;

-- cleanup
DROP TRIGGER trig_row_before_insupd ON rem1;
DROP TRIGGER trig_row_after_insupd ON rem1;
DROP TRIGGER trig_row_before_delete ON rem1;
DROP TRIGGER trig_row_after_delete ON rem1;


-- Test various RETURN statements in BEFORE triggers.
--
-- 在 BEFORE 触发器中测试各种 RETURN 语句。

CREATE FUNCTION trig_row_before_insupdate() RETURNS TRIGGER AS $$
  BEGIN
    NEW.f2 := NEW.f2 || ' triggered !';
    RETURN NEW;
  END
$$ language plpgsql;

CREATE TRIGGER trig_row_before_insupd
BEFORE INSERT OR UPDATE ON rem1
FOR EACH ROW EXECUTE PROCEDURE trig_row_before_insupdate();

-- The new values should have 'triggered' appended
--
-- 新值应附加“已触发”
INSERT INTO rem1 values(1, 'insert');
SELECT * from loc1;
INSERT INTO rem1 values(2, 'insert') RETURNING f2;
SELECT * from loc1;
UPDATE rem1 set f2 = '';
SELECT * from loc1;
UPDATE rem1 set f2 = 'skidoo' RETURNING f2;
SELECT * from loc1;

EXPLAIN (verbose, costs off)
UPDATE rem1 set f1 = 10;          -- all columns should be transmitted
UPDATE rem1 set f1 = 10;
SELECT * from loc1;

DELETE FROM rem1;

-- Add a second trigger, to check that the changes are propagated correctly
--
-- 添加第二个触发器，以检查更改是否正确传播
-- from trigger to trigger
--
-- 从触发器到触发器
CREATE TRIGGER trig_row_before_insupd2
BEFORE INSERT OR UPDATE ON rem1
FOR EACH ROW EXECUTE PROCEDURE trig_row_before_insupdate();

INSERT INTO rem1 values(1, 'insert');
SELECT * from loc1;
INSERT INTO rem1 values(2, 'insert') RETURNING f2;
SELECT * from loc1;
UPDATE rem1 set f2 = '';
SELECT * from loc1;
UPDATE rem1 set f2 = 'skidoo' RETURNING f2;
SELECT * from loc1;

DROP TRIGGER trig_row_before_insupd ON rem1;
DROP TRIGGER trig_row_before_insupd2 ON rem1;

DELETE from rem1;

INSERT INTO rem1 VALUES (1, 'test');

-- Test with a trigger returning NULL
--
-- 使用返回 NULL 的触发器进行测试
CREATE FUNCTION trig_null() RETURNS TRIGGER AS $$
  BEGIN
    RETURN NULL;
  END
$$ language plpgsql;

CREATE TRIGGER trig_null
BEFORE INSERT OR UPDATE OR DELETE ON rem1
FOR EACH ROW EXECUTE PROCEDURE trig_null();

-- Nothing should have changed.
--
-- 应该什么都没有改变。
INSERT INTO rem1 VALUES (2, 'test2');

SELECT * from loc1;

UPDATE rem1 SET f2 = 'test2';

SELECT * from loc1;

DELETE from rem1;

SELECT * from loc1;

DROP TRIGGER trig_null ON rem1;
DELETE from rem1;

-- Test a combination of local and remote triggers
--
-- 测试本地和远程触发器的组合
CREATE TRIGGER trig_row_before
BEFORE INSERT OR UPDATE OR DELETE ON rem1
FOR EACH ROW EXECUTE PROCEDURE trigger_data(23,'skidoo');

CREATE TRIGGER trig_row_after
AFTER INSERT OR UPDATE OR DELETE ON rem1
FOR EACH ROW EXECUTE PROCEDURE trigger_data(23,'skidoo');

CREATE TRIGGER trig_local_before BEFORE INSERT OR UPDATE ON loc1
FOR EACH ROW EXECUTE PROCEDURE trig_row_before_insupdate();

INSERT INTO rem1(f2) VALUES ('test');
UPDATE rem1 SET f2 = 'testo';

-- Test returning a system attribute
--
-- 测试返回系统属性
INSERT INTO rem1(f2) VALUES ('test') RETURNING ctid;

-- cleanup
DROP TRIGGER trig_row_before ON rem1;
DROP TRIGGER trig_row_after ON rem1;
DROP TRIGGER trig_local_before ON loc1;


-- Test direct foreign table modification functionality
--
-- 测试直接外表修改功能
EXPLAIN (verbose, costs off)
DELETE FROM rem1;                 -- can be pushed down
EXPLAIN (verbose, costs off)
DELETE FROM rem1 WHERE false;     -- currently can't be pushed down

-- Test with statement-level triggers
--
-- 使用语句级触发器进行测试
CREATE TRIGGER trig_stmt_before
	BEFORE DELETE OR INSERT OR UPDATE ON rem1
	FOR EACH STATEMENT EXECUTE PROCEDURE trigger_func();
EXPLAIN (verbose, costs off)
UPDATE rem1 set f2 = '';          -- can be pushed down
EXPLAIN (verbose, costs off)
DELETE FROM rem1;                 -- can be pushed down
DROP TRIGGER trig_stmt_before ON rem1;

CREATE TRIGGER trig_stmt_after
	AFTER DELETE OR INSERT OR UPDATE ON rem1
	FOR EACH STATEMENT EXECUTE PROCEDURE trigger_func();
EXPLAIN (verbose, costs off)
UPDATE rem1 set f2 = '';          -- can be pushed down
EXPLAIN (verbose, costs off)
DELETE FROM rem1;                 -- can be pushed down
DROP TRIGGER trig_stmt_after ON rem1;

-- Test with row-level ON INSERT triggers
--
-- 使用行级 ON INSERT 触发器进行测试
CREATE TRIGGER trig_row_before_insert
BEFORE INSERT ON rem1
FOR EACH ROW EXECUTE PROCEDURE trigger_data(23,'skidoo');
EXPLAIN (verbose, costs off)
UPDATE rem1 set f2 = '';          -- can be pushed down
EXPLAIN (verbose, costs off)
DELETE FROM rem1;                 -- can be pushed down
DROP TRIGGER trig_row_before_insert ON rem1;

CREATE TRIGGER trig_row_after_insert
AFTER INSERT ON rem1
FOR EACH ROW EXECUTE PROCEDURE trigger_data(23,'skidoo');
EXPLAIN (verbose, costs off)
UPDATE rem1 set f2 = '';          -- can be pushed down
EXPLAIN (verbose, costs off)
DELETE FROM rem1;                 -- can be pushed down
DROP TRIGGER trig_row_after_insert ON rem1;

-- Test with row-level ON UPDATE triggers
--
-- 使用行级 ON UPDATE 触发器进行测试
CREATE TRIGGER trig_row_before_update
BEFORE UPDATE ON rem1
FOR EACH ROW EXECUTE PROCEDURE trigger_data(23,'skidoo');
EXPLAIN (verbose, costs off)
UPDATE rem1 set f2 = '';          -- can't be pushed down
EXPLAIN (verbose, costs off)
DELETE FROM rem1;                 -- can be pushed down
DROP TRIGGER trig_row_before_update ON rem1;

CREATE TRIGGER trig_row_after_update
AFTER UPDATE ON rem1
FOR EACH ROW EXECUTE PROCEDURE trigger_data(23,'skidoo');
EXPLAIN (verbose, costs off)
UPDATE rem1 set f2 = '';          -- can't be pushed down
EXPLAIN (verbose, costs off)
DELETE FROM rem1;                 -- can be pushed down
DROP TRIGGER trig_row_after_update ON rem1;

-- Test with row-level ON DELETE triggers
--
-- 使用行级 ON DELETE 触发器进行测试
CREATE TRIGGER trig_row_before_delete
BEFORE DELETE ON rem1
FOR EACH ROW EXECUTE PROCEDURE trigger_data(23,'skidoo');
EXPLAIN (verbose, costs off)
UPDATE rem1 set f2 = '';          -- can be pushed down
EXPLAIN (verbose, costs off)
DELETE FROM rem1;                 -- can't be pushed down
DROP TRIGGER trig_row_before_delete ON rem1;

CREATE TRIGGER trig_row_after_delete
AFTER DELETE ON rem1
FOR EACH ROW EXECUTE PROCEDURE trigger_data(23,'skidoo');
EXPLAIN (verbose, costs off)
UPDATE rem1 set f2 = '';          -- can be pushed down
EXPLAIN (verbose, costs off)
DELETE FROM rem1;                 -- can't be pushed down
DROP TRIGGER trig_row_after_delete ON rem1;


-- We are allowed to create transition-table triggers on both kinds of
--
-- 我们可以在两种类型上创建转换表触发器
-- inheritance even if they contain foreign tables as children, but currently
--
-- 继承，即使它们包含外部表作为子项，但目前
-- collecting transition tuples from such foreign tables is not supported.
--
-- 不支持从此类外部表收集转换元组。

CREATE TABLE local_tbl (a text, b int);
CREATE FOREIGN TABLE foreign_tbl (a text, b int)
  SERVER loopback OPTIONS (table_name 'local_tbl');

INSERT INTO foreign_tbl VALUES ('AAA', 42);

-- Test case for partition hierarchy
--
-- 分区层次结构的测试用例
CREATE TABLE parent_tbl (a text, b int) PARTITION BY LIST (a);
ALTER TABLE parent_tbl ATTACH PARTITION foreign_tbl FOR VALUES IN ('AAA');

CREATE TRIGGER parent_tbl_insert_trig
  AFTER INSERT ON parent_tbl REFERENCING NEW TABLE AS new_table
  FOR EACH STATEMENT EXECUTE PROCEDURE trigger_func();
CREATE TRIGGER parent_tbl_update_trig
  AFTER UPDATE ON parent_tbl REFERENCING OLD TABLE AS old_table NEW TABLE AS new_table
  FOR EACH STATEMENT EXECUTE PROCEDURE trigger_func();
CREATE TRIGGER parent_tbl_delete_trig
  AFTER DELETE ON parent_tbl REFERENCING OLD TABLE AS old_table
  FOR EACH STATEMENT EXECUTE PROCEDURE trigger_func();

INSERT INTO parent_tbl VALUES ('AAA', 42);

COPY parent_tbl (a, b) FROM stdin;
AAA	42
\.

ALTER SERVER loopback OPTIONS (ADD batch_size '10');

INSERT INTO parent_tbl VALUES ('AAA', 42);

COPY parent_tbl (a, b) FROM stdin;
AAA	42
\.

ALTER SERVER loopback OPTIONS (DROP batch_size);

EXPLAIN (VERBOSE, COSTS OFF)
UPDATE parent_tbl SET b = b + 1;
UPDATE parent_tbl SET b = b + 1;

EXPLAIN (VERBOSE, COSTS OFF)
DELETE FROM parent_tbl;
DELETE FROM parent_tbl;

ALTER TABLE parent_tbl DETACH PARTITION foreign_tbl;
DROP TABLE parent_tbl;

-- Test case for non-partition hierarchy
--
-- 非分区层次结构的测试用例
CREATE TABLE parent_tbl (a text, b int);
ALTER FOREIGN TABLE foreign_tbl INHERIT parent_tbl;

CREATE TRIGGER parent_tbl_update_trig
  AFTER UPDATE ON parent_tbl REFERENCING OLD TABLE AS old_table NEW TABLE AS new_table
  FOR EACH STATEMENT EXECUTE PROCEDURE trigger_func();
CREATE TRIGGER parent_tbl_delete_trig
  AFTER DELETE ON parent_tbl REFERENCING OLD TABLE AS old_table
  FOR EACH STATEMENT EXECUTE PROCEDURE trigger_func();

EXPLAIN (VERBOSE, COSTS OFF)
UPDATE parent_tbl SET b = b + 1;
UPDATE parent_tbl SET b = b + 1;

EXPLAIN (VERBOSE, COSTS OFF)
DELETE FROM parent_tbl;
DELETE FROM parent_tbl;

ALTER FOREIGN TABLE foreign_tbl NO INHERIT parent_tbl;
DROP TABLE parent_tbl;

-- Cleanup
DROP FOREIGN TABLE foreign_tbl;
DROP TABLE local_tbl;

-- ===================================================================
-- test inheritance features
--
-- 测试继承特性
-- ===================================================================

CREATE TABLE a (aa TEXT);
CREATE TABLE loct (aa TEXT, bb TEXT);
ALTER TABLE a SET (autovacuum_enabled = 'false');
ALTER TABLE loct SET (autovacuum_enabled = 'false');
CREATE FOREIGN TABLE b (bb TEXT) INHERITS (a)
  SERVER loopback OPTIONS (table_name 'loct');

INSERT INTO a(aa) VALUES('aaa');
INSERT INTO a(aa) VALUES('aaaa');
INSERT INTO a(aa) VALUES('aaaaa');

INSERT INTO b(aa) VALUES('bbb');
INSERT INTO b(aa) VALUES('bbbb');
INSERT INTO b(aa) VALUES('bbbbb');

SELECT tableoid::regclass, * FROM a;
SELECT tableoid::regclass, * FROM b;
SELECT tableoid::regclass, * FROM ONLY a;

UPDATE a SET aa = 'zzzzzz' WHERE aa LIKE 'aaaa%';

SELECT tableoid::regclass, * FROM a;
SELECT tableoid::regclass, * FROM b;
SELECT tableoid::regclass, * FROM ONLY a;

UPDATE b SET aa = 'new';

SELECT tableoid::regclass, * FROM a;
SELECT tableoid::regclass, * FROM b;
SELECT tableoid::regclass, * FROM ONLY a;

UPDATE a SET aa = 'newtoo';

SELECT tableoid::regclass, * FROM a;
SELECT tableoid::regclass, * FROM b;
SELECT tableoid::regclass, * FROM ONLY a;

DELETE FROM a;

SELECT tableoid::regclass, * FROM a;
SELECT tableoid::regclass, * FROM b;
SELECT tableoid::regclass, * FROM ONLY a;

DROP TABLE a CASCADE;
DROP TABLE loct;

-- Check SELECT FOR UPDATE/SHARE with an inherited source table
--
-- 使用继承的源表检查 SELECT FOR UPDATE/SHARE
create table loct1 (f1 int, f2 int, f3 int);
create table loct2 (f1 int, f2 int, f3 int);

alter table loct1 set (autovacuum_enabled = 'false');
alter table loct2 set (autovacuum_enabled = 'false');

create table foo (f1 int, f2 int);
create foreign table foo2 (f3 int) inherits (foo)
  server loopback options (table_name 'loct1');
create table bar (f1 int, f2 int);
create foreign table bar2 (f3 int) inherits (bar)
  server loopback options (table_name 'loct2');

alter table foo set (autovacuum_enabled = 'false');
alter table bar set (autovacuum_enabled = 'false');

insert into foo values(1,1);
insert into foo values(3,3);
insert into foo2 values(2,2,2);
insert into foo2 values(4,4,4);
insert into bar values(1,11);
insert into bar values(2,22);
insert into bar values(6,66);
insert into bar2 values(3,33,33);
insert into bar2 values(4,44,44);
insert into bar2 values(7,77,77);

explain (verbose, costs off)
select * from bar where f1 in (select f1 from foo) for update;
select * from bar where f1 in (select f1 from foo) for update;

explain (verbose, costs off)
select * from bar where f1 in (select f1 from foo) for share;
select * from bar where f1 in (select f1 from foo) for share;

-- Now check SELECT FOR UPDATE/SHARE with an inherited source table,
--
-- 现在使用继承的源表检查 SELECT FOR UPDATE/SHARE，
-- where the parent is itself a foreign table
--
-- 其中父级本身就是一个外部表
create table loct4 (f1 int, f2 int, f3 int);
create foreign table foo2child (f3 int) inherits (foo2)
  server loopback options (table_name 'loct4');

explain (verbose, costs off)
select * from bar where f1 in (select f1 from foo2) for share;
select * from bar where f1 in (select f1 from foo2) for share;

drop foreign table foo2child;

-- And with a local child relation of the foreign table parent
--
-- 并与外部表父级的本地子级关系
create table foo2child (f3 int) inherits (foo2);

explain (verbose, costs off)
select * from bar where f1 in (select f1 from foo2) for share;
select * from bar where f1 in (select f1 from foo2) for share;

drop table foo2child;

-- Check UPDATE with inherited target and an inherited source table
--
-- 使用继承的目标和继承的源表检查 UPDATE
explain (verbose, costs off)
update bar set f2 = f2 + 100 where f1 in (select f1 from foo);
update bar set f2 = f2 + 100 where f1 in (select f1 from foo);

select tableoid::regclass, * from bar order by 1,2;

-- Check UPDATE with inherited target and an appendrel subquery
--
-- 使用继承的目标和附加子查询检查 UPDATE
explain (verbose, costs off)
update bar set f2 = f2 + 100
from
  ( select f1 from foo union all select f1+3 from foo ) ss
where bar.f1 = ss.f1;
update bar set f2 = f2 + 100
from
  ( select f1 from foo union all select f1+3 from foo ) ss
where bar.f1 = ss.f1;

select tableoid::regclass, * from bar order by 1,2;

-- Test forcing the remote server to produce sorted data for a merge join,
--
-- 测试强制远程服务器为合并连接生成排序数据，
-- but the foreign table is an inheritance child.
--
-- 但外表是一个继承子表。
truncate table loct1;
truncate table only foo;
\set num_rows_foo 2000
insert into loct1 select generate_series(0, :num_rows_foo, 2), generate_series(0, :num_rows_foo, 2), generate_series(0, :num_rows_foo, 2);
insert into foo select generate_series(1, :num_rows_foo, 2), generate_series(1, :num_rows_foo, 2);
SET enable_hashjoin to false;
SET enable_nestloop to false;
alter foreign table foo2 options (use_remote_estimate 'true');
create index i_loct1_f1 on loct1(f1);
create index i_foo_f1 on foo(f1);
analyze foo;
analyze loct1;
-- inner join; expressions in the clauses appear in the equivalence class list
--
-- 内连接；子句中的表达式出现在等价类列表中
explain (verbose, costs off)
	select foo.f1, loct1.f1 from foo join loct1 on (foo.f1 = loct1.f1) order by foo.f2 offset 10 limit 10;
select foo.f1, loct1.f1 from foo join loct1 on (foo.f1 = loct1.f1) order by foo.f2 offset 10 limit 10;
-- outer join; expressions in the clauses do not appear in equivalence class
--
-- 外连接；子句中的表达式未出现在等价类中
-- list but no output change as compared to the previous query
--
-- 列出但与之前的查询相比输出没有变化
explain (verbose, costs off)
	select foo.f1, loct1.f1 from foo left join loct1 on (foo.f1 = loct1.f1) order by foo.f2 offset 10 limit 10;
select foo.f1, loct1.f1 from foo left join loct1 on (foo.f1 = loct1.f1) order by foo.f2 offset 10 limit 10;
RESET enable_hashjoin;
RESET enable_nestloop;

-- Test that WHERE CURRENT OF is not supported
--
-- 测试是否不支持 WHERE CURRENT OF
begin;
declare c cursor for select * from bar where f1 = 7;
fetch from c;
update bar set f2 = null where current of c;
rollback;

explain (verbose, costs off)
delete from foo where f1 < 5 returning *;
delete from foo where f1 < 5 returning *;
explain (verbose, costs off)
update bar set f2 = f2 + 100 returning *;
update bar set f2 = f2 + 100 returning *;

-- Test that UPDATE/DELETE with inherited target works with row-level triggers
--
-- 测试带有继承目标的 UPDATE/DELETE 是否适用于行级触发器
CREATE TRIGGER trig_row_before
BEFORE UPDATE OR DELETE ON bar2
FOR EACH ROW EXECUTE PROCEDURE trigger_data(23,'skidoo');

CREATE TRIGGER trig_row_after
AFTER UPDATE OR DELETE ON bar2
FOR EACH ROW EXECUTE PROCEDURE trigger_data(23,'skidoo');

explain (verbose, costs off)
update bar set f2 = f2 + 100;
update bar set f2 = f2 + 100;

explain (verbose, costs off)
delete from bar where f2 < 400;
delete from bar where f2 < 400;

-- cleanup
drop table foo cascade;
drop table bar cascade;
drop table loct1;
drop table loct2;

-- Test pushing down UPDATE/DELETE joins to the remote server
--
-- 测试将 UPDATE/DELETE 连接下推到远程服务器
create table parent (a int, b text);
create table loct1 (a int, b text);
create table loct2 (a int, b text);
create foreign table remt1 (a int, b text)
  server loopback options (table_name 'loct1');
create foreign table remt2 (a int, b text)
  server loopback options (table_name 'loct2');
alter foreign table remt1 inherit parent;

insert into remt1 values (1, 'foo');
insert into remt1 values (2, 'bar');
insert into remt2 values (1, 'foo');
insert into remt2 values (2, 'bar');

analyze remt1;
analyze remt2;

explain (verbose, costs off)
update parent set b = parent.b || remt2.b from remt2 where parent.a = remt2.a returning *;
update parent set b = parent.b || remt2.b from remt2 where parent.a = remt2.a returning *;
explain (verbose, costs off)
delete from parent using remt2 where parent.a = remt2.a returning parent;
delete from parent using remt2 where parent.a = remt2.a returning parent;

-- cleanup
drop foreign table remt1;
drop foreign table remt2;
drop table loct1;
drop table loct2;
drop table parent;

-- ===================================================================
-- test tuple routing for foreign-table partitions
--
-- 测试外部表分区的元组路由
-- ===================================================================

-- Test insert tuple routing
--
-- 测试插入元组路由
create table itrtest (a int, b text) partition by list (a);
create table loct1 (a int check (a in (1)), b text);
create foreign table remp1 (a int check (a in (1)), b text) server loopback options (table_name 'loct1');
create table loct2 (a int check (a in (2)), b text);
create foreign table remp2 (b text, a int check (a in (2))) server loopback options (table_name 'loct2');
alter table itrtest attach partition remp1 for values in (1);
alter table itrtest attach partition remp2 for values in (2);

insert into itrtest values (1, 'foo');
insert into itrtest values (1, 'bar') returning *;
insert into itrtest values (2, 'baz');
insert into itrtest values (2, 'qux') returning *;
insert into itrtest values (1, 'test1'), (2, 'test2') returning *;

select tableoid::regclass, * FROM itrtest;
select tableoid::regclass, * FROM remp1;
select tableoid::regclass, * FROM remp2;

delete from itrtest;

-- MERGE ought to fail cleanly
--
-- MERGE 应该彻底失败
merge into itrtest using (select 1, 'foo') as source on (true)
  when matched then do nothing;

create unique index loct1_idx on loct1 (a);

-- DO NOTHING without an inference specification is supported
--
-- 支持在没有推理规范的情况下不执行任何操作
insert into itrtest values (1, 'foo') on conflict do nothing returning *;
insert into itrtest values (1, 'foo') on conflict do nothing returning *;

-- But other cases are not supported
--
-- 但其他情况不支持
insert into itrtest values (1, 'bar') on conflict (a) do nothing;
insert into itrtest values (1, 'bar') on conflict (a) do update set b = excluded.b;

select tableoid::regclass, * FROM itrtest;

delete from itrtest;

drop index loct1_idx;

-- Test that remote triggers work with insert tuple routing
--
-- 测试远程触发器是否与插入元组路由一起使用
create function br_insert_trigfunc() returns trigger as $$
begin
	new.b := new.b || ' triggered !';
	return new;
end
$$ language plpgsql;
create trigger loct1_br_insert_trigger before insert on loct1
	for each row execute procedure br_insert_trigfunc();
create trigger loct2_br_insert_trigger before insert on loct2
	for each row execute procedure br_insert_trigfunc();

-- The new values are concatenated with ' triggered !'
--
-- 新值与“已触发！”连接起来
insert into itrtest values (1, 'foo') returning *;
insert into itrtest values (2, 'qux') returning *;
insert into itrtest values (1, 'test1'), (2, 'test2') returning *;
with result as (insert into itrtest values (1, 'test1'), (2, 'test2') returning *) select * from result;

drop trigger loct1_br_insert_trigger on loct1;
drop trigger loct2_br_insert_trigger on loct2;

drop table itrtest;
drop table loct1;
drop table loct2;

-- Test update tuple routing
--
-- 测试更新元组路由
create table utrtest (a int, b text) partition by list (a);
create table loct (a int check (a in (1)), b text);
create foreign table remp (a int check (a in (1)), b text) server loopback options (table_name 'loct');
create table locp (a int check (a in (2)), b text);
alter table utrtest attach partition remp for values in (1);
alter table utrtest attach partition locp for values in (2);

insert into utrtest values (1, 'foo');
insert into utrtest values (2, 'qux');

select tableoid::regclass, * FROM utrtest;
select tableoid::regclass, * FROM remp;
select tableoid::regclass, * FROM locp;

-- It's not allowed to move a row from a partition that is foreign to another
--
-- 不允许从一个分区中移动与另一个分区无关的行
update utrtest set a = 2 where b = 'foo' returning *;

-- But the reverse is allowed
--
-- 但反过来也是允许的
update utrtest set a = 1 where b = 'qux' returning *;

select tableoid::regclass, * FROM utrtest;
select tableoid::regclass, * FROM remp;
select tableoid::regclass, * FROM locp;

-- The executor should not let unexercised FDWs shut down
--
-- 执行者不应让未执行的外籍家政工人关闭
update utrtest set a = 1 where b = 'foo';

-- Test that remote triggers work with update tuple routing
--
-- 测试远程触发器是否与更新元组路由一起使用
create trigger loct_br_insert_trigger before insert on loct
	for each row execute procedure br_insert_trigfunc();

delete from utrtest;
insert into utrtest values (2, 'qux');

-- Check case where the foreign partition is a subplan target rel
--
-- 检查外部分区是子计划目标 rel 的情况
explain (verbose, costs off)
update utrtest set a = 1 where a = 1 or a = 2 returning *;
-- The new values are concatenated with ' triggered !'
--
-- 新值与“已触发！”连接起来
update utrtest set a = 1 where a = 1 or a = 2 returning *;

delete from utrtest;
insert into utrtest values (2, 'qux');

-- Check case where the foreign partition isn't a subplan target rel
--
-- 检查外部分区不是子计划目标相关的情况
explain (verbose, costs off)
update utrtest set a = 1 where a = 2 returning *;
-- The new values are concatenated with ' triggered !'
--
-- 新值与“已触发！”连接起来
update utrtest set a = 1 where a = 2 returning *;

drop trigger loct_br_insert_trigger on loct;

-- We can move rows to a foreign partition that has been updated already,
--
-- 我们可以将行移动到已经更新的外部分区，
-- but can't move rows to a foreign partition that hasn't been updated yet
--
-- 但无法将行移动到尚未更新的外部分区

delete from utrtest;
insert into utrtest values (1, 'foo');
insert into utrtest values (2, 'qux');

-- Test the former case:
--
-- 测试前一种情况：
-- with a direct modification plan
--
-- 直接修改计划
explain (verbose, costs off)
update utrtest set a = 1 returning *;
update utrtest set a = 1 returning *;

delete from utrtest;
insert into utrtest values (1, 'foo');
insert into utrtest values (2, 'qux');

-- with a non-direct modification plan
--
-- 具有非直接修改计划
explain (verbose, costs off)
update utrtest set a = 1 from (values (1), (2)) s(x) where a = s.x returning *;
update utrtest set a = 1 from (values (1), (2)) s(x) where a = s.x returning *;

-- Change the definition of utrtest so that the foreign partition get updated
--
-- 更改 utrtest 的定义，以便更新外部分区
-- after the local partition
--
-- 本地分区后
delete from utrtest;
alter table utrtest detach partition remp;
drop foreign table remp;
alter table loct drop constraint loct_a_check;
alter table loct add check (a in (3));
create foreign table remp (a int check (a in (3)), b text) server loopback options (table_name 'loct');
alter table utrtest attach partition remp for values in (3);
insert into utrtest values (2, 'qux');
insert into utrtest values (3, 'xyzzy');

-- Test the latter case:
--
-- 测试后一种情况：
-- with a direct modification plan
--
-- 直接修改计划
explain (verbose, costs off)
update utrtest set a = 3 returning *;
update utrtest set a = 3 returning *; -- ERROR

-- with a non-direct modification plan
--
-- 具有非直接修改计划
explain (verbose, costs off)
update utrtest set a = 3 from (values (2), (3)) s(x) where a = s.x returning *;
update utrtest set a = 3 from (values (2), (3)) s(x) where a = s.x returning *; -- ERROR

drop table utrtest;
drop table loct;

-- Test copy tuple routing
--
-- 测试复制元组路由
create table ctrtest (a int, b text) partition by list (a);
create table loct1 (a int check (a in (1)), b text);
create foreign table remp1 (a int check (a in (1)), b text) server loopback options (table_name 'loct1');
create table loct2 (a int check (a in (2)), b text);
create foreign table remp2 (b text, a int check (a in (2))) server loopback options (table_name 'loct2');
alter table ctrtest attach partition remp1 for values in (1);
alter table ctrtest attach partition remp2 for values in (2);

copy ctrtest from stdin;
1	foo
2	qux
\.

select tableoid::regclass, * FROM ctrtest;
select tableoid::regclass, * FROM remp1;
select tableoid::regclass, * FROM remp2;

-- Copying into foreign partitions directly should work as well
--
-- 直接复制到外部分区应该也可以
copy remp1 from stdin;
1	bar
\.

select tableoid::regclass, * FROM remp1;

delete from ctrtest;

-- Test copy tuple routing with the batch_size option enabled
--
-- 启用batch_size选项测试复制元组路由
alter server loopback options (add batch_size '2');

copy ctrtest from stdin;
1	foo
1	bar
2	baz
2	qux
1	test1
2	test2
\.

select tableoid::regclass, * FROM ctrtest;
select tableoid::regclass, * FROM remp1;
select tableoid::regclass, * FROM remp2;

delete from ctrtest;

alter server loopback options (drop batch_size);

drop table ctrtest;
drop table loct1;
drop table loct2;

-- ===================================================================
-- test COPY FROM
--
-- 测试复制自
-- ===================================================================

create table loc2 (f1 int, f2 text);
alter table loc2 set (autovacuum_enabled = 'false');
create foreign table rem2 (f1 int, f2 text) server loopback options(table_name 'loc2');

-- Test basic functionality
--
-- 测试基本功能
copy rem2 from stdin;
1	foo
2	bar
\.
select * from rem2;

delete from rem2;

-- Test check constraints
--
-- 测试检查约束
alter table loc2 add constraint loc2_f1positive check (f1 >= 0);
alter foreign table rem2 add constraint rem2_f1positive check (f1 >= 0);

-- check constraint is enforced on the remote side, not locally
--
-- 检查约束在远程端强制执行，而不是在本地强制执行
copy rem2 from stdin;
1	foo
2	bar
\.
copy rem2 from stdin; -- ERROR
-1	xyzzy
\.
select * from rem2;

alter foreign table rem2 drop constraint rem2_f1positive;
alter table loc2 drop constraint loc2_f1positive;

delete from rem2;

-- Test local triggers
--
-- 测试本地触发器
create trigger trig_stmt_before before insert on rem2
	for each statement execute procedure trigger_func();
create trigger trig_stmt_after after insert on rem2
	for each statement execute procedure trigger_func();
create trigger trig_row_before before insert on rem2
	for each row execute procedure trigger_data(23,'skidoo');
create trigger trig_row_after after insert on rem2
	for each row execute procedure trigger_data(23,'skidoo');

copy rem2 from stdin;
1	foo
2	bar
\.
select * from rem2;

drop trigger trig_row_before on rem2;
drop trigger trig_row_after on rem2;
drop trigger trig_stmt_before on rem2;
drop trigger trig_stmt_after on rem2;

delete from rem2;

create trigger trig_row_before_insert before insert on rem2
	for each row execute procedure trig_row_before_insupdate();

-- The new values are concatenated with ' triggered !'
--
-- 新值与“已触发！”连接起来
copy rem2 from stdin;
1	foo
2	bar
\.
select * from rem2;

drop trigger trig_row_before_insert on rem2;

delete from rem2;

create trigger trig_null before insert on rem2
	for each row execute procedure trig_null();

-- Nothing happens
--
-- 什么也没发生
copy rem2 from stdin;
1	foo
2	bar
\.
select * from rem2;

drop trigger trig_null on rem2;

delete from rem2;

-- Test remote triggers
--
-- 测试远程触发器
create trigger trig_row_before_insert before insert on loc2
	for each row execute procedure trig_row_before_insupdate();

-- The new values are concatenated with ' triggered !'
--
-- 新值与“已触发！”连接起来
copy rem2 from stdin;
1	foo
2	bar
\.
select * from rem2;

drop trigger trig_row_before_insert on loc2;

delete from rem2;

create trigger trig_null before insert on loc2
	for each row execute procedure trig_null();

-- Nothing happens
--
-- 什么也没发生
copy rem2 from stdin;
1	foo
2	bar
\.
select * from rem2;

drop trigger trig_null on loc2;

delete from rem2;

-- Test a combination of local and remote triggers
--
-- 测试本地和远程触发器的组合
create trigger rem2_trig_row_before before insert on rem2
	for each row execute procedure trigger_data(23,'skidoo');
create trigger rem2_trig_row_after after insert on rem2
	for each row execute procedure trigger_data(23,'skidoo');
create trigger loc2_trig_row_before_insert before insert on loc2
	for each row execute procedure trig_row_before_insupdate();

copy rem2 from stdin;
1	foo
2	bar
\.
select * from rem2;

drop trigger rem2_trig_row_before on rem2;
drop trigger rem2_trig_row_after on rem2;
drop trigger loc2_trig_row_before_insert on loc2;

delete from rem2;

-- test COPY FROM with foreign table created in the same transaction
--
-- 使用在同一事务中创建的外部表测试 COPY FROM
create table loc3 (f1 int, f2 text);
begin;
create foreign table rem3 (f1 int, f2 text)
	server loopback options(table_name 'loc3');
copy rem3 from stdin;
1	foo
2	bar
\.
commit;
select * from rem3;
drop foreign table rem3;
drop table loc3;

-- Test COPY FROM with the batch_size option enabled
--
-- 在启用batch_size选项的情况下测试COPY FROM
alter server loopback options (add batch_size '2');

-- Test basic functionality
--
-- 测试基本功能
copy rem2 from stdin;
1	foo
2	bar
3	baz
\.
select * from rem2;

delete from rem2;

-- Test check constraints
--
-- 测试检查约束
alter table loc2 add constraint loc2_f1positive check (f1 >= 0);
alter foreign table rem2 add constraint rem2_f1positive check (f1 >= 0);

-- check constraint is enforced on the remote side, not locally
--
-- 检查约束在远程端强制执行，而不是在本地强制执行
copy rem2 from stdin;
1	foo
2	bar
3	baz
\.
copy rem2 from stdin; -- ERROR
-1	xyzzy
\.
select * from rem2;

alter foreign table rem2 drop constraint rem2_f1positive;
alter table loc2 drop constraint loc2_f1positive;

delete from rem2;

-- Test remote triggers
--
-- 测试远程触发器
create trigger trig_row_before_insert before insert on loc2
	for each row execute procedure trig_row_before_insupdate();

-- The new values are concatenated with ' triggered !'
--
-- 新值与“已触发！”连接起来
copy rem2 from stdin;
1	foo
2	bar
3	baz
\.
select * from rem2;

drop trigger trig_row_before_insert on loc2;

delete from rem2;

create trigger trig_null before insert on loc2
	for each row execute procedure trig_null();

-- Nothing happens
--
-- 什么也没发生
copy rem2 from stdin;
1	foo
2	bar
3	baz
\.
select * from rem2;

drop trigger trig_null on loc2;

delete from rem2;

-- Check with zero-column foreign table; batch insert will be disabled
--
-- 检查零列外部表；批量插入将被禁用
alter table loc2 drop column f1;
alter table loc2 drop column f2;
alter table rem2 drop column f1;
alter table rem2 drop column f2;
copy rem2 from stdin;



\.
select * from rem2;

delete from rem2;

alter server loopback options (drop batch_size);

-- ===================================================================
-- test for TRUNCATE
--
-- 测试截断
-- ===================================================================
CREATE TABLE tru_rtable0 (id int primary key);
CREATE FOREIGN TABLE tru_ftable (id int)
       SERVER loopback OPTIONS (table_name 'tru_rtable0');
INSERT INTO tru_rtable0 (SELECT x FROM generate_series(1,10) x);

CREATE TABLE tru_ptable (id int) PARTITION BY HASH(id);
CREATE TABLE tru_ptable__p0 PARTITION OF tru_ptable
                            FOR VALUES WITH (MODULUS 2, REMAINDER 0);
CREATE TABLE tru_rtable1 (id int primary key);
CREATE FOREIGN TABLE tru_ftable__p1 PARTITION OF tru_ptable
                                    FOR VALUES WITH (MODULUS 2, REMAINDER 1)
       SERVER loopback OPTIONS (table_name 'tru_rtable1');
INSERT INTO tru_ptable (SELECT x FROM generate_series(11,20) x);

CREATE TABLE tru_pk_table(id int primary key);
CREATE TABLE tru_fk_table(fkey int references tru_pk_table(id));
INSERT INTO tru_pk_table (SELECT x FROM generate_series(1,10) x);
INSERT INTO tru_fk_table (SELECT x % 10 + 1 FROM generate_series(5,25) x);
CREATE FOREIGN TABLE tru_pk_ftable (id int)
       SERVER loopback OPTIONS (table_name 'tru_pk_table');

CREATE TABLE tru_rtable_parent (id int);
CREATE TABLE tru_rtable_child (id int);
CREATE FOREIGN TABLE tru_ftable_parent (id int)
       SERVER loopback OPTIONS (table_name 'tru_rtable_parent');
CREATE FOREIGN TABLE tru_ftable_child () INHERITS (tru_ftable_parent)
       SERVER loopback OPTIONS (table_name 'tru_rtable_child');
INSERT INTO tru_rtable_parent (SELECT x FROM generate_series(1,8) x);
INSERT INTO tru_rtable_child  (SELECT x FROM generate_series(10, 18) x);

-- normal truncate
--
-- 正常截断
SELECT sum(id) FROM tru_ftable;        -- 55
TRUNCATE tru_ftable;
SELECT count(*) FROM tru_rtable0;		-- 0
SELECT count(*) FROM tru_ftable;		-- 0

-- 'truncatable' option
--
-- “可截断”选项
ALTER SERVER loopback OPTIONS (ADD truncatable 'false');
TRUNCATE tru_ftable;			-- error
ALTER FOREIGN TABLE tru_ftable OPTIONS (ADD truncatable 'true');
TRUNCATE tru_ftable;			-- accepted
ALTER FOREIGN TABLE tru_ftable OPTIONS (SET truncatable 'false');
TRUNCATE tru_ftable;			-- error
ALTER SERVER loopback OPTIONS (DROP truncatable);
ALTER FOREIGN TABLE tru_ftable OPTIONS (SET truncatable 'false');
TRUNCATE tru_ftable;			-- error
ALTER FOREIGN TABLE tru_ftable OPTIONS (SET truncatable 'true');
TRUNCATE tru_ftable;			-- accepted

-- partitioned table with both local and foreign tables as partitions
--
-- 以本地表和外部表作为分区的分区表
SELECT sum(id) FROM tru_ptable;        -- 155
TRUNCATE tru_ptable;
SELECT count(*) FROM tru_ptable;		-- 0
SELECT count(*) FROM tru_ptable__p0;	-- 0
SELECT count(*) FROM tru_ftable__p1;	-- 0
SELECT count(*) FROM tru_rtable1;		-- 0

-- 'CASCADE' option
--
-- “级联”选项
SELECT sum(id) FROM tru_pk_ftable;      -- 55
TRUNCATE tru_pk_ftable;	-- failed by FK reference
TRUNCATE tru_pk_ftable CASCADE;
SELECT count(*) FROM tru_pk_ftable;    -- 0
SELECT count(*) FROM tru_fk_table;		-- also truncated,0

-- truncate two tables at a command
--
-- 通过命令截断两个表
INSERT INTO tru_ftable (SELECT x FROM generate_series(1,8) x);
INSERT INTO tru_pk_ftable (SELECT x FROM generate_series(3,10) x);
SELECT count(*) from tru_ftable; -- 8
SELECT count(*) from tru_pk_ftable; -- 8
TRUNCATE tru_ftable, tru_pk_ftable CASCADE;
SELECT count(*) from tru_ftable; -- 0
SELECT count(*) from tru_pk_ftable; -- 0

-- truncate with ONLY clause
--
-- 使用 ONLY 子句截断
-- Since ONLY is specified, the table tru_ftable_child that inherits
--
-- 由于指定了ONLY，所以继承的表tru_ftable_child
-- tru_ftable_parent locally is not truncated.
--
-- tru_ftable_parent 本地不会被截断。
TRUNCATE ONLY tru_ftable_parent;
SELECT sum(id) FROM tru_ftable_parent;  -- 126
TRUNCATE tru_ftable_parent;
SELECT count(*) FROM tru_ftable_parent; -- 0

-- in case when remote table has inherited children
--
-- 如果远程表继承了子表
CREATE TABLE tru_rtable0_child () INHERITS (tru_rtable0);
INSERT INTO tru_rtable0 (SELECT x FROM generate_series(5,9) x);
INSERT INTO tru_rtable0_child (SELECT x FROM generate_series(10,14) x);
SELECT sum(id) FROM tru_ftable;   -- 95

-- Both parent and child tables in the foreign server are truncated
--
-- 外部服务器中的父表和子表都被截断
-- even though ONLY is specified because ONLY has no effect
--
-- 即使指定了 ONLY，因为 ONLY 没有效果
-- when truncating a foreign table.
--
-- 截断外部表时。
TRUNCATE ONLY tru_ftable;
SELECT count(*) FROM tru_ftable;   -- 0

INSERT INTO tru_rtable0 (SELECT x FROM generate_series(21,25) x);
INSERT INTO tru_rtable0_child (SELECT x FROM generate_series(26,30) x);
SELECT sum(id) FROM tru_ftable;		-- 255
TRUNCATE tru_ftable;			-- truncate both of parent and child
SELECT count(*) FROM tru_ftable;    -- 0

-- cleanup
DROP FOREIGN TABLE tru_ftable_parent, tru_ftable_child, tru_pk_ftable,tru_ftable__p1,tru_ftable;
DROP TABLE tru_rtable0, tru_rtable1, tru_ptable, tru_ptable__p0, tru_pk_table, tru_fk_table,
tru_rtable_parent,tru_rtable_child, tru_rtable0_child;

-- ===================================================================
-- test IMPORT FOREIGN SCHEMA
--
-- 测试导入外国模式
-- ===================================================================

CREATE SCHEMA import_source;
CREATE TABLE import_source.t1 (c1 int, c2 varchar NOT NULL);
CREATE TABLE import_source.t2 (c1 int default 42, c2 varchar NULL, c3 text collate "POSIX");
CREATE TYPE typ1 AS (m1 int, m2 varchar);
CREATE TABLE import_source.t3 (c1 timestamptz default now(), c2 typ1);
CREATE TABLE import_source."x 4" (c1 float8, "C 2" text, c3 varchar(42));
CREATE TABLE import_source."x 5" (c1 float8);
ALTER TABLE import_source."x 5" DROP COLUMN c1;
CREATE TABLE import_source."x 6" (c1 int, c2 int generated always as (c1 * 2) stored);
CREATE TABLE import_source.t4 (c1 int) PARTITION BY RANGE (c1);
CREATE TABLE import_source.t4_part PARTITION OF import_source.t4
  FOR VALUES FROM (1) TO (100);
CREATE TABLE import_source.t4_part2 PARTITION OF import_source.t4
  FOR VALUES FROM (100) TO (200);

CREATE SCHEMA import_dest1;
IMPORT FOREIGN SCHEMA import_source FROM SERVER loopback INTO import_dest1;
\det+ import_dest1.*
\d import_dest1.*

-- Options
CREATE SCHEMA import_dest2;
IMPORT FOREIGN SCHEMA import_source FROM SERVER loopback INTO import_dest2
  OPTIONS (import_default 'true');
\det+ import_dest2.*
\d import_dest2.*
CREATE SCHEMA import_dest3;
IMPORT FOREIGN SCHEMA import_source FROM SERVER loopback INTO import_dest3
  OPTIONS (import_collate 'false', import_generated 'false', import_not_null 'false');
\det+ import_dest3.*
\d import_dest3.*

-- Check LIMIT TO and EXCEPT
--
-- 检查 LIMIT TO 和 EXCEPT
CREATE SCHEMA import_dest4;
IMPORT FOREIGN SCHEMA import_source LIMIT TO (t1, nonesuch, t4_part)
  FROM SERVER loopback INTO import_dest4;
\det+ import_dest4.*
IMPORT FOREIGN SCHEMA import_source EXCEPT (t1, "x 4", nonesuch, t4_part)
  FROM SERVER loopback INTO import_dest4;
\det+ import_dest4.*

-- Assorted error cases
--
-- 各种错误情况
IMPORT FOREIGN SCHEMA import_source FROM SERVER loopback INTO import_dest4;
IMPORT FOREIGN SCHEMA nonesuch FROM SERVER loopback INTO import_dest4;
IMPORT FOREIGN SCHEMA nonesuch FROM SERVER loopback INTO notthere;
IMPORT FOREIGN SCHEMA nonesuch FROM SERVER nowhere INTO notthere;

-- Check case of a type present only on the remote server.
--
-- 检查仅在远程服务器上存在的类型的大小写。
-- We can fake this by dropping the type locally in our transaction.
--
-- 我们可以通过在交易中本地删除类型来伪造这一点。
CREATE TYPE "Colors" AS ENUM ('red', 'green', 'blue');
CREATE TABLE import_source.t5 (c1 int, c2 text collate "C", "Col" "Colors");

CREATE SCHEMA import_dest5;
BEGIN;
DROP TYPE "Colors" CASCADE;
IMPORT FOREIGN SCHEMA import_source LIMIT TO (t5)
  FROM SERVER loopback INTO import_dest5;  -- ERROR

ROLLBACK;

BEGIN;


CREATE SERVER fetch101 FOREIGN DATA WRAPPER postgres_fdw OPTIONS( fetch_size '101' );

SELECT count(*)
FROM pg_foreign_server
WHERE srvname = 'fetch101'
AND srvoptions @> array['fetch_size=101'];

ALTER SERVER fetch101 OPTIONS( SET fetch_size '202' );

SELECT count(*)
FROM pg_foreign_server
WHERE srvname = 'fetch101'
AND srvoptions @> array['fetch_size=101'];

SELECT count(*)
FROM pg_foreign_server
WHERE srvname = 'fetch101'
AND srvoptions @> array['fetch_size=202'];

CREATE FOREIGN TABLE table30000 ( x int ) SERVER fetch101 OPTIONS ( fetch_size '30000' );

SELECT COUNT(*)
FROM pg_foreign_table
WHERE ftrelid = 'table30000'::regclass
AND ftoptions @> array['fetch_size=30000'];

ALTER FOREIGN TABLE table30000 OPTIONS ( SET fetch_size '60000');

SELECT COUNT(*)
FROM pg_foreign_table
WHERE ftrelid = 'table30000'::regclass
AND ftoptions @> array['fetch_size=30000'];

SELECT COUNT(*)
FROM pg_foreign_table
WHERE ftrelid = 'table30000'::regclass
AND ftoptions @> array['fetch_size=60000'];

ROLLBACK;

-- ===================================================================
-- test partitionwise joins
--
-- 测试分区连接
-- ===================================================================
SET enable_partitionwise_join=on;

CREATE TABLE fprt1 (a int, b int, c varchar) PARTITION BY RANGE(a);
CREATE TABLE fprt1_p1 (LIKE fprt1);
CREATE TABLE fprt1_p2 (LIKE fprt1);
ALTER TABLE fprt1_p1 SET (autovacuum_enabled = 'false');
ALTER TABLE fprt1_p2 SET (autovacuum_enabled = 'false');
INSERT INTO fprt1_p1 SELECT i, i, to_char(i/50, 'FM0000') FROM generate_series(0, 249, 2) i;
INSERT INTO fprt1_p2 SELECT i, i, to_char(i/50, 'FM0000') FROM generate_series(250, 499, 2) i;
CREATE FOREIGN TABLE ftprt1_p1 PARTITION OF fprt1 FOR VALUES FROM (0) TO (250)
	SERVER loopback OPTIONS (table_name 'fprt1_p1', use_remote_estimate 'true');
CREATE FOREIGN TABLE ftprt1_p2 PARTITION OF fprt1 FOR VALUES FROM (250) TO (500)
	SERVER loopback OPTIONS (TABLE_NAME 'fprt1_p2');
ANALYZE fprt1;
ANALYZE fprt1_p1;
ANALYZE fprt1_p2;

CREATE TABLE fprt2 (a int, b int, c varchar) PARTITION BY RANGE(b);
CREATE TABLE fprt2_p1 (LIKE fprt2);
CREATE TABLE fprt2_p2 (LIKE fprt2);
ALTER TABLE fprt2_p1 SET (autovacuum_enabled = 'false');
ALTER TABLE fprt2_p2 SET (autovacuum_enabled = 'false');
INSERT INTO fprt2_p1 SELECT i, i, to_char(i/50, 'FM0000') FROM generate_series(0, 249, 3) i;
INSERT INTO fprt2_p2 SELECT i, i, to_char(i/50, 'FM0000') FROM generate_series(250, 499, 3) i;
CREATE FOREIGN TABLE ftprt2_p1 (b int, c varchar, a int)
	SERVER loopback OPTIONS (table_name 'fprt2_p1', use_remote_estimate 'true');
ALTER TABLE fprt2 ATTACH PARTITION ftprt2_p1 FOR VALUES FROM (0) TO (250);
CREATE FOREIGN TABLE ftprt2_p2 PARTITION OF fprt2 FOR VALUES FROM (250) TO (500)
	SERVER loopback OPTIONS (table_name 'fprt2_p2', use_remote_estimate 'true');
ANALYZE fprt2;
ANALYZE fprt2_p1;
ANALYZE fprt2_p2;

-- inner join three tables
--
-- 内连接三个表
EXPLAIN (COSTS OFF)
SELECT t1.a,t2.b,t3.c FROM fprt1 t1 INNER JOIN fprt2 t2 ON (t1.a = t2.b) INNER JOIN fprt1 t3 ON (t2.b = t3.a) WHERE t1.a % 25 =0 ORDER BY 1,2,3;
SELECT t1.a,t2.b,t3.c FROM fprt1 t1 INNER JOIN fprt2 t2 ON (t1.a = t2.b) INNER JOIN fprt1 t3 ON (t2.b = t3.a) WHERE t1.a % 25 =0 ORDER BY 1,2,3;

-- left outer join + nullable clause
--
-- 左外连接 + 可空子句
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.a,t2.b,t2.c FROM fprt1 t1 LEFT JOIN (SELECT * FROM fprt2 WHERE a < 10) t2 ON (t1.a = t2.b and t1.b = t2.a) WHERE t1.a < 10 ORDER BY 1,2,3;
SELECT t1.a,t2.b,t2.c FROM fprt1 t1 LEFT JOIN (SELECT * FROM fprt2 WHERE a < 10) t2 ON (t1.a = t2.b and t1.b = t2.a) WHERE t1.a < 10 ORDER BY 1,2,3;

-- with whole-row reference; partitionwise join does not apply
--
-- 具有整行参考；分区联接不适用
EXPLAIN (COSTS OFF)
SELECT t1.wr, t2.wr FROM (SELECT t1 wr, a FROM fprt1 t1 WHERE t1.a % 25 = 0) t1 FULL JOIN (SELECT t2 wr, b FROM fprt2 t2 WHERE t2.b % 25 = 0) t2 ON (t1.a = t2.b) ORDER BY 1,2;
SELECT t1.wr, t2.wr FROM (SELECT t1 wr, a FROM fprt1 t1 WHERE t1.a % 25 = 0) t1 FULL JOIN (SELECT t2 wr, b FROM fprt2 t2 WHERE t2.b % 25 = 0) t2 ON (t1.a = t2.b) ORDER BY 1,2;

-- join with lateral reference
--
-- 与横向参考连接
EXPLAIN (COSTS OFF)
SELECT t1.a,t1.b FROM fprt1 t1, LATERAL (SELECT t2.a, t2.b FROM fprt2 t2 WHERE t1.a = t2.b AND t1.b = t2.a) q WHERE t1.a%25 = 0 ORDER BY 1,2;
SELECT t1.a,t1.b FROM fprt1 t1, LATERAL (SELECT t2.a, t2.b FROM fprt2 t2 WHERE t1.a = t2.b AND t1.b = t2.a) q WHERE t1.a%25 = 0 ORDER BY 1,2;

-- with PHVs, partitionwise join selected but no join pushdown
--
-- 对于 PHV，选择了分区联接，但没有联接下推
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.phv, t2.b, t2.phv FROM (SELECT 't1_phv' phv, * FROM fprt1 WHERE a % 25 = 0) t1 FULL JOIN (SELECT 't2_phv' phv, * FROM fprt2 WHERE b % 25 = 0) t2 ON (t1.a = t2.b) ORDER BY t1.a, t2.b;
SELECT t1.a, t1.phv, t2.b, t2.phv FROM (SELECT 't1_phv' phv, * FROM fprt1 WHERE a % 25 = 0) t1 FULL JOIN (SELECT 't2_phv' phv, * FROM fprt2 WHERE b % 25 = 0) t2 ON (t1.a = t2.b) ORDER BY t1.a, t2.b;

-- test FOR UPDATE; partitionwise join does not apply
--
-- 测试更新；分区联接不适用
EXPLAIN (COSTS OFF)
SELECT t1.a, t2.b FROM fprt1 t1 INNER JOIN fprt2 t2 ON (t1.a = t2.b) WHERE t1.a % 25 = 0 ORDER BY 1,2 FOR UPDATE OF t1;
SELECT t1.a, t2.b FROM fprt1 t1 INNER JOIN fprt2 t2 ON (t1.a = t2.b) WHERE t1.a % 25 = 0 ORDER BY 1,2 FOR UPDATE OF t1;

RESET enable_partitionwise_join;


-- ===================================================================
-- test partitionwise aggregates
--
-- 测试分区聚合
-- ===================================================================

CREATE TABLE pagg_tab (a int, b int, c text) PARTITION BY RANGE(a);

CREATE TABLE pagg_tab_p1 (LIKE pagg_tab);
CREATE TABLE pagg_tab_p2 (LIKE pagg_tab);
CREATE TABLE pagg_tab_p3 (LIKE pagg_tab);

INSERT INTO pagg_tab_p1 SELECT i % 30, i % 50, to_char(i/30, 'FM0000') FROM generate_series(1, 3000) i WHERE (i % 30) < 10;
INSERT INTO pagg_tab_p2 SELECT i % 30, i % 50, to_char(i/30, 'FM0000') FROM generate_series(1, 3000) i WHERE (i % 30) < 20 and (i % 30) >= 10;
INSERT INTO pagg_tab_p3 SELECT i % 30, i % 50, to_char(i/30, 'FM0000') FROM generate_series(1, 3000) i WHERE (i % 30) < 30 and (i % 30) >= 20;

-- Create foreign partitions
--
-- 创建外部分区
CREATE FOREIGN TABLE fpagg_tab_p1 PARTITION OF pagg_tab FOR VALUES FROM (0) TO (10) SERVER loopback OPTIONS (table_name 'pagg_tab_p1');
CREATE FOREIGN TABLE fpagg_tab_p2 PARTITION OF pagg_tab FOR VALUES FROM (10) TO (20) SERVER loopback OPTIONS (table_name 'pagg_tab_p2');
CREATE FOREIGN TABLE fpagg_tab_p3 PARTITION OF pagg_tab FOR VALUES FROM (20) TO (30) SERVER loopback OPTIONS (table_name 'pagg_tab_p3');

ANALYZE pagg_tab;
ANALYZE fpagg_tab_p1;
ANALYZE fpagg_tab_p2;
ANALYZE fpagg_tab_p3;

-- When GROUP BY clause matches with PARTITION KEY.
--
-- 当 GROUP BY 子句与 PARTITION KEY 匹配时。
-- Plan with partitionwise aggregates is disabled
--
-- 禁用分区聚合计划
SET enable_partitionwise_aggregate TO false;
EXPLAIN (COSTS OFF)
SELECT a, sum(b), min(b), count(*) FROM pagg_tab GROUP BY a HAVING avg(b) < 22 ORDER BY 1;

-- Plan with partitionwise aggregates is enabled
--
-- 已启用分区聚合计划
SET enable_partitionwise_aggregate TO true;
EXPLAIN (COSTS OFF)
SELECT a, sum(b), min(b), count(*) FROM pagg_tab GROUP BY a HAVING avg(b) < 22 ORDER BY 1;
SELECT a, sum(b), min(b), count(*) FROM pagg_tab GROUP BY a HAVING avg(b) < 22 ORDER BY 1;

-- Check with whole-row reference
--
-- 检查整行参考
-- Should have all the columns in the target list for the given relation
--
-- 应具有给定关系的目标列表中的所有列
EXPLAIN (VERBOSE, COSTS OFF)
SELECT a, count(t1) FROM pagg_tab t1 GROUP BY a HAVING avg(b) < 22 ORDER BY 1;
SELECT a, count(t1) FROM pagg_tab t1 GROUP BY a HAVING avg(b) < 22 ORDER BY 1;

-- When GROUP BY clause does not match with PARTITION KEY.
--
-- 当 GROUP BY 子句与 PARTITION KEY 不匹配时。
EXPLAIN (COSTS OFF)
SELECT b, avg(a), max(a), count(*) FROM pagg_tab GROUP BY b HAVING sum(a) < 700 ORDER BY 1;

-- ===================================================================
-- access rights and superuser
--
-- 访问权限和超级用户
-- ===================================================================

-- Non-superuser cannot create a FDW without a password in the connstr
--
-- 如果 connstr 中没有密码，非超级用户无法创建 FDW
CREATE ROLE regress_nosuper NOSUPERUSER;

GRANT USAGE ON FOREIGN DATA WRAPPER postgres_fdw TO regress_nosuper;

SET ROLE regress_nosuper;

SHOW is_superuser;

-- This will be OK, we can create the FDW
--
-- 这样就可以了，我们可以创建 FDW
DO $d$
    BEGIN
        EXECUTE $$CREATE SERVER loopback_nopw FOREIGN DATA WRAPPER postgres_fdw
            OPTIONS (dbname '$$||current_database()||$$',
                     port '$$||current_setting('port')||$$'
            )$$;
    END;
$d$;

-- But creation of user mappings for non-superusers should fail
--
-- 但是为非超级用户创建用户映射应该会失败
CREATE USER MAPPING FOR public SERVER loopback_nopw;
CREATE USER MAPPING FOR CURRENT_USER SERVER loopback_nopw;

CREATE FOREIGN TABLE pg_temp.ft1_nopw (
	c1 int NOT NULL,
	c2 int NOT NULL,
	c3 text,
	c4 timestamptz,
	c5 timestamp,
	c6 varchar(10),
	c7 char(10) default 'ft1',
	c8 user_enum
) SERVER loopback_nopw OPTIONS (schema_name 'public', table_name 'ft1');

SELECT 1 FROM ft1_nopw LIMIT 1;

-- If we add a password to the connstr it'll fail, because we don't allow passwords
--
-- 如果我们向 connstr 添加密码，它将失败，因为我们不允许使用密码
-- in connstrs only in user mappings.
--
-- 仅在用户映射中的 connstrs 中。

ALTER SERVER loopback_nopw OPTIONS (ADD password 'dummypw');

-- If we add a password for our user mapping instead, we should get a different
--
-- 如果我们为用户映射添加密码，我们应该得到一个不同的
-- error because the password wasn't actually *used* when we run with trust auth.
--
-- 错误，因为当我们使用信任身份验证运行时，密码实际上并未“使用”。
--
-- This won't work with installcheck, but neither will most of the FDW checks.
--
-- 这不适用于 installcheck，但大多数 FDW 检查也不起作用。

ALTER USER MAPPING FOR CURRENT_USER SERVER loopback_nopw OPTIONS (ADD password 'dummypw');

SELECT 1 FROM ft1_nopw LIMIT 1;

-- Unpriv user cannot make the mapping passwordless
--
-- Unpriv 用户无法使映射无密码
ALTER USER MAPPING FOR CURRENT_USER SERVER loopback_nopw OPTIONS (ADD password_required 'false');


SELECT 1 FROM ft1_nopw LIMIT 1;

RESET ROLE;

-- But the superuser can
--
-- 但超级用户可以
ALTER USER MAPPING FOR regress_nosuper SERVER loopback_nopw OPTIONS (ADD password_required 'false');

SET ROLE regress_nosuper;

-- Should finally work now
--
-- 现在终于可以工作了
SELECT 1 FROM ft1_nopw LIMIT 1;

-- unpriv user also cannot set sslcert / sslkey on the user mapping
--
-- unpriv 用户也无法在用户映射上设置 sslcert / sslkey
-- first set password_required so we see the right error messages
--
-- 首先设置password_required，以便我们看到正确的错误消息
ALTER USER MAPPING FOR CURRENT_USER SERVER loopback_nopw OPTIONS (SET password_required 'true');
ALTER USER MAPPING FOR CURRENT_USER SERVER loopback_nopw OPTIONS (ADD sslcert 'foo.crt');
ALTER USER MAPPING FOR CURRENT_USER SERVER loopback_nopw OPTIONS (ADD sslkey 'foo.key');

-- We're done with the role named after a specific user and need to check the
--
-- 我们已经完成了以特定用户命名的角色，需要检查
-- changes to the public mapping.
--
-- 公共映射的更改。
DROP USER MAPPING FOR CURRENT_USER SERVER loopback_nopw;

-- This will fail again as it'll resolve the user mapping for public, which
--
-- 这将再次失败，因为它将解析公共用户映射，这
-- lacks password_required=false
--
-- 缺少password_required=false
SELECT 1 FROM ft1_nopw LIMIT 1;

RESET ROLE;

-- The user mapping for public is passwordless and lacks the password_required=false
--
-- public 的用户映射是无密码的并且缺少password_required=false
-- mapping option, but will work because the current user is a superuser.
--
-- 映射选项，但会起作用，因为当前用户是超级用户。
SELECT 1 FROM ft1_nopw LIMIT 1;

-- cleanup
DROP USER MAPPING FOR public SERVER loopback_nopw;
DROP OWNED BY regress_nosuper;
DROP ROLE regress_nosuper;

-- Clean-up
RESET enable_partitionwise_aggregate;

-- Two-phase transactions are not supported.
--
-- 不支持两阶段事务。
BEGIN;
SELECT count(*) FROM ft1;
-- error here
--
-- 此处出错
PREPARE TRANSACTION 'fdw_tpc';
ROLLBACK;

-- ===================================================================
-- reestablish new connection
--
-- 重新建立新连接
-- ===================================================================

-- Change application_name of remote connection to special one
--
-- 将远程连接的application_name修改为特殊的
-- so that we can easily terminate the connection later.
--
-- 以便我们稍后可以轻松终止连接。
ALTER SERVER loopback OPTIONS (application_name 'fdw_retry_check');

-- Make sure we have a remote connection.
--
-- 确保我们有远程连接。
SELECT 1 FROM ft1 LIMIT 1;

-- Terminate the remote connection and wait for the termination to complete.
--
-- 终止远程连接并等待终止完成。
-- (If a cache flush happens, the remote connection might have already been
--
-- （如果发生缓存刷新，远程连接可能已经被
-- dropped; so code this step in a way that doesn't fail if no connection.)
--
-- 掉落；因此，以一种即使没有连接也不会失败的方式编写此步骤。）
DO $$ BEGIN
PERFORM pg_terminate_backend(pid, 180000) FROM pg_stat_activity
	WHERE application_name = 'fdw_retry_check';
END $$;

-- This query should detect the broken connection when starting new remote
--
-- 此查询应在启动新远程时检测到断开的连接
-- transaction, reestablish new connection, and then succeed.
--
-- 事务，重新建立新连接，然后成功。
BEGIN;
SELECT 1 FROM ft1 LIMIT 1;

-- If we detect the broken connection when starting a new remote
--
-- 如果我们在启动新遥控器时检测到连接断开
-- subtransaction, we should fail instead of establishing a new connection.
--
-- 子事务，我们应该失败而不是建立新的连接。
-- Terminate the remote connection and wait for the termination to complete.
--
-- 终止远程连接并等待终止完成。
DO $$ BEGIN
PERFORM pg_terminate_backend(pid, 180000) FROM pg_stat_activity
	WHERE application_name = 'fdw_retry_check';
END $$;
SAVEPOINT s;
-- The text of the error might vary across platforms, so only show SQLSTATE.
--
-- 错误文本可能因平台而异，因此仅显示 SQLSTATE。
\set VERBOSITY sqlstate
SELECT 1 FROM ft1 LIMIT 1;    -- should fail
\set VERBOSITY default
COMMIT;

-- =============================================================================
-- test connection invalidation cases and postgres_fdw_get_connections function
--
-- 测试连接失效案例和 postgres_fdw_get_connections 函数
-- =============================================================================
-- Let's ensure to close all the existing cached connections.
--
-- 让我们确保关闭所有现有的缓存连接。
SELECT 1 FROM postgres_fdw_disconnect_all();
-- No cached connections, so no records should be output.
--
-- 没有缓存的连接，因此不应输出任何记录。
SELECT server_name FROM postgres_fdw_get_connections() ORDER BY 1;
-- This test case is for closing the connection in pgfdw_xact_callback
--
-- 该测试用例用于关闭 pgfdw_xact_callback 中的连接
BEGIN;
-- Connection xact depth becomes 1 i.e. the connection is in midst of the xact.
--
-- 连接 xact 深度变为 1，即连接位于 xact 中间。
SELECT 1 FROM ft1 LIMIT 1;
SELECT 1 FROM ft7 LIMIT 1;
-- List all the existing cached connections. loopback and loopback3 should be
--
-- 列出所有现有的缓存连接。 Loopback 和 Loopback3 应该是
-- output.
SELECT server_name FROM postgres_fdw_get_connections() ORDER BY 1;
-- Connections are not closed at the end of the alter and drop statements.
--
-- 在 alter 和 drop 语句结束时连接不会关闭。
-- That's because the connections are in midst of this xact,
--
-- 那是因为连接就在这个 xact 之中，
-- they are just marked as invalid in pgfdw_inval_callback.
--
-- 它们只是在 pgfdw_inval_callback 中被标记为无效。
ALTER SERVER loopback OPTIONS (ADD use_remote_estimate 'off');
DROP SERVER loopback3 CASCADE;
-- List all the existing cached connections. loopback and loopback3
--
-- 列出所有现有的缓存连接。环回和环回3
-- should be output as invalid connections. Also the server name and user name
--
-- 应输出为无效连接。还有服务器名称和用户名
-- for loopback3 should be NULL because both server and user mapping were
--
-- for Loopback3 应该为 NULL，因为服务器和用户映射都是
-- dropped. In this test, the PIDs of remote backends can be gathered from
--
-- 掉了。在这个测试中，远程后端的PID可以从
-- pg_stat_activity, and remote_backend_pid should match one of those PIDs.
--
-- pg_stat_activity 和remote_backend_pid 应与这些PID 之一匹配。
SELECT server_name, user_name = CURRENT_USER as "user_name = CURRENT_USER",
  valid, used_in_xact, closed,
  remote_backend_pid = ANY(SELECT pid FROM pg_stat_activity
  WHERE backend_type = 'client backend' AND pid <> pg_backend_pid())
  as remote_backend_pid
  FROM postgres_fdw_get_connections() ORDER BY 1;
-- The invalid connections get closed in pgfdw_xact_callback during commit.
--
-- 无效连接在提交期间在 pgfdw_xact_callback 中关闭。
COMMIT;
-- All cached connections were closed while committing above xact, so no
--
-- 在提交以上 xact 时，所有缓存的连接都已关闭，因此不会
-- records should be output.
--
-- 应输出记录。
SELECT server_name FROM postgres_fdw_get_connections() ORDER BY 1;

-- =======================================================================
-- test postgres_fdw_disconnect and postgres_fdw_disconnect_all functions
--
-- 测试 postgres_fdw_disconnect 和 postgres_fdw_disconnect_all 函数
-- =======================================================================
BEGIN;
-- Ensure to cache loopback connection.
--
-- 确保缓存环回连接。
SELECT 1 FROM ft1 LIMIT 1;
-- Ensure to cache loopback2 connection.
--
-- 确保缓存 Loopback2 连接。
SELECT 1 FROM ft6 LIMIT 1;
-- List all the existing cached connections. loopback and loopback2 should be
--
-- 列出所有现有的缓存连接。环回和环回2 应该是
-- output.
SELECT server_name FROM postgres_fdw_get_connections() ORDER BY 1;
-- Issue a warning and return false as loopback connection is still in use and
--
-- 发出警告并返回 false，因为环回连接仍在使用中并且
-- can not be closed.
--
-- 无法关闭。
SELECT postgres_fdw_disconnect('loopback');
-- List all the existing cached connections. loopback and loopback2 should be
--
-- 列出所有现有的缓存连接。环回和环回2 应该是
-- output.
SELECT server_name FROM postgres_fdw_get_connections() ORDER BY 1;
-- Return false as connections are still in use, warnings are issued.
--
-- 由于连接仍在使用中，因此返回 false，并发出警告。
-- But disable warnings temporarily because the order of them is not stable.
--
-- 但暂时禁用警告，因为它们的顺序不稳定。
SET client_min_messages = 'ERROR';
SELECT postgres_fdw_disconnect_all();
RESET client_min_messages;
COMMIT;
-- Ensure that loopback2 connection is closed.
--
-- 确保 Loopback2 连接已关闭。
SELECT 1 FROM postgres_fdw_disconnect('loopback2');
SELECT server_name FROM postgres_fdw_get_connections() WHERE server_name = 'loopback2';
-- Return false as loopback2 connection is closed already.
--
-- 返回 false，因为 Loopback2 连接已关闭。
SELECT postgres_fdw_disconnect('loopback2');
-- Return an error as there is no foreign server with given name.
--
-- 返回错误，因为没有给定名称的外部服务器。
SELECT postgres_fdw_disconnect('unknownserver');
-- Let's ensure to close all the existing cached connections.
--
-- 让我们确保关闭所有现有的缓存连接。
SELECT 1 FROM postgres_fdw_disconnect_all();
-- No cached connections, so no records should be output.
--
-- 没有缓存的连接，因此不应输出任何记录。
SELECT server_name FROM postgres_fdw_get_connections() ORDER BY 1;

-- =============================================================================
-- test case for having multiple cached connections for a foreign server
--
-- 外部服务器具有多个缓存连接的测试用例
-- =============================================================================
CREATE ROLE regress_multi_conn_user1 SUPERUSER;
CREATE ROLE regress_multi_conn_user2 SUPERUSER;
CREATE USER MAPPING FOR regress_multi_conn_user1 SERVER loopback;
CREATE USER MAPPING FOR regress_multi_conn_user2 SERVER loopback;

BEGIN;
-- Will cache loopback connection with user mapping for regress_multi_conn_user1
--
-- 将缓存带有 regress_multi_conn_user1 的用户映射的环回连接
SET ROLE regress_multi_conn_user1;
SELECT 1 FROM ft1 LIMIT 1;
RESET ROLE;

-- Will cache loopback connection with user mapping for regress_multi_conn_user2
--
-- 将缓存带有 regress_multi_conn_user2 用户映射的环回连接
SET ROLE regress_multi_conn_user2;
SELECT 1 FROM ft1 LIMIT 1;
RESET ROLE;

-- Should output two connections for loopback server
--
-- 应输出环回服务器的两个连接
SELECT server_name FROM postgres_fdw_get_connections() ORDER BY 1;
COMMIT;
-- Let's ensure to close all the existing cached connections.
--
-- 让我们确保关闭所有现有的缓存连接。
SELECT 1 FROM postgres_fdw_disconnect_all();
-- No cached connections, so no records should be output.
--
-- 没有缓存的连接，因此不应输出任何记录。
SELECT server_name FROM postgres_fdw_get_connections() ORDER BY 1;

-- Clean up
--
-- 清理
DROP USER MAPPING FOR regress_multi_conn_user1 SERVER loopback;
DROP USER MAPPING FOR regress_multi_conn_user2 SERVER loopback;
DROP ROLE regress_multi_conn_user1;
DROP ROLE regress_multi_conn_user2;

-- ===================================================================
-- Test foreign server level option keep_connections
--
-- 测试外部服务器级别选项 keep_connections
-- ===================================================================
-- By default, the connections associated with foreign server are cached i.e.
--
-- 默认情况下，与外部服务器关联的连接被缓存，即
-- keep_connections option is on. Set it to off.
--
-- keep_connections 选项已打开。将其设置为关闭。
ALTER SERVER loopback OPTIONS (keep_connections 'off');
-- connection to loopback server is closed at the end of xact
--
-- 到环回服务器的连接在 xact 结束时关闭
-- as keep_connections was set to off.
--
-- 因为 keep_connections 设置为关闭。
SELECT 1 FROM ft1 LIMIT 1;
-- No cached connections, so no records should be output.
--
-- 没有缓存的连接，因此不应输出任何记录。
SELECT server_name FROM postgres_fdw_get_connections() ORDER BY 1;
ALTER SERVER loopback OPTIONS (SET keep_connections 'on');

-- ===================================================================
-- batch insert
--
-- 批量插入
-- ===================================================================

BEGIN;

CREATE SERVER batch10 FOREIGN DATA WRAPPER postgres_fdw OPTIONS( batch_size '10' );

SELECT count(*)
FROM pg_foreign_server
WHERE srvname = 'batch10'
AND srvoptions @> array['batch_size=10'];

ALTER SERVER batch10 OPTIONS( SET batch_size '20' );

SELECT count(*)
FROM pg_foreign_server
WHERE srvname = 'batch10'
AND srvoptions @> array['batch_size=10'];

SELECT count(*)
FROM pg_foreign_server
WHERE srvname = 'batch10'
AND srvoptions @> array['batch_size=20'];

CREATE FOREIGN TABLE table30 ( x int ) SERVER batch10 OPTIONS ( batch_size '30' );

SELECT COUNT(*)
FROM pg_foreign_table
WHERE ftrelid = 'table30'::regclass
AND ftoptions @> array['batch_size=30'];

ALTER FOREIGN TABLE table30 OPTIONS ( SET batch_size '40');

SELECT COUNT(*)
FROM pg_foreign_table
WHERE ftrelid = 'table30'::regclass
AND ftoptions @> array['batch_size=30'];

SELECT COUNT(*)
FROM pg_foreign_table
WHERE ftrelid = 'table30'::regclass
AND ftoptions @> array['batch_size=40'];

ROLLBACK;

CREATE TABLE batch_table ( x int );

CREATE FOREIGN TABLE ftable ( x int ) SERVER loopback OPTIONS ( table_name 'batch_table', batch_size '10' );
EXPLAIN (VERBOSE, COSTS OFF) INSERT INTO ftable SELECT * FROM generate_series(1, 10) i;
INSERT INTO ftable SELECT * FROM generate_series(1, 10) i;
INSERT INTO ftable SELECT * FROM generate_series(11, 31) i;
INSERT INTO ftable VALUES (32);
INSERT INTO ftable VALUES (33), (34);
SELECT COUNT(*) FROM ftable;
TRUNCATE batch_table;
DROP FOREIGN TABLE ftable;

-- Disable batch insert
--
-- 禁用批量插入
CREATE FOREIGN TABLE ftable ( x int ) SERVER loopback OPTIONS ( table_name 'batch_table', batch_size '1' );
EXPLAIN (VERBOSE, COSTS OFF) INSERT INTO ftable VALUES (1), (2);
INSERT INTO ftable VALUES (1), (2);
SELECT COUNT(*) FROM ftable;

-- Disable batch inserting into foreign tables with BEFORE ROW INSERT triggers
--
-- 禁用使用 BEFORE ROW INSERT 触发器批量插入到外部表中
-- even if the batch_size option is enabled.
--
-- 即使启用了batch_size选项。
ALTER FOREIGN TABLE ftable OPTIONS ( SET batch_size '10' );
CREATE TRIGGER trig_row_before BEFORE INSERT ON ftable
FOR EACH ROW EXECUTE PROCEDURE trigger_data(23,'skidoo');
EXPLAIN (VERBOSE, COSTS OFF) INSERT INTO ftable VALUES (3), (4);
INSERT INTO ftable VALUES (3), (4);
SELECT COUNT(*) FROM ftable;

-- Clean up
--
-- 清理
DROP TRIGGER trig_row_before ON ftable;
DROP FOREIGN TABLE ftable;
DROP TABLE batch_table;

-- Use partitioning
--
-- 使用分区
CREATE TABLE batch_table ( x int ) PARTITION BY HASH (x);

CREATE TABLE batch_table_p0 (LIKE batch_table);
CREATE FOREIGN TABLE batch_table_p0f
	PARTITION OF batch_table
	FOR VALUES WITH (MODULUS 3, REMAINDER 0)
	SERVER loopback
	OPTIONS (table_name 'batch_table_p0', batch_size '10');

CREATE TABLE batch_table_p1 (LIKE batch_table);
CREATE FOREIGN TABLE batch_table_p1f
	PARTITION OF batch_table
	FOR VALUES WITH (MODULUS 3, REMAINDER 1)
	SERVER loopback
	OPTIONS (table_name 'batch_table_p1', batch_size '1');

CREATE TABLE batch_table_p2
	PARTITION OF batch_table
	FOR VALUES WITH (MODULUS 3, REMAINDER 2);

INSERT INTO batch_table SELECT * FROM generate_series(1, 66) i;
SELECT COUNT(*) FROM batch_table;

-- Clean up
--
-- 清理
DROP TABLE batch_table;
DROP TABLE batch_table_p0;
DROP TABLE batch_table_p1;

-- Check that batched mode also works for some inserts made during
--
-- 检查批处理模式是否也适用于在
-- cross-partition updates
--
-- 跨分区更新
CREATE TABLE batch_cp_upd_test (a int) PARTITION BY LIST (a);
CREATE TABLE batch_cp_upd_test1 (LIKE batch_cp_upd_test);
CREATE FOREIGN TABLE batch_cp_upd_test1_f
	PARTITION OF batch_cp_upd_test
	FOR VALUES IN (1)
	SERVER loopback
	OPTIONS (table_name 'batch_cp_upd_test1', batch_size '10');
CREATE TABLE batch_cp_upd_test2 PARTITION OF batch_cp_upd_test
	FOR VALUES IN (2);
CREATE TABLE batch_cp_upd_test3 (LIKE batch_cp_upd_test);
CREATE FOREIGN TABLE batch_cp_upd_test3_f
	PARTITION OF batch_cp_upd_test
	FOR VALUES IN (3)
	SERVER loopback
	OPTIONS (table_name 'batch_cp_upd_test3', batch_size '1');

-- Create statement triggers on remote tables that "log" any INSERTs
--
-- 在远程表上创建“记录”任何 INSERT 的语句触发器
-- performed on them.
--
-- 对他们进行的。
CREATE TABLE cmdlog (cmd text);
CREATE FUNCTION log_stmt() RETURNS TRIGGER LANGUAGE plpgsql AS $$
	BEGIN INSERT INTO public.cmdlog VALUES (TG_OP || ' on ' || TG_RELNAME); RETURN NULL; END;
$$;
CREATE TRIGGER stmt_trig AFTER INSERT ON batch_cp_upd_test1
	FOR EACH STATEMENT EXECUTE FUNCTION log_stmt();
CREATE TRIGGER stmt_trig AFTER INSERT ON batch_cp_upd_test3
	FOR EACH STATEMENT EXECUTE FUNCTION log_stmt();

-- This update moves rows from the local partition 'batch_cp_upd_test2' to the
--
-- 此更新将行从本地分区“batch_cp_upd_test2”移动到
-- foreign partition 'batch_cp_upd_test1', one that has insert batching
--
-- 外部分区“batch_cp_upd_test1”，具有插入批处理功能的分区
-- enabled, so a single INSERT for both rows.
--
-- 启用，因此对两行执行一次 INSERT。
INSERT INTO batch_cp_upd_test VALUES (2), (2);
UPDATE batch_cp_upd_test t SET a = 1 FROM (VALUES (1), (2)) s(a) WHERE t.a = s.a AND s.a = 2;

-- This one moves rows from the local partition 'batch_cp_upd_test2' to the
--
-- 此操作将行从本地分区“batch_cp_upd_test2”移动到
-- foreign partition 'batch_cp_upd_test2', one that has insert batching
--
-- 外部分区“batch_cp_upd_test2”，具有插入批处理功能的分区
-- disabled, so separate INSERTs for the two rows.
--
-- 禁用，因此将两行分开插入。
INSERT INTO batch_cp_upd_test VALUES (2), (2);
UPDATE batch_cp_upd_test t SET a = 3 FROM (VALUES (1), (2)) s(a) WHERE t.a = s.a AND s.a = 2;

SELECT tableoid::regclass, * FROM batch_cp_upd_test ORDER BY 1;

-- Should see 1 INSERT on batch_cp_upd_test1 and 2 on batch_cp_upd_test3 as
--
-- 应该在batch_cp_upd_test1上看到1个INSERT，在batch_cp_upd_test3上看到2个INSERT，如下所示
-- described above.
--
-- 如上所述。
SELECT * FROM cmdlog ORDER BY 1;

-- Clean up
--
-- 清理
DROP TABLE batch_cp_upd_test;
DROP TABLE batch_cp_upd_test1;
DROP TABLE batch_cp_upd_test3;
DROP TABLE cmdlog;
DROP FUNCTION log_stmt();

-- Use partitioning
--
-- 使用分区
ALTER SERVER loopback OPTIONS (ADD batch_size '10');

CREATE TABLE batch_table ( x int, field1 text, field2 text) PARTITION BY HASH (x);

CREATE TABLE batch_table_p0 (LIKE batch_table);
ALTER TABLE batch_table_p0 ADD CONSTRAINT p0_pkey PRIMARY KEY (x);
CREATE FOREIGN TABLE batch_table_p0f
	PARTITION OF batch_table
	FOR VALUES WITH (MODULUS 2, REMAINDER 0)
	SERVER loopback
	OPTIONS (table_name 'batch_table_p0');

CREATE TABLE batch_table_p1 (LIKE batch_table);
ALTER TABLE batch_table_p1 ADD CONSTRAINT p1_pkey PRIMARY KEY (x);
CREATE FOREIGN TABLE batch_table_p1f
	PARTITION OF batch_table
	FOR VALUES WITH (MODULUS 2, REMAINDER 1)
	SERVER loopback
	OPTIONS (table_name 'batch_table_p1');

INSERT INTO batch_table SELECT i, 'test'||i, 'test'|| i FROM generate_series(1, 50) i;
SELECT COUNT(*) FROM batch_table;
SELECT * FROM batch_table ORDER BY x;

-- Clean up
--
-- 清理
DROP TABLE batch_table;
DROP TABLE batch_table_p0;
DROP TABLE batch_table_p1;

ALTER SERVER loopback OPTIONS (DROP batch_size);

-- Test that pending inserts are handled properly when needed
--
-- 测试是否在需要时正确处理挂起的插入
CREATE TABLE batch_table (a text, b int);
CREATE FOREIGN TABLE ftable (a text, b int)
	SERVER loopback
	OPTIONS (table_name 'batch_table', batch_size '2');
CREATE TABLE ltable (a text, b int);
CREATE FUNCTION ftable_rowcount_trigf() RETURNS trigger LANGUAGE plpgsql AS
$$
begin
	raise notice '%: there are % rows in ftable',
		TG_NAME, (SELECT count(*) FROM ftable);
	if TG_OP = 'DELETE' then
		return OLD;
	else
		return NEW;
	end if;
end;
$$;
CREATE TRIGGER ftable_rowcount_trigger
BEFORE INSERT OR UPDATE OR DELETE ON ltable
FOR EACH ROW EXECUTE PROCEDURE ftable_rowcount_trigf();

WITH t AS (
	INSERT INTO ltable VALUES ('AAA', 42), ('BBB', 42) RETURNING *
)
INSERT INTO ftable SELECT * FROM t;

SELECT * FROM ltable;
SELECT * FROM ftable;
DELETE FROM ftable;

WITH t AS (
	UPDATE ltable SET b = b + 100 RETURNING *
)
INSERT INTO ftable SELECT * FROM t;

SELECT * FROM ltable;
SELECT * FROM ftable;
DELETE FROM ftable;

WITH t AS (
	DELETE FROM ltable RETURNING *
)
INSERT INTO ftable SELECT * FROM t;

SELECT * FROM ltable;
SELECT * FROM ftable;
DELETE FROM ftable;

-- Clean up
--
-- 清理
DROP FOREIGN TABLE ftable;
DROP TABLE batch_table;
DROP TRIGGER ftable_rowcount_trigger ON ltable;
DROP TABLE ltable;

CREATE TABLE parent (a text, b int) PARTITION BY LIST (a);
CREATE TABLE batch_table (a text, b int);
CREATE FOREIGN TABLE ftable
	PARTITION OF parent
	FOR VALUES IN ('AAA')
	SERVER loopback
	OPTIONS (table_name 'batch_table', batch_size '2');
CREATE TABLE ltable
	PARTITION OF parent
	FOR VALUES IN ('BBB');
CREATE TRIGGER ftable_rowcount_trigger
BEFORE INSERT ON ltable
FOR EACH ROW EXECUTE PROCEDURE ftable_rowcount_trigf();

INSERT INTO parent VALUES ('AAA', 42), ('BBB', 42), ('AAA', 42), ('BBB', 42);

SELECT tableoid::regclass, * FROM parent;

-- Clean up
--
-- 清理
DROP FOREIGN TABLE ftable;
DROP TABLE batch_table;
DROP TRIGGER ftable_rowcount_trigger ON ltable;
DROP TABLE ltable;
DROP TABLE parent;
DROP FUNCTION ftable_rowcount_trigf;

-- ===================================================================
-- test asynchronous execution
--
-- 测试异步执行
-- ===================================================================

ALTER SERVER loopback OPTIONS (DROP extensions);
ALTER SERVER loopback OPTIONS (ADD async_capable 'true');
ALTER SERVER loopback2 OPTIONS (ADD async_capable 'true');

CREATE TABLE async_pt (a int, b int, c text) PARTITION BY RANGE (a);
CREATE TABLE base_tbl1 (a int, b int, c text);
CREATE TABLE base_tbl2 (a int, b int, c text);
CREATE FOREIGN TABLE async_p1 PARTITION OF async_pt FOR VALUES FROM (1000) TO (2000)
  SERVER loopback OPTIONS (table_name 'base_tbl1');
CREATE FOREIGN TABLE async_p2 PARTITION OF async_pt FOR VALUES FROM (2000) TO (3000)
  SERVER loopback2 OPTIONS (table_name 'base_tbl2');
INSERT INTO async_p1 SELECT 1000 + i, i, to_char(i, 'FM0000') FROM generate_series(0, 999, 5) i;
INSERT INTO async_p2 SELECT 2000 + i, i, to_char(i, 'FM0000') FROM generate_series(0, 999, 5) i;
ANALYZE async_pt;

-- simple queries
--
-- 简单查询
CREATE TABLE result_tbl (a int, b int, c text);

EXPLAIN (VERBOSE, COSTS OFF)
INSERT INTO result_tbl SELECT * FROM async_pt WHERE b % 100 = 0;
INSERT INTO result_tbl SELECT * FROM async_pt WHERE b % 100 = 0;

SELECT * FROM result_tbl ORDER BY a;
DELETE FROM result_tbl;

EXPLAIN (VERBOSE, COSTS OFF)
INSERT INTO result_tbl SELECT * FROM async_pt WHERE b === 505;
INSERT INTO result_tbl SELECT * FROM async_pt WHERE b === 505;

SELECT * FROM result_tbl ORDER BY a;
DELETE FROM result_tbl;

EXPLAIN (VERBOSE, COSTS OFF)
INSERT INTO result_tbl SELECT a, b, 'AAA' || c FROM async_pt WHERE b === 505;
INSERT INTO result_tbl SELECT a, b, 'AAA' || c FROM async_pt WHERE b === 505;

SELECT * FROM result_tbl ORDER BY a;
DELETE FROM result_tbl;

-- Test error handling, if accessing one of the foreign partitions errors out
--
-- 测试错误处理，如果访问外部分区之一出错
CREATE FOREIGN TABLE async_p_broken PARTITION OF async_pt FOR VALUES FROM (10000) TO (10001)
  SERVER loopback OPTIONS (table_name 'non_existent_table');
SELECT * FROM async_pt;
DROP FOREIGN TABLE async_p_broken;

-- Check case where multiple partitions use the same connection
--
-- 检查多个分区使用同一连接的情况
CREATE TABLE base_tbl3 (a int, b int, c text);
CREATE FOREIGN TABLE async_p3 PARTITION OF async_pt FOR VALUES FROM (3000) TO (4000)
  SERVER loopback2 OPTIONS (table_name 'base_tbl3');
INSERT INTO async_p3 SELECT 3000 + i, i, to_char(i, 'FM0000') FROM generate_series(0, 999, 5) i;
ANALYZE async_pt;

EXPLAIN (VERBOSE, COSTS OFF)
INSERT INTO result_tbl SELECT * FROM async_pt WHERE b === 505;
INSERT INTO result_tbl SELECT * FROM async_pt WHERE b === 505;

SELECT * FROM result_tbl ORDER BY a;
DELETE FROM result_tbl;

DROP FOREIGN TABLE async_p3;
DROP TABLE base_tbl3;

-- Check case where the partitioned table has local/remote partitions
--
-- 检查分区表有本地/远程分区的情况
CREATE TABLE async_p3 PARTITION OF async_pt FOR VALUES FROM (3000) TO (4000);
INSERT INTO async_p3 SELECT 3000 + i, i, to_char(i, 'FM0000') FROM generate_series(0, 999, 5) i;
ANALYZE async_pt;

EXPLAIN (VERBOSE, COSTS OFF)
INSERT INTO result_tbl SELECT * FROM async_pt WHERE b === 505;
INSERT INTO result_tbl SELECT * FROM async_pt WHERE b === 505;

SELECT * FROM result_tbl ORDER BY a;
DELETE FROM result_tbl;

-- partitionwise joins
--
-- 分区连接
SET enable_partitionwise_join TO true;

CREATE TABLE join_tbl (a1 int, b1 int, c1 text, a2 int, b2 int, c2 text);

EXPLAIN (VERBOSE, COSTS OFF)
INSERT INTO join_tbl SELECT * FROM async_pt t1, async_pt t2 WHERE t1.a = t2.a AND t1.b = t2.b AND t1.b % 100 = 0;
INSERT INTO join_tbl SELECT * FROM async_pt t1, async_pt t2 WHERE t1.a = t2.a AND t1.b = t2.b AND t1.b % 100 = 0;

SELECT * FROM join_tbl ORDER BY a1;
DELETE FROM join_tbl;

EXPLAIN (VERBOSE, COSTS OFF)
INSERT INTO join_tbl SELECT t1.a, t1.b, 'AAA' || t1.c, t2.a, t2.b, 'AAA' || t2.c FROM async_pt t1, async_pt t2 WHERE t1.a = t2.a AND t1.b = t2.b AND t1.b % 100 = 0;
INSERT INTO join_tbl SELECT t1.a, t1.b, 'AAA' || t1.c, t2.a, t2.b, 'AAA' || t2.c FROM async_pt t1, async_pt t2 WHERE t1.a = t2.a AND t1.b = t2.b AND t1.b % 100 = 0;

SELECT * FROM join_tbl ORDER BY a1;
DELETE FROM join_tbl;

RESET enable_partitionwise_join;

-- Test rescan of an async Append node with do_exec_prune=false
--
-- 使用 do_exec_prune=false 测试异步追加节点的重新扫描
SET enable_hashjoin TO false;

EXPLAIN (VERBOSE, COSTS OFF)
INSERT INTO join_tbl SELECT * FROM async_p1 t1, async_pt t2 WHERE t1.a = t2.a AND t1.b = t2.b AND t1.b % 100 = 0;
INSERT INTO join_tbl SELECT * FROM async_p1 t1, async_pt t2 WHERE t1.a = t2.a AND t1.b = t2.b AND t1.b % 100 = 0;

SELECT * FROM join_tbl ORDER BY a1;
DELETE FROM join_tbl;

RESET enable_hashjoin;

-- Test interaction of async execution with plan-time partition pruning
--
-- 测试异步执行与计划时间分区修剪的交互
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM async_pt WHERE a < 3000;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM async_pt WHERE a < 2000;

-- Test interaction of async execution with run-time partition pruning
--
-- 测试异步执行与运行时分区修剪的交互
SET plan_cache_mode TO force_generic_plan;

PREPARE async_pt_query (int, int) AS
  INSERT INTO result_tbl SELECT * FROM async_pt WHERE a < $1 AND b === $2;

EXPLAIN (VERBOSE, COSTS OFF)
EXECUTE async_pt_query (3000, 505);
EXECUTE async_pt_query (3000, 505);

SELECT * FROM result_tbl ORDER BY a;
DELETE FROM result_tbl;

EXPLAIN (VERBOSE, COSTS OFF)
EXECUTE async_pt_query (2000, 505);
EXECUTE async_pt_query (2000, 505);

SELECT * FROM result_tbl ORDER BY a;
DELETE FROM result_tbl;

RESET plan_cache_mode;

CREATE TABLE local_tbl(a int, b int, c text);
INSERT INTO local_tbl VALUES (1505, 505, 'foo'), (2505, 505, 'bar');
ANALYZE local_tbl;

CREATE INDEX base_tbl1_idx ON base_tbl1 (a);
CREATE INDEX base_tbl2_idx ON base_tbl2 (a);
CREATE INDEX async_p3_idx ON async_p3 (a);
ANALYZE base_tbl1;
ANALYZE base_tbl2;
ANALYZE async_p3;

ALTER FOREIGN TABLE async_p1 OPTIONS (use_remote_estimate 'true');
ALTER FOREIGN TABLE async_p2 OPTIONS (use_remote_estimate 'true');

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM local_tbl, async_pt WHERE local_tbl.a = async_pt.a AND local_tbl.c = 'bar';
EXPLAIN (ANALYZE, COSTS OFF, SUMMARY OFF, TIMING OFF, BUFFERS OFF)
SELECT * FROM local_tbl, async_pt WHERE local_tbl.a = async_pt.a AND local_tbl.c = 'bar';
SELECT * FROM local_tbl, async_pt WHERE local_tbl.a = async_pt.a AND local_tbl.c = 'bar';

ALTER FOREIGN TABLE async_p1 OPTIONS (DROP use_remote_estimate);
ALTER FOREIGN TABLE async_p2 OPTIONS (DROP use_remote_estimate);

DROP TABLE local_tbl;
DROP INDEX base_tbl1_idx;
DROP INDEX base_tbl2_idx;
DROP INDEX async_p3_idx;

-- UNION queries
--
-- 联合查询
SET enable_sort TO off;
SET enable_incremental_sort TO off;
-- Adjust fdw_startup_cost so that we get an unordered path in the Append.
--
-- 调整fdw_startup_cost，以便我们在Append中获得无序路径。
ALTER SERVER loopback2 OPTIONS (ADD fdw_startup_cost '0.00');

EXPLAIN (VERBOSE, COSTS OFF)
INSERT INTO result_tbl
(SELECT a, b, 'AAA' || c FROM async_p1 ORDER BY a LIMIT 10)
UNION
(SELECT a, b, 'AAA' || c FROM async_p2 WHERE b < 10);
INSERT INTO result_tbl
(SELECT a, b, 'AAA' || c FROM async_p1 ORDER BY a LIMIT 10)
UNION
(SELECT a, b, 'AAA' || c FROM async_p2 WHERE b < 10);

SELECT * FROM result_tbl ORDER BY a;
DELETE FROM result_tbl;

EXPLAIN (VERBOSE, COSTS OFF)
INSERT INTO result_tbl
(SELECT a, b, 'AAA' || c FROM async_p1 ORDER BY a LIMIT 10)
UNION ALL
(SELECT a, b, 'AAA' || c FROM async_p2 WHERE b < 10);
INSERT INTO result_tbl
(SELECT a, b, 'AAA' || c FROM async_p1 ORDER BY a LIMIT 10)
UNION ALL
(SELECT a, b, 'AAA' || c FROM async_p2 WHERE b < 10);

SELECT * FROM result_tbl ORDER BY a;
DELETE FROM result_tbl;

RESET enable_incremental_sort;
RESET enable_sort;
ALTER SERVER loopback2 OPTIONS (DROP fdw_startup_cost);

-- Disable async execution if we use gating Result nodes for pseudoconstant
--
-- 如果我们使用伪常量的门控结果节点，则禁用异步执行
-- quals
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM async_pt WHERE CURRENT_USER = SESSION_USER;

EXPLAIN (VERBOSE, COSTS OFF)
(SELECT * FROM async_p1 WHERE CURRENT_USER = SESSION_USER)
UNION ALL
(SELECT * FROM async_p2 WHERE CURRENT_USER = SESSION_USER);

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM ((SELECT * FROM async_p1 WHERE b < 10) UNION ALL (SELECT * FROM async_p2 WHERE b < 10)) s WHERE CURRENT_USER = SESSION_USER;

-- Test that pending requests are processed properly
--
-- 测试挂起的请求是否得到正确处理
SET enable_mergejoin TO false;
SET enable_hashjoin TO false;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM async_pt t1, async_p2 t2 WHERE t1.a = t2.a AND t1.b === 505;
SELECT * FROM async_pt t1, async_p2 t2 WHERE t1.a = t2.a AND t1.b === 505;

CREATE TABLE local_tbl (a int, b int, c text);
INSERT INTO local_tbl VALUES (1505, 505, 'foo');
ANALYZE local_tbl;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM local_tbl t1 LEFT JOIN (SELECT *, (SELECT count(*) FROM async_pt WHERE a < 3000) FROM async_pt WHERE a < 3000) t2 ON t1.a = t2.a;
EXPLAIN (ANALYZE, COSTS OFF, SUMMARY OFF, TIMING OFF, BUFFERS OFF)
SELECT * FROM local_tbl t1 LEFT JOIN (SELECT *, (SELECT count(*) FROM async_pt WHERE a < 3000) FROM async_pt WHERE a < 3000) t2 ON t1.a = t2.a;
SELECT * FROM local_tbl t1 LEFT JOIN (SELECT *, (SELECT count(*) FROM async_pt WHERE a < 3000) FROM async_pt WHERE a < 3000) t2 ON t1.a = t2.a;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM async_pt t1 WHERE t1.b === 505 LIMIT 1;
EXPLAIN (ANALYZE, COSTS OFF, SUMMARY OFF, TIMING OFF, BUFFERS OFF)
SELECT * FROM async_pt t1 WHERE t1.b === 505 LIMIT 1;
SELECT * FROM async_pt t1 WHERE t1.b === 505 LIMIT 1;

-- Check with foreign modify
--
-- 与国外修改检查
CREATE TABLE base_tbl3 (a int, b int, c text);
CREATE FOREIGN TABLE remote_tbl (a int, b int, c text)
  SERVER loopback OPTIONS (table_name 'base_tbl3');
INSERT INTO remote_tbl VALUES (2505, 505, 'bar');

CREATE TABLE base_tbl4 (a int, b int, c text);
CREATE FOREIGN TABLE insert_tbl (a int, b int, c text)
  SERVER loopback OPTIONS (table_name 'base_tbl4');

EXPLAIN (VERBOSE, COSTS OFF)
INSERT INTO insert_tbl (SELECT * FROM local_tbl UNION ALL SELECT * FROM remote_tbl);
INSERT INTO insert_tbl (SELECT * FROM local_tbl UNION ALL SELECT * FROM remote_tbl);

SELECT * FROM insert_tbl ORDER BY a;

-- Check with direct modify
--
-- 直接修改检查
EXPLAIN (VERBOSE, COSTS OFF)
WITH t AS (UPDATE remote_tbl SET c = c || c RETURNING *)
INSERT INTO join_tbl SELECT * FROM async_pt LEFT JOIN t ON (async_pt.a = t.a AND async_pt.b = t.b) WHERE async_pt.b === 505;
WITH t AS (UPDATE remote_tbl SET c = c || c RETURNING *)
INSERT INTO join_tbl SELECT * FROM async_pt LEFT JOIN t ON (async_pt.a = t.a AND async_pt.b = t.b) WHERE async_pt.b === 505;

SELECT * FROM join_tbl ORDER BY a1;
DELETE FROM join_tbl;

DROP TABLE local_tbl;
DROP FOREIGN TABLE remote_tbl;
DROP FOREIGN TABLE insert_tbl;
DROP TABLE base_tbl3;
DROP TABLE base_tbl4;

RESET enable_mergejoin;
RESET enable_hashjoin;

-- Test that UPDATE/DELETE with inherited target works with async_capable enabled
--
-- 测试继承目标的 UPDATE/DELETE 在启用 async_capable 的情况下是否有效
EXPLAIN (VERBOSE, COSTS OFF)
UPDATE async_pt SET c = c || c WHERE b = 0 RETURNING *;
UPDATE async_pt SET c = c || c WHERE b = 0 RETURNING *;
EXPLAIN (VERBOSE, COSTS OFF)
DELETE FROM async_pt WHERE b = 0 RETURNING *;
DELETE FROM async_pt WHERE b = 0 RETURNING *;

-- Check EXPLAIN ANALYZE for a query that scans empty partitions asynchronously
--
-- 检查 EXPLAIN ANALYZE 以获取异步扫描空分区的查询
DELETE FROM async_p1;
DELETE FROM async_p2;
DELETE FROM async_p3;

EXPLAIN (ANALYZE, COSTS OFF, SUMMARY OFF, TIMING OFF, BUFFERS OFF)
SELECT * FROM async_pt;

-- Clean up
--
-- 清理
DROP TABLE async_pt;
DROP TABLE base_tbl1;
DROP TABLE base_tbl2;
DROP TABLE result_tbl;
DROP TABLE join_tbl;

-- Test that an asynchronous fetch is processed before restarting the scan in
--
-- 在重新启动扫描之前测试异步获取是否已处理
-- ReScanForeignScan
CREATE TABLE base_tbl (a int, b int);
INSERT INTO base_tbl VALUES (1, 11), (2, 22), (3, 33);
CREATE FOREIGN TABLE foreign_tbl (b int)
  SERVER loopback OPTIONS (table_name 'base_tbl');
CREATE FOREIGN TABLE foreign_tbl2 () INHERITS (foreign_tbl)
  SERVER loopback OPTIONS (table_name 'base_tbl');

EXPLAIN (VERBOSE, COSTS OFF)
SELECT a FROM base_tbl WHERE (a, random() > 0) IN (SELECT a, random() > 0 FROM foreign_tbl);
SELECT a FROM base_tbl WHERE (a, random() > 0) IN (SELECT a, random() > 0 FROM foreign_tbl);

-- Clean up
--
-- 清理
DROP FOREIGN TABLE foreign_tbl CASCADE;
DROP TABLE base_tbl;

ALTER SERVER loopback OPTIONS (DROP async_capable);
ALTER SERVER loopback2 OPTIONS (DROP async_capable);

-- ===================================================================
-- test invalid server, foreign table and foreign data wrapper options
--
-- 测试无效的服务器、外部表和外部数据包装器选项
-- ===================================================================
-- Invalid fdw_startup_cost option
--
-- fdw_startup_cost 选项无效
CREATE SERVER inv_scst FOREIGN DATA WRAPPER postgres_fdw
	OPTIONS(fdw_startup_cost '100$%$#$#');
-- Invalid fdw_tuple_cost option
--
-- 无效的 fdw_tuple_cost 选项
CREATE SERVER inv_scst FOREIGN DATA WRAPPER postgres_fdw
	OPTIONS(fdw_tuple_cost '100$%$#$#');
-- Invalid fetch_size option
--
-- fetch_size 选项无效
CREATE FOREIGN TABLE inv_fsz (c1 int )
	SERVER loopback OPTIONS (fetch_size '100$%$#$#');
-- Invalid batch_size option
--
-- 无效的batch_size选项
CREATE FOREIGN TABLE inv_bsz (c1 int )
	SERVER loopback OPTIONS (batch_size '100$%$#$#');

-- No option is allowed to be specified at foreign data wrapper level
--
-- 不允许在外部数据包装级别指定任何选项
ALTER FOREIGN DATA WRAPPER postgres_fdw OPTIONS (nonexistent 'fdw');

-- ===================================================================
-- test postgres_fdw.application_name GUC
--
-- 测试 postgres_fdw.application_name GUC
-- ===================================================================
-- To avoid race conditions in checking the remote session's application_name,
--
-- 为了避免检查远程会话的 application_name 时出现竞争条件，
-- use this view to make the remote session itself read its application_name.
--
-- 使用此视图使远程会话本身读取其 application_name。
CREATE VIEW my_application_name AS
  SELECT application_name FROM pg_stat_activity WHERE pid = pg_backend_pid();

CREATE FOREIGN TABLE remote_application_name (application_name text)
  SERVER loopback2
  OPTIONS (schema_name 'public', table_name 'my_application_name');

SELECT count(*) FROM remote_application_name;

-- Specify escape sequences in application_name option of a server
--
-- 在服务器的 application_name 选项中指定转义序列
-- object so as to test that they are replaced with status information
--
-- 对象以测试它们是否被状态信息替换
-- expectedly.  Note that we are also relying on ALTER SERVER to force
--
-- 预计。  请注意，我们还依赖 ALTER SERVER 来强制
-- the remote session to be restarted with its new application name.
--
-- 要使用新应用程序名称重新启动的远程会话。
--
-- Since pg_stat_activity.application_name may be truncated to less than
--
-- 由于 pg_stat_activity.application_name 可能会被截断为小于
-- NAMEDATALEN characters, note that substring() needs to be used
--
-- NAMEDATALEN字符，注意需要使用substring()
-- at the condition of test query to make sure that the string consisting
--
-- 在测试查询的条件下确保字符串包含
-- of database name and process ID is also less than that.
--
-- 数据库名称和进程 ID 也少于此。
ALTER SERVER loopback2 OPTIONS (application_name 'fdw_%d%p');
SELECT count(*) FROM remote_application_name
  WHERE application_name =
    substring('fdw_' || current_database() || pg_backend_pid() for
      current_setting('max_identifier_length')::int);

-- postgres_fdw.application_name overrides application_name option
--
-- postgres_fdw.application_name 覆盖 application_name 选项
-- of a server object if both settings are present.
--
-- 如果两个设置都存在，则为服务器对象。
ALTER SERVER loopback2 OPTIONS (SET application_name 'fdw_wrong');
SET postgres_fdw.application_name TO 'fdw_%a%u%%';
SELECT count(*) FROM remote_application_name
  WHERE application_name =
    substring('fdw_' || current_setting('application_name') ||
      CURRENT_USER || '%' for current_setting('max_identifier_length')::int);
RESET postgres_fdw.application_name;

-- Test %c (session ID) and %C (cluster name) escape sequences.
--
-- 测试 %c（会话 ID）和 %C（集群名称）转义序列。
ALTER SERVER loopback2 OPTIONS (SET application_name 'fdw_%C%c');
SELECT count(*) FROM remote_application_name
  WHERE application_name =
    substring('fdw_' || current_setting('cluster_name') ||
      to_hex(trunc(EXTRACT(EPOCH FROM (SELECT backend_start FROM
      pg_stat_get_activity(pg_backend_pid()))))::integer) || '.' ||
      to_hex(pg_backend_pid())
      for current_setting('max_identifier_length')::int);

-- Clean up.
--
-- 清理。
DROP FOREIGN TABLE remote_application_name;
DROP VIEW my_application_name;

-- ===================================================================
-- test parallel commit and parallel abort
--
-- 测试并行提交和并行中止
-- ===================================================================
ALTER SERVER loopback OPTIONS (ADD parallel_commit 'true');
ALTER SERVER loopback OPTIONS (ADD parallel_abort 'true');
ALTER SERVER loopback2 OPTIONS (ADD parallel_commit 'true');
ALTER SERVER loopback2 OPTIONS (ADD parallel_abort 'true');

CREATE TABLE ploc1 (f1 int, f2 text);
CREATE FOREIGN TABLE prem1 (f1 int, f2 text)
  SERVER loopback OPTIONS (table_name 'ploc1');
CREATE TABLE ploc2 (f1 int, f2 text);
CREATE FOREIGN TABLE prem2 (f1 int, f2 text)
  SERVER loopback2 OPTIONS (table_name 'ploc2');

BEGIN;
INSERT INTO prem1 VALUES (101, 'foo');
INSERT INTO prem2 VALUES (201, 'bar');
COMMIT;
SELECT * FROM prem1;
SELECT * FROM prem2;

BEGIN;
SAVEPOINT s;
INSERT INTO prem1 VALUES (102, 'foofoo');
INSERT INTO prem2 VALUES (202, 'barbar');
RELEASE SAVEPOINT s;
COMMIT;
SELECT * FROM prem1;
SELECT * FROM prem2;

-- This tests executing DEALLOCATE ALL against foreign servers in parallel
--
-- 此测试并行对外部服务器执行 DEALLOCATE ALL
-- during pre-commit
--
-- 在预提交期间
BEGIN;
SAVEPOINT s;
INSERT INTO prem1 VALUES (103, 'baz');
INSERT INTO prem2 VALUES (203, 'qux');
ROLLBACK TO SAVEPOINT s;
RELEASE SAVEPOINT s;
INSERT INTO prem1 VALUES (104, 'bazbaz');
INSERT INTO prem2 VALUES (204, 'quxqux');
COMMIT;
SELECT * FROM prem1;
SELECT * FROM prem2;

BEGIN;
INSERT INTO prem1 VALUES (105, 'test1');
INSERT INTO prem2 VALUES (205, 'test2');
ABORT;
SELECT * FROM prem1;
SELECT * FROM prem2;

-- This tests executing DEALLOCATE ALL against foreign servers in parallel
--
-- 此测试并行对外部服务器执行 DEALLOCATE ALL
-- during post-abort
--
-- 流产后期间
BEGIN;
SAVEPOINT s;
INSERT INTO prem1 VALUES (105, 'test1');
INSERT INTO prem2 VALUES (205, 'test2');
ROLLBACK TO SAVEPOINT s;
RELEASE SAVEPOINT s;
INSERT INTO prem1 VALUES (105, 'test1');
INSERT INTO prem2 VALUES (205, 'test2');
ABORT;
SELECT * FROM prem1;
SELECT * FROM prem2;

ALTER SERVER loopback OPTIONS (DROP parallel_commit);
ALTER SERVER loopback OPTIONS (DROP parallel_abort);
ALTER SERVER loopback2 OPTIONS (DROP parallel_commit);
ALTER SERVER loopback2 OPTIONS (DROP parallel_abort);

-- ===================================================================
-- test for ANALYZE sampling
--
-- 分析采样测试
-- ===================================================================

CREATE TABLE analyze_table (id int, a text, b bigint);

CREATE FOREIGN TABLE analyze_ftable (id int, a text, b bigint)
       SERVER loopback OPTIONS (table_name 'analyze_table');

INSERT INTO analyze_table (SELECT x FROM generate_series(1,1000) x);
ANALYZE analyze_table;

SET default_statistics_target = 10;
ANALYZE analyze_table;

ALTER SERVER loopback OPTIONS (analyze_sampling 'invalid');

ALTER SERVER loopback OPTIONS (analyze_sampling 'auto');
ANALYZE analyze_ftable;

ALTER SERVER loopback OPTIONS (SET analyze_sampling 'system');
ANALYZE analyze_ftable;

ALTER SERVER loopback OPTIONS (SET analyze_sampling 'bernoulli');
ANALYZE analyze_ftable;

ALTER SERVER loopback OPTIONS (SET analyze_sampling 'random');
ANALYZE analyze_ftable;

ALTER SERVER loopback OPTIONS (SET analyze_sampling 'off');
ANALYZE analyze_ftable;

-- cleanup
DROP FOREIGN TABLE analyze_ftable;
DROP TABLE analyze_table;

-- ===================================================================
-- test for postgres_fdw_get_connections function with check_conn = true
--
-- 使用 check_conn = true 测试 postgres_fdw_get_connections 函数
-- ===================================================================

-- Disable debug_discard_caches in order to manage remote connections
--
-- 禁用 debug_discard_caches 以管理远程连接
SET debug_discard_caches TO '0';

-- The text of the error might vary across platforms, so only show SQLSTATE.
--
-- 错误文本可能因平台而异，因此仅显示 SQLSTATE。
\set VERBOSITY sqlstate

SELECT 1 FROM postgres_fdw_disconnect_all();
ALTER SERVER loopback OPTIONS (SET application_name 'fdw_conn_check');
SELECT 1 FROM ft1 LIMIT 1;

-- Since the remote server is still connected, "closed" should be FALSE,
--
-- 由于远程服务器仍处于连接状态，因此“关闭”应该为 FALSE，
-- or NULL if the connection status check is not available.
--
-- 如果连接状态检查不可用，则为 NULL。
-- In this test, the remote backend handling this connection should have
--
-- 在此测试中，处理此连接的远程后端应该具有
-- application_name set to "fdw_conn_check", so remote_backend_pid should
--
-- application_name 设置为“fdw_conn_check”，因此remote_backend_pid 应该
-- match the PID from the pg_stat_activity entry with that application_name.
--
-- 将 pg_stat_activity 条目中的 PID 与该 application_name 进行匹配。
SELECT server_name,
  CASE WHEN closed IS NOT true THEN false ELSE true END AS closed,
  remote_backend_pid = (SELECT pid FROM pg_stat_activity
  WHERE application_name = 'fdw_conn_check') AS remote_backend_pid
  FROM postgres_fdw_get_connections(true);

-- After terminating the remote backend, since the connection is closed,
--
-- 终止远程后端后，由于连接已关闭，
-- "closed" should be TRUE, or NULL if the connection status check
--
-- “close”应该为 TRUE，如果连接状态检查则为 NULL
-- is not available. Despite the termination, remote_backend_pid should
--
-- 不可用。尽管终止，remote_backend_pid 应该
-- still show the non-zero PID of the terminated remote backend.
--
-- 仍然显示已终止的远程后端的非零 PID。
DO $$ BEGIN
PERFORM pg_terminate_backend(pid, 180000) FROM pg_stat_activity
  WHERE application_name = 'fdw_conn_check';
END $$;
SELECT server_name,
  CASE WHEN closed IS NOT false THEN true ELSE false END AS closed,
  remote_backend_pid <> 0 AS remote_backend_pid
  FROM postgres_fdw_get_connections(true);

-- Clean up
--
-- 清理
\set VERBOSITY default
RESET debug_discard_caches;

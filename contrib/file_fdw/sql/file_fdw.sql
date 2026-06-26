--
-- Test foreign-data wrapper file_fdw.
--
-- 测试外部数据包装器 file_fdw。
--

-- directory paths are passed to us in environment variables
--
-- 目录路径在环境变量中传递给我们
\getenv abs_srcdir PG_ABS_SRCDIR

-- Clean up in case a prior regression run failed
--
-- 清理以防先前的回归运行失败
SET client_min_messages TO 'warning';
DROP ROLE IF EXISTS regress_file_fdw_superuser, regress_file_fdw_user, regress_no_priv_user;
RESET client_min_messages;

CREATE ROLE regress_file_fdw_superuser LOGIN SUPERUSER; -- is a superuser
CREATE ROLE regress_file_fdw_user LOGIN;                -- has priv and user mapping
CREATE ROLE regress_no_priv_user LOGIN;                 -- has priv but no user mapping

-- Install file_fdw
--
-- 安装file_fdw
CREATE EXTENSION file_fdw;

-- create function to filter unstable results of EXPLAIN
--
-- 创建函数来过滤 EXPLAIN 的不稳定结果
CREATE FUNCTION explain_filter(text) RETURNS setof text
LANGUAGE plpgsql AS
$$
declare
    ln text;
begin
    for ln in execute $1
    loop
        -- Remove the path portion of foreign file names
        --
        -- 删除外部文件名的路径部分
        ln := regexp_replace(ln, 'Foreign File: .*/([a-z.]+)$', 'Foreign File: .../\1');
        return next ln;
    end loop;
end;
$$;

-- regress_file_fdw_superuser owns fdw-related objects
--
-- regress_file_fdw_superuser 拥有 fdw 相关对象
SET ROLE regress_file_fdw_superuser;
CREATE SERVER file_server FOREIGN DATA WRAPPER file_fdw;

-- privilege tests
--
-- 特权测试
SET ROLE regress_file_fdw_user;
CREATE FOREIGN DATA WRAPPER file_fdw2 HANDLER file_fdw_handler VALIDATOR file_fdw_validator;   -- ERROR
CREATE SERVER file_server2 FOREIGN DATA WRAPPER file_fdw;   -- ERROR
CREATE USER MAPPING FOR regress_file_fdw_user SERVER file_server;   -- ERROR

SET ROLE regress_file_fdw_superuser;
GRANT USAGE ON FOREIGN SERVER file_server TO regress_file_fdw_user;

SET ROLE regress_file_fdw_user;
CREATE USER MAPPING FOR regress_file_fdw_user SERVER file_server;

-- create user mappings and grant privilege to test users
--
-- 创建用户映射并授予测试用户权限
SET ROLE regress_file_fdw_superuser;
CREATE USER MAPPING FOR regress_file_fdw_superuser SERVER file_server;
CREATE USER MAPPING FOR regress_no_priv_user SERVER file_server;

-- validator tests
--
-- 验证器测试
CREATE FOREIGN TABLE tbl () SERVER file_server OPTIONS (foo 'bar');  -- ERROR
CREATE FOREIGN TABLE tbl () SERVER file_server OPTIONS ("a=b" 'true');  -- ERROR
CREATE FOREIGN TABLE tbl () SERVER file_server OPTIONS (format 'xml');  -- ERROR
CREATE FOREIGN TABLE tbl () SERVER file_server OPTIONS (format 'text', quote ':');          -- ERROR
CREATE FOREIGN TABLE tbl () SERVER file_server OPTIONS (format 'text', escape ':');         -- ERROR
CREATE FOREIGN TABLE tbl () SERVER file_server OPTIONS (format 'binary', header 'true');    -- ERROR
CREATE FOREIGN TABLE tbl () SERVER file_server OPTIONS (format 'binary', quote ':');        -- ERROR
CREATE FOREIGN TABLE tbl () SERVER file_server OPTIONS (format 'binary', escape ':');       -- ERROR
CREATE FOREIGN TABLE tbl () SERVER file_server OPTIONS (format 'text', delimiter 'a');      -- ERROR
CREATE FOREIGN TABLE tbl () SERVER file_server OPTIONS (format 'text', escape '-');         -- ERROR
CREATE FOREIGN TABLE tbl () SERVER file_server OPTIONS (format 'csv', quote '-', null '=-=');   -- ERROR
CREATE FOREIGN TABLE tbl () SERVER file_server OPTIONS (format 'csv', delimiter '-', null '=-=');    -- ERROR
CREATE FOREIGN TABLE tbl () SERVER file_server OPTIONS (format 'csv', delimiter '-', quote '-');    -- ERROR
CREATE FOREIGN TABLE tbl () SERVER file_server OPTIONS (format 'csv', delimiter '---');     -- ERROR
CREATE FOREIGN TABLE tbl () SERVER file_server OPTIONS (format 'csv', quote '---');         -- ERROR
CREATE FOREIGN TABLE tbl () SERVER file_server OPTIONS (format 'csv', escape '---');        -- ERROR
CREATE FOREIGN TABLE tbl () SERVER file_server OPTIONS (format 'text', delimiter '\');       -- ERROR
CREATE FOREIGN TABLE tbl () SERVER file_server OPTIONS (format 'text', delimiter '.');       -- ERROR
CREATE FOREIGN TABLE tbl () SERVER file_server OPTIONS (format 'text', delimiter '1');       -- ERROR
CREATE FOREIGN TABLE tbl () SERVER file_server OPTIONS (format 'text', delimiter 'a');       -- ERROR
CREATE FOREIGN TABLE tbl () SERVER file_server OPTIONS (format 'csv', delimiter '
');       -- ERROR
CREATE FOREIGN TABLE tbl () SERVER file_server OPTIONS (format 'csv', null '
');       -- ERROR
CREATE FOREIGN TABLE tbl () SERVER file_server OPTIONS (on_error 'unsupported');       -- ERROR
CREATE FOREIGN TABLE tbl () SERVER file_server OPTIONS (format 'binary', on_error 'ignore');       -- ERROR
CREATE FOREIGN TABLE tbl () SERVER file_server OPTIONS (log_verbosity 'unsupported');       -- ERROR
CREATE FOREIGN TABLE tbl () SERVER file_server OPTIONS (reject_limit '1');       -- ERROR
CREATE FOREIGN TABLE tbl () SERVER file_server OPTIONS (on_error 'ignore', reject_limit '0');       -- ERROR
CREATE FOREIGN TABLE tbl () SERVER file_server;  -- ERROR

\set filename :abs_srcdir '/data/agg.data'
CREATE FOREIGN TABLE agg_text (
	a	int2 CHECK (a >= 0),
	b	float4
) SERVER file_server
OPTIONS (format 'text', filename :'filename', delimiter '	', null '\N');
GRANT SELECT ON agg_text TO regress_file_fdw_user;

\set filename :abs_srcdir '/data/agg.csv'
CREATE FOREIGN TABLE agg_csv (
	a	int2,
	b	float4
) SERVER file_server
OPTIONS (format 'csv', filename :'filename', header 'true', delimiter ';', quote '@', escape '"', null '');
ALTER FOREIGN TABLE agg_csv ADD CHECK (a >= 0);

\set filename :abs_srcdir '/data/agg.bad'
CREATE FOREIGN TABLE agg_bad (
	a	int2,
	b	float4
) SERVER file_server
OPTIONS (format 'csv', filename :'filename', header 'true', delimiter ';', quote '@', escape '"', null '');
ALTER FOREIGN TABLE agg_bad ADD CHECK (a >= 0);

-- test header matching
--
-- 测试标头匹配
\set filename :abs_srcdir '/data/list1.csv'
CREATE FOREIGN TABLE header_match ("1" int, foo text) SERVER file_server
OPTIONS (format 'csv', filename :'filename', delimiter ',', header 'match');
SELECT * FROM header_match;
CREATE FOREIGN TABLE header_doesnt_match (a int, foo text) SERVER file_server
OPTIONS (format 'csv', filename :'filename', delimiter ',', header 'match');
SELECT * FROM header_doesnt_match; -- ERROR

-- per-column options tests
--
-- 每列选项测试
\set filename :abs_srcdir '/data/text.csv'
CREATE FOREIGN TABLE text_csv (
    word1 text OPTIONS (force_not_null 'true'),
    word2 text OPTIONS (force_not_null 'off'),
    word3 text OPTIONS (force_null 'true'),
    word4 text OPTIONS (force_null 'off')
) SERVER file_server
OPTIONS (format 'text', filename :'filename', null 'NULL');
SELECT * FROM text_csv; -- ERROR
ALTER FOREIGN TABLE text_csv OPTIONS (SET format 'csv');
\pset null _null_
SELECT * FROM text_csv;

-- force_not_null and force_null can be used together on the same column
--
-- force_not_null和force_null可以在同一列上一起使用
ALTER FOREIGN TABLE text_csv ALTER COLUMN word1 OPTIONS (force_null 'true');
ALTER FOREIGN TABLE text_csv ALTER COLUMN word3 OPTIONS (force_not_null 'true');

-- force_not_null is not allowed to be specified at any foreign object level:
--
-- 不允许在任何外来对象级别指定force_not_null：
ALTER FOREIGN DATA WRAPPER file_fdw OPTIONS (ADD force_not_null '*'); -- ERROR
ALTER SERVER file_server OPTIONS (ADD force_not_null '*'); -- ERROR
CREATE USER MAPPING FOR public SERVER file_server OPTIONS (force_not_null '*'); -- ERROR
CREATE FOREIGN TABLE tbl () SERVER file_server OPTIONS (force_not_null '*'); -- ERROR

-- force_null is not allowed to be specified at any foreign object level:
--
-- 不允许在任何外来对象级别指定force_null：
ALTER FOREIGN DATA WRAPPER file_fdw OPTIONS (ADD force_null '*'); -- ERROR
ALTER SERVER file_server OPTIONS (ADD force_null '*'); -- ERROR
CREATE USER MAPPING FOR public SERVER file_server OPTIONS (force_null '*'); -- ERROR
CREATE FOREIGN TABLE tbl () SERVER file_server OPTIONS (force_null '*'); -- ERROR

-- basic query tests
--
-- 基本查询测试
SELECT * FROM agg_text WHERE b > 10.0 ORDER BY a;
SELECT * FROM agg_csv ORDER BY a;
SELECT * FROM agg_csv c JOIN agg_text t ON (t.a = c.a) ORDER BY c.a;

-- error context report tests
--
-- 错误上下文报告测试
SELECT * FROM agg_bad;               -- ERROR

-- on_error, log_verbosity and reject_limit tests
--
-- on_error、log_verbosity 和reject_limit 测试
ALTER FOREIGN TABLE agg_bad OPTIONS (ADD on_error 'ignore');
SELECT * FROM agg_bad;
ALTER FOREIGN TABLE agg_bad OPTIONS (ADD log_verbosity 'silent');
SELECT * FROM agg_bad;
ALTER FOREIGN TABLE agg_bad OPTIONS (ADD reject_limit '1'); -- ERROR
SELECT * FROM agg_bad;
ALTER FOREIGN TABLE agg_bad OPTIONS (SET reject_limit '2');
SELECT * FROM agg_bad;
ANALYZE agg_bad;

-- misc query tests
--
-- 杂项查询测试
\t on
SELECT explain_filter('EXPLAIN (VERBOSE, COSTS FALSE) SELECT * FROM agg_csv');
\t off
PREPARE st(int) AS SELECT * FROM agg_csv WHERE a = $1;
EXECUTE st(100);
EXECUTE st(100);
DEALLOCATE st;

-- tableoid
SELECT tableoid::regclass, b FROM agg_csv;

-- updates aren't supported
--
-- 不支持更新
INSERT INTO agg_csv VALUES(1,2.0);
UPDATE agg_csv SET a = 1;
DELETE FROM agg_csv WHERE a = 100;
TRUNCATE agg_csv;
-- but this should be allowed
--
-- 但这应该被允许
SELECT * FROM agg_csv FOR UPDATE;

-- copy from isn't supported either
--
-- 也不支持复制自
COPY agg_csv FROM STDIN;
12	3.4
\.

-- constraint exclusion tests
--
-- 约束排除测试
\t on
SELECT explain_filter('EXPLAIN (VERBOSE, COSTS FALSE) SELECT * FROM agg_csv WHERE a < 0');
\t off
SELECT * FROM agg_csv WHERE a < 0;
SET constraint_exclusion = 'on';
\t on
SELECT explain_filter('EXPLAIN (VERBOSE, COSTS FALSE) SELECT * FROM agg_csv WHERE a < 0');
\t off
SELECT * FROM agg_csv WHERE a < 0;
RESET constraint_exclusion;

-- table inheritance tests
--
-- 表继承测试
CREATE TABLE agg (a int2, b float4);
ALTER FOREIGN TABLE agg_csv INHERIT agg;
SELECT tableoid::regclass, * FROM agg;
SELECT tableoid::regclass, * FROM agg_csv;
SELECT tableoid::regclass, * FROM ONLY agg;
-- updates aren't supported
--
-- 不支持更新
UPDATE agg SET a = 1;
DELETE FROM agg WHERE a = 100;
-- but this should be allowed
--
-- 但这应该被允许
SELECT tableoid::regclass, * FROM agg FOR UPDATE;
ALTER FOREIGN TABLE agg_csv NO INHERIT agg;
DROP TABLE agg;

-- declarative partitioning tests
--
-- 声明性分区测试
SET ROLE regress_file_fdw_superuser;
CREATE TABLE pt (a int, b text) partition by list (a);
\set filename :abs_srcdir '/data/list1.csv'
CREATE FOREIGN TABLE p1 partition of pt for values in (1) SERVER file_server
OPTIONS (format 'csv', filename :'filename', delimiter ',');
CREATE TABLE p2 partition of pt for values in (2);
SELECT tableoid::regclass, * FROM pt;
SELECT tableoid::regclass, * FROM p1;
SELECT tableoid::regclass, * FROM p2;
\set filename :abs_srcdir '/data/list2.bad'
COPY pt FROM :'filename' with (format 'csv', delimiter ','); -- ERROR
\set filename :abs_srcdir '/data/list2.csv'
COPY pt FROM :'filename' with (format 'csv', delimiter ',');
SELECT tableoid::regclass, * FROM pt;
SELECT tableoid::regclass, * FROM p1;
SELECT tableoid::regclass, * FROM p2;
INSERT INTO pt VALUES (1, 'xyzzy'); -- ERROR
INSERT INTO pt VALUES (2, 'xyzzy');
UPDATE pt set a = 1 where a = 2; -- ERROR
SELECT tableoid::regclass, * FROM pt;
SELECT tableoid::regclass, * FROM p1;
SELECT tableoid::regclass, * FROM p2;

-- Test DELETE/UPDATE/MERGE on a partitioned table when all partitions
--
-- 当所有分区都在分区表上测试 DELETE/UPDATE/MERGE
-- are excluded and only the dummy root result relation remains. The
--
-- 被排除，仅保留虚拟根结果关系。这
-- operation is a no-op but should not fail regardless of whether the
--
-- 操作是无操作，但无论是否执行都不应失败
-- foreign child was processed (pruning off) or not (pruning on).
--
-- 外国孩子被处理（剪枝）或不被处理（剪枝）。
DROP TABLE p2;
SET enable_partition_pruning TO off;
DELETE FROM pt WHERE false;
UPDATE pt SET b = 'x' WHERE false;
MERGE INTO pt t USING (VALUES (1, 'x'::text)) AS s(a, b)
  ON false WHEN MATCHED THEN UPDATE SET b = s.b;

SET enable_partition_pruning TO on;
DELETE FROM pt WHERE false;
UPDATE pt SET b = 'x' WHERE false;
MERGE INTO pt t USING (VALUES (1, 'x'::text)) AS s(a, b)
  ON false WHEN MATCHED THEN UPDATE SET b = s.b;

DROP TABLE pt;

-- generated column tests
--
-- 生成的列测试
\set filename :abs_srcdir '/data/list1.csv'
CREATE FOREIGN TABLE gft1 (a int, b text, c text GENERATED ALWAYS AS ('foo') STORED) SERVER file_server
OPTIONS (format 'csv', filename :'filename', delimiter ',');
SELECT a, c FROM gft1;
DROP FOREIGN TABLE gft1;

-- copy default tests
--
-- 复制默认测试
\set filename :abs_srcdir '/data/copy_default.csv'
CREATE FOREIGN TABLE copy_default (
	id integer,
	text_value text not null default 'test',
	ts_value timestamp without time zone not null default '2022-07-05'
) SERVER file_server
OPTIONS (format 'csv', filename :'filename', default '\D');
SELECT id, text_value, ts_value FROM copy_default;
DROP FOREIGN TABLE copy_default;

-- privilege tests
--
-- 特权测试
SET ROLE regress_file_fdw_superuser;
SELECT * FROM agg_text ORDER BY a;
SET ROLE regress_file_fdw_user;
SELECT * FROM agg_text ORDER BY a;
SET ROLE regress_no_priv_user;
SELECT * FROM agg_text ORDER BY a;   -- ERROR
SET ROLE regress_file_fdw_user;
\t on
SELECT explain_filter('EXPLAIN (VERBOSE, COSTS FALSE) SELECT * FROM agg_text WHERE a > 0');
\t off
-- file FDW allows foreign tables to be accessed without user mapping
--
-- 文件FDW允许在没有用户映射的情况下访问外部表
DROP USER MAPPING FOR regress_file_fdw_user SERVER file_server;
SELECT * FROM agg_text ORDER BY a;

-- privilege tests for object
--
-- 对象的权限测试
SET ROLE regress_file_fdw_superuser;
ALTER FOREIGN TABLE agg_text OWNER TO regress_file_fdw_user;
ALTER FOREIGN TABLE agg_text OPTIONS (SET format 'text');
SET ROLE regress_file_fdw_user;
ALTER FOREIGN TABLE agg_text OPTIONS (SET format 'text');
SET ROLE regress_file_fdw_superuser;

-- cleanup
RESET ROLE;
DROP EXTENSION file_fdw CASCADE;
DROP ROLE regress_file_fdw_superuser, regress_file_fdw_user, regress_no_priv_user;

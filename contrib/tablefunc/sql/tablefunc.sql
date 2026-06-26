CREATE EXTENSION tablefunc;

--
-- normal_rand()
-- no easy way to do this for regression testing
--
-- 对于回归测试来说没有简单的方法
--
SELECT avg(normal_rand)::int, count(*) FROM normal_rand(100, 250, 0.2);
-- negative number of tuples
--
-- 元组的负数
SELECT avg(normal_rand)::int, count(*) FROM normal_rand(-1, 250, 0.2);

--
-- crosstab()
--
CREATE TABLE ct(id int, rowclass text, rowid text, attribute text, val text);
\copy ct from 'data/ct.data'

SELECT * FROM crosstab2('SELECT rowid, attribute, val FROM ct where rowclass = ''group1'' and (attribute = ''att2'' or attribute = ''att3'') ORDER BY 1,2;');
SELECT * FROM crosstab3('SELECT rowid, attribute, val FROM ct where rowclass = ''group1'' and (attribute = ''att2'' or attribute = ''att3'') ORDER BY 1,2;');
SELECT * FROM crosstab4('SELECT rowid, attribute, val FROM ct where rowclass = ''group1'' and (attribute = ''att2'' or attribute = ''att3'') ORDER BY 1,2;');

SELECT * FROM crosstab2('SELECT rowid, attribute, val FROM ct where rowclass = ''group1'' ORDER BY 1,2;');
SELECT * FROM crosstab3('SELECT rowid, attribute, val FROM ct where rowclass = ''group1'' ORDER BY 1,2;');
SELECT * FROM crosstab4('SELECT rowid, attribute, val FROM ct where rowclass = ''group1'' ORDER BY 1,2;');

SELECT * FROM crosstab2('SELECT rowid, attribute, val FROM ct where rowclass = ''group2'' and (attribute = ''att1'' or attribute = ''att2'') ORDER BY 1,2;');
SELECT * FROM crosstab3('SELECT rowid, attribute, val FROM ct where rowclass = ''group2'' and (attribute = ''att1'' or attribute = ''att2'') ORDER BY 1,2;');
SELECT * FROM crosstab4('SELECT rowid, attribute, val FROM ct where rowclass = ''group2'' and (attribute = ''att1'' or attribute = ''att2'') ORDER BY 1,2;');

SELECT * FROM crosstab2('SELECT rowid, attribute, val FROM ct where rowclass = ''group2'' ORDER BY 1,2;');
SELECT * FROM crosstab3('SELECT rowid, attribute, val FROM ct where rowclass = ''group2'' ORDER BY 1,2;');
SELECT * FROM crosstab4('SELECT rowid, attribute, val FROM ct where rowclass = ''group2'' ORDER BY 1,2;');

SELECT * FROM crosstab('SELECT rowid, attribute, val FROM ct where rowclass = ''group1'' ORDER BY 1,2;') AS c(rowid text, att1 text, att2 text);
SELECT * FROM crosstab('SELECT rowid, attribute, val FROM ct where rowclass = ''group1'' ORDER BY 1,2;') AS c(rowid text, att1 text, att2 text, att3 text);
SELECT * FROM crosstab('SELECT rowid, attribute, val FROM ct where rowclass = ''group1'' ORDER BY 1,2;') AS c(rowid text, att1 text, att2 text, att3 text, att4 text);

-- check it works with OUT parameters, too
--
-- 检查它是否也适用于 OUT 参数

CREATE FUNCTION crosstab_out(text,
	OUT rowid text, OUT att1 text, OUT att2 text, OUT att3 text)
RETURNS setof record
AS '$libdir/tablefunc','crosstab'
LANGUAGE C STABLE STRICT;

SELECT * FROM crosstab_out('SELECT rowid, attribute, val FROM ct where rowclass = ''group1'' ORDER BY 1,2;');

-- check error reporting
--
-- 检查错误报告
SELECT * FROM crosstab('SELECT rowid, val FROM ct where rowclass = ''group1'' and (attribute = ''att2'' or attribute = ''att3'') ORDER BY 1,2;')
  AS ct(row_name text, category_1 text, category_2 text);
SELECT * FROM crosstab('SELECT rowid, attribute, val FROM ct where rowclass = ''group1'' and (attribute = ''att2'' or attribute = ''att3'') ORDER BY 1,2;')
  AS ct(row_name text);
SELECT * FROM crosstab('SELECT rowid, attribute, val FROM ct where rowclass = ''group1'' and (attribute = ''att2'' or attribute = ''att3'') ORDER BY 1,2;')
  AS ct(row_name int, category_1 text, category_2 text);
SELECT * FROM crosstab('SELECT rowid, attribute, val FROM ct where rowclass = ''group1'' and (attribute = ''att2'' or attribute = ''att3'') ORDER BY 1,2;')
  AS ct(row_name text, category_1 text, category_2 int);

--
-- hash based crosstab
--
-- 基于哈希的交叉表
--
create table cth(id serial, rowid text, rowdt timestamp, attribute text, val text);
insert into cth values(DEFAULT,'test1','01 March 2003','temperature','42');
insert into cth values(DEFAULT,'test1','01 March 2003','test_result','PASS');
-- the next line is intentionally left commented and is therefore a "missing" attribute
--
-- 下一行故意留下注释，因此是“缺失”属性
-- insert into cth values(DEFAULT,'test1','01 March 2003','test_startdate','28 February 2003');
--
-- 插入 cth 值(DEFAULT,'test1','2003 年 3 月 1 日','test_startdate','2003 年 2 月 28 日');
insert into cth values(DEFAULT,'test1','01 March 2003','volts','2.6987');
insert into cth values(DEFAULT,'test2','02 March 2003','temperature','53');
insert into cth values(DEFAULT,'test2','02 March 2003','test_result','FAIL');
insert into cth values(DEFAULT,'test2','02 March 2003','test_startdate','01 March 2003');
insert into cth values(DEFAULT,'test2','02 March 2003','volts','3.1234');
-- next group tests for NULL rowids
--
-- 下一组测试 NULL rowid
insert into cth values(DEFAULT,NULL,'25 October 2007','temperature','57');
insert into cth values(DEFAULT,NULL,'25 October 2007','test_result','PASS');
insert into cth values(DEFAULT,NULL,'25 October 2007','test_startdate','24 October 2007');
insert into cth values(DEFAULT,NULL,'25 October 2007','volts','1.41234');

-- return attributes as plain text
--
-- 以纯文本形式返回属性
SELECT * FROM crosstab(
  'SELECT rowid, rowdt, attribute, val FROM cth ORDER BY 1',
  'SELECT DISTINCT attribute FROM cth ORDER BY 1')
AS c(rowid text, rowdt timestamp, temperature text, test_result text, test_startdate text, volts text);

-- this time without rowdt
--
-- 这次没有 rowdt
SELECT * FROM crosstab(
  'SELECT rowid, attribute, val FROM cth ORDER BY 1',
  'SELECT DISTINCT attribute FROM cth ORDER BY 1')
AS c(rowid text, temperature text, test_result text, test_startdate text, volts text);

-- convert attributes to specific datatypes
--
-- 将属性转换为特定数据类型
SELECT * FROM crosstab(
  'SELECT rowid, rowdt, attribute, val FROM cth ORDER BY 1',
  'SELECT DISTINCT attribute FROM cth ORDER BY 1')
AS c(rowid text, rowdt timestamp, temperature int4, test_result text, test_startdate timestamp, volts float8);

-- source query and category query out of sync
--
-- 来源查询和类别查询不同步
SELECT * FROM crosstab(
  'SELECT rowid, rowdt, attribute, val FROM cth ORDER BY 1',
  'SELECT DISTINCT attribute FROM cth WHERE attribute IN (''temperature'',''test_result'',''test_startdate'') ORDER BY 1')
AS c(rowid text, rowdt timestamp, temperature int4, test_result text, test_startdate timestamp);

-- if category query generates no rows, get expected error
--
-- 如果类别查询未生成任何行，则得到预期错误
SELECT * FROM crosstab(
  'SELECT rowid, rowdt, attribute, val FROM cth ORDER BY 1',
  'SELECT DISTINCT attribute FROM cth WHERE attribute = ''a'' ORDER BY 1')
AS c(rowid text, rowdt timestamp, temperature int4, test_result text, test_startdate timestamp, volts float8);

-- if category query generates more than one column, get expected error
--
-- 如果类别查询生成多于一列，则会出现预期错误
SELECT * FROM crosstab(
  'SELECT rowid, rowdt, attribute, val FROM cth ORDER BY 1',
  'SELECT DISTINCT rowdt, attribute FROM cth ORDER BY 2')
AS c(rowid text, rowdt timestamp, temperature int4, test_result text, test_startdate timestamp, volts float8);

-- if category query generates a NULL value, get expected error
--
-- 如果类别查询生成 NULL 值，则得到预期错误
SELECT * FROM crosstab(
  'SELECT rowid, rowdt, attribute, val FROM cth ORDER BY 1',
  'SELECT NULL::text')
AS c(rowid text, rowdt timestamp, temperature int4, test_result text, test_startdate timestamp, volts float8);

-- if source query returns zero rows, get zero rows returned
--
-- 如果源查询返回零行，则返回零行
SELECT * FROM crosstab(
  'SELECT rowid, rowdt, attribute, val FROM cth WHERE false ORDER BY 1',
  'SELECT DISTINCT attribute FROM cth ORDER BY 1')
AS c(rowid text, rowdt timestamp, temperature text, test_result text, test_startdate text, volts text);

-- if source query returns zero rows, get zero rows returned even if category query generates no rows
--
-- 如果源查询返回零行，即使类别查询不生成任何行，也返回零行
SELECT * FROM crosstab(
  'SELECT rowid, rowdt, attribute, val FROM cth WHERE false ORDER BY 1',
  'SELECT DISTINCT attribute FROM cth WHERE false ORDER BY 1')
AS c(rowid text, rowdt timestamp, temperature text, test_result text, test_startdate text, volts text);

-- check errors with inappropriate input rowtype
--
-- 检查输入行类型不正确的错误
SELECT * FROM crosstab(
  'SELECT rowid, attribute FROM cth ORDER BY 1',
  'SELECT DISTINCT attribute FROM cth ORDER BY 1')
AS c(rowid text, temperature text, test_result text, test_startdate text, volts text);
SELECT * FROM crosstab(
  'SELECT rowid, rowdt, rowdt, attribute, val FROM cth ORDER BY 1',
  'SELECT DISTINCT attribute FROM cth ORDER BY 1')
AS c(rowid text, rowdt timestamp, temperature int4, test_result text, test_startdate timestamp, volts float8);

-- check errors with inappropriate result rowtype
--
-- 检查结果行类型不正确的错误
SELECT * FROM crosstab(
  'SELECT rowid, attribute, val FROM cth ORDER BY 1',
  'SELECT DISTINCT attribute FROM cth ORDER BY 1')
AS c(rowid text);

-- check it works with a named result rowtype
--
-- 检查它是否适用于命名结果行类型

create type my_crosstab_result as (
  rowid text, rowdt timestamp,
  temperature int4, test_result text, test_startdate timestamp, volts float8);

CREATE FUNCTION crosstab_named(text, text)
RETURNS setof my_crosstab_result
AS '$libdir/tablefunc','crosstab_hash'
LANGUAGE C STABLE STRICT;

SELECT * FROM crosstab_named(
  'SELECT rowid, rowdt, attribute, val FROM cth ORDER BY 1',
  'SELECT DISTINCT attribute FROM cth ORDER BY 1');

-- check it works with OUT parameters
--
-- 检查它是否与 OUT 参数一起工作

CREATE FUNCTION crosstab_out(text, text,
  OUT rowid text, OUT rowdt timestamp,
  OUT temperature int4, OUT test_result text,
  OUT test_startdate timestamp, OUT volts float8)
RETURNS setof record
AS '$libdir/tablefunc','crosstab_hash'
LANGUAGE C STABLE STRICT;

SELECT * FROM crosstab_out(
  'SELECT rowid, rowdt, attribute, val FROM cth ORDER BY 1',
  'SELECT DISTINCT attribute FROM cth ORDER BY 1');

--
-- connectby
--

-- test connectby with text based hierarchy
--
-- 使用基于文本的层次结构测试 connectby
CREATE TABLE connectby_text(keyid text, parent_keyid text, pos int);
\copy connectby_text from 'data/connectby_text.data'

-- with branch, without orderby
--
-- 有分支，无 orderby
SELECT * FROM connectby('connectby_text', 'keyid', 'parent_keyid', 'row2', 0, '~') AS t(keyid text, parent_keyid text, level int, branch text);

-- without branch, without orderby
--
-- 没有分支，没有 orderby
SELECT * FROM connectby('connectby_text', 'keyid', 'parent_keyid', 'row2', 0) AS t(keyid text, parent_keyid text, level int);

-- with branch, with orderby
--
-- 带分支，带 orderby
SELECT * FROM connectby('connectby_text', 'keyid', 'parent_keyid', 'pos', 'row2', 0, '~') AS t(keyid text, parent_keyid text, level int, branch text, pos int) ORDER BY t.pos;

-- without branch, with orderby
--
-- 没有分支，有 orderby
SELECT * FROM connectby('connectby_text', 'keyid', 'parent_keyid', 'pos', 'row2', 0) AS t(keyid text, parent_keyid text, level int, pos int) ORDER BY t.pos;

-- test connectby with int based hierarchy
--
-- 使用基于 int 的层次结构测试 connectby
CREATE TABLE connectby_int(keyid int, parent_keyid int);
\copy connectby_int from 'data/connectby_int.data'

-- with branch
--
-- 有分支
SELECT * FROM connectby('connectby_int', 'keyid', 'parent_keyid', '2', 0, '~') AS t(keyid int, parent_keyid int, level int, branch text);

-- without branch
--
-- 无分支
SELECT * FROM connectby('connectby_int', 'keyid', 'parent_keyid', '2', 0) AS t(keyid int, parent_keyid int, level int);

-- recursion detection
--
-- 递归检测
INSERT INTO connectby_int VALUES(10,9);
INSERT INTO connectby_int VALUES(11,10);
INSERT INTO connectby_int VALUES(9,11);

-- should fail due to infinite recursion
--
-- 由于无限递归应该失败
SELECT * FROM connectby('connectby_int', 'keyid', 'parent_keyid', '2', 0, '~') AS t(keyid int, parent_keyid int, level int, branch text);

-- infinite recursion failure avoided by depth limit
--
-- 通过深度限制避免无限递归失败
SELECT * FROM connectby('connectby_int', 'keyid', 'parent_keyid', '2', 4, '~') AS t(keyid int, parent_keyid int, level int, branch text);

-- should fail as first two columns must have the same type
--
-- 应该失败，因为前两列必须具有相同的类型
SELECT * FROM connectby('connectby_int', 'keyid', 'parent_keyid', '2', 0, '~') AS t(keyid text, parent_keyid int, level int, branch text);

-- should fail as key field datatype should match return datatype
--
-- 应该失败，因为键字段数据类型应该与返回数据类型匹配
SELECT * FROM connectby('connectby_int', 'keyid', 'parent_keyid', '2', 0, '~') AS t(keyid float8, parent_keyid float8, level int, branch text);
SELECT * FROM connectby('connectby_int', 'keyid', 'parent_keyid', '2', 0, '~') AS t(keyid int, parent_keyid float8, level int, branch text);

-- check other rowtype mismatch cases
--
-- 检查其他行类型不匹配的情况
SELECT * FROM connectby('connectby_int', 'keyid', 'parent_keyid', '2', 0) AS t(keyid int, parent_keyid int, level int, branch text);
SELECT * FROM connectby('connectby_int', 'keyid', 'parent_keyid', '2', 0, '~') AS t(keyid int, parent_keyid int, level int);
SELECT * FROM connectby('connectby_int', 'keyid', 'parent_keyid', '2', 0) AS t(keyid int, parent_keyid text, level int);
SELECT * FROM connectby('connectby_int', 'keyid', 'parent_keyid', '2', 0, '~') AS t(keyid int, parent_keyid int, level float, branch float);
SELECT * FROM connectby('connectby_int', 'keyid', 'parent_keyid', '2', 0, '~') AS t(keyid int, parent_keyid int, level int, branch float);
SELECT * FROM connectby('connectby_text', 'keyid', 'parent_keyid', 'pos', 'row2', 0, '~') AS t(keyid text, parent_keyid text, level int, branch text, pos text);
SELECT * FROM connectby('connectby_text', 'keyid', 'parent_keyid', 'pos', 'row2', 0) AS t(keyid text, parent_keyid text, level int, pos text);

-- tests for values using custom queries
--
-- 使用自定义查询测试值
-- query with one column - failed
--
-- 查询一列 - 失败
SELECT * FROM connectby('connectby_int', '1; --', 'parent_keyid', '2', 0) AS t(keyid int, parent_keyid int, level int);
-- query with two columns first value as NULL
--
-- 两列第一个值为 NULL 的查询
SELECT * FROM connectby('connectby_int', 'NULL::int, 1::int; --', 'parent_keyid', '2', 0) AS t(keyid int, parent_keyid int, level int);
-- query with two columns second value as NULL
--
-- 查询两列第二个值为 NULL
SELECT * FROM connectby('connectby_int', '1::int, NULL::int; --', 'parent_keyid', '2', 0) AS t(keyid int, parent_keyid int, level int);
-- query with two columns, both values as NULL
--
-- 查询两列，均为 NULL
SELECT * FROM connectby('connectby_int', 'NULL::int, NULL::int; --', 'parent_keyid', '2', 0) AS t(keyid int, parent_keyid int, level int);

-- test for falsely detected recursion
--
-- 测试错误检测到的递归
DROP TABLE connectby_int;
CREATE TABLE connectby_int(keyid int, parent_keyid int);
INSERT INTO connectby_int VALUES(11,NULL);
INSERT INTO connectby_int VALUES(10,11);
INSERT INTO connectby_int VALUES(111,11);
INSERT INTO connectby_int VALUES(1,111);
-- this should not fail due to recursion detection
--
-- 这不应因递归检测而失败
SELECT * FROM connectby('connectby_int', 'keyid', 'parent_keyid', '11', 0, '-') AS t(keyid int, parent_keyid int, level int, branch text);

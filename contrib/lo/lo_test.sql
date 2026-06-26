/* contrib/lo/lo_test.sql */

-- Adjust this setting to control where the objects get created.
--
-- 调整此设置以控制创建对象的位置。
SET search_path = public;

--
-- This runs some common tests against the type
--
-- 这会针对该类型运行一些常见的测试
--
-- It's used just for development
--
-- 它仅用于开发
--
-- XXX would be nice to turn this into a proper regression test
--
-- XXX 很高兴将其转变为适当的回归测试
--

-- Check what is in pg_largeobject
--
-- 检查 pg_largeobject 中有什么
SELECT count(oid) FROM pg_largeobject_metadata;

-- ignore any errors here - simply drop the table if it already exists
--
-- 忽略此处的任何错误 - 如果表已存在，只需删除该表
DROP TABLE a;

-- create the test table
--
-- 创建测试表
CREATE TABLE a (fname name,image lo);

-- insert a null object
--
-- 插入一个空对象
INSERT INTO a VALUES ('empty');

-- insert a large object based on a file
--
-- 插入基于文件的大对象
INSERT INTO a VALUES ('/etc/group', lo_import('/etc/group')::lo);

-- now select the table
--
-- 现在选择表格
SELECT * FROM a;

-- check that coercion to plain oid works
--
-- 检查对普通 oid 的强制是否有效
SELECT *,image::oid from a;

-- now test the trigger
--
-- 现在测试触发器
CREATE TRIGGER t_a
BEFORE UPDATE OR DELETE ON a
FOR EACH ROW
EXECUTE PROCEDURE lo_manage(image);

-- insert
INSERT INTO a VALUES ('aa', lo_import('/etc/hosts'));
SELECT * FROM a
WHERE fname LIKE 'aa%';

-- update
UPDATE a SET image=lo_import('/etc/group')::lo
WHERE fname='aa';
SELECT * FROM a
WHERE fname LIKE 'aa%';

-- update the 'empty' row which should be null
--
-- 更新应该为 null 的“空”行
UPDATE a SET image=lo_import('/etc/hosts')
WHERE fname='empty';
SELECT * FROM a
WHERE fname LIKE 'empty%';
UPDATE a SET image=null
WHERE fname='empty';
SELECT * FROM a
WHERE fname LIKE 'empty%';

-- delete the entry
--
-- 删除该条目
DELETE FROM a
WHERE fname='aa';
SELECT * FROM a
WHERE fname LIKE 'aa%';

-- This deletes the table contents. Note, if you comment this out, and
--
-- 这将删除表内容。请注意，如果您将其注释掉，并且
-- expect the drop table to remove the objects, think again. The trigger
--
-- 期望删除表删除对象，请再考虑一下。触发器
-- doesn't get fired by drop table.
--
-- 不会被删除表解雇。
DELETE FROM a;

-- finally drop the table
--
-- 最后放下桌子
DROP TABLE a;

-- Check what is in pg_largeobject ... if different from original, trouble
--
-- 检查 pg_largeobject 中有什么...如果与原始的不同，麻烦
SELECT count(oid) FROM pg_largeobject_metadata;

-- end of tests
--
-- 测试结束

-- bool check
--
-- 布尔检查

CREATE TABLE booltmp (a bool);

INSERT INTO booltmp VALUES (false), (true);

SET enable_seqscan=on;

SELECT count(*) FROM booltmp WHERE a <  true;

SELECT count(*) FROM booltmp WHERE a <= true;

SELECT count(*) FROM booltmp WHERE a  = true;

SELECT count(*) FROM booltmp WHERE a >= true;

SELECT count(*) FROM booltmp WHERE a >  true;

CREATE INDEX boolidx ON booltmp USING gist ( a );

SET enable_seqscan=off;

SELECT count(*) FROM booltmp WHERE a <  true;

SELECT count(*) FROM booltmp WHERE a <= true;

SELECT count(*) FROM booltmp WHERE a  = true;

SELECT count(*) FROM booltmp WHERE a >= true;

SELECT count(*) FROM booltmp WHERE a >  true;

-- Test index-only scans
--
-- 测试仅索引扫描
SET enable_bitmapscan=off;

EXPLAIN (COSTS OFF)
SELECT * FROM booltmp WHERE a;
SELECT * FROM booltmp WHERE a;

EXPLAIN (COSTS OFF)
SELECT * FROM booltmp WHERE NOT a;
SELECT * FROM booltmp WHERE NOT a;

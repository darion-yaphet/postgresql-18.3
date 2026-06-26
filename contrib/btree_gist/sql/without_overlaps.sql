-- Core must test WITHOUT OVERLAPS
--
-- 核心必须在没有重叠的情况下进行测试
-- with an int4range + daterange,
--
-- 使用 int4range + daterange，
-- so here we do some simple tests
--
-- 所以这里我们做一些简单的测试
-- to make sure int + daterange works too,
--
-- 确保 int + daterange 也能工作，
-- since that is the expected use-case.
--
-- 因为这是预期的用例。
CREATE TABLE temporal_rng (
  id integer,
  valid_at daterange,
  CONSTRAINT temporal_rng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS)
);
\d temporal_rng
SELECT pg_get_constraintdef(oid) FROM pg_constraint WHERE conname = 'temporal_rng_pk';
SELECT pg_get_indexdef(conindid, 0, true) FROM pg_constraint WHERE conname = 'temporal_rng_pk';

INSERT INTO temporal_rng VALUES
  (1, '[2000-01-01,2001-01-01)');
-- same key, doesn't overlap:
--
-- 相同的键，不重叠：
INSERT INTO temporal_rng VALUES
  (1, '[2001-01-01,2002-01-01)');
-- overlaps but different key:
--
-- 重叠但不同的键：
INSERT INTO temporal_rng VALUES
  (2, '[2000-01-01,2001-01-01)');
-- should fail:
--
-- 应该失败：
INSERT INTO temporal_rng VALUES
  (1, '[2000-06-01,2001-01-01)');

-- Foreign key
--
-- 外键
CREATE TABLE temporal_fk_rng2rng (
  id integer,
  valid_at daterange,
  parent_id integer,
  CONSTRAINT temporal_fk_rng2rng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS),
  CONSTRAINT temporal_fk_rng2rng_fk FOREIGN KEY (parent_id, PERIOD valid_at)
    REFERENCES temporal_rng (id, PERIOD valid_at)
);
\d temporal_fk_rng2rng
SELECT pg_get_constraintdef(oid) FROM pg_constraint WHERE conname = 'temporal_fk_rng2rng_fk';

-- okay
INSERT INTO temporal_fk_rng2rng VALUES
  (1, '[2000-01-01,2001-01-01)', 1);
-- okay spanning two parent records:
--
-- 可以跨越两个父记录：
INSERT INTO temporal_fk_rng2rng VALUES
  (2, '[2000-01-01,2002-01-01)', 1);
-- key is missing
--
-- 钥匙不见了
INSERT INTO temporal_fk_rng2rng VALUES
  (3, '[2000-01-01,2001-01-01)', 3);
-- key exist but is outside range
--
-- 键存在但超出范围
INSERT INTO temporal_fk_rng2rng VALUES
  (4, '[2001-01-01,2002-01-01)', 2);
-- key exist but is partly outside range
--
-- 键存在，但部分超出范围
INSERT INTO temporal_fk_rng2rng VALUES
  (5, '[2000-01-01,2002-01-01)', 2);

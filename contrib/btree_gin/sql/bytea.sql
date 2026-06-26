set enable_seqscan=off;
-- ensure consistent test output regardless of the default bytea format
--
-- 无论默认的 bytea 格式如何，都确保测试输出一致
SET bytea_output TO escape;

CREATE TABLE test_bytea (
	i bytea
);

INSERT INTO test_bytea VALUES ('a'),('ab'),('abc'),('abb'),('axy'),('xyz');

CREATE INDEX idx_bytea ON test_bytea USING gin (i);

SELECT * FROM test_bytea WHERE i<'abc'::bytea ORDER BY i;
SELECT * FROM test_bytea WHERE i<='abc'::bytea ORDER BY i;
SELECT * FROM test_bytea WHERE i='abc'::bytea ORDER BY i;
SELECT * FROM test_bytea WHERE i>='abc'::bytea ORDER BY i;
SELECT * FROM test_bytea WHERE i>'abc'::bytea ORDER BY i;

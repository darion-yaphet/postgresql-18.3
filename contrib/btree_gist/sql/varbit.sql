-- varbit check
--
-- 瓦比特检查

CREATE TABLE varbittmp (a varbit);

\copy varbittmp from 'data/varbit.data'

SET enable_seqscan=on;

SELECT count(*) FROM varbittmp WHERE a <   '1110100111010';

SELECT count(*) FROM varbittmp WHERE a <=  '1110100111010';

SELECT count(*) FROM varbittmp WHERE a  =  '1110100111010';

SELECT count(*) FROM varbittmp WHERE a >=  '1110100111010';

SELECT count(*) FROM varbittmp WHERE a >   '1110100111010';

CREATE INDEX varbitidx ON varbittmp USING GIST ( a );

SET enable_seqscan=off;

SELECT count(*) FROM varbittmp WHERE a <   '1110100111010'::varbit;

SELECT count(*) FROM varbittmp WHERE a <=  '1110100111010'::varbit;

SELECT count(*) FROM varbittmp WHERE a  =  '1110100111010'::varbit;

SELECT count(*) FROM varbittmp WHERE a >=  '1110100111010'::varbit;

SELECT count(*) FROM varbittmp WHERE a >   '1110100111010'::varbit;

-- Test index-only scans
--
-- 测试仅索引扫描
SET enable_bitmapscan=off;
EXPLAIN (COSTS OFF)
SELECT a FROM bittmp WHERE a BETWEEN '1000000' and '1000001';

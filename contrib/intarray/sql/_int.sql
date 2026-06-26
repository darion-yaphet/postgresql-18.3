CREATE EXTENSION intarray;

-- Check whether any of our opclasses fail amvalidate
--
-- 检查我们的任何 opclass 是否未通过 amvalidate
SELECT amname, opcname
FROM pg_opclass opc LEFT JOIN pg_am am ON am.oid = opcmethod
WHERE opc.oid >= 16384 AND NOT amvalidate(opc.oid);

SELECT intset(1234);
SELECT icount('{1234234,234234}');
SELECT sort('{1234234,-30,234234}');
SELECT sort('{1234234,-30,234234}','asc');
SELECT sort('{1234234,-30,234234}','desc');
SELECT sort_asc('{1234234,-30,234234}');
SELECT sort_desc('{1234234,-30,234234}');
SELECT uniq('{1234234,-30,-30,234234,-30}');
SELECT uniq(sort_asc('{1234234,-30,-30,234234,-30}'));
SELECT idx('{1234234,-30,-30,234234,-30}',-30);
SELECT subarray('{1234234,-30,-30,234234,-30}',2,3);
SELECT subarray('{1234234,-30,-30,234234,-30}',-1,1);
SELECT subarray('{1234234,-30,-30,234234,-30}',0,-1);

SELECT #'{1234234,234234}'::int[];
SELECT '{123,623,445}'::int[] + 1245;
SELECT '{123,623,445}'::int[] + 445;
SELECT '{123,623,445}'::int[] + '{1245,87,445}';
SELECT '{123,623,445}'::int[] - 623;
SELECT '{123,623,445}'::int[] - '{1623,623}';
SELECT '{123,623,445}'::int[] | 623;
SELECT '{123,623,445}'::int[] | 1623;
SELECT '{123,623,445}'::int[] | '{1623,623}';
SELECT '{123,623,445}'::int[] & '{1623,623}';
SELECT '{-1,3,1}'::int[] & '{1,2}';
SELECT '{1}'::int[] & '{2}'::int[];
SELECT array_dims('{1}'::int[] & '{2}'::int[]);
SELECT ('{1}'::int[] & '{2}'::int[]) = '{}'::int[];
SELECT ('{}'::int[] & '{}'::int[]) = '{}'::int[];


--test query_int
--
--测试query_int
SELECT '1'::query_int;
SELECT ' 1'::query_int;
SELECT '1 '::query_int;
SELECT ' 1 '::query_int;
SELECT ' ! 1 '::query_int;
SELECT '!1'::query_int;
SELECT '1|2'::query_int;
SELECT '1|!2'::query_int;
SELECT '!1|2'::query_int;
SELECT '!1|!2'::query_int;
SELECT '!(!1|!2)'::query_int;
SELECT '!(!1|2)'::query_int;
SELECT '!(1|!2)'::query_int;
SELECT '!(1|2)'::query_int;
SELECT '1&2'::query_int;
SELECT '!1&2'::query_int;
SELECT '1&!2'::query_int;
SELECT '!1&!2'::query_int;
SELECT '(1&2)'::query_int;
SELECT '1&(2)'::query_int;
SELECT '!(1)&2'::query_int;
SELECT '!(1&2)'::query_int;
SELECT '1|2&3'::query_int;
SELECT '1|(2&3)'::query_int;
SELECT '(1|2)&3'::query_int;
SELECT '1|2&!3'::query_int;
SELECT '1|!2&3'::query_int;
SELECT '!1|2&3'::query_int;
SELECT '!1|(2&3)'::query_int;
SELECT '!(1|2)&3'::query_int;
SELECT '(!1|2)&3'::query_int;
SELECT '1|(2|(4|(5|6)))'::query_int;
SELECT '1|2|4|5|6'::query_int;
SELECT '1&(2&(4&(5&6)))'::query_int;
SELECT '1&2&4&5&6'::query_int;
SELECT '1&(2&(4&(5|6)))'::query_int;
SELECT '1&(2&(4&(5|!6)))'::query_int;

-- test non-error-throwing input
--
-- 测试不抛出错误的输入

SELECT str as "query_int",
       pg_input_is_valid(str,'query_int') as ok,
       errinfo.sql_error_code,
       errinfo.message,
       errinfo.detail,
       errinfo.hint
FROM (VALUES ('1&(2&(4&(5|6)))'),
             ('1#(2&(4&(5&6)))'),
             ('foo'))
      AS a(str),
     LATERAL pg_input_error_info(a.str, 'query_int') as errinfo;



CREATE TABLE test__int( a int[] );
\copy test__int from 'data/test__int.data'
ANALYZE test__int;

SELECT count(*) from test__int WHERE a && '{23,50}';
SELECT count(*) from test__int WHERE a @@ '23|50';
SELECT count(*) from test__int WHERE a @> '{23,50}';
SELECT count(*) from test__int WHERE a @@ '23&50';
SELECT count(*) from test__int WHERE a @> '{20,23}';
SELECT count(*) from test__int WHERE a <@ '{73,23,20}';
SELECT count(*) from test__int WHERE a = '{73,23,20}';
SELECT count(*) from test__int WHERE a @@ '50&68';
SELECT count(*) from test__int WHERE a @> '{20,23}' or a @> '{50,68}';
SELECT count(*) from test__int WHERE a @@ '(20&23)|(50&68)';
SELECT count(*) from test__int WHERE a @@ '20 | !21';
SELECT count(*) from test__int WHERE a @@ '!20 & !21';
SELECT count(*) from test__int WHERE a @@ '!2733 & (2738 | 254)';

SET enable_seqscan = off;  -- not all of these would use index by default

CREATE INDEX text_idx on test__int using gist ( a gist__int_ops );

SELECT count(*) from test__int WHERE a && '{23,50}';
SELECT count(*) from test__int WHERE a @@ '23|50';
SELECT count(*) from test__int WHERE a @> '{23,50}';
SELECT count(*) from test__int WHERE a @@ '23&50';
SELECT count(*) from test__int WHERE a @> '{20,23}';
SELECT count(*) from test__int WHERE a <@ '{73,23,20}';
SELECT count(*) from test__int WHERE a = '{73,23,20}';
SELECT count(*) from test__int WHERE a @@ '50&68';
SELECT count(*) from test__int WHERE a @> '{20,23}' or a @> '{50,68}';
SELECT count(*) from test__int WHERE a @@ '(20&23)|(50&68)';
SELECT count(*) from test__int WHERE a @@ '20 | !21';
SELECT count(*) from test__int WHERE a @@ '!20 & !21';
SELECT count(*) from test__int WHERE a @@ '!2733 & (2738 | 254)';

INSERT INTO test__int SELECT array(SELECT x FROM generate_series(1, 1001) x); -- should fail

DROP INDEX text_idx;
CREATE INDEX text_idx on test__int using gist (a gist__int_ops(numranges = 0));
CREATE INDEX text_idx on test__int using gist (a gist__int_ops(numranges = 253));
CREATE INDEX text_idx on test__int using gist (a gist__int_ops(numranges = 252));

SELECT count(*) from test__int WHERE a && '{23,50}';
SELECT count(*) from test__int WHERE a @@ '23|50';
SELECT count(*) from test__int WHERE a @> '{23,50}';
SELECT count(*) from test__int WHERE a @@ '23&50';
SELECT count(*) from test__int WHERE a @> '{20,23}';
SELECT count(*) from test__int WHERE a <@ '{73,23,20}';
SELECT count(*) from test__int WHERE a = '{73,23,20}';
SELECT count(*) from test__int WHERE a @@ '50&68';
SELECT count(*) from test__int WHERE a @> '{20,23}' or a @> '{50,68}';
SELECT count(*) from test__int WHERE a @@ '(20&23)|(50&68)';
SELECT count(*) from test__int WHERE a @@ '20 | !21';
SELECT count(*) from test__int WHERE a @@ '!20 & !21';
SELECT count(*) from test__int WHERE a @@ '!2733 & (2738 | 254)';

DROP INDEX text_idx;
CREATE INDEX text_idx on test__int using gist (a gist__intbig_ops(siglen = 0));
CREATE INDEX text_idx on test__int using gist (a gist__intbig_ops(siglen = 2025));
CREATE INDEX text_idx on test__int using gist (a gist__intbig_ops(siglen = 2024));

SELECT count(*) from test__int WHERE a && '{23,50}';
SELECT count(*) from test__int WHERE a @@ '23|50';
SELECT count(*) from test__int WHERE a @> '{23,50}';
SELECT count(*) from test__int WHERE a @@ '23&50';
SELECT count(*) from test__int WHERE a @> '{20,23}';
SELECT count(*) from test__int WHERE a <@ '{73,23,20}';
SELECT count(*) from test__int WHERE a = '{73,23,20}';
SELECT count(*) from test__int WHERE a @@ '50&68';
SELECT count(*) from test__int WHERE a @> '{20,23}' or a @> '{50,68}';
SELECT count(*) from test__int WHERE a @@ '(20&23)|(50&68)';
SELECT count(*) from test__int WHERE a @@ '20 | !21';
SELECT count(*) from test__int WHERE a @@ '!20 & !21';
SELECT count(*) from test__int WHERE a @@ '!2733 & (2738 | 254)';

DROP INDEX text_idx;
CREATE INDEX text_idx on test__int using gist ( a gist__intbig_ops );

SELECT count(*) from test__int WHERE a && '{23,50}';
SELECT count(*) from test__int WHERE a @@ '23|50';
SELECT count(*) from test__int WHERE a @> '{23,50}';
SELECT count(*) from test__int WHERE a @@ '23&50';
SELECT count(*) from test__int WHERE a @> '{20,23}';
SELECT count(*) from test__int WHERE a <@ '{73,23,20}';
SELECT count(*) from test__int WHERE a = '{73,23,20}';
SELECT count(*) from test__int WHERE a @@ '50&68';
SELECT count(*) from test__int WHERE a @> '{20,23}' or a @> '{50,68}';
SELECT count(*) from test__int WHERE a @@ '(20&23)|(50&68)';
SELECT count(*) from test__int WHERE a @@ '20 | !21';
SELECT count(*) from test__int WHERE a @@ '!20 & !21';
SELECT count(*) from test__int WHERE a @@ '!2733 & (2738 | 254)';

DROP INDEX text_idx;
CREATE INDEX text_idx on test__int using gin ( a gin__int_ops );

SELECT count(*) from test__int WHERE a && '{23,50}';
SELECT count(*) from test__int WHERE a @@ '23|50';
SELECT count(*) from test__int WHERE a @> '{23,50}';
SELECT count(*) from test__int WHERE a @@ '23&50';
SELECT count(*) from test__int WHERE a @> '{20,23}';
SELECT count(*) from test__int WHERE a <@ '{73,23,20}';
SELECT count(*) from test__int WHERE a = '{73,23,20}';
SELECT count(*) from test__int WHERE a @@ '50&68';
SELECT count(*) from test__int WHERE a @> '{20,23}' or a @> '{50,68}';
SELECT count(*) from test__int WHERE a @@ '(20&23)|(50&68)';
SELECT count(*) from test__int WHERE a @@ '20 | !21';
SELECT count(*) from test__int WHERE a @@ '!20 & !21';
SELECT count(*) from test__int WHERE a @@ '!2733 & (2738 | 254)';

DROP INDEX text_idx;

-- Repeat the same queries with an extended data set. The data set is the
--
-- 对扩展数据集重复相同的查询。数据集是
-- same that we used before, except that each element in the array is
--
-- 与我们之前使用的相同，只是数组中的每个元素是
-- repeated three times, offset by 1000 and 2000. For example, {1, 5}
--
-- 重复3次，偏移1000和2000。例如，{1, 5}
-- becomes {1, 1001, 2001, 5, 1005, 2005}.
--
-- 变为 {1, 1001, 2001, 5, 1005, 2005}。
--
-- That has proven to be unreasonably effective at exercising codepaths in
--
-- 事实证明，这在执行代码路径方面非常有效
-- core GiST code related to splitting parent pages, which is not covered by
--
-- 与拆分父页面相关的核心 GiST 代码，未涵盖
-- other tests. This is a bit out-of-place as the point is to test core GiST
--
-- 其他测试。这有点不合适，因为重点是测试核心 GiST
-- code rather than this extension, but there is no suitable GiST opclass in
--
-- 代码而不是这个扩展，但是没有合适的 GiST opclass
-- core that would reach the same codepaths.
--
-- 将达到相同代码路径的核心。
CREATE TABLE more__int AS SELECT
   -- Leave alone NULLs, empty arrays and the one row that we use to test
   --
   -- 不考虑 NULL、空数组和我们用来测试的一行
   -- equality; also skip INT_MAX
   --
   -- 平等;也跳过 INT_MAX
   CASE WHEN a IS NULL OR a = '{}' OR a = '{73,23,20}' THEN a ELSE
     (select array_agg(u) || array_agg(u + 1000) || array_agg(u + 2000)
      from unnest(a) u where u < 2000000000)
   END AS a, a as b
   FROM test__int;
CREATE INDEX ON more__int using gist (a gist__int_ops(numranges = 252));

SELECT count(*) from more__int WHERE a && '{23,50}';
SELECT count(*) from more__int WHERE a @@ '23|50';
SELECT count(*) from more__int WHERE a @> '{23,50}';
SELECT count(*) from more__int WHERE a @@ '23&50';
SELECT count(*) from more__int WHERE a @> '{20,23}';
SELECT count(*) from more__int WHERE a <@ '{73,23,20}';
SELECT count(*) from more__int WHERE a = '{73,23,20}';
SELECT count(*) from more__int WHERE a @@ '50&68';
SELECT count(*) from more__int WHERE a @> '{20,23}' or a @> '{50,68}';
SELECT count(*) from more__int WHERE a @@ '(20&23)|(50&68)';
SELECT count(*) from more__int WHERE a @@ '20 | !21';
SELECT count(*) from more__int WHERE a @@ '!20 & !21';
SELECT count(*) from test__int WHERE a @@ '!2733 & (2738 | 254)';


RESET enable_seqscan;

SELECT getdatabaseencoding() <> 'UTF8' AS skip_test \gset
\if :skip_test
\quit
\endif

-- Index 50 translations of the word "Mathematics"
--
-- 索引“数学”一词的 50 个翻译
CREATE TEMP TABLE mb (s text);
\copy mb from 'data/trgm_utf8.data'
CREATE INDEX ON mb USING gist(s gist_trgm_ops);

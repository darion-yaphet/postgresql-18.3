CREATE EXTENSION dict_xsyn;

-- default configuration - match first word and return it among with all synonyms
--
-- 默认配置 - 匹配第一个单词并将其与所有同义词一起返回
ALTER TEXT SEARCH DICTIONARY xsyn (RULES='xsyn_sample', KEEPORIG=true, MATCHORIG=true, KEEPSYNONYMS=true, MATCHSYNONYMS=false);

--lexize
SELECT ts_lexize('xsyn', 'supernova');
SELECT ts_lexize('xsyn', 'sn');
SELECT ts_lexize('xsyn', 'grb');

-- the same, but return only synonyms
--
-- 相同，但仅返回同义词
ALTER TEXT SEARCH DICTIONARY xsyn (RULES='xsyn_sample', KEEPORIG=false, MATCHORIG=true, KEEPSYNONYMS=true, MATCHSYNONYMS=false);
SELECT ts_lexize('xsyn', 'supernova');
SELECT ts_lexize('xsyn', 'sn');
SELECT ts_lexize('xsyn', 'grb');

-- match any word and return all words
--
-- 匹配任意单词并返回所有单词
ALTER TEXT SEARCH DICTIONARY xsyn (RULES='xsyn_sample', KEEPORIG=true, MATCHORIG=true, KEEPSYNONYMS=true, MATCHSYNONYMS=true);
SELECT ts_lexize('xsyn', 'supernova');
SELECT ts_lexize('xsyn', 'sn');
SELECT ts_lexize('xsyn', 'grb');

-- match any word and return all words except first one
--
-- 匹配任何单词并返回除第一个单词之外的所有单词
ALTER TEXT SEARCH DICTIONARY xsyn (RULES='xsyn_sample', KEEPORIG=false, MATCHORIG=true, KEEPSYNONYMS=true, MATCHSYNONYMS=true);
SELECT ts_lexize('xsyn', 'supernova');
SELECT ts_lexize('xsyn', 'sn');
SELECT ts_lexize('xsyn', 'grb');

-- match any synonym but not first word, and return first word instead
--
-- 匹配任何同义词但不匹配第一个单词，并返回第一个单词
ALTER TEXT SEARCH DICTIONARY xsyn (RULES='xsyn_sample', KEEPORIG=true, MATCHORIG=false, KEEPSYNONYMS=false, MATCHSYNONYMS=true);
SELECT ts_lexize('xsyn', 'supernova');
SELECT ts_lexize('xsyn', 'sn');
SELECT ts_lexize('xsyn', 'grb');

-- do not match or return anything
--
-- 不匹配或返回任何内容
ALTER TEXT SEARCH DICTIONARY xsyn (RULES='xsyn_sample', KEEPORIG=false, MATCHORIG=false, KEEPSYNONYMS=false, MATCHSYNONYMS=false);
SELECT ts_lexize('xsyn', 'supernova');
SELECT ts_lexize('xsyn', 'sn');
SELECT ts_lexize('xsyn', 'grb');

-- match any word but return nothing
--
-- 匹配任何单词但不返回任何内容
ALTER TEXT SEARCH DICTIONARY xsyn (RULES='xsyn_sample', KEEPORIG=false, MATCHORIG=true, KEEPSYNONYMS=false, MATCHSYNONYMS=true);
SELECT ts_lexize('xsyn', 'supernova');
SELECT ts_lexize('xsyn', 'sn');
SELECT ts_lexize('xsyn', 'grb');

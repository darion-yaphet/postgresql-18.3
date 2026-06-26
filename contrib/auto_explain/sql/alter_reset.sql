--
-- This tests resetting unknown custom GUCs with reserved prefixes.  There's
--
-- 此测试使用保留前缀重置未知的自定义 GUC。  有
-- nothing specific to auto_explain; this is just a convenient place to put
--
-- auto_explain 没有什么具体的内容；这只是一个方便放置的地方
-- this test.
--
-- 这个测试。
--

SELECT current_database() AS datname \gset
CREATE ROLE regress_ae_role;

ALTER DATABASE :"datname" SET auto_explain.bogus = 1;
ALTER ROLE regress_ae_role SET auto_explain.bogus = 1;
ALTER ROLE regress_ae_role IN DATABASE :"datname" SET auto_explain.bogus = 1;
ALTER SYSTEM SET auto_explain.bogus = 1;

LOAD 'auto_explain';

ALTER DATABASE :"datname" RESET auto_explain.bogus;
ALTER ROLE regress_ae_role RESET auto_explain.bogus;
ALTER ROLE regress_ae_role IN DATABASE :"datname" RESET auto_explain.bogus;
ALTER SYSTEM RESET auto_explain.bogus;

DROP ROLE regress_ae_role;

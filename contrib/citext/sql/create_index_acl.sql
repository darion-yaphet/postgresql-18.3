-- Each DefineIndex() ACL check uses either the original userid or the table
--
-- 每个 DefineIndex() ACL 检查使用原始用户 ID 或表
-- owner userid; see its header comment.  Here, confirm that DefineIndex()
--
-- 所有者用户 ID；请参阅其标题评论。  在这里，确认 DefineIndex()
-- uses its original userid where necessary.  The test works by creating
--
-- 必要时使用其原始用户 ID。  该测试通过创建
-- indexes that refer to as many sorts of objects as possible, with the table
--
-- 索引引用尽可能多的对象类型，以及表
-- owner having as few applicable privileges as possible.  (The privileges.sql
--
-- 所有者拥有尽可能少的适用特权。  （特权.sql
-- regress_sro_user tests look for the opposite defect; they confirm that
--
-- regress_sro_user 测试寻找相反的缺陷；他们证实
-- DefineIndex() uses the table owner userid where necessary.)
--
-- DefineIndex() 在必要时使用表所有者用户 ID。）

SET allow_in_place_tablespaces = true;
CREATE TABLESPACE regress_create_idx_tblspace LOCATION '';
RESET allow_in_place_tablespaces;

BEGIN;
CREATE ROLE regress_minimal;
CREATE SCHEMA s;
CREATE EXTENSION citext SCHEMA s;
-- Revoke all conceivably-relevant ACLs within the extension.  The system
--
-- 撤销扩展中所有可能相关的 ACL。  系统
-- doesn't check all these ACLs, but this will provide some coverage if that
--
-- 不会检查所有这些 ACL，但这将提供一些覆盖范围
-- ever changes.
--
-- 永远改变。
REVOKE ALL ON TYPE s.citext FROM PUBLIC;
REVOKE ALL ON FUNCTION s.citext_pattern_lt FROM PUBLIC;
REVOKE ALL ON FUNCTION s.citext_pattern_le FROM PUBLIC;
REVOKE ALL ON FUNCTION s.citext_eq FROM PUBLIC;
REVOKE ALL ON FUNCTION s.citext_pattern_ge FROM PUBLIC;
REVOKE ALL ON FUNCTION s.citext_pattern_gt FROM PUBLIC;
REVOKE ALL ON FUNCTION s.citext_pattern_cmp FROM PUBLIC;
-- Functions sufficient for making an index column that has the side effect of
--
-- 足以制作具有副作用的索引列的函数
-- changing search_path at expression planning time.
--
-- 在表达规划时更改 search_path。
CREATE FUNCTION public.setter() RETURNS bool VOLATILE
  LANGUAGE SQL AS $$SET search_path = s; SELECT true$$;
CREATE FUNCTION s.const() RETURNS bool IMMUTABLE
  LANGUAGE SQL AS $$SELECT public.setter()$$;
CREATE FUNCTION s.index_this_expr(s.citext, bool) RETURNS s.citext IMMUTABLE
  LANGUAGE SQL AS $$SELECT $1$$;
REVOKE ALL ON FUNCTION public.setter FROM PUBLIC;
REVOKE ALL ON FUNCTION s.const FROM PUBLIC;
REVOKE ALL ON FUNCTION s.index_this_expr FROM PUBLIC;
-- Even for an empty table, expression planning calls s.const & public.setter.
--
-- 即使对于空表，表达式规划也会调用 s.const 和 public.setter。
GRANT EXECUTE ON FUNCTION public.setter TO regress_minimal;
GRANT EXECUTE ON FUNCTION s.const TO regress_minimal;
-- Function for index predicate.
--
-- 索引谓词的函数。
CREATE FUNCTION s.index_row_if(s.citext) RETURNS bool IMMUTABLE
  LANGUAGE SQL AS $$SELECT $1 IS NOT NULL$$;
REVOKE ALL ON FUNCTION s.index_row_if FROM PUBLIC;
-- Even for an empty table, CREATE INDEX checks ii_Predicate permissions.
--
-- 即使对于空表，CREATE INDEX 也会检查 ii_Predicate 权限。
GRANT EXECUTE ON FUNCTION s.index_row_if TO regress_minimal;
-- Non-extension, non-function objects.
--
-- 非扩展、非功能对象。
CREATE COLLATION s.coll (LOCALE="C");
CREATE TABLE s.x (y s.citext);
ALTER TABLE s.x OWNER TO regress_minimal;
-- Empty-table DefineIndex()
--
-- 空表 DefineIndex()
CREATE UNIQUE INDEX u0rows ON s.x USING btree
  ((s.index_this_expr(y, s.const())) COLLATE s.coll s.citext_pattern_ops)
  TABLESPACE regress_create_idx_tblspace
  WHERE s.index_row_if(y);
ALTER TABLE s.x ADD CONSTRAINT e0rows EXCLUDE USING btree
  ((s.index_this_expr(y, s.const())) COLLATE s.coll WITH s.=)
  USING INDEX TABLESPACE regress_create_idx_tblspace
  WHERE (s.index_row_if(y));
-- Make the table nonempty.
--
-- 使表非空。
INSERT INTO s.x VALUES ('foo'), ('bar');
-- If the INSERT runs the planner on index expressions, a search_path change
--
-- 如果 INSERT 在索引表达式上运行规划器，则 search_path 会发生变化
-- survives.  As of 2022-06, the INSERT reuses a cached plan.  It does so even
--
-- 幸存下来。  截至 2022 年 6 月，INSERT 重用缓存计划。  甚至它也这样做
-- under debug_discard_caches, since each index is new-in-transaction.  If
--
-- 在 debug_discard_caches 下，因为每个索引都是事务中的新索引。  如果
-- future work changes a cache lifecycle, this RESET may become necessary.
--
-- 未来的工作改变了缓存生命周期，这个 RESET 可能变得必要。
RESET search_path;
-- For a nonempty table, owner needs permissions throughout ii_Expressions.
--
-- 对于非空表，所有者需要整个 ii_Expressions 的权限。
GRANT EXECUTE ON FUNCTION s.index_this_expr TO regress_minimal;
CREATE UNIQUE INDEX u2rows ON s.x USING btree
  ((s.index_this_expr(y, s.const())) COLLATE s.coll s.citext_pattern_ops)
  TABLESPACE regress_create_idx_tblspace
  WHERE s.index_row_if(y);
ALTER TABLE s.x ADD CONSTRAINT e2rows EXCLUDE USING btree
  ((s.index_this_expr(y, s.const())) COLLATE s.coll WITH s.=)
  USING INDEX TABLESPACE regress_create_idx_tblspace
  WHERE (s.index_row_if(y));
-- Shall not find s.coll via search_path, despite the s.const->public.setter
--
-- 不应通过 search_path 找到 s.coll，尽管 s.const->public.setter
-- call having set search_path=s during expression planning.  Suppress the
--
-- 在表达规划期间设置 search_path=s 进行调用。  抑制
-- message itself, which depends on the database encoding.
--
-- 消息本身，这取决于数据库编码。
\set VERBOSITY sqlstate
ALTER TABLE s.x ADD CONSTRAINT underqualified EXCLUDE USING btree
  ((s.index_this_expr(y, s.const())) COLLATE coll WITH s.=)
  USING INDEX TABLESPACE regress_create_idx_tblspace
  WHERE (s.index_row_if(y));
\set VERBOSITY default
ROLLBACK;

DROP TABLESPACE regress_create_idx_tblspace;

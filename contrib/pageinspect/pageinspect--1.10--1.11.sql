/* contrib/pageinspect/pageinspect--1.10--1.11.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
--
-- 抱怨脚本是否源自 psql，而不是通过 ALTER EXTENSION
\echo Use "ALTER EXTENSION pageinspect UPDATE TO '1.11'" to load this file. \quit

--
-- Functions that fetch relation pages must be PARALLEL RESTRICTED,
--
-- 获取关系页面的函数必须是并行限制的，
-- not PARALLEL SAFE, otherwise they will fail when run on a
--
-- 不是并行安全的，否则它们在运行时会失败
-- temporary table in a parallel worker process.
--
-- 并行工作进程中的临时表。
--

ALTER FUNCTION get_raw_page(text, int8) PARALLEL RESTRICTED;
ALTER FUNCTION get_raw_page(text, text, int8) PARALLEL RESTRICTED;
-- tuple_data_split must be restricted because it may fetch TOAST data.
--
-- tuple_data_split 必须受到限制，因为它可能会获取 TOAST 数据。
ALTER FUNCTION tuple_data_split(oid, bytea, integer, integer, text) PARALLEL RESTRICTED;
ALTER FUNCTION tuple_data_split(oid, bytea, integer, integer, text, bool) PARALLEL RESTRICTED;
-- heap_page_item_attrs must be restricted because it calls tuple_data_split.
--
-- heap_page_item_attrs 必须受到限制，因为它调用 tuple_data_split。
ALTER FUNCTION heap_page_item_attrs(bytea, regclass, bool) PARALLEL RESTRICTED;
ALTER FUNCTION heap_page_item_attrs(bytea, regclass) PARALLEL RESTRICTED;
ALTER FUNCTION bt_metap(text) PARALLEL RESTRICTED;
ALTER FUNCTION bt_page_stats(text, int8) PARALLEL RESTRICTED;
ALTER FUNCTION bt_page_items(text, int8) PARALLEL RESTRICTED;
ALTER FUNCTION hash_bitmap_info(regclass, int8) PARALLEL RESTRICTED;
-- brin_page_items might be parallel safe, because it seems to touch
--
-- brin_page_items 可能是并行安全的，因为它似乎接触
-- only index metadata, but I don't think there's a point in risking it.
--
-- 仅索引元数据，但我认为没有必要冒险。
-- Likewise for gist_page_items.
--
-- 对于 gist_page_items 也是如此。
ALTER FUNCTION brin_page_items(bytea, regclass) PARALLEL RESTRICTED;
ALTER FUNCTION gist_page_items(bytea, regclass) PARALLEL RESTRICTED;

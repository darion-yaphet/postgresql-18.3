
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Runs the specified query and returns the emitted server log.
#
# 运行指定的查询并返回发出的服务器日志。
# params is an optional hash mapping GUC names to values;
#
# params 是将 GUC 名称映射到值的可选哈希值；
# any such settings are transmitted to the backend via PGOPTIONS.
#
# 任何此类设置都会通过 PGOPTIONS 传输到后端。
sub query_log
{
	my ($node, $sql, $params) = @_;
	$params ||= {};

	local $ENV{PGOPTIONS} = join " ",
	  map { "-c $_=$params->{$_}" } keys %$params;

	my $log = $node->logfile();
	my $offset = -s $log;

	$node->safe_psql("postgres", $sql);

	return slurp_file($log, $offset);
}

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init(auth_extra => [ '--create-role' => 'regress_user1' ]);
$node->append_conf('postgresql.conf',
	"session_preload_libraries = 'auto_explain'");
$node->append_conf('postgresql.conf', "auto_explain.log_min_duration = 0");
$node->append_conf('postgresql.conf', "auto_explain.log_analyze = on");
$node->start;

# Simple query.
#
# 简单查询。
my $log_contents = query_log($node, "SELECT * FROM pg_class;");

like(
	$log_contents,
	qr/Query Text: SELECT \* FROM pg_class;/,
	"query text logged, text mode");

unlike(
	$log_contents,
	qr/Query Parameters:/,
	"no query parameters logged when none, text mode");

like(
	$log_contents,
	qr/Seq Scan on pg_class/,
	"sequential scan logged, text mode");

# Prepared query.
#
# 准备好的查询。
$log_contents = query_log($node,
	q{PREPARE get_proc(name) AS SELECT * FROM pg_proc WHERE proname = $1; EXECUTE get_proc('int4pl');}
);

like(
	$log_contents,
	qr/Query Text: PREPARE get_proc\(name\) AS SELECT \* FROM pg_proc WHERE proname = \$1;/,
	"prepared query text logged, text mode");

like(
	$log_contents,
	qr/Query Parameters: \$1 = 'int4pl'/,
	"query parameters logged, text mode");

like(
	$log_contents,
	qr/Index Scan using pg_proc_proname_args_nsp_index on pg_proc/,
	"index scan logged, text mode");


# Prepared query with truncated parameters.
#
# 已准备好带有截断参数的查询。
$log_contents = query_log(
	$node,
	q{PREPARE get_type(name) AS SELECT * FROM pg_type WHERE typname = $1; EXECUTE get_type('float8');},
	{ "auto_explain.log_parameter_max_length" => 3 });

like(
	$log_contents,
	qr/Query Text: PREPARE get_type\(name\) AS SELECT \* FROM pg_type WHERE typname = \$1;/,
	"prepared query text logged, text mode");

like(
	$log_contents,
	qr/Query Parameters: \$1 = 'flo\.\.\.'/,
	"query parameters truncated, text mode");

# Prepared query with parameter logging disabled.
#
# 已准备好的查询已禁用参数日志记录。
$log_contents = query_log(
	$node,
	q{PREPARE get_type(name) AS SELECT * FROM pg_type WHERE typname = $1; EXECUTE get_type('float8');},
	{ "auto_explain.log_parameter_max_length" => 0 });

like(
	$log_contents,
	qr/Query Text: PREPARE get_type\(name\) AS SELECT \* FROM pg_type WHERE typname = \$1;/,
	"prepared query text logged, text mode");

unlike(
	$log_contents,
	qr/Query Parameters:/,
	"query parameters not logged when disabled, text mode");

# Query Identifier.
#
# 查询标识符。
# Logging enabled.
#
# 日志记录已启用。
$log_contents = query_log(
	$node,
	"SELECT * FROM pg_class;",
	{
		"auto_explain.log_verbose" => "on",
		"compute_query_id" => "on"
	});

like(
	$log_contents,
	qr/Query Identifier:/,
	"query identifier logged with compute_query_id=on, text mode");

# Logging disabled.
#
# 禁用日志记录。
$log_contents = query_log(
	$node,
	"SELECT * FROM pg_class;",
	{
		"auto_explain.log_verbose" => "on",
		"compute_query_id" => "regress"
	});

unlike(
	$log_contents,
	qr/Query Identifier:/,
	"query identifier not logged with compute_query_id=regress, text mode");

# JSON format.
#
# JSON 格式。
$log_contents = query_log(
	$node,
	"SELECT * FROM pg_class;",
	{ "auto_explain.log_format" => "json" });

like(
	$log_contents,
	qr/"Query Text": "SELECT \* FROM pg_class;"/,
	"query text logged, json mode");

unlike(
	$log_contents,
	qr/"Query Parameters":/,
	"query parameters not logged when none, json mode");

like(
	$log_contents,
	qr/"Node Type": "Seq Scan"[^}]*"Relation Name": "pg_class"/s,
	"sequential scan logged, json mode");

# Prepared query in JSON format.
#
# 准备好的 JSON 格式的查询。
$log_contents = query_log(
	$node,
	q{PREPARE get_class(name) AS SELECT * FROM pg_class WHERE relname = $1; EXECUTE get_class('pg_class');},
	{ "auto_explain.log_format" => "json" });

like(
	$log_contents,
	qr/"Query Text": "PREPARE get_class\(name\) AS SELECT \* FROM pg_class WHERE relname = \$1;"/,
	"prepared query text logged, json mode");

like(
	$log_contents,
	qr/"Node Type": "Index Scan"[^}]*"Index Name": "pg_class_relname_nsp_index"/s,
	"index scan logged, json mode");

# Check that PGC_SUSET parameters can be set by non-superuser if granted,
#
# 检查 PGC_SUSET 参数是否可以由非超级用户设置（如果授予），
# otherwise not
#
# 否则不

$node->safe_psql(
	"postgres", q{
CREATE USER regress_user1;
GRANT SET ON PARAMETER auto_explain.log_format TO regress_user1;
});

{
	local $ENV{PGUSER} = "regress_user1";

	$log_contents = query_log(
		$node,
		"SELECT * FROM pg_database;",
		{ "auto_explain.log_format" => "json" });

	like(
		$log_contents,
		qr/"Query Text": "SELECT \* FROM pg_database;"/,
		"query text logged, json mode selected by non-superuser");

	$log_contents = query_log(
		$node,
		"SELECT * FROM pg_database;",
		{ "auto_explain.log_level" => "log" });

	like(
		$log_contents,
		qr/WARNING: ( 42501:)? permission denied to set parameter "auto_explain\.log_level"/,
		"permission failure logged");

}    # end queries run as regress_user1

$node->safe_psql(
	"postgres", q{
REVOKE SET ON PARAMETER auto_explain.log_format FROM regress_user1;
DROP USER regress_user1;
});

# Test pg_get_loaded_modules() function.  This function is particularly
#
# 测试 pg_get_loaded_modules() 函数。  这个功能特别
# useful for modules with no SQL presence, such as auto_explain.
#
# 对于不存在 SQL 的模块很有用，例如 auto_explain。

my $res = $node->safe_psql(
	"postgres", q{
SELECT module_name,
       version = current_setting('server_version') as version_ok,
       regexp_replace(file_name, '\..*', '') as file_name_stripped
FROM pg_get_loaded_modules()
WHERE module_name = 'auto_explain';
});
like($res, qr/^auto_explain\|t\|auto_explain$/, "pg_get_loaded_modules() ok");

done_testing();

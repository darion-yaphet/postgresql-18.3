%{

/*#define YYDEBUG 1*/
/*-------------------------------------------------------------------------
 *
 * gram.y
 *	  POSTGRESQL BISON rules/actions
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/parser/gram.y
 *
 * HISTORY
 *	  AUTHOR			DATE			MAJOR EVENT
 *	  Andrew Yu			Sept, 1994		POSTQUEL to SQL conversion
 *	  Andrew Yu			Oct, 1994		lispy code conversion
 *
 * NOTES
 *	  CAPITALS are used to represent terminal symbols.
 *	  non-capitals are used to represent non-terminals.
 *
 *	  In general, nothing in this file should initiate database accesses
 *	  nor depend on changeable state (such as SET variables).  If you do
 *	  database accesses, your code will fail when we have aborted the
 *	  current transaction and are just parsing commands to find the next
 *	  ROLLBACK or COMMIT.  If you make use of SET variables, then you
 *	  will do the wrong thing in multi-query strings like this:
 *			SET constraint_exclusion TO off; SELECT * FROM foo;
 *	  because the entire string is parsed by gram.y before the SET gets
 *	  executed.  Anything that depends on the database or changeable state
 *	  should be handled during parse analysis so that it happens at the
 *	  right time not the wrong time.
 *
 * WARNINGS
 *	  If you use a list, make sure the datum is a node so that the printing
 *	  routines work.
 *
 *	  Sometimes we assign constants to makeStrings. Make sure we don't free
 *	  those.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <limits.h>

#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_am.h"
#include "catalog/pg_trigger.h"
#include "commands/defrem.h"
#include "commands/trigger.h"
#include "gramparse.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/parser.h"
#include "utils/datetime.h"
#include "utils/xml.h"


/*
 * Location tracking support.  Unlike bison's default, we only want
 * to track the start position not the end position of each nonterminal.
 * Nonterminals that reduce to empty receive position "-1".  Since a
 * production's leading RHS nonterminal(s) may have reduced to empty,
 * we have to scan to find the first one that's not -1.
 * 位置跟踪支持。与 bison 的默认行为不同，我们只想跟踪每个非终结符的起始位置，而不是结束位置。规约为空的非终结符接收位置 "-1"。由于产生式的首个右侧（RHS）非终结符可能已规约为空，我们必须扫描以找到第一个不是 -1 的终结符/非终结符。
 */
#define YYLLOC_DEFAULT(Current, Rhs, N) \
	do { \
		(Current) = (-1); \
		for (int _i = 1; _i <= (N); _i++) \
		{ \
			if ((Rhs)[_i] >= 0) \
			{ \
				(Current) = (Rhs)[_i]; \
				break; \
			} \
		} \
	} while (0)

/*
 * Bison doesn't allocate anything that needs to live across parser calls,
 * so we can easily have it use palloc instead of malloc.  This prevents
 * memory leaks if we error out during parsing.
 * Bison 不分配任何需要跨解析器调用生存的对象，因此我们可以很容易地让它使用 palloc 而不是 malloc。这可以防止我们在解析过程中出错时发生内存泄漏。
 */
#define YYMALLOC palloc
#define YYFREE   pfree

/* Private struct for the result of privilege_target production - 用于 privilege_target 产生式结果的私有结构体 */
typedef struct PrivTarget
{
	GrantTargetType targtype;
	ObjectType	objtype;
	List	   *objs;
} PrivTarget;

/* Private struct for the result of import_qualification production - 用于 import_qualification 产生式结果的私有结构体 */
typedef struct ImportQual
{
	ImportForeignSchemaType type;
	List	   *table_names;
} ImportQual;

/* Private struct for the result of select_limit & limit_clause productions - 用于 select_limit & limit_clause 产生式结果的私有结构体 */
typedef struct SelectLimit
{
	Node	   *limitOffset;
	Node	   *limitCount;
	LimitOption limitOption;	/* indicates presence of WITH TIES - 指示存在 WITH TIES */
	ParseLoc	offsetLoc;		/* location of OFFSET token, if present - OFFSET Token 的位置（如果存在） */
	ParseLoc	countLoc;		/* location of LIMIT/FETCH token, if present - LIMIT/FETCH Token 的位置（如果存在） */
	ParseLoc	optionLoc;		/* location of WITH TIES, if present - WITH TIES 的位置（如果存在） */
} SelectLimit;

/* Private struct for the result of group_clause production - 用于 group_clause 产生式结果的私有结构体 */
typedef struct GroupClause
{
	bool		distinct;
	List	   *list;
} GroupClause;

/* Private structs for the result of key_actions and key_action productions - 用于 key_actions 和 key_action 产生式结果的私有结构体 */
typedef struct KeyAction
{
	char		action;
	List	   *cols;
} KeyAction;

typedef struct KeyActions
{
	KeyAction *updateAction;
	KeyAction *deleteAction;
} KeyActions;

/* ConstraintAttributeSpec yields an integer bitmask of these flags: - ConstraintAttributeSpec 产生这些标志的整数位掩码： */
#define CAS_NOT_DEFERRABLE			0x01
#define CAS_DEFERRABLE				0x02
#define CAS_INITIALLY_IMMEDIATE		0x04
#define CAS_INITIALLY_DEFERRED		0x08
#define CAS_NOT_VALID				0x10
#define CAS_NO_INHERIT				0x20
#define CAS_NOT_ENFORCED			0x40
#define CAS_ENFORCED				0x80


#define parser_yyerror(msg)  scanner_yyerror(msg, yyscanner)
#define parser_errposition(pos)  scanner_errposition(pos, yyscanner)

static void base_yyerror(YYLTYPE *yylloc, core_yyscan_t yyscanner,
						 const char *msg);
static RawStmt *makeRawStmt(Node *stmt, int stmt_location);
static void updateRawStmtEnd(RawStmt *rs, int end_location);
static Node *makeColumnRef(char *colname, List *indirection,
						   int location, core_yyscan_t yyscanner);
static Node *makeTypeCast(Node *arg, TypeName *typename, int location);
static Node *makeStringConstCast(char *str, int location, TypeName *typename);
static Node *makeIntConst(int val, int location);
static Node *makeFloatConst(char *str, int location);
static Node *makeBoolAConst(bool state, int location);
static Node *makeBitStringConst(char *str, int location);
static Node *makeNullAConst(int location);
static Node *makeAConst(Node *v, int location);
static RoleSpec *makeRoleSpec(RoleSpecType type, int location);
static void check_qualified_name(List *names, core_yyscan_t yyscanner);
static List *check_func_name(List *names, core_yyscan_t yyscanner);
static List *check_indirection(List *indirection, core_yyscan_t yyscanner);
static List *extractArgTypes(List *parameters);
static List *extractAggrArgTypes(List *aggrargs);
static List *makeOrderedSetArgs(List *directargs, List *orderedargs,
								core_yyscan_t yyscanner);
static void insertSelectOptions(SelectStmt *stmt,
								List *sortClause, List *lockingClause,
								SelectLimit *limitClause,
								WithClause *withClause,
								core_yyscan_t yyscanner);
static Node *makeSetOp(SetOperation op, bool all, Node *larg, Node *rarg);
static Node *doNegate(Node *n, int location);
static void doNegateFloat(Float *v);
static Node *makeAndExpr(Node *lexpr, Node *rexpr, int location);
static Node *makeOrExpr(Node *lexpr, Node *rexpr, int location);
static Node *makeNotExpr(Node *expr, int location);
static Node *makeAArrayExpr(List *elements, int location, int end_location);
static Node *makeSQLValueFunction(SQLValueFunctionOp op, int32 typmod,
								  int location);
static Node *makeXmlExpr(XmlExprOp op, char *name, List *named_args,
						 List *args, int location);
static List *mergeTableFuncParameters(List *func_args, List *columns, core_yyscan_t yyscanner);
static TypeName *TableFuncTypeName(List *columns);
static RangeVar *makeRangeVarFromAnyName(List *names, int position, core_yyscan_t yyscanner);
static RangeVar *makeRangeVarFromQualifiedName(char *name, List *namelist, int location,
											   core_yyscan_t yyscanner);
static void SplitColQualList(List *qualList,
							 List **constraintList, CollateClause **collClause,
							 core_yyscan_t yyscanner);
static void processCASbits(int cas_bits, int location, const char *constrType,
			   bool *deferrable, bool *initdeferred, bool *is_enforced,
			   bool *not_valid, bool *no_inherit, core_yyscan_t yyscanner);
static PartitionStrategy parsePartitionStrategy(char *strategy, int location,
												core_yyscan_t yyscanner);
static void preprocess_pubobj_list(List *pubobjspec_list,
								   core_yyscan_t yyscanner);
static Node *makeRecursiveViewSelect(char *relname, List *aliases, Node *query);

%}

%pure-parser
%expect 0
%name-prefix="base_yy"
%locations

%parse-param {core_yyscan_t yyscanner}
%lex-param   {core_yyscan_t yyscanner}

%union
{
	core_YYSTYPE core_yystype;
	/* these fields must match core_YYSTYPE: - 这些字段必须与 core_YYSTYPE 匹配： */
	int			ival;
	char	   *str;
	const char *keyword;

	char		chr;
	bool		boolean;
	JoinType	jtype;
	DropBehavior dbehavior;
	OnCommitAction oncommit;
	List	   *list;
	Node	   *node;
	ObjectType	objtype;
	TypeName   *typnam;
	FunctionParameter *fun_param;
	FunctionParameterMode fun_param_mode;
	ObjectWithArgs *objwithargs;
	DefElem	   *defelt;
	SortBy	   *sortby;
	WindowDef  *windef;
	JoinExpr   *jexpr;
	IndexElem  *ielem;
	StatsElem  *selem;
	Alias	   *alias;
	RangeVar   *range;
	IntoClause *into;
	WithClause *with;
	InferClause	*infer;
	OnConflictClause *onconflict;
	A_Indices  *aind;
	ResTarget  *target;
	struct PrivTarget *privtarget;
	AccessPriv *accesspriv;
	struct ImportQual *importqual;
	InsertStmt *istmt;
	VariableSetStmt *vsetstmt;
	PartitionElem *partelem;
	PartitionSpec *partspec;
	PartitionBoundSpec *partboundspec;
	RoleSpec   *rolespec;
	PublicationObjSpec *publicationobjectspec;
	struct SelectLimit *selectlimit;
	SetQuantifier setquantifier;
	struct GroupClause *groupclause;
	MergeMatchKind mergematch;
	MergeWhenClause *mergewhen;
	struct KeyActions *keyactions;
	struct KeyAction *keyaction;
	ReturningClause *retclause;
	ReturningOptionKind retoptionkind;
}

%type <node>	stmt toplevel_stmt schema_stmt routine_body_stmt
		AlterEventTrigStmt AlterCollationStmt
		AlterDatabaseStmt AlterDatabaseSetStmt AlterDomainStmt AlterEnumStmt
		AlterFdwStmt AlterForeignServerStmt AlterGroupStmt
		AlterObjectDependsStmt AlterObjectSchemaStmt AlterOwnerStmt
		AlterOperatorStmt AlterTypeStmt AlterSeqStmt AlterSystemStmt AlterTableStmt
		AlterTblSpcStmt AlterExtensionStmt AlterExtensionContentsStmt
		AlterCompositeTypeStmt AlterUserMappingStmt
		AlterRoleStmt AlterRoleSetStmt AlterPolicyStmt AlterStatsStmt
		AlterDefaultPrivilegesStmt DefACLAction
		AnalyzeStmt CallStmt ClosePortalStmt ClusterStmt CommentStmt
		ConstraintsSetStmt CopyStmt CreateAsStmt CreateCastStmt
		CreateDomainStmt CreateExtensionStmt CreateGroupStmt CreateOpClassStmt
		CreateOpFamilyStmt AlterOpFamilyStmt CreatePLangStmt
		CreateSchemaStmt CreateSeqStmt CreateStmt CreateStatsStmt CreateTableSpaceStmt
		CreateFdwStmt CreateForeignServerStmt CreateForeignTableStmt
		CreateAssertionStmt CreateTransformStmt CreateTrigStmt CreateEventTrigStmt
		CreateUserStmt CreateUserMappingStmt CreateRoleStmt CreatePolicyStmt
		CreatedbStmt DeclareCursorStmt DefineStmt DeleteStmt DiscardStmt DoStmt
		DropOpClassStmt DropOpFamilyStmt DropStmt
		DropCastStmt DropRoleStmt
		DropdbStmt DropTableSpaceStmt
		DropTransformStmt
		DropUserMappingStmt ExplainStmt FetchStmt
		GrantStmt GrantRoleStmt ImportForeignSchemaStmt IndexStmt InsertStmt
		ListenStmt LoadStmt LockStmt MergeStmt NotifyStmt ExplainableStmt PreparableStmt
		CreateFunctionStmt AlterFunctionStmt ReindexStmt RemoveAggrStmt
		RemoveFuncStmt RemoveOperStmt RenameStmt ReturnStmt RevokeStmt RevokeRoleStmt
		RuleActionStmt RuleActionStmtOrEmpty RuleStmt
		SecLabelStmt SelectStmt TransactionStmt TransactionStmtLegacy TruncateStmt
		UnlistenStmt UpdateStmt VacuumStmt
		VariableResetStmt VariableSetStmt VariableShowStmt
		ViewStmt CheckPointStmt CreateConversionStmt
		DeallocateStmt PrepareStmt ExecuteStmt
		DropOwnedStmt ReassignOwnedStmt
		AlterTSConfigurationStmt AlterTSDictionaryStmt
		CreateMatViewStmt RefreshMatViewStmt CreateAmStmt
		CreatePublicationStmt AlterPublicationStmt
		CreateSubscriptionStmt AlterSubscriptionStmt DropSubscriptionStmt

%type <node>	select_no_parens select_with_parens select_clause
				simple_select values_clause
				PLpgSQL_Expr PLAssignStmt

%type <str>			opt_single_name
%type <list>		opt_qualified_name
%type <boolean>		opt_concurrently
%type <dbehavior>	opt_drop_behavior

%type <node>	alter_column_default opclass_item opclass_drop alter_using
%type <ival>	add_drop opt_asc_desc opt_nulls_order

%type <node>	alter_table_cmd alter_type_cmd opt_collate_clause
	   replica_identity partition_cmd index_partition_cmd
%type <list>	alter_table_cmds alter_type_cmds
%type <list>    alter_identity_column_option_list
%type <defelt>  alter_identity_column_option
%type <node>	set_statistics_value
%type <str>		set_access_method_name

%type <list>	createdb_opt_list createdb_opt_items copy_opt_list
				transaction_mode_list
				create_extension_opt_list alter_extension_opt_list
%type <defelt>	createdb_opt_item copy_opt_item
				transaction_mode_item
				create_extension_opt_item alter_extension_opt_item

%type <ival>	opt_lock lock_type cast_context
%type <str>		utility_option_name
%type <defelt>	utility_option_elem
%type <list>	utility_option_list
%type <node>	utility_option_arg
%type <defelt>	drop_option
%type <boolean>	opt_or_replace opt_no
				opt_grant_grant_option
				opt_nowait opt_if_exists opt_with_data
				opt_transaction_chain
%type <list>	grant_role_opt_list
%type <defelt>	grant_role_opt
%type <node>	grant_role_opt_value
%type <ival>	opt_nowait_or_skip

%type <list>	OptRoleList AlterOptRoleList
%type <defelt>	CreateOptRoleElem AlterOptRoleElem

%type <str>		opt_type
%type <str>		foreign_server_version opt_foreign_server_version
%type <str>		opt_in_database

%type <str>		parameter_name
%type <list>	OptSchemaEltList parameter_name_list

%type <chr>		am_type

%type <boolean> TriggerForSpec TriggerForType
%type <ival>	TriggerActionTime
%type <list>	TriggerEvents TriggerOneEvent
%type <node>	TriggerFuncArg
%type <node>	TriggerWhen
%type <str>		TransitionRelName
%type <boolean>	TransitionRowOrTable TransitionOldOrNew
%type <node>	TriggerTransition

%type <list>	event_trigger_when_list event_trigger_value_list
%type <defelt>	event_trigger_when_item
%type <chr>		enable_trigger

%type <str>		copy_file_name
				access_method_clause attr_name
				table_access_method_clause name cursor_name file_name
				cluster_index_specification

%type <list>	func_name handler_name qual_Op qual_all_Op subquery_Op
				opt_inline_handler opt_validator validator_clause
				opt_collate

%type <range>	qualified_name insert_target OptConstrFromTable

%type <str>		all_Op MathOp

%type <str>		row_security_cmd RowSecurityDefaultForCmd
%type <boolean> RowSecurityDefaultPermissive
%type <node>	RowSecurityOptionalWithCheck RowSecurityOptionalExpr
%type <list>	RowSecurityDefaultToRole RowSecurityOptionalToRole

%type <str>		iso_level opt_encoding
%type <rolespec> grantee
%type <list>	grantee_list
%type <accesspriv> privilege
%type <list>	privileges privilege_list
%type <privtarget> privilege_target
%type <objwithargs> function_with_argtypes aggregate_with_argtypes operator_with_argtypes
%type <list>	function_with_argtypes_list aggregate_with_argtypes_list operator_with_argtypes_list
%type <ival>	defacl_privilege_target
%type <defelt>	DefACLOption
%type <list>	DefACLOptionList
%type <ival>	import_qualification_type
%type <importqual> import_qualification
%type <node>	vacuum_relation
%type <selectlimit> opt_select_limit select_limit limit_clause

%type <list>	parse_toplevel stmtmulti routine_body_stmt_list
				OptTableElementList TableElementList OptInherit definition
				OptTypedTableElementList TypedTableElementList
				reloptions opt_reloptions
				OptWith opt_definition func_args func_args_list
				func_args_with_defaults func_args_with_defaults_list
				aggr_args aggr_args_list
				func_as createfunc_opt_list opt_createfunc_opt_list alterfunc_opt_list
				old_aggr_definition old_aggr_list
				oper_argtypes RuleActionList RuleActionMulti
				opt_column_list columnList opt_name_list
				sort_clause opt_sort_clause sortby_list index_params
				stats_params
				opt_include opt_c_include index_including_params
				name_list role_list from_clause from_list opt_array_bounds
				qualified_name_list any_name any_name_list type_name_list
				any_operator expr_list attrs
				distinct_clause opt_distinct_clause
				target_list opt_target_list insert_column_list set_target_list
				merge_values_clause
				set_clause_list set_clause
				def_list operator_def_list indirection opt_indirection
				reloption_list TriggerFuncArgs opclass_item_list opclass_drop_list
				opclass_purpose opt_opfamily transaction_mode_list_or_empty
				OptTableFuncElementList TableFuncElementList opt_type_modifiers
				prep_type_clause
				execute_param_clause using_clause
				returning_with_clause returning_options
				opt_enum_val_list enum_val_list table_func_column_list
				create_generic_options alter_generic_options
				relation_expr_list dostmt_opt_list
				transform_element_list transform_type_list
				TriggerTransitions TriggerReferencing
				vacuum_relation_list opt_vacuum_relation_list
				drop_option_list pub_obj_list

%type <retclause> returning_clause
%type <node>	returning_option
%type <retoptionkind> returning_option_kind
%type <node>	opt_routine_body
%type <groupclause> group_clause
%type <list>	group_by_list
%type <node>	group_by_item empty_grouping_set rollup_clause cube_clause
%type <node>	grouping_sets_clause

%type <list>	opt_fdw_options fdw_options
%type <defelt>	fdw_option

%type <range>	OptTempTableName
%type <into>	into_clause create_as_target create_mv_target

%type <defelt>	createfunc_opt_item common_func_opt_item dostmt_opt_item
%type <fun_param> func_arg func_arg_with_default table_func_column aggr_arg
%type <fun_param_mode> arg_class
%type <typnam>	func_return func_type

%type <boolean>  opt_trusted opt_restart_seqs
%type <ival>	 OptTemp
%type <ival>	 OptNoLog
%type <oncommit> OnCommitOption

%type <ival>	for_locking_strength
%type <node>	for_locking_item
%type <list>	for_locking_clause opt_for_locking_clause for_locking_items
%type <list>	locked_rels_list
%type <setquantifier> set_quantifier

%type <node>	join_qual
%type <jtype>	join_type

%type <list>	extract_list overlay_list position_list
%type <list>	substr_list trim_list
%type <list>	opt_interval interval_second
%type <str>		unicode_normal_form

%type <boolean> opt_instead
%type <boolean> opt_unique opt_verbose opt_full
%type <boolean> opt_freeze opt_analyze opt_default
%type <defelt>	opt_binary copy_delimiter

%type <boolean> copy_from opt_program

%type <ival>	event cursor_options opt_hold opt_set_data
%type <objtype>	object_type_any_name object_type_name object_type_name_on_any_name
				drop_type_name

%type <node>	fetch_args select_limit_value
				offset_clause select_offset_value
				select_fetch_first_value I_or_F_const
%type <ival>	row_or_rows first_or_next

%type <list>	OptSeqOptList SeqOptList OptParenthesizedSeqOptList
%type <defelt>	SeqOptElem

%type <istmt>	insert_rest
%type <infer>	opt_conf_expr
%type <onconflict> opt_on_conflict
%type <mergewhen>	merge_insert merge_update merge_delete

%type <mergematch> merge_when_tgt_matched merge_when_tgt_not_matched
%type <node>	merge_when_clause opt_merge_when_condition
%type <list>	merge_when_list

%type <vsetstmt> generic_set set_rest set_rest_more generic_reset reset_rest
				 SetResetClause FunctionSetResetClause

%type <node>	TableElement TypedTableElement ConstraintElem DomainConstraintElem TableFuncElement
%type <node>	columnDef columnOptions optionalPeriodName
%type <defelt>	def_elem reloption_elem old_aggr_elem operator_def_elem
%type <node>	def_arg columnElem where_clause where_or_current_clause
				a_expr b_expr c_expr AexprConst indirection_el opt_slice_bound
				columnref having_clause func_table xmltable array_expr
				OptWhereClause operator_def_arg
%type <list>	opt_column_and_period_list
%type <list>	rowsfrom_item rowsfrom_list opt_col_def_list
%type <boolean> opt_ordinality opt_without_overlaps
%type <list>	ExclusionConstraintList ExclusionConstraintElem
%type <list>	func_arg_list func_arg_list_opt
%type <node>	func_arg_expr
%type <list>	row explicit_row implicit_row type_list array_expr_list
%type <node>	case_expr case_arg when_clause case_default
%type <list>	when_clause_list
%type <node>	opt_search_clause opt_cycle_clause
%type <ival>	sub_type opt_materialized
%type <node>	NumericOnly
%type <list>	NumericOnly_list
%type <alias>	alias_clause opt_alias_clause opt_alias_clause_for_join_using
%type <list>	func_alias_clause
%type <sortby>	sortby
%type <ielem>	index_elem index_elem_options
%type <selem>	stats_param
%type <node>	table_ref
%type <jexpr>	joined_table
%type <range>	relation_expr
%type <range>	extended_relation_expr
%type <range>	relation_expr_opt_alias
%type <node>	tablesample_clause opt_repeatable_clause
%type <target>	target_el set_target insert_column_item

%type <str>		generic_option_name
%type <node>	generic_option_arg
%type <defelt>	generic_option_elem alter_generic_option_elem
%type <list>	generic_option_list alter_generic_option_list

%type <ival>	reindex_target_relation reindex_target_all
%type <list>	opt_reindex_option_list

%type <node>	copy_generic_opt_arg copy_generic_opt_arg_list_item
%type <defelt>	copy_generic_opt_elem
%type <list>	copy_generic_opt_list copy_generic_opt_arg_list
%type <list>	copy_options

%type <typnam>	Typename SimpleTypename ConstTypename
				GenericType Numeric opt_float JsonType
				Character ConstCharacter
				CharacterWithLength CharacterWithoutLength
				ConstDatetime ConstInterval
				Bit ConstBit BitWithLength BitWithoutLength
%type <str>		character
%type <str>		extract_arg
%type <boolean> opt_varying opt_timezone opt_no_inherit

%type <ival>	Iconst SignedIconst
%type <str>		Sconst comment_text notify_payload
%type <str>		RoleId opt_boolean_or_string
%type <list>	var_list
%type <str>		ColId ColLabel BareColLabel
%type <str>		NonReservedWord NonReservedWord_or_Sconst
%type <str>		var_name type_function_name param_name
%type <str>		createdb_opt_name plassign_target
%type <node>	var_value zone_value
%type <rolespec> auth_ident RoleSpec opt_granted_by
%type <publicationobjectspec> PublicationObjSpec

%type <keyword> unreserved_keyword type_func_name_keyword
%type <keyword> col_name_keyword reserved_keyword
%type <keyword> bare_label_keyword

%type <node>	DomainConstraint TableConstraint TableLikeClause
%type <ival>	TableLikeOptionList TableLikeOption
%type <str>		column_compression opt_column_compression column_storage opt_column_storage
%type <list>	ColQualList
%type <node>	ColConstraint ColConstraintElem ConstraintAttr
%type <ival>	key_match
%type <keyaction> key_delete key_update key_action
%type <keyactions> key_actions
%type <ival>	ConstraintAttributeSpec ConstraintAttributeElem
%type <str>		ExistingIndex

%type <list>	constraints_set_list
%type <boolean> constraints_set_mode
%type <str>		OptTableSpace OptConsTableSpace
%type <rolespec> OptTableSpaceOwner
%type <ival>	opt_check_option

%type <str>		opt_provider security_label

%type <target>	xml_attribute_el
%type <list>	xml_attribute_list xml_attributes
%type <node>	xml_root_version opt_xml_root_standalone
%type <node>	xmlexists_argument
%type <ival>	document_or_content
%type <boolean>	xml_indent_option xml_whitespace_option
%type <list>	xmltable_column_list xmltable_column_option_list
%type <node>	xmltable_column_el
%type <defelt>	xmltable_column_option_el
%type <list>	xml_namespace_list
%type <target>	xml_namespace_el

%type <node>	func_application func_expr_common_subexpr
%type <node>	func_expr func_expr_windowless
%type <node>	common_table_expr
%type <with>	with_clause opt_with_clause
%type <list>	cte_list

%type <list>	within_group_clause
%type <node>	filter_clause
%type <list>	window_clause window_definition_list opt_partition_clause
%type <windef>	window_definition over_clause window_specification
				opt_frame_clause frame_extent frame_bound
%type <ival>	opt_window_exclusion_clause
%type <str>		opt_existing_window_name
%type <boolean> opt_if_not_exists
%type <boolean> opt_unique_null_treatment
%type <ival>	generated_when override_kind opt_virtual_or_stored
%type <partspec>	PartitionSpec OptPartitionSpec
%type <partelem>	part_elem
%type <list>		part_params
%type <partboundspec> PartitionBoundSpec
%type <list>		hash_partbound
%type <defelt>		hash_partbound_elem

%type <node>	json_format_clause
				json_format_clause_opt
				json_value_expr
				json_returning_clause_opt
				json_name_and_value
				json_aggregate_func
				json_argument
				json_behavior
				json_on_error_clause_opt
				json_table
				json_table_column_definition
				json_table_column_path_clause_opt
%type <list>	json_name_and_value_list
				json_value_expr_list
				json_array_aggregate_order_by_clause_opt
				json_arguments
				json_behavior_clause_opt
				json_passing_clause_opt
				json_table_column_definition_list
%type <str>		json_table_path_name_opt
%type <ival>	json_behavior_type
				json_predicate_type_constraint
				json_quotes_clause_opt
				json_wrapper_behavior
%type <boolean>	json_key_uniqueness_constraint_opt
				json_object_constructor_null_clause_opt
				json_array_constructor_null_clause_opt


/*
 * Non-keyword token types.  These are hard-wired into the "flex" lexer.
 * They must be listed first so that their numeric codes do not depend on
 * the set of keywords.  PL/pgSQL depends on this so that it can share the
 * same lexer.  If you add/change tokens here, fix PL/pgSQL to match!
 *
 * UIDENT and USCONST are reduced to IDENT and SCONST in parser.c, so that
 * they need no productions here; but we must assign token codes to them.
 *
 * DOT_DOT is unused in the core SQL grammar, and so will always provoke
 * parse errors.  It is needed by PL/pgSQL.
 * 非关键字 Token 类型。这些是在 "flex" 词法分析器中硬编码的。它们必须首先列出，以便它们的数字代码不依赖于关键字集合。PL/pgSQL 依赖于此，以便它可以共享同一个词法分析器。如果您在此处添加/更改 Token，请修复 PL/pgSQL 以进行匹配！UIDENT 和 USCONST 在 parser.c 中被规约为 IDENT 和 SCONST，因此它们在此处不需要产生式；但我们必须向它们分配 Token 代码。DOT_DOT 在核心 SQL 语法中未使用，因此将始终引起解析错误。它被 PL/pgSQL 所需要。
 */
%token <str>	IDENT UIDENT FCONST SCONST USCONST BCONST XCONST Op
%token <ival>	ICONST PARAM
%token			TYPECAST DOT_DOT COLON_EQUALS EQUALS_GREATER
%token			LESS_EQUALS GREATER_EQUALS NOT_EQUALS

/*
 * If you want to make any keyword changes, update the keyword table in
 * src/include/parser/kwlist.h and add new keywords to the appropriate one
 * of the reserved-or-not-so-reserved keyword lists, below; search
 * this file for "Keyword category lists".
 * 如果您想对关键字进行任何更改，请更新 src/include/parser/kwlist.h 中的关键字表，并将新关键字添加到下面相应的保留或不那么保留的关键字列表中；在此文件中搜索 "Keyword category lists"。
 */

/* ordinary key words in alphabetical order - 按字母顺序排列的普通关键字 */
%token <keyword> ABORT_P ABSENT ABSOLUTE_P ACCESS ACTION ADD_P ADMIN AFTER
	AGGREGATE ALL ALSO ALTER ALWAYS ANALYSE ANALYZE AND ANY ARRAY AS ASC
	ASENSITIVE ASSERTION ASSIGNMENT ASYMMETRIC ATOMIC AT ATTACH ATTRIBUTE AUTHORIZATION

	BACKWARD BEFORE BEGIN_P BETWEEN BIGINT BINARY BIT
	BOOLEAN_P BOTH BREADTH BY

	CACHE CALL CALLED CASCADE CASCADED CASE CAST CATALOG_P CHAIN CHAR_P
	CHARACTER CHARACTERISTICS CHECK CHECKPOINT CLASS CLOSE
	CLUSTER COALESCE COLLATE COLLATION COLUMN COLUMNS COMMENT COMMENTS COMMIT
	COMMITTED COMPRESSION CONCURRENTLY CONDITIONAL CONFIGURATION CONFLICT
	CONNECTION CONSTRAINT CONSTRAINTS CONTENT_P CONTINUE_P CONVERSION_P COPY
	COST CREATE CROSS CSV CUBE CURRENT_P
	CURRENT_CATALOG CURRENT_DATE CURRENT_ROLE CURRENT_SCHEMA
	CURRENT_TIME CURRENT_TIMESTAMP CURRENT_USER CURSOR CYCLE

	DATA_P DATABASE DAY_P DEALLOCATE DEC DECIMAL_P DECLARE DEFAULT DEFAULTS
	DEFERRABLE DEFERRED DEFINER DELETE_P DELIMITER DELIMITERS DEPENDS DEPTH DESC
	DETACH DICTIONARY DISABLE_P DISCARD DISTINCT DO DOCUMENT_P DOMAIN_P
	DOUBLE_P DROP

	EACH ELSE EMPTY_P ENABLE_P ENCODING ENCRYPTED END_P ENFORCED ENUM_P ERROR_P
	ESCAPE EVENT EXCEPT EXCLUDE EXCLUDING EXCLUSIVE EXECUTE EXISTS EXPLAIN
	EXPRESSION EXTENSION EXTERNAL EXTRACT

	FALSE_P FAMILY FETCH FILTER FINALIZE FIRST_P FLOAT_P FOLLOWING FOR
	FORCE FOREIGN FORMAT FORWARD FREEZE FROM FULL FUNCTION FUNCTIONS

	GENERATED GLOBAL GRANT GRANTED GREATEST GROUP_P GROUPING GROUPS

	HANDLER HAVING HEADER_P HOLD HOUR_P

	IDENTITY_P IF_P ILIKE IMMEDIATE IMMUTABLE IMPLICIT_P IMPORT_P IN_P INCLUDE
	INCLUDING INCREMENT INDENT INDEX INDEXES INHERIT INHERITS INITIALLY INLINE_P
	INNER_P INOUT INPUT_P INSENSITIVE INSERT INSTEAD INT_P INTEGER
	INTERSECT INTERVAL INTO INVOKER IS ISNULL ISOLATION

	JOIN JSON JSON_ARRAY JSON_ARRAYAGG JSON_EXISTS JSON_OBJECT JSON_OBJECTAGG
	JSON_QUERY JSON_SCALAR JSON_SERIALIZE JSON_TABLE JSON_VALUE

	KEEP KEY KEYS

	LABEL LANGUAGE LARGE_P LAST_P LATERAL_P
	LEADING LEAKPROOF LEAST LEFT LEVEL LIKE LIMIT LISTEN LOAD LOCAL
	LOCALTIME LOCALTIMESTAMP LOCATION LOCK_P LOCKED LOGGED

	MAPPING MATCH MATCHED MATERIALIZED MAXVALUE MERGE MERGE_ACTION METHOD
	MINUTE_P MINVALUE MODE MONTH_P MOVE

	NAME_P NAMES NATIONAL NATURAL NCHAR NESTED NEW NEXT NFC NFD NFKC NFKD NO
	NONE NORMALIZE NORMALIZED
	NOT NOTHING NOTIFY NOTNULL NOWAIT NULL_P NULLIF
	NULLS_P NUMERIC

	OBJECT_P OBJECTS_P OF OFF OFFSET OIDS OLD OMIT ON ONLY OPERATOR OPTION OPTIONS OR
	ORDER ORDINALITY OTHERS OUT_P OUTER_P
	OVER OVERLAPS OVERLAY OVERRIDING OWNED OWNER

	PARALLEL PARAMETER PARSER PARTIAL PARTITION PASSING PASSWORD PATH
	PERIOD PLACING PLAN PLANS POLICY
	POSITION PRECEDING PRECISION PRESERVE PREPARE PREPARED PRIMARY
	PRIOR PRIVILEGES PROCEDURAL PROCEDURE PROCEDURES PROGRAM PUBLICATION

	QUOTE QUOTES

	RANGE READ REAL REASSIGN RECURSIVE REF_P REFERENCES REFERENCING
	REFRESH REINDEX RELATIVE_P RELEASE RENAME REPEATABLE REPLACE REPLICA
	RESET RESTART RESTRICT RETURN RETURNING RETURNS REVOKE RIGHT ROLE ROLLBACK ROLLUP
	ROUTINE ROUTINES ROW ROWS RULE

	SAVEPOINT SCALAR SCHEMA SCHEMAS SCROLL SEARCH SECOND_P SECURITY SELECT
	SEQUENCE SEQUENCES
	SERIALIZABLE SERVER SESSION SESSION_USER SET SETS SETOF SHARE SHOW
	SIMILAR SIMPLE SKIP SMALLINT SNAPSHOT SOME SOURCE SQL_P STABLE STANDALONE_P
	START STATEMENT STATISTICS STDIN STDOUT STORAGE STORED STRICT_P STRING_P STRIP_P
	SUBSCRIPTION SUBSTRING SUPPORT SYMMETRIC SYSID SYSTEM_P SYSTEM_USER

	TABLE TABLES TABLESAMPLE TABLESPACE TARGET TEMP TEMPLATE TEMPORARY TEXT_P THEN
	TIES TIME TIMESTAMP TO TRAILING TRANSACTION TRANSFORM
	TREAT TRIGGER TRIM TRUE_P
	TRUNCATE TRUSTED TYPE_P TYPES_P

	UESCAPE UNBOUNDED UNCONDITIONAL UNCOMMITTED UNENCRYPTED UNION UNIQUE UNKNOWN
	UNLISTEN UNLOGGED UNTIL UPDATE USER USING

	VACUUM VALID VALIDATE VALIDATOR VALUE_P VALUES VARCHAR VARIADIC VARYING
	VERBOSE VERSION_P VIEW VIEWS VIRTUAL VOLATILE

	WHEN WHERE WHITESPACE_P WINDOW WITH WITHIN WITHOUT WORK WRAPPER WRITE

	XML_P XMLATTRIBUTES XMLCONCAT XMLELEMENT XMLEXISTS XMLFOREST XMLNAMESPACES
	XMLPARSE XMLPI XMLROOT XMLSERIALIZE XMLTABLE

	YEAR_P YES_P

	ZONE

/*
 * The grammar thinks these are keywords, but they are not in the kwlist.h
 * list and so can never be entered directly.  The filter in parser.c
 * creates these tokens when required (based on looking one token ahead).
 *
 * NOT_LA exists so that productions such as NOT LIKE can be given the same
 * precedence as LIKE; otherwise they'd effectively have the same precedence
 * as NOT, at least with respect to their left-hand subexpression.
 * FORMAT_LA, NULLS_LA, WITH_LA, and WITHOUT_LA are needed to make the grammar
 * LALR(1).
 *
 * 语法分析器认为这些是关键字，但它们不在 kwlist.h 列表中，因此永远不能直接输入。
 * parser.c 中的过滤器在需要时（基于向前看一个 Token）创建这些 Token。NOT_LA 存在是为了使诸如 NOT LIKE 之类的产生式可以被赋予与 LIKE 相同的优先级；否则它们实际上将具有与 NOT 相同的优先级，至少对于它们的左侧子表达式是如此。FORMAT_LA、NULLS_LA、WITH_LA 和 WITHOUT_LA 是为了使语法成为 LALR(1) 所必需的。
 */
%token		FORMAT_LA NOT_LA NULLS_LA WITH_LA WITHOUT_LA

/*
 * The grammar likewise thinks these tokens are keywords, but they are never
 * generated by the scanner.  Rather, they can be injected by parser.c as
 * the initial token of the string (using the lookahead-token mechanism
 * implemented there).  This provides a way to tell the grammar to parse
 * something other than the usual list of SQL commands.
 * 语法分析器同样认为这些 Token 是关键字，但它们从未由扫描器生成。相反，它们可以由 parser.c 注入为字符串的初始 Token（使用那里实现的向前看 Token 机制）。这提供了一种方法来告诉语法分析器解析除通常的 SQL 命令列表之外的其他内容。
 */
%token		MODE_TYPE_NAME
%token		MODE_PLPGSQL_EXPR
%token		MODE_PLPGSQL_ASSIGN1
%token		MODE_PLPGSQL_ASSIGN2
%token		MODE_PLPGSQL_ASSIGN3


/* Precedence: lowest to highest - 优先级：从最低到最高 */
%left		UNION EXCEPT
%left		INTERSECT
%left		OR
%left		AND
%right		NOT
%nonassoc	IS ISNULL NOTNULL	/* IS sets precedence for IS NULL, etc - IS 为 IS NULL 等设置优先级 */
%nonassoc	'<' '>' '=' LESS_EQUALS GREATER_EQUALS NOT_EQUALS
%nonassoc	BETWEEN IN_P LIKE ILIKE SIMILAR NOT_LA
%nonassoc	ESCAPE			/* ESCAPE must be just above LIKE/ILIKE/SIMILAR - ESCAPE 必须紧接在 LIKE/ILIKE/SIMILAR 之上 */

/*
 * Sometimes it is necessary to assign precedence to keywords that are not
 * really part of the operator hierarchy, in order to resolve grammar
 * ambiguities.  It's best to avoid doing so whenever possible, because such
 * assignments have global effect and may hide ambiguities besides the one
 * you intended to solve.  (Attaching a precedence to a single rule with
 * %prec is far safer and should be preferred.)  If you must give precedence
 * to a new keyword, try very hard to give it the same precedence as IDENT.
 * If the keyword has IDENT's precedence then it clearly acts the same as
 * non-keywords and other similar keywords, thus reducing the risk of
 * unexpected precedence effects.
 *
 * We used to need to assign IDENT an explicit precedence just less than Op,
 * to support target_el without AS.  While that's not really necessary since
 * we removed postfix operators, we continue to do so because it provides a
 * reference point for a precedence level that we can assign to other
 * keywords that lack a natural precedence level.
 *
 * We need to do this for PARTITION, RANGE, ROWS, and GROUPS to support
 * opt_existing_window_name (see comment there).
 *
 * The frame_bound productions UNBOUNDED PRECEDING and UNBOUNDED FOLLOWING
 * are even messier: since UNBOUNDED is an unreserved keyword (per spec!),
 * there is no principled way to distinguish these from the productions
 * a_expr PRECEDING/FOLLOWING.  We hack this up by giving UNBOUNDED slightly
 * lower precedence than PRECEDING and FOLLOWING.  At present this doesn't
 * appear to cause UNBOUNDED to be treated differently from other unreserved
 * keywords anywhere else in the grammar, but it's definitely risky.  We can
 * blame any funny behavior of UNBOUNDED on the SQL standard, though.
 *
 * To support CUBE and ROLLUP in GROUP BY without reserving them, we give them
 * an explicit priority lower than '(', so that a rule with CUBE '(' will shift
 * rather than reducing a conflicting rule that takes CUBE as a function name.
 * Using the same precedence as IDENT seems right for the reasons given above.
 *
 * SET is likewise assigned the same precedence as IDENT, to support the
 * relation_expr_opt_alias production (see comment there).
 *
 * KEYS, OBJECT_P, SCALAR, VALUE_P, WITH, and WITHOUT are similarly assigned
 * the same precedence as IDENT.  This allows resolving conflicts in the
 * json_predicate_type_constraint and json_key_uniqueness_constraint_opt
 * productions (see comments there).
 *
 * Like the UNBOUNDED PRECEDING/FOLLOWING case, NESTED is assigned a lower
 * precedence than PATH to fix ambiguity in the json_table production.
 * 有时有必要为并非真正属于运算符层次结构的关键字分配优先级，以解决语法歧义。最好尽可能避免这样做，因为此类分配具有全局影响，可能会隐藏除您打算解决的歧义之外的其他歧义。（使用 %prec 将优先级附加到单个规则要安全得多，应该予以首选。）如果必须为新关键字指定优先级，请尽最大努力将其赋予与 IDENT 相同的优先级。如果该关键字具有 IDENT 的优先级，那么它显然与非关键字和其他类似关键字的行为相同，从而减少了意外优先级影响的风险。我们过去需要为 IDENT 分配一个比 Op 略低的显式优先级，以支持不带 AS 的 target_el。虽然自从我们移除了后缀运算符后，这并不是真正必要的，但我们继续这样做，因为它为我们可以分配给其他缺乏自然优先级水平的关键字的优先级水平提供了一个参考点。我们需要对 PARTITION、RANGE、ROWS 和 GROUPS 执行此操作以支持 opt_existing_window_name（请参阅此处的注释）。frame_bound 产生式 UNBOUNDED PRECEDING 和 UNBOUNDED FOLLOWING 更加混乱：由于 UNBOUNDED 是一个未保留关键字（根据规范！），因此没有原则性的方法可以将这些与产生式 a_expr PRECEDING/FOLLOWING 区分开来。我们通过赋予 UNBOUNDED 比 PRECEDING 和 FOLLOWING 略低的优先级来解决这个问题。目前，这似乎不会导致语法中其他任何地方的 UNBOUNDED 受到与其它未保留关键字不同的对待，但这绝对是有风险的。不过，我们可以把 UNBOUNDED 的任何滑稽行为归咎于 SQL 标准。为了在不保留 CUBE 和 ROLLUP 的情况下支持 GROUP BY 中的它们，我们赋予它们比 '(' 更低的显式优先级，以便具有 CUBE '(' 的规则会移进而不是规约将 CUBE 视为函数名的冲突规则。出于上述原因，使用与 IDENT 相同的优先级似乎是正确的。SET 同样被分配了与 IDENT 相同的优先级，以支持 relation_expr_opt_alias 产生式（请参阅此处的注释）。KEYS、OBJECT_P, SCALAR, VALUE_P, WITH, 和 WITHOUT 同样被分配了与 IDENT 相同的优先级。这允许解决 json_predicate_type_constraint 和 json_key_uniqueness_constraint_opt 产生式中的冲突（请参阅此处的注释）。类似于 UNBOUNDED PRECEDING/FOLLOWING 情况，NESTED 被分配了比 PATH 更低的优先级，以修复 json_table 产生式中的歧义。
 */
%nonassoc	UNBOUNDED NESTED /* ideally would have same precedence as IDENT - 理想情况下应具有与 IDENT 相同的优先级 */
%nonassoc	IDENT PARTITION RANGE ROWS GROUPS PRECEDING FOLLOWING CUBE ROLLUP
			SET KEYS OBJECT_P SCALAR VALUE_P WITH WITHOUT PATH
%left		Op OPERATOR		/* multi-character ops and user-defined operators - 多字符操作符和用户定义操作符 */
%left		'+' '-'
%left		'*' '/' '%'
%left		'^'
/* Unary Operators - 一元操作符 */
%left		AT				/* sets precedence for AT TIME ZONE, AT LOCAL - 为 AT TIME ZONE、AT LOCAL 设置优先级 */
%left		COLLATE
%right		UMINUS
%left		'[' ']'
%left		'(' ')'
%left		TYPECAST
%left		'.'
/*
 * These might seem to be low-precedence, but actually they are not part
 * of the arithmetic hierarchy at all in their use as JOIN operators.
 * We make them high-precedence to support their use as function names.
 * They wouldn't be given a precedence at all, were it not that we need
 * left-associativity among the JOIN rules themselves.
 * 这些看起来可能优先级较低，但实际上在用作 JOIN 运算符时，它们根本不是算术层次结构的一部分。我们将它们设为高优先级，以支持它们用作函数名。如果不是因为我们需要在 JOIN 规则本身之间具有左结合性，它们根本不会被赋予优先级。
 */
%left		JOIN CROSS LEFT FULL RIGHT INNER_P NATURAL

%%

/*
 *	The target production for the whole parse.
 *
 * Ordinarily we parse a list of statements, but if we see one of the
 * special MODE_XXX symbols as first token, we parse something else.
 * The options here correspond to enum RawParseMode, which see for details.
 * 整个解析的目标产生式。通常我们解析一个语句列表，但如果我们看到特殊的 MODE_XXX 符号之一作为第一个 Token，我们会解析其他内容。这里的选项对应于 enum RawParseMode，具体细节参见该枚举。
 */
parse_toplevel:
			stmtmulti
			{
				pg_yyget_extra(yyscanner)->parsetree = $1;
				(void) yynerrs;		/* suppress compiler warning - 抑制编译器警告 */
			}
			| MODE_TYPE_NAME Typename
			{
				pg_yyget_extra(yyscanner)->parsetree = list_make1($2);
			}
			| MODE_PLPGSQL_EXPR PLpgSQL_Expr
			{
				pg_yyget_extra(yyscanner)->parsetree =
					list_make1(makeRawStmt($2, @2));
			}
			| MODE_PLPGSQL_ASSIGN1 PLAssignStmt
			{
				PLAssignStmt *n = (PLAssignStmt *) $2;

				n->nnames = 1;
				pg_yyget_extra(yyscanner)->parsetree =
					list_make1(makeRawStmt((Node *) n, @2));
			}
			| MODE_PLPGSQL_ASSIGN2 PLAssignStmt
			{
				PLAssignStmt *n = (PLAssignStmt *) $2;

				n->nnames = 2;
				pg_yyget_extra(yyscanner)->parsetree =
					list_make1(makeRawStmt((Node *) n, @2));
			}
			| MODE_PLPGSQL_ASSIGN3 PLAssignStmt
			{
				PLAssignStmt *n = (PLAssignStmt *) $2;

				n->nnames = 3;
				pg_yyget_extra(yyscanner)->parsetree =
					list_make1(makeRawStmt((Node *) n, @2));
			}
		;

/*
 * At top level, we wrap each stmt with a RawStmt node carrying start location
 * and length of the stmt's text.
 * We also take care to discard empty statements entirely (which among other
 * things dodges the problem of assigning them a location).
 * 在顶层，我们用一个携带语句文本起始位置和长度的 RawStmt 节点包装每个语句。我们还注意完全丢弃空语句（这除其他外还避开了向它们分配位置的问题）。
 */
stmtmulti:	stmtmulti ';' toplevel_stmt
				{
					if ($1 != NIL)
					{
						/* update length of previous stmt - 更新前一个语句的长度 */
						updateRawStmtEnd(llast_node(RawStmt, $1), @2);
					}
					if ($3 != NULL)
						$$ = lappend($1, makeRawStmt($3, @3));
					else
						$$ = $1;
				}
			| toplevel_stmt
				{
					if ($1 != NULL)
						$$ = list_make1(makeRawStmt($1, @1));
					else
						$$ = NIL;
				}
		;

/*
 * toplevel_stmt includes BEGIN and END.  stmt does not include them, because
 * those words have different meanings in function bodies.
 * toplevel_stmt 包含 BEGIN 和 END。stmt 不包含它们，因为这些词在函数体中具有不同的含义。
 */
toplevel_stmt:
			stmt
			| TransactionStmtLegacy
		;

stmt:
			AlterEventTrigStmt
			| AlterCollationStmt
			| AlterDatabaseStmt
			| AlterDatabaseSetStmt
			| AlterDefaultPrivilegesStmt
			| AlterDomainStmt
			| AlterEnumStmt
			| AlterExtensionStmt
			| AlterExtensionContentsStmt
			| AlterFdwStmt
			| AlterForeignServerStmt
			| AlterFunctionStmt
			| AlterGroupStmt
			| AlterObjectDependsStmt
			| AlterObjectSchemaStmt
			| AlterOwnerStmt
			| AlterOperatorStmt
			| AlterTypeStmt
			| AlterPolicyStmt
			| AlterSeqStmt
			| AlterSystemStmt
			| AlterTableStmt
			| AlterTblSpcStmt
			| AlterCompositeTypeStmt
			| AlterPublicationStmt
			| AlterRoleSetStmt
			| AlterRoleStmt
			| AlterSubscriptionStmt
			| AlterStatsStmt
			| AlterTSConfigurationStmt
			| AlterTSDictionaryStmt
			| AlterUserMappingStmt
			| AnalyzeStmt
			| CallStmt
			| CheckPointStmt
			| ClosePortalStmt
			| ClusterStmt
			| CommentStmt
			| ConstraintsSetStmt
			| CopyStmt
			| CreateAmStmt
			| CreateAsStmt
			| CreateAssertionStmt
			| CreateCastStmt
			| CreateConversionStmt
			| CreateDomainStmt
			| CreateExtensionStmt
			| CreateFdwStmt
			| CreateForeignServerStmt
			| CreateForeignTableStmt
			| CreateFunctionStmt
			| CreateGroupStmt
			| CreateMatViewStmt
			| CreateOpClassStmt
			| CreateOpFamilyStmt
			| CreatePublicationStmt
			| AlterOpFamilyStmt
			| CreatePolicyStmt
			| CreatePLangStmt
			| CreateSchemaStmt
			| CreateSeqStmt
			| CreateStmt
			| CreateSubscriptionStmt
			| CreateStatsStmt
			| CreateTableSpaceStmt
			| CreateTransformStmt
			| CreateTrigStmt
			| CreateEventTrigStmt
			| CreateRoleStmt
			| CreateUserStmt
			| CreateUserMappingStmt
			| CreatedbStmt
			| DeallocateStmt
			| DeclareCursorStmt
			| DefineStmt
			| DeleteStmt
			| DiscardStmt
			| DoStmt
			| DropCastStmt
			| DropOpClassStmt
			| DropOpFamilyStmt
			| DropOwnedStmt
			| DropStmt
			| DropSubscriptionStmt
			| DropTableSpaceStmt
			| DropTransformStmt
			| DropRoleStmt
			| DropUserMappingStmt
			| DropdbStmt
			| ExecuteStmt
			| ExplainStmt
			| FetchStmt
			| GrantStmt
			| GrantRoleStmt
			| ImportForeignSchemaStmt
			| IndexStmt
			| InsertStmt
			| ListenStmt
			| RefreshMatViewStmt
			| LoadStmt
			| LockStmt
			| MergeStmt
			| NotifyStmt
			| PrepareStmt
			| ReassignOwnedStmt
			| ReindexStmt
			| RemoveAggrStmt
			| RemoveFuncStmt
			| RemoveOperStmt
			| RenameStmt
			| RevokeStmt
			| RevokeRoleStmt
			| RuleStmt
			| SecLabelStmt
			| SelectStmt
			| TransactionStmt
			| TruncateStmt
			| UnlistenStmt
			| UpdateStmt
			| VacuumStmt
			| VariableResetStmt
			| VariableSetStmt
			| VariableShowStmt
			| ViewStmt
			| /* EMPTY - 空 */
				{ $$ = NULL; }
		;

/*
 * Generic supporting productions for DDL
 * DDL 的通用支持产生式
 */
opt_single_name:
			ColId							{ $$ = $1; }
			| /* EMPTY - 空 */					{ $$ = NULL; }
		;

opt_qualified_name:
			any_name						{ $$ = $1; }
			| /* EMPTY - 空 */						{ $$ = NIL; }
		;

opt_concurrently:
			CONCURRENTLY					{ $$ = true; }
			| /* EMPTY - 空 */						{ $$ = false; }
		;

opt_drop_behavior:
			CASCADE							{ $$ = DROP_CASCADE; }
			| RESTRICT						{ $$ = DROP_RESTRICT; }
			| /* EMPTY - 空 */					{ $$ = DROP_RESTRICT; /* default - 默认值 */ }
		;

/*****************************************************************************
 *
 * CALL statement
 *
 * CALL 语句
 *****************************************************************************/

CallStmt:	CALL func_application
				{
					CallStmt   *n = makeNode(CallStmt);

					n->funccall = castNode(FuncCall, $2);
					$$ = (Node *) n;
				}
		;

/*****************************************************************************
 *
 * Create a new Postgres DBMS role
 *
 * 创建一个新的 Postgres DBMS 角色
 *****************************************************************************/

CreateRoleStmt:
			CREATE ROLE RoleId opt_with OptRoleList
				{
					CreateRoleStmt *n = makeNode(CreateRoleStmt);

					n->stmt_type = ROLESTMT_ROLE;
					n->role = $3;
					n->options = $5;
					$$ = (Node *) n;
				}
		;


opt_with:	WITH
			| WITH_LA
			| /* EMPTY - 空 */
		;

/*
 * Options for CREATE ROLE and ALTER ROLE (also used by CREATE/ALTER USER
 * for backwards compatibility).  Note: the only option required by SQL99
 * is "WITH ADMIN name".
 * CREATE ROLE 和 ALTER ROLE 的选项（出于向后兼容性，CREATE/ALTER USER 也使用这些选项）。注意：SQL99 唯一要求的选项是 "WITH ADMIN name"。
 */
OptRoleList:
			OptRoleList CreateOptRoleElem			{ $$ = lappend($1, $2); }
			| /* EMPTY - 空 */							{ $$ = NIL; }
		;

AlterOptRoleList:
			AlterOptRoleList AlterOptRoleElem		{ $$ = lappend($1, $2); }
			| /* EMPTY - 空 */							{ $$ = NIL; }
		;

AlterOptRoleElem:
			PASSWORD Sconst
				{
					$$ = makeDefElem("password",
									 (Node *) makeString($2), @1);
				}
			| PASSWORD NULL_P
				{
					$$ = makeDefElem("password", NULL, @1);
				}
			| ENCRYPTED PASSWORD Sconst
				{
					/*
					 * These days, passwords are always stored in encrypted
					 * form, so there is no difference between PASSWORD and
					 * ENCRYPTED PASSWORD.
					 * 如今，密码总是以加密形式存储，因此 PASSWORD 和 ENCRYPTED PASSWORD 之间没有区别。
					 */
					$$ = makeDefElem("password",
									 (Node *) makeString($3), @1);
				}
			| UNENCRYPTED PASSWORD Sconst
				{
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("UNENCRYPTED PASSWORD is no longer supported"),
							 errhint("Remove UNENCRYPTED to store the password in encrypted form instead."),
							 parser_errposition(@1)));
				}
			| INHERIT
				{
					$$ = makeDefElem("inherit", (Node *) makeBoolean(true), @1);
				}
			| CONNECTION LIMIT SignedIconst
				{
					$$ = makeDefElem("connectionlimit", (Node *) makeInteger($3), @1);
				}
			| VALID UNTIL Sconst
				{
					$$ = makeDefElem("validUntil", (Node *) makeString($3), @1);
				}
		/* Supported but not documented for roles, for use by ALTER GROUP. - 角色支持但未记录归档，供 ALTER GROUP 使用。 */
			| USER role_list
				{
					$$ = makeDefElem("rolemembers", (Node *) $2, @1);
				}
			| IDENT
				{
					/*
					 * We handle identifiers that aren't parser keywords with
					 * the following special-case codes, to avoid bloating the
					 * size of the main parser.
					 * 我们使用以下特例代码处理非解析器关键字的标识符，以避免膨胀主解析器的大小。
					 */
					if (strcmp($1, "superuser") == 0)
						$$ = makeDefElem("superuser", (Node *) makeBoolean(true), @1);
					else if (strcmp($1, "nosuperuser") == 0)
						$$ = makeDefElem("superuser", (Node *) makeBoolean(false), @1);
					else if (strcmp($1, "createrole") == 0)
						$$ = makeDefElem("createrole", (Node *) makeBoolean(true), @1);
					else if (strcmp($1, "nocreaterole") == 0)
						$$ = makeDefElem("createrole", (Node *) makeBoolean(false), @1);
					else if (strcmp($1, "replication") == 0)
						$$ = makeDefElem("isreplication", (Node *) makeBoolean(true), @1);
					else if (strcmp($1, "noreplication") == 0)
						$$ = makeDefElem("isreplication", (Node *) makeBoolean(false), @1);
					else if (strcmp($1, "createdb") == 0)
						$$ = makeDefElem("createdb", (Node *) makeBoolean(true), @1);
					else if (strcmp($1, "nocreatedb") == 0)
						$$ = makeDefElem("createdb", (Node *) makeBoolean(false), @1);
					else if (strcmp($1, "login") == 0)
						$$ = makeDefElem("canlogin", (Node *) makeBoolean(true), @1);
					else if (strcmp($1, "nologin") == 0)
						$$ = makeDefElem("canlogin", (Node *) makeBoolean(false), @1);
					else if (strcmp($1, "bypassrls") == 0)
						$$ = makeDefElem("bypassrls", (Node *) makeBoolean(true), @1);
					else if (strcmp($1, "nobypassrls") == 0)
						$$ = makeDefElem("bypassrls", (Node *) makeBoolean(false), @1);
					else if (strcmp($1, "noinherit") == 0)
					{
						/*
						 * Note that INHERIT is a keyword, so it's handled by main parser, but
						 * NOINHERIT is handled here.
						 * 请注意，INHERIT 是一个关键字，因此它由主解析器处理，但 NOINHERIT 在此处处理。
						 */
						$$ = makeDefElem("inherit", (Node *) makeBoolean(false), @1);
					}
					else
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("unrecognized role option \"%s\"", $1),
									 parser_errposition(@1)));
				}
		;

CreateOptRoleElem:
			AlterOptRoleElem			{ $$ = $1; }
			/* The following are not supported by ALTER ROLE/USER/GROUP - 以下内容不受 ALTER ROLE/USER/GROUP 支持 */
			| SYSID Iconst
				{
					$$ = makeDefElem("sysid", (Node *) makeInteger($2), @1);
				}
			| ADMIN role_list
				{
					$$ = makeDefElem("adminmembers", (Node *) $2, @1);
				}
			| ROLE role_list
				{
					$$ = makeDefElem("rolemembers", (Node *) $2, @1);
				}
			| IN_P ROLE role_list
				{
					$$ = makeDefElem("addroleto", (Node *) $3, @1);
				}
			| IN_P GROUP_P role_list
				{
					$$ = makeDefElem("addroleto", (Node *) $3, @1);
				}
		;


/*****************************************************************************
 *
 * Create a new Postgres DBMS user (role with implied login ability)
 *
 * 创建一个新的 Postgres DBMS 用户（具有隐式登录能力的角色）
 *****************************************************************************/

CreateUserStmt:
			CREATE USER RoleId opt_with OptRoleList
				{
					CreateRoleStmt *n = makeNode(CreateRoleStmt);

					n->stmt_type = ROLESTMT_USER;
					n->role = $3;
					n->options = $5;
					$$ = (Node *) n;
				}
		;


/*****************************************************************************
 *
 * Alter a postgresql DBMS role
 *
 * 修改 postgresql DBMS 角色
 *****************************************************************************/

AlterRoleStmt:
			ALTER ROLE RoleSpec opt_with AlterOptRoleList
				 {
					AlterRoleStmt *n = makeNode(AlterRoleStmt);

					n->role = $3;
					n->action = +1;	/* add, if there are members - 如果有成员，则添加 */
					n->options = $5;
					$$ = (Node *) n;
				 }
			| ALTER USER RoleSpec opt_with AlterOptRoleList
				 {
					AlterRoleStmt *n = makeNode(AlterRoleStmt);

					n->role = $3;
					n->action = +1;	/* add, if there are members - 如果有成员，则添加 */
					n->options = $5;
					$$ = (Node *) n;
				 }
		;

opt_in_database:
			   /* EMPTY - 空 */					{ $$ = NULL; }
			| IN_P DATABASE name	{ $$ = $3; }
		;

AlterRoleSetStmt:
			ALTER ROLE RoleSpec opt_in_database SetResetClause
				{
					AlterRoleSetStmt *n = makeNode(AlterRoleSetStmt);

					n->role = $3;
					n->database = $4;
					n->setstmt = $5;
					$$ = (Node *) n;
				}
			| ALTER ROLE ALL opt_in_database SetResetClause
				{
					AlterRoleSetStmt *n = makeNode(AlterRoleSetStmt);

					n->role = NULL;
					n->database = $4;
					n->setstmt = $5;
					$$ = (Node *) n;
				}
			| ALTER USER RoleSpec opt_in_database SetResetClause
				{
					AlterRoleSetStmt *n = makeNode(AlterRoleSetStmt);

					n->role = $3;
					n->database = $4;
					n->setstmt = $5;
					$$ = (Node *) n;
				}
			| ALTER USER ALL opt_in_database SetResetClause
				{
					AlterRoleSetStmt *n = makeNode(AlterRoleSetStmt);

					n->role = NULL;
					n->database = $4;
					n->setstmt = $5;
					$$ = (Node *) n;
				}
		;


/*****************************************************************************
 *
 * Drop a postgresql DBMS role
 *
 * XXX Ideally this would have CASCADE/RESTRICT options, but a role
 * might own objects in multiple databases, and there is presently no way to
 * implement cascading to other databases.  So we always behave as RESTRICT.
 * 删除 postgresql DBMS 角色。XXX 理想情况下，这应该有 CASCADE/RESTRICT 选项，但角色可能在多个数据库中拥有对象，目前没有办法实现向其他数据库的级联。所以我们的行为总是等同于 RESTRICT。
 *****************************************************************************/

DropRoleStmt:
			DROP ROLE role_list
				{
					DropRoleStmt *n = makeNode(DropRoleStmt);

					n->missing_ok = false;
					n->roles = $3;
					$$ = (Node *) n;
				}
			| DROP ROLE IF_P EXISTS role_list
				{
					DropRoleStmt *n = makeNode(DropRoleStmt);

					n->missing_ok = true;
					n->roles = $5;
					$$ = (Node *) n;
				}
			| DROP USER role_list
				{
					DropRoleStmt *n = makeNode(DropRoleStmt);

					n->missing_ok = false;
					n->roles = $3;
					$$ = (Node *) n;
				}
			| DROP USER IF_P EXISTS role_list
				{
					DropRoleStmt *n = makeNode(DropRoleStmt);

					n->roles = $5;
					n->missing_ok = true;
					$$ = (Node *) n;
				}
			| DROP GROUP_P role_list
				{
					DropRoleStmt *n = makeNode(DropRoleStmt);

					n->missing_ok = false;
					n->roles = $3;
					$$ = (Node *) n;
				}
			| DROP GROUP_P IF_P EXISTS role_list
				{
					DropRoleStmt *n = makeNode(DropRoleStmt);

					n->missing_ok = true;
					n->roles = $5;
					$$ = (Node *) n;
				}
			;


/*****************************************************************************
 *
 * Create a postgresql group (role without login ability)
 *
 * 创建一个 postgresql 用户组（没有登录能力的角色）
 *****************************************************************************/

CreateGroupStmt:
			CREATE GROUP_P RoleId opt_with OptRoleList
				{
					CreateRoleStmt *n = makeNode(CreateRoleStmt);

					n->stmt_type = ROLESTMT_GROUP;
					n->role = $3;
					n->options = $5;
					$$ = (Node *) n;
				}
		;


/*****************************************************************************
 *
 * Alter a postgresql group
 *
 * 修改 postgresql 用户组
 *****************************************************************************/

AlterGroupStmt:
			ALTER GROUP_P RoleSpec add_drop USER role_list
				{
					AlterRoleStmt *n = makeNode(AlterRoleStmt);

					n->role = $3;
					n->action = $4;
					n->options = list_make1(makeDefElem("rolemembers",
														(Node *) $6, @6));
					$$ = (Node *) n;
				}
		;

add_drop:	ADD_P									{ $$ = +1; }
			| DROP									{ $$ = -1; }
		;


/*****************************************************************************
 *
 * Manipulate a schema
 *
 * 操作模式（schema）
 *****************************************************************************/

CreateSchemaStmt:
			CREATE SCHEMA opt_single_name AUTHORIZATION RoleSpec OptSchemaEltList
				{
					CreateSchemaStmt *n = makeNode(CreateSchemaStmt);

					/* One can omit the schema name or the authorization id. - 可以省略模式名称或授权 ID。 */
					n->schemaname = $3;
					n->authrole = $5;
					n->schemaElts = $6;
					n->if_not_exists = false;
					$$ = (Node *) n;
				}
			| CREATE SCHEMA ColId OptSchemaEltList
				{
					CreateSchemaStmt *n = makeNode(CreateSchemaStmt);

					/* ...but not both - ...但不能两者都省略 */
					n->schemaname = $3;
					n->authrole = NULL;
					n->schemaElts = $4;
					n->if_not_exists = false;
					$$ = (Node *) n;
				}
			| CREATE SCHEMA IF_P NOT EXISTS opt_single_name AUTHORIZATION RoleSpec OptSchemaEltList
				{
					CreateSchemaStmt *n = makeNode(CreateSchemaStmt);

					/* schema name can be omitted here, too - 此处模式名称也可以省略 */
					n->schemaname = $6;
					n->authrole = $8;
					if ($9 != NIL)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("CREATE SCHEMA IF NOT EXISTS cannot include schema elements"),
								 parser_errposition(@9)));
					n->schemaElts = $9;
					n->if_not_exists = true;
					$$ = (Node *) n;
				}
			| CREATE SCHEMA IF_P NOT EXISTS ColId OptSchemaEltList
				{
					CreateSchemaStmt *n = makeNode(CreateSchemaStmt);

					/* ...but not here - ...但此处不能省略 */
					n->schemaname = $6;
					n->authrole = NULL;
					if ($7 != NIL)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("CREATE SCHEMA IF NOT EXISTS cannot include schema elements"),
								 parser_errposition(@7)));
					n->schemaElts = $7;
					n->if_not_exists = true;
					$$ = (Node *) n;
				}
		;

OptSchemaEltList:
			OptSchemaEltList schema_stmt
				{
					$$ = lappend($1, $2);
				}
			| /* EMPTY - 空 */
				{ $$ = NIL; }
		;

/*
 *	schema_stmt are the ones that can show up inside a CREATE SCHEMA
 *	statement (in addition to by themselves).
 * schema_stmt 是可以出现在 CREATE SCHEMA 语句内部的语句（除了它们自己单独出现之外）。
 */
schema_stmt:
			CreateStmt
			| IndexStmt
			| CreateSeqStmt
			| CreateTrigStmt
			| GrantStmt
			| ViewStmt
		;


/*****************************************************************************
 *
 * Set PG internal variable
 *	  SET name TO 'var_value'
 * Include SQL syntax (thomas 1997-10-22):
 *	  SET TIME ZONE 'var_value'
 *
 * 设置 PG 内部变量 SET name TO 'var_value'，包含 SQL 语法（thomas 1997-10-22）：SET TIME ZONE 'var_value'
 *****************************************************************************/

VariableSetStmt:
			SET set_rest
				{
					VariableSetStmt *n = $2;

					n->is_local = false;
					$$ = (Node *) n;
				}
			| SET LOCAL set_rest
				{
					VariableSetStmt *n = $3;

					n->is_local = true;
					$$ = (Node *) n;
				}
			| SET SESSION set_rest
				{
					VariableSetStmt *n = $3;

					n->is_local = false;
					$$ = (Node *) n;
				}
		;

set_rest:
			TRANSACTION transaction_mode_list
				{
					VariableSetStmt *n = makeNode(VariableSetStmt);

					n->kind = VAR_SET_MULTI;
					n->name = "TRANSACTION";
					n->args = $2;
					n->jumble_args = true;
					n->location = -1;
					$$ = n;
				}
			| SESSION CHARACTERISTICS AS TRANSACTION transaction_mode_list
				{
					VariableSetStmt *n = makeNode(VariableSetStmt);

					n->kind = VAR_SET_MULTI;
					n->name = "SESSION CHARACTERISTICS";
					n->args = $5;
					n->jumble_args = true;
					n->location = -1;
					$$ = n;
				}
			| set_rest_more
			;

generic_set:
			var_name TO var_list
				{
					VariableSetStmt *n = makeNode(VariableSetStmt);

					n->kind = VAR_SET_VALUE;
					n->name = $1;
					n->args = $3;
					n->location = @3;
					$$ = n;
				}
			| var_name '=' var_list
				{
					VariableSetStmt *n = makeNode(VariableSetStmt);

					n->kind = VAR_SET_VALUE;
					n->name = $1;
					n->args = $3;
					n->location = @3;
					$$ = n;
				}
			| var_name TO DEFAULT
				{
					VariableSetStmt *n = makeNode(VariableSetStmt);

					n->kind = VAR_SET_DEFAULT;
					n->name = $1;
					n->location = -1;
					$$ = n;
				}
			| var_name '=' DEFAULT
				{
					VariableSetStmt *n = makeNode(VariableSetStmt);

					n->kind = VAR_SET_DEFAULT;
					n->name = $1;
					n->location = -1;
					$$ = n;
				}
		;

set_rest_more:	/* Generic SET syntaxes: - 通用的 SET 语法： */
			generic_set							{$$ = $1;}
			| var_name FROM CURRENT_P
				{
					VariableSetStmt *n = makeNode(VariableSetStmt);

					n->kind = VAR_SET_CURRENT;
					n->name = $1;
					n->location = -1;
					$$ = n;
				}
			/* Special syntaxes mandated by SQL standard: - SQL 标准要求的特殊语法： */
			| TIME ZONE zone_value
				{
					VariableSetStmt *n = makeNode(VariableSetStmt);

					n->kind = VAR_SET_VALUE;
					n->name = "timezone";
					n->location = -1;
					n->jumble_args = true;
					if ($3 != NULL)
						n->args = list_make1($3);
					else
						n->kind = VAR_SET_DEFAULT;
					$$ = n;
				}
			| CATALOG_P Sconst
				{
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("current database cannot be changed"),
							 parser_errposition(@2)));
					$$ = NULL; /* not reached - 不可达 */
				}
			| SCHEMA Sconst
				{
					VariableSetStmt *n = makeNode(VariableSetStmt);

					n->kind = VAR_SET_VALUE;
					n->name = "search_path";
					n->args = list_make1(makeStringConst($2, @2));
					n->location = @2;
					$$ = n;
				}
			| NAMES opt_encoding
				{
					VariableSetStmt *n = makeNode(VariableSetStmt);

					n->kind = VAR_SET_VALUE;
					n->name = "client_encoding";
					n->location = @2;
					if ($2 != NULL)
						n->args = list_make1(makeStringConst($2, @2));
					else
						n->kind = VAR_SET_DEFAULT;
					$$ = n;
				}
			| ROLE NonReservedWord_or_Sconst
				{
					VariableSetStmt *n = makeNode(VariableSetStmt);

					n->kind = VAR_SET_VALUE;
					n->name = "role";
					n->args = list_make1(makeStringConst($2, @2));
					n->location = @2;
					$$ = n;
				}
			| SESSION AUTHORIZATION NonReservedWord_or_Sconst
				{
					VariableSetStmt *n = makeNode(VariableSetStmt);

					n->kind = VAR_SET_VALUE;
					n->name = "session_authorization";
					n->args = list_make1(makeStringConst($3, @3));
					n->location = @3;
					$$ = n;
				}
			| SESSION AUTHORIZATION DEFAULT
				{
					VariableSetStmt *n = makeNode(VariableSetStmt);

					n->kind = VAR_SET_DEFAULT;
					n->name = "session_authorization";
					n->location = -1;
					$$ = n;
				}
			| XML_P OPTION document_or_content
				{
					VariableSetStmt *n = makeNode(VariableSetStmt);

					n->kind = VAR_SET_VALUE;
					n->name = "xmloption";
					n->args = list_make1(makeStringConst($3 == XMLOPTION_DOCUMENT ? "DOCUMENT" : "CONTENT", @3));
					n->jumble_args = true;
					n->location = -1;
					$$ = n;
				}
			/* Special syntaxes invented by PostgreSQL: - PostgreSQL 发明的特殊语法： */
			| TRANSACTION SNAPSHOT Sconst
				{
					VariableSetStmt *n = makeNode(VariableSetStmt);

					n->kind = VAR_SET_MULTI;
					n->name = "TRANSACTION SNAPSHOT";
					n->args = list_make1(makeStringConst($3, @3));
					n->location = @3;
					$$ = n;
				}
		;

var_name:	ColId								{ $$ = $1; }
			| var_name '.' ColId
				{ $$ = psprintf("%s.%s", $1, $3); }
		;

var_list:	var_value								{ $$ = list_make1($1); }
			| var_list ',' var_value				{ $$ = lappend($1, $3); }
		;

var_value:	opt_boolean_or_string
				{ $$ = makeStringConst($1, @1); }
			| NumericOnly
				{ $$ = makeAConst($1, @1); }
		;

iso_level:	READ UNCOMMITTED						{ $$ = "read uncommitted"; }
			| READ COMMITTED						{ $$ = "read committed"; }
			| REPEATABLE READ						{ $$ = "repeatable read"; }
			| SERIALIZABLE							{ $$ = "serializable"; }
		;

opt_boolean_or_string:
			TRUE_P									{ $$ = "true"; }
			| FALSE_P								{ $$ = "false"; }
			| ON									{ $$ = "on"; }
			/*
			 * OFF is also accepted as a boolean value, but is handled by
			 * the NonReservedWord rule.  The action for booleans and strings
			 * is the same, so we don't need to distinguish them here.
			 * OFF 也被接受为布尔值，但由 NonReservedWord 规则处理。布尔值和字符串的操作是相同的，因此我们不需要在此处区分它们。
			 */
			| NonReservedWord_or_Sconst				{ $$ = $1; }
		;

/* Timezone values can be:
 * - a string such as 'pst8pdt'
 * - an identifier such as "pst8pdt"
 * - an integer or floating point number
 * - a time interval per SQL99
 * ColId gives reduce/reduce errors against ConstInterval and LOCAL,
 * so use IDENT (meaning we reject anything that is a key word).
 * 时区值可以是：- 类似于 'pst8pdt' 的字符串 - 类似于 "pst8pdt" 的标识符 - 整数或浮点数 - 符合 SQL99 的时间间隔。ColId 针对 ConstInterval 和 LOCAL 会给出规约/规约错误，因此使用 IDENT（意味着我们拒绝任何作为关键字的内容）。
 */
zone_value:
			Sconst
				{
					$$ = makeStringConst($1, @1);
				}
			| IDENT
				{
					$$ = makeStringConst($1, @1);
				}
			| ConstInterval Sconst opt_interval
				{
					TypeName   *t = $1;

					if ($3 != NIL)
					{
						A_Const	   *n = (A_Const *) linitial($3);

						if ((n->val.ival.ival & ~(INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE))) != 0)
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("time zone interval must be HOUR or HOUR TO MINUTE"),
									 parser_errposition(@3)));
					}
					t->typmods = $3;
					$$ = makeStringConstCast($2, @2, t);
				}
			| ConstInterval '(' Iconst ')' Sconst
				{
					TypeName   *t = $1;

					t->typmods = list_make2(makeIntConst(INTERVAL_FULL_RANGE, -1),
											makeIntConst($3, @3));
					$$ = makeStringConstCast($5, @5, t);
				}
			| NumericOnly							{ $$ = makeAConst($1, @1); }
			| DEFAULT								{ $$ = NULL; }
			| LOCAL									{ $$ = NULL; }
		;

opt_encoding:
			Sconst									{ $$ = $1; }
			| DEFAULT								{ $$ = NULL; }
			| /* EMPTY - 空 */								{ $$ = NULL; }
		;

NonReservedWord_or_Sconst:
			NonReservedWord							{ $$ = $1; }
			| Sconst								{ $$ = $1; }
		;

VariableResetStmt:
			RESET reset_rest						{ $$ = (Node *) $2; }
		;

reset_rest:
			generic_reset							{ $$ = $1; }
			| TIME ZONE
				{
					VariableSetStmt *n = makeNode(VariableSetStmt);

					n->kind = VAR_RESET;
					n->name = "timezone";
					n->location = -1;
					$$ = n;
				}
			| TRANSACTION ISOLATION LEVEL
				{
					VariableSetStmt *n = makeNode(VariableSetStmt);

					n->kind = VAR_RESET;
					n->name = "transaction_isolation";
					n->location = -1;
					$$ = n;
				}
			| SESSION AUTHORIZATION
				{
					VariableSetStmt *n = makeNode(VariableSetStmt);

					n->kind = VAR_RESET;
					n->name = "session_authorization";
					n->location = -1;
					$$ = n;
				}
		;

generic_reset:
			var_name
				{
					VariableSetStmt *n = makeNode(VariableSetStmt);

					n->kind = VAR_RESET;
					n->name = $1;
					n->location = -1;
					$$ = n;
				}
			| ALL
				{
					VariableSetStmt *n = makeNode(VariableSetStmt);

					n->kind = VAR_RESET_ALL;
					n->location = -1;
					$$ = n;
				}
		;

/* SetResetClause allows SET or RESET without LOCAL - SetResetClause 允许没有 LOCAL 的 SET 或 RESET */
SetResetClause:
			SET set_rest					{ $$ = $2; }
			| VariableResetStmt				{ $$ = (VariableSetStmt *) $1; }
		;

/* SetResetClause allows SET or RESET without LOCAL - SetResetClause 允许没有 LOCAL 的 SET 或 RESET */
FunctionSetResetClause:
			SET set_rest_more				{ $$ = $2; }
			| VariableResetStmt				{ $$ = (VariableSetStmt *) $1; }
		;


VariableShowStmt:
			SHOW var_name
				{
					VariableShowStmt *n = makeNode(VariableShowStmt);

					n->name = $2;
					$$ = (Node *) n;
				}
			| SHOW TIME ZONE
				{
					VariableShowStmt *n = makeNode(VariableShowStmt);

					n->name = "timezone";
					$$ = (Node *) n;
				}
			| SHOW TRANSACTION ISOLATION LEVEL
				{
					VariableShowStmt *n = makeNode(VariableShowStmt);

					n->name = "transaction_isolation";
					$$ = (Node *) n;
				}
			| SHOW SESSION AUTHORIZATION
				{
					VariableShowStmt *n = makeNode(VariableShowStmt);

					n->name = "session_authorization";
					$$ = (Node *) n;
				}
			| SHOW ALL
				{
					VariableShowStmt *n = makeNode(VariableShowStmt);

					n->name = "all";
					$$ = (Node *) n;
				}
		;


ConstraintsSetStmt:
			SET CONSTRAINTS constraints_set_list constraints_set_mode
				{
					ConstraintsSetStmt *n = makeNode(ConstraintsSetStmt);

					n->constraints = $3;
					n->deferred = $4;
					$$ = (Node *) n;
				}
		;

constraints_set_list:
			ALL										{ $$ = NIL; }
			| qualified_name_list					{ $$ = $1; }
		;

constraints_set_mode:
			DEFERRED								{ $$ = true; }
			| IMMEDIATE								{ $$ = false; }
		;


/*
 * Checkpoint statement
 * 检查点（Checkpoint）语句
 */
CheckPointStmt:
			CHECKPOINT
				{
					CheckPointStmt *n = makeNode(CheckPointStmt);

					$$ = (Node *) n;
				}
		;


/*****************************************************************************
 *
 * DISCARD { ALL | TEMP | PLANS | SEQUENCES }
 *
 *****************************************************************************/

DiscardStmt:
			DISCARD ALL
				{
					DiscardStmt *n = makeNode(DiscardStmt);

					n->target = DISCARD_ALL;
					$$ = (Node *) n;
				}
			| DISCARD TEMP
				{
					DiscardStmt *n = makeNode(DiscardStmt);

					n->target = DISCARD_TEMP;
					$$ = (Node *) n;
				}
			| DISCARD TEMPORARY
				{
					DiscardStmt *n = makeNode(DiscardStmt);

					n->target = DISCARD_TEMP;
					$$ = (Node *) n;
				}
			| DISCARD PLANS
				{
					DiscardStmt *n = makeNode(DiscardStmt);

					n->target = DISCARD_PLANS;
					$$ = (Node *) n;
				}
			| DISCARD SEQUENCES
				{
					DiscardStmt *n = makeNode(DiscardStmt);

					n->target = DISCARD_SEQUENCES;
					$$ = (Node *) n;
				}

		;


/*****************************************************************************
 *
 *	ALTER [ TABLE | INDEX | SEQUENCE | VIEW | MATERIALIZED VIEW | FOREIGN TABLE ] variations
 *
 * Note: we accept all subcommands for each of the variants, and sort
 * out what's really legal at execution time.
 * ALTER [ TABLE | INDEX | SEQUENCE | VIEW | MATERIALIZED VIEW | FOREIGN TABLE ] 变体。注意：we 接受每个变体的所有子命令，并在执行时整理出真正合法的命令。
 *****************************************************************************/

AlterTableStmt:
			ALTER TABLE relation_expr alter_table_cmds
				{
					AlterTableStmt *n = makeNode(AlterTableStmt);

					n->relation = $3;
					n->cmds = $4;
					n->objtype = OBJECT_TABLE;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
		|	ALTER TABLE IF_P EXISTS relation_expr alter_table_cmds
				{
					AlterTableStmt *n = makeNode(AlterTableStmt);

					n->relation = $5;
					n->cmds = $6;
					n->objtype = OBJECT_TABLE;
					n->missing_ok = true;
					$$ = (Node *) n;
				}
		|	ALTER TABLE relation_expr partition_cmd
				{
					AlterTableStmt *n = makeNode(AlterTableStmt);

					n->relation = $3;
					n->cmds = list_make1($4);
					n->objtype = OBJECT_TABLE;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
		|	ALTER TABLE IF_P EXISTS relation_expr partition_cmd
				{
					AlterTableStmt *n = makeNode(AlterTableStmt);

					n->relation = $5;
					n->cmds = list_make1($6);
					n->objtype = OBJECT_TABLE;
					n->missing_ok = true;
					$$ = (Node *) n;
				}
		|	ALTER TABLE ALL IN_P TABLESPACE name SET TABLESPACE name opt_nowait
				{
					AlterTableMoveAllStmt *n =
						makeNode(AlterTableMoveAllStmt);

					n->orig_tablespacename = $6;
					n->objtype = OBJECT_TABLE;
					n->roles = NIL;
					n->new_tablespacename = $9;
					n->nowait = $10;
					$$ = (Node *) n;
				}
		|	ALTER TABLE ALL IN_P TABLESPACE name OWNED BY role_list SET TABLESPACE name opt_nowait
				{
					AlterTableMoveAllStmt *n =
						makeNode(AlterTableMoveAllStmt);

					n->orig_tablespacename = $6;
					n->objtype = OBJECT_TABLE;
					n->roles = $9;
					n->new_tablespacename = $12;
					n->nowait = $13;
					$$ = (Node *) n;
				}
		|	ALTER INDEX qualified_name alter_table_cmds
				{
					AlterTableStmt *n = makeNode(AlterTableStmt);

					n->relation = $3;
					n->cmds = $4;
					n->objtype = OBJECT_INDEX;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
		|	ALTER INDEX IF_P EXISTS qualified_name alter_table_cmds
				{
					AlterTableStmt *n = makeNode(AlterTableStmt);

					n->relation = $5;
					n->cmds = $6;
					n->objtype = OBJECT_INDEX;
					n->missing_ok = true;
					$$ = (Node *) n;
				}
		|	ALTER INDEX qualified_name index_partition_cmd
				{
					AlterTableStmt *n = makeNode(AlterTableStmt);

					n->relation = $3;
					n->cmds = list_make1($4);
					n->objtype = OBJECT_INDEX;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
		|	ALTER INDEX ALL IN_P TABLESPACE name SET TABLESPACE name opt_nowait
				{
					AlterTableMoveAllStmt *n =
						makeNode(AlterTableMoveAllStmt);

					n->orig_tablespacename = $6;
					n->objtype = OBJECT_INDEX;
					n->roles = NIL;
					n->new_tablespacename = $9;
					n->nowait = $10;
					$$ = (Node *) n;
				}
		|	ALTER INDEX ALL IN_P TABLESPACE name OWNED BY role_list SET TABLESPACE name opt_nowait
				{
					AlterTableMoveAllStmt *n =
						makeNode(AlterTableMoveAllStmt);

					n->orig_tablespacename = $6;
					n->objtype = OBJECT_INDEX;
					n->roles = $9;
					n->new_tablespacename = $12;
					n->nowait = $13;
					$$ = (Node *) n;
				}
		|	ALTER SEQUENCE qualified_name alter_table_cmds
				{
					AlterTableStmt *n = makeNode(AlterTableStmt);

					n->relation = $3;
					n->cmds = $4;
					n->objtype = OBJECT_SEQUENCE;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
		|	ALTER SEQUENCE IF_P EXISTS qualified_name alter_table_cmds
				{
					AlterTableStmt *n = makeNode(AlterTableStmt);

					n->relation = $5;
					n->cmds = $6;
					n->objtype = OBJECT_SEQUENCE;
					n->missing_ok = true;
					$$ = (Node *) n;
				}
		|	ALTER VIEW qualified_name alter_table_cmds
				{
					AlterTableStmt *n = makeNode(AlterTableStmt);

					n->relation = $3;
					n->cmds = $4;
					n->objtype = OBJECT_VIEW;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
		|	ALTER VIEW IF_P EXISTS qualified_name alter_table_cmds
				{
					AlterTableStmt *n = makeNode(AlterTableStmt);

					n->relation = $5;
					n->cmds = $6;
					n->objtype = OBJECT_VIEW;
					n->missing_ok = true;
					$$ = (Node *) n;
				}
		|	ALTER MATERIALIZED VIEW qualified_name alter_table_cmds
				{
					AlterTableStmt *n = makeNode(AlterTableStmt);

					n->relation = $4;
					n->cmds = $5;
					n->objtype = OBJECT_MATVIEW;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
		|	ALTER MATERIALIZED VIEW IF_P EXISTS qualified_name alter_table_cmds
				{
					AlterTableStmt *n = makeNode(AlterTableStmt);

					n->relation = $6;
					n->cmds = $7;
					n->objtype = OBJECT_MATVIEW;
					n->missing_ok = true;
					$$ = (Node *) n;
				}
		|	ALTER MATERIALIZED VIEW ALL IN_P TABLESPACE name SET TABLESPACE name opt_nowait
				{
					AlterTableMoveAllStmt *n =
						makeNode(AlterTableMoveAllStmt);

					n->orig_tablespacename = $7;
					n->objtype = OBJECT_MATVIEW;
					n->roles = NIL;
					n->new_tablespacename = $10;
					n->nowait = $11;
					$$ = (Node *) n;
				}
		|	ALTER MATERIALIZED VIEW ALL IN_P TABLESPACE name OWNED BY role_list SET TABLESPACE name opt_nowait
				{
					AlterTableMoveAllStmt *n =
						makeNode(AlterTableMoveAllStmt);

					n->orig_tablespacename = $7;
					n->objtype = OBJECT_MATVIEW;
					n->roles = $10;
					n->new_tablespacename = $13;
					n->nowait = $14;
					$$ = (Node *) n;
				}
		|	ALTER FOREIGN TABLE relation_expr alter_table_cmds
				{
					AlterTableStmt *n = makeNode(AlterTableStmt);

					n->relation = $4;
					n->cmds = $5;
					n->objtype = OBJECT_FOREIGN_TABLE;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
		|	ALTER FOREIGN TABLE IF_P EXISTS relation_expr alter_table_cmds
				{
					AlterTableStmt *n = makeNode(AlterTableStmt);

					n->relation = $6;
					n->cmds = $7;
					n->objtype = OBJECT_FOREIGN_TABLE;
					n->missing_ok = true;
					$$ = (Node *) n;
				}
		;

alter_table_cmds:
			alter_table_cmd							{ $$ = list_make1($1); }
			| alter_table_cmds ',' alter_table_cmd	{ $$ = lappend($1, $3); }
		;

partition_cmd:
			/* ALTER TABLE <name> ATTACH PARTITION <table_name> FOR VALUES */
			ATTACH PARTITION qualified_name PartitionBoundSpec
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);
					PartitionCmd *cmd = makeNode(PartitionCmd);

					n->subtype = AT_AttachPartition;
					cmd->name = $3;
					cmd->bound = $4;
					cmd->concurrent = false;
					n->def = (Node *) cmd;

					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> DETACH PARTITION <partition_name> [CONCURRENTLY] */
			| DETACH PARTITION qualified_name opt_concurrently
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);
					PartitionCmd *cmd = makeNode(PartitionCmd);

					n->subtype = AT_DetachPartition;
					cmd->name = $3;
					cmd->bound = NULL;
					cmd->concurrent = $4;
					n->def = (Node *) cmd;

					$$ = (Node *) n;
				}
			| DETACH PARTITION qualified_name FINALIZE
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);
					PartitionCmd *cmd = makeNode(PartitionCmd);

					n->subtype = AT_DetachPartitionFinalize;
					cmd->name = $3;
					cmd->bound = NULL;
					cmd->concurrent = false;
					n->def = (Node *) cmd;
					$$ = (Node *) n;
				}
		;

index_partition_cmd:
			/* ALTER INDEX <name> ATTACH PARTITION <index_name> */
			ATTACH PARTITION qualified_name
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);
					PartitionCmd *cmd = makeNode(PartitionCmd);

					n->subtype = AT_AttachPartition;
					cmd->name = $3;
					cmd->bound = NULL;
					cmd->concurrent = false;
					n->def = (Node *) cmd;

					$$ = (Node *) n;
				}
		;

alter_table_cmd:
			/* ALTER TABLE <name> ADD <coldef> */
			ADD_P columnDef
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_AddColumn;
					n->def = $2;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> ADD IF NOT EXISTS <coldef> */
			| ADD_P IF_P NOT EXISTS columnDef
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_AddColumn;
					n->def = $5;
					n->missing_ok = true;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> ADD COLUMN <coldef> */
			| ADD_P COLUMN columnDef
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_AddColumn;
					n->def = $3;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> ADD COLUMN IF NOT EXISTS <coldef> */
			| ADD_P COLUMN IF_P NOT EXISTS columnDef
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_AddColumn;
					n->def = $6;
					n->missing_ok = true;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> ALTER [COLUMN] <colname> {SET DEFAULT <expr>|DROP DEFAULT} */
			| ALTER opt_column ColId alter_column_default
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_ColumnDefault;
					n->name = $3;
					n->def = $4;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> ALTER [COLUMN] <colname> DROP NOT NULL */
			| ALTER opt_column ColId DROP NOT NULL_P
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_DropNotNull;
					n->name = $3;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> ALTER [COLUMN] <colname> SET NOT NULL */
			| ALTER opt_column ColId SET NOT NULL_P
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_SetNotNull;
					n->name = $3;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> ALTER [COLUMN] <colname> SET EXPRESSION AS <expr> */
			| ALTER opt_column ColId SET EXPRESSION AS '(' a_expr ')'
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_SetExpression;
					n->name = $3;
					n->def = $8;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> ALTER [COLUMN] <colname> DROP EXPRESSION */
			| ALTER opt_column ColId DROP EXPRESSION
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_DropExpression;
					n->name = $3;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> ALTER [COLUMN] <colname> DROP EXPRESSION IF EXISTS */
			| ALTER opt_column ColId DROP EXPRESSION IF_P EXISTS
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_DropExpression;
					n->name = $3;
					n->missing_ok = true;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> ALTER [COLUMN] <colname> SET STATISTICS */
			| ALTER opt_column ColId SET STATISTICS set_statistics_value
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_SetStatistics;
					n->name = $3;
					n->def = $6;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> ALTER [COLUMN] <colnum> SET STATISTICS */
			| ALTER opt_column Iconst SET STATISTICS set_statistics_value
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					if ($3 <= 0 || $3 > PG_INT16_MAX)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								 errmsg("column number must be in range from 1 to %d", PG_INT16_MAX),
								 parser_errposition(@3)));

					n->subtype = AT_SetStatistics;
					n->num = (int16) $3;
					n->def = $6;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> ALTER [COLUMN] <colname> SET ( column_parameter = value [, ... ] ) */
			| ALTER opt_column ColId SET reloptions
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_SetOptions;
					n->name = $3;
					n->def = (Node *) $5;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> ALTER [COLUMN] <colname> RESET ( column_parameter [, ... ] ) */
			| ALTER opt_column ColId RESET reloptions
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_ResetOptions;
					n->name = $3;
					n->def = (Node *) $5;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> ALTER [COLUMN] <colname> SET STORAGE <storagemode> */
			| ALTER opt_column ColId SET column_storage
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_SetStorage;
					n->name = $3;
					n->def = (Node *) makeString($5);
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> ALTER [COLUMN] <colname> SET COMPRESSION <cm> */
			| ALTER opt_column ColId SET column_compression
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_SetCompression;
					n->name = $3;
					n->def = (Node *) makeString($5);
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> ALTER [COLUMN] <colname> ADD GENERATED ... AS IDENTITY ... */
			| ALTER opt_column ColId ADD_P GENERATED generated_when AS IDENTITY_P OptParenthesizedSeqOptList
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);
					Constraint *c = makeNode(Constraint);

					c->contype = CONSTR_IDENTITY;
					c->generated_when = $6;
					c->options = $9;
					c->location = @5;

					n->subtype = AT_AddIdentity;
					n->name = $3;
					n->def = (Node *) c;

					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> ALTER [COLUMN] <colname> SET <sequence options>/RESET */
			| ALTER opt_column ColId alter_identity_column_option_list
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_SetIdentity;
					n->name = $3;
					n->def = (Node *) $4;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> ALTER [COLUMN] <colname> DROP IDENTITY */
			| ALTER opt_column ColId DROP IDENTITY_P
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_DropIdentity;
					n->name = $3;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> ALTER [COLUMN] <colname> DROP IDENTITY IF EXISTS */
			| ALTER opt_column ColId DROP IDENTITY_P IF_P EXISTS
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_DropIdentity;
					n->name = $3;
					n->missing_ok = true;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> DROP [COLUMN] IF EXISTS <colname> [RESTRICT|CASCADE] */
			| DROP opt_column IF_P EXISTS ColId opt_drop_behavior
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_DropColumn;
					n->name = $5;
					n->behavior = $6;
					n->missing_ok = true;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> DROP [COLUMN] <colname> [RESTRICT|CASCADE] */
			| DROP opt_column ColId opt_drop_behavior
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_DropColumn;
					n->name = $3;
					n->behavior = $4;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			/*
			 * ALTER TABLE <name> ALTER [COLUMN] <colname> [SET DATA] TYPE <typename>
			 *		[ USING <expression> ]
			 */
			| ALTER opt_column ColId opt_set_data TYPE_P Typename opt_collate_clause alter_using
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);
					ColumnDef *def = makeNode(ColumnDef);

					n->subtype = AT_AlterColumnType;
					n->name = $3;
					n->def = (Node *) def;
					/* We only use these fields of the ColumnDef node - 我们仅使用 ColumnDef 节点的这些字段 */
					def->typeName = $6;
					def->collClause = (CollateClause *) $7;
					def->raw_default = $8;
					def->location = @3;
					$$ = (Node *) n;
				}
			/* ALTER FOREIGN TABLE <name> ALTER [COLUMN] <colname> OPTIONS */
			| ALTER opt_column ColId alter_generic_options
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_AlterColumnGenericOptions;
					n->name = $3;
					n->def = (Node *) $4;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> ADD CONSTRAINT ... */
			| ADD_P TableConstraint
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_AddConstraint;
					n->def = $2;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> ALTER CONSTRAINT ... */
			| ALTER CONSTRAINT name ConstraintAttributeSpec
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);
					ATAlterConstraint *c = makeNode(ATAlterConstraint);

					n->subtype = AT_AlterConstraint;
					n->def = (Node *) c;
					c->conname = $3;
					if ($4 & (CAS_NOT_ENFORCED | CAS_ENFORCED))
						c->alterEnforceability = true;
					if ($4 & (CAS_DEFERRABLE | CAS_NOT_DEFERRABLE |
							  CAS_INITIALLY_DEFERRED | CAS_INITIALLY_IMMEDIATE))
						c->alterDeferrability = true;
					if ($4 & CAS_NO_INHERIT)
						c->alterInheritability = true;
					/* handle unsupported case with specific error message - 用具体的错误信息处理不支持的情况 */
					if ($4 & CAS_NOT_VALID)
						ereport(ERROR,
								errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								errmsg("constraints cannot be altered to be NOT VALID"),
								parser_errposition(@4));
					processCASbits($4, @4, "FOREIGN KEY",
									&c->deferrable,
									&c->initdeferred,
									&c->is_enforced,
									NULL,
									&c->noinherit,
									yyscanner);
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> ALTER CONSTRAINT INHERIT */
			| ALTER CONSTRAINT name INHERIT
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);
					ATAlterConstraint *c = makeNode(ATAlterConstraint);

					n->subtype = AT_AlterConstraint;
					n->def = (Node *) c;
					c->conname = $3;
					c->alterInheritability = true;
					c->noinherit = false;

					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> VALIDATE CONSTRAINT ... */
			| VALIDATE CONSTRAINT name
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_ValidateConstraint;
					n->name = $3;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> DROP CONSTRAINT IF EXISTS <name> [RESTRICT|CASCADE] */
			| DROP CONSTRAINT IF_P EXISTS name opt_drop_behavior
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_DropConstraint;
					n->name = $5;
					n->behavior = $6;
					n->missing_ok = true;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> DROP CONSTRAINT <name> [RESTRICT|CASCADE] */
			| DROP CONSTRAINT name opt_drop_behavior
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_DropConstraint;
					n->name = $3;
					n->behavior = $4;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> SET WITHOUT OIDS, for backward compat */
			| SET WITHOUT OIDS
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_DropOids;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> CLUSTER ON <indexname> */
			| CLUSTER ON name
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_ClusterOn;
					n->name = $3;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> SET WITHOUT CLUSTER */
			| SET WITHOUT CLUSTER
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_DropCluster;
					n->name = NULL;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> SET LOGGED */
			| SET LOGGED
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_SetLogged;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> SET UNLOGGED */
			| SET UNLOGGED
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_SetUnLogged;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> ENABLE TRIGGER <trig> */
			| ENABLE_P TRIGGER name
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_EnableTrig;
					n->name = $3;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> ENABLE ALWAYS TRIGGER <trig> */
			| ENABLE_P ALWAYS TRIGGER name
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_EnableAlwaysTrig;
					n->name = $4;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> ENABLE REPLICA TRIGGER <trig> */
			| ENABLE_P REPLICA TRIGGER name
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_EnableReplicaTrig;
					n->name = $4;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> ENABLE TRIGGER ALL */
			| ENABLE_P TRIGGER ALL
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_EnableTrigAll;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> ENABLE TRIGGER USER */
			| ENABLE_P TRIGGER USER
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_EnableTrigUser;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> DISABLE TRIGGER <trig> */
			| DISABLE_P TRIGGER name
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_DisableTrig;
					n->name = $3;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> DISABLE TRIGGER ALL */
			| DISABLE_P TRIGGER ALL
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_DisableTrigAll;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> DISABLE TRIGGER USER */
			| DISABLE_P TRIGGER USER
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_DisableTrigUser;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> ENABLE RULE <rule> */
			| ENABLE_P RULE name
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_EnableRule;
					n->name = $3;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> ENABLE ALWAYS RULE <rule> */
			| ENABLE_P ALWAYS RULE name
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_EnableAlwaysRule;
					n->name = $4;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> ENABLE REPLICA RULE <rule> */
			| ENABLE_P REPLICA RULE name
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_EnableReplicaRule;
					n->name = $4;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> DISABLE RULE <rule> */
			| DISABLE_P RULE name
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_DisableRule;
					n->name = $3;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> INHERIT <parent> */
			| INHERIT qualified_name
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_AddInherit;
					n->def = (Node *) $2;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> NO INHERIT <parent> */
			| NO INHERIT qualified_name
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_DropInherit;
					n->def = (Node *) $3;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> OF <type_name> */
			| OF any_name
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);
					TypeName   *def = makeTypeNameFromNameList($2);

					def->location = @2;
					n->subtype = AT_AddOf;
					n->def = (Node *) def;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> NOT OF */
			| NOT OF
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_DropOf;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> OWNER TO RoleSpec */
			| OWNER TO RoleSpec
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_ChangeOwner;
					n->newowner = $3;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> SET ACCESS METHOD { <amname> | DEFAULT } */
			| SET ACCESS METHOD set_access_method_name
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_SetAccessMethod;
					n->name = $4;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> SET TABLESPACE <tablespacename> */
			| SET TABLESPACE name
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_SetTableSpace;
					n->name = $3;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> SET (...) */
			| SET reloptions
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_SetRelOptions;
					n->def = (Node *) $2;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> RESET (...) */
			| RESET reloptions
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_ResetRelOptions;
					n->def = (Node *) $2;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> REPLICA IDENTITY */
			| REPLICA IDENTITY_P replica_identity
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_ReplicaIdentity;
					n->def = $3;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> ENABLE ROW LEVEL SECURITY */
			| ENABLE_P ROW LEVEL SECURITY
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_EnableRowSecurity;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> DISABLE ROW LEVEL SECURITY */
			| DISABLE_P ROW LEVEL SECURITY
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_DisableRowSecurity;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> FORCE ROW LEVEL SECURITY */
			| FORCE ROW LEVEL SECURITY
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_ForceRowSecurity;
					$$ = (Node *) n;
				}
			/* ALTER TABLE <name> NO FORCE ROW LEVEL SECURITY */
			| NO FORCE ROW LEVEL SECURITY
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_NoForceRowSecurity;
					$$ = (Node *) n;
				}
			| alter_generic_options
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_GenericOptions;
					n->def = (Node *) $1;
					$$ = (Node *) n;
				}
		;

alter_column_default:
			SET DEFAULT a_expr			{ $$ = $3; }
			| DROP DEFAULT				{ $$ = NULL; }
		;

opt_collate_clause:
			COLLATE any_name
				{
					CollateClause *n = makeNode(CollateClause);

					n->arg = NULL;
					n->collname = $2;
					n->location = @1;
					$$ = (Node *) n;
				}
			| /* EMPTY - 空 */				{ $$ = NULL; }
		;

alter_using:
			USING a_expr				{ $$ = $2; }
			| /* EMPTY - 空 */				{ $$ = NULL; }
		;

replica_identity:
			NOTHING
				{
					ReplicaIdentityStmt *n = makeNode(ReplicaIdentityStmt);

					n->identity_type = REPLICA_IDENTITY_NOTHING;
					n->name = NULL;
					$$ = (Node *) n;
				}
			| FULL
				{
					ReplicaIdentityStmt *n = makeNode(ReplicaIdentityStmt);

					n->identity_type = REPLICA_IDENTITY_FULL;
					n->name = NULL;
					$$ = (Node *) n;
				}
			| DEFAULT
				{
					ReplicaIdentityStmt *n = makeNode(ReplicaIdentityStmt);

					n->identity_type = REPLICA_IDENTITY_DEFAULT;
					n->name = NULL;
					$$ = (Node *) n;
				}
			| USING INDEX name
				{
					ReplicaIdentityStmt *n = makeNode(ReplicaIdentityStmt);

					n->identity_type = REPLICA_IDENTITY_INDEX;
					n->name = $3;
					$$ = (Node *) n;
				}
;

reloptions:
			'(' reloption_list ')'					{ $$ = $2; }
		;

opt_reloptions:		WITH reloptions					{ $$ = $2; }
			 |		/* EMPTY - 空 */						{ $$ = NIL; }
		;

reloption_list:
			reloption_elem							{ $$ = list_make1($1); }
			| reloption_list ',' reloption_elem		{ $$ = lappend($1, $3); }
		;

/* This should match def_elem and also allow qualified names - 这应该与 def_elem 匹配，并且还允许限定名称 */
reloption_elem:
			ColLabel '=' def_arg
				{
					$$ = makeDefElem($1, (Node *) $3, @1);
				}
			| ColLabel
				{
					$$ = makeDefElem($1, NULL, @1);
				}
			| ColLabel '.' ColLabel '=' def_arg
				{
					$$ = makeDefElemExtended($1, $3, (Node *) $5,
											 DEFELEM_UNSPEC, @1);
				}
			| ColLabel '.' ColLabel
				{
					$$ = makeDefElemExtended($1, $3, NULL, DEFELEM_UNSPEC, @1);
				}
		;

alter_identity_column_option_list:
			alter_identity_column_option
				{ $$ = list_make1($1); }
			| alter_identity_column_option_list alter_identity_column_option
				{ $$ = lappend($1, $2); }
		;

alter_identity_column_option:
			RESTART
				{
					$$ = makeDefElem("restart", NULL, @1);
				}
			| RESTART opt_with NumericOnly
				{
					$$ = makeDefElem("restart", (Node *) $3, @1);
				}
			| SET SeqOptElem
				{
					if (strcmp($2->defname, "as") == 0 ||
						strcmp($2->defname, "restart") == 0 ||
						strcmp($2->defname, "owned_by") == 0)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("sequence option \"%s\" not supported here", $2->defname),
								 parser_errposition(@2)));
					$$ = $2;
				}
			| SET GENERATED generated_when
				{
					$$ = makeDefElem("generated", (Node *) makeInteger($3), @1);
				}
		;

set_statistics_value:
			SignedIconst					{ $$ = (Node *) makeInteger($1); }
			| DEFAULT						{ $$ = NULL; }
		;

set_access_method_name:
			ColId							{ $$ = $1; }
			| DEFAULT						{ $$ = NULL; }
		;

PartitionBoundSpec:
			/* a HASH partition - HASH 分区 */
			FOR VALUES WITH '(' hash_partbound ')'
				{
					ListCell   *lc;
					PartitionBoundSpec *n = makeNode(PartitionBoundSpec);

					n->strategy = PARTITION_STRATEGY_HASH;
					n->modulus = n->remainder = -1;

					foreach (lc, $5)
					{
						DefElem    *opt = lfirst_node(DefElem, lc);

						if (strcmp(opt->defname, "modulus") == 0)
						{
							if (n->modulus != -1)
								ereport(ERROR,
										(errcode(ERRCODE_DUPLICATE_OBJECT),
										 errmsg("modulus for hash partition provided more than once"),
										 parser_errposition(opt->location)));
							n->modulus = defGetInt32(opt);
						}
						else if (strcmp(opt->defname, "remainder") == 0)
						{
							if (n->remainder != -1)
								ereport(ERROR,
										(errcode(ERRCODE_DUPLICATE_OBJECT),
										 errmsg("remainder for hash partition provided more than once"),
										 parser_errposition(opt->location)));
							n->remainder = defGetInt32(opt);
						}
						else
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("unrecognized hash partition bound specification \"%s\"",
											opt->defname),
									 parser_errposition(opt->location)));
					}

					if (n->modulus == -1)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("modulus for hash partition must be specified"),
								 parser_errposition(@3)));
					if (n->remainder == -1)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("remainder for hash partition must be specified"),
								 parser_errposition(@3)));

					n->location = @3;

					$$ = n;
				}

			/* a LIST partition - LIST 分区 */
			| FOR VALUES IN_P '(' expr_list ')'
				{
					PartitionBoundSpec *n = makeNode(PartitionBoundSpec);

					n->strategy = PARTITION_STRATEGY_LIST;
					n->is_default = false;
					n->listdatums = $5;
					n->location = @3;

					$$ = n;
				}

			/* a RANGE partition - RANGE 分区 */
			| FOR VALUES FROM '(' expr_list ')' TO '(' expr_list ')'
				{
					PartitionBoundSpec *n = makeNode(PartitionBoundSpec);

					n->strategy = PARTITION_STRATEGY_RANGE;
					n->is_default = false;
					n->lowerdatums = $5;
					n->upperdatums = $9;
					n->location = @3;

					$$ = n;
				}

			/* a DEFAULT partition - DEFAULT 分区 */
			| DEFAULT
				{
					PartitionBoundSpec *n = makeNode(PartitionBoundSpec);

					n->is_default = true;
					n->location = @1;

					$$ = n;
				}
		;

hash_partbound_elem:
		NonReservedWord Iconst
			{
				$$ = makeDefElem($1, (Node *) makeInteger($2), @1);
			}
		;

hash_partbound:
		hash_partbound_elem
			{
				$$ = list_make1($1);
			}
		| hash_partbound ',' hash_partbound_elem
			{
				$$ = lappend($1, $3);
			}
		;

/*****************************************************************************
 *
 *	ALTER TYPE
 *
 * really variants of the ALTER TABLE subcommands with different spellings
 *****************************************************************************/

AlterCompositeTypeStmt:
			ALTER TYPE_P any_name alter_type_cmds
				{
					AlterTableStmt *n = makeNode(AlterTableStmt);

					/* can't use qualified_name, sigh - 不能使用 qualified_name，唉 */
					n->relation = makeRangeVarFromAnyName($3, @3, yyscanner);
					n->cmds = $4;
					n->objtype = OBJECT_TYPE;
					$$ = (Node *) n;
				}
			;

alter_type_cmds:
			alter_type_cmd							{ $$ = list_make1($1); }
			| alter_type_cmds ',' alter_type_cmd	{ $$ = lappend($1, $3); }
		;

alter_type_cmd:
			/* ALTER TYPE <name> ADD ATTRIBUTE <coldef> [RESTRICT|CASCADE] */
			ADD_P ATTRIBUTE TableFuncElement opt_drop_behavior
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_AddColumn;
					n->def = $3;
					n->behavior = $4;
					$$ = (Node *) n;
				}
			/* ALTER TYPE <name> DROP ATTRIBUTE IF EXISTS <attname> [RESTRICT|CASCADE] */
			| DROP ATTRIBUTE IF_P EXISTS ColId opt_drop_behavior
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_DropColumn;
					n->name = $5;
					n->behavior = $6;
					n->missing_ok = true;
					$$ = (Node *) n;
				}
			/* ALTER TYPE <name> DROP ATTRIBUTE <attname> [RESTRICT|CASCADE] */
			| DROP ATTRIBUTE ColId opt_drop_behavior
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);

					n->subtype = AT_DropColumn;
					n->name = $3;
					n->behavior = $4;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			/* ALTER TYPE <name> ALTER ATTRIBUTE <attname> [SET DATA] TYPE <typename> [RESTRICT|CASCADE] */
			| ALTER ATTRIBUTE ColId opt_set_data TYPE_P Typename opt_collate_clause opt_drop_behavior
				{
					AlterTableCmd *n = makeNode(AlterTableCmd);
					ColumnDef *def = makeNode(ColumnDef);

					n->subtype = AT_AlterColumnType;
					n->name = $3;
					n->def = (Node *) def;
					n->behavior = $8;
					/* We only use these fields of the ColumnDef node - 我们仅使用 ColumnDef 节点的这些字段 */
					def->typeName = $6;
					def->collClause = (CollateClause *) $7;
					def->raw_default = NULL;
					def->location = @3;
					$$ = (Node *) n;
				}
		;


/*****************************************************************************
 *
 *		QUERY :
 *				close <portalname>
 *
 * 查询：close <portalname>
 *****************************************************************************/

ClosePortalStmt:
			CLOSE cursor_name
				{
					ClosePortalStmt *n = makeNode(ClosePortalStmt);

					n->portalname = $2;
					$$ = (Node *) n;
				}
			| CLOSE ALL
				{
					ClosePortalStmt *n = makeNode(ClosePortalStmt);

					n->portalname = NULL;
					$$ = (Node *) n;
				}
		;


/*****************************************************************************
 *
 *		QUERY :
 *				COPY relname [(columnList)] FROM/TO file [WITH] [(options)]
 *				COPY ( query ) TO file	[WITH] [(options)]
 *
 *				where 'query' can be one of:
 *				{ SELECT | UPDATE | INSERT | DELETE }
 *
 *				and 'file' can be one of:
 *				{ PROGRAM 'command' | STDIN | STDOUT | 'filename' }
 *
 *				In the preferred syntax the options are comma-separated
 *				and use generic identifiers instead of keywords.  The pre-9.0
 *				syntax had a hard-wired, space-separated set of options.
 *
 *				Really old syntax, from versions 7.2 and prior:
 *				COPY [ BINARY ] table FROM/TO file
 *					[ [ USING ] DELIMITERS 'delimiter' ] ]
 *					[ WITH NULL AS 'null string' ]
 *				This option placement is not supported with COPY (query...).
 *
 * 查询：COPY 语法定义
 *****************************************************************************/

CopyStmt:	COPY opt_binary qualified_name opt_column_list
			copy_from opt_program copy_file_name copy_delimiter opt_with
			copy_options where_clause
				{
					CopyStmt *n = makeNode(CopyStmt);

					n->relation = $3;
					n->query = NULL;
					n->attlist = $4;
					n->is_from = $5;
					n->is_program = $6;
					n->filename = $7;
					n->whereClause = $11;

					if (n->is_program && n->filename == NULL)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("STDIN/STDOUT not allowed with PROGRAM"),
								 parser_errposition(@8)));

					if (!n->is_from && n->whereClause != NULL)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("WHERE clause not allowed with COPY TO"),
								 parser_errposition(@11)));

					n->options = NIL;
					/* Concatenate user-supplied flags - 连接用户提供的标志 */
					if ($2)
						n->options = lappend(n->options, $2);
					if ($8)
						n->options = lappend(n->options, $8);
					if ($10)
						n->options = list_concat(n->options, $10);
					$$ = (Node *) n;
				}
			| COPY '(' PreparableStmt ')' TO opt_program copy_file_name opt_with copy_options
				{
					CopyStmt *n = makeNode(CopyStmt);

					n->relation = NULL;
					n->query = $3;
					n->attlist = NIL;
					n->is_from = false;
					n->is_program = $6;
					n->filename = $7;
					n->options = $9;

					if (n->is_program && n->filename == NULL)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("STDIN/STDOUT not allowed with PROGRAM"),
								 parser_errposition(@5)));

					$$ = (Node *) n;
				}
		;

copy_from:
			FROM									{ $$ = true; }
			| TO									{ $$ = false; }
		;

opt_program:
			PROGRAM									{ $$ = true; }
			| /* EMPTY - 空 */							{ $$ = false; }
		;

/*
 * copy_file_name NULL indicates stdio is used. Whether stdin or stdout is
 * used depends on the direction. (It really doesn't make sense to copy from
 * stdout. We silently correct the "typo".)		 - AY 9/94
 * copy_file_name 为 NULL 表示使用 stdio。是使用 stdin 还是 stdout 取决于复制方向。（从 stdout 进行 copy 确实没有意义。我们默默地纠正了这个“笔误”。）- AY 9/94
 */
copy_file_name:
			Sconst									{ $$ = $1; }
			| STDIN									{ $$ = NULL; }
			| STDOUT								{ $$ = NULL; }
		;

copy_options: copy_opt_list							{ $$ = $1; }
			| '(' copy_generic_opt_list ')'			{ $$ = $2; }
		;

/* old COPY option syntax - 旧的 COPY 选项语法 */
copy_opt_list:
			copy_opt_list copy_opt_item				{ $$ = lappend($1, $2); }
			| /* EMPTY - 空 */							{ $$ = NIL; }
		;

copy_opt_item:
			BINARY
				{
					$$ = makeDefElem("format", (Node *) makeString("binary"), @1);
				}
			| FREEZE
				{
					$$ = makeDefElem("freeze", (Node *) makeBoolean(true), @1);
				}
			| DELIMITER opt_as Sconst
				{
					$$ = makeDefElem("delimiter", (Node *) makeString($3), @1);
				}
			| NULL_P opt_as Sconst
				{
					$$ = makeDefElem("null", (Node *) makeString($3), @1);
				}
			| CSV
				{
					$$ = makeDefElem("format", (Node *) makeString("csv"), @1);
				}
			| HEADER_P
				{
					$$ = makeDefElem("header", (Node *) makeBoolean(true), @1);
				}
			| QUOTE opt_as Sconst
				{
					$$ = makeDefElem("quote", (Node *) makeString($3), @1);
				}
			| ESCAPE opt_as Sconst
				{
					$$ = makeDefElem("escape", (Node *) makeString($3), @1);
				}
			| FORCE QUOTE columnList
				{
					$$ = makeDefElem("force_quote", (Node *) $3, @1);
				}
			| FORCE QUOTE '*'
				{
					$$ = makeDefElem("force_quote", (Node *) makeNode(A_Star), @1);
				}
			| FORCE NOT NULL_P columnList
				{
					$$ = makeDefElem("force_not_null", (Node *) $4, @1);
				}
			| FORCE NOT NULL_P '*'
				{
					$$ = makeDefElem("force_not_null", (Node *) makeNode(A_Star), @1);
				}
			| FORCE NULL_P columnList
				{
					$$ = makeDefElem("force_null", (Node *) $3, @1);
				}
			| FORCE NULL_P '*'
				{
					$$ = makeDefElem("force_null", (Node *) makeNode(A_Star), @1);
				}
			| ENCODING Sconst
				{
					$$ = makeDefElem("encoding", (Node *) makeString($2), @1);
				}
		;

/* The following exist for backward compatibility with very old versions - 以下内容是为了与非常旧的版本进行向后兼容而存在的 */

opt_binary:
			BINARY
				{
					$$ = makeDefElem("format", (Node *) makeString("binary"), @1);
				}
			| /* EMPTY - 空 */								{ $$ = NULL; }
		;

copy_delimiter:
			opt_using DELIMITERS Sconst
				{
					$$ = makeDefElem("delimiter", (Node *) makeString($3), @2);
				}
			| /* EMPTY - 空 */								{ $$ = NULL; }
		;

opt_using:
			USING
			| /* EMPTY - 空 */
		;

/* new COPY option syntax - 新的 COPY 选项语法 */
copy_generic_opt_list:
			copy_generic_opt_elem
				{
					$$ = list_make1($1);
				}
			| copy_generic_opt_list ',' copy_generic_opt_elem
				{
					$$ = lappend($1, $3);
				}
		;

copy_generic_opt_elem:
			ColLabel copy_generic_opt_arg
				{
					$$ = makeDefElem($1, $2, @1);
				}
		;

copy_generic_opt_arg:
			opt_boolean_or_string			{ $$ = (Node *) makeString($1); }
			| NumericOnly					{ $$ = (Node *) $1; }
			| '*'							{ $$ = (Node *) makeNode(A_Star); }
			| DEFAULT                       { $$ = (Node *) makeString("default"); }
			| '(' copy_generic_opt_arg_list ')'		{ $$ = (Node *) $2; }
			| /* EMPTY - 空 */					{ $$ = NULL; }
		;

copy_generic_opt_arg_list:
			  copy_generic_opt_arg_list_item
				{
					$$ = list_make1($1);
				}
			| copy_generic_opt_arg_list ',' copy_generic_opt_arg_list_item
				{
					$$ = lappend($1, $3);
				}
		;

/* beware of emitting non-string list elements here; see commands/define.c - 注意不要在这里发出非字符串的列表元素；参见 commands/define.c */
copy_generic_opt_arg_list_item:
			opt_boolean_or_string	{ $$ = (Node *) makeString($1); }
		;


/*****************************************************************************
 *
 *		QUERY :
 *				CREATE TABLE relname
 *
 * 查询：CREATE TABLE relname
 *****************************************************************************/

CreateStmt:	CREATE OptTemp TABLE qualified_name '(' OptTableElementList ')'
			OptInherit OptPartitionSpec table_access_method_clause OptWith
			OnCommitOption OptTableSpace
				{
					CreateStmt *n = makeNode(CreateStmt);

					$4->relpersistence = $2;
					n->relation = $4;
					n->tableElts = $6;
					n->inhRelations = $8;
					n->partspec = $9;
					n->ofTypename = NULL;
					n->constraints = NIL;
					n->accessMethod = $10;
					n->options = $11;
					n->oncommit = $12;
					n->tablespacename = $13;
					n->if_not_exists = false;
					$$ = (Node *) n;
				}
		| CREATE OptTemp TABLE IF_P NOT EXISTS qualified_name '('
			OptTableElementList ')' OptInherit OptPartitionSpec table_access_method_clause
			OptWith OnCommitOption OptTableSpace
				{
					CreateStmt *n = makeNode(CreateStmt);

					$7->relpersistence = $2;
					n->relation = $7;
					n->tableElts = $9;
					n->inhRelations = $11;
					n->partspec = $12;
					n->ofTypename = NULL;
					n->constraints = NIL;
					n->accessMethod = $13;
					n->options = $14;
					n->oncommit = $15;
					n->tablespacename = $16;
					n->if_not_exists = true;
					$$ = (Node *) n;
				}
		| CREATE OptTemp TABLE qualified_name OF any_name
			OptTypedTableElementList OptPartitionSpec table_access_method_clause
			OptWith OnCommitOption OptTableSpace
				{
					CreateStmt *n = makeNode(CreateStmt);

					$4->relpersistence = $2;
					n->relation = $4;
					n->tableElts = $7;
					n->inhRelations = NIL;
					n->partspec = $8;
					n->ofTypename = makeTypeNameFromNameList($6);
					n->ofTypename->location = @6;
					n->constraints = NIL;
					n->accessMethod = $9;
					n->options = $10;
					n->oncommit = $11;
					n->tablespacename = $12;
					n->if_not_exists = false;
					$$ = (Node *) n;
				}
		| CREATE OptTemp TABLE IF_P NOT EXISTS qualified_name OF any_name
			OptTypedTableElementList OptPartitionSpec table_access_method_clause
			OptWith OnCommitOption OptTableSpace
				{
					CreateStmt *n = makeNode(CreateStmt);

					$7->relpersistence = $2;
					n->relation = $7;
					n->tableElts = $10;
					n->inhRelations = NIL;
					n->partspec = $11;
					n->ofTypename = makeTypeNameFromNameList($9);
					n->ofTypename->location = @9;
					n->constraints = NIL;
					n->accessMethod = $12;
					n->options = $13;
					n->oncommit = $14;
					n->tablespacename = $15;
					n->if_not_exists = true;
					$$ = (Node *) n;
				}
		| CREATE OptTemp TABLE qualified_name PARTITION OF qualified_name
			OptTypedTableElementList PartitionBoundSpec OptPartitionSpec
			table_access_method_clause OptWith OnCommitOption OptTableSpace
				{
					CreateStmt *n = makeNode(CreateStmt);

					$4->relpersistence = $2;
					n->relation = $4;
					n->tableElts = $8;
					n->inhRelations = list_make1($7);
					n->partbound = $9;
					n->partspec = $10;
					n->ofTypename = NULL;
					n->constraints = NIL;
					n->accessMethod = $11;
					n->options = $12;
					n->oncommit = $13;
					n->tablespacename = $14;
					n->if_not_exists = false;
					$$ = (Node *) n;
				}
		| CREATE OptTemp TABLE IF_P NOT EXISTS qualified_name PARTITION OF
			qualified_name OptTypedTableElementList PartitionBoundSpec OptPartitionSpec
			table_access_method_clause OptWith OnCommitOption OptTableSpace
				{
					CreateStmt *n = makeNode(CreateStmt);

					$7->relpersistence = $2;
					n->relation = $7;
					n->tableElts = $11;
					n->inhRelations = list_make1($10);
					n->partbound = $12;
					n->partspec = $13;
					n->ofTypename = NULL;
					n->constraints = NIL;
					n->accessMethod = $14;
					n->options = $15;
					n->oncommit = $16;
					n->tablespacename = $17;
					n->if_not_exists = true;
					$$ = (Node *) n;
				}
		;

/*
 * Redundancy here is needed to avoid shift/reduce conflicts,
 * since TEMP is not a reserved word.  See also OptTempTableName.
 *
 * NOTE: we accept both GLOBAL and LOCAL options.  They currently do nothing,
 * but future versions might consider GLOBAL to request SQL-spec-compliant
 * temp table behavior, so warn about that.  Since we have no modules the
 * LOCAL keyword is really meaningless; furthermore, some other products
 * implement LOCAL as meaning the same as our default temp table behavior,
 * so we'll probably continue to treat LOCAL as a noise word.
 * 这里需要冗余以避免移进/规约冲突，因为 TEMP 不是保留字。另见 OptTempTableName。注意：我们接受 GLOBAL 和 LOCAL 选项。它们目前什么都不做，但未来的版本可能会考虑让 GLOBAL 请求符合 SQL 规范的临时表行为，因此对此发出警告。由于我们没有模块，LOCAL 关键字实际上是无意义的；此外，一些其他产品将 LOCAL 实现为与我们默认的临时表行为具有相同含义，因此我们将可能继续将 LOCAL 视为噪词。
 */
OptTemp:	TEMPORARY					{ $$ = RELPERSISTENCE_TEMP; }
			| TEMP						{ $$ = RELPERSISTENCE_TEMP; }
			| LOCAL TEMPORARY			{ $$ = RELPERSISTENCE_TEMP; }
			| LOCAL TEMP				{ $$ = RELPERSISTENCE_TEMP; }
			| GLOBAL TEMPORARY
				{
					ereport(WARNING,
							(errmsg("GLOBAL is deprecated in temporary table creation"),
							 parser_errposition(@1)));
					$$ = RELPERSISTENCE_TEMP;
				}
			| GLOBAL TEMP
				{
					ereport(WARNING,
							(errmsg("GLOBAL is deprecated in temporary table creation"),
							 parser_errposition(@1)));
					$$ = RELPERSISTENCE_TEMP;
				}
			| UNLOGGED					{ $$ = RELPERSISTENCE_UNLOGGED; }
			| /* EMPTY - 空 */					{ $$ = RELPERSISTENCE_PERMANENT; }
		;

OptTableElementList:
			TableElementList					{ $$ = $1; }
			| /* EMPTY - 空 */							{ $$ = NIL; }
		;

OptTypedTableElementList:
			'(' TypedTableElementList ')'		{ $$ = $2; }
			| /* EMPTY - 空 */							{ $$ = NIL; }
		;

TableElementList:
			TableElement
				{
					$$ = list_make1($1);
				}
			| TableElementList ',' TableElement
				{
					$$ = lappend($1, $3);
				}
		;

TypedTableElementList:
			TypedTableElement
				{
					$$ = list_make1($1);
				}
			| TypedTableElementList ',' TypedTableElement
				{
					$$ = lappend($1, $3);
				}
		;

TableElement:
			columnDef							{ $$ = $1; }
			| TableLikeClause					{ $$ = $1; }
			| TableConstraint					{ $$ = $1; }
		;

TypedTableElement:
			columnOptions						{ $$ = $1; }
			| TableConstraint					{ $$ = $1; }
		;

columnDef:	ColId Typename opt_column_storage opt_column_compression create_generic_options ColQualList
				{
					ColumnDef *n = makeNode(ColumnDef);

					n->colname = $1;
					n->typeName = $2;
					n->storage_name = $3;
					n->compression = $4;
					n->inhcount = 0;
					n->is_local = true;
					n->is_not_null = false;
					n->is_from_type = false;
					n->storage = 0;
					n->raw_default = NULL;
					n->cooked_default = NULL;
					n->collOid = InvalidOid;
					n->fdwoptions = $5;
					SplitColQualList($6, &n->constraints, &n->collClause,
									 yyscanner);
					n->location = @1;
					$$ = (Node *) n;
				}
		;

columnOptions:	ColId ColQualList
				{
					ColumnDef *n = makeNode(ColumnDef);

					n->colname = $1;
					n->typeName = NULL;
					n->inhcount = 0;
					n->is_local = true;
					n->is_not_null = false;
					n->is_from_type = false;
					n->storage = 0;
					n->raw_default = NULL;
					n->cooked_default = NULL;
					n->collOid = InvalidOid;
					SplitColQualList($2, &n->constraints, &n->collClause,
									 yyscanner);
					n->location = @1;
					$$ = (Node *) n;
				}
				| ColId WITH OPTIONS ColQualList
				{
					ColumnDef *n = makeNode(ColumnDef);

					n->colname = $1;
					n->typeName = NULL;
					n->inhcount = 0;
					n->is_local = true;
					n->is_not_null = false;
					n->is_from_type = false;
					n->storage = 0;
					n->raw_default = NULL;
					n->cooked_default = NULL;
					n->collOid = InvalidOid;
					SplitColQualList($4, &n->constraints, &n->collClause,
									 yyscanner);
					n->location = @1;
					$$ = (Node *) n;
				}
		;

column_compression:
			COMPRESSION ColId						{ $$ = $2; }
			| COMPRESSION DEFAULT					{ $$ = pstrdup("default"); }
		;

opt_column_compression:
			column_compression						{ $$ = $1; }
			| /* EMPTY - 空 */								{ $$ = NULL; }
		;

column_storage:
			STORAGE ColId							{ $$ = $2; }
			| STORAGE DEFAULT						{ $$ = pstrdup("default"); }
		;

opt_column_storage:
			column_storage							{ $$ = $1; }
			| /* EMPTY - 空 */								{ $$ = NULL; }
		;

ColQualList:
			ColQualList ColConstraint				{ $$ = lappend($1, $2); }
			| /* EMPTY - 空 */								{ $$ = NIL; }
		;

ColConstraint:
			CONSTRAINT name ColConstraintElem
				{
					Constraint *n = castNode(Constraint, $3);

					n->conname = $2;
					n->location = @1;
					$$ = (Node *) n;
				}
			| ColConstraintElem						{ $$ = $1; }
			| ConstraintAttr						{ $$ = $1; }
			| COLLATE any_name
				{
					/*
					 * Note: the CollateClause is momentarily included in
					 * the list built by ColQualList, but we split it out
					 * again in SplitColQualList.
					 * 注意：CollateClause 会暂时包含在由 ColQualList 构建的列表中，但我们在 SplitColQualList 中会再次将其分离出来。
					 */
					CollateClause *n = makeNode(CollateClause);

					n->arg = NULL;
					n->collname = $2;
					n->location = @1;
					$$ = (Node *) n;
				}
		;

/* DEFAULT NULL is already the default for Postgres.
 * But define it here and carry it forward into the system
 * to make it explicit.
 * - thomas 1998-09-13
 *
 * WITH NULL and NULL are not SQL-standard syntax elements,
 * so leave them out. Use DEFAULT NULL to explicitly indicate
 * that a column may have that value. WITH NULL leads to
 * shift/reduce conflicts with WITH TIME ZONE anyway.
 * - thomas 1999-01-08
 *
 * DEFAULT expression must be b_expr not a_expr to prevent shift/reduce
 * conflict on NOT (since NOT might start a subsequent NOT NULL constraint,
 * or be part of a_expr NOT LIKE or similar constructs).
 * DEFAULT NULL 已经是 Postgres 的默认值了。但在此处进行定义并将其携带到系统中以使其明确。- thomas 1998-09-13。WITH NULL 和 NULL 不是 SQL 标准的语法元素，因此将它们省去。使用 DEFAULT NULL 来显式指示列可以具有该值。反正 WITH NULL 会导致与 WITH TIME ZONE 的移进/规约冲突。- thomas 1999-01-08。DEFAULT 表达式必须 be b_expr 而不是 a_expr，以防止在 NOT 上产生移进/规约冲突（因为 NOT 可能会开启后续的 NOT NULL 约束，或者是 a_expr NOT LIKE 或类似结构的一部分）。
 */
ColConstraintElem:
			NOT NULL_P opt_no_inherit
				{
					Constraint *n = makeNode(Constraint);

					n->contype = CONSTR_NOTNULL;
					n->location = @1;
					n->is_no_inherit = $3;
					n->is_enforced = true;
					n->skip_validation = false;
					n->initially_valid = true;
					$$ = (Node *) n;
				}
			| NULL_P
				{
					Constraint *n = makeNode(Constraint);

					n->contype = CONSTR_NULL;
					n->location = @1;
					$$ = (Node *) n;
				}
			| UNIQUE opt_unique_null_treatment opt_definition OptConsTableSpace
				{
					Constraint *n = makeNode(Constraint);

					n->contype = CONSTR_UNIQUE;
					n->location = @1;
					n->nulls_not_distinct = !$2;
					n->keys = NULL;
					n->options = $3;
					n->indexname = NULL;
					n->indexspace = $4;
					$$ = (Node *) n;
				}
			| PRIMARY KEY opt_definition OptConsTableSpace
				{
					Constraint *n = makeNode(Constraint);

					n->contype = CONSTR_PRIMARY;
					n->location = @1;
					n->keys = NULL;
					n->options = $3;
					n->indexname = NULL;
					n->indexspace = $4;
					$$ = (Node *) n;
				}
			| CHECK '(' a_expr ')' opt_no_inherit
				{
					Constraint *n = makeNode(Constraint);

					n->contype = CONSTR_CHECK;
					n->location = @1;
					n->is_no_inherit = $5;
					n->raw_expr = $3;
					n->cooked_expr = NULL;
					n->is_enforced = true;
					n->skip_validation = false;
					n->initially_valid = true;
					$$ = (Node *) n;
				}
			| DEFAULT b_expr
				{
					Constraint *n = makeNode(Constraint);

					n->contype = CONSTR_DEFAULT;
					n->location = @1;
					n->raw_expr = $2;
					n->cooked_expr = NULL;
					$$ = (Node *) n;
				}
			| GENERATED generated_when AS IDENTITY_P OptParenthesizedSeqOptList
				{
					Constraint *n = makeNode(Constraint);

					n->contype = CONSTR_IDENTITY;
					n->generated_when = $2;
					n->options = $5;
					n->location = @1;
					$$ = (Node *) n;
				}
			| GENERATED generated_when AS '(' a_expr ')' opt_virtual_or_stored
				{
					Constraint *n = makeNode(Constraint);

					n->contype = CONSTR_GENERATED;
					n->generated_when = $2;
					n->raw_expr = $5;
					n->cooked_expr = NULL;
					n->generated_kind = $7;
					n->location = @1;

					/*
					 * Can't do this in the grammar because of shift/reduce
					 * conflicts.  (IDENTITY allows both ALWAYS and BY
					 * DEFAULT, but generated columns only allow ALWAYS.)  We
					 * can also give a more useful error message and location.
					 * 由于移进/规约冲突，无法在语法层面进行此操作。（IDENTITY 允许 ALWAYS 和 BY DEFAULT，但生成列（generated columns）仅允许 ALWAYS。）我们还可以给出更有用的错误消息和位置。
					 */
					if ($2 != ATTRIBUTE_IDENTITY_ALWAYS)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("for a generated column, GENERATED ALWAYS must be specified"),
								 parser_errposition(@2)));

					$$ = (Node *) n;
				}
			| REFERENCES qualified_name opt_column_list key_match key_actions
				{
					Constraint *n = makeNode(Constraint);

					n->contype = CONSTR_FOREIGN;
					n->location = @1;
					n->pktable = $2;
					n->fk_attrs = NIL;
					n->pk_attrs = $3;
					n->fk_matchtype = $4;
					n->fk_upd_action = ($5)->updateAction->action;
					n->fk_del_action = ($5)->deleteAction->action;
					n->fk_del_set_cols = ($5)->deleteAction->cols;
					n->is_enforced = true;
					n->skip_validation = false;
					n->initially_valid = true;
					$$ = (Node *) n;
				}
		;

opt_unique_null_treatment:
			NULLS_P DISTINCT		{ $$ = true; }
			| NULLS_P NOT DISTINCT	{ $$ = false; }
			| /* EMPTY - 空 */				{ $$ = true; }
		;

generated_when:
			ALWAYS			{ $$ = ATTRIBUTE_IDENTITY_ALWAYS; }
			| BY DEFAULT	{ $$ = ATTRIBUTE_IDENTITY_BY_DEFAULT; }
		;

opt_virtual_or_stored:
			STORED			{ $$ = ATTRIBUTE_GENERATED_STORED; }
			| VIRTUAL		{ $$ = ATTRIBUTE_GENERATED_VIRTUAL; }
			| /* EMPTY - 空 */		{ $$ = ATTRIBUTE_GENERATED_VIRTUAL; }
		;

/*
 * ConstraintAttr represents constraint attributes, which we parse as if
 * they were independent constraint clauses, in order to avoid shift/reduce
 * conflicts (since NOT might start either an independent NOT NULL clause
 * or an attribute).  parse_utilcmd.c is responsible for attaching the
 * attribute information to the preceding "real" constraint node, and for
 * complaining if attribute clauses appear in the wrong place or wrong
 * combinations.
 *
 * See also ConstraintAttributeSpec, which can be used in places where
 * there is no parsing conflict.  (Note: currently, NOT VALID and NO INHERIT
 * are allowed clauses in ConstraintAttributeSpec, but not here.  Someday we
 * might need to allow them here too, but for the moment it doesn't seem
 * useful in the statements that use ConstraintAttr.)
 * ConstraintAttr 表示约束属性，我们像解析独立的约束子句一样对其进行解析，以避免移进/规约冲突（因为 NOT 可能会启动一个独立的 NOT NULL 子句或一个属性）。parse_utilcmd.c 负责将属性信息附加到前面的“真实”约束节点，并负责在属性子句出现在错误位置或错误组合时报错。另见 ConstraintAttributeSpec，它可以在没有解析冲突的地方使用。（注意：目前，ConstraintAttributeSpec 中允许 NOT VALID 和 NO INHERIT 子句，但这里不允许。有朝一日我们可能也需要在这里允许它们，但目前在接收 ConstraintAttr 的语句中它似乎没有什么用处。）
 */
ConstraintAttr:
			DEFERRABLE
				{
					Constraint *n = makeNode(Constraint);

					n->contype = CONSTR_ATTR_DEFERRABLE;
					n->location = @1;
					$$ = (Node *) n;
				}
			| NOT DEFERRABLE
				{
					Constraint *n = makeNode(Constraint);

					n->contype = CONSTR_ATTR_NOT_DEFERRABLE;
					n->location = @1;
					$$ = (Node *) n;
				}
			| INITIALLY DEFERRED
				{
					Constraint *n = makeNode(Constraint);

					n->contype = CONSTR_ATTR_DEFERRED;
					n->location = @1;
					$$ = (Node *) n;
				}
			| INITIALLY IMMEDIATE
				{
					Constraint *n = makeNode(Constraint);

					n->contype = CONSTR_ATTR_IMMEDIATE;
					n->location = @1;
					$$ = (Node *) n;
				}
			| ENFORCED
				{
					Constraint *n = makeNode(Constraint);

					n->contype = CONSTR_ATTR_ENFORCED;
					n->location = @1;
					$$ = (Node *) n;
				}
			| NOT ENFORCED
				{
					Constraint *n = makeNode(Constraint);

					n->contype = CONSTR_ATTR_NOT_ENFORCED;
					n->location = @1;
					$$ = (Node *) n;
				}
		;


TableLikeClause:
			LIKE qualified_name TableLikeOptionList
				{
					TableLikeClause *n = makeNode(TableLikeClause);

					n->relation = $2;
					n->options = $3;
					n->relationOid = InvalidOid;
					$$ = (Node *) n;
				}
		;

TableLikeOptionList:
				TableLikeOptionList INCLUDING TableLikeOption	{ $$ = $1 | $3; }
				| TableLikeOptionList EXCLUDING TableLikeOption	{ $$ = $1 & ~$3; }
				| /* EMPTY - 空 */						{ $$ = 0; }
		;

TableLikeOption:
				COMMENTS			{ $$ = CREATE_TABLE_LIKE_COMMENTS; }
				| COMPRESSION		{ $$ = CREATE_TABLE_LIKE_COMPRESSION; }
				| CONSTRAINTS		{ $$ = CREATE_TABLE_LIKE_CONSTRAINTS; }
				| DEFAULTS			{ $$ = CREATE_TABLE_LIKE_DEFAULTS; }
				| IDENTITY_P		{ $$ = CREATE_TABLE_LIKE_IDENTITY; }
				| GENERATED			{ $$ = CREATE_TABLE_LIKE_GENERATED; }
				| INDEXES			{ $$ = CREATE_TABLE_LIKE_INDEXES; }
				| STATISTICS		{ $$ = CREATE_TABLE_LIKE_STATISTICS; }
				| STORAGE			{ $$ = CREATE_TABLE_LIKE_STORAGE; }
				| ALL				{ $$ = CREATE_TABLE_LIKE_ALL; }
		;


/* ConstraintElem specifies constraint syntax which is not embedded into
 *	a column definition. ColConstraintElem specifies the embedded form.
 * - thomas 1997-12-03
 * ConstraintElem 指定了不嵌入到列定义中的约束语法。ColConstraintElem 指定了嵌入形式。- thomas 1997-12-03
 */
TableConstraint:
			CONSTRAINT name ConstraintElem
				{
					Constraint *n = castNode(Constraint, $3);

					n->conname = $2;
					n->location = @1;
					$$ = (Node *) n;
				}
			| ConstraintElem						{ $$ = $1; }
		;

ConstraintElem:
			CHECK '(' a_expr ')' ConstraintAttributeSpec
				{
					Constraint *n = makeNode(Constraint);

					n->contype = CONSTR_CHECK;
					n->location = @1;
					n->raw_expr = $3;
					n->cooked_expr = NULL;
					processCASbits($5, @5, "CHECK",
								   NULL, NULL, &n->is_enforced, &n->skip_validation,
								   &n->is_no_inherit, yyscanner);
					n->initially_valid = !n->skip_validation;
					$$ = (Node *) n;
				}
			| NOT NULL_P ColId ConstraintAttributeSpec
				{
					Constraint *n = makeNode(Constraint);

					n->contype = CONSTR_NOTNULL;
					n->location = @1;
					n->keys = list_make1(makeString($3));
					processCASbits($4, @4, "NOT NULL",
								   NULL, NULL, NULL, &n->skip_validation,
								   &n->is_no_inherit, yyscanner);
					n->initially_valid = !n->skip_validation;
					$$ = (Node *) n;
				}
			| UNIQUE opt_unique_null_treatment '(' columnList opt_without_overlaps ')' opt_c_include opt_definition OptConsTableSpace
				ConstraintAttributeSpec
				{
					Constraint *n = makeNode(Constraint);

					n->contype = CONSTR_UNIQUE;
					n->location = @1;
					n->nulls_not_distinct = !$2;
					n->keys = $4;
					n->without_overlaps = $5;
					n->including = $7;
					n->options = $8;
					n->indexname = NULL;
					n->indexspace = $9;
					processCASbits($10, @10, "UNIQUE",
								   &n->deferrable, &n->initdeferred, NULL,
								   NULL, NULL, yyscanner);
					$$ = (Node *) n;
				}
			| UNIQUE ExistingIndex ConstraintAttributeSpec
				{
					Constraint *n = makeNode(Constraint);

					n->contype = CONSTR_UNIQUE;
					n->location = @1;
					n->keys = NIL;
					n->including = NIL;
					n->options = NIL;
					n->indexname = $2;
					n->indexspace = NULL;
					processCASbits($3, @3, "UNIQUE",
								   &n->deferrable, &n->initdeferred, NULL,
								   NULL, NULL, yyscanner);
					$$ = (Node *) n;
				}
			| PRIMARY KEY '(' columnList opt_without_overlaps ')' opt_c_include opt_definition OptConsTableSpace
				ConstraintAttributeSpec
				{
					Constraint *n = makeNode(Constraint);

					n->contype = CONSTR_PRIMARY;
					n->location = @1;
					n->keys = $4;
					n->without_overlaps = $5;
					n->including = $7;
					n->options = $8;
					n->indexname = NULL;
					n->indexspace = $9;
					processCASbits($10, @10, "PRIMARY KEY",
								   &n->deferrable, &n->initdeferred, NULL,
								   NULL, NULL, yyscanner);
					$$ = (Node *) n;
				}
			| PRIMARY KEY ExistingIndex ConstraintAttributeSpec
				{
					Constraint *n = makeNode(Constraint);

					n->contype = CONSTR_PRIMARY;
					n->location = @1;
					n->keys = NIL;
					n->including = NIL;
					n->options = NIL;
					n->indexname = $3;
					n->indexspace = NULL;
					processCASbits($4, @4, "PRIMARY KEY",
								   &n->deferrable, &n->initdeferred, NULL,
								   NULL, NULL, yyscanner);
					$$ = (Node *) n;
				}
			| EXCLUDE access_method_clause '(' ExclusionConstraintList ')'
				opt_c_include opt_definition OptConsTableSpace OptWhereClause
				ConstraintAttributeSpec
				{
					Constraint *n = makeNode(Constraint);

					n->contype = CONSTR_EXCLUSION;
					n->location = @1;
					n->access_method = $2;
					n->exclusions = $4;
					n->including = $6;
					n->options = $7;
					n->indexname = NULL;
					n->indexspace = $8;
					n->where_clause = $9;
					processCASbits($10, @10, "EXCLUDE",
								   &n->deferrable, &n->initdeferred, NULL,
								   NULL, NULL, yyscanner);
					$$ = (Node *) n;
				}
			| FOREIGN KEY '(' columnList optionalPeriodName ')' REFERENCES qualified_name
				opt_column_and_period_list key_match key_actions ConstraintAttributeSpec
				{
					Constraint *n = makeNode(Constraint);

					n->contype = CONSTR_FOREIGN;
					n->location = @1;
					n->pktable = $8;
					n->fk_attrs = $4;
					if ($5)
					{
						n->fk_attrs = lappend(n->fk_attrs, $5);
						n->fk_with_period = true;
					}
					n->pk_attrs = linitial($9);
					if (lsecond($9))
					{
						n->pk_attrs = lappend(n->pk_attrs, lsecond($9));
						n->pk_with_period = true;
					}
					n->fk_matchtype = $10;
					n->fk_upd_action = ($11)->updateAction->action;
					n->fk_del_action = ($11)->deleteAction->action;
					n->fk_del_set_cols = ($11)->deleteAction->cols;
					processCASbits($12, @12, "FOREIGN KEY",
								   &n->deferrable, &n->initdeferred,
								   &n->is_enforced, &n->skip_validation, NULL,
								   yyscanner);
					n->initially_valid = !n->skip_validation;
					$$ = (Node *) n;
				}
		;

/*
 * DomainConstraint is separate from TableConstraint because the syntax for
 * NOT NULL constraints is different.  For table constraints, we need to
 * accept a column name, but for domain constraints, we don't.  (We could
 * accept something like NOT NULL VALUE, but that seems weird.)  CREATE DOMAIN
 * (which uses ColQualList) has for a long time accepted NOT NULL without a
 * column name, so it makes sense that ALTER DOMAIN (which uses
 * DomainConstraint) does as well.  None of these syntaxes are per SQL
 * standard; we are just living with the bits of inconsistency that have built
 * up over time.
 * DomainConstraint 与 TableConstraint 分离，因为 NOT NULL 约束的语法不同。对于表约束，我们需要接受列名，但对于域约束，我们不需要。（我们可以接受类似 NOT NULL VALUE 的内容，但那看起来很奇怪。）CREATE DOMAIN（使用 ColQualList）长期以来接受不带列名的 NOT NULL，因此使用 DomainConstraint 的 ALTER DOMAIN 也接受是合理的。这些语法都不符合 SQL 标准；我们只是在容忍随着时间推移积累起来的不一致。
 */
DomainConstraint:
			CONSTRAINT name DomainConstraintElem
				{
					Constraint *n = castNode(Constraint, $3);

					n->conname = $2;
					n->location = @1;
					$$ = (Node *) n;
				}
			| DomainConstraintElem					{ $$ = $1; }
		;

DomainConstraintElem:
			CHECK '(' a_expr ')' ConstraintAttributeSpec
				{
					Constraint *n = makeNode(Constraint);

					n->contype = CONSTR_CHECK;
					n->location = @1;
					n->raw_expr = $3;
					n->cooked_expr = NULL;
					processCASbits($5, @5, "CHECK",
								   NULL, NULL, NULL, &n->skip_validation,
								   &n->is_no_inherit, yyscanner);
					n->is_enforced = true;
					n->initially_valid = !n->skip_validation;
					$$ = (Node *) n;
				}
			| NOT NULL_P ConstraintAttributeSpec
				{
					Constraint *n = makeNode(Constraint);

					n->contype = CONSTR_NOTNULL;
					n->location = @1;
					n->keys = list_make1(makeString("value"));
					/* no NOT VALID, NO INHERIT support - 没有 NOT VALID, NO INHERIT 支持 */
					processCASbits($3, @3, "NOT NULL",
								   NULL, NULL, NULL,
								   NULL, NULL, yyscanner);
					n->initially_valid = true;
					$$ = (Node *) n;
				}
		;

opt_no_inherit:	NO INHERIT							{  $$ = true; }
			| /* EMPTY - 空 */							{  $$ = false; }
		;

opt_without_overlaps:
			WITHOUT OVERLAPS						{ $$ = true; }
			| /* EMPTY - 空 */								{ $$ = false; }
	;

opt_column_list:
			'(' columnList ')'						{ $$ = $2; }
			| /* EMPTY - 空 */								{ $$ = NIL; }
		;

columnList:
			columnElem								{ $$ = list_make1($1); }
			| columnList ',' columnElem				{ $$ = lappend($1, $3); }
		;

optionalPeriodName:
			',' PERIOD columnElem { $$ = $3; }
			| /* EMPTY - 空 */               { $$ = NULL; }
	;

opt_column_and_period_list:
			'(' columnList optionalPeriodName ')'			{ $$ = list_make2($2, $3); }
			| /* EMPTY - 空 */								{ $$ = list_make2(NIL, NULL); }
		;

columnElem: ColId
				{
					$$ = (Node *) makeString($1);
				}
		;

opt_c_include:	INCLUDE '(' columnList ')'			{ $$ = $3; }
			 |		/* EMPTY - 空 */						{ $$ = NIL; }
		;

key_match:  MATCH FULL
			{
				$$ = FKCONSTR_MATCH_FULL;
			}
		| MATCH PARTIAL
			{
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("MATCH PARTIAL not yet implemented"),
						 parser_errposition(@1)));
				$$ = FKCONSTR_MATCH_PARTIAL;
			}
		| MATCH SIMPLE
			{
				$$ = FKCONSTR_MATCH_SIMPLE;
			}
		| /* EMPTY - 空 */
			{
				$$ = FKCONSTR_MATCH_SIMPLE;
			}
		;

ExclusionConstraintList:
			ExclusionConstraintElem					{ $$ = list_make1($1); }
			| ExclusionConstraintList ',' ExclusionConstraintElem
													{ $$ = lappend($1, $3); }
		;

ExclusionConstraintElem: index_elem WITH any_operator
			{
				$$ = list_make2($1, $3);
			}
			/* allow OPERATOR() decoration for the benefit of ruleutils.c - 允许为了 ruleutils.c 的利益使用 OPERATOR() 修饰 */
			| index_elem WITH OPERATOR '(' any_operator ')'
			{
				$$ = list_make2($1, $5);
			}
		;

OptWhereClause:
			WHERE '(' a_expr ')'					{ $$ = $3; }
			| /* EMPTY - 空 */								{ $$ = NULL; }
		;

key_actions:
			key_update
				{
					KeyActions *n = palloc(sizeof(KeyActions));

					n->updateAction = $1;
					n->deleteAction = palloc(sizeof(KeyAction));
					n->deleteAction->action = FKCONSTR_ACTION_NOACTION;
					n->deleteAction->cols = NIL;
					$$ = n;
				}
			| key_delete
				{
					KeyActions *n = palloc(sizeof(KeyActions));

					n->updateAction = palloc(sizeof(KeyAction));
					n->updateAction->action = FKCONSTR_ACTION_NOACTION;
					n->updateAction->cols = NIL;
					n->deleteAction = $1;
					$$ = n;
				}
			| key_update key_delete
				{
					KeyActions *n = palloc(sizeof(KeyActions));

					n->updateAction = $1;
					n->deleteAction = $2;
					$$ = n;
				}
			| key_delete key_update
				{
					KeyActions *n = palloc(sizeof(KeyActions));

					n->updateAction = $2;
					n->deleteAction = $1;
					$$ = n;
				}
			| /* EMPTY - 空 */
				{
					KeyActions *n = palloc(sizeof(KeyActions));

					n->updateAction = palloc(sizeof(KeyAction));
					n->updateAction->action = FKCONSTR_ACTION_NOACTION;
					n->updateAction->cols = NIL;
					n->deleteAction = palloc(sizeof(KeyAction));
					n->deleteAction->action = FKCONSTR_ACTION_NOACTION;
					n->deleteAction->cols = NIL;
					$$ = n;
				}
		;

key_update: ON UPDATE key_action
				{
					if (($3)->cols)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("a column list with %s is only supported for ON DELETE actions",
										($3)->action == FKCONSTR_ACTION_SETNULL ? "SET NULL" : "SET DEFAULT"),
								 parser_errposition(@1)));
					$$ = $3;
				}
		;

key_delete: ON DELETE_P key_action
				{
					$$ = $3;
				}
		;

key_action:
			NO ACTION
				{
					KeyAction *n = palloc(sizeof(KeyAction));

					n->action = FKCONSTR_ACTION_NOACTION;
					n->cols = NIL;
					$$ = n;
				}
			| RESTRICT
				{
					KeyAction *n = palloc(sizeof(KeyAction));

					n->action = FKCONSTR_ACTION_RESTRICT;
					n->cols = NIL;
					$$ = n;
				}
			| CASCADE
				{
					KeyAction *n = palloc(sizeof(KeyAction));

					n->action = FKCONSTR_ACTION_CASCADE;
					n->cols = NIL;
					$$ = n;
				}
			| SET NULL_P opt_column_list
				{
					KeyAction *n = palloc(sizeof(KeyAction));

					n->action = FKCONSTR_ACTION_SETNULL;
					n->cols = $3;
					$$ = n;
				}
			| SET DEFAULT opt_column_list
				{
					KeyAction *n = palloc(sizeof(KeyAction));

					n->action = FKCONSTR_ACTION_SETDEFAULT;
					n->cols = $3;
					$$ = n;
				}
		;

OptInherit: INHERITS '(' qualified_name_list ')'	{ $$ = $3; }
			| /* EMPTY - 空 */								{ $$ = NIL; }
		;

/* Optional partition key specification - 可选的分区键规范 */
OptPartitionSpec: PartitionSpec	{ $$ = $1; }
			| /* EMPTY - 空 */			{ $$ = NULL; }
		;

PartitionSpec: PARTITION BY ColId '(' part_params ')'
				{
					PartitionSpec *n = makeNode(PartitionSpec);

					n->strategy = parsePartitionStrategy($3, @3, yyscanner);
					n->partParams = $5;
					n->location = @1;

					$$ = n;
				}
		;

part_params:	part_elem						{ $$ = list_make1($1); }
			| part_params ',' part_elem			{ $$ = lappend($1, $3); }
		;

part_elem: ColId opt_collate opt_qualified_name
				{
					PartitionElem *n = makeNode(PartitionElem);

					n->name = $1;
					n->expr = NULL;
					n->collation = $2;
					n->opclass = $3;
					n->location = @1;
					$$ = n;
				}
			| func_expr_windowless opt_collate opt_qualified_name
				{
					PartitionElem *n = makeNode(PartitionElem);

					n->name = NULL;
					n->expr = $1;
					n->collation = $2;
					n->opclass = $3;
					n->location = @1;
					$$ = n;
				}
			| '(' a_expr ')' opt_collate opt_qualified_name
				{
					PartitionElem *n = makeNode(PartitionElem);

					n->name = NULL;
					n->expr = $2;
					n->collation = $4;
					n->opclass = $5;
					n->location = @1;
					$$ = n;
				}
		;

table_access_method_clause:
			USING name							{ $$ = $2; }
			| /* EMPTY - 空 */							{ $$ = NULL; }
		;

/* WITHOUT OIDS is legacy only - WITHOUT OIDS 仅是历史遗留 */
OptWith:
			WITH reloptions				{ $$ = $2; }
			| WITHOUT OIDS				{ $$ = NIL; }
			| /* EMPTY - 空 */					{ $$ = NIL; }
		;

OnCommitOption:  ON COMMIT DROP				{ $$ = ONCOMMIT_DROP; }
			| ON COMMIT DELETE_P ROWS		{ $$ = ONCOMMIT_DELETE_ROWS; }
			| ON COMMIT PRESERVE ROWS		{ $$ = ONCOMMIT_PRESERVE_ROWS; }
			| /* EMPTY - 空 */						{ $$ = ONCOMMIT_NOOP; }
		;

OptTableSpace:   TABLESPACE name					{ $$ = $2; }
			| /* EMPTY - 空 */								{ $$ = NULL; }
		;

OptConsTableSpace:   USING INDEX TABLESPACE name	{ $$ = $4; }
			| /* EMPTY - 空 */								{ $$ = NULL; }
		;

ExistingIndex:   USING INDEX name					{ $$ = $3; }
		;

/*****************************************************************************
 *
 *		QUERY :
 *				CREATE STATISTICS [[IF NOT EXISTS] stats_name] [(stat types)]
 *					ON expression-list FROM from_list
 *
 * Note: the expectation here is that the clauses after ON are a subset of
 * SELECT syntax, allowing for expressions and joined tables, and probably
 * someday a WHERE clause.  Much less than that is currently implemented,
 * but the grammar accepts it and then we'll throw FEATURE_NOT_SUPPORTED
 * errors as necessary at execution.
 *
 * Statistics name is optional unless IF NOT EXISTS is specified.
 *
 * 查询：CREATE STATISTICS 语法
 *****************************************************************************/

CreateStatsStmt:
			CREATE STATISTICS opt_qualified_name
			opt_name_list ON stats_params FROM from_list
				{
					CreateStatsStmt *n = makeNode(CreateStatsStmt);

					n->defnames = $3;
					n->stat_types = $4;
					n->exprs = $6;
					n->relations = $8;
					n->stxcomment = NULL;
					n->if_not_exists = false;
					$$ = (Node *) n;
				}
			| CREATE STATISTICS IF_P NOT EXISTS any_name
			opt_name_list ON stats_params FROM from_list
				{
					CreateStatsStmt *n = makeNode(CreateStatsStmt);

					n->defnames = $6;
					n->stat_types = $7;
					n->exprs = $9;
					n->relations = $11;
					n->stxcomment = NULL;
					n->if_not_exists = true;
					$$ = (Node *) n;
				}
			;

/*
 * Statistics attributes can be either simple column references, or arbitrary
 * expressions in parens.  For compatibility with index attributes permitted
 * in CREATE INDEX, we allow an expression that's just a function call to be
 * written without parens.
 * 统计信息属性可以是简单的列引用，也可以是括号中的任意表达式。为了与 CREATE INDEX 中允许的索引属性兼容，我们允许将仅是函数调用的表达式写为不带括号的形式。
 */

stats_params:	stats_param							{ $$ = list_make1($1); }
			| stats_params ',' stats_param			{ $$ = lappend($1, $3); }
		;

stats_param:	ColId
				{
					$$ = makeNode(StatsElem);
					$$->name = $1;
					$$->expr = NULL;
				}
			| func_expr_windowless
				{
					$$ = makeNode(StatsElem);
					$$->name = NULL;
					$$->expr = $1;
				}
			| '(' a_expr ')'
				{
					$$ = makeNode(StatsElem);
					$$->name = NULL;
					$$->expr = $2;
				}
		;

/*****************************************************************************
 *
 *		QUERY :
 *				ALTER STATISTICS [IF EXISTS] stats_name
 *					SET STATISTICS  <SignedIconst>
 *
 * 查询：ALTER STATISTICS 语法
 *****************************************************************************/

AlterStatsStmt:
			ALTER STATISTICS any_name SET STATISTICS set_statistics_value
				{
					AlterStatsStmt *n = makeNode(AlterStatsStmt);

					n->defnames = $3;
					n->missing_ok = false;
					n->stxstattarget = $6;
					$$ = (Node *) n;
				}
			| ALTER STATISTICS IF_P EXISTS any_name SET STATISTICS set_statistics_value
				{
					AlterStatsStmt *n = makeNode(AlterStatsStmt);

					n->defnames = $5;
					n->missing_ok = true;
					n->stxstattarget = $8;
					$$ = (Node *) n;
				}
			;

/*****************************************************************************
 *
 *		QUERY :
 *				CREATE TABLE relname AS SelectStmt [ WITH [NO] DATA ]
 *
 *
 * Note: SELECT ... INTO is a now-deprecated alternative for this.
 *
 * 查询：CREATE TABLE AS 语法。注意：SELECT ... INTO 现在是已弃用的替代方案。
 *****************************************************************************/

CreateAsStmt:
		CREATE OptTemp TABLE create_as_target AS SelectStmt opt_with_data
				{
					CreateTableAsStmt *ctas = makeNode(CreateTableAsStmt);

					ctas->query = $6;
					ctas->into = $4;
					ctas->objtype = OBJECT_TABLE;
					ctas->is_select_into = false;
					ctas->if_not_exists = false;
					/* cram additional flags into the IntoClause - 把额外的标志塞进 IntoClause 中 */
					$4->rel->relpersistence = $2;
					$4->skipData = !($7);
					$$ = (Node *) ctas;
				}
		| CREATE OptTemp TABLE IF_P NOT EXISTS create_as_target AS SelectStmt opt_with_data
				{
					CreateTableAsStmt *ctas = makeNode(CreateTableAsStmt);

					ctas->query = $9;
					ctas->into = $7;
					ctas->objtype = OBJECT_TABLE;
					ctas->is_select_into = false;
					ctas->if_not_exists = true;
					/* cram additional flags into the IntoClause - 把额外的标志塞进 IntoClause 中 */
					$7->rel->relpersistence = $2;
					$7->skipData = !($10);
					$$ = (Node *) ctas;
				}
		;

create_as_target:
			qualified_name opt_column_list table_access_method_clause
			OptWith OnCommitOption OptTableSpace
				{
					$$ = makeNode(IntoClause);
					$$->rel = $1;
					$$->colNames = $2;
					$$->accessMethod = $3;
					$$->options = $4;
					$$->onCommit = $5;
					$$->tableSpaceName = $6;
					$$->viewQuery = NULL;
					$$->skipData = false;		/* might get changed later - 稍后可能会更改 */
				}
		;

opt_with_data:
			WITH DATA_P								{ $$ = true; }
			| WITH NO DATA_P						{ $$ = false; }
			| /* EMPTY - 空 */								{ $$ = true; }
		;


/*****************************************************************************
 *
 *		QUERY :
 *				CREATE MATERIALIZED VIEW relname AS SelectStmt
 *
 * 查询：CREATE MATERIALIZED VIEW relname AS SelectStmt
 *****************************************************************************/

CreateMatViewStmt:
		CREATE OptNoLog MATERIALIZED VIEW create_mv_target AS SelectStmt opt_with_data
				{
					CreateTableAsStmt *ctas = makeNode(CreateTableAsStmt);

					ctas->query = $7;
					ctas->into = $5;
					ctas->objtype = OBJECT_MATVIEW;
					ctas->is_select_into = false;
					ctas->if_not_exists = false;
					/* cram additional flags into the IntoClause - 把额外的标志塞进 IntoClause 中 */
					$5->rel->relpersistence = $2;
					$5->skipData = !($8);
					$$ = (Node *) ctas;
				}
		| CREATE OptNoLog MATERIALIZED VIEW IF_P NOT EXISTS create_mv_target AS SelectStmt opt_with_data
				{
					CreateTableAsStmt *ctas = makeNode(CreateTableAsStmt);

					ctas->query = $10;
					ctas->into = $8;
					ctas->objtype = OBJECT_MATVIEW;
					ctas->is_select_into = false;
					ctas->if_not_exists = true;
					/* cram additional flags into the IntoClause - 把额外的标志塞进 IntoClause 中 */
					$8->rel->relpersistence = $2;
					$8->skipData = !($11);
					$$ = (Node *) ctas;
				}
		;

create_mv_target:
			qualified_name opt_column_list table_access_method_clause opt_reloptions OptTableSpace
				{
					$$ = makeNode(IntoClause);
					$$->rel = $1;
					$$->colNames = $2;
					$$->accessMethod = $3;
					$$->options = $4;
					$$->onCommit = ONCOMMIT_NOOP;
					$$->tableSpaceName = $5;
					$$->viewQuery = NULL;		/* filled at analysis time - 在分析时填充 */
					$$->skipData = false;		/* might get changed later - 稍后可能会更改 */
				}
		;

OptNoLog:	UNLOGGED					{ $$ = RELPERSISTENCE_UNLOGGED; }
			| /* EMPTY - 空 */					{ $$ = RELPERSISTENCE_PERMANENT; }
		;


/*****************************************************************************
 *
 *		QUERY :
 *				REFRESH MATERIALIZED VIEW qualified_name
 *
 * 查询：REFRESH MATERIALIZED VIEW qualified_name
 *****************************************************************************/

RefreshMatViewStmt:
			REFRESH MATERIALIZED VIEW opt_concurrently qualified_name opt_with_data
				{
					RefreshMatViewStmt *n = makeNode(RefreshMatViewStmt);

					n->concurrent = $4;
					n->relation = $5;
					n->skipData = !($6);
					$$ = (Node *) n;
				}
		;


/*****************************************************************************
 *
 *		QUERY :
 *				CREATE SEQUENCE seqname
 *				ALTER SEQUENCE seqname
 *
 * 查询：CREATE SEQUENCE seqname ALTER SEQUENCE seqname
 *****************************************************************************/

CreateSeqStmt:
			CREATE OptTemp SEQUENCE qualified_name OptSeqOptList
				{
					CreateSeqStmt *n = makeNode(CreateSeqStmt);

					$4->relpersistence = $2;
					n->sequence = $4;
					n->options = $5;
					n->ownerId = InvalidOid;
					n->if_not_exists = false;
					$$ = (Node *) n;
				}
			| CREATE OptTemp SEQUENCE IF_P NOT EXISTS qualified_name OptSeqOptList
				{
					CreateSeqStmt *n = makeNode(CreateSeqStmt);

					$7->relpersistence = $2;
					n->sequence = $7;
					n->options = $8;
					n->ownerId = InvalidOid;
					n->if_not_exists = true;
					$$ = (Node *) n;
				}
		;

AlterSeqStmt:
			ALTER SEQUENCE qualified_name SeqOptList
				{
					AlterSeqStmt *n = makeNode(AlterSeqStmt);

					n->sequence = $3;
					n->options = $4;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER SEQUENCE IF_P EXISTS qualified_name SeqOptList
				{
					AlterSeqStmt *n = makeNode(AlterSeqStmt);

					n->sequence = $5;
					n->options = $6;
					n->missing_ok = true;
					$$ = (Node *) n;
				}

		;

OptSeqOptList: SeqOptList							{ $$ = $1; }
			| /* EMPTY - 空 */								{ $$ = NIL; }
		;

OptParenthesizedSeqOptList: '(' SeqOptList ')'		{ $$ = $2; }
			| /* EMPTY - 空 */								{ $$ = NIL; }
		;

SeqOptList: SeqOptElem								{ $$ = list_make1($1); }
			| SeqOptList SeqOptElem					{ $$ = lappend($1, $2); }
		;

SeqOptElem: AS SimpleTypename
				{
					$$ = makeDefElem("as", (Node *) $2, @1);
				}
			| CACHE NumericOnly
				{
					$$ = makeDefElem("cache", (Node *) $2, @1);
				}
			| CYCLE
				{
					$$ = makeDefElem("cycle", (Node *) makeBoolean(true), @1);
				}
			| NO CYCLE
				{
					$$ = makeDefElem("cycle", (Node *) makeBoolean(false), @1);
				}
			| INCREMENT opt_by NumericOnly
				{
					$$ = makeDefElem("increment", (Node *) $3, @1);
				}
			| LOGGED
				{
					$$ = makeDefElem("logged", NULL, @1);
				}
			| MAXVALUE NumericOnly
				{
					$$ = makeDefElem("maxvalue", (Node *) $2, @1);
				}
			| MINVALUE NumericOnly
				{
					$$ = makeDefElem("minvalue", (Node *) $2, @1);
				}
			| NO MAXVALUE
				{
					$$ = makeDefElem("maxvalue", NULL, @1);
				}
			| NO MINVALUE
				{
					$$ = makeDefElem("minvalue", NULL, @1);
				}
			| OWNED BY any_name
				{
					$$ = makeDefElem("owned_by", (Node *) $3, @1);
				}
			| SEQUENCE NAME_P any_name
				{
					$$ = makeDefElem("sequence_name", (Node *) $3, @1);
				}
			| START opt_with NumericOnly
				{
					$$ = makeDefElem("start", (Node *) $3, @1);
				}
			| RESTART
				{
					$$ = makeDefElem("restart", NULL, @1);
				}
			| RESTART opt_with NumericOnly
				{
					$$ = makeDefElem("restart", (Node *) $3, @1);
				}
			| UNLOGGED
				{
					$$ = makeDefElem("unlogged", NULL, @1);
				}
		;

opt_by:		BY
			| /* EMPTY - 空 */
	  ;

NumericOnly:
			FCONST								{ $$ = (Node *) makeFloat($1); }
			| '+' FCONST						{ $$ = (Node *) makeFloat($2); }
			| '-' FCONST
				{
					Float	   *f = makeFloat($2);

					doNegateFloat(f);
					$$ = (Node *) f;
				}
			| SignedIconst						{ $$ = (Node *) makeInteger($1); }
		;

NumericOnly_list:	NumericOnly						{ $$ = list_make1($1); }
				| NumericOnly_list ',' NumericOnly	{ $$ = lappend($1, $3); }
		;

/*****************************************************************************
 *
 *		QUERIES :
 *				CREATE [OR REPLACE] [TRUSTED] [PROCEDURAL] LANGUAGE ...
 *				DROP [PROCEDURAL] LANGUAGE ...
 *
 * 查询：CREATE [OR REPLACE] [TRUSTED] [PROCEDURAL] LANGUAGE 语法
 *****************************************************************************/

CreatePLangStmt:
			CREATE opt_or_replace opt_trusted opt_procedural LANGUAGE name
			{
				/*
				 * We now interpret parameterless CREATE LANGUAGE as
				 * CREATE EXTENSION.  "OR REPLACE" is silently translated
				 * to "IF NOT EXISTS", which isn't quite the same, but
				 * seems more useful than throwing an error.  We just
				 * ignore TRUSTED, as the previous code would have too.
				 * 我们现在将无参数的 CREATE LANGUAGE 解释为 CREATE EXTENSION。"OR REPLACE" 被默默地转换为 "IF NOT EXISTS"，这不完全相同，但似乎比抛出错误更有用。我们只是忽略 TRUSTED，因为以前的代码也会这样做。
				 */
				CreateExtensionStmt *n = makeNode(CreateExtensionStmt);

				n->if_not_exists = $2;
				n->extname = $6;
				n->options = NIL;
				$$ = (Node *) n;
			}
			| CREATE opt_or_replace opt_trusted opt_procedural LANGUAGE name
			  HANDLER handler_name opt_inline_handler opt_validator
			{
				CreatePLangStmt *n = makeNode(CreatePLangStmt);

				n->replace = $2;
				n->plname = $6;
				n->plhandler = $8;
				n->plinline = $9;
				n->plvalidator = $10;
				n->pltrusted = $3;
				$$ = (Node *) n;
			}
		;

opt_trusted:
			TRUSTED									{ $$ = true; }
			| /* EMPTY - 空 */								{ $$ = false; }
		;

/* This ought to be just func_name, but that causes reduce/reduce conflicts
 * (CREATE LANGUAGE is the only place where func_name isn't followed by '(').
 * Work around by using simple names, instead.
 * 这应该只是 func_name，但那会引起规约/规约冲突（CREATE LANGUAGE 是唯一一个 func_name 后面没有跟 '(' 的地方）。通过使用简单名称来解决。
 */
handler_name:
			name						{ $$ = list_make1(makeString($1)); }
			| name attrs				{ $$ = lcons(makeString($1), $2); }
		;

opt_inline_handler:
			INLINE_P handler_name					{ $$ = $2; }
			| /* EMPTY - 空 */								{ $$ = NIL; }
		;

validator_clause:
			VALIDATOR handler_name					{ $$ = $2; }
			| NO VALIDATOR							{ $$ = NIL; }
		;

opt_validator:
			validator_clause						{ $$ = $1; }
			| /* EMPTY - 空 */								{ $$ = NIL; }
		;

opt_procedural:
			PROCEDURAL
			| /* EMPTY - 空 */
		;

/*****************************************************************************
 *
 *		QUERY:
 *             CREATE TABLESPACE tablespace LOCATION '/path/to/tablespace/'
 *
 * 查询：CREATE TABLESPACE 语法
 *****************************************************************************/

CreateTableSpaceStmt: CREATE TABLESPACE name OptTableSpaceOwner LOCATION Sconst opt_reloptions
				{
					CreateTableSpaceStmt *n = makeNode(CreateTableSpaceStmt);

					n->tablespacename = $3;
					n->owner = $4;
					n->location = $6;
					n->options = $7;
					$$ = (Node *) n;
				}
		;

OptTableSpaceOwner: OWNER RoleSpec		{ $$ = $2; }
			| /* EMPTY - 空 */				{ $$ = NULL; }
		;

/*****************************************************************************
 *
 *		QUERY :
 *				DROP TABLESPACE <tablespace>
 *
 *		No need for drop behaviour as we cannot implement dependencies for
 *		objects in other databases; we can only support RESTRICT.
 *
 * 查询：DROP TABLESPACE。由于我们无法为其他数据库中的对象实现依赖关系，因此无需删除行为；我们只能支持 RESTRICT。
 ****************************************************************************/

DropTableSpaceStmt: DROP TABLESPACE name
				{
					DropTableSpaceStmt *n = makeNode(DropTableSpaceStmt);

					n->tablespacename = $3;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
				|  DROP TABLESPACE IF_P EXISTS name
				{
					DropTableSpaceStmt *n = makeNode(DropTableSpaceStmt);

					n->tablespacename = $5;
					n->missing_ok = true;
					$$ = (Node *) n;
				}
		;

/*****************************************************************************
 *
 *		QUERY:
 *             CREATE EXTENSION extension
 *             [ WITH ] [ SCHEMA schema ] [ VERSION version ]
 *
 * 查询：CREATE EXTENSION 语法
 *****************************************************************************/

CreateExtensionStmt: CREATE EXTENSION name opt_with create_extension_opt_list
				{
					CreateExtensionStmt *n = makeNode(CreateExtensionStmt);

					n->extname = $3;
					n->if_not_exists = false;
					n->options = $5;
					$$ = (Node *) n;
				}
				| CREATE EXTENSION IF_P NOT EXISTS name opt_with create_extension_opt_list
				{
					CreateExtensionStmt *n = makeNode(CreateExtensionStmt);

					n->extname = $6;
					n->if_not_exists = true;
					n->options = $8;
					$$ = (Node *) n;
				}
		;

create_extension_opt_list:
			create_extension_opt_list create_extension_opt_item
				{ $$ = lappend($1, $2); }
			| /* EMPTY - 空 */
				{ $$ = NIL; }
		;

create_extension_opt_item:
			SCHEMA name
				{
					$$ = makeDefElem("schema", (Node *) makeString($2), @1);
				}
			| VERSION_P NonReservedWord_or_Sconst
				{
					$$ = makeDefElem("new_version", (Node *) makeString($2), @1);
				}
			| FROM NonReservedWord_or_Sconst
				{
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("CREATE EXTENSION ... FROM is no longer supported"),
							 parser_errposition(@1)));
				}
			| CASCADE
				{
					$$ = makeDefElem("cascade", (Node *) makeBoolean(true), @1);
				}
		;

/*****************************************************************************
 *
 * ALTER EXTENSION name UPDATE [ TO version ]
 *
 *****************************************************************************/

AlterExtensionStmt: ALTER EXTENSION name UPDATE alter_extension_opt_list
				{
					AlterExtensionStmt *n = makeNode(AlterExtensionStmt);

					n->extname = $3;
					n->options = $5;
					$$ = (Node *) n;
				}
		;

alter_extension_opt_list:
			alter_extension_opt_list alter_extension_opt_item
				{ $$ = lappend($1, $2); }
			| /* EMPTY - 空 */
				{ $$ = NIL; }
		;

alter_extension_opt_item:
			TO NonReservedWord_or_Sconst
				{
					$$ = makeDefElem("new_version", (Node *) makeString($2), @1);
				}
		;

/*****************************************************************************
 *
 * ALTER EXTENSION name ADD/DROP object-identifier
 *
 *****************************************************************************/

AlterExtensionContentsStmt:
			ALTER EXTENSION name add_drop object_type_name name
				{
					AlterExtensionContentsStmt *n = makeNode(AlterExtensionContentsStmt);

					n->extname = $3;
					n->action = $4;
					n->objtype = $5;
					n->object = (Node *) makeString($6);
					$$ = (Node *) n;
				}
			| ALTER EXTENSION name add_drop object_type_any_name any_name
				{
					AlterExtensionContentsStmt *n = makeNode(AlterExtensionContentsStmt);

					n->extname = $3;
					n->action = $4;
					n->objtype = $5;
					n->object = (Node *) $6;
					$$ = (Node *) n;
				}
			| ALTER EXTENSION name add_drop AGGREGATE aggregate_with_argtypes
				{
					AlterExtensionContentsStmt *n = makeNode(AlterExtensionContentsStmt);

					n->extname = $3;
					n->action = $4;
					n->objtype = OBJECT_AGGREGATE;
					n->object = (Node *) $6;
					$$ = (Node *) n;
				}
			| ALTER EXTENSION name add_drop CAST '(' Typename AS Typename ')'
				{
					AlterExtensionContentsStmt *n = makeNode(AlterExtensionContentsStmt);

					n->extname = $3;
					n->action = $4;
					n->objtype = OBJECT_CAST;
					n->object = (Node *) list_make2($7, $9);
					$$ = (Node *) n;
				}
			| ALTER EXTENSION name add_drop DOMAIN_P Typename
				{
					AlterExtensionContentsStmt *n = makeNode(AlterExtensionContentsStmt);

					n->extname = $3;
					n->action = $4;
					n->objtype = OBJECT_DOMAIN;
					n->object = (Node *) $6;
					$$ = (Node *) n;
				}
			| ALTER EXTENSION name add_drop FUNCTION function_with_argtypes
				{
					AlterExtensionContentsStmt *n = makeNode(AlterExtensionContentsStmt);

					n->extname = $3;
					n->action = $4;
					n->objtype = OBJECT_FUNCTION;
					n->object = (Node *) $6;
					$$ = (Node *) n;
				}
			| ALTER EXTENSION name add_drop OPERATOR operator_with_argtypes
				{
					AlterExtensionContentsStmt *n = makeNode(AlterExtensionContentsStmt);

					n->extname = $3;
					n->action = $4;
					n->objtype = OBJECT_OPERATOR;
					n->object = (Node *) $6;
					$$ = (Node *) n;
				}
			| ALTER EXTENSION name add_drop OPERATOR CLASS any_name USING name
				{
					AlterExtensionContentsStmt *n = makeNode(AlterExtensionContentsStmt);

					n->extname = $3;
					n->action = $4;
					n->objtype = OBJECT_OPCLASS;
					n->object = (Node *) lcons(makeString($9), $7);
					$$ = (Node *) n;
				}
			| ALTER EXTENSION name add_drop OPERATOR FAMILY any_name USING name
				{
					AlterExtensionContentsStmt *n = makeNode(AlterExtensionContentsStmt);

					n->extname = $3;
					n->action = $4;
					n->objtype = OBJECT_OPFAMILY;
					n->object = (Node *) lcons(makeString($9), $7);
					$$ = (Node *) n;
				}
			| ALTER EXTENSION name add_drop PROCEDURE function_with_argtypes
				{
					AlterExtensionContentsStmt *n = makeNode(AlterExtensionContentsStmt);

					n->extname = $3;
					n->action = $4;
					n->objtype = OBJECT_PROCEDURE;
					n->object = (Node *) $6;
					$$ = (Node *) n;
				}
			| ALTER EXTENSION name add_drop ROUTINE function_with_argtypes
				{
					AlterExtensionContentsStmt *n = makeNode(AlterExtensionContentsStmt);

					n->extname = $3;
					n->action = $4;
					n->objtype = OBJECT_ROUTINE;
					n->object = (Node *) $6;
					$$ = (Node *) n;
				}
			| ALTER EXTENSION name add_drop TRANSFORM FOR Typename LANGUAGE name
				{
					AlterExtensionContentsStmt *n = makeNode(AlterExtensionContentsStmt);

					n->extname = $3;
					n->action = $4;
					n->objtype = OBJECT_TRANSFORM;
					n->object = (Node *) list_make2($7, makeString($9));
					$$ = (Node *) n;
				}
			| ALTER EXTENSION name add_drop TYPE_P Typename
				{
					AlterExtensionContentsStmt *n = makeNode(AlterExtensionContentsStmt);

					n->extname = $3;
					n->action = $4;
					n->objtype = OBJECT_TYPE;
					n->object = (Node *) $6;
					$$ = (Node *) n;
				}
		;

/*****************************************************************************
 *
 *		QUERY:
 *             CREATE FOREIGN DATA WRAPPER name options
 *
 * 查询：CREATE FOREIGN DATA WRAPPER name options
 *****************************************************************************/

CreateFdwStmt: CREATE FOREIGN DATA_P WRAPPER name opt_fdw_options create_generic_options
				{
					CreateFdwStmt *n = makeNode(CreateFdwStmt);

					n->fdwname = $5;
					n->func_options = $6;
					n->options = $7;
					$$ = (Node *) n;
				}
		;

fdw_option:
			HANDLER handler_name				{ $$ = makeDefElem("handler", (Node *) $2, @1); }
			| NO HANDLER						{ $$ = makeDefElem("handler", NULL, @1); }
			| VALIDATOR handler_name			{ $$ = makeDefElem("validator", (Node *) $2, @1); }
			| NO VALIDATOR						{ $$ = makeDefElem("validator", NULL, @1); }
		;

fdw_options:
			fdw_option							{ $$ = list_make1($1); }
			| fdw_options fdw_option			{ $$ = lappend($1, $2); }
		;

opt_fdw_options:
			fdw_options							{ $$ = $1; }
			| /* EMPTY - 空 */							{ $$ = NIL; }
		;

/*****************************************************************************
 *
 *		QUERY :
 *				ALTER FOREIGN DATA WRAPPER name options
 *
 * 查询：ALTER FOREIGN DATA WRAPPER name options
 ****************************************************************************/

AlterFdwStmt: ALTER FOREIGN DATA_P WRAPPER name opt_fdw_options alter_generic_options
				{
					AlterFdwStmt *n = makeNode(AlterFdwStmt);

					n->fdwname = $5;
					n->func_options = $6;
					n->options = $7;
					$$ = (Node *) n;
				}
			| ALTER FOREIGN DATA_P WRAPPER name fdw_options
				{
					AlterFdwStmt *n = makeNode(AlterFdwStmt);

					n->fdwname = $5;
					n->func_options = $6;
					n->options = NIL;
					$$ = (Node *) n;
				}
		;

/* Options definition for CREATE FDW, SERVER and USER MAPPING - CREATE FDW、SERVER 和 USER MAPPING 的选项定义 */
create_generic_options:
			OPTIONS '(' generic_option_list ')'			{ $$ = $3; }
			| /* EMPTY - 空 */									{ $$ = NIL; }
		;

generic_option_list:
			generic_option_elem
				{
					$$ = list_make1($1);
				}
			| generic_option_list ',' generic_option_elem
				{
					$$ = lappend($1, $3);
				}
		;

/* Options definition for ALTER FDW, SERVER and USER MAPPING - ALTER FDW、SERVER 和 USER MAPPING 的选项定义 */
alter_generic_options:
			OPTIONS	'(' alter_generic_option_list ')'		{ $$ = $3; }
		;

alter_generic_option_list:
			alter_generic_option_elem
				{
					$$ = list_make1($1);
				}
			| alter_generic_option_list ',' alter_generic_option_elem
				{
					$$ = lappend($1, $3);
				}
		;

alter_generic_option_elem:
			generic_option_elem
				{
					$$ = $1;
				}
			| SET generic_option_elem
				{
					$$ = $2;
					$$->defaction = DEFELEM_SET;
				}
			| ADD_P generic_option_elem
				{
					$$ = $2;
					$$->defaction = DEFELEM_ADD;
				}
			| DROP generic_option_name
				{
					$$ = makeDefElemExtended(NULL, $2, NULL, DEFELEM_DROP, @2);
				}
		;

generic_option_elem:
			generic_option_name generic_option_arg
				{
					$$ = makeDefElem($1, $2, @1);
				}
		;

generic_option_name:
				ColLabel			{ $$ = $1; }
		;

/* We could use def_arg here, but the spec only requires string literals - 我们可以在这里使用 def_arg，但规范只要求字符串字面量 */
generic_option_arg:
				Sconst				{ $$ = (Node *) makeString($1); }
		;

/*****************************************************************************
 *
 *		QUERY:
 *             CREATE SERVER name [TYPE] [VERSION] [OPTIONS]
 *
 * 查询：CREATE SERVER name [TYPE] [VERSION] [OPTIONS]
 *****************************************************************************/

CreateForeignServerStmt: CREATE SERVER name opt_type opt_foreign_server_version
						 FOREIGN DATA_P WRAPPER name create_generic_options
				{
					CreateForeignServerStmt *n = makeNode(CreateForeignServerStmt);

					n->servername = $3;
					n->servertype = $4;
					n->version = $5;
					n->fdwname = $9;
					n->options = $10;
					n->if_not_exists = false;
					$$ = (Node *) n;
				}
				| CREATE SERVER IF_P NOT EXISTS name opt_type opt_foreign_server_version
						 FOREIGN DATA_P WRAPPER name create_generic_options
				{
					CreateForeignServerStmt *n = makeNode(CreateForeignServerStmt);

					n->servername = $6;
					n->servertype = $7;
					n->version = $8;
					n->fdwname = $12;
					n->options = $13;
					n->if_not_exists = true;
					$$ = (Node *) n;
				}
		;

opt_type:
			TYPE_P Sconst			{ $$ = $2; }
			| /* EMPTY - 空 */				{ $$ = NULL; }
		;


foreign_server_version:
			VERSION_P Sconst		{ $$ = $2; }
		|	VERSION_P NULL_P		{ $$ = NULL; }
		;

opt_foreign_server_version:
			foreign_server_version	{ $$ = $1; }
			| /* EMPTY - 空 */				{ $$ = NULL; }
		;

/*****************************************************************************
 *
 *		QUERY :
 *				ALTER SERVER name [VERSION] [OPTIONS]
 *
 * 查询：ALTER SERVER name [VERSION] [OPTIONS]
 ****************************************************************************/

AlterForeignServerStmt: ALTER SERVER name foreign_server_version alter_generic_options
				{
					AlterForeignServerStmt *n = makeNode(AlterForeignServerStmt);

					n->servername = $3;
					n->version = $4;
					n->options = $5;
					n->has_version = true;
					$$ = (Node *) n;
				}
			| ALTER SERVER name foreign_server_version
				{
					AlterForeignServerStmt *n = makeNode(AlterForeignServerStmt);

					n->servername = $3;
					n->version = $4;
					n->has_version = true;
					$$ = (Node *) n;
				}
			| ALTER SERVER name alter_generic_options
				{
					AlterForeignServerStmt *n = makeNode(AlterForeignServerStmt);

					n->servername = $3;
					n->options = $4;
					$$ = (Node *) n;
				}
		;

/*****************************************************************************
 *
 *		QUERY:
 *             CREATE FOREIGN TABLE relname (...) SERVER name (...)
 *
 * 查询：CREATE FOREIGN TABLE relname (...) SERVER name (...)
 *****************************************************************************/

CreateForeignTableStmt:
		CREATE FOREIGN TABLE qualified_name
			'(' OptTableElementList ')'
			OptInherit SERVER name create_generic_options
				{
					CreateForeignTableStmt *n = makeNode(CreateForeignTableStmt);

					$4->relpersistence = RELPERSISTENCE_PERMANENT;
					n->base.relation = $4;
					n->base.tableElts = $6;
					n->base.inhRelations = $8;
					n->base.ofTypename = NULL;
					n->base.constraints = NIL;
					n->base.options = NIL;
					n->base.oncommit = ONCOMMIT_NOOP;
					n->base.tablespacename = NULL;
					n->base.if_not_exists = false;
					/* FDW-specific data - 外部数据包装器（FDW）特定数据 */
					n->servername = $10;
					n->options = $11;
					$$ = (Node *) n;
				}
		| CREATE FOREIGN TABLE IF_P NOT EXISTS qualified_name
			'(' OptTableElementList ')'
			OptInherit SERVER name create_generic_options
				{
					CreateForeignTableStmt *n = makeNode(CreateForeignTableStmt);

					$7->relpersistence = RELPERSISTENCE_PERMANENT;
					n->base.relation = $7;
					n->base.tableElts = $9;
					n->base.inhRelations = $11;
					n->base.ofTypename = NULL;
					n->base.constraints = NIL;
					n->base.options = NIL;
					n->base.oncommit = ONCOMMIT_NOOP;
					n->base.tablespacename = NULL;
					n->base.if_not_exists = true;
					/* FDW-specific data - 外部数据包装器（FDW）特定数据 */
					n->servername = $13;
					n->options = $14;
					$$ = (Node *) n;
				}
		| CREATE FOREIGN TABLE qualified_name
			PARTITION OF qualified_name OptTypedTableElementList PartitionBoundSpec
			SERVER name create_generic_options
				{
					CreateForeignTableStmt *n = makeNode(CreateForeignTableStmt);

					$4->relpersistence = RELPERSISTENCE_PERMANENT;
					n->base.relation = $4;
					n->base.inhRelations = list_make1($7);
					n->base.tableElts = $8;
					n->base.partbound = $9;
					n->base.ofTypename = NULL;
					n->base.constraints = NIL;
					n->base.options = NIL;
					n->base.oncommit = ONCOMMIT_NOOP;
					n->base.tablespacename = NULL;
					n->base.if_not_exists = false;
					/* FDW-specific data - 外部数据包装器（FDW）特定数据 */
					n->servername = $11;
					n->options = $12;
					$$ = (Node *) n;
				}
		| CREATE FOREIGN TABLE IF_P NOT EXISTS qualified_name
			PARTITION OF qualified_name OptTypedTableElementList PartitionBoundSpec
			SERVER name create_generic_options
				{
					CreateForeignTableStmt *n = makeNode(CreateForeignTableStmt);

					$7->relpersistence = RELPERSISTENCE_PERMANENT;
					n->base.relation = $7;
					n->base.inhRelations = list_make1($10);
					n->base.tableElts = $11;
					n->base.partbound = $12;
					n->base.ofTypename = NULL;
					n->base.constraints = NIL;
					n->base.options = NIL;
					n->base.oncommit = ONCOMMIT_NOOP;
					n->base.tablespacename = NULL;
					n->base.if_not_exists = true;
					/* FDW-specific data - 外部数据包装器（FDW）特定数据 */
					n->servername = $14;
					n->options = $15;
					$$ = (Node *) n;
				}
		;

/*****************************************************************************
 *
 *		QUERY:
 *				IMPORT FOREIGN SCHEMA remote_schema
 *				[ { LIMIT TO | EXCEPT } ( table_list ) ]
 *				FROM SERVER server_name INTO local_schema [ OPTIONS (...) ]
 *
 * 查询：IMPORT FOREIGN SCHEMA remote_schema [ { LIMIT TO | EXCEPT } ( table_list ) ] FROM SERVER server_name INTO local_schema [ OPTIONS (...) ]
 ****************************************************************************/

ImportForeignSchemaStmt:
		IMPORT_P FOREIGN SCHEMA name import_qualification
		  FROM SERVER name INTO name create_generic_options
			{
				ImportForeignSchemaStmt *n = makeNode(ImportForeignSchemaStmt);

				n->server_name = $8;
				n->remote_schema = $4;
				n->local_schema = $10;
				n->list_type = $5->type;
				n->table_list = $5->table_names;
				n->options = $11;
				$$ = (Node *) n;
			}
		;

import_qualification_type:
		LIMIT TO				{ $$ = FDW_IMPORT_SCHEMA_LIMIT_TO; }
		| EXCEPT				{ $$ = FDW_IMPORT_SCHEMA_EXCEPT; }
		;

import_qualification:
		import_qualification_type '(' relation_expr_list ')'
			{
				ImportQual *n = (ImportQual *) palloc(sizeof(ImportQual));

				n->type = $1;
				n->table_names = $3;
				$$ = n;
			}
		| /* EMPTY - 空 */
			{
				ImportQual *n = (ImportQual *) palloc(sizeof(ImportQual));
				n->type = FDW_IMPORT_SCHEMA_ALL;
				n->table_names = NIL;
				$$ = n;
			}
		;

/*****************************************************************************
 *
 *		QUERY:
 *             CREATE USER MAPPING FOR auth_ident SERVER name [OPTIONS]
 *
 * 查询：CREATE USER MAPPING FOR auth_ident SERVER name [OPTIONS]
 *****************************************************************************/

CreateUserMappingStmt: CREATE USER MAPPING FOR auth_ident SERVER name create_generic_options
				{
					CreateUserMappingStmt *n = makeNode(CreateUserMappingStmt);

					n->user = $5;
					n->servername = $7;
					n->options = $8;
					n->if_not_exists = false;
					$$ = (Node *) n;
				}
				| CREATE USER MAPPING IF_P NOT EXISTS FOR auth_ident SERVER name create_generic_options
				{
					CreateUserMappingStmt *n = makeNode(CreateUserMappingStmt);

					n->user = $8;
					n->servername = $10;
					n->options = $11;
					n->if_not_exists = true;
					$$ = (Node *) n;
				}
		;

/* User mapping authorization identifier - 用户映射授权标识符 */
auth_ident: RoleSpec			{ $$ = $1; }
			| USER				{ $$ = makeRoleSpec(ROLESPEC_CURRENT_USER, @1); }
		;

/*****************************************************************************
 *
 *		QUERY :
 *				DROP USER MAPPING FOR auth_ident SERVER name
 *
 * XXX you'd think this should have a CASCADE/RESTRICT option, even if it's
 * only pro forma; but the SQL standard doesn't show one.
 * 查询：DROP USER MAPPING FOR auth_ident SERVER name。XXX 你可能会认为这应该有一个 CASCADE/RESTRICT 选项，即使它只是形式上的；但 SQL 标准并没有显示。
 ****************************************************************************/

DropUserMappingStmt: DROP USER MAPPING FOR auth_ident SERVER name
				{
					DropUserMappingStmt *n = makeNode(DropUserMappingStmt);

					n->user = $5;
					n->servername = $7;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
				|  DROP USER MAPPING IF_P EXISTS FOR auth_ident SERVER name
				{
					DropUserMappingStmt *n = makeNode(DropUserMappingStmt);

					n->user = $7;
					n->servername = $9;
					n->missing_ok = true;
					$$ = (Node *) n;
				}
		;

/*****************************************************************************
 *
 *		QUERY :
 *				ALTER USER MAPPING FOR auth_ident SERVER name OPTIONS
 *
 * 查询：ALTER USER MAPPING FOR auth_ident SERVER name OPTIONS
 ****************************************************************************/

AlterUserMappingStmt: ALTER USER MAPPING FOR auth_ident SERVER name alter_generic_options
				{
					AlterUserMappingStmt *n = makeNode(AlterUserMappingStmt);

					n->user = $5;
					n->servername = $7;
					n->options = $8;
					$$ = (Node *) n;
				}
		;

/*****************************************************************************
 *
 *		QUERIES:
 *				CREATE POLICY name ON table
 *					[AS { PERMISSIVE | RESTRICTIVE } ]
 *					[FOR { SELECT | INSERT | UPDATE | DELETE } ]
 *					[TO role, ...]
 *					[USING (qual)] [WITH CHECK (with check qual)]
 *				ALTER POLICY name ON table [TO role, ...]
 *					[USING (qual)] [WITH CHECK (with check qual)]
 *
 * 查询：CREATE POLICY name ON table [AS { PERMISSIVE | RESTRICTIVE } ] [FOR { SELECT | INSERT | UPDATE | DELETE } ] [TO role, ...] [USING (qual)] [WITH CHECK (with check qual)] 以及 ALTER POLICY...
 *****************************************************************************/

CreatePolicyStmt:
			CREATE POLICY name ON qualified_name RowSecurityDefaultPermissive
				RowSecurityDefaultForCmd RowSecurityDefaultToRole
				RowSecurityOptionalExpr RowSecurityOptionalWithCheck
				{
					CreatePolicyStmt *n = makeNode(CreatePolicyStmt);

					n->policy_name = $3;
					n->table = $5;
					n->permissive = $6;
					n->cmd_name = $7;
					n->roles = $8;
					n->qual = $9;
					n->with_check = $10;
					$$ = (Node *) n;
				}
		;

AlterPolicyStmt:
			ALTER POLICY name ON qualified_name RowSecurityOptionalToRole
				RowSecurityOptionalExpr RowSecurityOptionalWithCheck
				{
					AlterPolicyStmt *n = makeNode(AlterPolicyStmt);

					n->policy_name = $3;
					n->table = $5;
					n->roles = $6;
					n->qual = $7;
					n->with_check = $8;
					$$ = (Node *) n;
				}
		;

RowSecurityOptionalExpr:
			USING '(' a_expr ')'	{ $$ = $3; }
			| /* EMPTY - 空 */			{ $$ = NULL; }
		;

RowSecurityOptionalWithCheck:
			WITH CHECK '(' a_expr ')'		{ $$ = $4; }
			| /* EMPTY - 空 */					{ $$ = NULL; }
		;

RowSecurityDefaultToRole:
			TO role_list			{ $$ = $2; }
			| /* EMPTY - 空 */			{ $$ = list_make1(makeRoleSpec(ROLESPEC_PUBLIC, -1)); }
		;

RowSecurityOptionalToRole:
			TO role_list			{ $$ = $2; }
			| /* EMPTY - 空 */			{ $$ = NULL; }
		;

RowSecurityDefaultPermissive:
			AS IDENT
				{
					if (strcmp($2, "permissive") == 0)
						$$ = true;
					else if (strcmp($2, "restrictive") == 0)
						$$ = false;
					else
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("unrecognized row security option \"%s\"", $2),
								 errhint("Only PERMISSIVE or RESTRICTIVE policies are supported currently."),
								 parser_errposition(@2)));

				}
			| /* EMPTY - 空 */			{ $$ = true; }
		;

RowSecurityDefaultForCmd:
			FOR row_security_cmd	{ $$ = $2; }
			| /* EMPTY - 空 */			{ $$ = "all"; }
		;

row_security_cmd:
			ALL				{ $$ = "all"; }
		|	SELECT			{ $$ = "select"; }
		|	INSERT			{ $$ = "insert"; }
		|	UPDATE			{ $$ = "update"; }
		|	DELETE_P		{ $$ = "delete"; }
		;

/*****************************************************************************
 *
 *		QUERY:
 *             CREATE ACCESS METHOD name HANDLER handler_name
 *
 * 查询：CREATE ACCESS METHOD name HANDLER handler_name
 *****************************************************************************/

CreateAmStmt: CREATE ACCESS METHOD name TYPE_P am_type HANDLER handler_name
				{
					CreateAmStmt *n = makeNode(CreateAmStmt);

					n->amname = $4;
					n->handler_name = $8;
					n->amtype = $6;
					$$ = (Node *) n;
				}
		;

am_type:
			INDEX			{ $$ = AMTYPE_INDEX; }
		|	TABLE			{ $$ = AMTYPE_TABLE; }
		;

/*****************************************************************************
 *
 *		QUERIES :
 *				CREATE TRIGGER ...
 *
 * 查询：CREATE TRIGGER ...
 *****************************************************************************/

CreateTrigStmt:
			CREATE opt_or_replace TRIGGER name TriggerActionTime TriggerEvents ON
			qualified_name TriggerReferencing TriggerForSpec TriggerWhen
			EXECUTE FUNCTION_or_PROCEDURE func_name '(' TriggerFuncArgs ')'
				{
					CreateTrigStmt *n = makeNode(CreateTrigStmt);

					n->replace = $2;
					n->isconstraint = false;
					n->trigname = $4;
					n->relation = $8;
					n->funcname = $14;
					n->args = $16;
					n->row = $10;
					n->timing = $5;
					n->events = intVal(linitial($6));
					n->columns = (List *) lsecond($6);
					n->whenClause = $11;
					n->transitionRels = $9;
					n->deferrable = false;
					n->initdeferred = false;
					n->constrrel = NULL;
					$$ = (Node *) n;
				}
		  | CREATE opt_or_replace CONSTRAINT TRIGGER name AFTER TriggerEvents ON
			qualified_name OptConstrFromTable ConstraintAttributeSpec
			FOR EACH ROW TriggerWhen
			EXECUTE FUNCTION_or_PROCEDURE func_name '(' TriggerFuncArgs ')'
				{
					CreateTrigStmt *n = makeNode(CreateTrigStmt);

					n->replace = $2;
					if (n->replace) /* not supported, see CreateTrigger - 不支持，参见 CreateTrigger */
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("CREATE OR REPLACE CONSTRAINT TRIGGER is not supported"),
								 parser_errposition(@1)));
					n->isconstraint = true;
					n->trigname = $5;
					n->relation = $9;
					n->funcname = $18;
					n->args = $20;
					n->row = true;
					n->timing = TRIGGER_TYPE_AFTER;
					n->events = intVal(linitial($7));
					n->columns = (List *) lsecond($7);
					n->whenClause = $15;
					n->transitionRels = NIL;
					processCASbits($11, @11, "TRIGGER",
								   &n->deferrable, &n->initdeferred, NULL,
								   NULL, NULL, yyscanner);
					n->constrrel = $10;
					$$ = (Node *) n;
				}
		;

TriggerActionTime:
			BEFORE								{ $$ = TRIGGER_TYPE_BEFORE; }
			| AFTER								{ $$ = TRIGGER_TYPE_AFTER; }
			| INSTEAD OF						{ $$ = TRIGGER_TYPE_INSTEAD; }
		;

TriggerEvents:
			TriggerOneEvent
				{ $$ = $1; }
			| TriggerEvents OR TriggerOneEvent
				{
					int			events1 = intVal(linitial($1));
					int			events2 = intVal(linitial($3));
					List	   *columns1 = (List *) lsecond($1);
					List	   *columns2 = (List *) lsecond($3);

					if (events1 & events2)
						parser_yyerror("duplicate trigger events specified");
					/*
					 * concat'ing the columns lists loses information about
					 * which columns went with which event, but so long as
					 * only UPDATE carries columns and we disallow multiple
					 * UPDATE items, it doesn't matter.  Command execution
					 * should just ignore the columns for non-UPDATE events.
					 * 拼接列列表会丢失有关哪些列与哪个事件相关的信息，但只要只有 UPDATE 携带列并且我们不允许有多个 UPDATE 项，这并不重要。命令执行应该只忽略非 UPDATE 事件的列。
					 */
					$$ = list_make2(makeInteger(events1 | events2),
									list_concat(columns1, columns2));
				}
		;

TriggerOneEvent:
			INSERT
				{ $$ = list_make2(makeInteger(TRIGGER_TYPE_INSERT), NIL); }
			| DELETE_P
				{ $$ = list_make2(makeInteger(TRIGGER_TYPE_DELETE), NIL); }
			| UPDATE
				{ $$ = list_make2(makeInteger(TRIGGER_TYPE_UPDATE), NIL); }
			| UPDATE OF columnList
				{ $$ = list_make2(makeInteger(TRIGGER_TYPE_UPDATE), $3); }
			| TRUNCATE
				{ $$ = list_make2(makeInteger(TRIGGER_TYPE_TRUNCATE), NIL); }
		;

TriggerReferencing:
			REFERENCING TriggerTransitions			{ $$ = $2; }
			| /* EMPTY - 空 */								{ $$ = NIL; }
		;

TriggerTransitions:
			TriggerTransition						{ $$ = list_make1($1); }
			| TriggerTransitions TriggerTransition	{ $$ = lappend($1, $2); }
		;

TriggerTransition:
			TransitionOldOrNew TransitionRowOrTable opt_as TransitionRelName
				{
					TriggerTransition *n = makeNode(TriggerTransition);

					n->name = $4;
					n->isNew = $1;
					n->isTable = $2;
					$$ = (Node *) n;
				}
		;

TransitionOldOrNew:
			NEW										{ $$ = true; }
			| OLD									{ $$ = false; }
		;

TransitionRowOrTable:
			TABLE									{ $$ = true; }
			/*
			 * According to the standard, lack of a keyword here implies ROW.
			 * Support for that would require prohibiting ROW entirely here,
			 * reserving the keyword ROW, and/or requiring AS (instead of
			 * allowing it to be optional, as the standard specifies) as the
			 * next token.  Requiring ROW seems cleanest and easiest to
			 * explain.
			 * 根据标准，这里缺少关键字意味着 ROW。对它的支持将要求在此处完全禁止 ROW，保留关键字 ROW，和/或要求将 AS（而不是像标准指定的那样允许它是可选的）作为下一个 Token。要求 ROW 似乎最干净，最容易解释。
			 */
			| ROW									{ $$ = false; }
		;

TransitionRelName:
			ColId									{ $$ = $1; }
		;

TriggerForSpec:
			FOR TriggerForOptEach TriggerForType
				{
					$$ = $3;
				}
			| /* EMPTY - 空 */
				{
					/*
					 * If ROW/STATEMENT not specified, default to
					 * STATEMENT, per SQL
					 * 如果未指定 ROW/STATEMENT，根据 SQL 规范，默认值为 STATEMENT
					 */
					$$ = false;
				}
		;

TriggerForOptEach:
			EACH
			| /* EMPTY - 空 */
		;

TriggerForType:
			ROW										{ $$ = true; }
			| STATEMENT								{ $$ = false; }
		;

TriggerWhen:
			WHEN '(' a_expr ')'						{ $$ = $3; }
			| /* EMPTY - 空 */								{ $$ = NULL; }
		;

FUNCTION_or_PROCEDURE:
			FUNCTION
		|	PROCEDURE
		;

TriggerFuncArgs:
			TriggerFuncArg							{ $$ = list_make1($1); }
			| TriggerFuncArgs ',' TriggerFuncArg	{ $$ = lappend($1, $3); }
			| /* EMPTY - 空 */								{ $$ = NIL; }
		;

TriggerFuncArg:
			Iconst
				{
					$$ = (Node *) makeString(psprintf("%d", $1));
				}
			| FCONST								{ $$ = (Node *) makeString($1); }
			| Sconst								{ $$ = (Node *) makeString($1); }
			| ColLabel								{ $$ = (Node *) makeString($1); }
		;

OptConstrFromTable:
			FROM qualified_name						{ $$ = $2; }
			| /* EMPTY - 空 */								{ $$ = NULL; }
		;

ConstraintAttributeSpec:
			/* EMPTY - 空 */
				{ $$ = 0; }
			| ConstraintAttributeSpec ConstraintAttributeElem
				{
					/*
					 * We must complain about conflicting options.
					 * We could, but choose not to, complain about redundant
					 * options (ie, where $2's bit is already set in $1).
					 * 我们必须对冲突的选项报错。我们可以但选择不对冗余选项报错（即，当 $2 的位已在 $1 中设置时）。
					 */
					int		newspec = $1 | $2;

					/* special message for this case - 针对此情况的特殊信息 */
					if ((newspec & (CAS_NOT_DEFERRABLE | CAS_INITIALLY_DEFERRED)) == (CAS_NOT_DEFERRABLE | CAS_INITIALLY_DEFERRED))
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("constraint declared INITIALLY DEFERRED must be DEFERRABLE"),
								 parser_errposition(@2)));
					/* generic message for other conflicts - 针对其他冲突的通用信息 */
					if ((newspec & (CAS_NOT_DEFERRABLE | CAS_DEFERRABLE)) == (CAS_NOT_DEFERRABLE | CAS_DEFERRABLE) ||
						(newspec & (CAS_INITIALLY_IMMEDIATE | CAS_INITIALLY_DEFERRED)) == (CAS_INITIALLY_IMMEDIATE | CAS_INITIALLY_DEFERRED) ||
						(newspec & (CAS_NOT_ENFORCED | CAS_ENFORCED)) == (CAS_NOT_ENFORCED | CAS_ENFORCED))
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("conflicting constraint properties"),
								 parser_errposition(@2)));
					$$ = newspec;
				}
		;

ConstraintAttributeElem:
			NOT DEFERRABLE					{ $$ = CAS_NOT_DEFERRABLE; }
			| DEFERRABLE					{ $$ = CAS_DEFERRABLE; }
			| INITIALLY IMMEDIATE			{ $$ = CAS_INITIALLY_IMMEDIATE; }
			| INITIALLY DEFERRED			{ $$ = CAS_INITIALLY_DEFERRED; }
			| NOT VALID						{ $$ = CAS_NOT_VALID; }
			| NO INHERIT					{ $$ = CAS_NO_INHERIT; }
			| NOT ENFORCED					{ $$ = CAS_NOT_ENFORCED; }
			| ENFORCED						{ $$ = CAS_ENFORCED; }
		;


/*****************************************************************************
 *
 *		QUERIES :
 *				CREATE EVENT TRIGGER ...
 *				ALTER EVENT TRIGGER ...
 *
 * 查询：CREATE EVENT TRIGGER ... ALTER EVENT TRIGGER ...
 *****************************************************************************/

CreateEventTrigStmt:
			CREATE EVENT TRIGGER name ON ColLabel
			EXECUTE FUNCTION_or_PROCEDURE func_name '(' ')'
				{
					CreateEventTrigStmt *n = makeNode(CreateEventTrigStmt);

					n->trigname = $4;
					n->eventname = $6;
					n->whenclause = NULL;
					n->funcname = $9;
					$$ = (Node *) n;
				}
		  | CREATE EVENT TRIGGER name ON ColLabel
			WHEN event_trigger_when_list
			EXECUTE FUNCTION_or_PROCEDURE func_name '(' ')'
				{
					CreateEventTrigStmt *n = makeNode(CreateEventTrigStmt);

					n->trigname = $4;
					n->eventname = $6;
					n->whenclause = $8;
					n->funcname = $11;
					$$ = (Node *) n;
				}
		;

event_trigger_when_list:
		  event_trigger_when_item
			{ $$ = list_make1($1); }
		| event_trigger_when_list AND event_trigger_when_item
			{ $$ = lappend($1, $3); }
		;

event_trigger_when_item:
		ColId IN_P '(' event_trigger_value_list ')'
			{ $$ = makeDefElem($1, (Node *) $4, @1); }
		;

event_trigger_value_list:
		  SCONST
			{ $$ = list_make1(makeString($1)); }
		| event_trigger_value_list ',' SCONST
			{ $$ = lappend($1, makeString($3)); }
		;

AlterEventTrigStmt:
			ALTER EVENT TRIGGER name enable_trigger
				{
					AlterEventTrigStmt *n = makeNode(AlterEventTrigStmt);

					n->trigname = $4;
					n->tgenabled = $5;
					$$ = (Node *) n;
				}
		;

enable_trigger:
			ENABLE_P					{ $$ = TRIGGER_FIRES_ON_ORIGIN; }
			| ENABLE_P REPLICA			{ $$ = TRIGGER_FIRES_ON_REPLICA; }
			| ENABLE_P ALWAYS			{ $$ = TRIGGER_FIRES_ALWAYS; }
			| DISABLE_P					{ $$ = TRIGGER_DISABLED; }
		;

/*****************************************************************************
 *
 *		QUERY :
 *				CREATE ASSERTION ...
 *
 * 查询：CREATE ASSERTION ...
 *****************************************************************************/

CreateAssertionStmt:
			CREATE ASSERTION any_name CHECK '(' a_expr ')' ConstraintAttributeSpec
				{
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("CREATE ASSERTION is not yet implemented"),
							 parser_errposition(@1)));

					$$ = NULL;
				}
		;


/*****************************************************************************
 *
 *		QUERY :
 *				define (aggregate,operator,type)
 *
 * 查询：define (aggregate,operator,type)
 *****************************************************************************/

DefineStmt:
			CREATE opt_or_replace AGGREGATE func_name aggr_args definition
				{
					DefineStmt *n = makeNode(DefineStmt);

					n->kind = OBJECT_AGGREGATE;
					n->oldstyle = false;
					n->replace = $2;
					n->defnames = $4;
					n->args = $5;
					n->definition = $6;
					$$ = (Node *) n;
				}
			| CREATE opt_or_replace AGGREGATE func_name old_aggr_definition
				{
					/* old-style (pre-8.2) syntax for CREATE AGGREGATE - 旧式（8.2版本之前）的 CREATE AGGREGATE 语法 */
					DefineStmt *n = makeNode(DefineStmt);

					n->kind = OBJECT_AGGREGATE;
					n->oldstyle = true;
					n->replace = $2;
					n->defnames = $4;
					n->args = NIL;
					n->definition = $5;
					$$ = (Node *) n;
				}
			| CREATE OPERATOR any_operator definition
				{
					DefineStmt *n = makeNode(DefineStmt);

					n->kind = OBJECT_OPERATOR;
					n->oldstyle = false;
					n->defnames = $3;
					n->args = NIL;
					n->definition = $4;
					$$ = (Node *) n;
				}
			| CREATE TYPE_P any_name definition
				{
					DefineStmt *n = makeNode(DefineStmt);

					n->kind = OBJECT_TYPE;
					n->oldstyle = false;
					n->defnames = $3;
					n->args = NIL;
					n->definition = $4;
					$$ = (Node *) n;
				}
			| CREATE TYPE_P any_name
				{
					/* Shell type (identified by lack of definition) - Shell 类型（通过缺少定义来识别） */
					DefineStmt *n = makeNode(DefineStmt);

					n->kind = OBJECT_TYPE;
					n->oldstyle = false;
					n->defnames = $3;
					n->args = NIL;
					n->definition = NIL;
					$$ = (Node *) n;
				}
			| CREATE TYPE_P any_name AS '(' OptTableFuncElementList ')'
				{
					CompositeTypeStmt *n = makeNode(CompositeTypeStmt);

					/* can't use qualified_name, sigh - 不能使用 qualified_name，唉 */
					n->typevar = makeRangeVarFromAnyName($3, @3, yyscanner);
					n->coldeflist = $6;
					$$ = (Node *) n;
				}
			| CREATE TYPE_P any_name AS ENUM_P '(' opt_enum_val_list ')'
				{
					CreateEnumStmt *n = makeNode(CreateEnumStmt);

					n->typeName = $3;
					n->vals = $7;
					$$ = (Node *) n;
				}
			| CREATE TYPE_P any_name AS RANGE definition
				{
					CreateRangeStmt *n = makeNode(CreateRangeStmt);

					n->typeName = $3;
					n->params = $6;
					$$ = (Node *) n;
				}
			| CREATE TEXT_P SEARCH PARSER any_name definition
				{
					DefineStmt *n = makeNode(DefineStmt);

					n->kind = OBJECT_TSPARSER;
					n->args = NIL;
					n->defnames = $5;
					n->definition = $6;
					$$ = (Node *) n;
				}
			| CREATE TEXT_P SEARCH DICTIONARY any_name definition
				{
					DefineStmt *n = makeNode(DefineStmt);

					n->kind = OBJECT_TSDICTIONARY;
					n->args = NIL;
					n->defnames = $5;
					n->definition = $6;
					$$ = (Node *) n;
				}
			| CREATE TEXT_P SEARCH TEMPLATE any_name definition
				{
					DefineStmt *n = makeNode(DefineStmt);

					n->kind = OBJECT_TSTEMPLATE;
					n->args = NIL;
					n->defnames = $5;
					n->definition = $6;
					$$ = (Node *) n;
				}
			| CREATE TEXT_P SEARCH CONFIGURATION any_name definition
				{
					DefineStmt *n = makeNode(DefineStmt);

					n->kind = OBJECT_TSCONFIGURATION;
					n->args = NIL;
					n->defnames = $5;
					n->definition = $6;
					$$ = (Node *) n;
				}
			| CREATE COLLATION any_name definition
				{
					DefineStmt *n = makeNode(DefineStmt);

					n->kind = OBJECT_COLLATION;
					n->args = NIL;
					n->defnames = $3;
					n->definition = $4;
					$$ = (Node *) n;
				}
			| CREATE COLLATION IF_P NOT EXISTS any_name definition
				{
					DefineStmt *n = makeNode(DefineStmt);

					n->kind = OBJECT_COLLATION;
					n->args = NIL;
					n->defnames = $6;
					n->definition = $7;
					n->if_not_exists = true;
					$$ = (Node *) n;
				}
			| CREATE COLLATION any_name FROM any_name
				{
					DefineStmt *n = makeNode(DefineStmt);

					n->kind = OBJECT_COLLATION;
					n->args = NIL;
					n->defnames = $3;
					n->definition = list_make1(makeDefElem("from", (Node *) $5, @5));
					$$ = (Node *) n;
				}
			| CREATE COLLATION IF_P NOT EXISTS any_name FROM any_name
				{
					DefineStmt *n = makeNode(DefineStmt);

					n->kind = OBJECT_COLLATION;
					n->args = NIL;
					n->defnames = $6;
					n->definition = list_make1(makeDefElem("from", (Node *) $8, @8));
					n->if_not_exists = true;
					$$ = (Node *) n;
				}
		;

definition: '(' def_list ')'						{ $$ = $2; }
		;

def_list:	def_elem								{ $$ = list_make1($1); }
			| def_list ',' def_elem					{ $$ = lappend($1, $3); }
		;

def_elem:	ColLabel '=' def_arg
				{
					$$ = makeDefElem($1, (Node *) $3, @1);
				}
			| ColLabel
				{
					$$ = makeDefElem($1, NULL, @1);
				}
		;

/* Note: any simple identifier will be returned as a type name! - 注意：任何简单标识符都将作为类型名称返回！ */
def_arg:	func_type						{ $$ = (Node *) $1; }
			| reserved_keyword				{ $$ = (Node *) makeString(pstrdup($1)); }
			| qual_all_Op					{ $$ = (Node *) $1; }
			| NumericOnly					{ $$ = (Node *) $1; }
			| Sconst						{ $$ = (Node *) makeString($1); }
			| NONE							{ $$ = (Node *) makeString(pstrdup($1)); }
		;

old_aggr_definition: '(' old_aggr_list ')'			{ $$ = $2; }
		;

old_aggr_list: old_aggr_elem						{ $$ = list_make1($1); }
			| old_aggr_list ',' old_aggr_elem		{ $$ = lappend($1, $3); }
		;

/*
 * Must use IDENT here to avoid reduce/reduce conflicts; fortunately none of
 * the item names needed in old aggregate definitions are likely to become
 * SQL keywords.
 * 此处必须使用 IDENT 以避免规约/规约冲突；幸运的是，旧聚合定义中需要的项名称都不太可能成为 SQL 关键字。
 */
old_aggr_elem:  IDENT '=' def_arg
				{
					$$ = makeDefElem($1, (Node *) $3, @1);
				}
		;

opt_enum_val_list:
		enum_val_list							{ $$ = $1; }
		| /* EMPTY - 空 */								{ $$ = NIL; }
		;

enum_val_list:	Sconst
				{ $$ = list_make1(makeString($1)); }
			| enum_val_list ',' Sconst
				{ $$ = lappend($1, makeString($3)); }
		;

/*****************************************************************************
 *
 *	ALTER TYPE enumtype ADD ...
 *
 *****************************************************************************/

AlterEnumStmt:
		ALTER TYPE_P any_name ADD_P VALUE_P opt_if_not_exists Sconst
			{
				AlterEnumStmt *n = makeNode(AlterEnumStmt);

				n->typeName = $3;
				n->oldVal = NULL;
				n->newVal = $7;
				n->newValNeighbor = NULL;
				n->newValIsAfter = true;
				n->skipIfNewValExists = $6;
				$$ = (Node *) n;
			}
		 | ALTER TYPE_P any_name ADD_P VALUE_P opt_if_not_exists Sconst BEFORE Sconst
			{
				AlterEnumStmt *n = makeNode(AlterEnumStmt);

				n->typeName = $3;
				n->oldVal = NULL;
				n->newVal = $7;
				n->newValNeighbor = $9;
				n->newValIsAfter = false;
				n->skipIfNewValExists = $6;
				$$ = (Node *) n;
			}
		 | ALTER TYPE_P any_name ADD_P VALUE_P opt_if_not_exists Sconst AFTER Sconst
			{
				AlterEnumStmt *n = makeNode(AlterEnumStmt);

				n->typeName = $3;
				n->oldVal = NULL;
				n->newVal = $7;
				n->newValNeighbor = $9;
				n->newValIsAfter = true;
				n->skipIfNewValExists = $6;
				$$ = (Node *) n;
			}
		 | ALTER TYPE_P any_name RENAME VALUE_P Sconst TO Sconst
			{
				AlterEnumStmt *n = makeNode(AlterEnumStmt);

				n->typeName = $3;
				n->oldVal = $6;
				n->newVal = $8;
				n->newValNeighbor = NULL;
				n->newValIsAfter = false;
				n->skipIfNewValExists = false;
				$$ = (Node *) n;
			}
		 | ALTER TYPE_P any_name DROP VALUE_P Sconst
			{
				/*
				 * The following problems must be solved before this can be
				 * implemented:
				 *
				 * - There must be no instance of the target value in
				 *   any table.
				 *
				 * - The value must not appear in any catalog metadata,
				 *   such as stored view expressions or column defaults.
				 *
				 * - The value must not appear in any non-leaf page of a
				 *   btree (and similar issues with other index types).
				 *   This is problematic because a value could persist
				 *   there long after it's gone from user-visible data.
				 *
				 * - Concurrent sessions must not be able to insert the
				 *   value while the preceding conditions are being checked.
				 *
				 * - Possibly more...
				 * 在实现此项之前必须解决以下问题：- 任何表中都不能存在目标值的实例。- 该值不得出现在任何系统目录元数据中，例如存储的视图表达式或列默认值。- 该值不得出现在 btree 的任何非叶子页面中（其他索引类型也有类似问题）。这很有问题，因为在该值从用户可见数据中消失后，它可能会长期保留在那里。- 在检查上述条件时，并发会话不得插入该值。- 可能还有更多...
				 */
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("dropping an enum value is not implemented"),
						 parser_errposition(@4)));
			}
		 ;

opt_if_not_exists: IF_P NOT EXISTS              { $$ = true; }
		| /* EMPTY - 空 */                          { $$ = false; }
		;


/*****************************************************************************
 *
 *		QUERIES :
 *				CREATE OPERATOR CLASS ...
 *				CREATE OPERATOR FAMILY ...
 *				ALTER OPERATOR FAMILY ...
 *				DROP OPERATOR CLASS ...
 *				DROP OPERATOR FAMILY ...
 *
 * 查询：CREATE OPERATOR CLASS ... CREATE OPERATOR FAMILY ... ALTER OPERATOR FAMILY ... DROP OPERATOR CLASS ... DROP OPERATOR FAMILY ...
 *****************************************************************************/

CreateOpClassStmt:
			CREATE OPERATOR CLASS any_name opt_default FOR TYPE_P Typename
			USING name opt_opfamily AS opclass_item_list
				{
					CreateOpClassStmt *n = makeNode(CreateOpClassStmt);

					n->opclassname = $4;
					n->isDefault = $5;
					n->datatype = $8;
					n->amname = $10;
					n->opfamilyname = $11;
					n->items = $13;
					$$ = (Node *) n;
				}
		;

opclass_item_list:
			opclass_item							{ $$ = list_make1($1); }
			| opclass_item_list ',' opclass_item	{ $$ = lappend($1, $3); }
		;

opclass_item:
			OPERATOR Iconst any_operator opclass_purpose
				{
					CreateOpClassItem *n = makeNode(CreateOpClassItem);
					ObjectWithArgs *owa = makeNode(ObjectWithArgs);

					owa->objname = $3;
					owa->objargs = NIL;
					n->itemtype = OPCLASS_ITEM_OPERATOR;
					n->name = owa;
					n->number = $2;
					n->order_family = $4;
					$$ = (Node *) n;
				}
			| OPERATOR Iconst operator_with_argtypes opclass_purpose
				{
					CreateOpClassItem *n = makeNode(CreateOpClassItem);

					n->itemtype = OPCLASS_ITEM_OPERATOR;
					n->name = $3;
					n->number = $2;
					n->order_family = $4;
					$$ = (Node *) n;
				}
			| FUNCTION Iconst function_with_argtypes
				{
					CreateOpClassItem *n = makeNode(CreateOpClassItem);

					n->itemtype = OPCLASS_ITEM_FUNCTION;
					n->name = $3;
					n->number = $2;
					$$ = (Node *) n;
				}
			| FUNCTION Iconst '(' type_list ')' function_with_argtypes
				{
					CreateOpClassItem *n = makeNode(CreateOpClassItem);

					n->itemtype = OPCLASS_ITEM_FUNCTION;
					n->name = $6;
					n->number = $2;
					n->class_args = $4;
					$$ = (Node *) n;
				}
			| STORAGE Typename
				{
					CreateOpClassItem *n = makeNode(CreateOpClassItem);

					n->itemtype = OPCLASS_ITEM_STORAGETYPE;
					n->storedtype = $2;
					$$ = (Node *) n;
				}
		;

opt_default:	DEFAULT						{ $$ = true; }
			| /* EMPTY - 空 */						{ $$ = false; }
		;

opt_opfamily:	FAMILY any_name				{ $$ = $2; }
			| /* EMPTY - 空 */						{ $$ = NIL; }
		;

opclass_purpose: FOR SEARCH					{ $$ = NIL; }
			| FOR ORDER BY any_name			{ $$ = $4; }
			| /* EMPTY - 空 */						{ $$ = NIL; }
		;


CreateOpFamilyStmt:
			CREATE OPERATOR FAMILY any_name USING name
				{
					CreateOpFamilyStmt *n = makeNode(CreateOpFamilyStmt);

					n->opfamilyname = $4;
					n->amname = $6;
					$$ = (Node *) n;
				}
		;

AlterOpFamilyStmt:
			ALTER OPERATOR FAMILY any_name USING name ADD_P opclass_item_list
				{
					AlterOpFamilyStmt *n = makeNode(AlterOpFamilyStmt);

					n->opfamilyname = $4;
					n->amname = $6;
					n->isDrop = false;
					n->items = $8;
					$$ = (Node *) n;
				}
			| ALTER OPERATOR FAMILY any_name USING name DROP opclass_drop_list
				{
					AlterOpFamilyStmt *n = makeNode(AlterOpFamilyStmt);

					n->opfamilyname = $4;
					n->amname = $6;
					n->isDrop = true;
					n->items = $8;
					$$ = (Node *) n;
				}
		;

opclass_drop_list:
			opclass_drop							{ $$ = list_make1($1); }
			| opclass_drop_list ',' opclass_drop	{ $$ = lappend($1, $3); }
		;

opclass_drop:
			OPERATOR Iconst '(' type_list ')'
				{
					CreateOpClassItem *n = makeNode(CreateOpClassItem);

					n->itemtype = OPCLASS_ITEM_OPERATOR;
					n->number = $2;
					n->class_args = $4;
					$$ = (Node *) n;
				}
			| FUNCTION Iconst '(' type_list ')'
				{
					CreateOpClassItem *n = makeNode(CreateOpClassItem);

					n->itemtype = OPCLASS_ITEM_FUNCTION;
					n->number = $2;
					n->class_args = $4;
					$$ = (Node *) n;
				}
		;


DropOpClassStmt:
			DROP OPERATOR CLASS any_name USING name opt_drop_behavior
				{
					DropStmt *n = makeNode(DropStmt);

					n->objects = list_make1(lcons(makeString($6), $4));
					n->removeType = OBJECT_OPCLASS;
					n->behavior = $7;
					n->missing_ok = false;
					n->concurrent = false;
					$$ = (Node *) n;
				}
			| DROP OPERATOR CLASS IF_P EXISTS any_name USING name opt_drop_behavior
				{
					DropStmt *n = makeNode(DropStmt);

					n->objects = list_make1(lcons(makeString($8), $6));
					n->removeType = OBJECT_OPCLASS;
					n->behavior = $9;
					n->missing_ok = true;
					n->concurrent = false;
					$$ = (Node *) n;
				}
		;

DropOpFamilyStmt:
			DROP OPERATOR FAMILY any_name USING name opt_drop_behavior
				{
					DropStmt *n = makeNode(DropStmt);

					n->objects = list_make1(lcons(makeString($6), $4));
					n->removeType = OBJECT_OPFAMILY;
					n->behavior = $7;
					n->missing_ok = false;
					n->concurrent = false;
					$$ = (Node *) n;
				}
			| DROP OPERATOR FAMILY IF_P EXISTS any_name USING name opt_drop_behavior
				{
					DropStmt *n = makeNode(DropStmt);

					n->objects = list_make1(lcons(makeString($8), $6));
					n->removeType = OBJECT_OPFAMILY;
					n->behavior = $9;
					n->missing_ok = true;
					n->concurrent = false;
					$$ = (Node *) n;
				}
		;


/*****************************************************************************
 *
 *		QUERY:
 *
 *		DROP OWNED BY username [, username ...] [ RESTRICT | CASCADE ]
 *		REASSIGN OWNED BY username [, username ...] TO username
 *
 * 查询：DROP OWNED BY / REASSIGN OWNED BY 语法
 *****************************************************************************/
DropOwnedStmt:
			DROP OWNED BY role_list opt_drop_behavior
				{
					DropOwnedStmt *n = makeNode(DropOwnedStmt);

					n->roles = $4;
					n->behavior = $5;
					$$ = (Node *) n;
				}
		;

ReassignOwnedStmt:
			REASSIGN OWNED BY role_list TO RoleSpec
				{
					ReassignOwnedStmt *n = makeNode(ReassignOwnedStmt);

					n->roles = $4;
					n->newrole = $6;
					$$ = (Node *) n;
				}
		;

/*****************************************************************************
 *
 *		QUERY:
 *
 *		DROP itemtype [ IF EXISTS ] itemname [, itemname ...]
 *           [ RESTRICT | CASCADE ]
 *
 * 查询：DROP itemtype [ IF EXISTS ] itemname [, itemname ...] [ RESTRICT | CASCADE ]
 *****************************************************************************/

DropStmt:	DROP object_type_any_name IF_P EXISTS any_name_list opt_drop_behavior
				{
					DropStmt *n = makeNode(DropStmt);

					n->removeType = $2;
					n->missing_ok = true;
					n->objects = $5;
					n->behavior = $6;
					n->concurrent = false;
					$$ = (Node *) n;
				}
			| DROP object_type_any_name any_name_list opt_drop_behavior
				{
					DropStmt *n = makeNode(DropStmt);

					n->removeType = $2;
					n->missing_ok = false;
					n->objects = $3;
					n->behavior = $4;
					n->concurrent = false;
					$$ = (Node *) n;
				}
			| DROP drop_type_name IF_P EXISTS name_list opt_drop_behavior
				{
					DropStmt *n = makeNode(DropStmt);

					n->removeType = $2;
					n->missing_ok = true;
					n->objects = $5;
					n->behavior = $6;
					n->concurrent = false;
					$$ = (Node *) n;
				}
			| DROP drop_type_name name_list opt_drop_behavior
				{
					DropStmt *n = makeNode(DropStmt);

					n->removeType = $2;
					n->missing_ok = false;
					n->objects = $3;
					n->behavior = $4;
					n->concurrent = false;
					$$ = (Node *) n;
				}
			| DROP object_type_name_on_any_name name ON any_name opt_drop_behavior
				{
					DropStmt *n = makeNode(DropStmt);

					n->removeType = $2;
					n->objects = list_make1(lappend($5, makeString($3)));
					n->behavior = $6;
					n->missing_ok = false;
					n->concurrent = false;
					$$ = (Node *) n;
				}
			| DROP object_type_name_on_any_name IF_P EXISTS name ON any_name opt_drop_behavior
				{
					DropStmt *n = makeNode(DropStmt);

					n->removeType = $2;
					n->objects = list_make1(lappend($7, makeString($5)));
					n->behavior = $8;
					n->missing_ok = true;
					n->concurrent = false;
					$$ = (Node *) n;
				}
			| DROP TYPE_P type_name_list opt_drop_behavior
				{
					DropStmt *n = makeNode(DropStmt);

					n->removeType = OBJECT_TYPE;
					n->missing_ok = false;
					n->objects = $3;
					n->behavior = $4;
					n->concurrent = false;
					$$ = (Node *) n;
				}
			| DROP TYPE_P IF_P EXISTS type_name_list opt_drop_behavior
				{
					DropStmt *n = makeNode(DropStmt);

					n->removeType = OBJECT_TYPE;
					n->missing_ok = true;
					n->objects = $5;
					n->behavior = $6;
					n->concurrent = false;
					$$ = (Node *) n;
				}
			| DROP DOMAIN_P type_name_list opt_drop_behavior
				{
					DropStmt *n = makeNode(DropStmt);

					n->removeType = OBJECT_DOMAIN;
					n->missing_ok = false;
					n->objects = $3;
					n->behavior = $4;
					n->concurrent = false;
					$$ = (Node *) n;
				}
			| DROP DOMAIN_P IF_P EXISTS type_name_list opt_drop_behavior
				{
					DropStmt *n = makeNode(DropStmt);

					n->removeType = OBJECT_DOMAIN;
					n->missing_ok = true;
					n->objects = $5;
					n->behavior = $6;
					n->concurrent = false;
					$$ = (Node *) n;
				}
			| DROP INDEX CONCURRENTLY any_name_list opt_drop_behavior
				{
					DropStmt *n = makeNode(DropStmt);

					n->removeType = OBJECT_INDEX;
					n->missing_ok = false;
					n->objects = $4;
					n->behavior = $5;
					n->concurrent = true;
					$$ = (Node *) n;
				}
			| DROP INDEX CONCURRENTLY IF_P EXISTS any_name_list opt_drop_behavior
				{
					DropStmt *n = makeNode(DropStmt);

					n->removeType = OBJECT_INDEX;
					n->missing_ok = true;
					n->objects = $6;
					n->behavior = $7;
					n->concurrent = true;
					$$ = (Node *) n;
				}
		;

/* object types taking any_name/any_name_list - 接受 any_name/any_name_list 的对象类型 */
object_type_any_name:
			TABLE									{ $$ = OBJECT_TABLE; }
			| SEQUENCE								{ $$ = OBJECT_SEQUENCE; }
			| VIEW									{ $$ = OBJECT_VIEW; }
			| MATERIALIZED VIEW						{ $$ = OBJECT_MATVIEW; }
			| INDEX									{ $$ = OBJECT_INDEX; }
			| FOREIGN TABLE							{ $$ = OBJECT_FOREIGN_TABLE; }
			| COLLATION								{ $$ = OBJECT_COLLATION; }
			| CONVERSION_P							{ $$ = OBJECT_CONVERSION; }
			| STATISTICS							{ $$ = OBJECT_STATISTIC_EXT; }
			| TEXT_P SEARCH PARSER					{ $$ = OBJECT_TSPARSER; }
			| TEXT_P SEARCH DICTIONARY				{ $$ = OBJECT_TSDICTIONARY; }
			| TEXT_P SEARCH TEMPLATE				{ $$ = OBJECT_TSTEMPLATE; }
			| TEXT_P SEARCH CONFIGURATION			{ $$ = OBJECT_TSCONFIGURATION; }
		;

/*
 * object types taking name/name_list
 *
 * DROP handles some of them separately
 * 接受 name/name_list 的对象类型，DROP 会单独处理其中一些
 */

object_type_name:
			drop_type_name							{ $$ = $1; }
			| DATABASE								{ $$ = OBJECT_DATABASE; }
			| ROLE									{ $$ = OBJECT_ROLE; }
			| SUBSCRIPTION							{ $$ = OBJECT_SUBSCRIPTION; }
			| TABLESPACE							{ $$ = OBJECT_TABLESPACE; }
		;

drop_type_name:
			ACCESS METHOD							{ $$ = OBJECT_ACCESS_METHOD; }
			| EVENT TRIGGER							{ $$ = OBJECT_EVENT_TRIGGER; }
			| EXTENSION								{ $$ = OBJECT_EXTENSION; }
			| FOREIGN DATA_P WRAPPER				{ $$ = OBJECT_FDW; }
			| opt_procedural LANGUAGE				{ $$ = OBJECT_LANGUAGE; }
			| PUBLICATION							{ $$ = OBJECT_PUBLICATION; }
			| SCHEMA								{ $$ = OBJECT_SCHEMA; }
			| SERVER								{ $$ = OBJECT_FOREIGN_SERVER; }
		;

/* object types attached to a table - 附属于表的对象类型 */
object_type_name_on_any_name:
			POLICY									{ $$ = OBJECT_POLICY; }
			| RULE									{ $$ = OBJECT_RULE; }
			| TRIGGER								{ $$ = OBJECT_TRIGGER; }
		;

any_name_list:
			any_name								{ $$ = list_make1($1); }
			| any_name_list ',' any_name			{ $$ = lappend($1, $3); }
		;

any_name:	ColId						{ $$ = list_make1(makeString($1)); }
			| ColId attrs				{ $$ = lcons(makeString($1), $2); }
		;

attrs:		'.' attr_name
					{ $$ = list_make1(makeString($2)); }
			| attrs '.' attr_name
					{ $$ = lappend($1, makeString($3)); }
		;

type_name_list:
			Typename								{ $$ = list_make1($1); }
			| type_name_list ',' Typename			{ $$ = lappend($1, $3); }
		;

/*****************************************************************************
 *
 *		QUERY:
 *				truncate table relname1, relname2, ...
 *
 * 查询：truncate table relname1, relname2, ...
 *****************************************************************************/

TruncateStmt:
			TRUNCATE opt_table relation_expr_list opt_restart_seqs opt_drop_behavior
				{
					TruncateStmt *n = makeNode(TruncateStmt);

					n->relations = $3;
					n->restart_seqs = $4;
					n->behavior = $5;
					$$ = (Node *) n;
				}
		;

opt_restart_seqs:
			CONTINUE_P IDENTITY_P		{ $$ = false; }
			| RESTART IDENTITY_P		{ $$ = true; }
			| /* EMPTY - 空 */				{ $$ = false; }
		;

/*****************************************************************************
 *
 * COMMENT ON <object> IS <text>
 *
 *****************************************************************************/

CommentStmt:
			COMMENT ON object_type_any_name any_name IS comment_text
				{
					CommentStmt *n = makeNode(CommentStmt);

					n->objtype = $3;
					n->object = (Node *) $4;
					n->comment = $6;
					$$ = (Node *) n;
				}
			| COMMENT ON COLUMN any_name IS comment_text
				{
					CommentStmt *n = makeNode(CommentStmt);

					n->objtype = OBJECT_COLUMN;
					n->object = (Node *) $4;
					n->comment = $6;
					$$ = (Node *) n;
				}
			| COMMENT ON object_type_name name IS comment_text
				{
					CommentStmt *n = makeNode(CommentStmt);

					n->objtype = $3;
					n->object = (Node *) makeString($4);
					n->comment = $6;
					$$ = (Node *) n;
				}
			| COMMENT ON TYPE_P Typename IS comment_text
				{
					CommentStmt *n = makeNode(CommentStmt);

					n->objtype = OBJECT_TYPE;
					n->object = (Node *) $4;
					n->comment = $6;
					$$ = (Node *) n;
				}
			| COMMENT ON DOMAIN_P Typename IS comment_text
				{
					CommentStmt *n = makeNode(CommentStmt);

					n->objtype = OBJECT_DOMAIN;
					n->object = (Node *) $4;
					n->comment = $6;
					$$ = (Node *) n;
				}
			| COMMENT ON AGGREGATE aggregate_with_argtypes IS comment_text
				{
					CommentStmt *n = makeNode(CommentStmt);

					n->objtype = OBJECT_AGGREGATE;
					n->object = (Node *) $4;
					n->comment = $6;
					$$ = (Node *) n;
				}
			| COMMENT ON FUNCTION function_with_argtypes IS comment_text
				{
					CommentStmt *n = makeNode(CommentStmt);

					n->objtype = OBJECT_FUNCTION;
					n->object = (Node *) $4;
					n->comment = $6;
					$$ = (Node *) n;
				}
			| COMMENT ON OPERATOR operator_with_argtypes IS comment_text
				{
					CommentStmt *n = makeNode(CommentStmt);

					n->objtype = OBJECT_OPERATOR;
					n->object = (Node *) $4;
					n->comment = $6;
					$$ = (Node *) n;
				}
			| COMMENT ON CONSTRAINT name ON any_name IS comment_text
				{
					CommentStmt *n = makeNode(CommentStmt);

					n->objtype = OBJECT_TABCONSTRAINT;
					n->object = (Node *) lappend($6, makeString($4));
					n->comment = $8;
					$$ = (Node *) n;
				}
			| COMMENT ON CONSTRAINT name ON DOMAIN_P any_name IS comment_text
				{
					CommentStmt *n = makeNode(CommentStmt);

					n->objtype = OBJECT_DOMCONSTRAINT;
					/*
					 * should use Typename not any_name in the production, but
					 * there's a shift/reduce conflict if we do that, so fix it
					 * up here.
					 * 在产生式中本应使用 Typename 而不是 any_name，但如果这样做会有移进/规约冲突，因此在这里进行修正。
					 */
					n->object = (Node *) list_make2(makeTypeNameFromNameList($7), makeString($4));
					n->comment = $9;
					$$ = (Node *) n;
				}
			| COMMENT ON object_type_name_on_any_name name ON any_name IS comment_text
				{
					CommentStmt *n = makeNode(CommentStmt);

					n->objtype = $3;
					n->object = (Node *) lappend($6, makeString($4));
					n->comment = $8;
					$$ = (Node *) n;
				}
			| COMMENT ON PROCEDURE function_with_argtypes IS comment_text
				{
					CommentStmt *n = makeNode(CommentStmt);

					n->objtype = OBJECT_PROCEDURE;
					n->object = (Node *) $4;
					n->comment = $6;
					$$ = (Node *) n;
				}
			| COMMENT ON ROUTINE function_with_argtypes IS comment_text
				{
					CommentStmt *n = makeNode(CommentStmt);

					n->objtype = OBJECT_ROUTINE;
					n->object = (Node *) $4;
					n->comment = $6;
					$$ = (Node *) n;
				}
			| COMMENT ON TRANSFORM FOR Typename LANGUAGE name IS comment_text
				{
					CommentStmt *n = makeNode(CommentStmt);

					n->objtype = OBJECT_TRANSFORM;
					n->object = (Node *) list_make2($5, makeString($7));
					n->comment = $9;
					$$ = (Node *) n;
				}
			| COMMENT ON OPERATOR CLASS any_name USING name IS comment_text
				{
					CommentStmt *n = makeNode(CommentStmt);

					n->objtype = OBJECT_OPCLASS;
					n->object = (Node *) lcons(makeString($7), $5);
					n->comment = $9;
					$$ = (Node *) n;
				}
			| COMMENT ON OPERATOR FAMILY any_name USING name IS comment_text
				{
					CommentStmt *n = makeNode(CommentStmt);

					n->objtype = OBJECT_OPFAMILY;
					n->object = (Node *) lcons(makeString($7), $5);
					n->comment = $9;
					$$ = (Node *) n;
				}
			| COMMENT ON LARGE_P OBJECT_P NumericOnly IS comment_text
				{
					CommentStmt *n = makeNode(CommentStmt);

					n->objtype = OBJECT_LARGEOBJECT;
					n->object = (Node *) $5;
					n->comment = $7;
					$$ = (Node *) n;
				}
			| COMMENT ON CAST '(' Typename AS Typename ')' IS comment_text
				{
					CommentStmt *n = makeNode(CommentStmt);

					n->objtype = OBJECT_CAST;
					n->object = (Node *) list_make2($5, $7);
					n->comment = $10;
					$$ = (Node *) n;
				}
		;

comment_text:
			Sconst								{ $$ = $1; }
			| NULL_P							{ $$ = NULL; }
		;


/*****************************************************************************
 *
 *  SECURITY LABEL [FOR <provider>] ON <object> IS <label>
 *
 *  As with COMMENT ON, <object> can refer to various types of database
 *  objects (e.g. TABLE, COLUMN, etc.).
 *
 * SECURITY LABEL [FOR <provider>] ON <object> IS <label> 与 COMMENT ON 类似，<object> 可以指各种类型的数据库对象（例如 TABLE、COLUMN 等）。
 *****************************************************************************/

SecLabelStmt:
			SECURITY LABEL opt_provider ON object_type_any_name any_name
			IS security_label
				{
					SecLabelStmt *n = makeNode(SecLabelStmt);

					n->provider = $3;
					n->objtype = $5;
					n->object = (Node *) $6;
					n->label = $8;
					$$ = (Node *) n;
				}
			| SECURITY LABEL opt_provider ON COLUMN any_name
			  IS security_label
				{
					SecLabelStmt *n = makeNode(SecLabelStmt);

					n->provider = $3;
					n->objtype = OBJECT_COLUMN;
					n->object = (Node *) $6;
					n->label = $8;
					$$ = (Node *) n;
				}
			| SECURITY LABEL opt_provider ON object_type_name name
			  IS security_label
				{
					SecLabelStmt *n = makeNode(SecLabelStmt);

					n->provider = $3;
					n->objtype = $5;
					n->object = (Node *) makeString($6);
					n->label = $8;
					$$ = (Node *) n;
				}
			| SECURITY LABEL opt_provider ON TYPE_P Typename
			  IS security_label
				{
					SecLabelStmt *n = makeNode(SecLabelStmt);

					n->provider = $3;
					n->objtype = OBJECT_TYPE;
					n->object = (Node *) $6;
					n->label = $8;
					$$ = (Node *) n;
				}
			| SECURITY LABEL opt_provider ON DOMAIN_P Typename
			  IS security_label
				{
					SecLabelStmt *n = makeNode(SecLabelStmt);

					n->provider = $3;
					n->objtype = OBJECT_DOMAIN;
					n->object = (Node *) $6;
					n->label = $8;
					$$ = (Node *) n;
				}
			| SECURITY LABEL opt_provider ON AGGREGATE aggregate_with_argtypes
			  IS security_label
				{
					SecLabelStmt *n = makeNode(SecLabelStmt);

					n->provider = $3;
					n->objtype = OBJECT_AGGREGATE;
					n->object = (Node *) $6;
					n->label = $8;
					$$ = (Node *) n;
				}
			| SECURITY LABEL opt_provider ON FUNCTION function_with_argtypes
			  IS security_label
				{
					SecLabelStmt *n = makeNode(SecLabelStmt);

					n->provider = $3;
					n->objtype = OBJECT_FUNCTION;
					n->object = (Node *) $6;
					n->label = $8;
					$$ = (Node *) n;
				}
			| SECURITY LABEL opt_provider ON LARGE_P OBJECT_P NumericOnly
			  IS security_label
				{
					SecLabelStmt *n = makeNode(SecLabelStmt);

					n->provider = $3;
					n->objtype = OBJECT_LARGEOBJECT;
					n->object = (Node *) $7;
					n->label = $9;
					$$ = (Node *) n;
				}
			| SECURITY LABEL opt_provider ON PROCEDURE function_with_argtypes
			  IS security_label
				{
					SecLabelStmt *n = makeNode(SecLabelStmt);

					n->provider = $3;
					n->objtype = OBJECT_PROCEDURE;
					n->object = (Node *) $6;
					n->label = $8;
					$$ = (Node *) n;
				}
			| SECURITY LABEL opt_provider ON ROUTINE function_with_argtypes
			  IS security_label
				{
					SecLabelStmt *n = makeNode(SecLabelStmt);

					n->provider = $3;
					n->objtype = OBJECT_ROUTINE;
					n->object = (Node *) $6;
					n->label = $8;
					$$ = (Node *) n;
				}
		;

opt_provider:	FOR NonReservedWord_or_Sconst	{ $$ = $2; }
				| /* EMPTY - 空 */					{ $$ = NULL; }
		;

security_label:	Sconst				{ $$ = $1; }
				| NULL_P			{ $$ = NULL; }
		;

/*****************************************************************************
 *
 *		QUERY:
 *			fetch/move
 *
 * 查询：fetch/move
 *****************************************************************************/

FetchStmt:	FETCH fetch_args
				{
					FetchStmt *n = (FetchStmt *) $2;

					n->ismove = false;
					$$ = (Node *) n;
				}
			| MOVE fetch_args
				{
					FetchStmt *n = (FetchStmt *) $2;

					n->ismove = true;
					$$ = (Node *) n;
				}
		;

fetch_args:	cursor_name
				{
					FetchStmt *n = makeNode(FetchStmt);

					n->portalname = $1;
					n->direction = FETCH_FORWARD;
					n->howMany = 1;
					$$ = (Node *) n;
				}
			| from_in cursor_name
				{
					FetchStmt *n = makeNode(FetchStmt);

					n->portalname = $2;
					n->direction = FETCH_FORWARD;
					n->howMany = 1;
					$$ = (Node *) n;
				}
			| NEXT opt_from_in cursor_name
				{
					FetchStmt *n = makeNode(FetchStmt);

					n->portalname = $3;
					n->direction = FETCH_FORWARD;
					n->howMany = 1;
					$$ = (Node *) n;
				}
			| PRIOR opt_from_in cursor_name
				{
					FetchStmt *n = makeNode(FetchStmt);

					n->portalname = $3;
					n->direction = FETCH_BACKWARD;
					n->howMany = 1;
					$$ = (Node *) n;
				}
			| FIRST_P opt_from_in cursor_name
				{
					FetchStmt *n = makeNode(FetchStmt);

					n->portalname = $3;
					n->direction = FETCH_ABSOLUTE;
					n->howMany = 1;
					$$ = (Node *) n;
				}
			| LAST_P opt_from_in cursor_name
				{
					FetchStmt *n = makeNode(FetchStmt);

					n->portalname = $3;
					n->direction = FETCH_ABSOLUTE;
					n->howMany = -1;
					$$ = (Node *) n;
				}
			| ABSOLUTE_P SignedIconst opt_from_in cursor_name
				{
					FetchStmt *n = makeNode(FetchStmt);

					n->portalname = $4;
					n->direction = FETCH_ABSOLUTE;
					n->howMany = $2;
					$$ = (Node *) n;
				}
			| RELATIVE_P SignedIconst opt_from_in cursor_name
				{
					FetchStmt *n = makeNode(FetchStmt);

					n->portalname = $4;
					n->direction = FETCH_RELATIVE;
					n->howMany = $2;
					$$ = (Node *) n;
				}
			| SignedIconst opt_from_in cursor_name
				{
					FetchStmt *n = makeNode(FetchStmt);

					n->portalname = $3;
					n->direction = FETCH_FORWARD;
					n->howMany = $1;
					$$ = (Node *) n;
				}
			| ALL opt_from_in cursor_name
				{
					FetchStmt *n = makeNode(FetchStmt);

					n->portalname = $3;
					n->direction = FETCH_FORWARD;
					n->howMany = FETCH_ALL;
					$$ = (Node *) n;
				}
			| FORWARD opt_from_in cursor_name
				{
					FetchStmt *n = makeNode(FetchStmt);

					n->portalname = $3;
					n->direction = FETCH_FORWARD;
					n->howMany = 1;
					$$ = (Node *) n;
				}
			| FORWARD SignedIconst opt_from_in cursor_name
				{
					FetchStmt *n = makeNode(FetchStmt);

					n->portalname = $4;
					n->direction = FETCH_FORWARD;
					n->howMany = $2;
					$$ = (Node *) n;
				}
			| FORWARD ALL opt_from_in cursor_name
				{
					FetchStmt *n = makeNode(FetchStmt);

					n->portalname = $4;
					n->direction = FETCH_FORWARD;
					n->howMany = FETCH_ALL;
					$$ = (Node *) n;
				}
			| BACKWARD opt_from_in cursor_name
				{
					FetchStmt *n = makeNode(FetchStmt);

					n->portalname = $3;
					n->direction = FETCH_BACKWARD;
					n->howMany = 1;
					$$ = (Node *) n;
				}
			| BACKWARD SignedIconst opt_from_in cursor_name
				{
					FetchStmt *n = makeNode(FetchStmt);

					n->portalname = $4;
					n->direction = FETCH_BACKWARD;
					n->howMany = $2;
					$$ = (Node *) n;
				}
			| BACKWARD ALL opt_from_in cursor_name
				{
					FetchStmt *n = makeNode(FetchStmt);

					n->portalname = $4;
					n->direction = FETCH_BACKWARD;
					n->howMany = FETCH_ALL;
					$$ = (Node *) n;
				}
		;

from_in:	FROM
			| IN_P
		;

opt_from_in:	from_in
			| /* EMPTY - 空 */
		;


/*****************************************************************************
 *
 * GRANT and REVOKE statements
 *
 * GRANT 和 REVOKE 语句
 *****************************************************************************/

GrantStmt:	GRANT privileges ON privilege_target TO grantee_list
			opt_grant_grant_option opt_granted_by
				{
					GrantStmt *n = makeNode(GrantStmt);

					n->is_grant = true;
					n->privileges = $2;
					n->targtype = ($4)->targtype;
					n->objtype = ($4)->objtype;
					n->objects = ($4)->objs;
					n->grantees = $6;
					n->grant_option = $7;
					n->grantor = $8;
					$$ = (Node *) n;
				}
		;

RevokeStmt:
			REVOKE privileges ON privilege_target
			FROM grantee_list opt_granted_by opt_drop_behavior
				{
					GrantStmt *n = makeNode(GrantStmt);

					n->is_grant = false;
					n->grant_option = false;
					n->privileges = $2;
					n->targtype = ($4)->targtype;
					n->objtype = ($4)->objtype;
					n->objects = ($4)->objs;
					n->grantees = $6;
					n->grantor = $7;
					n->behavior = $8;
					$$ = (Node *) n;
				}
			| REVOKE GRANT OPTION FOR privileges ON privilege_target
			FROM grantee_list opt_granted_by opt_drop_behavior
				{
					GrantStmt *n = makeNode(GrantStmt);

					n->is_grant = false;
					n->grant_option = true;
					n->privileges = $5;
					n->targtype = ($7)->targtype;
					n->objtype = ($7)->objtype;
					n->objects = ($7)->objs;
					n->grantees = $9;
					n->grantor = $10;
					n->behavior = $11;
					$$ = (Node *) n;
				}
		;


/*
 * Privilege names are represented as strings; the validity of the privilege
 * names gets checked at execution.  This is a bit annoying but we have little
 * choice because of the syntactic conflict with lists of role names in
 * GRANT/REVOKE.  What's more, we have to call out in the "privilege"
 * production any reserved keywords that need to be usable as privilege names.
 * 特权名称表示为字符串；特权名称的有效性在执行时进行检查。这有点令人讨厌，但由于 GRANT/REVOKE 中角色名称列表的语法冲突，我们别无选择。更重要的是，我们必须在 "privilege" 产生式中调用任何需要可用作特权名称的保留关键字。
 */

/* either ALL [PRIVILEGES] or a list of individual privileges - 要么是 ALL [PRIVILEGES]，要么是单个特权的列表 */
privileges: privilege_list
				{ $$ = $1; }
			| ALL
				{ $$ = NIL; }
			| ALL PRIVILEGES
				{ $$ = NIL; }
			| ALL '(' columnList ')'
				{
					AccessPriv *n = makeNode(AccessPriv);

					n->priv_name = NULL;
					n->cols = $3;
					$$ = list_make1(n);
				}
			| ALL PRIVILEGES '(' columnList ')'
				{
					AccessPriv *n = makeNode(AccessPriv);

					n->priv_name = NULL;
					n->cols = $4;
					$$ = list_make1(n);
				}
		;

privilege_list:	privilege							{ $$ = list_make1($1); }
			| privilege_list ',' privilege			{ $$ = lappend($1, $3); }
		;

privilege:	SELECT opt_column_list
			{
				AccessPriv *n = makeNode(AccessPriv);

				n->priv_name = pstrdup($1);
				n->cols = $2;
				$$ = n;
			}
		| REFERENCES opt_column_list
			{
				AccessPriv *n = makeNode(AccessPriv);

				n->priv_name = pstrdup($1);
				n->cols = $2;
				$$ = n;
			}
		| CREATE opt_column_list
			{
				AccessPriv *n = makeNode(AccessPriv);

				n->priv_name = pstrdup($1);
				n->cols = $2;
				$$ = n;
			}
		| ALTER SYSTEM_P
			{
				AccessPriv *n = makeNode(AccessPriv);
				n->priv_name = pstrdup("alter system");
				n->cols = NIL;
				$$ = n;
			}
		| ColId opt_column_list
			{
				AccessPriv *n = makeNode(AccessPriv);

				n->priv_name = $1;
				n->cols = $2;
				$$ = n;
			}
		;

parameter_name_list:
		parameter_name
			{
				$$ = list_make1(makeString($1));
			}
		| parameter_name_list ',' parameter_name
			{
				$$ = lappend($1, makeString($3));
			}
		;

parameter_name:
		ColId
			{
				$$ = $1;
			}
		| parameter_name '.' ColId
			{
				$$ = psprintf("%s.%s", $1, $3);
			}
		;


/* Don't bother trying to fold the first two rules into one using
 * opt_table.  You're going to get conflicts.
 * 不要费心尝试使用 opt_table 将前两个规则合并为一个。您会遇到冲突。
 */
privilege_target:
			qualified_name_list
				{
					PrivTarget *n = (PrivTarget *) palloc(sizeof(PrivTarget));

					n->targtype = ACL_TARGET_OBJECT;
					n->objtype = OBJECT_TABLE;
					n->objs = $1;
					$$ = n;
				}
			| TABLE qualified_name_list
				{
					PrivTarget *n = (PrivTarget *) palloc(sizeof(PrivTarget));

					n->targtype = ACL_TARGET_OBJECT;
					n->objtype = OBJECT_TABLE;
					n->objs = $2;
					$$ = n;
				}
			| SEQUENCE qualified_name_list
				{
					PrivTarget *n = (PrivTarget *) palloc(sizeof(PrivTarget));

					n->targtype = ACL_TARGET_OBJECT;
					n->objtype = OBJECT_SEQUENCE;
					n->objs = $2;
					$$ = n;
				}
			| FOREIGN DATA_P WRAPPER name_list
				{
					PrivTarget *n = (PrivTarget *) palloc(sizeof(PrivTarget));

					n->targtype = ACL_TARGET_OBJECT;
					n->objtype = OBJECT_FDW;
					n->objs = $4;
					$$ = n;
				}
			| FOREIGN SERVER name_list
				{
					PrivTarget *n = (PrivTarget *) palloc(sizeof(PrivTarget));

					n->targtype = ACL_TARGET_OBJECT;
					n->objtype = OBJECT_FOREIGN_SERVER;
					n->objs = $3;
					$$ = n;
				}
			| FUNCTION function_with_argtypes_list
				{
					PrivTarget *n = (PrivTarget *) palloc(sizeof(PrivTarget));

					n->targtype = ACL_TARGET_OBJECT;
					n->objtype = OBJECT_FUNCTION;
					n->objs = $2;
					$$ = n;
				}
			| PROCEDURE function_with_argtypes_list
				{
					PrivTarget *n = (PrivTarget *) palloc(sizeof(PrivTarget));

					n->targtype = ACL_TARGET_OBJECT;
					n->objtype = OBJECT_PROCEDURE;
					n->objs = $2;
					$$ = n;
				}
			| ROUTINE function_with_argtypes_list
				{
					PrivTarget *n = (PrivTarget *) palloc(sizeof(PrivTarget));

					n->targtype = ACL_TARGET_OBJECT;
					n->objtype = OBJECT_ROUTINE;
					n->objs = $2;
					$$ = n;
				}
			| DATABASE name_list
				{
					PrivTarget *n = (PrivTarget *) palloc(sizeof(PrivTarget));

					n->targtype = ACL_TARGET_OBJECT;
					n->objtype = OBJECT_DATABASE;
					n->objs = $2;
					$$ = n;
				}
			| DOMAIN_P any_name_list
				{
					PrivTarget *n = (PrivTarget *) palloc(sizeof(PrivTarget));

					n->targtype = ACL_TARGET_OBJECT;
					n->objtype = OBJECT_DOMAIN;
					n->objs = $2;
					$$ = n;
				}
			| LANGUAGE name_list
				{
					PrivTarget *n = (PrivTarget *) palloc(sizeof(PrivTarget));

					n->targtype = ACL_TARGET_OBJECT;
					n->objtype = OBJECT_LANGUAGE;
					n->objs = $2;
					$$ = n;
				}
			| LARGE_P OBJECT_P NumericOnly_list
				{
					PrivTarget *n = (PrivTarget *) palloc(sizeof(PrivTarget));

					n->targtype = ACL_TARGET_OBJECT;
					n->objtype = OBJECT_LARGEOBJECT;
					n->objs = $3;
					$$ = n;
				}
			| PARAMETER parameter_name_list
				{
					PrivTarget *n = (PrivTarget *) palloc(sizeof(PrivTarget));
					n->targtype = ACL_TARGET_OBJECT;
					n->objtype = OBJECT_PARAMETER_ACL;
					n->objs = $2;
					$$ = n;
				}
			| SCHEMA name_list
				{
					PrivTarget *n = (PrivTarget *) palloc(sizeof(PrivTarget));

					n->targtype = ACL_TARGET_OBJECT;
					n->objtype = OBJECT_SCHEMA;
					n->objs = $2;
					$$ = n;
				}
			| TABLESPACE name_list
				{
					PrivTarget *n = (PrivTarget *) palloc(sizeof(PrivTarget));

					n->targtype = ACL_TARGET_OBJECT;
					n->objtype = OBJECT_TABLESPACE;
					n->objs = $2;
					$$ = n;
				}
			| TYPE_P any_name_list
				{
					PrivTarget *n = (PrivTarget *) palloc(sizeof(PrivTarget));

					n->targtype = ACL_TARGET_OBJECT;
					n->objtype = OBJECT_TYPE;
					n->objs = $2;
					$$ = n;
				}
			| ALL TABLES IN_P SCHEMA name_list
				{
					PrivTarget *n = (PrivTarget *) palloc(sizeof(PrivTarget));

					n->targtype = ACL_TARGET_ALL_IN_SCHEMA;
					n->objtype = OBJECT_TABLE;
					n->objs = $5;
					$$ = n;
				}
			| ALL SEQUENCES IN_P SCHEMA name_list
				{
					PrivTarget *n = (PrivTarget *) palloc(sizeof(PrivTarget));

					n->targtype = ACL_TARGET_ALL_IN_SCHEMA;
					n->objtype = OBJECT_SEQUENCE;
					n->objs = $5;
					$$ = n;
				}
			| ALL FUNCTIONS IN_P SCHEMA name_list
				{
					PrivTarget *n = (PrivTarget *) palloc(sizeof(PrivTarget));

					n->targtype = ACL_TARGET_ALL_IN_SCHEMA;
					n->objtype = OBJECT_FUNCTION;
					n->objs = $5;
					$$ = n;
				}
			| ALL PROCEDURES IN_P SCHEMA name_list
				{
					PrivTarget *n = (PrivTarget *) palloc(sizeof(PrivTarget));

					n->targtype = ACL_TARGET_ALL_IN_SCHEMA;
					n->objtype = OBJECT_PROCEDURE;
					n->objs = $5;
					$$ = n;
				}
			| ALL ROUTINES IN_P SCHEMA name_list
				{
					PrivTarget *n = (PrivTarget *) palloc(sizeof(PrivTarget));

					n->targtype = ACL_TARGET_ALL_IN_SCHEMA;
					n->objtype = OBJECT_ROUTINE;
					n->objs = $5;
					$$ = n;
				}
		;


grantee_list:
			grantee									{ $$ = list_make1($1); }
			| grantee_list ',' grantee				{ $$ = lappend($1, $3); }
		;

grantee:
			RoleSpec								{ $$ = $1; }
			| GROUP_P RoleSpec						{ $$ = $2; }
		;


opt_grant_grant_option:
			WITH GRANT OPTION { $$ = true; }
			| /* EMPTY - 空 */ { $$ = false; }
		;

/*****************************************************************************
 *
 * GRANT and REVOKE ROLE statements
 *
 * GRANT 和 REVOKE ROLE 语句
 *****************************************************************************/

GrantRoleStmt:
			GRANT privilege_list TO role_list opt_granted_by
				{
					GrantRoleStmt *n = makeNode(GrantRoleStmt);

					n->is_grant = true;
					n->granted_roles = $2;
					n->grantee_roles = $4;
					n->opt = NIL;
					n->grantor = $5;
					$$ = (Node *) n;
				}
		  | GRANT privilege_list TO role_list WITH grant_role_opt_list opt_granted_by
				{
					GrantRoleStmt *n = makeNode(GrantRoleStmt);

					n->is_grant = true;
					n->granted_roles = $2;
					n->grantee_roles = $4;
					n->opt = $6;
					n->grantor = $7;
					$$ = (Node *) n;
				}
		;

RevokeRoleStmt:
			REVOKE privilege_list FROM role_list opt_granted_by opt_drop_behavior
				{
					GrantRoleStmt *n = makeNode(GrantRoleStmt);

					n->is_grant = false;
					n->opt = NIL;
					n->granted_roles = $2;
					n->grantee_roles = $4;
					n->grantor = $5;
					n->behavior = $6;
					$$ = (Node *) n;
				}
			| REVOKE ColId OPTION FOR privilege_list FROM role_list opt_granted_by opt_drop_behavior
				{
					GrantRoleStmt *n = makeNode(GrantRoleStmt);
					DefElem *opt;

					opt = makeDefElem(pstrdup($2),
									  (Node *) makeBoolean(false), @2);
					n->is_grant = false;
					n->opt = list_make1(opt);
					n->granted_roles = $5;
					n->grantee_roles = $7;
					n->grantor = $8;
					n->behavior = $9;
					$$ = (Node *) n;
				}
		;

grant_role_opt_list:
			grant_role_opt_list ',' grant_role_opt	{ $$ = lappend($1, $3); }
			| grant_role_opt						{ $$ = list_make1($1); }
		;

grant_role_opt:
		ColLabel grant_role_opt_value
			{
				$$ = makeDefElem(pstrdup($1), $2, @1);
			}
		;

grant_role_opt_value:
		OPTION			{ $$ = (Node *) makeBoolean(true); }
		| TRUE_P		{ $$ = (Node *) makeBoolean(true); }
		| FALSE_P		{ $$ = (Node *) makeBoolean(false); }
		;

opt_granted_by: GRANTED BY RoleSpec						{ $$ = $3; }
			| /* EMPTY - 空 */									{ $$ = NULL; }
		;

/*****************************************************************************
 *
 * ALTER DEFAULT PRIVILEGES statement
 *
 * ALTER DEFAULT PRIVILEGES 语句
 *****************************************************************************/

AlterDefaultPrivilegesStmt:
			ALTER DEFAULT PRIVILEGES DefACLOptionList DefACLAction
				{
					AlterDefaultPrivilegesStmt *n = makeNode(AlterDefaultPrivilegesStmt);

					n->options = $4;
					n->action = (GrantStmt *) $5;
					$$ = (Node *) n;
				}
		;

DefACLOptionList:
			DefACLOptionList DefACLOption			{ $$ = lappend($1, $2); }
			| /* EMPTY - 空 */							{ $$ = NIL; }
		;

DefACLOption:
			IN_P SCHEMA name_list
				{
					$$ = makeDefElem("schemas", (Node *) $3, @1);
				}
			| FOR ROLE role_list
				{
					$$ = makeDefElem("roles", (Node *) $3, @1);
				}
			| FOR USER role_list
				{
					$$ = makeDefElem("roles", (Node *) $3, @1);
				}
		;

/*
 * This should match GRANT/REVOKE, except that individual target objects
 * are not mentioned and we only allow a subset of object types.
 * 这应该与 GRANT/REVOKE 匹配，除了没有提及单个目标对象，并且我们仅允许对象类型的子集。
 */
DefACLAction:
			GRANT privileges ON defacl_privilege_target TO grantee_list
			opt_grant_grant_option
				{
					GrantStmt *n = makeNode(GrantStmt);

					n->is_grant = true;
					n->privileges = $2;
					n->targtype = ACL_TARGET_DEFAULTS;
					n->objtype = $4;
					n->objects = NIL;
					n->grantees = $6;
					n->grant_option = $7;
					$$ = (Node *) n;
				}
			| REVOKE privileges ON defacl_privilege_target
			FROM grantee_list opt_drop_behavior
				{
					GrantStmt *n = makeNode(GrantStmt);

					n->is_grant = false;
					n->grant_option = false;
					n->privileges = $2;
					n->targtype = ACL_TARGET_DEFAULTS;
					n->objtype = $4;
					n->objects = NIL;
					n->grantees = $6;
					n->behavior = $7;
					$$ = (Node *) n;
				}
			| REVOKE GRANT OPTION FOR privileges ON defacl_privilege_target
			FROM grantee_list opt_drop_behavior
				{
					GrantStmt *n = makeNode(GrantStmt);

					n->is_grant = false;
					n->grant_option = true;
					n->privileges = $5;
					n->targtype = ACL_TARGET_DEFAULTS;
					n->objtype = $7;
					n->objects = NIL;
					n->grantees = $9;
					n->behavior = $10;
					$$ = (Node *) n;
				}
		;

defacl_privilege_target:
			TABLES			{ $$ = OBJECT_TABLE; }
			| FUNCTIONS		{ $$ = OBJECT_FUNCTION; }
			| ROUTINES		{ $$ = OBJECT_FUNCTION; }
			| SEQUENCES		{ $$ = OBJECT_SEQUENCE; }
			| TYPES_P		{ $$ = OBJECT_TYPE; }
			| SCHEMAS		{ $$ = OBJECT_SCHEMA; }
			| LARGE_P OBJECTS_P	{ $$ = OBJECT_LARGEOBJECT; }
		;


/*****************************************************************************
 *
 *		QUERY: CREATE INDEX
 *
 * Note: we cannot put TABLESPACE clause after WHERE clause unless we are
 * willing to make TABLESPACE a fully reserved word.
 * 查询：CREATE INDEX。注意：我们不能将 TABLESPACE 子句放在 WHERE 子句之后，除非我们愿意将 TABLESPACE 设为完全保留的关键字。
 *****************************************************************************/

IndexStmt:	CREATE opt_unique INDEX opt_concurrently opt_single_name
			ON relation_expr access_method_clause '(' index_params ')'
			opt_include opt_unique_null_treatment opt_reloptions OptTableSpace where_clause
				{
					IndexStmt *n = makeNode(IndexStmt);

					n->unique = $2;
					n->concurrent = $4;
					n->idxname = $5;
					n->relation = $7;
					n->accessMethod = $8;
					n->indexParams = $10;
					n->indexIncludingParams = $12;
					n->nulls_not_distinct = !$13;
					n->options = $14;
					n->tableSpace = $15;
					n->whereClause = $16;
					n->excludeOpNames = NIL;
					n->idxcomment = NULL;
					n->indexOid = InvalidOid;
					n->oldNumber = InvalidRelFileNumber;
					n->oldCreateSubid = InvalidSubTransactionId;
					n->oldFirstRelfilelocatorSubid = InvalidSubTransactionId;
					n->primary = false;
					n->isconstraint = false;
					n->deferrable = false;
					n->initdeferred = false;
					n->transformed = false;
					n->if_not_exists = false;
					n->reset_default_tblspc = false;
					$$ = (Node *) n;
				}
			| CREATE opt_unique INDEX opt_concurrently IF_P NOT EXISTS name
			ON relation_expr access_method_clause '(' index_params ')'
			opt_include opt_unique_null_treatment opt_reloptions OptTableSpace where_clause
				{
					IndexStmt *n = makeNode(IndexStmt);

					n->unique = $2;
					n->concurrent = $4;
					n->idxname = $8;
					n->relation = $10;
					n->accessMethod = $11;
					n->indexParams = $13;
					n->indexIncludingParams = $15;
					n->nulls_not_distinct = !$16;
					n->options = $17;
					n->tableSpace = $18;
					n->whereClause = $19;
					n->excludeOpNames = NIL;
					n->idxcomment = NULL;
					n->indexOid = InvalidOid;
					n->oldNumber = InvalidRelFileNumber;
					n->oldCreateSubid = InvalidSubTransactionId;
					n->oldFirstRelfilelocatorSubid = InvalidSubTransactionId;
					n->primary = false;
					n->isconstraint = false;
					n->deferrable = false;
					n->initdeferred = false;
					n->transformed = false;
					n->if_not_exists = true;
					n->reset_default_tblspc = false;
					$$ = (Node *) n;
				}
		;

opt_unique:
			UNIQUE									{ $$ = true; }
			| /* EMPTY - 空 */								{ $$ = false; }
		;

access_method_clause:
			USING name								{ $$ = $2; }
			| /* EMPTY - 空 */								{ $$ = DEFAULT_INDEX_TYPE; }
		;

index_params:	index_elem							{ $$ = list_make1($1); }
			| index_params ',' index_elem			{ $$ = lappend($1, $3); }
		;


index_elem_options:
	opt_collate opt_qualified_name opt_asc_desc opt_nulls_order
		{
			$$ = makeNode(IndexElem);
			$$->name = NULL;
			$$->expr = NULL;
			$$->indexcolname = NULL;
			$$->collation = $1;
			$$->opclass = $2;
			$$->opclassopts = NIL;
			$$->ordering = $3;
			$$->nulls_ordering = $4;
		}
	| opt_collate any_name reloptions opt_asc_desc opt_nulls_order
		{
			$$ = makeNode(IndexElem);
			$$->name = NULL;
			$$->expr = NULL;
			$$->indexcolname = NULL;
			$$->collation = $1;
			$$->opclass = $2;
			$$->opclassopts = $3;
			$$->ordering = $4;
			$$->nulls_ordering = $5;
		}
	;

/*
 * Index attributes can be either simple column references, or arbitrary
 * expressions in parens.  For backwards-compatibility reasons, we allow
 * an expression that's just a function call to be written without parens.
 * 索引属性可以是简单的列引用，也可以是括号中的任意表达式。出于向后兼容的原因，我们允许将仅是函数调用的表达式写为不带括号的形式。
 */
index_elem: ColId index_elem_options
				{
					$$ = $2;
					$$->name = $1;
				}
			| func_expr_windowless index_elem_options
				{
					$$ = $2;
					$$->expr = $1;
				}
			| '(' a_expr ')' index_elem_options
				{
					$$ = $4;
					$$->expr = $2;
				}
		;

opt_include:		INCLUDE '(' index_including_params ')'			{ $$ = $3; }
			 |		/* EMPTY - 空 */						{ $$ = NIL; }
		;

index_including_params:	index_elem						{ $$ = list_make1($1); }
			| index_including_params ',' index_elem		{ $$ = lappend($1, $3); }
		;

opt_collate: COLLATE any_name						{ $$ = $2; }
			| /* EMPTY - 空 */								{ $$ = NIL; }
		;


opt_asc_desc: ASC							{ $$ = SORTBY_ASC; }
			| DESC							{ $$ = SORTBY_DESC; }
			| /* EMPTY - 空 */						{ $$ = SORTBY_DEFAULT; }
		;

opt_nulls_order: NULLS_LA FIRST_P			{ $$ = SORTBY_NULLS_FIRST; }
			| NULLS_LA LAST_P				{ $$ = SORTBY_NULLS_LAST; }
			| /* EMPTY - 空 */						{ $$ = SORTBY_NULLS_DEFAULT; }
		;


/*****************************************************************************
 *
 *		QUERY:
 *				create [or replace] function <fname>
 *						[(<type-1> { , <type-n>})]
 *						returns <type-r>
 *						as <filename or code in language as appropriate>
 *						language <lang> [with parameters]
 *
 * 查询：create [or replace] function <fname> [(<type-1> { , <type-n>})] returns <type-r> as <filename or code in language as appropriate> language <lang> [with parameters]
 *****************************************************************************/

CreateFunctionStmt:
			CREATE opt_or_replace FUNCTION func_name func_args_with_defaults
			RETURNS func_return opt_createfunc_opt_list opt_routine_body
				{
					CreateFunctionStmt *n = makeNode(CreateFunctionStmt);

					n->is_procedure = false;
					n->replace = $2;
					n->funcname = $4;
					n->parameters = $5;
					n->returnType = $7;
					n->options = $8;
					n->sql_body = $9;
					$$ = (Node *) n;
				}
			| CREATE opt_or_replace FUNCTION func_name func_args_with_defaults
			  RETURNS TABLE '(' table_func_column_list ')' opt_createfunc_opt_list opt_routine_body
				{
					CreateFunctionStmt *n = makeNode(CreateFunctionStmt);

					n->is_procedure = false;
					n->replace = $2;
					n->funcname = $4;
					n->parameters = mergeTableFuncParameters($5, $9, yyscanner);
					n->returnType = TableFuncTypeName($9);
					n->returnType->location = @7;
					n->options = $11;
					n->sql_body = $12;
					$$ = (Node *) n;
				}
			| CREATE opt_or_replace FUNCTION func_name func_args_with_defaults
			  opt_createfunc_opt_list opt_routine_body
				{
					CreateFunctionStmt *n = makeNode(CreateFunctionStmt);

					n->is_procedure = false;
					n->replace = $2;
					n->funcname = $4;
					n->parameters = $5;
					n->returnType = NULL;
					n->options = $6;
					n->sql_body = $7;
					$$ = (Node *) n;
				}
			| CREATE opt_or_replace PROCEDURE func_name func_args_with_defaults
			  opt_createfunc_opt_list opt_routine_body
				{
					CreateFunctionStmt *n = makeNode(CreateFunctionStmt);

					n->is_procedure = true;
					n->replace = $2;
					n->funcname = $4;
					n->parameters = $5;
					n->returnType = NULL;
					n->options = $6;
					n->sql_body = $7;
					$$ = (Node *) n;
				}
		;

opt_or_replace:
			OR REPLACE								{ $$ = true; }
			| /* EMPTY - 空 */								{ $$ = false; }
		;

func_args:	'(' func_args_list ')'					{ $$ = $2; }
			| '(' ')'								{ $$ = NIL; }
		;

func_args_list:
			func_arg								{ $$ = list_make1($1); }
			| func_args_list ',' func_arg			{ $$ = lappend($1, $3); }
		;

function_with_argtypes_list:
			function_with_argtypes					{ $$ = list_make1($1); }
			| function_with_argtypes_list ',' function_with_argtypes
													{ $$ = lappend($1, $3); }
		;

function_with_argtypes:
			func_name func_args
				{
					ObjectWithArgs *n = makeNode(ObjectWithArgs);

					n->objname = $1;
					n->objargs = extractArgTypes($2);
					n->objfuncargs = $2;
					$$ = n;
				}
			/*
			 * Because of reduce/reduce conflicts, we can't use func_name
			 * below, but we can write it out the long way, which actually
			 * allows more cases.
			 * 由于规约/规约冲突，我们无法在下面使用 func_name，但我们可以用长路径将其写出来，这实际上允许了更多情况。
			 */
			| type_func_name_keyword
				{
					ObjectWithArgs *n = makeNode(ObjectWithArgs);

					n->objname = list_make1(makeString(pstrdup($1)));
					n->args_unspecified = true;
					$$ = n;
				}
			| ColId
				{
					ObjectWithArgs *n = makeNode(ObjectWithArgs);

					n->objname = list_make1(makeString($1));
					n->args_unspecified = true;
					$$ = n;
				}
			| ColId indirection
				{
					ObjectWithArgs *n = makeNode(ObjectWithArgs);

					n->objname = check_func_name(lcons(makeString($1), $2),
												  yyscanner);
					n->args_unspecified = true;
					$$ = n;
				}
		;

/*
 * func_args_with_defaults is separate because we only want to accept
 * defaults in CREATE FUNCTION, not in ALTER etc.
 * func_args_with_defaults 是独立的，因为我们只想在 CREATE FUNCTION 中接受默认值，而在 ALTER 等中不接受。
 */
func_args_with_defaults:
		'(' func_args_with_defaults_list ')'		{ $$ = $2; }
		| '(' ')'									{ $$ = NIL; }
		;

func_args_with_defaults_list:
		func_arg_with_default						{ $$ = list_make1($1); }
		| func_args_with_defaults_list ',' func_arg_with_default
													{ $$ = lappend($1, $3); }
		;

/*
 * The style with arg_class first is SQL99 standard, but Oracle puts
 * param_name first; accept both since it's likely people will try both
 * anyway.  Don't bother trying to save productions by letting arg_class
 * have an empty alternative ... you'll get shift/reduce conflicts.
 *
 * We can catch over-specified arguments here if we want to,
 * but for now better to silently swallow typmod, etc.
 * - thomas 2000-03-22
 * arg_class 在前的风格是 SQL99 标准，但 Oracle 将 param_name 放在前面；接受这两者，因为人们很可能会尝试这两者。不要麻烦地试图通过让 arg_class 具有空替代项来节省产生式...您会遇到移进/规约冲突。如果我们愿意，我们可以在这里捕获过度指定的参数，但目前最好默默地吞下 typmod 等。- thomas 2000-03-22
 */
func_arg:
			arg_class param_name func_type
				{
					FunctionParameter *n = makeNode(FunctionParameter);

					n->name = $2;
					n->argType = $3;
					n->mode = $1;
					n->defexpr = NULL;
					n->location = @1;
					$$ = n;
				}
			| param_name arg_class func_type
				{
					FunctionParameter *n = makeNode(FunctionParameter);

					n->name = $1;
					n->argType = $3;
					n->mode = $2;
					n->defexpr = NULL;
					n->location = @1;
					$$ = n;
				}
			| param_name func_type
				{
					FunctionParameter *n = makeNode(FunctionParameter);

					n->name = $1;
					n->argType = $2;
					n->mode = FUNC_PARAM_DEFAULT;
					n->defexpr = NULL;
					n->location = @1;
					$$ = n;
				}
			| arg_class func_type
				{
					FunctionParameter *n = makeNode(FunctionParameter);

					n->name = NULL;
					n->argType = $2;
					n->mode = $1;
					n->defexpr = NULL;
					n->location = @1;
					$$ = n;
				}
			| func_type
				{
					FunctionParameter *n = makeNode(FunctionParameter);

					n->name = NULL;
					n->argType = $1;
					n->mode = FUNC_PARAM_DEFAULT;
					n->defexpr = NULL;
					n->location = @1;
					$$ = n;
				}
		;

/* INOUT is SQL99 standard, IN OUT is for Oracle compatibility - INOUT 是 SQL99 标准，IN OUT 是为了 Oracle 兼容性 */
arg_class:	IN_P								{ $$ = FUNC_PARAM_IN; }
			| OUT_P								{ $$ = FUNC_PARAM_OUT; }
			| INOUT								{ $$ = FUNC_PARAM_INOUT; }
			| IN_P OUT_P						{ $$ = FUNC_PARAM_INOUT; }
			| VARIADIC							{ $$ = FUNC_PARAM_VARIADIC; }
		;

/*
 * Ideally param_name should be ColId, but that causes too many conflicts.
 * 理想情况下，param_name 应该是 ColId，但这会引起太多的冲突。
 */
param_name:	type_function_name
		;

func_return:
			func_type
				{
					/* We can catch over-specified results here if we want to,
					 * but for now better to silently swallow typmod, etc.
					 * - thomas 2000-03-22
					 * 如果我们愿意，我们可以在这里捕获过度指定的结果，但目前最好默默地吞下 typmod 等。- thomas 2000-03-22
					 */
					$$ = $1;
				}
		;

/*
 * We would like to make the %TYPE productions here be ColId attrs etc,
 * but that causes reduce/reduce conflicts.  type_function_name
 * is next best choice.
 * 我们希望在这里使 %TYPE 产生式成为 ColId 属性等，但这会引起规约/规约冲突。type_function_name 是次优选择。
 */
func_type:	Typename								{ $$ = $1; }
			| type_function_name attrs '%' TYPE_P
				{
					$$ = makeTypeNameFromNameList(lcons(makeString($1), $2));
					$$->pct_type = true;
					$$->location = @1;
				}
			| SETOF type_function_name attrs '%' TYPE_P
				{
					$$ = makeTypeNameFromNameList(lcons(makeString($2), $3));
					$$->pct_type = true;
					$$->setof = true;
					$$->location = @2;
				}
		;

func_arg_with_default:
		func_arg
				{
					$$ = $1;
				}
		| func_arg DEFAULT a_expr
				{
					$$ = $1;
					$$->defexpr = $3;
				}
		| func_arg '=' a_expr
				{
					$$ = $1;
					$$->defexpr = $3;
				}
		;

/* Aggregate args can be most things that function args can be - 聚合参数可以是函数参数所能成为的大多数内容 */
aggr_arg:	func_arg
				{
					if (!($1->mode == FUNC_PARAM_DEFAULT ||
						  $1->mode == FUNC_PARAM_IN ||
						  $1->mode == FUNC_PARAM_VARIADIC))
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("aggregates cannot have output arguments"),
								 parser_errposition(@1)));
					$$ = $1;
				}
		;

/*
 * The SQL standard offers no guidance on how to declare aggregate argument
 * lists, since it doesn't have CREATE AGGREGATE etc.  We accept these cases:
 *
 * (*)									- normal agg with no args
 * (aggr_arg,...)						- normal agg with args
 * (ORDER BY aggr_arg,...)				- ordered-set agg with no direct args
 * (aggr_arg,... ORDER BY aggr_arg,...)	- ordered-set agg with direct args
 *
 * The zero-argument case is spelled with '*' for consistency with COUNT(*).
 *
 * An additional restriction is that if the direct-args list ends in a
 * VARIADIC item, the ordered-args list must contain exactly one item that
 * is also VARIADIC with the same type.  This allows us to collapse the two
 * VARIADIC items into one, which is necessary to represent the aggregate in
 * pg_proc.  We check this at the grammar stage so that we can return a list
 * in which the second VARIADIC item is already discarded, avoiding extra work
 * in cases such as DROP AGGREGATE.
 *
 * The return value of this production is a two-element list, in which the
 * first item is a sublist of FunctionParameter nodes (with any duplicate
 * VARIADIC item already dropped, as per above) and the second is an Integer
 * node, containing -1 if there was no ORDER BY and otherwise the number
 * of argument declarations before the ORDER BY.  (If this number is equal
 * to the first sublist's length, then we dropped a duplicate VARIADIC item.)
 * This representation is passed as-is to CREATE AGGREGATE; for operations
 * on existing aggregates, we can just apply extractArgTypes to the first
 * sublist.
 * SQL 标准没有提供关于如何声明聚合参数列表的指导，因为它没有 CREATE AGGREGATE 等。我们接受以下情况：(*) - 无参数的普通聚合， (aggr_arg,...) - 有参数的普通聚合， (ORDER BY aggr_arg,...) - 无直接参数的有序集聚合， (aggr_arg,... ORDER BY aggr_arg,...) - 有直接参数的有序集聚合。零参数情况使用 '*' 拼写，以与 COUNT(*) 保持一致。另一个限制是，如果直接参数列表以 VARIADIC 项结尾，则有序参数列表必须包含且仅包含一个也是相同类型的 VARIADIC 项。这允许我们将两个 VARIADIC 项合并为一个，这对于在 pg_proc 中表示聚合是必要的。我们在语法阶段对此进行检查，以便我们可以返回一个已丢弃第二个 VARIADIC 项的列表，从而避免在诸如 DROP AGGREGATE 的情况下进行额外的工作。此产生式的返回值是一个双元素列表，其中第一项是 FunctionParameter 节点的子列表（已按上述要求丢弃了重复的 VARIADIC 项），第二项是一个 Integer 节点，如果没有 ORDER BY 则包含 -1，否则包含 ORDER BY 之前的参数声明数。（如果此数量等于第一个子列表的长度，则我们丢弃了重复的 VARIADIC 项。）此表示形式原封不动地传递给 CREATE AGGREGATE；对于对现有聚合的操作，我们可以直接对第一个子列表应用 extractArgTypes。
 */
aggr_args:	'(' '*' ')'
				{
					$$ = list_make2(NIL, makeInteger(-1));
				}
			| '(' aggr_args_list ')'
				{
					$$ = list_make2($2, makeInteger(-1));
				}
			| '(' ORDER BY aggr_args_list ')'
				{
					$$ = list_make2($4, makeInteger(0));
				}
			| '(' aggr_args_list ORDER BY aggr_args_list ')'
				{
					/* this is the only case requiring consistency checking - 这是唯一需要一致性检查的情况 */
					$$ = makeOrderedSetArgs($2, $5, yyscanner);
				}
		;

aggr_args_list:
			aggr_arg								{ $$ = list_make1($1); }
			| aggr_args_list ',' aggr_arg			{ $$ = lappend($1, $3); }
		;

aggregate_with_argtypes:
			func_name aggr_args
				{
					ObjectWithArgs *n = makeNode(ObjectWithArgs);

					n->objname = $1;
					n->objargs = extractAggrArgTypes($2);
					n->objfuncargs = (List *) linitial($2);
					$$ = n;
				}
		;

aggregate_with_argtypes_list:
			aggregate_with_argtypes					{ $$ = list_make1($1); }
			| aggregate_with_argtypes_list ',' aggregate_with_argtypes
													{ $$ = lappend($1, $3); }
		;

opt_createfunc_opt_list:
			createfunc_opt_list
			| /* EMPTY - 空 */ { $$ = NIL; }
	;

createfunc_opt_list:
			/* Must be at least one to prevent conflict - 必须至少有一个以防止冲突 */
			createfunc_opt_item						{ $$ = list_make1($1); }
			| createfunc_opt_list createfunc_opt_item { $$ = lappend($1, $2); }
	;

/*
 * Options common to both CREATE FUNCTION and ALTER FUNCTION
 * CREATE FUNCTION 和 ALTER FUNCTION 共有的选项
 */
common_func_opt_item:
			CALLED ON NULL_P INPUT_P
				{
					$$ = makeDefElem("strict", (Node *) makeBoolean(false), @1);
				}
			| RETURNS NULL_P ON NULL_P INPUT_P
				{
					$$ = makeDefElem("strict", (Node *) makeBoolean(true), @1);
				}
			| STRICT_P
				{
					$$ = makeDefElem("strict", (Node *) makeBoolean(true), @1);
				}
			| IMMUTABLE
				{
					$$ = makeDefElem("volatility", (Node *) makeString("immutable"), @1);
				}
			| STABLE
				{
					$$ = makeDefElem("volatility", (Node *) makeString("stable"), @1);
				}
			| VOLATILE
				{
					$$ = makeDefElem("volatility", (Node *) makeString("volatile"), @1);
				}
			| EXTERNAL SECURITY DEFINER
				{
					$$ = makeDefElem("security", (Node *) makeBoolean(true), @1);
				}
			| EXTERNAL SECURITY INVOKER
				{
					$$ = makeDefElem("security", (Node *) makeBoolean(false), @1);
				}
			| SECURITY DEFINER
				{
					$$ = makeDefElem("security", (Node *) makeBoolean(true), @1);
				}
			| SECURITY INVOKER
				{
					$$ = makeDefElem("security", (Node *) makeBoolean(false), @1);
				}
			| LEAKPROOF
				{
					$$ = makeDefElem("leakproof", (Node *) makeBoolean(true), @1);
				}
			| NOT LEAKPROOF
				{
					$$ = makeDefElem("leakproof", (Node *) makeBoolean(false), @1);
				}
			| COST NumericOnly
				{
					$$ = makeDefElem("cost", (Node *) $2, @1);
				}
			| ROWS NumericOnly
				{
					$$ = makeDefElem("rows", (Node *) $2, @1);
				}
			| SUPPORT any_name
				{
					$$ = makeDefElem("support", (Node *) $2, @1);
				}
			| FunctionSetResetClause
				{
					/* we abuse the normal content of a DefElem here - 我们在这里滥用了 DefElem 的正常内容 */
					$$ = makeDefElem("set", (Node *) $1, @1);
				}
			| PARALLEL ColId
				{
					$$ = makeDefElem("parallel", (Node *) makeString($2), @1);
				}
		;

createfunc_opt_item:
			AS func_as
				{
					$$ = makeDefElem("as", (Node *) $2, @1);
				}
			| LANGUAGE NonReservedWord_or_Sconst
				{
					$$ = makeDefElem("language", (Node *) makeString($2), @1);
				}
			| TRANSFORM transform_type_list
				{
					$$ = makeDefElem("transform", (Node *) $2, @1);
				}
			| WINDOW
				{
					$$ = makeDefElem("window", (Node *) makeBoolean(true), @1);
				}
			| common_func_opt_item
				{
					$$ = $1;
				}
		;

func_as:	Sconst						{ $$ = list_make1(makeString($1)); }
			| Sconst ',' Sconst
				{
					$$ = list_make2(makeString($1), makeString($3));
				}
		;

ReturnStmt:	RETURN a_expr
				{
					ReturnStmt *r = makeNode(ReturnStmt);

					r->returnval = (Node *) $2;
					$$ = (Node *) r;
				}
		;

opt_routine_body:
			ReturnStmt
				{
					$$ = $1;
				}
			| BEGIN_P ATOMIC routine_body_stmt_list END_P
				{
					/*
					 * A compound statement is stored as a single-item list
					 * containing the list of statements as its member.  That
					 * way, the parse analysis code can tell apart an empty
					 * body from no body at all.
					 * 复合语句存储为包含语句列表作为其成员的单项列表。这样，解析分析代码就可以将空主体与根本没有主体区分开来。
					 */
					$$ = (Node *) list_make1($3);
				}
			| /* EMPTY - 空 */
				{
					$$ = NULL;
				}
		;

routine_body_stmt_list:
			routine_body_stmt_list routine_body_stmt ';'
				{
					/* As in stmtmulti, discard empty statements - 与 stmtmulti 中一样，丢弃空语句 */
					if ($2 != NULL)
						$$ = lappend($1, $2);
					else
						$$ = $1;
				}
			| /* EMPTY - 空 */
				{
					$$ = NIL;
				}
		;

routine_body_stmt:
			stmt
			| ReturnStmt
		;

transform_type_list:
			FOR TYPE_P Typename { $$ = list_make1($3); }
			| transform_type_list ',' FOR TYPE_P Typename { $$ = lappend($1, $5); }
		;

opt_definition:
			WITH definition							{ $$ = $2; }
			| /* EMPTY - 空 */								{ $$ = NIL; }
		;

table_func_column:	param_name func_type
				{
					FunctionParameter *n = makeNode(FunctionParameter);

					n->name = $1;
					n->argType = $2;
					n->mode = FUNC_PARAM_TABLE;
					n->defexpr = NULL;
					n->location = @1;
					$$ = n;
				}
		;

table_func_column_list:
			table_func_column
				{
					$$ = list_make1($1);
				}
			| table_func_column_list ',' table_func_column
				{
					$$ = lappend($1, $3);
				}
		;

/*****************************************************************************
 * ALTER FUNCTION / ALTER PROCEDURE / ALTER ROUTINE
 *
 * RENAME and OWNER subcommands are already provided by the generic
 * ALTER infrastructure, here we just specify alterations that can
 * only be applied to functions.
 *
 * ALTER FUNCTION / ALTER PROCEDURE / ALTER ROUTINE 的 RENAME 和 OWNER 子命令已由通用的 ALTER 基础设施提供，在此我们仅指定仅能应用于函数的更改。
 *****************************************************************************/
AlterFunctionStmt:
			ALTER FUNCTION function_with_argtypes alterfunc_opt_list opt_restrict
				{
					AlterFunctionStmt *n = makeNode(AlterFunctionStmt);

					n->objtype = OBJECT_FUNCTION;
					n->func = $3;
					n->actions = $4;
					$$ = (Node *) n;
				}
			| ALTER PROCEDURE function_with_argtypes alterfunc_opt_list opt_restrict
				{
					AlterFunctionStmt *n = makeNode(AlterFunctionStmt);

					n->objtype = OBJECT_PROCEDURE;
					n->func = $3;
					n->actions = $4;
					$$ = (Node *) n;
				}
			| ALTER ROUTINE function_with_argtypes alterfunc_opt_list opt_restrict
				{
					AlterFunctionStmt *n = makeNode(AlterFunctionStmt);

					n->objtype = OBJECT_ROUTINE;
					n->func = $3;
					n->actions = $4;
					$$ = (Node *) n;
				}
		;

alterfunc_opt_list:
			/* At least one option must be specified - 必须至少指定一个选项 */
			common_func_opt_item					{ $$ = list_make1($1); }
			| alterfunc_opt_list common_func_opt_item { $$ = lappend($1, $2); }
		;

/* Ignored, merely for SQL compliance - 已忽略，仅为了符合 SQL 规范 */
opt_restrict:
			RESTRICT
			| /* EMPTY - 空 */
		;


/*****************************************************************************
 *
 *		QUERY:
 *
 *		DROP FUNCTION funcname (arg1, arg2, ...) [ RESTRICT | CASCADE ]
 *		DROP PROCEDURE procname (arg1, arg2, ...) [ RESTRICT | CASCADE ]
 *		DROP ROUTINE routname (arg1, arg2, ...) [ RESTRICT | CASCADE ]
 *		DROP AGGREGATE aggname (arg1, ...) [ RESTRICT | CASCADE ]
 *		DROP OPERATOR opname (leftoperand_typ, rightoperand_typ) [ RESTRICT | CASCADE ]
 *
 * 查询：DROP FUNCTION / PROCEDURE / ROUTINE / AGGREGATE / OPERATOR 语法
 *****************************************************************************/

RemoveFuncStmt:
			DROP FUNCTION function_with_argtypes_list opt_drop_behavior
				{
					DropStmt *n = makeNode(DropStmt);

					n->removeType = OBJECT_FUNCTION;
					n->objects = $3;
					n->behavior = $4;
					n->missing_ok = false;
					n->concurrent = false;
					$$ = (Node *) n;
				}
			| DROP FUNCTION IF_P EXISTS function_with_argtypes_list opt_drop_behavior
				{
					DropStmt *n = makeNode(DropStmt);

					n->removeType = OBJECT_FUNCTION;
					n->objects = $5;
					n->behavior = $6;
					n->missing_ok = true;
					n->concurrent = false;
					$$ = (Node *) n;
				}
			| DROP PROCEDURE function_with_argtypes_list opt_drop_behavior
				{
					DropStmt *n = makeNode(DropStmt);

					n->removeType = OBJECT_PROCEDURE;
					n->objects = $3;
					n->behavior = $4;
					n->missing_ok = false;
					n->concurrent = false;
					$$ = (Node *) n;
				}
			| DROP PROCEDURE IF_P EXISTS function_with_argtypes_list opt_drop_behavior
				{
					DropStmt *n = makeNode(DropStmt);

					n->removeType = OBJECT_PROCEDURE;
					n->objects = $5;
					n->behavior = $6;
					n->missing_ok = true;
					n->concurrent = false;
					$$ = (Node *) n;
				}
			| DROP ROUTINE function_with_argtypes_list opt_drop_behavior
				{
					DropStmt *n = makeNode(DropStmt);

					n->removeType = OBJECT_ROUTINE;
					n->objects = $3;
					n->behavior = $4;
					n->missing_ok = false;
					n->concurrent = false;
					$$ = (Node *) n;
				}
			| DROP ROUTINE IF_P EXISTS function_with_argtypes_list opt_drop_behavior
				{
					DropStmt *n = makeNode(DropStmt);

					n->removeType = OBJECT_ROUTINE;
					n->objects = $5;
					n->behavior = $6;
					n->missing_ok = true;
					n->concurrent = false;
					$$ = (Node *) n;
				}
		;

RemoveAggrStmt:
			DROP AGGREGATE aggregate_with_argtypes_list opt_drop_behavior
				{
					DropStmt *n = makeNode(DropStmt);

					n->removeType = OBJECT_AGGREGATE;
					n->objects = $3;
					n->behavior = $4;
					n->missing_ok = false;
					n->concurrent = false;
					$$ = (Node *) n;
				}
			| DROP AGGREGATE IF_P EXISTS aggregate_with_argtypes_list opt_drop_behavior
				{
					DropStmt *n = makeNode(DropStmt);

					n->removeType = OBJECT_AGGREGATE;
					n->objects = $5;
					n->behavior = $6;
					n->missing_ok = true;
					n->concurrent = false;
					$$ = (Node *) n;
				}
		;

RemoveOperStmt:
			DROP OPERATOR operator_with_argtypes_list opt_drop_behavior
				{
					DropStmt *n = makeNode(DropStmt);

					n->removeType = OBJECT_OPERATOR;
					n->objects = $3;
					n->behavior = $4;
					n->missing_ok = false;
					n->concurrent = false;
					$$ = (Node *) n;
				}
			| DROP OPERATOR IF_P EXISTS operator_with_argtypes_list opt_drop_behavior
				{
					DropStmt *n = makeNode(DropStmt);

					n->removeType = OBJECT_OPERATOR;
					n->objects = $5;
					n->behavior = $6;
					n->missing_ok = true;
					n->concurrent = false;
					$$ = (Node *) n;
				}
		;

oper_argtypes:
			'(' Typename ')'
				{
				   ereport(ERROR,
						   (errcode(ERRCODE_SYNTAX_ERROR),
							errmsg("missing argument"),
							errhint("Use NONE to denote the missing argument of a unary operator."),
							parser_errposition(@3)));
				}
			| '(' Typename ',' Typename ')'
					{ $$ = list_make2($2, $4); }
			| '(' NONE ',' Typename ')'					/* left unary - 左一元 */
					{ $$ = list_make2(NULL, $4); }
			| '(' Typename ',' NONE ')'					/* right unary - 右一元 */
					{ $$ = list_make2($2, NULL); }
		;

any_operator:
			all_Op
					{ $$ = list_make1(makeString($1)); }
			| ColId '.' any_operator
					{ $$ = lcons(makeString($1), $3); }
		;

operator_with_argtypes_list:
			operator_with_argtypes					{ $$ = list_make1($1); }
			| operator_with_argtypes_list ',' operator_with_argtypes
													{ $$ = lappend($1, $3); }
		;

operator_with_argtypes:
			any_operator oper_argtypes
				{
					ObjectWithArgs *n = makeNode(ObjectWithArgs);

					n->objname = $1;
					n->objargs = $2;
					$$ = n;
				}
		;

/*****************************************************************************
 *
 *		DO <anonymous code block> [ LANGUAGE language ]
 *
 * We use a DefElem list for future extensibility, and to allow flexibility
 * in the clause order.
 *
 * DO <anonymous code block> [ LANGUAGE language ] 我们使用 DefElem 列表以供未来扩展，并允许在子句顺序上具有灵活性。
 *****************************************************************************/

DoStmt: DO dostmt_opt_list
				{
					DoStmt *n = makeNode(DoStmt);

					n->args = $2;
					$$ = (Node *) n;
				}
		;

dostmt_opt_list:
			dostmt_opt_item						{ $$ = list_make1($1); }
			| dostmt_opt_list dostmt_opt_item	{ $$ = lappend($1, $2); }
		;

dostmt_opt_item:
			Sconst
				{
					$$ = makeDefElem("as", (Node *) makeString($1), @1);
				}
			| LANGUAGE NonReservedWord_or_Sconst
				{
					$$ = makeDefElem("language", (Node *) makeString($2), @1);
				}
		;

/*****************************************************************************
 *
 *		CREATE CAST / DROP CAST
 *
 *****************************************************************************/

CreateCastStmt: CREATE CAST '(' Typename AS Typename ')'
					WITH FUNCTION function_with_argtypes cast_context
				{
					CreateCastStmt *n = makeNode(CreateCastStmt);

					n->sourcetype = $4;
					n->targettype = $6;
					n->func = $10;
					n->context = (CoercionContext) $11;
					n->inout = false;
					$$ = (Node *) n;
				}
			| CREATE CAST '(' Typename AS Typename ')'
					WITHOUT FUNCTION cast_context
				{
					CreateCastStmt *n = makeNode(CreateCastStmt);

					n->sourcetype = $4;
					n->targettype = $6;
					n->func = NULL;
					n->context = (CoercionContext) $10;
					n->inout = false;
					$$ = (Node *) n;
				}
			| CREATE CAST '(' Typename AS Typename ')'
					WITH INOUT cast_context
				{
					CreateCastStmt *n = makeNode(CreateCastStmt);

					n->sourcetype = $4;
					n->targettype = $6;
					n->func = NULL;
					n->context = (CoercionContext) $10;
					n->inout = true;
					$$ = (Node *) n;
				}
		;

cast_context:  AS IMPLICIT_P					{ $$ = COERCION_IMPLICIT; }
		| AS ASSIGNMENT							{ $$ = COERCION_ASSIGNMENT; }
		| /* EMPTY - 空 */								{ $$ = COERCION_EXPLICIT; }
		;


DropCastStmt: DROP CAST opt_if_exists '(' Typename AS Typename ')' opt_drop_behavior
				{
					DropStmt *n = makeNode(DropStmt);

					n->removeType = OBJECT_CAST;
					n->objects = list_make1(list_make2($5, $7));
					n->behavior = $9;
					n->missing_ok = $3;
					n->concurrent = false;
					$$ = (Node *) n;
				}
		;

opt_if_exists: IF_P EXISTS						{ $$ = true; }
		| /* EMPTY - 空 */								{ $$ = false; }
		;


/*****************************************************************************
 *
 *		CREATE TRANSFORM / DROP TRANSFORM
 *
 *****************************************************************************/

CreateTransformStmt: CREATE opt_or_replace TRANSFORM FOR Typename LANGUAGE name '(' transform_element_list ')'
				{
					CreateTransformStmt *n = makeNode(CreateTransformStmt);

					n->replace = $2;
					n->type_name = $5;
					n->lang = $7;
					n->fromsql = linitial($9);
					n->tosql = lsecond($9);
					$$ = (Node *) n;
				}
		;

transform_element_list: FROM SQL_P WITH FUNCTION function_with_argtypes ',' TO SQL_P WITH FUNCTION function_with_argtypes
				{
					$$ = list_make2($5, $11);
				}
				| TO SQL_P WITH FUNCTION function_with_argtypes ',' FROM SQL_P WITH FUNCTION function_with_argtypes
				{
					$$ = list_make2($11, $5);
				}
				| FROM SQL_P WITH FUNCTION function_with_argtypes
				{
					$$ = list_make2($5, NULL);
				}
				| TO SQL_P WITH FUNCTION function_with_argtypes
				{
					$$ = list_make2(NULL, $5);
				}
		;


DropTransformStmt: DROP TRANSFORM opt_if_exists FOR Typename LANGUAGE name opt_drop_behavior
				{
					DropStmt *n = makeNode(DropStmt);

					n->removeType = OBJECT_TRANSFORM;
					n->objects = list_make1(list_make2($5, makeString($7)));
					n->behavior = $8;
					n->missing_ok = $3;
					$$ = (Node *) n;
				}
		;


/*****************************************************************************
 *
 *		QUERY:
 *
 *		REINDEX [ (options) ] {INDEX | TABLE | SCHEMA} [CONCURRENTLY] <name>
 *		REINDEX [ (options) ] {DATABASE | SYSTEM} [CONCURRENTLY] [<name>]
 * 查询：REINDEX 语法
 *****************************************************************************/

ReindexStmt:
			REINDEX opt_reindex_option_list reindex_target_relation opt_concurrently qualified_name
				{
					ReindexStmt *n = makeNode(ReindexStmt);

					n->kind = $3;
					n->relation = $5;
					n->name = NULL;
					n->params = $2;
					if ($4)
						n->params = lappend(n->params,
											makeDefElem("concurrently", NULL, @4));
					$$ = (Node *) n;
				}
			| REINDEX opt_reindex_option_list SCHEMA opt_concurrently name
				{
					ReindexStmt *n = makeNode(ReindexStmt);

					n->kind = REINDEX_OBJECT_SCHEMA;
					n->relation = NULL;
					n->name = $5;
					n->params = $2;
					if ($4)
						n->params = lappend(n->params,
											makeDefElem("concurrently", NULL, @4));
					$$ = (Node *) n;
				}
			| REINDEX opt_reindex_option_list reindex_target_all opt_concurrently opt_single_name
				{
					ReindexStmt *n = makeNode(ReindexStmt);

					n->kind = $3;
					n->relation = NULL;
					n->name = $5;
					n->params = $2;
					if ($4)
						n->params = lappend(n->params,
											makeDefElem("concurrently", NULL, @4));
					$$ = (Node *) n;
				}
		;
reindex_target_relation:
			INDEX					{ $$ = REINDEX_OBJECT_INDEX; }
			| TABLE					{ $$ = REINDEX_OBJECT_TABLE; }
		;
reindex_target_all:
			SYSTEM_P				{ $$ = REINDEX_OBJECT_SYSTEM; }
			| DATABASE				{ $$ = REINDEX_OBJECT_DATABASE; }
		;
opt_reindex_option_list:
			'(' utility_option_list ')'				{ $$ = $2; }
			| /* EMPTY - 空 */							{ $$ = NULL; }
		;

/*****************************************************************************
 *
 * ALTER TABLESPACE
 *
 *****************************************************************************/

AlterTblSpcStmt:
			ALTER TABLESPACE name SET reloptions
				{
					AlterTableSpaceOptionsStmt *n =
						makeNode(AlterTableSpaceOptionsStmt);

					n->tablespacename = $3;
					n->options = $5;
					n->isReset = false;
					$$ = (Node *) n;
				}
			| ALTER TABLESPACE name RESET reloptions
				{
					AlterTableSpaceOptionsStmt *n =
						makeNode(AlterTableSpaceOptionsStmt);

					n->tablespacename = $3;
					n->options = $5;
					n->isReset = true;
					$$ = (Node *) n;
				}
		;

/*****************************************************************************
 *
 * ALTER THING name RENAME TO newname
 *
 *****************************************************************************/

RenameStmt: ALTER AGGREGATE aggregate_with_argtypes RENAME TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_AGGREGATE;
					n->object = (Node *) $3;
					n->newname = $6;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER COLLATION any_name RENAME TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_COLLATION;
					n->object = (Node *) $3;
					n->newname = $6;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER CONVERSION_P any_name RENAME TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_CONVERSION;
					n->object = (Node *) $3;
					n->newname = $6;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER DATABASE name RENAME TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_DATABASE;
					n->subname = $3;
					n->newname = $6;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER DOMAIN_P any_name RENAME TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_DOMAIN;
					n->object = (Node *) $3;
					n->newname = $6;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER DOMAIN_P any_name RENAME CONSTRAINT name TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_DOMCONSTRAINT;
					n->object = (Node *) $3;
					n->subname = $6;
					n->newname = $8;
					$$ = (Node *) n;
				}
			| ALTER FOREIGN DATA_P WRAPPER name RENAME TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_FDW;
					n->object = (Node *) makeString($5);
					n->newname = $8;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER FUNCTION function_with_argtypes RENAME TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_FUNCTION;
					n->object = (Node *) $3;
					n->newname = $6;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER GROUP_P RoleId RENAME TO RoleId
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_ROLE;
					n->subname = $3;
					n->newname = $6;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER opt_procedural LANGUAGE name RENAME TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_LANGUAGE;
					n->object = (Node *) makeString($4);
					n->newname = $7;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER OPERATOR CLASS any_name USING name RENAME TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_OPCLASS;
					n->object = (Node *) lcons(makeString($6), $4);
					n->newname = $9;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER OPERATOR FAMILY any_name USING name RENAME TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_OPFAMILY;
					n->object = (Node *) lcons(makeString($6), $4);
					n->newname = $9;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER POLICY name ON qualified_name RENAME TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_POLICY;
					n->relation = $5;
					n->subname = $3;
					n->newname = $8;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER POLICY IF_P EXISTS name ON qualified_name RENAME TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_POLICY;
					n->relation = $7;
					n->subname = $5;
					n->newname = $10;
					n->missing_ok = true;
					$$ = (Node *) n;
				}
			| ALTER PROCEDURE function_with_argtypes RENAME TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_PROCEDURE;
					n->object = (Node *) $3;
					n->newname = $6;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER PUBLICATION name RENAME TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_PUBLICATION;
					n->object = (Node *) makeString($3);
					n->newname = $6;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER ROUTINE function_with_argtypes RENAME TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_ROUTINE;
					n->object = (Node *) $3;
					n->newname = $6;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER SCHEMA name RENAME TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_SCHEMA;
					n->subname = $3;
					n->newname = $6;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER SERVER name RENAME TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_FOREIGN_SERVER;
					n->object = (Node *) makeString($3);
					n->newname = $6;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER SUBSCRIPTION name RENAME TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_SUBSCRIPTION;
					n->object = (Node *) makeString($3);
					n->newname = $6;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER TABLE relation_expr RENAME TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_TABLE;
					n->relation = $3;
					n->subname = NULL;
					n->newname = $6;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER TABLE IF_P EXISTS relation_expr RENAME TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_TABLE;
					n->relation = $5;
					n->subname = NULL;
					n->newname = $8;
					n->missing_ok = true;
					$$ = (Node *) n;
				}
			| ALTER SEQUENCE qualified_name RENAME TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_SEQUENCE;
					n->relation = $3;
					n->subname = NULL;
					n->newname = $6;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER SEQUENCE IF_P EXISTS qualified_name RENAME TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_SEQUENCE;
					n->relation = $5;
					n->subname = NULL;
					n->newname = $8;
					n->missing_ok = true;
					$$ = (Node *) n;
				}
			| ALTER VIEW qualified_name RENAME TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_VIEW;
					n->relation = $3;
					n->subname = NULL;
					n->newname = $6;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER VIEW IF_P EXISTS qualified_name RENAME TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_VIEW;
					n->relation = $5;
					n->subname = NULL;
					n->newname = $8;
					n->missing_ok = true;
					$$ = (Node *) n;
				}
			| ALTER MATERIALIZED VIEW qualified_name RENAME TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_MATVIEW;
					n->relation = $4;
					n->subname = NULL;
					n->newname = $7;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER MATERIALIZED VIEW IF_P EXISTS qualified_name RENAME TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_MATVIEW;
					n->relation = $6;
					n->subname = NULL;
					n->newname = $9;
					n->missing_ok = true;
					$$ = (Node *) n;
				}
			| ALTER INDEX qualified_name RENAME TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_INDEX;
					n->relation = $3;
					n->subname = NULL;
					n->newname = $6;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER INDEX IF_P EXISTS qualified_name RENAME TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_INDEX;
					n->relation = $5;
					n->subname = NULL;
					n->newname = $8;
					n->missing_ok = true;
					$$ = (Node *) n;
				}
			| ALTER FOREIGN TABLE relation_expr RENAME TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_FOREIGN_TABLE;
					n->relation = $4;
					n->subname = NULL;
					n->newname = $7;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER FOREIGN TABLE IF_P EXISTS relation_expr RENAME TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_FOREIGN_TABLE;
					n->relation = $6;
					n->subname = NULL;
					n->newname = $9;
					n->missing_ok = true;
					$$ = (Node *) n;
				}
			| ALTER TABLE relation_expr RENAME opt_column name TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_COLUMN;
					n->relationType = OBJECT_TABLE;
					n->relation = $3;
					n->subname = $6;
					n->newname = $8;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER TABLE IF_P EXISTS relation_expr RENAME opt_column name TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_COLUMN;
					n->relationType = OBJECT_TABLE;
					n->relation = $5;
					n->subname = $8;
					n->newname = $10;
					n->missing_ok = true;
					$$ = (Node *) n;
				}
			| ALTER VIEW qualified_name RENAME opt_column name TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_COLUMN;
					n->relationType = OBJECT_VIEW;
					n->relation = $3;
					n->subname = $6;
					n->newname = $8;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER VIEW IF_P EXISTS qualified_name RENAME opt_column name TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_COLUMN;
					n->relationType = OBJECT_VIEW;
					n->relation = $5;
					n->subname = $8;
					n->newname = $10;
					n->missing_ok = true;
					$$ = (Node *) n;
				}
			| ALTER MATERIALIZED VIEW qualified_name RENAME opt_column name TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_COLUMN;
					n->relationType = OBJECT_MATVIEW;
					n->relation = $4;
					n->subname = $7;
					n->newname = $9;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER MATERIALIZED VIEW IF_P EXISTS qualified_name RENAME opt_column name TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_COLUMN;
					n->relationType = OBJECT_MATVIEW;
					n->relation = $6;
					n->subname = $9;
					n->newname = $11;
					n->missing_ok = true;
					$$ = (Node *) n;
				}
			| ALTER TABLE relation_expr RENAME CONSTRAINT name TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_TABCONSTRAINT;
					n->relation = $3;
					n->subname = $6;
					n->newname = $8;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER TABLE IF_P EXISTS relation_expr RENAME CONSTRAINT name TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_TABCONSTRAINT;
					n->relation = $5;
					n->subname = $8;
					n->newname = $10;
					n->missing_ok = true;
					$$ = (Node *) n;
				}
			| ALTER FOREIGN TABLE relation_expr RENAME opt_column name TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_COLUMN;
					n->relationType = OBJECT_FOREIGN_TABLE;
					n->relation = $4;
					n->subname = $7;
					n->newname = $9;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER FOREIGN TABLE IF_P EXISTS relation_expr RENAME opt_column name TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_COLUMN;
					n->relationType = OBJECT_FOREIGN_TABLE;
					n->relation = $6;
					n->subname = $9;
					n->newname = $11;
					n->missing_ok = true;
					$$ = (Node *) n;
				}
			| ALTER RULE name ON qualified_name RENAME TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_RULE;
					n->relation = $5;
					n->subname = $3;
					n->newname = $8;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER TRIGGER name ON qualified_name RENAME TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_TRIGGER;
					n->relation = $5;
					n->subname = $3;
					n->newname = $8;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER EVENT TRIGGER name RENAME TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_EVENT_TRIGGER;
					n->object = (Node *) makeString($4);
					n->newname = $7;
					$$ = (Node *) n;
				}
			| ALTER ROLE RoleId RENAME TO RoleId
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_ROLE;
					n->subname = $3;
					n->newname = $6;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER USER RoleId RENAME TO RoleId
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_ROLE;
					n->subname = $3;
					n->newname = $6;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER TABLESPACE name RENAME TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_TABLESPACE;
					n->subname = $3;
					n->newname = $6;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER STATISTICS any_name RENAME TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_STATISTIC_EXT;
					n->object = (Node *) $3;
					n->newname = $6;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER TEXT_P SEARCH PARSER any_name RENAME TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_TSPARSER;
					n->object = (Node *) $5;
					n->newname = $8;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER TEXT_P SEARCH DICTIONARY any_name RENAME TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_TSDICTIONARY;
					n->object = (Node *) $5;
					n->newname = $8;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER TEXT_P SEARCH TEMPLATE any_name RENAME TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_TSTEMPLATE;
					n->object = (Node *) $5;
					n->newname = $8;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER TEXT_P SEARCH CONFIGURATION any_name RENAME TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_TSCONFIGURATION;
					n->object = (Node *) $5;
					n->newname = $8;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER TYPE_P any_name RENAME TO name
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_TYPE;
					n->object = (Node *) $3;
					n->newname = $6;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER TYPE_P any_name RENAME ATTRIBUTE name TO name opt_drop_behavior
				{
					RenameStmt *n = makeNode(RenameStmt);

					n->renameType = OBJECT_ATTRIBUTE;
					n->relationType = OBJECT_TYPE;
					n->relation = makeRangeVarFromAnyName($3, @3, yyscanner);
					n->subname = $6;
					n->newname = $8;
					n->behavior = $9;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
		;

opt_column: COLUMN
			| /* EMPTY - 空 */
		;

opt_set_data: SET DATA_P							{ $$ = 1; }
			| /* EMPTY - 空 */								{ $$ = 0; }
		;

/*****************************************************************************
 *
 * ALTER THING name DEPENDS ON EXTENSION name
 *
 *****************************************************************************/

AlterObjectDependsStmt:
			ALTER FUNCTION function_with_argtypes opt_no DEPENDS ON EXTENSION name
				{
					AlterObjectDependsStmt *n = makeNode(AlterObjectDependsStmt);

					n->objectType = OBJECT_FUNCTION;
					n->object = (Node *) $3;
					n->extname = makeString($8);
					n->remove = $4;
					$$ = (Node *) n;
				}
			| ALTER PROCEDURE function_with_argtypes opt_no DEPENDS ON EXTENSION name
				{
					AlterObjectDependsStmt *n = makeNode(AlterObjectDependsStmt);

					n->objectType = OBJECT_PROCEDURE;
					n->object = (Node *) $3;
					n->extname = makeString($8);
					n->remove = $4;
					$$ = (Node *) n;
				}
			| ALTER ROUTINE function_with_argtypes opt_no DEPENDS ON EXTENSION name
				{
					AlterObjectDependsStmt *n = makeNode(AlterObjectDependsStmt);

					n->objectType = OBJECT_ROUTINE;
					n->object = (Node *) $3;
					n->extname = makeString($8);
					n->remove = $4;
					$$ = (Node *) n;
				}
			| ALTER TRIGGER name ON qualified_name opt_no DEPENDS ON EXTENSION name
				{
					AlterObjectDependsStmt *n = makeNode(AlterObjectDependsStmt);

					n->objectType = OBJECT_TRIGGER;
					n->relation = $5;
					n->object = (Node *) list_make1(makeString($3));
					n->extname = makeString($10);
					n->remove = $6;
					$$ = (Node *) n;
				}
			| ALTER MATERIALIZED VIEW qualified_name opt_no DEPENDS ON EXTENSION name
				{
					AlterObjectDependsStmt *n = makeNode(AlterObjectDependsStmt);

					n->objectType = OBJECT_MATVIEW;
					n->relation = $4;
					n->extname = makeString($9);
					n->remove = $5;
					$$ = (Node *) n;
				}
			| ALTER INDEX qualified_name opt_no DEPENDS ON EXTENSION name
				{
					AlterObjectDependsStmt *n = makeNode(AlterObjectDependsStmt);

					n->objectType = OBJECT_INDEX;
					n->relation = $3;
					n->extname = makeString($8);
					n->remove = $4;
					$$ = (Node *) n;
				}
		;

opt_no:		NO				{ $$ = true; }
			| /* EMPTY - 空 */	{ $$ = false;	}
		;

/*****************************************************************************
 *
 * ALTER THING name SET SCHEMA name
 *
 *****************************************************************************/

AlterObjectSchemaStmt:
			ALTER AGGREGATE aggregate_with_argtypes SET SCHEMA name
				{
					AlterObjectSchemaStmt *n = makeNode(AlterObjectSchemaStmt);

					n->objectType = OBJECT_AGGREGATE;
					n->object = (Node *) $3;
					n->newschema = $6;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER COLLATION any_name SET SCHEMA name
				{
					AlterObjectSchemaStmt *n = makeNode(AlterObjectSchemaStmt);

					n->objectType = OBJECT_COLLATION;
					n->object = (Node *) $3;
					n->newschema = $6;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER CONVERSION_P any_name SET SCHEMA name
				{
					AlterObjectSchemaStmt *n = makeNode(AlterObjectSchemaStmt);

					n->objectType = OBJECT_CONVERSION;
					n->object = (Node *) $3;
					n->newschema = $6;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER DOMAIN_P any_name SET SCHEMA name
				{
					AlterObjectSchemaStmt *n = makeNode(AlterObjectSchemaStmt);

					n->objectType = OBJECT_DOMAIN;
					n->object = (Node *) $3;
					n->newschema = $6;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER EXTENSION name SET SCHEMA name
				{
					AlterObjectSchemaStmt *n = makeNode(AlterObjectSchemaStmt);

					n->objectType = OBJECT_EXTENSION;
					n->object = (Node *) makeString($3);
					n->newschema = $6;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER FUNCTION function_with_argtypes SET SCHEMA name
				{
					AlterObjectSchemaStmt *n = makeNode(AlterObjectSchemaStmt);

					n->objectType = OBJECT_FUNCTION;
					n->object = (Node *) $3;
					n->newschema = $6;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER OPERATOR operator_with_argtypes SET SCHEMA name
				{
					AlterObjectSchemaStmt *n = makeNode(AlterObjectSchemaStmt);

					n->objectType = OBJECT_OPERATOR;
					n->object = (Node *) $3;
					n->newschema = $6;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER OPERATOR CLASS any_name USING name SET SCHEMA name
				{
					AlterObjectSchemaStmt *n = makeNode(AlterObjectSchemaStmt);

					n->objectType = OBJECT_OPCLASS;
					n->object = (Node *) lcons(makeString($6), $4);
					n->newschema = $9;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER OPERATOR FAMILY any_name USING name SET SCHEMA name
				{
					AlterObjectSchemaStmt *n = makeNode(AlterObjectSchemaStmt);

					n->objectType = OBJECT_OPFAMILY;
					n->object = (Node *) lcons(makeString($6), $4);
					n->newschema = $9;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER PROCEDURE function_with_argtypes SET SCHEMA name
				{
					AlterObjectSchemaStmt *n = makeNode(AlterObjectSchemaStmt);

					n->objectType = OBJECT_PROCEDURE;
					n->object = (Node *) $3;
					n->newschema = $6;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER ROUTINE function_with_argtypes SET SCHEMA name
				{
					AlterObjectSchemaStmt *n = makeNode(AlterObjectSchemaStmt);

					n->objectType = OBJECT_ROUTINE;
					n->object = (Node *) $3;
					n->newschema = $6;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER TABLE relation_expr SET SCHEMA name
				{
					AlterObjectSchemaStmt *n = makeNode(AlterObjectSchemaStmt);

					n->objectType = OBJECT_TABLE;
					n->relation = $3;
					n->newschema = $6;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER TABLE IF_P EXISTS relation_expr SET SCHEMA name
				{
					AlterObjectSchemaStmt *n = makeNode(AlterObjectSchemaStmt);

					n->objectType = OBJECT_TABLE;
					n->relation = $5;
					n->newschema = $8;
					n->missing_ok = true;
					$$ = (Node *) n;
				}
			| ALTER STATISTICS any_name SET SCHEMA name
				{
					AlterObjectSchemaStmt *n = makeNode(AlterObjectSchemaStmt);

					n->objectType = OBJECT_STATISTIC_EXT;
					n->object = (Node *) $3;
					n->newschema = $6;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER TEXT_P SEARCH PARSER any_name SET SCHEMA name
				{
					AlterObjectSchemaStmt *n = makeNode(AlterObjectSchemaStmt);

					n->objectType = OBJECT_TSPARSER;
					n->object = (Node *) $5;
					n->newschema = $8;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER TEXT_P SEARCH DICTIONARY any_name SET SCHEMA name
				{
					AlterObjectSchemaStmt *n = makeNode(AlterObjectSchemaStmt);

					n->objectType = OBJECT_TSDICTIONARY;
					n->object = (Node *) $5;
					n->newschema = $8;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER TEXT_P SEARCH TEMPLATE any_name SET SCHEMA name
				{
					AlterObjectSchemaStmt *n = makeNode(AlterObjectSchemaStmt);

					n->objectType = OBJECT_TSTEMPLATE;
					n->object = (Node *) $5;
					n->newschema = $8;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER TEXT_P SEARCH CONFIGURATION any_name SET SCHEMA name
				{
					AlterObjectSchemaStmt *n = makeNode(AlterObjectSchemaStmt);

					n->objectType = OBJECT_TSCONFIGURATION;
					n->object = (Node *) $5;
					n->newschema = $8;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER SEQUENCE qualified_name SET SCHEMA name
				{
					AlterObjectSchemaStmt *n = makeNode(AlterObjectSchemaStmt);

					n->objectType = OBJECT_SEQUENCE;
					n->relation = $3;
					n->newschema = $6;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER SEQUENCE IF_P EXISTS qualified_name SET SCHEMA name
				{
					AlterObjectSchemaStmt *n = makeNode(AlterObjectSchemaStmt);

					n->objectType = OBJECT_SEQUENCE;
					n->relation = $5;
					n->newschema = $8;
					n->missing_ok = true;
					$$ = (Node *) n;
				}
			| ALTER VIEW qualified_name SET SCHEMA name
				{
					AlterObjectSchemaStmt *n = makeNode(AlterObjectSchemaStmt);

					n->objectType = OBJECT_VIEW;
					n->relation = $3;
					n->newschema = $6;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER VIEW IF_P EXISTS qualified_name SET SCHEMA name
				{
					AlterObjectSchemaStmt *n = makeNode(AlterObjectSchemaStmt);

					n->objectType = OBJECT_VIEW;
					n->relation = $5;
					n->newschema = $8;
					n->missing_ok = true;
					$$ = (Node *) n;
				}
			| ALTER MATERIALIZED VIEW qualified_name SET SCHEMA name
				{
					AlterObjectSchemaStmt *n = makeNode(AlterObjectSchemaStmt);

					n->objectType = OBJECT_MATVIEW;
					n->relation = $4;
					n->newschema = $7;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER MATERIALIZED VIEW IF_P EXISTS qualified_name SET SCHEMA name
				{
					AlterObjectSchemaStmt *n = makeNode(AlterObjectSchemaStmt);

					n->objectType = OBJECT_MATVIEW;
					n->relation = $6;
					n->newschema = $9;
					n->missing_ok = true;
					$$ = (Node *) n;
				}
			| ALTER FOREIGN TABLE relation_expr SET SCHEMA name
				{
					AlterObjectSchemaStmt *n = makeNode(AlterObjectSchemaStmt);

					n->objectType = OBJECT_FOREIGN_TABLE;
					n->relation = $4;
					n->newschema = $7;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER FOREIGN TABLE IF_P EXISTS relation_expr SET SCHEMA name
				{
					AlterObjectSchemaStmt *n = makeNode(AlterObjectSchemaStmt);

					n->objectType = OBJECT_FOREIGN_TABLE;
					n->relation = $6;
					n->newschema = $9;
					n->missing_ok = true;
					$$ = (Node *) n;
				}
			| ALTER TYPE_P any_name SET SCHEMA name
				{
					AlterObjectSchemaStmt *n = makeNode(AlterObjectSchemaStmt);

					n->objectType = OBJECT_TYPE;
					n->object = (Node *) $3;
					n->newschema = $6;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
		;

/*****************************************************************************
 *
 * ALTER OPERATOR name SET define
 *
 *****************************************************************************/

AlterOperatorStmt:
			ALTER OPERATOR operator_with_argtypes SET '(' operator_def_list ')'
				{
					AlterOperatorStmt *n = makeNode(AlterOperatorStmt);

					n->opername = $3;
					n->options = $6;
					$$ = (Node *) n;
				}
		;

operator_def_list:	operator_def_elem								{ $$ = list_make1($1); }
			| operator_def_list ',' operator_def_elem				{ $$ = lappend($1, $3); }
		;

operator_def_elem: ColLabel '=' NONE
						{ $$ = makeDefElem($1, NULL, @1); }
				   | ColLabel '=' operator_def_arg
						{ $$ = makeDefElem($1, (Node *) $3, @1); }
				   | ColLabel
						{ $$ = makeDefElem($1, NULL, @1); }
		;

/* must be similar enough to def_arg to avoid reduce/reduce conflicts - 必须与 def_arg 足够相似，以避免规约/规约冲突 */
operator_def_arg:
			func_type						{ $$ = (Node *) $1; }
			| reserved_keyword				{ $$ = (Node *) makeString(pstrdup($1)); }
			| qual_all_Op					{ $$ = (Node *) $1; }
			| NumericOnly					{ $$ = (Node *) $1; }
			| Sconst						{ $$ = (Node *) makeString($1); }
		;

/*****************************************************************************
 *
 * ALTER TYPE name SET define
 *
 * We repurpose ALTER OPERATOR's version of "definition" here
 *
 *****************************************************************************/

AlterTypeStmt:
			ALTER TYPE_P any_name SET '(' operator_def_list ')'
				{
					AlterTypeStmt *n = makeNode(AlterTypeStmt);

					n->typeName = $3;
					n->options = $6;
					$$ = (Node *) n;
				}
		;

/*****************************************************************************
 *
 * ALTER THING name OWNER TO newname
 *
 *****************************************************************************/

AlterOwnerStmt: ALTER AGGREGATE aggregate_with_argtypes OWNER TO RoleSpec
				{
					AlterOwnerStmt *n = makeNode(AlterOwnerStmt);

					n->objectType = OBJECT_AGGREGATE;
					n->object = (Node *) $3;
					n->newowner = $6;
					$$ = (Node *) n;
				}
			| ALTER COLLATION any_name OWNER TO RoleSpec
				{
					AlterOwnerStmt *n = makeNode(AlterOwnerStmt);

					n->objectType = OBJECT_COLLATION;
					n->object = (Node *) $3;
					n->newowner = $6;
					$$ = (Node *) n;
				}
			| ALTER CONVERSION_P any_name OWNER TO RoleSpec
				{
					AlterOwnerStmt *n = makeNode(AlterOwnerStmt);

					n->objectType = OBJECT_CONVERSION;
					n->object = (Node *) $3;
					n->newowner = $6;
					$$ = (Node *) n;
				}
			| ALTER DATABASE name OWNER TO RoleSpec
				{
					AlterOwnerStmt *n = makeNode(AlterOwnerStmt);

					n->objectType = OBJECT_DATABASE;
					n->object = (Node *) makeString($3);
					n->newowner = $6;
					$$ = (Node *) n;
				}
			| ALTER DOMAIN_P any_name OWNER TO RoleSpec
				{
					AlterOwnerStmt *n = makeNode(AlterOwnerStmt);

					n->objectType = OBJECT_DOMAIN;
					n->object = (Node *) $3;
					n->newowner = $6;
					$$ = (Node *) n;
				}
			| ALTER FUNCTION function_with_argtypes OWNER TO RoleSpec
				{
					AlterOwnerStmt *n = makeNode(AlterOwnerStmt);

					n->objectType = OBJECT_FUNCTION;
					n->object = (Node *) $3;
					n->newowner = $6;
					$$ = (Node *) n;
				}
			| ALTER opt_procedural LANGUAGE name OWNER TO RoleSpec
				{
					AlterOwnerStmt *n = makeNode(AlterOwnerStmt);

					n->objectType = OBJECT_LANGUAGE;
					n->object = (Node *) makeString($4);
					n->newowner = $7;
					$$ = (Node *) n;
				}
			| ALTER LARGE_P OBJECT_P NumericOnly OWNER TO RoleSpec
				{
					AlterOwnerStmt *n = makeNode(AlterOwnerStmt);

					n->objectType = OBJECT_LARGEOBJECT;
					n->object = (Node *) $4;
					n->newowner = $7;
					$$ = (Node *) n;
				}
			| ALTER OPERATOR operator_with_argtypes OWNER TO RoleSpec
				{
					AlterOwnerStmt *n = makeNode(AlterOwnerStmt);

					n->objectType = OBJECT_OPERATOR;
					n->object = (Node *) $3;
					n->newowner = $6;
					$$ = (Node *) n;
				}
			| ALTER OPERATOR CLASS any_name USING name OWNER TO RoleSpec
				{
					AlterOwnerStmt *n = makeNode(AlterOwnerStmt);

					n->objectType = OBJECT_OPCLASS;
					n->object = (Node *) lcons(makeString($6), $4);
					n->newowner = $9;
					$$ = (Node *) n;
				}
			| ALTER OPERATOR FAMILY any_name USING name OWNER TO RoleSpec
				{
					AlterOwnerStmt *n = makeNode(AlterOwnerStmt);

					n->objectType = OBJECT_OPFAMILY;
					n->object = (Node *) lcons(makeString($6), $4);
					n->newowner = $9;
					$$ = (Node *) n;
				}
			| ALTER PROCEDURE function_with_argtypes OWNER TO RoleSpec
				{
					AlterOwnerStmt *n = makeNode(AlterOwnerStmt);

					n->objectType = OBJECT_PROCEDURE;
					n->object = (Node *) $3;
					n->newowner = $6;
					$$ = (Node *) n;
				}
			| ALTER ROUTINE function_with_argtypes OWNER TO RoleSpec
				{
					AlterOwnerStmt *n = makeNode(AlterOwnerStmt);

					n->objectType = OBJECT_ROUTINE;
					n->object = (Node *) $3;
					n->newowner = $6;
					$$ = (Node *) n;
				}
			| ALTER SCHEMA name OWNER TO RoleSpec
				{
					AlterOwnerStmt *n = makeNode(AlterOwnerStmt);

					n->objectType = OBJECT_SCHEMA;
					n->object = (Node *) makeString($3);
					n->newowner = $6;
					$$ = (Node *) n;
				}
			| ALTER TYPE_P any_name OWNER TO RoleSpec
				{
					AlterOwnerStmt *n = makeNode(AlterOwnerStmt);

					n->objectType = OBJECT_TYPE;
					n->object = (Node *) $3;
					n->newowner = $6;
					$$ = (Node *) n;
				}
			| ALTER TABLESPACE name OWNER TO RoleSpec
				{
					AlterOwnerStmt *n = makeNode(AlterOwnerStmt);

					n->objectType = OBJECT_TABLESPACE;
					n->object = (Node *) makeString($3);
					n->newowner = $6;
					$$ = (Node *) n;
				}
			| ALTER STATISTICS any_name OWNER TO RoleSpec
				{
					AlterOwnerStmt *n = makeNode(AlterOwnerStmt);

					n->objectType = OBJECT_STATISTIC_EXT;
					n->object = (Node *) $3;
					n->newowner = $6;
					$$ = (Node *) n;
				}
			| ALTER TEXT_P SEARCH DICTIONARY any_name OWNER TO RoleSpec
				{
					AlterOwnerStmt *n = makeNode(AlterOwnerStmt);

					n->objectType = OBJECT_TSDICTIONARY;
					n->object = (Node *) $5;
					n->newowner = $8;
					$$ = (Node *) n;
				}
			| ALTER TEXT_P SEARCH CONFIGURATION any_name OWNER TO RoleSpec
				{
					AlterOwnerStmt *n = makeNode(AlterOwnerStmt);

					n->objectType = OBJECT_TSCONFIGURATION;
					n->object = (Node *) $5;
					n->newowner = $8;
					$$ = (Node *) n;
				}
			| ALTER FOREIGN DATA_P WRAPPER name OWNER TO RoleSpec
				{
					AlterOwnerStmt *n = makeNode(AlterOwnerStmt);

					n->objectType = OBJECT_FDW;
					n->object = (Node *) makeString($5);
					n->newowner = $8;
					$$ = (Node *) n;
				}
			| ALTER SERVER name OWNER TO RoleSpec
				{
					AlterOwnerStmt *n = makeNode(AlterOwnerStmt);

					n->objectType = OBJECT_FOREIGN_SERVER;
					n->object = (Node *) makeString($3);
					n->newowner = $6;
					$$ = (Node *) n;
				}
			| ALTER EVENT TRIGGER name OWNER TO RoleSpec
				{
					AlterOwnerStmt *n = makeNode(AlterOwnerStmt);

					n->objectType = OBJECT_EVENT_TRIGGER;
					n->object = (Node *) makeString($4);
					n->newowner = $7;
					$$ = (Node *) n;
				}
			| ALTER PUBLICATION name OWNER TO RoleSpec
				{
					AlterOwnerStmt *n = makeNode(AlterOwnerStmt);

					n->objectType = OBJECT_PUBLICATION;
					n->object = (Node *) makeString($3);
					n->newowner = $6;
					$$ = (Node *) n;
				}
			| ALTER SUBSCRIPTION name OWNER TO RoleSpec
				{
					AlterOwnerStmt *n = makeNode(AlterOwnerStmt);

					n->objectType = OBJECT_SUBSCRIPTION;
					n->object = (Node *) makeString($3);
					n->newowner = $6;
					$$ = (Node *) n;
				}
		;


/*****************************************************************************
 *
 * CREATE PUBLICATION name [WITH options]
 *
 * CREATE PUBLICATION FOR ALL TABLES [WITH options]
 *
 * CREATE PUBLICATION FOR pub_obj [, ...] [WITH options]
 *
 * pub_obj is one of:
 *
 *		TABLE table [, ...]
 *		TABLES IN SCHEMA schema [, ...]
 *
 * 查询：CREATE PUBLICATION 语法
 *****************************************************************************/

CreatePublicationStmt:
			CREATE PUBLICATION name opt_definition
				{
					CreatePublicationStmt *n = makeNode(CreatePublicationStmt);

					n->pubname = $3;
					n->options = $4;
					$$ = (Node *) n;
				}
			| CREATE PUBLICATION name FOR ALL TABLES opt_definition
				{
					CreatePublicationStmt *n = makeNode(CreatePublicationStmt);

					n->pubname = $3;
					n->options = $7;
					n->for_all_tables = true;
					$$ = (Node *) n;
				}
			| CREATE PUBLICATION name FOR pub_obj_list opt_definition
				{
					CreatePublicationStmt *n = makeNode(CreatePublicationStmt);

					n->pubname = $3;
					n->options = $6;
					n->pubobjects = (List *) $5;
					preprocess_pubobj_list(n->pubobjects, yyscanner);
					$$ = (Node *) n;
				}
		;

/*
 * FOR TABLE and FOR TABLES IN SCHEMA specifications
 *
 * This rule parses publication objects with and without keyword prefixes.
 *
 * The actual type of the object without keyword prefix depends on the previous
 * one with keyword prefix. It will be preprocessed in preprocess_pubobj_list().
 *
 * For the object without keyword prefix, we cannot just use relation_expr here,
 * because some extended expressions in relation_expr cannot be used as a
 * schemaname and we cannot differentiate it. So, we extract the rules from
 * relation_expr here.
 * FOR TABLE 和 FOR TABLES IN SCHEMA 规范。此规则解析带有和不带有关键字前缀的发布对象。不带有关键字前缀的对象的实际类型取决于前一个带有关键字前缀的对象。它将在 preprocess_pubobj_list() 中进行预处理。对于没有关键字前缀的对象，我们不能在这里直接使用 relation_expr，因为 relation_expr 中的某些扩展表达式不能用作模式名称（schemaname），并且我们无法对其进行区分。因此，我们在此处从 relation_expr 中提取规则。
 */
PublicationObjSpec:
			TABLE relation_expr opt_column_list OptWhereClause
				{
					$$ = makeNode(PublicationObjSpec);
					$$->pubobjtype = PUBLICATIONOBJ_TABLE;
					$$->pubtable = makeNode(PublicationTable);
					$$->pubtable->relation = $2;
					$$->pubtable->columns = $3;
					$$->pubtable->whereClause = $4;
				}
			| TABLES IN_P SCHEMA ColId
				{
					$$ = makeNode(PublicationObjSpec);
					$$->pubobjtype = PUBLICATIONOBJ_TABLES_IN_SCHEMA;
					$$->name = $4;
					$$->location = @4;
				}
			| TABLES IN_P SCHEMA CURRENT_SCHEMA
				{
					$$ = makeNode(PublicationObjSpec);
					$$->pubobjtype = PUBLICATIONOBJ_TABLES_IN_CUR_SCHEMA;
					$$->location = @4;
				}
			| ColId opt_column_list OptWhereClause
				{
					$$ = makeNode(PublicationObjSpec);
					$$->pubobjtype = PUBLICATIONOBJ_CONTINUATION;
					/*
					 * If either a row filter or column list is specified, create
					 * a PublicationTable object.
					 * 如果指定了行过滤器或列列表，则创建一个 PublicationTable 对象。
					 */
					if ($2 || $3)
					{
						/*
						 * The OptWhereClause must be stored here but it is
						 * valid only for tables. For non-table objects, an
						 * error will be thrown later via
						 * preprocess_pubobj_list().
						 * OptWhereClause 必须存储在此处，但它仅对表有效。对于 non-table 对象，稍后将通过 preprocess_pubobj_list() 抛出错误。
						 */
						$$->pubtable = makeNode(PublicationTable);
						$$->pubtable->relation = makeRangeVar(NULL, $1, @1);
						$$->pubtable->columns = $2;
						$$->pubtable->whereClause = $3;
					}
					else
					{
						$$->name = $1;
					}
					$$->location = @1;
				}
			| ColId indirection opt_column_list OptWhereClause
				{
					$$ = makeNode(PublicationObjSpec);
					$$->pubobjtype = PUBLICATIONOBJ_CONTINUATION;
					$$->pubtable = makeNode(PublicationTable);
					$$->pubtable->relation = makeRangeVarFromQualifiedName($1, $2, @1, yyscanner);
					$$->pubtable->columns = $3;
					$$->pubtable->whereClause = $4;
					$$->location = @1;
				}
			/* grammar like tablename * , ONLY tablename, ONLY ( tablename ) - 类似于 tablename *，ONLY tablename，ONLY ( tablename ) 的语法 */
			| extended_relation_expr opt_column_list OptWhereClause
				{
					$$ = makeNode(PublicationObjSpec);
					$$->pubobjtype = PUBLICATIONOBJ_CONTINUATION;
					$$->pubtable = makeNode(PublicationTable);
					$$->pubtable->relation = $1;
					$$->pubtable->columns = $2;
					$$->pubtable->whereClause = $3;
				}
			| CURRENT_SCHEMA
				{
					$$ = makeNode(PublicationObjSpec);
					$$->pubobjtype = PUBLICATIONOBJ_CONTINUATION;
					$$->location = @1;
				}
				;

pub_obj_list:	PublicationObjSpec
					{ $$ = list_make1($1); }
			| pub_obj_list ',' PublicationObjSpec
					{ $$ = lappend($1, $3); }
	;

/*****************************************************************************
 *
 * ALTER PUBLICATION name SET ( options )
 *
 * ALTER PUBLICATION name ADD pub_obj [, ...]
 *
 * ALTER PUBLICATION name DROP pub_obj [, ...]
 *
 * ALTER PUBLICATION name SET pub_obj [, ...]
 *
 * pub_obj is one of:
 *
 *		TABLE table_name [, ...]
 *		TABLES IN SCHEMA schema_name [, ...]
 *
 * 查询：ALTER PUBLICATION 语法
 *****************************************************************************/

AlterPublicationStmt:
			ALTER PUBLICATION name SET definition
				{
					AlterPublicationStmt *n = makeNode(AlterPublicationStmt);

					n->pubname = $3;
					n->options = $5;
					$$ = (Node *) n;
				}
			| ALTER PUBLICATION name ADD_P pub_obj_list
				{
					AlterPublicationStmt *n = makeNode(AlterPublicationStmt);

					n->pubname = $3;
					n->pubobjects = $5;
					preprocess_pubobj_list(n->pubobjects, yyscanner);
					n->action = AP_AddObjects;
					$$ = (Node *) n;
				}
			| ALTER PUBLICATION name SET pub_obj_list
				{
					AlterPublicationStmt *n = makeNode(AlterPublicationStmt);

					n->pubname = $3;
					n->pubobjects = $5;
					preprocess_pubobj_list(n->pubobjects, yyscanner);
					n->action = AP_SetObjects;
					$$ = (Node *) n;
				}
			| ALTER PUBLICATION name DROP pub_obj_list
				{
					AlterPublicationStmt *n = makeNode(AlterPublicationStmt);

					n->pubname = $3;
					n->pubobjects = $5;
					preprocess_pubobj_list(n->pubobjects, yyscanner);
					n->action = AP_DropObjects;
					$$ = (Node *) n;
				}
		;

/*****************************************************************************
 *
 * CREATE SUBSCRIPTION name ...
 *
 *****************************************************************************/

CreateSubscriptionStmt:
			CREATE SUBSCRIPTION name CONNECTION Sconst PUBLICATION name_list opt_definition
				{
					CreateSubscriptionStmt *n =
						makeNode(CreateSubscriptionStmt);
					n->subname = $3;
					n->conninfo = $5;
					n->publication = $7;
					n->options = $8;
					$$ = (Node *) n;
				}
		;

/*****************************************************************************
 *
 * ALTER SUBSCRIPTION name ...
 *
 *****************************************************************************/

AlterSubscriptionStmt:
			ALTER SUBSCRIPTION name SET definition
				{
					AlterSubscriptionStmt *n =
						makeNode(AlterSubscriptionStmt);

					n->kind = ALTER_SUBSCRIPTION_OPTIONS;
					n->subname = $3;
					n->options = $5;
					$$ = (Node *) n;
				}
			| ALTER SUBSCRIPTION name CONNECTION Sconst
				{
					AlterSubscriptionStmt *n =
						makeNode(AlterSubscriptionStmt);

					n->kind = ALTER_SUBSCRIPTION_CONNECTION;
					n->subname = $3;
					n->conninfo = $5;
					$$ = (Node *) n;
				}
			| ALTER SUBSCRIPTION name REFRESH PUBLICATION opt_definition
				{
					AlterSubscriptionStmt *n =
						makeNode(AlterSubscriptionStmt);

					n->kind = ALTER_SUBSCRIPTION_REFRESH;
					n->subname = $3;
					n->options = $6;
					$$ = (Node *) n;
				}
			| ALTER SUBSCRIPTION name ADD_P PUBLICATION name_list opt_definition
				{
					AlterSubscriptionStmt *n =
						makeNode(AlterSubscriptionStmt);

					n->kind = ALTER_SUBSCRIPTION_ADD_PUBLICATION;
					n->subname = $3;
					n->publication = $6;
					n->options = $7;
					$$ = (Node *) n;
				}
			| ALTER SUBSCRIPTION name DROP PUBLICATION name_list opt_definition
				{
					AlterSubscriptionStmt *n =
						makeNode(AlterSubscriptionStmt);

					n->kind = ALTER_SUBSCRIPTION_DROP_PUBLICATION;
					n->subname = $3;
					n->publication = $6;
					n->options = $7;
					$$ = (Node *) n;
				}
			| ALTER SUBSCRIPTION name SET PUBLICATION name_list opt_definition
				{
					AlterSubscriptionStmt *n =
						makeNode(AlterSubscriptionStmt);

					n->kind = ALTER_SUBSCRIPTION_SET_PUBLICATION;
					n->subname = $3;
					n->publication = $6;
					n->options = $7;
					$$ = (Node *) n;
				}
			| ALTER SUBSCRIPTION name ENABLE_P
				{
					AlterSubscriptionStmt *n =
						makeNode(AlterSubscriptionStmt);

					n->kind = ALTER_SUBSCRIPTION_ENABLED;
					n->subname = $3;
					n->options = list_make1(makeDefElem("enabled",
											(Node *) makeBoolean(true), @1));
					$$ = (Node *) n;
				}
			| ALTER SUBSCRIPTION name DISABLE_P
				{
					AlterSubscriptionStmt *n =
						makeNode(AlterSubscriptionStmt);

					n->kind = ALTER_SUBSCRIPTION_ENABLED;
					n->subname = $3;
					n->options = list_make1(makeDefElem("enabled",
											(Node *) makeBoolean(false), @1));
					$$ = (Node *) n;
				}
			| ALTER SUBSCRIPTION name SKIP definition
				{
					AlterSubscriptionStmt *n =
						makeNode(AlterSubscriptionStmt);

					n->kind = ALTER_SUBSCRIPTION_SKIP;
					n->subname = $3;
					n->options = $5;
					$$ = (Node *) n;
				}
		;

/*****************************************************************************
 *
 * DROP SUBSCRIPTION [ IF EXISTS ] name
 *
 *****************************************************************************/

DropSubscriptionStmt: DROP SUBSCRIPTION name opt_drop_behavior
				{
					DropSubscriptionStmt *n = makeNode(DropSubscriptionStmt);

					n->subname = $3;
					n->missing_ok = false;
					n->behavior = $4;
					$$ = (Node *) n;
				}
				|  DROP SUBSCRIPTION IF_P EXISTS name opt_drop_behavior
				{
					DropSubscriptionStmt *n = makeNode(DropSubscriptionStmt);

					n->subname = $5;
					n->missing_ok = true;
					n->behavior = $6;
					$$ = (Node *) n;
				}
		;

/*****************************************************************************
 *
 *		QUERY:	Define Rewrite Rule
 *
 * 查询：定义重写规则
 *****************************************************************************/

RuleStmt:	CREATE opt_or_replace RULE name AS
			ON event TO qualified_name where_clause
			DO opt_instead RuleActionList
				{
					RuleStmt   *n = makeNode(RuleStmt);

					n->replace = $2;
					n->relation = $9;
					n->rulename = $4;
					n->whereClause = $10;
					n->event = $7;
					n->instead = $12;
					n->actions = $13;
					$$ = (Node *) n;
				}
		;

RuleActionList:
			NOTHING									{ $$ = NIL; }
			| RuleActionStmt						{ $$ = list_make1($1); }
			| '(' RuleActionMulti ')'				{ $$ = $2; }
		;

/* the thrashing around here is to discard "empty" statements... - 这里的折腾是为了丢弃 "空" 语句... */
RuleActionMulti:
			RuleActionMulti ';' RuleActionStmtOrEmpty
				{ if ($3 != NULL)
					$$ = lappend($1, $3);
				  else
					$$ = $1;
				}
			| RuleActionStmtOrEmpty
				{ if ($1 != NULL)
					$$ = list_make1($1);
				  else
					$$ = NIL;
				}
		;

RuleActionStmt:
			SelectStmt
			| InsertStmt
			| UpdateStmt
			| DeleteStmt
			| NotifyStmt
		;

RuleActionStmtOrEmpty:
			RuleActionStmt							{ $$ = $1; }
			|	/* EMPTY - 空 */							{ $$ = NULL; }
		;

event:		SELECT									{ $$ = CMD_SELECT; }
			| UPDATE								{ $$ = CMD_UPDATE; }
			| DELETE_P								{ $$ = CMD_DELETE; }
			| INSERT								{ $$ = CMD_INSERT; }
		 ;

opt_instead:
			INSTEAD									{ $$ = true; }
			| ALSO									{ $$ = false; }
			| /* EMPTY - 空 */								{ $$ = false; }
		;


/*****************************************************************************
 *
 *		QUERY:
 *				NOTIFY <identifier> can appear both in rule bodies and
 *				as a query-level command
 *
 * 查询：NOTIFY <identifier> 既可以出现在规则主体中，也可以作为查询级命令出现
 *****************************************************************************/

NotifyStmt: NOTIFY ColId notify_payload
				{
					NotifyStmt *n = makeNode(NotifyStmt);

					n->conditionname = $2;
					n->payload = $3;
					$$ = (Node *) n;
				}
		;

notify_payload:
			',' Sconst							{ $$ = $2; }
			| /* EMPTY - 空 */							{ $$ = NULL; }
		;

ListenStmt: LISTEN ColId
				{
					ListenStmt *n = makeNode(ListenStmt);

					n->conditionname = $2;
					$$ = (Node *) n;
				}
		;

UnlistenStmt:
			UNLISTEN ColId
				{
					UnlistenStmt *n = makeNode(UnlistenStmt);

					n->conditionname = $2;
					$$ = (Node *) n;
				}
			| UNLISTEN '*'
				{
					UnlistenStmt *n = makeNode(UnlistenStmt);

					n->conditionname = NULL;
					$$ = (Node *) n;
				}
		;


/*****************************************************************************
 *
 *		Transactions:
 *
 *		BEGIN / COMMIT / ROLLBACK
 *		(also older versions END / ABORT)
 *
 * 事务：BEGIN / COMMIT / ROLLBACK（以及较早的版本 END / ABORT）
 *****************************************************************************/

TransactionStmt:
			ABORT_P opt_transaction opt_transaction_chain
				{
					TransactionStmt *n = makeNode(TransactionStmt);

					n->kind = TRANS_STMT_ROLLBACK;
					n->options = NIL;
					n->chain = $3;
					n->location = -1;
					$$ = (Node *) n;
				}
			| START TRANSACTION transaction_mode_list_or_empty
				{
					TransactionStmt *n = makeNode(TransactionStmt);

					n->kind = TRANS_STMT_START;
					n->options = $3;
					n->location = -1;
					$$ = (Node *) n;
				}
			| COMMIT opt_transaction opt_transaction_chain
				{
					TransactionStmt *n = makeNode(TransactionStmt);

					n->kind = TRANS_STMT_COMMIT;
					n->options = NIL;
					n->chain = $3;
					n->location = -1;
					$$ = (Node *) n;
				}
			| ROLLBACK opt_transaction opt_transaction_chain
				{
					TransactionStmt *n = makeNode(TransactionStmt);

					n->kind = TRANS_STMT_ROLLBACK;
					n->options = NIL;
					n->chain = $3;
					n->location = -1;
					$$ = (Node *) n;
				}
			| SAVEPOINT ColId
				{
					TransactionStmt *n = makeNode(TransactionStmt);

					n->kind = TRANS_STMT_SAVEPOINT;
					n->savepoint_name = $2;
					n->location = @2;
					$$ = (Node *) n;
				}
			| RELEASE SAVEPOINT ColId
				{
					TransactionStmt *n = makeNode(TransactionStmt);

					n->kind = TRANS_STMT_RELEASE;
					n->savepoint_name = $3;
					n->location = @3;
					$$ = (Node *) n;
				}
			| RELEASE ColId
				{
					TransactionStmt *n = makeNode(TransactionStmt);

					n->kind = TRANS_STMT_RELEASE;
					n->savepoint_name = $2;
					n->location = @2;
					$$ = (Node *) n;
				}
			| ROLLBACK opt_transaction TO SAVEPOINT ColId
				{
					TransactionStmt *n = makeNode(TransactionStmt);

					n->kind = TRANS_STMT_ROLLBACK_TO;
					n->savepoint_name = $5;
					n->location = @5;
					$$ = (Node *) n;
				}
			| ROLLBACK opt_transaction TO ColId
				{
					TransactionStmt *n = makeNode(TransactionStmt);

					n->kind = TRANS_STMT_ROLLBACK_TO;
					n->savepoint_name = $4;
					n->location = @4;
					$$ = (Node *) n;
				}
			| PREPARE TRANSACTION Sconst
				{
					TransactionStmt *n = makeNode(TransactionStmt);

					n->kind = TRANS_STMT_PREPARE;
					n->gid = $3;
					n->location = @3;
					$$ = (Node *) n;
				}
			| COMMIT PREPARED Sconst
				{
					TransactionStmt *n = makeNode(TransactionStmt);

					n->kind = TRANS_STMT_COMMIT_PREPARED;
					n->gid = $3;
					n->location = @3;
					$$ = (Node *) n;
				}
			| ROLLBACK PREPARED Sconst
				{
					TransactionStmt *n = makeNode(TransactionStmt);

					n->kind = TRANS_STMT_ROLLBACK_PREPARED;
					n->gid = $3;
					n->location = @3;
					$$ = (Node *) n;
				}
		;

TransactionStmtLegacy:
			BEGIN_P opt_transaction transaction_mode_list_or_empty
				{
					TransactionStmt *n = makeNode(TransactionStmt);

					n->kind = TRANS_STMT_BEGIN;
					n->options = $3;
					n->location = -1;
					$$ = (Node *) n;
				}
			| END_P opt_transaction opt_transaction_chain
				{
					TransactionStmt *n = makeNode(TransactionStmt);

					n->kind = TRANS_STMT_COMMIT;
					n->options = NIL;
					n->chain = $3;
					n->location = -1;
					$$ = (Node *) n;
				}
		;

opt_transaction:	WORK
			| TRANSACTION
			| /* EMPTY - 空 */
		;

transaction_mode_item:
			ISOLATION LEVEL iso_level
					{ $$ = makeDefElem("transaction_isolation",
									   makeStringConst($3, @3), @1); }
			| READ ONLY
					{ $$ = makeDefElem("transaction_read_only",
									   makeIntConst(true, @1), @1); }
			| READ WRITE
					{ $$ = makeDefElem("transaction_read_only",
									   makeIntConst(false, @1), @1); }
			| DEFERRABLE
					{ $$ = makeDefElem("transaction_deferrable",
									   makeIntConst(true, @1), @1); }
			| NOT DEFERRABLE
					{ $$ = makeDefElem("transaction_deferrable",
									   makeIntConst(false, @1), @1); }
		;

/* Syntax with commas is SQL-spec, without commas is Postgres historical - 带逗号的语法是 SQL 规范，不带逗号的语法是 Postgres 历史遗留的 */
transaction_mode_list:
			transaction_mode_item
					{ $$ = list_make1($1); }
			| transaction_mode_list ',' transaction_mode_item
					{ $$ = lappend($1, $3); }
			| transaction_mode_list transaction_mode_item
					{ $$ = lappend($1, $2); }
		;

transaction_mode_list_or_empty:
			transaction_mode_list
			| /* EMPTY - 空 */
					{ $$ = NIL; }
		;

opt_transaction_chain:
			AND CHAIN		{ $$ = true; }
			| AND NO CHAIN	{ $$ = false; }
			| /* EMPTY - 空 */	{ $$ = false; }
		;


/*****************************************************************************
 *
 *	QUERY:
 *		CREATE [ OR REPLACE ] [ TEMP ] VIEW <viewname> '('target-list ')'
 *			AS <query> [ WITH [ CASCADED | LOCAL ] CHECK OPTION ]
 *
 * 查询：CREATE [ OR REPLACE ] [ TEMP ] VIEW <viewname> '('target-list ')' AS <query> [ WITH [ CASCADED | LOCAL ] CHECK OPTION ]
 *****************************************************************************/

ViewStmt: CREATE OptTemp VIEW qualified_name opt_column_list opt_reloptions
				AS SelectStmt opt_check_option
				{
					ViewStmt   *n = makeNode(ViewStmt);

					n->view = $4;
					n->view->relpersistence = $2;
					n->aliases = $5;
					n->query = $8;
					n->replace = false;
					n->options = $6;
					n->withCheckOption = $9;
					$$ = (Node *) n;
				}
		| CREATE OR REPLACE OptTemp VIEW qualified_name opt_column_list opt_reloptions
				AS SelectStmt opt_check_option
				{
					ViewStmt   *n = makeNode(ViewStmt);

					n->view = $6;
					n->view->relpersistence = $4;
					n->aliases = $7;
					n->query = $10;
					n->replace = true;
					n->options = $8;
					n->withCheckOption = $11;
					$$ = (Node *) n;
				}
		| CREATE OptTemp RECURSIVE VIEW qualified_name '(' columnList ')' opt_reloptions
				AS SelectStmt opt_check_option
				{
					ViewStmt   *n = makeNode(ViewStmt);

					n->view = $5;
					n->view->relpersistence = $2;
					n->aliases = $7;
					n->query = makeRecursiveViewSelect(n->view->relname, n->aliases, $11);
					n->replace = false;
					n->options = $9;
					n->withCheckOption = $12;
					if (n->withCheckOption != NO_CHECK_OPTION)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("WITH CHECK OPTION not supported on recursive views"),
								 parser_errposition(@12)));
					$$ = (Node *) n;
				}
		| CREATE OR REPLACE OptTemp RECURSIVE VIEW qualified_name '(' columnList ')' opt_reloptions
				AS SelectStmt opt_check_option
				{
					ViewStmt   *n = makeNode(ViewStmt);

					n->view = $7;
					n->view->relpersistence = $4;
					n->aliases = $9;
					n->query = makeRecursiveViewSelect(n->view->relname, n->aliases, $13);
					n->replace = true;
					n->options = $11;
					n->withCheckOption = $14;
					if (n->withCheckOption != NO_CHECK_OPTION)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("WITH CHECK OPTION not supported on recursive views"),
								 parser_errposition(@14)));
					$$ = (Node *) n;
				}
		;

opt_check_option:
		WITH CHECK OPTION				{ $$ = CASCADED_CHECK_OPTION; }
		| WITH CASCADED CHECK OPTION	{ $$ = CASCADED_CHECK_OPTION; }
		| WITH LOCAL CHECK OPTION		{ $$ = LOCAL_CHECK_OPTION; }
		| /* EMPTY - 空 */					{ $$ = NO_CHECK_OPTION; }
		;

/*****************************************************************************
 *
 *		QUERY:
 *				LOAD "filename"
 *
 * 查询：LOAD "filename"
 *****************************************************************************/

LoadStmt:	LOAD file_name
				{
					LoadStmt   *n = makeNode(LoadStmt);

					n->filename = $2;
					$$ = (Node *) n;
				}
		;


/*****************************************************************************
 *
 *		CREATE DATABASE
 *
 *****************************************************************************/

CreatedbStmt:
			CREATE DATABASE name opt_with createdb_opt_list
				{
					CreatedbStmt *n = makeNode(CreatedbStmt);

					n->dbname = $3;
					n->options = $5;
					$$ = (Node *) n;
				}
		;

createdb_opt_list:
			createdb_opt_items						{ $$ = $1; }
			| /* EMPTY - 空 */							{ $$ = NIL; }
		;

createdb_opt_items:
			createdb_opt_item						{ $$ = list_make1($1); }
			| createdb_opt_items createdb_opt_item	{ $$ = lappend($1, $2); }
		;

createdb_opt_item:
			createdb_opt_name opt_equal NumericOnly
				{
					$$ = makeDefElem($1, $3, @1);
				}
			| createdb_opt_name opt_equal opt_boolean_or_string
				{
					$$ = makeDefElem($1, (Node *) makeString($3), @1);
				}
			| createdb_opt_name opt_equal DEFAULT
				{
					$$ = makeDefElem($1, NULL, @1);
				}
		;

/*
 * Ideally we'd use ColId here, but that causes shift/reduce conflicts against
 * the ALTER DATABASE SET/RESET syntaxes.  Instead call out specific keywords
 * we need, and allow IDENT so that database option names don't have to be
 * parser keywords unless they are already keywords for other reasons.
 *
 * XXX this coding technique is fragile since if someone makes a formerly
 * non-keyword option name into a keyword and forgets to add it here, the
 * option will silently break.  Best defense is to provide a regression test
 * exercising every such option, at least at the syntax level.
 * 理想情况下，我们在这里使用 ColId，但这会引起针对 ALTER DATABASE SET/RESET 语法的移进/规约冲突。相反，调用我们需要的数据特定关键字，并允许 IDENT，这样数据库选项名称就不必是解析器关键字，除非它们由于其他原因已经是关键字。XXX 这种编码技术很脆弱，因为如果有人将以前的非关键字选项名称变为关键字并忘记将其添加到此处，该选项将默默地失效。最好的防御手段是提供一个测试每个此类选项的回归测试，至少在语法级别上。
 */
createdb_opt_name:
			IDENT							{ $$ = $1; }
			| CONNECTION LIMIT				{ $$ = pstrdup("connection_limit"); }
			| ENCODING						{ $$ = pstrdup($1); }
			| LOCATION						{ $$ = pstrdup($1); }
			| OWNER							{ $$ = pstrdup($1); }
			| TABLESPACE					{ $$ = pstrdup($1); }
			| TEMPLATE						{ $$ = pstrdup($1); }
		;

/*
 *	Though the equals sign doesn't match other WITH options, pg_dump uses
 *	equals for backward compatibility, and it doesn't seem worth removing it.
 * 虽然等号与其他 WITH 选项不匹配，但 pg_dump 为了向后兼容性使用等号，并且似乎不值得将其删除。
 */
opt_equal:	'='
			| /* EMPTY - 空 */
		;


/*****************************************************************************
 *
 *		ALTER DATABASE
 *
 * 理想情况下，我们在这里使用 ColId，但这会引起针对 ALTER DATABASE SET/RESET 语法的移进/规约冲突。相反，调用我们需要的数据特定关键字，并允许 IDENT，这样数据库选项名称就不必是解析器关键字，除非它们由于其他原因已经是关键字。XXX 这种编码技术很脆弱，因为如果有人将以前的非关键字选项名称变为关键字并忘记将其添加到此处，该选项将默默地失效。最好的防御手段是提供一个测试每个此类选项的回归测试，至少在语法级别上。
 *****************************************************************************/

AlterDatabaseStmt:
			ALTER DATABASE name WITH createdb_opt_list
				 {
					AlterDatabaseStmt *n = makeNode(AlterDatabaseStmt);

					n->dbname = $3;
					n->options = $5;
					$$ = (Node *) n;
				 }
			| ALTER DATABASE name createdb_opt_list
				 {
					AlterDatabaseStmt *n = makeNode(AlterDatabaseStmt);

					n->dbname = $3;
					n->options = $4;
					$$ = (Node *) n;
				 }
			| ALTER DATABASE name SET TABLESPACE name
				 {
					AlterDatabaseStmt *n = makeNode(AlterDatabaseStmt);

					n->dbname = $3;
					n->options = list_make1(makeDefElem("tablespace",
														(Node *) makeString($6), @6));
					$$ = (Node *) n;
				 }
			| ALTER DATABASE name REFRESH COLLATION VERSION_P
				 {
					AlterDatabaseRefreshCollStmt *n = makeNode(AlterDatabaseRefreshCollStmt);

					n->dbname = $3;
					$$ = (Node *) n;
				 }
		;

AlterDatabaseSetStmt:
			ALTER DATABASE name SetResetClause
				{
					AlterDatabaseSetStmt *n = makeNode(AlterDatabaseSetStmt);

					n->dbname = $3;
					n->setstmt = $4;
					$$ = (Node *) n;
				}
		;


/*****************************************************************************
 *
 *		DROP DATABASE [ IF EXISTS ] dbname [ [ WITH ] ( options ) ]
 *
 * This is implicitly CASCADE, no need for drop behavior
 * DROP DATABASE [ IF EXISTS ] dbname [ [ WITH ] ( options ) ] 这是隐式的 CASCADE，不需要删除行为
 *****************************************************************************/

DropdbStmt: DROP DATABASE name
				{
					DropdbStmt *n = makeNode(DropdbStmt);

					n->dbname = $3;
					n->missing_ok = false;
					n->options = NULL;
					$$ = (Node *) n;
				}
			| DROP DATABASE IF_P EXISTS name
				{
					DropdbStmt *n = makeNode(DropdbStmt);

					n->dbname = $5;
					n->missing_ok = true;
					n->options = NULL;
					$$ = (Node *) n;
				}
			| DROP DATABASE name opt_with '(' drop_option_list ')'
				{
					DropdbStmt *n = makeNode(DropdbStmt);

					n->dbname = $3;
					n->missing_ok = false;
					n->options = $6;
					$$ = (Node *) n;
				}
			| DROP DATABASE IF_P EXISTS name opt_with '(' drop_option_list ')'
				{
					DropdbStmt *n = makeNode(DropdbStmt);

					n->dbname = $5;
					n->missing_ok = true;
					n->options = $8;
					$$ = (Node *) n;
				}
		;

drop_option_list:
			drop_option
				{
					$$ = list_make1((Node *) $1);
				}
			| drop_option_list ',' drop_option
				{
					$$ = lappend($1, (Node *) $3);
				}
		;

/*
 * Currently only the FORCE option is supported, but the syntax is designed
 * to be extensible so that we can add more options in the future if required.
 * 目前仅支持 FORCE 选项，但该语法的开发设计成是可扩展的，以便在将来需要时添加更多选项。
 */
drop_option:
			FORCE
				{
					$$ = makeDefElem("force", NULL, @1);
				}
		;

/*****************************************************************************
 *
 *		ALTER COLLATION
 *
 *****************************************************************************/

AlterCollationStmt: ALTER COLLATION any_name REFRESH VERSION_P
				{
					AlterCollationStmt *n = makeNode(AlterCollationStmt);

					n->collname = $3;
					$$ = (Node *) n;
				}
		;


/*****************************************************************************
 *
 *		ALTER SYSTEM
 *
 * This is used to change configuration parameters persistently.
 * ALTER SYSTEM。这用于持久地更改配置参数。
 *****************************************************************************/

AlterSystemStmt:
			ALTER SYSTEM_P SET generic_set
				{
					AlterSystemStmt *n = makeNode(AlterSystemStmt);

					n->setstmt = $4;
					$$ = (Node *) n;
				}
			| ALTER SYSTEM_P RESET generic_reset
				{
					AlterSystemStmt *n = makeNode(AlterSystemStmt);

					n->setstmt = $4;
					$$ = (Node *) n;
				}
		;


/*****************************************************************************
 *
 * Manipulate a domain
 *
 * 操作域（domain）
 *****************************************************************************/

CreateDomainStmt:
			CREATE DOMAIN_P any_name opt_as Typename ColQualList
				{
					CreateDomainStmt *n = makeNode(CreateDomainStmt);

					n->domainname = $3;
					n->typeName = $5;
					SplitColQualList($6, &n->constraints, &n->collClause,
									 yyscanner);
					$$ = (Node *) n;
				}
		;

AlterDomainStmt:
			/* ALTER DOMAIN <domain> {SET DEFAULT <expr>|DROP DEFAULT} */
			ALTER DOMAIN_P any_name alter_column_default
				{
					AlterDomainStmt *n = makeNode(AlterDomainStmt);

					n->subtype = 'T';
					n->typeName = $3;
					n->def = $4;
					$$ = (Node *) n;
				}
			/* ALTER DOMAIN <domain> DROP NOT NULL */
			| ALTER DOMAIN_P any_name DROP NOT NULL_P
				{
					AlterDomainStmt *n = makeNode(AlterDomainStmt);

					n->subtype = 'N';
					n->typeName = $3;
					$$ = (Node *) n;
				}
			/* ALTER DOMAIN <domain> SET NOT NULL */
			| ALTER DOMAIN_P any_name SET NOT NULL_P
				{
					AlterDomainStmt *n = makeNode(AlterDomainStmt);

					n->subtype = 'O';
					n->typeName = $3;
					$$ = (Node *) n;
				}
			/* ALTER DOMAIN <domain> ADD CONSTRAINT ... */
			| ALTER DOMAIN_P any_name ADD_P DomainConstraint
				{
					AlterDomainStmt *n = makeNode(AlterDomainStmt);

					n->subtype = 'C';
					n->typeName = $3;
					n->def = $5;
					$$ = (Node *) n;
				}
			/* ALTER DOMAIN <domain> DROP CONSTRAINT <name> [RESTRICT|CASCADE] */
			| ALTER DOMAIN_P any_name DROP CONSTRAINT name opt_drop_behavior
				{
					AlterDomainStmt *n = makeNode(AlterDomainStmt);

					n->subtype = 'X';
					n->typeName = $3;
					n->name = $6;
					n->behavior = $7;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			/* ALTER DOMAIN <domain> DROP CONSTRAINT IF EXISTS <name> [RESTRICT|CASCADE] */
			| ALTER DOMAIN_P any_name DROP CONSTRAINT IF_P EXISTS name opt_drop_behavior
				{
					AlterDomainStmt *n = makeNode(AlterDomainStmt);

					n->subtype = 'X';
					n->typeName = $3;
					n->name = $8;
					n->behavior = $9;
					n->missing_ok = true;
					$$ = (Node *) n;
				}
			/* ALTER DOMAIN <domain> VALIDATE CONSTRAINT <name> */
			| ALTER DOMAIN_P any_name VALIDATE CONSTRAINT name
				{
					AlterDomainStmt *n = makeNode(AlterDomainStmt);

					n->subtype = 'V';
					n->typeName = $3;
					n->name = $6;
					$$ = (Node *) n;
				}
			;

opt_as:		AS
			| /* EMPTY - 空 */
		;


/*****************************************************************************
 *
 * Manipulate a text search dictionary or configuration
 *
 * 操作文本搜索字典或配置
 *****************************************************************************/

AlterTSDictionaryStmt:
			ALTER TEXT_P SEARCH DICTIONARY any_name definition
				{
					AlterTSDictionaryStmt *n = makeNode(AlterTSDictionaryStmt);

					n->dictname = $5;
					n->options = $6;
					$$ = (Node *) n;
				}
		;

AlterTSConfigurationStmt:
			ALTER TEXT_P SEARCH CONFIGURATION any_name ADD_P MAPPING FOR name_list any_with any_name_list
				{
					AlterTSConfigurationStmt *n = makeNode(AlterTSConfigurationStmt);

					n->kind = ALTER_TSCONFIG_ADD_MAPPING;
					n->cfgname = $5;
					n->tokentype = $9;
					n->dicts = $11;
					n->override = false;
					n->replace = false;
					$$ = (Node *) n;
				}
			| ALTER TEXT_P SEARCH CONFIGURATION any_name ALTER MAPPING FOR name_list any_with any_name_list
				{
					AlterTSConfigurationStmt *n = makeNode(AlterTSConfigurationStmt);

					n->kind = ALTER_TSCONFIG_ALTER_MAPPING_FOR_TOKEN;
					n->cfgname = $5;
					n->tokentype = $9;
					n->dicts = $11;
					n->override = true;
					n->replace = false;
					$$ = (Node *) n;
				}
			| ALTER TEXT_P SEARCH CONFIGURATION any_name ALTER MAPPING REPLACE any_name any_with any_name
				{
					AlterTSConfigurationStmt *n = makeNode(AlterTSConfigurationStmt);

					n->kind = ALTER_TSCONFIG_REPLACE_DICT;
					n->cfgname = $5;
					n->tokentype = NIL;
					n->dicts = list_make2($9,$11);
					n->override = false;
					n->replace = true;
					$$ = (Node *) n;
				}
			| ALTER TEXT_P SEARCH CONFIGURATION any_name ALTER MAPPING FOR name_list REPLACE any_name any_with any_name
				{
					AlterTSConfigurationStmt *n = makeNode(AlterTSConfigurationStmt);

					n->kind = ALTER_TSCONFIG_REPLACE_DICT_FOR_TOKEN;
					n->cfgname = $5;
					n->tokentype = $9;
					n->dicts = list_make2($11,$13);
					n->override = false;
					n->replace = true;
					$$ = (Node *) n;
				}
			| ALTER TEXT_P SEARCH CONFIGURATION any_name DROP MAPPING FOR name_list
				{
					AlterTSConfigurationStmt *n = makeNode(AlterTSConfigurationStmt);

					n->kind = ALTER_TSCONFIG_DROP_MAPPING;
					n->cfgname = $5;
					n->tokentype = $9;
					n->missing_ok = false;
					$$ = (Node *) n;
				}
			| ALTER TEXT_P SEARCH CONFIGURATION any_name DROP MAPPING IF_P EXISTS FOR name_list
				{
					AlterTSConfigurationStmt *n = makeNode(AlterTSConfigurationStmt);

					n->kind = ALTER_TSCONFIG_DROP_MAPPING;
					n->cfgname = $5;
					n->tokentype = $11;
					n->missing_ok = true;
					$$ = (Node *) n;
				}
		;

/* Use this if TIME or ORDINALITY after WITH should be taken as an identifier - 如果 WITH 后面的 TIME 或 ORDINALITY 应该被当作标识符，则使用此项 */
any_with:	WITH
			| WITH_LA
		;


/*****************************************************************************
 *
 * Manipulate a conversion
 *
 *		CREATE [DEFAULT] CONVERSION <conversion_name>
 *		FOR <encoding_name> TO <encoding_name> FROM <func_name>
 *
 * 操作转换 CREATE [DEFAULT] CONVERSION <conversion_name> FOR <encoding_name> TO <encoding_name> FROM <func_name>
 *****************************************************************************/

CreateConversionStmt:
			CREATE opt_default CONVERSION_P any_name FOR Sconst
			TO Sconst FROM any_name
			{
				CreateConversionStmt *n = makeNode(CreateConversionStmt);

				n->conversion_name = $4;
				n->for_encoding_name = $6;
				n->to_encoding_name = $8;
				n->func_name = $10;
				n->def = $2;
				$$ = (Node *) n;
			}
		;

/*****************************************************************************
 *
 *		QUERY:
 *				CLUSTER (options) [ <qualified_name> [ USING <index_name> ] ]
 *				CLUSTER [VERBOSE] [ <qualified_name> [ USING <index_name> ] ]
 *				CLUSTER [VERBOSE] <index_name> ON <qualified_name> (for pre-8.3)
 *
 * 查询: CLUSTER (选项) [ <限制名> [ USING <索引名> ] ] CLUSTER [VERBOSE] [ <限制名> [ USING <索引名> ] ] CLUSTER [VERBOSE] <索引名> ON <限制名> (用于 8.3 之前版本)
 *****************************************************************************/

ClusterStmt:
			CLUSTER '(' utility_option_list ')' qualified_name cluster_index_specification
				{
					ClusterStmt *n = makeNode(ClusterStmt);

					n->relation = $5;
					n->indexname = $6;
					n->params = $3;
					$$ = (Node *) n;
				}
			| CLUSTER '(' utility_option_list ')'
				{
					ClusterStmt *n = makeNode(ClusterStmt);

					n->relation = NULL;
					n->indexname = NULL;
					n->params = $3;
					$$ = (Node *) n;
				}
			/* unparenthesized VERBOSE kept for pre-14 compatibility - 保留不带括号的 VERBOSE 以兼容 14 之前的版本 */
			| CLUSTER opt_verbose qualified_name cluster_index_specification
				{
					ClusterStmt *n = makeNode(ClusterStmt);

					n->relation = $3;
					n->indexname = $4;
					n->params = NIL;
					if ($2)
						n->params = lappend(n->params, makeDefElem("verbose", NULL, @2));
					$$ = (Node *) n;
				}
			/* unparenthesized VERBOSE kept for pre-17 compatibility - 保留不带括号的 VERBOSE 以兼容 17 之前的版本 */
			| CLUSTER opt_verbose
				{
					ClusterStmt *n = makeNode(ClusterStmt);

					n->relation = NULL;
					n->indexname = NULL;
					n->params = NIL;
					if ($2)
						n->params = lappend(n->params, makeDefElem("verbose", NULL, @2));
					$$ = (Node *) n;
				}
			/* kept for pre-8.3 compatibility - 保留以兼容 8.3 之前的版本 */
			| CLUSTER opt_verbose name ON qualified_name
				{
					ClusterStmt *n = makeNode(ClusterStmt);

					n->relation = $5;
					n->indexname = $3;
					n->params = NIL;
					if ($2)
						n->params = lappend(n->params, makeDefElem("verbose", NULL, @2));
					$$ = (Node *) n;
				}
		;

cluster_index_specification:
			USING name				{ $$ = $2; }
			| /* EMPTY - 空 */				{ $$ = NULL; }
		;


/*****************************************************************************
 *
 *		QUERY:
 *				VACUUM
 *				ANALYZE
 *
 * 查询: VACUUM ANALYZE
 *****************************************************************************/

VacuumStmt: VACUUM opt_full opt_freeze opt_verbose opt_analyze opt_vacuum_relation_list
				{
					VacuumStmt *n = makeNode(VacuumStmt);

					n->options = NIL;
					if ($2)
						n->options = lappend(n->options,
											 makeDefElem("full", NULL, @2));
					if ($3)
						n->options = lappend(n->options,
											 makeDefElem("freeze", NULL, @3));
					if ($4)
						n->options = lappend(n->options,
											 makeDefElem("verbose", NULL, @4));
					if ($5)
						n->options = lappend(n->options,
											 makeDefElem("analyze", NULL, @5));
					n->rels = $6;
					n->is_vacuumcmd = true;
					$$ = (Node *) n;
				}
			| VACUUM '(' utility_option_list ')' opt_vacuum_relation_list
				{
					VacuumStmt *n = makeNode(VacuumStmt);

					n->options = $3;
					n->rels = $5;
					n->is_vacuumcmd = true;
					$$ = (Node *) n;
				}
		;

AnalyzeStmt: analyze_keyword opt_verbose opt_vacuum_relation_list
				{
					VacuumStmt *n = makeNode(VacuumStmt);

					n->options = NIL;
					if ($2)
						n->options = lappend(n->options,
											 makeDefElem("verbose", NULL, @2));
					n->rels = $3;
					n->is_vacuumcmd = false;
					$$ = (Node *) n;
				}
			| analyze_keyword '(' utility_option_list ')' opt_vacuum_relation_list
				{
					VacuumStmt *n = makeNode(VacuumStmt);

					n->options = $3;
					n->rels = $5;
					n->is_vacuumcmd = false;
					$$ = (Node *) n;
				}
		;

utility_option_list:
			utility_option_elem
				{
					$$ = list_make1($1);
				}
			| utility_option_list ',' utility_option_elem
				{
					$$ = lappend($1, $3);
				}
		;

analyze_keyword:
			ANALYZE
			| ANALYSE /* British - 英式英语 */
		;

utility_option_elem:
			utility_option_name utility_option_arg
				{
					$$ = makeDefElem($1, $2, @1);
				}
		;

utility_option_name:
			NonReservedWord							{ $$ = $1; }
			| analyze_keyword						{ $$ = "analyze"; }
			| FORMAT_LA								{ $$ = "format"; }
		;

utility_option_arg:
			opt_boolean_or_string					{ $$ = (Node *) makeString($1); }
			| NumericOnly							{ $$ = (Node *) $1; }
			| /* EMPTY - 空 */							{ $$ = NULL; }
		;

opt_analyze:
			analyze_keyword							{ $$ = true; }
			| /* EMPTY - 空 */								{ $$ = false; }
		;

opt_verbose:
			VERBOSE									{ $$ = true; }
			| /* EMPTY - 空 */								{ $$ = false; }
		;

opt_full:	FULL									{ $$ = true; }
			| /* EMPTY - 空 */								{ $$ = false; }
		;

opt_freeze: FREEZE									{ $$ = true; }
			| /* EMPTY - 空 */								{ $$ = false; }
		;

opt_name_list:
			'(' name_list ')'						{ $$ = $2; }
			| /* EMPTY - 空 */								{ $$ = NIL; }
		;

vacuum_relation:
			relation_expr opt_name_list
				{
					$$ = (Node *) makeVacuumRelation($1, InvalidOid, $2);
				}
		;

vacuum_relation_list:
			vacuum_relation
					{ $$ = list_make1($1); }
			| vacuum_relation_list ',' vacuum_relation
					{ $$ = lappend($1, $3); }
		;

opt_vacuum_relation_list:
			vacuum_relation_list					{ $$ = $1; }
			| /* EMPTY - 空 */								{ $$ = NIL; }
		;


/*****************************************************************************
 *
 *		QUERY:
 *				EXPLAIN [ANALYZE] [VERBOSE] query
 *				EXPLAIN ( options ) query
 *
 * 查询: EXPLAIN [ANALYZE] [VERBOSE] 查询 EXPLAIN ( 选项 ) 查询
 *****************************************************************************/

ExplainStmt:
		EXPLAIN ExplainableStmt
				{
					ExplainStmt *n = makeNode(ExplainStmt);

					n->query = $2;
					n->options = NIL;
					$$ = (Node *) n;
				}
		| EXPLAIN analyze_keyword opt_verbose ExplainableStmt
				{
					ExplainStmt *n = makeNode(ExplainStmt);

					n->query = $4;
					n->options = list_make1(makeDefElem("analyze", NULL, @2));
					if ($3)
						n->options = lappend(n->options,
											 makeDefElem("verbose", NULL, @3));
					$$ = (Node *) n;
				}
		| EXPLAIN VERBOSE ExplainableStmt
				{
					ExplainStmt *n = makeNode(ExplainStmt);

					n->query = $3;
					n->options = list_make1(makeDefElem("verbose", NULL, @2));
					$$ = (Node *) n;
				}
		| EXPLAIN '(' utility_option_list ')' ExplainableStmt
				{
					ExplainStmt *n = makeNode(ExplainStmt);

					n->query = $5;
					n->options = $3;
					$$ = (Node *) n;
				}
		;

ExplainableStmt:
			SelectStmt
			| InsertStmt
			| UpdateStmt
			| DeleteStmt
			| MergeStmt
			| DeclareCursorStmt
			| CreateAsStmt
			| CreateMatViewStmt
			| RefreshMatViewStmt
			| ExecuteStmt					/* by default all are $$=$1 - 默认全部为 $$=$1 */
		;

/*****************************************************************************
 *
 *		QUERY:
 *				PREPARE <plan_name> [(args, ...)] AS <query>
 *
 * 查询: PREPARE <计划名称> [(参数, ...)] AS <查询>
 *****************************************************************************/

PrepareStmt: PREPARE name prep_type_clause AS PreparableStmt
				{
					PrepareStmt *n = makeNode(PrepareStmt);

					n->name = $2;
					n->argtypes = $3;
					n->query = $5;
					$$ = (Node *) n;
				}
		;

prep_type_clause: '(' type_list ')'			{ $$ = $2; }
				| /* EMPTY - 空 */				{ $$ = NIL; }
		;

PreparableStmt:
			SelectStmt
			| InsertStmt
			| UpdateStmt
			| DeleteStmt
			| MergeStmt						/* by default all are $$=$1 - 默认全部为 $$=$1 */
		;

/*****************************************************************************
 *
 * EXECUTE <plan_name> [(params, ...)]
 * CREATE TABLE <name> AS EXECUTE <plan_name> [(params, ...)]
 *
 * EXECUTE <计划名称> [(参数, ...)] CREATE TABLE <名称> AS EXECUTE <计划名称> [(参数, ...)]
 *****************************************************************************/

ExecuteStmt: EXECUTE name execute_param_clause
				{
					ExecuteStmt *n = makeNode(ExecuteStmt);

					n->name = $2;
					n->params = $3;
					$$ = (Node *) n;
				}
			| CREATE OptTemp TABLE create_as_target AS
				EXECUTE name execute_param_clause opt_with_data
				{
					CreateTableAsStmt *ctas = makeNode(CreateTableAsStmt);
					ExecuteStmt *n = makeNode(ExecuteStmt);

					n->name = $7;
					n->params = $8;
					ctas->query = (Node *) n;
					ctas->into = $4;
					ctas->objtype = OBJECT_TABLE;
					ctas->is_select_into = false;
					ctas->if_not_exists = false;
					/* cram additional flags into the IntoClause - 把额外的标志塞进 IntoClause 中 */
					$4->rel->relpersistence = $2;
					$4->skipData = !($9);
					$$ = (Node *) ctas;
				}
			| CREATE OptTemp TABLE IF_P NOT EXISTS create_as_target AS
				EXECUTE name execute_param_clause opt_with_data
				{
					CreateTableAsStmt *ctas = makeNode(CreateTableAsStmt);
					ExecuteStmt *n = makeNode(ExecuteStmt);

					n->name = $10;
					n->params = $11;
					ctas->query = (Node *) n;
					ctas->into = $7;
					ctas->objtype = OBJECT_TABLE;
					ctas->is_select_into = false;
					ctas->if_not_exists = true;
					/* cram additional flags into the IntoClause - 把额外的标志塞进 IntoClause 中 */
					$7->rel->relpersistence = $2;
					$7->skipData = !($12);
					$$ = (Node *) ctas;
				}
		;

execute_param_clause: '(' expr_list ')'				{ $$ = $2; }
					| /* EMPTY - 空 */					{ $$ = NIL; }
					;

/*****************************************************************************
 *
 *		QUERY:
 *				DEALLOCATE [PREPARE] <plan_name>
 *
 * 查询: DEALLOCATE [PREPARE] <计划名称>
 *****************************************************************************/

DeallocateStmt: DEALLOCATE name
					{
						DeallocateStmt *n = makeNode(DeallocateStmt);

						n->name = $2;
						n->isall = false;
						n->location = @2;
						$$ = (Node *) n;
					}
				| DEALLOCATE PREPARE name
					{
						DeallocateStmt *n = makeNode(DeallocateStmt);

						n->name = $3;
						n->isall = false;
						n->location = @3;
						$$ = (Node *) n;
					}
				| DEALLOCATE ALL
					{
						DeallocateStmt *n = makeNode(DeallocateStmt);

						n->name = NULL;
						n->isall = true;
						n->location = -1;
						$$ = (Node *) n;
					}
				| DEALLOCATE PREPARE ALL
					{
						DeallocateStmt *n = makeNode(DeallocateStmt);

						n->name = NULL;
						n->isall = true;
						n->location = -1;
						$$ = (Node *) n;
					}
		;

/*****************************************************************************
 *
 *		QUERY:
 *				INSERT STATEMENTS
 *
 * 查询: INSERT 语句
 *****************************************************************************/

InsertStmt:
			opt_with_clause INSERT INTO insert_target insert_rest
			opt_on_conflict returning_clause
				{
					$5->relation = $4;
					$5->onConflictClause = $6;
					$5->returningClause = $7;
					$5->withClause = $1;
					$$ = (Node *) $5;
				}
		;

/*
 * Can't easily make AS optional here, because VALUES in insert_rest would
 * have a shift/reduce conflict with VALUES as an optional alias.  We could
 * easily allow unreserved_keywords as optional aliases, but that'd be an odd
 * divergence from other places.  So just require AS for now.
 * 在此处无法轻易使 AS 成为可选的，因为 insert_rest 中的 VALUES 会与作为可选别名的 VALUES 产生 移进/规约 (shift/reduce) 冲突。我们可以轻易允许非保留关键字作为可选别名，但那会与其他地方产生奇怪的分歧。因此目前要求必须使用 AS。
 */
insert_target:
			qualified_name
				{
					$$ = $1;
				}
			| qualified_name AS ColId
				{
					$1->alias = makeAlias($3, NIL);
					$$ = $1;
				}
		;

insert_rest:
			SelectStmt
				{
					$$ = makeNode(InsertStmt);
					$$->cols = NIL;
					$$->selectStmt = $1;
				}
			| OVERRIDING override_kind VALUE_P SelectStmt
				{
					$$ = makeNode(InsertStmt);
					$$->cols = NIL;
					$$->override = $2;
					$$->selectStmt = $4;
				}
			| '(' insert_column_list ')' SelectStmt
				{
					$$ = makeNode(InsertStmt);
					$$->cols = $2;
					$$->selectStmt = $4;
				}
			| '(' insert_column_list ')' OVERRIDING override_kind VALUE_P SelectStmt
				{
					$$ = makeNode(InsertStmt);
					$$->cols = $2;
					$$->override = $5;
					$$->selectStmt = $7;
				}
			| DEFAULT VALUES
				{
					$$ = makeNode(InsertStmt);
					$$->cols = NIL;
					$$->selectStmt = NULL;
				}
		;

override_kind:
			USER		{ $$ = OVERRIDING_USER_VALUE; }
			| SYSTEM_P	{ $$ = OVERRIDING_SYSTEM_VALUE; }
		;

insert_column_list:
			insert_column_item
					{ $$ = list_make1($1); }
			| insert_column_list ',' insert_column_item
					{ $$ = lappend($1, $3); }
		;

insert_column_item:
			ColId opt_indirection
				{
					$$ = makeNode(ResTarget);
					$$->name = $1;
					$$->indirection = check_indirection($2, yyscanner);
					$$->val = NULL;
					$$->location = @1;
				}
		;

opt_on_conflict:
			ON CONFLICT opt_conf_expr DO UPDATE SET set_clause_list	where_clause
				{
					$$ = makeNode(OnConflictClause);
					$$->action = ONCONFLICT_UPDATE;
					$$->infer = $3;
					$$->targetList = $7;
					$$->whereClause = $8;
					$$->location = @1;
				}
			|
			ON CONFLICT opt_conf_expr DO NOTHING
				{
					$$ = makeNode(OnConflictClause);
					$$->action = ONCONFLICT_NOTHING;
					$$->infer = $3;
					$$->targetList = NIL;
					$$->whereClause = NULL;
					$$->location = @1;
				}
			| /* EMPTY - 空 */
				{
					$$ = NULL;
				}
		;

opt_conf_expr:
			'(' index_params ')' where_clause
				{
					$$ = makeNode(InferClause);
					$$->indexElems = $2;
					$$->whereClause = $4;
					$$->conname = NULL;
					$$->location = @1;
				}
			|
			ON CONSTRAINT name
				{
					$$ = makeNode(InferClause);
					$$->indexElems = NIL;
					$$->whereClause = NULL;
					$$->conname = $3;
					$$->location = @1;
				}
			| /* EMPTY - 空 */
				{
					$$ = NULL;
				}
		;

returning_clause:
			RETURNING returning_with_clause target_list
				{
					ReturningClause *n = makeNode(ReturningClause);

					n->options = $2;
					n->exprs = $3;
					$$ = n;
				}
			| /* EMPTY - 空 */
				{
					$$ = NULL;
				}
		;

returning_with_clause:
			WITH '(' returning_options ')'		{ $$ = $3; }
			| /* EMPTY - 空 */						{ $$ = NIL; }
		;

returning_options:
			returning_option							{ $$ = list_make1($1); }
			| returning_options ',' returning_option	{ $$ = lappend($1, $3); }
		;

returning_option:
			returning_option_kind AS ColId
				{
					ReturningOption *n = makeNode(ReturningOption);

					n->option = $1;
					n->value = $3;
					n->location = @1;
					$$ = (Node *) n;
				}
		;

returning_option_kind:
			OLD			{ $$ = RETURNING_OPTION_OLD; }
			| NEW		{ $$ = RETURNING_OPTION_NEW; }
		;


/*****************************************************************************
 *
 *		QUERY:
 *				DELETE STATEMENTS
 *
 * 查询: DELETE 语句
 *****************************************************************************/

DeleteStmt: opt_with_clause DELETE_P FROM relation_expr_opt_alias
			using_clause where_or_current_clause returning_clause
				{
					DeleteStmt *n = makeNode(DeleteStmt);

					n->relation = $4;
					n->usingClause = $5;
					n->whereClause = $6;
					n->returningClause = $7;
					n->withClause = $1;
					$$ = (Node *) n;
				}
		;

using_clause:
				USING from_list						{ $$ = $2; }
			| /* EMPTY - 空 */								{ $$ = NIL; }
		;


/*****************************************************************************
 *
 *		QUERY:
 *				LOCK TABLE
 *
 * 查询: LOCK TABLE
 *****************************************************************************/

LockStmt:	LOCK_P opt_table relation_expr_list opt_lock opt_nowait
				{
					LockStmt   *n = makeNode(LockStmt);

					n->relations = $3;
					n->mode = $4;
					n->nowait = $5;
					$$ = (Node *) n;
				}
		;

opt_lock:	IN_P lock_type MODE				{ $$ = $2; }
			| /* EMPTY - 空 */						{ $$ = AccessExclusiveLock; }
		;

lock_type:	ACCESS SHARE					{ $$ = AccessShareLock; }
			| ROW SHARE						{ $$ = RowShareLock; }
			| ROW EXCLUSIVE					{ $$ = RowExclusiveLock; }
			| SHARE UPDATE EXCLUSIVE		{ $$ = ShareUpdateExclusiveLock; }
			| SHARE							{ $$ = ShareLock; }
			| SHARE ROW EXCLUSIVE			{ $$ = ShareRowExclusiveLock; }
			| EXCLUSIVE						{ $$ = ExclusiveLock; }
			| ACCESS EXCLUSIVE				{ $$ = AccessExclusiveLock; }
		;

opt_nowait:	NOWAIT							{ $$ = true; }
			| /* EMPTY - 空 */						{ $$ = false; }
		;

opt_nowait_or_skip:
			NOWAIT							{ $$ = LockWaitError; }
			| SKIP LOCKED					{ $$ = LockWaitSkip; }
			| /* EMPTY - 空 */						{ $$ = LockWaitBlock; }
		;


/*****************************************************************************
 *
 *		QUERY:
 *				UpdateStmt (UPDATE)
 *
 * 查询: UpdateStmt (UPDATE)
 *****************************************************************************/

UpdateStmt: opt_with_clause UPDATE relation_expr_opt_alias
			SET set_clause_list
			from_clause
			where_or_current_clause
			returning_clause
				{
					UpdateStmt *n = makeNode(UpdateStmt);

					n->relation = $3;
					n->targetList = $5;
					n->fromClause = $6;
					n->whereClause = $7;
					n->returningClause = $8;
					n->withClause = $1;
					$$ = (Node *) n;
				}
		;

set_clause_list:
			set_clause							{ $$ = $1; }
			| set_clause_list ',' set_clause	{ $$ = list_concat($1,$3); }
		;

set_clause:
			set_target '=' a_expr
				{
					$1->val = (Node *) $3;
					$$ = list_make1($1);
				}
			| '(' set_target_list ')' '=' a_expr
				{
					int			ncolumns = list_length($2);
					int			i = 1;
					ListCell   *col_cell;

					/* Create a MultiAssignRef source for each target - 为每个目标创建一个 MultiAssignRef 源 */
					foreach(col_cell, $2)
					{
						ResTarget  *res_col = (ResTarget *) lfirst(col_cell);
						MultiAssignRef *r = makeNode(MultiAssignRef);

						r->source = (Node *) $5;
						r->colno = i;
						r->ncolumns = ncolumns;
						res_col->val = (Node *) r;
						i++;
					}

					$$ = $2;
				}
		;

set_target:
			ColId opt_indirection
				{
					$$ = makeNode(ResTarget);
					$$->name = $1;
					$$->indirection = check_indirection($2, yyscanner);
					$$->val = NULL;	/* upper production sets this - 上层产生式设置此项 */
					$$->location = @1;
				}
		;

set_target_list:
			set_target								{ $$ = list_make1($1); }
			| set_target_list ',' set_target		{ $$ = lappend($1,$3); }
		;


/*****************************************************************************
 *
 *		QUERY:
 *				MERGE
 *
 * 查询: MERGE
 *****************************************************************************/

MergeStmt:
			opt_with_clause MERGE INTO relation_expr_opt_alias
			USING table_ref
			ON a_expr
			merge_when_list
			returning_clause
				{
					MergeStmt  *m = makeNode(MergeStmt);

					m->withClause = $1;
					m->relation = $4;
					m->sourceRelation = $6;
					m->joinCondition = $8;
					m->mergeWhenClauses = $9;
					m->returningClause = $10;

					$$ = (Node *) m;
				}
		;

merge_when_list:
			merge_when_clause						{ $$ = list_make1($1); }
			| merge_when_list merge_when_clause		{ $$ = lappend($1,$2); }
		;

/*
 * A WHEN clause may be WHEN MATCHED, WHEN NOT MATCHED BY SOURCE, or WHEN NOT
 * MATCHED [BY TARGET]. The first two cases match target tuples, and support
 * UPDATE/DELETE/DO NOTHING actions. The third case does not match target
 * tuples, and only supports INSERT/DO NOTHING actions.
 * WHEN 子句可以是 WHEN MATCHED、WHEN NOT MATCHED BY SOURCE 或 WHEN NOT MATCHED [BY TARGET]。前两种情况匹配目标元组，并支持 UPDATE/DELETE/DO NOTHING 操作。第三种情况不匹配目标元组，且仅支持 INSERT/DO NOTHING 操作。
 */
merge_when_clause:
			merge_when_tgt_matched opt_merge_when_condition THEN merge_update
				{
					$4->matchKind = $1;
					$4->condition = $2;

					$$ = (Node *) $4;
				}
			| merge_when_tgt_matched opt_merge_when_condition THEN merge_delete
				{
					$4->matchKind = $1;
					$4->condition = $2;

					$$ = (Node *) $4;
				}
			| merge_when_tgt_not_matched opt_merge_when_condition THEN merge_insert
				{
					$4->matchKind = $1;
					$4->condition = $2;

					$$ = (Node *) $4;
				}
			| merge_when_tgt_matched opt_merge_when_condition THEN DO NOTHING
				{
					MergeWhenClause *m = makeNode(MergeWhenClause);

					m->matchKind = $1;
					m->commandType = CMD_NOTHING;
					m->condition = $2;

					$$ = (Node *) m;
				}
			| merge_when_tgt_not_matched opt_merge_when_condition THEN DO NOTHING
				{
					MergeWhenClause *m = makeNode(MergeWhenClause);

					m->matchKind = $1;
					m->commandType = CMD_NOTHING;
					m->condition = $2;

					$$ = (Node *) m;
				}
		;

merge_when_tgt_matched:
			WHEN MATCHED					{ $$ = MERGE_WHEN_MATCHED; }
			| WHEN NOT MATCHED BY SOURCE	{ $$ = MERGE_WHEN_NOT_MATCHED_BY_SOURCE; }
		;

merge_when_tgt_not_matched:
			WHEN NOT MATCHED				{ $$ = MERGE_WHEN_NOT_MATCHED_BY_TARGET; }
			| WHEN NOT MATCHED BY TARGET	{ $$ = MERGE_WHEN_NOT_MATCHED_BY_TARGET; }
		;

opt_merge_when_condition:
			AND a_expr				{ $$ = $2; }
			|						{ $$ = NULL; }
		;

merge_update:
			UPDATE SET set_clause_list
				{
					MergeWhenClause *n = makeNode(MergeWhenClause);
					n->commandType = CMD_UPDATE;
					n->override = OVERRIDING_NOT_SET;
					n->targetList = $3;
					n->values = NIL;

					$$ = n;
				}
		;

merge_delete:
			DELETE_P
				{
					MergeWhenClause *n = makeNode(MergeWhenClause);
					n->commandType = CMD_DELETE;
					n->override = OVERRIDING_NOT_SET;
					n->targetList = NIL;
					n->values = NIL;

					$$ = n;
				}
		;

merge_insert:
			INSERT merge_values_clause
				{
					MergeWhenClause *n = makeNode(MergeWhenClause);
					n->commandType = CMD_INSERT;
					n->override = OVERRIDING_NOT_SET;
					n->targetList = NIL;
					n->values = $2;
					$$ = n;
				}
			| INSERT OVERRIDING override_kind VALUE_P merge_values_clause
				{
					MergeWhenClause *n = makeNode(MergeWhenClause);
					n->commandType = CMD_INSERT;
					n->override = $3;
					n->targetList = NIL;
					n->values = $5;
					$$ = n;
				}
			| INSERT '(' insert_column_list ')' merge_values_clause
				{
					MergeWhenClause *n = makeNode(MergeWhenClause);
					n->commandType = CMD_INSERT;
					n->override = OVERRIDING_NOT_SET;
					n->targetList = $3;
					n->values = $5;
					$$ = n;
				}
			| INSERT '(' insert_column_list ')' OVERRIDING override_kind VALUE_P merge_values_clause
				{
					MergeWhenClause *n = makeNode(MergeWhenClause);
					n->commandType = CMD_INSERT;
					n->override = $6;
					n->targetList = $3;
					n->values = $8;
					$$ = n;
				}
			| INSERT DEFAULT VALUES
				{
					MergeWhenClause *n = makeNode(MergeWhenClause);
					n->commandType = CMD_INSERT;
					n->override = OVERRIDING_NOT_SET;
					n->targetList = NIL;
					n->values = NIL;
					$$ = n;
				}
		;

merge_values_clause:
			VALUES '(' expr_list ')'
				{
					$$ = $3;
				}
		;

/*****************************************************************************
 *
 *		QUERY:
 *				CURSOR STATEMENTS
 *
 * 查询: 游标语句
 *****************************************************************************/
DeclareCursorStmt: DECLARE cursor_name cursor_options CURSOR opt_hold FOR SelectStmt
				{
					DeclareCursorStmt *n = makeNode(DeclareCursorStmt);

					n->portalname = $2;
					/* currently we always set FAST_PLAN option - 目前我们总是设置 FAST_PLAN 选项 */
					n->options = $3 | $5 | CURSOR_OPT_FAST_PLAN;
					n->query = $7;
					$$ = (Node *) n;
				}
		;

cursor_name:	name						{ $$ = $1; }
		;

cursor_options: /* EMPTY - 空 */					{ $$ = 0; }
			| cursor_options NO SCROLL		{ $$ = $1 | CURSOR_OPT_NO_SCROLL; }
			| cursor_options SCROLL			{ $$ = $1 | CURSOR_OPT_SCROLL; }
			| cursor_options BINARY			{ $$ = $1 | CURSOR_OPT_BINARY; }
			| cursor_options ASENSITIVE		{ $$ = $1 | CURSOR_OPT_ASENSITIVE; }
			| cursor_options INSENSITIVE	{ $$ = $1 | CURSOR_OPT_INSENSITIVE; }
		;

opt_hold: /* EMPTY - 空 */						{ $$ = 0; }
			| WITH HOLD						{ $$ = CURSOR_OPT_HOLD; }
			| WITHOUT HOLD					{ $$ = 0; }
		;

/*****************************************************************************
 *
 *		QUERY:
 *				SELECT STATEMENTS
 *
 * 查询: SELECT 语句
 *****************************************************************************/

/* A complete SELECT statement looks like this.
 *
 * The rule returns either a single SelectStmt node or a tree of them,
 * representing a set-operation tree.
 *
 * There is an ambiguity when a sub-SELECT is within an a_expr and there
 * are excess parentheses: do the parentheses belong to the sub-SELECT or
 * to the surrounding a_expr?  We don't really care, but bison wants to know.
 * To resolve the ambiguity, we are careful to define the grammar so that
 * the decision is staved off as long as possible: as long as we can keep
 * absorbing parentheses into the sub-SELECT, we will do so, and only when
 * it's no longer possible to do that will we decide that parens belong to
 * the expression.	For example, in "SELECT (((SELECT 2)) + 3)" the extra
 * parentheses are treated as part of the sub-select.  The necessity of doing
 * it that way is shown by "SELECT (((SELECT 2)) UNION SELECT 2)".	Had we
 * parsed "((SELECT 2))" as an a_expr, it'd be too late to go back to the
 * SELECT viewpoint when we see the UNION.
 *
 * This approach is implemented by defining a nonterminal select_with_parens,
 * which represents a SELECT with at least one outer layer of parentheses,
 * and being careful to use select_with_parens, never '(' SelectStmt ')',
 * in the expression grammar.  We will then have shift-reduce conflicts
 * which we can resolve in favor of always treating '(' <select> ')' as
 * a select_with_parens.  To resolve the conflicts, the productions that
 * conflict with the select_with_parens productions are manually given
 * precedences lower than the precedence of ')', thereby ensuring that we
 * shift ')' (and then reduce to select_with_parens) rather than trying to
 * reduce the inner <select> nonterminal to something else.  We use UMINUS
 * precedence for this, which is a fairly arbitrary choice.
 *
 * To be able to define select_with_parens itself without ambiguity, we need
 * a nonterminal select_no_parens that represents a SELECT structure with no
 * outermost parentheses.  This is a little bit tedious, but it works.
 *
 * In non-expression contexts, we use SelectStmt which can represent a SELECT
 * with or without outer parentheses.
 * 一个完整的 SELECT 语句结构如下。该规则返回单个 SelectStmt 节点或它们组成的树（表示集合操作树）。当子查询（sub-SELECT）处于 a_expr 中且有多余括号时，会产生歧义：这些括号是属于子查询还是属于外层的 a_expr？我们并不真正关心，但 bison 需要知道。为了解决这个歧义，我们仔细定义了语法，使得做决定的时间尽可能往后推迟：只要我们能继续将括号吸收到子查询中，我们就会这样做，只有当无法再这样做时，我们才会断定括号属于表达式。例如，在 "SELECT (((SELECT 2)) + 3)" 中，多余的括号被视作子查询的一部分。这种处理方式的必要性由 "SELECT (((SELECT 2)) UNION SELECT 2)" 来说明。如果我们把 "((SELECT 2))" 解析为一个 a_expr，那么等我们看到 UNION 时再退回到 SELECT 视角就太迟了。这一方案的实现方式是：定义一个非终结符 select_with_parens，它表示至少有一层外层括号的 SELECT，并在表达式语法中小心地使用 select_with_parens，而绝不使用 '(' SelectStmt ')'。这样我们就会遇到 移进-规约 (shift-reduce) 冲突，我们可以通过总是将 '(' <select> ')' 视为 select_with_parens 来解决这一冲突。为了解决这些冲突，与 select_with_parens 产生式冲突的其他产生式会被手动赋予比 ')' 更低的优先级，从而确保我们移进 ')'（然后规约为 select_with_parens）而不是尝试将内部的 <select> 非终结符规约为其他东西。为此我们使用了 UMINUS 优先级，这是一个相当任意的选择。为了能毫无歧义地定义 select_with_parens 本身，我们需要一个非终结符 select_no_parens，它代表没有最外层括号的 SELECT 结构。这稍微有点繁琐，但是可行。在非表达式上下文中，我们使用 SelectStmt，它可以表示带有或不带有外层括号的 SELECT。
 */

SelectStmt: select_no_parens			%prec UMINUS
			| select_with_parens		%prec UMINUS
		;

select_with_parens:
			'(' select_no_parens ')'				{ $$ = $2; }
			| '(' select_with_parens ')'			{ $$ = $2; }
		;

/*
 * This rule parses the equivalent of the standard's <query expression>.
 * The duplicative productions are annoying, but hard to get rid of without
 * creating shift/reduce conflicts.
 *
 *	The locking clause (FOR UPDATE etc) may be before or after LIMIT/OFFSET.
 *	In <=7.2.X, LIMIT/OFFSET had to be after FOR UPDATE
 *	We now support both orderings, but prefer LIMIT/OFFSET before the locking
 * clause.
 *	2002-08-28 bjm
 * 此规则解析等价于 SQL 标准中的 <查询表达式> (query expression)。重复的产生式很烦人，但若不创建 移进/规约 (shift/reduce) 冲突，就很难消除它们。锁定子句（如 FOR UPDATE 等）可以在 LIMIT/OFFSET 之前或之后。在 <=7.2.X 中，LIMIT/OFFSET 必须在 FOR UPDATE 之后。我们现在支持这两种顺序，但更倾向于将 LIMIT/OFFSET 放在锁定子句之前。 2002-08-28 bjm
 */
select_no_parens:
			simple_select						{ $$ = $1; }
			| select_clause sort_clause
				{
					insertSelectOptions((SelectStmt *) $1, $2, NIL,
										NULL, NULL,
										yyscanner);
					$$ = $1;
				}
			| select_clause opt_sort_clause for_locking_clause opt_select_limit
				{
					insertSelectOptions((SelectStmt *) $1, $2, $3,
										$4,
										NULL,
										yyscanner);
					$$ = $1;
				}
			| select_clause opt_sort_clause select_limit opt_for_locking_clause
				{
					insertSelectOptions((SelectStmt *) $1, $2, $4,
										$3,
										NULL,
										yyscanner);
					$$ = $1;
				}
			| with_clause select_clause
				{
					insertSelectOptions((SelectStmt *) $2, NULL, NIL,
										NULL,
										$1,
										yyscanner);
					$$ = $2;
				}
			| with_clause select_clause sort_clause
				{
					insertSelectOptions((SelectStmt *) $2, $3, NIL,
										NULL,
										$1,
										yyscanner);
					$$ = $2;
				}
			| with_clause select_clause opt_sort_clause for_locking_clause opt_select_limit
				{
					insertSelectOptions((SelectStmt *) $2, $3, $4,
										$5,
										$1,
										yyscanner);
					$$ = $2;
				}
			| with_clause select_clause opt_sort_clause select_limit opt_for_locking_clause
				{
					insertSelectOptions((SelectStmt *) $2, $3, $5,
										$4,
										$1,
										yyscanner);
					$$ = $2;
				}
		;

select_clause:
			simple_select							{ $$ = $1; }
			| select_with_parens					{ $$ = $1; }
		;

/*
 * This rule parses SELECT statements that can appear within set operations,
 * including UNION, INTERSECT and EXCEPT.  '(' and ')' can be used to specify
 * the ordering of the set operations.	Without '(' and ')' we want the
 * operations to be ordered per the precedence specs at the head of this file.
 *
 * As with select_no_parens, simple_select cannot have outer parentheses,
 * but can have parenthesized subclauses.
 *
 * It might appear that we could fold the first two alternatives into one
 * by using opt_distinct_clause.  However, that causes a shift/reduce conflict
 * against INSERT ... SELECT ... ON CONFLICT.  We avoid the ambiguity by
 * requiring SELECT DISTINCT [ON] to be followed by a non-empty target_list.
 *
 * Note that sort clauses cannot be included at this level --- SQL requires
 *		SELECT foo UNION SELECT bar ORDER BY baz
 * to be parsed as
 *		(SELECT foo UNION SELECT bar) ORDER BY baz
 * not
 *		SELECT foo UNION (SELECT bar ORDER BY baz)
 * Likewise for WITH, FOR UPDATE and LIMIT.  Therefore, those clauses are
 * described as part of the select_no_parens production, not simple_select.
 * This does not limit functionality, because you can reintroduce these
 * clauses inside parentheses.
 *
 * NOTE: only the leftmost component SelectStmt should have INTO.
 * However, this is not checked by the grammar; parse analysis must check it.
 * 此规则解析可出现在集合操作（包括 UNION、INTERSECT 和 EXCEPT）中的 SELECT 语句。'(' 和 ')' 可用于指定集合操作的顺序。如果没有 '(' 和 ')'，我们希望按照本文件开头的优先级说明来排列操作顺序。与 select_no_parens 类似，simple_select 不能包含外层括号，但可以包含带括号的子句。看起来我们似乎可以通过使用 opt_distinct_clause 将前两个可选项合并为一个。然而，这会与 INSERT ... SELECT ... ON CONFLICT 产生移进/规约冲突。我们通过要求 SELECT DISTINCT [ON] 后面必须跟一个非空的 target_list 来避免这种歧义。请注意，在此级别不能包含排序子句 —— SQL 要求将 SELECT foo UNION SELECT bar ORDER BY baz 解析为 (SELECT foo UNION SELECT bar) ORDER BY baz，而不是 SELECT foo UNION (SELECT bar ORDER BY baz)。对于 WITH、FOR UPDATE 和 LIMIT 也是如此。因此，这些子句被描述为 select_no_parens 产生式的一部分，而不是 simple_select。这并不会限制功能，因为您可以在括号内重新引入这些子句。注意：只有最左边的 SelectStmt 组件应该有 INTO。然而，语法本身不对此进行检查；解析分析必须检查它。
 */
simple_select:
			SELECT opt_all_clause opt_target_list
			into_clause from_clause where_clause
			group_clause having_clause window_clause
				{
					SelectStmt *n = makeNode(SelectStmt);

					n->targetList = $3;
					n->intoClause = $4;
					n->fromClause = $5;
					n->whereClause = $6;
					n->groupClause = ($7)->list;
					n->groupDistinct = ($7)->distinct;
					n->havingClause = $8;
					n->windowClause = $9;
					$$ = (Node *) n;
				}
			| SELECT distinct_clause target_list
			into_clause from_clause where_clause
			group_clause having_clause window_clause
				{
					SelectStmt *n = makeNode(SelectStmt);

					n->distinctClause = $2;
					n->targetList = $3;
					n->intoClause = $4;
					n->fromClause = $5;
					n->whereClause = $6;
					n->groupClause = ($7)->list;
					n->groupDistinct = ($7)->distinct;
					n->havingClause = $8;
					n->windowClause = $9;
					$$ = (Node *) n;
				}
			| values_clause							{ $$ = $1; }
			| TABLE relation_expr
				{
					/* same as SELECT * FROM relation_expr - 等同于 SELECT * FROM relation_expr */
					ColumnRef  *cr = makeNode(ColumnRef);
					ResTarget  *rt = makeNode(ResTarget);
					SelectStmt *n = makeNode(SelectStmt);

					cr->fields = list_make1(makeNode(A_Star));
					cr->location = -1;

					rt->name = NULL;
					rt->indirection = NIL;
					rt->val = (Node *) cr;
					rt->location = -1;

					n->targetList = list_make1(rt);
					n->fromClause = list_make1($2);
					$$ = (Node *) n;
				}
			| select_clause UNION set_quantifier select_clause
				{
					$$ = makeSetOp(SETOP_UNION, $3 == SET_QUANTIFIER_ALL, $1, $4);
				}
			| select_clause INTERSECT set_quantifier select_clause
				{
					$$ = makeSetOp(SETOP_INTERSECT, $3 == SET_QUANTIFIER_ALL, $1, $4);
				}
			| select_clause EXCEPT set_quantifier select_clause
				{
					$$ = makeSetOp(SETOP_EXCEPT, $3 == SET_QUANTIFIER_ALL, $1, $4);
				}
		;

/*
 * SQL standard WITH clause looks like:
 *
 * WITH [ RECURSIVE ] <query name> [ (<column>,...) ]
 *		AS (query) [ SEARCH or CYCLE clause ]
 *
 * Recognizing WITH_LA here allows a CTE to be named TIME or ORDINALITY.
 * SQL 标准的 WITH 子句形如：WITH [ RECURSIVE ] <查询名> [ (<列名>,...) ] AS (查询) [ SEARCH 或 CYCLE 子句 ] 在此处识别 WITH_LA 允许将 CTE 命名为 TIME 或 ORDINALITY。
 */
with_clause:
		WITH cte_list
			{
				$$ = makeNode(WithClause);
				$$->ctes = $2;
				$$->recursive = false;
				$$->location = @1;
			}
		| WITH_LA cte_list
			{
				$$ = makeNode(WithClause);
				$$->ctes = $2;
				$$->recursive = false;
				$$->location = @1;
			}
		| WITH RECURSIVE cte_list
			{
				$$ = makeNode(WithClause);
				$$->ctes = $3;
				$$->recursive = true;
				$$->location = @1;
			}
		;

cte_list:
		common_table_expr						{ $$ = list_make1($1); }
		| cte_list ',' common_table_expr		{ $$ = lappend($1, $3); }
		;

common_table_expr:  name opt_name_list AS opt_materialized '(' PreparableStmt ')' opt_search_clause opt_cycle_clause
			{
				CommonTableExpr *n = makeNode(CommonTableExpr);

				n->ctename = $1;
				n->aliascolnames = $2;
				n->ctematerialized = $4;
				n->ctequery = $6;
				n->search_clause = castNode(CTESearchClause, $8);
				n->cycle_clause = castNode(CTECycleClause, $9);
				n->location = @1;
				$$ = (Node *) n;
			}
		;

opt_materialized:
		MATERIALIZED							{ $$ = CTEMaterializeAlways; }
		| NOT MATERIALIZED						{ $$ = CTEMaterializeNever; }
		| /* EMPTY - 空 */								{ $$ = CTEMaterializeDefault; }
		;

opt_search_clause:
		SEARCH DEPTH FIRST_P BY columnList SET ColId
			{
				CTESearchClause *n = makeNode(CTESearchClause);

				n->search_col_list = $5;
				n->search_breadth_first = false;
				n->search_seq_column = $7;
				n->location = @1;
				$$ = (Node *) n;
			}
		| SEARCH BREADTH FIRST_P BY columnList SET ColId
			{
				CTESearchClause *n = makeNode(CTESearchClause);

				n->search_col_list = $5;
				n->search_breadth_first = true;
				n->search_seq_column = $7;
				n->location = @1;
				$$ = (Node *) n;
			}
		| /* EMPTY - 空 */
			{
				$$ = NULL;
			}
		;

opt_cycle_clause:
		CYCLE columnList SET ColId TO AexprConst DEFAULT AexprConst USING ColId
			{
				CTECycleClause *n = makeNode(CTECycleClause);

				n->cycle_col_list = $2;
				n->cycle_mark_column = $4;
				n->cycle_mark_value = $6;
				n->cycle_mark_default = $8;
				n->cycle_path_column = $10;
				n->location = @1;
				$$ = (Node *) n;
			}
		| CYCLE columnList SET ColId USING ColId
			{
				CTECycleClause *n = makeNode(CTECycleClause);

				n->cycle_col_list = $2;
				n->cycle_mark_column = $4;
				n->cycle_mark_value = makeBoolAConst(true, -1);
				n->cycle_mark_default = makeBoolAConst(false, -1);
				n->cycle_path_column = $6;
				n->location = @1;
				$$ = (Node *) n;
			}
		| /* EMPTY - 空 */
			{
				$$ = NULL;
			}
		;

opt_with_clause:
		with_clause								{ $$ = $1; }
		| /* EMPTY - 空 */								{ $$ = NULL; }
		;

into_clause:
			INTO OptTempTableName
				{
					$$ = makeNode(IntoClause);
					$$->rel = $2;
					$$->colNames = NIL;
					$$->options = NIL;
					$$->onCommit = ONCOMMIT_NOOP;
					$$->tableSpaceName = NULL;
					$$->viewQuery = NULL;
					$$->skipData = false;
				}
			| /* EMPTY - 空 */
				{ $$ = NULL; }
		;

/*
 * Redundancy here is needed to avoid shift/reduce conflicts,
 * since TEMP is not a reserved word.  See also OptTemp.
 * 由于 TEMP 不是保留字，因此需要此处的冗余以避免移进/规约冲突。另请参见 OptTemp。
 */
OptTempTableName:
			TEMPORARY opt_table qualified_name
				{
					$$ = $3;
					$$->relpersistence = RELPERSISTENCE_TEMP;
				}
			| TEMP opt_table qualified_name
				{
					$$ = $3;
					$$->relpersistence = RELPERSISTENCE_TEMP;
				}
			| LOCAL TEMPORARY opt_table qualified_name
				{
					$$ = $4;
					$$->relpersistence = RELPERSISTENCE_TEMP;
				}
			| LOCAL TEMP opt_table qualified_name
				{
					$$ = $4;
					$$->relpersistence = RELPERSISTENCE_TEMP;
				}
			| GLOBAL TEMPORARY opt_table qualified_name
				{
					ereport(WARNING,
							(errmsg("GLOBAL is deprecated in temporary table creation"),
							 parser_errposition(@1)));
					$$ = $4;
					$$->relpersistence = RELPERSISTENCE_TEMP;
				}
			| GLOBAL TEMP opt_table qualified_name
				{
					ereport(WARNING,
							(errmsg("GLOBAL is deprecated in temporary table creation"),
							 parser_errposition(@1)));
					$$ = $4;
					$$->relpersistence = RELPERSISTENCE_TEMP;
				}
			| UNLOGGED opt_table qualified_name
				{
					$$ = $3;
					$$->relpersistence = RELPERSISTENCE_UNLOGGED;
				}
			| TABLE qualified_name
				{
					$$ = $2;
					$$->relpersistence = RELPERSISTENCE_PERMANENT;
				}
			| qualified_name
				{
					$$ = $1;
					$$->relpersistence = RELPERSISTENCE_PERMANENT;
				}
		;

opt_table:	TABLE
			| /* EMPTY - 空 */
		;

set_quantifier:
			ALL										{ $$ = SET_QUANTIFIER_ALL; }
			| DISTINCT								{ $$ = SET_QUANTIFIER_DISTINCT; }
			| /* EMPTY - 空 */								{ $$ = SET_QUANTIFIER_DEFAULT; }
		;

/* We use (NIL) as a placeholder to indicate that all target expressions
 * should be placed in the DISTINCT list during parsetree analysis.
 * 我们使用 (NIL) 作为占位符，以指示在解析树分析期间应将所有目标表达式放入 DISTINCT 列表中。
 */
distinct_clause:
			DISTINCT								{ $$ = list_make1(NIL); }
			| DISTINCT ON '(' expr_list ')'			{ $$ = $4; }
		;

opt_all_clause:
			ALL
			| /* EMPTY - 空 */
		;

opt_distinct_clause:
			distinct_clause							{ $$ = $1; }
			| opt_all_clause						{ $$ = NIL; }
		;

opt_sort_clause:
			sort_clause								{ $$ = $1; }
			| /* EMPTY - 空 */								{ $$ = NIL; }
		;

sort_clause:
			ORDER BY sortby_list					{ $$ = $3; }
		;

sortby_list:
			sortby									{ $$ = list_make1($1); }
			| sortby_list ',' sortby				{ $$ = lappend($1, $3); }
		;

sortby:		a_expr USING qual_all_Op opt_nulls_order
				{
					$$ = makeNode(SortBy);
					$$->node = $1;
					$$->sortby_dir = SORTBY_USING;
					$$->sortby_nulls = $4;
					$$->useOp = $3;
					$$->location = @3;
				}
			| a_expr opt_asc_desc opt_nulls_order
				{
					$$ = makeNode(SortBy);
					$$->node = $1;
					$$->sortby_dir = $2;
					$$->sortby_nulls = $3;
					$$->useOp = NIL;
					$$->location = -1;		/* no operator - 无操作符 */
				}
		;


select_limit:
			limit_clause offset_clause
				{
					$$ = $1;
					($$)->limitOffset = $2;
					($$)->offsetLoc = @2;
				}
			| offset_clause limit_clause
				{
					$$ = $2;
					($$)->limitOffset = $1;
					($$)->offsetLoc = @1;
				}
			| limit_clause
				{
					$$ = $1;
				}
			| offset_clause
				{
					SelectLimit *n = (SelectLimit *) palloc(sizeof(SelectLimit));

					n->limitOffset = $1;
					n->limitCount = NULL;
					n->limitOption = LIMIT_OPTION_COUNT;
					n->offsetLoc = @1;
					n->countLoc = -1;
					n->optionLoc = -1;
					$$ = n;
				}
		;

opt_select_limit:
			select_limit						{ $$ = $1; }
			| /* EMPTY - 空 */						{ $$ = NULL; }
		;

limit_clause:
			LIMIT select_limit_value
				{
					SelectLimit *n = (SelectLimit *) palloc(sizeof(SelectLimit));

					n->limitOffset = NULL;
					n->limitCount = $2;
					n->limitOption = LIMIT_OPTION_COUNT;
					n->offsetLoc = -1;
					n->countLoc = @1;
					n->optionLoc = -1;
					$$ = n;
				}
			| LIMIT select_limit_value ',' select_offset_value
				{
					/* Disabled because it was too confusing, bjm 2002-02-18 - 已禁用，因为太容易混淆，bjm 2002-02-18 */
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("LIMIT #,# syntax is not supported"),
							 errhint("Use separate LIMIT and OFFSET clauses."),
							 parser_errposition(@1)));
				}
			/* SQL:2008 syntax - SQL:2008 语法 */
			/* to avoid shift/reduce conflicts, handle the optional value with
			 * a separate production rather than an opt_ expression.  The fact
			 * that ONLY is fully reserved means that this way, we defer any
			 * decision about what rule reduces ROW or ROWS to the point where
			 * we can see the ONLY token in the lookahead slot.
			 * 为避免移进/规约冲突，用一个单独的产生式来处理可选值，而不是 opt_ 表达式。ONLY 是完全保留字这一事实意味着，通过这种方式，我们可以将决定由哪个规则来规约 ROW 或 ROWS 推迟到我们能在前瞻槽 (lookahead slot) 中看到 ONLY 标记的时刻。
			 */
			| FETCH first_or_next select_fetch_first_value row_or_rows ONLY
				{
					SelectLimit *n = (SelectLimit *) palloc(sizeof(SelectLimit));

					n->limitOffset = NULL;
					n->limitCount = $3;
					n->limitOption = LIMIT_OPTION_COUNT;
					n->offsetLoc = -1;
					n->countLoc = @1;
					n->optionLoc = -1;
					$$ = n;
				}
			| FETCH first_or_next select_fetch_first_value row_or_rows WITH TIES
				{
					SelectLimit *n = (SelectLimit *) palloc(sizeof(SelectLimit));

					n->limitOffset = NULL;
					n->limitCount = $3;
					n->limitOption = LIMIT_OPTION_WITH_TIES;
					n->offsetLoc = -1;
					n->countLoc = @1;
					n->optionLoc = @5;
					$$ = n;
				}
			| FETCH first_or_next row_or_rows ONLY
				{
					SelectLimit *n = (SelectLimit *) palloc(sizeof(SelectLimit));

					n->limitOffset = NULL;
					n->limitCount = makeIntConst(1, -1);
					n->limitOption = LIMIT_OPTION_COUNT;
					n->offsetLoc = -1;
					n->countLoc = @1;
					n->optionLoc = -1;
					$$ = n;
				}
			| FETCH first_or_next row_or_rows WITH TIES
				{
					SelectLimit *n = (SelectLimit *) palloc(sizeof(SelectLimit));

					n->limitOffset = NULL;
					n->limitCount = makeIntConst(1, -1);
					n->limitOption = LIMIT_OPTION_WITH_TIES;
					n->offsetLoc = -1;
					n->countLoc = @1;
					n->optionLoc = @4;
					$$ = n;
				}
		;

offset_clause:
			OFFSET select_offset_value
				{ $$ = $2; }
			/* SQL:2008 syntax - SQL:2008 语法 */
			| OFFSET select_fetch_first_value row_or_rows
				{ $$ = $2; }
		;

select_limit_value:
			a_expr									{ $$ = $1; }
			| ALL
				{
					/* LIMIT ALL is represented as a NULL constant - LIMIT ALL 被表示为一个 NULL 常量 */
					$$ = makeNullAConst(@1);
				}
		;

select_offset_value:
			a_expr									{ $$ = $1; }
		;

/*
 * Allowing full expressions without parentheses causes various parsing
 * problems with the trailing ROW/ROWS key words.  SQL spec only calls for
 * <simple value specification>, which is either a literal or a parameter (but
 * an <SQL parameter reference> could be an identifier, bringing up conflicts
 * with ROW/ROWS). We solve this by leveraging the presence of ONLY (see above)
 * to determine whether the expression is missing rather than trying to make it
 * optional in this rule.
 *
 * c_expr covers almost all the spec-required cases (and more), but it doesn't
 * cover signed numeric literals, which are allowed by the spec. So we include
 * those here explicitly. We need FCONST as well as ICONST because values that
 * don't fit in the platform's "long", but do fit in bigint, should still be
 * accepted here. (This is possible in 64-bit Windows as well as all 32-bit
 * builds.)
 * 允许不带括号的完整表达式会导致与尾随的 ROW/ROWS 关键字发生各种解析问题。SQL 规范仅要求 <简单值规范>，这可以是字面量或参数（但 <SQL 参数引用> 可能是标识符，从而与 ROW/ROWS 产生冲突）。我们通过利用 ONLY 的存在（参见上文）来确定表达式是否缺失，而不是试图在此规则中使其成为可选的，从而解决了这个问题。c_expr 涵盖了几乎所有规范要求的案例（以及更多），但它不涵盖规范允许的有符号数字字面量。因此我们在此处明确地包含它们。我们既需要 FCONST 也需要 ICONST，因为那些在平台的 "long" 中放不下、但在 bigint 中能放下的值，在这里仍应该被接受。（这在 64 位 Windows 以及所有 32 位构建中都是可能的。）
 */
select_fetch_first_value:
			c_expr									{ $$ = $1; }
			| '+' I_or_F_const
				{ $$ = (Node *) makeSimpleA_Expr(AEXPR_OP, "+", NULL, $2, @1); }
			| '-' I_or_F_const
				{ $$ = doNegate($2, @1); }
		;

I_or_F_const:
			Iconst									{ $$ = makeIntConst($1,@1); }
			| FCONST								{ $$ = makeFloatConst($1,@1); }
		;

/* noise words - 噪词/无义词 */
row_or_rows: ROW									{ $$ = 0; }
			| ROWS									{ $$ = 0; }
		;

first_or_next: FIRST_P								{ $$ = 0; }
			| NEXT									{ $$ = 0; }
		;


/*
 * This syntax for group_clause tries to follow the spec quite closely.
 * However, the spec allows only column references, not expressions,
 * which introduces an ambiguity between implicit row constructors
 * (a,b) and lists of column references.
 *
 * We handle this by using the a_expr production for what the spec calls
 * <ordinary grouping set>, which in the spec represents either one column
 * reference or a parenthesized list of column references. Then, we check the
 * top node of the a_expr to see if it's an implicit RowExpr, and if so, just
 * grab and use the list, discarding the node. (this is done in parse analysis,
 * not here)
 *
 * (we abuse the row_format field of RowExpr to distinguish implicit and
 * explicit row constructors; it's debatable if anyone sanely wants to use them
 * in a group clause, but if they have a reason to, we make it possible.)
 *
 * Each item in the group_clause list is either an expression tree or a
 * GroupingSet node of some type.
 * 此 group_clause 语法试图非常紧密地遵循规范。然而，规范仅允许列引用，而不允许表达式，这在隐式行构造器 (a,b) 与列引用列表之间引入了歧义。我们通过对规范中称为 <普通分组集> (ordinary grouping set) 的部分使用 a_expr 产生式来处理，这在规范中代表一个列引用或一个带括号的列引用列表。然后，我们检查 a_expr 的顶级节点以查看它是否是一个隐式 RowExpr，如果是的话，就直接获取并使用该列表，并丢弃该节点。（这是在解析分析中完成的，而不是在这里）（我们滥用了 RowExpr 的 row_format 字段来区分隐式和显式行构造器；任何人是否想要在分组子句中使用它们是有争议的，但如果他们有理由这么做，我们使其成为可能。）group_clause 列表中的每一项都是一个表达式树或某种类型的 GroupingSet 节点。
 */
group_clause:
			GROUP_P BY set_quantifier group_by_list
				{
					GroupClause *n = (GroupClause *) palloc(sizeof(GroupClause));

					n->distinct = $3 == SET_QUANTIFIER_DISTINCT;
					n->list = $4;
					$$ = n;
				}
			| /* EMPTY - 空 */
				{
					GroupClause *n = (GroupClause *) palloc(sizeof(GroupClause));

					n->distinct = false;
					n->list = NIL;
					$$ = n;
				}
		;

group_by_list:
			group_by_item							{ $$ = list_make1($1); }
			| group_by_list ',' group_by_item		{ $$ = lappend($1,$3); }
		;

group_by_item:
			a_expr									{ $$ = $1; }
			| empty_grouping_set					{ $$ = $1; }
			| cube_clause							{ $$ = $1; }
			| rollup_clause							{ $$ = $1; }
			| grouping_sets_clause					{ $$ = $1; }
		;

empty_grouping_set:
			'(' ')'
				{
					$$ = (Node *) makeGroupingSet(GROUPING_SET_EMPTY, NIL, @1);
				}
		;

/*
 * These hacks rely on setting precedence of CUBE and ROLLUP below that of '(',
 * so that they shift in these rules rather than reducing the conflicting
 * unreserved_keyword rule.
 * 这些黑客手段（hacks）依赖于将 CUBE 和 ROLLUP 的优先级设置为低于 '(' 的优先级，从而使得它们在这些规则中进行移进，而不是规约冲突的 unreserved_keyword 规则。
 */

rollup_clause:
			ROLLUP '(' expr_list ')'
				{
					$$ = (Node *) makeGroupingSet(GROUPING_SET_ROLLUP, $3, @1);
				}
		;

cube_clause:
			CUBE '(' expr_list ')'
				{
					$$ = (Node *) makeGroupingSet(GROUPING_SET_CUBE, $3, @1);
				}
		;

grouping_sets_clause:
			GROUPING SETS '(' group_by_list ')'
				{
					$$ = (Node *) makeGroupingSet(GROUPING_SET_SETS, $4, @1);
				}
		;

having_clause:
			HAVING a_expr							{ $$ = $2; }
			| /* EMPTY - 空 */								{ $$ = NULL; }
		;

for_locking_clause:
			for_locking_items						{ $$ = $1; }
			| FOR READ ONLY							{ $$ = NIL; }
		;

opt_for_locking_clause:
			for_locking_clause						{ $$ = $1; }
			| /* EMPTY - 空 */							{ $$ = NIL; }
		;

for_locking_items:
			for_locking_item						{ $$ = list_make1($1); }
			| for_locking_items for_locking_item	{ $$ = lappend($1, $2); }
		;

for_locking_item:
			for_locking_strength locked_rels_list opt_nowait_or_skip
				{
					LockingClause *n = makeNode(LockingClause);

					n->lockedRels = $2;
					n->strength = $1;
					n->waitPolicy = $3;
					$$ = (Node *) n;
				}
		;

for_locking_strength:
			FOR UPDATE							{ $$ = LCS_FORUPDATE; }
			| FOR NO KEY UPDATE					{ $$ = LCS_FORNOKEYUPDATE; }
			| FOR SHARE							{ $$ = LCS_FORSHARE; }
			| FOR KEY SHARE						{ $$ = LCS_FORKEYSHARE; }
		;

locked_rels_list:
			OF qualified_name_list					{ $$ = $2; }
			| /* EMPTY - 空 */							{ $$ = NIL; }
		;


/*
 * We should allow ROW '(' expr_list ')' too, but that seems to require
 * making VALUES a fully reserved word, which will probably break more apps
 * than allowing the noise-word is worth.
 * 我们也应该允许 ROW '(' expr_list ')'，但这似乎需要将 VALUES 设为完全保留字，这可能会破坏比允许该噪词所值更多的应用程序。
 */
values_clause:
			VALUES '(' expr_list ')'
				{
					SelectStmt *n = makeNode(SelectStmt);

					n->valuesLists = list_make1($3);
					$$ = (Node *) n;
				}
			| values_clause ',' '(' expr_list ')'
				{
					SelectStmt *n = (SelectStmt *) $1;

					n->valuesLists = lappend(n->valuesLists, $4);
					$$ = (Node *) n;
				}
		;


/*****************************************************************************
 *
 *	clauses common to all Optimizable Stmts:
 *		from_clause		- allow list of both JOIN expressions and table names
 *		where_clause	- qualifications for joins or restrictions
 *
 * 所有可优化语句的通用子句：from_clause - 允许包含 JOIN 表达式和表名的列表 where_clause - 用于连接的限定条件或限制条件
 *****************************************************************************/

from_clause:
			FROM from_list							{ $$ = $2; }
			| /* EMPTY - 空 */								{ $$ = NIL; }
		;

from_list:
			table_ref								{ $$ = list_make1($1); }
			| from_list ',' table_ref				{ $$ = lappend($1, $3); }
		;

/*
 * table_ref is where an alias clause can be attached.
 * table_ref 是可以附加别名子句的地方。
 */
table_ref:	relation_expr opt_alias_clause
				{
					$1->alias = $2;
					$$ = (Node *) $1;
				}
			| relation_expr opt_alias_clause tablesample_clause
				{
					RangeTableSample *n = (RangeTableSample *) $3;

					$1->alias = $2;
					/* relation_expr goes inside the RangeTableSample node - relation_expr 进入 RangeTableSample 节点内部 */
					n->relation = (Node *) $1;
					$$ = (Node *) n;
				}
			| func_table func_alias_clause
				{
					RangeFunction *n = (RangeFunction *) $1;

					n->alias = linitial($2);
					n->coldeflist = lsecond($2);
					$$ = (Node *) n;
				}
			| LATERAL_P func_table func_alias_clause
				{
					RangeFunction *n = (RangeFunction *) $2;

					n->lateral = true;
					n->alias = linitial($3);
					n->coldeflist = lsecond($3);
					$$ = (Node *) n;
				}
			| xmltable opt_alias_clause
				{
					RangeTableFunc *n = (RangeTableFunc *) $1;

					n->alias = $2;
					$$ = (Node *) n;
				}
			| LATERAL_P xmltable opt_alias_clause
				{
					RangeTableFunc *n = (RangeTableFunc *) $2;

					n->lateral = true;
					n->alias = $3;
					$$ = (Node *) n;
				}
			| select_with_parens opt_alias_clause
				{
					RangeSubselect *n = makeNode(RangeSubselect);

					n->lateral = false;
					n->subquery = $1;
					n->alias = $2;
					$$ = (Node *) n;
				}
			| LATERAL_P select_with_parens opt_alias_clause
				{
					RangeSubselect *n = makeNode(RangeSubselect);

					n->lateral = true;
					n->subquery = $2;
					n->alias = $3;
					$$ = (Node *) n;
				}
			| joined_table
				{
					$$ = (Node *) $1;
				}
			| '(' joined_table ')' alias_clause
				{
					$2->alias = $4;
					$$ = (Node *) $2;
				}
			| json_table opt_alias_clause
				{
					JsonTable  *jt = castNode(JsonTable, $1);

					jt->alias = $2;
					$$ = (Node *) jt;
				}
			| LATERAL_P json_table opt_alias_clause
				{
					JsonTable  *jt = castNode(JsonTable, $2);

					jt->alias = $3;
					jt->lateral = true;
					$$ = (Node *) jt;
				}
		;


/*
 * It may seem silly to separate joined_table from table_ref, but there is
 * method in SQL's madness: if you don't do it this way you get reduce-
 * reduce conflicts, because it's not clear to the parser generator whether
 * to expect alias_clause after ')' or not.  For the same reason we must
 * treat 'JOIN' and 'join_type JOIN' separately, rather than allowing
 * join_type to expand to empty; if we try it, the parser generator can't
 * figure out when to reduce an empty join_type right after table_ref.
 *
 * Note that a CROSS JOIN is the same as an unqualified
 * INNER JOIN, and an INNER JOIN/ON has the same shape
 * but a qualification expression to limit membership.
 * A NATURAL JOIN implicitly matches column names between
 * tables and the shape is determined by which columns are
 * in common. We'll collect columns during the later transformations.
 * 将 joined_table 从 table_ref 中分离出来似乎很愚蠢，但 SQL 的疯狂中是有章法的：如果不这样做，就会产生 规约-规约 (reduce-reduce) 冲突，因为解析器生成器不清楚在 ')' 之后是否应该期待 alias_clause。出于同样的原因，我们必须将 'JOIN' 和 'join_type JOIN' 分开处理，而不是允许 join_type 扩展为空；如果我们尝试空扩展，解析器生成器就无法知道何时在 table_ref 之后立即规约一个空的 join_type。注意，CROSS JOIN 与不带限定条件的 INNER JOIN 相同，而带有 ON 的 INNER JOIN 结构相同但带有限定表达式以限制成员。NATURAL JOIN 会隐式匹配表之间的列名，其结构由公共列决定。我们将在后面的转换中收集列。
 */

joined_table:
			'(' joined_table ')'
				{
					$$ = $2;
				}
			| table_ref CROSS JOIN table_ref
				{
					/* CROSS JOIN is same as unqualified inner join - CROSS JOIN 与不带限定条件的 inner join 相同 */
					JoinExpr   *n = makeNode(JoinExpr);

					n->jointype = JOIN_INNER;
					n->isNatural = false;
					n->larg = $1;
					n->rarg = $4;
					n->usingClause = NIL;
					n->join_using_alias = NULL;
					n->quals = NULL;
					$$ = n;
				}
			| table_ref join_type JOIN table_ref join_qual
				{
					JoinExpr   *n = makeNode(JoinExpr);

					n->jointype = $2;
					n->isNatural = false;
					n->larg = $1;
					n->rarg = $4;
					if ($5 != NULL && IsA($5, List))
					{
						 /* USING clause - USING 子句 */
						n->usingClause = linitial_node(List, castNode(List, $5));
						n->join_using_alias = lsecond_node(Alias, castNode(List, $5));
					}
					else
					{
						/* ON clause - ON 子句 */
						n->quals = $5;
					}
					$$ = n;
				}
			| table_ref JOIN table_ref join_qual
				{
					/* letting join_type reduce to empty doesn't work - 允许 join_type 规约为空不起作用 */
					JoinExpr   *n = makeNode(JoinExpr);

					n->jointype = JOIN_INNER;
					n->isNatural = false;
					n->larg = $1;
					n->rarg = $3;
					if ($4 != NULL && IsA($4, List))
					{
						/* USING clause - USING 子句 */
						n->usingClause = linitial_node(List, castNode(List, $4));
						n->join_using_alias = lsecond_node(Alias, castNode(List, $4));
					}
					else
					{
						/* ON clause - ON 子句 */
						n->quals = $4;
					}
					$$ = n;
				}
			| table_ref NATURAL join_type JOIN table_ref
				{
					JoinExpr   *n = makeNode(JoinExpr);

					n->jointype = $3;
					n->isNatural = true;
					n->larg = $1;
					n->rarg = $5;
					n->usingClause = NIL; /* figure out which columns later... - 稍后确定哪些列... */
					n->join_using_alias = NULL;
					n->quals = NULL; /* fill later - 稍后填充 */
					$$ = n;
				}
			| table_ref NATURAL JOIN table_ref
				{
					/* letting join_type reduce to empty doesn't work - 允许 join_type 规约为空不起作用 */
					JoinExpr   *n = makeNode(JoinExpr);

					n->jointype = JOIN_INNER;
					n->isNatural = true;
					n->larg = $1;
					n->rarg = $4;
					n->usingClause = NIL; /* figure out which columns later... - 稍后确定哪些列... */
					n->join_using_alias = NULL;
					n->quals = NULL; /* fill later - 稍后填充 */
					$$ = n;
				}
		;

alias_clause:
			AS ColId '(' name_list ')'
				{
					$$ = makeNode(Alias);
					$$->aliasname = $2;
					$$->colnames = $4;
				}
			| AS ColId
				{
					$$ = makeNode(Alias);
					$$->aliasname = $2;
				}
			| ColId '(' name_list ')'
				{
					$$ = makeNode(Alias);
					$$->aliasname = $1;
					$$->colnames = $3;
				}
			| ColId
				{
					$$ = makeNode(Alias);
					$$->aliasname = $1;
				}
		;

opt_alias_clause: alias_clause						{ $$ = $1; }
			| /* EMPTY - 空 */								{ $$ = NULL; }
		;

/*
 * The alias clause after JOIN ... USING only accepts the AS ColId spelling,
 * per SQL standard.  (The grammar could parse the other variants, but they
 * don't seem to be useful, and it might lead to parser problems in the
 * future.)
 * 根据 SQL 标准，JOIN ... USING 之后的别名子句仅接受 AS ColId 拼写。（语法可以解析其他变体，但它们似乎没有什么用处，并且可能会在未来导致解析器问题。）
 */
opt_alias_clause_for_join_using:
			AS ColId
				{
					$$ = makeNode(Alias);
					$$->aliasname = $2;
					/* the column name list will be inserted later - 列名列表将在稍后插入 */
				}
			| /* EMPTY - 空 */								{ $$ = NULL; }
		;

/*
 * func_alias_clause can include both an Alias and a coldeflist, so we make it
 * return a 2-element list that gets disassembled by calling production.
 * func_alias_clause 可以同时包含 Alias 和 coldeflist，因此我们让它返回一个双元素列表，该列表由调用它的产生式进行解构。
 */
func_alias_clause:
			alias_clause
				{
					$$ = list_make2($1, NIL);
				}
			| AS '(' TableFuncElementList ')'
				{
					$$ = list_make2(NULL, $3);
				}
			| AS ColId '(' TableFuncElementList ')'
				{
					Alias	   *a = makeNode(Alias);

					a->aliasname = $2;
					$$ = list_make2(a, $4);
				}
			| ColId '(' TableFuncElementList ')'
				{
					Alias	   *a = makeNode(Alias);

					a->aliasname = $1;
					$$ = list_make2(a, $3);
				}
			| /* EMPTY - 空 */
				{
					$$ = list_make2(NULL, NIL);
				}
		;

join_type:	FULL opt_outer							{ $$ = JOIN_FULL; }
			| LEFT opt_outer						{ $$ = JOIN_LEFT; }
			| RIGHT opt_outer						{ $$ = JOIN_RIGHT; }
			| INNER_P								{ $$ = JOIN_INNER; }
		;

/* OUTER is just noise... - OUTER 只是噪词/无义词... */
opt_outer: OUTER_P
			| /* EMPTY - 空 */
		;

/* JOIN qualification clauses
 * Possibilities are:
 *	USING ( column list ) [ AS alias ]
 *						  allows only unqualified column names,
 *						  which must match between tables.
 *	ON expr allows more general qualifications.
 *
 * We return USING as a two-element List (the first item being a sub-List
 * of the common column names, and the second either an Alias item or NULL).
 * An ON-expr will not be a List, so it can be told apart that way.
 * JOIN 限定子句 的可能性有：USING ( 列名列表 ) [ AS 别名 ]，它仅允许未限定的列名，这些列名在表之间必须匹配。ON 表达式允许更通用的限定条件。我们以包含两个元素的 List 形式返回 USING（第一个项是公共列名的子 List，第二个项是 Alias 项或 NULL）。ON 表达式不会是一个 List，因此可以通过这种方式进行区分。
 */

join_qual: USING '(' name_list ')' opt_alias_clause_for_join_using
				{
					$$ = (Node *) list_make2($3, $5);
				}
			| ON a_expr
				{
					$$ = $2;
				}
		;


relation_expr:
			qualified_name
				{
					/* inheritance query, implicitly - 隐式继承查询 */
					$$ = $1;
					$$->inh = true;
					$$->alias = NULL;
				}
			| extended_relation_expr
				{
					$$ = $1;
				}
		;

extended_relation_expr:
			qualified_name '*'
				{
					/* inheritance query, explicitly - 显式继承查询 */
					$$ = $1;
					$$->inh = true;
					$$->alias = NULL;
				}
			| ONLY qualified_name
				{
					/* no inheritance - 无继承 */
					$$ = $2;
					$$->inh = false;
					$$->alias = NULL;
				}
			| ONLY '(' qualified_name ')'
				{
					/* no inheritance, SQL99-style syntax - 无继承，SQL99 风格语法 */
					$$ = $3;
					$$->inh = false;
					$$->alias = NULL;
				}
		;


relation_expr_list:
			relation_expr							{ $$ = list_make1($1); }
			| relation_expr_list ',' relation_expr	{ $$ = lappend($1, $3); }
		;


/*
 * Given "UPDATE foo set set ...", we have to decide without looking any
 * further ahead whether the first "set" is an alias or the UPDATE's SET
 * keyword.  Since "set" is allowed as a column name both interpretations
 * are feasible.  We resolve the shift/reduce conflict by giving the first
 * relation_expr_opt_alias production a higher precedence than the SET token
 * has, causing the parser to prefer to reduce, in effect assuming that the
 * SET is not an alias.
 * 给定 "UPDATE foo set set ..." 时，我们必须在不进一步向前看的情况下断定第一个 "set" 是一个别名还是 UPDATE 的 SET 关键字。由于允许 "set" 作为列名，这两种解释都是可行的。我们通过赋予第一个 relation_expr_opt_alias 产生式比 SET 标记更高的优先级来解决移进/规约冲突，这使得解析器更倾向于进行规约，实际上是假设该 SET 不是一个别名。
 */
relation_expr_opt_alias: relation_expr					%prec UMINUS
				{
					$$ = $1;
				}
			| relation_expr ColId
				{
					Alias	   *alias = makeNode(Alias);

					alias->aliasname = $2;
					$1->alias = alias;
					$$ = $1;
				}
			| relation_expr AS ColId
				{
					Alias	   *alias = makeNode(Alias);

					alias->aliasname = $3;
					$1->alias = alias;
					$$ = $1;
				}
		;

/*
 * TABLESAMPLE decoration in a FROM item
 * FROM 项中的 TABLESAMPLE 装饰
 */
tablesample_clause:
			TABLESAMPLE func_name '(' expr_list ')' opt_repeatable_clause
				{
					RangeTableSample *n = makeNode(RangeTableSample);

					/* n->relation will be filled in later - n->relation 将在稍后填充 */
					n->method = $2;
					n->args = $4;
					n->repeatable = $6;
					n->location = @2;
					$$ = (Node *) n;
				}
		;

opt_repeatable_clause:
			REPEATABLE '(' a_expr ')'	{ $$ = (Node *) $3; }
			| /* EMPTY - 空 */					{ $$ = NULL; }
		;

/*
 * func_table represents a function invocation in a FROM list. It can be
 * a plain function call, like "foo(...)", or a ROWS FROM expression with
 * one or more function calls, "ROWS FROM (foo(...), bar(...))",
 * optionally with WITH ORDINALITY attached.
 * In the ROWS FROM syntax, a column definition list can be given for each
 * function, for example:
 *     ROWS FROM (foo() AS (foo_res_a text, foo_res_b text),
 *                bar() AS (bar_res_a text, bar_res_b text))
 * It's also possible to attach a column definition list to the RangeFunction
 * as a whole, but that's handled by the table_ref production.
 * func_table 表示 FROM 列表中的函数调用。它可以是普通的函数调用，如 "foo(...)"，也可以是带有一个或多个函数调用的 ROWS FROM 表达式，如 "ROWS FROM (foo(...), bar(...))"，且可选择附加 WITH ORDINALITY。在 ROWS FROM 语法中，可以为每个函数提供列定义列表，例如：ROWS FROM (foo() AS (foo_res_a text, foo_res_b text), bar() AS (bar_res_a text, bar_res_b text))。也可以将列定义列表附加到 RangeFunction 整体上，但那是由 table_ref 产生式处理的。
 */
func_table: func_expr_windowless opt_ordinality
				{
					RangeFunction *n = makeNode(RangeFunction);

					n->lateral = false;
					n->ordinality = $2;
					n->is_rowsfrom = false;
					n->functions = list_make1(list_make2($1, NIL));
					/* alias and coldeflist are set by table_ref production - alias 和 coldeflist 由 table_ref 产生式设置 */
					$$ = (Node *) n;
				}
			| ROWS FROM '(' rowsfrom_list ')' opt_ordinality
				{
					RangeFunction *n = makeNode(RangeFunction);

					n->lateral = false;
					n->ordinality = $6;
					n->is_rowsfrom = true;
					n->functions = $4;
					/* alias and coldeflist are set by table_ref production - alias 和 coldeflist 由 table_ref 产生式设置 */
					$$ = (Node *) n;
				}
		;

rowsfrom_item: func_expr_windowless opt_col_def_list
				{ $$ = list_make2($1, $2); }
		;

rowsfrom_list:
			rowsfrom_item						{ $$ = list_make1($1); }
			| rowsfrom_list ',' rowsfrom_item	{ $$ = lappend($1, $3); }
		;

opt_col_def_list: AS '(' TableFuncElementList ')'	{ $$ = $3; }
			| /* EMPTY - 空 */								{ $$ = NIL; }
		;

opt_ordinality: WITH_LA ORDINALITY					{ $$ = true; }
			| /* EMPTY - 空 */								{ $$ = false; }
		;


where_clause:
			WHERE a_expr							{ $$ = $2; }
			| /* EMPTY - 空 */								{ $$ = NULL; }
		;

/* variant for UPDATE and DELETE - UPDATE 和 DELETE 的变体 */
where_or_current_clause:
			WHERE a_expr							{ $$ = $2; }
			| WHERE CURRENT_P OF cursor_name
				{
					CurrentOfExpr *n = makeNode(CurrentOfExpr);

					/* cvarno is filled in by parse analysis - cvarno 由解析分析填充 */
					n->cursor_name = $4;
					n->cursor_param = 0;
					$$ = (Node *) n;
				}
			| /* EMPTY - 空 */								{ $$ = NULL; }
		;


OptTableFuncElementList:
			TableFuncElementList				{ $$ = $1; }
			| /* EMPTY - 空 */							{ $$ = NIL; }
		;

TableFuncElementList:
			TableFuncElement
				{
					$$ = list_make1($1);
				}
			| TableFuncElementList ',' TableFuncElement
				{
					$$ = lappend($1, $3);
				}
		;

TableFuncElement:	ColId Typename opt_collate_clause
				{
					ColumnDef *n = makeNode(ColumnDef);

					n->colname = $1;
					n->typeName = $2;
					n->inhcount = 0;
					n->is_local = true;
					n->is_not_null = false;
					n->is_from_type = false;
					n->storage = 0;
					n->raw_default = NULL;
					n->cooked_default = NULL;
					n->collClause = (CollateClause *) $3;
					n->collOid = InvalidOid;
					n->constraints = NIL;
					n->location = @1;
					$$ = (Node *) n;
				}
		;

/*
 * XMLTABLE
 */
xmltable:
			XMLTABLE '(' c_expr xmlexists_argument COLUMNS xmltable_column_list ')'
				{
					RangeTableFunc *n = makeNode(RangeTableFunc);

					n->rowexpr = $3;
					n->docexpr = $4;
					n->columns = $6;
					n->namespaces = NIL;
					n->location = @1;
					$$ = (Node *) n;
				}
			| XMLTABLE '(' XMLNAMESPACES '(' xml_namespace_list ')' ','
				c_expr xmlexists_argument COLUMNS xmltable_column_list ')'
				{
					RangeTableFunc *n = makeNode(RangeTableFunc);

					n->rowexpr = $8;
					n->docexpr = $9;
					n->columns = $11;
					n->namespaces = $5;
					n->location = @1;
					$$ = (Node *) n;
				}
		;

xmltable_column_list: xmltable_column_el					{ $$ = list_make1($1); }
			| xmltable_column_list ',' xmltable_column_el	{ $$ = lappend($1, $3); }
		;

xmltable_column_el:
			ColId Typename
				{
					RangeTableFuncCol *fc = makeNode(RangeTableFuncCol);

					fc->colname = $1;
					fc->for_ordinality = false;
					fc->typeName = $2;
					fc->is_not_null = false;
					fc->colexpr = NULL;
					fc->coldefexpr = NULL;
					fc->location = @1;

					$$ = (Node *) fc;
				}
			| ColId Typename xmltable_column_option_list
				{
					RangeTableFuncCol *fc = makeNode(RangeTableFuncCol);
					ListCell   *option;
					bool		nullability_seen = false;

					fc->colname = $1;
					fc->typeName = $2;
					fc->for_ordinality = false;
					fc->is_not_null = false;
					fc->colexpr = NULL;
					fc->coldefexpr = NULL;
					fc->location = @1;

					foreach(option, $3)
					{
						DefElem   *defel = (DefElem *) lfirst(option);

						if (strcmp(defel->defname, "default") == 0)
						{
							if (fc->coldefexpr != NULL)
								ereport(ERROR,
										(errcode(ERRCODE_SYNTAX_ERROR),
										 errmsg("only one DEFAULT value is allowed"),
										 parser_errposition(defel->location)));
							fc->coldefexpr = defel->arg;
						}
						else if (strcmp(defel->defname, "path") == 0)
						{
							if (fc->colexpr != NULL)
								ereport(ERROR,
										(errcode(ERRCODE_SYNTAX_ERROR),
										 errmsg("only one PATH value per column is allowed"),
										 parser_errposition(defel->location)));
							fc->colexpr = defel->arg;
						}
						else if (strcmp(defel->defname, "__pg__is_not_null") == 0)
						{
							if (nullability_seen)
								ereport(ERROR,
										(errcode(ERRCODE_SYNTAX_ERROR),
										 errmsg("conflicting or redundant NULL / NOT NULL declarations for column \"%s\"", fc->colname),
										 parser_errposition(defel->location)));
							fc->is_not_null = boolVal(defel->arg);
							nullability_seen = true;
						}
						else
						{
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("unrecognized column option \"%s\"",
											defel->defname),
									 parser_errposition(defel->location)));
						}
					}
					$$ = (Node *) fc;
				}
			| ColId FOR ORDINALITY
				{
					RangeTableFuncCol *fc = makeNode(RangeTableFuncCol);

					fc->colname = $1;
					fc->for_ordinality = true;
					/* other fields are ignored, initialized by makeNode - 其他字段被忽略，由 makeNode 进行初始化 */
					fc->location = @1;

					$$ = (Node *) fc;
				}
		;

xmltable_column_option_list:
			xmltable_column_option_el
				{ $$ = list_make1($1); }
			| xmltable_column_option_list xmltable_column_option_el
				{ $$ = lappend($1, $2); }
		;

xmltable_column_option_el:
			IDENT b_expr
				{
					if (strcmp($1, "__pg__is_not_null") == 0)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("option name \"%s\" cannot be used in XMLTABLE", $1),
								 parser_errposition(@1)));
					$$ = makeDefElem($1, $2, @1);
				}
			| DEFAULT b_expr
				{ $$ = makeDefElem("default", $2, @1); }
			| NOT NULL_P
				{ $$ = makeDefElem("__pg__is_not_null", (Node *) makeBoolean(true), @1); }
			| NULL_P
				{ $$ = makeDefElem("__pg__is_not_null", (Node *) makeBoolean(false), @1); }
			| PATH b_expr
				{ $$ = makeDefElem("path", $2, @1); }
		;

xml_namespace_list:
			xml_namespace_el
				{ $$ = list_make1($1); }
			| xml_namespace_list ',' xml_namespace_el
				{ $$ = lappend($1, $3); }
		;

xml_namespace_el:
			b_expr AS ColLabel
				{
					$$ = makeNode(ResTarget);
					$$->name = $3;
					$$->indirection = NIL;
					$$->val = $1;
					$$->location = @1;
				}
			| DEFAULT b_expr
				{
					$$ = makeNode(ResTarget);
					$$->name = NULL;
					$$->indirection = NIL;
					$$->val = $2;
					$$->location = @1;
				}
		;

json_table:
			JSON_TABLE '('
				json_value_expr ',' a_expr json_table_path_name_opt
				json_passing_clause_opt
				COLUMNS '(' json_table_column_definition_list ')'
				json_on_error_clause_opt
			')'
				{
					JsonTable *n = makeNode(JsonTable);
					char	  *pathstring;

					n->context_item = (JsonValueExpr *) $3;
					if (!IsA($5, A_Const) ||
						castNode(A_Const, $5)->val.node.type != T_String)
						ereport(ERROR,
								errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								errmsg("only string constants are supported in JSON_TABLE path specification"),
								parser_errposition(@5));
					pathstring = castNode(A_Const, $5)->val.sval.sval;
					n->pathspec = makeJsonTablePathSpec(pathstring, $6, @5, @6);
					n->passing = $7;
					n->columns = $10;
					n->on_error = (JsonBehavior *) $12;
					n->location = @1;
					$$ = (Node *) n;
				}
		;

json_table_path_name_opt:
			AS name			{ $$ = $2; }
			| /* empty - 空 */	{ $$ = NULL; }
		;

json_table_column_definition_list:
			json_table_column_definition
				{ $$ = list_make1($1); }
			| json_table_column_definition_list ',' json_table_column_definition
				{ $$ = lappend($1, $3); }
		;

json_table_column_definition:
			ColId FOR ORDINALITY
				{
					JsonTableColumn *n = makeNode(JsonTableColumn);

					n->coltype = JTC_FOR_ORDINALITY;
					n->name = $1;
					n->location = @1;
					$$ = (Node *) n;
				}
			| ColId Typename
				json_table_column_path_clause_opt
				json_wrapper_behavior
				json_quotes_clause_opt
				json_behavior_clause_opt
				{
					JsonTableColumn *n = makeNode(JsonTableColumn);

					n->coltype = JTC_REGULAR;
					n->name = $1;
					n->typeName = $2;
					n->format = makeJsonFormat(JS_FORMAT_DEFAULT, JS_ENC_DEFAULT, -1);
					n->pathspec = (JsonTablePathSpec *) $3;
					n->wrapper = $4;
					n->quotes = $5;
					n->on_empty = (JsonBehavior *) linitial($6);
					n->on_error = (JsonBehavior *) lsecond($6);
					n->location = @1;
					$$ = (Node *) n;
				}
			| ColId Typename json_format_clause
				json_table_column_path_clause_opt
				json_wrapper_behavior
				json_quotes_clause_opt
				json_behavior_clause_opt
				{
					JsonTableColumn *n = makeNode(JsonTableColumn);

					n->coltype = JTC_FORMATTED;
					n->name = $1;
					n->typeName = $2;
					n->format = (JsonFormat *) $3;
					n->pathspec = (JsonTablePathSpec *) $4;
					n->wrapper = $5;
					n->quotes = $6;
					n->on_empty = (JsonBehavior *) linitial($7);
					n->on_error = (JsonBehavior *) lsecond($7);
					n->location = @1;
					$$ = (Node *) n;
				}
			| ColId Typename
				EXISTS json_table_column_path_clause_opt
				json_on_error_clause_opt
				{
					JsonTableColumn *n = makeNode(JsonTableColumn);

					n->coltype = JTC_EXISTS;
					n->name = $1;
					n->typeName = $2;
					n->format = makeJsonFormat(JS_FORMAT_DEFAULT, JS_ENC_DEFAULT, -1);
					n->wrapper = JSW_NONE;
					n->quotes = JS_QUOTES_UNSPEC;
					n->pathspec = (JsonTablePathSpec *) $4;
					n->on_empty = NULL;
					n->on_error = (JsonBehavior *) $5;
					n->location = @1;
					$$ = (Node *) n;
				}
			| NESTED path_opt Sconst
				COLUMNS '(' json_table_column_definition_list ')'
				{
					JsonTableColumn *n = makeNode(JsonTableColumn);

					n->coltype = JTC_NESTED;
					n->pathspec = (JsonTablePathSpec *)
						makeJsonTablePathSpec($3, NULL, @3, -1);
					n->columns = $6;
					n->location = @1;
					$$ = (Node *) n;
				}
			| NESTED path_opt Sconst AS name
				COLUMNS '(' json_table_column_definition_list ')'
				{
					JsonTableColumn *n = makeNode(JsonTableColumn);

					n->coltype = JTC_NESTED;
					n->pathspec = (JsonTablePathSpec *)
						makeJsonTablePathSpec($3, $5, @3, @5);
					n->columns = $8;
					n->location = @1;
					$$ = (Node *) n;
				}
		;

path_opt:
			PATH
			| /* EMPTY - 空 */
		;

json_table_column_path_clause_opt:
			PATH Sconst
				{ $$ = (Node *) makeJsonTablePathSpec($2, NULL, @2, -1); }
			| /* EMPTY - 空 */
				{ $$ = NULL; }
		;

/*****************************************************************************
 *
 *	Type syntax
 *		SQL introduces a large amount of type-specific syntax.
 *		Define individual clauses to handle these cases, and use
 *		 the generic case to handle regular type-extensible Postgres syntax.
 *		- thomas 1997-10-10
 *
 * 类型语法：SQL 引入了大量特定于类型的语法。定义单独的子句来处理这些情况，并使用通用情况来处理常规的、类型可扩展的 Postgres 语法。- thomas 1997-10-10
 *****************************************************************************/

Typename:	SimpleTypename opt_array_bounds
				{
					$$ = $1;
					$$->arrayBounds = $2;
				}
			| SETOF SimpleTypename opt_array_bounds
				{
					$$ = $2;
					$$->arrayBounds = $3;
					$$->setof = true;
				}
			/* SQL standard syntax, currently only one-dimensional */
			| SimpleTypename ARRAY '[' Iconst ']'
				{
					$$ = $1;
					$$->arrayBounds = list_make1(makeInteger($4));
				}
			| SETOF SimpleTypename ARRAY '[' Iconst ']'
				{
					$$ = $2;
					$$->arrayBounds = list_make1(makeInteger($5));
					$$->setof = true;
				}
			| SimpleTypename ARRAY
				{
					$$ = $1;
					$$->arrayBounds = list_make1(makeInteger(-1));
				}
			| SETOF SimpleTypename ARRAY
				{
					$$ = $2;
					$$->arrayBounds = list_make1(makeInteger(-1));
					$$->setof = true;
				}
		;

opt_array_bounds:
			opt_array_bounds '[' ']'
					{  $$ = lappend($1, makeInteger(-1)); }
			| opt_array_bounds '[' Iconst ']'
					{  $$ = lappend($1, makeInteger($3)); }
			| /* EMPTY - 空 */
					{  $$ = NIL; }
		;

SimpleTypename:
			GenericType								{ $$ = $1; }
			| Numeric								{ $$ = $1; }
			| Bit									{ $$ = $1; }
			| Character								{ $$ = $1; }
			| ConstDatetime							{ $$ = $1; }
			| ConstInterval opt_interval
				{
					$$ = $1;
					$$->typmods = $2;
				}
			| ConstInterval '(' Iconst ')'
				{
					$$ = $1;
					$$->typmods = list_make2(makeIntConst(INTERVAL_FULL_RANGE, -1),
											 makeIntConst($3, @3));
				}
			| JsonType								{ $$ = $1; }
		;

/* We have a separate ConstTypename to allow defaulting fixed-length
 * types such as CHAR() and BIT() to an unspecified length.
 * SQL9x requires that these default to a length of one, but this
 * makes no sense for constructs like CHAR 'hi' and BIT '0101',
 * where there is an obvious better choice to make.
 * Note that ConstInterval is not included here since it must
 * be pushed up higher in the rules to accommodate the postfix
 * options (e.g. INTERVAL '1' YEAR). Likewise, we have to handle
 * the generic-type-name case in AexprConst to avoid premature
 * reduce/reduce conflicts against function names.
 */
ConstTypename:
			Numeric									{ $$ = $1; }
			| ConstBit								{ $$ = $1; }
			| ConstCharacter						{ $$ = $1; }
			| ConstDatetime							{ $$ = $1; }
			| JsonType								{ $$ = $1; }
		;

/*
 * GenericType covers all type names that don't have special syntax mandated
 * by the standard, including qualified names.  We also allow type modifiers.
 * To avoid parsing conflicts against function invocations, the modifiers
 * have to be shown as expr_list here, but parse analysis will only accept
 * constants for them.
 */
GenericType:
			type_function_name opt_type_modifiers
				{
					$$ = makeTypeName($1);
					$$->typmods = $2;
					$$->location = @1;
				}
			| type_function_name attrs opt_type_modifiers
				{
					$$ = makeTypeNameFromNameList(lcons(makeString($1), $2));
					$$->typmods = $3;
					$$->location = @1;
				}
		;

opt_type_modifiers: '(' expr_list ')'				{ $$ = $2; }
					| /* EMPTY - 空 */					{ $$ = NIL; }
		;

/*
 * SQL numeric data types
 */
Numeric:	INT_P
				{
					$$ = SystemTypeName("int4");
					$$->location = @1;
				}
			| INTEGER
				{
					$$ = SystemTypeName("int4");
					$$->location = @1;
				}
			| SMALLINT
				{
					$$ = SystemTypeName("int2");
					$$->location = @1;
				}
			| BIGINT
				{
					$$ = SystemTypeName("int8");
					$$->location = @1;
				}
			| REAL
				{
					$$ = SystemTypeName("float4");
					$$->location = @1;
				}
			| FLOAT_P opt_float
				{
					$$ = $2;
					$$->location = @1;
				}
			| DOUBLE_P PRECISION
				{
					$$ = SystemTypeName("float8");
					$$->location = @1;
				}
			| DECIMAL_P opt_type_modifiers
				{
					$$ = SystemTypeName("numeric");
					$$->typmods = $2;
					$$->location = @1;
				}
			| DEC opt_type_modifiers
				{
					$$ = SystemTypeName("numeric");
					$$->typmods = $2;
					$$->location = @1;
				}
			| NUMERIC opt_type_modifiers
				{
					$$ = SystemTypeName("numeric");
					$$->typmods = $2;
					$$->location = @1;
				}
			| BOOLEAN_P
				{
					$$ = SystemTypeName("bool");
					$$->location = @1;
				}
		;

opt_float:	'(' Iconst ')'
				{
					/*
					 * Check FLOAT() precision limits assuming IEEE floating
					 * types - thomas 1997-09-18
					 */
					if ($2 < 1)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								 errmsg("precision for type float must be at least 1 bit"),
								 parser_errposition(@2)));
					else if ($2 <= 24)
						$$ = SystemTypeName("float4");
					else if ($2 <= 53)
						$$ = SystemTypeName("float8");
					else
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								 errmsg("precision for type float must be less than 54 bits"),
								 parser_errposition(@2)));
				}
			| /* EMPTY - 空 */
				{
					$$ = SystemTypeName("float8");
				}
		;

/*
 * SQL bit-field data types
 * The following implements BIT() and BIT VARYING().
 */
Bit:		BitWithLength
				{
					$$ = $1;
				}
			| BitWithoutLength
				{
					$$ = $1;
				}
		;

/* ConstBit is like Bit except "BIT" defaults to unspecified length */
/* See notes for ConstCharacter, which addresses same issue for "CHAR" */
ConstBit:	BitWithLength
				{
					$$ = $1;
				}
			| BitWithoutLength
				{
					$$ = $1;
					$$->typmods = NIL;
				}
		;

BitWithLength:
			BIT opt_varying '(' expr_list ')'
				{
					char *typname;

					typname = $2 ? "varbit" : "bit";
					$$ = SystemTypeName(typname);
					$$->typmods = $4;
					$$->location = @1;
				}
		;

BitWithoutLength:
			BIT opt_varying
				{
					/* bit defaults to bit(1), varbit to no limit */
					if ($2)
					{
						$$ = SystemTypeName("varbit");
					}
					else
					{
						$$ = SystemTypeName("bit");
						$$->typmods = list_make1(makeIntConst(1, -1));
					}
					$$->location = @1;
				}
		;


/*
 * SQL character data types
 * The following implements CHAR() and VARCHAR().
 */
Character:  CharacterWithLength
				{
					$$ = $1;
				}
			| CharacterWithoutLength
				{
					$$ = $1;
				}
		;

ConstCharacter:  CharacterWithLength
				{
					$$ = $1;
				}
			| CharacterWithoutLength
				{
					/* Length was not specified so allow to be unrestricted.
					 * This handles problems with fixed-length (bpchar) strings
					 * which in column definitions must default to a length
					 * of one, but should not be constrained if the length
					 * was not specified.
					 */
					$$ = $1;
					$$->typmods = NIL;
				}
		;

CharacterWithLength:  character '(' Iconst ')'
				{
					$$ = SystemTypeName($1);
					$$->typmods = list_make1(makeIntConst($3, @3));
					$$->location = @1;
				}
		;

CharacterWithoutLength:	 character
				{
					$$ = SystemTypeName($1);
					/* char defaults to char(1), varchar to no limit */
					if (strcmp($1, "bpchar") == 0)
						$$->typmods = list_make1(makeIntConst(1, -1));
					$$->location = @1;
				}
		;

character:	CHARACTER opt_varying
										{ $$ = $2 ? "varchar": "bpchar"; }
			| CHAR_P opt_varying
										{ $$ = $2 ? "varchar": "bpchar"; }
			| VARCHAR
										{ $$ = "varchar"; }
			| NATIONAL CHARACTER opt_varying
										{ $$ = $3 ? "varchar": "bpchar"; }
			| NATIONAL CHAR_P opt_varying
										{ $$ = $3 ? "varchar": "bpchar"; }
			| NCHAR opt_varying
										{ $$ = $2 ? "varchar": "bpchar"; }
		;

opt_varying:
			VARYING									{ $$ = true; }
			| /* EMPTY - 空 */								{ $$ = false; }
		;

/*
 * SQL date/time types
 */
ConstDatetime:
			TIMESTAMP '(' Iconst ')' opt_timezone
				{
					if ($5)
						$$ = SystemTypeName("timestamptz");
					else
						$$ = SystemTypeName("timestamp");
					$$->typmods = list_make1(makeIntConst($3, @3));
					$$->location = @1;
				}
			| TIMESTAMP opt_timezone
				{
					if ($2)
						$$ = SystemTypeName("timestamptz");
					else
						$$ = SystemTypeName("timestamp");
					$$->location = @1;
				}
			| TIME '(' Iconst ')' opt_timezone
				{
					if ($5)
						$$ = SystemTypeName("timetz");
					else
						$$ = SystemTypeName("time");
					$$->typmods = list_make1(makeIntConst($3, @3));
					$$->location = @1;
				}
			| TIME opt_timezone
				{
					if ($2)
						$$ = SystemTypeName("timetz");
					else
						$$ = SystemTypeName("time");
					$$->location = @1;
				}
		;

ConstInterval:
			INTERVAL
				{
					$$ = SystemTypeName("interval");
					$$->location = @1;
				}
		;

opt_timezone:
			WITH_LA TIME ZONE						{ $$ = true; }
			| WITHOUT_LA TIME ZONE					{ $$ = false; }
			| /* EMPTY - 空 */								{ $$ = false; }
		;

opt_interval:
			YEAR_P
				{ $$ = list_make1(makeIntConst(INTERVAL_MASK(YEAR), @1)); }
			| MONTH_P
				{ $$ = list_make1(makeIntConst(INTERVAL_MASK(MONTH), @1)); }
			| DAY_P
				{ $$ = list_make1(makeIntConst(INTERVAL_MASK(DAY), @1)); }
			| HOUR_P
				{ $$ = list_make1(makeIntConst(INTERVAL_MASK(HOUR), @1)); }
			| MINUTE_P
				{ $$ = list_make1(makeIntConst(INTERVAL_MASK(MINUTE), @1)); }
			| interval_second
				{ $$ = $1; }
			| YEAR_P TO MONTH_P
				{
					$$ = list_make1(makeIntConst(INTERVAL_MASK(YEAR) |
												 INTERVAL_MASK(MONTH), @1));
				}
			| DAY_P TO HOUR_P
				{
					$$ = list_make1(makeIntConst(INTERVAL_MASK(DAY) |
												 INTERVAL_MASK(HOUR), @1));
				}
			| DAY_P TO MINUTE_P
				{
					$$ = list_make1(makeIntConst(INTERVAL_MASK(DAY) |
												 INTERVAL_MASK(HOUR) |
												 INTERVAL_MASK(MINUTE), @1));
				}
			| DAY_P TO interval_second
				{
					$$ = $3;
					linitial($$) = makeIntConst(INTERVAL_MASK(DAY) |
												INTERVAL_MASK(HOUR) |
												INTERVAL_MASK(MINUTE) |
												INTERVAL_MASK(SECOND), @1);
				}
			| HOUR_P TO MINUTE_P
				{
					$$ = list_make1(makeIntConst(INTERVAL_MASK(HOUR) |
												 INTERVAL_MASK(MINUTE), @1));
				}
			| HOUR_P TO interval_second
				{
					$$ = $3;
					linitial($$) = makeIntConst(INTERVAL_MASK(HOUR) |
												INTERVAL_MASK(MINUTE) |
												INTERVAL_MASK(SECOND), @1);
				}
			| MINUTE_P TO interval_second
				{
					$$ = $3;
					linitial($$) = makeIntConst(INTERVAL_MASK(MINUTE) |
												INTERVAL_MASK(SECOND), @1);
				}
			| /* EMPTY - 空 */
				{ $$ = NIL; }
		;

interval_second:
			SECOND_P
				{
					$$ = list_make1(makeIntConst(INTERVAL_MASK(SECOND), @1));
				}
			| SECOND_P '(' Iconst ')'
				{
					$$ = list_make2(makeIntConst(INTERVAL_MASK(SECOND), @1),
									makeIntConst($3, @3));
				}
		;

JsonType:
			JSON
				{
					$$ = SystemTypeName("json");
					$$->location = @1;
				}
		;

/*****************************************************************************
 *
 *	expression grammar
 *
 *****************************************************************************/

/*
 * General expressions
 * This is the heart of the expression syntax.
 *
 * We have two expression types: a_expr is the unrestricted kind, and
 * b_expr is a subset that must be used in some places to avoid shift/reduce
 * conflicts.  For example, we can't do BETWEEN as "BETWEEN a_expr AND a_expr"
 * because that use of AND conflicts with AND as a boolean operator.  So,
 * b_expr is used in BETWEEN and we remove boolean keywords from b_expr.
 *
 * Note that '(' a_expr ')' is a b_expr, so an unrestricted expression can
 * always be used by surrounding it with parens.
 *
 * c_expr is all the productions that are common to a_expr and b_expr;
 * it's factored out just to eliminate redundant coding.
 *
 * Be careful of productions involving more than one terminal token.
 * By default, bison will assign such productions the precedence of their
 * last terminal, but in nearly all cases you want it to be the precedence
 * of the first terminal instead; otherwise you will not get the behavior
 * you expect!  So we use %prec annotations freely to set precedences.
 */
a_expr:		c_expr									{ $$ = $1; }
			| a_expr TYPECAST Typename
					{ $$ = makeTypeCast($1, $3, @2); }
			| a_expr COLLATE any_name
				{
					CollateClause *n = makeNode(CollateClause);

					n->arg = $1;
					n->collname = $3;
					n->location = @2;
					$$ = (Node *) n;
				}
			| a_expr AT TIME ZONE a_expr			%prec AT
				{
					$$ = (Node *) makeFuncCall(SystemFuncName("timezone"),
											   list_make2($5, $1),
											   COERCE_SQL_SYNTAX,
											   @2);
				}
			| a_expr AT LOCAL						%prec AT
				{
					$$ = (Node *) makeFuncCall(SystemFuncName("timezone"),
											   list_make1($1),
											   COERCE_SQL_SYNTAX,
											   -1);
				}
		/*
		 * These operators must be called out explicitly in order to make use
		 * of bison's automatic operator-precedence handling.  All other
		 * operator names are handled by the generic productions using "Op",
		 * below; and all those operators will have the same precedence.
		 *
		 * If you add more explicitly-known operators, be sure to add them
		 * also to b_expr and to the MathOp list below.
		 * 这些操作符必须显式调用，以便利用 Bison 的自动操作符优先级处理。所有其他操作符名称均由使用下面 "Op" 的通用产生式处理；并且所有这些操作符将具有相同的优先级。如果您添加更多显式已知的操作符，请务必也将它们添加到 b_expr 和下面的 MathOp 列表中。
		 */
			| '+' a_expr					%prec UMINUS
				{ $$ = (Node *) makeSimpleA_Expr(AEXPR_OP, "+", NULL, $2, @1); }
			| '-' a_expr					%prec UMINUS
				{ $$ = doNegate($2, @1); }
			| a_expr '+' a_expr
				{ $$ = (Node *) makeSimpleA_Expr(AEXPR_OP, "+", $1, $3, @2); }
			| a_expr '-' a_expr
				{ $$ = (Node *) makeSimpleA_Expr(AEXPR_OP, "-", $1, $3, @2); }
			| a_expr '*' a_expr
				{ $$ = (Node *) makeSimpleA_Expr(AEXPR_OP, "*", $1, $3, @2); }
			| a_expr '/' a_expr
				{ $$ = (Node *) makeSimpleA_Expr(AEXPR_OP, "/", $1, $3, @2); }
			| a_expr '%' a_expr
				{ $$ = (Node *) makeSimpleA_Expr(AEXPR_OP, "%", $1, $3, @2); }
			| a_expr '^' a_expr
				{ $$ = (Node *) makeSimpleA_Expr(AEXPR_OP, "^", $1, $3, @2); }
			| a_expr '<' a_expr
				{ $$ = (Node *) makeSimpleA_Expr(AEXPR_OP, "<", $1, $3, @2); }
			| a_expr '>' a_expr
				{ $$ = (Node *) makeSimpleA_Expr(AEXPR_OP, ">", $1, $3, @2); }
			| a_expr '=' a_expr
				{ $$ = (Node *) makeSimpleA_Expr(AEXPR_OP, "=", $1, $3, @2); }
			| a_expr LESS_EQUALS a_expr
				{ $$ = (Node *) makeSimpleA_Expr(AEXPR_OP, "<=", $1, $3, @2); }
			| a_expr GREATER_EQUALS a_expr
				{ $$ = (Node *) makeSimpleA_Expr(AEXPR_OP, ">=", $1, $3, @2); }
			| a_expr NOT_EQUALS a_expr
				{ $$ = (Node *) makeSimpleA_Expr(AEXPR_OP, "<>", $1, $3, @2); }

			| a_expr qual_Op a_expr				%prec Op
				{ $$ = (Node *) makeA_Expr(AEXPR_OP, $2, $1, $3, @2); }
			| qual_Op a_expr					%prec Op
				{ $$ = (Node *) makeA_Expr(AEXPR_OP, $1, NULL, $2, @1); }

			| a_expr AND a_expr
				{ $$ = makeAndExpr($1, $3, @2); }
			| a_expr OR a_expr
				{ $$ = makeOrExpr($1, $3, @2); }
			| NOT a_expr
				{ $$ = makeNotExpr($2, @1); }
			| NOT_LA a_expr						%prec NOT
				{ $$ = makeNotExpr($2, @1); }

			| a_expr LIKE a_expr
				{
					$$ = (Node *) makeSimpleA_Expr(AEXPR_LIKE, "~~",
												   $1, $3, @2);
				}
			| a_expr LIKE a_expr ESCAPE a_expr					%prec LIKE
				{
					FuncCall   *n = makeFuncCall(SystemFuncName("like_escape"),
												 list_make2($3, $5),
												 COERCE_EXPLICIT_CALL,
												 @2);
					$$ = (Node *) makeSimpleA_Expr(AEXPR_LIKE, "~~",
												   $1, (Node *) n, @2);
				}
			| a_expr NOT_LA LIKE a_expr							%prec NOT_LA
				{
					$$ = (Node *) makeSimpleA_Expr(AEXPR_LIKE, "!~~",
												   $1, $4, @2);
				}
			| a_expr NOT_LA LIKE a_expr ESCAPE a_expr			%prec NOT_LA
				{
					FuncCall   *n = makeFuncCall(SystemFuncName("like_escape"),
												 list_make2($4, $6),
												 COERCE_EXPLICIT_CALL,
												 @2);
					$$ = (Node *) makeSimpleA_Expr(AEXPR_LIKE, "!~~",
												   $1, (Node *) n, @2);
				}
			| a_expr ILIKE a_expr
				{
					$$ = (Node *) makeSimpleA_Expr(AEXPR_ILIKE, "~~*",
												   $1, $3, @2);
				}
			| a_expr ILIKE a_expr ESCAPE a_expr					%prec ILIKE
				{
					FuncCall   *n = makeFuncCall(SystemFuncName("like_escape"),
												 list_make2($3, $5),
												 COERCE_EXPLICIT_CALL,
												 @2);
					$$ = (Node *) makeSimpleA_Expr(AEXPR_ILIKE, "~~*",
												   $1, (Node *) n, @2);
				}
			| a_expr NOT_LA ILIKE a_expr						%prec NOT_LA
				{
					$$ = (Node *) makeSimpleA_Expr(AEXPR_ILIKE, "!~~*",
												   $1, $4, @2);
				}
			| a_expr NOT_LA ILIKE a_expr ESCAPE a_expr			%prec NOT_LA
				{
					FuncCall   *n = makeFuncCall(SystemFuncName("like_escape"),
												 list_make2($4, $6),
												 COERCE_EXPLICIT_CALL,
												 @2);
					$$ = (Node *) makeSimpleA_Expr(AEXPR_ILIKE, "!~~*",
												   $1, (Node *) n, @2);
				}

			| a_expr SIMILAR TO a_expr							%prec SIMILAR
				{
					FuncCall   *n = makeFuncCall(SystemFuncName("similar_to_escape"),
												 list_make1($4),
												 COERCE_EXPLICIT_CALL,
												 @2);
					$$ = (Node *) makeSimpleA_Expr(AEXPR_SIMILAR, "~",
												   $1, (Node *) n, @2);
				}
			| a_expr SIMILAR TO a_expr ESCAPE a_expr			%prec SIMILAR
				{
					FuncCall   *n = makeFuncCall(SystemFuncName("similar_to_escape"),
												 list_make2($4, $6),
												 COERCE_EXPLICIT_CALL,
												 @2);
					$$ = (Node *) makeSimpleA_Expr(AEXPR_SIMILAR, "~",
												   $1, (Node *) n, @2);
				}
			| a_expr NOT_LA SIMILAR TO a_expr					%prec NOT_LA
				{
					FuncCall   *n = makeFuncCall(SystemFuncName("similar_to_escape"),
												 list_make1($5),
												 COERCE_EXPLICIT_CALL,
												 @2);
					$$ = (Node *) makeSimpleA_Expr(AEXPR_SIMILAR, "!~",
												   $1, (Node *) n, @2);
				}
			| a_expr NOT_LA SIMILAR TO a_expr ESCAPE a_expr		%prec NOT_LA
				{
					FuncCall   *n = makeFuncCall(SystemFuncName("similar_to_escape"),
												 list_make2($5, $7),
												 COERCE_EXPLICIT_CALL,
												 @2);
					$$ = (Node *) makeSimpleA_Expr(AEXPR_SIMILAR, "!~",
												   $1, (Node *) n, @2);
				}

			/* NullTest clause
			 * Define SQL-style Null test clause.
			 * Allow two forms described in the standard:
			 *	a IS NULL
			 *	a IS NOT NULL
			 * Allow two SQL extensions
			 *	a ISNULL
			 *	a NOTNULL
			 * NullTest 子句。定义 SQL 风格的 Null 测试子句。允许标准中描述的两种形式：a IS NULL、a IS NOT NULL。允许两种 SQL 扩展：a ISNULL、a NOTNULL
			 */
			| a_expr IS NULL_P							%prec IS
				{
					NullTest   *n = makeNode(NullTest);

					n->arg = (Expr *) $1;
					n->nulltesttype = IS_NULL;
					n->location = @2;
					$$ = (Node *) n;
				}
			| a_expr ISNULL
				{
					NullTest   *n = makeNode(NullTest);

					n->arg = (Expr *) $1;
					n->nulltesttype = IS_NULL;
					n->location = @2;
					$$ = (Node *) n;
				}
			| a_expr IS NOT NULL_P						%prec IS
				{
					NullTest   *n = makeNode(NullTest);

					n->arg = (Expr *) $1;
					n->nulltesttype = IS_NOT_NULL;
					n->location = @2;
					$$ = (Node *) n;
				}
			| a_expr NOTNULL
				{
					NullTest   *n = makeNode(NullTest);

					n->arg = (Expr *) $1;
					n->nulltesttype = IS_NOT_NULL;
					n->location = @2;
					$$ = (Node *) n;
				}
			| row OVERLAPS row
				{
					if (list_length($1) != 2)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("wrong number of parameters on left side of OVERLAPS expression"),
								 parser_errposition(@1)));
					if (list_length($3) != 2)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("wrong number of parameters on right side of OVERLAPS expression"),
								 parser_errposition(@3)));
					$$ = (Node *) makeFuncCall(SystemFuncName("overlaps"),
											   list_concat($1, $3),
											   COERCE_SQL_SYNTAX,
											   @2);
				}
			| a_expr IS TRUE_P							%prec IS
				{
					BooleanTest *b = makeNode(BooleanTest);

					b->arg = (Expr *) $1;
					b->booltesttype = IS_TRUE;
					b->location = @2;
					$$ = (Node *) b;
				}
			| a_expr IS NOT TRUE_P						%prec IS
				{
					BooleanTest *b = makeNode(BooleanTest);

					b->arg = (Expr *) $1;
					b->booltesttype = IS_NOT_TRUE;
					b->location = @2;
					$$ = (Node *) b;
				}
			| a_expr IS FALSE_P							%prec IS
				{
					BooleanTest *b = makeNode(BooleanTest);

					b->arg = (Expr *) $1;
					b->booltesttype = IS_FALSE;
					b->location = @2;
					$$ = (Node *) b;
				}
			| a_expr IS NOT FALSE_P						%prec IS
				{
					BooleanTest *b = makeNode(BooleanTest);

					b->arg = (Expr *) $1;
					b->booltesttype = IS_NOT_FALSE;
					b->location = @2;
					$$ = (Node *) b;
				}
			| a_expr IS UNKNOWN							%prec IS
				{
					BooleanTest *b = makeNode(BooleanTest);

					b->arg = (Expr *) $1;
					b->booltesttype = IS_UNKNOWN;
					b->location = @2;
					$$ = (Node *) b;
				}
			| a_expr IS NOT UNKNOWN						%prec IS
				{
					BooleanTest *b = makeNode(BooleanTest);

					b->arg = (Expr *) $1;
					b->booltesttype = IS_NOT_UNKNOWN;
					b->location = @2;
					$$ = (Node *) b;
				}
			| a_expr IS DISTINCT FROM a_expr			%prec IS
				{
					$$ = (Node *) makeSimpleA_Expr(AEXPR_DISTINCT, "=", $1, $5, @2);
				}
			| a_expr IS NOT DISTINCT FROM a_expr		%prec IS
				{
					$$ = (Node *) makeSimpleA_Expr(AEXPR_NOT_DISTINCT, "=", $1, $6, @2);
				}
			| a_expr BETWEEN opt_asymmetric b_expr AND a_expr		%prec BETWEEN
				{
					$$ = (Node *) makeSimpleA_Expr(AEXPR_BETWEEN,
												   "BETWEEN",
												   $1,
												   (Node *) list_make2($4, $6),
												   @2);
				}
			| a_expr NOT_LA BETWEEN opt_asymmetric b_expr AND a_expr %prec NOT_LA
				{
					$$ = (Node *) makeSimpleA_Expr(AEXPR_NOT_BETWEEN,
												   "NOT BETWEEN",
												   $1,
												   (Node *) list_make2($5, $7),
												   @2);
				}
			| a_expr BETWEEN SYMMETRIC b_expr AND a_expr			%prec BETWEEN
				{
					$$ = (Node *) makeSimpleA_Expr(AEXPR_BETWEEN_SYM,
												   "BETWEEN SYMMETRIC",
												   $1,
												   (Node *) list_make2($4, $6),
												   @2);
				}
			| a_expr NOT_LA BETWEEN SYMMETRIC b_expr AND a_expr		%prec NOT_LA
				{
					$$ = (Node *) makeSimpleA_Expr(AEXPR_NOT_BETWEEN_SYM,
												   "NOT BETWEEN SYMMETRIC",
												   $1,
												   (Node *) list_make2($5, $7),
												   @2);
				}
			| a_expr IN_P select_with_parens
				{
					/* generate foo = ANY (subquery) - 生成 foo = ANY (subquery) */
					SubLink	   *n = makeNode(SubLink);

					n->subselect = $3;
					n->subLinkType = ANY_SUBLINK;
					n->subLinkId = 0;
					n->testexpr = $1;
					n->operName = NIL;		/* show it's IN not = ANY - 显示它是 IN 而不是 = ANY */
					n->location = @2;
					$$ = (Node *) n;
				}
			| a_expr IN_P '(' expr_list ')'
				{
					/* generate scalar IN expression - 生成标量 IN 表达式 */
					A_Expr *n = makeSimpleA_Expr(AEXPR_IN, "=", $1, (Node *) $4, @2);

					n->rexpr_list_start = @3;
					n->rexpr_list_end = @5;
					$$ = (Node *) n;
				}
			| a_expr NOT_LA IN_P select_with_parens			%prec NOT_LA
				{
					/* generate NOT (foo = ANY (subquery)) - 生成 NOT (foo = ANY (subquery)) */
					SubLink	   *n = makeNode(SubLink);

					n->subselect = $4;
					n->subLinkType = ANY_SUBLINK;
					n->subLinkId = 0;
					n->testexpr = $1;
					n->operName = NIL;		/* show it's IN not = ANY - 显示它是 IN 而不是 = ANY */
					n->location = @2;
					/* Stick a NOT on top; must have same parse location - 在顶部粘贴一个 NOT；必须具有相同的解析位置 */
					$$ = makeNotExpr((Node *) n, @2);
				}
			| a_expr NOT_LA IN_P '(' expr_list ')'
				{
					/* generate scalar NOT IN expression - 生成标量 NOT IN 表达式 */
					A_Expr *n = makeSimpleA_Expr(AEXPR_IN, "<>", $1, (Node *) $5, @2);

					n->rexpr_list_start = @4;
					n->rexpr_list_end = @6;
					$$ = (Node *) n;
				}
			| a_expr subquery_Op sub_type select_with_parens	%prec Op
				{
					SubLink	   *n = makeNode(SubLink);

					n->subLinkType = $3;
					n->subLinkId = 0;
					n->testexpr = $1;
					n->operName = $2;
					n->subselect = $4;
					n->location = @2;
					$$ = (Node *) n;
				}
			| a_expr subquery_Op sub_type '(' a_expr ')'		%prec Op
				{
					if ($3 == ANY_SUBLINK)
						$$ = (Node *) makeA_Expr(AEXPR_OP_ANY, $2, $1, $5, @2);
					else
						$$ = (Node *) makeA_Expr(AEXPR_OP_ALL, $2, $1, $5, @2);
				}
			| UNIQUE opt_unique_null_treatment select_with_parens
				{
					/* Not sure how to get rid of the parentheses
					 * but there are lots of shift/reduce errors without them.
					 *
					 * Should be able to implement this by plopping the entire
					 * select into a node, then transforming the target expressions
					 * from whatever they are into count(*), and testing the
					 * entire result equal to one.
					 * But, will probably implement a separate node in the executor.
					 * 不知道如何去掉括号，但没有它们会有很多移进/规约错误。应该能够通过将整个 select 放入节点，然后将目标表达式转换为 count(*)，并测试整个结果等于一来实现这一点。但是，可能会在执行器中实现一个单独的节点。
					 */
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("UNIQUE predicate is not yet implemented"),
							 parser_errposition(@1)));
				}
			| a_expr IS DOCUMENT_P					%prec IS
				{
					$$ = makeXmlExpr(IS_DOCUMENT, NULL, NIL,
									 list_make1($1), @2);
				}
			| a_expr IS NOT DOCUMENT_P				%prec IS
				{
					$$ = makeNotExpr(makeXmlExpr(IS_DOCUMENT, NULL, NIL,
												 list_make1($1), @2),
									 @2);
				}
			| a_expr IS NORMALIZED								%prec IS
				{
					$$ = (Node *) makeFuncCall(SystemFuncName("is_normalized"),
											   list_make1($1),
											   COERCE_SQL_SYNTAX,
											   @2);
				}
			| a_expr IS unicode_normal_form NORMALIZED			%prec IS
				{
					$$ = (Node *) makeFuncCall(SystemFuncName("is_normalized"),
											   list_make2($1, makeStringConst($3, @3)),
											   COERCE_SQL_SYNTAX,
											   @2);
				}
			| a_expr IS NOT NORMALIZED							%prec IS
				{
					$$ = makeNotExpr((Node *) makeFuncCall(SystemFuncName("is_normalized"),
														   list_make1($1),
														   COERCE_SQL_SYNTAX,
														   @2),
									 @2);
				}
			| a_expr IS NOT unicode_normal_form NORMALIZED		%prec IS
				{
					$$ = makeNotExpr((Node *) makeFuncCall(SystemFuncName("is_normalized"),
														   list_make2($1, makeStringConst($4, @4)),
														   COERCE_SQL_SYNTAX,
														   @2),
									 @2);
				}
			| a_expr IS json_predicate_type_constraint
					json_key_uniqueness_constraint_opt		%prec IS
				{
					JsonFormat *format = makeJsonFormat(JS_FORMAT_DEFAULT, JS_ENC_DEFAULT, -1);

					$$ = makeJsonIsPredicate($1, format, $3, $4, @1);
				}
			/*
			 * Required by SQL/JSON, but there are conflicts
			| a_expr
				json_format_clause
				IS  json_predicate_type_constraint
					json_key_uniqueness_constraint_opt		%prec IS
				{
					$$ = makeJsonIsPredicate($1, $2, $4, $5, @1);
				}
 SQL/JSON 所需，但存在冲突
			*/
			| a_expr IS NOT
					json_predicate_type_constraint
					json_key_uniqueness_constraint_opt		%prec IS
				{
					JsonFormat *format = makeJsonFormat(JS_FORMAT_DEFAULT, JS_ENC_DEFAULT, -1);

					$$ = makeNotExpr(makeJsonIsPredicate($1, format, $4, $5, @1), @1);
				}
			/*
			 * Required by SQL/JSON, but there are conflicts
			| a_expr
				json_format_clause
				IS NOT
					json_predicate_type_constraint
					json_key_uniqueness_constraint_opt		%prec IS
				{
					$$ = makeNotExpr(makeJsonIsPredicate($1, $2, $5, $6, @1), @1);
				}
 SQL/JSON 所需，但存在冲突（带有 NOT 的情况）
			*/
			| DEFAULT
				{
					/*
					 * The SQL spec only allows DEFAULT in "contextually typed
					 * expressions", but for us, it's easier to allow it in
					 * any a_expr and then throw error during parse analysis
					 * if it's in an inappropriate context.  This way also
					 * lets us say something smarter than "syntax error".
					 * SQL 规范仅允许在 "上下文类型表达式" 中使用 DEFAULT，但对于我们，更容易允许它在任何 a_expr 中使用，然后如果在不恰当的上下文中则在解析分析期间抛出错误。这种方式也让我们能说出比 "语法错误" 更聪明的话。
					 */
					SetToDefault *n = makeNode(SetToDefault);

					/* parse analysis will fill in the rest - 解析分析（parse analysis）将填充其余部分 */
					n->location = @1;
					$$ = (Node *) n;
				}
		;

/*
 * Restricted expressions
 *
 * b_expr is a subset of the complete expression syntax defined by a_expr.
 *
 * Presently, AND, NOT, IS, and IN are the a_expr keywords that would
 * cause trouble in the places where b_expr is used.  For simplicity, we
 * just eliminate all the boolean-keyword-operator productions from b_expr.
 * 受限表达式 b_expr 是由 a_expr 定义的完整表达式语法的子集。目前，AND、NOT、IS 和 IN 是在 b_expr 使用的地方会引起麻烦的 a_expr 关键字。为了简单起见，我们只是从 b_expr 中消除了所有布尔关键字操作符产生式。
 */
b_expr:		c_expr
				{ $$ = $1; }
			| b_expr TYPECAST Typename
				{ $$ = makeTypeCast($1, $3, @2); }
			| '+' b_expr					%prec UMINUS
				{ $$ = (Node *) makeSimpleA_Expr(AEXPR_OP, "+", NULL, $2, @1); }
			| '-' b_expr					%prec UMINUS
				{ $$ = doNegate($2, @1); }
			| b_expr '+' b_expr
				{ $$ = (Node *) makeSimpleA_Expr(AEXPR_OP, "+", $1, $3, @2); }
			| b_expr '-' b_expr
				{ $$ = (Node *) makeSimpleA_Expr(AEXPR_OP, "-", $1, $3, @2); }
			| b_expr '*' b_expr
				{ $$ = (Node *) makeSimpleA_Expr(AEXPR_OP, "*", $1, $3, @2); }
			| b_expr '/' b_expr
				{ $$ = (Node *) makeSimpleA_Expr(AEXPR_OP, "/", $1, $3, @2); }
			| b_expr '%' b_expr
				{ $$ = (Node *) makeSimpleA_Expr(AEXPR_OP, "%", $1, $3, @2); }
			| b_expr '^' b_expr
				{ $$ = (Node *) makeSimpleA_Expr(AEXPR_OP, "^", $1, $3, @2); }
			| b_expr '<' b_expr
				{ $$ = (Node *) makeSimpleA_Expr(AEXPR_OP, "<", $1, $3, @2); }
			| b_expr '>' b_expr
				{ $$ = (Node *) makeSimpleA_Expr(AEXPR_OP, ">", $1, $3, @2); }
			| b_expr '=' b_expr
				{ $$ = (Node *) makeSimpleA_Expr(AEXPR_OP, "=", $1, $3, @2); }
			| b_expr LESS_EQUALS b_expr
				{ $$ = (Node *) makeSimpleA_Expr(AEXPR_OP, "<=", $1, $3, @2); }
			| b_expr GREATER_EQUALS b_expr
				{ $$ = (Node *) makeSimpleA_Expr(AEXPR_OP, ">=", $1, $3, @2); }
			| b_expr NOT_EQUALS b_expr
				{ $$ = (Node *) makeSimpleA_Expr(AEXPR_OP, "<>", $1, $3, @2); }
			| b_expr qual_Op b_expr				%prec Op
				{ $$ = (Node *) makeA_Expr(AEXPR_OP, $2, $1, $3, @2); }
			| qual_Op b_expr					%prec Op
				{ $$ = (Node *) makeA_Expr(AEXPR_OP, $1, NULL, $2, @1); }
			| b_expr IS DISTINCT FROM b_expr		%prec IS
				{
					$$ = (Node *) makeSimpleA_Expr(AEXPR_DISTINCT, "=", $1, $5, @2);
				}
			| b_expr IS NOT DISTINCT FROM b_expr	%prec IS
				{
					$$ = (Node *) makeSimpleA_Expr(AEXPR_NOT_DISTINCT, "=", $1, $6, @2);
				}
			| b_expr IS DOCUMENT_P					%prec IS
				{
					$$ = makeXmlExpr(IS_DOCUMENT, NULL, NIL,
									 list_make1($1), @2);
				}
			| b_expr IS NOT DOCUMENT_P				%prec IS
				{
					$$ = makeNotExpr(makeXmlExpr(IS_DOCUMENT, NULL, NIL,
												 list_make1($1), @2),
									 @2);
				}
		;

/*
 * Productions that can be used in both a_expr and b_expr.
 *
 * Note: productions that refer recursively to a_expr or b_expr mostly
 * cannot appear here.	However, it's OK to refer to a_exprs that occur
 * inside parentheses, such as function arguments; that cannot introduce
 * ambiguity to the b_expr syntax.
 * 可在 a_expr 和 b_expr 中使用的产生式。注意：递归引用 a_expr 或 b_expr 的产生式大多不能出现在这里。然而，引用出现在括号内的 a_expr（如函数参数）是可以的；这不会给 b_expr 语法引入歧义。
 */
c_expr:		columnref								{ $$ = $1; }
			| AexprConst							{ $$ = $1; }
			| PARAM opt_indirection
				{
					ParamRef   *p = makeNode(ParamRef);

					p->number = $1;
					p->location = @1;
					if ($2)
					{
						A_Indirection *n = makeNode(A_Indirection);

						n->arg = (Node *) p;
						n->indirection = check_indirection($2, yyscanner);
						$$ = (Node *) n;
					}
					else
						$$ = (Node *) p;
				}
			| '(' a_expr ')' opt_indirection
				{
					if ($4)
					{
						A_Indirection *n = makeNode(A_Indirection);

						n->arg = $2;
						n->indirection = check_indirection($4, yyscanner);
						$$ = (Node *) n;
					}
					else
						$$ = $2;
				}
			| case_expr
				{ $$ = $1; }
			| func_expr
				{ $$ = $1; }
			| select_with_parens			%prec UMINUS
				{
					SubLink	   *n = makeNode(SubLink);

					n->subLinkType = EXPR_SUBLINK;
					n->subLinkId = 0;
					n->testexpr = NULL;
					n->operName = NIL;
					n->subselect = $1;
					n->location = @1;
					$$ = (Node *) n;
				}
			| select_with_parens indirection
				{
					/*
					 * Because the select_with_parens nonterminal is designed
					 * to "eat" as many levels of parens as possible, the
					 * '(' a_expr ')' opt_indirection production above will
					 * fail to match a sub-SELECT with indirection decoration;
					 * the sub-SELECT won't be regarded as an a_expr as long
					 * as there are parens around it.  To support applying
					 * subscripting or field selection to a sub-SELECT result,
					 * we need this redundant-looking production.
					 * 由于 select_with_parens 非终结符设计为尽可能多地 "吞掉" 多层括号，上面的 '(' a_expr ')' opt_indirection 产生式将无法匹配带有间接修饰的子 SELECT；只要周围有括号，子 SELECT 就不会被视为 a_expr。为了支持对子 SELECT 结果应用下标或字段选择，我们需要这个看起来冗余的产生式。
					 */
					SubLink	   *n = makeNode(SubLink);
					A_Indirection *a = makeNode(A_Indirection);

					n->subLinkType = EXPR_SUBLINK;
					n->subLinkId = 0;
					n->testexpr = NULL;
					n->operName = NIL;
					n->subselect = $1;
					n->location = @1;
					a->arg = (Node *) n;
					a->indirection = check_indirection($2, yyscanner);
					$$ = (Node *) a;
				}
			| EXISTS select_with_parens
				{
					SubLink	   *n = makeNode(SubLink);

					n->subLinkType = EXISTS_SUBLINK;
					n->subLinkId = 0;
					n->testexpr = NULL;
					n->operName = NIL;
					n->subselect = $2;
					n->location = @1;
					$$ = (Node *) n;
				}
			| ARRAY select_with_parens
				{
					SubLink	   *n = makeNode(SubLink);

					n->subLinkType = ARRAY_SUBLINK;
					n->subLinkId = 0;
					n->testexpr = NULL;
					n->operName = NIL;
					n->subselect = $2;
					n->location = @1;
					$$ = (Node *) n;
				}
			| ARRAY array_expr
				{
					A_ArrayExpr *n = castNode(A_ArrayExpr, $2);

					/* point outermost A_ArrayExpr to the ARRAY keyword - 将最外层的 A_ArrayExpr 指向 ARRAY 关键字 */
					n->location = @1;
					$$ = (Node *) n;
				}
			| explicit_row
				{
					RowExpr	   *r = makeNode(RowExpr);

					r->args = $1;
					r->row_typeid = InvalidOid;	/* not analyzed yet - 尚未分析 */
					r->colnames = NIL;	/* to be filled in during analysis - 将在分析期间填充 */
					r->row_format = COERCE_EXPLICIT_CALL; /* abuse - 滥用 */
					r->location = @1;
					$$ = (Node *) r;
				}
			| implicit_row
				{
					RowExpr	   *r = makeNode(RowExpr);

					r->args = $1;
					r->row_typeid = InvalidOid;	/* not analyzed yet - 尚未分析 */
					r->colnames = NIL;	/* to be filled in during analysis - 将在分析期间填充 */
					r->row_format = COERCE_IMPLICIT_CAST; /* abuse - 滥用 */
					r->location = @1;
					$$ = (Node *) r;
				}
			| GROUPING '(' expr_list ')'
			  {
				  GroupingFunc *g = makeNode(GroupingFunc);

				  g->args = $3;
				  g->location = @1;
				  $$ = (Node *) g;
			  }
		;

func_application: func_name '(' ')'
				{
					$$ = (Node *) makeFuncCall($1, NIL,
											   COERCE_EXPLICIT_CALL,
											   @1);
				}
			| func_name '(' func_arg_list opt_sort_clause ')'
				{
					FuncCall   *n = makeFuncCall($1, $3,
												 COERCE_EXPLICIT_CALL,
												 @1);

					n->agg_order = $4;
					$$ = (Node *) n;
				}
			| func_name '(' VARIADIC func_arg_expr opt_sort_clause ')'
				{
					FuncCall   *n = makeFuncCall($1, list_make1($4),
												 COERCE_EXPLICIT_CALL,
												 @1);

					n->func_variadic = true;
					n->agg_order = $5;
					$$ = (Node *) n;
				}
			| func_name '(' func_arg_list ',' VARIADIC func_arg_expr opt_sort_clause ')'
				{
					FuncCall   *n = makeFuncCall($1, lappend($3, $6),
												 COERCE_EXPLICIT_CALL,
												 @1);

					n->func_variadic = true;
					n->agg_order = $7;
					$$ = (Node *) n;
				}
			| func_name '(' ALL func_arg_list opt_sort_clause ')'
				{
					FuncCall   *n = makeFuncCall($1, $4,
												 COERCE_EXPLICIT_CALL,
												 @1);

					n->agg_order = $5;
					/* Ideally we'd mark the FuncCall node to indicate
					 * "must be an aggregate", but there's no provision
					 * for that in FuncCall at the moment.
					 * 理想情况下，我们会标记 FuncCall 节点以指示 "必须是聚合"，但目前 FuncCall 中没有此规定。
					 */
					$$ = (Node *) n;
				}
			| func_name '(' DISTINCT func_arg_list opt_sort_clause ')'
				{
					FuncCall   *n = makeFuncCall($1, $4,
												 COERCE_EXPLICIT_CALL,
												 @1);

					n->agg_order = $5;
					n->agg_distinct = true;
					$$ = (Node *) n;
				}
			| func_name '(' '*' ')'
				{
					/*
					 * We consider AGGREGATE(*) to invoke a parameterless
					 * aggregate.  This does the right thing for COUNT(*),
					 * and there are no other aggregates in SQL that accept
					 * '*' as parameter.
					 *
					 * The FuncCall node is also marked agg_star = true,
					 * so that later processing can detect what the argument
					 * really was.
					 * 我们认为 AGGREGATE(*) 调用了无参数的聚合。这对于 COUNT(*) 是正确的处理，并且 SQL 中没有其他聚合接受 '*' 作为参数。FuncCall 节点也被标记为 agg_star = true，以便稍后的处理可以检测到实际参数是什么。
					 */
					FuncCall   *n = makeFuncCall($1, NIL,
												 COERCE_EXPLICIT_CALL,
												 @1);

					n->agg_star = true;
					$$ = (Node *) n;
				}
		;


/*
 * func_expr and its cousin func_expr_windowless are split out from c_expr just
 * so that we have classifications for "everything that is a function call or
 * looks like one".  This isn't very important, but it saves us having to
 * document which variants are legal in places like "FROM function()" or the
 * backwards-compatible functional-index syntax for CREATE INDEX.
 * (Note that many of the special SQL functions wouldn't actually make any
 * sense as functional index entries, but we ignore that consideration here.)
 * func_expr 及其近亲 func_expr_windowless 从 c_expr 中分离出来，只是为了我们能对 "所有是函数调用或看起来像函数调用的内容" 进行分类。这并不是非常重要，但它免去了我们记录哪些变体在 "FROM function()" 等地方是合法的。
 */
func_expr: func_application within_group_clause filter_clause over_clause
				{
					FuncCall   *n = (FuncCall *) $1;

					/*
					 * The order clause for WITHIN GROUP and the one for
					 * plain-aggregate ORDER BY share a field, so we have to
					 * check here that at most one is present.  We also check
					 * for DISTINCT and VARIADIC here to give a better error
					 * location.  Other consistency checks are deferred to
					 * parse analysis.
					 * WITHIN GROUP 的 order 子句和普通聚合的 ORDER BY 共享一个字段，因此我们必须在此处检查是否最多只存在一个。我们在此处还检查 DISTINCT 和 VARIADIC，以提供更好的错误位置。其他一致性检查延迟到解析分析中。
					 */
					if ($2 != NIL)
					{
						if (n->agg_order != NIL)
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("cannot use multiple ORDER BY clauses with WITHIN GROUP"),
									 parser_errposition(@2)));
						if (n->agg_distinct)
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("cannot use DISTINCT with WITHIN GROUP"),
									 parser_errposition(@2)));
						if (n->func_variadic)
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("cannot use VARIADIC with WITHIN GROUP"),
									 parser_errposition(@2)));
						n->agg_order = $2;
						n->agg_within_group = true;
					}
					n->agg_filter = $3;
					n->over = $4;
					$$ = (Node *) n;
				}
			| json_aggregate_func filter_clause over_clause
				{
					JsonAggConstructor *n = IsA($1, JsonObjectAgg) ?
						((JsonObjectAgg *) $1)->constructor :
						((JsonArrayAgg *) $1)->constructor;

					n->agg_filter = $2;
					n->over = $3;
					$$ = (Node *) $1;
				}
			| func_expr_common_subexpr
				{ $$ = $1; }
		;

/*
 * Like func_expr but does not accept WINDOW functions directly
 * (but they can still be contained in arguments for functions etc).
 * Use this when window expressions are not allowed, where needed to
 * disambiguate the grammar (e.g. in CREATE INDEX).
 * 与 func_expr 类似，但不直接接受 WINDOW 函数（但它们仍然可以包含在函数的参数等中）。在不允许窗口表达式时使用此项，以在需要时消除语法歧义（例如在 CREATE INDEX 中）。
 */
func_expr_windowless:
			func_application						{ $$ = $1; }
			| func_expr_common_subexpr				{ $$ = $1; }
			| json_aggregate_func					{ $$ = $1; }
		;

/*
 * Special expressions that are considered to be functions.
 * 被视为函数的特殊表达式。
 */
func_expr_common_subexpr:
			COLLATION FOR '(' a_expr ')'
				{
					$$ = (Node *) makeFuncCall(SystemFuncName("pg_collation_for"),
											   list_make1($4),
											   COERCE_SQL_SYNTAX,
											   @1);
				}
			| CURRENT_DATE
				{
					$$ = makeSQLValueFunction(SVFOP_CURRENT_DATE, -1, @1);
				}
			| CURRENT_TIME
				{
					$$ = makeSQLValueFunction(SVFOP_CURRENT_TIME, -1, @1);
				}
			| CURRENT_TIME '(' Iconst ')'
				{
					$$ = makeSQLValueFunction(SVFOP_CURRENT_TIME_N, $3, @1);
				}
			| CURRENT_TIMESTAMP
				{
					$$ = makeSQLValueFunction(SVFOP_CURRENT_TIMESTAMP, -1, @1);
				}
			| CURRENT_TIMESTAMP '(' Iconst ')'
				{
					$$ = makeSQLValueFunction(SVFOP_CURRENT_TIMESTAMP_N, $3, @1);
				}
			| LOCALTIME
				{
					$$ = makeSQLValueFunction(SVFOP_LOCALTIME, -1, @1);
				}
			| LOCALTIME '(' Iconst ')'
				{
					$$ = makeSQLValueFunction(SVFOP_LOCALTIME_N, $3, @1);
				}
			| LOCALTIMESTAMP
				{
					$$ = makeSQLValueFunction(SVFOP_LOCALTIMESTAMP, -1, @1);
				}
			| LOCALTIMESTAMP '(' Iconst ')'
				{
					$$ = makeSQLValueFunction(SVFOP_LOCALTIMESTAMP_N, $3, @1);
				}
			| CURRENT_ROLE
				{
					$$ = makeSQLValueFunction(SVFOP_CURRENT_ROLE, -1, @1);
				}
			| CURRENT_USER
				{
					$$ = makeSQLValueFunction(SVFOP_CURRENT_USER, -1, @1);
				}
			| SESSION_USER
				{
					$$ = makeSQLValueFunction(SVFOP_SESSION_USER, -1, @1);
				}
			| SYSTEM_USER
				{
					$$ = (Node *) makeFuncCall(SystemFuncName("system_user"),
											   NIL,
											   COERCE_SQL_SYNTAX,
											   @1);
				}
			| USER
				{
					$$ = makeSQLValueFunction(SVFOP_USER, -1, @1);
				}
			| CURRENT_CATALOG
				{
					$$ = makeSQLValueFunction(SVFOP_CURRENT_CATALOG, -1, @1);
				}
			| CURRENT_SCHEMA
				{
					$$ = makeSQLValueFunction(SVFOP_CURRENT_SCHEMA, -1, @1);
				}
			| CAST '(' a_expr AS Typename ')'
				{ $$ = makeTypeCast($3, $5, @1); }
			| EXTRACT '(' extract_list ')'
				{
					$$ = (Node *) makeFuncCall(SystemFuncName("extract"),
											   $3,
											   COERCE_SQL_SYNTAX,
											   @1);
				}
			| NORMALIZE '(' a_expr ')'
				{
					$$ = (Node *) makeFuncCall(SystemFuncName("normalize"),
											   list_make1($3),
											   COERCE_SQL_SYNTAX,
											   @1);
				}
			| NORMALIZE '(' a_expr ',' unicode_normal_form ')'
				{
					$$ = (Node *) makeFuncCall(SystemFuncName("normalize"),
											   list_make2($3, makeStringConst($5, @5)),
											   COERCE_SQL_SYNTAX,
											   @1);
				}
			| OVERLAY '(' overlay_list ')'
				{
					$$ = (Node *) makeFuncCall(SystemFuncName("overlay"),
											   $3,
											   COERCE_SQL_SYNTAX,
											   @1);
				}
			| OVERLAY '(' func_arg_list_opt ')'
				{
					/*
					 * allow functions named overlay() to be called without
					 * special syntax
					 * 允许无特殊语法调用名为 overlay() 的函数
					 */
					$$ = (Node *) makeFuncCall(list_make1(makeString("overlay")),
											   $3,
											   COERCE_EXPLICIT_CALL,
											   @1);
				}
			| POSITION '(' position_list ')'
				{
					/*
					 * position(A in B) is converted to position(B, A)
					 *
					 * We deliberately don't offer a "plain syntax" option
					 * for position(), because the reversal of the arguments
					 * creates too much risk of confusion.
					 * position(A in B) 转换为 position(B, A)。我们故意不为 position() 提供 "普通语法" 选项，因为参数的反转会带来太多的混淆风险。
					 */
					$$ = (Node *) makeFuncCall(SystemFuncName("position"),
											   $3,
											   COERCE_SQL_SYNTAX,
											   @1);
				}
			| SUBSTRING '(' substr_list ')'
				{
					/* substring(A from B for C) is converted to
					 * substring(A, B, C) - thomas 2000-11-28
					 * substring(A from B for C) 转换为 substring(A, B, C) - thomas 2000-11-28
					 */
					$$ = (Node *) makeFuncCall(SystemFuncName("substring"),
											   $3,
											   COERCE_SQL_SYNTAX,
											   @1);
				}
			| SUBSTRING '(' func_arg_list_opt ')'
				{
					/*
					 * allow functions named substring() to be called without
					 * special syntax
					 * 允许无特殊语法调用名为 substring() 的函数
					 */
					$$ = (Node *) makeFuncCall(list_make1(makeString("substring")),
											   $3,
											   COERCE_EXPLICIT_CALL,
											   @1);
				}
			| TREAT '(' a_expr AS Typename ')'
				{
					/* TREAT(expr AS target) converts expr of a particular type to target,
					 * which is defined to be a subtype of the original expression.
					 * In SQL99, this is intended for use with structured UDTs,
					 * but let's make this a generally useful form allowing stronger
					 * coercions than are handled by implicit casting.
					 *
					 * Convert SystemTypeName() to SystemFuncName() even though
					 * at the moment they result in the same thing.
					 * TREAT(expr AS target) 将特定类型的 expr 转换为 target，后者定义为原始表达式的子类型。在 SQL99 中，这旨在与结构化 UDT 一起使用，但让我们将其作为一种通常有用的形式，允许比隐式类型转换处理的转换更强的强制转换。将 SystemTypeName() 转换为 SystemFuncName()，即使目前它们的结果相同。
					 */
					$$ = (Node *) makeFuncCall(SystemFuncName(strVal(llast($5->names))),
											   list_make1($3),
											   COERCE_EXPLICIT_CALL,
											   @1);
				}
			| TRIM '(' BOTH trim_list ')'
				{
					/* various trim expressions are defined in SQL
					 * - thomas 1997-07-19
					 * SQL 中定义了各种 trim 表达式 - thomas 1997-07-19
					 */
					$$ = (Node *) makeFuncCall(SystemFuncName("btrim"),
											   $4,
											   COERCE_SQL_SYNTAX,
											   @1);
				}
			| TRIM '(' LEADING trim_list ')'
				{
					$$ = (Node *) makeFuncCall(SystemFuncName("ltrim"),
											   $4,
											   COERCE_SQL_SYNTAX,
											   @1);
				}
			| TRIM '(' TRAILING trim_list ')'
				{
					$$ = (Node *) makeFuncCall(SystemFuncName("rtrim"),
											   $4,
											   COERCE_SQL_SYNTAX,
											   @1);
				}
			| TRIM '(' trim_list ')'
				{
					$$ = (Node *) makeFuncCall(SystemFuncName("btrim"),
											   $3,
											   COERCE_SQL_SYNTAX,
											   @1);
				}
			| NULLIF '(' a_expr ',' a_expr ')'
				{
					$$ = (Node *) makeSimpleA_Expr(AEXPR_NULLIF, "=", $3, $5, @1);
				}
			| COALESCE '(' expr_list ')'
				{
					CoalesceExpr *c = makeNode(CoalesceExpr);

					c->args = $3;
					c->location = @1;
					$$ = (Node *) c;
				}
			| GREATEST '(' expr_list ')'
				{
					MinMaxExpr *v = makeNode(MinMaxExpr);

					v->args = $3;
					v->op = IS_GREATEST;
					v->location = @1;
					$$ = (Node *) v;
				}
			| LEAST '(' expr_list ')'
				{
					MinMaxExpr *v = makeNode(MinMaxExpr);

					v->args = $3;
					v->op = IS_LEAST;
					v->location = @1;
					$$ = (Node *) v;
				}
			| XMLCONCAT '(' expr_list ')'
				{
					$$ = makeXmlExpr(IS_XMLCONCAT, NULL, NIL, $3, @1);
				}
			| XMLELEMENT '(' NAME_P ColLabel ')'
				{
					$$ = makeXmlExpr(IS_XMLELEMENT, $4, NIL, NIL, @1);
				}
			| XMLELEMENT '(' NAME_P ColLabel ',' xml_attributes ')'
				{
					$$ = makeXmlExpr(IS_XMLELEMENT, $4, $6, NIL, @1);
				}
			| XMLELEMENT '(' NAME_P ColLabel ',' expr_list ')'
				{
					$$ = makeXmlExpr(IS_XMLELEMENT, $4, NIL, $6, @1);
				}
			| XMLELEMENT '(' NAME_P ColLabel ',' xml_attributes ',' expr_list ')'
				{
					$$ = makeXmlExpr(IS_XMLELEMENT, $4, $6, $8, @1);
				}
			| XMLEXISTS '(' c_expr xmlexists_argument ')'
				{
					/* xmlexists(A PASSING [BY REF] B [BY REF]) is
/* xmlexists(A PASSING [BY REF] B [BY REF]) 转换为 xmlexists(A, B)
					 * converted to xmlexists(A, B)*/
					$$ = (Node *) makeFuncCall(SystemFuncName("xmlexists"),
											   list_make2($3, $4),
											   COERCE_SQL_SYNTAX,
											   @1);
				}
			| XMLFOREST '(' xml_attribute_list ')'
				{
					$$ = makeXmlExpr(IS_XMLFOREST, NULL, $3, NIL, @1);
				}
			| XMLPARSE '(' document_or_content a_expr xml_whitespace_option ')'
				{
					XmlExpr *x = (XmlExpr *)
						makeXmlExpr(IS_XMLPARSE, NULL, NIL,
									list_make2($4, makeBoolAConst($5, -1)),
									@1);

					x->xmloption = $3;
					$$ = (Node *) x;
				}
			| XMLPI '(' NAME_P ColLabel ')'
				{
					$$ = makeXmlExpr(IS_XMLPI, $4, NULL, NIL, @1);
				}
			| XMLPI '(' NAME_P ColLabel ',' a_expr ')'
				{
					$$ = makeXmlExpr(IS_XMLPI, $4, NULL, list_make1($6), @1);
				}
			| XMLROOT '(' a_expr ',' xml_root_version opt_xml_root_standalone ')'
				{
					$$ = makeXmlExpr(IS_XMLROOT, NULL, NIL,
									 list_make3($3, $5, $6), @1);
				}
			| XMLSERIALIZE '(' document_or_content a_expr AS SimpleTypename xml_indent_option ')'
				{
					XmlSerialize *n = makeNode(XmlSerialize);

					n->xmloption = $3;
					n->expr = $4;
					n->typeName = $6;
					n->indent = $7;
					n->location = @1;
					$$ = (Node *) n;
				}
			| JSON_OBJECT '(' func_arg_list ')'
				{
					/* Support for legacy (non-standard) json_object() - 对遗留的（非标准的）json_object() 的支持 */
					$$ = (Node *) makeFuncCall(SystemFuncName("json_object"),
											   $3, COERCE_EXPLICIT_CALL, @1);
				}
			| JSON_OBJECT '(' json_name_and_value_list
				json_object_constructor_null_clause_opt
				json_key_uniqueness_constraint_opt
				json_returning_clause_opt ')'
				{
					JsonObjectConstructor *n = makeNode(JsonObjectConstructor);

					n->exprs = $3;
					n->absent_on_null = $4;
					n->unique = $5;
					n->output = (JsonOutput *) $6;
					n->location = @1;
					$$ = (Node *) n;
				}
			| JSON_OBJECT '(' json_returning_clause_opt ')'
				{
					JsonObjectConstructor *n = makeNode(JsonObjectConstructor);

					n->exprs = NULL;
					n->absent_on_null = false;
					n->unique = false;
					n->output = (JsonOutput *) $3;
					n->location = @1;
					$$ = (Node *) n;
				}
			| JSON_ARRAY '('
				json_value_expr_list
				json_array_constructor_null_clause_opt
				json_returning_clause_opt
			')'
				{
					JsonArrayConstructor *n = makeNode(JsonArrayConstructor);

					n->exprs = $3;
					n->absent_on_null = $4;
					n->output = (JsonOutput *) $5;
					n->location = @1;
					$$ = (Node *) n;
				}
			| JSON_ARRAY '('
				select_no_parens
				json_format_clause_opt
				/* json_array_constructor_null_clause_opt - json_array_constructor_null_clause_opt 选项 */
				json_returning_clause_opt
			')'
				{
					JsonArrayQueryConstructor *n = makeNode(JsonArrayQueryConstructor);

					n->query = $3;
					n->format = (JsonFormat *) $4;
					n->absent_on_null = true;	/* XXX - XXX 标记 */
					n->output = (JsonOutput *) $5;
					n->location = @1;
					$$ = (Node *) n;
				}
			| JSON_ARRAY '('
				json_returning_clause_opt
			')'
				{
					JsonArrayConstructor *n = makeNode(JsonArrayConstructor);

					n->exprs = NIL;
					n->absent_on_null = true;
					n->output = (JsonOutput *) $3;
					n->location = @1;
					$$ = (Node *) n;
				}
			| JSON '(' json_value_expr json_key_uniqueness_constraint_opt ')'
				{
					JsonParseExpr *n = makeNode(JsonParseExpr);

					n->expr = (JsonValueExpr *) $3;
					n->unique_keys = $4;
					n->output = NULL;
					n->location = @1;
					$$ = (Node *) n;
				}
			| JSON_SCALAR '(' a_expr ')'
				{
					JsonScalarExpr *n = makeNode(JsonScalarExpr);

					n->expr = (Expr *) $3;
					n->output = NULL;
					n->location = @1;
					$$ = (Node *) n;
				}
			| JSON_SERIALIZE '(' json_value_expr json_returning_clause_opt ')'
				{
					JsonSerializeExpr *n = makeNode(JsonSerializeExpr);

					n->expr = (JsonValueExpr *) $3;
					n->output = (JsonOutput *) $4;
					n->location = @1;
					$$ = (Node *) n;
				}
			| MERGE_ACTION '(' ')'
				{
					MergeSupportFunc *m = makeNode(MergeSupportFunc);

					m->msftype = TEXTOID;
					m->location = @1;
					$$ = (Node *) m;
				}
			| JSON_QUERY '('
				json_value_expr ',' a_expr json_passing_clause_opt
				json_returning_clause_opt
				json_wrapper_behavior
				json_quotes_clause_opt
				json_behavior_clause_opt
			')'
				{
					JsonFuncExpr *n = makeNode(JsonFuncExpr);

					n->op = JSON_QUERY_OP;
					n->context_item = (JsonValueExpr *) $3;
					n->pathspec = $5;
					n->passing = $6;
					n->output = (JsonOutput *) $7;
					n->wrapper = $8;
					n->quotes = $9;
					n->on_empty = (JsonBehavior *) linitial($10);
					n->on_error = (JsonBehavior *) lsecond($10);
					n->location = @1;
					$$ = (Node *) n;
				}
			| JSON_EXISTS '('
				json_value_expr ',' a_expr json_passing_clause_opt
				json_on_error_clause_opt
			')'
				{
					JsonFuncExpr *n = makeNode(JsonFuncExpr);

					n->op = JSON_EXISTS_OP;
					n->context_item = (JsonValueExpr *) $3;
					n->pathspec = $5;
					n->passing = $6;
					n->output = NULL;
					n->on_error = (JsonBehavior *) $7;
					n->location = @1;
					$$ = (Node *) n;
				}
			| JSON_VALUE '('
				json_value_expr ',' a_expr json_passing_clause_opt
				json_returning_clause_opt
				json_behavior_clause_opt
			')'
				{
					JsonFuncExpr *n = makeNode(JsonFuncExpr);

					n->op = JSON_VALUE_OP;
					n->context_item = (JsonValueExpr *) $3;
					n->pathspec = $5;
					n->passing = $6;
					n->output = (JsonOutput *) $7;
					n->on_empty = (JsonBehavior *) linitial($8);
					n->on_error = (JsonBehavior *) lsecond($8);
					n->location = @1;
					$$ = (Node *) n;
				}
			;


/*
 * SQL/XML support
 * SQL/XML 支持
 */
xml_root_version: VERSION_P a_expr
				{ $$ = $2; }
			| VERSION_P NO VALUE_P
				{ $$ = makeNullAConst(-1); }
		;

opt_xml_root_standalone: ',' STANDALONE_P YES_P
				{ $$ = makeIntConst(XML_STANDALONE_YES, -1); }
			| ',' STANDALONE_P NO
				{ $$ = makeIntConst(XML_STANDALONE_NO, -1); }
			| ',' STANDALONE_P NO VALUE_P
				{ $$ = makeIntConst(XML_STANDALONE_NO_VALUE, -1); }
			| /* EMPTY - 空 */
				{ $$ = makeIntConst(XML_STANDALONE_OMITTED, -1); }
		;

xml_attributes: XMLATTRIBUTES '(' xml_attribute_list ')'	{ $$ = $3; }
		;

xml_attribute_list:	xml_attribute_el					{ $$ = list_make1($1); }
			| xml_attribute_list ',' xml_attribute_el	{ $$ = lappend($1, $3); }
		;

xml_attribute_el: a_expr AS ColLabel
				{
					$$ = makeNode(ResTarget);
					$$->name = $3;
					$$->indirection = NIL;
					$$->val = (Node *) $1;
					$$->location = @1;
				}
			| a_expr
				{
					$$ = makeNode(ResTarget);
					$$->name = NULL;
					$$->indirection = NIL;
					$$->val = (Node *) $1;
					$$->location = @1;
				}
		;

document_or_content: DOCUMENT_P						{ $$ = XMLOPTION_DOCUMENT; }
			| CONTENT_P								{ $$ = XMLOPTION_CONTENT; }
		;

xml_indent_option: INDENT							{ $$ = true; }
			| NO INDENT								{ $$ = false; }
			| /* EMPTY - 空 */								{ $$ = false; }
		;

xml_whitespace_option: PRESERVE WHITESPACE_P		{ $$ = true; }
			| STRIP_P WHITESPACE_P					{ $$ = false; }
			| /* EMPTY - 空 */								{ $$ = false; }
		;

/* We allow several variants for SQL and other compatibility. - 我们为了 SQL 和其它兼容性允许几种变体。 */
xmlexists_argument:
			PASSING c_expr
				{
					$$ = $2;
				}
			| PASSING c_expr xml_passing_mech
				{
					$$ = $2;
				}
			| PASSING xml_passing_mech c_expr
				{
					$$ = $3;
				}
			| PASSING xml_passing_mech c_expr xml_passing_mech
				{
					$$ = $3;
				}
		;

xml_passing_mech:
			BY REF_P
			| BY VALUE_P
		;


/*
 * Aggregate decoration clauses
 * 聚合装饰子句
 */
within_group_clause:
			WITHIN GROUP_P '(' sort_clause ')'		{ $$ = $4; }
			| /* EMPTY - 空 */								{ $$ = NIL; }
		;

filter_clause:
			FILTER '(' WHERE a_expr ')'				{ $$ = $4; }
			| /* EMPTY - 空 */								{ $$ = NULL; }
		;


/*
 * Window Definitions
 * 窗口定义（Window Definitions）
 */
window_clause:
			WINDOW window_definition_list			{ $$ = $2; }
			| /* EMPTY - 空 */								{ $$ = NIL; }
		;

window_definition_list:
			window_definition						{ $$ = list_make1($1); }
			| window_definition_list ',' window_definition
													{ $$ = lappend($1, $3); }
		;

window_definition:
			ColId AS window_specification
				{
					WindowDef  *n = $3;

					n->name = $1;
					$$ = n;
				}
		;

over_clause: OVER window_specification
				{ $$ = $2; }
			| OVER ColId
				{
					WindowDef  *n = makeNode(WindowDef);

					n->name = $2;
					n->refname = NULL;
					n->partitionClause = NIL;
					n->orderClause = NIL;
					n->frameOptions = FRAMEOPTION_DEFAULTS;
					n->startOffset = NULL;
					n->endOffset = NULL;
					n->location = @2;
					$$ = n;
				}
			| /* EMPTY - 空 */
				{ $$ = NULL; }
		;

window_specification: '(' opt_existing_window_name opt_partition_clause
						opt_sort_clause opt_frame_clause ')'
				{
					WindowDef  *n = makeNode(WindowDef);

					n->name = NULL;
					n->refname = $2;
					n->partitionClause = $3;
					n->orderClause = $4;
					/* copy relevant fields of opt_frame_clause - 复制 opt_frame_clause 的相关字段 */
					n->frameOptions = $5->frameOptions;
					n->startOffset = $5->startOffset;
					n->endOffset = $5->endOffset;
					n->location = @1;
					$$ = n;
				}
		;

/*
 * If we see PARTITION, RANGE, ROWS or GROUPS as the first token after the '('
 * of a window_specification, we want the assumption to be that there is
 * no existing_window_name; but those keywords are unreserved and so could
 * be ColIds.  We fix this by making them have the same precedence as IDENT
 * and giving the empty production here a slightly higher precedence, so
 * that the shift/reduce conflict is resolved in favor of reducing the rule.
 * These keywords are thus precluded from being an existing_window_name but
 * are not reserved for any other purpose.
 * 如果我们在 window_specification 的 '(' 之后看到 PARTITION、RANGE、ROWS 或 GROUPS 作为第一个 Token，我们希望假设没有 existing_window_name；但这些关键字是未保留的，因此可以是 ColId。我们通过使它们具有与 IDENT 相同的优先级并赋予这里的空产生式略高的优先级来解决此问题，从而使移进/规约冲突有利于规约该规则。因此，这些关键字被排除在作为 existing_window_name 的可能性之外，但并未因任何其他目的而被保留。
 */
opt_existing_window_name: ColId						{ $$ = $1; }
			| /* EMPTY - 空 */				%prec Op		{ $$ = NULL; }
		;

opt_partition_clause: PARTITION BY expr_list		{ $$ = $3; }
			| /* EMPTY - 空 */								{ $$ = NIL; }
		;

/*
 * For frame clauses, we return a WindowDef, but only some fields are used:
 * frameOptions, startOffset, and endOffset.
 * 对于 frame 子句，我们返回一个 WindowDef，但仅使用某些字段：frameOptions、startOffset 和 endOffset。
 */
opt_frame_clause:
			RANGE frame_extent opt_window_exclusion_clause
				{
					WindowDef  *n = $2;

					n->frameOptions |= FRAMEOPTION_NONDEFAULT | FRAMEOPTION_RANGE;
					n->frameOptions |= $3;
					$$ = n;
				}
			| ROWS frame_extent opt_window_exclusion_clause
				{
					WindowDef  *n = $2;

					n->frameOptions |= FRAMEOPTION_NONDEFAULT | FRAMEOPTION_ROWS;
					n->frameOptions |= $3;
					$$ = n;
				}
			| GROUPS frame_extent opt_window_exclusion_clause
				{
					WindowDef  *n = $2;

					n->frameOptions |= FRAMEOPTION_NONDEFAULT | FRAMEOPTION_GROUPS;
					n->frameOptions |= $3;
					$$ = n;
				}
			| /* EMPTY - 空 */
				{
					WindowDef  *n = makeNode(WindowDef);

					n->frameOptions = FRAMEOPTION_DEFAULTS;
					n->startOffset = NULL;
					n->endOffset = NULL;
					$$ = n;
				}
		;

frame_extent: frame_bound
				{
					WindowDef  *n = $1;

					/* reject invalid cases - 拒绝无效情况 */
					if (n->frameOptions & FRAMEOPTION_START_UNBOUNDED_FOLLOWING)
						ereport(ERROR,
								(errcode(ERRCODE_WINDOWING_ERROR),
								 errmsg("frame start cannot be UNBOUNDED FOLLOWING"),
								 parser_errposition(@1)));
					if (n->frameOptions & FRAMEOPTION_START_OFFSET_FOLLOWING)
						ereport(ERROR,
								(errcode(ERRCODE_WINDOWING_ERROR),
								 errmsg("frame starting from following row cannot end with current row"),
								 parser_errposition(@1)));
					n->frameOptions |= FRAMEOPTION_END_CURRENT_ROW;
					$$ = n;
				}
			| BETWEEN frame_bound AND frame_bound
				{
					WindowDef  *n1 = $2;
					WindowDef  *n2 = $4;

					/* form merged options - 形成合并的选项 */
					int		frameOptions = n1->frameOptions;
					/* shift converts START_ options to END_ options - 移位将 START_ 选项转换为 END_ 选项 */
					frameOptions |= n2->frameOptions << 1;
					frameOptions |= FRAMEOPTION_BETWEEN;
					/* reject invalid cases - 拒绝无效情况 */
					if (frameOptions & FRAMEOPTION_START_UNBOUNDED_FOLLOWING)
						ereport(ERROR,
								(errcode(ERRCODE_WINDOWING_ERROR),
								 errmsg("frame start cannot be UNBOUNDED FOLLOWING"),
								 parser_errposition(@2)));
					if (frameOptions & FRAMEOPTION_END_UNBOUNDED_PRECEDING)
						ereport(ERROR,
								(errcode(ERRCODE_WINDOWING_ERROR),
								 errmsg("frame end cannot be UNBOUNDED PRECEDING"),
								 parser_errposition(@4)));
					if ((frameOptions & FRAMEOPTION_START_CURRENT_ROW) &&
						(frameOptions & FRAMEOPTION_END_OFFSET_PRECEDING))
						ereport(ERROR,
								(errcode(ERRCODE_WINDOWING_ERROR),
								 errmsg("frame starting from current row cannot have preceding rows"),
								 parser_errposition(@4)));
					if ((frameOptions & FRAMEOPTION_START_OFFSET_FOLLOWING) &&
						(frameOptions & (FRAMEOPTION_END_OFFSET_PRECEDING |
										 FRAMEOPTION_END_CURRENT_ROW)))
						ereport(ERROR,
								(errcode(ERRCODE_WINDOWING_ERROR),
								 errmsg("frame starting from following row cannot have preceding rows"),
								 parser_errposition(@4)));
					n1->frameOptions = frameOptions;
					n1->endOffset = n2->startOffset;
					$$ = n1;
				}
		;

/*
 * This is used for both frame start and frame end, with output set up on
 * the assumption it's frame start; the frame_extent productions must reject
 * invalid cases.
 * 这既用于 frame 的开始，也用于 frame 的结束，输出基于它是 frame 开始的假设进行设置；frame_extent 产生式必须拒绝无效情况。
 */
frame_bound:
			UNBOUNDED PRECEDING
				{
					WindowDef  *n = makeNode(WindowDef);

					n->frameOptions = FRAMEOPTION_START_UNBOUNDED_PRECEDING;
					n->startOffset = NULL;
					n->endOffset = NULL;
					$$ = n;
				}
			| UNBOUNDED FOLLOWING
				{
					WindowDef  *n = makeNode(WindowDef);

					n->frameOptions = FRAMEOPTION_START_UNBOUNDED_FOLLOWING;
					n->startOffset = NULL;
					n->endOffset = NULL;
					$$ = n;
				}
			| CURRENT_P ROW
				{
					WindowDef  *n = makeNode(WindowDef);

					n->frameOptions = FRAMEOPTION_START_CURRENT_ROW;
					n->startOffset = NULL;
					n->endOffset = NULL;
					$$ = n;
				}
			| a_expr PRECEDING
				{
					WindowDef  *n = makeNode(WindowDef);

					n->frameOptions = FRAMEOPTION_START_OFFSET_PRECEDING;
					n->startOffset = $1;
					n->endOffset = NULL;
					$$ = n;
				}
			| a_expr FOLLOWING
				{
					WindowDef  *n = makeNode(WindowDef);

					n->frameOptions = FRAMEOPTION_START_OFFSET_FOLLOWING;
					n->startOffset = $1;
					n->endOffset = NULL;
					$$ = n;
				}
		;

opt_window_exclusion_clause:
			EXCLUDE CURRENT_P ROW	{ $$ = FRAMEOPTION_EXCLUDE_CURRENT_ROW; }
			| EXCLUDE GROUP_P		{ $$ = FRAMEOPTION_EXCLUDE_GROUP; }
			| EXCLUDE TIES			{ $$ = FRAMEOPTION_EXCLUDE_TIES; }
			| EXCLUDE NO OTHERS		{ $$ = 0; }
			| /* EMPTY - 空 */				{ $$ = 0; }
		;


/*
 * Supporting nonterminals for expressions.
 * 表达式的支持非终结符。
 */

/* Explicit row production.
 *
 * SQL99 allows an optional ROW keyword, so we can now do single-element rows
 * without conflicting with the parenthesized a_expr production.  Without the
 * ROW keyword, there must be more than one a_expr inside the parens.
 * 显式行（row）产生式。SQL99 允许可选的 ROW 关键字，因此我们现在可以进行单元素行，而不会与带括号的 a_expr 产生式冲突。没有 ROW 关键字，括号内必须有多个 a_expr。
 */
row:		ROW '(' expr_list ')'					{ $$ = $3; }
			| ROW '(' ')'							{ $$ = NIL; }
			| '(' expr_list ',' a_expr ')'			{ $$ = lappend($2, $4); }
		;

explicit_row:	ROW '(' expr_list ')'				{ $$ = $3; }
			| ROW '(' ')'							{ $$ = NIL; }
		;

implicit_row:	'(' expr_list ',' a_expr ')'		{ $$ = lappend($2, $4); }
		;

sub_type:	ANY										{ $$ = ANY_SUBLINK; }
			| SOME									{ $$ = ANY_SUBLINK; }
			| ALL									{ $$ = ALL_SUBLINK; }
		;

all_Op:		Op										{ $$ = $1; }
			| MathOp								{ $$ = $1; }
		;

MathOp:		 '+'									{ $$ = "+"; }
			| '-'									{ $$ = "-"; }
			| '*'									{ $$ = "*"; }
			| '/'									{ $$ = "/"; }
			| '%'									{ $$ = "%"; }
			| '^'									{ $$ = "^"; }
			| '<'									{ $$ = "<"; }
			| '>'									{ $$ = ">"; }
			| '='									{ $$ = "="; }
			| LESS_EQUALS							{ $$ = "<="; }
			| GREATER_EQUALS						{ $$ = ">="; }
			| NOT_EQUALS							{ $$ = "<>"; }
		;

qual_Op:	Op
					{ $$ = list_make1(makeString($1)); }
			| OPERATOR '(' any_operator ')'
					{ $$ = $3; }
		;

qual_all_Op:
			all_Op
					{ $$ = list_make1(makeString($1)); }
			| OPERATOR '(' any_operator ')'
					{ $$ = $3; }
		;

subquery_Op:
			all_Op
					{ $$ = list_make1(makeString($1)); }
			| OPERATOR '(' any_operator ')'
					{ $$ = $3; }
			| LIKE
					{ $$ = list_make1(makeString("~~")); }
			| NOT_LA LIKE
					{ $$ = list_make1(makeString("!~~")); }
			| ILIKE
					{ $$ = list_make1(makeString("~~*")); }
			| NOT_LA ILIKE
					{ $$ = list_make1(makeString("!~~*")); }
/* cannot put SIMILAR TO here, because SIMILAR TO is a hack.
 * the regular expression is preprocessed by a function (similar_to_escape),
 * and the ~ operator for posix regular expressions is used.
 *        x SIMILAR TO y     ->    x ~ similar_to_escape(y)
 * this transformation is made on the fly by the parser upwards.
 * however the SubLink structure which handles any/some/all stuff
 * is not ready for such a thing.
 * 不能在这里放 SIMILAR TO，因为 SIMILAR TO 是个黑客手段。正则表达式由函数（similar_to_escape）进行预处理，并使用 posix 正则表达式的 ~ 运算符。x SIMILAR TO y -> x ~ similar_to_escape(y) 这种转换是由解析器向上飞速完成的。然而，处理 any/some/all 内容的 SubLink 结构还没有准备好应对这种事情。
 */
			;

expr_list:	a_expr
				{
					$$ = list_make1($1);
				}
			| expr_list ',' a_expr
				{
					$$ = lappend($1, $3);
				}
		;

/* function arguments can have names - 函数参数可以有名称 */
func_arg_list:  func_arg_expr
				{
					$$ = list_make1($1);
				}
			| func_arg_list ',' func_arg_expr
				{
					$$ = lappend($1, $3);
				}
		;

func_arg_expr:  a_expr
				{
					$$ = $1;
				}
			| param_name COLON_EQUALS a_expr
				{
					NamedArgExpr *na = makeNode(NamedArgExpr);

					na->name = $1;
					na->arg = (Expr *) $3;
					na->argnumber = -1;		/* until determined - 直到确定 */
					na->location = @1;
					$$ = (Node *) na;
				}
			| param_name EQUALS_GREATER a_expr
				{
					NamedArgExpr *na = makeNode(NamedArgExpr);

					na->name = $1;
					na->arg = (Expr *) $3;
					na->argnumber = -1;		/* until determined - 直到确定 */
					na->location = @1;
					$$ = (Node *) na;
				}
		;

func_arg_list_opt:	func_arg_list					{ $$ = $1; }
			| /* EMPTY - 空 */								{ $$ = NIL; }
		;

type_list:	Typename								{ $$ = list_make1($1); }
			| type_list ',' Typename				{ $$ = lappend($1, $3); }
		;

array_expr: '[' expr_list ']'
				{
					$$ = makeAArrayExpr($2, @1, @3);
				}
			| '[' array_expr_list ']'
				{
					$$ = makeAArrayExpr($2, @1, @3);
				}
			| '[' ']'
				{
					$$ = makeAArrayExpr(NIL, @1, @2);
				}
		;

array_expr_list: array_expr							{ $$ = list_make1($1); }
			| array_expr_list ',' array_expr		{ $$ = lappend($1, $3); }
		;


extract_list:
			extract_arg FROM a_expr
				{
					$$ = list_make2(makeStringConst($1, @1), $3);
				}
		;

/* Allow delimited string Sconst in extract_arg as an SQL extension.
 * - thomas 2001-04-12
 * 在 extract_arg 中允许定界字符串 Sconst 作为 SQL 扩展。- thomas 2001-04-12
 */
extract_arg:
			IDENT									{ $$ = $1; }
			| YEAR_P								{ $$ = "year"; }
			| MONTH_P								{ $$ = "month"; }
			| DAY_P									{ $$ = "day"; }
			| HOUR_P								{ $$ = "hour"; }
			| MINUTE_P								{ $$ = "minute"; }
			| SECOND_P								{ $$ = "second"; }
			| Sconst								{ $$ = $1; }
		;

unicode_normal_form:
			NFC										{ $$ = "NFC"; }
			| NFD									{ $$ = "NFD"; }
			| NFKC									{ $$ = "NFKC"; }
			| NFKD									{ $$ = "NFKD"; }
		;

/* OVERLAY() arguments - OVERLAY() 参数 */
overlay_list:
			a_expr PLACING a_expr FROM a_expr FOR a_expr
				{
					/* overlay(A PLACING B FROM C FOR D) is converted to overlay(A, B, C, D) - overlay(A PLACING B FROM C FOR D) 转换为 overlay(A, B, C, D) */
					$$ = list_make4($1, $3, $5, $7);
				}
			| a_expr PLACING a_expr FROM a_expr
				{
					/* overlay(A PLACING B FROM C) is converted to overlay(A, B, C) - overlay(A PLACING B FROM C) 转换为 overlay(A, B, C) */
					$$ = list_make3($1, $3, $5);
				}
		;

/* position_list uses b_expr not a_expr to avoid conflict with general IN - position_list 使用 b_expr 而不是 a_expr，以避免与通用的 IN 冲突 */
position_list:
			b_expr IN_P b_expr						{ $$ = list_make2($3, $1); }
		;

/*
 * SUBSTRING() arguments
 *
 * Note that SQL:1999 has both
 *     text FROM int FOR int
 * and
 *     text FROM pattern FOR escape
 *
 * In the parser we map them both to a call to the substring() function and
 * rely on type resolution to pick the right one.
 *
 * In SQL:2003, the second variant was changed to
 *     text SIMILAR pattern ESCAPE escape
 * We could in theory map that to a different function internally, but
 * since we still support the SQL:1999 version, we don't.  However,
 * ruleutils.c will reverse-list the call in the newer style.
 * SUBSTRING() 参数。注意，SQL:1999 既有 text FROM int FOR int，也有 text FROM pattern FOR escape。在解析器中，我们都将它们映射为对 substring() 函数的调用，并依赖类型解析来选择正确的函数。在 SQL:2003 中，第二种变体更改为 text SIMILAR pattern ESCAPE escape。理论上我们可以在内部将其映射到不同的函数，但由于我们仍然支持 SQL:1999 版本，所以我们没有这么做。但是，ruleutils.c 会以较新的样式逆向列出该调用。
 */
substr_list:
			a_expr FROM a_expr FOR a_expr
				{
					$$ = list_make3($1, $3, $5);
				}
			| a_expr FOR a_expr FROM a_expr
				{
					/* not legal per SQL, but might as well allow it - 根据 SQL 这是不合法的，但不妨允许它 */
					$$ = list_make3($1, $5, $3);
				}
			| a_expr FROM a_expr
				{
					/*
					 * Because we aren't restricting data types here, this
					 * syntax can end up resolving to textregexsubstr().
					 * We've historically allowed that to happen, so continue
					 * to accept it.  However, ruleutils.c will reverse-list
					 * such a call in regular function call syntax.
					 * 因为我们在这里不限制数据类型，这种语法最终可能会解析为 textregexsubstr()。我们历史上允许发生这种情况，因此继续接受它。然而，ruleutils.c 会在常规函数调用语法中逆向列出此类调用。
					 */
					$$ = list_make2($1, $3);
				}
			| a_expr FOR a_expr
				{
					/* not legal per SQL - 根据 SQL 这是不合法的 */

					/*
					 * Since there are no cases where this syntax allows
					 * a textual FOR value, we forcibly cast the argument
					 * to int4.  The possible matches in pg_proc are
					 * substring(text,int4) and substring(text,text),
					 * and we don't want the parser to choose the latter,
					 * which it is likely to do if the second argument
					 * is unknown or doesn't have an implicit cast to int4.
					 * 由于没有这种语法允许文本形式的 FOR 值的情况，我们强制将参数转换为 int4。pg_proc 中的可能匹配项是 substring(text,int4) 和 substring(text,text)，我们不希望解析器选择后者，如果第二个参数是未知类型或没有隐式转换为 int4，解析器很可能会这样做。
					 */
					$$ = list_make3($1, makeIntConst(1, -1),
									makeTypeCast($3,
												 SystemTypeName("int4"), -1));
				}
			| a_expr SIMILAR a_expr ESCAPE a_expr
				{
					$$ = list_make3($1, $3, $5);
				}
		;

trim_list:	a_expr FROM expr_list					{ $$ = lappend($3, $1); }
			| FROM expr_list						{ $$ = $2; }
			| expr_list								{ $$ = $1; }
		;

/*
 * Define SQL-style CASE clause.
 * - Full specification
 *	CASE WHEN a = b THEN c ... ELSE d END
 * - Implicit argument
 *	CASE a WHEN b THEN c ... ELSE d END
 * 定义 SQL 风格的 CASE 子句。- 完整规格 CASE WHEN a = b THEN c ... ELSE d END - 隐式参数 CASE a WHEN b THEN c ... ELSE d END
 */
case_expr:	CASE case_arg when_clause_list case_default END_P
				{
					CaseExpr   *c = makeNode(CaseExpr);

					c->casetype = InvalidOid; /* not analyzed yet - 尚未分析 */
					c->arg = (Expr *) $2;
					c->args = $3;
					c->defresult = (Expr *) $4;
					c->location = @1;
					$$ = (Node *) c;
				}
		;

when_clause_list:
			/* There must be at least one - 必须至少有一个 */
			when_clause								{ $$ = list_make1($1); }
			| when_clause_list when_clause			{ $$ = lappend($1, $2); }
		;

when_clause:
			WHEN a_expr THEN a_expr
				{
					CaseWhen   *w = makeNode(CaseWhen);

					w->expr = (Expr *) $2;
					w->result = (Expr *) $4;
					w->location = @1;
					$$ = (Node *) w;
				}
		;

case_default:
			ELSE a_expr								{ $$ = $2; }
			| /* EMPTY - 空 */								{ $$ = NULL; }
		;

case_arg:	a_expr									{ $$ = $1; }
			| /* EMPTY - 空 */								{ $$ = NULL; }
		;

columnref:	ColId
				{
					$$ = makeColumnRef($1, NIL, @1, yyscanner);
				}
			| ColId indirection
				{
					$$ = makeColumnRef($1, $2, @1, yyscanner);
				}
		;

indirection_el:
			'.' attr_name
				{
					$$ = (Node *) makeString($2);
				}
			| '.' '*'
				{
					$$ = (Node *) makeNode(A_Star);
				}
			| '[' a_expr ']'
				{
					A_Indices *ai = makeNode(A_Indices);

					ai->is_slice = false;
					ai->lidx = NULL;
					ai->uidx = $2;
					$$ = (Node *) ai;
				}
			| '[' opt_slice_bound ':' opt_slice_bound ']'
				{
					A_Indices *ai = makeNode(A_Indices);

					ai->is_slice = true;
					ai->lidx = $2;
					ai->uidx = $4;
					$$ = (Node *) ai;
				}
		;

opt_slice_bound:
			a_expr									{ $$ = $1; }
			| /* EMPTY - 空 */								{ $$ = NULL; }
		;

indirection:
			indirection_el							{ $$ = list_make1($1); }
			| indirection indirection_el			{ $$ = lappend($1, $2); }
		;

opt_indirection:
			/* EMPTY - 空 */								{ $$ = NIL; }
			| opt_indirection indirection_el		{ $$ = lappend($1, $2); }
		;

opt_asymmetric: ASYMMETRIC
			| /* EMPTY - 空 */
		;

/* SQL/JSON support */
json_passing_clause_opt:
			PASSING json_arguments					{ $$ = $2; }
			| /* EMPTY - 空 */								{ $$ = NIL; }
		;

json_arguments:
			json_argument							{ $$ = list_make1($1); }
			| json_arguments ',' json_argument		{ $$ = lappend($1, $3); }
		;

json_argument:
			json_value_expr AS ColLabel
			{
				JsonArgument *n = makeNode(JsonArgument);

				n->val = (JsonValueExpr *) $1;
				n->name = $3;
				$$ = (Node *) n;
			}
		;

/* ARRAY is a noise word - ARRAY 是个噪词 */
json_wrapper_behavior:
			  WITHOUT WRAPPER					{ $$ = JSW_NONE; }
			| WITHOUT ARRAY	WRAPPER				{ $$ = JSW_NONE; }
			| WITH WRAPPER						{ $$ = JSW_UNCONDITIONAL; }
			| WITH ARRAY WRAPPER				{ $$ = JSW_UNCONDITIONAL; }
			| WITH CONDITIONAL ARRAY WRAPPER	{ $$ = JSW_CONDITIONAL; }
			| WITH UNCONDITIONAL ARRAY WRAPPER	{ $$ = JSW_UNCONDITIONAL; }
			| WITH CONDITIONAL WRAPPER			{ $$ = JSW_CONDITIONAL; }
			| WITH UNCONDITIONAL WRAPPER		{ $$ = JSW_UNCONDITIONAL; }
			| /* empty - 空 */						{ $$ = JSW_UNSPEC; }
		;

json_behavior:
			DEFAULT a_expr
				{ $$ = (Node *) makeJsonBehavior(JSON_BEHAVIOR_DEFAULT, $2, @1); }
			| json_behavior_type
				{ $$ = (Node *) makeJsonBehavior($1, NULL, @1); }
		;

json_behavior_type:
			ERROR_P		{ $$ = JSON_BEHAVIOR_ERROR; }
			| NULL_P	{ $$ = JSON_BEHAVIOR_NULL; }
			| TRUE_P	{ $$ = JSON_BEHAVIOR_TRUE; }
			| FALSE_P	{ $$ = JSON_BEHAVIOR_FALSE; }
			| UNKNOWN	{ $$ = JSON_BEHAVIOR_UNKNOWN; }
			| EMPTY_P ARRAY	{ $$ = JSON_BEHAVIOR_EMPTY_ARRAY; }
			| EMPTY_P OBJECT_P	{ $$ = JSON_BEHAVIOR_EMPTY_OBJECT; }
			/* non-standard, for Oracle compatibility only - 非标准，仅为了 Oracle 兼容性 */
			| EMPTY_P	{ $$ = JSON_BEHAVIOR_EMPTY_ARRAY; }
		;

json_behavior_clause_opt:
			json_behavior ON EMPTY_P
				{ $$ = list_make2($1, NULL); }
			| json_behavior ON ERROR_P
				{ $$ = list_make2(NULL, $1); }
			| json_behavior ON EMPTY_P json_behavior ON ERROR_P
				{ $$ = list_make2($1, $4); }
			| /* EMPTY - 空 */
				{ $$ = list_make2(NULL, NULL); }
		;

json_on_error_clause_opt:
			json_behavior ON ERROR_P
				{ $$ = $1; }
			| /* EMPTY - 空 */
				{ $$ = NULL; }
		;

json_value_expr:
			a_expr json_format_clause_opt
			{
				/* formatted_expr will be set during parse-analysis. - formatted_expr 将在解析分析（parse-analysis）期间设置。 */
				$$ = (Node *) makeJsonValueExpr((Expr *) $1, NULL,
												castNode(JsonFormat, $2));
			}
		;

json_format_clause:
			FORMAT_LA JSON ENCODING name
				{
					int		encoding;

					if (!pg_strcasecmp($4, "utf8"))
						encoding = JS_ENC_UTF8;
					else if (!pg_strcasecmp($4, "utf16"))
						encoding = JS_ENC_UTF16;
					else if (!pg_strcasecmp($4, "utf32"))
						encoding = JS_ENC_UTF32;
					else
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								 errmsg("unrecognized JSON encoding: %s", $4),
								 parser_errposition(@4)));

					$$ = (Node *) makeJsonFormat(JS_FORMAT_JSON, encoding, @1);
				}
			| FORMAT_LA JSON
				{
					$$ = (Node *) makeJsonFormat(JS_FORMAT_JSON, JS_ENC_DEFAULT, @1);
				}
		;

json_format_clause_opt:
			json_format_clause
				{
					$$ = $1;
				}
			| /* EMPTY - 空 */
				{
					$$ = (Node *) makeJsonFormat(JS_FORMAT_DEFAULT, JS_ENC_DEFAULT, -1);
				}
		;

json_quotes_clause_opt:
			KEEP QUOTES ON SCALAR STRING_P		{ $$ = JS_QUOTES_KEEP; }
			| KEEP QUOTES						{ $$ = JS_QUOTES_KEEP; }
			| OMIT QUOTES ON SCALAR STRING_P	{ $$ = JS_QUOTES_OMIT; }
			| OMIT QUOTES						{ $$ = JS_QUOTES_OMIT; }
			| /* EMPTY - 空 */						{ $$ = JS_QUOTES_UNSPEC; }
		;

json_returning_clause_opt:
			RETURNING Typename json_format_clause_opt
				{
					JsonOutput *n = makeNode(JsonOutput);

					n->typeName = $2;
					n->returning = makeNode(JsonReturning);
					n->returning->format = (JsonFormat *) $3;
					$$ = (Node *) n;
				}
			| /* EMPTY - 空 */							{ $$ = NULL; }
		;

/*
 * We must assign the only-JSON production a precedence less than IDENT in
 * order to favor shifting over reduction when JSON is followed by VALUE_P,
 * OBJECT_P, or SCALAR.  (ARRAY doesn't need that treatment, because it's a
 * fully reserved word.)  Because json_predicate_type_constraint is always
 * followed by json_key_uniqueness_constraint_opt, we also need the only-JSON
 * production to have precedence less than WITH and WITHOUT.  UNBOUNDED isn't
 * really related to this syntax, but it's a convenient choice because it
 * already has a precedence less than IDENT for other reasons.
 * 我们必须赋予 only-JSON 产生式小于 IDENT 的优先级，以便在 JSON 后面跟有 VALUE_P、OBJECT_P 或 SCALAR 时，有利于移进而不是规约。（ARRAY 不需要这种处理，因为它是一个完全保留的关键字。）因为 json_predicate_type_constraint 后面总是跟着 json_key_uniqueness_constraint_opt，我们还需要 only-JSON 产生式的优先级小于 WITH 和 WITHOUT。UNBOUNDED 实际上与此语法无关，但它是一个方便的选择，因为由于其他原因它已经具有了比 IDENT 更低的优先级。
 */
json_predicate_type_constraint:
			JSON					%prec UNBOUNDED	{ $$ = JS_TYPE_ANY; }
			| JSON VALUE_P							{ $$ = JS_TYPE_ANY; }
			| JSON ARRAY							{ $$ = JS_TYPE_ARRAY; }
			| JSON OBJECT_P							{ $$ = JS_TYPE_OBJECT; }
			| JSON SCALAR							{ $$ = JS_TYPE_SCALAR; }
		;

/*
 * KEYS is a noise word here.  To avoid shift/reduce conflicts, assign the
 * KEYS-less productions a precedence less than IDENT (i.e., less than KEYS).
 * This prevents reducing them when the next token is KEYS.
 * KEYS 在这里是一个噪词。为了避免移进/规约冲突，分配无 KEYS 的产生式一个小于 IDENT 的优先级（即，小于 KEYS）。这可以防止在下一个 Token 是 KEYS 时规约它们。
 */
json_key_uniqueness_constraint_opt:
			WITH UNIQUE KEYS							{ $$ = true; }
			| WITH UNIQUE				%prec UNBOUNDED	{ $$ = true; }
			| WITHOUT UNIQUE KEYS						{ $$ = false; }
			| WITHOUT UNIQUE			%prec UNBOUNDED	{ $$ = false; }
			| /* EMPTY - 空 */ 				%prec UNBOUNDED	{ $$ = false; }
		;

json_name_and_value_list:
			json_name_and_value
				{ $$ = list_make1($1); }
			| json_name_and_value_list ',' json_name_and_value
				{ $$ = lappend($1, $3); }
		;

json_name_and_value:
/* Supporting this syntax seems to require major surgery
			KEY c_expr VALUE_P json_value_expr
				{ $$ = makeJsonKeyValue($2, $4); }
			|
 支持这种语法似乎需要大动作
*/
			c_expr VALUE_P json_value_expr
				{ $$ = makeJsonKeyValue($1, $3); }
			|
			a_expr ':' json_value_expr
				{ $$ = makeJsonKeyValue($1, $3); }
		;

/* empty means false for objects, true for arrays - 空意味着对于对象为 false，对于数组为 true */
json_object_constructor_null_clause_opt:
			NULL_P ON NULL_P					{ $$ = false; }
			| ABSENT ON NULL_P					{ $$ = true; }
			| /* EMPTY - 空 */						{ $$ = false; }
		;

json_array_constructor_null_clause_opt:
			NULL_P ON NULL_P						{ $$ = false; }
			| ABSENT ON NULL_P						{ $$ = true; }
			| /* EMPTY - 空 */							{ $$ = true; }
		;

json_value_expr_list:
			json_value_expr								{ $$ = list_make1($1); }
			| json_value_expr_list ',' json_value_expr	{ $$ = lappend($1, $3);}
		;

json_aggregate_func:
			JSON_OBJECTAGG '('
				json_name_and_value
				json_object_constructor_null_clause_opt
				json_key_uniqueness_constraint_opt
				json_returning_clause_opt
			')'
				{
					JsonObjectAgg *n = makeNode(JsonObjectAgg);

					n->arg = (JsonKeyValue *) $3;
					n->absent_on_null = $4;
					n->unique = $5;
					n->constructor = makeNode(JsonAggConstructor);
					n->constructor->output = (JsonOutput *) $6;
					n->constructor->agg_order = NULL;
					n->constructor->location = @1;
					$$ = (Node *) n;
				}
			| JSON_ARRAYAGG '('
				json_value_expr
				json_array_aggregate_order_by_clause_opt
				json_array_constructor_null_clause_opt
				json_returning_clause_opt
			')'
				{
					JsonArrayAgg *n = makeNode(JsonArrayAgg);

					n->arg = (JsonValueExpr *) $3;
					n->absent_on_null = $5;
					n->constructor = makeNode(JsonAggConstructor);
					n->constructor->agg_order = $4;
					n->constructor->output = (JsonOutput *) $6;
					n->constructor->location = @1;
					$$ = (Node *) n;
				}
		;

json_array_aggregate_order_by_clause_opt:
			ORDER BY sortby_list					{ $$ = $3; }
			| /* EMPTY - 空 */							{ $$ = NIL; }
		;

/*****************************************************************************
 *
 *	target list for SELECT
 *
 * SELECT 的目标列表（target list）
 *****************************************************************************/

opt_target_list: target_list						{ $$ = $1; }
			| /* EMPTY - 空 */							{ $$ = NIL; }
		;

target_list:
			target_el								{ $$ = list_make1($1); }
			| target_list ',' target_el				{ $$ = lappend($1, $3); }
		;

target_el:	a_expr AS ColLabel
				{
					$$ = makeNode(ResTarget);
					$$->name = $3;
					$$->indirection = NIL;
					$$->val = (Node *) $1;
					$$->location = @1;
				}
			| a_expr BareColLabel
				{
					$$ = makeNode(ResTarget);
					$$->name = $2;
					$$->indirection = NIL;
					$$->val = (Node *) $1;
					$$->location = @1;
				}
			| a_expr
				{
					$$ = makeNode(ResTarget);
					$$->name = NULL;
					$$->indirection = NIL;
					$$->val = (Node *) $1;
					$$->location = @1;
				}
			| '*'
				{
					ColumnRef  *n = makeNode(ColumnRef);

					n->fields = list_make1(makeNode(A_Star));
					n->location = @1;

					$$ = makeNode(ResTarget);
					$$->name = NULL;
					$$->indirection = NIL;
					$$->val = (Node *) n;
					$$->location = @1;
				}
		;


/*****************************************************************************
 *
 *	Names and constants
 *
 * 名称和常量
 *****************************************************************************/

qualified_name_list:
			qualified_name							{ $$ = list_make1($1); }
			| qualified_name_list ',' qualified_name { $$ = lappend($1, $3); }
		;

/*
 * The production for a qualified relation name has to exactly match the
 * production for a qualified func_name, because in a FROM clause we cannot
 * tell which we are parsing until we see what comes after it ('(' for a
 * func_name, something else for a relation). Therefore we allow 'indirection'
 * which may contain subscripts, and reject that case in the C code.
 * 限定关系名称的产生式必须与限定 func_name 的产生式完全匹配，因为在 FROM 子句中，直到我们看到它后面跟的是什么之前（func_name 后面跟的是 '('，关系后面跟的是其他内容），我们无法分辨我们正在解析哪一个。因此，我们允许可能包含下标的 'indirection'，并在 C 代码中拒绝这种情况。
 */
qualified_name:
			ColId
				{
					$$ = makeRangeVar(NULL, $1, @1);
				}
			| ColId indirection
				{
					$$ = makeRangeVarFromQualifiedName($1, $2, @1, yyscanner);
				}
		;

name_list:	name
					{ $$ = list_make1(makeString($1)); }
			| name_list ',' name
					{ $$ = lappend($1, makeString($3)); }
		;


name:		ColId									{ $$ = $1; };

attr_name:	ColLabel								{ $$ = $1; };

file_name:	Sconst									{ $$ = $1; };

/*
 * The production for a qualified func_name has to exactly match the
 * production for a qualified columnref, because we cannot tell which we
 * are parsing until we see what comes after it ('(' or Sconst for a func_name,
 * anything else for a columnref).  Therefore we allow 'indirection' which
 * may contain subscripts, and reject that case in the C code.  (If we
 * ever implement SQL99-like methods, such syntax may actually become legal!)
 * 限定 func_name 的产生式必须与限定 columnref 的产生式完全匹配，因为直到我们看到它后面跟的是什么之前（func_name 后面是 '(' 或 Sconst，columnref 后面是其他任何内容），我们无法分辨我们正在解析哪一个。因此，我们允许可能包含下标的 'indirection'，并在 C 代码中拒绝这种情况。（如果我们有朝一日实现类似于 SQL99 的方法，这种语法实际上可能会变得合法！）
 */
func_name:	type_function_name
					{ $$ = list_make1(makeString($1)); }
			| ColId indirection
					{
						$$ = check_func_name(lcons(makeString($1), $2),
											 yyscanner);
					}
		;


/*
 * Constants
 * 常量
 */
AexprConst: Iconst
				{
					$$ = makeIntConst($1, @1);
				}
			| FCONST
				{
					$$ = makeFloatConst($1, @1);
				}
			| Sconst
				{
					$$ = makeStringConst($1, @1);
				}
			| BCONST
				{
					$$ = makeBitStringConst($1, @1);
				}
			| XCONST
				{
					/* This is a bit constant per SQL99:
					 * Without Feature F511, "BIT data type",
					 * a <general literal> shall not be a
					 * <bit string literal> or a <hex string literal>.
					 * 根据 SQL99，这是一个位常量：没有特性 F511 "BIT 数据类型"，一个 <general literal> 不得是 <bit string literal> 或 <hex string literal>。
					 */
					$$ = makeBitStringConst($1, @1);
				}
			| func_name Sconst
				{
					/* generic type 'literal' syntax - 通用类型 'literal' 语法 */
					TypeName   *t = makeTypeNameFromNameList($1);

					t->location = @1;
					$$ = makeStringConstCast($2, @2, t);
				}
			| func_name '(' func_arg_list opt_sort_clause ')' Sconst
				{
					/* generic syntax with a type modifier - 带有类型修饰符的通用语法 */
					TypeName   *t = makeTypeNameFromNameList($1);
					ListCell   *lc;

					/*
					 * We must use func_arg_list and opt_sort_clause in the
					 * production to avoid reduce/reduce conflicts, but we
					 * don't actually wish to allow NamedArgExpr in this
					 * context, nor ORDER BY.
					 * 我们必须在产生式中使用 func_arg_list 和 opt_sort_clause 以避免规约/规约冲突，但我们实际上不希望在此上下文中允许 NamedArgExpr，也不允许 ORDER BY。
					 */
					foreach(lc, $3)
					{
						NamedArgExpr *arg = (NamedArgExpr *) lfirst(lc);

						if (IsA(arg, NamedArgExpr))
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("type modifier cannot have parameter name"),
									 parser_errposition(arg->location)));
					}
					if ($4 != NIL)
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("type modifier cannot have ORDER BY"),
									 parser_errposition(@4)));

					t->typmods = $3;
					t->location = @1;
					$$ = makeStringConstCast($6, @6, t);
				}
			| ConstTypename Sconst
				{
					$$ = makeStringConstCast($2, @2, $1);
				}
			| ConstInterval Sconst opt_interval
				{
					TypeName   *t = $1;

					t->typmods = $3;
					$$ = makeStringConstCast($2, @2, t);
				}
			| ConstInterval '(' Iconst ')' Sconst
				{
					TypeName   *t = $1;

					t->typmods = list_make2(makeIntConst(INTERVAL_FULL_RANGE, -1),
											makeIntConst($3, @3));
					$$ = makeStringConstCast($5, @5, t);
				}
			| TRUE_P
				{
					$$ = makeBoolAConst(true, @1);
				}
			| FALSE_P
				{
					$$ = makeBoolAConst(false, @1);
				}
			| NULL_P
				{
					$$ = makeNullAConst(@1);
				}
		;

Iconst:		ICONST									{ $$ = $1; };
Sconst:		SCONST									{ $$ = $1; };

SignedIconst: Iconst								{ $$ = $1; }
			| '+' Iconst							{ $$ = + $2; }
			| '-' Iconst							{ $$ = - $2; }
		;

/* Role specifications - 角色规范 */
RoleId:		RoleSpec
				{
					RoleSpec   *spc = (RoleSpec *) $1;

					switch (spc->roletype)
					{
						case ROLESPEC_CSTRING:
							$$ = spc->rolename;
							break;
						case ROLESPEC_PUBLIC:
							ereport(ERROR,
									(errcode(ERRCODE_RESERVED_NAME),
									 errmsg("role name \"%s\" is reserved",
											"public"),
									 parser_errposition(@1)));
							break;
						case ROLESPEC_SESSION_USER:
							ereport(ERROR,
									(errcode(ERRCODE_RESERVED_NAME),
									 errmsg("%s cannot be used as a role name here",
											"SESSION_USER"),
									 parser_errposition(@1)));
							break;
						case ROLESPEC_CURRENT_USER:
							ereport(ERROR,
									(errcode(ERRCODE_RESERVED_NAME),
									 errmsg("%s cannot be used as a role name here",
											"CURRENT_USER"),
									 parser_errposition(@1)));
							break;
						case ROLESPEC_CURRENT_ROLE:
							ereport(ERROR,
									(errcode(ERRCODE_RESERVED_NAME),
									 errmsg("%s cannot be used as a role name here",
											"CURRENT_ROLE"),
									 parser_errposition(@1)));
							break;
					}
				}
			;

RoleSpec:	NonReservedWord
				{
					/*
					 * "public" and "none" are not keywords, but they must
					 * be treated specially here.
					 * "public" 和 "none" 不是关键字，但它们在此处必须被特殊处理。
					 */
					RoleSpec   *n;

					if (strcmp($1, "public") == 0)
					{
						n = (RoleSpec *) makeRoleSpec(ROLESPEC_PUBLIC, @1);
						n->roletype = ROLESPEC_PUBLIC;
					}
					else if (strcmp($1, "none") == 0)
					{
						ereport(ERROR,
								(errcode(ERRCODE_RESERVED_NAME),
								 errmsg("role name \"%s\" is reserved",
										"none"),
								 parser_errposition(@1)));
					}
					else
					{
						n = makeRoleSpec(ROLESPEC_CSTRING, @1);
						n->rolename = pstrdup($1);
					}
					$$ = n;
				}
			| CURRENT_ROLE
				{
					$$ = makeRoleSpec(ROLESPEC_CURRENT_ROLE, @1);
				}
			| CURRENT_USER
				{
					$$ = makeRoleSpec(ROLESPEC_CURRENT_USER, @1);
				}
			| SESSION_USER
				{
					$$ = makeRoleSpec(ROLESPEC_SESSION_USER, @1);
				}
		;

role_list:	RoleSpec
				{ $$ = list_make1($1); }
			| role_list ',' RoleSpec
				{ $$ = lappend($1, $3); }
		;


/*****************************************************************************
 *
 * PL/pgSQL extensions
 *
 * You'd think a PL/pgSQL "expression" should be just an a_expr, but
 * historically it can include just about anything that can follow SELECT.
 * Therefore the returned struct is a SelectStmt.
 * PL/pgSQL 扩展。您可能会认为 PL/pgSQL 的 "表达式" 应该只是一个 a_expr，但历史上它可以包含几乎任何可以跟在 SELECT 之后的内容。因此返回的结构体是一个 SelectStmt。
 *****************************************************************************/

PLpgSQL_Expr: opt_distinct_clause opt_target_list
			from_clause where_clause
			group_clause having_clause window_clause
			opt_sort_clause opt_select_limit opt_for_locking_clause
				{
					SelectStmt *n = makeNode(SelectStmt);

					n->distinctClause = $1;
					n->targetList = $2;
					n->fromClause = $3;
					n->whereClause = $4;
					n->groupClause = ($5)->list;
					n->groupDistinct = ($5)->distinct;
					n->havingClause = $6;
					n->windowClause = $7;
					n->sortClause = $8;
					if ($9)
					{
						n->limitOffset = $9->limitOffset;
						n->limitCount = $9->limitCount;
						if (!n->sortClause &&
							$9->limitOption == LIMIT_OPTION_WITH_TIES)
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("WITH TIES cannot be specified without ORDER BY clause"),
									 parser_errposition($9->optionLoc)));
						n->limitOption = $9->limitOption;
					}
					n->lockingClause = $10;
					$$ = (Node *) n;
				}
		;

/*
 * PL/pgSQL Assignment statement: name opt_indirection := PLpgSQL_Expr
 * PL/pgSQL 赋值语句：name opt_indirection := PLpgSQL_Expr
 */

PLAssignStmt: plassign_target opt_indirection plassign_equals PLpgSQL_Expr
				{
					PLAssignStmt *n = makeNode(PLAssignStmt);

					n->name = $1;
					n->indirection = check_indirection($2, yyscanner);
					/* nnames will be filled by calling production - nnames 将通过调用产生式来填充 */
					n->val = (SelectStmt *) $4;
					n->location = @1;
					$$ = (Node *) n;
				}
		;

plassign_target: ColId							{ $$ = $1; }
			| PARAM								{ $$ = psprintf("$%d", $1); }
		;

plassign_equals: COLON_EQUALS
			| '='
		;


/*
 * Name classification hierarchy.
 *
 * IDENT is the lexeme returned by the lexer for identifiers that match
 * no known keyword.  In most cases, we can accept certain keywords as
 * names, not only IDENTs.	We prefer to accept as many such keywords
 * as possible to minimize the impact of "reserved words" on programmers.
 * So, we divide names into several possible classes.  The classification
 * is chosen in part to make keywords acceptable as names wherever possible.
 * 名称分类层次结构。IDENT 是词法分析器为不匹配任何已知关键字的标识符返回的词素。在大多数情况下，我们可以接受某些关键字作为名称，而不仅仅是 IDENT。We 更倾向于接受尽可能多的此类关键字，以尽量减少 "保留字" 对程序员的影响。因此，我们将名称分为几个可能的类别。选择该分类的部分原因是为了使关键字在可能的情况下可以用作名称。
 */

/* Column identifier --- names that can be column, table, etc names.
/* 列标识符 --- 可以是列、表等的名称。
 */
ColId:		IDENT									{ $$ = $1; }
			| unreserved_keyword					{ $$ = pstrdup($1); }
			| col_name_keyword						{ $$ = pstrdup($1); }
		;

/* Type/function identifier --- names that can be type or function names.
/* 类型/函数标识符 --- 可以是类型或函数名称。
 */
type_function_name:	IDENT							{ $$ = $1; }
			| unreserved_keyword					{ $$ = pstrdup($1); }
			| type_func_name_keyword				{ $$ = pstrdup($1); }
		;

/* Any not-fully-reserved word --- these names can be, eg, role names.
/* 任何未完全保留的词 --- 这些名称可以是，例如，角色名称。
 */
NonReservedWord:	IDENT							{ $$ = $1; }
			| unreserved_keyword					{ $$ = pstrdup($1); }
			| col_name_keyword						{ $$ = pstrdup($1); }
			| type_func_name_keyword				{ $$ = pstrdup($1); }
		;

/* Column label --- allowed labels in "AS" clauses.
 * This presently includes *all* Postgres keywords.
 * 列标签 --- "AS" 子句中允许的标签。这目前包括 *所有* Postgres 关键字。
 */
ColLabel:	IDENT									{ $$ = $1; }
			| unreserved_keyword					{ $$ = pstrdup($1); }
			| col_name_keyword						{ $$ = pstrdup($1); }
			| type_func_name_keyword				{ $$ = pstrdup($1); }
			| reserved_keyword						{ $$ = pstrdup($1); }
		;

/* Bare column label --- names that can be column labels without writing "AS".
 * This classification is orthogonal to the other keyword categories.
 * 裸列标签 --- 无需写 "AS" 即可作为列标签的名称。这种分类与其他关键字类别正交。
 */
BareColLabel:	IDENT								{ $$ = $1; }
			| bare_label_keyword					{ $$ = pstrdup($1); }
		;


/*
 * Keyword category lists.  Generally, every keyword present in
 * the Postgres grammar should appear in exactly one of these lists.
 *
 * Put a new keyword into the first list that it can go into without causing
 * shift or reduce conflicts.  The earlier lists define "less reserved"
 * categories of keywords.
 *
 * Make sure that each keyword's category in kwlist.h matches where
 * it is listed here.  (Someday we may be able to generate these lists and
 * kwlist.h's table from one source of truth.)
 * 关键字类别列表。通常，Postgres 语法中存在的每个关键字都应该准确地出现在这些列表中的一个。在不引起移进或规约冲突的情况下，将新关键字放入它可以进入的第一列表中。较早的列表定义了关键字的 "较少保留" 类别。确保 kwlist.h 中每个关键字的类别与其在此处列出的位置相匹配。（总有一天我们能够从一个单一事实来源生成这些列表和 kwlist.h 表。）
 */

/* "Unreserved" keywords --- available for use as any kind of name.
/* "未保留" 关键字 --- 可用作任何类型的名称。
 */
unreserved_keyword:
			  ABORT_P
			| ABSENT
			| ABSOLUTE_P
			| ACCESS
			| ACTION
			| ADD_P
			| ADMIN
			| AFTER
			| AGGREGATE
			| ALSO
			| ALTER
			| ALWAYS
			| ASENSITIVE
			| ASSERTION
			| ASSIGNMENT
			| AT
			| ATOMIC
			| ATTACH
			| ATTRIBUTE
			| BACKWARD
			| BEFORE
			| BEGIN_P
			| BREADTH
			| BY
			| CACHE
			| CALL
			| CALLED
			| CASCADE
			| CASCADED
			| CATALOG_P
			| CHAIN
			| CHARACTERISTICS
			| CHECKPOINT
			| CLASS
			| CLOSE
			| CLUSTER
			| COLUMNS
			| COMMENT
			| COMMENTS
			| COMMIT
			| COMMITTED
			| COMPRESSION
			| CONDITIONAL
			| CONFIGURATION
			| CONFLICT
			| CONNECTION
			| CONSTRAINTS
			| CONTENT_P
			| CONTINUE_P
			| CONVERSION_P
			| COPY
			| COST
			| CSV
			| CUBE
			| CURRENT_P
			| CURSOR
			| CYCLE
			| DATA_P
			| DATABASE
			| DAY_P
			| DEALLOCATE
			| DECLARE
			| DEFAULTS
			| DEFERRED
			| DEFINER
			| DELETE_P
			| DELIMITER
			| DELIMITERS
			| DEPENDS
			| DEPTH
			| DETACH
			| DICTIONARY
			| DISABLE_P
			| DISCARD
			| DOCUMENT_P
			| DOMAIN_P
			| DOUBLE_P
			| DROP
			| EACH
			| EMPTY_P
			| ENABLE_P
			| ENCODING
			| ENCRYPTED
			| ENFORCED
			| ENUM_P
			| ERROR_P
			| ESCAPE
			| EVENT
			| EXCLUDE
			| EXCLUDING
			| EXCLUSIVE
			| EXECUTE
			| EXPLAIN
			| EXPRESSION
			| EXTENSION
			| EXTERNAL
			| FAMILY
			| FILTER
			| FINALIZE
			| FIRST_P
			| FOLLOWING
			| FORCE
			| FORMAT
			| FORWARD
			| FUNCTION
			| FUNCTIONS
			| GENERATED
			| GLOBAL
			| GRANTED
			| GROUPS
			| HANDLER
			| HEADER_P
			| HOLD
			| HOUR_P
			| IDENTITY_P
			| IF_P
			| IMMEDIATE
			| IMMUTABLE
			| IMPLICIT_P
			| IMPORT_P
			| INCLUDE
			| INCLUDING
			| INCREMENT
			| INDENT
			| INDEX
			| INDEXES
			| INHERIT
			| INHERITS
			| INLINE_P
			| INPUT_P
			| INSENSITIVE
			| INSERT
			| INSTEAD
			| INVOKER
			| ISOLATION
			| KEEP
			| KEY
			| KEYS
			| LABEL
			| LANGUAGE
			| LARGE_P
			| LAST_P
			| LEAKPROOF
			| LEVEL
			| LISTEN
			| LOAD
			| LOCAL
			| LOCATION
			| LOCK_P
			| LOCKED
			| LOGGED
			| MAPPING
			| MATCH
			| MATCHED
			| MATERIALIZED
			| MAXVALUE
			| MERGE
			| METHOD
			| MINUTE_P
			| MINVALUE
			| MODE
			| MONTH_P
			| MOVE
			| NAME_P
			| NAMES
			| NESTED
			| NEW
			| NEXT
			| NFC
			| NFD
			| NFKC
			| NFKD
			| NO
			| NORMALIZED
			| NOTHING
			| NOTIFY
			| NOWAIT
			| NULLS_P
			| OBJECT_P
			| OBJECTS_P
			| OF
			| OFF
			| OIDS
			| OLD
			| OMIT
			| OPERATOR
			| OPTION
			| OPTIONS
			| ORDINALITY
			| OTHERS
			| OVER
			| OVERRIDING
			| OWNED
			| OWNER
			| PARALLEL
			| PARAMETER
			| PARSER
			| PARTIAL
			| PARTITION
			| PASSING
			| PASSWORD
			| PATH
			| PERIOD
			| PLAN
			| PLANS
			| POLICY
			| PRECEDING
			| PREPARE
			| PREPARED
			| PRESERVE
			| PRIOR
			| PRIVILEGES
			| PROCEDURAL
			| PROCEDURE
			| PROCEDURES
			| PROGRAM
			| PUBLICATION
			| QUOTE
			| QUOTES
			| RANGE
			| READ
			| REASSIGN
			| RECURSIVE
			| REF_P
			| REFERENCING
			| REFRESH
			| REINDEX
			| RELATIVE_P
			| RELEASE
			| RENAME
			| REPEATABLE
			| REPLACE
			| REPLICA
			| RESET
			| RESTART
			| RESTRICT
			| RETURN
			| RETURNS
			| REVOKE
			| ROLE
			| ROLLBACK
			| ROLLUP
			| ROUTINE
			| ROUTINES
			| ROWS
			| RULE
			| SAVEPOINT
			| SCALAR
			| SCHEMA
			| SCHEMAS
			| SCROLL
			| SEARCH
			| SECOND_P
			| SECURITY
			| SEQUENCE
			| SEQUENCES
			| SERIALIZABLE
			| SERVER
			| SESSION
			| SET
			| SETS
			| SHARE
			| SHOW
			| SIMPLE
			| SKIP
			| SNAPSHOT
			| SOURCE
			| SQL_P
			| STABLE
			| STANDALONE_P
			| START
			| STATEMENT
			| STATISTICS
			| STDIN
			| STDOUT
			| STORAGE
			| STORED
			| STRICT_P
			| STRING_P
			| STRIP_P
			| SUBSCRIPTION
			| SUPPORT
			| SYSID
			| SYSTEM_P
			| TABLES
			| TABLESPACE
			| TARGET
			| TEMP
			| TEMPLATE
			| TEMPORARY
			| TEXT_P
			| TIES
			| TRANSACTION
			| TRANSFORM
			| TRIGGER
			| TRUNCATE
			| TRUSTED
			| TYPE_P
			| TYPES_P
			| UESCAPE
			| UNBOUNDED
			| UNCOMMITTED
			| UNCONDITIONAL
			| UNENCRYPTED
			| UNKNOWN
			| UNLISTEN
			| UNLOGGED
			| UNTIL
			| UPDATE
			| VACUUM
			| VALID
			| VALIDATE
			| VALIDATOR
			| VALUE_P
			| VARYING
			| VERSION_P
			| VIEW
			| VIEWS
			| VIRTUAL
			| VOLATILE
			| WHITESPACE_P
			| WITHIN
			| WITHOUT
			| WORK
			| WRAPPER
			| WRITE
			| XML_P
			| YEAR_P
			| YES_P
			| ZONE
		;

/* Column identifier --- keywords that can be column, table, etc names.
 *
 * Many of these keywords will in fact be recognized as type or function
 * names too; but they have special productions for the purpose, and so
 * can't be treated as "generic" type or function names.
 *
 * The type names appearing here are not usable as function names
 * because they can be followed by '(' in typename productions, which
 * looks too much like a function call for an LR(1) parser.
 * 列标识符 --- 可以是列、表等名称的关键字。许多此类关键字实际上也会被识别为类型或函数名称；但它们为此目的有特殊的产生式，因此不能被视为 "通用" 的类型或函数名称。此处出现的类型名称不能用作函数名称，因为它们在 typename 产生式中可以后跟 '('，这对于 LR(1) 解析器来说太像函数调用了。
 */
col_name_keyword:
			  BETWEEN
			| BIGINT
			| BIT
			| BOOLEAN_P
			| CHAR_P
			| CHARACTER
			| COALESCE
			| DEC
			| DECIMAL_P
			| EXISTS
			| EXTRACT
			| FLOAT_P
			| GREATEST
			| GROUPING
			| INOUT
			| INT_P
			| INTEGER
			| INTERVAL
			| JSON
			| JSON_ARRAY
			| JSON_ARRAYAGG
			| JSON_EXISTS
			| JSON_OBJECT
			| JSON_OBJECTAGG
			| JSON_QUERY
			| JSON_SCALAR
			| JSON_SERIALIZE
			| JSON_TABLE
			| JSON_VALUE
			| LEAST
			| MERGE_ACTION
			| NATIONAL
			| NCHAR
			| NONE
			| NORMALIZE
			| NULLIF
			| NUMERIC
			| OUT_P
			| OVERLAY
			| POSITION
			| PRECISION
			| REAL
			| ROW
			| SETOF
			| SMALLINT
			| SUBSTRING
			| TIME
			| TIMESTAMP
			| TREAT
			| TRIM
			| VALUES
			| VARCHAR
			| XMLATTRIBUTES
			| XMLCONCAT
			| XMLELEMENT
			| XMLEXISTS
			| XMLFOREST
			| XMLNAMESPACES
			| XMLPARSE
			| XMLPI
			| XMLROOT
			| XMLSERIALIZE
			| XMLTABLE
		;

/* Type/function identifier --- keywords that can be type or function names.
 *
 * Most of these are keywords that are used as operators in expressions;
 * in general such keywords can't be column names because they would be
 * ambiguous with variables, but they are unambiguous as function identifiers.
 *
 * Do not include POSITION, SUBSTRING, etc here since they have explicit
 * productions in a_expr to support the goofy SQL9x argument syntax.
 * - thomas 2000-11-28
 * 类型/函数标识符 --- 可以是类型或函数名称的关键字。其中大多数是表达式中用作运算符的关键字；通常，此类关键字不能作为列名，因为它们与变量冲突而产生歧义，但它们作为函数标识符是没有歧义的。这里不要包括 POSITION、SUBSTRING 等，因为它们在 a_expr 中有显式产生式以支持古怪的 SQL9x 参数语法。- thomas 2000-11-28
 */
type_func_name_keyword:
			  AUTHORIZATION
			| BINARY
			| COLLATION
			| CONCURRENTLY
			| CROSS
			| CURRENT_SCHEMA
			| FREEZE
			| FULL
			| ILIKE
			| INNER_P
			| IS
			| ISNULL
			| JOIN
			| LEFT
			| LIKE
			| NATURAL
			| NOTNULL
			| OUTER_P
			| OVERLAPS
			| RIGHT
			| SIMILAR
			| TABLESAMPLE
			| VERBOSE
		;

/* Reserved keyword --- these keywords are usable only as a ColLabel.
 *
 * Keywords appear here if they could not be distinguished from variable,
 * type, or function names in some contexts.  Don't put things here unless
 * forced to.
 * 保留关键字 --- 这些关键字仅可用作 ColLabel。如果某些关键字在某些上下文中无法与变量、类型或函数名称区分开来，则它们将出现在此处。除非被迫，否则不要把东西放在这里。
 */
reserved_keyword:
			  ALL
			| ANALYSE
			| ANALYZE
			| AND
			| ANY
			| ARRAY
			| AS
			| ASC
			| ASYMMETRIC
			| BOTH
			| CASE
			| CAST
			| CHECK
			| COLLATE
			| COLUMN
			| CONSTRAINT
			| CREATE
			| CURRENT_CATALOG
			| CURRENT_DATE
			| CURRENT_ROLE
			| CURRENT_TIME
			| CURRENT_TIMESTAMP
			| CURRENT_USER
			| DEFAULT
			| DEFERRABLE
			| DESC
			| DISTINCT
			| DO
			| ELSE
			| END_P
			| EXCEPT
			| FALSE_P
			| FETCH
			| FOR
			| FOREIGN
			| FROM
			| GRANT
			| GROUP_P
			| HAVING
			| IN_P
			| INITIALLY
			| INTERSECT
			| INTO
			| LATERAL_P
			| LEADING
			| LIMIT
			| LOCALTIME
			| LOCALTIMESTAMP
			| NOT
			| NULL_P
			| OFFSET
			| ON
			| ONLY
			| OR
			| ORDER
			| PLACING
			| PRIMARY
			| REFERENCES
			| RETURNING
			| SELECT
			| SESSION_USER
			| SOME
			| SYMMETRIC
			| SYSTEM_USER
			| TABLE
			| THEN
			| TO
			| TRAILING
			| TRUE_P
			| UNION
			| UNIQUE
			| USER
			| USING
			| VARIADIC
			| WHEN
			| WHERE
			| WINDOW
			| WITH
		;

/*
 * While all keywords can be used as column labels when preceded by AS,
 * not all of them can be used as a "bare" column label without AS.
 * Those that can be used as a bare label must be listed here,
 * in addition to appearing in one of the category lists above.
 *
 * Always add a new keyword to this list if possible.  Mark it BARE_LABEL
 * in kwlist.h if it is included here, or AS_LABEL if it is not.
 * 虽然所有关键字在前面有 AS 时都可以用作列标签，但并非所有关键字在没有 AS 时都可以用作 "裸" 列标签。那些可以用作裸标签的关键字，除了出现在上面的类别列表之一之外，还必须在此处列出。如果可能的话，总是将新关键字添加到此列表中。如果它包含在这里，在 kwlist.h 中标记为 BARE_LABEL，否则标记为 AS_LABEL。
 */
bare_label_keyword:
			  ABORT_P
			| ABSENT
			| ABSOLUTE_P
			| ACCESS
			| ACTION
			| ADD_P
			| ADMIN
			| AFTER
			| AGGREGATE
			| ALL
			| ALSO
			| ALTER
			| ALWAYS
			| ANALYSE
			| ANALYZE
			| AND
			| ANY
			| ASC
			| ASENSITIVE
			| ASSERTION
			| ASSIGNMENT
			| ASYMMETRIC
			| AT
			| ATOMIC
			| ATTACH
			| ATTRIBUTE
			| AUTHORIZATION
			| BACKWARD
			| BEFORE
			| BEGIN_P
			| BETWEEN
			| BIGINT
			| BINARY
			| BIT
			| BOOLEAN_P
			| BOTH
			| BREADTH
			| BY
			| CACHE
			| CALL
			| CALLED
			| CASCADE
			| CASCADED
			| CASE
			| CAST
			| CATALOG_P
			| CHAIN
			| CHARACTERISTICS
			| CHECK
			| CHECKPOINT
			| CLASS
			| CLOSE
			| CLUSTER
			| COALESCE
			| COLLATE
			| COLLATION
			| COLUMN
			| COLUMNS
			| COMMENT
			| COMMENTS
			| COMMIT
			| COMMITTED
			| COMPRESSION
			| CONCURRENTLY
			| CONDITIONAL
			| CONFIGURATION
			| CONFLICT
			| CONNECTION
			| CONSTRAINT
			| CONSTRAINTS
			| CONTENT_P
			| CONTINUE_P
			| CONVERSION_P
			| COPY
			| COST
			| CROSS
			| CSV
			| CUBE
			| CURRENT_P
			| CURRENT_CATALOG
			| CURRENT_DATE
			| CURRENT_ROLE
			| CURRENT_SCHEMA
			| CURRENT_TIME
			| CURRENT_TIMESTAMP
			| CURRENT_USER
			| CURSOR
			| CYCLE
			| DATA_P
			| DATABASE
			| DEALLOCATE
			| DEC
			| DECIMAL_P
			| DECLARE
			| DEFAULT
			| DEFAULTS
			| DEFERRABLE
			| DEFERRED
			| DEFINER
			| DELETE_P
			| DELIMITER
			| DELIMITERS
			| DEPENDS
			| DEPTH
			| DESC
			| DETACH
			| DICTIONARY
			| DISABLE_P
			| DISCARD
			| DISTINCT
			| DO
			| DOCUMENT_P
			| DOMAIN_P
			| DOUBLE_P
			| DROP
			| EACH
			| ELSE
			| EMPTY_P
			| ENABLE_P
			| ENCODING
			| ENCRYPTED
			| END_P
			| ENFORCED
			| ENUM_P
			| ERROR_P
			| ESCAPE
			| EVENT
			| EXCLUDE
			| EXCLUDING
			| EXCLUSIVE
			| EXECUTE
			| EXISTS
			| EXPLAIN
			| EXPRESSION
			| EXTENSION
			| EXTERNAL
			| EXTRACT
			| FALSE_P
			| FAMILY
			| FINALIZE
			| FIRST_P
			| FLOAT_P
			| FOLLOWING
			| FORCE
			| FOREIGN
			| FORMAT
			| FORWARD
			| FREEZE
			| FULL
			| FUNCTION
			| FUNCTIONS
			| GENERATED
			| GLOBAL
			| GRANTED
			| GREATEST
			| GROUPING
			| GROUPS
			| HANDLER
			| HEADER_P
			| HOLD
			| IDENTITY_P
			| IF_P
			| ILIKE
			| IMMEDIATE
			| IMMUTABLE
			| IMPLICIT_P
			| IMPORT_P
			| IN_P
			| INCLUDE
			| INCLUDING
			| INCREMENT
			| INDENT
			| INDEX
			| INDEXES
			| INHERIT
			| INHERITS
			| INITIALLY
			| INLINE_P
			| INNER_P
			| INOUT
			| INPUT_P
			| INSENSITIVE
			| INSERT
			| INSTEAD
			| INT_P
			| INTEGER
			| INTERVAL
			| INVOKER
			| IS
			| ISOLATION
			| JOIN
			| JSON
			| JSON_ARRAY
			| JSON_ARRAYAGG
			| JSON_EXISTS
			| JSON_OBJECT
			| JSON_OBJECTAGG
			| JSON_QUERY
			| JSON_SCALAR
			| JSON_SERIALIZE
			| JSON_TABLE
			| JSON_VALUE
			| KEEP
			| KEY
			| KEYS
			| LABEL
			| LANGUAGE
			| LARGE_P
			| LAST_P
			| LATERAL_P
			| LEADING
			| LEAKPROOF
			| LEAST
			| LEFT
			| LEVEL
			| LIKE
			| LISTEN
			| LOAD
			| LOCAL
			| LOCALTIME
			| LOCALTIMESTAMP
			| LOCATION
			| LOCK_P
			| LOCKED
			| LOGGED
			| MAPPING
			| MATCH
			| MATCHED
			| MATERIALIZED
			| MAXVALUE
			| MERGE
			| MERGE_ACTION
			| METHOD
			| MINVALUE
			| MODE
			| MOVE
			| NAME_P
			| NAMES
			| NATIONAL
			| NATURAL
			| NCHAR
			| NESTED
			| NEW
			| NEXT
			| NFC
			| NFD
			| NFKC
			| NFKD
			| NO
			| NONE
			| NORMALIZE
			| NORMALIZED
			| NOT
			| NOTHING
			| NOTIFY
			| NOWAIT
			| NULL_P
			| NULLIF
			| NULLS_P
			| NUMERIC
			| OBJECT_P
			| OBJECTS_P
			| OF
			| OFF
			| OIDS
			| OLD
			| OMIT
			| ONLY
			| OPERATOR
			| OPTION
			| OPTIONS
			| OR
			| ORDINALITY
			| OTHERS
			| OUT_P
			| OUTER_P
			| OVERLAY
			| OVERRIDING
			| OWNED
			| OWNER
			| PARALLEL
			| PARAMETER
			| PARSER
			| PARTIAL
			| PARTITION
			| PASSING
			| PASSWORD
			| PATH
			| PERIOD
			| PLACING
			| PLAN
			| PLANS
			| POLICY
			| POSITION
			| PRECEDING
			| PREPARE
			| PREPARED
			| PRESERVE
			| PRIMARY
			| PRIOR
			| PRIVILEGES
			| PROCEDURAL
			| PROCEDURE
			| PROCEDURES
			| PROGRAM
			| PUBLICATION
			| QUOTE
			| QUOTES
			| RANGE
			| READ
			| REAL
			| REASSIGN
			| RECURSIVE
			| REF_P
			| REFERENCES
			| REFERENCING
			| REFRESH
			| REINDEX
			| RELATIVE_P
			| RELEASE
			| RENAME
			| REPEATABLE
			| REPLACE
			| REPLICA
			| RESET
			| RESTART
			| RESTRICT
			| RETURN
			| RETURNS
			| REVOKE
			| RIGHT
			| ROLE
			| ROLLBACK
			| ROLLUP
			| ROUTINE
			| ROUTINES
			| ROW
			| ROWS
			| RULE
			| SAVEPOINT
			| SCALAR
			| SCHEMA
			| SCHEMAS
			| SCROLL
			| SEARCH
			| SECURITY
			| SELECT
			| SEQUENCE
			| SEQUENCES
			| SERIALIZABLE
			| SERVER
			| SESSION
			| SESSION_USER
			| SET
			| SETOF
			| SETS
			| SHARE
			| SHOW
			| SIMILAR
			| SIMPLE
			| SKIP
			| SMALLINT
			| SNAPSHOT
			| SOME
			| SOURCE
			| SQL_P
			| STABLE
			| STANDALONE_P
			| START
			| STATEMENT
			| STATISTICS
			| STDIN
			| STDOUT
			| STORAGE
			| STORED
			| STRICT_P
			| STRING_P
			| STRIP_P
			| SUBSCRIPTION
			| SUBSTRING
			| SUPPORT
			| SYMMETRIC
			| SYSID
			| SYSTEM_P
			| SYSTEM_USER
			| TABLE
			| TABLES
			| TABLESAMPLE
			| TABLESPACE
			| TARGET
			| TEMP
			| TEMPLATE
			| TEMPORARY
			| TEXT_P
			| THEN
			| TIES
			| TIME
			| TIMESTAMP
			| TRAILING
			| TRANSACTION
			| TRANSFORM
			| TREAT
			| TRIGGER
			| TRIM
			| TRUE_P
			| TRUNCATE
			| TRUSTED
			| TYPE_P
			| TYPES_P
			| UESCAPE
			| UNBOUNDED
			| UNCOMMITTED
			| UNCONDITIONAL
			| UNENCRYPTED
			| UNIQUE
			| UNKNOWN
			| UNLISTEN
			| UNLOGGED
			| UNTIL
			| UPDATE
			| USER
			| USING
			| VACUUM
			| VALID
			| VALIDATE
			| VALIDATOR
			| VALUE_P
			| VALUES
			| VARCHAR
			| VARIADIC
			| VERBOSE
			| VERSION_P
			| VIEW
			| VIEWS
			| VIRTUAL
			| VOLATILE
			| WHEN
			| WHITESPACE_P
			| WORK
			| WRAPPER
			| WRITE
			| XML_P
			| XMLATTRIBUTES
			| XMLCONCAT
			| XMLELEMENT
			| XMLEXISTS
			| XMLFOREST
			| XMLNAMESPACES
			| XMLPARSE
			| XMLPI
			| XMLROOT
			| XMLSERIALIZE
			| XMLTABLE
			| YES_P
			| ZONE
		;

%%

/*
 * The signature of this function is required by bison.  However, we
 * ignore the passed yylloc and instead use the last token position
 * available from the scanner.
 */
static void
base_yyerror(YYLTYPE *yylloc, core_yyscan_t yyscanner, const char *msg)
{
	parser_yyerror(msg);
}

static RawStmt *
makeRawStmt(Node *stmt, int stmt_location)
{
	RawStmt    *rs = makeNode(RawStmt);

	rs->stmt = stmt;
	rs->stmt_location = stmt_location;
	rs->stmt_len = 0;			/* might get changed later - 稍后可能会更改 */
	return rs;
}

/* Adjust a RawStmt to reflect that it doesn't run to the end of the string */
static void
updateRawStmtEnd(RawStmt *rs, int end_location)
{
	/*
	 * If we already set the length, don't change it.  This is for situations
	 * like "select foo ;; select bar" where the same statement will be last
	 * in the string for more than one semicolon.
	 */
	if (rs->stmt_len > 0)
		return;

	/* OK, update length of RawStmt */
	rs->stmt_len = end_location - rs->stmt_location;
}

static Node *
makeColumnRef(char *colname, List *indirection,
			  int location, core_yyscan_t yyscanner)
{
	/*
	 * Generate a ColumnRef node, with an A_Indirection node added if there is
	 * any subscripting in the specified indirection list.  However, any field
	 * selection at the start of the indirection list must be transposed into
	 * the "fields" part of the ColumnRef node.
	 */
	ColumnRef  *c = makeNode(ColumnRef);
	int			nfields = 0;
	ListCell   *l;

	c->location = location;
	foreach(l, indirection)
	{
		if (IsA(lfirst(l), A_Indices))
		{
			A_Indirection *i = makeNode(A_Indirection);

			if (nfields == 0)
			{
				/* easy case - all indirection goes to A_Indirection */
				c->fields = list_make1(makeString(colname));
				i->indirection = check_indirection(indirection, yyscanner);
			}
			else
			{
				/* got to split the list in two */
				i->indirection = check_indirection(list_copy_tail(indirection,
																  nfields),
												   yyscanner);
				indirection = list_truncate(indirection, nfields);
				c->fields = lcons(makeString(colname), indirection);
			}
			i->arg = (Node *) c;
			return (Node *) i;
		}
		else if (IsA(lfirst(l), A_Star))
		{
			/* We only allow '*' at the end of a ColumnRef */
			if (lnext(indirection, l) != NULL)
				parser_yyerror("improper use of \"*\"");
		}
		nfields++;
	}
	/* No subscripting, so all indirection gets added to field list */
	c->fields = lcons(makeString(colname), indirection);
	return (Node *) c;
}

static Node *
makeTypeCast(Node *arg, TypeName *typename, int location)
{
	TypeCast   *n = makeNode(TypeCast);

	n->arg = arg;
	n->typeName = typename;
	n->location = location;
	return (Node *) n;
}

static Node *
makeStringConstCast(char *str, int location, TypeName *typename)
{
	Node	   *s = makeStringConst(str, location);

	return makeTypeCast(s, typename, -1);
}

static Node *
makeIntConst(int val, int location)
{
	A_Const    *n = makeNode(A_Const);

	n->val.ival.type = T_Integer;
	n->val.ival.ival = val;
	n->location = location;

	return (Node *) n;
}

static Node *
makeFloatConst(char *str, int location)
{
	A_Const    *n = makeNode(A_Const);

	n->val.fval.type = T_Float;
	n->val.fval.fval = str;
	n->location = location;

	return (Node *) n;
}

static Node *
makeBoolAConst(bool state, int location)
{
	A_Const    *n = makeNode(A_Const);

	n->val.boolval.type = T_Boolean;
	n->val.boolval.boolval = state;
	n->location = location;

	return (Node *) n;
}

static Node *
makeBitStringConst(char *str, int location)
{
	A_Const    *n = makeNode(A_Const);

	n->val.bsval.type = T_BitString;
	n->val.bsval.bsval = str;
	n->location = location;

	return (Node *) n;
}

static Node *
makeNullAConst(int location)
{
	A_Const    *n = makeNode(A_Const);

	n->isnull = true;
	n->location = location;

	return (Node *) n;
}

static Node *
makeAConst(Node *v, int location)
{
	Node	   *n;

	switch (v->type)
	{
		case T_Float:
			n = makeFloatConst(castNode(Float, v)->fval, location);
			break;

		case T_Integer:
			n = makeIntConst(castNode(Integer, v)->ival, location);
			break;

		default:
			/* currently not used */
			Assert(false);
			n = NULL;
	}

	return n;
}

/* makeRoleSpec
 * Create a RoleSpec with the given type
 */
static RoleSpec *
makeRoleSpec(RoleSpecType type, int location)
{
	RoleSpec   *spec = makeNode(RoleSpec);

	spec->roletype = type;
	spec->location = location;

	return spec;
}

/* check_qualified_name --- check the result of qualified_name production
 *
 * It's easiest to let the grammar production for qualified_name allow
 * subscripts and '*', which we then must reject here.
 */
static void
check_qualified_name(List *names, core_yyscan_t yyscanner)
{
	ListCell   *i;

	foreach(i, names)
	{
		if (!IsA(lfirst(i), String))
			parser_yyerror("syntax error");
	}
}

/* check_func_name --- check the result of func_name production
 *
 * It's easiest to let the grammar production for func_name allow subscripts
 * and '*', which we then must reject here.
 */
static List *
check_func_name(List *names, core_yyscan_t yyscanner)
{
	ListCell   *i;

	foreach(i, names)
	{
		if (!IsA(lfirst(i), String))
			parser_yyerror("syntax error");
	}
	return names;
}

/* check_indirection --- check the result of indirection production
 *
 * We only allow '*' at the end of the list, but it's hard to enforce that
 * in the grammar, so do it here.
 */
static List *
check_indirection(List *indirection, core_yyscan_t yyscanner)
{
	ListCell   *l;

	foreach(l, indirection)
	{
		if (IsA(lfirst(l), A_Star))
		{
			if (lnext(indirection, l) != NULL)
				parser_yyerror("improper use of \"*\"");
		}
	}
	return indirection;
}

/* extractArgTypes()
 * Given a list of FunctionParameter nodes, extract a list of just the
 * argument types (TypeNames) for input parameters only.  This is what
 * is needed to look up an existing function, which is what is wanted by
 * the productions that use this call.
 */
static List *
extractArgTypes(List *parameters)
{
	List	   *result = NIL;
	ListCell   *i;

	foreach(i, parameters)
	{
		FunctionParameter *p = (FunctionParameter *) lfirst(i);

		if (p->mode != FUNC_PARAM_OUT && p->mode != FUNC_PARAM_TABLE)
			result = lappend(result, p->argType);
	}
	return result;
}

/* extractAggrArgTypes()
 * As above, but work from the output of the aggr_args production.
 */
static List *
extractAggrArgTypes(List *aggrargs)
{
	Assert(list_length(aggrargs) == 2);
	return extractArgTypes((List *) linitial(aggrargs));
}

/* makeOrderedSetArgs()
 * Build the result of the aggr_args production (which see the comments for).
 * This handles only the case where both given lists are nonempty, so that
 * we have to deal with multiple VARIADIC arguments.
 * 空
 */
static List *
makeOrderedSetArgs(List *directargs, List *orderedargs,
				   core_yyscan_t yyscanner)
{
	FunctionParameter *lastd = (FunctionParameter *) llast(directargs);
	Integer    *ndirectargs;

	/* No restriction unless last direct arg is VARIADIC */
	if (lastd->mode == FUNC_PARAM_VARIADIC)
	{
		FunctionParameter *firsto = (FunctionParameter *) linitial(orderedargs);

		/*
		 * We ignore the names, though the aggr_arg production allows them; it
		 * doesn't allow default values, so those need not be checked.
		 * 默认值
		 */
		if (list_length(orderedargs) != 1 ||
			firsto->mode != FUNC_PARAM_VARIADIC ||
			!equal(lastd->argType, firsto->argType))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("an ordered-set aggregate with a VARIADIC direct argument must have one VARIADIC aggregated argument of the same data type"),
					 parser_errposition(firsto->location)));

		/* OK, drop the duplicate VARIADIC argument from the internal form */
		orderedargs = NIL;
	}

	/* don't merge into the next line, as list_concat changes directargs */
	ndirectargs = makeInteger(list_length(directargs));

	return list_make2(list_concat(directargs, orderedargs),
					  ndirectargs);
}

/* insertSelectOptions()
 * Insert ORDER BY, etc into an already-constructed SelectStmt.
 *
 * This routine is just to avoid duplicating code in SelectStmt productions.
 */
static void
insertSelectOptions(SelectStmt *stmt,
					List *sortClause, List *lockingClause,
					SelectLimit *limitClause,
					WithClause *withClause,
					core_yyscan_t yyscanner)
{
	Assert(IsA(stmt, SelectStmt));

	/*
	 * Tests here are to reject constructs like
	 *	(SELECT foo ORDER BY bar) ORDER BY baz
	 */
	if (sortClause)
	{
		if (stmt->sortClause)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("multiple ORDER BY clauses not allowed"),
					 parser_errposition(exprLocation((Node *) sortClause))));
		stmt->sortClause = sortClause;
	}
	/* We can handle multiple locking clauses, though */
	stmt->lockingClause = list_concat(stmt->lockingClause, lockingClause);
	if (limitClause && limitClause->limitOffset)
	{
		if (stmt->limitOffset)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("multiple OFFSET clauses not allowed"),
					 parser_errposition(limitClause->offsetLoc)));
		stmt->limitOffset = limitClause->limitOffset;
	}
	if (limitClause && limitClause->limitCount)
	{
		if (stmt->limitCount)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("multiple LIMIT clauses not allowed"),
					 parser_errposition(limitClause->countLoc)));
		stmt->limitCount = limitClause->limitCount;
	}
	if (limitClause)
	{
		/* If there was a conflict, we must have detected it above */
		Assert(!stmt->limitOption);
		if (!stmt->sortClause && limitClause->limitOption == LIMIT_OPTION_WITH_TIES)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("WITH TIES cannot be specified without ORDER BY clause"),
					 parser_errposition(limitClause->optionLoc)));
		if (limitClause->limitOption == LIMIT_OPTION_WITH_TIES && stmt->lockingClause)
		{
			ListCell   *lc;

			foreach(lc, stmt->lockingClause)
			{
				LockingClause *lock = lfirst_node(LockingClause, lc);

				if (lock->waitPolicy == LockWaitSkip)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("%s and %s options cannot be used together",
									"SKIP LOCKED", "WITH TIES"),
							 parser_errposition(limitClause->optionLoc)));
			}
		}
		stmt->limitOption = limitClause->limitOption;
	}
	if (withClause)
	{
		if (stmt->withClause)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("multiple WITH clauses not allowed"),
					 parser_errposition(exprLocation((Node *) withClause))));
		stmt->withClause = withClause;
	}
}

static Node *
makeSetOp(SetOperation op, bool all, Node *larg, Node *rarg)
{
	SelectStmt *n = makeNode(SelectStmt);

	n->op = op;
	n->all = all;
	n->larg = (SelectStmt *) larg;
	n->rarg = (SelectStmt *) rarg;
	return (Node *) n;
}

/* SystemFuncName()
 * Build a properly-qualified reference to a built-in function.
 */
List *
SystemFuncName(char *name)
{
	return list_make2(makeString("pg_catalog"), makeString(name));
}

/* SystemTypeName()
 * Build a properly-qualified reference to a built-in type.
 *
 * typmod is defaulted, but may be changed afterwards by caller.
 * Likewise for the location.
 * 默认值
 */
TypeName *
SystemTypeName(char *name)
{
	return makeTypeNameFromNameList(list_make2(makeString("pg_catalog"),
											   makeString(name)));
}

/* doNegate()
 * Handle negation of a numeric constant.
 *
 * Formerly, we did this here because the optimizer couldn't cope with
 * indexquals that looked like "var = -4" --- it wants "var = const"
 * and a unary minus operator applied to a constant didn't qualify.
 * As of Postgres 7.0, that problem doesn't exist anymore because there
 * is a constant-subexpression simplifier in the optimizer.  However,
 * there's still a good reason for doing this here, which is that we can
 * postpone committing to a particular internal representation for simple
 * negative constants.	It's better to leave "-123.456" in string form
 * until we know what the desired type is.
 */
static Node *
doNegate(Node *n, int location)
{
	if (IsA(n, A_Const))
	{
		A_Const    *con = (A_Const *) n;

		/* report the constant's location as that of the '-' sign */
		con->location = location;

		if (IsA(&con->val, Integer))
		{
			con->val.ival.ival = -con->val.ival.ival;
			return n;
		}
		if (IsA(&con->val, Float))
		{
			doNegateFloat(&con->val.fval);
			return n;
		}
	}

	return (Node *) makeSimpleA_Expr(AEXPR_OP, "-", NULL, n, location);
}

static void
doNegateFloat(Float *v)
{
	char	   *oldval = v->fval;

	if (*oldval == '+')
		oldval++;
	if (*oldval == '-')
		v->fval = oldval + 1;	/* just strip the '-' */
	else
		v->fval = psprintf("-%s", oldval);
}

static Node *
makeAndExpr(Node *lexpr, Node *rexpr, int location)
{
	/* Flatten "a AND b AND c ..." to a single BoolExpr on sight */
	if (IsA(lexpr, BoolExpr))
	{
		BoolExpr   *blexpr = (BoolExpr *) lexpr;

		if (blexpr->boolop == AND_EXPR)
		{
			blexpr->args = lappend(blexpr->args, rexpr);
			return (Node *) blexpr;
		}
	}
	return (Node *) makeBoolExpr(AND_EXPR, list_make2(lexpr, rexpr), location);
}

static Node *
makeOrExpr(Node *lexpr, Node *rexpr, int location)
{
	/* Flatten "a OR b OR c ..." to a single BoolExpr on sight */
	if (IsA(lexpr, BoolExpr))
	{
		BoolExpr   *blexpr = (BoolExpr *) lexpr;

		if (blexpr->boolop == OR_EXPR)
		{
			blexpr->args = lappend(blexpr->args, rexpr);
			return (Node *) blexpr;
		}
	}
	return (Node *) makeBoolExpr(OR_EXPR, list_make2(lexpr, rexpr), location);
}

static Node *
makeNotExpr(Node *expr, int location)
{
	return (Node *) makeBoolExpr(NOT_EXPR, list_make1(expr), location);
}

static Node *
makeAArrayExpr(List *elements, int location, int location_end)
{
	A_ArrayExpr *n = makeNode(A_ArrayExpr);

	n->elements = elements;
	n->location = location;
	n->list_start = location;
	n->list_end = location_end;
	return (Node *) n;
}

static Node *
makeSQLValueFunction(SQLValueFunctionOp op, int32 typmod, int location)
{
	SQLValueFunction *svf = makeNode(SQLValueFunction);

	svf->op = op;
	/* svf->type will be filled during parse analysis */
	svf->typmod = typmod;
	svf->location = location;
	return (Node *) svf;
}

static Node *
makeXmlExpr(XmlExprOp op, char *name, List *named_args, List *args,
			int location)
{
	XmlExpr    *x = makeNode(XmlExpr);

	x->op = op;
	x->name = name;

	/*
	 * named_args is a list of ResTarget; it'll be split apart into separate
	 * expression and name lists in transformXmlExpr().
	 */
	x->named_args = named_args;
	x->arg_names = NIL;
	x->args = args;
	/* xmloption, if relevant, must be filled in by caller */
	/* type and typmod will be filled in during parse analysis */
	x->type = InvalidOid;		/* marks the node as not analyzed */
	x->location = location;
	return (Node *) x;
}

/*
 * Merge the input and output parameters of a table function.
 */
static List *
mergeTableFuncParameters(List *func_args, List *columns, core_yyscan_t yyscanner)
{
	ListCell   *lc;

	/* Explicit OUT and INOUT parameters shouldn't be used in this syntax */
	foreach(lc, func_args)
	{
		FunctionParameter *p = (FunctionParameter *) lfirst(lc);

		if (p->mode != FUNC_PARAM_DEFAULT &&
			p->mode != FUNC_PARAM_IN &&
			p->mode != FUNC_PARAM_VARIADIC)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("OUT and INOUT arguments aren't allowed in TABLE functions"),
					 parser_errposition(p->location)));
	}

	return list_concat(func_args, columns);
}

/*
 * Determine return type of a TABLE function.  A single result column
 * returns setof that column's type; otherwise return setof record.
 */
static TypeName *
TableFuncTypeName(List *columns)
{
	TypeName   *result;

	if (list_length(columns) == 1)
	{
		FunctionParameter *p = (FunctionParameter *) linitial(columns);

		result = copyObject(p->argType);
	}
	else
		result = SystemTypeName("record");

	result->setof = true;

	return result;
}

/*
 * Convert a list of (dotted) names to a RangeVar (like
 * makeRangeVarFromNameList, but with position support).  The
 * "AnyName" refers to the any_name production in the grammar.
 */
static RangeVar *
makeRangeVarFromAnyName(List *names, int position, core_yyscan_t yyscanner)
{
	RangeVar   *r = makeNode(RangeVar);

	switch (list_length(names))
	{
		case 1:
			r->catalogname = NULL;
			r->schemaname = NULL;
			r->relname = strVal(linitial(names));
			break;
		case 2:
			r->catalogname = NULL;
			r->schemaname = strVal(linitial(names));
			r->relname = strVal(lsecond(names));
			break;
		case 3:
			r->catalogname = strVal(linitial(names));
			r->schemaname = strVal(lsecond(names));
			r->relname = strVal(lthird(names));
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("improper qualified name (too many dotted names): %s",
							NameListToString(names)),
					 parser_errposition(position)));
			break;
	}

	r->relpersistence = RELPERSISTENCE_PERMANENT;
	r->location = position;

	return r;
}

/*
 * Convert a relation_name with name and namelist to a RangeVar using
 * makeRangeVar.
 */
static RangeVar *
makeRangeVarFromQualifiedName(char *name, List *namelist, int location,
							  core_yyscan_t yyscanner)
{
	RangeVar   *r;

	check_qualified_name(namelist, yyscanner);
	r = makeRangeVar(NULL, NULL, location);

	switch (list_length(namelist))
	{
		case 1:
			r->catalogname = NULL;
			r->schemaname = name;
			r->relname = strVal(linitial(namelist));
			break;
		case 2:
			r->catalogname = name;
			r->schemaname = strVal(linitial(namelist));
			r->relname = strVal(lsecond(namelist));
			break;
		default:
			ereport(ERROR,
					errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("improper qualified name (too many dotted names): %s",
						   NameListToString(lcons(makeString(name), namelist))),
					parser_errposition(location));
			break;
	}

	return r;
}

/* Separate Constraint nodes from COLLATE clauses in a ColQualList */
static void
SplitColQualList(List *qualList,
				 List **constraintList, CollateClause **collClause,
				 core_yyscan_t yyscanner)
{
	ListCell   *cell;

	*collClause = NULL;
	foreach(cell, qualList)
	{
		Node	   *n = (Node *) lfirst(cell);

		if (IsA(n, Constraint))
		{
			/* keep it in list */
			continue;
		}
		if (IsA(n, CollateClause))
		{
			CollateClause *c = (CollateClause *) n;

			if (*collClause)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("multiple COLLATE clauses not allowed"),
						 parser_errposition(c->location)));
			*collClause = c;
		}
		else
			elog(ERROR, "unexpected node type %d", (int) n->type);
		/* remove non-Constraint nodes from qualList */
		qualList = foreach_delete_current(qualList, cell);
	}
	*constraintList = qualList;
}

/*
 * Process result of ConstraintAttributeSpec, and set appropriate bool flags
 * in the output command node.  Pass NULL for any flags the particular
 * command doesn't support.
 */
static void
processCASbits(int cas_bits, int location, const char *constrType,
			   bool *deferrable, bool *initdeferred, bool *is_enforced,
			   bool *not_valid, bool *no_inherit, core_yyscan_t yyscanner)
{
	/* defaults - 默认值 */
	if (deferrable)
		*deferrable = false;
	if (initdeferred)
		*initdeferred = false;
	if (not_valid)
		*not_valid = false;
	if (is_enforced)
		*is_enforced = true;

	if (cas_bits & (CAS_DEFERRABLE | CAS_INITIALLY_DEFERRED))
	{
		if (deferrable)
			*deferrable = true;
		else
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			/* translator: %s is CHECK, UNIQUE, or similar */
					 errmsg("%s constraints cannot be marked DEFERRABLE",
							constrType),
					 parser_errposition(location)));
	}

	if (cas_bits & CAS_INITIALLY_DEFERRED)
	{
		if (initdeferred)
			*initdeferred = true;
		else
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			/* translator: %s is CHECK, UNIQUE, or similar */
					 errmsg("%s constraints cannot be marked DEFERRABLE",
							constrType),
					 parser_errposition(location)));
	}

	if (cas_bits & CAS_NOT_VALID)
	{
		if (not_valid)
			*not_valid = true;
		else
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			/* translator: %s is CHECK, UNIQUE, or similar */
					 errmsg("%s constraints cannot be marked NOT VALID",
							constrType),
					 parser_errposition(location)));
	}

	if (cas_bits & CAS_NO_INHERIT)
	{
		if (no_inherit)
			*no_inherit = true;
		else
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			/* translator: %s is CHECK, UNIQUE, or similar */
					 errmsg("%s constraints cannot be marked NO INHERIT",
							constrType),
					 parser_errposition(location)));
	}

	if (cas_bits & CAS_NOT_ENFORCED)
	{
		if (is_enforced)
			*is_enforced = false;
		else
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 /* translator: %s is CHECK, UNIQUE, or similar */
					 errmsg("%s constraints cannot be marked NOT ENFORCED",
							constrType),
					 parser_errposition(location)));

		/*
		 * NB: The validated status is irrelevant when the constraint is set to
		 * NOT ENFORCED, but for consistency, it should be set accordingly.
		 * This ensures that if the constraint is later changed to ENFORCED, it
		 * will automatically be in the correct NOT VALIDATED state.
		 */
		if (not_valid)
			*not_valid = true;
	}

	if (cas_bits & CAS_ENFORCED)
	{
		if (is_enforced)
			*is_enforced = true;
		else
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 /* translator: %s is CHECK, UNIQUE, or similar */
					 errmsg("%s constraints cannot be marked ENFORCED",
							constrType),
					 parser_errposition(location)));
	}
}

/*
 * Parse a user-supplied partition strategy string into parse node
 * PartitionStrategy representation, or die trying.
 */
static PartitionStrategy
parsePartitionStrategy(char *strategy, int location, core_yyscan_t yyscanner)
{
	if (pg_strcasecmp(strategy, "list") == 0)
		return PARTITION_STRATEGY_LIST;
	else if (pg_strcasecmp(strategy, "range") == 0)
		return PARTITION_STRATEGY_RANGE;
	else if (pg_strcasecmp(strategy, "hash") == 0)
		return PARTITION_STRATEGY_HASH;

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("unrecognized partitioning strategy \"%s\"", strategy),
			 parser_errposition(location)));
	return PARTITION_STRATEGY_LIST; /* keep compiler quiet */

}

/*
 * Process pubobjspec_list to check for errors in any of the objects and
 * convert PUBLICATIONOBJ_CONTINUATION into appropriate PublicationObjSpecType.
 */
static void
preprocess_pubobj_list(List *pubobjspec_list, core_yyscan_t yyscanner)
{
	ListCell   *cell;
	PublicationObjSpec *pubobj;
	PublicationObjSpecType prevobjtype = PUBLICATIONOBJ_CONTINUATION;

	if (!pubobjspec_list)
		return;

	pubobj = (PublicationObjSpec *) linitial(pubobjspec_list);
	if (pubobj->pubobjtype == PUBLICATIONOBJ_CONTINUATION)
		ereport(ERROR,
				errcode(ERRCODE_SYNTAX_ERROR),
				errmsg("invalid publication object list"),
				errdetail("One of TABLE or TABLES IN SCHEMA must be specified before a standalone table or schema name."),
				parser_errposition(pubobj->location));

	foreach(cell, pubobjspec_list)
	{
		pubobj = (PublicationObjSpec *) lfirst(cell);

		if (pubobj->pubobjtype == PUBLICATIONOBJ_CONTINUATION)
			pubobj->pubobjtype = prevobjtype;

		if (pubobj->pubobjtype == PUBLICATIONOBJ_TABLE)
		{
			/* relation name or pubtable must be set for this type of object */
			if (!pubobj->name && !pubobj->pubtable)
				ereport(ERROR,
						errcode(ERRCODE_SYNTAX_ERROR),
						errmsg("invalid table name"),
						parser_errposition(pubobj->location));

			if (pubobj->name)
			{
				/* convert it to PublicationTable */
				PublicationTable *pubtable = makeNode(PublicationTable);

				pubtable->relation =
					makeRangeVar(NULL, pubobj->name, pubobj->location);
				pubobj->pubtable = pubtable;
				pubobj->name = NULL;
			}
		}
		else if (pubobj->pubobjtype == PUBLICATIONOBJ_TABLES_IN_SCHEMA ||
				 pubobj->pubobjtype == PUBLICATIONOBJ_TABLES_IN_CUR_SCHEMA)
		{
			/* WHERE clause is not allowed on a schema object */
			if (pubobj->pubtable && pubobj->pubtable->whereClause)
				ereport(ERROR,
						errcode(ERRCODE_SYNTAX_ERROR),
						errmsg("WHERE clause not allowed for schema"),
						parser_errposition(pubobj->location));

			/* Column list is not allowed on a schema object */
			if (pubobj->pubtable && pubobj->pubtable->columns)
				ereport(ERROR,
						errcode(ERRCODE_SYNTAX_ERROR),
						errmsg("column specification not allowed for schema"),
						parser_errposition(pubobj->location));

			/*
			 * We can distinguish between the different type of schema objects
			 * based on whether name and pubtable is set.
			 */
			if (pubobj->name)
				pubobj->pubobjtype = PUBLICATIONOBJ_TABLES_IN_SCHEMA;
			else if (!pubobj->name && !pubobj->pubtable)
				pubobj->pubobjtype = PUBLICATIONOBJ_TABLES_IN_CUR_SCHEMA;
			else
				ereport(ERROR,
						errcode(ERRCODE_SYNTAX_ERROR),
						errmsg("invalid schema name"),
						parser_errposition(pubobj->location));
		}

		prevobjtype = pubobj->pubobjtype;
	}
}

/*----------
 * Recursive view transformation
 *
 * Convert
 *
 *     CREATE RECURSIVE VIEW relname (aliases) AS query
 *
 * to
 *
 *     CREATE VIEW relname (aliases) AS
 *         WITH RECURSIVE relname (aliases) AS (query)
 *         SELECT aliases FROM relname
 *
 * Actually, just the WITH ... part, which is then inserted into the original
 * view definition as the query.
 * ----------
 */
static Node *
makeRecursiveViewSelect(char *relname, List *aliases, Node *query)
{
	SelectStmt *s = makeNode(SelectStmt);
	WithClause *w = makeNode(WithClause);
	CommonTableExpr *cte = makeNode(CommonTableExpr);
	List	   *tl = NIL;
	ListCell   *lc;

	/* create common table expression */
	cte->ctename = relname;
	cte->aliascolnames = aliases;
	cte->ctematerialized = CTEMaterializeDefault;
	cte->ctequery = query;
	cte->location = -1;

	/* create WITH clause and attach CTE */
	w->recursive = true;
	w->ctes = list_make1(cte);
	w->location = -1;

	/*
	 * create target list for the new SELECT from the alias list of the
	 * recursive view specification
	 */
	foreach(lc, aliases)
	{
		ResTarget  *rt = makeNode(ResTarget);

		rt->name = NULL;
		rt->indirection = NIL;
		rt->val = makeColumnRef(strVal(lfirst(lc)), NIL, -1, 0);
		rt->location = -1;

		tl = lappend(tl, rt);
	}

	/*
	 * create new SELECT combining WITH clause, target list, and fake FROM
	 * clause
	 */
	s->withClause = w;
	s->targetList = tl;
	s->fromClause = list_make1(makeRangeVar(NULL, relname, -1));

	return (Node *) s;
}

/* parser_init()
 * Initialize to parse one query string
 */
void
parser_init(base_yy_extra_type *yyext)
{
	yyext->parsetree = NIL;		/* in case grammar forgets to set it */
}

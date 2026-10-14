#include "autocomplete_qualifier.hpp"
#include "parse_tree_walk.hpp"
#include "duckdb/common/allocator.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/peg/compiled_grammar.hpp"
#include "duckdb/parser/peg/matcher/choice_matcher.hpp"
#include "duckdb/parser/peg/matcher/list_matcher.hpp"
#include "duckdb/parser/peg/matcher/optional_matcher.hpp"
#include "duckdb/parser/peg/matcher/repeat_matcher.hpp"
#include "duckdb/parser/peg/transformer/parse_result.hpp"
#include "duckdb/parser/token_iterator.hpp"
#include "duckdb/storage/arena_allocator.hpp"
#include <functional>
#include <mutex>

namespace duckdb {
namespace smart_autocomplete {

namespace {

//! Inverse of SQL doubled-quote escaping when \p text is wrapped in \p quote; otherwise returns \p text.
static string TryUnescapeQuotes(const string &text, char quote = '"') {
	if (text.size() >= 2 && text.front() == quote && text.back() == quote) {
		return StringUtil::Replace(text.substr(1, text.size() - 2), string(2, quote), string(1, quote));
	}
	return text;
}

//! Match \p tokens (no end-of-input sentinel) against \p root and build its parse tree.
static optional_ptr<ParseResult> MatchTokens(const Matcher &root, vector<MatcherToken> tokens,
                                             ParseResultAllocator &allocator) {
	idx_t end_offset = tokens.empty() ? 0 : tokens.back().offset + tokens.back().length;
	tokens.emplace_back("", end_offset, TokenType::END_OF_INPUT);
	vector<MatcherSuggestion> suggestions;
	idx_t max_token_index = 0;
	TokenIterator token_iterator(tokens);
	ArenaAllocator process_allocator(Allocator::DefaultAllocator());
	MatchContext match_context(suggestions, allocator, process_allocator, max_token_index,
	                           MatchMode::BUILD_PARSE_RESULT, IdentifierCaseMode::PRESERVE_CASE);
	MatchState state(token_iterator, match_context);
	auto result = root.MatchParseResult(state);
	if (!result.IsSuccess()) {
		return nullptr;
	}
	return result.GetParseResult();
}

static bool IsDotToken(const MatcherToken &t) {
	return t.text == ".";
}

static bool IsPartialCompletionToken(const MatcherToken &t) {
	if (t.unterminated) {
		return true;
	}
	switch (t.type) {
	case TokenType::IDENTIFIER:
	case TokenType::CATALOG_NAME:
	case TokenType::SCHEMA_NAME:
	case TokenType::TABLE_NAME:
	case TokenType::COLUMN_NAME:
	case TokenType::TYPE_NAME:
		return true;
	default:
		return false;
	}
}

//! Segments immediately before the cursor position, dot-separated, closest first
//! (e.g. `a . b . c . <cursor>` -> ["c","b","a"] up to \p max_segments).
static vector<string> CollectDottedChainBackwards(const vector<MatcherToken> &tokens, idx_t max_segments) {
	vector<string> segments;
	if (tokens.empty() || max_segments == 0) {
		return segments;
	}
	idx_t pos = tokens.size() - 1;
	if (IsPartialCompletionToken(tokens[pos])) {
		if (pos == 0) {
			return segments;
		}
		pos--;
	}
	while (segments.size() < max_segments) {
		if (!IsDotToken(tokens[pos])) {
			break;
		}
		if (pos == 0) {
			break;
		}
		pos--;
		if (!IsPartialCompletionToken(tokens[pos])) {
			break;
		}
		segments.push_back(TryUnescapeQuotes(tokens[pos].text));
		if (pos == 0) {
			break;
		}
		pos--;
	}
	return segments;
}

// Returns the index of the FROM keyword that encloses the cursor, respecting
// paren nesting. The cursor is at the end of the token stream. We track the
// last FROM seen at each paren depth and return the one matching the cursor's
// enclosing depth.
static idx_t FindEnclosingFromKeyword(const vector<MatcherToken> &tokens) {
	// last_from_at_depth[d] = index of last FROM seen when paren depth == d
	vector<idx_t> last_from_at_depth(1, DConstants::INVALID_INDEX);
	idx_t depth = 0;
	for (idx_t i = 0; i < tokens.size(); i++) {
		const auto &t = tokens[i];
		if (t.type == TokenType::KEYWORD && StringUtil::CIEquals(t.text, "from")) {
			if (depth >= last_from_at_depth.size()) {
				last_from_at_depth.resize(depth + 1, DConstants::INVALID_INDEX);
			}
			last_from_at_depth[depth] = i;
		} else if (t.text == "(") {
			depth++;
			if (depth >= last_from_at_depth.size()) {
				last_from_at_depth.resize(depth + 1, DConstants::INVALID_INDEX);
			}
		} else if (t.text == ")") {
			if (depth > 0) {
				depth--;
			}
		}
	}
	// cursor's enclosing depth = depth after scanning all tokens
	if (depth < last_from_at_depth.size() && last_from_at_depth[depth] != DConstants::INVALID_INDEX) {
		return last_from_at_depth[depth];
	}
	// fallback: last FROM overall
	for (idx_t i = last_from_at_depth.size(); i-- > 0;) {
		if (last_from_at_depth[i] != DConstants::INVALID_INDEX) {
			return last_from_at_depth[i];
		}
	}
	return DConstants::INVALID_INDEX;
}

static bool IsFromTerminatorKeyword(const string &text) {
	static const char *terminators[] = {"where",     "group",  "having",    "qualify", "order",
	                                    "limit",     "offset", "window",    "sample",  "union",
	                                    "intersect", "except", "returning", "select",  nullptr};
	for (idx_t i = 0; terminators[i]; i++) {
		if (StringUtil::CIEquals(text, terminators[i])) {
			return true;
		}
	}
	return false;
}

// Returns the index (exclusive end) of the FROM-clause token slice starting at
// from_idx. Stops at the first top-level (paren-depth 0 relative to from_idx)
// terminator keyword, semicolon, or closing paren that would close the
// enclosing subquery.
static idx_t ComputeFromSliceEnd(const vector<MatcherToken> &tokens, idx_t from_idx) {
	idx_t depth = 0;
	for (idx_t i = from_idx + 1; i < tokens.size(); i++) {
		const auto &t = tokens[i];
		if (t.text == "(") {
			depth++;
		} else if (t.text == ")") {
			if (depth == 0) {
				// closes the enclosing subquery — stop before it
				return i;
			}
			depth--;
		} else if (depth == 0 && t.text == ";") {
			return i;
		} else if (depth == 0 && t.type == TokenType::KEYWORD && IsFromTerminatorKeyword(t.text)) {
			return i;
		}
	}
	return tokens.size();
}

// List(D) <- D (',' D)* ','?  ->  [D, Optional(Repeat(List(',' D))), Optional(',')]
static vector<reference<ParseResult>> ExtractParseResultsFromList(ParseResult &parse_result) {
	vector<reference<ParseResult>> result;
	auto &list_pr = parse_result.Cast<ListParseResult>();
	auto children = list_pr.GetChildren();
	if (children.empty()) {
		return result;
	}
	result.push_back(children[0]);
	if (children.size() < 2) {
		return result;
	}
	auto &opt_child = children[1].get().Cast<OptionalParseResult>();
	if (opt_child.HasResult()) {
		auto &repeat_result = opt_child.GetResult().Cast<RepeatParseResult>();
		for (auto &child : repeat_result.GetChildren()) {
			auto &list_child = child.get().Cast<ListParseResult>();
			if (list_child.GetChildren().size() > 1) {
				result.push_back(list_child.GetChild(1));
			}
		}
	}
	return result;
}

// Helper: find nth SchemaName/CatalogName/TableName identifier in a parse tree using
// a simple counter-based depth-first walk. Returns empty string if not found.
// Collect all IDENTIFIER leaves of a (qualified) name, in order.
static void CollectIdentifiers(ParseResult &pr, vector<string> &out) {
	switch (pr.type) {
	case ParseResultType::IDENTIFIER:
		out.push_back(pr.Cast<IdentifierParseResult>().identifier.GetIdentifierName());
		break;
	case ParseResultType::LIST:
		for (auto &c : pr.Cast<ListParseResult>().GetChildren()) {
			CollectIdentifiers(c.get(), out);
		}
		break;
	case ParseResultType::REPEAT:
		for (auto &c : pr.Cast<RepeatParseResult>().GetChildren()) {
			CollectIdentifiers(c.get(), out);
		}
		break;
	case ParseResultType::CHOICE:
		CollectIdentifiers(pr.Cast<ChoiceParseResult>().GetResult(), out);
		break;
	case ParseResultType::OPTIONAL: {
		auto &op = pr.Cast<OptionalParseResult>();
		if (op.HasResult()) {
			CollectIdentifiers(op.GetResult(), out);
		}
		break;
	}
	default:
		break;
	}
}

static TableQualifiedName ParseQualifiedNameFromBaseTableName(ParseResult &base_table_name_pr) {
	TableQualifiedName qn;
	if (auto catalog_node = FindDescendantByName(base_table_name_pr, "CatalogReservedSchemaTable")) {
		// CatalogReservedSchemaTable <- CatalogQualification ReservedSchemaQualification+ ReservedTableName
		// Identifiers in order: catalog, schema(s), table
		vector<string> ids;
		CollectIdentifiers(*catalog_node, ids);
		if (ids.size() >= 3) {
			qn.catalog = ids.front();
			qn.schema = ids[ids.size() - 2];
			qn.name = ids.back();
		}
		return qn;
	}
	if (auto schema_node = FindDescendantByName(base_table_name_pr, "SchemaReservedTable")) {
		// SchemaReservedTable <- SchemaQualification ReservedTableName
		// Identifiers in order: schema, table
		vector<string> ids;
		CollectIdentifiers(*schema_node, ids);
		if (ids.size() >= 2) {
			qn.schema = ids[0];
			qn.name = ids[1];
		}
		return qn;
	}
	// Unqualified table name — just a single identifier
	qn.name = ExtractFirstIdentifier(base_table_name_pr);
	return qn;
}

static void FillVisibleTableFromBaseTableRef(ParseResult &pr, VisibleTable &out) {
	auto &list_pr = pr.Cast<ListParseResult>();
	// BaseTableRef <- TableAliasColon? BaseTableName TableAlias? AtClause? SampleClause?
	// Child 1 is BaseTableName.
	if (list_pr.GetChildren().size() < 2) {
		return;
	}
	out.name = ParseQualifiedNameFromBaseTableName(list_pr.GetChild(1));
	out.alias.clear();
	if (auto colon = FindDescendantByName(pr, "TableAliasColon")) {
		out.alias = ExtractFirstIdentifier(*colon);
	}
	if (auto talias = FindDescendantByName(pr, "TableAlias")) {
		out.alias = ExtractFirstIdentifier(*talias);
	}
}

// Max recursion depth for star-expansion inside subqueries
static constexpr idx_t kMaxSubqueryRecurseDepth = 3;

// Attempt to derive a stable column name from one AliasedExpression node.
// Returns empty string if we can't derive a name.
static string SynthesizeColumnName(ParseResult &aliased_expr) {
	// ColIdExpression <- ColId ':' Expression  → take the ColId
	if (aliased_expr.Name() == "ColIdExpression") {
		auto &list = aliased_expr.Cast<ListParseResult>();
		return list.GetChildren().empty() ? string() : ExtractFirstIdentifier(list.GetChild(0));
	}
	// ExpressionAsCollabel <- Expression 'AS' ColLabelOrString → take ColLabelOrString
	if (aliased_expr.Name() == "ExpressionAsCollabel") {
		auto &list = aliased_expr.Cast<ListParseResult>();
		// child 2 is ColLabelOrString
		if (list.GetChildren().size() >= 3) {
			try {
				return ExtractFirstIdentifier(list.GetChild(2));
			} catch (...) {
			}
		}
		return string();
	}
	// ExpressionOptIdentifier <- Expression Identifier?
	if (aliased_expr.Name() == "ExpressionOptIdentifier") {
		auto &list = aliased_expr.Cast<ListParseResult>();
		// child 1 is optional Identifier (the alias without AS)
		if (list.GetChildren().size() >= 2) {
			auto &opt = list.Child<OptionalParseResult>(1);
			if (opt.HasResult()) {
				try {
					return ExtractFirstIdentifier(opt.GetResult());
				} catch (...) {
				}
			}
		}
		// Fall back: if the expression itself is a qualified column, take last segment
		if (!list.GetChildren().empty()) {
			auto &expr = list.GetChild(0);
			// Walk looking for an identifier chain with dots — take last identifier
			// We look for the last IdentifierParseResult inside the expression subtree
			// Strategy: traverse looking for dot-separated chain nodes
			// Use a simple recursive leaf-gather and return the last one
			std::function<string(ParseResult &)> last_ident = [&](ParseResult &node) -> string {
				if (node.type == ParseResultType::IDENTIFIER) {
					return node.Cast<IdentifierParseResult>().identifier.GetIdentifierName();
				}
				string last;
				switch (node.type) {
				case ParseResultType::LIST:
					for (auto &c : node.Cast<ListParseResult>().GetChildren()) {
						auto s = last_ident(c.get());
						if (!s.empty()) {
							last = s;
						}
					}
					break;
				case ParseResultType::CHOICE:
					last = last_ident(node.Cast<ChoiceParseResult>().GetResult());
					break;
				case ParseResultType::OPTIONAL: {
					auto &op = node.Cast<OptionalParseResult>();
					if (op.HasResult()) {
						last = last_ident(op.GetResult());
					}
					break;
				}
				default:
					break;
				}
				return last;
			};
			return last_ident(expr);
		}
	}
	return string();
}

static void CollectVisibleRefsRecursive(ParseResult &pr, vector<VisibleTable> &out, idx_t recurse_depth = 0);

// Given a SubqueryReference node, synthesize the projected column names.
// Uses the primary SELECT's TargetList. Returns the list of column names.
static vector<string> SynthesizeColumnsFromSubquery(ParseResult &subquery_ref, idx_t recurse_depth) {
	vector<string> cols;
	if (recurse_depth >= kMaxSubqueryRecurseDepth) {
		return cols;
	}
	// SubqueryReference <- Parens(SelectStatementInternal)
	// Walk: SubqueryReference -> find SelectClause -> find TargetList
	auto target_list_ptr = FindDescendantByName(subquery_ref, "TargetList");
	if (!target_list_ptr) {
		return cols;
	}
	// TargetList? in SelectClause wraps TargetList in an OptionalParseResult.
	// Unwrap the Optional to get the actual TargetList ListParseResult.
	ParseResult *tl_node = target_list_ptr.get();
	if (tl_node->type == ParseResultType::OPTIONAL) {
		auto &opt = tl_node->Cast<OptionalParseResult>();
		if (!opt.HasResult()) {
			return cols;
		}
		tl_node = &opt.GetResult();
	}
	bool has_star = false;
	// TargetList <- List(AliasedExpression).
	// The named rule wrapper has one child: the List body ListParseResult.
	auto &tl_wrapper = tl_node->Cast<ListParseResult>();
	if (tl_wrapper.GetChildren().empty()) {
		return cols;
	}
	auto &tl_list = tl_wrapper.Child<ListParseResult>(0);
	auto items = ExtractParseResultsFromList(tl_list);
	for (auto &item_ref : items) {
		ParseResult &item = item_ref.get();
		// Check if it's a star or qualified-star
		if (FindDescendantByName(item, "StarExpression") || FindDescendantByName(item, "QualifiedStarExpression")) {
			has_star = true;
			continue;
		}
		// AliasedExpression is a named rule -> ListParseResult with one child: ChoiceParseResult.
		// Unwrap: LIST("AliasedExpression") -> CHOICE -> actual alternative.
		ParseResult *aliased = &item;
		if (aliased->type == ParseResultType::LIST && aliased->Name() == "AliasedExpression") {
			auto &al = aliased->Cast<ListParseResult>();
			if (!al.GetChildren().empty()) {
				aliased = &al.GetChild(0);
			}
		}
		if (aliased->type == ParseResultType::CHOICE) {
			aliased = &aliased->Cast<ChoiceParseResult>().GetResult();
		}
		auto name = SynthesizeColumnName(*aliased);
		if (!name.empty()) {
			cols.push_back(std::move(name));
		}
	}
	if (has_star) {
		// Recursively gather columns from the inner FROM clause.
		auto inner_from = FindDescendantByName(subquery_ref, "FromClause");
		if (inner_from) {
			vector<VisibleTable> inner_tables;
			CollectVisibleRefsRecursive(*inner_from, inner_tables, recurse_depth + 1);
			for (auto &ivt : inner_tables) {
				if (!ivt.catalog_backed) {
					// Inner subquery/CTE with known synthesized columns — inline them.
					for (auto &c : ivt.synthesized_columns) {
						cols.push_back(c);
					}
				}
				// Catalog-backed inner tables are returned via the out parameter below.
			}
		}
	}
	return cols;
}

// Variant that also fills star_sources on the VisibleTable when the subquery uses SELECT *.
static void SynthesizeColumnsFromSubqueryInto(ParseResult &subquery_ref, idx_t recurse_depth, VisibleTable &out) {
	out.synthesized_columns = SynthesizeColumnsFromSubquery(subquery_ref, recurse_depth);
	// If synthesized_columns is empty, the subquery may have used SELECT *.
	// Collect catalog-backed inner tables so the suggestion layer can do a catalog lookup.
	if (out.synthesized_columns.empty()) {
		auto inner_from = FindDescendantByName(subquery_ref, "FromClause");
		if (inner_from) {
			vector<VisibleTable> inner_tables;
			CollectVisibleRefsRecursive(*inner_from, inner_tables, recurse_depth + 1);
			for (auto &ivt : inner_tables) {
				if (ivt.catalog_backed && !ivt.name.name.empty()) {
					out.star_sources.push_back(ivt.name);
				}
			}
		}
	}
}

static void CollectVisibleRefsRecursive(ParseResult &pr, vector<VisibleTable> &out, idx_t recurse_depth) {
	// ChoiceParseResult inherits its name from the matched child (e.g., "BaseTableRef").
	// Unwrap CHOICE nodes so the real parse result is passed to named-rule handlers.
	if (pr.type == ParseResultType::CHOICE) {
		CollectVisibleRefsRecursive(pr.Cast<ChoiceParseResult>().GetResult(), out, recurse_depth);
		return;
	}
	if (pr.Name() == "BaseTableRef") {
		VisibleTable vt;
		FillVisibleTableFromBaseTableRef(pr, vt);
		out.push_back(std::move(vt));
		return;
	}
	if (pr.Name() == "TableSubquery") {
		// TableSubquery <- TableAliasColon? Lateral? SubqueryReference TableAlias?
		// Only useful if there is an alias (otherwise can't be referenced)
		string alias_str;
		if (auto colon = FindDescendantByName(pr, "TableAliasColon")) {
			alias_str = ExtractFirstIdentifier(*colon);
		}
		if (auto talias = FindDescendantByName(pr, "TableAlias")) {
			alias_str = ExtractFirstIdentifier(*talias);
		}
		if (alias_str.empty()) {
			return;
		}
		auto subquery_ref = FindDescendantByName(pr, "SubqueryReference");
		if (!subquery_ref) {
			return;
		}
		VisibleTable vt;
		vt.alias = alias_str;
		vt.catalog_backed = false;
		SynthesizeColumnsFromSubqueryInto(*subquery_ref, recurse_depth, vt);
		out.push_back(std::move(vt));
		return;
	}
	switch (pr.type) {
	case ParseResultType::LIST: {
		for (auto &child : pr.Cast<ListParseResult>().GetChildren()) {
			CollectVisibleRefsRecursive(child.get(), out, recurse_depth);
		}
		break;
	}
	case ParseResultType::OPTIONAL:
		if (pr.Cast<OptionalParseResult>().HasResult()) {
			CollectVisibleRefsRecursive(pr.Cast<OptionalParseResult>().GetResult(), out, recurse_depth);
		}
		break;
	case ParseResultType::REPEAT:
		for (auto &child : pr.Cast<RepeatParseResult>().GetChildren()) {
			CollectVisibleRefsRecursive(child.get(), out, recurse_depth);
		}
		break;
	default:
		break;
	}
}

} // namespace

void CollectVisibleTablesFromTokens(const Matcher &from_clause_root, const vector<MatcherToken> &tokens,
                                    vector<VisibleTable> &visible_tables) {
	visible_tables.clear();
	try {
		const idx_t from_idx = FindEnclosingFromKeyword(tokens);
		if (from_idx == DConstants::INVALID_INDEX) {
			return;
		}
		const idx_t slice_end = ComputeFromSliceEnd(tokens, from_idx);
		auto begin_it = tokens.begin();
		vector<MatcherToken> slice(begin_it + NumericCast<std::ptrdiff_t>(from_idx),
		                           begin_it + NumericCast<std::ptrdiff_t>(slice_end));
		ParseResultAllocator allocator;
		auto parse_root = MatchTokens(from_clause_root, std::move(slice), allocator);
		if (!parse_root) {
			return;
		}
		auto &from_clause = parse_root->Cast<ListParseResult>();
		auto &table_ref_list_node = from_clause.Child<ListParseResult>(1);
		for (auto &table_ref_pr : ExtractParseResultsFromList(table_ref_list_node)) {
			CollectVisibleRefsRecursive(table_ref_pr.get(), visible_tables);
		}
	} catch (const Exception &) {
		visible_tables.clear();
	}
}

// Synthesize column names from a CTEBody.
// WithStatement <- ColIdOrString ColumnAliases? UsingKey? 'AS' Materialized? CTEBody
// CTEBody <- CTESelectBody / CTEDMLBody
static vector<string> SynthesizeCTEColumns(ParseResult &with_statement) {
	// Check for explicit column aliases first
	// ColumnAliases <- Parens(List(ColIdOrString))
	auto icl_opt = FindDescendantByName(with_statement, "ColumnAliases");
	if (icl_opt) {
		// ColumnAliases? in WithStatement may wrap the node in an OptionalParseResult.
		// Unwrap the Optional, then walk the resulting node for all identifiers.
		ParseResult *icl_node = icl_opt.get();
		while (icl_node && icl_node->type == ParseResultType::OPTIONAL) {
			auto &opt = icl_node->Cast<OptionalParseResult>();
			icl_node = opt.HasResult() ? &opt.GetResult() : nullptr;
		}
		if (icl_node) {
			vector<string> cols;
			// Walk to find all identifiers inside ColumnAliases <- Parens(List(ColIdOrString))
			std::function<void(ParseResult &)> gather = [&](ParseResult &node) {
				if (node.type == ParseResultType::IDENTIFIER) {
					cols.push_back(node.Cast<IdentifierParseResult>().identifier.GetIdentifierName());
					return;
				}
				if (node.type == ParseResultType::STRING) {
					cols.push_back(node.Cast<StringLiteralParseResult>().GetRawString());
					return;
				}
				switch (node.type) {
				case ParseResultType::LIST:
					for (auto &c : node.Cast<ListParseResult>().GetChildren()) {
						gather(c.get());
					}
					break;
				case ParseResultType::CHOICE:
					gather(node.Cast<ChoiceParseResult>().GetResult());
					break;
				case ParseResultType::OPTIONAL: {
					auto &op = node.Cast<OptionalParseResult>();
					if (op.HasResult()) {
						gather(op.GetResult());
					}
					break;
				}
				case ParseResultType::REPEAT:
					for (auto &c : node.Cast<RepeatParseResult>().GetChildren()) {
						gather(c.get());
					}
					break;
				default:
					break;
				}
			};
			gather(*icl_node);
			if (!cols.empty()) {
				return cols;
			}
		}
	}
	// Fall back to TargetList synthesis from CTEBody
	// For RECURSIVE CTEs use first branch of UNION (first TargetList found)
	auto target_list = FindDescendantByName(with_statement, "TargetList");
	if (!target_list) {
		return {};
	}
	vector<string> cols;
	// Unwrap Optional wrapper if present (TargetList? in SelectClause).
	ParseResult *tl_node2 = target_list.get();
	if (tl_node2->type == ParseResultType::OPTIONAL) {
		auto &opt2 = tl_node2->Cast<OptionalParseResult>();
		if (!opt2.HasResult()) {
			return cols;
		}
		tl_node2 = &opt2.GetResult();
	}
	// TargetList <- List(AliasedExpression) — named rule wrapper has one child: the List body.
	auto &tl_wrapper2 = tl_node2->Cast<ListParseResult>();
	if (tl_wrapper2.GetChildren().empty()) {
		return cols;
	}
	auto &tl_list = tl_wrapper2.Child<ListParseResult>(0);
	auto items = ExtractParseResultsFromList(tl_list);
	for (auto &item_ref : items) {
		ParseResult &item = item_ref.get();
		// Skip stars
		if (FindDescendantByName(item, "StarExpression") || FindDescendantByName(item, "QualifiedStarExpression")) {
			continue;
		}
		// AliasedExpression is a named rule -> ListParseResult with one child: ChoiceParseResult.
		ParseResult *aliased = &item;
		if (aliased->type == ParseResultType::LIST && aliased->Name() == "AliasedExpression") {
			auto &al = aliased->Cast<ListParseResult>();
			if (!al.GetChildren().empty()) {
				aliased = &al.GetChild(0);
			}
		}
		if (aliased->type == ParseResultType::CHOICE) {
			aliased = &aliased->Cast<ChoiceParseResult>().GetResult();
		}
		auto name = SynthesizeColumnName(*aliased);
		if (!name.empty()) {
			cols.push_back(std::move(name));
		}
	}
	return cols;
}

void CollectCTEVisibleTables(const Matcher &with_clause_root, const vector<MatcherToken> &tokens,
                             vector<VisibleTable> &visible_tables) {
	// Compute the cursor's paren depth
	idx_t cursor_depth = 0;
	for (auto &t : tokens) {
		if (t.text == "(") {
			cursor_depth++;
		} else if (t.text == ")" && cursor_depth > 0) {
			cursor_depth--;
		}
	}
	// Find all WITH keywords at paren-depth <= cursor_depth
	// Process outermost first (they can be shadowed by inner ones)
	case_insensitive_map_t<VisibleTable> cte_map;
	idx_t depth = 0;
	for (idx_t i = 0; i < tokens.size(); i++) {
		const auto &t = tokens[i];
		if (t.text == "(") {
			depth++;
		} else if (t.text == ")" && depth > 0) {
			depth--;
		} else if (t.type == TokenType::KEYWORD && StringUtil::CIEquals(t.text, "with") && depth <= cursor_depth) {
			// Slice from this WITH up to a top-level statement keyword or until end-of-slice
			// Find the end: first SELECT/INSERT/UPDATE/DELETE/MERGE at this depth
			idx_t slice_end = tokens.size();
			idx_t inner_depth = 0;
			for (idx_t j = i + 1; j < tokens.size(); j++) {
				const auto &tj = tokens[j];
				if (tj.text == "(") {
					inner_depth++;
				} else if (tj.text == ")" && inner_depth > 0) {
					inner_depth--;
				} else if (inner_depth == 0 && tj.type == TokenType::KEYWORD) {
					if (StringUtil::CIEquals(tj.text, "select") || StringUtil::CIEquals(tj.text, "insert") ||
					    StringUtil::CIEquals(tj.text, "update") || StringUtil::CIEquals(tj.text, "delete") ||
					    StringUtil::CIEquals(tj.text, "merge")) {
						slice_end = j;
						break;
					}
				}
			}
			auto begin_it = tokens.begin();
			vector<MatcherToken> slice(begin_it + NumericCast<std::ptrdiff_t>(i),
			                           begin_it + NumericCast<std::ptrdiff_t>(slice_end));
			try {
				ParseResultAllocator allocator;
				auto parse_root = MatchTokens(with_clause_root, std::move(slice), allocator);
				if (!parse_root) {
					continue;
				}
				// WithClause <- 'WITH' Recursive? List(WithStatement)
				// Extract each WithStatement
				auto with_stmts_list = FindDescendantByName(*parse_root, "List");
				if (!with_stmts_list) {
					continue;
				}
				auto stmt_items = ExtractParseResultsFromList(*with_stmts_list);
				for (auto &stmt_ref : stmt_items) {
					ParseResult &stmt = stmt_ref.get();
					// WithStatement <- ColIdOrString ColumnAliases? UsingKey? 'AS' Materialized? CTEBody
					auto name_node = FindDescendantByName(stmt, "ColIdOrString");
					if (!name_node) {
						continue;
					}
					string cte_name;
					try {
						cte_name = ExtractFirstIdentifier(*name_node);
					} catch (...) {
						continue;
					}
					if (cte_name.empty()) {
						continue;
					}
					VisibleTable vt;
					vt.alias = cte_name;
					vt.catalog_backed = false;
					vt.synthesized_columns = SynthesizeCTEColumns(stmt);
					// Inner CTEs shadow outer ones (map insert overwrites)
					cte_map[cte_name] = std::move(vt);
				}
			} catch (const Exception &) {
			}
		}
	}
	for (auto &kv : cte_map) {
		visible_tables.push_back(std::move(kv.second));
	}
}

void FillQualifierContext(const vector<MatcherToken> &tokens, SuggestionState type, AutoCompleteFilter &filter) {
	filter.catalog_name.clear();
	filter.schema_name.clear();
	filter.table_ref.clear();
	switch (type) {
	case SuggestionState::SUGGEST_TABLE_NAME: {
		auto segs = CollectDottedChainBackwards(tokens, 2);
		if (segs.size() >= 1) {
			filter.schema_name = segs[0];
		}
		if (segs.size() >= 2) {
			filter.catalog_name = segs[1];
		}
		break;
	}
	case SuggestionState::SUGGEST_SCHEMA_NAME: {
		auto segs = CollectDottedChainBackwards(tokens, 1);
		if (!segs.empty()) {
			filter.catalog_name = segs[0];
		}
		break;
	}
	case SuggestionState::SUGGEST_COLUMN_NAME: {
		auto segs = CollectDottedChainBackwards(tokens, 3);
		if (segs.size() >= 1) {
			filter.table_ref = segs[0];
		}
		if (segs.size() >= 2) {
			filter.schema_name = segs[1];
		}
		if (segs.size() >= 3) {
			filter.catalog_name = segs[2];
		}
		break;
	}
	default:
		break;
	}
}

static QualifierMatchers FindQualifierMatchers(const CompiledGrammar &grammar) {
	QualifierMatchers result;
	reference_set_t<const Matcher> visited;
	vector<reference<const Matcher>> pending;
	pending.push_back(grammar.ProgramMatcher());
	while (!pending.empty() && (!result.from_clause || !result.with_clause)) {
		auto &matcher = pending.back().get();
		pending.pop_back();
		if (!visited.insert(matcher).second) {
			continue;
		}
		auto rule = matcher.GetRule();
		if (rule && matcher.Type() == MatcherType::LIST) {
			if (!result.from_clause && StringUtil::CIEquals(rule->name, "FromClause")) {
				result.from_clause = &matcher;
			} else if (!result.with_clause && StringUtil::CIEquals(rule->name, "WithClause")) {
				result.with_clause = &matcher;
			}
		}
		switch (matcher.Type()) {
		case MatcherType::LIST:
			for (auto &child : matcher.Cast<ListMatcher>().matchers) {
				pending.push_back(child.get());
			}
			break;
		case MatcherType::CHOICE:
			for (auto &child : matcher.Cast<ChoiceMatcher>().matchers) {
				pending.push_back(child.get());
			}
			break;
		case MatcherType::OPTIONAL:
			pending.push_back(matcher.Cast<OptionalMatcher>().GetChildMatcher());
			break;
		case MatcherType::REPEAT:
			pending.push_back(matcher.Cast<RepeatMatcher>().GetChildMatcher());
			break;
		default:
			break;
		}
	}
	return result;
}

QualifierMatchers GetQualifierMatchers(const shared_ptr<CompiledGrammar> &grammar) {
	// The matcher graph is immutable once compiled; remember the lookup for the most recent grammar.
	static std::mutex cache_lock;
	static weak_ptr<CompiledGrammar> cached_grammar;
	static QualifierMatchers cached_matchers;
	std::lock_guard<std::mutex> guard(cache_lock);
	auto cached = cached_grammar.lock();
	if (cached && cached.get() == grammar.get()) {
		return cached_matchers;
	}
	cached_matchers = FindQualifierMatchers(*grammar);
	cached_grammar = grammar;
	return cached_matchers;
}

} // namespace smart_autocomplete
} // namespace duckdb

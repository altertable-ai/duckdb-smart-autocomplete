//===----------------------------------------------------------------------===//
//                         DuckDB
//
// autocomplete_qualifier.hpp
//
// Helpers to derive catalog/schema/table context from tokenized SQL for
// qualifier-aware autocomplete.
//===----------------------------------------------------------------------===//

#pragma once

#include "matcher.hpp"
#include "duckdb/parser/qualified_name.hpp"

namespace duckdb {

struct VisibleTable {
	QualifiedName name;
	string alias;
	//! Non-empty when columns come from a subquery/CTE projection (not the catalog).
	vector<string> synthesized_columns;
	//! Catalog-backed tables whose columns a SELECT * in this subquery expands to.
	//! Used when synthesized_columns is empty because the subquery projected with *.
	vector<QualifiedName> star_sources;
	//! When false, use synthesized_columns instead of catalog lookup.
	bool catalog_backed = true;
};

//! Filter for catalog-backed suggestions (schema / table / column lists).
struct AutoCompleteFilter {
	string catalog_name;
	string schema_name;
	//! For qualified columns: identifier chain before the column (table or alias).
	string table_ref;
	vector<VisibleTable> visible_tables;
};

//! Populate catalog_context / schema_context / table_ref on a suggestion from the token stream.
void FillMatcherQualifierContext(const vector<MatcherToken> &tokens, SuggestionState type,
                                 MatcherSuggestion &suggestion);

//! Collect tables (and optional aliases) from the FROM clause for column scoping.
//! Uses the cached \p from_clause_root matcher (`FromClause` rule) and strict mode: if the slice does not
//! fully parse, \p visible_tables is left empty.
void CollectVisibleTablesFromTokens(Matcher &from_clause_root, ParseResultAllocator &allocator,
                                    const vector<MatcherToken> &tokens, vector<VisibleTable> &visible_tables);

//! Collect CTE bindings visible at the cursor position from any enclosing WITH clauses.
//! Inner CTEs shadow outer CTEs with the same name (SQL scoping rules).
void CollectCTEVisibleTables(Matcher &with_clause_root, ParseResultAllocator &allocator,
                             const vector<MatcherToken> &tokens, vector<VisibleTable> &visible_tables);

} // namespace duckdb

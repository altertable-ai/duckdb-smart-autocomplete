//===----------------------------------------------------------------------===//
//                         DuckDB
//
// autocomplete_qualifier.hpp
//
// Helpers to derive catalog/schema/table context from tokenized SQL for
// qualifier-aware autocomplete.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/peg/matcher.hpp"

namespace duckdb {
struct CompiledGrammar;

namespace smart_autocomplete {

//! catalog.schema.name as written in the query (unresolved; catalog/schema may be empty).
struct TableQualifiedName {
	string catalog;
	string schema;
	string name;
};

struct VisibleTable {
	TableQualifiedName name;
	string alias;
	//! Non-empty when columns come from a subquery/CTE projection (not the catalog).
	vector<string> synthesized_columns;
	//! Catalog-backed tables whose columns a SELECT * in this subquery expands to.
	//! Used when synthesized_columns is empty because the subquery projected with *.
	vector<TableQualifiedName> star_sources;
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

//! Matchers of the grammar rules used to analyse the query around the cursor.
struct QualifierMatchers {
	optional_ptr<const Matcher> from_clause;
	optional_ptr<const Matcher> with_clause;
};

//! Look up the `FromClause` / `WithClause` rule matchers in a compiled grammar (cached per grammar).
QualifierMatchers GetQualifierMatchers(const shared_ptr<CompiledGrammar> &grammar);

//! Populate catalog_name / schema_name / table_ref on \p filter from the dotted chain before the cursor.
//! \p tokens must not contain end-of-input sentinels.
void FillQualifierContext(const vector<MatcherToken> &tokens, SuggestionState type, AutoCompleteFilter &filter);

//! Collect tables (and optional aliases) from the FROM clause for column scoping.
//! Uses the `FromClause` rule matcher; if the slice does not parse, \p visible_tables is left empty.
void CollectVisibleTablesFromTokens(const Matcher &from_clause_root, const vector<MatcherToken> &tokens,
                                    vector<VisibleTable> &visible_tables);

//! Collect CTE bindings visible at the cursor position from any enclosing WITH clauses.
//! Inner CTEs shadow outer CTEs with the same name (SQL scoping rules).
void CollectCTEVisibleTables(const Matcher &with_clause_root, const vector<MatcherToken> &tokens,
                             vector<VisibleTable> &visible_tables);

} // namespace smart_autocomplete
} // namespace duckdb

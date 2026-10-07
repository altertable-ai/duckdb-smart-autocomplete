//===----------------------------------------------------------------------===//
//                         DuckDB
//
// autocomplete_catalog_provider.hpp
//
// Smart autocomplete: catalog provider interface with qualifier-aware filters.
// Lives in duckdb::smart_autocomplete so it does not clash with the (filter-less)
// duckdb::AutoCompleteCatalogProvider that ships in DuckDB core.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "autocomplete_qualifier.hpp"
#include "duckdb/parser/peg/compiled_grammar.hpp"

namespace duckdb {
namespace smart_autocomplete {

//! Parameters for autocomplete suggestion generation.
struct AutoCompleteParameters {
	idx_t max_suggestion_count = 20;
	idx_t max_file_suggestion_count = 1;
	idx_t max_exact_suggestion_count = 100;
	bool suggestion_contains_files = false;
};

//! Abstract interface for providing catalog-aware autocomplete suggestions.
//! The PEG parser and keyword matcher work without this — catalog suggestions
//! are additive (table names, column names, function names, etc.).
class AutoCompleteCatalogProvider {
public:
	virtual ~AutoCompleteCatalogProvider() = default;

	virtual vector<AutoCompleteCandidate> SuggestCatalogName() = 0;
	virtual vector<AutoCompleteCandidate> SuggestSchemaName(const AutoCompleteFilter &filter) = 0;
	virtual vector<AutoCompleteCandidate> SuggestTableName(const AutoCompleteFilter &filter) = 0;
	virtual vector<AutoCompleteCandidate> SuggestType() = 0;
	virtual vector<AutoCompleteCandidate> SuggestColumnName(const AutoCompleteFilter &filter) = 0;
	virtual vector<AutoCompleteCandidate> SuggestFileName(string &prefix, idx_t &last_pos) = 0;
	virtual vector<AutoCompleteCandidate> SuggestScalarFunctionName() = 0;
	virtual vector<AutoCompleteCandidate> SuggestTableFunctionName() = 0;
	virtual vector<AutoCompleteCandidate> SuggestPragmaName() = 0;
	virtual vector<AutoCompleteCandidate> SuggestSettingName() = 0;

	//! Get the compiled grammar for syntax-level autocomplete.
	virtual shared_ptr<CompiledGrammar> GetCompiledGrammar() = 0;
};

//! Get a human-readable string for a suggestion type.
string GetSuggestionType(SuggestionState type);

//! Generate autocomplete suggestions for the given SQL text.
//! Uses the PEG grammar for syntax and the provider for catalog awareness.
vector<AutoCompleteSuggestion> GenerateAutoCompleteSuggestions(AutoCompleteCatalogProvider &provider, const string &sql,
                                                               AutoCompleteParameters &parameters);

} // namespace smart_autocomplete
} // namespace duckdb

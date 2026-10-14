//===----------------------------------------------------------------------===//
//                         DuckDB
//
// parse_tree_walk.hpp
//
// Small DFS helpers over PEG MatchParseResult trees for autocomplete.
//===----------------------------------------------------------------------===//

#pragma once

// matcher.hpp defines the inline ParseResult::SetNameFrom(const Matcher &) that
// ListParseResult's constructor calls. Including only parse_result.hpp leaves that
// function undefined in this translation unit (-Wundefined-inline).
#include "duckdb/parser/peg/matcher.hpp"

namespace duckdb {
namespace smart_autocomplete {

//! DFS through List/Choice/Optional/Repeat wrappers. Skips empty Optionals.
//! Returns the first node whose `name` equals \p rule_name (grammar rule name).
optional_ptr<ParseResult> FindDescendantByName(ParseResult &pr, const string &rule_name);

//! Unwraps Choice/List to the first IDENTIFIER or STRING leaf and returns its logical text.
string ExtractFirstIdentifier(ParseResult &pr);

} // namespace smart_autocomplete
} // namespace duckdb

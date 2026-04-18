//===----------------------------------------------------------------------===//
//                         DuckDB
//
// parse_tree_walk.hpp
//
// Small DFS helpers over PEG MatchParseResult trees for autocomplete.
//===----------------------------------------------------------------------===//

#pragma once

#include "transformer/parse_result.hpp"

namespace duckdb {

//! DFS through List/Choice/Optional/Repeat wrappers. Skips empty Optionals.
//! Returns the first node whose `name` equals \p rule_name (grammar rule name).
optional_ptr<ParseResult> FindDescendantByName(ParseResult &pr, const string &rule_name);

//! Unwraps Choice/List to the first IDENTIFIER or STRING leaf and returns its logical text.
string ExtractFirstIdentifier(ParseResult &pr);

} // namespace duckdb

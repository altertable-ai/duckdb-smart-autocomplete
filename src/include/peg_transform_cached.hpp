//===----------------------------------------------------------------------===//
// Smart autocomplete: transform pre-matched PEG parse tree using factory maps.
// Declaration only — implementation in peg_transform_with_cached_matcher.cpp.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/parser/parser_options.hpp"
#include "duckdb/parser/sql_statement.hpp"
#include "matcher.hpp"

namespace duckdb {

struct PEGMatcher;

unique_ptr<SQLStatement> PEGTransformWithCachedMatcher(vector<MatcherToken> &tokens, ParserOptions &options,
                                                       shared_ptr<PEGMatcher> peg_matcher);

} // namespace duckdb

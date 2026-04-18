//===----------------------------------------------------------------------===//
//                         DuckDB
//
// peg_matcher_cache.hpp
//
// Smart autocomplete only: shared compiled PEG matchers (Statement / FromClause
// / WithClause) cached on ParserExtensionInfo.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/parser/parser_extension.hpp"
#include "matcher.hpp"
#include <mutex>

namespace duckdb {

struct PEGMatcher {
	MatcherAllocator allocator;

	Matcher &Root() {
		return *root;
	}

	Matcher &FromClauseRoot() {
		return *from_clause_root;
	}

	Matcher &WithClauseRoot() {
		return *with_clause_root;
	}

private:
	friend struct PEGMatcherCache;
	optional_ptr<Matcher> root;
	optional_ptr<Matcher> from_clause_root;
	optional_ptr<Matcher> with_clause_root;
};

struct PEGMatcherCache : ParserExtensionInfo {
	shared_ptr<PEGMatcher> GetMatcher();
	void Invalidate();

private:
	std::mutex mutex;
	shared_ptr<PEGMatcher> matcher;
};

} // namespace duckdb

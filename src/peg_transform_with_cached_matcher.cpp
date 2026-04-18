#include "peg_matcher_cache.hpp"
#include "duckdb/common/allocator.hpp"
#include "duckdb/common/exception/parser_exception.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/storage/arena_allocator.hpp"

// Read PEGTransformerFactory singleton maps without modifying the DuckDB submodule
// (members are private in transformer/peg_transformer.hpp).
#define private public
#include "transformer/peg_transformer.hpp"
#undef private

namespace duckdb {

unique_ptr<SQLStatement> PEGTransformWithCachedMatcher(vector<MatcherToken> &tokens, ParserOptions &options,
                                                     shared_ptr<PEGMatcher> peg_matcher) {
	D_ASSERT(peg_matcher);
	string token_stream;
	for (auto &token : tokens) {
		token_stream += token.text + " ";
	}
	vector<MatcherSuggestion> suggestions;
	ParseResultAllocator parse_result_allocator;
	idx_t max_token_index = 0;
	MatchState state(tokens, suggestions, parse_result_allocator, max_token_index, options.preserve_identifier_case);
	auto match_result = peg_matcher->Root().MatchParseResult(state);
	if (match_result == nullptr || state.token_index < state.tokens.size()) {
		idx_t error_token_idx = state.GetMaxTokenIndex();
		if (error_token_idx >= tokens.size()) {
			error_token_idx = tokens.size() - 1;
		}
		auto &error_token = tokens[error_token_idx];
		string token_list;
		for (idx_t i = 0; i < tokens.size(); i++) {
			if (!token_list.empty()) {
				token_list += "\n";
			}
			if (i < 10) {
				token_list += " ";
			}
			token_list += to_string(i) + ":" + tokens[i].text;
		}
		auto error_message = "Syntax error at or near \"" + error_token.text + "\"";
		throw ParserException::SyntaxError(token_stream, error_message, error_token.offset);
	}
	match_result->name = "Statement";
	ArenaAllocator transformer_allocator(Allocator::DefaultAllocator());
	PEGTransformerState transformer_state(tokens);
	auto &factory = PEGTransformerFactory::GetInstance();
	PEGTransformer transformer(transformer_allocator, transformer_state, factory.sql_transform_functions,
	                           factory.parser.rules, factory.enum_mappings, options);
	auto result = transformer.Transform<unique_ptr<SQLStatement>>(match_result);
	if (!transformer.pivot_entries.empty()) {
		result = transformer.CreatePivotStatement(std::move(result));
	}
	transformer.Clear();
	return result;
}

} // namespace duckdb

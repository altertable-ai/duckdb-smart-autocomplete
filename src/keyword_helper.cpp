#include "keyword_helper.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

string PEGKeywordHelper::UnescapeQuotes(const string &text, const char quote) {
	return StringUtil::Replace(text, string(2, quote), string(1, quote));
}

string PEGKeywordHelper::TryUnescapeQuotes(const string &text, const char quote) {
	if (text.size() >= 2 && text.front() == quote && text.back() == quote) {
		return UnescapeQuotes(text.substr(1, text.size() - 2), quote);
	}
	return text;
}

PEGKeywordHelper &PEGKeywordHelper::Instance() {
	static PEGKeywordHelper instance;
	return instance;
}

PEGKeywordHelper::PEGKeywordHelper() {
	InitializeKeywordMaps();
}

bool PEGKeywordHelper::KeywordCategoryType(const std::string &text, const PEGKeywordCategory type) const {
	switch (type) {
	case PEGKeywordCategory::KEYWORD_RESERVED: {
		auto it = reserved_keyword_map.find(text);
		return it != reserved_keyword_map.end();
	}
	case PEGKeywordCategory::KEYWORD_UNRESERVED: {
		auto it = unreserved_keyword_map.find(text);
		return it != unreserved_keyword_map.end();
	}
	case PEGKeywordCategory::KEYWORD_TYPE_FUNC: {
		auto it = typefunc_keyword_map.find(text);
		return it != typefunc_keyword_map.end();
	}
	case PEGKeywordCategory::KEYWORD_COL_NAME: {
		auto it = colname_keyword_map.find(text);
		return it != colname_keyword_map.end();
	}
	default:
		return false;
	}
}
} // namespace duckdb

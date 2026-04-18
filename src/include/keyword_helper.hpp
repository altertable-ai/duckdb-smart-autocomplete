#pragma once

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/string.hpp"

namespace duckdb {
enum class PEGKeywordCategory : uint8_t {
	KEYWORD_NONE,
	KEYWORD_UNRESERVED,
	KEYWORD_RESERVED,
	KEYWORD_TYPE_FUNC,
	KEYWORD_COL_NAME
};

class PEGKeywordHelper {
public:
	static PEGKeywordHelper &Instance();

	//! Inverse of SQL doubled-quote escaping (inner segment of a quoted identifier or string literal).
	static string UnescapeQuotes(const string &text, char quote = '"');
	//! If \p text is wrapped in \p quote, unwrap and unescape inner doubled quotes; otherwise return \p text.
	static string TryUnescapeQuotes(const string &text, char quote = '"');

	bool KeywordCategoryType(const string &text, PEGKeywordCategory type) const;
	void InitializeKeywordMaps();
	bool IsKeyword(const string &text) {
		if (reserved_keyword_map.count(text) != 0 || unreserved_keyword_map.count(text) != 0 ||
		    colname_keyword_map.count(text) != 0 || typefunc_keyword_map.count(text) != 0) {
			return true;
		}
		return false;
	};

private:
	PEGKeywordHelper();
	bool initialized;
	case_insensitive_set_t reserved_keyword_map;
	case_insensitive_set_t unreserved_keyword_map;
	case_insensitive_set_t colname_keyword_map;
	case_insensitive_set_t typefunc_keyword_map;
};

} // namespace duckdb

#include "parse_tree_walk.hpp"

#include "duckdb/common/exception.hpp"

namespace duckdb {
namespace smart_autocomplete {

optional_ptr<ParseResult> FindDescendantByName(ParseResult &pr, const string &rule_name) {
	if (pr.Name() == rule_name) {
		return optional_ptr<ParseResult>(&pr);
	}
	switch (pr.type) {
	case ParseResultType::LIST: {
		for (auto &child : pr.Cast<ListParseResult>().GetChildren()) {
			auto found = FindDescendantByName(child.get(), rule_name);
			if (found) {
				return found;
			}
		}
		break;
	}
	case ParseResultType::CHOICE:
		return FindDescendantByName(pr.Cast<ChoiceParseResult>().GetResult(), rule_name);
	case ParseResultType::OPTIONAL: {
		auto &opt = pr.Cast<OptionalParseResult>();
		if (opt.HasResult()) {
			return FindDescendantByName(opt.GetResult(), rule_name);
		}
		break;
	}
	case ParseResultType::REPEAT: {
		for (auto &child : pr.Cast<RepeatParseResult>().GetChildren()) {
			auto found = FindDescendantByName(child.get(), rule_name);
			if (found) {
				return found;
			}
		}
		break;
	}
	default:
		break;
	}
	return nullptr;
}

string ExtractFirstIdentifier(ParseResult &pr) {
	switch (pr.type) {
	case ParseResultType::IDENTIFIER:
		return pr.Cast<IdentifierParseResult>().identifier.GetIdentifierName();
	case ParseResultType::STRING:
		return pr.Cast<StringLiteralParseResult>().GetRawString();
	case ParseResultType::CHOICE:
		return ExtractFirstIdentifier(pr.Cast<ChoiceParseResult>().GetResult());
	case ParseResultType::KEYWORD:
		// unreserved keyword used as an identifier (e.g. ColId <- UnreservedKeyword / ...)
		return pr.Cast<KeywordParseResult>().keyword;
	case ParseResultType::LIST: {
		// skip leading keywords (e.g. TableAliasAs <- 'AS' IdentifierOrStringLiteral ColumnAliases?)
		auto children = pr.Cast<ListParseResult>().GetChildren();
		for (idx_t i = 0; i < children.size(); i++) {
			auto &child = children[i].get();
			if (child.type == ParseResultType::KEYWORD && i + 1 < children.size()) {
				continue;
			}
			return ExtractFirstIdentifier(child);
		}
		break;
	}
	case ParseResultType::OPTIONAL: {
		auto &opt = pr.Cast<OptionalParseResult>();
		if (opt.HasResult()) {
			return ExtractFirstIdentifier(opt.GetResult());
		}
		break;
	}
	default:
		break;
	}
	throw InternalException("ExtractFirstIdentifier: unexpected node type %s for rule %s", ParseResultToString(pr.type),
	                        pr.Name());
}

} // namespace smart_autocomplete
} // namespace duckdb

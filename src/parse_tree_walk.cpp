#include "parse_tree_walk.hpp"

#include "duckdb/common/exception.hpp"

namespace duckdb {

optional_ptr<ParseResult> FindDescendantByName(ParseResult &pr, const string &rule_name) {
	if (pr.name == rule_name) {
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
	case ParseResultType::CHOICE: {
		return FindDescendantByName(pr.Cast<ChoiceParseResult>().GetResult(), rule_name);
	}
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
		return pr.Cast<IdentifierParseResult>().identifier;
	case ParseResultType::STRING:
		return pr.Cast<StringLiteralParseResult>().GetRawString();
	case ParseResultType::CHOICE:
		return ExtractFirstIdentifier(pr.Cast<ChoiceParseResult>().GetResult());
	case ParseResultType::LIST: {
		auto &list_pr = pr.Cast<ListParseResult>();
		if (list_pr.GetChildren().empty()) {
			break;
		}
		return ExtractFirstIdentifier(list_pr.GetChild(0));
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
	                        pr.name);
}

} // namespace duckdb

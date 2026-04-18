#include "parse_tree_walk.hpp"

#include "duckdb/common/exception.hpp"

namespace duckdb {

optional_ptr<ParseResult> FindDescendantByName(ParseResult &pr, const string &rule_name) {
	if (pr.name == rule_name) {
		return optional_ptr<ParseResult>(&pr);
	}
	switch (pr.type) {
	case ParseResultType::LIST: {
		for (auto child : pr.Cast<ListParseResult>().GetChildren()) {
			if (!child) {
				continue;
			}
			auto found = FindDescendantByName(*child, rule_name);
			if (found) {
				return found;
			}
		}
		break;
	}
	case ParseResultType::CHOICE: {
		auto &ch = pr.Cast<ChoiceParseResult>();
		if (ch.result) {
			return FindDescendantByName(*ch.result, rule_name);
		}
		break;
	}
	case ParseResultType::OPTIONAL: {
		auto &opt = pr.Cast<OptionalParseResult>();
		if (opt.HasResult()) {
			return FindDescendantByName(*opt.optional_result, rule_name);
		}
		break;
	}
	case ParseResultType::REPEAT: {
		for (auto child : pr.Cast<RepeatParseResult>().children) {
			if (!child) {
				continue;
			}
			auto found = FindDescendantByName(*child, rule_name);
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
	case ParseResultType::CHOICE: {
		auto &ch = pr.Cast<ChoiceParseResult>();
		if (ch.result) {
			return ExtractFirstIdentifier(*ch.result);
		}
		break;
	}
	case ParseResultType::LIST: {
		auto &list_pr = pr.Cast<ListParseResult>();
		if (list_pr.GetChildren().empty()) {
			break;
		}
		auto c0 = list_pr.GetChild(0);
		if (c0) {
			return ExtractFirstIdentifier(*c0);
		}
		break;
	}
	case ParseResultType::OPTIONAL: {
		auto &opt = pr.Cast<OptionalParseResult>();
		if (opt.HasResult()) {
			return ExtractFirstIdentifier(*opt.optional_result);
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

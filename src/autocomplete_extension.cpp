#include "smart_autocomplete_extension.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "peg_transform_cached.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "matcher.hpp"
#include "peg_matcher_cache.hpp"
#include "autocomplete_catalog_provider.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/catalog/catalog_entry/pragma_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/catalog/standard_entry.hpp"
#include "duckdb/catalog/catalog_search_path.hpp"
#include "duckdb/main/client_data.hpp"
#include "tokenizer.hpp"
#include "duckdb/parser/parser_extension.hpp"
#include "duckdb/main/extension_callback_manager.hpp"

namespace duckdb {

static PEGMatcherCache &GetPEGMatcherCache(DBConfig &config) {
	for (auto &ext : config.GetCallbackManager().ParserExtensions()) {
		if (ext.parser_info) {
			auto *cache = dynamic_cast<PEGMatcherCache *>(ext.parser_info.get());
			if (cache) {
				return *cache;
			}
		}
	}
	throw InternalException("PEG autocomplete parser extension not registered");
}

struct SQLAutoCompleteFunctionData : public TableFunctionData {
	explicit SQLAutoCompleteFunctionData(vector<AutoCompleteSuggestion> suggestions_p)
	    : suggestions(std::move(suggestions_p)) {
	}

	vector<AutoCompleteSuggestion> suggestions;
};

struct SQLAutoCompleteData : public GlobalTableFunctionState {
	SQLAutoCompleteData() : offset(0) {
	}

	idx_t offset;
};

// AutoCompleteParameters is now in autocomplete_catalog_provider.hpp

// The following functions have been moved to autocomplete_core.cpp:
// - GetSuggestionType (now non-static, declared in autocomplete_catalog_provider.hpp)
// - PreferCaseMatching
// - ComputeSuggestions

static vector<shared_ptr<AttachedDatabase>> GetAllCatalogs(ClientContext &context) {
	vector<shared_ptr<AttachedDatabase>> result;

	auto &database_manager = DatabaseManager::Get(context);
	auto databases = database_manager.GetDatabases(context);
	for (auto &database : databases) {
		result.push_back(database);
	}
	return result;
}

static vector<reference<SchemaCatalogEntry>> GetAllSchemas(ClientContext &context) {
	return Catalog::GetAllSchemas(context);
}

static vector<reference<CatalogEntry>> GetAllTables(ClientContext &context, bool for_table_names) {
	vector<reference<CatalogEntry>> result;
	// scan all the schemas for tables and collect them and collect them
	// for column names we avoid adding internal entries, because it pollutes the auto-complete too much
	// for table names this is generally fine, however
	auto schemas = Catalog::GetAllSchemas(context);
	for (auto &schema_ref : schemas) {
		auto &schema = schema_ref.get();
		schema.Scan(context, CatalogType::TABLE_ENTRY, [&](CatalogEntry &entry) {
			if (!entry.internal || for_table_names) {
				result.push_back(entry);
			}
		});
	};
	if (for_table_names) {
		for (auto &schema_ref : schemas) {
			auto &schema = schema_ref.get();
			schema.Scan(context, CatalogType::TABLE_FUNCTION_ENTRY,
			            [&](CatalogEntry &entry) { result.push_back(entry); });
		};
	} else {
		for (auto &schema_ref : schemas) {
			auto &schema = schema_ref.get();
			schema.Scan(context, CatalogType::SCALAR_FUNCTION_ENTRY,
			            [&](CatalogEntry &entry) { result.push_back(entry); });
		};
	}
	return result;
}

static vector<reference<CatalogEntry>> GetAllTypes(ClientContext &context) {
	vector<reference<CatalogEntry>> result;
	// scan all the schemas for types and collect them
	auto schemas = Catalog::GetAllSchemas(context);
	for (auto &schema_ref : schemas) {
		auto &schema = schema_ref.get();
		schema.Scan(context, CatalogType::TYPE_ENTRY, [&](CatalogEntry &entry) { result.push_back(entry); });
	};
	return result;
}

static vector<AutoCompleteCandidate> SuggestCatalogName(ClientContext &context) {
	vector<AutoCompleteCandidate> suggestions;
	auto all_entries = GetAllCatalogs(context);
	for (auto &entry_ref : all_entries) {
		auto &entry = *entry_ref;
		AutoCompleteCandidate candidate(entry.name, SuggestionState::SUGGEST_CATALOG_NAME, 0);
		candidate.extra_char = '.';
		suggestions.push_back(std::move(candidate));
	}
	return suggestions;
}

static vector<AutoCompleteCandidate> SuggestSchemaName(ClientContext &context, const AutoCompleteFilter &filter) {
	vector<AutoCompleteCandidate> suggestions;
	if (!filter.catalog_name.empty()) {
		auto db = DatabaseManager::Get(context).GetDatabase(context, filter.catalog_name);
		if (!db) {
			return suggestions;
		}
		auto schemas = Catalog::GetSchemas(context, filter.catalog_name);
		for (auto &schema_ref : schemas) {
			auto &entry = schema_ref.get();
			AutoCompleteCandidate candidate(entry.name, SuggestionState::SUGGEST_SCHEMA_NAME, 0);
			candidate.extra_char = '.';
			suggestions.push_back(std::move(candidate));
		}
		return suggestions;
	}
	auto all_entries = GetAllSchemas(context);
	for (auto &entry_ref : all_entries) {
		auto &entry = entry_ref.get();
		AutoCompleteCandidate candidate(entry.name, SuggestionState::SUGGEST_SCHEMA_NAME, 0);
		candidate.extra_char = '.';
		suggestions.push_back(std::move(candidate));
	}
	return suggestions;
}

static vector<AutoCompleteCandidate> SuggestTableName(ClientContext &context, const AutoCompleteFilter &filter) {
	vector<AutoCompleteCandidate> suggestions;
	if (!filter.schema_name.empty()) {
		const string catalog_arg = filter.catalog_name.empty() ? string() : filter.catalog_name;
		optional_ptr<SchemaCatalogEntry> schema_entry =
		    Catalog::GetSchema(context, catalog_arg, filter.schema_name, OnEntryNotFound::RETURN_NULL);
		if (!schema_entry) {
			return suggestions;
		}
		schema_entry->Scan(context, CatalogType::TABLE_ENTRY, [&](CatalogEntry &entry) {
			int32_t bonus = (entry.internal || entry.type == CatalogType::TABLE_FUNCTION_ENTRY) ? 0 : 1;
			suggestions.emplace_back(entry.name, SuggestionState::SUGGEST_TABLE_NAME, bonus);
		});
		schema_entry->Scan(context, CatalogType::TABLE_FUNCTION_ENTRY, [&](CatalogEntry &entry) {
			suggestions.emplace_back(entry.name, SuggestionState::SUGGEST_TABLE_NAME, 0);
		});
		return suggestions;
	}
	auto all_entries = GetAllTables(context, true);
	const auto &search_path = *ClientData::Get(context).catalog_search_path;
	for (auto &entry_ref : all_entries) {
		auto &entry = entry_ref.get();
		auto &standard_entry = entry.Cast<StandardEntry>();
		if (!search_path.SchemaInSearchPath(context, standard_entry.catalog.GetName(), standard_entry.schema.name)) {
			continue;
		}
		int32_t bonus = (entry.internal || entry.type == CatalogType::TABLE_FUNCTION_ENTRY) ? 0 : 1;
		suggestions.emplace_back(entry.name, SuggestionState::SUGGEST_TABLE_NAME, bonus);
	}
	return suggestions;
}

static vector<AutoCompleteCandidate> SuggestType(ClientContext &context) {
	vector<AutoCompleteCandidate> suggestions;
	auto all_entries = GetAllTypes(context);
	for (auto &entry_ref : all_entries) {
		auto &entry = entry_ref.get();
		if (entry.name.empty()) {
			continue;
		}
		// prioritize user-defined types
		int32_t bonus = (entry.internal) ? 0 : 1;
		suggestions.emplace_back(entry.name, SuggestionState::SUGGEST_TYPE_NAME, bonus, CandidateType::KEYWORD);
	}
	return suggestions;
}
static void AppendColumnsFromEntry(CatalogEntry &entry, vector<AutoCompleteCandidate> &suggestions) {
	if (entry.type == CatalogType::TABLE_ENTRY) {
		auto &table = entry.Cast<TableCatalogEntry>();
		int32_t bonus = entry.internal ? 0 : 3;
		for (auto &col : table.GetColumns().Logical()) {
			auto col_name = col.GetName();
			if (col_name.empty()) {
				continue;
			}
			suggestions.emplace_back(std::move(col_name), SuggestionState::SUGGEST_COLUMN_NAME, bonus);
		}
	} else if (entry.type == CatalogType::VIEW_ENTRY) {
		auto &view = entry.Cast<ViewCatalogEntry>();
		int32_t bonus = entry.internal ? 0 : 3;
		auto column_info = view.GetColumnInfo();
		if (column_info) {
			for (idx_t n = 0; n < column_info->names.size(); n++) {
				auto &name = n < view.aliases.size() ? view.aliases[n] : column_info->names[n];
				suggestions.emplace_back(name, SuggestionState::SUGGEST_COLUMN_NAME, bonus);
			}
		} else {
			for (auto &col : view.aliases) {
				suggestions.emplace_back(col, SuggestionState::SUGGEST_COLUMN_NAME, bonus);
			}
		}
	} else {
		if (entry.name.empty() || StringUtil::CharacterIsOperator(entry.name[0])) {
			return;
		}
		int32_t bonus = entry.internal ? 0 : 2;
		suggestions.emplace_back(entry.name, SuggestionState::SUGGEST_COLUMN_NAME, bonus);
	}
}

static optional_ptr<CatalogEntry> GetCatalogTableOrView(ClientContext &context, const string &catalog_name,
                                                        const string &schema_name, const string &object_name) {
	EntryLookupInfo table_lookup(CatalogType::TABLE_ENTRY, object_name);
	auto entry = Catalog::GetEntry(context, catalog_name, schema_name, table_lookup, OnEntryNotFound::RETURN_NULL);
	if (entry) {
		return entry;
	}
	EntryLookupInfo view_lookup(CatalogType::VIEW_ENTRY, object_name);
	return Catalog::GetEntry(context, catalog_name, schema_name, view_lookup, OnEntryNotFound::RETURN_NULL);
}

static optional_ptr<CatalogEntry> ResolveQualifiedVisibleTable(ClientContext &context, const QualifiedName &qn) {
	if (qn.name.empty()) {
		return nullptr;
	}
	if (!qn.schema.empty()) {
		const string catalog_arg = qn.catalog.empty() ? string() : qn.catalog;
		return GetCatalogTableOrView(context, catalog_arg, qn.schema, qn.name);
	}
	for (auto &entry_ref : GetAllTables(context, false)) {
		auto &entry = entry_ref.get();
		if (entry.type != CatalogType::TABLE_ENTRY && entry.type != CatalogType::VIEW_ENTRY) {
			continue;
		}
		if (StringUtil::CIEquals(entry.name, qn.name)) {
			return optional_ptr<CatalogEntry>(&entry);
		}
	}
	return nullptr;
}

static vector<AutoCompleteCandidate> SuggestColumnNameUnfiltered(ClientContext &context) {
	vector<AutoCompleteCandidate> suggestions;
	auto all_entries = GetAllTables(context, false);
	for (auto &entry_ref : all_entries) {
		AppendColumnsFromEntry(entry_ref.get(), suggestions);
	}
	return suggestions;
}

static void AppendSynthesizedColumns(const VisibleTable &vt, vector<AutoCompleteCandidate> &out) {
	for (auto &col : vt.synthesized_columns) {
		out.emplace_back(col, SuggestionState::SUGGEST_COLUMN_NAME, 0);
	}
}

static vector<AutoCompleteCandidate> SuggestColumnName(ClientContext &context, const AutoCompleteFilter &filter) {
	if (!filter.table_ref.empty()) {
		// Step 1: alias match (subquery/CTE or catalog-backed BaseTableRef alias)
		for (auto &vt : filter.visible_tables) {
			if (!vt.alias.empty() && StringUtil::CIEquals(vt.alias, filter.table_ref)) {
				if (!vt.catalog_backed) {
					// Synthesized columns (subquery or CTE).
					vector<AutoCompleteCandidate> suggestions;
					AppendSynthesizedColumns(vt, suggestions);
					if (!suggestions.empty()) {
						return suggestions;
					}
					// synthesized_columns empty — subquery used SELECT *; fall back to
					// catalog lookups on the tables the star expands from.
					for (auto &src : vt.star_sources) {
						auto entry = ResolveQualifiedVisibleTable(context, src);
						if (entry) {
							AppendColumnsFromEntry(*entry, suggestions);
						}
					}
					return suggestions;
				}
				auto entry = ResolveQualifiedVisibleTable(context, vt.name);
				if (entry) {
					vector<AutoCompleteCandidate> suggestions;
					AppendColumnsFromEntry(*entry, suggestions);
					return suggestions;
				}
			}
		}
		// Step 2: unaliased base-table name match
		for (auto &vt : filter.visible_tables) {
			if (StringUtil::CIEquals(vt.name.name, filter.table_ref)) {
				if (!filter.catalog_name.empty() && !StringUtil::CIEquals(vt.name.catalog, filter.catalog_name)) {
					continue;
				}
				if (!filter.schema_name.empty() && !StringUtil::CIEquals(vt.name.schema, filter.schema_name)) {
					continue;
				}
				auto entry = ResolveQualifiedVisibleTable(context, vt.name);
				if (entry) {
					vector<AutoCompleteCandidate> suggestions;
					AppendColumnsFromEntry(*entry, suggestions);
					return suggestions;
				}
			}
		}
		// Step 3: schema.table catalog lookup
		if (!filter.schema_name.empty()) {
			const string catalog_arg = filter.catalog_name.empty() ? string() : filter.catalog_name;
			auto entry = GetCatalogTableOrView(context, catalog_arg, filter.schema_name, filter.table_ref);
			if (entry) {
				vector<AutoCompleteCandidate> suggestions;
				AppendColumnsFromEntry(*entry, suggestions);
				return suggestions;
			}
		}
		return {};
	}
	if (!filter.visible_tables.empty()) {
		case_insensitive_set_t seen;
		vector<AutoCompleteCandidate> suggestions;
		for (auto &vt : filter.visible_tables) {
			if (!vt.catalog_backed) {
				for (auto &col : vt.synthesized_columns) {
					if (seen.insert(col).second) {
						suggestions.emplace_back(col, SuggestionState::SUGGEST_COLUMN_NAME, 0);
					}
				}
				// Also expand star_sources (SELECT * subqueries) for unqualified completion.
				for (auto &src : vt.star_sources) {
					auto entry = ResolveQualifiedVisibleTable(context, src);
					if (!entry) {
						continue;
					}
					vector<AutoCompleteCandidate> chunk;
					AppendColumnsFromEntry(*entry, chunk);
					for (auto &cand : chunk) {
						if (seen.insert(cand.candidate).second) {
							suggestions.push_back(std::move(cand));
						}
					}
				}
				continue;
			}
			auto entry = ResolveQualifiedVisibleTable(context, vt.name);
			if (!entry) {
				continue;
			}
			vector<AutoCompleteCandidate> chunk;
			AppendColumnsFromEntry(*entry, chunk);
			for (auto &cand : chunk) {
				if (seen.insert(cand.candidate).second) {
					suggestions.push_back(std::move(cand));
				}
			}
		}
		return suggestions;
	}
	return SuggestColumnNameUnfiltered(context);
}

static bool KnownExtension(const string &fname) {
	vector<string> known_extensions {".parquet", ".csv", ".tsv", ".csv.gz", ".tsv.gz", ".tbl"};
	for (auto &ext : known_extensions) {
		if (StringUtil::EndsWith(fname, ext)) {
			return true;
		}
	}
	return false;
}

static vector<AutoCompleteCandidate> SuggestPragmaName(ClientContext &context) {
	vector<AutoCompleteCandidate> suggestions;
	auto all_pragmas = Catalog::GetAllEntries(context, CatalogType::PRAGMA_FUNCTION_ENTRY);
	for (const auto &pragma : all_pragmas) {
		AutoCompleteCandidate candidate(pragma.get().name, SuggestionState::SUGGEST_PRAGMA_NAME, 0);
		suggestions.push_back(std::move(candidate));
	}
	return suggestions;
}

static vector<AutoCompleteCandidate> SuggestSettingName(ClientContext &context) {
	auto &db_config = DBConfig::GetConfig(context);
	const auto &options = db_config.GetOptions();
	vector<AutoCompleteCandidate> suggestions;
	for (const auto &option : options) {
		AutoCompleteCandidate candidate(option.name, SuggestionState::SUGGEST_SETTING_NAME, 0);
		suggestions.push_back(std::move(candidate));
	}
	const auto &option_aliases = db_config.GetAliases();
	for (const auto &option_alias : option_aliases) {
		AutoCompleteCandidate candidate(option_alias.alias, SuggestionState::SUGGEST_SETTING_NAME, 0);
		suggestions.push_back(std::move(candidate));
	}
	for (auto &entry : db_config.GetExtensionSettings()) {
		AutoCompleteCandidate candidate(entry.first, SuggestionState::SUGGEST_SETTING_NAME, 0);
		suggestions.push_back(std::move(candidate));
	}
	return suggestions;
}

static vector<AutoCompleteCandidate> SuggestScalarFunctionName(ClientContext &context) {
	vector<AutoCompleteCandidate> suggestions;
	auto scalar_functions = Catalog::GetAllEntries(context, CatalogType::SCALAR_FUNCTION_ENTRY);
	for (const auto &scalar_function : scalar_functions) {
		AutoCompleteCandidate candidate(scalar_function.get().name, SuggestionState::SUGGEST_SCALAR_FUNCTION_NAME, 0);
		suggestions.push_back(std::move(candidate));
	}

	return suggestions;
}

static vector<AutoCompleteCandidate> SuggestTableFunctionName(ClientContext &context) {
	vector<AutoCompleteCandidate> suggestions;
	auto table_functions = Catalog::GetAllEntries(context, CatalogType::TABLE_FUNCTION_ENTRY);
	for (const auto &table_function : table_functions) {
		AutoCompleteCandidate candidate(table_function.get().name, SuggestionState::SUGGEST_TABLE_FUNCTION_NAME, 0);
		suggestions.push_back(std::move(candidate));
	}

	return suggestions;
}

static vector<AutoCompleteCandidate> SuggestFileName(ClientContext &context, string &prefix, idx_t &last_pos) {
	vector<AutoCompleteCandidate> result;
	if (!Settings::Get<EnableExternalAccessSetting>(context)) {
		// if enable_external_access is disabled we don't search the file system
		return result;
	}
	auto &fs = FileSystem::GetFileSystem(context);
	string search_dir;
	auto is_path_absolute = fs.IsPathAbsolute(prefix);
	last_pos += prefix.size();
	for (idx_t i = prefix.size(); i > 0; i--, last_pos--) {
		if (prefix[i - 1] == '/' || prefix[i - 1] == '\\') {
			search_dir = prefix.substr(0, i - 1);
			prefix = prefix.substr(i);
			break;
		}
	}
	if (search_dir.empty()) {
		search_dir = is_path_absolute ? "/" : ".";
	} else {
		search_dir = fs.ExpandPath(search_dir);
	}
	fs.ListFiles(search_dir, [&](const string &fname, bool is_dir) {
		string suggestion;
		char extra_char;
		if (is_dir) {
			extra_char = fs.PathSeparator(fname)[0];
		} else {
			extra_char = '\'';
		}
		int score = 0;
		if (is_dir && fname[0] != '.') {
			score = 2;
		}
		if (KnownExtension(fname)) {
			score = 1;
		}
		auto state = is_dir ? SuggestionState::SUGGEST_DIRECTORY : SuggestionState::SUGGEST_FILE_NAME;
		result.emplace_back(fname, state, score);
		result.back().extra_char = extra_char;
		result.back().candidate_type = CandidateType::LITERAL;
	});
	return result;
}

// The following functions have been moved to autocomplete_core.cpp:
// - UnicodeSpace struct
// - ReplaceUnicodeSpaces
// - IsValidDollarQuotedStringTagFirstChar
// - IsValidDollarQuotedStringTagSubsequentChar
// - StripUnicodeSpaces
// - AutoCompleteTokenizer class
// - GenerateAutoCompleteSuggestions

// Forward declaration for StripUnicodeSpaces (defined in autocomplete_core.cpp)
bool StripUnicodeSpaces(const string &query_str, string &new_query);

// ClientContext-based provider — wraps existing Suggest* functions for in-process use.
class ClientContextCatalogProvider : public AutoCompleteCatalogProvider {
public:
	explicit ClientContextCatalogProvider(ClientContext &context) : context(context) {
	}
	vector<AutoCompleteCandidate> SuggestCatalogName() override {
		return ::duckdb::SuggestCatalogName(context);
	}
	vector<AutoCompleteCandidate> SuggestSchemaName(const AutoCompleteFilter &filter) override {
		return ::duckdb::SuggestSchemaName(context, filter);
	}
	vector<AutoCompleteCandidate> SuggestTableName(const AutoCompleteFilter &filter) override {
		return ::duckdb::SuggestTableName(context, filter);
	}
	vector<AutoCompleteCandidate> SuggestType() override {
		return ::duckdb::SuggestType(context);
	}
	vector<AutoCompleteCandidate> SuggestColumnName(const AutoCompleteFilter &filter) override {
		return ::duckdb::SuggestColumnName(context, filter);
	}
	vector<AutoCompleteCandidate> SuggestFileName(string &prefix, idx_t &last_pos) override {
		return ::duckdb::SuggestFileName(context, prefix, last_pos);
	}
	vector<AutoCompleteCandidate> SuggestScalarFunctionName() override {
		return ::duckdb::SuggestScalarFunctionName(context);
	}
	vector<AutoCompleteCandidate> SuggestTableFunctionName() override {
		return ::duckdb::SuggestTableFunctionName(context);
	}
	vector<AutoCompleteCandidate> SuggestPragmaName() override {
		return ::duckdb::SuggestPragmaName(context);
	}
	vector<AutoCompleteCandidate> SuggestSettingName() override {
		return ::duckdb::SuggestSettingName(context);
	}
	shared_ptr<PEGMatcher> GetPEGMatcher() override {
		return GetPEGMatcherCache(DBConfig::GetConfig(context)).GetMatcher();
	}

private:
	ClientContext &context;
};

static duckdb::unique_ptr<SQLAutoCompleteFunctionData> GenerateSuggestions(ClientContext &context, const string &sql,
                                                                           AutoCompleteParameters &parameters) {
	ClientContextCatalogProvider provider(context);
	auto result = GenerateAutoCompleteSuggestions(provider, sql, parameters);
	return make_uniq<SQLAutoCompleteFunctionData>(std::move(result));
}

static duckdb::unique_ptr<FunctionData> SQLAutoCompleteBind(ClientContext &context, TableFunctionBindInput &input,
                                                            vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("sql_smart_auto_complete first parameter cannot be NULL");
	}
	AutoCompleteParameters parameters;
	for (auto &param : input.named_parameters) {
		if (param.first == "max_suggestion_count") {
			parameters.max_suggestion_count = UBigIntValue::Get(param.second);
		} else if (param.first == "max_file_suggestion_count") {
			parameters.max_file_suggestion_count = UBigIntValue::Get(param.second);
		} else if (param.first == "max_exact_suggestion_count") {
			parameters.max_exact_suggestion_count = UBigIntValue::Get(param.second);
		} else {
			throw InternalException("Unsupported parameter for SQL auto complete");
		}
	}

	names.emplace_back("suggestion");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("suggestion_start");
	return_types.emplace_back(LogicalType::INTEGER);

	names.emplace_back("suggestion_type");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("suggestion_score");
	return_types.emplace_back(LogicalType::UBIGINT);

	names.emplace_back("extra_char");
	return_types.emplace_back(LogicalType::VARCHAR);

	return GenerateSuggestions(context, StringValue::Get(input.inputs[0]), parameters);
}

unique_ptr<GlobalTableFunctionState> SQLAutoCompleteInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<SQLAutoCompleteData>();
}

void SQLAutoCompleteFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<SQLAutoCompleteFunctionData>();
	auto &data = data_p.global_state->Cast<SQLAutoCompleteData>();
	if (data.offset >= bind_data.suggestions.size()) {
		// finished returning values
		return;
	}
	// start returning values
	// either fill up the chunk or return all the remaining columns
	idx_t count = 0;
	while (data.offset < bind_data.suggestions.size() && count < STANDARD_VECTOR_SIZE) {
		auto &entry = bind_data.suggestions[data.offset++];

		// suggestion, VARCHAR
		output.SetValue(0, count, Value(entry.text));

		// suggestion_start, INTEGER
		output.SetValue(1, count, Value::INTEGER(NumericCast<int32_t>(entry.pos)));

		// suggestion_type, VARCHAR
		output.SetValue(2, count, Value(entry.type));

		// suggestion-score, VARCHAR
		output.SetValue(3, count, Value::UBIGINT(entry.score));

		// extra_char, VARCHAR
		output.SetValue(4, count, entry.extra_char == '\0' ? Value() : Value(string(1, entry.extra_char)));
		count++;
	}
	output.SetCardinality(count);
}

class ParserTokenizer : public BaseTokenizer {
public:
	explicit ParserTokenizer(const string &sql, vector<MatcherToken> &tokens) : BaseTokenizer(sql, tokens) {
	}
	void OnStatementEnd(idx_t pos) override {
		statements.push_back(std::move(tokens));
		tokens.clear();
	}
	void OnLastToken(TokenizeState state, string last_word, idx_t last_pos) override {
		if (last_word.empty()) {
			return;
		}
		tokens.emplace_back(std::move(last_word), last_pos);
	}

	vector<vector<MatcherToken>> statements;
};

class PEGParserExtension : public ParserExtension {
public:
	PEGParserExtension() {
		parser_override = PEGParser;
		parser_info = make_shared_ptr<PEGMatcherCache>();
	}

	static ParserOverrideResult PEGParser(ParserExtensionInfo *info, const string &query, ParserOptions &options) {
		auto *cache_ptr = dynamic_cast<PEGMatcherCache *>(info);
		if (!cache_ptr) {
			throw InternalException("smart_autocomplete: parser extension info is not PEGMatcherCache");
		}
		auto peg_matcher = cache_ptr->GetMatcher();

		vector<MatcherToken> root_tokens;
		string clean_sql;
		const string &sql_ref = StripUnicodeSpaces(query, clean_sql) ? clean_sql : query;

		ParserTokenizer tokenizer(sql_ref, root_tokens);
		tokenizer.TokenizeInput();
		tokenizer.statements.push_back(std::move(root_tokens));

		try {
			vector<unique_ptr<SQLStatement>> result;
			for (auto &tokenized_statement : tokenizer.statements) {
				if (tokenized_statement.empty()) {
					continue;
				}
				auto statement = PEGTransformWithCachedMatcher(tokenized_statement, options, peg_matcher);
				if (!statement) {
					continue;
				}
				statement->stmt_location = NumericCast<idx_t>(tokenized_statement[0].offset);
				auto last_pos = tokenized_statement[tokenized_statement.size() - 1].offset +
				                tokenized_statement[tokenized_statement.size() - 1].length;
				statement->stmt_length = last_pos - tokenized_statement[0].offset;
				statement->query = query;
				result.push_back(std::move(statement));
			}
			if (!result.empty()) {
				auto &last_statement = result.back();
				last_statement->stmt_length = query.size() - last_statement->stmt_location;
				for (auto &statement : result) {
					statement->query = query.substr(statement->stmt_location, statement->stmt_length);
					statement->stmt_location = 0;
					statement->stmt_length = statement->query.size();
					if (statement->type == StatementType::CREATE_STATEMENT) {
						auto &create = statement->Cast<CreateStatement>();
						create.info->sql = statement->query;
					}
				}
			}
			return ParserOverrideResult(std::move(result));
		} catch (std::exception &e) {
			return ParserOverrideResult(e);
		}
	}
};

static void LoadInternal(ExtensionLoader &loader) {
	TableFunction auto_complete_fun("sql_smart_auto_complete", {LogicalType::VARCHAR}, SQLAutoCompleteFunction,
	                                SQLAutoCompleteBind, SQLAutoCompleteInit);
	auto_complete_fun.named_parameters["max_suggestion_count"] = LogicalType::UBIGINT;
	auto_complete_fun.named_parameters["max_file_suggestion_count"] = LogicalType::UBIGINT;
	auto_complete_fun.named_parameters["max_exact_suggestion_count"] = LogicalType::UBIGINT;
	loader.RegisterFunction(auto_complete_fun);

	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	ParserExtension::Register(config, PEGParserExtension());
}

void SmartAutocompleteExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string SmartAutocompleteExtension::Name() {
	return "smart_autocomplete";
}

std::string SmartAutocompleteExtension::Version() const {
	return DefaultVersion();
}

} // namespace duckdb
extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(smart_autocomplete, loader) {
	LoadInternal(loader);
}
}

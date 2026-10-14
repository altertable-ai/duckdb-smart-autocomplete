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
#include "duckdb/parser/peg/compiled_grammar.hpp"
#include "autocomplete_catalog_provider.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/catalog/catalog_entry/pragma_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/catalog/standard_entry.hpp"
#include "duckdb/catalog/catalog_search_path.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/parser/qualified_name.hpp"

namespace duckdb {
namespace smart_autocomplete {

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

static void AppendTablesFromSchema(ClientContext &context, SchemaCatalogEntry &schema_entry,
                                   vector<AutoCompleteCandidate> &suggestions, int32_t user_table_bonus) {
	schema_entry.Scan(context, CatalogType::TABLE_ENTRY, [&](CatalogEntry &entry) {
		int32_t bonus = (entry.internal || entry.type == CatalogType::TABLE_FUNCTION_ENTRY) ? 0 : user_table_bonus;
		suggestions.emplace_back(entry.name, SuggestionState::SUGGEST_TABLE_NAME, bonus);
	});
	schema_entry.Scan(context, CatalogType::TABLE_FUNCTION_ENTRY, [&](CatalogEntry &entry) {
		suggestions.emplace_back(entry.name, SuggestionState::SUGGEST_TABLE_NAME, 0);
	});
}

//! Schemas the binder searches for `catalog.table` (search path for that catalog, else its default schema).
static vector<Identifier> SchemasForUnqualifiedCatalogTable(ClientContext &context, AttachedDatabase &db) {
	auto schemas = ClientData::Get(context).catalog_search_path->GetSchemasForCatalog(db.GetName());
	if (schemas.empty()) {
		auto default_schema = db.GetCatalog().GetDefaultSchema();
		if (default_schema) {
			schemas.push_back(*default_schema);
		}
	}
	return schemas;
}

static vector<AutoCompleteCandidate> SuggestSchemaName(ClientContext &context, const AutoCompleteFilter &filter) {
	vector<AutoCompleteCandidate> suggestions;
	if (!filter.catalog_name.empty()) {
		auto db = DatabaseManager::Get(context).GetDatabase(context, Identifier(filter.catalog_name));
		if (!db) {
			return suggestions;
		}
		auto schemas = Catalog::GetSchemas(context, filter.catalog_name);
		for (auto &schema_ref : schemas) {
			auto &entry = schema_ref.get();
			// Outrank unfiltered table functions that the grammar also offers after `catalog.`.
			AutoCompleteCandidate candidate(entry.name, SuggestionState::SUGGEST_SCHEMA_NAME, 2);
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
		optional_ptr<SchemaCatalogEntry> schema_entry = Catalog::GetSchema(
		    context, Identifier(filter.catalog_name), Identifier(filter.schema_name), OnEntryNotFound::RETURN_NULL);
		if (schema_entry) {
			AppendTablesFromSchema(context, *schema_entry, suggestions, 1);
			return suggestions;
		}
		// `catalog.table` is parsed as schema.table. If the qualifier is a catalog, suggest tables the binder
		// resolves in that catalog (search-path schemas, otherwise the catalog default schema).
		if (filter.catalog_name.empty()) {
			auto db = DatabaseManager::Get(context).GetDatabase(context, Identifier(filter.schema_name));
			if (db) {
				case_insensitive_set_t seen;
				for (auto &schema_name : SchemasForUnqualifiedCatalogTable(context, *db)) {
					auto default_schema =
					    Catalog::GetSchema(context, db->GetName(), schema_name, OnEntryNotFound::RETURN_NULL);
					if (!default_schema) {
						continue;
					}
					vector<AutoCompleteCandidate> chunk;
					AppendTablesFromSchema(context, *default_schema, chunk, 2);
					for (auto &candidate : chunk) {
						if (seen.insert(candidate.candidate).second) {
							suggestions.push_back(std::move(candidate));
						}
					}
				}
			}
		}
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
		if (entry.name.GetIdentifierName().empty()) {
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
		auto &entry_name = entry.name.GetIdentifierName();
		if (entry_name.empty() || StringUtil::CharacterIsOperator(entry_name[0])) {
			return;
		}
		int32_t bonus = entry.internal ? 0 : 2;
		suggestions.emplace_back(entry.name, SuggestionState::SUGGEST_COLUMN_NAME, bonus);
	}
}

static optional_ptr<CatalogEntry> GetCatalogTableOrView(ClientContext &context, const string &catalog_name,
                                                        const string &schema_name, const string &object_name) {
	QualifiedName qualified_name {Identifier(catalog_name), Identifier(schema_name), Identifier(object_name)};
	EntryLookupInfo table_lookup(CatalogType::TABLE_ENTRY, qualified_name);
	auto entry = Catalog::GetEntry(context, table_lookup, OnEntryNotFound::RETURN_NULL);
	if (entry) {
		return entry;
	}
	EntryLookupInfo view_lookup(CatalogType::VIEW_ENTRY, qualified_name);
	return Catalog::GetEntry(context, view_lookup, OnEntryNotFound::RETURN_NULL);
}

static optional_ptr<CatalogEntry> ResolveQualifiedVisibleTable(ClientContext &context, const TableQualifiedName &qn) {
	if (qn.name.empty()) {
		return nullptr;
	}
	if (!qn.schema.empty()) {
		return GetCatalogTableOrView(context, qn.catalog, qn.schema, qn.name);
	}
	for (auto &entry_ref : GetAllTables(context, false)) {
		auto &entry = entry_ref.get();
		if (entry.type != CatalogType::TABLE_ENTRY && entry.type != CatalogType::VIEW_ENTRY) {
			continue;
		}
		if (StringUtil::CIEquals(entry.name.GetIdentifierName(), qn.name)) {
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
		// Step 3: schema.table catalog lookup. A two-part name may be catalog.table (default schema).
		if (!filter.schema_name.empty()) {
			auto schema_entry = Catalog::GetSchema(context, Identifier(filter.catalog_name),
			                                       Identifier(filter.schema_name), OnEntryNotFound::RETURN_NULL);
			if (schema_entry) {
				auto entry = GetCatalogTableOrView(context, filter.catalog_name, filter.schema_name, filter.table_ref);
				if (entry) {
					vector<AutoCompleteCandidate> suggestions;
					AppendColumnsFromEntry(*entry, suggestions);
					return suggestions;
				}
				return {};
			}
			if (filter.catalog_name.empty()) {
				auto db = DatabaseManager::Get(context).GetDatabase(context, Identifier(filter.schema_name));
				if (db) {
					for (auto &schema_name : SchemasForUnqualifiedCatalogTable(context, *db)) {
						auto entry = GetCatalogTableOrView(context, db->GetName().GetIdentifierName(),
						                                   schema_name.GetIdentifierName(), filter.table_ref);
						if (entry) {
							vector<AutoCompleteCandidate> suggestions;
							AppendColumnsFromEntry(*entry, suggestions);
							return suggestions;
						}
					}
				}
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

// ClientContext-based provider — wraps existing Suggest* functions for in-process use.
class ClientContextCatalogProvider : public AutoCompleteCatalogProvider {
public:
	explicit ClientContextCatalogProvider(ClientContext &context) : context(context) {
	}
	vector<AutoCompleteCandidate> SuggestCatalogName() override {
		return ::duckdb::smart_autocomplete::SuggestCatalogName(context);
	}
	vector<AutoCompleteCandidate> SuggestSchemaName(const AutoCompleteFilter &filter) override {
		return ::duckdb::smart_autocomplete::SuggestSchemaName(context, filter);
	}
	vector<AutoCompleteCandidate> SuggestTableName(const AutoCompleteFilter &filter) override {
		return ::duckdb::smart_autocomplete::SuggestTableName(context, filter);
	}
	vector<AutoCompleteCandidate> SuggestType() override {
		return ::duckdb::smart_autocomplete::SuggestType(context);
	}
	vector<AutoCompleteCandidate> SuggestColumnName(const AutoCompleteFilter &filter) override {
		return ::duckdb::smart_autocomplete::SuggestColumnName(context, filter);
	}
	vector<AutoCompleteCandidate> SuggestFileName(string &prefix, idx_t &last_pos) override {
		return ::duckdb::smart_autocomplete::SuggestFileName(context, prefix, last_pos);
	}
	vector<AutoCompleteCandidate> SuggestScalarFunctionName() override {
		return ::duckdb::smart_autocomplete::SuggestScalarFunctionName(context);
	}
	vector<AutoCompleteCandidate> SuggestTableFunctionName() override {
		return ::duckdb::smart_autocomplete::SuggestTableFunctionName(context);
	}
	vector<AutoCompleteCandidate> SuggestPragmaName() override {
		return ::duckdb::smart_autocomplete::SuggestPragmaName(context);
	}
	vector<AutoCompleteCandidate> SuggestSettingName() override {
		return ::duckdb::smart_autocomplete::SuggestSettingName(context);
	}
	shared_ptr<CompiledGrammar> GetCompiledGrammar() override {
		return CompiledGrammar::Get(context);
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
                                                            vector<LogicalType> &return_types,
                                                            vector<Identifier> &names) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("sql_smart_auto_complete first parameter cannot be NULL");
	}
	AutoCompleteParameters parameters;
	for (auto &param : input.named_parameters) {
		if (param.second.IsNull()) {
			throw BinderException("sql_smart_auto_complete %s cannot be NULL", param.first);
		}
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

	// suggestion, VARCHAR
	auto &suggestion = output.data[0];
	// suggestion_start, INTEGER
	auto &suggestion_start = output.data[1];
	// suggestion_type, VARCHAR
	auto &suggestion_type = output.data[2];
	// suggestion_score, UBIGINT
	auto &suggestion_score = output.data[3];
	// extra_char, VARCHAR
	auto &extra_char = output.data[4];

	while (data.offset < bind_data.suggestions.size() && count < STANDARD_VECTOR_SIZE) {
		auto &entry = bind_data.suggestions[data.offset++];

		suggestion.Append(Value(entry.text));
		suggestion_start.Append(Value::INTEGER(NumericCast<int32_t>(entry.pos)));
		suggestion_type.Append(Value(entry.type));
		suggestion_score.Append(Value::UBIGINT(entry.score));
		extra_char.Append(entry.extra_char == '\0' ? Value() : Value(string(1, entry.extra_char)));
		count++;
	}
}

static void LoadInternal(ExtensionLoader &loader) {
	TableFunction auto_complete_fun("sql_smart_auto_complete",
	                                FunctionSignature().AddPositionalOnly("sql", LogicalType::VARCHAR),
	                                SQLAutoCompleteFunction, SQLAutoCompleteBind, SQLAutoCompleteInit);
	auto_complete_fun.GetSignature().WithTypedKwargs("options", [](TypedKwargs &options) {
		options.Add("max_suggestion_count", LogicalType::UBIGINT)
		    .Add("max_file_suggestion_count", LogicalType::UBIGINT)
		    .Add("max_exact_suggestion_count", LogicalType::UBIGINT);
	});
	loader.RegisterFunction(auto_complete_fun);
}

} // namespace smart_autocomplete

void SmartAutocompleteExtension::Load(ExtensionLoader &loader) {
	smart_autocomplete::LoadInternal(loader);
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
	duckdb::smart_autocomplete::LoadInternal(loader);
}
}

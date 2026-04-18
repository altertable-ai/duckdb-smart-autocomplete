# DuckDB Smart Autocomplete

Out-of-tree [DuckDB](https://duckdb.org) extension that exposes the experimental **PEG-based SQL autocomplete** stack: catalog-aware suggestions (schemas, tables, columns, functions, files, keywords) plus optional helpers for tokenization and SQL formatting.

The implementation is ported from the in-tree `extension/autocomplete` work on [duckdb/duckdb](https://github.com/duckdb/duckdb) so it can ship and iterate without waiting for upstream merge. CI builds against that fork’s `main` branch, which carries the parser and AST headers this code expects.

## Install (from a repository)

Once published to a repository (for example [community extensions](https://duckdb.org/community_extensions/overview)):

```sql
INSTALL smart_autocomplete FROM community;
LOAD smart_autocomplete;
```

For unsigned local builds:

```sql
LOAD '/path/to/smart_autocomplete.duckdb_extension';
```

You may need `allow_unsigned_extensions` / `duckdb -unsigned` depending on how the binary was produced.

## Main API

| Name                                            | Kind           | Purpose                                                                                                                                                                            |
| ----------------------------------------------- | -------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **`sql_smart_auto_complete(sql VARCHAR, ...)`** | Table function | Rows of suggestions with position, type, score, and optional extra character. Named parameters: `max_suggestion_count`, `max_file_suggestion_count`, `max_exact_suggestion_count`. |

This extension registers the same **PEG parser extension** hook as the in-tree `autocomplete` stack so `sql_smart_auto_complete` can resolve the matcher cache.

## Quick try

```sql
LOAD smart_autocomplete;  -- omit if built-in / autoloaded
FROM sql_smart_auto_complete('SELECT * FRO');
```

## Contributing & community extensions

This repo follows the [DuckDB extension template](https://github.com/duckdb/extension-template) layout (`extension_config.cmake`, `extension-ci-tools`, distribution workflow). To propose listing on community extensions, add a descriptor in [duckdb/community-extensions](https://github.com/duckdb/community-extensions) once your CI produces signed artifacts you are happy with.

## License

See [LICENSE](LICENSE). Autocomplete sources retain DuckDB’s licensing lineage from the upstream contribution.

## See also

- [docs/README.md](docs/README.md) — template-oriented notes (submodules, CLion, distribution).
- [docs/UPDATING.md](docs/UPDATING.md) — bumping DuckDB / CI tool versions.

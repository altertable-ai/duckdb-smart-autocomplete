# DuckDB Smart Autocomplete

Out-of-tree [DuckDB](https://duckdb.org) extension that exposes the experimental **PEG-based SQL autocomplete** stack with catalog-aware suggestions (schemas, tables, columns, functions, files, keywords).

The implementation is ported from the in-tree `extension/autocomplete` work on [duckdb/duckdb](https://github.com/duckdb/duckdb) so it can ship and iterate without waiting for upstream merge.

## Main API

| Name                                            | Kind           | Purpose                                                                                                                                                                            |
| ----------------------------------------------- | -------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **`sql_smart_auto_complete(sql VARCHAR, ...)`** | Table function | Rows of suggestions with position, type, score, and optional extra character. Named parameters: `max_suggestion_count`, `max_file_suggestion_count`, `max_exact_suggestion_count`. |

This extension registers the same **PEG parser extension** hook as the in-tree `autocomplete` stack so `sql_smart_auto_complete` can resolve the matcher cache.

## Quick try

```sql
LOAD smart_autocomplete;
FROM sql_smart_auto_complete('SELECT * FRO');
```

## Contributing & community extensions

This repo follows the [DuckDB extension template](https://github.com/duckdb/extension-template) layout (`extension_config.cmake`, `extension-ci-tools`, distribution workflow). To propose listing on community extensions, add a descriptor in [duckdb/community-extensions](https://github.com/duckdb/community-extensions) once your CI produces signed artifacts you are happy with.

## License

See [LICENSE](LICENSE). Autocomplete sources retain DuckDB’s licensing lineage from the upstream contribution.

## See also

- [docs/README.md](docs/README.md) — template-oriented notes (submodules, CLion, distribution).
- [docs/UPDATING.md](docs/UPDATING.md) — bumping DuckDB / CI tool versions.

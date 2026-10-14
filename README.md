# DuckDB Smart Autocomplete

Out-of-tree [DuckDB](https://duckdb.org) extension that layers **smarter, context-aware SQL autocomplete** on top of DuckDB's PEG parser: qualifier-aware suggestions (`schema.table`, `catalog.schema.table`, `alias.column`, `cte.column`) and column suggestions scoped to the tables, subqueries and CTEs actually visible in `FROM` / `WITH`. DuckDB's built-in `sql_auto_complete` instead suggests every column in the catalog and offers nothing after a qualifier such as `myschema.` or `c.`.

Since DuckDB 2.0 the PEG grammar, tokenizer, matcher and transformer live in DuckDB core (`src/parser/peg`); this extension doesn't vendor any of it and only builds on the core headers. See [src/README.md](src/README.md).

## Main API

| Name                                            | Kind           | Purpose                                                                                                                                                                            |
| ----------------------------------------------- | -------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **`sql_smart_auto_complete(sql VARCHAR, ...)`** | Table function | Rows of suggestions with position, type, score, and optional extra character. Named parameters: `max_suggestion_count`, `max_file_suggestion_count`, `max_exact_suggestion_count`. |

The grammar is taken from the connection (`CompiledGrammar::Get`), so grammar extensions loaded by other extensions are honoured. Unlike the 1.x versions, the extension no longer registers a parser override.

## Quick try

```sql
LOAD smart_autocomplete;
FROM sql_smart_auto_complete('SELECT * FRO');
```

## License

See [LICENSE](LICENSE). Autocomplete sources retain DuckDB’s licensing lineage from the upstream contribution.

## See also

- [docs/README.md](docs/README.md) — template-oriented notes (submodules, CLion, distribution).
- [docs/UPDATING.md](docs/UPDATING.md) — bumping DuckDB / CI tool versions.

# Smart autocomplete (`src/`)

This directory is the **out-of-tree** `smart_autocomplete` extension. It layers "smart" behavior (qualified names, parse-tree walks, catalog integration) on top of DuckDB's **PEG autocomplete stack**, which is **not copied here**. Since DuckDB 2.0 the grammar, tokenizer, matcher and transformer live in DuckDB core (`duckdb/src/parser/peg`, headers under `duckdb/parser/peg/`) and are linked into `libduckdb`; this extension only uses them.

Everything specific to this extension lives in the `duckdb::smart_autocomplete` namespace so it does not clash with the core `GenerateAutoCompleteSuggestions` / `AutoCompleteCatalogProvider` symbols.

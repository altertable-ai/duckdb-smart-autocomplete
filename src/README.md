# Smart autocomplete (`src/`)

This directory is the **out-of-tree** `smart_autocomplete` extension. It layers “smart” behavior (qualified names, parse-tree walks, catalog integration) on top of DuckDB’s **PEG autocomplete stack**, which is **not copied here**: grammar, tokenizer, parser, transformer sources, generated grammar bytes, keyword maps, and shared headers (`ast/`, `parser/`, `transformer/` under the autocomplete include tree) all come from the **`duckdb` submodule** at `duckdb/extension/autocomplete/`.

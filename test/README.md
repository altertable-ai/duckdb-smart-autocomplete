# Tests

SQL tests live under `sql/` as [SQLLogic tests](https://duckdb.org/dev/sqllogictest/intro.html). Autocomplete coverage lives alongside other SQL tests in `sql/` (ported from the in-tree DuckDB suite, with `sql_auto_complete` renamed to `sql_smart_auto_complete` and `require smart_autocomplete`).

From the repository root:

```bash
make test
# or
make test_debug
```

Ensure a `data` symlink exists (`ln -sfn duckdb/data data`) so `{DATA_DIR}` resolves for file-suggestion tests on Unix/macOS.

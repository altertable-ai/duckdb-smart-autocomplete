# Built by DuckDB's extension CI; SOURCE_DIR is this repository root.
# Default include path is ${SOURCE_DIR}/src/include (see duckdb_extension_load).
duckdb_extension_load(smart_autocomplete
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)

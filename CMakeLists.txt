cmake_minimum_required(VERSION 3.10)

# Out-of-tree extension id (INSTALL / LOAD / require)
set(TARGET_NAME smart_autocomplete)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

# Smart autocomplete layer in src/. Since DuckDB 2.0 the PEG grammar, tokenizer,
# matcher and transformer live in DuckDB core (src/parser/peg), so this
# extension only compiles its own sources and uses the core headers
# (duckdb/parser/peg/...).
include_directories(src/include)

set(AUTOCOMPLETE_EXTENSION_FILES
    src/autocomplete_extension.cpp src/autocomplete_core.cpp
    src/autocomplete_qualifier.cpp src/parse_tree_walk.cpp)

# build_static_extension / build_loadable_extension must be called from the
# extension root (not from a subdirectory) so the resulting
# smart_autocomplete.duckdb_extension lands in
# build/<cfg>/extension/smart_autocomplete/ rather than
# build/<cfg>/extension/smart_autocomplete/src/.
build_static_extension(${TARGET_NAME} ${AUTOCOMPLETE_EXTENSION_FILES})
set(PARAMETERS "-warnings")
build_loadable_extension(${TARGET_NAME} ${PARAMETERS}
                         ${AUTOCOMPLETE_EXTENSION_FILES})

install(
  TARGETS ${TARGET_NAME}_extension
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

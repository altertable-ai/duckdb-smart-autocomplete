# Smart autocomplete layer in src/; PEG parser/transformer from DuckDB tree
# (submodule).
set(DUCKDB_AUTOCOMPLETE_DIR "${CMAKE_SOURCE_DIR}/extension/autocomplete")

# Patched headers in include/ must shadow upstream (matcher, keyword_helper, …).
include_directories(include)
include_directories(${DUCKDB_AUTOCOMPLETE_DIR}/include)
# Upstream sources use `#include "include/parser/...` (expects the extension
# root on the path).
include_directories(${DUCKDB_AUTOCOMPLETE_DIR})

# Must be set before parser/transformer subdirs: they append
# $<TARGET_OBJECTS:...> via PARENT_SCOPE.
set(AUTOCOMPLETE_EXTENSION_FILES
    autocomplete_extension.cpp
    autocomplete_core.cpp
    autocomplete_qualifier.cpp
    parse_tree_walk.cpp
    matcher.cpp
    keyword_helper.cpp
    ${DUCKDB_AUTOCOMPLETE_DIR}/keyword_map.cpp)

set(_PEG_BUILD_DIR "${CMAKE_BINARY_DIR}/extension/smart_autocomplete_peg")
add_subdirectory(${DUCKDB_AUTOCOMPLETE_DIR}/parser ${_PEG_BUILD_DIR}/parser)
add_subdirectory(${DUCKDB_AUTOCOMPLETE_DIR}/transformer
                 ${_PEG_BUILD_DIR}/transformer)

build_static_extension(${TARGET_NAME} ${AUTOCOMPLETE_EXTENSION_FILES})
set(PARAMETERS "-warnings")
build_loadable_extension(${TARGET_NAME} ${PARAMETERS}
                         ${AUTOCOMPLETE_EXTENSION_FILES})

install(
  TARGETS ${TARGET_NAME}_extension
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

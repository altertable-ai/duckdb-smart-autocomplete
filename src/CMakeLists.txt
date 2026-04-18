# Smart autocomplete layer in src/; PEG parser/transformer from DuckDB tree
# (submodule). CMAKE_SOURCE_DIR is the DuckDB tree when this extension is built
# in-tree; for a standalone cmake from this repo, fall back via CMAKE_CURRENT_LIST_DIR.
if(EXISTS "${CMAKE_SOURCE_DIR}/extension/autocomplete/CMakeLists.txt")
  set(DUCKDB_AUTOCOMPLETE_DIR "${CMAKE_SOURCE_DIR}/extension/autocomplete")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../duckdb/extension/autocomplete/CMakeLists.txt")
  set(DUCKDB_AUTOCOMPLETE_DIR "${CMAKE_CURRENT_LIST_DIR}/../duckdb/extension/autocomplete")
else()
  message(FATAL_ERROR "Could not find duckdb/extension/autocomplete (set DUCKDB_AUTOCOMPLETE_DIR manually)")
endif()

# Extension headers in include/ shadow duckdb/extension/autocomplete/include (matcher,
# keyword_helper, …) so the duckdb submodule stays unmodified.
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
    peg_transform_with_cached_matcher.cpp
    matcher.cpp
    keyword_helper.cpp
    ${DUCKDB_AUTOCOMPLETE_DIR}/tokenizer.cpp
    ${DUCKDB_AUTOCOMPLETE_DIR}/keyword_map.cpp)

set(_PEG_BUILD_DIR "${CMAKE_BINARY_DIR}/extension/smart_autocomplete_peg")
add_subdirectory(${DUCKDB_AUTOCOMPLETE_DIR}/parser ${_PEG_BUILD_DIR}/parser)
add_subdirectory(${DUCKDB_AUTOCOMPLETE_DIR}/transformer
                 ${_PEG_BUILD_DIR}/transformer)

# PEG transformer sources include matcher.hpp; they must see the same shadow
# headers as this extension (TokenType / MatcherToken layout), not the submodule
# copy, or the link unit violates ODR and miscompiles at runtime.
if(TARGET duckdb_peg_transformer)
  target_include_directories(duckdb_peg_transformer BEFORE
                             PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include")
endif()

build_static_extension(${TARGET_NAME} ${AUTOCOMPLETE_EXTENSION_FILES})
set(PARAMETERS "-warnings")
build_loadable_extension(${TARGET_NAME} ${PARAMETERS}
                         ${AUTOCOMPLETE_EXTENSION_FILES})

install(
  TARGETS ${TARGET_NAME}_extension
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

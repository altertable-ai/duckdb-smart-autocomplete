cmake_minimum_required(VERSION 3.10)

# Out-of-tree extension id (INSTALL / LOAD / require)
set(TARGET_NAME smart_autocomplete)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

# Smart autocomplete layer in src/; PEG parser/transformer from DuckDB tree
# (submodule). CMAKE_SOURCE_DIR is the DuckDB tree when this extension is built
# in-tree; for a standalone cmake from this repo, fall back via CMAKE_CURRENT_LIST_DIR.
if(EXISTS "${CMAKE_SOURCE_DIR}/extension/autocomplete/CMakeLists.txt")
  set(DUCKDB_AUTOCOMPLETE_DIR "${CMAKE_SOURCE_DIR}/extension/autocomplete")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/duckdb/extension/autocomplete/CMakeLists.txt")
  set(DUCKDB_AUTOCOMPLETE_DIR "${CMAKE_CURRENT_LIST_DIR}/duckdb/extension/autocomplete")
else()
  message(FATAL_ERROR "Could not find duckdb/extension/autocomplete (set DUCKDB_AUTOCOMPLETE_DIR manually)")
endif()

# Patched headers in src/include/ must shadow upstream
# (matcher, keyword_helper, …).
include_directories(src/include)
include_directories(${DUCKDB_AUTOCOMPLETE_DIR}/include)
# Upstream sources use `#include "include/parser/...` (expects the extension
# root on the path).
include_directories(${DUCKDB_AUTOCOMPLETE_DIR})

# Must be set before parser/transformer subdirs: they append
# $<TARGET_OBJECTS:...> via PARENT_SCOPE.
set(AUTOCOMPLETE_EXTENSION_FILES
    src/autocomplete_extension.cpp
    src/autocomplete_core.cpp
    src/autocomplete_qualifier.cpp
    src/parse_tree_walk.cpp
    src/peg_transform_with_cached_matcher.cpp
    src/matcher.cpp
    src/keyword_helper.cpp
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
                             PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src/include")
endif()

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

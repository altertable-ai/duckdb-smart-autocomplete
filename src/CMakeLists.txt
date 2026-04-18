# PEG-based SQL autocomplete (sources under src/)
include_directories(include)

set(AUTOCOMPLETE_EXTENSION_FILES
    autocomplete_extension.cpp
    autocomplete_core.cpp
    autocomplete_qualifier.cpp
    parse_tree_walk.cpp
    matcher.cpp
    keyword_helper.cpp
    keyword_map.cpp)

add_subdirectory(transformer)
add_subdirectory(parser)

build_static_extension(${TARGET_NAME} ${AUTOCOMPLETE_EXTENSION_FILES})
set(PARAMETERS "-warnings")
build_loadable_extension(${TARGET_NAME} ${PARAMETERS}
                         ${AUTOCOMPLETE_EXTENSION_FILES})

install(
  TARGETS ${TARGET_NAME}_extension
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

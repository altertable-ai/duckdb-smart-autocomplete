cmake_minimum_required(VERSION 3.10)

# Out-of-tree extension id (INSTALL / LOAD / require)
set(TARGET_NAME smart_autocomplete)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

add_subdirectory(src)

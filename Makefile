PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Extension id (matches INSTALL / LOAD / require)
EXT_NAME=smart_autocomplete
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# DuckDB 2.0 publishes compile_commands.json under .cache/clangd (see .clangd).
# A leftover build/clangd cache pins CMAKE_CXX_STANDARD=11, which configure rejects.
clangd:
	cmake $(GENERATOR) $(BUILD_FLAGS) $(EXT_DEBUG_FLAGS) $(VCPKG_MANIFEST_FLAGS) -DCMAKE_BUILD_TYPE=Debug -S $(DUCKDB_SRCDIR) -B .cache/clangd/debug
	cp .cache/clangd/debug/compile_commands.json .cache/clangd/compile_commands.json

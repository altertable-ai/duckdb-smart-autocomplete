PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Extension id (matches INSTALL / LOAD / require)
EXT_NAME=smart_autocomplete
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

clangd:
	mkdir -p build/clangd
	cmake $(GENERATOR) $(BUILD_FLAGS) $(EXT_DEBUG_FLAGS) $(VCPKG_MANIFEST_FLAGS) -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=1 -S $(DUCKDB_SRCDIR) -B build/clangd

# Developer notes

Project documentation for **duckdb-smart-autocomplete** lives in the repository root: [README.md](../README.md).

The sections below are inherited from the [DuckDB extension template](https://github.com/duckdb/extension-template) and still apply (submodules, build layout, CLion, distribution).

---

# DuckDB extension template (reference)

This repository started from the official template so we can develop, test, and distribute a DuckDB extension with the standard `extension-ci-tools` workflow.

## Getting started

Create your own repo from the template, then clone with submodules:

```sh
git clone --recurse-submodules https://github.com/<you>/<your-new-extension-repo>.git
```

## Building

### Managing dependencies

DuckDB extensions can use [vcpkg](https://vcpkg.io/) when listed in `vcpkg.json`. This project keeps an empty dependency list; add packages only when needed.

### Updating submodules

| Submodule            | Repository                                                                                  |
| -------------------- | ------------------------------------------------------------------------------------------- |
| `duckdb`             | Core DuckDB sources (here: [altertable-ai/duckdb](https://github.com/altertable-ai/duckdb)) |
| `extension-ci-tools` | [duckdb/extension-ci-tools](https://github.com/duckdb/extension-ci-tools)                   |

```bash
git submodule update --init --recursive
```

### Build steps

```sh
make
```

Binaries (paths vary slightly by platform):

- `build/release/duckdb`
- `build/release/test/unittest`
- `build/release/extension/<name>/<name>.duckdb_extension`

### Tips for speedy builds

Install [ccache](https://ccache.dev/) and [ninja](https://ninja-build.org/), then:

```sh
GEN=ninja make
```

## Running tests

```sh
make test
```

## CLion

Open `./duckdb/CMakeLists.txt` as the project, then set the project root to this repository (`Tools → CMake → Change Project Root`). See the template’s upstream README for full CLion notes.

## Distributing

- **Community extensions**: see [community extensions documentation](https://duckdb.org/community_extensions/documentation).
- **Artifacts**: the default GitHub Actions workflow uploads extension binaries as workflow artifacts.

## Versioning

Update `.github/workflows/MainDistributionPipeline.yml` (`duckdb_version`, `ci_tools_version`, `override_duckdb_repository`) when you intentionally move to a new DuckDB baseline.

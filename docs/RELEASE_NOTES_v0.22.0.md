# duckdb-salesforce v0.22.0

> **DuckDB v2.0.1 added to the supported matrix.** Minor release (a change to
> the supported-version set is a minor per project policy). Supported set is
> now **{v1.5.6, v2.0.0, v2.0.1}**. No C++ source, SQL surface, function, or
> behavior change from v0.21.0 - v2.0.1 is a bugfix patch on the v2.0 LTS line
> and the existing dual-line v2.x path already covered it.

## What changed

- **DuckDB v2.0.1 added** (released 2026-11-20, "Bugfix Release" on the v2.0
  "cyanoptera" LTS line). Supported set grows from {v1.5.6, v2.0.0} to
  **{v1.5.6, v2.0.0, v2.0.1}**. Added alongside v2.0.0 (not replacing it), so
  users pinned to either v2.0.x patch are covered.
- v2.0.1 needed **no source change**: its extension-facing changes are CI /
  infra / Wasm / C-API-entrypoint only, none touching the C++ storage-extension
  API, and the dual-line `SF_DUCKDB_V2` path already targets the v2.x line.
- **CI matrix grown from 2 to 3 DuckDB refs** in `MainDistributionPipeline.yml`
  (`v1.5.6`, `v2.0.0`, `v2.0.1`) - 3 refs x 3 platforms = 9 platform jobs.
- `scripts/build_matrix.ps1`: default refs updated to the three supported refs,
  and `-DENABLE_UNITTEST_CPP_TESTS=OFF` added to the configure flags. That flag
  disables upstream DuckDB's own C++ API unit tests (`tools/cpp`), which fail to
  compile on the v2.x line under MSVC (`test_cpp_api_catalog.cpp` C2248); they
  are not this extension's tests and the official CI build disables them too.
  This unblocks isolated v2.x local matrix validation.
- **Documentation**: README (supported list + validated matrix table),
  `docs/INSTALL.md`, the Linux/Windows EN guides, and the public
  `docs/ROADMAP.md` now declare the v1.5.6 + v2.0.0 + v2.0.1 matrix.

## Packaging notes

- Release assets are built against **DuckDB v1.5.6**, matching the production
  1.5.x line and the existing release workflow. Unchanged from prior releases.
- DuckDB v2.0.0 / v2.0.1 users build from source against their ref, or install
  from the DuckDB community registry after its baseline and the extension
  descriptor are updated through the separate maintainer-approved process.
- Published v0.20.x / v0.21.0 tags and releases remain untouched.

## Why v0.22.0 (minor)

A change to the supported DuckDB version set is a minor under the project's
loose versioning policy (precedents: v0.15.0 and v0.21.0, both version-set
changes). The runtime code is unchanged from v0.21.0.

## Validation

No `src/`, `test/`, or `CMakeLists.txt` change versus v0.21.0; only the CI
matrix, build-matrix script, `vcpkg.json` version string, and documentation
differ. v2.0.1 was validated as a new ref before release.

**Local Windows/MSVC Release, isolated build against v2.0.1** (`build_matrix.ps1`,
ref `v2.0.1` @ `8d78114e`, `-DENABLE_UNITTEST_CPP_TESTS=OFF`):

| DuckDB | Passed files | Failed | Skipped | Assertions |
|---|---:|---:|---:|---:|
| v2.0.1 (`8d78114e`) | 60 | 0 | 0 | 1693 |

v1.5.6 and v2.0.0 are unchanged from the v0.21.0 cycle (CI run 43268185610,
6/6) and are re-proven by this release's official CI run below.

### Official CI run record

`MainDistributionPipeline.yml` run [45174818027](https://github.com/flozer/duckdb-salesforce/actions/runs/45174818027)
on `main` at `4ac5f08` completed successfully: **9/9 platform jobs passed** (3
DuckDB refs x 3 platforms). The 3 per-ref `Generate matrix` jobs also passed;
the 3 `matrix.duckdb_arch` jobs are expected template placeholders and are
skipped.

| Platform | DuckDB v1.5.6 | DuckDB v2.0.0 | DuckDB v2.0.1 |
|---|---:|---:|---:|
| `linux_amd64` | pass | pass | pass |
| `windows_amd64` | pass | pass | pass |
| `osx_arm64` | pass | pass | pass |

## Gates

- **Community update**: pending explicit C.5 GO. The live community descriptor
  continues to serve v0.20.2 until a separate maintainer-approved submission.

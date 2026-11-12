# duckdb-salesforce v0.21.0

> **Supported DuckDB baseline reduced to v1.5.6 + v2.0.0.** Minor release
> (dropping a supported version is a minor per project policy). No runtime,
> SQL surface, function, or behavior change from v0.20.2 - the binary is
> identical; only the supported-version set, CI matrix, and documentation
> changed.

## What changed

- **Baseline dropped to {v1.5.6, v2.0.0}.** v1.5.4 and v1.5.5 are no longer
  supported, built, or tested as of 2026-11-12 (maintainer decision, scope
  reduction). DuckDB v2.0.0 is now a shipped LTS line ("Cinnamon Teal",
  2026-10-21, supported until at least October 2027) and the production line
  is v1.5.6; maintaining the non-LTS v1.5.4/v1.5.5 only doubled the CI matrix.
  Both passed cleanly through v0.20.2. v1.5.2/v1.5.3 were dropped earlier
  (2026-09-14).
- **CI matrix reduced from 4 to 2 DuckDB refs** in
  `MainDistributionPipeline.yml` (`v1.5.6`, `v2.0.0`), and the quality /
  sanitizer and local build-matrix defaults updated to match.
- **Documentation**: README (supported matrix + "Dropped versions"),
  `docs/INSTALL.md`, the Linux/Windows EN guides, and the public
  `docs/ROADMAP.md` now declare the v1.5.6 + v2.0.0 baseline.
- **`docs/ARCHITECTURE.md`**: the section 13 C++ file tree now marks the
  never-created files (`salesforce_transport_selector`, `observability`,
  `dbt_sources`, `remote_connector_base`) as `NOT SHIPPED` inline, so the tree
  is not misread despite the existing correction note.
- No C++ source, function, setting, supported transport, or write capability
  changed. The only test change is a harness-correctness fix to
  `salesforce_quota_concurrency.test` (session-scope mocks switched to
  `SET GLOBAL` so `concurrentloop` worker threads inherit them; no coverage
  change). The v0.21.0 runtime binary is byte-identical to v0.20.2 - test
  files are not compiled into the loadable extension.

## Packaging notes

- Release assets are built against **DuckDB v1.5.6**, matching the production
  1.5.x line and the existing release workflow.
- DuckDB v2.0.0 users build from source against v2.0.0, or install from the
  DuckDB community registry after its build baseline and the extension
  descriptor are updated through the separate maintainer-approved process.
- Published v0.20.x tags and releases remain untouched.

## Why v0.21.0 (minor)

Dropping a supported DuckDB version is a minor under the project's loose
versioning policy (precedent: v0.15.0, which dropped v1.5.0/v1.5.1). The
runtime code is unchanged from v0.20.2.

## Validation

The v0.21.0 runtime binary is byte-identical to v0.20.2: no `src/` or
`CMakeLists.txt` file changed between the two tags, and the only `test/` change
is the harness-correctness fix noted above, which is not compiled into the
loadable extension. The remaining changes are the supported-version set, the CI
matrix, the build scripts, the `vcpkg.json` version string, and documentation.
The authoritative build/test gate for v0.21.0 is the official CI run recorded
below, which rebuilds the identical binary and runs the full offline SQL suite
against both supported DuckDB refs on all three platforms. No C++ files changed,
so clang-format was not applicable.

### Official CI run record

`MainDistributionPipeline.yml` run
[43268185610](https://github.com/flozer/duckdb-salesforce/actions/runs/43268185610)
on `main` at commit `53950ac`, completed 2026-11-12: **6/6 platform jobs
passed** (both "Generate matrix" setup jobs also green).

| Platform | DuckDB v1.5.6 | DuckDB v2.0.0 |
|---|---:|---:|
| `linux_amd64` | pass | pass |
| `windows_amd64` | pass | pass |
| `osx_arm64` | pass | pass |

The commit under test (`53950ac`) adds the quota-concurrency test-harness fix
(session-scope mocks switched to `SET GLOBAL` so `concurrentloop` worker
threads inherit them); the runtime binary is unchanged from v0.20.2.

## Gates

- **Community update**: pending explicit C.5 GO. The DuckDB community registry
  builds against its own baseline, so a v0.21.0 descriptor update is submitted
  only when the registry's baseline and the maintainer approval allow it. The
  live community descriptor continues to serve v0.20.2 until then.

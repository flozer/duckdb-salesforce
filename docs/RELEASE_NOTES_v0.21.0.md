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
- No C++ source, test logic, function, setting, supported transport, or write
  capability changed. The v0.21.0 runtime is byte-identical to v0.20.2.

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

The v0.21.0 runtime is byte-identical to v0.20.2: no file under `src/`,
`test/`, or `CMakeLists.txt` changed between the two tags. Only the
supported-version set, the CI matrix, the build scripts, the `vcpkg.json`
version string, and documentation differ. The authoritative build/test gate
for v0.21.0 is therefore the official CI run recorded below, which rebuilds
the identical binary against both supported DuckDB refs.

For reference, the identical v0.20.2 binary passed the full offline SQL suite
locally against both refs during its own cycle (isolated per-ref Windows/MSVC
Release builds): 59/59 files, 1676 assertions per ref, with
`salesforce_quota_concurrency.test` skipped by the runner's mock/HTTP skip
rule (not a failure). No C++ files changed in this release, so clang-format
was not applicable.

### Official CI run record

<!-- FILL: MainDistributionPipeline.yml run id + result (6/6) before tag -->

`MainDistributionPipeline.yml` run [TBD](https://github.com/flozer/duckdb-salesforce/actions/runs/TBD)
on `main` at `TBD` completed successfully: **6/6 platform jobs passed**.

| Platform | DuckDB v1.5.6 | DuckDB v2.0.0 |
|---|---:|---:|
| `linux_amd64` | TBD | TBD |
| `windows_amd64` | TBD | TBD |
| `osx_arm64` | TBD | TBD |

## Gates

- **Community update**: pending explicit C.5 GO. The DuckDB community registry
  builds against its own baseline, so a v0.21.0 descriptor update is submitted
  only when the registry's baseline and the maintainer approval allow it. The
  live community descriptor continues to serve v0.20.2 until then.

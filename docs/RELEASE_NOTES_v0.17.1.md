# duckdb-salesforce v0.17.1

> **Own-repo release (compat-only, no `src/` changes).** Official support
> declaration for **DuckDB v1.5.6**: the CI matrix grows to nine jobs
> (v1.5.4/v1.5.5/v1.5.6 × linux_amd64/windows_amd64/osx_arm64) and the
> release-asset packaging baseline moves to v1.5.6. Precedent: v0.14.2 (the
> v1.5.5 compat release). Patch bump — no functional change.

## What changed

- `MainDistributionPipeline.yml` matrix: `["v1.5.4", "v1.5.5"]` →
  `["v1.5.4", "v1.5.5", "v1.5.6"]` (6 → 9 platform jobs).
- `release-assets.yml`: `DUCKDB_VERSION` `v1.5.5` → `v1.5.6` (the version the
  published Linux/Windows assets are built and LOAD-smoked against).
- README / `docs/INSTALL.md` / `docs/ROADMAP.md` updated to the
  three-version supported surface. v1.5.2/v1.5.3 remain dropped (2026-09-14).

## Evidence base for the v1.5.6 declaration

- Validated locally since 2026-09-28: build + full offline suite green with
  numbers identical to v1.5.4/v1.5.5 (most recent local 3-ref matrix below).
- `v1.5.6` (`069cc9f9`) is the development submodule pin since 2026-09-29.
- The DuckDB community catalog's own CI has been building this extension
  green against v1.5.6 since its 2026-09-28 baseline bump (#2824) — visible
  on update PR #2850's checks (linux + windows green).

## Validation

### Local 3-ref matrix run record

`scripts/build_matrix.ps1 -Tags v1.5.4,v1.5.5,v1.5.6 -Baseline v1.5.4
-VcpkgToolchain C:/Users/fernando.souza/vcpkg/scripts/buildsystems/vcpkg.cmake`
(2026-09-29, MSVC 19.44.35227, x64, Release, from-scratch per-ref isolated
build dirs on the P4 tree; committed submodule pin untouched):

| DuckDB | Build | Passed | Failed | Skipped | Assertions | Status |
|---|---|---:|---:|---:|---:|---|
| v1.5.4 (`08e34c44`) | ok | 51 | 0 | 1 | 1514 | PASS-WITH-SKIPS |
| v1.5.5 (`d8cdaa33`) | ok | 51 | 0 | 1 | 1514 | PASS-WITH-SKIPS |
| v1.5.6 (`069cc9f9`) | ok | 51 | 0 | 1 | 1514 | PASS-WITH-SKIPS |

The single skip is `salesforce_quota_concurrency.test`'s documented
conditional skip.

### Official CI run record

`MainDistributionPipeline.yml` run
[36634470965](https://github.com/flozer/duckdb-salesforce/actions/runs/36634470965)
(2026-09-29, `workflow_dispatch` on this branch @ `1c48d64`; first 9-job
matrix) — **9/9 green**:

| DuckDB | linux_amd64 | windows_amd64 | osx_arm64 |
|---|---|---|---|
| v1.5.4 | Pass | Pass | Pass |
| v1.5.5 | Pass | Pass | Pass |
| v1.5.6 | Pass | Pass | Pass |

## Gates

- Community update: **not submitted and not scheduled by this release** — per
  the maintainer's standing rule, any `duckdb/community-extensions` action
  requires a separate, explicit OK.

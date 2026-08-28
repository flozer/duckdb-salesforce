# duckdb-salesforce v0.14.2

> **Own-repo release.** Compatibility-only release: DuckDB v1.5.5 support. No
> community-extensions operation is part of this release; community baseline
> stays `v0.14.1`, unchanged.

Own-repo release on top of `v0.14.1` (`dec897d`). Scope: DuckDB **v1.5.5**
compatibility. **No functional changes to `src/`** — v1.5.5 built and tested
clean against the extension's existing code, unmodified.

## What changed

- DuckDB pin (this release's build target) moved from v1.5.3 to **v1.5.5**.
- `scripts/build_matrix.ps1` generalized into a portable local test harness
  (see `compat/duckdb-v1.5.5-readiness`, merged in
  [#54](https://github.com/flozer/duckdb-salesforce/pull/54)).
- `.github/workflows/release-assets.yml` restructured so a GitHub Release can
  never be partially published: Linux and Windows now only build, test, and
  upload a *temporary* workflow artifact each; a separate `publish` job,
  gated on both platform jobs succeeding, is the sole place a release is
  created or an asset is uploaded. SHA-256 checksums are generated for every
  published asset.
- `.github/workflows/DuckDBMainCanary.yml` added: a manual-only,
  non-gating canary against `duckdb/main`, informational only.

## Validation

Official `MainDistributionPipeline.yml` matrix, run
[33175570085](https://github.com/flozer/duckdb-salesforce/actions/runs/33175570085)
(2026-08-28):

| DuckDB | linux_amd64 | windows_amd64 | osx_arm64 |
|---|---|---|---|
| v1.5.4 | Pass | Pass | Pass |
| v1.5.5 | Pass | Pass | Pass |

**v1.5.5 is green on all three platforms the official matrix covers** (Linux,
Windows, macOS). No `src/` change was required to build or pass the full
offline test suite against v1.5.5.

A separate, pre-existing legacy-compatibility issue was also observed on this
same CI run: v1.5.2 and v1.5.3 currently fail on `windows_amd64` while
compiling DuckDB's vendored `fmt` header against the current GitHub Windows
toolchain. The exact ownership — legacy DuckDB configuration, current MSVC
behavior, CI tooling, or their interaction — has not yet been isolated. It is
tracked as a separate issue and does not affect this release, which targets
v1.5.5 only.

A non-gating canary against `duckdb/main` (informational, not a supported
version) confirmed real, expected API drift ahead of any future DuckDB v2.0 —
see
[docs/superpowers/specs/2026-08-27-duckdb-v2-c-api-readiness.md](superpowers/specs/2026-08-27-duckdb-v2-c-api-readiness.md).
**No DuckDB v2.0 compatibility is claimed by this release.**

## Published assets

This release's own-repo build assets are produced only for the platforms
`release-assets.yml` builds — **Linux x64 and Windows x64**. There is no
macOS asset from this workflow; the official CI matrix above already proves
the extension builds and passes on `osx_arm64` via
`MainDistributionPipeline.yml`, independent of this release's own asset
pipeline.

- `duckdb-salesforce-0.14.2-linux-x64.tar.gz`
- `duckdb-salesforce-0.14.2-windows-x64.zip`
- `SHA256SUMS.txt`

Built against DuckDB **v1.5.5**. Version-locked: use a matching DuckDB
v1.5.5 (or another ABI-compatible v1.5.x) CLI/build to `LOAD` this asset.

## Gates

- No community-extensions operation. `docs/community/description.yml` is
  unchanged. Community baseline stays `v0.14.1`.
- No `duckdb/community-extensions` PR, merge, or descriptor update.

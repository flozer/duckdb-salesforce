# duckdb-salesforce v0.15.0

> **Own-repo release.** Aggregates cycles #001-#006 on top of `v0.14.2`
> (`4b10437`): CI/tooling hygiene, a **user-visible drop of DuckDB
> v1.5.2/v1.5.3 support**, code formatting, informational sanitizer CI, and
> documentation corrections. **No SOQL/scan/transport/quota behavior
> change** — every change here is CI, build config, formatting, or docs.
> Minor bump (not patch) because of the supported-version-matrix reduction —
> see "Why v0.15.0, not v0.14.3" below.

Own-repo release on top of `v0.14.2` (`4b10437`). Scope: cycles #001-#006,
five PM->DEV cycles of CI hygiene, a compatibility-matrix reduction, and
quality/documentation work, none of which changed `src/` behavior except a
pure reformat (no logic change).

## What changed

**#001/#002 — CI hygiene**
- `extension-ci-tools` pin aligned between `.gitmodules` and
  `MainDistributionPipeline.yml`'s `ci_tools_version` (both now
  `64aec33f`).
- `SECURITY.md` corrected (was still describing a pre-v0.9 unpublished
  state).

**#003 — Dropped DuckDB v1.5.2/v1.5.3 support** (user-visible surface
change)
- Official supported matrix reduced to **DuckDB v1.5.4 and v1.5.5 only**.
  v1.5.2/v1.5.3 had a pre-existing, unresolved Windows/MSVC build failure in
  DuckDB's vendored `fmt` header (R-003); rather than chase that root cause,
  the project stopped announcing support for those versions. Not a code
  fix — a scope reduction.
- README, `docs/INSTALL.md`, install guides (EN/PT), and release-asset
  scripts updated to match.

**#004 — Quality sprint (concurrency test, `.clang-format`, `CHANGELOG.md`,
architecture docs)**
- `test/sql/salesforce_quota_concurrency.test`: real concurrent-access test
  for the quota governor (8 threads via SQLLogicTest `concurrentloop`),
  passing functionally on Release across all 3 platforms.
- `.clang-format` added (copied from the `duckdb` submodule, unmodified) and
  two informational, non-blocking sanitizer CI jobs (ASan+UBSan, TSan) —
  see R-010 below for why those two jobs stayed link-failing at the time.
- `CHANGELOG.md` added, aggregating all prior release notes.
- `docs/ARCHITECTURE.md` §15 roadmap section rewritten to match delivered
  reality.

**#005 — R-010 root-cause investigation (no code change)**
- Diagnosed the Debug/Linux link failure discovered in #004
  (`multiple definition of 'duckdb::LogicalType::VARCHAR'`). Root cause:
  **not sanitizer-specific** — a `DISABLE_SANITIZER=1` control build failed
  identically. The real cause is GCC's `STB_GNU_UNIQUE` symbol binding
  colliding under an unoptimized (`-O0`, Debug-only) build combined with
  this extension's `EXTENSION_STATIC_BUILD=1` static-embedding of
  `libduckdb_static.a`, rooted in a vendored DuckDB header
  (`duckdb/src/include/duckdb/common/types.hpp:414`). Confirmed to occur in
  the real distributable extension target, but **only in Debug** — Release
  (what ships) and the real `LOAD` path are unaffected. Closed as
  structural/upstream, no fix in this repo's scope; no code changed.

**#006 — `.clang-format` applied, Appendix A/B corrected, this release
prepared**
- All 38/42 non-conformant files in `src/` reformatted with the
  `.clang-format` from #004 (clang-format 11.0.1, matching
  `duckdb/scripts/format.py`'s own pin). **No behavior change** — formatting
  only, verified by an unchanged assertion count (see Validation).
- `docs/ARCHITECTURE.md` Appendix A: REST/Bulk crossover corrected from a
  stale 10,000 to the shipped `sf_auto_bulk_threshold` default of
  **50,000** (`src/salesforce_extension.cpp:206-209`); the GraphQL/UI API
  transport described there was never implemented and is now explicitly
  marked historical/not-implemented rather than deleted.
- `docs/ARCHITECTURE.md` Appendix B ("Vault Mode"): marked as a direction
  that was not adopted, linking to `docs/ROADMAP.md`'s current
  materialization position.
- This release (`docs/RELEASE_NOTES_v0.15.0.md`, `vcpkg.json` bump, a
  community-submission draft and plan) prepared. **No tag, no GitHub
  Release, no community-extensions action taken** — see Gates below.

## Why v0.15.0, not v0.14.3

The project does not declare strict SemVer (see `CHANGELOG.md`'s own
note), but the drop of DuckDB v1.5.2/v1.5.3 support (#003) is a
**user-visible reduction of the supported-version surface**, not an
internal chore — a user relying on v1.5.2/v1.5.3 compatibility loses
official support in this release. That alone justifies a minor bump over a
patch. Everything else in #001-#006 (CI pin alignment, formatting,
informational sanitizer CI, docs corrections) is internal/non-functional
and would not by itself have forced a minor bump, but travels in the same
release.

## Validation

Official `MainDistributionPipeline.yml` matrix, run
[34971612922](https://github.com/flozer/duckdb-salesforce/actions/runs/34971612922)
(2026-09-15, run against the `chore/pm006-format-appendix-community` branch
post-reformat, pre-tag):

| DuckDB | linux_amd64 | windows_amd64 | osx_arm64 |
|---|---|---|---|
| v1.5.4 | Pass | Pass | Pass |
| v1.5.5 | Pass | Pass | Pass |

**6/6 green.** Offline mock suite: **1452 assertions, 0 failures, 49 test
cases** (5 live tests gated/skipped, unchanged from the pre-reformat count
recorded in cycle #004) — confirms the `src/` reformat changed no behavior.

`clang-format --dry-run --Werror` over all of `src/`: **0/42 files would
change** (verified in the same CI run that applied the formatting; see
DEV->PM #006 for the full before/after log).

## Published assets

Not yet published — **this release has not been tagged**. Per PM->DEV #006,
this cycle prepares release material only; tagging, `release-assets.yml`,
and the GitHub Release are a separate, explicitly human-authorized step.

## Gates

- No community-extensions operation. `docs/community/description.yml` is
  unchanged. Community baseline stays `v0.14.1`.
- No `duckdb/community-extensions` PR, merge, fork, or descriptor update.
- A review draft (`docs/community/description.v0.15.0.draft.yml`) and a
  submission plan (`docs/community/C6_SUBMISSION_PLAN.md`) were prepared
  for a future update, gated on a separate, explicit maintainer **C.5 GO**.
- No git tag created for this release.

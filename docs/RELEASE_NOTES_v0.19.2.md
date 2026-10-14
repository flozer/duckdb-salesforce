# duckdb-salesforce v0.19.2

> **Own-repo release.** Second feedback cycle from the data-lakehouse
> deployment (response: `docs/feedback/lakehouse-response-2026-10.md`):
> one **documentation correction** (the delete-sync pitfall statement about
> `SystemModstamp` was wrong), one **column addition** to
> `salesforce_deleted_ids()`, one **accepted ATTACH option**, one
> **plan-time validation**, and one **new tuning setting**. Patch bump —
> no behavior removed, no supported-version change.

## What changed

1. **Doc fix: deletes DO update `SystemModstamp`.** The v0.19.0 pitfall
   statement ("deletes do not update SystemModstamp") was incorrect —
   live-proven while triaging the feedback: a watermark-based `queryAll`
   load carried 8,576/8,576 deleted rows inside the watermark itself,
   because deletion stamps `SystemModstamp` with the deletion time and the
   deleted row stays visible via `queryAll` until the recycle-bin purge
   (~15 days). What genuinely escapes a watermark load: **purges before the
   next load**, and objects with no `IsDeleted` field (e.g. `User`).
   Corrected in the EN/PT usage guides, function manuals, and the in-binary
   `salesforce_deleted_ids()` description.
2. **`salesforce_deleted_ids()` now returns `latest_date_covered`** (4th
   output column, feedback P4): the Replication API coverage frontier. On
   wide windows Salesforce may cover less than requested per call; the
   function already loops from there, and the column lets a sweep checkpoint
   how far getDeleted actually covered. `queryAll`-source rows emit NULL.
3. **`read_only` ATTACH option accepted** (feedback P5a): a no-op kept for
   policy standardization with tooling that appends `READ_ONLY TRUE` to
   every source. The catalog is read-only by design — every mutating
   operation throws — so the flag changes nothing; unknown-option errors no
   longer reject it.
4. **Id literal validation at plan time** (feedback P5b): an `=`, `!=`, or
   `IN` literal compared to an Id-type field must be a valid Salesforce Id
   (15 or 18 alphanumeric characters). A malformed value now fails
   immediately with a clear, field-named error instead of Salesforce's
   generic `INVALID_QUERY_FILTER_OPERATOR` (live-proven 2026-10-06: short
   prefixes and non-alphanumeric junk are rejected server-side; only full
   15/18-char ids are accepted).
5. **`sf_timeout_read_s` setting** (feedback P1 mitigation): HTTP read
   timeout in seconds per attempt (default 60, clamped to `[1,3600]`).
   A legitimate heavy server-side `COUNT()` can now be given minutes
   instead of failing at the 60s default; with `sf_retry_max` attempts the
   worst-case wall time is roughly `attempts x timeout`.

## Why v0.19.2 (patch)

Additive consumer-feedback cycle: one column, one accepted option, one
setting, one plan-time validation, and documentation corrections. No
function signature breaks, no supported DuckDB version change (baseline
remains v1.5.4/v1.5.5/v1.5.6).

## Validation

- Local pinned build (submodule pin v1.5.6, MSVC/Ninja `Release`): full build
  green; offline suite **59 files, 1657 assertions, 0 failures** (guard
  28/28 functions documented).
- New/updated tests: `salesforce_id_validation.test` (11 assertions,
  including the aggregate `count(*)` path), `salesforce_delete_sync.test`
  rewritten for the 4th column (27), `salesforce_attach_options.test` +5
  (read_only accepted; invalid values still rejected).
- `clang-format` (project pin 11.0.1) clean.
- Official `MainDistributionPipeline.yml` on the release-prep branch: 9/9
  green (run
  [38452235441](https://github.com/flozer/duckdb-salesforce/actions/runs/38452235441),
  PR
  [#88](https://github.com/flozer/duckdb-salesforce/pull/88)); post-merge
  `main` run recorded below.

### Official CI run record

Post-merge `main`: run
[38552907137](https://github.com/flozer/duckdb-salesforce/actions/runs/38552907137)
(2026-10-14, `workflow_dispatch` against `main` @ `d524500`, after PR
[#88](https://github.com/flozer/duckdb-salesforce/pull/88) merged) — **9/9
green**:

| DuckDB | linux_amd64 | windows_amd64 | osx_arm64 |
|---|---|---|---|
| v1.5.4 | Pass | Pass | Pass |
| v1.5.5 | Pass | Pass | Pass |
| v1.5.6 | Pass | Pass | Pass |

Release-prep branch pre-merge run also 9/9:
[38452235441](https://github.com/flozer/duckdb-salesforce/actions/runs/38452235441)
(2026-10-13, PR
[#88](https://github.com/flozer/duckdb-salesforce/pull/88)).

## Gates

- Community update: **PENDING** — descriptor update to v0.19.2 prepared after
  the own-repo release lands; the live
  `docs/community/description.yml` mirror keeps serving v0.19.1 until the
  upstream PR merges (per the C.5 gate).

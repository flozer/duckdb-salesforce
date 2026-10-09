# duckdb-salesforce v0.19.1

> **Own-repo release.** Bug-fix release for `salesforce_deleted_ids()`,
> closing three real-world gaps reported from a data-lakehouse deployment
> and closed by a maintainer live smoke (2026-10-06): **queryAll fallback
> truncated at exactly 2,000 ids**, **objects with no `IsDeleted` field
> (e.g. `User`) failed**, and **`getDeleted()` was never used for windows
> longer than 15 minutes**. Patch bump — fixes a function shipped in
> v0.19.0; no new features.

## What fixed

1. **queryAll sweep is now paginated.** The fallback fetched ONE page via
   `AuthorizedGet` and ignored `nextRecordsUrl` — a sweep silently stopped at
   exactly 2,000 ids. It now paginates via the session `FetchPage` helper
   (bounded at 100k pages).
2. **`getDeleted()` is the primary source for ANY window width.** The v0.19.0
   design capped it at 15-minute windows and fell back to queryAll for wider
   ones — meaning a 15-day sweep never used it. A live test proved the
   15-minute cap **does not exist** (a 15-day window is accepted in one call),
   so the cap is removed: getDeleted now serves any window, resuming from
   `latestDateCovered` when Salesforce covers less than requested, with a
   seen-set boundary dedup (resume slices can re-report records deleted
   exactly at the frontier).
3. **Objects with no `IsDeleted` field (e.g. `User`) work via getDeleted**
   (live-proven) — previously they only failed when the queryAll fallback ran
   (`INVALID_FIELD`). With getDeleted as primary, they resolve normally.

The queryAll sweep remains available as an explicit opt-in:
`source := 'queryAll'`.

## Why v0.19.1 (patch)

Fixes incorrect behavior of the `salesforce_deleted_ids()` function
introduced in v0.19.0 (truncation + wrong-source routing). No new features,
no supported-version change.

## Validation

- Local pinned build (submodule pin v1.5.6, MSVC/Ninja `Release`): full build
  green; offline suite **55 files, 1641 assertions, 0 failures** (guard
  28/28 functions documented).
- `clang-format` (project pin 11.0.1) clean.
- Official `MainDistributionPipeline.yml` on the fix branch: 9/9 green (run
  [37980413759](https://github.com/flozer/duckdb-salesforce/actions/runs/37980413759));
  post-merge `main` run recorded below.

### Official CI run record

TO BE FILLED BEFORE TAGGING.

## Gates

- Community update: **submitted with maintainer OK** on 2026-10-06 — the
  v0.19.1 descriptor update goes out as a follow-up PR on
  [duckdb/community-extensions#2948](https://github.com/duckdb/community-extensions/pull/2948)'s
  lineage (single jump v0.18.0 → v0.19.1), also gated on the same explicit
  maintainer OK already given for this cycle.

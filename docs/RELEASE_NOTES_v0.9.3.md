# Release notes — v0.9.3

> **Range-pushdown and datalake backfill hardening release.**
> v0.9.3 fixes a live production backfill issue where DuckDB could rewrite a
> two-sided timestamp range into a `BETWEEN` expression before pushdown, causing
> the Salesforce scan path to leave the range residual and over-fetch remotely.
> The community extension update is intentionally deferred for a later approved
> pack of changes.

## What's fixed

- **Two-sided range pushdown.** `CreatedDate >= lo AND CreatedDate < hi` and
  equivalent exact ranges now translate to SOQL when DuckDB represents them as a
  `BoundBetweenExpression`.
- **Residual safety.** Unsupported or unsafe range bounds, including defensive
  NULL-bound cases, stay residual so results remain correct.
- **Large Bulk backfill guardrails.** `sf_bulk_poll_budget` replaces the fixed
  Bulk polling budget, and `sf_bulk_require_predicate` can fail a forced Bulk
  scan before job creation when no predicate was pushed.
- **Bulk diagnostics.** `salesforce_query_cost()` now reports `bulk_polls` for
  Bulk jobs and distinguishes Salesforce server-side filtering from DuckDB
  residual filtering after a full remote scan.
- **Datalake seed guidance.** EN/PT docs now include a safe initial-seed recipe:
  validate quota and selectivity, prove pushdown with `salesforce_query_cost()`,
  split by pushed date windows, then let DuckDB/dbt own materialization and
  incremental maintenance.

## Evidence

- **Offline mock suite**: 1,968 assertions, 72 cases, 0 failures; 8 live tests
  skipped by design because they are maintainer-gated.
- **Focused coverage**: range pushdown across scan, REST, `COUNT(*)`, Bulk, and
  `queryAll`; Bulk guardrails and `bulk_polls` diagnostics.
- **Local build**: Windows release build linked
  `extension/salesforce/salesforce.duckdb_extension`.
- **Live v0.9 smoke**: `scripts/run_smoke_v0.9.ps1` passed with exit code 0.
- **Live §15 range probe, REST/COUNT**: `Produto_Oferta__c` two-sided
  `CreatedDate` range pushed to SOQL with `residual_filters = 0` and
  `count_pushdown = true`.
- **Live §15 range probe, Bulk**: same range pushed to SOQL with
  `residual_filters = 0`, `bulk_polls = 14`, and server-side filtering guidance.
- **GitHub Release assets**: CI run `27435133372` completed successfully and
  published exactly the expected `v0.9.3` Linux and Windows assets listed below;
  no stray `v0.9.2` assets remain attached to the release.

## Compatibility

- Default behavior remains conservative.
- Existing REST/Bulk scans keep their SQL surface.
- New Bulk guardrails are opt-in unless a user sets them.
- `v0.9.2` remains the current `duckdb/community-extensions` published baseline
  until Fernando gives a separate explicit GO for a community update PR.

## Release assets

The GitHub Release `v0.9.3` uses this file as the changelog and publishes:

- `duckdb-salesforce-0.9.3-linux-x64.tar.gz`
- `duckdb-salesforce-0.9.3-windows-x64.zip`

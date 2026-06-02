# Release notes — v0.2.0

> **Status: released (v0.2.0).** Built + tested against **DuckDB v1.5.3** (also
> verified on v1.5.2). Extensions are **version-locked** to the DuckDB used at
> build time. Live validation is manual-only; CI never contacts Salesforce.

Builds on **v0.1.0** (read-only REST connector). v0.2 makes the connector
faster, broader, and multi-version-verified — still read-only, REST-only.

## What's new in v0.2

- **Lazy / streaming scan (#11).** The scan fetches query pages on demand (page
  granularity) instead of all up front, so a small `LIMIT` lets DuckDB stop
  before later pages are fetched. `LIMIT` is still not pushed to SOQL (no hook);
  applied residually.
- **Metadata (describe) cache (#12).** Each sObject's schema is described once
  per ATTACH and reused; in-memory only, dropped at DETACH; no record/data cache.
- **Global object listing (#14).** `duckdb_tables()` / `information_schema.tables`
  list queryable sObjects via one global describe (`GET /sobjects`), cached.
  Columns stay lazy (described on first reference). `SHOW ALL TABLES` does not
  show name-only entries; `information_schema.columns` is empty until an object
  is referenced.
- **Broader predicate pushdown (#15).** `IN (...)`, `LIKE` (prefix/suffix/
  contains), and `OR` are translated to SOQL as **superset prefilters** and kept
  in the residual filter set — DuckDB reapplies them, so results are identical
  even when SOQL is broader (e.g. Salesforce case-insensitive `LIKE`). Guards:
  non-filterable → residual, huge `IN` (>200) → residual, `WHERE` > 4000 chars
  → residual, mixed-safety `OR` → whole `OR` residual.
- **DuckDB release build matrix (#17).** `scripts/build_matrix.ps1` builds + runs
  the offline suite per DuckDB release. Verified: v1.5.2 and v1.5.3 both build
  with 262 offline assertions green.

## Carried from v0.1

ATTACH + OAuth 2.0 refresh-token over HTTPS (TLS on, token in memory); lazy
sObject describe → DuckDB schema; `SELECT * FROM sf.<Object>` via REST `/query`
+ `queryMore`; JSON → typed vectors; projection + conservative predicate
pushdown (`=, <>, <, <=, >, >=, IS [NOT] NULL, AND`).

## Validation

- **Offline suite: 262 assertions across 11 files**, no network in CI (mocked
  HTTP); 4 gated `*_live.test` skipped without `SF_LIVE_*`.
- Build matrix green on DuckDB v1.5.2 + v1.5.3.
- **Manual live smoke (real maintainer-authorized org)** on the v0.2 build,
  every new feature confirmed (no secrets/data recorded here):
  - object listing via `information_schema.tables` returns standard + custom
    (`__c`) objects (#14);
  - `DESCRIBE sf.Account` returns its full column schema;
  - `SELECT … LIMIT 5` returns quickly (lazy scan, #11);
  - `WHERE Name LIKE 'A%'` and `WHERE Name IN (…)` return the expected filtered
    rows (pushdown #15, residual-refined);
  - a full `COUNT(*)` walked all pages — `salesforce_last_scan_pages()` reported
    the page count (lazy pagination, #11);
  - `salesforce_global_describe_calls()` = 1 and `salesforce_describe_calls()`
    counted per-object describes (cache + listing, #12/#14).

## Limitations (unchanged from v0.1 unless noted)

- Read-only; REST only (no Bulk/GraphQL/Tooling/Metadata API); no relationship
  traversal.
- `LIMIT` not pushed to SOQL (scan is lazy and may stop early).
- Metadata cache is in-memory only.
- Live validation is manual-only against a maintainer-authorized org; CI never
  contacts Salesforce or uses secrets.
- See the README **Pushdown** and **v0.1 limitations** sections for the full
  CAN/CANNOT detail.

## Release status

Tagged **`v0.2.0`**: offline suite green (262), build matrix green (v1.5.2 +
v1.5.3), and a manual live smoke on this build passed against a real
maintainer-authorized org (above). Human-approved.

## Not in v0.2 (backlog)

- **#13 Tooling API fast schema** — paused; revisit with real pain data (slow
  schema? global describe insufficient? richer fields needed?).
- Bulk API 2.0 + transport selection + quota governor (v0.3).
- GitHub Actions CI matrix.

## Community publication

Per `docs/ARCHITECTURE.md` C.5: no push/PR/tag/release to
`duckdb/community-extensions` without explicit human go/no-go after multi-version
smoke + package review. Development stays in `flozer/duckdb-salesforce`.

# duckdb-salesforce Roadmap

Status: post-`v0.8.1` roadmap. This file records the strategic direction after
the connector reached feature maturity, cross-platform CI, public documentation,
and a community-submission-ready package.

The core mission is to provide the best bridge between Salesforce and DuckDB for
analytics. The extension should expose Salesforce data safely, efficiently, and
transparently. DuckDB should continue to own materialization, joins,
transformations, Parquet export, and analytical workflows.

## Guiding Principles

- Keep the connector focused on Salesforce data access for DuckDB.
- Do not turn the extension into an ETL, CDC, replication, orchestration, or
  governance product.
- Strengthen the core before adding broad new features: correctness,
  compatibility, observability, and predictable performance come first.
- Push work to Salesforce only when semantics are safe and measurable.
- Leave DuckDB-native strengths to DuckDB: `CREATE TABLE AS`, `COPY`, views,
  joins, aggregation fallback, Parquet, and local persistence.
- Keep live Salesforce tests maintainer-controlled. CI remains offline,
  mock-only, and secret-free.
- Keep `duckdb/community-extensions` blocked until the explicit C.5 human gate
  is granted.

## Delivered Baseline

The connector already provides:

- OAuth refresh-token authentication over HTTPS.
- REST `/query` and `queryMore` scans.
- Bulk API 2.0 scans with auto/forced transport selection.
- Lazy REST and Bulk result streaming.
- Bulk PK chunking with parallel execution.
- Quota governor for Bulk starts.
- Query-cost diagnostics.
- Global object listing and metadata cache.
- REST Describe and Tooling API schema discovery.
- Parent relationship traversal, opt-in.
- Projection and predicate pushdown, including `IN`, `LIKE`, and `OR` with
  residual-safe behavior.
- `COUNT(*)` pushdown for safe zero-column scans.
- Cross-platform CI on `linux_amd64`, `windows_amd64`, and `osx_arm64` for
  DuckDB `v1.5.2` and `v1.5.3`.
- Public bilingual documentation, contribution docs, MIT license, third-party
  notices, and community descriptor draft.

## v0.9: Salesforce API Coverage

Goal: cover important Salesforce read surfaces that improve the bridge without
creating a replication product.

### 1. `queryAll` / Archived And Deleted Records

Add an opt-in read mode that uses Salesforce `queryAll` where supported.

Scope:

- Add a setting such as `sf_query_mode = 'query' | 'queryAll'`.
- Use `queryAll` only when explicitly requested.
- Document that this exposes archived/deleted records according to Salesforce
  API semantics.
- Keep default behavior unchanged.
- Keep diagnostics clear: query mode should appear in `salesforce_query_cost()`
  or an equivalent last-scan diagnostic.

Out of scope:

- Change-data-capture.
- Replication history.
- Managed tombstone storage.
- Incremental snapshot orchestration.

Acceptance:

- Mock tests prove `queryAll` URL/path selection.
- Default `query` behavior remains unchanged.
- Deleted/archived behavior is documented as Salesforce-controlled.
- No secrets or live tests in CI.

### 2. Bulk CSV Edge-Case Hardening

Strengthen the existing Bulk CSV bridge.

Scope:

- Add focused tests for multiline strings, escaped quotes, commas, CRLF/LF,
  empty values, null handling, base64, decimals, dates, times, timestamps, and
  timezone offsets.
- Compare REST JSON and Bulk CSV decoding for representative values.
- Improve error messages for malformed CSV or unsupported typed conversion.

Out of scope:

- New Bulk write/ingest APIs.
- CSV export tooling.
- ETL-level data quality rules.

Acceptance:

- Existing REST/Bulk tests stay green.
- Edge cases have explicit regression coverage.
- Errors remain field-named and secret-free.

## v1.0: Analytical Pushdown

Goal: reduce over-fetch for common analytical queries while preserving DuckDB
fallback correctness.

### 3. `COUNT(field)` Pushdown — DEFERRED

> **Status: DEFERRED (PM decision).** `COUNT(*)` already covers the main pain.
> `COUNT(field)` cannot reuse the `COUNT(*)` zero-column trick (the scan can't
> distinguish `COUNT(field)` from `SELECT field`), so it would require a DuckDB
> **OptimizerExtension** (plan rewrite + rebinding) — new machinery for a
> marginal win over the already-correct normal-scan count. Per ACTION_GUIDE
> (strengthen simple core > add complexity), deferred until there is real pain.
> `COUNT(field)` remains correct today via the normal scan + DuckDB aggregation.

Add safe non-null count pushdown where SOQL semantics match DuckDB expectations.

Scope:

- Evaluate `COUNT(field)` for scalar fields.
- Avoid pushdown when null or type semantics are uncertain.
- Keep residual fallback for unsupported expressions.

Acceptance:

- Generated SOQL is asserted.
- Result correctness is asserted against fallback behavior.
- Unsupported shapes remain correct through DuckDB.

### 4. Simple Aggregate Pushdown — TRANSPARENT FORM DEFERRED

> **Status: TRANSPARENT pushdown DEFERRED (PM decision, feasibility-checked).**
> Transparent `MIN`/`MAX`/`SUM`/`AVG` pushdown is NOT achievable without a plan
> rewrite. DuckDB runs the aggregate in an operator ABOVE the scan, so the scan
> must project the column and sees `column_ids = {x}` — indistinguishable from a
> plain `SELECT x`. The `TableFunction` API (v1.5.3) has projection/filter/limit
> pushdown hooks but **no aggregate-pushdown hook**, and the scan cannot
> speculate (emitting a single aggregate row would corrupt a non-aggregate
> `SELECT x`). The `COUNT(*)` zero-column trick does not transfer, because
> `MIN/MAX` always project the column. Enabling it would require an
> **OptimizerExtension** (rewrite `Aggregate→Scan`) — the same machinery that
> deferred `COUNT(field)` (§3). Per ACTION_GUIDE (strengthen simple core > add
> heavy layers), deferred. `MIN/MAX/SUM/AVG` remain correct today via the normal
> scan + DuckDB aggregation.
>
> **Planned instead: an explicit, opt-in `salesforce_aggregate()` table
> function** — server-side SOQL aggregates without dragging rows down, chosen
> explicitly by the user (no pretense of transparent pushdown, no optimizer).
> Short plan pending.

Future (only if/when an OptimizerExtension is justified) — transparent form:

- Only scalar fields with safe type mappings.
- No relationship aggregate pushdown in the first cut.
- No partial or approximate results.
- Pushdown only occurs for safe fields and aggregate shapes.
- Unsupported expressions remain local in DuckDB.
- Diagnostics show pushed aggregate versus fallback.

### 5. Simple `GROUP BY`

Evaluate SOQL aggregate queries for simple grouping.

Scope:

- Single object.
- Group by scalar fields.
- No rollups/cubes in the first cut.
- Preserve DuckDB fallback for unsupported cases.

Acceptance:

- Correct result shape and names.
- Clear limitations in docs.
- No silent semantic drift.

## v1.1: Relationship Depth

Goal: expose more of Salesforce's natural object graph without turning the
connector into a join engine.

### 6. Grandparent Traversal

Extend opt-in parent traversal from one level to safe multi-level SOQL paths,
for example `Contact.Account.Owner.Name`.

Scope:

- Depth limit aligned with Salesforce SOQL rules.
- Opt-in only.
- Skip polymorphic or ambiguous relationships.
- Keep child relationships out of scope.

Acceptance:

- Default schema remains unchanged.
- Multi-level parent fields decode correctly.
- Unsupported paths fail clearly or remain unavailable.

### 7. Relationship Diagnostics

Make relationship expansion observable.

Scope:

- Show which relationships were expanded.
- Explain skipped polymorphic/unavailable relationships.
- Document over-fetch and residual predicate behavior.

Acceptance:

- Diagnostics help users understand relationship behavior.
- No behavior change for default scans.

## v1.2: Operator Experience And Auth

Goal: make the bridge easier to operate without taking over orchestration.

### 8. Auth UX Improvements

Evaluate additional ways to provide credentials safely.

Options:

- SFDX auth URL input for local developers.
- JWT Bearer flow for headless environments.
- Clearer credential validation and error messages.

Out of scope:

- Secret persistence managed by the extension.
- Browser-based OAuth flows inside DuckDB.
- CI live Salesforce authentication.

Acceptance:

- Secrets are never logged.
- Existing refresh-token flow remains supported.
- Docs explain trade-offs.

### 9. macOS Live TLS Validation Or Trust Store Support

The macOS CI proves build and offline tests. Live Salesforce TLS on macOS remains
documented but not validated.

Options:

- Add a macOS trust-store path.
- Validate the existing OpenSSL path with a maintainer-run macOS smoke.
- Keep `SSL_CERT_FILE` as the documented workaround if no code change is needed.

Acceptance:

- The chosen path is documented.
- CI remains mock-only.
- No secrets are introduced.

## Documentation-Only: Materialization With DuckDB

Materialization is a DuckDB workflow, not a connector feature.

The project should document patterns such as:

- `CREATE TABLE local_account AS SELECT ... FROM sf.Account`.
- `COPY (SELECT ... FROM sf.Account) TO 'account.parquet'`.
- Incremental refresh examples using `SystemModstamp`.
- User-managed checkpoint tables in DuckDB.
- dbt/Airflow/Dagster examples as external orchestration patterns.

The connector may improve the bridge that makes these workflows possible. It
should not own persistent checkpoints, scheduling, replication state, or
orchestration.

## Community Publication Gate

No agent may create a branch, PR, push, fork change, or other action against
`duckdb/community-extensions` without explicit human approval.

Before any community action:

- Latest tagged release points to the intended submission ref.
- CI matrix is green.
- Package/release review is complete.
- Security docs are reviewed.
- License and notices are present.
- Documentation is public-ready.
- Human maintainer gives explicit C.5 go.

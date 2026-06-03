# duckdb-salesforce Roadmap

Status: planned roadmap. Updated after `v0.6.0`.

`v0.6.0` is the current validated release: read-only REST and Bulk, OAuth/TLS,
schema describe, Tooling schema discovery, lazy REST scans, Bulk query jobs,
quota governance for Bulk starts, query cost diagnostics, COUNT pushdown,
in-memory metadata cache, global object listing, parent relationship traversal,
projection pushdown, residual-safe predicate pushdown, and DuckDB release build
matrix.

This roadmap schedules the remaining capabilities needed before broad
distribution. `duckdb/community-extensions` remains blocked by the C.5 human
publication gate in `docs/ARCHITECTURE.md`.

## Guiding Principles

- Keep correctness first: if pushdown semantics are uncertain, keep DuckDB
  residual filtering.
- Keep API usage visible: large scans must expose quota and performance cost.
- Keep live Salesforce tests manual-only with maintainer-controlled credentials.
- Keep CI offline and secret-free.
- Build per DuckDB release; extension binaries are version-locked to the DuckDB
  release used to build them.

## v0.3: Large Extraction Path

Goal: make large materializations and exports practical.

### 1. Bulk API 2.0 Query Path

Use Bulk API 2.0 for large scans/materialization:

```sql
CREATE TABLE account_snapshot AS
SELECT * FROM sf.Account;

COPY (
  SELECT * FROM sf.Account
  WHERE LastModifiedDate >= TIMESTAMP '2025-01-01'
) TO 'account_snapshot.parquet';
```

Scope:

- Create Bulk query jobs.
- Poll job status with bounded backoff.
- Stream/download CSV result chunks.
- Decode into DuckDB vectors using existing schema/type mapping.
- Keep REST path for small/interative queries.
- Keep all secrets masked.

Acceptance:

- Mocked Bulk job lifecycle test: create -> poll -> result pages -> close.
- Large mock scan uses Bulk path when selected.
- REST path still used for small scans.
- Bulk errors surface clearly without token/secret leaks.
- Manual live smoke demonstrates one large read on maintainer-authorized org.

### 2. Transport Selection

Choose REST or Bulk by cost/shape.

Scope:

- Add configurable threshold knobs.
- Prefer REST for small/selective/interactive queries.
- Prefer Bulk for large full scans and materializations.
- Document that threshold is heuristic and org-dependent.

Acceptance:

- Tests prove REST selected below threshold.
- Tests prove Bulk selected above threshold.
- User can force REST/Bulk for diagnosis.
- Selection reason is visible in debug diagnostics.

## v0.4: Quota And Operational Safety

Goal: prevent accidental API exhaustion and make behavior predictable.

### 3. Quota Governor

Scope:

- Query Salesforce `/limits`.
- Track approximate calls made by the current ATTACH/session.
- Add reserve percentage setting.
- Fail early when an operation would likely consume unsafe quota.
- Distinguish `429` transient throttling from hard daily quota failures.

Acceptance:

- Mocked `/limits` low-quota case blocks large scan with clear error.
- `429` backoff remains retryable.
- `REQUEST_LIMIT_EXCEEDED` fails without retry loop.
- Docs explain quota reserve and override behavior.

### 4. Query Cost Diagnostics

Scope:

- Document Salesforce selectivity guidance.
- Add diagnostics for generated SOQL, pushed filters, residual filters, pages
  fetched, and selected transport.
- Evaluate Query Plan support only if there is a stable API path.

Acceptance:

- User can inspect last SOQL, pushed/residual filter summary, pages fetched, and
  transport selected.
- Docs explain indexed/selective filters and custom-index implications.

## v0.5: Analytical Pushdown

Goal: reduce over-fetch for common analytical queries.

### 5. Aggregate Pushdown

Scope:

- Push `COUNT(*)` / `COUNT(field)` where semantics are safe.
- Evaluate `MIN`, `MAX`, `SUM`, `AVG`.
- Evaluate `GROUP BY` for simple single-object queries.
- Keep fallback to DuckDB for unsupported shapes.

Acceptance:

- `SELECT COUNT(*) FROM sf.Account` generates aggregate SOQL and does not scan
  all records.
- Unsupported aggregate shapes remain correct via DuckDB fallback.
- Tests assert generated SOQL and result correctness.

## v0.6: Schema Depth And Relationships

Goal: improve metadata richness and relationship ergonomics.

### 6. Tooling API Fast Schema Discovery

Scope:

- Revisit only if real usage shows REST Describe/global describe pain.
- Use Tooling API for richer/faster schema discovery where it helps.
- Keep REST Describe as fallback.
- Reuse in-memory metadata cache.

Acceptance:

- Tooling path improves a measured schema-discovery case.
- REST fallback remains green.
- No eager all-field describe unless explicitly requested.

### 7. Relationship Support

Scope:

- Parent field traversal where SOQL supports it.
- Keep DuckDB local joins as the default recommended path.
- Avoid child relationship fan-out until semantics and cardinality are clear.

Acceptance:

- Simple parent traversal query works or is documented as unsupported.
- Local join workflow remains documented and tested.
- Unsupported relationship predicates fall back or fail clearly.

## v0.7: Bulk Streaming And Chunking

Goal: make large Bulk extraction memory-bounded and optionally parallel.

### 8. Lazy Bulk Result Streaming

Scope:

- Stream Bulk `Sforce-Locator` result pages during scan instead of eagerly
  fetching all CSV rows in `InitGlobal`.
- Keep typed CSV decoding and existing Bulk job lifecycle.
- Preserve quota governor behavior before job start.
- Keep `LIMIT` caveat honest: Bulk still does not receive server-side LIMIT.

Acceptance:

- Large mock Bulk result does not materialize all pages before first output.
- `salesforce_query_cost()` reports Bulk rows/pages consistently.
- Existing forced/auto Bulk tests remain green.
- Manual smoke shows lower memory pressure or equivalent behavior.

### 9. PK Chunking / Parallel Bulk Extraction

Scope:

- Split large object reads into key/range chunks where Salesforce supports it.
- Run chunks with bounded parallelism.
- Preserve ordering caveats and residual correctness.
- Keep single-thread path available as fallback.

Acceptance:

- Chunked extraction returns the same rows as unchunked extraction in mocks.
- Parallelism is configurable and bounded.
- Failures in one chunk surface clearly.
- Docs explain when PK chunking applies and when it does not.

## v0.8: Materialization And Snapshot Correctness

Goal: make repeatable local snapshots practical.

### 10. Incremental Materialization / Vault Mode

Scope:

- Add documented workflows for materializing Salesforce objects into local DuckDB
  tables or Parquet.
- Support incremental refresh keyed by `SystemModstamp` with a configurable
  lookback window.
- Store checkpoint metadata locally only when the user explicitly chooses a
  materialization workflow.

Acceptance:

- Initial materialization works for a selected object.
- Incremental refresh re-reads the lookback window and avoids missed edge rows.
- Checkpoint state is inspectable and resettable.
- Docs explain snapshot boundaries and operational safety.

### 11. `queryAll` / Deleted Record Coverage

Scope:

- Add an opt-in path for deleted/archived records where Salesforce supports it.
- Keep default query path unchanged.
- Integrate with materialization workflows.

Acceptance:

- `queryAll` smoke covers records that `/query` omits.
- Default scans remain unchanged.
- Docs explain deleted/archived semantics.

### 12. Bulk CSV Edge-Case Hardening

Scope:

- Add regression tests for known Bulk CSV datetime/epoch edge cases from the
  research log.
- Keep JSON and CSV cast paths aligned.

Acceptance:

- Datetime CSV variants decode correctly or fail with clear field-level errors.
- Existing REST and Bulk decoding tests remain green.

## v0.9: Resumability And Operator UX

Goal: handle quota/rate pressure without losing progress.

### 13. Rate-Limit Early-Exit And Resume

Scope:

- Convert quota pressure during materialization into a graceful checkpointed
  stop when possible.
- Resume from the checkpoint in the next run.
- Keep ad-hoc interactive scans simple.

Acceptance:

- Mock low-quota run exits with checkpoint rather than partial silent failure.
- Resume continues from the recorded state.
- Diagnostics explain why the run stopped.

### 14. Auth UX Improvements

Scope:

- Evaluate SFDX auth URL input for faster local setup.
- Evaluate JWT Bearer for headless/CI-style environments.
- Keep refresh-token flow as the default documented path.

Acceptance:

- New auth mode is opt-in and secret-safe.
- Existing auth tests remain green.
- Docs describe when each auth mode is appropriate.

## v1.0: Distribution Hardening

Goal: prepare for wider distribution while preserving C.5.

### 15. CI Matrix: Windows And Linux

Scope:

- Add GitHub Actions only after local matrix remains stable.
- Build/test against supported DuckDB releases.
- Keep live Salesforce tests skipped in CI.

Acceptance:

- Windows and Linux builds pass offline tests.
- CI never requires Salesforce secrets.
- Failures by DuckDB release are visible and version-scoped.

### 16. Package And Release Review

Scope:

- Review license/dependency packaging.
- Validate `vcpkg`, OpenSSL, and `httplib` packaging.
- Produce release artifacts for supported DuckDB versions.
- Review docs for install, auth, smoke, limitations, and security.

Acceptance:

- Local package install works.
- Release artifact matches DuckDB version.
- Docs are sufficient for a new user to connect to a maintainer-authorized org.

## Community Publication Gate

No branch, tag, release, pull request, or artifact may be pushed to
`duckdb/community-extensions` without explicit human approval.

Before any community action:

- Latest tagged release is smoke-tested manually.
- Windows/Linux build matrix is green.
- Package/release review is complete.
- Security docs are reviewed.
- Human maintainer gives explicit go/no-go.

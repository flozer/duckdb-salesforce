# duckdb-salesforce Roadmap

Status: planned roadmap after `v0.2.0`.

`v0.2.0` is the current validated release: read-only REST, OAuth/TLS, schema
describe, lazy scans, in-memory metadata cache, global object listing, projection
pushdown, residual-safe predicate pushdown, and DuckDB release build matrix.

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

## v0.7: Distribution Hardening

Goal: prepare for wider distribution while preserving C.5.

### 8. CI Matrix: Windows And Linux

Scope:

- Add GitHub Actions only after local matrix remains stable.
- Build/test against supported DuckDB releases.
- Keep live Salesforce tests skipped in CI.

Acceptance:

- Windows and Linux builds pass offline tests.
- CI never requires Salesforce secrets.
- Failures by DuckDB release are visible and version-scoped.

### 9. Package And Release Review

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


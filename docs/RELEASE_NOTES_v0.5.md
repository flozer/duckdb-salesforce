# Release Notes — v0.5.0

> **STATUS: VALIDATED — tagged `v0.5.0`.** Validated by maintainer-provided
> manual live smoke evidence against an authorized org, plus a green offline
> suite. The DEV did **not** independently rerun the live smoke (no `SF_LIVE_*`
> credentials were present); the live validation is attributed to the
> maintainer's reviewed evidence. Covers the v0.4 §4 (query cost diagnostics) +
> v0.5 §5 (COUNT pushdown) work on `flozer/duckdb-salesforce` `main`. Nothing in
> this cycle touches `duckdb/community-extensions` (gate C.5).

Commits: `8fd5db2` (§4 query cost), `6973e66` (§5 COUNT pushdown + WHERE fix).

---

## What's new

### `salesforce_query_cost()` — query cost diagnostics (v0.4 §4)

A single-row, **last-scan** view of what a scan cost and why: SOQL, resolved
transport (+ est_rows + reason), projection ratio, pushed vs residual filter
counts, the pushed `WHERE`, REST pages, rows delivered to DuckDB, Bulk flag,
`count_pushdown`, the quota decision, and short selectivity guidance.

```sql
SELECT Id, Name FROM sf.Account WHERE Name = 'Acme';
SELECT * FROM salesforce_query_cost();
```

Columns: `object, soql, transport, est_rows, transport_reason, projected_fields,
total_fields, pushed_filters, residual_filters, where_pushed, pages_fetched,
rows_emitted, bulk, count_pushdown, quota_remaining, quota_allowed, guidance`.

Complements (does not replace) the granular `salesforce_last_soql()`,
`salesforce_last_transport()`, `salesforce_last_quota()`,
`salesforce_last_scan_pages()`. Last-scan, best-effort, single-threaded.

### COUNT pushdown (v0.5 §5)

A scan that needs only the **row count and zero real columns** (`COUNT(*)`,
`SELECT 1 FROM …`, `EXISTS`-style) now runs a single `SELECT COUNT() FROM <obj>
[WHERE …]` and emits that many empty rows for DuckDB to count — instead of
paging every record. The 54k-row / ~9s `COUNT(*)` smoke becomes one cheap call.

Applies only when **all** hold (otherwise the normal scan runs — always correct):

- zero real columns projected;
- **no residual filter** (a non-pushable predicate forces a real scan);
- the `COUNT()` probe succeeds (failure → full scan);
- `sf_force_transport` not forced to `'bulk'` (a forced Bulk is honoured).

`salesforce_query_cost()` reports `count_pushdown=true`, `pages_fetched=0`,
`rows_emitted=<totalSize>`.

### Correctness fix — `COUNT(*) … WHERE …` could over-count (not just perf)

> **This is a correctness fix, not only a performance change.**

DuckDB invokes the table function's filter-pushdown hook **more than once** for
an aggregate plan: a first call carrying the predicate, then a second call with
an **empty** filter list. The hook rebuilt the SOQL `WHERE` from scratch each
call, so the empty second call **wiped** the `WHERE` produced by the first. The
generated SOQL then had **no `WHERE`**, and `COUNT(*) … WHERE <pred>` **silently
counted all rows** (ignoring the predicate).

This affected aggregate-with-filter queries since v0.1. Fixed: the hook now
skips empty calls and **accumulates** the pushed `WHERE` across calls (and
`Copy()` carries the pushed/residual filter counts). Verified:
`COUNT(*) FROM sf.Account WHERE Name = 'Acme'` now generates
`SELECT COUNT() FROM Account WHERE Name = 'Acme'`.

> **Upgrade note:** if you ran `COUNT(*)`/aggregate queries with a `WHERE` on a
> pushable, filterable field against a real org on an earlier build, those
> counts may have been too high. Re-run them on v0.5.0.

---

## Limitations

- **COUNT-only.** Only `COUNT(*)`-class (zero-real-column) scans are pushed.
  **`COUNT(field)` (non-null count), `GROUP BY`, `SUM`, `AVG`, `MIN`, `MAX` are
  NOT pushed** — they run as a normal scan with DuckDB aggregating locally.
- COUNT pushdown does not engage under forced `sf_force_transport='bulk'`.
- `salesforce_query_cost()` is **last-scan, best-effort** (overwritten by the
  next scan; single-threaded), not a history.
- All prior limitations stand (read-only; Bulk ignores `LIMIT` + eager fetch;
  `auto` cannot see `LIMIT`; quota governor gates Bulk starts only).

---

## Smoke checklist (manual, maintainer-only)

Run against a user-authorized org controlled by the maintainer. Automated CI
must never contact Salesforce or require secrets.

- [ ] **`COUNT(*)`** — `SELECT COUNT(*) FROM sf.Account;` returns the org count
      fast; `salesforce_query_cost()` → `count_pushdown=true`, `pages_fetched=0`,
      `soql` = `SELECT COUNT() FROM Account`.
- [ ] **`COUNT(*)` WHERE** — `SELECT COUNT(*) FROM sf.Account WHERE Name = '…';`
      returns the **filtered** count; `soql` carries `WHERE …`. (Confirms the
      correctness fix against a real org.)
- [ ] **residual fallback** — `COUNT(*)` with a predicate on a non-filterable
      field → `count_pushdown=false`, a real scan runs, count still correct.
- [ ] **`salesforce_query_cost()`** — inspect a normal data scan: transport,
      pushed/residual filters, pages, rows_emitted, guidance look right.

---

## Validation record (for tagging)

Gate status at tag time — all satisfied:

- [x] Offline test suite green — 17 `test/sql/*.test` files, verified by the DEV.
- [x] Manual smoke: `COUNT(*)` — maintainer-attested.
- [x] Manual smoke: `COUNT(*)` WHERE (filtered count, the fix) — maintainer-attested.
- [x] Manual smoke: residual fallback — maintainer-attested.
- [x] Manual smoke: `salesforce_query_cost()` reviewed — maintainer-attested.
- [x] Human go — maintainer authorized the tag.

Maintainer smoke evidence (secret-free; reviewed from 3 `salesforce_query_cost()`
captures against an authorized org):

| Case | soql | count_pushdown | pages_fetched | rows_emitted |
| --- | --- | --- | --- | --- |
| `COUNT(*)` | `SELECT COUNT() FROM Account` | `true` | `0` | `54710` |
| `COUNT(*) WHERE Name != ''` | `SELECT COUNT() FROM Account WHERE Name != ''` | `true` | `0` | `54710` |
| residual fallback | `SELECT Name FROM Account` | `false` | `28` | `54710` |

- Case 2 confirms the `WHERE` is carried into the `COUNT()` SOQL (the
  over-count correctness fix) against a real org.
- Case 3 confirms a non-pushable predicate falls back to a correct normal scan
  (28 pages), with guidance flagging the residual filter / over-fetch.

Attribution / honesty notes:

- The live smoke was **not** independently rerun by the DEV — no `SF_LIVE_*`
  credentials were present. The live validation is the **maintainer's** reviewed
  evidence.
- No secrets, tokens, org identifiers, or response bodies are recorded here.
- Automated CI never contacts Salesforce and never requires secrets.

Tag: annotated `v0.5.0` on `flozer/duckdb-salesforce`. **Nothing** pushed to
`duckdb/community-extensions` (gate C.5).

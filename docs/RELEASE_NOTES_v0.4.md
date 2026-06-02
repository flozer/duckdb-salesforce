# Release Notes — v0.4.0 (DRAFT)

> **STATUS: DRAFT — NOT TAGGED.** This note covers the v0.3 + v0.4 work landed on
> `flozer/duckdb-salesforce` `main`. Do not tag until the **Before tagging**
> checklist below is complete and a human gives the go. Nothing in this cycle
> touches `duckdb/community-extensions` (gate C.5).

This release bundles two milestones delivered together:

- **v0.3** — Bulk API 2.0 query path + transport selection.
- **v0.4** — quota governor protecting Bulk starts.

Commits: `5841461` (v0.3 §1 Bulk), `05f2601` (v0.3 §2 auto), `668f3cf` (v0.4 quota).

---

## What's new

### Bulk API 2.0 query path (v0.3 §1)

A scan can now run over a **Bulk API 2.0 query job** instead of the REST
`/query` endpoint. The Bulk path creates a query job, polls it to
`JobComplete`, and downloads the CSV result pages (following `Sforce-Locator`).
It uses the **same** optimised SOQL as REST, so projection + predicate pushdown
apply identically — only the delivery mechanism differs.

- Shares the bearer auth + single `401 → refresh → retry` with the REST path.
- A `Failed`/`Aborted` job raises a clean, secret-free error.
- RFC4180 CSV decoding (quotes, `""` escapes, embedded comma/newline, LF/CRLF);
  typed cast core shared with the REST/JSON path.

### `sf_force_transport` — `rest` | `bulk` | `auto`

```sql
SET sf_force_transport = 'rest';   -- default: lazy REST /query + queryMore
SET sf_force_transport = 'bulk';   -- force Bulk API 2.0 query job
SET sf_force_transport = 'auto';   -- opt-in: probe row count, pick rest/bulk
```

- **Default stays `rest`** — interactive behaviour is unchanged.
- `bulk` forces the Bulk path (large extractions, `CREATE TABLE AS`, `COPY`).

### Transport auto-selection (`auto`, v0.3 §2)

When `sf_force_transport='auto'`, the scan probes the row count once
(`SELECT COUNT()` — **one REST call, zero row egress**) and picks Bulk only for
large reads.

| Signal | Decision |
| --- | --- |
| est. rows `> sf_auto_bulk_threshold` (default 50000) | **Bulk** |
| est. rows `<= threshold` | **REST** |
| aggregate-only scan (`COUNT(*)`, no real column) | **REST** (no probe) |
| probe failed (HTTP error / no `totalSize`) | **REST** (never blocks) |
| `sf_auto_probe = false` | **REST** (probe skipped) |
| forced `rest`/`bulk` | no probe runs |

Decision is made **once**, before the first row — there is no mid-stream
escalation.

### Quota governor (v0.4)

Before starting a **Bulk** job, the governor reads the org's `GET /limits` once
(cached in memory per `instance_url`, TTL-bounded, never persisted) and refuses
to start the job when the remaining daily API allocation is at/below the
reserve:

> threshold = `max(sf_quota_min_remaining, sf_quota_reserve_pct% × DailyApiRequests.Max)`
> — allowed iff `Remaining > threshold`.

- **Bulk-only.** REST scans are **not** preflight-gated.
- **Fail-open by default** — if `/limits` cannot be read, the Bulk job proceeds
  (`sf_quota_fail_open=false` hardens it to block).
- **Warn mode** — `sf_quota_enforce=false` consults + reports but proceeds.
- `429` (short-term rate limit) is retried; `REQUEST_LIMIT_EXCEEDED` (daily cap)
  is **terminal** and never retried.
- Errors never include a bearer token, secret, or raw response body.

---

## Settings

| Setting | Default | Meaning |
| --- | --- | --- |
| `sf_force_transport` | `rest` | `rest` \| `bulk` \| `auto` |
| `sf_auto_bulk_threshold` | `50000` | rows above which `auto` picks Bulk |
| `sf_auto_probe` | `true` | run the `auto` COUNT() probe (`false` → always REST) |
| `sf_quota_enabled` | `true` | `false` → skip `/limits`, never block |
| `sf_quota_enforce` | `true` | `false` → consult + report, but proceed (warn) |
| `sf_quota_fail_open` | `true` | `/limits` unavailable → allow; `false` → block |
| `sf_quota_reserve_pct` | `10` | reserve % of `DailyApiRequests.Max` |
| `sf_quota_min_remaining` | `1000` | absolute floor of remaining requests |
| `sf_quota_cache_seconds` | `60` | in-memory `/limits` TTL per `instance_url` (`0` = off) |

### Diagnostics (user-facing)

```sql
SELECT * FROM salesforce_last_transport();  -- (transport, est_rows, reason)
SELECT * FROM salesforce_last_quota();      -- (limit_name, max, remaining, threshold, allowed, reason)
```

(Other `salesforce_last_*` / `salesforce_*_calls` table functions are DEBUG/TEST
only and not a stable public API.)

---

## Limitations

- **Read-only.** No DML.
- **Bulk ignores SOQL `LIMIT`** — the full result set is fetched; `LIMIT` is
  applied residually by DuckDB. Use `rest` when a small `LIMIT` should read
  little.
- **Bulk fetch is eager** — the whole result is downloaded in `InitGlobal`, so
  memory scales with result size. No streaming of Bulk pages yet.
- **`auto` cannot see `LIMIT`** (DuckDB does not expose it to a table function):
  a small `LIMIT` over a huge object may still estimate large and pick Bulk.
  Force `sf_force_transport='rest'` for interactive small-`LIMIT` reads.
- **No mid-stream escalation** — transport is decided once.
- **Quota governor protects Bulk starts only; REST scans are not
  preflight-gated.** A REST scan can still consume API calls (one per page).
- `auto`'s probe costs **one** `COUNT()` API call (zero row egress).

---

## Smoke checklist (manual, maintainer-only)

Run against a user-authorized org controlled by the maintainer. Automated CI
must never contact Salesforce or require secrets.

- [ ] **REST scan** — `SET sf_force_transport='rest'; SELECT Id, Name FROM sf.Account WHERE Name = '…' LIMIT 5;` returns rows; `salesforce_last_transport()` → `rest`.
- [ ] **Bulk scan** — `SET sf_force_transport='bulk'; SELECT … FROM sf.<BigObject>;` returns the full set; `salesforce_last_transport()` → `bulk`, `salesforce_last_quota()` shows the check.
- [ ] **auto → small** — `SET sf_force_transport='auto';` on a small object → `salesforce_last_transport()` shows `rest` with `est_rows` and a `<= threshold` reason.
- [ ] **auto → large** — same on a large object (or lower `sf_auto_bulk_threshold`) → `bulk` with a `> threshold` reason.
- [ ] **Quota** — confirm `salesforce_last_quota()` reports real `Max`/`Remaining` after a Bulk run; optionally lower `sf_quota_min_remaining` to force a block and confirm the clear error (then restore).
- [ ] **Pushdown sanity** — `salesforce_last_soql()` / job create body shows projection + WHERE.

---

## Before tagging

All must be true before tagging `v0.4.0`:

- [ ] Offline test suite green (all `test/sql/*.test`).
- [ ] Manual smoke: REST scan.
- [ ] Manual smoke: Bulk scan.
- [ ] Manual smoke: `auto` large + small.
- [ ] Manual quota check (live) **or** documented mock evidence.
- [ ] Human go.

Only then: tag `v0.4.0` on `flozer/duckdb-salesforce`. **Nothing** in
`duckdb/community-extensions` (gate C.5).

# Release Notes — v0.7.0

> **STATUS: VALIDATED — tagged `v0.7.0`.** Validated by maintainer-provided
> manual live smoke evidence against an authorized org, plus a green offline
> suite. The DEV did **not** independently rerun the live smoke (no `SF_LIVE_*`
> credentials were present); the live validation is attributed to the
> maintainer's reviewed evidence. Covers v0.7 §8 (lazy Bulk result streaming) +
> §9 cut 1 (sequential PK chunking) on `flozer/duckdb-salesforce` `main`.
> Nothing in this cycle touches `duckdb/community-extensions` (gate C.5).

Commits: `f49a41d` (§8 lazy streaming), `888c33a` (§9 PK chunking), `cc382bf`
(§9 boundary-length fix, #27), guidance-text patch.

---

## What's new

### §8 — Lazy Bulk result streaming

The Bulk transport no longer downloads the whole result up front. The job is
still **created and polled to `JobComplete`** in `InitGlobal` (a Bulk API 2.0
query job must finish server-side before any results exist), but result CSV
pages are now fetched **on demand** as the scan drains them, following the
`Sforce-Locator`.

- Memory no longer scales with the full result.
- A small `LIMIT` stops pulling early — later result pages are **never
  downloaded** (`LIMIT` is still **not** promised server-side; the job itself
  runs to completion).
- A `Failed`/`Aborted` job, a results HTTP error, or a repeated locator each
  raise a clean, secret-free error.

### §9 cut 1 — Sequential PK chunking

`sf_bulk_chunks = N` (default `1` = off, capped at 8, **Bulk-only**) splits a
Bulk scan into **N disjoint `Id` ranges**, each run as its own Bulk job and
streamed via §8.

```sql
SET sf_force_transport = 'bulk';
SET sf_bulk_chunks = 4;
SELECT * FROM sf.Account;          -- 4 jobs over Id ranges, unioned
```

- Ranges come from a `MIN(Id), MAX(Id)` probe (one REST call) split by uniform
  base62 lexical interpolation — contiguous, disjoint, exhaustive over
  `[min, max]`.
- The quota governor is consulted **per job**.
- Probe failure / empty object → falls back to a single chunk.
- **Sequential** in this cut; real parallel execution is the §9b follow-up.

### Diagnostic changes

- **`salesforce_query_cost().pages_fetched` is now a real count for Bulk** (it
  was `NULL` before v0.7) — proves the lazy page download.
- **New `salesforce_query_cost().bulk_chunks`** — the PK-chunk count (`1` = no
  chunking).

---

## Limitations

- The Bulk **job still completes server-side before any results** — §8 makes the
  *download* lazy, not the job's execution time.
- **PK chunks are sequential**, not parallel (§9b is the follow-up).
- The lexical Id split can produce **uneven or empty** chunks (Salesforce `Id`s
  aren't uniformly dense) — coverage is exact, balance is not guaranteed.
- **No global row order** across chunks — use `ORDER BY` if you need it.
- `sf_bulk_chunks` is **Bulk-only**; REST ignores it.
- `LIMIT` is still not pushed server-side (Bulk ignores SOQL `LIMIT`).
- All prior limitations stand (read-only; `auto` cannot see `LIMIT`; quota gates
  Bulk starts; COUNT-only aggregate pushdown; relationships depth-1 parent only;
  Tooling coarse types).

---

## Smoke checklist (manual, maintainer-only)

Run against a user-authorized org controlled by the maintainer. Automated CI
must never contact Salesforce or require secrets.

- [ ] **Bulk small `LIMIT`** — `SET sf_force_transport='bulk';
      SELECT * FROM sf.<BigObject> LIMIT 100;` returns fast;
      `salesforce_query_cost().pages_fetched` is small (not the full page count).
- [ ] **Bulk full scan** — same object, no `LIMIT` → `pages_fetched` reflects
      several pages; `rows_emitted` = full count.
- [ ] **PK chunking** — `SET sf_bulk_chunks=2;` (and `=3`) → query returns the
      **same row set** as un-chunked (union correct, no dups/gaps);
      `salesforce_query_cost().bulk_chunks` = 2 / 3.
- [ ] **Quota** — `salesforce_last_quota()` shows `allowed = true` across the
      chunked run.
- [ ] **Diagnostics** — `salesforce_query_cost()` shows sane `bulk_chunks`,
      `pages_fetched`, `rows_emitted`.

---

## Validation record (for tagging)

Gate status at tag time — all satisfied:

- [x] Offline test suite green — 21 `test/sql/*.test` files, verified by the DEV.
- [x] Manual smoke: Bulk lazy streaming (§8) — maintainer-attested.
- [x] Manual smoke: `sf_bulk_chunks=2` and `=3` — maintainer-attested.
- [x] Manual smoke: quota allowed + `salesforce_query_cost()` reviewed — maintainer-attested.
- [x] Human go — maintainer authorized the tag.

Maintainer smoke evidence (secret-free; reviewed from `salesforce_query_cost()` /
`salesforce_last_quota()` captures against an authorized org):

| `sf_bulk_chunks` | count | `bulk_chunks` | `pages_fetched` | `rows_emitted` | `quota_allowed` |
| --- | --- | --- | --- | --- | --- |
| 2 | 54712 | 2 | 2 | 54712 | true |
| 3 | 54712 | 3 | 3 | 54712 | true |

- Both chunk counts return the **same total (54712)** → disjoint + exhaustive
  partition (no dups, no gaps); no `INVALID_QUERY_FILTER_OPERATOR` → the §9
  boundary-length fix (#27) holds live.
- Quota healthy: remaining ~83k, threshold ~14180, `allowed=true`.

Attribution / honesty notes:

- The live smoke was **not** independently rerun by the DEV — no `SF_LIVE_*`
  credentials were present. The live validation is the **maintainer's** reviewed
  evidence.
- No secrets, tokens, org identifiers, or response bodies are recorded here.
- Automated CI never contacts Salesforce and never requires secrets.

Tag: annotated `v0.7.0` on `flozer/duckdb-salesforce`. **Nothing** pushed to
`duckdb/community-extensions` (gate C.5). §9b (parallel Bulk execution) comes
only after the tag.

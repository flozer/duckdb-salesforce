# Release Notes — v0.7.1

> **STATUS: VALIDATED — tagged `v0.7.1`.** A small follow-up to v0.7.0: §9b
> **parallel** Bulk execution. Validated by maintainer-provided manual live
> smoke evidence against an authorized org, plus a green offline suite. The DEV
> did **not** independently rerun the live smoke (no `SF_LIVE_*` credentials
> were present); the live validation is attributed to the maintainer's reviewed
> evidence. Nothing in this cycle touches `duckdb/community-extensions`
> (gate C.5).

Commit: `7f94cae` (§9b parallel Bulk execution).

---

## What's new

### §9b — Parallel Bulk execution

`sf_bulk_chunks=N` now runs its chunks **in parallel** instead of sequentially.
Each chunk is a DuckDB scan thread with its **own** client/session/Bulk job,
claiming chunk indices from a shared atomic dispenser; the chunk job + lazy page
streaming (§8) live in per-thread local state. `MaxThreads()` = chunk count
(DuckDB may clamp to the hardware / `threads` setting — the contract is "up to N
chunks concurrently", not exactly N threads).

- **Default `sf_bulk_chunks=1` is unchanged** (single thread, identical to v0.7.0).
- Quota governor is still consulted **per job**; REST / COUNT paths are untouched
  (they stay single-threaded). **No global row order** across chunks.

(No setting changes, no new diagnostics — `bulk_chunks` / `pages_fetched` /
`rows_emitted` from v0.7.0 still apply, now aggregated across threads.)

---

## Validation record

Gate status at tag time — all satisfied:

- [x] Offline test suite green — 21 `test/sql/*.test` files, verified by the DEV.
- [x] Manual smoke: `sf_bulk_chunks=2` and `=3` parallel — maintainer-attested.
- [x] Human go — maintainer authorized the tag.

Maintainer smoke evidence (secret-free; reviewed live captures against an
authorized org):

| `sf_bulk_chunks` | `rows_emitted` | `pages_fetched` | `bulk_chunks` | `quota_allowed` |
| --- | --- | --- | --- | --- |
| 2 | 54713 | 2 | 2 | true |
| 3 | 54713 | 3 | 3 | true |

- Both chunk counts return the **same total (54713)** → partitioning stays
  correct under parallel execution (same union, no dups/gaps); no errors.
- Quota healthy: remaining 83529, threshold 14180, `allowed=true`.
- `salesforce_query_cost().guidance` (corrected in v0.7.0): "Bulk job completes
  server-side first; result pages stream lazily; LIMIT is not server-side".

Attribution / honesty notes:

- The live smoke was **not** independently rerun by the DEV — no `SF_LIVE_*`
  credentials were present. The live validation is the **maintainer's** reviewed
  evidence.
- The offline mock pins `threads=1` for a deterministic scripted sequence, so
  the mock validates partitioning + union + counts; **concurrency itself is
  what this live smoke confirms**.
- No secrets, tokens, org identifiers, or response bodies are recorded here.

Tag: annotated `v0.7.1` on `flozer/duckdb-salesforce`. **Nothing** pushed to
`duckdb/community-extensions` (gate C.5).

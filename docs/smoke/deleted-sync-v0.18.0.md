# Live smoke — delete-sync sweep (P1.4 follow-up / consumer feedback #1)

> Date: 2026-09-30. Maintainer-run live smoke on the real org
> (`vitoriastone.my.salesforce.com`), maintainer-approved, read-only.
> PII-free: only aggregate counts; no records, no names.
> Purpose: measure how many soft-deleted rows exist in the recycle bin
> (~15 days) per bronze object — the contamination surface of a
> watermark-based incremental load, which never notices deletes because
> **deletes do not update `SystemModstamp`**.

## Setup

- Mode: `SET sf_query_mode = 'queryAll'` (archived + soft-deleted included).
- Server-side per object (1 aggregate call each, predicate pushed):
  `SELECT … FROM salesforce_aggregate('sf', <object>,
  'COUNT(Id) n_del, MIN(SystemModstamp) mn, MAX(SystemModstamp) mx',
  'IsDeleted = true')`.
- Consistency check on Account: queryAll total vs `IsDeleted = false` total.

## Results

| Object | Deleted (bin, ~15 days) | Note |
|---|---:|---|
| Lead | 0 | |
| Contact | 0 | |
| Account | 0 | total queryAll = total `IsDeleted = false` = 54,999 ✔ |
| Case | 0 | |
| Produto_Oferta__c | 0 | 3,166,619 live rows |
| User | n/a | `User` has no `IsDeleted` — Salesforce rejects with `INVALID_FIELD`; propagated as a clear error ✔ |

## Verdict

- **Mechanism verified live:** `queryAll + IsDeleted = true` pushes
  server-side (COUNT pushdown applies) and is consistent with the live-row
  counts. The periodic delete-sweep pattern works today, read-only, one
  aggregate call per object.
- **Current contamination on this org: 0** across all bronze objects — no
  active incident; the gap is **structural** (the first real delete would be
  invisible to a watermark-only load).
- Consequence: the delete-sync primitive
  (`salesforce_deleted_ids()`, getDeleted() + queryAll fallback) remains on
  the roadmap as the sync primitive; the sweep recipe above is the
  no-code stopgap until then.
- Hardening note: the transient-retry budget is now tunable (`sf_retry_max`,
  `sf_retry_backoff_ms`), shipping in the same release.

## Reproduce

Any of the aggregate queries above with `sf_query_mode = 'queryAll'`.

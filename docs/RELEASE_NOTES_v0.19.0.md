# duckdb-salesforce v0.19.0

> **Own-repo release.** Closes the consumer-feedback backlog: the
> **delete-sync primitive** (`salesforce_deleted_ids`, #1), the **Bulk job
> resume** recovery path (`salesforce_bulk_resume`, #4), the **user-callable
> scan** with per-call overrides and a validated raw filter
> (`salesforce_scan`, #3b), **retry tuning settings** (#2), **error remedy
> hints** (#5), and the **watermark/delete pitfall documentation** (#6) —
> plus the live delete-sweep evidence that grounded the design. Two new
> table functions (25 → 27... 28 with the scan), minor bump.

## What changed

**`salesforce_deleted_ids(catalog, object [, since] [, until])` (#1, #83)**

The delete-sync primitive: returns ids deleted in the org within a window, so
watermark-based incremental loads can pair their upsert with a delete sweep
(**deletes do not update `SystemModstamp`** — a watermark load never notices
them; live-org smoke `docs/smoke/deleted-sync-v0.18.0.md` documented the
pattern and the 0-in-bin measurement recipe). Windows ≤ 15 minutes use the
Replication API `getDeleted()` (exact `deleted_date`, `source='getDeleted'`);
wider windows fall back to a `queryAll` `IsDeleted` scan (`deleted_date` NULL,
~15-day recycle-bin retention, `source='queryAll'`). Credentials from the
attached catalog; read-only.

**`salesforce_bulk_resume(catalog, job_id)` (#4, #84)**

Re-streams the result rows of an existing Bulk API 2.0 query job by id — the
recovery path when a load dies mid-stream of a job that already completed
server-side (the retry does not re-create the job or re-run the query). First
results page fetched at bind; its CSV header defines the output schema (one
VARCHAR column per field). Rows stream lazily following the Sforce-Locator.
Invalid/expired job ids fail fast with the surfaced errorCode + remedy hint.

**User-callable `salesforce_scan(catalog, object [, filter])` (#3b, #84)**

Direct scan using an attached catalog's credentials without the catalog table
layer, with per-call named-parameter overrides:

- `filter :=` — validated raw SOQL predicate (no `;`, no nested SELECT,
  ≤ 4000 chars) AND-ed into the server-side WHERE, so `COUNT(*)`/aggregate
  pushdowns and diagnostics see the combined predicate;
- `query_mode :=`, `transport :=`, `chunks :=` — per-call overrides; the
  per-call flavor of the v0.18.0 ATTACH options (both close the dbt
  session-leak hazard: a `pre_hook` `SET sf_query_mode='queryAll'` bleeding
  into the next model on a shared connection);
- schema from the sObject describe at bind (flat fields — relationship STRUCT
  expansion remains a catalog-table feature); plan projection/predicate
  pushdown and safety rules identical to catalog tables.

**Retry tuning (`sf_retry_max` / `sf_retry_backoff_ms`) (#2, #82)**

The HTTP client's transient-failure retry (429/5xx/connection errors) was
hardcoded at 3 attempts / 200 ms linear backoff — too tight to survive a
multi-second SSL blip, which turned single-scan failures into full orchestrator
re-runs. Both are now session settings (clamped [1,10] / [0,60000]; defaults
preserve the historical behavior). The 401 → refresh → retry path is separate
and always applies.

**Error remedy hints (#5, #83)**

Well-known Salesforce errorCodes now surface with an embedded remedy hint:
`INVALID_SESSION_ID` (refresh-token revoked?), `REQUEST_LIMIT_EXCEEDED` (→
`salesforce_last_quota()`/`salesforce_query_cost()`), `QUERY_TIMEOUT` (→
`salesforce_query_explain()` residual check), `INVALID_FIELD`/`INVALID_TYPE`
(→ metadata helpers), `OPERATION_TOO_LARGE`, and 429 (→ retry settings).

**Docs (#6)** — the watermark/delete pitfall is documented in the `queryAll`
sections (EN/PT manuals + skill) and the usage guides gained three new
sections: the delete-sync pattern (11.1/12.1), direct scans (11.2/12.2), and
Bulk job resume (11.3/12.3).

## Why v0.19.0 (minor)

Two new table functions plus a new user-callable scan surface — same loose
policy as v0.9–v0.18 (new feature = minor).

## Validation

- Local pinned build (submodule pin v1.5.6, MSVC/Ninja `Release`): full build
  green; post-merge offline suite **54 files, 1631 assertions, 0 failures**
  (guard 28/28 functions documented).
- `clang-format` (project pin 11.0.1) clean over all touched files.
- Per-branch official CI runs, all **9/9 green**: #82 → 37382058302, #83 →
  37389982231, #84 → 37398708717. Post-merge `main` run recorded below.

### Official CI run record

`MainDistributionPipeline.yml` run
[37440051373](https://github.com/flozer/duckdb-salesforce/actions/runs/37440051373)
(2026-10-06, `workflow_dispatch` against post-merge `main` @ `7d041f9`;
release-prep commit docs-only) — **9/9 green**:

| DuckDB | linux_amd64 | windows_amd64 | osx_arm64 |
|---|---|---|---|
| v1.5.4 | Pass | Pass | Pass |
| v1.5.5 | Pass | Pass | Pass |
| v1.5.6 | Pass | Pass | Pass |

Per-branch pre-merge runs also 9/9: #82 → 37382058302, #83 → 37389982231,
#84 → 37398708717.

## Gates

- Community update: **submitted with maintainer OK** on 2026-10-06 —
  [duckdb/community-extensions#2948](https://github.com/duckdb/community-extensions/pull/2948)
  (single jump v0.18.0 → v0.19.0, \`repo.ref\` pinned to \`af02412\`). The live
  \`docs/community/description.yml\` mirror stays at v0.18.0 until that PR
  merges.

# duckdb-salesforce

A DuckDB extension that exposes Salesforce orgs as queryable SQL tables, via the
official Salesforce REST / Bulk APIs. Architectural sibling of
[duckdb-firebird](https://github.com/flozer/duckdb-firebird) — same
catalog/storage + table-function scanner + pushdown design, different backend.

> **Status: v0.1 (read-only REST) — early, not production-hardened.**
> ATTACH authenticates via OAuth, sObjects are resolved on demand, and
> `SELECT * FROM salesforce.<Object>` returns typed rows over the REST query
> API, with SOQL projection + a conservative predicate pushdown.
> **Live validation is manual-only** and must be run only against an org the
> maintainer is authorized to use; **automated CI never contacts Salesforce or
> requires secrets**. See
> [`v0.1-readonly-rest`](https://github.com/flozer/duckdb-salesforce/milestone/1)
> and the [limitations](#v01-limitations) below.

## Usage

```sql
LOAD salesforce;

-- Use a sandbox Connected App; for a sandbox add login_url 'https://test.salesforce.com'.
ATTACH 'salesforce://production' AS sf (TYPE salesforce,
    client_id 'xxx', client_secret 'xxx', refresh_token 'xxx');

SELECT Id, Name FROM sf.Account WHERE Name = 'Acme';
```

## Transport: REST vs Bulk (v0.3)

A scan runs over one of two transports, chosen by the `sf_force_transport`
setting. Both use the **same** optimised SOQL — projection + predicate pushdown
apply identically — so only the delivery mechanism differs.

```sql
SET sf_force_transport = 'rest';   -- default: lazy REST /query + queryMore
SET sf_force_transport = 'bulk';   -- force Bulk API 2.0 query job (CSV results)
SET sf_force_transport = 'auto';   -- opt-in: probe row count, pick rest/bulk
```

| | `rest` (default) | `bulk` |
| --- | --- | --- |
| Mechanism | `/query` + `queryMore` pages | create job → poll → download CSV (`Sforce-Locator` paging) |
| Streaming | lazy — stops early on small `LIMIT` | eager — whole result downloaded before the first row is emitted |
| Best for | interactive queries, small/medium results | large extractions, `CREATE TABLE AS`, `COPY` |
| 401 handling | refresh-token retry (once) | same — on create, poll, and each results page |

Notes / limitations of the `bulk` path:

- **`LIMIT` is not honoured server-side.** Bulk API 2.0 query jobs ignore SOQL
  `LIMIT`; the job still runs to completion server-side. But since v0.7 §8 the
  result **pages stream lazily**, so a small `LIMIT` stops pulling early and
  later result pages are **never downloaded** (`salesforce_query_cost()` shows a
  smaller `pages_fetched`). The job's server-side execution time is unchanged.
- **Lazy result streaming (v0.7 §8).** The job is created and polled to
  `JobComplete` in `InitGlobal` (a Bulk job must finish before results exist),
  but result CSV pages are fetched **on demand** as the scan drains them
  (following the `Sforce-Locator`) — memory no longer scales with the full
  result. `salesforce_query_cost().pages_fetched` reports the real Bulk page
  count (it was `NULL` before v0.7).
- A `Failed`/`Aborted` job, a results HTTP error, or a repeated locator each
  raise a clean, secret-free error.

### PK chunking (`sf_bulk_chunks`, v0.7 §9 — sequential)

For a large Bulk extraction, `sf_bulk_chunks = N` (default `1` = off, capped at
8) splits the scan into **N disjoint `Id` ranges**, each run as its own Bulk job
and streamed (§8). Cut 1 runs the chunks **sequentially** (real parallelism is a
follow-up).

```sql
SET sf_force_transport = 'bulk';
SET sf_bulk_chunks = 4;
SELECT * FROM sf.Account;          -- 4 jobs over Id ranges, unioned
SELECT bulk_chunks FROM salesforce_query_cost();
```

- Ranges come from a `MIN(Id), MAX(Id)` probe (one REST call) split by **uniform
  lexical interpolation**. Chunks may be **uneven or empty** (Salesforce `Id`s
  aren't uniformly dense) — coverage is exact (disjoint + exhaustive over
  `[min, max]`), balance is not guaranteed.
- **Bulk-only** — REST ignores it. The quota governor is consulted **per job**.
- **No global row order** across chunks (`ORDER BY` if you need it).
- Probe failure / empty object → falls back to a single chunk.

### Auto-selection (`'auto'`, v0.3 §2)

`'auto'` is **opt-in**; the default stays `'rest'` so interactive use is
unchanged. When set, the scan probes the row count once (a `SELECT COUNT()` REST
call — **one request, zero row egress**) and picks Bulk only for large reads:

| Signal | Decision |
| --- | --- |
| Estimated rows `> sf_auto_bulk_threshold` (default `50000`) | **Bulk** |
| Estimated rows `<= threshold` | **REST** |
| Aggregate-only scan (`COUNT(*)`, no real column projected) | **REST** (no probe) |
| Probe failed (HTTP error / no `totalSize`) | **REST** (never blocks the query) |
| `sf_auto_probe = false` | **REST** (probe skipped) |
| `sf_force_transport = 'rest'` or `'bulk'` | **forced** — no probe runs |

```sql
SET sf_force_transport   = 'auto';
SET sf_auto_bulk_threshold = 100000;   -- rows; tune the rest/bulk cutover
SET sf_auto_probe        = true;       -- false => 'auto' always uses REST

-- Why did the last scan pick what it picked?
SELECT * FROM salesforce_last_transport();   -- (transport, est_rows, reason)
```

⚠️ **`LIMIT` caveat.** DuckDB does not expose a query's `LIMIT` to a table
function, so `'auto'` cannot see it: a small `LIMIT` over a huge object may still
estimate large and pick Bulk (which over-fetches). For interactive small-`LIMIT`
reads on big objects, force `SET sf_force_transport = 'rest'`. There is **no
mid-stream escalation** — the transport is decided once, before the first row.

## Quota governor (v0.4)

> **v0.4 quota governor currently protects Bulk starts; REST scans are not
> preflight-gated.** A REST scan can still consume API calls (one per page); the
> governor deliberately does not probe `/limits` for REST so interactive flows
> stay cheap and a small query is never blocked.

Before starting a **Bulk** query job, the governor reads the org's
`GET /limits` once (cached in memory per `instance_url`, TTL-bounded — never
persisted to disk) and refuses to start the job when the remaining daily API
allocation is at/below the reserve:

> threshold = `max(sf_quota_min_remaining, sf_quota_reserve_pct% × DailyApiRequests.Max)`
> — allowed iff `Remaining > threshold`.

| Setting | Default | Meaning |
| --- | --- | --- |
| `sf_quota_enabled` | `true` | `false` → skip `/limits` entirely, never block |
| `sf_quota_enforce` | `true` | `false` → consult + report, but proceed (warn-only) |
| `sf_quota_fail_open` | `true` | `/limits` unavailable → allow; `false` → block with a clear error |
| `sf_quota_reserve_pct` | `10` | reserve % of `DailyApiRequests.Max` |
| `sf_quota_min_remaining` | `1000` | absolute floor of remaining requests |
| `sf_quota_cache_seconds` | `60` | in-memory `/limits` TTL per `instance_url` (`0` = no cache) |

```sql
SELECT * FROM salesforce_last_quota();
-- (limit_name, max, remaining, threshold, allowed, reason)
```

Notes:

- **Fail-open by default.** If `/limits` cannot be read, the Bulk job proceeds
  and the diagnostic reason is `limits unavailable -> allowed (fail-open)`. Set
  `sf_quota_fail_open = false` to harden (then it blocks with a clear error).
- **`429` vs `REQUEST_LIMIT_EXCEEDED`.** HTTP `429` is a short-term rate limit
  and is retried with backoff. `REQUEST_LIMIT_EXCEEDED` (the daily allocation)
  is **terminal** — surfaced as a clear error, never retried (it resets at the
  org's midnight).
- Errors never include a bearer token, secret, or raw response body.
- `DailyBulkV2QueryJobs` is also honoured when the org reports it.

## Query cost diagnostics (v0.4 §4)

`salesforce_query_cost()` returns a **single row** describing the **last scan**
(not a history) — what SOQL it generated, how much was pushed down, and short,
actionable selectivity guidance:

```sql
SELECT Id, Name FROM sf.Account WHERE Name = 'Acme';
SELECT * FROM salesforce_query_cost();
```

| column | meaning |
| --- | --- |
| `object` | scanned sObject |
| `soql` | the SOQL sent (your own query text — no secret) |
| `transport` | resolved `rest` / `bulk` |
| `est_rows` | `auto` row estimate (NULL if no probe ran) |
| `transport_reason` | why that transport |
| `projected_fields` / `total_fields` | projection-pushdown ratio |
| `pushed_filters` / `residual_filters` | predicates pushed to SOQL vs applied by DuckDB |
| `where_pushed` | the SOQL `WHERE` (empty → none) |
| `pages_fetched` | query pages fetched — REST `queryMore` pages, or (since v0.7 §8) Bulk `/results` pages streamed |
| `rows_emitted` | rows **delivered to DuckDB** (not rows downloaded) |
| `bulk` | Bulk transport? |
| `quota_remaining` / `quota_allowed` | governor decision (NULL when quota was not consulted, e.g. REST) |
| `guidance` | short selectivity hints (e.g. "no predicate pushed", "N filter(s) residual — over-fetch") |

It complements — does not replace — the granular `salesforce_last_soql()`,
`salesforce_last_transport()`, `salesforce_last_quota()`, and
`salesforce_last_scan_pages()` functions.

> **Last-scan, best-effort.** It reflects only the most recent scan in the
> process and is overwritten by the next one; scans are single-threaded.

## Relationship support: parent traversal (v0.6 §7)

Opt-in parent (lookup / master-detail) traversal. With `sf_relationships =
'parent'`, each **single-target** parent relationship becomes a **STRUCT column**
named by its Salesforce relationship name, so you can dot into it:

```sql
SET sf_relationships = 'parent';   -- opt-in; default 'off'
SELECT Id, Account.Name FROM sf.Contact;
-- Contact gains a column  Account STRUCT(Id, Name, ...);  SOQL uses Account.Name
```

Scope + caveats (correctness first; default `off` changes nothing):

- **Default `off`** — schema and `SELECT *` are unchanged unless you opt in.
- **Parent only, depth 1.** Grandparent (`Account.Owner.Name`), child
  subqueries, and relationship fan-out are **not** supported. Use DuckDB joins
  for child/complex cases.
- **Polymorphic relationships skipped** (e.g. `OwnerId` → User/Group): not
  expanded.
- **Over-fetch:** selecting the struct fetches **all** the parent's scalar
  fields (DuckDB doesn't expose which subfield was accessed).
- **No subfield pushdown:** `WHERE Account.Name = …` is applied residually by
  DuckDB (not pushed to SOQL) in this cut.
- A null/missing parent → null struct; a missing subfield → null.
- Describe-source only (not Tooling) in this cut; the parent describe reuses the
  per-attach cache.

## Fast schema discovery: Tooling API (v0.6 §6)

By default each sObject's schema comes from one **REST describe** (authoritative).
For orgs where you touch many tables, `sf_schema_source = 'tooling'` switches
discovery to the **Tooling API** (`FieldDefinition`), which fetches the fields of
**many objects in one query** — collapsing N describes into one/few calls.

```sql
SET sf_schema_source = 'tooling';   -- opt-in; default is 'describe'
SELECT table_name FROM duckdb_tables() WHERE database_name = 'sf';  -- warms the list
SELECT * FROM sf.Account;           -- resolves Account (+ other listed objects) in one Tooling query
SELECT calls FROM salesforce_tooling_calls();   -- proves Tooling use / batching
```

Caveats (correctness is preserved — REST describe is always the safety net):

- **Coarser types.** Tooling `DataType` is a display string (`Text(255)`,
  `Number(18,0)`, `Date/Time`, …); it is mapped best-effort. An **ambiguous /
  unmapped** type (`Formula`, `Roll-Up Summary`, unknown) makes that object
  **fall back to REST describe**. Tooling failure or an object absent from the
  result also falls back, per object.
- **Reduced pushdown.** `FieldDefinition` filterability is treated
  conservatively: a field is only pushable if Tooling marks it filterable —
  otherwise predicates stay **residual** (correct, just less pushdown).
- Compound fields are dropped, same as describe.
- Default stays `'describe'` (authoritative) — `'tooling'` is purely opt-in.

## Aggregate pushdown: COUNT (v0.5 §5)

A scan that needs only the **row count and zero real columns** — `COUNT(*)`,
`SELECT 1 FROM …`, `EXISTS`-style — runs a single `SELECT COUNT() FROM <obj>
[WHERE …]` and emits that many empty rows for DuckDB to count, instead of paging
every record. The 54k-row / 9s `COUNT(*)` becomes one cheap call.

```sql
SELECT COUNT(*) FROM sf.Account WHERE Name = 'Acme';
-- sends: SELECT COUNT() FROM Account WHERE Name = 'Acme'  (no data pages)
SELECT count_pushdown, pages_fetched, rows_emitted FROM salesforce_query_cost();
-- true, 0, <totalSize>
```

Applies only when **all** hold (otherwise the normal scan runs — always correct):

- zero real columns projected (pure row-count);
- **no residual filter** (a non-pushable predicate forces a real scan);
- the `COUNT()` probe succeeds (on failure it falls back to a full scan);
- `sf_force_transport` is not forced to `'bulk'` — a forced Bulk is **honoured**,
  not overridden (use `rest`/`auto` to get COUNT pushdown).

> **COUNT-only for now.** `COUNT(field)` (non-null count), `GROUP BY`, `SUM`,
> `AVG`, `MIN`, `MAX` are **not** pushed in this cut — they run as a normal scan
> with DuckDB aggregating locally.

## Pushdown (v0.1)

Pushdown to SOQL is a best-effort over-fetch optimisation; anything not pushed is
applied by DuckDB residually, so results are always correct.

| Operation | v0.1 |
| --- | --- |
| Projection (`SELECT a, b`) | ✅ pushed to SOQL |
| Predicate `=, <>, <, <=, >, >=` (filterable field, constant) | ✅ pushed |
| `IS NULL` / `IS NOT NULL` | ✅ pushed (`= null` / `!= null`) |
| `AND` of pushable predicates | ✅ pushed |
| `IN (...)` (constant list, up to 200 items) | ✅ pushed as a superset **prefilter**, kept residual (DuckDB refines) |
| `LIKE 'A%'` / `'%z'` / `'%m%'` | ✅ pushed as a superset **prefilter**, kept residual — Salesforce `LIKE` is case-insensitive, so DuckDB re-applies it locally to refine |
| `OR` of pushable predicates (all children safe) | ✅ pushed as superset, kept residual; mixed safe/unsafe → whole `OR` residual |
| functions / casts / `NOT` / regex / nested-unsupported | ⛔ residual (DuckDB) |
| Predicate on a non-filterable field | ⛔ residual |
| `WHERE` longer than 4000 chars | ⛔ residualised (guard) |
| `LIMIT n` | ⚠️ not pushed to SOQL (this DuckDB build exposes no LIMIT hook), **but the scan is lazy** — it streams pages and may stop before fetching later pages, so a small LIMIT no longer reads the whole object |

## Architecture

The full design (22 deliverables: auth flow, query flow, REST/Bulk/Tooling/
Metadata strategies, pushdown table, cache, C++ structure, roadmap, quotas,
risks, tests) lives in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). The v0.1
delivery track (Definition of Done, non-goals, security gate) is **Appendix C**.

## Build

```sh
git submodule update --init --recursive
make release        # or: make debug
```

The live HTTPS transport uses vendored [httplib](third_party/httplib) (MIT) +
**OpenSSL**, pulled via vcpkg manifest mode (`vcpkg.json`). On Windows without
GNU `make`, configure DuckDB directly inside a VS dev shell with the vcpkg
toolchain (`-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake`,
`-DVCPKG_TARGET_TRIPLET=x64-windows-static`) — the triplet must match DuckDB's
static (`/MT`) MSVC runtime; OpenSSL is built on first configure. TLS
certificate verification is always on; there is no insecure build flag.

Pinned via the `duckdb` and `extension-ci-tools` submodules to the
**DuckDB v1.5.3 release** (the minimum supported version):

- `duckdb` @ `14eca11bd9` (release **v1.5.3**)
- `extension-ci-tools` @ `18c54662`

DuckDB extensions are **version-locked**: a `.duckdb_extension` built against a
given DuckDB release only loads in that release. "Supporting 1.5.3 onward" means
**rebuilding + testing per release**, not shipping one universal binary.

## Supported DuckDB versions

| DuckDB release | Status |
| --- | --- |
| **v1.5.3** | ✅ baseline (committed submodule pin), build + offline suite green |
| v1.5.2 | ✅ build + offline suite green (matrix) |

The repo's `duckdb` submodule pins **v1.5.3** (`14eca11bd9`); `extension-ci-tools`
pins `18c54662`. To build/test against a matrix of releases locally (each in its
own `build/matrix/<tag>` dir, the committed pin restored at the end):

```powershell
pwsh -File scripts/build_matrix.ps1            # default: v1.5.2, v1.5.3
```

Newer releases are added to the matrix as they ship; if a future release needs
an API adaptation, that is tracked as its own issue (the baseline must stay
green). The `StorageExtension::Register` path and the `attach` +
`create_transaction_manager` dispatch requirement are verified against this
build (see `src/salesforce_storage.cpp`).

## Testing

The whole suite runs **fully offline** — every test mocks the Salesforce HTTP
layer, so CI never contacts a real org.

```sh
make test_debug              # CI path (GNU make + extension-ci-tools)
# or run the unittest binary directly against the SQL tests:
build/release/test/unittest "test/sql/salesforce_scan.test"
```

The real network paths are exercised only by the gated `*_live.test` files,
which are **skipped** unless `SF_LIVE_CLIENT_ID` / `SF_LIVE_CLIENT_SECRET` /
`SF_LIVE_REFRESH_TOKEN` are set — **never set these in CI**. As of v0.1 the
offline suite is 166 assertions across 8 files, with 4 live files skipped.

For a repeatable, secret-safe manual sandbox validation (and the criteria to
tag `v0.1.0`), see [SMOKE.md](SMOKE.md).

## v0.1 limitations

Known and intentional for this cut (see `docs/ARCHITECTURE.md` Appendix C):

- **Live validation is manual-only.** Run only against an org you are
  authorized to use; automated CI never contacts Salesforce or uses secrets.
  Not production-hardened.
- **`LIMIT` is not pushed to SOQL** (this DuckDB build exposes no LIMIT hook),
  but the **scan is lazy**: it streams query pages and may stop before fetching
  later pages, so a small `LIMIT` no longer reads the whole object. A full
  `COUNT(*)`/unfiltered scan still walks every page.
- **Object listing** is available via `duckdb_tables()` and
  `information_schema.tables` — the first listing runs one global describe
  (`GET /sobjects`, queryable objects only), cached until DETACH. **Columns are
  lazy**: `information_schema.columns` is empty for an object until it is
  referenced (then a per-object describe fills its schema). `SHOW ALL TABLES`
  does not show name-only (unreferenced) entries — use `duckdb_tables()`.
- **REST only.** No Bulk API 2.0, GraphQL, Tooling or Metadata API yet; no
  relationship/join traversal.
- **Metadata cache is in-memory only.** Each sObject's schema (describe) is
  cached per ATTACH and reused (described once per object); nothing is persisted
  to disk, and record/data are never cached. The cache is dropped on DETACH.
- **Read-only.** All catalog mutations (CREATE/INSERT/UPDATE/DELETE/...) throw.

## Community publication

Per `docs/ARCHITECTURE.md` Appendix C.5, there is **no** push, PR, tag, or
release to `duckdb/community-extensions` without explicit human go/no-go after
multi-test evidence. Development stays in this repository.

## License

See [LICENSE](LICENSE) (to be added).

# Research & Improvements

Status: living research log. **No commitments** — this captures ideas mined from
external Salesforce tooling, mapped to where they could land in
`duckdb-salesforce`, with sources for deeper follow-up.

This document complements [`ROADMAP.md`](ROADMAP.md) (what we plan to build) and
[`ARCHITECTURE.md`](ARCHITECTURE.md) (how it is built). Nothing here changes the
read-only posture or the C.5 community-publication gate; write/deploy surfaces
seen in the surveyed projects are explicitly **out of scope**.

Guiding filter for everything below: **correctness first, read-only only, keep
CI offline/secret-free.** An idea earns a place here only if it improves
performance, materialization, correctness, or UX without violating those.

## Current-state audit (re-verified 2026-06-05)

**Major update:** since the first audit the maintainer shipped v0.7→v1.x and
implemented **most** of the actionable ideas below. Re-verified against
`origin/main` (tags v0.7.0…v0.9.0 + `feat(meta) salesforce_refresh_metadata`).
The connector also **re-scoped to "bridge-first"** (see ROADMAP): it is
deliberately *not* an ETL/CDC/replication product — DuckDB owns materialization,
joins, Parquet, and persistence. That intentionally de-scopes several ideas
below (marked ⛔ out of scope).

### Delivered since first audit ✅

| Idea (this doc) | Status | Evidence on `origin/main` |
| --- | --- | --- |
| PK chunking on Bulk (A1) | ✅ v0.7 §9 | `salesforce_scan.cpp`: `sf_bulk_chunks`, Id-range chunks, MIN/MAX probe |
| Parallel scan | ✅ v0.7 §9b | per-thread `LocalState` claims chunk indices; `MaxThreads>1` on Bulk path |
| Streaming Bulk (A2) | ✅ | ROADMAP "Lazy REST and Bulk result streaming" |
| SFDX auth URL (E10) | ✅ v1.0 | `auth_source = options \| env \| sfdx_url` |
| JWT bearer flow (E11) | ✅ v1.0 | `auth_source='jwt'`, RS256-signed assertion (`salesforce_auth.cpp`) |
| `queryAll` / deleted (C7) | ✅ v0.9 §1 | `sf_query_mode = 'query' \| 'queryAll'` |
| Bulk CSV type parity (C8) | ✅ v0.9 §2 | "Bulk CSV edge-case hardening + REST/Bulk type parity" tests |
| Manual cache refresh (G14) | ✅ v1.3 | `salesforce_refresh_metadata(catalog [, object])` |
| Aggregate pushdown beyond COUNT | ✅ | `salesforce_aggregate()` — SUM/AVG/MIN/MAX + `GROUP BY` (explicit opt-in) |
| Grandparent traversal | ✅ v1.0 | `sf_relationship_depth` (depth 2) + `salesforce_relationships()` diag |

That covers **9 of my actionable recommendations**, including the #1 perf item
(PK chunking) and the headline UX item (SFDX auth URL). The first audit's "none
implemented yet" is now obsolete.

### Intentionally out of scope ⛔ (bridge-first re-scope)

The new ROADMAP explicitly excludes CDC/replication/orchestration. These doc
ideas are therefore **not** to be pursued as connector features — DuckDB does
them:

- **Vault Mode / Parquet materialization (B4)** → use DuckDB `CREATE TABLE AS` /
  `COPY ... TO 'x.parquet'`.
- **Incremental `SystemModstamp` + lookback (B5/C6)** → CDC; ROADMAP v0.9 §1
  lists "Change-data-capture / Incremental snapshot orchestration" as out of
  scope.
- **`getUpdated()`/`getDeleted()` Replication API (C8b)** → replication; out of
  scope. (A user can still `WHERE SystemModstamp > …` themselves — the connector
  just won't orchestrate it.)
- **Rate-limit early-exit + resume (D9)** → replication orchestration; out of
  scope. The quota governor (block-only) stays the connector's role.

### Still open & in-scope (opportunistic)

- **REST-vs-Bulk object/type deny-list (A3)** — correctness/reliability guard so
  `'auto'`/`'bulk'` never picks Bulk for a Bulk-incompatible object/type.
  *Unverified on `origin/main` — check before proposing.*
- **SOAP picklist/record-type enrichment (F12)** — narrow schema enrichment.
- **Confirm `__mdt` / Custom Settings (F13)** + document.
- **GraphQL (low fit)** — unchanged verdict below.

## How to read this

Each surveyed project gets an honest verdict — what is transferable, what is
not, and why. Then the **transferable ideas** are consolidated and prioritized.
External code is treated as a **reference**, never a dependency (license +
language differences); we re-implement in C++.

---

## Surveyed projects

### 1. `certinia/apex-mdapi` — Apex Metadata API wrapper

- **What it is:** a server-side **Apex** library (~8,000-line `MetadataService.cls`,
  generated from the Metadata API WSDL) that lets Apex code manipulate org
  metadata (create/update/deploy objects, fields, layouts, rules).
- **Layer mismatch:** it is write-heavy, runs *inside* Salesforce, and targets
  **configuration metadata** — opposite of our external, read-only **data**
  connector. Most of it is not transferable.
- **Transferable (narrow, schema-enrichment only):**
  - A concrete, production-validated **reference for the SOAP Metadata API
    envelope** (type/field/enum names) for the read-only calls our
    `ARCHITECTURE.md` §8 already plans (`describeMetadata`, `listMetadata`,
    `readMetadata`) — de-risks our planned "lightweight SOAP envelope builder."
  - Response shapes for `readMetadata('CustomField', …)` →
    `Picklist`/`ValueSet`/`RecordType` structs, which directly inform populating
    the planned `__sf_picklists` / `__sf_recordtypes` caches (§10). Picklist
    values are **not** on Tooling `FieldDefinition`, so this is the realistic
    source.
- **Do NOT take:** deploy/CRUD/`retrieve`-zip surface (breaks read-only + C.5);
  full SOAP stack (we need ~3–4 read calls — hand-rolled envelopes per §8).
- **Reinforces, not changes:** confirms Metadata API is SOAP/slow → keep it a
  lazy, aggressively-cached fallback exactly as designed.
- **Bonus (no Metadata API needed):** Custom Metadata Types (`__mdt`) and Custom
  Settings are SOQL-queryable like sObjects and likely already work in our
  scanner — worth confirming + documenting as explicit support.

### 2. `forcedotcom/cli` — Salesforce CLI (`sf`)

- **What it is:** a Node.js developer CLI. The GitHub repo is an issue tracker;
  real logic lives in `@salesforce/core`, `plugin-data`, `plugin-auth`, and
  `jsforce`. Mostly dev tooling (deploy/retrieve, scratch orgs, packaging) —
  irrelevant to us — **except** for auth and data.
- **Transferable (high-value UX):**
  - **🥇 SFDX auth URL interop.** Accept the single opaque string the CLI already
    produces/consumes:
    `force://<clientId>:<clientSecret>:<refreshToken>@<instanceUrl>`
    (no `https://` on `instanceUrl`; `clientSecret` may be empty). This collapses
    `client_id` + `client_secret` + `refresh_token` + `instance_url` + `login_url`
    into one field the user already knows how to generate, and carries the
    instance URL (My Domain) for free. Lands in `salesforce_auth.cpp` /
    `salesforce_config.cpp`.
  - **Read the CLI auth store** (`~/.sfdx/<username>.json`, newer encrypted
    `~/.sf/`) so `ATTACH 'salesforce://<alias>'` reuses an existing login — zero
    secrets in SQL. Caveat: the new store is encrypted (keychain / `~/.sf/key`),
    OS-specific; the auth URL is the portable first step.
  - **JWT Bearer flow** (private key + connected app, headless, no refresh token,
    no browser) — ideal for CI where a DuckDB extension runs. Already planned as
    `JwtBearerStrategy` (ARCHITECTURE v0.5); CLI/jsforce are the flow reference.
  - **`sf data export bulk` semantics** + **`jsforce.bulk2.query()` streaming** —
    reference for making our Bulk path **lazy/streaming** instead of the current
    eager `InitGlobal` fetch.
- **Do NOT take:** deploy/retrieve, scratch orgs, packaging, "being a CLI."

### 3. `Shankar-naik-123/salesforce-backup-v2` — ❌ nothing usable

- **Description vs. reality mismatch.** The repo advertises "backup Salesforce
  via Bulk API v2 → CSV," but the actual `dockerAPI/` code is the **stock
  ASP.NET Core Web API template** (`WeatherForecast.cs`,
  `WeatherForecastController.cs`, boilerplate `Program.cs`/`Startup.cs`, a
  Dockerfile). **No Salesforce code anywhere** — no auth, no Bulk client, no CSV
  logic. Treat as an abandoned/placeholder scaffold; nothing to mine.

### 4. Backup / ETL / connector ecosystem (real implementations)

Surveyed to find performance + materialization patterns with actual code:

- **`heroku/salesforce-bulk`** (Python) & **`egen/spark-salesforce`** (Scala,
  PR #15) — **PK chunking** + parallel/serial concurrency for >10M-row objects.
- **`airbytehq/airbyte` `source-salesforce`** (Python) — production incremental
  sync, rate-limit early-exit/resume, REST-vs-Bulk object deny-list, deleted
  records.
- **`graxlabs/duckdb`** — "SQL for SFDC via DuckDB": **materialize CSV → query in
  DuckDB** pattern (⚠️ repo may now be unavailable — only the summary snippet was
  retrievable; re-verify before relying on it).
- **`dlthub/dlt`** salesforce source (Python) — lean incremental + Parquet
  materialization model.
- **`neowit/backup-force.com`** (Scala) — simple incremental via
  `LastModifiedDate`.
- **Correctness gotchas:** `simple-salesforce#290` (Bulk CSV returns datetime as
  integer/epoch) and `jsforce#1546` (`bulk2.query()` `ERR_STREAM_WRITE_AFTER_END`
  on certain objects, e.g. `CampaignMember`).

---

## Consolidated transferable ideas

### A. Performance

1. **PK chunking on the Bulk path** — *highest-leverage performance item.*
   Today the Bulk path fetches **eagerly in `InitGlobal`** and does not
   parallelize. PK chunking splits an object into `Id`-range partitions
   (default 100k; range 1k–250k), each an independent batch. DuckDB table
   functions already support multi-threaded `InitLocal`, so each chunk becomes a
   parallel work unit → scales to >10M rows without serializing the download or
   blowing up memory.
   - Refs: `heroku/salesforce-bulk`, `egen/spark-salesforce#15`, Salesforce PK
     Chunking docs.
   - Lands in: `salesforce_scan.cpp` (parallel `InitLocal`), Bulk job creation.

2. **Lazy/streaming Bulk results** — stream `Sforce-Locator` pages into the scan
   instead of buffering all pages eagerly (current README-acknowledged limit).
   - Refs: `jsforce.bulk2.query()` streaming; watch `jsforce#1546` edge case.

3. **REST-vs-Bulk object deny-list** — Airbyte forces REST for objects/types
   Bulk can't handle (`base64`, `complexvalue`, ~23 named objects). Our `'auto'`
   only looks at row count; an **object/type-level** guard is a correctness +
   reliability win so `'auto'`/`'bulk'` never picks Bulk for an
   incompatible object.
   - Lands in: transport selection in `salesforce_scan.cpp` / config.

### B. Materialization

4. **Snapshot-to-local ("Vault Mode")** — the `graxlabs/duckdb` materialize-then-
   query pattern validates our planned offline catalog (ARCHITECTURE v0.8–0.9):
   Salesforce → Parquet / local DuckDB, with the metadata cache persisted so the
   catalog rebuilds with no network. PK chunking (A1) + per-chunk Parquet =
   materialization that scales.

5. **Incremental materialization keyed on `SystemModstamp`** — `dlt`/Airbyte/
   `backup-force.com` all confirm serious materialization is **incremental, not
   full-refresh**. See C5 for the correctness subtlety.

### C. Correctness

6. **`SystemModstamp` replication key + lookback window** — Airbyte re-reads a
   short window (default `PT10M`) on each incremental run because Salesforce
   `Modstamp` is **eventually consistent**; without it, edge records are missed.
   Non-obvious and essential for any incremental refresh.

7. **Deleted/archived records via `queryAll` / `isDeleted`** — our `/query`
   excludes deleted/archived rows. A faithful snapshot needs `queryAll` (recycle
   bin, `isDeleted=true`). Make it opt-in so default behavior is unchanged.

8b. **`getUpdated()` / `getDeleted()` Replication API** — the *canonical*
   Salesforce delta primitive (SOAP; some REST exposure). Returns the IDs
   changed/deleted in a timespan plus a `latestDateCovered` cursor to carry into
   the next run. This is the **only reliable way to capture hard deletes** for an
   incremental materialization — a `SystemModstamp > x` filter (C6) catches
   updates but never sees rows that vanished. Pair C6 (updates) with
   `getDeleted()` (tombstones) for a correct incremental snapshot.
   - Refs: Salesforce `getUpdated()`/`getDeleted()` SOAP docs; "Monitoring Record
     Activity via Data Replication API's" (Andy in the Cloud).

8. **Bulk CSV datetime decoding** — Bulk CSV can return datetimes as
   integer/epoch (`simple-salesforce#290`); our CSV decoder
   (`salesforce_csv` / `salesforce_value`) must handle this explicitly so the
   Bulk path matches REST. Add an offline test covering it.

### D. Resilience

9. **Rate-limit early-exit + resume** — Airbyte ends gracefully at the daily
   limit and resumes from the cursor next run. Complements our quota governor
   (which only blocks today): an incremental materialization could checkpoint and
   resume instead of failing.
   - Lands in: `salesforce_quota.cpp` + materialization state.

### E. UX (auth)

10. **SFDX auth URL in `ATTACH`** (`force://…`) — see project #2. Low effort,
    high UX, security-aligned (less raw secret handling). Optional follow-on:
    read the CLI auth store for alias-based attach.

11. **JWT Bearer flow** — headless/CI auth without a refresh token; already on
    the roadmap (v0.5) — CLI/jsforce as reference.

### F. Schema enrichment (narrow)

12. **SOAP Metadata fallback for picklists/record types** — per ARCHITECTURE §8/
    §10, with `apex-mdapi` as the envelope/response-shape reference. Lazy +
    cached only.

13. **Confirm `__mdt` / Custom Settings** are queryable through the existing
    scanner and document it.

### G. Operability / cache

14. **Manual metadata-cache refresh** — the DuckDB `postgres_scanner` exposes
    `pg_clear_cache()`; `mysql_scanner` likewise. We cache schema in-memory per
    ATTACH with no way to refresh without DETACH/ATTACH. Add the
    `salesforce_refresh_metadata('sf')` function ARCHITECTURE §10 already
    specifies (global + per-object). Cheap, high operability value; mirrors a
    proven core-extension pattern.
    - Refs: DuckDB `duckdb-postgres` (schema cache + `pg_clear_cache`).

## Evaluated but low fit

- **GraphQL API** (`/services/data/vXX.0/graphql`). *Not* aligned with our
  large-extraction core: without `upperBound` (v60.0+) a query caps at **4,000
  records**; `first` is **200–2000**; **≤10 subqueries**, each ≤2000 rows. It is
  cursor-paginated like SOQL but with tighter ceilings — strictly worse than
  REST `queryMore`/Bulk for volume. Its *only* real edge is **consolidated
  relationship fetch in one round-trip**, which could help the §7 parent/child
  traversal (e.g. reduce over-fetch, reach grandparents) — a narrow, later
  optimization, never the bulk path. Refs: `jpmonette/salesforce-graphql`;
  Salesforce GraphQL pagination/limits docs.

---

## Suggested priority (revised 2026-06-05)

The original top-5 are **done** (A1, C7/C8, E10) or **de-scoped** (B5/C6, D9 —
CDC/replication). What remains worth doing, in scope:

1. **REST-vs-Bulk object/type deny-list** (A3) — small correctness/reliability
   guard; verify whether v0.7's transport selection already covers it.
2. **Confirm + document `__mdt` / Custom Settings** (F13) — likely already works;
   cheap to validate and document.
3. **SOAP picklist/record-type enrichment** (F12) — only if real schema gaps
   surface; lazy + cached.

Everything materialization/incremental/replication-shaped is now a
**DuckDB-side** workflow (CTAS/COPY/Parquet) by design, not a connector feature.

---

## Sources

Auth / CLI:
- Salesforce CLI: <https://github.com/forcedotcom/cli>
- `plugin-auth` (SFDX auth URL): <https://github.com/salesforcecli/plugin-auth>
- SFDX auth URL format (Amplify DX): <https://dx.appirio.com/project-setup/salesforce-dx-auth/>

Metadata API:
- `certinia/apex-mdapi`: <https://github.com/certinia/apex-mdapi>

Performance — PK chunking & parallel bulk:
- `heroku/salesforce-bulk`: <https://github.com/heroku/salesforce-bulk>
- `egen/spark-salesforce` PR #15: <https://github.com/egen/spark-salesforce/pull/15/files>
- PK Chunking (Salesforce docs): <https://developer.salesforce.com/docs/atlas.en-us.api_asynch.meta/api_asynch/async_api_headers_enable_pk_chunking.htm>
- PK Chunking walkthrough: <https://developer.salesforce.com/docs/atlas.en-us.api_asynch.meta/api_asynch/asynch_api_code_curl_walkthrough_pk_chunking.htm>

Incremental / ETL / materialization:
- Airbyte Salesforce source: <https://docs.airbyte.com/integrations/sources/salesforce>
- Airbyte source docs (repo): <https://github.com/airbytehq/airbyte/blob/master/docs/integrations/sources/salesforce.md>
- `dlthub/dlt`: <https://github.com/dlt-hub/dlt>
- `neowit/backup-force.com`: <https://github.com/neowit/backup-force.com>
- `graxlabs/duckdb` (verify availability): <https://github.com/graxlabs/duckdb>
- GRAX + DuckDB write-up: <https://www.grax.com/blog/sql-and-salesforce-with-duckdb-and-grax/>

Replication / delta API:
- `getUpdated()` SOAP: <https://developer.salesforce.com/docs/atlas.en-us.api.meta/api/sforce_api_calls_getupdated.htm>
- `getDeleted()` SOAP: <https://developer.salesforce.com/docs/atlas.en-us.api.meta/api/sforce_api_calls_getdeleted.htm>
- Data Replication APIs walkthrough (Andy in the Cloud): <https://andyinthecloud.com/2016/03/26/monitoring-record-activity-via-data-replication-apis/>

GraphQL (low fit — see "Evaluated but low fit"):
- Salesforce GraphQL pagination/limits: <https://developer.salesforce.com/docs/platform/graphql/guide/paginate.html>
- `jpmonette/salesforce-graphql`: <https://github.com/jpmonette/salesforce-graphql>

DuckDB core-extension patterns (architecture reference):
- `duckdb/duckdb-postgres` (ATTACH scanner, schema cache, `pg_clear_cache`): <https://github.com/duckdb/duckdb-postgres>
- PostgreSQL extension docs: <https://duckdb.org/docs/lts/core_extensions/postgres>

Streaming & correctness gotchas:
- jsforce BulkV2 (streaming): <https://jsforce.github.io/jsforce/classes/api_bulk2.BulkV2.html>
- jsforce #1546 (`ERR_STREAM_WRITE_AFTER_END`): <https://github.com/jsforce/jsforce/issues/1546>
- simple-salesforce #290 (Bulk datetime as integer): <https://github.com/simple-salesforce/simple-salesforce/issues/290>

Surveyed, no value (recorded so we don't revisit):
- `Shankar-naik-123/salesforce-backup-v2` (stock .NET scaffold, no SF code):
  <https://github.com/Shankar-naik-123/salesforce-backup-v2>

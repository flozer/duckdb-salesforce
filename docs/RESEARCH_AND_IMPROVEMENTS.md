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

## Current implementation status

This research was reviewed after `v0.6.0`. Several ideas that were originally
future-facing are now implemented:

- REST vs Bulk transport selection exists through `sf_force_transport`
  (`rest` / `bulk` / `auto`).
- Bulk API 2.0 query jobs exist, including CSV result decoding.
- The quota governor exists for Bulk job starts.
- Query cost diagnostics exist through `salesforce_query_cost()`.
- COUNT pushdown exists for zero-column `COUNT(*)`-class scans.
- Tooling API schema discovery exists through `sf_schema_source='tooling'`.
- Parent relationship traversal exists through `sf_relationships='parent'`.
- Local DuckDB release matrix coverage exists for `v1.5.2` and `v1.5.3`.

The remaining high-value gaps are therefore mostly about **large extraction
ergonomics and repeatable materialization**, not the first read-only connector
surface.

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

---

## Suggested priority

Post-`v0.6.0`, the recommended order is:

1. **Bulk streaming + PK chunking** (A1/A2) — remove the eager Bulk fetch
   limitation, reduce memory pressure, and unlock parallel large extraction.
2. **Incremental materialization / Vault Mode** (B4/B5/C6) — local snapshot and
   repeatable refresh by `SystemModstamp` with a lookback window.
3. **Snapshot correctness** (C7/C8) — `queryAll` / deleted records and Bulk CSV
   datetime edge-case tests.
4. **Rate-limit early-exit + resume** (D9) — convert quota pressure from a hard
   stop into checkpointed, resumable materialization.
5. **Distribution hardening** — CI Win/Linux and package/release review before
   any community submission.

Lower / opportunistic: REST-vs-Bulk object deny-list (A3), SFDX auth URL (E10),
JWT Bearer (E11), SOAP picklist enrichment (F12), `__mdt` confirmation (F13).

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

Streaming & correctness gotchas:
- jsforce BulkV2 (streaming): <https://jsforce.github.io/jsforce/classes/api_bulk2.BulkV2.html>
- jsforce #1546 (`ERR_STREAM_WRITE_AFTER_END`): <https://github.com/jsforce/jsforce/issues/1546>
- simple-salesforce #290 (Bulk datetime as integer): <https://github.com/simple-salesforce/simple-salesforce/issues/290>

Surveyed, no value (recorded so we don't revisit):
- `Shankar-naik-123/salesforce-backup-v2` (stock .NET scaffold, no SF code):
  <https://github.com/Shankar-naik-123/salesforce-backup-v2>
</content>
</invoke>

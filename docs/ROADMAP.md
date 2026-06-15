# duckdb-salesforce Roadmap

Status: post-`v0.10.1` development roadmap. `v0.10.1` is the latest tagged own
release; `v0.11.0` is in preparation — it adds the v1.6 Report Bridge metadata +
explainability work (describe-validated base/token/single-hop-relationship
resolution for `salesforce_report_soql()` + structured diagnostics, Phases 1–4)
on top of the §16 Report Bridge and the `v0.9.3` datalake / range-pushdown +
Bulk-backfill hardening. `v0.11.0` is not yet tagged (pending smoke/CI review).
The approved `duckdb/community-extensions` baseline remains `v0.9.2`; the
community catalog is NOT updated until a future feature pack is bundled and an
explicit human GO is given. This file records the strategic direction after the
connector reached feature maturity, cross-platform CI, and public documentation.

The core mission is to provide the best bridge between Salesforce and DuckDB for
analytics. The extension should expose Salesforce data safely, efficiently, and
transparently. DuckDB should continue to own materialization, joins,
transformations, Parquet export, and analytical workflows.

## Guiding Principles

- Keep the connector focused on Salesforce data access for DuckDB.
- Do not turn the extension into an ETL, CDC, replication, orchestration, or
  governance product.
- Strengthen the core before adding broad new features: correctness,
  compatibility, observability, and predictable performance come first.
- Push work to Salesforce only when semantics are safe and measurable.
- Leave DuckDB-native strengths to DuckDB: `CREATE TABLE AS`, `COPY`, views,
  joins, aggregation fallback, Parquet, and local persistence.
- Keep live Salesforce tests maintainer-controlled. CI remains offline,
  mock-only, and secret-free.
- Keep `duckdb/community-extensions` blocked until the explicit C.5 human gate
  is granted.

## Post-Community Release Discipline

`v0.9.2` is the approved community baseline. Treat it as a product release, not
as a development checkpoint.

`v0.10.0` is the current own release: it adds the §16 Report Bridge on top of
`v0.9.3`. It is a normal product release of this repo and does NOT change the
community baseline — `duckdb/community-extensions` stays at `v0.9.2` until a
future feature pack is bundled, release notes and descriptor are aligned, smoke
evidence is captured, and an explicit human GO is given. Tag/GitHub Release and
Linux/Windows assets for `v0.10.0` are handled by the release-assets workflow;
only the community update remains gated.

- Keep `main` stable and releasable.
- Do roadmap/planning work in a dedicated planning branch.
- Do implementation work in short-lived feature branches, one risk area at a
  time.
- Do not reuse or move an already published release tag. `v0.9.2` is immutable;
  fixes after that baseline need a new tag/version.
- Prefer additive, opt-in surfaces for new behavior. Keep existing scan,
  authentication, transport selection, and descriptor behavior unchanged unless
  the roadmap explicitly calls for a breaking or compatibility-sensitive change.
- Before merging a feature branch into `main`, require code review, focused
  tests for the touched behavior, local build validation, and green CI.
- Create a new release tag only after the validated feature branch has merged to
  `main`.
- For every own repo release, keep a dedicated `docs/RELEASE_NOTES_vX.Y.Z.md`
  changelog, publish GitHub Release assets for Linux and Windows, confirm the
  asset names match the release version, and record the evidence before treating
  the release as complete.
- Before publishing a new community update, require release notes, version/tag
  alignment, smoke evidence, descriptor review, and an explicit human GO for the
  `duckdb/community-extensions` PR.
- Never push branches, tags, commits, releases, or pull requests to
  `duckdb/community-extensions` without explicit maintainer approval.

## Delivered Baseline

The connector already provides:

- OAuth refresh-token authentication over HTTPS.
- REST `/query` and `queryMore` scans.
- Bulk API 2.0 scans with auto/forced transport selection.
- Lazy REST and Bulk result streaming.
- Bulk PK chunking with parallel execution.
- Quota governor for Bulk starts.
- Query-cost diagnostics.
- Global object listing and metadata cache.
- REST Describe and Tooling API schema discovery.
- Parent relationship traversal, opt-in.
- Projection and predicate pushdown, including `IN`, `LIKE`, and `OR` with
  residual-safe behavior.
- `COUNT(*)` pushdown for safe zero-column scans.
- Cross-platform CI on `linux_amd64`, `windows_amd64`, and `osx_arm64` for
  DuckDB `v1.5.2` and `v1.5.3`.
- Public bilingual documentation, contribution docs, MIT license, third-party
  notices, and community descriptor draft.

## Next own-repo release candidate (unreleased on `main`)

Accumulated on `main` since `v0.12.1` (no tag yet; community candidate `v0.12.1`
stays frozen + parked on the upstream Windows CI blocker
`duckdb/community-extensions#2061`). Provisional next own-repo tag: **`v0.13.0`**
(adds a new function → minor bump). Contents:

- **`salesforce_relationship_graph(catalog, object [, max_depth])`** — §18 cut 1,
  on-demand read-only parent relationship enumerator (explicit per-edge status;
  depth `[1,4]`). Diagnostic-only.
- **Compound/address token resolver — mechanism only.** Object-keyed report-side
  normalizer + Metadata-Engine validation; **the real token map is empty**
  (entries gated on org fixtures). Not announceable as "ADDRESS2 resolved".
- **Report-type → base-object builtin map contract clarified** (§16 Phase 1.1
  cut 1) — fixture-backed, no `reportTypeMetadata` inference.

Release prep (docs/smoke) may proceed; **tagging is GO-gated**. Evidence-gated
follow-ups (new report-type pairs; real ADDRESS/PHONE token entries; cut 2
`reportTypeMetadata`) stay blocked until org data lands.

## v0.9: Salesforce API Coverage

Goal: cover important Salesforce read surfaces that improve the bridge without
creating a replication product.

### 1. `queryAll` / Archived And Deleted Records

Add an opt-in read mode that uses Salesforce `queryAll` where supported.

Scope:

- Add a setting such as `sf_query_mode = 'query' | 'queryAll'`.
- Use `queryAll` only when explicitly requested.
- Document that this exposes archived/deleted records according to Salesforce
  API semantics.
- Keep default behavior unchanged.
- Keep diagnostics clear: query mode should appear in `salesforce_query_cost()`
  or an equivalent last-scan diagnostic.

Out of scope:

- Change-data-capture.
- Replication history.
- Managed tombstone storage.
- Incremental snapshot orchestration.

Acceptance:

- Mock tests prove `queryAll` URL/path selection.
- Default `query` behavior remains unchanged.
- Deleted/archived behavior is documented as Salesforce-controlled.
- No secrets or live tests in CI.

### 2. Bulk CSV Edge-Case Hardening

Strengthen the existing Bulk CSV bridge.

Scope:

- Add focused tests for multiline strings, escaped quotes, commas, CRLF/LF,
  empty values, null handling, base64, decimals, dates, times, timestamps, and
  timezone offsets.
- Compare REST JSON and Bulk CSV decoding for representative values.
- Improve error messages for malformed CSV or unsupported typed conversion.

Out of scope:

- New Bulk write/ingest APIs.
- CSV export tooling.
- ETL-level data quality rules.

Acceptance:

- Existing REST/Bulk tests stay green.
- Edge cases have explicit regression coverage.
- Errors remain field-named and secret-free.

## v1.0: Analytical Pushdown

Goal: reduce over-fetch for common analytical queries while preserving DuckDB
fallback correctness.

### 3. `COUNT(field)` Pushdown — DEFERRED

> **Status: DEFERRED (PM decision).** `COUNT(*)` already covers the main pain.
> `COUNT(field)` cannot reuse the `COUNT(*)` zero-column trick (the scan can't
> distinguish `COUNT(field)` from `SELECT field`), so it would require a DuckDB
> **OptimizerExtension** (plan rewrite + rebinding) — new machinery for a
> marginal win over the already-correct normal-scan count. Per ACTION_GUIDE
> (strengthen simple core > add complexity), deferred until there is real pain.
> `COUNT(field)` remains correct today via the normal scan + DuckDB aggregation.

Add safe non-null count pushdown where SOQL semantics match DuckDB expectations.

Scope:

- Evaluate `COUNT(field)` for scalar fields.
- Avoid pushdown when null or type semantics are uncertain.
- Keep residual fallback for unsupported expressions.

Acceptance:

- Generated SOQL is asserted.
- Result correctness is asserted against fallback behavior.
- Unsupported shapes remain correct through DuckDB.

### 4. Simple Aggregate Pushdown — TRANSPARENT FORM DEFERRED

> **Status: TRANSPARENT pushdown DEFERRED (PM decision, feasibility-checked).**
> Transparent `MIN`/`MAX`/`SUM`/`AVG` pushdown is NOT achievable without a plan
> rewrite. DuckDB runs the aggregate in an operator ABOVE the scan, so the scan
> must project the column and sees `column_ids = {x}` — indistinguishable from a
> plain `SELECT x`. The `TableFunction` API (v1.5.3) has projection/filter/limit
> pushdown hooks but **no aggregate-pushdown hook**, and the scan cannot
> speculate (emitting a single aggregate row would corrupt a non-aggregate
> `SELECT x`). The `COUNT(*)` zero-column trick does not transfer, because
> `MIN/MAX` always project the column. Enabling it would require an
> **OptimizerExtension** (rewrite `Aggregate→Scan`) — the same machinery that
> deferred `COUNT(field)` (§3). Per ACTION_GUIDE (strengthen simple core > add
> heavy layers), deferred. `MIN/MAX/SUM/AVG` remain correct today via the normal
> scan + DuckDB aggregation.
>
> **Planned instead: an explicit, opt-in `salesforce_aggregate()` table
> function** — server-side SOQL aggregates without dragging rows down, chosen
> explicitly by the user (no pretense of transparent pushdown, no optimizer).
> Short plan pending.

Future (only if/when an OptimizerExtension is justified) — transparent form:

- Only scalar fields with safe type mappings.
- No relationship aggregate pushdown in the first cut.
- No partial or approximate results.
- Pushdown only occurs for safe fields and aggregate shapes.
- Unsupported expressions remain local in DuckDB.
- Diagnostics show pushed aggregate versus fallback.

### 5. Simple `GROUP BY`

Evaluate SOQL aggregate queries for simple grouping.

Scope:

- Single object.
- Group by scalar fields.
- No rollups/cubes in the first cut.
- Preserve DuckDB fallback for unsupported cases.

Acceptance:

- Correct result shape and names.
- Clear limitations in docs.
- No silent semantic drift.

## v1.1: Relationship Depth

Goal: expose more of Salesforce's natural object graph without turning the
connector into a join engine.

### 6. Grandparent Traversal

Extend opt-in parent traversal from one level to safe multi-level SOQL paths,
for example `Contact.Account.Owner.Name`.

Scope:

- Depth limit aligned with Salesforce SOQL rules.
- Opt-in only.
- Skip polymorphic or ambiguous relationships.
- Keep child relationships out of scope.

Acceptance:

- Default schema remains unchanged.
- Multi-level parent fields decode correctly.
- Unsupported paths fail clearly or remain unavailable.

### 7. Relationship Diagnostics

Make relationship expansion observable.

Scope:

- Show which relationships were expanded.
- Explain skipped polymorphic/unavailable relationships.
- Document over-fetch and residual predicate behavior.

Acceptance:

- Diagnostics help users understand relationship behavior.
- No behavior change for default scans.

## v1.2: Operator Experience And Auth

Goal: make the bridge easier to operate without taking over orchestration.

### 8. Auth UX Improvements

Evaluate additional ways to provide credentials safely.

Options:

- SFDX auth URL input for local developers.
- JWT Bearer flow for headless environments.
- Clearer credential validation and error messages.

Out of scope:

- Secret persistence managed by the extension.
- Browser-based OAuth flows inside DuckDB.
- CI live Salesforce authentication.

Acceptance:

- Secrets are never logged.
- Existing refresh-token flow remains supported.
- Docs explain trade-offs.

### 9. macOS Live TLS Validation Or Trust Store Support

The macOS CI proves build and offline tests. Live Salesforce TLS on macOS remains
documented but not validated.

Options:

- Add a macOS trust-store path.
- Validate the existing OpenSSL path with a maintainer-run macOS smoke.
- Keep `SSL_CERT_FILE` as the documented workaround if no code change is needed.

Acceptance:

- The chosen path is documented.
- CI remains mock-only.
- No secrets are introduced.

## v1.3: Operability And Salesforce Coverage Hardening

Goal: improve the bridge's reliability and day-to-day operability without
turning the connector into an ETL or replication system.

### 10. Manual Metadata Cache Refresh

Add an explicit cache refresh helper, similar in spirit to cache-clear functions
in other DuckDB scanners.

Candidate surface:

- `salesforce_refresh_metadata('sf')` refreshes global/object-list and schema
  cache for the attached Salesforce catalog.
- `salesforce_refresh_metadata('sf', 'Account')` refreshes one object.

Scope:

- In-memory cache only.
- No persistent cache.
- No data/record cache.
- Clear diagnostics for missing catalog or non-Salesforce catalog.

Acceptance:

- Repeated schema resolution uses cache before refresh.
- Refresh causes a new Describe/global describe on next access.
- Object-specific refresh does not clear unrelated objects.
- Offline mock tests prove describe counters before/after refresh.

### 11. REST-vs-Bulk Compatibility Guard — DONE (base64/blob, metadata-driven)

> **Status: DONE.** Cut 1 is a metadata-driven base64/blob guard (no hardcoded
> object deny-list — that drifts). **Live-confirmed premise**: Bulk API 2.0
> query CSV rejects blob fields ("Blob field not supported in Bulk V2 Query with
> CSV content type"). Implemented: a projected `base64` field (any depth,
> including a nested parent STRUCT field) makes Bulk incompatible.
> `sf_force_transport='auto'` falls back to REST with a recorded reason;
> forced `sf_force_transport='bulk'` errors clearly **before** creating a job
> (`projected base64 field 'NAME' is not supported by Bulk API 2.0 CSV`); `rest`
> is unchanged. Reason surfaced in `salesforce_last_transport()` /
> `salesforce_query_cost()`. Other Bulk-unsupported objects still surface
> reactively as a clear Bulk-job error (no deny-list, by design).

Original scope (kept for reference):

- Maintain a small documented deny-list or compatibility rule set.
- Consider field/type signals from Describe where available, for example fields
  that cannot be represented safely in Bulk CSV.
- Apply the guard to `sf_force_transport='auto'`.
- Decide whether forced `sf_force_transport='bulk'` should error clearly or
  warn and proceed; default should favor correctness.

Acceptance (met):

- Auto transport avoids the base64/blob case.
- Diagnostics explain the decision.
- Forced Bulk behavior is explicit and documented.

### 11a. REST BLOB decode for blob bodies — DONE (documented limitation)

> **Status: DONE.** Live-confirmed: a REST query returns blob BODY fields
> (`Attachment.Body`, `ContentVersion.VersionData`) as a **URL reference**, not
> inline base64. Maintainer decision: keep the field typed BLOB, do NOT switch
> base64→VARCHAR (would break inline-base64 semantics/tests), and do NOT auto-
> fetch the URL (a per-row/field call — expensive and surprising). Instead the
> scanner raises a clear, documented limitation when it sees a URL-reference
> value: "Salesforce returned a URL reference for blob/base64 field 'NAME';
> inline BLOB decoding is not supported by REST query. Select non-blob fields or
> fetch the blob URL outside the scanner." Other undecodable values get a
> secret-free generic error (field + length, never content). Inline base64 that
> REST returns inline still decodes normally. Combined with §11 (Bulk rejects
> blobs), blob BODY fields are not directly readable as bytes on either
> transport — fetch out-of-band by record Id. Docs updated EN/PT.

### 12. Custom Metadata And Custom Settings Confirmation — DONE

> **Status: DONE (confirmation, no production code).** Custom Metadata Types
> (`__mdt`) and queryable Custom Settings (`__c`, List + Hierarchy) flow through
> the existing path with no special handling: `GlobalDescribe` lists them
> (queryable filter), REST describe resolves their fields, and SELECT scans them
> like any sObject (projection + predicate pushdown apply); writes throw
> (read-only). It is data access, not the Metadata API. Visibility follows
> user/org permissions (protected components may not appear). Covered by
> test/sql/salesforce_custom_metadata.test (listing + describe + scan + read-only)
> and documented EN/PT. Mid-session schema changes: refresh via
> salesforce_refresh_metadata().

Confirm and document how Salesforce Custom Metadata Types (`__mdt`) and Custom
Settings behave through the existing scanner.

Scope:

- Verify queryability through global object listing and scans.
- Document expected naming and limitations.
- Add mock or live-smoke guidance if the behavior depends on org setup.

Acceptance:

- Docs explain whether `__mdt` / Custom Settings can be queried like other
  sObjects.
- Any limitation is visible and not left as tribal knowledge.

### 13. Bulk Datetime Epoch Hardening — DONE (contract test + docs, no code change)

> **Status: DONE.** No authoritative evidence that Bulk API 2.0 query CSV ever
> emits epoch/integer datetimes (Salesforce docs inaccessible to bots; no live
> repro). Per "don't guess units, don't change the decoder without proven pain":
> behavior is UNCHANGED. The current contract is already safe — datetime/date/
> time arrive as ISO 8601 and decode to TIMESTAMP/DATE/TIME (UTC wall-clock,
> REST + Bulk parity); a numeric/epoch value is NOT interpreted (ambiguous
> seconds vs milliseconds) and raises a clear, field-named, secret-free error
> ("field 'NAME' (Salesforce type 'datetime') could not be decoded as
> TIMESTAMP") — the value is never echoed. Locked by
> test/sql/salesforce_datetime_epoch.test (ISO decodes; epoch datetime, numeric
> DATE, numeric TIME each fail clearly; REST/Bulk parity). Docs EN/PT. If a real
> org ever returns epoch datetimes, open an issue with a structural example.

Salesforce Bulk CSV implementations and client libraries have historically
reported datetime edge cases, including epoch/integer-like values. Strengthen
the decoder if needed.

Scope:

- Add regression tests for datetime CSV values that arrive as integer/epoch-like
  cells if Salesforce can emit them for some Bulk paths.
- Preserve existing ISO datetime behavior.
- Keep errors field-named and secret-free.

Acceptance:

- ISO datetime behavior remains unchanged.
- Epoch-like datetime input is either decoded correctly or rejected clearly with
  a documented reason.
- REST JSON and Bulk CSV parity remains tested.

### 14. Narrow Metadata Fallback For Picklists And Record Types — DONE

> **Status: DONE.** Two explicit, read-only table functions, parsed from the
> REST describe (which already carries picklistValues + recordTypeInfos) — no
> Metadata API, no SOAP, no Tooling, no writes:
> - `salesforce_picklist_values(catalog, object, field)` -> value, label,
>   active, is_default (the field's FULL catalog: active + inactive; NOT
>   record-type-filtered, NO dependent-picklist resolution).
> - `salesforce_record_types(catalog, object)` -> developer_name, label,
>   record_type_id, active, is_default.
> Lazy + cached per ATTACH (raw describe cached per object, reused by both
> functions, cleared by salesforce_refresh_metadata()). Default schema/scan
> untouched. Clear errors (unknown/non-SF catalog, field-not-found);
> non-picklist field -> 0 rows. Covered by test/sql/salesforce_metadata.test;
> docs EN/PT.

Evaluate a narrow, lazy metadata enrichment path for cases where Describe or
Tooling does not expose enough detail for analytical users.

Scope:

- Read-only metadata only.
- Picklist values and record type labels are the first candidates.
- Lazy and cached per ATTACH.
- REST/Tooling/Describe remain the primary schema path.

Out of scope:

- Metadata deploy/retrieve.
- Apex Metadata API CRUD.
- SOAP write operations.
- Data catalog or governance features.

Acceptance:

- Feature is opt-in or lazy enough not to slow common scans.
- No metadata writes exist.
- Docs state exactly which metadata is enriched.

## v1.4: Datalake Seed And Backfill Hardening — DELIVERED (v0.9.3)

Goal: make large initial read loads predictable for datalake users without
turning the connector into an ETL, CDC, replication, or orchestration product.
DuckDB, dbt, and the lakehouse own materialization and incremental maintenance;
the connector owns safe, observable Salesforce reads.

### 15. Predicate Range Pushdown And Bulk Backfill Guardrails — DELIVERED (v0.9.3)

> **Status: DELIVERED in `v0.9.3`.** Exact two-sided range pushdown via
> `BoundBetween` on `CreatedDate`/`SystemModstamp` (and any field) with zero
> residual across scan, REST, Bulk, `queryAll`, and `COUNT(*)`; unsupported
> bounds (function/cast/non-literal/NULL) stay residual and correct. Configurable
> Bulk poll budget (`sf_bulk_poll_budget`) replacing the hardcoded 600; opt-in
> `sf_bulk_require_predicate` fail-fast guard; `salesforce_query_cost().bulk_polls`
> plus guidance distinguishing "Salesforce is filtering server-side" from "DuckDB
> is filtering after a full remote scan". Offline mock regression tests
> (`salesforce_range_pushdown.test`, `salesforce_bulk_guardrails.test`) and an
> EN/PT seed/backfill recipe. Merged to `main`; `v0.9.3` tag/GitHub Release
> complete with Linux and Windows assets; community baseline unaffected (stays
> `v0.9.2`). The scope/acceptance notes below are retained as the as-built
> record.

The production `Produto_Oferta__c` seed exposed a practical failure mode: a full
roughly 4M-row Bulk read hit the current Bulk polling budget, and monthly
`CreatedDate >= X AND CreatedDate < Y` windows behaved like the full scan. A
direct SOQL aggregate proved Salesforce can filter the date range quickly, and
single lower-bound predicates on `CreatedDate`/`SystemModstamp` push down, but
the two-sided range form remained residual in the table-scan path. This makes
the initial seed hard to split safely.

Scope:

- Harden predicate pushdown for exact two-sided ranges on the same Salesforce
  field, especially `CreatedDate` and `SystemModstamp`, across normal scans,
  `COUNT(*)` pushdown, REST, Bulk, and `queryAll`.
- Preserve existing semantics: push only exact ranges whose DuckDB and SOQL
  timestamp/date comparisons match; leave uncertain casts, functions, timezone
  conversions, or non-literal bounds residual.
- Add regression coverage for `field >= lower AND field < upper`,
  `field >= lower AND field <= upper`, equivalent reversed operands, and the
  aggregate/count plan shape that previously lost the pushed range.
- Ensure `salesforce_query_cost()` makes the failure mode visible: pushed range
  text, pushed/residual filter counts, transport, count-pushdown status, Bulk
  chunk count, poll count, and clear guidance when a selective-looking range
  remains residual.
- Add Bulk backfill guardrails for large read jobs: configurable poll budget or
  timeout, progress diagnostics, and a fail-fast warning when Bulk is about to
  run with no pushed predicate against a large object.
- Document a safe initial-seed pattern for datalake users: validate selectivity
  with `COUNT(*)`/`salesforce_query_cost()`, split the first load by pushed
  `CreatedDate` or `SystemModstamp` windows, write data through DuckDB/dbt to
  Parquet or lake tables, then use incremental maintenance keyed on
  `SystemModstamp`.

Out of scope:

- Connector-managed scheduling, checkpoints, CDC, retries across windows, or
  lakehouse writes.
- New Salesforce write APIs or Bulk ingest APIs.
- Guessing date windows automatically before diagnostics prove a predicate is
  pushed.
- Treating residual filters as safe for large backfills. Residual filters remain
  correct, but they are not acceptable as the only filter for a planned large
  extraction.
- Live Salesforce tests in CI.

Acceptance:

- Offline mock tests prove exact range pushdown for `CreatedDate` and
  `SystemModstamp` with zero residual filters in scan, REST, Bulk, `queryAll`,
  and `COUNT(*)` paths.
- Offline mock tests prove unsupported date expressions stay residual and keep
  results correct.
- Diagnostics show pushed range, residual count, Bulk poll count, and guidance
  that distinguishes "Salesforce is filtering" from "DuckDB is filtering after a
  full remote scan".
- A mock Bulk job can exceed the old poll count without losing progress
  visibility when an explicit higher poll budget is configured.
- Documentation includes a maintainer-reviewed seed/backfill recipe and warns
  that `salesforce_aggregate()` may validate server-side selectivity, but normal
  extraction must still prove pushdown through `salesforce_query_cost()`.
- CI remains offline, mock-only, and secret-free; live datalake smoke tests
  remain maintainer-gated.
- Default behavior remains conservative and existing successful scans keep the
  same semantics.

## v1.5: Report Bridge

Goal: help users bridge validated Salesforce reports into DuckDB workflows
without turning the connector into a report runner, ETL tool, or replication
engine.

### 16. Salesforce Report Bridge

> **Status: DELIVERED in `v0.10.0`.** Three opt-in, read-only functions:
> `salesforce_reports('sf')`, `salesforce_report('sf', id)` (tabular sample +
> reserved `__sf_report_*` diagnostics), `salesforce_report_soql('sf', id)`
> (structured ingredients + best-effort, identifier/literal-safe candidate SOQL).
> Offline mock tests + a maintainer live smoke (evidence:
> `docs/smoke/report-bridge-v0.10.0.md`).
>
> **Future enhancement — base-object mapping.** `salesforce_report_soql()`
> currently derives `base_object` from `reportMetadata.reportType.type`, which is
> the report type's internal name (e.g. `CustomEntity$…`), not the underlying
> sObject API name. This is **safe today** because an unsafe name yields
> `translatable = false`, but it makes most real reports non-translatable.
> Investigate the report `/describe` `reportTypeMetadata` (or related describe
> metadata) to map report type → actual base sObject when it can be done safely,
> raising the translatable rate without weakening the safety guards.
>
> **Phase 1.1 cut 1 DELIVERED (unreleased on `main`, post-v0.12.1) — builtin map
> contract clarified, fixture-backed.** The standard `reportType.type → base
> sObject` map (`BuiltinReportTypeObject`) is now an explicit **evidence-backed
> contract**: every entry is proven by a real report (live smoke / captured
> fixture); a type absent from it yields **no candidate** (base unresolved →
> block), never a guess. The 3 proven pairs
> (`ContactList`/`AccountList`/`OpportunityList`) are annotated with provenance;
> the structure is ready for fixture-backed additions in separate commits. This
> is **not** general `reportTypeMetadata` resolution. **Gated follow-ups (need
> org evidence):** new `reportType.type → object` pairs (separate commit +
> fixture); **cut 2** — `reportTypeMetadata` first-category unique-object
> resolution — only with a real PII-free `/describe` sample proving it
> unambiguous. `column_prefix` stays hint-only; no Metadata API.
>
> **Compound/address token resolver — MECHANISM DELIVERED (unreleased on `main`,
> post-v0.12.1).** Salesforce report builtin positional tokens (`ADDRESS2_CITY`,
> `PHONE1`, …) are not sObject field API names. A report-side, object-keyed
> normalizer maps `(base, token)` → a candidate component field, which is then
> validated against Describe via the Metadata Engine (existence + filterability
> for WHERE) before it can enter SOQL; WHERE uses the same path. **The real map
> is empty** — entries are added only with captured org fixture evidence
> (separate commit). No partial translation: unmapped / wrong-object / absent /
> non-filterable → the report blocks. **Not yet announceable as "ADDRESS2
> resolved"** until a proven map exists.

Salesforce reports are useful business-authored definitions, but they are not
SOQL queries and do not expose an underlying query. This feature should provide a
read-only bridge for report discovery, small report execution, and best-effort
SOQL reconstruction that users must validate before scaling through normal
`sf.<Object>` scans.

Scope:

- Document that report definitions can already be listed through the standard
  queryable `Report` sObject:

  ```sql
  SELECT Id, Name, DeveloperName, FolderName, Format FROM sf.Report;
  ```

- Optionally add a thin `salesforce_reports()` convenience wrapper. It lists
  report definitions, not report data.
- Add an opt-in `salesforce_report('<reportId>')` table function over the
  Salesforce Reports & Dashboards REST API
  (`GET`/`POST /services/data/vXX.0/analytics/reports/{id}`), synchronous only.
  Async `/instances` support is a future cut.
- Surface Salesforce report execution limits clearly: maximum 2,000 returned
  rows with no pagination, up to 100 columns, up to 20 custom field filters,
  roughly 500 synchronous runs per hour, and 20 concurrent synchronous runs.
- Decode only tabular report results in the first cut. These map cleanly to flat
  rows through `factMap["T!T"].rows`, `detailColumns`, and
  `reportExtendedMetadata`. Summary and matrix reports remain unsupported.
- Add a read-only `salesforce_report_soql('<reportId>')` helper built on
  `GET /services/data/vXX.0/analytics/reports/{id}/describe`. It returns the
  reliable ingredients (report type/base object, column API names, filters, and
  boolean filter logic) plus a best-effort synthesized `soql` string, a
  `translatable` boolean, and `caveats` text.
- Translate only safe shapes: single-object tabular reports, projections from
  `detailColumns`, simple comparisons (`=`, `!=`, `<`, `>`), `contains` as
  `LIKE`, `AND`/`OR` filter logic, supported Salesforce date literals, and
  Top-N as `ORDER BY` plus `LIMIT`.
- Return `translatable = false` for multi-object report types, with/without
  cross filters, summary or matrix groupings and aggregates, bucket fields,
  custom summary formulas, and formula columns.
- Keep the intended human workflow explicit: a business analyst validates the
  report in Salesforce; a data engineer executes a small ground-truth sample via
  `salesforce_report()`, inspects field API names and candidate SOQL via
  `salesforce_report_soql()`, validates the candidate against the report sample,
  then materializes at scale through normal `sf.<Object>` scans using Bulk, PK
  chunking, and pushdown.

Out of scope:

- Large extraction through the Reports API. The 2,000-row Salesforce cap makes
  `salesforce_report()` unsuitable for bulk extraction.
- Exact report-to-SOQL equivalence. The connector reconstructs a candidate SOQL;
  it does not extract a hidden query.
- Summary or matrix report execution.
- Automatic multi-object report-to-SOQL translation.
- Incremental refresh, orchestration, CDC, or replication. Users should maintain
  materialized outputs in DuckDB with `CREATE TABLE AS` plus periodic `MERGE` or
  `INSERT` keyed on `Id` or `SystemModstamp`.
- Live Salesforce tests in CI.

Acceptance:

- Offline mock tests prove report definition listing, tabular
  `salesforce_report()` fact-map parsing, and explicit surfacing of the
  2,000-row cap.
- Offline mock tests prove `salesforce_report_soql()` synthesis for a
  single-object tabular report and `translatable = false` for unsupported
  multi-object, summary, bucket, or formula shapes.
- Documentation states that candidate SOQL is best-effort and must be validated
  against an executed report sample; the connector makes no guarantee of exact
  equivalence.
- Default behavior remains unchanged. All report bridge behavior is opt-in.
- CI remains offline, mock-only, and secret-free; live tests remain
  maintainer-gated.
- The `duckdb/community-extensions` C.5 human publication gate remains in force
  for any release that includes this capability.

## v1.6: Core Metadata And Explainability

Goal: strengthen the extension core by making Salesforce metadata, relationship
choices, and SOQL planning decisions visible and testable inside DuckDB without
turning the project into a Salesforce administration, CRUD, metadata deployment,
or ETL platform.

### 17. Metadata Engine v2 — INCLUDE, NEAR-TERM

> **Partially DELIVERED (v0.11.0): Report Bridge translation validation +
> explainability.** `salesforce_report_soql()` now resolves base object, report
> tokens, and single-hop relationship fields strictly against REST Describe /
> Describe Global (queryability, field existence, filterability, relationshipName,
> single referenceTo), and exposes structured explainability columns
> (`base_object_resolved_by`, `translation_status`, `blocked_by`,
> `unresolved_columns`, `unresolved_filters`, `confidence`). No partial SOQL.
> **DELIVERED (v0.11.1): Metadata Engine v2 + analyst metadata diagnostics.** A
> shared, read-only, per-catalog metadata engine now backs both the Report Bridge
> and the diagnostics — one de-duped Describe Global (full sObject + queryable
> flag, queryable-only list derived) + per-object Describe, invalidated by
> `salesforce_refresh_metadata()`. `salesforce_report_soql()` is migrated onto it
> (zero output change). Standalone analyst table functions shipped:
> `salesforce_metadata_objects(catalog)` and `salesforce_metadata_fields(catalog,
> object)` (with `reference_to` / `picklist_values` LIST columns). Broader
> scan-planning reuse is covered by §19 (`salesforce_query_explain()`).

The existing connector already uses Describe, Describe Global, Tooling metadata,
picklist/record-type helpers, and an in-memory metadata cache. The next step is
not a broad metadata product; it is a tighter read-only metadata engine that
serves scan planning, Report Bridge translation, relationship safety, and user
diagnostics.

Accepted scope:

- Treat REST Describe and Describe Global as authoritative for queryability,
  field existence, field type, filterability, sortability, relationship name,
  reference targets, picklist values, and record-type metadata.
- Keep Tooling/EntityDefinition/FieldDefinition as optional enrichment only when
  REST Describe is insufficient or too expensive for a narrow question.
- Cache metadata per attached catalog, in memory only, with
  `salesforce_refresh_metadata()` as the invalidation path.
- Expose metadata through small, queryable table functions only when it directly
  helps analysts/debuggers understand a query or report translation decision.
- Reuse the metadata engine for `salesforce_report_soql()` so generated SOQL
  only references objects and fields validated against Salesforce metadata.

Out of scope:

- Metadata deployment, schema mutation, CRUD/write APIs, admin automation, and
  persistent metadata databases.
- Broad heuristic mappings such as `CUST_NAME -> Name` unless a future release
  proves them through official metadata plus org validation.

Acceptance:

- Object resolution proves `queryable=true` via Describe Global.
- Projected fields must exist in sObject Describe.
- Filter fields must exist and be `filterable=true`; otherwise diagnostics and
  Report Bridge return a conservative non-translatable result.
- Offline mock tests cover object missing, object not queryable, field missing,
  field not filterable, and metadata-call failure.

### 18. Relationship Resolver v2 — INCLUDE, MID-TERM / INCREMENTAL

> **Cut 1 DELIVERED (unreleased on `main`, post-v0.12.1):**
> `salesforce_relationship_graph(catalog, object [, max_depth])` — a read-only,
> on-demand **parent** relationship enumerator sourced through the shared
> Metadata Engine. One row per edge with an explicit status: `resolved`,
> `polymorphic` (all `referenceTo` targets reported, not traversed),
> `self_reference` (direct), `cyclic` (longer path), `not_queryable`,
> `not_describable`. Depth default 1, clamped `[1,4]`. Diagnostic-only — no scan
> / pushdown / Report Bridge change; distinct from the last-scan
> `salesforce_relationships()`. **Remaining (future):** child relationships,
> cardinality/junction typing, and feeding Report Bridge once proven safe.

The extension already supports opt-in parent relationship traversal and
relationship diagnostics. A full relationship graph explorer can help users, but
it must stay a read-only metadata surface and should not become an automatic
join planner.

Accepted scope:

- Derive parent relationship paths from REST Describe (`relationshipName`,
  `referenceTo`) and expose them through an explicit table function or an
  extension of existing relationship diagnostics.
- Mark polymorphic, self-referential, cyclic, unavailable, or ambiguous paths
  explicitly instead of guessing.
- Limit traversal depth by default; multi-level exploration must be opt-in and
  bounded.
- Feed Report Bridge and diagnostics with relationship metadata only after paths
  are proven safe.

Out of scope:

- Child relationship graph expansion in the first cut.
- Automatic SQL join rewriting, relationship aggregate pushdown, or hidden
  multi-object report translation.
- Cross-filter translation until semi-/anti-join limitations are studied and
  covered by mock and live evidence.

Acceptance:

- Relationship output includes source object, relationship name, target object,
  direction, cardinality/relationship type when known, and caveat/status.
- Cycles and polymorphic references are detected and reported.
- No scan behavior changes unless the user opts into relationship expansion.

### 19. Explain SOQL / Explain Salesforce — DELIVERED (v0.12.0)

> **DELIVERED (v0.12.0): `salesforce_query_explain()`.** A read-only, last-scan
> table function (no args) that explains the most recent catalog scan
> field-by-field, annotated via Metadata Engine v2: one row per projected field /
> conjunctive filter (`role` = projection|filter) plus meta rows
> (relationship|count|transport). Columns: object/field/role, resolved,
> filterable, sortable, relationship_name, reference_to, pushed, residual, a
> closed `reason` token, and guidance. It distinguishes Salesforce server-side
> filtering (`pushed`) from DuckDB residual filtering — the over-fetch warning
> sign. Diagnostic-only: zero scan behavior change, `salesforce_query_cost()`
> unchanged, returns zero rows when no scan has run, degrades (never throws) when
> metadata is unavailable. LIMIT is not diagnosable (not exposed to the table
> function) and is intentionally omitted rather than guessed. Parser-level
> `EXPLAIN SALESFORCE` syntax stays future.

The connector already exposes `salesforce_query_cost()` and related diagnostics.
The next useful step is an explicit explain surface that tells users what the
extension would send to Salesforce and why. This should start as a table
function or diagnostic helper, not as a deep integration with DuckDB's native
`EXPLAIN`, because native plan integration is higher-risk and less portable.

Accepted scope:

- Expose generated SOQL, pushed filters, residual filters, transport choice,
  count pushdown, Bulk polling budget, metadata cache hits/misses when tracked,
  Report Bridge translatability, and caveats.
- Make the output stable and structured so tests and users can reason about it.
- Prefer a function such as `salesforce_explain(...)` or a last-query diagnostic
  extension before attempting parser-level `EXPLAIN SALESFORCE` syntax.
- Keep all explain operations read-only and secret-free.

Out of scope:

- Custom SQL syntax in the first cut.
- Promising Salesforce's own optimizer plan unless the connector calls an
  official API that provides one.
- Executing candidate SOQL automatically as part of explain; validation should
  remain an explicit smoke/test workflow or a separate future function.

Acceptance:

- Offline tests prove REST/Bulk/queryAll/count/report translation decisions.
- Explain output distinguishes Salesforce server-side filtering from DuckDB
  residual filtering.
- Unsupported features produce caveats instead of optimistic claims.

### Rejected Or Deferred From This Evaluation

- **Reject for core:** metadata deployment framework, CRUD/write integration,
  Salesforce administration tooling, persistent local metadata warehouse, and ETL
  orchestration. They conflict with the read-only analytics-first mission.
- **Defer:** `translation_status = FULL | PARTIAL | DETAIL_EQUIVALENT | NONE`.
  Useful later, but the current `translatable` boolean keeps the Report Bridge
  contract small and safe.
- **Defer:** automatic candidate-SOQL verification/fingerprinting. Valuable for
  smoke tooling, but it consumes quota and should not be hidden inside
  `salesforce_report_soql()` or explain paths.
- **Defer:** summary/matrix/detail-equivalent report translation. It can be
  useful, but it must never be reported as full equivalence.

## Documentation-Only: Materialization With DuckDB

Materialization is a DuckDB workflow, not a connector feature.

The project should document patterns such as:

- `CREATE TABLE local_account AS SELECT ... FROM sf.Account`.
- `COPY (SELECT ... FROM sf.Account) TO 'account.parquet'`.
- Incremental refresh examples using `SystemModstamp`.
- User-managed checkpoint tables in DuckDB.
- dbt/Airflow/Dagster examples as external orchestration patterns.

The connector may improve the bridge that makes these workflows possible. It
should not own persistent checkpoints, scheduling, replication state, or
orchestration.

Operational quota notes for large datalake seeds:

- A recent `Produto_Oferta__c` seed check showed comfortable Salesforce quota
  headroom: `DailyApiRequests` 75,812 remaining of 141,800, Bulk API batches
  15,000 remaining of 15,000, Bulk V2 Query Jobs 9,948 remaining of 10,000, and
  Bulk file storage 965,252 MB remaining of 976,562 MB.
- A REST seed of roughly 4M rows is expected to use about 2,000 REST calls when
  paged at 2,000 rows per request, which fits comfortably inside that observed
  daily REST headroom. Treat this as a point-in-time quota check, not a static
  guarantee.
- The documented seed recipe should include a small `/limits` preflight before
  large loads. Salesforce's `GET /limits` endpoint can be used to inspect
  remaining daily REST and Bulk quotas; the check itself is not counted against
  `DailyApiRequests`.
- A reusable helper such as `scripts/salesforce/check_quota.sh` is acceptable as
  developer/operator tooling, but quota checking remains an operator preflight,
  not connector-owned scheduling or orchestration.

## Community Publication Gate

No agent may create a branch, PR, push, fork change, or other action against
`duckdb/community-extensions` without explicit human approval.

Before any community action:

- Latest tagged release points to the intended submission ref.
- CI matrix is green.
- Package/release review is complete.
- Security docs are reviewed.
- License and notices are present.
- Documentation is public-ready.
- Human maintainer gives explicit C.5 go.

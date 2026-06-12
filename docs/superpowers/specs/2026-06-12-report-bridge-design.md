# Report Bridge — Technical Design (ROADMAP §16, v1.5)

Status: IMPLEMENTED (feat/report-bridge). First cut of a read-only bridge from
Salesforce reports into DuckDB workflows. It is NOT a report runner, ETL, or
replication engine.

> **As-built notes (reconciled with the implementation):**
> - Real signatures take the attached **catalog alias** as the first argument
>   (the repo's attach-based pattern, like `salesforce_aggregate`):
>   `salesforce_reports('sf')`, `salesforce_report('sf', report_id)`,
>   `salesforce_report_soql('sf', report_id)`. (The headings below predate this
>   and omit the alias.)
> - `salesforce_report()` reserved diagnostic columns are collision-guarded: a
>   report label equal to a reserved `__sf_report_*` name falls back to the API
>   name, and duplicate labels are disambiguated with a `_N` suffix.
> - `salesforce_report_soql()` candidate SOQL is hardened (SOQL≠SQL): string
>   literals are quoted+escaped, identifiers validated, date/boolean/null values
>   and `OR`/`NOT`/grouped boolean-filter logic are left `translatable = false`.
> - The Phase-A `salesforce_report_fetch_raw` test harness was removed once the
>   three real functions covered the analytics paths end-to-end.

## Goal

Let a user discover Salesforce report definitions, execute a small report as a
**validation sample**, and obtain a **best-effort** candidate SOQL they must
validate before scaling through normal `sf.<Object>` scans. The report is the
business-authored source of truth for *what* to query; the connector helps
reconstruct an equivalent SOQL, it does not extract a hidden query.

## Salesforce APIs used

- **`Report` sObject** (queryable via normal SOQL) — for listing definitions.
- **Reports & Dashboards REST API** (synchronous only, first cut):
  - `GET`/`POST /services/data/vXX.0/analytics/reports/{id}` — run a report,
    returns `factMap`, `reportMetadata`, `reportExtendedMetadata`.
  - `GET /services/data/vXX.0/analytics/reports/{id}/describe` — report
    metadata (type/base object, columns, filters, boolean filter logic).
- API version reused from the `ATTACH` (`api_version`), same auth/refresh path
  as every other call. Async `/instances` is **out of first cut**.

## Proposed SQL functions

### 1. `salesforce_reports(catalog)` — thin discovery wrapper

Read-only table function listing report **definitions** (not data), backed by
the queryable `Report` sObject.

Columns: `Id`, `Name`, `DeveloperName`, `FolderName`, `Format`.

Equivalent raw query (documented):
```sql
SELECT Id, Name, DeveloperName, FolderName, Format FROM sf.Report;
```
No report execution, no rows, no folders API, no permissions expansion.

### 2. `salesforce_report(catalog, '<reportId>')` — synchronous tabular run (sample)

Runs the report synchronously and returns its rows, decoding **tabular** reports
only (`factMap["T!T"].rows` + `detailColumns` + `reportExtendedMetadata`).
Summary and matrix reports are out of first cut.

Row cap behavior (Salesforce caps synchronous report data at 2,000 rows, no
pagination): **return the capped sample + loud, unmissable diagnostics**.

Diagnostics ship as **reserved trailing columns**, namespaced with a
`__sf_report_` prefix so they cannot collide with real report field names
(Salesforce field/column API names never start with `__sf_report_`). Every
result row carries the same diagnostic values:

- `__sf_report_truncated` BOOLEAN — true when Salesforce did not return all data.
- `__sf_report_all_data` BOOLEAN — the API's `allData` when present (else NULL).
- `__sf_report_max_rows` BIGINT — the synchronous cap (2000).
- `__sf_report_guidance` VARCHAR — e.g. "report result is a validation sample
  only; scale via `sf.<Object>`".

The report's own columns come first (named from `detailColumns` /
`reportExtendedMetadata`); the four reserved columns are appended last. Never
silently imply completeness. No hard error by default. (A future opt-in
`sf_report_truncation = 'sample' | 'error'` setting is noted but NOT in the
first cut.)

### 3. `salesforce_report_soql(catalog, '<reportId>')` — best-effort SOQL reconstruction

Single-row table built on the report `describe`. One function, one call —
mirrors the `salesforce_query_cost()` diagnostic style.

Columns:
- `report_id` VARCHAR
- `report_name` VARCHAR
- `report_type` VARCHAR
- `base_object` VARCHAR
- `columns` LIST<VARCHAR>
- `filters` LIST<STRUCT(field VARCHAR, op VARCHAR, value VARCHAR)>
- `soql` VARCHAR  (may be NULL/partial when `translatable = false`)
- `translatable` BOOLEAN
- `caveats` VARCHAR  (must explain *why* when not translatable)

Translate only safe shapes → `translatable = true`:
- single-object tabular report type
- projections from `detailColumns`
- simple comparisons `=`, `!=`, `<`, `>`; `contains` → `LIKE`
- `AND`/`OR` boolean filter logic
- supported Salesforce date literals
- Top-N → `ORDER BY` + `LIMIT`

Return `translatable = false` (with `caveats`) for: multi-object report types,
with/without cross filters, summary/matrix groupings or aggregates, bucket
fields, custom summary formulas, formula columns, unsupported operators.

Candidate SOQL is a **validatable candidate, not an equivalence contract**: the
user must run it and compare against the `salesforce_report()` sample before
trusting it. The connector makes no claim of exact report-to-SOQL equivalence.
This caveat is restated in the acceptance criteria and the user docs, not just
here.

## Intended human workflow (documented)

1. Analyst validates the report in Salesforce.
2. Data engineer runs a small ground-truth sample via `salesforce_report()`.
3. Inspects field API names + candidate SOQL via `salesforce_report_soql()`.
4. Validates the candidate SOQL output against the report sample.
5. Materializes at scale through normal `sf.<Object>` scans (Bulk, PK chunking,
   pushdown) + DuckDB/dbt.

## Salesforce limits to surface clearly

- Synchronous report data: **2,000 rows, no pagination**.
- Up to 100 columns; up to 20 custom-field filters.
- Roughly **500 synchronous runs/hour**, **20 concurrent** synchronous runs.

## Mock tests required (offline, secret-free)

- `salesforce_reports()`: asserts generated SOQL against `Report`; docs state it
  lists definitions, not data.
- `salesforce_report()`: tabular `factMap` parsing → flat rows; `truncated` /
  `all_data` / `max_rows` surfaced; explicit 2,000-row cap case.
- `salesforce_report_soql()`: synthesis for a single-object tabular report;
  `translatable = false` + caveats for multi-object, summary, bucket, formula
  shapes.
- Reuse the scripted mock HTTP client; add analytics endpoints + a `Report`
  describe/run fixture sequence. No live calls, no secrets in CI.

## Quota / permission risks

- Reports API synchronous run limits (~500/hr, 20 concurrent) are **separate**
  from REST `/query` quota; a tight loop of report runs can exhaust them fast.
  `salesforce_report()` is sample-only by design, which bounds this.
- Report execution consumes API calls; `describe` is comparatively cheap.
- The running user needs access to the **report and its folder**, and the org
  must have the Analytics API enabled. Permission failures must surface as
  clear, secret-free errors (consistent with existing error handling).
- Largest correctness risk — a user mistaking the 2,000-row sample for the full
  dataset — is mitigated by the loud truncation diagnostics + docs.

## Minimal live smoke (maintainer-gated, creds loaded)

1. List definitions via `salesforce_reports()` (or `sf.Report`).
2. Run one small known **tabular** report via `salesforce_report(id)`; compare
   row count + a couple of values against the Salesforce UI.
3. `salesforce_report_soql(id)` on the same report; eyeball candidate SOQL +
   `translatable`; run that SOQL via `sf.<Object>` and compare to the sample.
4. Confirm a known large report returns `truncated = true` at 2,000 rows.

## Acceptance criteria (first cut)

Objective, offline-verifiable unless noted:

- A simple **tabular** mock report returns its rows through `salesforce_report()`.
- A mock report exceeding 2,000 rows returns the capped **sample** with the
  reserved diagnostic columns set (`__sf_report_truncated = true`,
  `__sf_report_max_rows = 2000`, guidance present) — never a silent full-result
  claim.
- `salesforce_report_soql()` returns `translatable = false` with explanatory
  `caveats` for summary, matrix, multi-object, bucket-field, and formula-column
  reports; `translatable = true` with candidate `soql` for a single-object
  tabular report.
- Docs and `caveats` state the `soql` is a **validatable candidate, not an
  equivalence contract** — the user validates it against the report sample.
- `salesforce_reports()` lists definitions (not data); its generated SOQL
  against the `Report` sObject is asserted.
- **No live Salesforce tests in CI**; CI stays offline, mock-only, secret-free.
- **No community update**; Report Bridge stays out of any release/community pack
  until explicit approval. All behavior is opt-in; default behavior unchanged.

## Out of scope (first cut)

- Summary/matrix report execution.
- Async `/instances` report runs.
- Automatic multi-object report→SOQL translation.
- Large extraction through the Reports API (the 2,000-row cap makes it unfit).
- Incremental refresh, scheduling, CDC, replication.
- Live Salesforce tests in CI; community publication remains C.5-gated.

## Default behavior

All report-bridge behavior is opt-in via the new functions. No existing scan,
auth, transport, or descriptor behavior changes.

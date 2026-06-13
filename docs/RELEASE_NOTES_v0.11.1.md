# duckdb-salesforce v0.11.1

Own-repo release on top of `v0.11.0` (`7ea8225`). Scope: **Metadata Engine v2**
(ROADMAP v1.6 §17, Phases A+B+C+D) — a shared, read-only, per-catalog metadata
cache plus two read-only diagnostic functions. Internal plumbing + diagnostics
only: **zero scan behavior change, zero `salesforce_report_soql()` output
change**. The approved `duckdb/community-extensions` baseline remains `v0.9.2` —
community is **not** updated.

## Highlights

### Metadata Engine v2 (shared, per-catalog, read-only)

A single metadata path now backs both the Report Bridge and the diagnostic
functions:

- **One engine per attached catalog** (per org / per `ATTACH`); never shared
  across orgs. In-memory only — no Tooling API, no Metadata API, no persistence.
- **De-duplicated Salesforce calls.** Describe Global is fetched once and reused;
  per-object Describe is memoized by object name. A cache hit issues no
  Salesforce call.
- **Single source of truth for the global list.** The full global describe
  (every sObject + its `queryable` flag) is cached once; the queryable-only name
  list used by scan planning and `report_soql()` is *derived* from it — same
  result as before, one Describe Global.
- **`report_soql()` migrated onto the engine** (Phase B): the duplicated
  resolution logic was removed; output is byte-for-byte unchanged.

### New read-only diagnostic functions

- **`salesforce_metadata_objects(catalog)`** → `(object_name VARCHAR,
  queryable BOOLEAN)`. One row per global sObject, exposing the real `queryable`
  flag (non-queryable objects included). Sourced through the shared engine.
- **`salesforce_metadata_fields(catalog, object)`** → `(object_name, field_name,
  type, filterable, sortable, relationship_name, reference_to LIST<VARCHAR>,
  picklist_values LIST<VARCHAR>)`. One row per field. `reference_to` comes from
  the Describe `referenceTo` (polymorphic targets listed, not resolved);
  `picklist_values` parses `picklistValues[*].value`. A field with no targets /
  no picklist yields an **empty list, never NULL**.

### Cache invalidation

- **`salesforce_refresh_metadata(catalog[, object])`** is the invalidation
  contract. With an object it drops that object's Describe; without one it drops
  the global list + every object Describe. The next read re-fetches.

## Properties (by design)

- **Read-only.** These functions only describe schema; they never read or print
  record data.
- **Zero scan behavior change.** `sf.<Object>` scans, pushdown, and catalog
  build are unaffected — they keep using the existing (untouched) global-describe
  path.
- **Zero `report_soql()` output change.** Migration onto the shared engine is
  result-identical; the offline suite asserts the unchanged output.
- **Per-catalog cache.** Bound to the `ATTACH` alias / org; a fresh `ATTACH`
  gets a fresh engine.

## Changes since v0.11.0

- `feat(metadata)`: Phase A shared engine + `salesforce_metadata_fields` (`6a95e18`, `78c5f71`).
- `feat(report)`: Phase B `report_soql()` migrated onto the shared engine (`68b423f`, `5c3a957`).
- `feat(metadata)`: Phase C `reference_to` + `picklist_values` LIST columns (`1b96fe2`).
- `feat(metadata)`: Phase D `salesforce_metadata_objects(catalog)` + `global_infos_` single source (`cd6d398`).

## Evidence

- Offline mock suite green (full `*salesforce*`: 2452 assertions, 0 fail; 8 live
  tests maintainer-gated/skipped in CI).
- Live maintainer smoke (PII-free): `docs/smoke/metadata-engine-v0.11.1.md` —
  `salesforce_metadata_objects` / `salesforce_metadata_fields` /
  `salesforce_refresh_metadata` against a real org, schema metadata only, no
  record rows, no secrets.

## Gates

- Tag/GitHub Release and Linux/Windows assets are produced by the release-assets
  workflow on the `v0.11.1` tag — **only on explicit maintainer GO**.
- No community update; `docs/community/description.yml` stays `0.9.2`.

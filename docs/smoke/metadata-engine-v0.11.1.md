# Metadata Engine v2 — live smoke evidence (v0.11.1 candidate)

Live maintainer smoke of Metadata Engine v2 (ROADMAP v1.6 §17, Phases A+B+C+D)
against a real org, using the locally-built Release shell — not the community
extension. PII-free: **schema metadata only** (object names, field names, types,
queryable/filterable flags, reference targets, picklist values). No record data
is selected or printed; no secrets are printed.

Runner: `scripts/run_smoke_metadata_v0.11.1.ps1`.

## Status: PENDING live run (maintainer-gated)

No live credentials were present in the prep environment
(`SF_CLIENT_ID`/`SF_CLIENT_SECRET`/`SF_REFRESH_TOKEN` unset), so the live run is
deferred to the maintainer. The runner's credential preflight was exercised and
blocks cleanly (`BLOCKED: missing credentials`, exit 3) without printing any
secret. To produce evidence:

```
$env:SF_CLIENT_ID=...; $env:SF_CLIENT_SECRET=...; $env:SF_REFRESH_TOKEN=...
pwsh -File scripts/run_smoke_metadata_v0.11.1.ps1
```

Then paste the PII-free output into the sections below and flip Status to PASS.

## Run

| Field | Value |
|---|---|
| Timestamp | _pending_ |
| Git commit | `<short sha>` (docs/v0.11.1-prep) |
| Shell | `build/release/duckdb.exe` (local Release; **build via `shell` target**) |
| Extension | statically linked local artifact — NOT community |
| Org / login_url | _pending_ (env auth) |
| Object tested | _pending_ (auto: first queryable, or `SF_METADATA_OBJECT`) |

## Expected result: PASS (functions execute; metadata correct)

### 1. `salesforce_metadata_objects('sf')` — object inventory
- Counts: `total_objects`, `queryable_objects`, `non_queryable_objects`
  (expect both queryable=true and queryable=false present in a real org).
- Sample (first N): `object_name, queryable` — schema names only.

_paste output_

### 2. `salesforce_metadata_fields('sf', '<object>')` — field metadata
- Counts: `total_fields`, `filterable_fields`, `reference_fields`,
  `picklist_fields`.
- Sample (first N): `field_name, type, filterable, sortable, relationship_name,
  reference_to, picklist_values`. Reference fields show their `referenceTo`
  targets (polymorphic listed, not resolved); picklist fields show allowed
  values; non-list fields show `[]` (empty, never NULL).

_paste output_

### 3. `salesforce_refresh_metadata('sf')` — invalidation
- Returns `catalog, scope, object` (catalog-wide refresh: drops global + every
  object Describe).

_paste output_

### 4. Re-read after refresh — proves re-fetch works
- `salesforce_metadata_objects('sf')` count after invalidation matches step 1.

_paste output_

## What this proves

- Describe Global is de-duped behind the shared engine; `metadata_objects` and
  `metadata_fields` read through one per-catalog cache.
- `refresh` invalidates and the next read re-fetches without error.
- All output is read-only schema metadata — no record rows, no secrets.

## Gates

No tag, no release, no community update. `docs/community/description.yml` stays
`version: 0.9.2`. No PII recorded.

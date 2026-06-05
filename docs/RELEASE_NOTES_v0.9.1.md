# Release notes — v0.9.1 (DRAFT — NOT TAGGED)

> **Status: DRAFT.** Not tagged, not published. No remote CI run, no GitHub
> release, nothing submitted to `duckdb/community-extensions`. v1.0.0 is reserved
> for the post-C.5 / "stable public API" milestone — this cycle is hardening plus
> useful metadata-driven functions, so it lands as **v0.9.1**. Tag only after a
> maintainer smoke/validation (see *Light smoke*).

Range: `v0.9.0..HEAD`.

---

## Highlights

The v1.3 "Operability and Salesforce coverage hardening" cycle: a manual metadata
cache refresh, two read-only metadata functions (picklist values, record types),
a Bulk/blob compatibility guard, and documented contracts for blob bodies and
datetime — all bridge-first, read-only, **no Metadata API**.

## New features

- **`salesforce_refresh_metadata(catalog [, object])`** (`f8fc24c`) — clear the
  attached catalog's in-memory metadata cache so the next reference re-describes.
  Object omitted = global (listing + all schemas + parent + describe caches); a
  named object clears only that object. In-memory only; no network call itself.
  Returns one row: `catalog`, `scope` (global/object), `object`.
- **`salesforce_picklist_values(catalog, object, field)`** (`310fe12`) — one row
  per picklist value: `value`, `label`, `active`, `is_default`. The field's FULL
  catalog (active + inactive); not record-type-filtered; no dependent-picklist
  resolution.
- **`salesforce_record_types(catalog, object)`** (`310fe12`) — one row per record
  type: `developer_name`, `label`, `record_type_id`, `active`, `is_default`.
  - Both parse the REST describe (which already carries `picklistValues` +
    `recordTypeInfos`) — no Metadata API, no SOAP, no Tooling, no writes. Cached
    per ATTACH (raw describe per object, reused by both, cleared by
    `salesforce_refresh_metadata()`). Default schema/scan untouched.

## Hardening / documented contracts

- **Bulk base64/blob guard** (`3973724`) — live-confirmed that Bulk API 2.0 query
  CSV rejects blob fields. A projected `base64` field (any depth) makes Bulk
  incompatible: `auto` stays on REST (recorded reason); forced `bulk` errors
  clearly before any job; reason in `salesforce_last_transport()` /
  `salesforce_query_cost()`.
- **REST blob-body limitation** (`2f1ca0d`) — REST returns blob bodies
  (`Attachment.Body`, `ContentVersion.VersionData`) as a URL reference, not inline
  base64. The scanner does not follow it; it raises a clear, documented error.
  Combined with the Bulk guard: blob bodies are not directly byte-readable on
  either transport — fetch out-of-band by record Id. Inline base64 still decodes.
- **Custom Metadata / Custom Settings confirmed** (`0f99071`) — `__mdt` and
  queryable Custom Settings (`__c`, List + Hierarchy) flow through as ordinary
  read-only sObjects (listing + describe + scan). Data access, not the Metadata
  API; visibility per permissions. No production code — confirmation + tests + docs.
- **Datetime ISO / epoch contract** (`0d1604b`) — datetime/date/time are ISO 8601
  on both transports (UTC wall-clock; REST/Bulk parity). A numeric/epoch value is
  NOT interpreted (ambiguous s-vs-ms) — clear, field-named, secret-free error
  rather than a bogus timestamp. No behavior change; contract locked by test.
- **EnvLookup fix** (`f0f16ad`) — an empty `sf_mock_env` (its default) now falls
  through to the real OS environment; previously it silently broke env / sfdx_url
  / jwt auth outside the offline tests (surfaced by the v0.9.0 live smoke).

## Release prep carried in this range

- Community submission package refreshed to the `v0.9.0` ref + green CI evidence
  recorded (`1316518`, `c9eb1b1`). Roadmap re-aligned bridge-first (`da9c52e`).
  Still **blocked by C.5** — nothing submitted.

## Test evidence (offline)

- **Offline mock suite: 34 test files, 921 assertions — green** (local Windows,
  Release build, DuckDB v1.5.3 pin). No Salesforce contact, no secrets.
- New/updated tests this cycle: `salesforce_refresh_metadata`, `salesforce_metadata`
  (picklist + record types), `salesforce_bulk_guard`, `salesforce_blob_limitation`,
  `salesforce_custom_metadata`, `salesforce_datetime_epoch` (+ migrated
  `salesforce_bulk` / `salesforce_bulk_csv`, extended `salesforce_auth_source`).
- Remote CI is manual-only and was **not** run for this draft.

## Light smoke (optional, pre-tag)

Run against a real org before tagging. Harness:
`scripts/run_smoke_v0.9.1.ps1` + `scripts/smoke_v0.9.1.sql` (env auth, secret-free).

- [ ] `salesforce_picklist_values()` on a real picklist field (e.g. `Account.Industry`).
- [ ] `salesforce_record_types()` on an object that has record types.
- [ ] `salesforce_refresh_metadata()` (object + global) returns its row.
- [ ] a normal REST `SELECT` still works.
- [ ] a normal Bulk `SELECT` (non-blob fields) still works.
- [ ] no token / secret in any output.

## Community status

**Blocked by C.5 (explicit human GO).** Nothing prepared as a branch or PR in
`duckdb/community-extensions`. See `docs/PRE_COMMUNITY_CHECKLIST.md`.

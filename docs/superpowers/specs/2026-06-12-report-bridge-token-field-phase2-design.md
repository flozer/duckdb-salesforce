# Report Bridge v1.6 — Phase 2: Report Token → Field API Name (design)

Status: DESIGN ONLY. No C++ in this branch. Scope: `salesforce_report_soql()`
column/filter **token → field API name** resolution on the validated base
object. Builds on Phase 1 (base resolver). Relationship traversal stays Phase 3.

## Goal

Report column/filter tokens are report-internal (`FIRST_NAME`, `EMAIL`,
`CREATED_DATE`), not SOQL field API names (`FirstName`, `Email`, `CreatedDate`).
Phase 2 resolves each token to a **real field that exists on the validated base
object's Describe**, so a simple single-object tabular report can become
`translatable=true`. Never guesses a name that is not in the Describe.

## Precondition

Phase 2 runs only when Phase 1 resolved the base object via a **strong source**:
`custom_entity_suffix` or `builtin_report_type_map`. `column_prefix` remains
hint-only and continues to force `translatable=false` (it cannot prove the report
root), so token resolution is not attempted for it.

## Resolution sources (per token, in order)

A candidate field name is produced, then **must be confirmed to exist** on the
base sObject Describe (case-insensitive). First confirmed candidate wins.

1. **`reportExtendedMetadata.detailColumnInfo`** — use `label`/`dataType` as
   context only. Not authoritative for the API name on its own unless it carries
   a clear field reference; do not trust it alone to invent a name.
2. **sObject Describe match** (conservative):
   - exact token == field name;
   - case-insensitive token == field name;
   - simple `UPPER_SNAKE` → `PascalCase` normalization, then case-insensitive
     match: split on `_`, capitalize each part, join
     (`FIRST_NAME`→`FirstName`, `CREATED_DATE`→`CreatedDate`, `EMAIL`→`Email`).
3. **Small builtin token map** (fixture-backed, for common irregulars):
   - `ID → Id`, `NAME → Name`, `FIRST_NAME → FirstName`, `LAST_NAME → LastName`,
     `EMAIL → Email`, `PHONE → Phone`, `CREATED_DATE → CreatedDate`.
   - `LAST_UPDATE → LastModifiedDate` **only if confirmed in a real fixture** —
     do not invent.
   - Every mapped target must still exist on the Describe.

If no source yields a field that exists on the Describe → unresolved.

## Rules

- All **projected** columns must resolve and exist on the Describe.
- All **filter** fields must resolve, exist, and be `filterable = true`.
- Any unresolved token → `translatable = false`, `soql = NULL`, caveat naming the
  token (e.g. "report token 'WEIRD_TOKEN' did not resolve to a field on
  'Contact'"). No partial translation.
- The emitted SOQL uses the **resolved API names** (`SELECT FirstName, LastName
  FROM Contact`); the structured `columns`/`filters` ingredients keep the
  original report tokens.
- No relationship resolution this phase. A dotted token whose prefix is NOT the
  base object (`ACCOUNT.NAME` on base `Contact`) → unresolved → Phase 3.
- Optional, only if cheap: a dotted token whose prefix **equals the base object**
  (`Contact.FIRST_NAME` on base `Contact`) may strip the prefix and resolve the
  remainder as a normal token. If it adds churn, defer to Phase 3.
- No schema change; provenance/unresolved reasons go in `caveats`. Resolver kind
  per token (`resolved_by`) is deferred to Phase 4.

## TDD cases (mock-first)

1. `ContactList`→`Contact`; columns `FIRST_NAME`,`LAST_NAME`; Describe has
   `FirstName`,`LastName` → `translatable=true`, SOQL `SELECT FirstName, LastName
   FROM Contact`.
2. filter `EMAIL equals 'x'`; `Email` exists + filterable → `WHERE Email = 'x'`.
3. unknown `WEIRD_TOKEN` → `translatable=false`, `soql=NULL`, caveat names it.
4. token resolves but the filter field is `filterable=false` → `translatable=false`.
5. `ACCOUNT.NAME` on base `Contact` (relationship) → unresolved → Phase 3
   (`translatable=false`).
6. `Contact.FIRST_NAME` (prefix == base) → resolves to `FirstName` (if the
   optional same-base prefix-strip is implemented; else documented as Phase 3).

## Acceptance

- The real `ContactList` smoke advances past the base-object block to resolving
  simple `Contact` fields. If it still blocks because of `Account.*` relationship
  columns, that is **expected → Phase 3**.
- Offline mock + full `*salesforce*` green. No live tests in CI.

## Out of scope (Phase 2)

- Relationship/`__r`/dotted-not-base tokens (Phase 3).
- `resolved_by`/`confidence` columns (Phase 4).
- Broad/auto token maps. New tag/release/community. `description.yml` stays
  `0.9.2`.

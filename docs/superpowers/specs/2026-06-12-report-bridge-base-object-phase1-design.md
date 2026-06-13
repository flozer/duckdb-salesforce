# Report Bridge v1.6 — Phase 1: Base-Object Resolver (design)

Status: DESIGN ONLY. No C++ in this branch. Scope: `salesforce_report_soql()`
base-object resolution only. Removes the base-object block for `translatable`;
field/filter validation (Phase 2/3) is unchanged. Conservative: better an honest
`translatable=false` than a wrong SOQL.

## Goal

Today `base_object` = `reportType.type` verbatim, so standard
(`ContactList`) and custom (`CustomEntity$X`) report types fail Describe-Global
validation and force `translatable=false`. Phase 1 resolves the report type to a
real, queryable sObject through a small, ordered set of safe sources. It does
**not** touch token→field mapping (Phase 2) or relationships (Phase 3).

## Source priority (highest → lowest)

Each candidate must pass `IsSafeIdentifier()` **and** exist as queryable in
`GlobalDescribe()`. First source that yields a validated object wins.

1. **`custom_entity_suffix`** — `CustomEntity$X` → `X`.
   - e.g. `CustomEntity$et4ae5__IndividualEmailResult__c` → `et4ae5__IndividualEmailResult__c`.
   - Accept only if `X` is a safe identifier and queryable in Describe Global.
   - `resolved_by = custom_entity_suffix`.

2. **`builtin_report_type_map`** — small, explicit, fixture-backed map for
   standard report types. Start minimal:
   - `ContactList → Contact`
   - `AccountList → Account`
   - (`OpportunityList`/`Opportunity → Opportunity` only if confirmed in mock/live)
   - Every mapped target is still validated against Describe Global. No broad map.
   - `resolved_by = builtin_report_type_map`.

3. **`column_prefix`** (weak fallback) — dotted prefix shared by columns/filters.
   - Accept only if there is a **single dominant** prefix and it is queryable.
   - Mixed/ambiguous prefixes → reject.
   - Lower confidence than the two sources above.
   - `resolved_by = column_prefix`.

If none resolves → `translatable=false`, `soql=NULL`, caveat
"base object could not be resolved safely".

**`reportTypeMetadata`** — NOT a dependency in Phase 1. Add a spike/mock to
capture whether the report `/describe` payload exposes a clear base-object field;
if it does, promote to a higher-priority source as **Phase 1.1**. Phase 1 does
not block on it.

## Output shape (no schema change in Phase 1)

Keep the current columns. Do not add a column now (minimize surface/churn).
Carry the resolution provenance in `caveats`, e.g.:

- `base object resolved from CustomEntity suffix`
- `base object resolved from builtin report type map: ContactList -> Contact`
- `base object resolved from column prefix (low confidence)`
- `base object could not be resolved safely`

`base_object` reflects the resolved sObject when found (else the raw report-type
string, with `translatable=false`). Dedicated `base_object_resolved_by` /
`base_object_confidence` columns are deferred to **Phase 4 (Explainability)**.

## Safety rule (unchanged invariant)

Resolving the base object **only removes the base block**. `translatable=true`
still requires ALL existing gates to pass:

- every projected field exists on the base sObject Describe;
- every filter field exists AND is `filterable=true`;
- only safe operators; safe (`AND`-only / in-range) boolean filter;
- no cross filters; TABULAR only.

A resolved base must never bypass field/filter validation.

## TDD cases (mock-first)

1. `ContactList` + Describe Global has `Contact` queryable → `base_object=Contact`
   (resolved via builtin map); translatable depends on remaining gates.
2. `ContactList` + `Contact` absent/not queryable → `translatable=false`,
   `soql=NULL`.
3. `CustomEntity$Account__c` + queryable → `base_object=Account__c` accepted.
4. Unknown standard type, no dotted prefix → honest reject (`translatable=false`).
5. Single validated dotted prefix → base accepted via `column_prefix`.
6. Mixed/ambiguous dotted prefixes → reject.
7. Resolved base does NOT bypass field/filter validation — a resolved base with a
   missing/non-filterable field still yields `translatable=false` (guards intact).

## Acceptance

- The earlier live smoke report (`ContactList`) must **stop failing with "base
  object not found"**: base resolves to `Contact` (if queryable). If it still
  yields `translatable=false` because tokens like `FIRST_NAME` don't resolve on
  `Contact`, that is **expected** and belongs to Phase 2 (token→field).
- Offline mock + full `*salesforce*` green. No live tests in CI.

## Out of scope (Phase 1)

- Token→field mapping (Phase 2), relationship fields (Phase 3),
  explainability columns (Phase 4).
- `reportTypeMetadata` as a dependency (spike only; possible Phase 1.1).
- Any broad/auto report-type map. New tag/release/community.

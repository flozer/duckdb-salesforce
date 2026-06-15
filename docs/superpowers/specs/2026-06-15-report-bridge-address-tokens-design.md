# Report Bridge — compound/address token resolver — design (v1.6, §16 follow-up)

Status: **DESIGN ONLY — no code.** Awaiting GO for TDD.
Date: 2026-06-15. Base: `main` = `3bc9071`.

## Objective

Resolve the **specific, real, observed** Salesforce report builtin column tokens
for compound/address and phone fields — e.g. `ADDRESS2_CITY`, `ADDRESS2_STREET`,
`PHONE1`/`PHONE2`/`PHONE3` — to real sObject field API names in
`salesforce_report_soql()`, **validated against the Metadata Engine**, so a
report that today blocks on these tokens can translate. **Narrow**: only tokens
proven by a real report fixture. **No partial translation, no guessing.**

## The problem (grounded)

- `salesforce_report_soql()` reads `reportMetadata.detailColumns` (token strings)
  and resolves each via `engine.ResolveField` (as-is / `BuiltinReportToken` /
  `UPPER_SNAKE→PascalCase`), confirmed against Describe.
- Compound/address report columns are **positional builtin tokens**, not field
  API names: `ADDRESS2_CITY`, `PHONE1`, … `NormalizeSnakeToken("ADDRESS2_CITY")`
  → `Address2City`, which is not a real field → unresolved → the whole report
  blocks (`translation_status=none`).
- `reportExtendedMetadata.detailColumnInfo[<token>]` exposes only a human
  **label** (e.g. "Mailing City") + a `dataType` — **not** the underlying field
  API name.
- The token is **ambiguous by itself**: `ADDRESS2_*` does not name which compound
  address (Mailing vs Other vs Billing/Shipping); the mapping is **object- and
  position-specific** and only knowable from the real report.

## Approach (recommended): fixture-backed, object-aware token map → validate

A small, explicit map from a **builtin report token** to a **component field API
name**, keyed by base object (or `*` when universal), applied as a new candidate
step **before** the existing `ResolveField` fallback, then **validated against
Describe** exactly like every other token:

```
ResolveReportToken(base_object, token) -> candidate field API name (or "")
   e.g. (Contact, ADDRESS2_CITY) -> "OtherCity"      // from the real fixture
        (Contact, ADDRESS2_STREET) -> "OtherStreet"
        (*,       PHONE1)          -> "Phone"          // if universal & fixture-proven
   then engine.ResolveField(base, candidate) MUST confirm it exists
   (and filterable=true when the token is used in a WHERE filter).
```

- The map is **populated only from real report fixtures** — every entry is
  backed by a captured report where we know the actual target field. **Not a
  giant generated table.**
- A token with **no map entry**, or whose mapped field **fails Describe
  validation**, does **not** translate → the report blocks
  (`translation_status=none`, `blocked_by=projected_field` for a column /
  `filter_field` for a filter). Same conservative contract as today.
- Where it lives: a **Report-Bridge-side** normalizer (report.cpp) that produces
  the candidate field name, then hands it to the generic
  `engine.ResolveField` for authoritative validation. Keeps the Metadata Engine
  generic (no report-token vocabulary leaks into it).

### Why not label-matching

Matching `detailColumnInfo` label "Mailing City" to a Describe field `label`
"Mailing City" is locale-dependent and fuzzy — rejected (would be a guess). The
fixture-backed API-name map is deterministic and auditable.

## Conservative rules (unchanged from §16 Phase 4)

- **No partial SOQL.** Any unresolved/ambiguous column or filter →
  `translatable=false`, `soql=NULL`, `translation_status=none`,
  `blocked_by=projected_field|filter_field`, with the token listed in
  `unresolved_columns`/`unresolved_filters`.
- WHERE tokens must resolve to a **filterable** field; address components that
  are not filterable block the filter.
- `confidence` stays `1.0` only on full translation, else `0.0`.

## Out of scope

- Child relationships / cardinality / junction typing (that is §18 future).
- Label/locale-based fuzzy matching.
- Address as a single compound STRUCT in SOQL (we resolve to scalar component
  fields the report actually used).
- Any token not present in a captured real fixture.

## TDD plan (fixtures from the real report)

Driven by the **real report fixture that produced `ADDRESS2_*` / `PHONE1-3`**
(mocked via `sf_mock_report_describe_body` + `sf_mock_describe_body`):

- a report whose `detailColumns` include `ADDRESS2_CITY`, `ADDRESS2_STREET`,
  `PHONE1` → each maps to the fixture's real field, validates against Describe,
  and the report translates with those fields in the SOQL SELECT.
- a mapped token whose target field is **absent** from Describe → blocks
  (`projected_field`, token in `unresolved_columns`).
- an address token used in a **non-filterable** filter → blocks (`filter_field`).
- an **unmapped** compound token (e.g. a token not in the fixture map) → blocks,
  not guessed.
- regression: `salesforce_report_soql.test` existing cases unchanged.
- full `*salesforce*` green.

## Open questions for PM (need the fixture to finalize)

1. **Fixture source** — please confirm the real report + object that produced
   `ADDRESS2_CITY` / `PHONE1-3`, and the **expected target field API names** per
   token. The map entries are populated from this (RED test). Without it I can
   build the mechanism but not the authoritative entries.
2. **Universal vs object-specific** — are `PHONE1-3` / `ADDRESS*` mappings the
   same across objects, or per base object? Recommend **object-keyed** entries
   (with `*` only where a token is provably universal) to avoid the positional
   ambiguity that killed `column_prefix`.
3. **Scope of first cut** — only the exact tokens in the provided fixture
   (recommended), or a slightly broader documented set (e.g. all `ADDRESS_*`
   components for the same object)? Lean: **only fixture-proven tokens**.
4. **Placement** — Report-Bridge-side normalizer feeding `engine.ResolveField`
   (recommended), vs adding report-token vocabulary into the engine. Lean:
   report-side.

## Logistics

- Branch: `design/report-bridge-address-tokens` (this doc). On GO: implement on
  `feat/report-bridge-address-tokens`; design doc merges with the first RED test.
- v0.12.1 stays the frozen community candidate; this block accrues to the next
  own-repo release, not the parked community package.
- No code until explicit GO (and the fixture mappings from Q1).

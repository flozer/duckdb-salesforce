# Report Bridge — base-object mapping via report /describe — design (§16, Phase 1.1)

Status: **DESIGN ONLY — no code.** Awaiting GO for TDD **and** a real PII-free
`/describe` sample (see the critical open question).
Date: 2026-06-15. Base: `main` = `66db28f`.

## Objective

Raise the translatable rate of `salesforce_report_soql()` by resolving the
**base sObject** from `/analytics/reports/{id}/describe` metadata — especially
`reportTypeMetadata` — **when, and only when, the describe carries an
unambiguous, validatable base object**. Never infer aggressively. All existing
guards stay; resolving the base only removes the *base* block — every
projected/filtered/relationship field is still validated.

## Current state (Phase 1)

`ReportSoqlBind` builds an ordered base-object candidate list, accepts the first
that `IsSafeIdentifier` **and** `engine.IsQueryable`:

1. `custom_entity_suffix` — `reportType.type` `CustomEntity$X` → `X` (strong).
2. `builtin_report_type_map` — small standard-type map (`ContactList`→`Contact`).
3. `column_prefix` — dominant dotted prefix; **hint-only**, fills provenance but
   is forced to **block** (`base_object`), never enables `translatable=true`.

`reportTypeMetadata` is explicitly NOT a source yet (code comment, line ~683).
Most real reports use **standard** report types whose `reportType.type` is an
opaque internal name not in the small builtin map → base unresolved → blocked.
That is the gap this phase targets.

## What `/analytics/reports/{id}/describe` actually returns (known shape)

- `reportMetadata.reportType` = `{ "type": <internal id>, "label": <human> }`.
  - custom report types: `type` = `CustomEntity$<ApiName>` (already used).
  - standard report types: `type` = an opaque token (e.g. `ContactList`);
    `label` is a localized human string (NOT an API name).
- `reportExtendedMetadata.detailColumnInfo[<col>]` = `{ label, dataType }` — no
  field/object API name.
- `reportTypeMetadata` = `{ categories: [ { label, columns: { <apiName>: {...} } }, ... ], ... }`.
  Categories group the available columns; the **first** category is typically the
  primary object, and its `columns` keys are **field API names** (bare for base
  fields, dotted for related). But `reportTypeMetadata` does **not** appear to
  carry a single explicit "base object API name" field.

**Honest assessment:** the describe likely has **no single unambiguous base
sObject API name field**. So aggressive inference is off the table (PM rule).
Two conservative, validatable signals are worth evaluating against a real sample:

- **(A) Expand the builtin standard-type map** (`reportType.type` → object),
  fixture-backed, exactly like the token map — every entry proven by a real
  report. Highest confidence; no inference.
- **(B) `reportTypeMetadata` first-category bare-column inference** — take the
  bare (non-dotted) column API names of `categories[0]`, find the unique
  queryable object whose Describe contains *all* of them. Accept **only if
  exactly one** object qualifies; otherwise ambiguous → block. This needs a real
  sample to judge reliability/cost (it may require describing several candidate
  objects — possibly expensive / still ambiguous).

## Proposal

Add a new candidate source, validated by `IsQueryable` like the others, slotted
**below `custom_entity_suffix` and the builtin map, above `column_prefix`**:

- **Cut 1 (safe, recommended):** **(A)** — grow `BuiltinReportTypeObject` from a
  real fixture set (provenance `builtin_report_type_map`). Pure, no inference,
  immediate value for the standard types we can prove. Mirrors the token-map
  discipline: **entries only with fixture evidence.**
- **Cut 2 (only if a sample proves it safe):** **(B)** the
  `reportTypeMetadata` first-category unique-object resolution, new provenance
  `report_type_metadata`. Accept only on a unique queryable match; ambiguous →
  block. Gated on the real sample showing it is unambiguous and not too costly.

Guards unchanged: field/filter/relationship validation, no partial SOQL,
`column_prefix` stays hint-only, `confidence` 1.0 only on full translation. No
Metadata API.

## TDD plan (realistic mocked `/describe`)

- positive: a report whose `reportType.type` is a (fixture-proven) standard type
  → base resolves via the expanded builtin map, validated queryable, report
  translates (with already-resolvable columns).
- ambiguous: `reportTypeMetadata` first-category columns match >1 queryable
  object (cut 2) → **block** `base_object`, not guessed.
- not-queryable: candidate base not in Describe Global → block.
- fallback intact: `CustomEntity$X` still wins when present.
- `column_prefix` still cannot enable translation.
- regression: `salesforce_report_soql.test` unchanged.

## CRITICAL open question (need this to finalize — same discipline as tokens)

**Please provide a real, PII-free `/analytics/reports/{id}/describe` JSON** (or
just the relevant slices) for one or two real reports, so I can confirm **which
fields actually + unambiguously indicate the base object**:

- `reportMetadata.reportType` (`type` + `label`),
- the top-level keys of `reportTypeMetadata`,
- `reportTypeMetadata.categories[0]` (label + the column apiName keys).

Without it I can build mechanism (A) and seed the builtin map only from proven
report types; I will **not** implement (B) or guess fields. This is the exact
point you flagged: the design must name the real `reportTypeMetadata` fields, and
that requires seeing one.

## Other open questions (recommendations inline)

1. **Cut 1 only, or cut 1 + cut 2?** Lean **cut 1 now** (builtin-map expansion,
   fixture-backed); evaluate cut 2 only after the sample.
2. **Provenance label** for cut 2 → `report_type_metadata`. OK?
3. **Builtin-map entries** — fixture-backed only (recommended), like the token
   map. Confirm.

## Logistics

- Branch: `design/report-base-object-mapping` (this doc). On GO: implement on
  `feat/report-base-object-mapping`.
- v0.12.1 stays the frozen community candidate; accrues to the next own-repo
  release. No code until GO (+ the `/describe` sample for anything beyond
  fixture-proven builtin-map entries).

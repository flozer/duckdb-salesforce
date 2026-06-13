# Report Bridge v1.6 — Phase 4: Explainability (design)

Status: DESIGN ONLY. No C++ in this branch. Scope: make
`salesforce_report_soql()` explainable — surface *why* it translated or refused
as structured columns, not only `translatable` + free-text `caveats`. Builds on
Phases 1–3. This phase **adds columns** (small, stable). Still no partial SOQL.

## New columns (appended after `caveats`)

| Column | Type | Meaning |
|---|---|---|
| `base_object_resolved_by` | VARCHAR | `custom_entity_suffix` \| `builtin_report_type_map` \| `column_prefix_hint` \| `unresolved` |
| `translation_status` | VARCHAR | closed set: `full` \| `none` (no `partial` — we never emit partial SOQL) |
| `blocked_by` | VARCHAR | first primary blocker (closed set below); `none` when full |
| `unresolved_columns` | LIST(VARCHAR) | projected tokens that did not resolve |
| `unresolved_filters` | LIST(VARCHAR) | filter tokens that did not resolve |
| `confidence` | DOUBLE | `1.0` when `full`; `0.0` when `none` (no false granularity yet) |

Existing columns (`report_id`, `report_name`, `report_type`, `base_object`,
`columns`, `filters`, `soql`, `translatable`, `caveats`) are unchanged. `caveats`
stays human-readable text — not removed or hidden.

## `blocked_by` closed set + precedence

When `translatable=false`, `blocked_by` is the FIRST blocker in pipeline order:

1. `report_shape` — non-TABULAR (summary/matrix/grouped).
2. `cross_filter` — report has cross filters.
3. `base_object` — base could not be resolved to a queryable object (incl. the
   `column_prefix_hint` case, which never yields `full`).
4. `projected_field` — a projected token did not resolve (plain, non-relationship).
5. `relationship` — a projected/filter token failed specifically as a
   relationship (unknown relationshipName, polymorphic `referenceTo>1`,
   multi-hop, or non-queryable related object).
6. `filter_field` — a filter token did not resolve (plain, non-relationship).
7. `filterability` — a filter field exists but is not `filterable`.
8. `operator` — unsupported filter operator.
9. `literal` — a filter value is an ambiguous date/boolean/null literal.
10. `boolean_filter` — `reportBooleanFilter` uses OR/NOT/grouping/out-of-range.
11. `none` — translatable.

(Implementation note: `resolve_field` gains a failure-reason out so a token miss
is classified as `relationship` vs plain `projected_field`/`filter_field`. The
first blocker set during the existing pipeline wins; later ones still append
caveats but do not overwrite `blocked_by`.)

## Value rules

- `translatable=true`:
  `translation_status='full'`, `blocked_by='none'`,
  `unresolved_columns=[]`, `unresolved_filters=[]`, `confidence=1.0`,
  `base_object_resolved_by` ∈ {`custom_entity_suffix`,`builtin_report_type_map`}.
- `translatable=false`:
  `translation_status='none'`, `soql=NULL`, `confidence=0.0`, `blocked_by` = first
  real reason; `base_object_resolved_by` reflects the source used
  (`column_prefix_hint` when only a prefix was inferred, else `unresolved`).
- `unresolved_columns` / `unresolved_filters` collect ALL unresolved tokens of
  their kind (independent of which category is `blocked_by`).
- `column_prefix_hint` never produces `full`.

## Non-goals (Phase 4)

- No partial SOQL, no `partial` status.
- No complex/nested structs (flat scalar + two VARCHAR lists only).
- No ranking engine / fuzzy confidence — `confidence` is `1.0`/`0.0` for now.
- `caveats` text remains; explainability columns complement it, not replace it.

## TDD cases (mock-first)

1. `ContactList` + `FIRST_NAME` resolves → `translation_status='full'`,
   `blocked_by='none'`, `base_object_resolved_by='builtin_report_type_map'`,
   `unresolved_columns=[]`, `confidence=1.0`.
2. unknown projected token `X` → `translation_status='none'`,
   `blocked_by='projected_field'`, `unresolved_columns=['X']`, `confidence=0.0`.
3. unknown filter token `X` → `blocked_by='filter_field'`,
   `unresolved_filters=['X']`.
4. WHERE field exists but not filterable → `blocked_by='filterability'`.
5. polymorphic/multi-hop relationship → `blocked_by='relationship'`,
   token in the matching unresolved list.
6. summary/matrix → `blocked_by='report_shape'`.
7. boolean filter `1 OR 2` → `blocked_by='boolean_filter'`.
8. cross filter present → `blocked_by='cross_filter'`.

## Acceptance

- A diagnostic consumer can read `translation_status`, `blocked_by`,
  `unresolved_columns`, `unresolved_filters`, `confidence`,
  `base_object_resolved_by` and know exactly why `soql` is NULL — without parsing
  free-text caveats.
- Offline mock + full `*salesforce*` green. No live tests in CI. No schema churn
  beyond the six appended columns. No tag/release/community; `description.yml`
  stays `0.9.2`.

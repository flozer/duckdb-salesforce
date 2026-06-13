# Metadata-driven scan diagnostics — design (v1.6 next block)

Status: **DESIGN ONLY — no code.** Awaiting GO for TDD.
Date: 2026-06-13. Base: `main` = `60d44e8` (post v0.11.1).

## Objective

Improve scan/planning *explainability* using the Metadata Engine v2 — explain
WHY the last scan pushed / did not push each filter and projection, annotated
with real field metadata (filterable / sortable / relationship / referenceTo).

**Hard constraint — zero behavior change.** No change to pushdown, transport
selection, filter translation, generated SOQL, scan caching, or row output. This
block only *reads* what the scan already computed and *annotates* it. The proof
obligation (below) is a first-class deliverable.

## Decision 1 — new function, not an extension of `salesforce_query_cost()`

**New `salesforce_query_explain()`.** Rationale:

- `salesforce_query_cost()` is a fixed 20-column **1-row summary** of the last
  scan (object, soql, transport, counts, pages, quota, guidance). Adding per-field
  detail would either break its shape or force NULL-padded multi-row semantics
  onto a function consumers treat as a single row.
- `query_explain()` is naturally **N rows** (one per projected field / filter),
  a different shape and purpose. Keep them as siblings: `query_cost` = "what did
  the last scan cost", `query_explain` = "field-by-field, why".
- `salesforce_query_cost()` stays **byte-identical** (regression-asserted).

## Decision 2 — explains the LAST scan, no parameters (first cut)

`salesforce_query_explain()` takes **no arguments** and explains the most recent
catalog scan, mirroring `query_cost()`'s last-wins singleton model
(`ScanCost g_cost` in `salesforce_diag.cpp`, `MaxThreads=1`).

- Rejected for first cut: parameterized `salesforce_query_explain('sf', 'SELECT
  …')`. Explaining an arbitrary query would require **planning/binding a query
  without executing it** — new machinery, real risk of diverging from the actual
  scan path, and a much larger surface. Out of scope; revisit only if needed.
- Consequence: like `query_cost()`, the function reflects whatever scan ran last
  in this process. Documented explicitly.

## Decision 3 — how per-field rows are sourced (additive diagnostic capture)

Today the scan **counts** pushed/residual filters but does not enumerate them:

- `ScanPushdownComplexFilter` (`salesforce_scan.cpp:762`) calls `PushdownToSoql`,
  then sets `bind.pushed_filter_count += (before - after)` and
  `bind.residual_filter_count = filters.size()` — the residual `Expression`s
  remain in the `filters` vector; the pushed predicate is accumulated into
  `bind.pushed_where`.
- `DiagRecordScan(...)` (`salesforce_scan.cpp:455`) records counts +
  `where_pushed` + projection counts into the `ScanCost` singleton.

To produce per-element rows we **capture, at the exact point the scan already
classifies them, the structured lists** — this is *diagnostic capture of
already-computed data*, NOT an execution change:

- **Projection list:** the projected field names (already derived as
  `select_fields` for the SOQL `SELECT`).
- **Pushed filters:** `vector<{field_name, op}>` — decoded from the filters
  `PushdownToSoql` consumed (column ref → `projection_to_field` → `bind.fields[i].name`).
- **Residual filters:** `vector<{field_name, op, reason}>` — decoded from the
  `Expression`s left in `filters` after `PushdownToSoql` (plus the always-residual
  `LIMIT`).

New write-only diagnostic mirrors (e.g. `DiagSetExplainProjection(...)`,
`DiagSetExplainFilters(pushed, residual)`) added next to the existing
`DiagSet*` setters. They are pure sinks: nothing in the scan path reads them, so
they cannot alter execution. If decoding a residual expression is not cleanly
possible (nested/!-wrapped/function expr), it is recorded as a single residual
row with `field_name=NULL, reason=complex_expression` — honest, not omitted.

`query_explain()` then annotates each captured element through the **shared
Metadata Engine** (`GetSalesforceCatalogMetadataEngine` → `ResolveField` /
`ResolveRelationship` / describe lookups for filterable/sortable/referenceTo).

> Note: the engine is per-catalog and keyed by ATTACH alias. The diag singleton
> records the object + the *alias is not currently stored*. First cut resolves
> metadata against the **catalog that owns the last-scanned object**; design must
> confirm we can recover the alias (candidate: also record the catalog alias in
> `DiagRecordScan`, additive). If the alias is unavailable, annotation columns
> degrade to `resolved=false, reason=metadata_unavailable` — execution untouched.

## Proposed schema (first cut)

One row per projected field and per filter of the last scan:

| column | type | meaning |
|---|---|---|
| `object_name` | VARCHAR | last-scanned base object |
| `field_name` | VARCHAR | field (NULL for an undecodable complex residual) |
| `role` | VARCHAR | closed set: `projection` \| `filter` |
| `resolved` | BOOLEAN | field resolved in the metadata cache |
| `filterable` | BOOLEAN | from describe (NULL if unresolved) |
| `sortable` | BOOLEAN | from describe (NULL if unresolved) |
| `relationship_name` | VARCHAR | set when the field is a single-hop relationship path (NULL otherwise) |
| `reference_to` | LIST<VARCHAR> | relationship targets (empty when not a relationship) |
| `pushed` | BOOLEAN | filter pushed to SOQL; projection pushed into SELECT |
| `residual` | BOOLEAN | filter applied residually by DuckDB (NULL for projection) |
| `reason` | VARCHAR | closed set (below) |
| `guidance` | VARCHAR | short, actionable hint |

`role` is deliberately a small closed set so later phases can add
`relationship` / `count` / `transport` rows without breaking the shape.

### Closed `reason` set (first cut)

- `pushed_to_soql` — filter translated into the SOQL WHERE.
- `projected` — column included in SELECT projection.
- `not_filterable` — field resolved but `filterable=false` in describe → cannot
  be a SOQL predicate; DuckDB filters residually (over-fetch).
- `unsupported_operator` — operator/predicate shape SOQL cannot express.
- `complex_expression` — OR / NOT / function / cross-field / nested expr kept
  residual (may have `field_name=NULL`).
- `unresolved_field` — field not found in the metadata cache.
- `metadata_unavailable` — engine/alias unavailable; annotation degraded.

The closed set is asserted in tests; any new reason requires a spec update.

### Not diagnosable: LIMIT (dropped from Phase 1)

`limit_residual` was in the draft reason set but is **removed**. In this DuckDB
build the table function never receives the query LIMIT
(`salesforce_scan.cpp:290-292`: "LIMIT pushdown is not wired … LIMIT is applied
residually by DuckDB"). LIMIT is applied *above* the scan, so there is no real
scan/diag state from which `salesforce_query_explain()` could observe it.
Emitting a `limit_residual` row would be fabricated, not observed — disallowed
under the no-invention discipline. Future: only if DuckDB exposes the LIMIT to
the table function, or if another mechanism becomes available **without changing
scan semantics**.

## Relationship handling (first cut, conservative)

- A `Rel.Field` projection/filter is annotated via `engine.ResolveRelationship`
  (single-hop, single non-polymorphic `referenceTo`, queryable target — same
  rule as Report Bridge). On success: `relationship_name` + `reference_to` set.
- Polymorphic / multi-hop → `resolved=false`, `reason=unresolved_field`,
  `relationship_name=NULL` — diagnostic only; **never** changes the scan.
- Deep `relationship` role rows are deferred to a later phase.

## Rules (restating the gate)

- Uses the shared metadata cache (`SalesforceMetadataEngine`) — no extra
  Describe Global / Describe beyond what the cache already holds.
- If metadata is unavailable or a field does not resolve, the diagnostic *says
  so* (`resolved=false`, `reason=metadata_unavailable`/`unresolved_field`); it
  never changes execution.
- `salesforce_query_cost()` unchanged (shape + output asserted identical).
- No scan / pushdown / transport / SOQL / cache behavior change.
- No tag / release / community. `description.yml` stays `0.9.2`. TDD with mocks.

## Proving "zero behavior change"

1. **`salesforce_query_cost.test` output asserted byte-identical** before/after.
2. **Existing scan/pushdown tests unchanged and green** — the execution path
   (`ScanPushdownComplexFilter`, `BuildSelectSoql`, transport selection) is not
   edited except to *call additive write-only diag sinks*; the generated SOQL and
   row output are re-asserted unchanged.
3. **New diag setters are write-only**: a code-review check (and the reviewer
   subagent) confirms no scan-path read of the explain mirrors.
4. **New `salesforce_query_explain.test`**: run a mocked scan with a mix of
   (a) a filterable pushed filter, (b) a non-filterable / complex residual
   filter, (c) a relationship field, (d) a metadata-unavailable degrade case;
   assert per-row `role`/`pushed`/`residual`/`reason` and the metadata
   annotations; assert `query_cost()` still identical in the same test.
   (LIMIT is not testable — not observable by the table function; see above.)
5. **Full `*salesforce*` green.**

## Open questions for PM sign-off (Decisions above are my recommendation)

1. **New fn vs extend `query_cost`** → recommend **new `salesforce_query_explain()`**.
2. **Last-query only vs parameterized** → recommend **last-query, no params** (first cut).
3. **Linking residual/pushed to metadata** → capture structured lists at the
   existing classification point (additive diag sinks) + annotate via the engine.
4. **Closed `reason` values** → the 8 listed above.
5. **Relationship** → single-hop annotate; polymorphic/multi-hop unresolved;
   deep relationship rows deferred.
6. **Zero-behavior-change proof** → the 5 obligations above.

## Phasing

- **Phase 1 (this block):** `salesforce_query_explain()`, last-scan, rows for
  `projection` + `filter` roles, metadata-annotated, closed reason set, proof
  obligations. Small.
- **Phase 2 (later, separate GO):** `relationship` / `count` / `transport` role
  rows; possibly a parameterized explain if a real need appears.

## Logistics

- Branch: `design/metadata-driven-scan-diagnostics` (this doc). Implementation
  branch on GO: `feat/query-explain` (TBD).
- No code until explicit GO. On GO: design doc merges with the first RED test.

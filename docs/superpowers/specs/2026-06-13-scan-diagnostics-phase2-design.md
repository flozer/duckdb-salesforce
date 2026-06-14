# Scan diagnostics Phase 2 — design (salesforce_query_explain rows)

Status: **DESIGN ONLY — no code.** Awaiting GO for TDD.
Date: 2026-06-13. Base: `main` = `449fbd6` (Phase 1 merged, unreleased).

## Objective

Round out `salesforce_query_explain()` for v0.12.0 by adding meta rows that
explain the rest of the last scan's plan — **relationship traversal, count
pushdown, and transport** — still **diagnostic-only, last-scan, no parameters,
zero behavior change.** `salesforce_query_cost()` stays byte-identical.

No new Salesforce calls. No scan/pushdown/transport/SOQL change. Quota row is
**out of scope** (PM call) to keep the block small.

## What is observable (grounding)

- **count / transport / mode / est_rows** — already captured in `ScanCost`
  (`count_pushdown`, `transport`, `transport_reason`, `query_mode`, `est_rows`).
  Phase 2 only needs to carry them into `DiagExplainSnapshot` (additive,
  write-only) and emit rows. No scan change.
- **relationship traversal (resolved)** — observable: a projected
  `is_relationship` field is expanded by `EmitRelationshipSoql`
  (`salesforce_scan.cpp:254`). Capture the traversed relationship paths there
  (write-only), same pattern as the projection/filter capture.
- **relationship blocked** — **NOT observable at scan time.** Polymorphic /
  un-describable parents are skipped at *describe* time (`salesforce_storage.cpp:274`)
  and never become scan fields. They are already surfaced by
  `salesforce_relationships()` (RelDiag). Emitting a "blocked relationship" row
  from scan state would be fabricated. **Phase 2 emits relationship rows for
  TRAVERSED relationships only**; the design doc points users to
  `salesforce_relationships()` for blocked/skipped ones. (Same no-invention
  discipline that dropped `limit_residual` in Phase 1.)

## Row representation (reuse the existing 12-column shape)

`salesforce_query_explain()` is unreleased (only on `main`, not tagged), so the
schema MAY still evolve — but reusing the existing columns keeps it stable and
avoids a churny widen. Meta rows reuse columns as follows:

| role | field_name | resolved | filterable/sortable | relationship_name | reference_to | pushed | residual | reason | guidance |
|---|---|---|---|---|---|---|---|---|---|
| `relationship` | dotted path base (e.g. `Account`) | true | NULL | rel name | targets | false | false | `relationship_traversed` | "single-hop parent relationship expanded" |
| `count` | NULL | NULL | NULL | NULL | [] | =count_pushdown | false | `count_pushdown` \| `count_not_pushed` | existing count guidance |
| `transport` | NULL | NULL | NULL | NULL | [] | false | false | `transport_rest` \| `transport_bulk` | transport_reason (+ `queryAll` when query_mode=queryAll, + est_rows) |

`resolved` becomes **nullable** (meta rows that are not field resolutions emit
NULL). `filterable`/`sortable` already nullable.

### Closed reason set — Phase 2 additions

Phase 1 (kept): `pushed_to_soql`, `projected`, `not_filterable`,
`unsupported_operator`, `complex_expression`, `unresolved_field`,
`metadata_unavailable`.

Phase 2 adds: `relationship_traversed`, `count_pushdown`, `count_not_pushed`,
`transport_rest`, `transport_bulk`.

Closed `role` set becomes: `projection` | `filter` | `relationship` | `count` |
`transport`.

## Row emission rules

- **relationship**: one row per traversed top-level relationship of the last
  scan (deduped by relationship path). Only when `sf_relationships='parent'`
  expanded at least one. `reference_to` = the parent target(s) from describe.
- **count**: exactly ONE row, always. `pushed=count_pushdown`, reason reflects
  it (`count_pushdown` when the scan served `SELECT COUNT()`, else
  `count_not_pushed`). One row keeps it low-noise while still reporting both
  states (PM ask).
- **transport**: exactly ONE row, always. reason = `transport_rest` /
  `transport_bulk`; `guidance` carries `transport_reason`, the `queryAll`
  marker, and `est_rows` when known. (Bulk chunk/poll counts stay in
  `query_cost`; not duplicated here.)

Ordering: projection rows, then filter rows (Phase 1), then relationship, count,
transport (stable, appended) — so existing Phase 1 assertions that filter by
`role` are unaffected.

## Capture plan (additive, write-only — unchanged discipline)

- Extend `DiagExplainSnapshot` with `transport`, `transport_reason`,
  `count_pushdown`, `query_mode`, `est_rows` (already in `ScanCost`; just copy
  them in `DiagGetExplain`).
- Add a relationship capture: in InitGlobal, when building `select_fields`,
  record each traversed top-level `is_relationship` field as a
  `DiagExplainItem{role="relationship", field=rel target/name, field_known=true}`
  with its `relationship_name`/`reference_to` — appended to the same write-only
  `explain` vector handed to `DiagSetExplain`. No execution change.
- `query_explain` bind emits the count + transport rows from the snapshot
  scalars, and annotates relationship rows via the engine (target object
  queryable? — annotate `reference_to`).

Write-only invariant holds: nothing in the scan path reads these back; the
relationship/count/transport capture does not touch `pushed_where`, generated
SOQL, counts, or transport selection.

## Proving zero behavior change (same 5 obligations)

1. `query_cost.test` byte-identical.
2. Existing scan/pushdown/relationship tests unchanged and green.
3. Write-only invariant re-verified (reviewer: no scan-path read of the new
   snapshot fields / relationship capture).
4. Phase 1 `query_explain.test` assertions still pass (role-filtered queries
   unaffected by the appended meta rows).
5. New `query_explain` Phase 2 assertions: a relationship-expanded scan emits a
   `relationship` row with target; a `COUNT(*)` scan emits `count` row
   `count_pushdown`; a normal scan emits `count_not_pushed` + a `transport`
   row (`transport_rest`); a forced-Bulk scan emits `transport_bulk`. Full suite
   green.

## Open questions for PM sign-off (recommendations inline)

1. **Relationship blocked rows** → recommend **traversed-only**; blocked stays
   in `salesforce_relationships()` (not observable at scan; no fabrication).
2. **count row when not a count scan** → recommend **always one row**
   (`count_pushdown` | `count_not_pushed`).
3. **transport detail placement** → recommend **reuse `guidance`** for
   transport_reason + queryAll + est_rows (no new column).
4. **`resolved` becomes nullable for meta rows** → recommend yes.
5. **Quota row** → recommend **out of scope** (PM already leaned this way).

## Logistics

- Branch: `design/scan-diagnostics-phase2` (this doc). On GO: implement on
  `feat/query-explain-phase2`, design doc merges with the first RED test.
- No code until explicit GO.

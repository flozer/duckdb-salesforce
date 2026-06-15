# Relationship Resolver v2 — design (ROADMAP v1.6 §18)

Status: **DESIGN ONLY — no code.** Awaiting GO for TDD.
Date: 2026-06-15. Base: `main` = `156d2a4`.

## Objective

A **read-only, on-demand** relationship metadata surface: given a catalog +
object, enumerate its **parent** relationship paths derived strictly from REST
Describe (`relationshipName`, `referenceTo`), with **explicit status** for every
edge — `resolved`, `polymorphic`, `self_reference`, `cyclic`, `not_queryable`,
`not_describable`, `depth_limited`. Bounded depth. Sourced through the shared
Metadata Engine v2 (de-duped Describe). **No scan / pushdown / Report Bridge
behavior change.**

This is distinct from the existing `salesforce_relationships()`, which is a
**last-scan reactive** snapshot of what parent-expansion did during the most
recent schema resolution (depth 1–2, tied to `sf_relationships`). v2 is a
**proactive graph enumerator** the user can point at any object, independent of
whether expansion is enabled — parallel to `salesforce_metadata_objects()` /
`salesforce_metadata_fields()` (engine-backed, catalog-scoped, on-demand).

## Surface

New table function (no change to existing functions):

```
salesforce_relationship_graph(catalog, object [, max_depth])
```

- `catalog` VARCHAR (ATTACH alias), `object` VARCHAR — both required, non-NULL.
- `max_depth` INTEGER, optional, default **1** (parent only). Clamped to
  `[1, 4]`. Depth 1 = direct parents; 2 = grandparents; etc.

### Schema (one row per discovered edge)

| col | type | notes |
|---|---|---|
| `source_object` | VARCHAR | object the edge departs from (the parent at the prior level) |
| `relationship_name` | VARCHAR | describe `relationshipName` (the dotted segment) |
| `path` | VARCHAR | dotted path from the root, e.g. `Account.Owner` |
| `depth_level` | INTEGER | 1 = parent, 2 = grandparent, … |
| `target_object` | VARCHAR | resolved parent sObject; **NULL** if polymorphic/unresolved |
| `reference_to` | LIST(VARCHAR) | all describe `referenceTo` targets (1 normally; >1 polymorphic) |
| `direction` | VARCHAR | `parent` (first cut is parent-only) |
| `relationship_type` | VARCHAR | `reference` when known (no child/junction typing in cut 1) |
| `status` | VARCHAR | closed set (below) |
| `caveat` | VARCHAR | short human reason; NULL when `resolved` |

### Status closed set

| status | meaning | recurse? |
|---|---|---|
| `resolved` | single `referenceTo`, queryable target, describable | yes (until depth) |
| `polymorphic` | `referenceTo` count > 1 — no single target | no |
| `self_reference` | target == an ancestor on the path (incl. self) | no |
| `cyclic` | target already visited on this path (cycle) | no |
| `not_queryable` | target not `queryable` in Describe Global | no |
| `not_describable` | target Describe failed / unavailable | no |
| `depth_limited` | edge exists at `max_depth`; deeper not explored | no |

## Algorithm (read-only, engine-backed)

DFS/BFS over parent reference fields, starting at `object`, bounded by
`max_depth`, carrying the visited-ancestor set for cycle/self detection:

1. Describe `current` via `engine.GetObjectDescribe` (cached/de-duped).
2. For each field with a non-empty `relationship_name`:
   - emit a row; set `reference_to` from the field.
   - `referenceTo.size() != 1` → `polymorphic` (target NULL), don't recurse.
   - target = `referenceTo[0]`. If target ∈ visited-ancestors → `self_reference`
     (target==current's chain) / `cyclic`; don't recurse.
   - `!engine.IsQueryable(target)` → `not_queryable`; don't recurse.
   - else `resolved`. If `depth_level == max_depth` → still `resolved` but mark a
     trailing `depth_limited` only if deeper edges exist? **(open Q3)**. Else
     recurse with `target` appended to the path + visited set.
   - target Describe failing mid-recursion → `not_describable`.

Cost: one Describe per distinct object visited (engine-cached; counted by
`salesforce_describe_calls()`). Bounded by `max_depth` and the object's fan-out.

## Out of scope (cut 1)

- **Child relationships** (`childRelationships`) — Describe parse doesn't capture
  them today; parent-only first, child graph is a later cut.
- Automatic SQL join rewriting, relationship aggregate pushdown, cross-filter
  translation.
- Feeding Report Bridge / `report_soql` from v2 — only after the surface is
  proven safe (later block). `report_soql` keeps its current single-hop
  `engine.ResolveRelationship`.
- Cardinality/junction-object typing beyond `reference`.

## No behavior change

- New function only; `salesforce_relationships()`, scan, pushdown, `report_soql`
  unchanged. Read-only; never mutates the engine beyond cached Describes (same as
  the other diagnostics). Independent of `sf_relationships` (works with expansion
  off).

## TDD plan (mocked)

Mock multi-object Describe via `sf_mock_describe_body` `|~|` sequencing +
`sf_mock_sobjects_body` for queryability. New `test/sql/salesforce_relationship_graph.test`:

- **single-hop resolved**: `Contact.Account` → `resolved`, target `Account`,
  `reference_to=['Account']`, depth 1.
- **polymorphic**: `Contact.Owner` referenceTo `[User, Group]` → `polymorphic`,
  target NULL, `reference_to=['User','Group']`, no recursion.
- **depth 2 (grandparent)**: `max_depth:=2` → `Contact.Account` then
  `Account.Parent` (Account→Account) flagged `self_reference`/`cyclic`.
- **not_queryable**: target absent from `sobjects` queryable set → `not_queryable`.
- **depth bound**: `max_depth:=1` stops at parents; deeper not emitted.
- **cache**: distinct-object Describe de-duped (assert `salesforce_describe_calls()`).
- **NULL guards**: NULL catalog/object rejected.
- existing `salesforce_relationships.test` / scan tests stay green (no change).

## Open questions for PM (recommendations inline)

1. **Name** — `salesforce_relationship_graph(catalog, object[, max_depth])`
   (recommended) vs `salesforce_relationship_paths` vs extending
   `salesforce_relationships()`. Recommend a **new** fn; keep the last-scan diag
   separate.
2. **Depth** — default `1`, clamp `[1,4]` (recommended). OK?
3. **`depth_limited` semantics** — emit a distinct `depth_limited` row when an
   edge exists exactly at `max_depth` (signals "more below, not explored"), or
   just stop silently? Recommend: the edge at `max_depth` is `resolved`; do not
   synthesize extra `depth_limited` rows (keep it honest/simple) — reserve
   `depth_limited` for a future explicit "truncated" marker. **(lean: drop
   `depth_limited` from cut 1 status set.)**
4. **Polymorphic recursion** — do NOT recurse into polymorphic targets in cut 1
   (report the edge only). Recommend.
5. **Self vs cyclic** — distinguish `self_reference` (target == immediate source)
   from `cyclic` (target == a non-immediate ancestor), or collapse into one
   `cyclic`? Recommend keep both (clearer).

## Logistics

- Branch: `design/relationship-resolver-v2` (this doc). On GO: implement on
  `feat/relationship-resolver-v2`; design doc merges with the first RED test.
- No code until explicit GO.

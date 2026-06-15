# Relationship Resolver v2 — child relationships — design (ROADMAP §18 cut 2)

Status: **DESIGN ONLY — no code.** Awaiting GO for TDD.
Date: 2026-06-15. Base: `main` = `febc2a0` (v0.13.0 released).

## Objective

Extend `salesforce_relationship_graph()` with **child** relationships (the
one-to-many side), **opt-in**, read-only, metadata-only, via the shared Metadata
Engine. Complements the parent enumerator (cut 1) without touching scan /
pushdown / Report Bridge / community. Mock-testable; no org fixture required.

## Grounding

- REST Describe carries a top-level `childRelationships` array (sibling to
  `fields`), each entry: `{ childSObject, field, relationshipName, ... }`
  (`relationshipName` may be **null** → not subquery-addressable in SOQL).
- We do **not** parse it today. `SalesforceDescribe` has `fields` + the
  synthesised parent STRUCT bits only.
- The graph already has a `direction` column (currently always `parent`) and a
  `relationship_type` column — child rows slot in cleanly.

## Surface (opt-in, non-breaking)

Keep cut-1 behavior identical by default. Add an opt-in:

```
salesforce_relationship_graph(catalog, object [, max_depth] [, include_children := false])
```

- Default `include_children = false` → **exactly today's parent-only output**.
- `include_children := true` → also emit the queried object's **direct child
  relationships** (see depth rule). Recommend a **named** boolean param (DuckDB
  named table-function args) so the positional `max_depth` varargs stays intact.

## Child edge rows (reuse the 10-column schema)

| col | child-edge value |
|---|---|
| `source_object` | the queried/current object |
| `relationship_name` | `childRelationships[].relationshipName` (NULL → see status) |
| `path` | the child relationship name (or `child:<ChildObject>` when unnamed) |
| `depth_level` | the level at which the child set is listed |
| `target_object` | `childSObject` (the child sObject) |
| `reference_to` | `[<field>]` — the child's FK field that points back |
| `direction` | **`child`** |
| `relationship_type` | **`childRelationship`** |
| `status` | `resolved` \| `unnamed_child` \| `not_queryable` \| `not_describable` |
| `caveat` | short reason (NULL when `resolved`) |

New status: **`unnamed_child`** — `relationshipName` is null, so the child set is
not SOQL-subquery-addressable; reported (honest), not traversed.

## Depth / fan-out rule (conservative)

Child relationships fan out heavily (an Account can have 40+). Cut 2:

- List child relationships of the **root object only** (depth 1), **not**
  recursed and **not** expanded under resolved parents. Parent traversal is
  unchanged and still bounded by `max_depth`; children are a flat, single-level
  add-on. Deep/child-of-child graphs = future.
- No artificial row cap (it is a diagnostic; the user sees the full child list),
  but the single-level rule keeps it bounded. **(open Q4)**

## Parsing (additive)

- Add `struct SalesforceChildRelationship { string child_object; string field; string relationship_name; };`
  and `vector<SalesforceChildRelationship> child_relationships;` to
  `SalesforceDescribe` (default empty → no impact on existing consumers).
- Parse `childRelationships[*].{childSObject, field, relationshipName}` in
  `ParseDescribe`. Additive; Report Bridge / scan unaffected.

## Conservative rules

- Read-only, diagnostic-only; no scan/pushdown/Report Bridge change.
- `include_children` defaults false → byte-identical to cut 1 for existing calls.
- Child `target_object` validated via `IsQueryable`; `unnamed_child` and
  not-queryable/not-describable children reported with status, not traversed.
- No SOQL subquery generation (that would be a Report-Bridge/scan concern, out
  of scope) — this is purely a metadata graph view.

## TDD plan (mocked)

- `include_children` omitted/false → output identical to cut 1 (regression:
  `salesforce_relationship_graph.test` unchanged).
- `include_children := true` on an object whose describe has `childRelationships`
  → child rows with `direction=child`, `relationship_type=childRelationship`,
  `target_object=childSObject`, `reference_to=[field]`, `status=resolved`.
- a child with null `relationshipName` → `status=unnamed_child` (reported).
- a child whose `childSObject` is not queryable → `not_queryable`.
- parent + child mixed in one call (parents bounded by `max_depth`, children
  single-level).
- full `*salesforce*` green.

## Open questions for PM (recommendations inline)

1. **Opt-in mechanism** — named `include_children := false` (recommended) vs a
   `direction` enum (`parent`|`child`|`both`) vs a separate function. Lean:
   named boolean, default false (non-breaking).
2. **Child depth** — root-only single level (recommended) vs interleaving
   children at each parent level. Lean: root-only for cut 2.
3. **`unnamed_child`** — report it as a status (recommended, honest) vs silently
   skip null-relationshipName children. Lean: report.
4. **Fan-out** — no row cap + single-level (recommended) vs a documented cap.
   Lean: no cap, rely on single-level.
5. **`relationship_type` label** — `childRelationship`. OK?

## Logistics

- Branch: `design/relationship-graph-children` (this doc). On GO: implement on
  `feat/relationship-graph-children`.
- No tag/release/community; v0.12.1 community candidate stays frozen/parked;
  accrues to a future own-repo release. No code until explicit GO.

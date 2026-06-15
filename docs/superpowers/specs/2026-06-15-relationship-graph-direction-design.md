# Relationship graph — direction filter — design (ROADMAP §18 usability)

Status: **DESIGN ONLY — no code.** Awaiting GO for TDD.
Date: 2026-06-15. Base: `main` = `d1b1412`.

## Objective

Add a `direction := 'parent' | 'child' | 'both'` named param to
`salesforce_relationship_graph()` so callers can list only parents, only
children, or both — without the noise of the other side. **Schema unchanged**,
read-only, metadata-only, no scan/pushdown/Report Bridge change. **Backward
compatible**: existing calls behave exactly as today.

## Current API (after cut 1 + cut 2)

```
salesforce_relationship_graph(catalog, object [, max_depth] [, include_children := false])
```

- no `include_children` → **parent-only** (default).
- `include_children := true` → **both** (parents + root child relationships).

## Proposed: add `direction`, keep behavior identical by default

```
salesforce_relationship_graph(catalog, object [, max_depth]
                              [, include_children := false] [, direction := 'parent'])
```

**Effective direction (resolution order — preserves every current behavior):**

1. If `direction` is provided → it is authoritative: `parent` | `child` | `both`.
2. else if `include_children := true` → `both`.
3. else → `parent` (today's default).

So:

| call | effective | same as today? |
|---|---|---|
| (neither) | parent | ✓ (default) |
| `include_children := true` | both | ✓ (cut 2) |
| `direction := 'parent'` | parent | new (explicit) |
| `direction := 'child'` | child | new |
| `direction := 'both'` | both | new |
| `include_children := true, direction := 'parent'` | parent | new (direction wins) |

- **`direction` wins** when both are set (explicit param is authoritative).
  `include_children` stays as a backward-compatible convenience alias. **(open Q1)**
- Invalid `direction` (not parent/child/both, case-insensitive) → clear
  `BinderException`.

## Semantics per direction

- `parent` — parent DFS, bounded by `max_depth` (cut 1 behavior). No child rows.
- `child` — root object's **direct** child relationships only (single level, as
  cut 2). `max_depth` does not affect children (they are root-only); documented.
  No parent rows.
- `both` — parents (bounded by `max_depth`) + root children. Equivalent to
  today's `include_children := true`.

Output schema is **unchanged** (`direction` is an input filter; the existing
`direction` *column* still labels each row `parent`/`child`).

## Out of scope

- Deep child-of-child traversal, cardinality/junction typing (§18 future).
- No new columns, no scan/Report Bridge change.

## TDD plan (mocked)

- regression: `(neither)` → parent-only, byte-identical to cut 1;
  `include_children := true` → both, identical to cut 2.
- `direction := 'parent'` → only `direction='parent'` rows.
- `direction := 'child'` → only `direction='child'` rows (no parents).
- `direction := 'both'` → both.
- precedence: `include_children := true, direction := 'parent'` → parent-only.
- invalid `direction := 'sideways'` → error.
- `direction := 'child'` ignores `max_depth` (children stay single-level).
- full `*salesforce*` green; existing relationship-graph tests unchanged.

## Smoke

Add `-Direction <parent|child|both>` to `run_smoke_relationship_graph.ps1`
(passes `direction := '<v>'`); keep `-IncludeChildren` working.

## Open questions for PM (recommendations inline)

1. **Conflict** `include_children := true` + `direction := 'parent'` —
   **direction wins** (recommended) vs raise an error on contradiction. Lean:
   direction wins (explicit param authoritative; no friction).
2. **Keep `include_children`** as a backward-compat alias (recommended) vs
   soft-deprecate in docs now. Lean: keep, no deprecation noise yet.
3. **Default** stays `parent` (recommended) — never silently start emitting
   children. Confirm.

## Logistics

- Branch: `design/relationship-graph-direction` (this doc). On GO: implement on
  `feat/relationship-graph-direction`.
- No tag/release/community; v0.12.1 community candidate frozen/parked; accrues to
  v0.14.0 with cut 2. No code until explicit GO.

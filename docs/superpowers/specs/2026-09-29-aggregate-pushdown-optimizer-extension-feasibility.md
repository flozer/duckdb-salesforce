# Aggregate Pushdown via OptimizerExtension — Feasibility Study (P1.1)

> Status: **spec for GO/NO-GO** (roadmap P1.1, 2026-09-29). Investigation
> only — no `src/` change was made for this document. All DuckDB source
> references are read at tag `v1.5.5` (`d8cdaa33`) unless noted; the API is
> byte-identical across `v1.5.4`, `v1.5.5`, and `v1.5.6` (header md5
> compared). Target roadmap items: §3 `COUNT(field)` pushdown and §4
> transparent `MIN`/`MAX` pushdown (both DEFERRED in `docs/ROADMAP.md`).

## 1. Problem

Today the scan cannot distinguish these plans at bind time:

```sql
SELECT COUNT(Name) FROM sf.Account WHERE Industry = 'Tech';  -- wants pushdown
SELECT Name FROM sf.Account WHERE Industry = 'Tech';         -- must not change
```

Both bind to a `salesforce_scan` `LogicalGet` projecting column `Name` — the
aggregate executes in a `LogicalAggregate` operator ABOVE the scan (the
correct-but-unpushed fallback: full row fetch + local aggregation). The
`TableFunction` API has no aggregate-pushdown hook, and the existing
`COUNT(*)` trick (zero projected columns; the scan itself runs
`SELECT COUNT() FROM obj` and emits marker rows,
`src/salesforce_scan.cpp:284`+) only covers the zero-column case.

The question for P1.1: can an extension-owned **`OptimizerExtension`**
rewrite the post-optimization plan so these aggregates run server-side,
safely, with residual fallback?

## 2. API findings (all verified against v1.5.5 source)

**Hook exists, stable, and is the documented extension point.**

- `src/include/duckdb/optimizer/optimizer_extension.hpp`: `OptimizerExtension`
  exposes two callbacks — `pre_optimize_function` (before built-in passes)
  and `optimize_function` (**after all built-in passes**), both receiving
  `(OptimizerExtensionInput&, unique_ptr<LogicalOperator> &plan)` and free to
  restructure the plan.
- `src/optimizer/optimizer.cpp::Optimize()`: the pipeline runs
  pre-extensions → `RunBuiltInOptimizers()` (expression rewriter, filter
  pushdown, join order, unused columns, statistics propagation, TopN, ...) →
  **post-extensions** → `Planner::VerifyPlan(context, plan)`. Each
  `RunOptimizer` step already calls `Optimizer::Verify` (binding consistency
  check) around the pass — so a malformed rewrite fails loudly in tests,
  not silently in production.
- `OptimizerExtension::Register(DBConfig&, extension)` is the registration
  path — same shape as the `StorageExtension::Register(config, ...)` call
  this extension already does in `LoadInternal`
  (`src/salesforce_extension.cpp:41-42`).
- A working loadable-extension example ships in-tree:
  `test/extension/loadable_extension_optimizer_demo.cpp` (matches nodes by
  `get.function.name == "parquet_scan"` — exactly our identification
  strategy for `"salesforce_scan"`).
- Header identical (md5) across v1.5.4 / v1.5.5 / v1.5.6. For DuckDB 2.0:
  re-verify at the next canary run (R-009) — out of scope here, and this
  work ships first on the 1.5.x matrix.

**Operators involved (post-bind, post-optimization):**

- `LogicalAggregate` (`logical_aggregate.hpp`): `groups`,
  `grouping_sets`, `aggregates` (inherited `children`-independent expression
  lists), `group_index`/`aggregate_index` table indices.
- `LogicalGet` (`logical_get.hpp`): `table_index`, `function` (our
  `TableFunction`), `bind_data` (`unique_ptr<FunctionData>` — **ours**:
  `SalesforceScanBindData`, `src/include/salesforce_scan.hpp:16`),
  `returned_types`/`names` (the Get's output schema),
  `table_filters` (unused by our function — we only set
  `projection_pushdown` + `pushdown_complex_filter`, so DuckDB never
  injects standard `TableFilter`s into our Gets).
- `LogicalProjection(idx_t table_index, vector<unique_ptr<Expression>>)` —
  constructible with an arbitrary/existing table index; existing optimizer
  passes do exactly that (`column_lifetime_analyzer.cpp:249` reuses an
  index; `common_subplan_optimizer.cpp:825` generates one).

## 3. Design sketch

New pass, registered as a post-`optimize_function` (after built-ins, so no
later pass can undo or rework what we emit; the plan is final).

**Pattern matched (all guards must hold; any failure = no rewrite, and
today's correct local-aggregation fallback remains):**

1. Root-walk finds `LogicalAggregate` whose **child is directly** our
   `LogicalGet` (`function.name == "salesforce_scan"`); no intervening
   operators.
2. `aggregate.groups.empty()` && `grouping_sets.empty()` (no GROUP BY —
   transparent grouping stays deferred; local grouping is already correct).
3. Every expression in `aggregates` is a `BoundAggregateExpression` with a
   single `BoundColumnRefExpression` child resolving into the Get, and the
   function is in the supported set: first cut **`COUNT(col)`**; phase 2
   `COUNT_DISTINCT(col)`; phase 3 `MIN(col)`/`MAX(col)`.
4. Field guard: the referenced field maps to a queryable, non-relationship
   (no dotted name), non-blob (`base64`) field in
   `bind_data.fields`; for `MIN`/`MAX` additionally numeric/temporal/boolean
   types only (strings: Salesforce collation semantics unverified — stay
   local; see §6).
5. **Residual-safety guard**: `bind_data.residual_filter_count == 0`
   (our pushdown hook translated every conjunct; by extension-hook time
   `bind_data.pushed_where` is final). Any residual conjunct would otherwise
   be applied post-aggregate against the single-row result — a correctness
   violation, so it disables the rewrite entirely.
6. Kill-switch setting respected (see §5).

**Plan surgery (bindings stay valid by construction):**

1. Cast `get.bind_data` to `SalesforceScanBindData`; append a
   `pushed_aggregates` vector: `{func, field_api_name, out_type, output_name}`
   per term. `COUNT` → `BIGINT`; `MIN`/`MAX` → child column type.
2. Rebuild the Get's output schema: `returned_types`/`names` = the aggregate
   outputs, same order as the original aggregate list.
3. Replace the `LogicalAggregate` node with
   `LogicalProjection(agg.aggregate_index-derived bindings, colrefs)` —
   reusing the **aggregate's table index** so every parent reference
   `(agg_index, i)` remains valid, mapping output `i` to
   `ColumnBinding(get.table_index, i)`. Parents (Limit/Order/Projection/
   Filter-HAVING) need no changes.
4. At scan init, when `pushed_aggregates` is non-empty: run ONE SOQL
   aggregate query — `SELECT <aggs> FROM <object> [WHERE <pushed_where>]`
   (same endpoint-selection/mode rules as the existing `COUNT()` probe and
   `salesforce_aggregate()` path; `queryAll` honored via config) — emit the
   single result row, skip pagination entirely.

**Why post-pass instead of `pre_optimize_function`:** before built-ins, the
plan would still flow through filter pushdown, unused-column removal,
statistics propagation, etc., any of which may restructure the subtree under
and around the rewritten Get (re-pushing filters, pruning the new columns).
Post-pass sees the settled plan and owns its rewrite — deterministic and
minimal interaction surface.

**SOQL shape produced (first cut):**

```sql
SELECT COUNT(Name) FROM Account WHERE Industry = 'Tech'
```

## 4. Correctness notes

- `COUNT(field)`: DuckDB counts non-null; SOQL `COUNT(field)` counts
  non-null. Equivalent. (Null-safety of the field itself is irrelevant to
  the count.)
- No-GROUP-BY aggregates always produce exactly one row, empty org included
  (SOQL aggregate without GROUP BY returns one row with nulls; matches
  DuckDB `MIN` over empty = `NULL`, `COUNT` = `0`). The mocked response
  contract is already exercised by the `salesforce_aggregate()` test files.
- `COUNT(DISTINCT col)` → SOQL `COUNT_DISTINCT(col)` — equivalent semantics
  (distinct non-null); phase 2 to keep cut 1 minimal.
- Aggregates over expressions (`MIN(x+1)`), windows, HAVING-containing
  shapes, GROUP BY, joins under the aggregate, and any multi-scan plan:
  pattern does not match → fallback. Fallback is today's behavior — never
  incorrect, only unoptimized.
- Quota/cost: one extra API call per pushed aggregate query (replaces the
  full row-scan calls — strictly cheaper than the fallback). Surfaced via
  `salesforce_last_soql()`, a new `salesforce_query_explain()` meta row
  (`role = aggregate`, one row per pushed term), and
  `salesforce_query_cost()` (count/aggregate pushdown field).

## 5. Rollout stance

- **Default ON, with a kill-switch setting** `sf_aggregate_pushdown`
  (BOOLEAN, default `true`). Precedent: `COUNT(*)` pushdown is transparent
  and on by default today. The switch gives operators an instant revert to
  the old plan shape without a downgrade. (Alternative — opt-in default
  OFF — trades away the benefit for most users; recommended against given
  the guard design, but it is a one-line default change if preferred.)
- Diagnostics make the rewrite visible in every surface we already ship
  (`last_soql`, `query_cost`, `query_explain`); a plan-shape change is also
  visible in native `EXPLAIN`.

## 6. Open items for implementation (P1.2+)

1. Live/mock evidence for SOQL aggregate response shape on **empty orgs**
   (one row with nulls) — add a dedicated mock case mirroring
   `salesforce_aggregate`'s contract.
2. String `MIN`/`MAX`: SOQL lexicographic vs DuckDB binary collation —
   keep local (residual) until a deliberate decision; document.
3. `SUM`/`AVG` (SOQL supported): verify DECIMAL precision/scale round-trip
   against Salesforce before promising; separate cut (P1.4).
4. Dotted (relationship) aggregate fields: SOQL supports
   `COUNT(Account.Name)`; defer past cut 1 to keep the field guard simple.
5. DuckDB 2.0: re-run this API survey at the next canary (R-009); nothing in
   the design depends on 1.5-only behavior that we know of.
6. Update `test/sql/salesforce_functions_docs.test` counts only if any new
   *registered function* is added (the kill-switch is a setting, not a
   function — no count change expected).

## 7. Effort and risk

| Phase | Scope | Rough size |
|---|---|---|
| P1.2a | Optimizer pass + bind-data spec + scan init aggregate path + `COUNT(field)` + kill-switch + diagnostics + mock tests | ~250-400 lines `src/`, ~1 test file, 1 setting, docs EN/PT |
| P1.2b | `COUNT_DISTINCT` | small delta |
| P1.3 | `MIN`/`MAX` numeric/temporal/boolean | small delta + type-guard table |
| P1.4 | `SUM`/`AVG` | gated on decimal evidence (§6.3) |

Risks and mitigations: this is the extension's first logical-plan mutation —
mitigated by the strict all-guards pattern, `Optimizer::Verify` running
around the pass, the kill-switch, and a fallback that is byte-for-byte
today's behavior. The rewrite is purely plan-shape: no new DuckDB API
surface, no dependency on statistics, no interaction with the transport
selector beyond reusing the existing single-row aggregate fetch that
`salesforce_aggregate()` and the `COUNT()` probe already exercise on all
three platforms.

## 8. Recommendation

**GO.** The API exists, is stable across the supported matrix, is
documented by an in-tree loadable-extension example, and the rewrite is
small, guardable, and fully observable. Suggested execution: P1.2a
(`COUNT(field)`) as the first cut behind `sf_aggregate_pushdown`, then
phase by phase per §6 — each with its own mock tests, EN/PT docs, and the
standard validate→tag→release→CI→C.5 release discipline.
